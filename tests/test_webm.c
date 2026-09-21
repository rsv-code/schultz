/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_webm.c - writing a WebM file, and reading it back again.
 *
 * The writer has no way to be checked on its own: a file is right only if
 * something else can open it. So these encode a few seconds of picture and
 * sound, write a file, and then hand that file to the reader the toolkit
 * already has. What goes in has to come back out.
 *
 * Files are written under build/tests, which the build already makes, and
 * removed on the way out.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_hints.h>

#include "greatest.h"
#include "schultz_audio.h"
#include "schultz_image.h"
#include "schultz_video.h"
#include "schultz_widget.h"

enum {
    WIDE  = 160u,
    TALL  = 120u,
    RATE  = 30u,      /**< Pictures a second. */
    COUNT = 60u       /**< Two seconds of them. */
};

#define MADE_PATH "build/tests/made.webm"

/* Pictures a second as nanoseconds apart, so the times are exact. */
#define STEP_NS (1000000000ull / RATE)

/*
 * Noise that changes every picture. A flat colour compresses to almost
 * nothing, and a file of nearly empty packets would pass tests that a real
 * one fails.
 */
static void draw_picture(uint32_t *argb, uint32_t frame)
{
    uint32_t i;
    uint32_t seed = frame * 2654435761u + 1u;

    for (i = 0; i < WIDE * TALL; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        argb[i] = 0xFF000000u | (seed & 0x00FFFFFFu);
    }
}

/*
 * Writes a whole file: encode COUNT pictures, and enough sound to reach the
 * end of them when asked for.
 *
 * Note that sound is not one packet per picture. An Opus packet is twenty
 * milliseconds and a picture at thirty a second is thirty three, so this
 * feeds sound until it has caught up with where the picture is. A file whose
 * two tracks stop at different times is not wrong, exactly, but it is not
 * what a camera produces and it makes seeking read oddly.
 */
static int32_t write_a_file(const char *path, uint32_t codec, int32_t sound,
                            uint32_t *out_pictures, uint32_t *out_sounds)
{
    schultz_video_writer  *writer  = NULL;
    schultz_video_encoder *encoder = NULL;
    schultz_audio         *audio   = NULL;
    schultz_handle         voice   = SCHULTZ_HANDLE_NONE;
    uint32_t     *argb = NULL;
    float        *pcm  = NULL;
    uint64_t      made_ms = 0u;
    uint32_t      pictures = 0u;
    uint32_t      sounds = 0u;
    uint32_t      i;
    uint32_t      j;
    int32_t       result;

    *out_pictures = 0u;
    *out_sounds   = 0u;

    argb = (uint32_t *)malloc(WIDE * TALL * sizeof(*argb));
    pcm  = (float *)malloc(960u * sizeof(*pcm));
    if (argb == NULL || pcm == NULL) {
        free(argb);
        free(pcm);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    result = schultz_video_writer_create(path, codec, WIDE, TALL, &writer);
    if (result == SCHULTZ_OK) {
        result = schultz_video_encoder_create(codec, WIDE, TALL, RATE,
                                              400000u, &encoder);
    }
    if (result == SCHULTZ_OK && sound) {
        result = schultz_audio_create(&audio);
        if (result == SCHULTZ_OK) {
            result = schultz_audio_encoder_create(audio, 1u, 48000u, &voice);
        }
        if (result == SCHULTZ_OK) {
            result = schultz_video_writer_add_sound(writer, 1u);
        }
    }

    for (i = 0; result == SCHULTZ_OK && i < COUNT; i++) {
        const void *bytes = NULL;
        uint64_t    length = 0u;
        uint64_t    when = 0u;
        int32_t     keyframe = 0;
        uint64_t    at_ns = (uint64_t)i * STEP_NS;

        /* A keyframe every fifth picture, so there is more than one place to
         * seek to. A file with a single keyframe at the start seeks to the
         * start whatever is asked for, and hides every indexing mistake. */
        if (i > 0u && (i % 5u) == 0u) {
            schultz_video_encoder_force_keyframe(encoder);
        }
        draw_picture(argb, i);
        result = schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                                   at_ns);
        while (result == SCHULTZ_OK &&
               schultz_video_encoder_read_packet(encoder, &bytes, &length,
                                                 &when, &keyframe)
                   == SCHULTZ_OK) {
            result = schultz_video_writer_write_picture(writer, bytes, length,
                                                        when, keyframe);
            if (result == SCHULTZ_OK) {
                pictures++;
            }
        }

        while (result == SCHULTZ_OK && voice != SCHULTZ_HANDLE_NONE &&
               made_ms <= at_ns / 1000000ull) {
            for (j = 0; j < 960u; j++) {
                pcm[j] = 0.3f * (float)(((made_ms * 48u + j) % 120u) / 60.0
                                        - 1.0);
            }
            schultz_audio_encoder_write(audio, voice, pcm, 960u);
            while (schultz_audio_encoder_read_packet(audio, voice, &bytes,
                                                     &length, &when)
                   == SCHULTZ_OK) {
                result = schultz_video_writer_write_sound(writer, bytes,
                                                          length, when);
                if (result == SCHULTZ_OK) {
                    sounds++;
                }
            }
            made_ms += 20u;
        }
    }

    if (result == SCHULTZ_OK) {
        result = schultz_video_writer_close(writer);
    } else {
        schultz_video_writer_close(writer);
    }
    schultz_video_encoder_destroy(encoder);
    if (audio != NULL) {
        schultz_audio_encoder_destroy(audio, voice);
        schultz_audio_destroy(audio);
    }
    free(argb);
    free(pcm);
    *out_pictures = pictures;
    *out_sounds   = sounds;
    return result;
}

