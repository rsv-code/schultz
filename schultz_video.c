/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * schultz_video.c - a node that shows video fed to it.
 *
 * Four stages, in order, and the file is laid out the same way:
 *
 *   1. bytes arrive from the host and are kept in one growing buffer
 *   2. nestegg reads that buffer and hands back packets with timestamps
 *   3. libvpx turns a packet into a picture, which arrives as I420 planes
 *   4. those planes become premultiplied ARGB and go into a slot
 *   5. the node takes the slot whose time has come and draws it
 *
 * Stage four is ours rather than a library's.
 *
 * **Where the line between threads falls.** Stages one to four may run on a
 * decode thread. Stage five never does. So a decode thread touches the byte
 * buffer, the demuxer, the decoder and the slots, and nothing else: not the
 * tree, not the draw list, not the image table, not a handle. Everything it
 * produces is a buffer of pixels and a timestamp, which the node collects on
 * the thread that advances the tree.
 *
 * That is the whole of the concurrency. Keeping it that narrow is what stops
 * video making the rest of the toolkit concurrent.
 */

#include "schultz_video.h"

#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>

#include <nestegg/nestegg.h>
#include <opus_multistream.h>

#define VPX_CODEC_DISABLE_COMPAT 1
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>

#include "schultz_audio.h"
#include "schultz_image.h"
#include "schultz_layout.h"
#include "schultz_paint.h"
#include "schultz_widget.h"
#include "schultz_widgets.h"

/**
 * @brief One decoded picture, waiting for its moment.
 *
 * Two of these. The decoder fills whichever is free and the node empties
 * whichever is due, which is enough that decoding overlaps drawing and few
 * enough that pausing throws almost nothing away.
 */
typedef struct {
    uint32_t *argb;     /**< Premultiplied ARGB, width by height. */
    size_t    words;    /**< How many pixels the buffer can hold. */
    uint32_t  width;
    uint32_t  height;
    uint64_t  when_ns;  /**< When this picture should be shown. */
    int32_t   full;     /**< Nonzero when it holds a picture. */
} schultz_video_slot;

enum {
    /** How many pictures are held ahead of the one showing. */
    SCHULTZ_VIDEO_SLOTS = 2,
    /**
     * How many pictures are held ahead of those, still compressed.
     *
     * Sound and picture arrive interleaved in one file, so reading far enough
     * ahead to keep the sound buffer full means reading past pictures there
     * is nowhere to put yet. Keeping those as they came off the disk costs a
     * few kilobytes each instead of the megabytes a decoded one costs.
     */
    SCHULTZ_VIDEO_AHEAD = 48
};

/**
 * @brief How far ahead of the picture the sound is decoded, in nanoseconds.
 *
 * The sound is what the device asks for on its own schedule, and it asks
 * without waiting. Two decoded pictures is eighty milliseconds at twenty four
 * a second, and a stall anywhere -- a slow frame, a busy machine -- eats that
 * and the sound breaks up. Four hundred milliseconds survives one.
 */
#define SCHULTZ_VIDEO_SOUND_AHEAD_NS 400000000u

/*
 * How far a pushed stream gets ahead of what is still wanted before any of it
 * is let go of.
 *
 * Compacting moves what is kept, so doing it for a few bytes would cost more
 * than it saves. It cannot happen more finely than a cluster in any case, so
 * this only has to be worth a cluster or two. What it costs is about one byte moved for every byte that
 * arrives, which is nothing beside decoding the picture those bytes carry.
 */
#define SCHULTZ_VIDEO_STREAM_SLACK (32u * 1024u)

/* The four bytes a cluster starts with. */
#define SCHULTZ_VIDEO_CLUSTER_ID_0 0x1Fu
#define SCHULTZ_VIDEO_CLUSTER_ID_1 0x43u
#define SCHULTZ_VIDEO_CLUSTER_ID_2 0xB6u
#define SCHULTZ_VIDEO_CLUSTER_ID_3 0x75u

/** What a video node carries. */
typedef struct {
    /*
     * What the host has written and has not been finished with.
     *
     * Opening a WebM stream reads its header, and a stream that runs dry and
     * then receives more is opened again, so the header is kept for as long
     * as the stream plays. Everything the demuxer has read and decoded is
     * let go of; what is kept after the header therefore starts partway in,
     * at `resume`, and always at a cluster, because nestegg_offset_seek is
     * the only seek that needs no cue table and it only works there.
     *
     * The buffer holds [0, head) and then [resume, size), with nothing in
     * between. `size` stays the count of bytes the host has written, so it
     * goes on meaning the same to everything that reads it.
     *
     * Unused when the node was given a file instead: a file is read where it
     * lies.
     */
    unsigned char *bytes;
    size_t         size;
    size_t         capacity;
    size_t         at;        /**< Where nestegg's cursor is. */
    size_t         head;      /**< Header kept for reopening. */
    size_t         resume;    /**< Where what is kept takes up again. */
    size_t         packet_at; /**< Where the packet being read began. */

    /*
     * The other way in. When this is set the bytes above are unused and
     * nestegg reads the file directly, which is also what makes seeking
     * possible: the whole film is still there to be read again.
     */
    FILE          *file;

    nestegg       *demux;     /**< NULL until there is enough to open. */
    int32_t        gave_up;   /**< Set when the file cannot be read at all. */
    unsigned int   track;     /**< Which track is the video one. */
    uint32_t       codec;     /**< One of SCHULTZ_VIDEO_CODEC_*. */
    uint32_t       width;
    uint32_t       height;

    vpx_codec_ctx_t codec_ctx;
    int32_t         codec_ready;

    /*
     * The sound track, when there is one. Opus is the only audio codec WebM
     * carries that can be shipped without a patent licence, and it is the one
     * the toolkit already vendors.
     */
    unsigned int     sound_track;
    int32_t          have_sound;
    OpusMSDecoder   *opus;
    uint32_t         rate;      /**< Always 48000; Opus decodes to that. */
    uint32_t         channels;
    uint32_t         pre_skip;  /**< Samples the encoder says to throw away. */

    /*
     * The pictures in flight, and the lock over them. Everything below this
     * line up to `stop` is shared between the decode thread and the node, and
     * nothing else is.
     */
    schultz_video_slot slots[SCHULTZ_VIDEO_SLOTS];
    uint32_t           fill;      /**< Which slot the decoder writes next. */
    uint32_t           drain;     /**< Which slot the node reads next. */
    SDL_Mutex         *lock;      /**< NULL when there is no thread. */
    SDL_Condition     *room;      /**< Signalled when a slot comes free. */
    SDL_Thread        *thread;    /**< NULL when decoding on the caller's. */
    uint32_t           threads;   /**< What the host asked for. */
    int32_t            stop;      /**< Asks the thread to finish. */
    int32_t            running;   /**< Whether there is anything to decode. */

    /*
     * Decoded sound, waiting for its turn to be handed to the sound system.
     * It sits here rather than going straight out because writing to a stream
     * means touching a handle, and a decode thread does not touch handles.
     * The video slots bound how far ahead this can get: two pictures.
     */
    float           *pcm;
    size_t           pcm_words;   /**< What the buffer can hold. */
    size_t           pcm_filled;  /**< What is in it. */
    uint64_t         sound_frames; /**< Sample frames decoded since the last
                                    *   move, which says how far the sound
                                    *   has been read. */

    /*
     * Pictures read but not decoded, because the slots were full and the
     * sound still wanted more of the file read. Oldest first.
     */
    nestegg_packet  *ahead[SCHULTZ_VIDEO_AHEAD];
    uint64_t         ahead_ns[SCHULTZ_VIDEO_AHEAD];
    /**
     * Where in the stream each of those was read from.
     *
     * This is what says how far back a pushed stream still needs its bytes.
     * A packet that has left this queue has been decoded into a slot and its
     * bytes copied twice over, so nothing refers to them again; one still in
     * it has not. The oldest here is therefore the furthest back anything is
     * wanted, and everything before it may be let go of.
     */
    size_t           ahead_at[SCHULTZ_VIDEO_AHEAD];
    uint32_t         ahead_in;
    uint32_t         ahead_out;
    uint32_t         ahead_count;

    schultz_audio       *audio;  /**< Not owned; the tree's. */
    schultz_handle       sound;  /**< The stream being written, or none. */
    uint64_t             sound_written; /**< Sample frames handed over. */
    uint64_t             sound_base_ns;  /**< Film time of the first of them. */
    int32_t              sound_reset;    /**< It holds the wrong film. */
    /*
     * Whether the clock knows where in the film the file actually is.
     *
     * Asking to move to a point in a film does not move it to that point. It
     * moves to the keyframe at or before it, because that is the last place a
     * decoder can start from, and on an ordinary film that can be seconds
     * earlier. Until a packet arrives to say where that was, the clock is
     * working from what was asked for rather than what was given.
     */
    int32_t              rebase;
    float                volume;
    int32_t              muted;

    /*
     * The controls, when any were asked for. They are ordinary child nodes in
     * a row across the bottom, so a theme that restyles a button restyles
     * these, and they are made and destroyed together.
     */
    uint32_t       controls;
    schultz_handle row;
    schultz_handle play_button;
    schultz_handle stop_button;
    schultz_handle position;     /**< The bar, as a slider. */
    schultz_handle time_label;
    schultz_handle mute_button;
    schultz_handle volume_slider;
    float          position_set; /**< What was last written to the bar. */
    float          volume_set;   /**< What was last written to the volume. */
    uint64_t       time_shown;   /**< The second the label last said. */
    int32_t        showed_playing; /**< What the play button last read. */
    int32_t        showed_muted;   /**< What the mute button last read. */

    schultz_image_table *images; /**< Not owned; the tree's. */
    schultz_handle       frame;  /**< The picture showing, or none. */

    /*
     * How much had arrived when a read last ran out. A stream that is still
     * arriving looks exactly like a file that has ended -- the read returns
     * nothing either way -- so the only way to tell them apart is to notice
     * that more has come since.
     */
    size_t         starved_at;
    /*
     * The timestamp of the last packet actually decoded, and whether there
     * has been one. Seeking after a reopen lands on the keyframe at or before
     * where playback had got to, so the packets between the two would be
     * decoded and shown a second time. This is what says which those are.
     */
    uint64_t       last_ns;
    int32_t        have_last;
    uint64_t       shown;     /**< Frames decoded and shown. */
    uint64_t       duration_ns; /**< How long the film is, or zero. */
    uint64_t       clock_ns;  /**< How far into the film we are. */
    uint64_t       next_ns;   /**< When the next undrawn packet belongs. */
    int32_t        have_next; /**< Whether next_ns means anything. */
    int32_t        playing;
    int32_t        looping;
    int32_t        ended;
} schultz_video_data;

static const schultz_widget_vtable schultz_video_widget;

static schultz_video_data *schultz_video_of(const schultz_tree *tree,
                                            schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_video_widget) {
        return NULL;
    }
    return (schultz_video_data *)schultz_node_widget_data(tree, node);
}

/*
 * With no thread there is nothing to exclude, so the lock is never made and
 * these do nothing. Writing it this way rather than testing for a thread at
 * every use keeps one version of each function instead of two.
 */
static void schultz_video_hold(schultz_video_data *video)
{
    if (video->lock != NULL) {
        SDL_LockMutex(video->lock);
    }
}

static void schultz_video_release(schultz_video_data *video)
{
    if (video->lock != NULL) {
        SDL_UnlockMutex(video->lock);
    }
}

/* ---------------------------------------------------------- 1. the source */

/*
 * nestegg reads through these three, and there are two things they can read
 * from.
 *
 * A file is read where it lies, so a long film costs a buffer rather than its
 * whole length in memory. A stream is a buffer of everything the host has
 * written, because a pushed stream cannot offer anything else: the bytes that
 * have arrived are the file, and the file grows.
 */
/*
 * Where an offset into the stream sits in the buffer, or nothing when it is
 * in the part that was let go of. Before anything is dropped, head and resume
 * are equal and this is the offset itself.
 */
