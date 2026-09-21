/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_audio.c - playing samples the host writes, and reading the microphone.
 *
 * Headless, and on any machine. SDL's dummy driver is asked for before
 * anything opens, so these tests want no sound card, no microphone and no
 * display: the device accepts everything written and plays it to nowhere,
 * which is enough to assert what this interface promises. What it cannot
 * prove is that a real card makes a noise; that is what the demo is for.
 */

#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include "greatest.h"
#include "schultz_audio.h"

/* Two channels of sixteen bit silence, as a host would hand it over. */
static unsigned char quiet[4096];

static schultz_audio *open_audio(void)
{
    schultz_audio *audio = NULL;

    /* Before anything opens a device, and it decides which driver is used. */
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (schultz_audio_create(&audio) != SCHULTZ_OK) {
        return NULL;
    }
    return audio;
}

TEST the_system_starts_and_stops(void)
{
    schultz_audio *audio = open_audio();

    ASSERT(audio != NULL);
    ASSERT_EQ(1.0f, schultz_audio_volume(audio));
    schultz_audio_destroy(audio);
    /* Destroying nothing is allowed, so a host need not check first. */
    schultz_audio_destroy(NULL);
    PASS();
}

TEST the_volume_is_kept_and_refuses_nonsense(void)
{
    schultz_audio *audio = open_audio();

    ASSERT(audio != NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_set_volume(audio, 0.25f));
    ASSERT_EQ(0.25f, schultz_audio_volume(audio));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_set_volume(audio, -1.0f));
    ASSERT_EQ(0.25f, schultz_audio_volume(audio));
    schultz_audio_destroy(audio);
    PASS();
}

TEST a_stream_refuses_what_it_cannot_play(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle stream = SCHULTZ_HANDLE_NONE;

    ASSERT(audio != NULL);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_stream_create(audio, 99u, 2u, 48000u, &stream));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_S16, 0u,
                                          48000u, &stream));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_S16, 2u, 0u,
                                          &stream));
    schultz_audio_destroy(audio);
    PASS();
}

TEST what_is_written_is_waiting_to_be_played(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle stream = SCHULTZ_HANDLE_NONE;

    ASSERT(audio != NULL);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_S16, 2u,
                                          48000u, &stream));
    ASSERT_EQ(0u, schultz_audio_stream_queued(audio, stream));

    /*
     * Written while it is not playing, so nothing has been taken off the
     * front and the answer is the whole of what went in. This is the number a
     * host writes its loop against.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_stream_write(audio, stream, quiet,
                                                     sizeof(quiet)));
    ASSERT(schultz_audio_stream_queued(audio, stream) > 0u);

    /* A station change throws away the seconds already buffered. */
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_stream_clear(audio, stream));
    ASSERT_EQ(0u, schultz_audio_stream_queued(audio, stream));
    schultz_audio_destroy(audio);
    PASS();
}

TEST writing_nothing_is_allowed_and_writing_nowhere_is_not(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle stream = SCHULTZ_HANDLE_NONE;

    ASSERT(audio != NULL);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_F32, 1u,
                                          22050u, &stream));
    /* An empty chunk is a thing that happens at the end of a file. */
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_stream_write(audio, stream, NULL, 0u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_stream_write(audio, stream, NULL, 16u));
    schultz_audio_destroy(audio);
    PASS();
}

TEST a_stream_plays_and_pauses(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle stream = SCHULTZ_HANDLE_NONE;

    ASSERT(audio != NULL);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_S16, 2u,
                                          48000u, &stream));
    ASSERT_FALSE(schultz_audio_stream_is_playing(audio, stream));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_stream_play(audio, stream));
    ASSERT(schultz_audio_stream_is_playing(audio, stream));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_stream_pause(audio, stream));
    ASSERT_FALSE(schultz_audio_stream_is_playing(audio, stream));
    schultz_audio_destroy(audio);
    PASS();
}