/* ------------------------------------------------------------ reading back */

typedef struct {
    schultz_tree        *tree;
    schultz_image_table *images;
    schultz_audio       *audio;   /**< The dummy driver; see main. */
    uint64_t             clock_ms;
} reader;

static int32_t reader_setup(reader *r)
{
    int32_t result;

    memset(r, 0, sizeof(*r));
    result = schultz_tree_create(&r->tree);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(r->tree, schultz_rect_make(0, 0, 640, 480));
    result = schultz_image_table_create(&r->images);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_image_table(r->tree, r->images);
    if (schultz_audio_create(&r->audio) == SCHULTZ_OK) {
        schultz_tree_set_audio(r->tree, r->audio);
    }
    return SCHULTZ_OK;
}

static void reader_teardown(reader *r)
{
    schultz_tree_destroy(r->tree);
    schultz_image_table_destroy(r->images);
    if (r->audio != NULL) {
        schultz_audio_destroy(r->audio);
    }
}

/* Runs the tree's clock forward, which is what decodes. */
static void run_for(reader *r, uint32_t ms, uint32_t step_ms)
{
    uint32_t at;

    for (at = 0; at < ms; at += step_ms) {
        r->clock_ms += step_ms;
        schultz_tree_advance(r->tree, r->clock_ms);
    }
}