static int32_t schultz_video_stream_spot(const schultz_video_data *video,
                                         size_t at, size_t *out_spot)
{
    if (at < video->head) {
        *out_spot = at;
        return 1;
    }
    if (at >= video->resume && at <= video->size) {
        *out_spot = video->head + (at - video->resume);
        return 1;
    }
    return 0;
}

/* How many bytes the buffer is holding. */
static size_t schultz_video_stream_held(const schultz_video_data *video)
{
    return video->head + (video->size - video->resume);
}

/*
 * Lets go of the bytes nothing wants any more.
 *
 * The furthest back anything is wanted is where the oldest packet still
 * waiting to be decoded was read from. Anything before that has been decoded
 * into a slot, and its bytes copied out twice over on the way, so nothing
 * refers to them again.
 *
 * What is kept has to start at a cluster, because that is the only place the
 * demuxer can be told to take up from when it is built again.
 */
static void schultz_video_stream_trim(schultz_video_data *video)
{
    size_t wanted;
    size_t found = 0u;
    size_t spot;
    size_t limit;
    size_t i;

    if (video->head == 0u || video->bytes == NULL) {
        return;                 /* not opened yet, so the header is unknown */
    }
    wanted = (video->ahead_count > 0u) ? video->ahead_at[video->ahead_out]
                                       : video->packet_at;
    if (wanted <= video->resume ||
        wanted - video->resume < SCHULTZ_VIDEO_STREAM_SLACK) {
        return;
    }
    if (!schultz_video_stream_spot(video, video->resume, &spot) ||
        !schultz_video_stream_spot(video, wanted, &limit)) {
        return;
    }

    /* The last cluster starting at or before what is still wanted. */
    for (i = spot; i + 4u <= limit; i++) {
        if (video->bytes[i]      == SCHULTZ_VIDEO_CLUSTER_ID_0 &&
            video->bytes[i + 1u] == SCHULTZ_VIDEO_CLUSTER_ID_1 &&
            video->bytes[i + 2u] == SCHULTZ_VIDEO_CLUSTER_ID_2 &&
            video->bytes[i + 3u] == SCHULTZ_VIDEO_CLUSTER_ID_3) {
            found = i;
        }
    }
    if (found <= spot) {
        return;                 /* no cluster to move up to */
    }
    {
        size_t gone = found - spot;
        size_t tail = schultz_video_stream_held(video) - found;

        memmove(video->bytes + video->head, video->bytes + found, tail);
        video->resume += gone;
    }
}

static int64_t schultz_video_source_read(void *buffer, size_t length,
                                         void *userdata)
{
    schultz_video_data *video = (schultz_video_data *)userdata;
    size_t left;

    if (length == 0u) {
        return 0;
    }
    if (video->file != NULL) {
        size_t got = fread(buffer, 1, length, video->file);

        video->at += got;
        return (int64_t)got;    /* zero at the end, which is what it wants */
    }
    left = (video->at < video->size) ? video->size - video->at : 0u;
    if (left == 0u) {
        return 0;               /* end of what has arrived, so far */
    }
    if (length > left) {
        length = left;          /* short reads are allowed and are the point */
    }
    {
        size_t spot;

        if (!schultz_video_stream_spot(video, video->at, &spot)) {
            return -1;          /* in the part that was let go of */
        }
        memcpy(buffer, video->bytes + spot, length);
    }
    video->at += length;
    return (int64_t)length;
}

static int schultz_video_source_seek(int64_t offset, int whence, void *userdata)
{
    schultz_video_data *video = (schultz_video_data *)userdata;
    int64_t base;
    long where;

    if (video->file != NULL) {
        int how = (whence == NESTEGG_SEEK_SET) ? SEEK_SET
                : (whence == NESTEGG_SEEK_CUR) ? SEEK_CUR
                : (whence == NESTEGG_SEEK_END) ? SEEK_END : -1;

        if (how < 0 || fseek(video->file, (long)offset, how) != 0) {
            return -1;
        }
        where = ftell(video->file);
        if (where < 0) {
            return -1;
        }
        video->at = (size_t)where;
        return 0;
    }
    switch (whence) {
    case NESTEGG_SEEK_SET: base = 0; break;
    case NESTEGG_SEEK_CUR: base = (int64_t)video->at; break;
    case NESTEGG_SEEK_END: base = (int64_t)video->size; break;
    default: return -1;
    }
    if (base + offset < 0 || (uint64_t)(base + offset) > video->size) {
        return -1;
    }
    {
        size_t want = (size_t)(base + offset);
        size_t spot;

        if (!schultz_video_stream_spot(video, want, &spot)) {
            return -1;          /* somewhere that was let go of */
        }
        video->at = want;
    }
    return 0;
}

static int64_t schultz_video_source_tell(void *userdata)
{
    return (int64_t)((schultz_video_data *)userdata)->at;
}

/* ------------------------------------------- 4. I420 to premultiplied ARGB */

/*
 * The one conversion needed, because VP8 and VP9 profile 0 produce exactly
 * one format: eight bit luma at full size and two chroma planes at half.
 *
 * These are the BT.601 studio swing coefficients in 8.8 fixed point, which is
 * what the overwhelming majority of video carries and what a decoder reports
 * when it says nothing. Luma runs 16 to 235 and chroma 16 to 240, so the
 * offsets are taken off before scaling and the result is clamped: a bright
 * highlight is above 235 and would wrap to black without it.
 *
 * Alpha is opaque and the colour channels are already premultiplied by it,
 * which for an opaque pixel means they are left alone.
 */