TEST a_stream_keeps_its_own_volume(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle stream = SCHULTZ_HANDLE_NONE;

    ASSERT(audio != NULL);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_S16, 2u,
                                          48000u, &stream));
    ASSERT_EQ(1.0f, schultz_audio_stream_volume(audio, stream));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_stream_set_volume(audio, stream, 0.5f));
    ASSERT_EQ(0.5f, schultz_audio_stream_volume(audio, stream));

    /*
     * The system's volume is applied on top rather than instead, so quieting
     * the application does not disturb the balance inside it.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_set_volume(audio, 0.5f));
    ASSERT_EQ(0.5f, schultz_audio_stream_volume(audio, stream));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_stream_set_volume(audio, stream, -0.1f));
    schultz_audio_destroy(audio);
    PASS();
}

TEST a_destroyed_stream_is_a_dead_handle(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle stream = SCHULTZ_HANDLE_NONE;

    ASSERT(audio != NULL);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_S16, 2u,
                                          48000u, &stream));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_stream_destroy(audio, stream));

    /* Using it afterwards is an error code, not a crash, which is the whole
     * point of a handle carrying a generation. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_audio_stream_write(audio, stream, quiet, 16u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_audio_stream_play(audio, stream));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_audio_stream_destroy(audio, stream));
    ASSERT_EQ(0u, schultz_audio_stream_queued(audio, stream));
    schultz_audio_destroy(audio);
    PASS();
}

TEST several_streams_live_at_once(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle first = SCHULTZ_HANDLE_NONE;
    schultz_handle second = SCHULTZ_HANDLE_NONE;

    ASSERT(audio != NULL);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_S16, 2u,
                                          48000u, &first));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_F32, 1u,
                                          22050u, &second));
    ASSERT(first != second);
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_stream_write(audio, first, quiet,
                                                     sizeof(quiet)));
    ASSERT(schultz_audio_stream_queued(audio, first) > 0u);
    ASSERT_EQ(0u, schultz_audio_stream_queued(audio, second));

    /* One goes, the other stays: they are not a single device between them. */
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_stream_destroy(audio, first));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_stream_write(audio, second, quiet,
                                                     sizeof(quiet)));
    schultz_audio_destroy(audio);
    PASS();
}

/* ------------------------------------------------------------ from files */

/*
 * A WAV of a short tone, built here rather than kept as a file. Sixteen bit,
 * one channel, eight thousand a second, which is the smallest thing every
 * decoder in the world agrees about.
 */
static uint64_t build_wav(unsigned char *into, uint64_t room)
{
    const uint32_t rate = 8000u;
    const uint32_t frames = 400u;
    const uint32_t data = frames * 2u;
    const uint32_t size = 36u + data;
    uint32_t i;
    unsigned char *at = into;

    if (room < 44u + data) {
        return 0u;
    }
    memcpy(at, "RIFF", 4); at += 4;
    SDL_memcpy(at, &size, 4); at += 4;
    memcpy(at, "WAVEfmt ", 8); at += 8;
    { uint32_t n = 16u;    SDL_memcpy(at, &n, 4); at += 4; }
    { uint16_t n = 1u;     SDL_memcpy(at, &n, 2); at += 2; } /* PCM */
    { uint16_t n = 1u;     SDL_memcpy(at, &n, 2); at += 2; } /* mono */
    { uint32_t n = rate;   SDL_memcpy(at, &n, 4); at += 4; }
    { uint32_t n = rate*2; SDL_memcpy(at, &n, 4); at += 4; }
    { uint16_t n = 2u;     SDL_memcpy(at, &n, 2); at += 2; }
    { uint16_t n = 16u;    SDL_memcpy(at, &n, 2); at += 2; }
    memcpy(at, "data", 4); at += 4;
    SDL_memcpy(at, &data, 4); at += 4;
    for (i = 0u; i < frames; i++) {
        int16_t sample = (int16_t)((i % 40u < 20u) ? 3000 : -3000);

        SDL_memcpy(at, &sample, 2);
        at += 2;
    }
    return (uint64_t)(at - into);
}

