/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_webm.c
 * @brief Writing packets into a WebM file.
 *
 * The other direction from nestegg, which reads one. Small enough to write
 * rather than vendor: WebM is a named subset of Matroska, and a file carrying
 * one video track and one sound track needs perhaps a dozen of Matroska's
 * elements. nestegg cannot do this -- it has no write functions at all -- and
 * the library that can, libwebm, is C++ and would be the only C++ dependency
 * in the toolkit's own code.
 *
 * ## What a WebM file is
 *
 * Every element is the same three things: an identifier, a length, and a body
 * that is either a value or more elements. That is EBML, and it is the whole
 * format once you know it.
 *
 * Identifiers and lengths are both variable width, which is what the two
 * write_id and write_size helpers below are for. A length has its width in
 * its leading bits -- one leading zero bit for each extra byte -- so 0x81
 * means "one byte, value 1" and 0x4001 means "two bytes, value 1".
 *
 * The file this writes is:
 *
 *     EBML                 what kind of file this is
 *     Segment              everything else, in one element
 *       Info               how time is counted, and how long the film is
 *       Tracks             what is inside: codecs, picture size, channels
 *       Cluster            a second or so of packets, starting on a keyframe
 *       Cluster            ...
 *       Cues               where each keyframe landed, so seeking works
 *
 * ## The two things that have to be patched
 *
 * The Segment's length and the film's duration are not known until the last
 * packet has been written. Both are written as placeholders on the way past
 * and filled in by seeking back at the end, which is why this writes to a
 * file rather than to a stream. A caller that needs a stream wants unknown
 * lengths and no Cues, which is a different file and not what this is for.
 */

#include "schultz_video.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How time is counted in the file: one tick is a millisecond. */
#define SCHULTZ_WEBM_TICK_NS 1000000u

/* A cluster covers about this long, unless a keyframe starts a new one. */
#define SCHULTZ_WEBM_CLUSTER_MS 1000u

/*
 * Room kept at the top of the segment for the table of contents.
 *
 * Three entries of about twenty bytes each, and the rest padded out. It has
 * to be written before the things it points at, and one of those -- the index
 * -- is not written until the file is finished, so the space is reserved on
 * the way past and filled in at the end.
 */
#define SCHULTZ_WEBM_SEEKHEAD_ROOM 128u

enum {
    SCHULTZ_WEBM_VIDEO_TRACK = 1u,
    SCHULTZ_WEBM_SOUND_TRACK = 2u
};

/** Where one keyframe landed, so a player can jump to it. */
typedef struct {
    uint64_t when_ms;
    uint64_t at;            /**< Bytes from the start of the segment body. */
} schultz_webm_cue;

struct schultz_video_writer {
    FILE    *file;
    uint32_t codec;
    uint32_t width;
    uint32_t height;
    int32_t  have_sound;
    uint32_t channels;
    int32_t  started;       /**< The header has been written. */

    long     segment_body;  /**< Positions in Cues are relative to this. */
    long     segment_size_at;
    long     duration_at;
    long     seekhead_at;   /**< The reserved table of contents. */
    uint64_t info_at;       /**< All three are relative to segment_body. */
    uint64_t tracks_at;
    uint64_t cues_at;
    int32_t  have_cues;

    /*
     * The cluster being built, held in memory because its length has to be
     * written before its contents and is only known once it is finished.
     */
    unsigned char *cluster;
    size_t         cluster_room;
    size_t         cluster_used;
    uint64_t       cluster_ms;
    int32_t        cluster_open;

    schultz_webm_cue *cues;
    uint32_t          cue_count;
    uint32_t          cue_room;

    /*
     * The last time written, kept for each track rather than for the file.
     * The two tracks interleave: a sound packet every twenty milliseconds
     * against a picture every thirty three, so a sound packet very often
     * belongs slightly before the picture written just before it. Insisting
     * on one rising time across both would refuse most of the sound, which
     * is exactly what an earlier version of this did.
     */
    uint64_t last_ms[3];
    int32_t  have_last[3];
    uint64_t longest_ms;    /**< The end of the film, for Duration. */
};

/* ------------------------------------------------------------------ EBML */