static schultz_handle play_made(reader *r, const char *path)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    if (schultz_video_create(r->tree, schultz_tree_root(r->tree), 0u, &node)
        != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    schultz_node_set_bounds(r->tree, node, schultz_rect_make(0, 0, WIDE,
                                                             TALL));
    if (schultz_video_open_file(r->tree, node, path) != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    schultz_video_play(r->tree, node);
    return node;
}

/* ---------------------------------------------------------------- refusals */

TEST a_writer_refuses_what_it_should(void)
{
    schultz_video_writer *writer = (schultz_video_writer *)0x1;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_create(MADE_PATH,
                                          SCHULTZ_VIDEO_CODEC_VP9, WIDE,
                                          TALL, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_create(NULL, SCHULTZ_VIDEO_CODEC_VP9,
                                          WIDE, TALL, &writer));
    ASSERT_EQ(NULL, writer);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_create(MADE_PATH,
                                          SCHULTZ_VIDEO_CODEC_VP9, 0u, TALL,
                                          &writer));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_create(MADE_PATH,
                                          SCHULTZ_VIDEO_CODEC_VP9, WIDE, 0u,
                                          &writer));
    /* A codec it cannot name in the file is a codec it will not take. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_create(MADE_PATH, 999u, WIDE, TALL,
                                          &writer));
    /* And a directory that is not there. */
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_video_writer_create("build/tests/no-such-place/x.webm",
                                          SCHULTZ_VIDEO_CODEC_VP9, WIDE,
                                          TALL, &writer));

    /* Every call answers for a writer that was never made. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_add_sound(NULL, 1u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_write_picture(NULL, "x", 1u, 0u, 1));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_write_sound(NULL, "x", 1u, 0u));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_writer_close(NULL));
    PASS();
}

TEST a_sound_track_is_only_one_or_two_channels(void)
{
    schultz_video_writer *writer = NULL;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_writer_create(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP9,
                                          WIDE, TALL, &writer));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_add_sound(writer, 0u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_add_sound(writer, 3u));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_writer_add_sound(writer, 1u));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_writer_add_sound(writer, 2u));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_writer_close(writer));
    remove(MADE_PATH);
    PASS();
}

TEST a_sound_track_cannot_be_added_once_writing_has_started(void)
{
    schultz_video_writer *writer = NULL;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_writer_create(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP9,
                                          WIDE, TALL, &writer));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_writer_write_picture(writer, "packet", 6u, 0u, 1));
    /* The list of tracks sits at the top of the file and has gone out. */
    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_video_writer_add_sound(writer, 1u));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_writer_close(writer));
    remove(MADE_PATH);
    PASS();
}

TEST sound_with_no_sound_track_is_refused(void)
{
    schultz_video_writer *writer = NULL;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_writer_create(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP9,
                                          WIDE, TALL, &writer));
    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_video_writer_write_sound(writer, "packet", 6u, 0u));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_writer_close(writer));
    remove(MADE_PATH);
    PASS();
}

TEST an_empty_packet_is_refused(void)
{
    schultz_video_writer *writer = NULL;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_writer_create(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP9,
                                          WIDE, TALL, &writer));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_write_picture(writer, NULL, 6u, 0u, 1));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_write_picture(writer, "packet", 0u, 0u, 1));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_writer_close(writer));
    remove(MADE_PATH);
    PASS();
}

TEST a_picture_that_goes_back_in_time_is_refused(void)
{
    schultz_video_writer *writer = NULL;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_writer_create(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP9,
                                          WIDE, TALL, &writer));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_writer_write_picture(writer, "packet", 6u,
                                                 500000000u, 1));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_write_picture(writer, "packet", 6u,
                                                 100000000u, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_writer_close(writer));
    remove(MADE_PATH);
    PASS();
}

/*
 * The two tracks do not share a clock.
 *
 * An encoder hands back pictures well ahead of the sound that belongs with
 * them, so the times arriving at the writer jump back and forth between the
 * tracks all the time. A writer that held one "last time" for the whole file
 * would throw nearly every sound packet away.
 */
TEST each_track_keeps_its_own_clock(void)
{
    schultz_video_writer *writer = NULL;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_writer_create(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP9,
                                          WIDE, TALL, &writer));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_writer_add_sound(writer, 1u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_writer_write_picture(writer, "packet", 6u,
                                                 500000000u, 1));
    /* Well behind the picture, and still fine: it is a different track. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_writer_write_sound(writer, "packet", 6u,
                                               20000000u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_writer_write_sound(writer, "packet", 6u,
                                               40000000u));
    /* But a sound packet behind the sound before it is still refused. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_writer_write_sound(writer, "packet", 6u,
                                               20000000u));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_writer_close(writer));
    remove(MADE_PATH);
    PASS();
}

TEST a_writer_that_was_given_nothing_closes_cleanly(void)
{
    schultz_video_writer *writer = NULL;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_writer_create(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP8,
                                          WIDE, TALL, &writer));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_writer_close(writer));
    remove(MADE_PATH);
    PASS();
}

/* -------------------------------------------------------------- what it is */

