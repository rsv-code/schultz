/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_audio.c
 * @brief Sound out and microphone in, over SDL's audio streams.
 *
 * SDL's audio is already stream shaped: data goes in however the host has it,
 * SDL converts and resamples it to whatever the device wants, and several
 * streams bound to one device mix without being asked. So this file is thin.
 * What it adds is the shape the rest of the toolkit uses -- a system, handles
 * out of a table, plain result codes -- and the one rule that matters at a
 * language boundary: the host writes bytes, and nothing here ever calls the
 * host back for them.
 *
 * Volume is kept here as well as handed to SDL, because a stream's own volume
 * and the system's have to be multiplied together and SDL holds one number
 * per stream. Setting either recomputes what SDL is told.
 */

#include "schultz_audio.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <opus.h>

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include "schultz_handle.h"

/**
 * @brief Bytes the host has pushed, waiting for the decoder to read them.
 *
 * A plain growing buffer with a read position rather than a true ring,
 * because the decoder has to be able to go back to the beginning: it works
 * out what kind of file it is looking at by reading the first few bytes and
 * then seeking back. Once it is well past the start, what it has read is
 * dropped from the front and the buffer stops growing.
 *
 * Two threads touch this. The host writes on its own, and the decoder reads
 * on the mixer's, so everything here is done under the lock.
 */
typedef struct {
    SDL_Mutex     *lock;
    unsigned char *bytes;    /**< What is held. */
    uint64_t       size;     /**< How much room there is. */
    uint64_t       length;   /**< How much of it is filled. */
    uint64_t       at;       /**< How far the decoder has read. */
    uint64_t       dropped;  /**< Bytes thrown away from the front. */
    int32_t        finished; /**< The host says nothing more is coming. */
} schultz_audio_feed;

/** @brief What a handle from this system names. */
enum {
    SCHULTZ_AUDIO_KIND_STREAM = 0, /**< Samples the host writes. */
    SCHULTZ_AUDIO_KIND_RECORDER,   /**< Samples the microphone made. */
    SCHULTZ_AUDIO_KIND_SOUND,      /**< A file, decoded and kept. */
    SCHULTZ_AUDIO_KIND_DECODER,    /**< A file arriving in pieces. */
    SCHULTZ_AUDIO_KIND_ENCODER,    /**< Samples going out as Opus packets. */
    SCHULTZ_AUDIO_KIND_PACKETS     /**< Opus packets coming back as samples. */
};

/**
 * @brief The most sample frames one Opus packet can hold, at 48000.
 *
 * A hundred and twenty milliseconds, which is the longest frame Opus codes.
 * Nothing here produces one that long -- the encoder uses twenty -- but a
 * packet from somebody else's encoder may be, and the buffer a packet is
 * decoded into has to be big enough for the largest that can arrive.
 */
#define SCHULTZ_AUDIO_OPUS_MOST 5760u

/**
 * @brief How long one Opus packet covers, in sample frames at 48000.
 *
 * Twenty milliseconds, which is what a call uses everywhere. Shorter cuts the
 * delay and costs proportionally more header for the same sound; longer saves
 * very little and is heard as lag.
 */
#define SCHULTZ_AUDIO_OPUS_FRAMES 960u

/** @brief How much a feed keeps before it starts dropping what was read. */
#define SCHULTZ_AUDIO_FEED_KEEP (1024u * 1024u)

/** @brief One of the three things a handle can name. */
typedef struct {
    uint32_t            kind;    /**< One of the values above. */
    SDL_AudioStream    *stream;  /**< A stream or a recorder. */
    MIX_Audio          *sound;   /**< A sound, decoded and ready. */
    float               volume;  /**< Its own, before the system's is added. */
    int32_t             running; /**< Playing, or capturing. */

    /* A decoder: bytes pushed in, a reader over them, a track playing it. */
    schultz_audio_feed *feed;
    SDL_IOStream       *reader;
    MIX_Track          *track;

    /* An encoder: samples held until there are enough, and the last packet. */
    OpusEncoder        *opus;
    OpusDecoder        *opus_in;  /**< A packet decoder, rather than one. */
    uint64_t            ready_frames; /**< Decoded and not yet read. */
    int32_t             have_ready;
    float              *pcm;      /**< Waiting samples, interleaved. */
    uint64_t            pcm_frames;  /**< How many are waiting. */
    uint64_t            pcm_room;    /**< How many fit. */
    unsigned char      *packet;
    uint64_t            packet_length;
    uint64_t            sent_frames; /**< Coded so far, for the timestamp. */
    uint64_t            packet_when_ns;
    uint32_t            bitrate;

    /* A recorder saving to a file: where it is going and how much has gone. */
    SDL_IOStream       *file;
    uint64_t            written;  /**< Bytes of samples in the file. */
    uint32_t            format;   /**< What was asked for, for the header. */
    uint32_t            channels;
    uint32_t            rate;
} schultz_audio_channel;

/** @brief How many sounds may overlap. */
#define SCHULTZ_AUDIO_VOICES 8u

/**
 * @brief The system: one table and a volume.
 *
 * One table for both kinds rather than one each. Two tables number from zero
 * independently, so the first recorder and the first stream come out as the
 * same number, and a recorder handed to a stream call would find a live
 * stream and act on it. Sharing the table makes every handle unique within
 * the system, and the kind is then checked as well, so naming the wrong one
 * is an error rather than a surprise.
 */
struct schultz_audio {
    schultz_handle_table channels; /**< Streams, recorders and sounds. */
    float                volume;   /**< Everything is scaled by this. */
    int32_t              started;  /**< SDL's audio subsystem is up. */

    /*
     * The decoder, made the first time a file is asked for rather than with
     * the system. An application that only writes its own samples never needs
     * one, and opening a device it will not use is a device somebody else
     * cannot have.
     */
    /*
     * Which devices things open on. Zero is "whatever the platform says is
     * the default", which is what everything gets until an application says
     * otherwise, and which follows the platform when a headset arrives.
     */
    SDL_AudioDeviceID    output;        /**< Where sound goes. */
    SDL_AudioDeviceID    input;         /**< Where the microphone is. */
    /*
     * The device lists, as they were last handed out, so that a change can be
     * reported without anything calling the host. See
     * schultz_audio_devices_changed.
     */
    SDL_AudioDeviceID   *seen_outputs;
    int                  seen_output_count;
    SDL_AudioDeviceID   *seen_inputs;
    int                  seen_input_count;
    /* Names live until the next listing, so one may be handed out safely. */
    SDL_AudioDeviceID   *listed;
    int                  listed_count;

    int32_t              mixer_started; /**< MIX_Init has run. */
    MIX_Mixer           *mixer;         /**< NULL until a file is loaded. */
    MIX_Track           *voices[SCHULTZ_AUDIO_VOICES]; /**< Sounds play here. */
    MIX_Track           *music;         /**< The one long thing. */
    MIX_Audio           *music_audio;   /**< What it is playing. */
    float                music_volume;  /**< Its own, like a stream's. */
};

/*
 * SDL's name for one of the two formats offered here.
 *
 * Two rather than all of them because these are what anything produces, and
 * every extra one is a number a host has to look up. A host with something
 * else converts before writing, which SDL would have done anyway.
 */
static SDL_AudioFormat schultz_audio_sdl_format(uint32_t format)
{
    return (format == SCHULTZ_AUDIO_F32) ? SDL_AUDIO_F32 : SDL_AUDIO_S16;
}

static int32_t schultz_audio_format_valid(uint32_t format)
{
    return (format == SCHULTZ_AUDIO_S16 || format == SCHULTZ_AUDIO_F32);
}