/*
 * An identifier is written exactly as it is spelled in the specification,
 * leading byte first, and the leading bits already say how long it is. So
 * there is nothing to encode: the only question is how many bytes to send.
 */
static int32_t schultz_webm_id(FILE *file, uint32_t id)
{
    unsigned char out[4];
    uint32_t bytes = 1u;
    uint32_t i;

    if (id > 0x00FFFFFFu)      { bytes = 4u; }
    else if (id > 0x0000FFFFu) { bytes = 3u; }
    else if (id > 0x000000FFu) { bytes = 2u; }
    for (i = 0; i < bytes; i++) {
        out[i] = (unsigned char)((id >> ((bytes - 1u - i) * 8u)) & 0xFFu);
    }
    return (fwrite(out, 1, bytes, file) == bytes) ? SCHULTZ_OK
                                                  : SCHULTZ_ERR_UNREADABLE;
}

/*
 * A length, in the fewest bytes that hold it.
 *
 * The leading bits say the width: one bit set in the first byte for a single
 * byte, one zero then a set bit for two, and so on. The value goes in what is
 * left, which is why each extra byte only buys seven more bits.
 */
static int32_t schultz_webm_size(FILE *file, uint64_t size, uint32_t bytes)
{
    unsigned char out[8];
    uint32_t i;

    if (bytes == 0u) {
        uint64_t limit = 0x7Fu;

        for (bytes = 1u; bytes < 8u; bytes++) {
            if (size < limit) {
                break;
            }
            limit = (limit << 7) | 0x7Fu;
        }
    }
    for (i = 0; i < bytes; i++) {
        out[i] = (unsigned char)((size >> ((bytes - 1u - i) * 8u)) & 0xFFu);
    }
    out[0] = (unsigned char)(out[0] | (0x80u >> (bytes - 1u)));
    return (fwrite(out, 1, bytes, file) == bytes) ? SCHULTZ_OK
                                                  : SCHULTZ_ERR_UNREADABLE;
}

/* A whole number, in the fewest bytes that hold it. */
static int32_t schultz_webm_number(FILE *file, uint32_t id, uint64_t value)
{
    unsigned char out[8];
    uint32_t bytes = 1u;
    uint32_t i;
    uint64_t left = value >> 8;

    while (left > 0u && bytes < 8u) {
        bytes++;
        left >>= 8;
    }
    for (i = 0; i < bytes; i++) {
        out[i] = (unsigned char)((value >> ((bytes - 1u - i) * 8u)) & 0xFFu);
    }
    if (schultz_webm_id(file, id) != SCHULTZ_OK ||
        schultz_webm_size(file, bytes, 0u) != SCHULTZ_OK) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    return (fwrite(out, 1, bytes, file) == bytes) ? SCHULTZ_OK
                                                  : SCHULTZ_ERR_UNREADABLE;
}

/* Bytes, whatever they are: a string, or a codec's private setup. */
static int32_t schultz_webm_bytes(FILE *file, uint32_t id, const void *data,
                                  size_t length)
{
    if (schultz_webm_id(file, id) != SCHULTZ_OK ||
        schultz_webm_size(file, (uint64_t)length, 0u) != SCHULTZ_OK) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    if (length == 0u) {
        return SCHULTZ_OK;
    }
    return (fwrite(data, 1, length, file) == length) ? SCHULTZ_OK
                                                     : SCHULTZ_ERR_UNREADABLE;
}

static int32_t schultz_webm_text(FILE *file, uint32_t id, const char *text)
{
    return schultz_webm_bytes(file, id, text, strlen(text));
}

/*
 * A double, big endian. Matroska stores the sample rate and the duration this
 * way, and nothing else here needs a floating point number.
 *
 * Assembled a byte at a time out of the bits rather than by casting a pointer,
 * so it does not depend on the machine storing doubles the same way round.
 */
static int32_t schultz_webm_double(FILE *file, uint32_t id, double value)
{
    unsigned char out[8];
    uint64_t bits;
    uint32_t i;

    memcpy(&bits, &value, sizeof(bits));
    for (i = 0; i < 8u; i++) {
        out[i] = (unsigned char)((bits >> ((7u - i) * 8u)) & 0xFFu);
    }
    if (schultz_webm_id(file, id) != SCHULTZ_OK ||
        schultz_webm_size(file, 8u, 0u) != SCHULTZ_OK) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    return (fwrite(out, 1, 8u, file) == 8u) ? SCHULTZ_OK
                                            : SCHULTZ_ERR_UNREADABLE;
}