TEST the_build_carries_no_copyleft_decoder(void)
{
    int count;
    int i;

    ASSERT(MIX_Init());
    count = MIX_GetNumAudioDecoders();
    ASSERT(count > 0);
    for (i = 0; i < count; i++) {
        const char *name = MIX_GetAudioDecoder(i);

        if (name == NULL) {
            continue;
        }
        /*
         * Absent for their licences rather than for their merits: libxmp,
         * libmpg123, FluidSynth and game-music-emu are copyleft, and
         * everything here is linked statically into one archive. This test is
         * how that decision is kept rather than remembered. See
         * THIRD_PARTY_NOTICES.md.
         */
        ASSERT(SDL_strstr(name, "XMP") == NULL);
        ASSERT(SDL_strstr(name, "MPG123") == NULL);
        ASSERT(SDL_strstr(name, "FLUIDSYNTH") == NULL);
        ASSERT(SDL_strstr(name, "TIMIDITY") == NULL);
        ASSERT(SDL_strstr(name, "GME") == NULL);
        ASSERT(SDL_strcmp(name, "MOD") != 0);
        ASSERT(SDL_strcmp(name, "MIDI") != 0);
    }
    MIX_Quit();
    PASS();
}

TEST every_format_that_should_decode_does(void)
{
    /*
     * A decoder is named for the code that does the work rather than for the
     * format, which is what this test is really checking. SDL_mixer bundles a
     * small decoder for FLAC and one for Vorbis -- dr_flac and stb_vorbis,
     * which announce themselves as DRFLAC and STBVORBIS -- and it is built
     * against the format authors' own libraries instead, which announce
     * themselves as FLAC and VORBIS. Both are switched in scripts/
     * build_deps.sh, and turning the bundled ones off is easy to lose in a
     * version bump, at which point the toolkit quietly goes back to the small
     * decoders and nobody notices.
     *
     * MP3 is the one exception and stays DRMP3: the only other MP3 decoder
     * SDL_mixer knows is libmpg123, which is copyleft.
     */
    static const char *const wanted[] = {
        "WAV", "DRMP3", "FLAC", "VORBIS", "OPUS", "WAVPACK"
    };
    static const char *const unwanted[] = { "DRFLAC", "STBVORBIS" };
    int count;
    int i;
    size_t w;

    ASSERT(MIX_Init());
    count = MIX_GetNumAudioDecoders();
    ASSERT(count > 0);

    for (w = 0; w < SDL_arraysize(wanted); w++) {
        int found = 0;

        for (i = 0; i < count; i++) {
            const char *name = MIX_GetAudioDecoder(i);

            if (name != NULL && SDL_strcmp(name, wanted[w]) == 0) {
                found = 1;
            }
        }
        ASSERT_EQ_FMT(1, found, "%d");
    }
    for (w = 0; w < SDL_arraysize(unwanted); w++) {
        for (i = 0; i < count; i++) {
            const char *name = MIX_GetAudioDecoder(i);

            if (name != NULL) {
                ASSERT(SDL_strcmp(name, unwanted[w]) != 0);
            }
        }
    }
    MIX_Quit();
    PASS();
}

TEST a_sound_loads_from_memory_and_plays(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle sound = SCHULTZ_HANDLE_NONE;
    unsigned char wav[1024];
    uint64_t length;

    ASSERT(audio != NULL);
    length = build_wav(wav, sizeof(wav));
    ASSERT(length > 44u);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_sound_load_memory(audio, wav, length, &sound));
    ASSERT_EQ(1.0f, schultz_sound_volume(audio, sound));
    ASSERT_EQ(SCHULTZ_OK, schultz_sound_play(audio, sound));

    /* The same sound twice over, which is what a run of key presses is. */
    ASSERT_EQ(SCHULTZ_OK, schultz_sound_play(audio, sound));
    ASSERT_EQ(SCHULTZ_OK, schultz_sound_stop(audio, sound));

    ASSERT_EQ(SCHULTZ_OK, schultz_sound_set_volume(audio, sound, 0.5f));
    ASSERT_EQ(0.5f, schultz_sound_volume(audio, sound));
    ASSERT_EQ(SCHULTZ_OK, schultz_sound_destroy(audio, sound));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_sound_play(audio, sound));
    schultz_audio_destroy(audio);
    PASS();
}

TEST bytes_that_are_not_a_sound_say_so(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle sound = SCHULTZ_HANDLE_NONE;
    unsigned char rubbish[64];

    ASSERT(audio != NULL);
    memset(rubbish, 'x', sizeof(rubbish));
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_sound_load_memory(audio, rubbish, sizeof(rubbish),
                                        &sound));
    /* A missing file is the same answer, and neither is a bad call. */
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_sound_load_file(audio, "nothing-is-here.wav", &sound));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_sound_load_file(audio, NULL, &sound));
    schultz_audio_destroy(audio);
    PASS();
}