/*
 * The channel a handle names, or NULL when it names none of that kind.
 *
 * The kind is part of the question. A recorder is not a stream that happens
 * to run backwards, and a host that passes one where the other belongs has a
 * bug worth being told about rather than a call that quietly works on
 * something else.
 */
static void schultz_audio_feed_free(schultz_audio_feed *feed);
static int32_t schultz_audio_need_mixer(schultz_audio *audio);
static void schultz_audio_wav_header(schultz_audio_channel *channel);

static schultz_audio_channel *schultz_audio_channel_of(
    const schultz_audio *audio, schultz_handle handle, uint32_t kind)
{
    schultz_audio_channel *channel;
    void *object = NULL;

    if (schultz_handle_table_lookup(&audio->channels, handle, &object)
            != SCHULTZ_OK) {
        return NULL;
    }
    channel = (schultz_audio_channel *)object;
    return (channel->kind == kind) ? channel : NULL;
}

/* What SDL is told, which is the two volumes multiplied. */
static void schultz_audio_apply_volume(const schultz_audio *audio,
                                       schultz_audio_channel *channel)
{
    if (channel->stream != NULL) {
        SDL_SetAudioStreamGain(channel->stream,
                               channel->volume * audio->volume);
    }
}

/*
 * Opens one logical device and wraps it in a channel.
 *
 * SDL_OpenAudioDeviceStream is the short road: it opens a logical device on
 * the physical one, makes a stream, and binds them, so several of these mix
 * without anything here arranging it. Playback and recording differ only in
 * which default device is asked for.
 */
static int32_t schultz_audio_channel_create(schultz_audio *audio,
                                            SDL_AudioDeviceID device,
                                            uint32_t format,
                                            uint32_t channels, uint32_t rate,
                                            uint32_t kind,
                                            schultz_handle *out_handle)
{
    schultz_audio_channel *channel;
    SDL_AudioSpec spec;
    int32_t result;

    if (audio == NULL || out_handle == NULL || channels == 0u || rate == 0u ||
        !schultz_audio_format_valid(format)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    spec.format   = schultz_audio_sdl_format(format);
    spec.channels = (int)channels;
    spec.freq     = (int)rate;

    channel = (schultz_audio_channel *)calloc(1u, sizeof(*channel));
    if (channel == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    channel->volume   = 1.0f;
    channel->kind     = kind;
    channel->format   = format;
    channel->channels = channels;
    channel->rate     = rate;
    channel->stream = SDL_OpenAudioDeviceStream(device, &spec, NULL, NULL);
    if (channel->stream == NULL) {
        /*
         * No device, or the platform said no. Nothing the host passed would
         * have changed it, which is what this code means.
         */
        free(channel);
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    schultz_audio_apply_volume(audio, channel);

    result = schultz_handle_table_insert(&audio->channels, channel,
                                         out_handle);
    if (result != SCHULTZ_OK) {
        SDL_DestroyAudioStream(channel->stream);
        free(channel);
        return result;
    }
    return SCHULTZ_OK;
}

static int32_t schultz_audio_channel_destroy(schultz_audio *audio,
                                             schultz_handle handle,
                                             uint32_t kind)
{
    schultz_audio_channel *channel = schultz_audio_channel_of(audio, handle,
                                                              kind);

    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* A file still open is finished properly rather than left with a header
     * saying it holds nothing. */
    if (channel->file != NULL) {
        SDL_SetAudioStreamPutCallback(channel->stream, NULL, NULL);
        schultz_audio_wav_header(channel);
        SDL_CloseIO(channel->file);
        channel->file = NULL;
    }
    /* The stream owns the logical device it was opened with, so this closes
     * both. A sound holds decoded samples instead. */
    if (channel->stream != NULL) {
        SDL_DestroyAudioStream(channel->stream);
    }
    if (channel->sound != NULL) {
        MIX_DestroyAudio(channel->sound);
    }
    /* In this order: the track can no longer be reading when the bytes it
     * reads from go away. */
    if (channel->track != NULL) {
        MIX_DestroyTrack(channel->track);
    }
    if (channel->reader != NULL) {
        SDL_CloseIO(channel->reader);
    }
    if (channel->opus != NULL) {
        opus_encoder_destroy(channel->opus);
    }
    if (channel->opus_in != NULL) {
        opus_decoder_destroy(channel->opus_in);
    }
    free(channel->pcm);
    free(channel->packet);
    schultz_audio_feed_free(channel->feed);
    free(channel);
    return schultz_handle_table_remove(&audio->channels, handle);
}

/*
 * Closes everything a table holds, for taking the whole system down.
 *
 * The slot array is walked rather than the handle interface, the way the font
 * system does it: this is teardown, and every live slot has to be released
 * whether or not a host still holds its handle.
 */
static void schultz_audio_table_clear(schultz_handle_table *table)
{
    uint32_t i;

    for (i = 0u; i < table->capacity; i++) {
        if (table->slots[i].live) {
            schultz_audio_channel *channel =
                (schultz_audio_channel *)table->slots[i].object;

            if (channel->stream != NULL) {
                SDL_DestroyAudioStream(channel->stream);
            }
            if (channel->file != NULL) {
                SDL_SetAudioStreamPutCallback(channel->stream, NULL, NULL);
                schultz_audio_wav_header(channel);
                SDL_CloseIO(channel->file);
            }
            if (channel->sound != NULL) {
                MIX_DestroyAudio(channel->sound);
            }
            if (channel->track != NULL) {
                MIX_DestroyTrack(channel->track);
            }
            if (channel->reader != NULL) {
                SDL_CloseIO(channel->reader);
            }
            if (channel->opus != NULL) {
                opus_encoder_destroy(channel->opus);
            }
            if (channel->opus_in != NULL) {
                opus_decoder_destroy(channel->opus_in);
            }
            free(channel->pcm);
            free(channel->packet);
            schultz_audio_feed_free(channel->feed);
            free(channel);
        }
    }
}

int32_t schultz_audio_create(schultz_audio **out_audio)
{
    schultz_audio *audio;
    int32_t result;

    if (out_audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_audio = NULL;

    audio = (schultz_audio *)calloc(1u, sizeof(*audio));
    if (audio == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    audio->volume       = 1.0f;
    audio->music_volume = 1.0f;
    audio->output       = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    audio->input        = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;

    /*
     * The subsystem rather than SDL itself, because a window may have started
     * SDL already and a host with no window may not have. Either way this is
     * the only thing here that knows audio needs starting.
     */
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        free(audio);
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    audio->started = 1;

    result = schultz_handle_table_init(&audio->channels, 4u);
    if (result != SCHULTZ_OK) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        free(audio);
        return result;
    }
    *out_audio = audio;
    return SCHULTZ_OK;
}

void schultz_audio_destroy(schultz_audio *audio)
{
    if (audio == NULL) {
        return;
    }
    schultz_audio_table_clear(&audio->channels);
    schultz_handle_table_free(&audio->channels);
    /*
     * The mixer owns its tracks, so taking it down takes them with it. What
     * it does not own is the song, which was loaded here.
     */
    if (audio->music_audio != NULL) {
        MIX_DestroyAudio(audio->music_audio);
    }
    if (audio->mixer != NULL) {
        MIX_DestroyMixer(audio->mixer);
    }
    if (audio->mixer_started) {
        MIX_Quit();
    }
    SDL_free(audio->listed);
    SDL_free(audio->seen_outputs);
    SDL_free(audio->seen_inputs);
    if (audio->started) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    free(audio);
}

int32_t schultz_audio_set_volume(schultz_audio *audio, float volume)
{
    uint32_t i;

    if (audio == NULL || volume < 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    audio->volume = volume;
    if (audio->mixer != NULL) {
        MIX_SetMixerGain(audio->mixer, volume);
    }

    /* Everything already playing, so one call quiets an application rather
     * than only what it starts next. */
    for (i = 0u; i < audio->channels.capacity; i++) {
        if (audio->channels.slots[i].live) {
            schultz_audio_apply_volume(audio,
                (schultz_audio_channel *)audio->channels.slots[i].object);
        }
    }
    return SCHULTZ_OK;
}

float schultz_audio_volume(const schultz_audio *audio)
{
    return (audio == NULL) ? 0.0f : audio->volume;
}

/* ------------------------------------------------------------- streaming */

int32_t schultz_audio_stream_create(schultz_audio *audio, uint32_t format,
                                    uint32_t channels, uint32_t rate,
                                    schultz_handle *out_stream)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return schultz_audio_channel_create(audio, audio->output, format,
                                        channels, rate,
                                        SCHULTZ_AUDIO_KIND_STREAM,
                                        out_stream);
}

int32_t schultz_audio_stream_write(schultz_audio *audio,
                                   schultz_handle stream, const void *bytes,
                                   uint64_t length)
{
    schultz_audio_channel *channel;

    if (audio == NULL || (bytes == NULL && length != 0u)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, stream, SCHULTZ_AUDIO_KIND_STREAM);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (length == 0u) {
        return SCHULTZ_OK;
    }
    /* Copied by SDL, so the bytes are the host's again when this returns. */
    /*
     * SDL takes an int, so a length that does not fit one would arrive as a
     * different number, or a negative one. Refused rather than narrowed:
     * truncation here feeds the wrong byte count to something that trusts
     * it.
     */
    if (length > (uint64_t)INT_MAX) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (!SDL_PutAudioStreamData(channel->stream, bytes, (int)length)) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    return SCHULTZ_OK;
}

uint64_t schultz_audio_stream_queued(const schultz_audio *audio,
                                     schultz_handle stream)
{
    schultz_audio_channel *channel;
    int queued;

    if (audio == NULL) {
        return 0u;
    }
    channel = schultz_audio_channel_of(audio, stream, SCHULTZ_AUDIO_KIND_STREAM);
    if (channel == NULL) {
        return 0u;
    }
    queued = SDL_GetAudioStreamQueued(channel->stream);
    return (queued < 0) ? 0u : (uint64_t)queued;
}

int32_t schultz_audio_stream_play(schultz_audio *audio, schultz_handle stream)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, stream, SCHULTZ_AUDIO_KIND_STREAM);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (!SDL_ResumeAudioStreamDevice(channel->stream)) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    channel->running = 1;
    return SCHULTZ_OK;
}

int32_t schultz_audio_stream_pause(schultz_audio *audio,
                                   schultz_handle stream)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, stream, SCHULTZ_AUDIO_KIND_STREAM);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    SDL_PauseAudioStreamDevice(channel->stream);
    channel->running = 0;
    return SCHULTZ_OK;
}