/* ---------------------------------------------------------- the header */

/* An OpusHead, which is what a WebM file carries to set an Opus decoder up.
 * Laid out by RFC 7845 section 5.1, and the same nineteen bytes every time
 * bar the channel count. */
static void schultz_webm_opus_head(unsigned char *head, uint32_t channels)
{
    memcpy(head, "OpusHead", 8);
    head[8]  = 1u;                      /* version */
    head[9]  = (unsigned char)channels;
    head[10] = 0u;                      /* pre-skip, low byte */
    head[11] = 0u;
    head[12] = 0x80u;                   /* input rate 48000, little endian */
    head[13] = 0xBBu;
    head[14] = 0u;
    head[15] = 0u;
    head[16] = 0u;                      /* output gain */
    head[17] = 0u;
    head[18] = 0u;                      /* channel mapping family */
}

static int32_t schultz_webm_tracks(schultz_video_writer *writer)
{
    FILE *file = writer->file;
    long tracks_size_at;
    long tracks_body;
    long entry_size_at;
    long entry_body;
    long inner_size_at;
    long inner_body;
    long here;

    if (schultz_webm_id(file, 0x1654AE6Bu) != SCHULTZ_OK) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    tracks_size_at = ftell(file);
    if (schultz_webm_size(file, 0u, 8u) != SCHULTZ_OK) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    tracks_body = ftell(file);

    /* The picture. */
    schultz_webm_id(file, 0xAEu);
    entry_size_at = ftell(file);
    schultz_webm_size(file, 0u, 8u);
    entry_body = ftell(file);

    schultz_webm_number(file, 0xD7u, SCHULTZ_WEBM_VIDEO_TRACK);
    schultz_webm_number(file, 0x73C5u, 1u);     /* TrackUID */
    schultz_webm_number(file, 0x83u, 1u);       /* TrackType: video */
    schultz_webm_number(file, 0x9Cu, 0u);       /* no lacing */
    schultz_webm_text(file, 0x86u,
                      (writer->codec == (uint32_t)SCHULTZ_VIDEO_CODEC_VP9)
                          ? "V_VP9" : "V_VP8");
    schultz_webm_id(file, 0xE0u);               /* Video */
    inner_size_at = ftell(file);
    schultz_webm_size(file, 0u, 8u);
    inner_body = ftell(file);
    schultz_webm_number(file, 0xB0u, writer->width);
    schultz_webm_number(file, 0xBAu, writer->height);
    here = ftell(file);
    fseek(file, inner_size_at, SEEK_SET);
    schultz_webm_size(file, (uint64_t)(here - inner_body), 8u);
    fseek(file, here, SEEK_SET);

    here = ftell(file);
    fseek(file, entry_size_at, SEEK_SET);
    schultz_webm_size(file, (uint64_t)(here - entry_body), 8u);
    fseek(file, here, SEEK_SET);

    /* The sound, when there is any. */
    if (writer->have_sound) {
        unsigned char head[19];

        schultz_webm_id(file, 0xAEu);
        entry_size_at = ftell(file);
        schultz_webm_size(file, 0u, 8u);
        entry_body = ftell(file);

        schultz_webm_number(file, 0xD7u, SCHULTZ_WEBM_SOUND_TRACK);
        schultz_webm_number(file, 0x73C5u, 2u);
        schultz_webm_number(file, 0x83u, 2u);   /* TrackType: audio */
        schultz_webm_number(file, 0x9Cu, 0u);
        schultz_webm_text(file, 0x86u, "A_OPUS");
        /*
         * How much of the start a decoder should throw away, and how much it
         * should be given before a seek to warm up. Opus in WebM carries both
         * and players expect them; the pre-roll is the 80 ms Opus asks for.
         */
        schultz_webm_number(file, 0x56AAu, 0u);
        schultz_webm_number(file, 0x56BBu, 80000000u);
        schultz_webm_id(file, 0xE1u);           /* Audio */
        inner_size_at = ftell(file);
        schultz_webm_size(file, 0u, 8u);
        inner_body = ftell(file);
        schultz_webm_double(file, 0xB5u, 48000.0);
        schultz_webm_number(file, 0x9Fu, writer->channels);
        here = ftell(file);
        fseek(file, inner_size_at, SEEK_SET);
        schultz_webm_size(file, (uint64_t)(here - inner_body), 8u);
        fseek(file, here, SEEK_SET);

        schultz_webm_opus_head(head, writer->channels);
        schultz_webm_bytes(file, 0x63A2u, head, sizeof(head));

        here = ftell(file);
        fseek(file, entry_size_at, SEEK_SET);
        schultz_webm_size(file, (uint64_t)(here - entry_body), 8u);
        fseek(file, here, SEEK_SET);
    }

    here = ftell(file);
    fseek(file, tracks_size_at, SEEK_SET);
    schultz_webm_size(file, (uint64_t)(here - tracks_body), 8u);
    fseek(file, here, SEEK_SET);
    return SCHULTZ_OK;
}