TEST a_sound_is_not_a_stream(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle sound = SCHULTZ_HANDLE_NONE;
    schultz_handle stream = SCHULTZ_HANDLE_NONE;
    unsigned char wav[1024];
    uint64_t length;

    ASSERT(audio != NULL);
    length = build_wav(wav, sizeof(wav));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_sound_load_memory(audio, wav, length, &sound));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_S16, 2u,
                                          48000u, &stream));
    /* Three kinds out of one table, and each call takes only its own. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_audio_stream_play(audio, sound));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_sound_play(audio, stream));
    schultz_audio_destroy(audio);
    PASS();
}

TEST music_plays_from_a_file(void)
{
    schultz_audio *audio = open_audio();
    unsigned char wav[1024];
    uint64_t length;
    const char *path = "build/tests/tone.wav";
    SDL_IOStream *out;

    ASSERT(audio != NULL);
    length = build_wav(wav, sizeof(wav));
    out = SDL_IOFromFile(path, "wb");
    ASSERT(out != NULL);
    SDL_WriteIO(out, wav, (size_t)length);
    SDL_CloseIO(out);

    ASSERT_FALSE(schultz_music_is_playing(audio));
    ASSERT_EQ(1.0f, schultz_music_volume(audio));
    ASSERT_EQ(SCHULTZ_OK, schultz_music_play(audio, path, 0));

    ASSERT_EQ(SCHULTZ_OK, schultz_music_set_volume(audio, 0.3f));
    ASSERT_EQ(0.3f, schultz_music_volume(audio));
    ASSERT_EQ(SCHULTZ_OK, schultz_music_pause(audio));
    ASSERT_EQ(SCHULTZ_OK, schultz_music_resume(audio));
    ASSERT_EQ(SCHULTZ_OK, schultz_music_stop(audio));
    ASSERT_FALSE(schultz_music_is_playing(audio));

    /* Asking for a file that is not there is an answer, not a crash. */
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_music_play(audio, "nothing-is-here.ogg", 0));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_music_play(audio, path, -2));
    schultz_audio_destroy(audio);
    PASS();
}

/* ----------------------------------------- a file arriving in pieces */

TEST a_decoder_plays_a_file_written_in_pieces(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle decoder = SCHULTZ_HANDLE_NONE;
    unsigned char wav[1024];
    uint64_t length;
    uint64_t at;

    ASSERT(audio != NULL);
    length = build_wav(wav, sizeof(wav));
    ASSERT(length > 44u);

    ASSERT_EQ(SCHULTZ_OK, schultz_audio_decoder_create(audio, &decoder));
    ASSERT_EQ(0u, schultz_audio_decoder_queued(audio, decoder));

    /*
     * In pieces, the way bytes come off a socket rather than out of a file.
     * The first piece has to carry the start, because that is what the
     * decoder reads to see what kind of file this is.
     */
    for (at = 0u; at < length; at += 100u) {
        uint64_t piece = (length - at < 100u) ? length - at : 100u;

        ASSERT_EQ(SCHULTZ_OK,
                  schultz_audio_decoder_write(audio, decoder, wav + at,
                                              piece));
    }
    ASSERT(schultz_audio_decoder_queued(audio, decoder) > 0u);
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_decoder_play(audio, decoder));

    ASSERT_EQ(SCHULTZ_OK, schultz_audio_decoder_finish(audio, decoder));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_decoder_pause(audio, decoder));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_decoder_destroy(audio, decoder));
    schultz_audio_destroy(audio);
    PASS();
}

TEST a_decoder_with_nothing_in_it_cannot_start(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle decoder = SCHULTZ_HANDLE_NONE;

    ASSERT(audio != NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_decoder_create(audio, &decoder));

    /*
     * Nothing has been written, so there is nothing to work out the format
     * from. Saying so is better than playing silence for ever, and the
     * remedy is in the message: write some bytes first.
     */
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_audio_decoder_play(audio, decoder));
    schultz_audio_destroy(audio);
    PASS();
}