int32_t schultz_audio_stream_is_playing(const schultz_audio *audio,
                                        schultz_handle stream)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return 0;
    }
    channel = schultz_audio_channel_of(audio, stream, SCHULTZ_AUDIO_KIND_STREAM);
    return (channel == NULL) ? 0 : channel->running;
}

int32_t schultz_audio_stream_clear(schultz_audio *audio,
                                   schultz_handle stream)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, stream, SCHULTZ_AUDIO_KIND_STREAM);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    SDL_ClearAudioStream(channel->stream);
    return SCHULTZ_OK;
}

int32_t schultz_audio_stream_set_volume(schultz_audio *audio,
                                        schultz_handle stream, float volume)
{
    schultz_audio_channel *channel;

    if (audio == NULL || volume < 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, stream, SCHULTZ_AUDIO_KIND_STREAM);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    channel->volume = volume;
    schultz_audio_apply_volume(audio, channel);
    return SCHULTZ_OK;
}

float schultz_audio_stream_volume(const schultz_audio *audio,
                                  schultz_handle stream)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return 0.0f;
    }
    channel = schultz_audio_channel_of(audio, stream, SCHULTZ_AUDIO_KIND_STREAM);
    return (channel == NULL) ? 0.0f : channel->volume;
}

int32_t schultz_audio_stream_destroy(schultz_audio *audio,
                                     schultz_handle stream)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return schultz_audio_channel_destroy(audio, stream,
                                         SCHULTZ_AUDIO_KIND_STREAM);
}

/* -------------------------------------------------------------- devices */

/*
 * The devices of one kind, kept until the next listing.
 *
 * SDL hands back an array it allocated and expects it back. Holding the last
 * one here is what lets a name be handed to a host as a plain pointer: it
 * stays good until the next time anything asks, which is exactly how long a
 * host needs it to fill in a menu.
 */
static SDL_AudioDeviceID *schultz_audio_list(schultz_audio *audio,
                                             int32_t recording, int *count)
{
    SDL_free(audio->listed);
    audio->listed = recording ? SDL_GetAudioRecordingDevices(count)
                              : SDL_GetAudioPlaybackDevices(count);
    if (audio->listed == NULL) {
        *count = 0;
        audio->listed_count = 0;
        return NULL;
    }
    audio->listed_count = *count;
    return audio->listed;
}

uint32_t schultz_audio_output_count(schultz_audio *audio)
{
    int count = 0;

    if (audio == NULL) {
        return 0u;
    }
    schultz_audio_list(audio, 0, &count);
    return (count < 0) ? 0u : (uint32_t)count;
}

uint32_t schultz_audio_input_count(schultz_audio *audio)
{
    int count = 0;

    if (audio == NULL) {
        return 0u;
    }
    schultz_audio_list(audio, 1, &count);
    return (count < 0) ? 0u : (uint32_t)count;
}

const char *schultz_audio_device_name(schultz_audio *audio, uint32_t index)
{
    if (audio == NULL || audio->listed == NULL ||
        index >= (uint32_t)audio->listed_count) {
        return NULL;
    }
    return SDL_GetAudioDeviceName(audio->listed[index]);
}

uint64_t schultz_audio_device_id(schultz_audio *audio, uint32_t index)
{
    if (audio == NULL || audio->listed == NULL ||
        index >= (uint32_t)audio->listed_count) {
        return 0u;
    }
    return (uint64_t)audio->listed[index];
}

int32_t schultz_audio_set_output(schultz_audio *audio, uint64_t device)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    audio->output = (device == 0u) ? SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK
                                   : (SDL_AudioDeviceID)device;
    return SCHULTZ_OK;
}

uint64_t schultz_audio_output(const schultz_audio *audio)
{
    if (audio == NULL || audio->output == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
        return 0u;
    }
    return (uint64_t)audio->output;
}

int32_t schultz_audio_set_input(schultz_audio *audio, uint64_t device)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    audio->input = (device == 0u) ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
                                  : (SDL_AudioDeviceID)device;
    return SCHULTZ_OK;
}

uint64_t schultz_audio_input(const schultz_audio *audio)
{
    if (audio == NULL || audio->input == SDL_AUDIO_DEVICE_DEFAULT_RECORDING) {
        return 0u;
    }
    return (uint64_t)audio->input;
}

/* Whether one list differs from what was kept. */
static int32_t schultz_audio_list_changed(SDL_AudioDeviceID **kept,
                                          int *kept_count,
                                          SDL_AudioDeviceID *now, int count)
{
    int32_t changed = 0;
    int i;

    if (count != *kept_count) {
        changed = 1;
    } else {
        for (i = 0; i < count; i++) {
            if ((*kept)[i] != now[i]) {
                changed = 1;
                break;
            }
        }
    }
    if (changed) {
        SDL_free(*kept);
        *kept = NULL;
        *kept_count = 0;
        if (count > 0) {
            *kept = (SDL_AudioDeviceID *)SDL_malloc(
                (size_t)count * sizeof(**kept));
            if (*kept != NULL) {
                SDL_memcpy(*kept, now, (size_t)count * sizeof(**kept));
                *kept_count = count;
            }
        }
    }
    return changed;
}