static void schultz_video_i420_to_argb(const unsigned char *y, int y_stride,
                                       const unsigned char *u, int u_stride,
                                       const unsigned char *v, int v_stride,
                                       uint32_t *out, uint32_t width,
                                       uint32_t height)
{
    uint32_t row;

    for (row = 0; row < height; row++) {
        const unsigned char *y_row = y + (size_t)row * (size_t)y_stride;
        const unsigned char *u_row = u + (size_t)(row >> 1) * (size_t)u_stride;
        const unsigned char *v_row = v + (size_t)(row >> 1) * (size_t)v_stride;
        uint32_t *dest = out + (size_t)row * width;
        uint32_t col;

        for (col = 0; col < width; col++) {
            int c = (int)y_row[col] - 16;
            int d = (int)u_row[col >> 1] - 128;
            int e = (int)v_row[col >> 1] - 128;
            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;

            if (r < 0)   { r = 0; }
            if (r > 255) { r = 255; }
            if (g < 0)   { g = 0; }
            if (g > 255) { g = 255; }
            if (b < 0)   { b = 0; }
            if (b > 255) { b = 255; }

            dest[col] = 0xFF000000u | ((uint32_t)r << 16) |
                        ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

/* ------------------------------------------------------ 2. opening the file */

/* Finds the first video track and the codec it carries. */
static int32_t schultz_video_take_track(schultz_video_data *video)
{
    unsigned int count = 0u;
    unsigned int i;

    if (nestegg_track_count(video->demux, &count) != 0) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        nestegg_video_params params;

        if (nestegg_track_type(video->demux, i) != NESTEGG_TRACK_VIDEO) {
            continue;
        }
        switch (nestegg_track_codec_id(video->demux, i)) {
        case NESTEGG_CODEC_VP8: video->codec = SCHULTZ_VIDEO_CODEC_VP8; break;
        case NESTEGG_CODEC_VP9: video->codec = SCHULTZ_VIDEO_CODEC_VP9; break;
        default: continue;      /* a codec this cannot ship; keep looking */
        }
        if (nestegg_track_video_params(video->demux, i, &params) != 0) {
            continue;
        }
        video->track  = i;
        video->width  = params.width;
        video->height = params.height;
        return (video->width > 0u && video->height > 0u);
    }
    return 0;
}

/*
 * Finds the sound track and builds a decoder for it.
 *
 * An OpusHead sits in the track's private data and says everything needed:
 * how many channels, how many samples the encoder wants thrown away at the
 * start, and for more than two channels how they are grouped into streams.
 * The layout is fixed by RFC 7845 section 5.1.
 *
 * Opus always decodes to 48000 samples a second whatever it was recorded at,
 * so there is no rate to read.
 */
static void schultz_video_take_sound(schultz_video_data *video)
{
    unsigned int count = 0u;
    unsigned int i;

    if (nestegg_track_count(video->demux, &count) != 0) {
        return;
    }
    for (i = 0; i < count; i++) {
        unsigned char *head = NULL;
        size_t length = 0u;
        unsigned char mapping[255];
        int streams;
        int coupled;
        int family;
        int error = 0;
        uint32_t channel;

        if (nestegg_track_type(video->demux, i) != NESTEGG_TRACK_AUDIO ||
            nestegg_track_codec_id(video->demux, i) != NESTEGG_CODEC_OPUS) {
            continue;
        }
        if (nestegg_track_codec_data(video->demux, i, 0, &head, &length) != 0 ||
            head == NULL || length < 19u || memcmp(head, "OpusHead", 8) != 0) {
            continue;
        }
        video->channels = head[9];
        video->pre_skip = (uint32_t)head[10] | ((uint32_t)head[11] << 8);
        family = head[18];
        if (video->channels == 0u || video->channels > 255u) {
            continue;
        }
        if (family == 0) {
            /* One stream, and stereo is that stream's two channels. */
            streams = 1;
            coupled = (video->channels == 2u) ? 1 : 0;
            for (channel = 0; channel < video->channels; channel++) {
                mapping[channel] = (unsigned char)channel;
            }
        } else {
            if (length < 21u + video->channels) {
                continue;       /* the table the head promised is not there */
            }
            streams = head[19];
            coupled = head[20];
            memcpy(mapping, head + 21, video->channels);
        }
        video->opus = opus_multistream_decoder_create(
            48000, (int)video->channels, streams, coupled, mapping, &error);
        if (video->opus == NULL || error != OPUS_OK) {
            video->opus = NULL;
            continue;           /* silent rather than refusing to play */
        }
        video->rate        = 48000u;
        video->sound_track = i;
        video->have_sound  = 1;
        return;
    }
}

/*
 * Tries to open what has arrived so far, once. Failing is not final: a stream
 * two hundred bytes in has not arrived yet, and the next write may be enough.
 * Only a file that opens and then turns out to carry nothing this can play is
 * given up on.
 */
static void schultz_video_try_open(schultz_video_data *video)
{
    nestegg_io io;
    vpx_codec_dec_cfg_t cfg;

    if (video->demux != NULL || video->gave_up) {
        return;
    }
    io.read     = schultz_video_source_read;
    io.seek     = schultz_video_source_seek;
    io.tell     = schultz_video_source_tell;
    io.userdata = video;

    video->at = 0u;
    if (nestegg_init(&video->demux, io, NULL, -1) != 0) {
        video->demux = NULL;
        return;                 /* not enough yet, or not WebM */
    }
    if (video->file == NULL) {
        /* Whatever opening reached is the header, and is kept from now on. */
        if (video->at > video->head) {
            video->head = video->at;
        }
        if (video->resume < video->head) {
            video->resume = video->head;
        }
        /* Take up at the first cluster still held. */
        if (video->resume > video->head &&
            nestegg_offset_seek(video->demux,
                                (uint64_t)video->resume) != 0) {
            nestegg_destroy(video->demux);
            video->demux = NULL;
            return;
        }
    }
    if (!schultz_video_take_track(video)) {
        nestegg_destroy(video->demux);
        video->demux  = NULL;
        video->gave_up = 1;     /* opened, and holds nothing playable */
        return;
    }

    /*
     * What had arrived when this opened. A read that finds nothing counts as
     * the file ending unless more has come in since, and without this the
     * first genuine end of a complete file would be mistaken for a stream
     * that had not caught up and would be played again.
     */
    if (!video->have_sound) {
        schultz_video_take_sound(video);
    }
    video->starved_at = video->size;
    if (nestegg_duration(video->demux, &video->duration_ns) != 0) {
        video->duration_ns = 0u;    /* a stream need not say */
    }
    if (video->codec_ready) {
        return;                 /* reopened: the decoder is already built */
    }
    memset(&cfg, 0, sizeof(cfg));
    /*
     * One of the threads asked for reads the file, so the rest are the ones
     * the decoder may split a picture between. Asking for none leaves the
     * decoder on whichever thread calls it, which is the one below.
     */
    cfg.threads = (video->threads > 1u) ? video->threads - 1u : 1u;
    if (vpx_codec_dec_init(&video->codec_ctx,
                           (video->codec == SCHULTZ_VIDEO_CODEC_VP9)
                               ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx(),
                           &cfg, 0) != VPX_CODEC_OK) {
        nestegg_destroy(video->demux);
        video->demux   = NULL;
        video->gave_up = 1;
        return;
    }
    video->codec_ready = 1;
}

/* ------------------------------------------------- 3. a packet to a picture */

/*
 * Puts whatever the decoder produced into a slot, converted.
 *
 * This is the last thing a decode thread does and the furthest it reaches. No
 * tree, no handle, no image table: a buffer of pixels and the time it belongs
 * at, and the node collects it later.
 *
 * Called with the lock held, and with a slot already free: the caller checks
 * that before it decodes, because a picture with nowhere to go is a picture
 * lost rather than delayed.
 */
static int32_t schultz_video_fill_slot(schultz_video_data *video,
                                       uint64_t when_ns)
{
    vpx_codec_iter_t iter = NULL;
    const vpx_image_t *picture;
    int32_t filled = 0;

    while ((picture = vpx_codec_get_frame(&video->codec_ctx, &iter)) != NULL) {
        size_t words = (size_t)picture->d_w * picture->d_h;
        schultz_video_slot *slot;

        if (picture->fmt != VPX_IMG_FMT_I420) {
            continue;           /* profile 0 is all this ships */
        }
        if (video->slots[video->fill].full) {
            /*
             * One packet can yield more than one picture. The caller made
             * room for the first; the rest wait for the next pass, which is
             * why this returns rather than blocking with a picture in hand.
             */
            return filled;
        }
        slot = &video->slots[video->fill];
        if (words > slot->words) {
            uint32_t *grown = (uint32_t *)realloc(slot->argb,
                                                  words * sizeof(*grown));
            if (grown == NULL) {
                return filled;
            }
            slot->argb  = grown;
            slot->words = words;
        }
        schultz_video_i420_to_argb(picture->planes[VPX_PLANE_Y],
                                   picture->stride[VPX_PLANE_Y],
                                   picture->planes[VPX_PLANE_U],
                                   picture->stride[VPX_PLANE_U],
                                   picture->planes[VPX_PLANE_V],
                                   picture->stride[VPX_PLANE_V],
                                   slot->argb, picture->d_w, picture->d_h);
        slot->width   = picture->d_w;
        slot->height  = picture->d_h;
        slot->when_ns = when_ns;
        slot->full    = 1;
        video->fill   = (video->fill + 1u) % SCHULTZ_VIDEO_SLOTS;
        filled = 1;
    }
    return filled;
}

/* ------------------------------------------------- 3b. a packet to sound */

/*
 * Decodes one Opus packet onto the end of the waiting buffer.
 *
 * Called with the lock held, on whichever thread is decoding. Like the
 * pictures, the sound stops here: handing it to the sound system means
 * touching a handle, and that belongs to the thread that advances the tree.
 *
 * 5760 is the most samples a channel can carry in one Opus packet, which is
 * 120 milliseconds at 48000. Asking for room for that is what the decoder
 * requires; anything less and it refuses the packet rather than truncating.
 */
static void schultz_video_fill_sound(schultz_video_data *video,
                                     const unsigned char *data, size_t length)
{
    const int most = 5760;
    size_t room = (size_t)most * video->channels;
    size_t want = video->pcm_filled + room;
    int got;

    if (video->opus == NULL) {
        return;
    }
    if (want > video->pcm_words) {
        float *grown = (float *)realloc(video->pcm, want * sizeof(*grown));

        if (grown == NULL) {
            return;
        }
        video->pcm       = grown;
        video->pcm_words = want;
    }
    got = opus_multistream_decode_float(video->opus, data, (opus_int32)length,
                                        video->pcm + video->pcm_filled, most,
                                        0);
    if (got <= 0) {
        return;
    }
    /*
     * The encoder padded the front of the stream and the head says by how
     * much. Those samples are not part of the recording and playing them is
     * an audible click.
     */
    if (video->pre_skip > 0u) {
        uint32_t drop = (uint32_t)got;

        if (drop > video->pre_skip) {
            drop = video->pre_skip;
        }
        video->pre_skip -= drop;
        got -= (int)drop;
        if (got > 0) {
            memmove(video->pcm + video->pcm_filled,
                    video->pcm + video->pcm_filled + drop * video->channels,
                    (size_t)got * video->channels * sizeof(*video->pcm));
        }
    }
    video->pcm_filled   += (size_t)got * video->channels;
    video->sound_frames += (uint64_t)got;
}

/* How far into the film the sound has been decoded. */
static uint64_t schultz_video_sound_reach(const schultz_video_data *video)
{
    if (!video->have_sound || video->rate == 0u) {
        return 0u;
    }
    return video->sound_base_ns +
           video->sound_frames * 1000000000u / video->rate;
}

/* Whether the sound is far enough ahead that reading can stop. */
static int32_t schultz_video_sound_full(const schultz_video_data *video)
{
    if (!video->have_sound) {
        return 1;               /* nothing to keep ahead */
    }
    return schultz_video_sound_reach(video) >=
           video->clock_ns + SCHULTZ_VIDEO_SOUND_AHEAD_NS;
}

/*
 * Throws away sound that belongs to where the film used to be.
 *
 * The stream itself is not emptied here, because emptying it means touching a
 * handle and this is reached from the decode thread when a film loops. The
 * flag says what has to happen, and the next turn does it.
 */
static void schultz_video_drop_sound(schultz_video_data *video, uint64_t at_ns)
{
    video->pcm_filled    = 0u;
    video->sound_written = 0u;
    video->sound_frames  = 0u;
    video->sound_base_ns = at_ns;
    video->sound_reset   = 1;
    video->rebase        = 1;
}

/*
 * Opens what has arrived again from the beginning, and returns to where
 * playback had got to.
 *
 * This is what a stream needs. A WebM file can be opened as soon as its
 * header has arrived, which is a few hundred bytes, long before there is a
 * cluster to read. The parser then has a view of a file that stops before any
 * media, and winding it back with nestegg_read_reset does not recover: it
 * restores the state it was in, which is the state that had nothing to read.
 *
 * Starting over does recover, because every byte written is still here. The
 * cost is parsing the header again, which is small and stops entirely once
 * the file is complete and reads start succeeding.
 */
static void schultz_video_reopen(schultz_video_data *video)
{
    uint64_t was_at = video->clock_ns;

    if (video->demux != NULL) {
        nestegg_destroy(video->demux);
        video->demux = NULL;
    }
    schultz_video_try_open(video);
    /*
     * Back to where it had got to, by time. Not once the middle has been let
     * go of: a seek by time hunts for its target through the clusters, and
     * there is now a hole for it to fall into. Opening has already taken up
     * at the first cluster still held, which is where it should carry on
     * from anyway.
     */
    if (video->demux != NULL && was_at > 0u &&
        video->resume == video->head) {
        nestegg_track_seek(video->demux, video->track, was_at);
    }
    video->have_next = 0;
}

/* Lets go of every picture read but not yet decoded. */
static void schultz_video_drop_ahead(schultz_video_data *video)
{
    while (video->ahead_count > 0u) {
        nestegg_free_packet(video->ahead[video->ahead_out]);
        video->ahead[video->ahead_out] = NULL;
        video->ahead_out = (video->ahead_out + 1u) % SCHULTZ_VIDEO_AHEAD;
        video->ahead_count--;
    }
    video->ahead_in  = 0u;
    video->ahead_out = 0u;
}

/* Empties the slots. Whatever is in them belongs to where the film was. */
static void schultz_video_drop_slots(schultz_video_data *video)
{
    uint32_t i;

    for (i = 0; i < SCHULTZ_VIDEO_SLOTS; i++) {
        video->slots[i].full = 0;
    }
    video->fill  = 0u;
    video->drain = 0u;
    schultz_video_drop_ahead(video);
}

/*
 * Starts again from the top, which is what looping and replaying both need.
 *
 * By timestamp rather than by offset. nestegg_offset_seek only understands
 * the start of a cluster, and byte zero is the file header rather than a
 * cluster, so seeking there leaves the parser unable to read anything.
 */
static void schultz_video_rewind(schultz_video_data *video)
{
    if (video->demux != NULL) {
        nestegg_track_seek(video->demux, video->track, 0u);
    }
    schultz_video_drop_slots(video);
    schultz_video_drop_sound(video, 0u);
    video->clock_ns   = 0u;
    video->have_next  = 0;
    video->have_last  = 0;      /* going round again is meant to repeat */
    video->ended      = 0;
    video->starved_at = video->size;
}

/*
 * Reads and decodes everything the clock has reached.
 *
 * A packet carries the time it should be shown at. The clock advances with
 * the tree's, so a film plays at its own speed rather than at the display's,
 * and a slow decode falls behind rather than running fast.
 */
/* Decodes one picture that was read ahead, into a slot that has come free. */
static int32_t schultz_video_decode_ahead(schultz_video_data *video)
{
    nestegg_packet *packet;
    uint64_t tstamp;
    unsigned int chunks = 0u;
    unsigned int chunk;
    int32_t drew = 0;

    if (video->ahead_count == 0u || video->slots[video->fill].full) {
        return 0;
    }
    packet = video->ahead[video->ahead_out];
    tstamp = video->ahead_ns[video->ahead_out];
    video->ahead[video->ahead_out] = NULL;
    video->ahead_out = (video->ahead_out + 1u) % SCHULTZ_VIDEO_AHEAD;
    video->ahead_count--;

    if (nestegg_packet_count(packet, &chunks) == 0) {
        for (chunk = 0; chunk < chunks; chunk++) {
            unsigned char *data = NULL;
            size_t length = 0u;

            if (nestegg_packet_data(packet, chunk, &data, &length) != 0) {
                continue;
            }
            /* The decoder takes an unsigned int. A packet longer than one
             * holds would arrive as a smaller number than the buffer really
             * is, so it is skipped instead. */
            if (length > UINT_MAX) {
                continue;
            }
            if (vpx_codec_decode(&video->codec_ctx, data,
                                 (unsigned int)length, NULL, 0)
                    == VPX_CODEC_OK) {
                drew |= schultz_video_fill_slot(video, tstamp);
            }
        }
    }
    nestegg_free_packet(packet);
    return drew;
}

/* Puts a picture aside until there is a slot for it. */
static void schultz_video_keep_ahead(schultz_video_data *video,
                                     nestegg_packet *packet, uint64_t tstamp)
{
    video->ahead[video->ahead_in]    = packet;
    video->ahead_ns[video->ahead_in] = tstamp;
    video->ahead_at[video->ahead_in] = video->packet_at;
    video->ahead_in = (video->ahead_in + 1u) % SCHULTZ_VIDEO_AHEAD;
    video->ahead_count++;
}

/*
 * Reads and decodes as far as the slots and the sound allow.
 *
 * Sound and picture are interleaved in one file, so keeping the sound buffer
 * full means reading past pictures there is nowhere to put yet. Those are
 * kept as they came off the disk and decoded when a slot comes free, which
 * costs a few kilobytes each rather than the megabytes a decoded one costs.
 *
 * Reading stops when the sound is far enough ahead and there is nowhere to
 * put another picture. A film with no sound stops as soon as the slots are
 * full, which is what it did before any of this.
 */
static int32_t schultz_video_advance(schultz_video_data *video)
{
    int32_t drew = 0;
    int guard = 0;

    /* What was read ahead is older than anything still in the file. */
    while (schultz_video_decode_ahead(video)) {
        drew = 1;
    }

    /*
     * The guard is not a frame limit, it is a runaway stop: a file whose
     * timestamps do not advance would otherwise spin here forever.
     */
    while (guard++ < 256) {
        nestegg_packet *packet = NULL;
        unsigned int track = 0u;
        uint64_t tstamp = 0u;
        int32_t room_for_picture;
        int read;

        if (video->stop) {
            break;
        }
        /*
         * Somewhere to put a picture, if the next packet is one. A free slot
         * has already taken from the queue above, so what is left to ask is
         * whether the queue itself has room.
         */
        room_for_picture = video->ahead_count < SCHULTZ_VIDEO_AHEAD;
        if (!room_for_picture) {
            break;              /* the next picture would have nowhere to go */
        }
        if (schultz_video_sound_full(video) &&
            (video->slots[video->fill].full || video->ahead_count > 0u)) {
            break;              /* sound is ahead, pictures are stacked up */
        }

        /*
         * Between calls into the demuxer, which is the only safe moment: it
         * holds no pointer into the buffer, only a cursor this side of the
         * io callbacks, and that cursor is an offset into the stream rather
         * than into the allocation.
         *
         * Here rather than only where bytes arrive, because what may be let
         * go of is decided by what has been decoded, and that happens here.
         */
        schultz_video_stream_trim(video);
        video->packet_at = video->at;
        read = nestegg_read_packet(video->demux, &packet);
        if (read <= 0 || packet == NULL) {
            /*
             * Nothing came back. Either the file has ended or it has not
             * finished arriving, and those look identical from here. More
             * bytes since the last time this happened means the second, so
             * the parser is wound back to where it was and asked again.
             */
            if (video->file == NULL && video->size > video->starved_at) {
                video->starved_at = video->size;
                schultz_video_reopen(video);
                if (video->demux != NULL) {
                    continue;
                }
            }
            video->ended = 1;
            if (video->looping) {
                schultz_video_rewind(video);
                continue;
            }
            break;
        }
        if (nestegg_packet_track(packet, &track) != 0) {
            nestegg_free_packet(packet);
            continue;
        }
        if (video->have_sound && track == video->sound_track) {
            unsigned int piece;
            unsigned int pieces = 0u;

            /*
             * The first sound after a move says where the film really is.
             * Taking that rather than the time that was asked for is what
             * keeps picture and sound together: they are the same recording,
             * so whatever this packet says, the pictures around it say too.
             */
            if (video->rebase &&
                nestegg_packet_tstamp(packet, &tstamp) == 0) {
                video->sound_base_ns = tstamp;
                video->clock_ns      = tstamp;
                video->rebase        = 0;
            }
            if (nestegg_packet_count(packet, &pieces) == 0) {
                for (piece = 0; piece < pieces; piece++) {
                    unsigned char *data = NULL;
                    size_t length = 0u;

                    if (nestegg_packet_data(packet, piece, &data,
                                            &length) == 0) {
                        schultz_video_fill_sound(video, data, length);
                    }
                }
            }
            nestegg_free_packet(packet);
            continue;
        }
        if (track != video->track) {
            nestegg_free_packet(packet);
            continue;           /* somebody else's track */
        }
        if (nestegg_packet_tstamp(packet, &tstamp) == 0) {
            /*
             * The same for a film with no sound, where the pictures are the
             * only thing that can say where the file landed.
             */
            if (video->rebase && !video->have_sound) {
                video->clock_ns = tstamp;
                video->rebase   = 0;
            }
            video->next_ns   = tstamp;
            video->have_next = 1;
            if (video->have_last && tstamp <= video->last_ns) {
                /* Already seen, before a reopen wound the file back. */
                nestegg_free_packet(packet);
                continue;
            }
            video->last_ns   = tstamp;
            video->have_last = 1;
        }
        if (!video->slots[video->fill].full && video->ahead_count == 0u) {
            unsigned int chunks = 0u;
            unsigned int chunk;

            if (nestegg_packet_count(packet, &chunks) == 0) {
                for (chunk = 0; chunk < chunks; chunk++) {
                    unsigned char *data = NULL;
                    size_t length = 0u;

                    if (nestegg_packet_data(packet, chunk, &data,
                                            &length) != 0) {
                        continue;
                    }
                    if (length > UINT_MAX) {
                        continue;
                    }
                    if (vpx_codec_decode(&video->codec_ctx, data,
                                         (unsigned int)length, NULL, 0)
                            == VPX_CODEC_OK) {
                        drew |= schultz_video_fill_slot(video, tstamp);
                    }
                }
            }
            nestegg_free_packet(packet);
        } else {
            schultz_video_keep_ahead(video, packet, tstamp);
        }
    }
    return drew;
}

/*
 * The decode thread, when there is one.
 *
 * It decodes ahead until the slots are full, waits until one is emptied, and
 * goes again. Everything it touches is under the lock and listed at the top
 * of this file.
 */
static int schultz_video_thread(void *data)
{
    schultz_video_data *video = (schultz_video_data *)data;

    SDL_LockMutex(video->lock);
    while (!video->stop) {
        if (!video->running) {
            SDL_WaitCondition(video->room, video->lock);
            continue;
        }
        if (video->demux == NULL) {
            schultz_video_try_open(video);
            if (video->demux == NULL) {
                /*
                 * Not enough has arrived. Waiting on the condition rather
                 * than spinning: a write signals it, which is what wakes this.
                 */
                SDL_WaitCondition(video->room, video->lock);
                continue;
            }
        }
        if (video->ended && !video->looping) {
            SDL_WaitCondition(video->room, video->lock);
            continue;
        }
        if (!schultz_video_advance(video) && !video->stop) {
            /*
             * Nothing came of that pass: the file has ended, or the slots are
             * full, or more bytes are wanted. All three are waits rather than
             * spins.
             *
             * Not when the answer is that this thread is finishing. The
             * broadcast that says so has already been sent, so waiting for
             * another would be waiting forever.
             */
            SDL_WaitCondition(video->room, video->lock);
        }
    }
    SDL_UnlockMutex(video->lock);
    return 0;
}

/*
 * Takes the picture whose time has come and hands it to the image table.
 *
 * This is stage five and it is always on the thread that advances the tree,
 * which is why it is the only place a handle is made.
 *
 * A picture that is late is shown anyway and a picture that is early is left
 * alone. Skipping ahead when several are late is what keeps a slow decode
 * from playing in slow motion.
 */
static int32_t schultz_video_publish(schultz_tree *tree, schultz_handle node,
                                     schultz_video_data *video)
{
    int32_t shown = 0;

    schultz_video_hold(video);
    /*
     * The first picture goes up whatever time it carries. A film need not
     * start at zero -- a clip cut out of a longer one often does not, and the
     * one here starts 42 milliseconds in -- and a player at rest showing a
     * hole until somebody presses play looks broken.
     */
    while (video->slots[video->drain].full &&
           (video->shown == 0u ||
            video->slots[video->drain].when_ns <= video->clock_ns)) {
        schultz_video_slot *slot = &video->slots[video->drain];
        schultz_handle fresh = SCHULTZ_HANDLE_NONE;

        if (video->images != NULL &&
            schultz_image_set_pixels(video->images, slot->argb, slot->width,
                                     slot->height, 0u, &fresh) == SCHULTZ_OK) {
            /*
             * The one before goes now. A film is thirty of these a second and
             * the table would otherwise grow a handle a frame for as long as
             * it plays.
             */
            if (video->frame != SCHULTZ_HANDLE_NONE) {
                schultz_image_unload(video->images, video->frame);
            }
            video->frame  = fresh;
            video->width  = slot->width;
            video->height = slot->height;
            video->shown++;
            shown = 1;
        }
        slot->full  = 0;
        video->drain = (video->drain + 1u) % SCHULTZ_VIDEO_SLOTS;
        if (video->room != NULL) {
            SDL_SignalCondition(video->room);
        }
    }
    schultz_video_release(video);
    if (shown) {
        schultz_node_invalidate(tree, node);
    }
    return shown;
}

/* ---------------------------------------------------- 5b. sound going out */

/*
 * Hands whatever sound has been decoded to the sound system.
 *
 * Always on the thread that advances the tree, because a stream is a handle.
 * The stream is made on the first turn there is anything to write: a film
 * with no sound never makes one.
 */
static void schultz_video_hand_over_sound(const schultz_tree *tree,
                                          schultz_video_data *video)
{
    if (!video->have_sound) {
        return;
    }
    /*
     * Asked for here rather than remembered from when the node was made.
     *
     * A host creates its sound system and its widgets in whatever order suits
     * it, and plenty of them make the window and its tree first. Reading the
     * tree once at creation turned that into an ordering rule nothing stated:
     * a video node built before schultz_tree_set_audio kept a NULL and played
     * silently for the rest of its life, with no error and with
     * schultz_video_has_sound reporting there was no sound to play.
     *
     * Only while there is no stream yet. Once one exists it belongs to the
     * system that made it, and every call that touches it is already guarded
     * on the handle, so the pointer has to keep pointing at the system that
     * owns the handle rather than at whatever the tree holds now.
     */
    if (video->sound == SCHULTZ_HANDLE_NONE) {
        video->audio = (schultz_audio *)schultz_tree_audio(tree);
    }
    if (video->audio == NULL) {
        return;
    }
    schultz_video_hold(video);
    if (video->sound_reset) {
        if (video->sound != SCHULTZ_HANDLE_NONE) {
            schultz_audio_stream_clear(video->audio, video->sound);
        }
        video->sound_reset = 0;
    }
    if (video->pcm_filled > 0u) {
        if (video->sound == SCHULTZ_HANDLE_NONE) {
            if (schultz_audio_stream_create(video->audio, SCHULTZ_AUDIO_F32,
                                            video->channels, video->rate,
                                            &video->sound) != SCHULTZ_OK) {
                video->have_sound = 0;  /* no device; play it silently */
                video->pcm_filled = 0u;
                schultz_video_release(video);
                return;
            }
            schultz_audio_stream_set_volume(video->audio, video->sound,
                                            video->muted ? 0.0f
                                                         : video->volume);
        }
        if (schultz_audio_stream_write(video->audio, video->sound, video->pcm,
                                       (uint64_t)(video->pcm_filled *
                                                  sizeof(*video->pcm)))
                == SCHULTZ_OK) {
            video->sound_written += video->pcm_filled / video->channels;
        }
        video->pcm_filled = 0u;
    }
    schultz_video_release(video);
    if (video->sound != SCHULTZ_HANDLE_NONE && video->playing &&
        !schultz_audio_stream_is_playing(video->audio, video->sound)) {
        schultz_audio_stream_play(video->audio, video->sound);
    }
}

/*
 * Where the sound has actually got to, which is where the film is.
 *
 * What has been handed over less what is still waiting, which is exact
 * because a stream counts what is waiting in the format it was written in.
 * Taking the sound as the clock is what keeps picture and sound together: a
 * decode that falls behind drops pictures instead of drifting.
 */
static uint64_t schultz_video_sound_clock(const schultz_video_data *video)
{
    uint64_t per_frame = (uint64_t)video->channels * sizeof(float);
    uint64_t waiting = schultz_audio_stream_queued(video->audio, video->sound);
    uint64_t frames = (per_frame == 0u) ? 0u : waiting / per_frame;
    uint64_t played = (video->sound_written > frames)
                    ? video->sound_written - frames : 0u;

    return video->sound_base_ns + played * 1000000000u / video->rate;
}

/* ------------------------------------------------------------ 6. controls */

/*
 * The row of controls. They are child nodes rather than something painted
 * here, so they take the theme, take keyboard focus and are read out by the
 * accessibility layer exactly as any other button and slider are.
 *
 * The row sits in a BorderPane's bottom slot, which spans the full width at
 * whatever height the row needs. The picture is painted across the node's
 * whole bounds underneath it, which is what every player looks like.
 */

/* Minutes and seconds, the way a player writes them. */
static void schultz_video_clock_text(uint64_t at_ms, char *out, size_t size)
{
    uint64_t seconds = at_ms / 1000u;

    snprintf(out, size, "%u:%02u", (unsigned)(seconds / 60u),
             (unsigned)(seconds % 60u));
}

/* Elapsed and total together, which is one label rather than two. */
static void schultz_video_time_text(const schultz_video_data *video,
                                    char *out, size_t size)
{
    char at[16];
    char total[16];

    schultz_video_clock_text(video->clock_ns / 1000000u, at, sizeof(at));
    if (video->duration_ns > 0u) {
        schultz_video_clock_text(video->duration_ns / 1000000u, total,
                                 sizeof(total));
        snprintf(out, size, "%s / %s", at, total);
    } else {
        snprintf(out, size, "%s", at);
    }
}

/* Takes the whole row away, which is what asking for no controls means. */
static void schultz_video_clear_controls(schultz_tree *tree,
                                         schultz_video_data *video)
{
    if (video->row != SCHULTZ_HANDLE_NONE) {
        schultz_node_destroy(tree, video->row);
    }
    video->row           = SCHULTZ_HANDLE_NONE;
    video->play_button   = SCHULTZ_HANDLE_NONE;
    video->stop_button   = SCHULTZ_HANDLE_NONE;
    video->position      = SCHULTZ_HANDLE_NONE;
    video->time_label    = SCHULTZ_HANDLE_NONE;
    video->mute_button   = SCHULTZ_HANDLE_NONE;
    video->volume_slider = SCHULTZ_HANDLE_NONE;
    video->controls      = SCHULTZ_VIDEO_CONTROLS_NONE;
}

/* Builds the row, in the order a player puts them in. */
static int32_t schultz_video_build_controls(schultz_tree *tree,
                                            schultz_handle node,
                                            schultz_video_data *video,
                                            uint32_t controls)
{
    schultz_layout_params params;
    int32_t result;

    result = schultz_node_create(tree, node, &video->row);
    if (result != SCHULTZ_OK) {
        video->row = SCHULTZ_HANDLE_NONE;
        return result;
    }
    schultz_node_set_pane(tree, video->row, schultz_pane_hbox());
    schultz_node_set_spacing(tree, video->row, 8.0f, 8.0f);
    schultz_layout_params_default(&params);
    params.slot = SCHULTZ_SLOT_BOTTOM;
    schultz_node_set_layout_params(tree, video->row, &params);

    if (controls & SCHULTZ_VIDEO_CONTROL_PLAY) {
        schultz_button_create(tree, video->row, "Play", &video->play_button);
    }
    if (controls & SCHULTZ_VIDEO_CONTROL_STOP) {
        schultz_button_create(tree, video->row, "Stop", &video->stop_button);
    }
    if (controls & SCHULTZ_VIDEO_CONTROL_POSITION) {
        /*
         * A thousandth of the film, so the bar keeps its range when a
         * different film of a different length is opened into the same node.
         */
        schultz_slider_create(tree, video->row, SCHULTZ_ORIENT_HORIZONTAL,
                              0.0f, 1000.0f, 0.0f, &video->position);
        schultz_layout_params_default(&params);
        params.grow  = SCHULTZ_GROW_ALWAYS;
        params.align = SCHULTZ_ALIGN_CENTER;
        schultz_node_set_layout_params(tree, video->position, &params);
        /*
         * A bar over a stream shows where the film is and refuses a drag,
         * because the bytes behind it are gone.
         */
        if (video->file == NULL) {
            schultz_node_set_state(tree, video->position,
                                   SCHULTZ_STATE_VISIBLE);
        }
    }
    if (controls & SCHULTZ_VIDEO_CONTROL_TIME) {
        char text[40];

        schultz_video_time_text(video, text, sizeof(text));
        schultz_label_create(tree, video->row, text, &video->time_label);
    }
    if (controls & SCHULTZ_VIDEO_CONTROL_MUTE) {
        schultz_button_create(tree, video->row,
                              video->muted ? "Sound" : "Mute",
                              &video->mute_button);
    }
    if (controls & SCHULTZ_VIDEO_CONTROL_VOLUME) {
        schultz_slider_create(tree, video->row, SCHULTZ_ORIENT_HORIZONTAL,
                              0.0f, 1.0f, video->volume,
                              &video->volume_slider);
        schultz_node_set_pref_size(tree, video->volume_slider, 80.0f,
                                   SCHULTZ_SIZE_UNSET);
        schultz_node_set_max_size(tree, video->volume_slider, 80.0f,
                                  SCHULTZ_SIZE_UNSET);
    }
    video->controls       = controls;
    video->position_set   = 0.0f;
    video->volume_set     = video->volume;
    /* Nothing any of these can be, so the first turn writes all three. */
    video->time_shown     = (uint64_t)-1;
    video->showed_playing = -1;
    video->showed_muted   = -1;
    return SCHULTZ_OK;
}

/*
 * Brings the row up to date with the film, once a turn.
 *
 * The sliders are read as well as written, which is how a drag is noticed: a
 * value that is not the one last written there came from the person.
 */
static void schultz_video_follow_controls(schultz_tree *tree,
                                          schultz_handle node,
                                          schultz_video_data *video)
{
    uint64_t at_ms = video->clock_ns / 1000000u;
    uint64_t total = video->duration_ns / 1000000u;

    /*
     * Only when it changes. Setting a caption throws the old string away,
     * reshapes the new one and invalidates the layout, so writing the same
     * word sixty times a second would re-lay out the whole window for
     * nothing -- and a node showing controls is ticked whether it is playing
     * or not.
     */
    if (video->play_button != SCHULTZ_HANDLE_NONE &&
        video->showed_playing != video->playing) {
        schultz_label_set_text(tree,
                               schultz_button_label(tree, video->play_button),
                               video->playing ? "Pause" : "Play");
        video->showed_playing = video->playing;
    }
    if (video->position != SCHULTZ_HANDLE_NONE && total > 0u) {
        float now = schultz_slider_value(tree, video->position);

        if (now != video->position_set && video->file != NULL) {
            /* Dragged. Where it was put is where the film goes. */
            schultz_video_seek(tree, node,
                               (uint64_t)(now / 1000.0f * (float)total));
            video->position_set = now;
        } else {
            float where = (float)at_ms / (float)total * 1000.0f;

            if (where > 1000.0f) { where = 1000.0f; }
            schultz_slider_set_value(tree, video->position, where);
            video->position_set = schultz_slider_value(tree, video->position);
        }
    }
    if (video->volume_slider != SCHULTZ_HANDLE_NONE) {
        float now = schultz_slider_value(tree, video->volume_slider);

        if (now != video->volume_set) {
            schultz_video_set_volume(tree, node, now);
            video->volume_set = now;
        }
    }
    if (video->mute_button != SCHULTZ_HANDLE_NONE &&
        video->showed_muted != video->muted) {
        schultz_label_set_text(tree,
                               schultz_button_label(tree, video->mute_button),
                               video->muted ? "Sound" : "Mute");
        video->showed_muted = video->muted;
    }
    /*
     * The text only changes once a second, and rewriting it means reshaping
     * it, so the second it last said is remembered.
     */
    if (video->time_label != SCHULTZ_HANDLE_NONE &&
        at_ms / 1000u != video->time_shown) {
        char text[40];

        schultz_video_time_text(video, text, sizeof(text));
        schultz_label_set_text(tree, video->time_label, text);
        video->time_shown = at_ms / 1000u;
    }
}

/* A press on one of the buttons. The row is made of ordinary nodes, so their
 * clicks arrive here on the way up the tree. */
static int32_t schultz_video_event(schultz_tree *tree, schultz_handle node,
                                   const schultz_event *event)
{
    schultz_video_data *video = schultz_video_of(tree, node);

    if (video == NULL || event->type != SCHULTZ_EVENT_CLICK) {
        return SCHULTZ_OK;
    }
    if (event->target == video->play_button) {
        if (video->playing) {
            schultz_video_pause(tree, node);
        } else {
            schultz_video_play(tree, node);
        }
        return SCHULTZ_EVENT_CONSUMED;
    }
    if (event->target == video->stop_button) {
        schultz_video_stop(tree, node);
        return SCHULTZ_EVENT_CONSUMED;
    }
    if (event->target == video->mute_button) {
        schultz_video_set_muted(tree, node, !video->muted);
        return SCHULTZ_EVENT_CONSUMED;
    }
    return SCHULTZ_OK;
}

/* ------------------------------------------------------------- the widget */

/* One pass: the sound out, the clock on, and whatever is due on the screen. */
static int32_t schultz_video_turn(schultz_tree *tree, schultz_handle node,
                                  schultz_video_data *video,
                                  uint32_t elapsed_ms)
{
    if (video->controls != SCHULTZ_VIDEO_CONTROLS_NONE) {
        schultz_video_follow_controls(tree, node, video);
    }
    if (video->gave_up) {
        return 0;
    }
    schultz_video_hand_over_sound(tree, video);
    /*
     * The clock only moves while playing, which is the whole of what pausing
     * does: a paused film keeps its slots and its place and shows nothing new.
     *
     * Sound is the clock when there is sound. Otherwise the tree's is, which
     * means a silent film plays at its own speed and a slow decode falls
     * behind rather than running fast.
     */
    if (video->have_sound && video->sound != SCHULTZ_HANDLE_NONE) {
        video->clock_ns = schultz_video_sound_clock(video);
    } else if (video->playing) {
        video->clock_ns += (uint64_t)elapsed_ms * 1000000u;
    }
    if (video->thread != NULL) {
        /*
         * With a thread the decoding is already happening. All that is left
         * here is waking it, in case it ran out of slots or bytes, and taking
         * whatever has come due.
         */
        schultz_video_hold(video);
        SDL_SignalCondition(video->room);
        schultz_video_release(video);
        return schultz_video_publish(tree, node, video);
    }
    /*
     * Stopped, with its picture already up: nothing to do. Stopped without
     * one yet is different, because the first picture goes up whether or not
     * anybody has pressed play.
     */
    if (!video->playing && video->shown > 0u) {
        return 0;
    }
    if (video->demux == NULL) {
        schultz_video_try_open(video);
        if (video->demux == NULL) {
            return 0;           /* still waiting for enough bytes */
        }
    }
    if (!(video->ended && !video->looping)) {
        schultz_video_advance(video);
    }
    return schultz_video_publish(tree, node, video);
}

static int32_t schultz_video_tick(schultz_tree *tree, schultz_handle node,
                                  uint64_t now_ms, uint32_t elapsed_ms)
{
    schultz_video_data *video = schultz_video_of(tree, node);
    int32_t drew;

    (void)now_ms;
    if (video == NULL) {
        return 0;
    }
    drew = schultz_video_turn(tree, node, video, elapsed_ms);
    /*
     * A node that is not playing and shows no controls has nothing left to
     * do once its first picture is up, so it stops asking to be ticked. That
     * is what lets a film sitting there stopped cost nothing at all.
     */
    if (!video->playing && video->shown > 0u &&
        video->controls == SCHULTZ_VIDEO_CONTROLS_NONE) {
        schultz_node_set_animating(tree, node, 0);
    }
    return drew;
}

/*
 * The picture, fitted inside the node and centred, keeping its proportions.
 * A node the wrong shape gets bars rather than a stretched picture, because
 * a stretched picture looks like a bug and bars look like a choice.
 */
static int32_t schultz_video_paint(schultz_tree *tree, schultz_handle node,
                                   schultz_draw_list *list,
                                   schultz_arena *arena)
{
    const schultz_video_data *video = schultz_video_of(tree, node);
    schultz_rect bounds;
    schultz_rect into;
    float scale;

    (void)arena;
    if (video == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    if (video->frame == SCHULTZ_HANDLE_NONE || video->width == 0u ||
        video->height == 0u) {
        return SCHULTZ_OK;      /* nothing yet: draw nothing, not a hole */
    }

    scale = bounds.width / (float)video->width;
    if ((float)video->height * scale > bounds.height) {
        scale = bounds.height / (float)video->height;
    }
    into = schultz_rect_make(
        bounds.x + (bounds.width - (float)video->width * scale) * 0.5f,
        bounds.y + (bounds.height - (float)video->height * scale) * 0.5f,
        (float)video->width * scale, (float)video->height * scale);

    return schultz_draw_image(list, video->frame,
                              schultz_rect_make(0.0f, 0.0f, 0.0f, 0.0f), into,
                              255u);
}

static void schultz_video_destroy(void *data)
{
    schultz_video_data *video = (schultz_video_data *)data;
    uint32_t i;

    if (video == NULL) {
        return;
    }
    if (video->thread != NULL) {
        /*
         * Asked to finish, then waited for. A thread that is asleep on a full
         * set of slots has to be woken to see the flag at all.
         */
        schultz_video_hold(video);
        video->stop = 1;
        SDL_BroadcastCondition(video->room);
        schultz_video_release(video);
        SDL_WaitThread(video->thread, NULL);
    }
    if (video->codec_ready) {
        vpx_codec_destroy(&video->codec_ctx);
    }
    if (video->demux != NULL) {
        nestegg_destroy(video->demux);
    }
    if (video->images != NULL && video->frame != SCHULTZ_HANDLE_NONE) {
        schultz_image_unload(video->images, video->frame);
    }
    if (video->file != NULL) {
        fclose(video->file);
    }
    if (video->opus != NULL) {
        opus_multistream_decoder_destroy(video->opus);
    }
    if (video->audio != NULL && video->sound != SCHULTZ_HANDLE_NONE) {
        schultz_audio_stream_destroy(video->audio, video->sound);
    }
    free(video->pcm);
    schultz_video_drop_ahead(video);
    for (i = 0; i < SCHULTZ_VIDEO_SLOTS; i++) {
        free(video->slots[i].argb);
    }
    if (video->room != NULL) {
        SDL_DestroyCondition(video->room);
    }
    if (video->lock != NULL) {
        SDL_DestroyMutex(video->lock);
    }
    free(video->bytes);
    free(video);
}

static const schultz_widget_vtable schultz_video_widget = {
    .paint = schultz_video_paint, .event = schultz_video_event,
    .tick = schultz_video_tick, .destroy = schultz_video_destroy
};

/* ------------------------------------------------------------ the interface */

int32_t schultz_video_create(schultz_tree *tree, schultz_handle parent,
                             uint32_t threads, schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_video_data *video;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    video = (schultz_video_data *)calloc(1, sizeof(*video));
    if (video == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    video->frame    = SCHULTZ_HANDLE_NONE;
    video->sound    = SCHULTZ_HANDLE_NONE;
    video->volume   = 1.0f;
    video->rate     = 48000u;
    video->channels = 2u;
    video->images   = (schultz_image_table *)schultz_tree_image_table(tree);
    /*
     * No sound system read here on purpose. It is read when sound is first
     * wanted, so that a host may set one after building its widgets. See
     * schultz_video_hand_over_sound.
     */
    video->threads  = threads;
    if (threads > 0u) {
        video->lock = SDL_CreateMutex();
        video->room = SDL_CreateCondition();
        if (video->lock != NULL && video->room != NULL) {
            video->thread = SDL_CreateThread(schultz_video_thread,
                                             "schultz video", video);
        }
        if (video->thread == NULL) {
            /*
             * The system would not give one. Decoding on the calling thread
             * is slower but correct, so the node is still worth having.
             */
            if (video->room != NULL) {
                SDL_DestroyCondition(video->room);
                video->room = NULL;
            }
            if (video->lock != NULL) {
                SDL_DestroyMutex(video->lock);
                video->lock = NULL;
            }
            video->threads = 0u;
        }
    }

    schultz_node_set_widget(tree, node, &schultz_video_widget, video);
    video->row           = SCHULTZ_HANDLE_NONE;
    video->play_button   = SCHULTZ_HANDLE_NONE;
    video->stop_button   = SCHULTZ_HANDLE_NONE;
    video->position      = SCHULTZ_HANDLE_NONE;
    video->time_label    = SCHULTZ_HANDLE_NONE;
    video->mute_button   = SCHULTZ_HANDLE_NONE;
    video->volume_slider = SCHULTZ_HANDLE_NONE;
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_IMAGE);
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_video_write(schultz_tree *tree, schultz_handle node,
                            const void *bytes, uint64_t length)
{
    schultz_video_data *video = schultz_video_of(tree, node);

    if (bytes == NULL && length != 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (video == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (length == 0u) {
        return SCHULTZ_OK;
    }
    /*
     * The length comes from outside as a uint64_t. Refused before it is
     * added to what is already held, because a value near the top wraps the
     * sum into a small number, the buffer is grown to that, and the copy
     * below still writes the length that was asked for.
     */
    if (length > SIZE_MAX - video->size) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_video_hold(video);
    /* Let go of what nothing wants before making room for more. */
    schultz_video_stream_trim(video);
    if (schultz_video_stream_held(video) + (size_t)length > video->capacity) {
        size_t needed = schultz_video_stream_held(video) + (size_t)length;
        size_t want = video->capacity ? video->capacity : 65536u;
        unsigned char *grown;

        /* Doubling, and stopping short of a wrap rather than looping for
         * ever or arriving at zero. */
        while (want < needed) {
            if (want > SIZE_MAX / 2u) {
                want = needed;
                break;
            }
            want *= 2u;
        }
        grown = (unsigned char *)realloc(video->bytes, want);
        if (grown == NULL) {
            schultz_video_release(video);
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        video->bytes    = grown;
        video->capacity = want;
    }
    memcpy(video->bytes + schultz_video_stream_held(video), bytes,
           (size_t)length);
    video->size += (size_t)length;
    /*
     * A read that found nothing set this, because a stream still arriving and
     * a file that has ended look the same from the parser. A byte arriving is
     * the proof that it was the first, so the end is taken back.
     */
    video->ended   = 0;
    video->running = 1;
    if (video->room != NULL) {
        SDL_SignalCondition(video->room);   /* a starved thread can go again */
    }
    schultz_video_release(video);
    /* As in schultz_video_open_file: enough to get the first picture up. */
    schultz_node_set_animating(tree, node, 1);
    return SCHULTZ_OK;
}

int32_t schultz_video_open_file(schultz_tree *tree, schultz_handle node,
                                const char *path)
{
    schultz_video_data *video = schultz_video_of(tree, node);
    FILE *file;

    if (path == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (video == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return SCHULTZ_ERR_UNREADABLE;
    }

    schultz_video_hold(video);
    /*
     * Everything the node held belonged to whatever it was playing before,
     * including the decoder: a VP8 file after a VP9 one needs a different
     * one built.
     */
    if (video->demux != NULL) {
        nestegg_destroy(video->demux);
        video->demux = NULL;
    }
    if (video->codec_ready) {
        vpx_codec_destroy(&video->codec_ctx);
        video->codec_ready = 0;
    }
    if (video->opus != NULL) {
        opus_multistream_decoder_destroy(video->opus);
        video->opus = NULL;
    }
    video->have_sound = 0;
    schultz_video_drop_sound(video, 0u);
    if (video->file != NULL) {
        fclose(video->file);
    }
    free(video->bytes);
    video->bytes    = NULL;
    video->size     = 0u;
    video->capacity = 0u;
    video->file      = file;
    video->at        = 0u;
    video->head      = 0u;
    video->resume    = 0u;
    video->packet_at = 0u;
    video->codec    = (uint32_t)SCHULTZ_VIDEO_CODEC_NONE;
    video->gave_up  = 0;
    schultz_video_drop_slots(video);
    video->clock_ns    = 0u;
    video->duration_ns = 0u;
    video->have_next   = 0;
    video->have_last   = 0;
    video->ended       = 0;
    video->starved_at  = 0u;
    /*
     * Decoding starts now rather than at the first play, so the first picture
     * is on screen before anyone presses anything. A film that is not playing
     * shows its opening frame, which is what a player looks like at rest.
     */
    video->running = 1;
    schultz_video_try_open(video);
    if (video->room != NULL) {
        SDL_SignalCondition(video->room);
    }
    schultz_video_release(video);
    /*
     * Ticked from here, so the first picture reaches the screen without
     * anyone pressing play. The turn that shows it is the one that stops
     * this again.
     */
    schultz_node_set_animating(tree, node, 1);
    return SCHULTZ_OK;
}

uint64_t schultz_video_held(const schultz_tree *tree, schultz_handle node)
{
    schultz_video_data *video = (schultz_video_data *)
        schultz_video_of(tree, node);
    uint64_t held;

    if (video == NULL || video->file != NULL) {
        return 0u;
    }
    schultz_video_hold(video);
    held = (uint64_t)schultz_video_stream_held(video);
    schultz_video_release(video);
    return held;
}

uint64_t schultz_video_queued(const schultz_tree *tree, schultz_handle node)
{
    schultz_video_data *video = (schultz_video_data *)
        schultz_video_of(tree, node);
    uint64_t left;

    if (video == NULL) {
        return 0u;
    }
    schultz_video_hold(video);
    left = (video->at >= video->size)
         ? 0u : (uint64_t)(video->size - video->at);
    schultz_video_release(video);
    return left;
}

int32_t schultz_video_play(schultz_tree *tree, schultz_handle node)
{
    schultz_video_data *video = schultz_video_of(tree, node);

    if (video == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_video_hold(video);
    if (video->ended && !video->looping) {
        schultz_video_rewind(video);
    }
    video->playing = 1;
    video->running = 1;
    if (video->room != NULL) {
        SDL_SignalCondition(video->room);    /* wake a thread that was idle */
    }
    schultz_video_release(video);
    if (video->sound != SCHULTZ_HANDLE_NONE) {
        schultz_audio_stream_play(video->audio, video->sound);
    }
    return schultz_node_set_animating(tree, node, 1);
}

int32_t schultz_video_pause(schultz_tree *tree, schultz_handle node)
{
    schultz_video_data *video = schultz_video_of(tree, node);

    if (video == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_video_hold(video);
    video->playing = 0;
    /*
     * The decode thread is left running. It fills its two slots and waits, so
     * resuming shows a picture at once instead of waiting for one, and a
     * paused film costs two frames of memory rather than a thread doing
     * nothing.
     */
    schultz_video_release(video);
    if (video->sound != SCHULTZ_HANDLE_NONE) {
        schultz_audio_stream_pause(video->audio, video->sound);
    }
    /*
     * A node showing controls keeps being ticked while paused, because the
     * controls have to keep saying what is true: the button has to read Play
     * again, and a drag on the bar has to be noticed.
     */
    return schultz_node_set_animating(
        tree, node, video->controls != SCHULTZ_VIDEO_CONTROLS_NONE);
}

int32_t schultz_video_stop(schultz_tree *tree, schultz_handle node)
{
    schultz_video_data *video = schultz_video_of(tree, node);

    if (video == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_video_hold(video);
    video->playing = 0;
    if (video->file != NULL && video->demux != NULL) {
        schultz_video_rewind(video);
        schultz_video_drop_sound(video, 0u);
    }
    schultz_video_release(video);
    if (video->sound != SCHULTZ_HANDLE_NONE) {
        schultz_audio_stream_pause(video->audio, video->sound);
    }
    return schultz_node_set_animating(
        tree, node, video->controls != SCHULTZ_VIDEO_CONTROLS_NONE);
}

int32_t schultz_video_is_playing(const schultz_tree *tree,
                                 schultz_handle node)
{
    const schultz_video_data *video = schultz_video_of(tree, node);

    return (video != NULL && video->playing) ? 1 : 0;
}

int32_t schultz_video_set_looping(schultz_tree *tree, schultz_handle node,
                                  int32_t looping)
{
    schultz_video_data *video = schultz_video_of(tree, node);

    if (video == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_video_hold(video);
    video->looping = looping ? 1 : 0;
    if (video->looping && video->room != NULL) {
        SDL_SignalCondition(video->room);    /* the end is no longer the end */
    }
    schultz_video_release(video);
    return SCHULTZ_OK;
}

int32_t schultz_video_can_seek(const schultz_tree *tree, schultz_handle node)
{
    const schultz_video_data *video = schultz_video_of(tree, node);

    return (video != NULL && video->file != NULL) ? 1 : 0;
}

int32_t schultz_video_seek(schultz_tree *tree, schultz_handle node,
                           uint64_t at_ms)
{
    schultz_video_data *video = schultz_video_of(tree, node);
    uint64_t at_ns = at_ms * 1000000u;

    (void)tree;
    if (video == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (video->file == NULL || video->demux == NULL) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    schultz_video_hold(video);
    if (nestegg_track_seek(video->demux, video->track, at_ns) != 0) {
        schultz_video_release(video);
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    /*
     * The slots hold pictures from where the film used to be, so they go.
     * `have_last` goes with them: it exists to drop packets already seen, and
     * after a move backwards every packet is one of those.
     */
    schultz_video_drop_slots(video);
    schultz_video_drop_sound(video, at_ns);
    video->clock_ns  = at_ns;
    video->have_next = 0;
    video->have_last = 0;
    video->ended     = 0;
    if (video->room != NULL) {
        SDL_SignalCondition(video->room);
    }
    schultz_video_release(video);
    return SCHULTZ_OK;
}

uint64_t schultz_video_position(const schultz_tree *tree, schultz_handle node)
{
    const schultz_video_data *video = schultz_video_of(tree, node);

    return (video == NULL) ? 0u : video->clock_ns / 1000000u;
}

uint64_t schultz_video_duration(const schultz_tree *tree, schultz_handle node)
{
    schultz_video_data *video = (schultz_video_data *)
        schultz_video_of(tree, node);
    uint64_t length;

    if (video == NULL) {
        return 0u;
    }
    schultz_video_hold(video);
    length = video->duration_ns / 1000000u;
    schultz_video_release(video);
    return length;
}

int32_t schultz_video_set_controls(schultz_tree *tree, schultz_handle node,
                                   uint32_t controls)
{
    schultz_video_data *video = schultz_video_of(tree, node);
    int32_t result;

    if (video == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_video_clear_controls(tree, video);
    if (controls == SCHULTZ_VIDEO_CONTROLS_NONE) {
        schultz_node_set_animating(tree, node, video->playing);
        return SCHULTZ_OK;
    }
    /*
     * The pane goes on with the first set of controls rather than at create,
     * so a node that never shows any never carries one.
     */
    schultz_node_set_pane(tree, node, schultz_pane_border());
    result = schultz_video_build_controls(tree, node, video, controls);
    if (result == SCHULTZ_OK) {
        schultz_node_set_animating(tree, node, 1);
    }
    return result;
}

uint32_t schultz_video_controls(const schultz_tree *tree, schultz_handle node)
{
    const schultz_video_data *video = schultz_video_of(tree, node);

    return (video == NULL) ? (uint32_t)SCHULTZ_VIDEO_CONTROLS_NONE
                           : video->controls;
}

int32_t schultz_video_has_sound(const schultz_tree *tree, schultz_handle node)
{
    const schultz_video_data *video = schultz_video_of(tree, node);
    const void *audio;

    if (video == NULL || !video->have_sound) {
        return 0;
    }
    /*
     * The tree's answer until there is a stream, which is what makes this
     * true as soon as a sound system exists rather than only after the first
     * turn that handed sound over.
     */
    audio = (video->sound == SCHULTZ_HANDLE_NONE)
                ? schultz_tree_audio(tree) : (const void *)video->audio;
    return (audio != NULL) ? 1 : 0;
}

int32_t schultz_video_set_volume(schultz_tree *tree, schultz_handle node,
                                 float volume)
{
    schultz_video_data *video = schultz_video_of(tree, node);

    if (video == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (volume < 0.0f) { volume = 0.0f; }
    if (volume > 1.0f) { volume = 1.0f; }
    video->volume = volume;
    if (video->sound != SCHULTZ_HANDLE_NONE && !video->muted) {
        schultz_audio_stream_set_volume(video->audio, video->sound, volume);
    }
    if (video->volume_slider != SCHULTZ_HANDLE_NONE) {
        schultz_slider_set_value(tree, video->volume_slider, volume);
        video->volume_set = volume;
    }
    return SCHULTZ_OK;
}

float schultz_video_volume(const schultz_tree *tree, schultz_handle node)
{
    const schultz_video_data *video = schultz_video_of(tree, node);

    return (video == NULL) ? 1.0f : video->volume;
}

int32_t schultz_video_set_muted(schultz_tree *tree, schultz_handle node,
                                int32_t muted)
{
    schultz_video_data *video = schultz_video_of(tree, node);

    (void)tree;
    if (video == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    video->muted = muted ? 1 : 0;
    if (video->sound != SCHULTZ_HANDLE_NONE) {
        schultz_audio_stream_set_volume(video->audio, video->sound,
                                        video->muted ? 0.0f : video->volume);
    }
    return SCHULTZ_OK;
}

int32_t schultz_video_is_muted(const schultz_tree *tree, schultz_handle node)
{
    const schultz_video_data *video = schultz_video_of(tree, node);

    return (video != NULL && video->muted) ? 1 : 0;
}

int32_t schultz_video_size(const schultz_tree *tree, schultz_handle node,
                           uint32_t *out_width, uint32_t *out_height)
{
    schultz_video_data *video = (schultz_video_data *)
        schultz_video_of(tree, node);

    if (out_width == NULL || out_height == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_width  = 0u;
    *out_height = 0u;
    if (video == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_video_hold(video);
    *out_width  = video->width;
    *out_height = video->height;
    schultz_video_release(video);
    return SCHULTZ_OK;
}

uint32_t schultz_video_codec(const schultz_tree *tree, schultz_handle node)
{
    schultz_video_data *video = (schultz_video_data *)
        schultz_video_of(tree, node);
    uint32_t codec;

    if (video == NULL) {
        return (uint32_t)SCHULTZ_VIDEO_CODEC_NONE;
    }
    schultz_video_hold(video);
    codec = video->codec;
    schultz_video_release(video);
    return codec;
}

uint64_t schultz_video_frames_shown(const schultz_tree *tree,
                                    schultz_handle node)
{
    const schultz_video_data *video = schultz_video_of(tree, node);

    return (video == NULL) ? 0u : video->shown;
}

/* -------------------------------------------------- 7. the other direction */

/*
 * Encoding is the mirror of everything above, and much shorter, because the
 * hard parts of playing a film -- timing, buffering, keeping sound and
 * picture together -- are the caller's problem on the way out. Pictures go
 * in, packets come out, and what happens to them is somebody else's business.
 */

/** What an encoder carries. */
struct schultz_video_encoder {
    vpx_codec_ctx_t  ctx;
    vpx_codec_enc_cfg_t cfg;
    uint32_t         codec;     /**< One of SCHULTZ_VIDEO_CODEC_*. */
    int32_t          ready;
    vpx_image_t      picture;   /**< The I420 the encoder is given. */
    int32_t          have_picture;
    uint32_t         width;
    uint32_t         height;
    uint32_t         bitrate;   /**< Bits a second, as asked for. */
    uint32_t         speed;     /**< Zero to SCHULTZ_VIDEO_SPEED_MOST. */
    int32_t          want_keyframe;

    /*
     * Where the last packet was copied to. libvpx's own buffer is good until
     * the next call into the encoder, and the interface promises the same, so
     * a copy is what makes that promise keepable whatever libvpx does next.
     */
    unsigned char   *packet;
    size_t           packet_size;
    size_t           packet_length;
    uint64_t         packet_when_ns;
    int32_t          packet_keyframe;

    vpx_codec_iter_t iter;      /**< Where read_packet has got to. */
    uint64_t         last_ns;
    int32_t          have_last;
};

/*
 * Premultiplied ARGB to I420, which is what the encoder wants.
 *
 * The exact inverse of schultz_video_i420_to_argb above, in the same BT.601
 * studio swing and the same 8.8 fixed point. Chroma is averaged over each two
 * by two block, because that is what half size chroma planes mean; taking one
 * pixel of the four instead is faster and visibly worse on anything with a
 * sharp coloured edge in it.
 */
static void schultz_video_argb_to_i420(const uint32_t *argb, uint32_t width,
                                       uint32_t height, unsigned char *y,
                                       int y_stride, unsigned char *u,
                                       int u_stride, unsigned char *v,
                                       int v_stride)
{
    uint32_t row;
    uint32_t col;

    for (row = 0; row < height; row++) {
        const uint32_t *from = argb + (size_t)row * width;
        unsigned char *y_row = y + (size_t)row * (size_t)y_stride;

        for (col = 0; col < width; col++) {
            int r = (int)((from[col] >> 16) & 0xFFu);
            int g = (int)((from[col] >> 8) & 0xFFu);
            int b = (int)(from[col] & 0xFFu);

            y_row[col] = (unsigned char)
                (((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    for (row = 0; row + 1 < height + 1; row += 2) {
        unsigned char *u_row = u + (size_t)(row >> 1) * (size_t)u_stride;
        unsigned char *v_row = v + (size_t)(row >> 1) * (size_t)v_stride;

        for (col = 0; col + 1 < width + 1; col += 2) {
            int red = 0;
            int green = 0;
            int blue = 0;
            int taken = 0;
            uint32_t dy;
            uint32_t dx;
            int cb;
            int cr;

            for (dy = 0; dy < 2u; dy++) {
                if (row + dy >= height) {
                    continue;   /* an odd height has half a block at the end */
                }
                for (dx = 0; dx < 2u; dx++) {
                    uint32_t pixel;

                    if (col + dx >= width) {
                        continue;
                    }
                    pixel = argb[(size_t)(row + dy) * width + col + dx];
                    red   += (int)((pixel >> 16) & 0xFFu);
                    green += (int)((pixel >> 8) & 0xFFu);
                    blue  += (int)(pixel & 0xFFu);
                    taken++;
                }
            }
            if (taken == 0) {
                continue;
            }
            red /= taken;
            green /= taken;
            blue /= taken;
            cb = ((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128;
            cr = ((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128;
            if (cb < 0)   { cb = 0; }
            if (cb > 255) { cb = 255; }
            if (cr < 0)   { cr = 0; }
            if (cr > 255) { cr = 255; }
            u_row[col >> 1] = (unsigned char)cb;
            v_row[col >> 1] = (unsigned char)cr;
        }
    }
}

int32_t schultz_video_encoder_create(uint32_t codec, uint32_t width,
                                     uint32_t height, uint32_t per_second,
                                     uint32_t bitrate,
                                     schultz_video_encoder **out_encoder)
{
    schultz_video_encoder *encoder;
    vpx_codec_iface_t *iface;

    if (out_encoder == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_encoder = NULL;
    if (width == 0u || height == 0u || per_second == 0u || bitrate == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (codec == (uint32_t)SCHULTZ_VIDEO_CODEC_VP9) {
        iface = vpx_codec_vp9_cx();
    } else if (codec == (uint32_t)SCHULTZ_VIDEO_CODEC_VP8) {
        iface = vpx_codec_vp8_cx();
    } else {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    encoder = (schultz_video_encoder *)calloc(1, sizeof(*encoder));
    if (encoder == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    encoder->codec = codec;
    if (vpx_codec_enc_config_default(iface, &encoder->cfg, 0)
            != VPX_CODEC_OK) {
        free(encoder);
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    encoder->cfg.g_w = width;
    encoder->cfg.g_h = height;
    /*
     * Milliseconds, so a picture's time can be handed over as it came from
     * the clock rather than converted into whatever the frame rate implies.
     * libvpx only uses the timebase to work out how long a picture lasts.
     */
    encoder->cfg.g_timebase.num = 1;
    encoder->cfg.g_timebase.den = 1000;
    encoder->cfg.rc_target_bitrate = bitrate / 1000u;
    if (encoder->cfg.rc_target_bitrate == 0u) {
        encoder->cfg.rc_target_bitrate = 1u;  /* libvpx counts in kilobits */
    }
    /*
     * One picture in, one answer out, and never a picture held back to look
     * at the next one first. A call cannot wait for the future, and lag is
     * the one thing people notice immediately.
     */
    encoder->cfg.g_lag_in_frames = 0;
    encoder->cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
    encoder->cfg.rc_end_usage = VPX_CBR;
    encoder->cfg.g_threads = 1;

    if (vpx_codec_enc_init(&encoder->ctx, iface, &encoder->cfg, 0)
            != VPX_CODEC_OK) {
        free(encoder);
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    encoder->ready = 1;
    /*
     * Near the quick end, because this is for a call and lag is what people
     * notice first. libvpx's slow end looks for the best picture it can find
     * and will spend far longer than a frame doing it, which is right for a
     * file being prepared once and wrong for something somebody is waiting
     * on. schultz_video_encoder_set_speed is how a host says otherwise.
     */
    schultz_video_encoder_set_speed(encoder, 8u);
    if (vpx_img_alloc(&encoder->picture, VPX_IMG_FMT_I420, width, height, 1)
            == NULL) {
        vpx_codec_destroy(&encoder->ctx);
        free(encoder);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    encoder->have_picture = 1;
    encoder->width   = width;
    encoder->height  = height;
    encoder->bitrate = bitrate;
    (void)per_second;
    *out_encoder = encoder;
    return SCHULTZ_OK;
}

void schultz_video_encoder_destroy(schultz_video_encoder *encoder)
{
    if (encoder == NULL) {
        return;
    }
    if (encoder->have_picture) {
        vpx_img_free(&encoder->picture);
    }
    if (encoder->ready) {
        vpx_codec_destroy(&encoder->ctx);
    }
    free(encoder->packet);
    free(encoder);
}

int32_t schultz_video_encoder_write_frame(schultz_video_encoder *encoder,
                                          const uint32_t *argb,
                                          uint32_t width, uint32_t height,
                                          uint64_t when_ns)
{
    uint64_t when_ms;
    vpx_enc_frame_flags_t flags = 0;

    if (encoder == NULL || argb == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (width != encoder->width || height != encoder->height) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    when_ms = when_ns / 1000000u;
    if (encoder->have_last && when_ms <= encoder->last_ns) {
        /*
         * Not later than the one before. Refused rather than nudged forward,
         * because a caller handing over pictures out of order has a bug and
         * quietly renumbering them hides it until a decoder somewhere else
         * cannot play the result.
         */
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    schultz_video_argb_to_i420(argb, width, height,
                               encoder->picture.planes[VPX_PLANE_Y],
                               encoder->picture.stride[VPX_PLANE_Y],
                               encoder->picture.planes[VPX_PLANE_U],
                               encoder->picture.stride[VPX_PLANE_U],
                               encoder->picture.planes[VPX_PLANE_V],
                               encoder->picture.stride[VPX_PLANE_V]);

    if (encoder->want_keyframe) {
        flags |= VPX_EFLAG_FORCE_KF;
        encoder->want_keyframe = 0;
    }
    /*
     * One millisecond as the duration rather than the real one. VP8 uses it
     * only for rate control and a real gap would make a dropped picture look
     * like a long one; the bitrate is what actually governs the size.
     */
    if (vpx_codec_encode(&encoder->ctx, &encoder->picture,
                         (vpx_codec_pts_t)when_ms, 1, flags,
                         VPX_DL_REALTIME) != VPX_CODEC_OK) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    encoder->last_ns   = when_ms;
    encoder->have_last = 1;
    encoder->iter      = NULL;  /* read_packet starts from the first again */
    return SCHULTZ_OK;
}

int32_t schultz_video_encoder_read_packet(schultz_video_encoder *encoder,
                                          const void **out_bytes,
                                          uint64_t *out_length,
                                          uint64_t *out_when_ns,
                                          int32_t *out_keyframe)
{
    const vpx_codec_cx_pkt_t *packet;

    if (encoder == NULL || out_bytes == NULL || out_length == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_bytes  = NULL;
    *out_length = 0u;

    while ((packet = vpx_codec_get_cx_data(&encoder->ctx, &encoder->iter))
               != NULL) {
        if (packet->kind != VPX_CODEC_CX_FRAME_PKT) {
            continue;           /* statistics and the like, not a picture */
        }
        if (packet->data.frame.sz > encoder->packet_size) {
            unsigned char *grown = (unsigned char *)
                realloc(encoder->packet, packet->data.frame.sz);

            if (grown == NULL) {
                return SCHULTZ_ERR_OUT_OF_MEMORY;
            }
            encoder->packet      = grown;
            encoder->packet_size = packet->data.frame.sz;
        }
        memcpy(encoder->packet, packet->data.frame.buf,
               packet->data.frame.sz);
        encoder->packet_length   = packet->data.frame.sz;
        encoder->packet_when_ns  = (uint64_t)packet->data.frame.pts *
                                   1000000u;
        encoder->packet_keyframe =
            (packet->data.frame.flags & VPX_FRAME_IS_KEY) ? 1 : 0;

        *out_bytes  = encoder->packet;
        *out_length = (uint64_t)encoder->packet_length;
        if (out_when_ns != NULL)  { *out_when_ns = encoder->packet_when_ns; }
        if (out_keyframe != NULL) { *out_keyframe = encoder->packet_keyframe; }
        return SCHULTZ_OK;
    }
    return SCHULTZ_ERR_EXHAUSTED;
}

int32_t schultz_video_encoder_set_bitrate(schultz_video_encoder *encoder,
                                          uint32_t bitrate)
{
    if (encoder == NULL || bitrate == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    encoder->cfg.rc_target_bitrate = bitrate / 1000u;
    if (encoder->cfg.rc_target_bitrate == 0u) {
        encoder->cfg.rc_target_bitrate = 1u;
    }
    if (vpx_codec_enc_config_set(&encoder->ctx, &encoder->cfg)
            != VPX_CODEC_OK) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    encoder->bitrate = bitrate;
    return SCHULTZ_OK;
}

int32_t schultz_video_encoder_force_keyframe(schultz_video_encoder *encoder)
{
    if (encoder == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    encoder->want_keyframe = 1;
    return SCHULTZ_OK;
}

uint32_t schultz_video_encoder_bitrate(const schultz_video_encoder *encoder)
{
    return (encoder == NULL) ? 0u : encoder->bitrate;
}

uint32_t schultz_video_encoder_codec(const schultz_video_encoder *encoder)
{
    return (encoder == NULL) ? (uint32_t)SCHULTZ_VIDEO_CODEC_NONE
                             : encoder->codec;
}

int32_t schultz_video_encoder_set_speed(schultz_video_encoder *encoder,
                                        uint32_t speed)
{
    int effort;

    if (encoder == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (speed > (uint32_t)SCHULTZ_VIDEO_SPEED_MOST) {
        speed = (uint32_t)SCHULTZ_VIDEO_SPEED_MOST;
    }
    /*
     * libvpx counts this the same way round -- higher is quicker -- so the
     * number goes straight through. The two codecs stop at different points,
     * VP9 at nine and VP8 at sixteen, so the one shared range is nought to
     * ten and VP9's top two settings are the same setting.
     */
    effort = (int)speed;
    if (encoder->codec == (uint32_t)SCHULTZ_VIDEO_CODEC_VP9 && effort > 9) {
        effort = 9;
    }
    if (vpx_codec_control(&encoder->ctx, VP8E_SET_CPUUSED, effort)
            != VPX_CODEC_OK) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    encoder->speed = speed;
    return SCHULTZ_OK;
}

uint32_t schultz_video_encoder_speed(const schultz_video_encoder *encoder)
{
    return (encoder == NULL) ? 0u : encoder->speed;
}

/* ------------------------------------------------- 8. and back again */

/** What a bare packet decoder carries. */
struct schultz_video_decoder {
    vpx_codec_ctx_t  ctx;
    int32_t          ready;
    vpx_codec_iter_t iter;
    uint32_t        *argb;
    size_t           words;
    uint32_t         width;
    uint32_t         height;
};

int32_t schultz_video_decoder_create(uint32_t codec,
                                     schultz_video_decoder **out_decoder)
{
    schultz_video_decoder *decoder;
    vpx_codec_dec_cfg_t cfg;
    vpx_codec_iface_t *iface;

    if (out_decoder == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_decoder = NULL;
    if (codec == (uint32_t)SCHULTZ_VIDEO_CODEC_VP9) {
        iface = vpx_codec_vp9_dx();
    } else if (codec == (uint32_t)SCHULTZ_VIDEO_CODEC_VP8) {
        iface = vpx_codec_vp8_dx();
    } else {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    decoder = (schultz_video_decoder *)calloc(1, sizeof(*decoder));
    if (decoder == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.threads = 1;
    if (vpx_codec_dec_init(&decoder->ctx, iface, &cfg, 0)
            != VPX_CODEC_OK) {
        free(decoder);
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    decoder->ready = 1;
    *out_decoder = decoder;
    return SCHULTZ_OK;
}

void schultz_video_decoder_destroy(schultz_video_decoder *decoder)
{
    if (decoder == NULL) {
        return;
    }
    if (decoder->ready) {
        vpx_codec_destroy(&decoder->ctx);
    }
    free(decoder->argb);
    free(decoder);
}

int32_t schultz_video_decoder_write_packet(schultz_video_decoder *decoder,
                                           const void *bytes, uint64_t length)
{
    if (decoder == NULL || bytes == NULL || length == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /* The decoder takes an unsigned int; anything longer is refused rather
     * than narrowed into a different number. */
    if (length > (uint64_t)UINT_MAX) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (vpx_codec_decode(&decoder->ctx, (const unsigned char *)bytes,
                         (unsigned int)length, NULL, 0) != VPX_CODEC_OK) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    decoder->iter = NULL;       /* read_frame starts from the first again */
    return SCHULTZ_OK;
}

int32_t schultz_video_decoder_read_frame(schultz_video_decoder *decoder,
                                         const uint32_t **out_pixels,
                                         uint32_t *out_width,
                                         uint32_t *out_height)
{
    const vpx_image_t *picture;

    if (decoder == NULL || out_pixels == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_pixels = NULL;
    while ((picture = vpx_codec_get_frame(&decoder->ctx, &decoder->iter))
               != NULL) {
        size_t words = (size_t)picture->d_w * picture->d_h;

        if (picture->fmt != VPX_IMG_FMT_I420) {
            continue;           /* profile 0 is all this ships */
        }
        if (words > decoder->words) {
            uint32_t *grown = (uint32_t *)realloc(decoder->argb,
                                                  words * sizeof(*grown));

            if (grown == NULL) {
                return SCHULTZ_ERR_OUT_OF_MEMORY;
            }
            decoder->argb  = grown;
            decoder->words = words;
        }
        schultz_video_i420_to_argb(picture->planes[VPX_PLANE_Y],
                                   picture->stride[VPX_PLANE_Y],
                                   picture->planes[VPX_PLANE_U],
                                   picture->stride[VPX_PLANE_U],
                                   picture->planes[VPX_PLANE_V],
                                   picture->stride[VPX_PLANE_V],
                                   decoder->argb, picture->d_w, picture->d_h);
        decoder->width  = picture->d_w;
        decoder->height = picture->d_h;

        *out_pixels = decoder->argb;
        if (out_width != NULL)  { *out_width = decoder->width; }
        if (out_height != NULL) { *out_height = decoder->height; }
        return SCHULTZ_OK;
    }
    return SCHULTZ_ERR_EXHAUSTED;
}