TEST a_decoder_keeps_its_volume_and_its_kind(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle decoder = SCHULTZ_HANDLE_NONE;
    schultz_handle stream = SCHULTZ_HANDLE_NONE;
    unsigned char wav[1024];

    ASSERT(audio != NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_decoder_create(audio, &decoder));
    ASSERT_EQ(1.0f, schultz_audio_decoder_volume(audio, decoder));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_decoder_set_volume(audio, decoder, 0.4f));
    ASSERT_EQ(0.4f, schultz_audio_decoder_volume(audio, decoder));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_decoder_set_volume(audio, decoder, -1.0f));
    ASSERT_FALSE(schultz_audio_decoder_is_playing(audio, decoder));

    /* A fourth kind out of the one table, and still nothing takes another's
     * handle. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_S16, 2u,
                                          48000u, &stream));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_audio_decoder_write(audio, stream, wav, 4u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_audio_stream_write(audio, decoder, wav, 4u));
    schultz_audio_destroy(audio);
    PASS();
}

TEST a_decoder_drops_what_it_has_read(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle decoder = SCHULTZ_HANDLE_NONE;
    unsigned char block[8192];
    uint64_t written = 0u;
    uint64_t i;

    ASSERT(audio != NULL);
    memset(block, 0, sizeof(block));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_decoder_create(audio, &decoder));

    /*
     * More than a station sends in a minute, none of it read, so all of it is
     * still waiting. What this asserts is that writing a lot is allowed and
     * counted honestly; the dropping happens once a decoder is reading, which
     * is the case the play test covers.
     */
    for (i = 0u; i < 64u; i++) {
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_audio_decoder_write(audio, decoder, block,
                                              sizeof(block)));
        written += sizeof(block);
    }
    ASSERT_EQ(written, schultz_audio_decoder_queued(audio, decoder));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_decoder_destroy(audio, decoder));
    schultz_audio_destroy(audio);
    PASS();
}

/* -------------------------------------------------------------- devices */

TEST devices_can_be_listed_and_named(void)
{
    schultz_audio *audio = open_audio();
    uint32_t outputs;
    uint32_t i;

    ASSERT(audio != NULL);
    outputs = schultz_audio_output_count(audio);
    /* The dummy driver offers one, and a real machine offers more. Zero is
     * possible on a machine with no sound at all, and is not a failure. */
    for (i = 0u; i < outputs; i++) {
        ASSERT(schultz_audio_device_name(audio, i) != NULL);
        ASSERT(schultz_audio_device_id(audio, i) != 0u);
    }
    /* Past the end answers nothing rather than wandering off. */
    ASSERT_EQ(NULL, schultz_audio_device_name(audio, outputs + 5u));
    ASSERT_EQ(0u, schultz_audio_device_id(audio, outputs + 5u));

    schultz_audio_input_count(audio);
    schultz_audio_destroy(audio);
    PASS();
}

TEST a_device_can_be_chosen_and_read_back(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle stream = SCHULTZ_HANDLE_NONE;
    uint64_t first;

    ASSERT(audio != NULL);
    /* Zero means the platform's own choice, which is what a system starts
     * with and what follows a headset being plugged in. */
    ASSERT_EQ(0u, schultz_audio_output(audio));
    ASSERT_EQ(0u, schultz_audio_input(audio));

    if (schultz_audio_output_count(audio) == 0u) {
        schultz_audio_destroy(audio);
        SKIP();
    }
    first = schultz_audio_device_id(audio, 0u);
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_set_output(audio, first));
    ASSERT_EQ(first, schultz_audio_output(audio));

    /* And what opens next opens there. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_S16, 2u,
                                          48000u, &stream));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_set_output(audio, 0u));
    ASSERT_EQ(0u, schultz_audio_output(audio));

    /* The microphone is chosen the same way and kept apart from the
     * speakers: a headset is often both, and often neither is wanted. */
    if (schultz_audio_input_count(audio) > 0u) {
        uint64_t mic = schultz_audio_device_id(audio, 0u);

        ASSERT_EQ(SCHULTZ_OK, schultz_audio_set_input(audio, mic));
        ASSERT_EQ(mic, schultz_audio_input(audio));
        ASSERT_EQ(0u, schultz_audio_output(audio)); /* untouched */
        ASSERT_EQ(SCHULTZ_OK, schultz_audio_set_input(audio, 0u));
    }
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT, schultz_audio_set_input(NULL, 0u));
    schultz_audio_destroy(audio);
    PASS();
}