int32_t schultz_audio_devices_changed(schultz_audio *audio)
{
    int count = 0;
    SDL_AudioDeviceID *now;
    int32_t changed = 0;

    if (audio == NULL) {
        return 0;
    }
    /*
     * Asked rather than announced, like everything else here. SDL keeps its
     * own list up to date as devices come and go, so this reads it and says
     * whether it differs from the last time anybody asked. A host with no
     * window and no event loop still finds out.
     */
    now = SDL_GetAudioPlaybackDevices(&count);
    if (now != NULL) {
        changed |= schultz_audio_list_changed(&audio->seen_outputs,
                                              &audio->seen_output_count,
                                              now, count);
        SDL_free(now);
    }
    now = SDL_GetAudioRecordingDevices(&count);
    if (now != NULL) {
        changed |= schultz_audio_list_changed(&audio->seen_inputs,
                                              &audio->seen_input_count,
                                              now, count);
        SDL_free(now);
    }
    return changed;
}

/* ------------------------------------------------- a file arriving in bits */

static void schultz_audio_feed_free(schultz_audio_feed *feed)
{
    if (feed == NULL) {
        return;
    }
    if (feed->lock != NULL) {
        SDL_DestroyMutex(feed->lock);
    }
    free(feed->bytes);
    free(feed);
}

static schultz_audio_feed *schultz_audio_feed_create(void)
{
    schultz_audio_feed *feed =
        (schultz_audio_feed *)calloc(1u, sizeof(*feed));

    if (feed == NULL) {
        return NULL;
    }
    feed->lock = SDL_CreateMutex();
    if (feed->lock == NULL) {
        free(feed);
        return NULL;
    }
    return feed;
}

/* Adds to the end, making room and dropping what has been read if need be. */
static int32_t schultz_audio_feed_write(schultz_audio_feed *feed,
                                        const void *bytes, uint64_t length)
{
    int32_t result = SCHULTZ_OK;

    /*
     * The length comes from outside as a uint64_t, and everything below adds
     * it to what is already held. A value near the top would wrap that sum,
     * grow the buffer to the small number it wrapped to, and then copy the
     * length that was asked for. Refused before any of it, and before the
     * lock, because nothing here can help with it.
     */
    if (length > SIZE_MAX / 2u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    SDL_LockMutex(feed->lock);
    /*
     * What the decoder has already read is only kept so that it can seek back
     * to the start while it works out the format. Once it is well past that,
     * the front is dropped and the buffer stops growing, which is what lets a
     * station play for hours in a fixed amount of memory.
     */
    if (feed->at > SCHULTZ_AUDIO_FEED_KEEP) {
        memmove(feed->bytes, feed->bytes + feed->at,
                (size_t)(feed->length - feed->at));
        feed->length  -= feed->at;
        feed->dropped += feed->at;
        feed->at       = 0u;
    }
    if (length > (uint64_t)SIZE_MAX - feed->length) {
        SDL_UnlockMutex(feed->lock);
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (feed->length + length > feed->size) {
        uint64_t needed = feed->length + length;
        uint64_t bigger = (feed->size == 0u) ? 65536u : feed->size;
        unsigned char *grown;

        /* Doubling, and stopping short of a wrap rather than looping for
         * ever or arriving at zero. */
        while (bigger < needed) {
            if (bigger > (uint64_t)SIZE_MAX / 2u) {
                bigger = needed;
                break;
            }
            bigger *= 2u;
        }
        grown = (unsigned char *)realloc(feed->bytes, (size_t)bigger);
        if (grown == NULL) {
            result = SCHULTZ_ERR_OUT_OF_MEMORY;
        } else {
            feed->bytes = grown;
            feed->size  = bigger;
        }
    }
    if (result == SCHULTZ_OK) {
        memcpy(feed->bytes + feed->length, bytes, (size_t)length);
        feed->length += length;
    }
    SDL_UnlockMutex(feed->lock);
    return result;
}

/*
 * The decoder reading. This runs on the mixer's thread.
 *
 * Nothing waiting is not the end of the file: a station that has not sent the
 * next second yet is still a station. Saying so with SDL_IO_STATUS_NOT_READY
 * rather than returning an end of file is what keeps the track alive until
 * the host says it is really finished.
 */
static size_t schultz_audio_feed_read(void *userdata, void *into, size_t size,
                                      SDL_IOStatus *status)
{
    schultz_audio_feed *feed = (schultz_audio_feed *)userdata;
    uint64_t waiting;
    size_t taken = 0u;

    SDL_LockMutex(feed->lock);
    waiting = feed->length - feed->at;
    if (waiting == 0u) {
        *status = feed->finished ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_NOT_READY;
    } else {
        taken = (waiting < (uint64_t)size) ? (size_t)waiting : size;
        memcpy(into, feed->bytes + feed->at, taken);
        feed->at += taken;
    }
    SDL_UnlockMutex(feed->lock);
    return taken;
}

/*
 * Seeking, as far as a live stream can.
 *
 * Backwards is allowed while the bytes are still held, which covers the one
 * seek that matters: a decoder reads the first few bytes to see what the file
 * is and then goes back to the beginning. Once the front has been dropped
 * there is nothing to go back to, and saying so is better than pretending.
 */
static Sint64 schultz_audio_feed_seek(void *userdata, Sint64 offset,
                                      SDL_IOWhence whence)
{
    schultz_audio_feed *feed = (schultz_audio_feed *)userdata;
    Sint64 want;
    Sint64 answer = -1;

    SDL_LockMutex(feed->lock);
    switch (whence) {
    case SDL_IO_SEEK_SET: want = offset; break;
    case SDL_IO_SEEK_CUR: want = (Sint64)(feed->dropped + feed->at) + offset;
                          break;
    default:              want = (Sint64)(feed->dropped + feed->length)
                                 + offset; break;
    }
    if (want >= (Sint64)feed->dropped &&
        want <= (Sint64)(feed->dropped + feed->length)) {
        feed->at = (uint64_t)want - feed->dropped;
        answer   = want;
    }
    SDL_UnlockMutex(feed->lock);
    return answer;
}

/* Nobody knows how long a station is. */
static Sint64 schultz_audio_feed_size(void *userdata)
{
    (void)userdata;
    return -1;
}

int32_t schultz_audio_decoder_create(schultz_audio *audio,
                                     schultz_handle *out_decoder)
{
    schultz_audio_channel *channel;
    SDL_IOStreamInterface reader;
    int32_t result;

    if (audio == NULL || out_decoder == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_audio_need_mixer(audio);
    if (result != SCHULTZ_OK) {
        return result;
    }
    channel = (schultz_audio_channel *)calloc(1u, sizeof(*channel));
    if (channel == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    channel->kind   = SCHULTZ_AUDIO_KIND_DECODER;
    channel->volume = 1.0f;
    channel->feed   = schultz_audio_feed_create();
    if (channel->feed == NULL) {
        free(channel);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    SDL_INIT_INTERFACE(&reader);
    reader.size  = schultz_audio_feed_size;
    reader.seek  = schultz_audio_feed_seek;
    reader.read  = schultz_audio_feed_read;
    channel->reader = SDL_OpenIO(&reader, channel->feed);
    channel->track  = MIX_CreateTrack(audio->mixer);
    if (channel->reader == NULL || channel->track == NULL) {
        if (channel->reader != NULL) { SDL_CloseIO(channel->reader); }
        if (channel->track != NULL)  { MIX_DestroyTrack(channel->track); }
        schultz_audio_feed_free(channel->feed);
        free(channel);
        return SCHULTZ_ERR_UNAVAILABLE;
    }

    result = schultz_handle_table_insert(&audio->channels, channel,
                                         out_decoder);
    if (result != SCHULTZ_OK) {
        SDL_CloseIO(channel->reader);
        MIX_DestroyTrack(channel->track);
        schultz_audio_feed_free(channel->feed);
        free(channel);
    }
    return result;
}

int32_t schultz_audio_decoder_write(schultz_audio *audio,
                                    schultz_handle decoder, const void *bytes,
                                    uint64_t length)
{
    schultz_audio_channel *channel;

    if (audio == NULL || (bytes == NULL && length != 0u)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, decoder,
                                       SCHULTZ_AUDIO_KIND_DECODER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (length == 0u) {
        return SCHULTZ_OK;
    }
    return schultz_audio_feed_write(channel->feed, bytes, length);
}

uint64_t schultz_audio_decoder_queued(const schultz_audio *audio,
                                      schultz_handle decoder)
{
    schultz_audio_channel *channel;
    uint64_t waiting;

    if (audio == NULL) {
        return 0u;
    }
    channel = schultz_audio_channel_of(audio, decoder,
                                       SCHULTZ_AUDIO_KIND_DECODER);
    if (channel == NULL) {
        return 0u;
    }
    SDL_LockMutex(channel->feed->lock);
    waiting = channel->feed->length - channel->feed->at;
    SDL_UnlockMutex(channel->feed->lock);
    return waiting;
}

int32_t schultz_audio_decoder_finish(schultz_audio *audio,
                                     schultz_handle decoder)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, decoder,
                                       SCHULTZ_AUDIO_KIND_DECODER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    SDL_LockMutex(channel->feed->lock);
    channel->feed->finished = 1;
    SDL_UnlockMutex(channel->feed->lock);
    return SCHULTZ_OK;
}

int32_t schultz_audio_decoder_play(schultz_audio *audio,
                                   schultz_handle decoder)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, decoder,
                                       SCHULTZ_AUDIO_KIND_DECODER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (channel->running) {
        MIX_ResumeTrack(channel->track);
        return SCHULTZ_OK;
    }
    /*
     * The decoder reads the first bytes here to see what it is playing, so a
     * host that has written nothing yet gets nothing decided. Writing a
     * little before playing is the contract, and it is what any source of
     * this kind does anyway: nobody starts a station on zero bytes.
     */
    if (!MIX_SetTrackIOStream(channel->track, channel->reader, false)) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    MIX_SetTrackGain(channel->track, channel->volume);
    if (!MIX_PlayTrack(channel->track, 0)) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    channel->running = 1;
    return SCHULTZ_OK;
}

int32_t schultz_audio_decoder_pause(schultz_audio *audio,
                                    schultz_handle decoder)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, decoder,
                                       SCHULTZ_AUDIO_KIND_DECODER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    MIX_PauseTrack(channel->track);
    return SCHULTZ_OK;
}

int32_t schultz_audio_decoder_is_playing(const schultz_audio *audio,
                                         schultz_handle decoder)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return 0;
    }
    channel = schultz_audio_channel_of(audio, decoder,
                                       SCHULTZ_AUDIO_KIND_DECODER);
    if (channel == NULL) {
        return 0;
    }
    return MIX_TrackPlaying(channel->track) ? 1 : 0;
}

int32_t schultz_audio_decoder_set_volume(schultz_audio *audio,
                                         schultz_handle decoder, float volume)
{
    schultz_audio_channel *channel;

    if (audio == NULL || volume < 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, decoder,
                                       SCHULTZ_AUDIO_KIND_DECODER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    channel->volume = volume;
    MIX_SetTrackGain(channel->track, volume);
    return SCHULTZ_OK;
}

float schultz_audio_decoder_volume(const schultz_audio *audio,
                                   schultz_handle decoder)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return 0.0f;
    }
    channel = schultz_audio_channel_of(audio, decoder,
                                       SCHULTZ_AUDIO_KIND_DECODER);
    return (channel == NULL) ? 0.0f : channel->volume;
}