/*
 * The top of the file, written when the first packet arrives rather than at
 * create, so a sound track can still be added in between.
 */
static int32_t schultz_webm_start(schultz_video_writer *writer)
{
    FILE *file = writer->file;
    long size_at;
    long body;
    long here;

    /* What kind of file this is. */
    if (schultz_webm_id(file, 0x1A45DFA3u) != SCHULTZ_OK) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    size_at = ftell(file);
    schultz_webm_size(file, 0u, 8u);
    body = ftell(file);
    schultz_webm_number(file, 0x4286u, 1u);     /* EBMLVersion */
    schultz_webm_number(file, 0x42F7u, 1u);     /* EBMLReadVersion */
    schultz_webm_number(file, 0x42F2u, 4u);     /* EBMLMaxIDLength */
    schultz_webm_number(file, 0x42F3u, 8u);     /* EBMLMaxSizeLength */
    schultz_webm_text(file, 0x4282u, "webm");   /* DocType */
    schultz_webm_number(file, 0x4287u, 2u);     /* DocTypeVersion */
    schultz_webm_number(file, 0x4285u, 2u);     /* DocTypeReadVersion */
    here = ftell(file);
    fseek(file, size_at, SEEK_SET);
    schultz_webm_size(file, (uint64_t)(here - body), 8u);
    fseek(file, here, SEEK_SET);

    /*
     * Everything else lives inside one Segment. Its length cannot be known
     * until the last packet is written, so eight bytes are reserved and
     * filled in by schultz_video_writer_close.
     */
    schultz_webm_id(file, 0x18538067u);
    writer->segment_size_at = ftell(file);
    schultz_webm_size(file, 0u, 8u);
    writer->segment_body = ftell(file);

    /*
     * Space for the table of contents, which says where Info, Tracks and the
     * index are. It goes first because that is the point of it -- a player
     * reading the top of the file learns where everything is without
     * scanning -- and it is filled in at the end because the index's position
     * is not known until then. Until then it is one big Void, which every
     * reader skips.
     */
    writer->seekhead_at = ftell(file);
    schultz_webm_id(file, 0xECu);               /* Void */
    schultz_webm_size(file, SCHULTZ_WEBM_SEEKHEAD_ROOM - 3u, 2u);
    {
        unsigned char nothing[SCHULTZ_WEBM_SEEKHEAD_ROOM];

        memset(nothing, 0, sizeof(nothing));
        fwrite(nothing, 1, SCHULTZ_WEBM_SEEKHEAD_ROOM - 3u, file);
    }

    /* How time is counted, and how long the film is. */
    writer->info_at = (uint64_t)(ftell(file) - writer->segment_body);
    schultz_webm_id(file, 0x1549A966u);
    size_at = ftell(file);
    schultz_webm_size(file, 0u, 8u);
    body = ftell(file);
    schultz_webm_number(file, 0x2AD7B1u, SCHULTZ_WEBM_TICK_NS);
    schultz_webm_text(file, 0x4D80u, "Schultz");
    schultz_webm_text(file, 0x5741u, "Schultz");
    writer->duration_at = ftell(file);
    schultz_webm_double(file, 0x4489u, 0.0);    /* filled in at the end */
    here = ftell(file);
    fseek(file, size_at, SEEK_SET);
    schultz_webm_size(file, (uint64_t)(here - body), 8u);
    fseek(file, here, SEEK_SET);

    writer->tracks_at = (uint64_t)(ftell(file) - writer->segment_body);
    if (schultz_webm_tracks(writer) != SCHULTZ_OK) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    writer->started = 1;
    return SCHULTZ_OK;
}