TEST a_change_in_the_devices_is_reported_once(void)
{
    schultz_audio *audio = open_audio();

    ASSERT(audio != NULL);
    /* The first ask has nothing to compare against, so it answers yes. */
    ASSERT(schultz_audio_devices_changed(audio));
    /* Nothing has moved since, so the next one answers no rather than
     * crying wolf on every turn of a loop. */
    ASSERT_FALSE(schultz_audio_devices_changed(audio));
    ASSERT_FALSE(schultz_audio_devices_changed(audio));
    schultz_audio_destroy(audio);
    PASS();
}

/* --------------------------------------------------------------- the mic */

TEST a_recorder_starts_and_stops(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle recorder = SCHULTZ_HANDLE_NONE;
    int32_t result;

    ASSERT(audio != NULL);
    result = schultz_audio_recorder_create(audio, SCHULTZ_AUDIO_S16, 1u,
                                           16000u, &recorder);
    if (result == SCHULTZ_ERR_UNAVAILABLE) {
        /* A machine with no microphone, or one that said no. That is an
         * answer rather than a failure, and it is the one this code means. */
        schultz_audio_destroy(audio);
        SKIP();
    }
    ASSERT_EQ(SCHULTZ_OK, result);
    ASSERT_FALSE(schultz_audio_recorder_is_running(audio, recorder));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_recorder_start(audio, recorder));
    ASSERT(schultz_audio_recorder_is_running(audio, recorder));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_recorder_stop(audio, recorder));
    ASSERT_FALSE(schultz_audio_recorder_is_running(audio, recorder));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_recorder_destroy(audio, recorder));
    schultz_audio_destroy(audio);
    PASS();
}

TEST reading_a_quiet_microphone_takes_nothing(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle recorder = SCHULTZ_HANDLE_NONE;
    unsigned char into[256];
    uint64_t read = 99u;
    int32_t result;

    ASSERT(audio != NULL);
    result = schultz_audio_recorder_create(audio, SCHULTZ_AUDIO_S16, 1u,
                                           16000u, &recorder);
    if (result == SCHULTZ_ERR_UNAVAILABLE) {
        schultz_audio_destroy(audio);
        SKIP();
    }
    ASSERT_EQ(SCHULTZ_OK, result);

    /*
     * Nothing has been captured yet, so there is nothing waiting and a read
     * takes nothing and says so.
     * Reading an empty microphone is ordinary, not an error: a host polls
     * this on its own clock and most of the time the answer is nothing.
     */
    ASSERT_EQ(0u, schultz_audio_recorder_available(audio, recorder));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_recorder_read(audio, recorder, into,
                                                      sizeof(into), &read));
    ASSERT_EQ(0u, read);
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_recorder_clear(audio, recorder));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_recorder_read(audio, recorder, into,
                                          sizeof(into), NULL));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_recorder_destroy(audio, recorder));
    schultz_audio_destroy(audio);
    PASS();
}

TEST a_stream_and_a_recorder_do_not_share_a_handle(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle stream = SCHULTZ_HANDLE_NONE;
    schultz_handle recorder = SCHULTZ_HANDLE_NONE;

    ASSERT(audio != NULL);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_stream_create(audio, SCHULTZ_AUDIO_S16, 2u,
                                          48000u, &stream));
    if (schultz_audio_recorder_create(audio, SCHULTZ_AUDIO_S16, 1u, 16000u,
                                      &recorder) == SCHULTZ_ERR_UNAVAILABLE) {
        schultz_audio_destroy(audio);
        SKIP();
    }
    /*
     * They are numbered from separate tables, so the two may collide as
     * numbers. What must not happen is one being accepted where the other
     * belongs.
     */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_audio_stream_play(audio, recorder));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_audio_recorder_start(audio, stream));
    schultz_audio_destroy(audio);
    PASS();
}