int32_t schultz_audio_decoder_destroy(schultz_audio *audio,
                                      schultz_handle decoder)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return schultz_audio_channel_destroy(audio, decoder,
                                         SCHULTZ_AUDIO_KIND_DECODER);
}

/* ---------------------------------------------------------------- files */

/*
 * Starts the decoder, once, the first time a file is asked for.
 *
 * Deliberately not done when the system is made. An application that writes
 * its own samples never decodes anything, and opening a mixing device it will
 * not use takes one from whoever would have.
 */
static int32_t schultz_audio_need_mixer(schultz_audio *audio)
{
    uint32_t i;

    if (audio->mixer != NULL) {
        return SCHULTZ_OK;
    }
    if (!audio->mixer_started) {
        if (!MIX_Init()) {
            return SCHULTZ_ERR_UNAVAILABLE;
        }
        audio->mixer_started = 1;
    }
    audio->mixer = MIX_CreateMixerDevice(audio->output, NULL);
    if (audio->mixer == NULL) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    MIX_SetMixerGain(audio->mixer, audio->volume);

    /*
     * A handful of tracks, made once. A sound is samples and a track is
     * somewhere to play them, so playing the same click twice before the
     * first has finished needs two tracks rather than two copies of the
     * click.
     */
    for (i = 0u; i < SCHULTZ_AUDIO_VOICES; i++) {
        audio->voices[i] = MIX_CreateTrack(audio->mixer);
    }
    audio->music = MIX_CreateTrack(audio->mixer);
    return SCHULTZ_OK;
}

/* A track that is not busy, or NULL when every one of them is. */
static MIX_Track *schultz_audio_free_voice(schultz_audio *audio)
{
    uint32_t i;

    for (i = 0u; i < SCHULTZ_AUDIO_VOICES; i++) {
        if (audio->voices[i] != NULL && !MIX_TrackPlaying(audio->voices[i])) {
            return audio->voices[i];
        }
    }
    return NULL;
}