TEST a_written_file_starts_with_the_ebml_header(void)
{
    uint32_t pictures = 0u;
    uint32_t sounds = 0u;
    unsigned char head[4];
    FILE *file;

    ASSERT_EQ(SCHULTZ_OK, write_a_file(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP9, 0,
                                       &pictures, &sounds));
    ASSERT_EQ(COUNT, pictures);

    file = fopen(MADE_PATH, "rb");
    ASSERT(file != NULL);
    ASSERT_EQ(4u, fread(head, 1u, 4u, file));
    fclose(file);
    /* Every WebM begins with the EBML header. */
    ASSERT_EQ(0x1Au, head[0]);
    ASSERT_EQ(0x45u, head[1]);
    ASSERT_EQ(0xDFu, head[2]);
    ASSERT_EQ(0xA3u, head[3]);
    remove(MADE_PATH);
    PASS();
}

/* ------------------------------------------------------------- round trips */

TEST a_written_vp9_file_reads_back(void)
{
    reader r;
    schultz_handle node;
    uint32_t pictures = 0u;
    uint32_t sounds = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;

    ASSERT_EQ(SCHULTZ_OK, write_a_file(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP9, 0,
                                       &pictures, &sounds));
    ASSERT_EQ(SCHULTZ_OK, reader_setup(&r));
    node = play_made(&r, MADE_PATH);
    ASSERT(node != SCHULTZ_HANDLE_NONE);
    run_for(&r, 200u, 33u);

    ASSERT_EQ((uint32_t)SCHULTZ_VIDEO_CODEC_VP9,
              schultz_video_codec(r.tree, node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_size(r.tree, node, &width, &height));
    ASSERT_EQ(WIDE, width);
    ASSERT_EQ(TALL, height);
    reader_teardown(&r);
    remove(MADE_PATH);
    PASS();
}

TEST a_written_vp8_file_reads_back(void)
{
    reader r;
    schultz_handle node;
    uint32_t pictures = 0u;
    uint32_t sounds = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;

    ASSERT_EQ(SCHULTZ_OK, write_a_file(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP8, 0,
                                       &pictures, &sounds));
    ASSERT_EQ(SCHULTZ_OK, reader_setup(&r));
    node = play_made(&r, MADE_PATH);
    ASSERT(node != SCHULTZ_HANDLE_NONE);
    run_for(&r, 200u, 33u);

    ASSERT_EQ((uint32_t)SCHULTZ_VIDEO_CODEC_VP8,
              schultz_video_codec(r.tree, node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_size(r.tree, node, &width, &height));
    ASSERT_EQ(WIDE, width);
    ASSERT_EQ(TALL, height);
    reader_teardown(&r);
    remove(MADE_PATH);
    PASS();
}

/*
 * How long the film is. Only known once everything has been written, so it is
 * patched into the header at close; a writer that skipped that leaves a file
 * whose length reads as nothing, and a slider with nowhere to slide.
 */
TEST a_written_file_knows_how_long_it_is(void)
{
    reader r;
    schultz_handle node;
    uint32_t pictures = 0u;
    uint32_t sounds = 0u;
    uint64_t duration;

    ASSERT_EQ(SCHULTZ_OK, write_a_file(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP9, 0,
                                       &pictures, &sounds));
    ASSERT_EQ(SCHULTZ_OK, reader_setup(&r));
    node = play_made(&r, MADE_PATH);
    ASSERT(node != SCHULTZ_HANDLE_NONE);
    run_for(&r, 200u, 33u);

    /* COUNT pictures at RATE a second, and the last one starts a frame
     * before the end, so this is a shade under two seconds. */
    duration = schultz_video_duration(r.tree, node);
    ASSERT(duration > 1900u);
    ASSERT(duration < 2010u);
    reader_teardown(&r);
    remove(MADE_PATH);
    PASS();
}

TEST every_picture_written_comes_back(void)
{
    reader r;
    schultz_handle node;
    uint32_t pictures = 0u;
    uint32_t sounds = 0u;

    ASSERT_EQ(SCHULTZ_OK, write_a_file(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP9, 0,
                                       &pictures, &sounds));
    ASSERT_EQ(COUNT, pictures);
    ASSERT_EQ(SCHULTZ_OK, reader_setup(&r));
    node = play_made(&r, MADE_PATH);
    ASSERT(node != SCHULTZ_HANDLE_NONE);
    /* Past the end of the film, a small step at a time so nothing is
     * skipped for being late. */
    run_for(&r, 2600u, 10u);
    ASSERT_EQ(COUNT, schultz_video_frames_shown(r.tree, node));
    reader_teardown(&r);
    remove(MADE_PATH);
    PASS();
}

TEST a_file_written_with_sound_reads_back_with_sound(void)
{
    reader r;
    schultz_handle node;
    uint32_t pictures = 0u;
    uint32_t sounds = 0u;

    ASSERT_EQ(SCHULTZ_OK, write_a_file(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP9, 1,
                                       &pictures, &sounds));
    ASSERT_EQ(COUNT, pictures);
    /* Two seconds of twenty millisecond packets, give or take the last. */
    ASSERT(sounds > 90u);
    ASSERT_EQ(SCHULTZ_OK, reader_setup(&r));
    node = play_made(&r, MADE_PATH);
    ASSERT(node != SCHULTZ_HANDLE_NONE);
    run_for(&r, 300u, 10u);
    ASSERT_EQ(1, schultz_video_has_sound(r.tree, node));
    reader_teardown(&r);
    remove(MADE_PATH);
    PASS();
}

/*
 * Seeking, which is the whole reason the index is written.
 *
 * Keyframes go in every fifth picture, so there is a cue roughly every
 * hundred and sixty milliseconds. Asking for a second should land close to a
 * second. A file with no index lands at the start whatever is asked for,
 * which is what this would catch.
 */
TEST seeking_a_written_file_lands_where_it_was_asked(void)
{
    reader r;
    schultz_handle node;
    uint32_t pictures = 0u;
    uint32_t sounds = 0u;
    uint64_t at;

    ASSERT_EQ(SCHULTZ_OK, write_a_file(MADE_PATH, SCHULTZ_VIDEO_CODEC_VP9, 0,
                                       &pictures, &sounds));
    ASSERT_EQ(SCHULTZ_OK, reader_setup(&r));
    node = play_made(&r, MADE_PATH);
    ASSERT(node != SCHULTZ_HANDLE_NONE);
    run_for(&r, 200u, 33u);

    /* Paused first, so the clock does not carry on while the reader
     * catches up and the number below is where the seek landed rather than
     * where the film has since got to. */
    schultz_video_pause(r.tree, node);
    ASSERT_EQ(SCHULTZ_OK, schultz_video_seek(r.tree, node, 1000u));
    run_for(&r, 100u, 10u);
    at = schultz_video_position(r.tree, node);
    /* It lands on the keyframe at or before what was asked for, never past
     * it, and never back at the start. */
    ASSERT(at > 800u);
    ASSERT(at <= 1000u);
    reader_teardown(&r);
    remove(MADE_PATH);
    PASS();
}

SUITE(writing)
{
    RUN_TEST(a_writer_refuses_what_it_should);
    RUN_TEST(a_sound_track_is_only_one_or_two_channels);
    RUN_TEST(a_sound_track_cannot_be_added_once_writing_has_started);
    RUN_TEST(sound_with_no_sound_track_is_refused);
    RUN_TEST(an_empty_packet_is_refused);
    RUN_TEST(a_picture_that_goes_back_in_time_is_refused);
    RUN_TEST(each_track_keeps_its_own_clock);
    RUN_TEST(a_writer_that_was_given_nothing_closes_cleanly);
    RUN_TEST(a_written_file_starts_with_the_ebml_header);
}

SUITE(round_trip)
{
    RUN_TEST(a_written_vp9_file_reads_back);
    RUN_TEST(a_written_vp8_file_reads_back);
    RUN_TEST(a_written_file_knows_how_long_it_is);
    RUN_TEST(every_picture_written_comes_back);
    RUN_TEST(a_file_written_with_sound_reads_back_with_sound);
    RUN_TEST(seeking_a_written_file_lands_where_it_was_asked);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    /* The sound encoder needs the sound system, which opens a device. The
     * dummy driver plays to nowhere, so this wants no sound card. */
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(writing);
    RUN_SUITE(round_trip);
    GREATEST_MAIN_END();
}