TEST a_recorder_writes_a_wav_file(void)
{
    schultz_audio *audio = open_audio();
    schultz_handle recorder = SCHULTZ_HANDLE_NONE;
    const char *path = "build/tests/recorded.wav";
    unsigned char header[44];
    SDL_IOStream *in;
    int32_t result;

    ASSERT(audio != NULL);
    result = schultz_audio_recorder_create(audio, SCHULTZ_AUDIO_S16, 1u,
                                           16000u, &recorder);
    if (result == SCHULTZ_ERR_UNAVAILABLE) {
        schultz_audio_destroy(audio);
        SKIP();
    }
    ASSERT_EQ(SCHULTZ_OK, result);
    ASSERT_FALSE(schultz_audio_recorder_is_saving(audio, recorder));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_recorder_save(audio, recorder, path));
    ASSERT(schultz_audio_recorder_is_saving(audio, recorder));
    /* Twice over is refused rather than quietly abandoning the first file. */
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED,
              schultz_audio_recorder_save(audio, recorder, path));

    ASSERT_EQ(SCHULTZ_OK, schultz_audio_recorder_start(audio, recorder));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_recorder_stop(audio, recorder));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_recorder_stop_saving(audio, recorder));
    ASSERT_FALSE(schultz_audio_recorder_is_saving(audio, recorder));
    /* Stopping again is allowed and does nothing. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_recorder_stop_saving(audio, recorder));

    /*
     * A file with a real WAV header, whatever the microphone gave. A dummy
     * device gives nothing, so the length may be zero, and a WAV of zero
     * samples is still a WAV that opens.
     */
    in = SDL_IOFromFile(path, "rb");
    ASSERT(in != NULL);
    ASSERT_EQ(sizeof(header), SDL_ReadIO(in, header, sizeof(header)));
    SDL_CloseIO(in);
    ASSERT_EQ(0, SDL_memcmp(header, "RIFF", 4));
    ASSERT_EQ(0, SDL_memcmp(header + 8, "WAVEfmt ", 8));
    ASSERT_EQ(0, SDL_memcmp(header + 36, "data", 4));
    ASSERT_EQ(16u, (uint32_t)header[34]); /* sixteen bits a sample */
    ASSERT_EQ(schultz_audio_recorder_saved(audio, recorder),
              (uint64_t)(header[40] | (header[41] << 8) |
                         (header[42] << 16) | ((uint64_t)header[43] << 24)));

    ASSERT_EQ(SCHULTZ_OK, schultz_audio_recorder_destroy(audio, recorder));
    schultz_audio_destroy(audio);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(the_system_starts_and_stops);
    RUN_TEST(the_volume_is_kept_and_refuses_nonsense);
    RUN_TEST(a_stream_refuses_what_it_cannot_play);
    RUN_TEST(what_is_written_is_waiting_to_be_played);
    RUN_TEST(writing_nothing_is_allowed_and_writing_nowhere_is_not);
    RUN_TEST(a_stream_plays_and_pauses);
    RUN_TEST(a_stream_keeps_its_own_volume);
    RUN_TEST(a_destroyed_stream_is_a_dead_handle);
    RUN_TEST(several_streams_live_at_once);
    RUN_TEST(the_build_carries_no_copyleft_decoder);
    RUN_TEST(every_format_that_should_decode_does);
    RUN_TEST(a_sound_loads_from_memory_and_plays);
    RUN_TEST(bytes_that_are_not_a_sound_say_so);
    RUN_TEST(a_sound_is_not_a_stream);
    RUN_TEST(music_plays_from_a_file);
    RUN_TEST(a_decoder_plays_a_file_written_in_pieces);
    RUN_TEST(a_decoder_with_nothing_in_it_cannot_start);
    RUN_TEST(a_decoder_keeps_its_volume_and_its_kind);
    RUN_TEST(a_decoder_drops_what_it_has_read);
    RUN_TEST(a_recorder_starts_and_stops);
    RUN_TEST(reading_a_quiet_microphone_takes_nothing);
    RUN_TEST(a_recorder_writes_a_wav_file);
    RUN_TEST(devices_can_be_listed_and_named);
    RUN_TEST(a_device_can_be_chosen_and_read_back);
    RUN_TEST(a_change_in_the_devices_is_reported_once);
    RUN_TEST(a_stream_and_a_recorder_do_not_share_a_handle);
    GREATEST_MAIN_END();
}