/* Wraps decoded samples in a handle, however they were decoded. */
static int32_t schultz_audio_sound_keep(schultz_audio *audio,
                                        MIX_Audio *decoded,
                                        schultz_handle *out_sound)
{
    schultz_audio_channel *channel;
    int32_t result;

    channel = (schultz_audio_channel *)calloc(1u, sizeof(*channel));
    if (channel == NULL) {
        MIX_DestroyAudio(decoded);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    channel->kind   = SCHULTZ_AUDIO_KIND_SOUND;
    channel->sound  = decoded;
    channel->volume = 1.0f;

    result = schultz_handle_table_insert(&audio->channels, channel,
                                         out_sound);
    if (result != SCHULTZ_OK) {
        MIX_DestroyAudio(decoded);
        free(channel);
    }
    return result;
}

int32_t schultz_sound_load_file(schultz_audio *audio, const char *path,
                                schultz_handle *out_sound)
{
    MIX_Audio *decoded;
    int32_t result;

    if (audio == NULL || path == NULL || out_sound == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_audio_need_mixer(audio);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /* Decoded now rather than while it plays: a sound is short and is played
     * often, and decoding it every time is work done again for nothing. */
    decoded = MIX_LoadAudio(audio->mixer, path, true);
    if (decoded == NULL) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    return schultz_audio_sound_keep(audio, decoded, out_sound);
}

int32_t schultz_sound_load_memory(schultz_audio *audio, const void *bytes,
                                  uint64_t length, schultz_handle *out_sound)
{
    SDL_IOStream *io;
    MIX_Audio *decoded;
    int32_t result;

    if (audio == NULL || bytes == NULL || length == 0u || out_sound == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_audio_need_mixer(audio);
    if (result != SCHULTZ_OK) {
        return result;
    }
    io = SDL_IOFromConstMem(bytes, (size_t)length);
    if (io == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    /* The last argument closes the reader for us, whether or not the decode
     * worked. */
    decoded = MIX_LoadAudio_IO(audio->mixer, io, true, true);
    if (decoded == NULL) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    return schultz_audio_sound_keep(audio, decoded, out_sound);
}

int32_t schultz_sound_play(schultz_audio *audio, schultz_handle sound)
{
    schultz_audio_channel *channel;
    MIX_Track *voice;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, sound,
                                       SCHULTZ_AUDIO_KIND_SOUND);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    voice = schultz_audio_free_voice(audio);
    if (voice == NULL) {
        /* Every track is busy. Saying so is better than cutting one of them
         * off: the one already playing is the one somebody can hear. */
        return SCHULTZ_ERR_EXHAUSTED;
    }
    if (!MIX_SetTrackAudio(voice, channel->sound)) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    MIX_SetTrackGain(voice, channel->volume);
    if (!MIX_PlayTrack(voice, 0)) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    return SCHULTZ_OK;
}

int32_t schultz_sound_stop(schultz_audio *audio, schultz_handle sound)
{
    schultz_audio_channel *channel;
    uint32_t i;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, sound,
                                       SCHULTZ_AUDIO_KIND_SOUND);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* Wherever it is playing, which may be several tracks at once. */
    for (i = 0u; i < SCHULTZ_AUDIO_VOICES; i++) {
        if (audio->voices[i] != NULL &&
            MIX_GetTrackAudio(audio->voices[i]) == channel->sound) {
            MIX_StopTrack(audio->voices[i], 0);
        }
    }
    return SCHULTZ_OK;
}

int32_t schultz_sound_set_volume(schultz_audio *audio, schultz_handle sound,
                                 float volume)
{
    schultz_audio_channel *channel;

    if (audio == NULL || volume < 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, sound,
                                       SCHULTZ_AUDIO_KIND_SOUND);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* Kept for the next time it is played, since a sound is samples rather
     * than something playing. */
    channel->volume = volume;
    return SCHULTZ_OK;
}

float schultz_sound_volume(const schultz_audio *audio, schultz_handle sound)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return 0.0f;
    }
    channel = schultz_audio_channel_of(audio, sound,
                                       SCHULTZ_AUDIO_KIND_SOUND);
    return (channel == NULL) ? 0.0f : channel->volume;
}

int32_t schultz_sound_destroy(schultz_audio *audio, schultz_handle sound)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return schultz_audio_channel_destroy(audio, sound,
                                         SCHULTZ_AUDIO_KIND_SOUND);
}

/* ---------------------------------------------------------------- music */

int32_t schultz_music_play(schultz_audio *audio, const char *path,
                           int32_t repeats)
{
    MIX_Audio *decoded;
    int32_t result;

    if (audio == NULL || path == NULL || repeats < -1) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_audio_need_mixer(audio);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (audio->music == NULL) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    /* Decoded as it plays rather than up front: a song is minutes long and
     * holding all of it as samples is tens of megabytes for no gain. */
    decoded = MIX_LoadAudio(audio->mixer, path, false);
    if (decoded == NULL) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    MIX_StopTrack(audio->music, 0);
    if (audio->music_audio != NULL) {
        MIX_DestroyAudio(audio->music_audio);
    }
    audio->music_audio = decoded;

    if (!MIX_SetTrackAudio(audio->music, decoded)) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    MIX_SetTrackGain(audio->music, audio->music_volume);
    MIX_SetTrackLoops(audio->music, repeats);
    if (!MIX_PlayTrack(audio->music, 0)) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    return SCHULTZ_OK;
}

int32_t schultz_music_pause(schultz_audio *audio)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (audio->music != NULL) {
        MIX_PauseTrack(audio->music);
    }
    return SCHULTZ_OK;
}

int32_t schultz_music_resume(schultz_audio *audio)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (audio->music != NULL) {
        MIX_ResumeTrack(audio->music);
    }
    return SCHULTZ_OK;
}

int32_t schultz_music_stop(schultz_audio *audio)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (audio->music != NULL) {
        MIX_StopTrack(audio->music, 0);
    }
    return SCHULTZ_OK;
}

int32_t schultz_music_is_playing(const schultz_audio *audio)
{
    if (audio == NULL || audio->music == NULL) {
        return 0;
    }
    return MIX_TrackPlaying(audio->music) ? 1 : 0;
}

int32_t schultz_music_set_volume(schultz_audio *audio, float volume)
{
    if (audio == NULL || volume < 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    audio->music_volume = volume;
    if (audio->music != NULL) {
        MIX_SetTrackGain(audio->music, volume);
    }
    return SCHULTZ_OK;
}

float schultz_music_volume(const schultz_audio *audio)
{
    return (audio == NULL) ? 0.0f : audio->music_volume;
}

/* ------------------------------------------------------------- recording */

int32_t schultz_audio_recorder_create(schultz_audio *audio, uint32_t format,
                                      uint32_t channels, uint32_t rate,
                                      schultz_handle *out_recorder)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return schultz_audio_channel_create(audio, audio->input, format,
                                        channels, rate,
                                        SCHULTZ_AUDIO_KIND_RECORDER,
                                        out_recorder);
}

int32_t schultz_audio_recorder_start(schultz_audio *audio,
                                     schultz_handle recorder)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, recorder,
                                     SCHULTZ_AUDIO_KIND_RECORDER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (!SDL_ResumeAudioStreamDevice(channel->stream)) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    channel->running = 1;
    return SCHULTZ_OK;
}

int32_t schultz_audio_recorder_stop(schultz_audio *audio,
                                    schultz_handle recorder)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, recorder,
                                     SCHULTZ_AUDIO_KIND_RECORDER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    SDL_PauseAudioStreamDevice(channel->stream);
    channel->running = 0;
    return SCHULTZ_OK;
}

int32_t schultz_audio_recorder_is_running(const schultz_audio *audio,
                                          schultz_handle recorder)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return 0;
    }
    channel = schultz_audio_channel_of(audio, recorder,
                                     SCHULTZ_AUDIO_KIND_RECORDER);
    return (channel == NULL) ? 0 : channel->running;
}

uint64_t schultz_audio_recorder_available(const schultz_audio *audio,
                                          schultz_handle recorder)
{
    schultz_audio_channel *channel;
    int waiting;

    if (audio == NULL) {
        return 0u;
    }
    channel = schultz_audio_channel_of(audio, recorder,
                                     SCHULTZ_AUDIO_KIND_RECORDER);
    if (channel == NULL) {
        return 0u;
    }
    waiting = SDL_GetAudioStreamAvailable(channel->stream);
    return (waiting < 0) ? 0u : (uint64_t)waiting;
}