/* --------------------------------------------------------- the clusters */

static int32_t schultz_webm_room(schultz_video_writer *writer, size_t want)
{
    size_t needed;

    /*
     * What the cluster has to hold once this is added. Checked rather than
     * added, because a length that wraps here asks for a small buffer and is
     * then copied in at its full size, which is the whole distance from a bad
     * argument to a rewritten heap.
     */
    if (want > SIZE_MAX - writer->cluster_used) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    needed = writer->cluster_used + want;
    if (needed <= writer->cluster_room) {
        return SCHULTZ_OK;
    }
    {
        size_t room = (writer->cluster_room > 0u) ? writer->cluster_room
                                                  : 65536u;
        unsigned char *grown;

        /*
         * Doubling, and stopping if doubling would wrap. Without the second
         * test a large enough request turns this into a loop that never ends
         * or a size of zero.
         */
        while (room < needed) {
            if (room > SIZE_MAX / 2u) {
                room = needed;
                break;
            }
            room *= 2u;
        }
        grown = (unsigned char *)realloc(writer->cluster, room);
        if (grown == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        writer->cluster      = grown;
        writer->cluster_room = room;
    }
    return SCHULTZ_OK;
}

/* Writes the cluster being held out to the file, with its length in front. */
static int32_t schultz_webm_flush(schultz_video_writer *writer)
{
    FILE *file = writer->file;
    long size_at;
    long body;
    long here;

    if (!writer->cluster_open || writer->cluster_used == 0u) {
        writer->cluster_open = 0;
        writer->cluster_used = 0u;
        return SCHULTZ_OK;
    }
    if (schultz_webm_id(file, 0x1F43B675u) != SCHULTZ_OK) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    size_at = ftell(file);
    schultz_webm_size(file, 0u, 8u);
    body = ftell(file);
    schultz_webm_number(file, 0xE7u, writer->cluster_ms);    /* Timecode */
    if (fwrite(writer->cluster, 1, writer->cluster_used, file)
            != writer->cluster_used) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    here = ftell(file);
    fseek(file, size_at, SEEK_SET);
    schultz_webm_size(file, (uint64_t)(here - body), 8u);
    fseek(file, here, SEEK_SET);

    writer->cluster_open = 0;
    writer->cluster_used = 0u;
    return SCHULTZ_OK;
}

static int32_t schultz_webm_remember_cue(schultz_video_writer *writer,
                                         uint64_t when_ms, long at)
{
    if (writer->cue_count == writer->cue_room) {
        uint32_t room = (writer->cue_room > 0u) ? writer->cue_room * 2u : 64u;
        schultz_webm_cue *grown = (schultz_webm_cue *)
            realloc(writer->cues, (size_t)room * sizeof(*grown));

        if (grown == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        writer->cues    = grown;
        writer->cue_room = room;
    }
    writer->cues[writer->cue_count].when_ms = when_ms;
    writer->cues[writer->cue_count].at =
        (uint64_t)(at - writer->segment_body);
    writer->cue_count++;
    return SCHULTZ_OK;
}

/*
 * One packet, into the cluster being held.
 *
 * A SimpleBlock is the track it belongs to, how far it is from the cluster's
 * own time, one byte of flags, and then the packet. The time is a signed
 * sixteen bit number, which is why a cluster covers about a second: a longer
 * one would not be able to say when its last packet belonged.
 */
static int32_t schultz_webm_block(schultz_video_writer *writer,
                                  uint32_t track, const void *bytes,
                                  uint64_t length, uint64_t when_ms,
                                  int32_t keyframe)
{
    unsigned char head[4];
    int32_t away = (int32_t)((int64_t)when_ms - (int64_t)writer->cluster_ms);
    size_t want;
    unsigned char *at;

    if (away < -32768 || away > 32767) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    head[0] = (unsigned char)(0x80u | track);    /* the track, as a length */
    head[1] = (unsigned char)(((uint32_t)away >> 8) & 0xFFu);
    head[2] = (unsigned char)((uint32_t)away & 0xFFu);
    head[3] = (unsigned char)(keyframe ? 0x80u : 0x00u);

    /*
     * The identifier, a length of at most eight bytes, and the block. The
     * length arrives as a uint64_t from outside, so it is refused before it
     * is added to anything: a value near the top wraps the sum, and the copy
     * below would then write the size that was asked for into the buffer
     * that was not.
     */
    if (length > SIZE_MAX - (1u + 8u + sizeof(head))) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    want = 1u + 8u + sizeof(head) + (size_t)length;
    if (schultz_webm_room(writer, want) != SCHULTZ_OK) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    at = writer->cluster + writer->cluster_used;
    *at++ = 0xA3u;                               /* SimpleBlock */
    {
        uint64_t size = sizeof(head) + length;
        uint32_t width = 1u;
        uint64_t limit = 0x7Fu;
        uint32_t i;

        while (width < 8u && size >= limit) {
            width++;
            limit = (limit << 7) | 0x7Fu;
        }
        for (i = 0; i < width; i++) {
            at[i] = (unsigned char)((size >> ((width - 1u - i) * 8u)) & 0xFFu);
        }
        at[0] = (unsigned char)(at[0] | (0x80u >> (width - 1u)));
        at += width;
        writer->cluster_used += 1u + width;
    }
    memcpy(at, head, sizeof(head));
    memcpy(at + sizeof(head), bytes, (size_t)length);
    writer->cluster_used += sizeof(head) + (size_t)length;
    return SCHULTZ_OK;
}

static int32_t schultz_webm_add(schultz_video_writer *writer, uint32_t track,
                                const void *bytes, uint64_t length,
                                uint64_t when_ns, int32_t keyframe)
{
    uint64_t when_ms = when_ns / SCHULTZ_WEBM_TICK_NS;
    int32_t result;

    if (writer == NULL || bytes == NULL || length == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (!writer->started) {
        result = schultz_webm_start(writer);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    if (writer->have_last[track] && when_ms < writer->last_ms[track]) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    /*
     * A new cluster on a keyframe, or when this one has gone on long enough.
     * Starting on a keyframe is what makes the index worth having: a player
     * jumping to a cue lands on a cluster it can decode from.
     */
    if (writer->cluster_open &&
        ((keyframe && track == SCHULTZ_WEBM_VIDEO_TRACK) ||
         when_ms >= writer->cluster_ms + SCHULTZ_WEBM_CLUSTER_MS)) {
        result = schultz_webm_flush(writer);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    if (!writer->cluster_open) {
        writer->cluster_ms   = when_ms;
        writer->cluster_open = 1;
        if (keyframe && track == SCHULTZ_WEBM_VIDEO_TRACK) {
            result = schultz_webm_remember_cue(writer, when_ms,
                                               ftell(writer->file));
            if (result != SCHULTZ_OK) {
                return result;
            }
        }
    }

    result = schultz_webm_block(writer, track, bytes, length, when_ms,
                                keyframe);
    if (result != SCHULTZ_OK) {
        return result;
    }
    writer->last_ms[track]   = when_ms;
    writer->have_last[track] = 1;
    if (when_ms > writer->longest_ms) {
        writer->longest_ms = when_ms;
    }
    return SCHULTZ_OK;
}

/* ------------------------------------------------------------ the interface */

int32_t schultz_video_writer_create(const char *path, uint32_t codec,
                                    uint32_t width, uint32_t height,
                                    schultz_video_writer **out_writer)
{
    schultz_video_writer *writer;

    if (out_writer == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_writer = NULL;
    if (path == NULL || width == 0u || height == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (codec != (uint32_t)SCHULTZ_VIDEO_CODEC_VP8 &&
        codec != (uint32_t)SCHULTZ_VIDEO_CODEC_VP9) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    writer = (schultz_video_writer *)calloc(1, sizeof(*writer));
    if (writer == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    writer->file = fopen(path, "wb+");
    if (writer->file == NULL) {
        free(writer);
        return SCHULTZ_ERR_UNREADABLE;
    }
    writer->codec  = codec;
    writer->width  = width;
    writer->height = height;
    *out_writer = writer;
    return SCHULTZ_OK;
}

int32_t schultz_video_writer_add_sound(schultz_video_writer *writer,
                                       uint32_t channels)
{
    if (writer == NULL || (channels != 1u && channels != 2u)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (writer->started) {
        /* The tracks are described at the top of the file and it has already
         * been written. Nothing can be added to that list now. */
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    writer->have_sound = 1;
    writer->channels   = channels;
    return SCHULTZ_OK;
}

int32_t schultz_video_writer_write_picture(schultz_video_writer *writer,
                                           const void *bytes, uint64_t length,
                                           uint64_t when_ns, int32_t keyframe)
{
    return schultz_webm_add(writer, SCHULTZ_WEBM_VIDEO_TRACK, bytes, length,
                            when_ns, keyframe);
}

int32_t schultz_video_writer_write_sound(schultz_video_writer *writer,
                                         const void *bytes, uint64_t length,
                                         uint64_t when_ns)
{
    if (writer == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (!writer->have_sound) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    /*
     * Every Opus packet stands on its own, so every one is marked as a packet
     * that can be decoded alone. Marking them otherwise would have a player
     * hunting for a keyframe that does not exist in a sound track.
     */
    return schultz_webm_add(writer, SCHULTZ_WEBM_SOUND_TRACK, bytes, length,
                            when_ns, 1);
}

/* The index: which time is at which cluster. */
static int32_t schultz_webm_cues(schultz_video_writer *writer)
{
    FILE *file = writer->file;
    long size_at;
    long body;
    long here;
    uint32_t i;

    if (writer->cue_count == 0u) {
        return SCHULTZ_OK;
    }
    writer->cues_at = (uint64_t)(ftell(file) - writer->segment_body);
    writer->have_cues = 1;
    schultz_webm_id(file, 0x1C53BB6Bu);
    size_at = ftell(file);
    schultz_webm_size(file, 0u, 8u);
    body = ftell(file);

    for (i = 0; i < writer->cue_count; i++) {
        long point_at;
        long point_body;
        long inner_at;
        long inner_body;

        schultz_webm_id(file, 0xBBu);            /* CuePoint */
        point_at = ftell(file);
        schultz_webm_size(file, 0u, 8u);
        point_body = ftell(file);
        schultz_webm_number(file, 0xB3u, writer->cues[i].when_ms);
        schultz_webm_id(file, 0xB7u);            /* CueTrackPositions */
        inner_at = ftell(file);
        schultz_webm_size(file, 0u, 8u);
        inner_body = ftell(file);
        schultz_webm_number(file, 0xF7u, SCHULTZ_WEBM_VIDEO_TRACK);
        schultz_webm_number(file, 0xF1u, writer->cues[i].at);
        here = ftell(file);
        fseek(file, inner_at, SEEK_SET);
        schultz_webm_size(file, (uint64_t)(here - inner_body), 8u);
        fseek(file, here, SEEK_SET);

        here = ftell(file);
        fseek(file, point_at, SEEK_SET);
        schultz_webm_size(file, (uint64_t)(here - point_body), 8u);
        fseek(file, here, SEEK_SET);
    }

    here = ftell(file);
    fseek(file, size_at, SEEK_SET);
    schultz_webm_size(file, (uint64_t)(here - body), 8u);
    fseek(file, here, SEEK_SET);
    return SCHULTZ_OK;
}

/* One entry: which element, and how far into the segment it starts. */
static void schultz_webm_seek_entry(FILE *file, uint32_t id, uint64_t at)
{
    unsigned char spelled[4];
    uint32_t bytes = 1u;
    uint32_t i;
    long size_at;
    long body;
    long here;

    if (id > 0x00FFFFFFu)      { bytes = 4u; }
    else if (id > 0x0000FFFFu) { bytes = 3u; }
    else if (id > 0x000000FFu) { bytes = 2u; }
    for (i = 0; i < bytes; i++) {
        spelled[i] = (unsigned char)((id >> ((bytes - 1u - i) * 8u)) & 0xFFu);
    }
    schultz_webm_id(file, 0x4DBBu);             /* Seek */
    size_at = ftell(file);
    schultz_webm_size(file, 0u, 1u);
    body = ftell(file);
    schultz_webm_bytes(file, 0x53ABu, spelled, bytes);   /* SeekID */
    schultz_webm_number(file, 0x53ACu, at);              /* SeekPosition */
    here = ftell(file);
    fseek(file, size_at, SEEK_SET);
    schultz_webm_size(file, (uint64_t)(here - body), 1u);
    fseek(file, here, SEEK_SET);
}

/*
 * Fills in the space reserved at the top of the segment.
 *
 * Whatever is not used is left as a Void, which readers skip, so the entries
 * may be any size up to the room reserved without moving anything after them.
 */
static int32_t schultz_webm_seekhead(schultz_video_writer *writer)
{
    FILE *file = writer->file;
    long body;
    long used;
    long left;

    if (fseek(file, writer->seekhead_at, SEEK_SET) != 0) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    schultz_webm_id(file, 0x114D9B74u);         /* SeekHead */
    /*
     * Two bytes for the length, so the header is always six bytes and the
     * arithmetic below does not depend on how many entries there are.
     */
    schultz_webm_size(file, 0u, 2u);
    body = ftell(file);
    schultz_webm_seek_entry(file, 0x1549A966u, writer->info_at);
    schultz_webm_seek_entry(file, 0x1654AE6Bu, writer->tracks_at);
    if (writer->have_cues) {
        schultz_webm_seek_entry(file, 0x1C53BB6Bu, writer->cues_at);
    }
    used = ftell(file) - body;

    fseek(file, writer->seekhead_at + 4L, SEEK_SET);
    schultz_webm_size(file, (uint64_t)used, 2u);
    fseek(file, body + used, SEEK_SET);

    /*
     * The rest, as a Void. Three bytes of its own header, so anything less
     * than that left over would not fit one -- the room reserved is a round
     * number well clear of that.
     */
    left = (long)SCHULTZ_WEBM_SEEKHEAD_ROOM - (6L + used);
    if (left < 3L) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    schultz_webm_id(file, 0xECu);
    schultz_webm_size(file, (uint64_t)(left - 3L), 2u);
    {
        unsigned char nothing[SCHULTZ_WEBM_SEEKHEAD_ROOM];

        memset(nothing, 0, sizeof(nothing));
        fwrite(nothing, 1, (size_t)(left - 3L), file);
    }
    return SCHULTZ_OK;
}

int32_t schultz_video_writer_close(schultz_video_writer *writer)
{
    int32_t result = SCHULTZ_OK;

    if (writer == NULL) {
        return SCHULTZ_OK;
    }
    if (writer->started) {
        long end;

        if (schultz_webm_flush(writer) != SCHULTZ_OK ||
            schultz_webm_cues(writer) != SCHULTZ_OK) {
            result = SCHULTZ_ERR_UNREADABLE;
        }
        end = ftell(writer->file);

        /*
         * The two that could only be known now. The placeholders were left at
         * a fixed width on the way past, so filling them in is a seek and a
         * write of exactly the same number of bytes.
         */
        if (fseek(writer->file, writer->segment_size_at, SEEK_SET) != 0 ||
            schultz_webm_size(writer->file,
                              (uint64_t)(end - writer->segment_body), 8u)
                != SCHULTZ_OK) {
            result = SCHULTZ_ERR_UNREADABLE;
        }
        if (fseek(writer->file, writer->duration_at, SEEK_SET) != 0 ||
            schultz_webm_double(writer->file, 0x4489u,
                                (double)writer->longest_ms) != SCHULTZ_OK) {
            result = SCHULTZ_ERR_UNREADABLE;
        }
        if (schultz_webm_seekhead(writer) != SCHULTZ_OK) {
            result = SCHULTZ_ERR_UNREADABLE;
        }
        fseek(writer->file, end, SEEK_SET);
    }
    if (fclose(writer->file) != 0) {
        result = SCHULTZ_ERR_UNREADABLE;
    }
    free(writer->cluster);
    free(writer->cues);
    free(writer);
    return result;
}