int32_t schultz_audio_recorder_read(schultz_audio *audio,
                                    schultz_handle recorder, void *bytes,
                                    uint64_t length, uint64_t *out_read)
{
    schultz_audio_channel *channel;
    int read;

    if (audio == NULL || out_read == NULL || (bytes == NULL && length != 0u)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_read = 0u;
    channel = schultz_audio_channel_of(audio, recorder,
                                     SCHULTZ_AUDIO_KIND_RECORDER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (length == 0u) {
        return SCHULTZ_OK;
    }
    if (length > (uint64_t)INT_MAX) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    read = SDL_GetAudioStreamData(channel->stream, bytes, (int)length);
    if (read > 0) {
        *out_read = (uint64_t)read;
    }
    return SCHULTZ_OK;
}

/*
 * Writes a WAV header with the sizes it will have when everything is written.
 *
 * Written twice: once with nothing in it, so that a file left behind by a
 * crash is still openable, and once at the end with the real lengths. A WAV
 * header cannot be written last because it comes first.
 */
static void schultz_audio_wav_header(schultz_audio_channel *channel)
{
    unsigned char header[44];
    unsigned char *at = header;
    uint32_t bits = (channel->format == SCHULTZ_AUDIO_F32) ? 32u : 16u;
    uint32_t frame = channel->channels * (bits / 8u);
    uint32_t data = (uint32_t)channel->written;
    uint32_t size = 36u + data;
    uint16_t kind = (channel->format == SCHULTZ_AUDIO_F32) ? 3u : 1u;

    SDL_memcpy(at, "RIFF", 4);      at += 4;
    SDL_memcpy(at, &size, 4);       at += 4;
    SDL_memcpy(at, "WAVEfmt ", 8);  at += 8;
    { uint32_t n = 16u;             SDL_memcpy(at, &n, 4); at += 4; }
    SDL_memcpy(at, &kind, 2);       at += 2;
    { uint16_t n = (uint16_t)channel->channels;
                                    SDL_memcpy(at, &n, 2); at += 2; }
    SDL_memcpy(at, &channel->rate, 4); at += 4;
    { uint32_t n = channel->rate * frame; SDL_memcpy(at, &n, 4); at += 4; }
    { uint16_t n = (uint16_t)frame; SDL_memcpy(at, &n, 2); at += 2; }
    { uint16_t n = (uint16_t)bits;  SDL_memcpy(at, &n, 2); at += 2; }
    SDL_memcpy(at, "data", 4);      at += 4;
    SDL_memcpy(at, &data, 4);

    SDL_SeekIO(channel->file, 0, SDL_IO_SEEK_SET);
    SDL_WriteIO(channel->file, header, sizeof(header));
    SDL_SeekIO(channel->file, 0, SDL_IO_SEEK_END);
}

/*
 * Takes what the microphone has made and puts it in the file.
 *
 * SDL calls this on its own thread whenever the device has produced
 * something. It is inside the toolkit and written in C, so it breaks nothing:
 * the rule is that the toolkit never calls the *host* back, and this calls
 * nobody.
 */
static void SDLCALL schultz_audio_to_file(void *userdata,
                                          SDL_AudioStream *stream,
                                          int more, int total)
{
    schultz_audio_channel *channel = (schultz_audio_channel *)userdata;
    unsigned char block[4096];
    int taken;

    (void)more;
    (void)total;
    if (channel->file == NULL) {
        return;
    }
    while ((taken = SDL_GetAudioStreamData(stream, block, sizeof(block))) > 0) {
        SDL_WriteIO(channel->file, block, (size_t)taken);
        channel->written += (uint64_t)taken;
    }
}

int32_t schultz_audio_recorder_save(schultz_audio *audio,
                                    schultz_handle recorder, const char *path)
{
    schultz_audio_channel *channel;

    if (audio == NULL || path == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, recorder,
                                       SCHULTZ_AUDIO_KIND_RECORDER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (channel->file != NULL) {
        return SCHULTZ_ERR_EXHAUSTED; /* already saving somewhere */
    }
    channel->file = SDL_IOFromFile(path, "wb");
    if (channel->file == NULL) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    channel->written = 0u;
    schultz_audio_wav_header(channel);

    /*
     * From here the toolkit drains the microphone, so nothing is left for
     * schultz_audio_recorder_read. Saving and reading are two ways to the
     * same bytes and an application picks one.
     */
    SDL_SetAudioStreamPutCallback(channel->stream, schultz_audio_to_file,
                                  channel);
    return SCHULTZ_OK;
}

int32_t schultz_audio_recorder_stop_saving(schultz_audio *audio,
                                           schultz_handle recorder)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, recorder,
                                       SCHULTZ_AUDIO_KIND_RECORDER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (channel->file == NULL) {
        return SCHULTZ_OK;
    }
    /* Off first, so nothing is writing while the header is corrected. */
    SDL_SetAudioStreamPutCallback(channel->stream, NULL, NULL);
    schultz_audio_wav_header(channel);
    SDL_CloseIO(channel->file);
    channel->file = NULL;
    return SCHULTZ_OK;
}

int32_t schultz_audio_recorder_is_saving(const schultz_audio *audio,
                                         schultz_handle recorder)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return 0;
    }
    channel = schultz_audio_channel_of(audio, recorder,
                                       SCHULTZ_AUDIO_KIND_RECORDER);
    return (channel == NULL || channel->file == NULL) ? 0 : 1;
}

uint64_t schultz_audio_recorder_saved(const schultz_audio *audio,
                                      schultz_handle recorder)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return 0u;
    }
    channel = schultz_audio_channel_of(audio, recorder,
                                       SCHULTZ_AUDIO_KIND_RECORDER);
    return (channel == NULL) ? 0u : channel->written;
}

int32_t schultz_audio_recorder_clear(schultz_audio *audio,
                                     schultz_handle recorder)
{
    schultz_audio_channel *channel;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, recorder,
                                     SCHULTZ_AUDIO_KIND_RECORDER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    SDL_ClearAudioStream(channel->stream);
    return SCHULTZ_OK;
}

int32_t schultz_audio_recorder_destroy(schultz_audio *audio,
                                       schultz_handle recorder)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return schultz_audio_channel_destroy(audio, recorder,
                                         SCHULTZ_AUDIO_KIND_RECORDER);
}


/* ---------------------------------------------------- the other direction */

/*
 * Opus encoding. Short, because Opus asks for very little: a rate, a channel
 * count and what the sound is for, and then fixed lengths of samples.
 *
 * The one piece of work here is holding samples until there are enough for a
 * packet. A caller writing whatever the microphone gave it has no reason to
 * be handing over exactly 960 sample frames at a time, and Opus will not code
 * anything else.
 */
int32_t schultz_audio_encoder_create(schultz_audio *audio, uint32_t channels,
                                     uint32_t bitrate,
                                     schultz_handle *out_encoder)
{
    schultz_audio_channel *channel;
    int error = OPUS_OK;
    int32_t result;

    if (audio == NULL || out_encoder == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (channels != 1u && channels != 2u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (bitrate == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = (schultz_audio_channel *)calloc(1, sizeof(*channel));
    if (channel == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    channel->kind     = SCHULTZ_AUDIO_KIND_ENCODER;
    channel->channels = channels;
    channel->rate     = 48000u;
    channel->volume   = 1.0f;

    /*
     * VOIP rather than AUDIO: it is tuned for speech and it is what a call
     * wants. A host encoding music would want the other, and that is a
     * setting to add when something asks for it rather than to guess at now.
     */
    channel->opus = opus_encoder_create(48000, (int)channels,
                                        OPUS_APPLICATION_VOIP, &error);
    if (channel->opus == NULL || error != OPUS_OK) {
        free(channel);
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    opus_encoder_ctl(channel->opus, OPUS_SET_BITRATE((opus_int32)bitrate));
    channel->bitrate = bitrate;

    /* Room for one packet's worth to be waiting, which is all that ever is. */
    channel->pcm_room = SCHULTZ_AUDIO_OPUS_FRAMES * 2u;
    channel->pcm = (float *)malloc((size_t)channel->pcm_room *
                                   channels * sizeof(float));
    /* Opus never produces more than this for one packet at any bitrate. */
    channel->packet = (unsigned char *)malloc(4000u);
    if (channel->pcm == NULL || channel->packet == NULL) {
        opus_encoder_destroy(channel->opus);
        free(channel->pcm);
        free(channel->packet);
        free(channel);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    result = schultz_handle_table_insert(&audio->channels, channel,
                                         out_encoder);
    if (result != SCHULTZ_OK) {
        opus_encoder_destroy(channel->opus);
        free(channel->pcm);
        free(channel->packet);
        free(channel);
        return result;
    }
    return SCHULTZ_OK;
}

int32_t schultz_audio_encoder_write(schultz_audio *audio,
                                    schultz_handle encoder,
                                    const float *samples, uint64_t frames)
{
    schultz_audio_channel *channel;
    uint64_t want;

    if (audio == NULL || (samples == NULL && frames != 0u)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, encoder,
                                       SCHULTZ_AUDIO_KIND_ENCODER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (frames == 0u) {
        return SCHULTZ_OK;
    }
    /*
     * Frames arrive as a uint64_t and are added to what is already waiting,
     * then multiplied by the channel count and the size of a sample. Each
     * step is checked: the sum can wrap, and so can the product even when
     * the sum did not, and either one buys a buffer smaller than the copy
     * that follows.
     */
    if (frames > (uint64_t)SIZE_MAX - channel->pcm_frames) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    want = channel->pcm_frames + frames;
    if (want > (uint64_t)SIZE_MAX / (channel->channels * sizeof(float))) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (want > channel->pcm_room) {
        float *grown = (float *)realloc(channel->pcm,
                                        (size_t)want * channel->channels *
                                        sizeof(float));

        if (grown == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        channel->pcm      = grown;
        channel->pcm_room = want;
    }
    memcpy(channel->pcm + channel->pcm_frames * channel->channels, samples,
           (size_t)frames * channel->channels * sizeof(float));
    channel->pcm_frames += frames;
    return SCHULTZ_OK;
}

int32_t schultz_audio_encoder_read_packet(schultz_audio *audio,
                                          schultz_handle encoder,
                                          const void **out_bytes,
                                          uint64_t *out_length,
                                          uint64_t *out_when_ns)
{
    schultz_audio_channel *channel;
    opus_int32 wrote;
    uint64_t left;

    if (audio == NULL || out_bytes == NULL || out_length == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_bytes  = NULL;
    *out_length = 0u;
    channel = schultz_audio_channel_of(audio, encoder,
                                       SCHULTZ_AUDIO_KIND_ENCODER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (channel->pcm_frames < SCHULTZ_AUDIO_OPUS_FRAMES) {
        return SCHULTZ_ERR_EXHAUSTED;   /* not enough for a packet yet */
    }

    wrote = opus_encode_float(channel->opus, channel->pcm,
                              (int)SCHULTZ_AUDIO_OPUS_FRAMES,
                              channel->packet, 4000);
    if (wrote < 0) {
        return SCHULTZ_ERR_EXHAUSTED;
    }
    channel->packet_length  = (uint64_t)wrote;
    channel->packet_when_ns = channel->sent_frames * 1000000000u / 48000u;
    channel->sent_frames   += SCHULTZ_AUDIO_OPUS_FRAMES;

    /* What is left slides down to the front, ready for the next one. */
    left = channel->pcm_frames - SCHULTZ_AUDIO_OPUS_FRAMES;
    if (left > 0u) {
        memmove(channel->pcm,
                channel->pcm + SCHULTZ_AUDIO_OPUS_FRAMES * channel->channels,
                (size_t)left * channel->channels * sizeof(float));
    }
    channel->pcm_frames = left;

    *out_bytes  = channel->packet;
    *out_length = channel->packet_length;
    if (out_when_ns != NULL) {
        *out_when_ns = channel->packet_when_ns;
    }
    return SCHULTZ_OK;
}

int32_t schultz_audio_encoder_set_bitrate(schultz_audio *audio,
                                          schultz_handle encoder,
                                          uint32_t bitrate)
{
    schultz_audio_channel *channel;

    if (audio == NULL || bitrate == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, encoder,
                                       SCHULTZ_AUDIO_KIND_ENCODER);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    opus_encoder_ctl(channel->opus, OPUS_SET_BITRATE((opus_int32)bitrate));
    channel->bitrate = bitrate;
    return SCHULTZ_OK;
}

int32_t schultz_audio_encoder_destroy(schultz_audio *audio,
                                      schultz_handle encoder)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return schultz_audio_channel_destroy(audio, encoder,
                                         SCHULTZ_AUDIO_KIND_ENCODER);
}

/*
 * And the other end of it.
 *
 * Shorter than the encoder, because a decoder has nothing to hold back: one
 * packet in is one lot of samples out, however many that turns out to be.
 */
int32_t schultz_audio_packet_decoder_create(schultz_audio *audio,
                                            uint32_t channels,
                                            schultz_handle *out_decoder)
{
    schultz_audio_channel *channel;
    int error = OPUS_OK;
    int32_t result;

    if (audio == NULL || out_decoder == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (channels != 1u && channels != 2u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = (schultz_audio_channel *)calloc(1, sizeof(*channel));
    if (channel == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    channel->kind     = SCHULTZ_AUDIO_KIND_PACKETS;
    channel->channels = channels;
    channel->rate     = 48000u;
    channel->volume   = 1.0f;

    channel->opus_in = opus_decoder_create(48000, (int)channels, &error);
    if (channel->opus_in == NULL || error != OPUS_OK) {
        free(channel);
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    channel->pcm_room = SCHULTZ_AUDIO_OPUS_MOST;
    channel->pcm = (float *)malloc((size_t)channel->pcm_room * channels *
                                   sizeof(float));
    if (channel->pcm == NULL) {
        opus_decoder_destroy(channel->opus_in);
        free(channel);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    result = schultz_handle_table_insert(&audio->channels, channel,
                                         out_decoder);
    if (result != SCHULTZ_OK) {
        opus_decoder_destroy(channel->opus_in);
        free(channel->pcm);
        free(channel);
        return result;
    }
    return SCHULTZ_OK;
}

int32_t schultz_audio_packet_decoder_write(schultz_audio *audio,
                                           schultz_handle decoder,
                                           const void *bytes, uint64_t length)
{
    schultz_audio_channel *channel;
    int got;

    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    channel = schultz_audio_channel_of(audio, decoder,
                                       SCHULTZ_AUDIO_KIND_PACKETS);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /*
     * NULL is not a mistake here: it is how a caller says a packet was lost,
     * and Opus works out something plausible to cover the gap. That is what
     * keeps a call going through a bad moment instead of clicking.
     */
    /*
     * Opus takes a signed 32 bit length. A packet longer than that is not a
     * packet, and narrowing it would hand the decoder a different size than
     * the buffer really is.
     */
    if (length > (uint64_t)INT32_MAX) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    got = opus_decode_float(channel->opus_in,
                            (const unsigned char *)bytes,
                            (opus_int32)length, channel->pcm,
                            (int)SCHULTZ_AUDIO_OPUS_MOST, 0);
    if (got < 0) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    channel->ready_frames = (uint64_t)got;
    channel->have_ready   = 1;
    return SCHULTZ_OK;
}

int32_t schultz_audio_packet_decoder_read(schultz_audio *audio,
                                          schultz_handle decoder,
                                          const float **out_samples,
                                          uint64_t *out_frames)
{
    schultz_audio_channel *channel;

    if (audio == NULL || out_samples == NULL || out_frames == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_samples = NULL;
    *out_frames  = 0u;
    channel = schultz_audio_channel_of(audio, decoder,
                                       SCHULTZ_AUDIO_KIND_PACKETS);
    if (channel == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (!channel->have_ready) {
        return SCHULTZ_ERR_EXHAUSTED;
    }
    channel->have_ready = 0;
    *out_samples = channel->pcm;
    *out_frames  = channel->ready_frames;
    return SCHULTZ_OK;
}

int32_t schultz_audio_packet_decoder_destroy(schultz_audio *audio,
                                             schultz_handle decoder)
{
    if (audio == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return schultz_audio_channel_destroy(audio, decoder,
                                         SCHULTZ_AUDIO_KIND_PACKETS);
}
