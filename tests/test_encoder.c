/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_encoder.c - pictures in, packets out, and back again.
 *
 * Nothing here opens a file, a socket or a window. An encoder takes pictures
 * and produces packets; what happens to them is somebody else's problem, and
 * that is exactly what makes this testable in memory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL_hints.h>

#include "greatest.h"
#include "schultz_audio.h"
#include "schultz_video.h"

enum { WIDE = 160u, TALL = 120u };

/*
 * A picture with something in it that moves.
 *
 * A flat colour compresses to almost nothing and would make the bitrate tests
 * meaningless, so this is noise that changes with the frame number: the
 * hardest thing to compress, which is what makes a bitrate ceiling show.
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

/* A picture of one colour, for checking what survives a round trip. */
static void fill_picture(uint32_t *argb, uint32_t colour)
{
    uint32_t i;

    for (i = 0; i < WIDE * TALL; i++) {
        argb[i] = colour;
    }
}

/* ------------------------------------------------------------- refusals */

TEST an_encoder_refuses_what_it_should(void)
{
    schultz_video_encoder *encoder = (schultz_video_encoder *)0x1;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP8, WIDE,
                                           TALL, 30u, 400000u, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP8, 0u,
                                           TALL, 30u, 400000u, &encoder));
    ASSERT_EQ(NULL, encoder);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP8, WIDE,
                                           TALL, 0u, 400000u, &encoder));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP8, WIDE,
                                           TALL, 30u, 0u, &encoder));

    /* And every call answers for an encoder that was never made. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_force_keyframe(NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_set_bitrate(NULL, 100000u));
    ASSERT_EQ(0u, schultz_video_encoder_bitrate(NULL));
    schultz_video_encoder_destroy(NULL);
    PASS();
}

TEST a_picture_the_wrong_size_is_refused(void)
{
    schultz_video_encoder *encoder = NULL;
    uint32_t *argb = (uint32_t *)malloc(WIDE * TALL * sizeof(uint32_t));

    ASSERT(argb != NULL);
    draw_picture(argb, 0u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP8, WIDE,
                                           TALL, 30u, 400000u, &encoder));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_write_frame(encoder, argb, WIDE + 2u,
                                                TALL, 1000000u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_write_frame(encoder, NULL, WIDE, TALL,
                                                1000000u));
    schultz_video_encoder_destroy(encoder);
    free(argb);
    PASS();
}

/*
 * Time has to go forward. A caller handing pictures over out of order has a
 * bug, and renumbering them here would hide it until a decoder somewhere else
 * could not play the result.
 */
TEST a_picture_that_goes_back_in_time_is_refused(void)
{
    schultz_video_encoder *encoder = NULL;
    uint32_t *argb = (uint32_t *)malloc(WIDE * TALL * sizeof(uint32_t));

    ASSERT(argb != NULL);
    draw_picture(argb, 0u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP8, WIDE,
                                           TALL, 30u, 400000u, &encoder));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                                100000000u));
    /* The same moment again, and an earlier one. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                                100000000u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                                50000000u));
    /* And a later one is taken. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                                200000000u));
    schultz_video_encoder_destroy(encoder);
    free(argb);
    PASS();
}

/* ------------------------------------------------------------- packets */

TEST the_first_packet_is_a_keyframe_and_carries_its_time(void)
{
    schultz_video_encoder *encoder = NULL;
    uint32_t *argb = (uint32_t *)malloc(WIDE * TALL * sizeof(uint32_t));
    const void *bytes = NULL;
    uint64_t length = 0u;
    uint64_t when = 0u;
    int32_t keyframe = 0;

    ASSERT(argb != NULL);
    draw_picture(argb, 1u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP8, WIDE,
                                           TALL, 30u, 400000u, &encoder));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                                33000000u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_read_packet(encoder, &bytes, &length,
                                                &when, &keyframe));
    ASSERT(bytes != NULL);
    ASSERT(length > 0u);
    ASSERT_EQ(1, keyframe);
    /* Thirty three milliseconds, carried through and handed back. */
    ASSERT_EQ(33000000u, when);

    /* And there is nothing more from that one picture. */
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED,
              schultz_video_encoder_read_packet(encoder, &bytes, &length,
                                                &when, &keyframe));

    schultz_video_encoder_destroy(encoder);
    free(argb);
    PASS();
}

/*
 * After the first, pictures are coded against the ones before them, and only
 * asking makes another keyframe.
 */
TEST asking_for_a_keyframe_makes_the_next_one_a_keyframe(void)
{
    schultz_video_encoder *encoder = NULL;
    uint32_t *argb = (uint32_t *)malloc(WIDE * TALL * sizeof(uint32_t));
    const void *bytes = NULL;
    uint64_t length = 0u;
    int32_t keyframe = 0;
    uint32_t i;
    uint32_t keyframes = 0u;

    ASSERT(argb != NULL);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP8, WIDE,
                                           TALL, 30u, 400000u, &encoder));

    /* Ten pictures, and the first is the only keyframe among them. */
    for (i = 0; i < 10u; i++) {
        draw_picture(argb, i);
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                                    (uint64_t)(i + 1u) *
                                                    33000000u));
        while (schultz_video_encoder_read_packet(encoder, &bytes, &length,
                                                 NULL, &keyframe)
                   == SCHULTZ_OK) {
            keyframes += (uint32_t)(keyframe != 0);
        }
    }
    ASSERT_EQ(1u, keyframes);

    /* Now ask, and the very next one is one. */
    ASSERT_EQ(SCHULTZ_OK, schultz_video_encoder_force_keyframe(encoder));
    draw_picture(argb, 11u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                                11u * 33000000u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_read_packet(encoder, &bytes, &length,
                                                NULL, &keyframe));
    ASSERT_EQ(1, keyframe);

    schultz_video_encoder_destroy(encoder);
    free(argb);
    PASS();
}

/* How many bytes a hundred pictures cost at a given target. */
static uint64_t bytes_at(uint32_t bitrate)
{
    schultz_video_encoder *encoder = NULL;
    uint32_t *argb = (uint32_t *)malloc(WIDE * TALL * sizeof(uint32_t));
    const void *bytes = NULL;
    uint64_t length = 0u;
    uint64_t total = 0u;
    uint32_t i;

    if (argb == NULL ||
        schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP8, WIDE, TALL,
                                     30u, bitrate, &encoder) != SCHULTZ_OK) {
        free(argb);
        return 0u;
    }
    for (i = 0; i < 100u; i++) {
        draw_picture(argb, i);
        if (schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                              (uint64_t)(i + 1u) * 33000000u)
                != SCHULTZ_OK) {
            continue;
        }
        while (schultz_video_encoder_read_packet(encoder, &bytes, &length,
                                                 NULL, NULL) == SCHULTZ_OK) {
            total += length;
        }
    }
    schultz_video_encoder_destroy(encoder);
    free(argb);
    return total;
}

/*
 * The target actually governs the size. Measured over a hundred pictures
 * rather than one, because rate control works over seconds: one picture at a
 * low target can easily be larger than one at a high one.
 */
TEST the_target_bitrate_governs_how_much_comes_out(void)
{
    uint64_t lean = bytes_at(100000u);
    uint64_t rich = bytes_at(1000000u);

    ASSERT(lean > 0u);
    ASSERT(rich > 0u);
    /* Ten times the target is not ten times the bytes, but it is clearly
     * more; half again is a margin no rate control would miss. */
    ASSERT(rich > lean + lean / 2u);
    PASS();
}

TEST the_bitrate_can_be_changed_and_is_reported(void)
{
    schultz_video_encoder *encoder = NULL;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP8, WIDE,
                                           TALL, 30u, 400000u, &encoder));
    ASSERT_EQ(400000u, schultz_video_encoder_bitrate(encoder));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_set_bitrate(encoder, 250000u));
    ASSERT_EQ(250000u, schultz_video_encoder_bitrate(encoder));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_set_bitrate(encoder, 0u));
    ASSERT_EQ(250000u, schultz_video_encoder_bitrate(encoder));
    schultz_video_encoder_destroy(encoder);
    PASS();
}

/* --------------------------------------------------------- and back again */

TEST a_decoder_refuses_what_it_should(void)
{
    schultz_video_decoder *decoder = NULL;
    const uint32_t *pixels = NULL;
    unsigned char rubbish[32];

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_decoder_create(SCHULTZ_VIDEO_CODEC_VP8, NULL));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_decoder_create(SCHULTZ_VIDEO_CODEC_VP8,
                                           &decoder));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_decoder_write_packet(decoder, NULL, 10u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_decoder_write_packet(decoder, rubbish, 0u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_decoder_read_frame(NULL, &pixels, NULL, NULL));

    /* Nothing has been written, so there is nothing to read. */
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED,
              schultz_video_decoder_read_frame(decoder, &pixels, NULL, NULL));

    memset(rubbish, 0xA5, sizeof(rubbish));
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_video_decoder_write_packet(decoder, rubbish,
                                                 sizeof(rubbish)));
    schultz_video_decoder_destroy(decoder);
    schultz_video_decoder_destroy(NULL);
    PASS();
}

/*
 * The whole loop, with no file and no network in it: a picture goes into an
 * encoder, the packet comes out, goes into a decoder, and a picture comes
 * back the same size and close enough in colour.
 *
 * Close enough, not identical: this is a lossy codec and the colour has been
 * through ARGB, then I420 at half chroma, then VP8, then back. A flat colour
 * is what makes the comparison meaningful at all.
 */
TEST a_picture_survives_the_round_trip(void)
{
    schultz_video_encoder *encoder = NULL;
    schultz_video_decoder *decoder = NULL;
    uint32_t *argb = (uint32_t *)malloc(WIDE * TALL * sizeof(uint32_t));
    const void *bytes = NULL;
    const uint32_t *back = NULL;
    uint64_t length = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t middle;
    int red;
    int green;
    int blue;

    ASSERT(argb != NULL);
    /* A strong green, which no amount of chroma halving turns into anything
     * else. */
    fill_picture(argb, 0xFF208820u);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP8, WIDE,
                                           TALL, 30u, 800000u, &encoder));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_decoder_create(SCHULTZ_VIDEO_CODEC_VP8,
                                           &decoder));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                                33000000u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_read_packet(encoder, &bytes, &length,
                                                NULL, NULL));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_decoder_write_packet(decoder, bytes, length));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_decoder_read_frame(decoder, &back, &width,
                                               &height));
    ASSERT(back != NULL);
    ASSERT_EQ((uint32_t)WIDE, width);
    ASSERT_EQ((uint32_t)TALL, height);

    middle = back[(TALL / 2u) * WIDE + WIDE / 2u];
    red   = (int)((middle >> 16) & 0xFFu);
    green = (int)((middle >> 8) & 0xFFu);
    blue  = (int)(middle & 0xFFu);
    ASSERT_EQ(0xFFu, (middle >> 24) & 0xFFu);   /* opaque, as it went in */
    /* Within a dozen levels of 0x20, 0x88, 0x20. */
    ASSERT(red > 0x20 - 14 && red < 0x20 + 14);
    ASSERT(green > 0x88 - 14 && green < 0x88 + 14);
    ASSERT(blue > 0x20 - 14 && blue < 0x20 + 14);

    schultz_video_decoder_destroy(decoder);
    schultz_video_encoder_destroy(encoder);
    free(argb);
    PASS();
}

/* A run of pictures through both, which is the shape a call has. */
TEST a_run_of_pictures_goes_through_and_comes_back(void)
{
    schultz_video_encoder *encoder = NULL;
    schultz_video_decoder *decoder = NULL;
    uint32_t *argb = (uint32_t *)malloc(WIDE * TALL * sizeof(uint32_t));
    const void *bytes = NULL;
    const uint32_t *back = NULL;
    uint64_t length = 0u;
    uint32_t i;
    uint32_t came_back = 0u;

    ASSERT(argb != NULL);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP8, WIDE,
                                           TALL, 30u, 500000u, &encoder));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_decoder_create(SCHULTZ_VIDEO_CODEC_VP8,
                                           &decoder));

    for (i = 0; i < 30u; i++) {
        draw_picture(argb, i);
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                                    (uint64_t)(i + 1u) *
                                                    33000000u));
        while (schultz_video_encoder_read_packet(encoder, &bytes, &length,
                                                 NULL, NULL) == SCHULTZ_OK) {
            ASSERT_EQ(SCHULTZ_OK,
                      schultz_video_decoder_write_packet(decoder, bytes,
                                                         length));
            while (schultz_video_decoder_read_frame(decoder, &back, NULL, NULL)
                       == SCHULTZ_OK) {
                came_back++;
            }
        }
    }
    ASSERT_EQ(30u, came_back);

    schultz_video_decoder_destroy(decoder);
    schultz_video_encoder_destroy(encoder);
    free(argb);
    PASS();
}

/* ------------------------------------------------------------- how quickly */

TEST the_speed_starts_at_eight_and_reads_back(void)
{
    schultz_video_encoder *encoder = NULL;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP9, WIDE,
                                           TALL, 30u, 400000u, &encoder));
    /* Near the quick end, because this is built for calls. */
    ASSERT_EQ(8u, schultz_video_encoder_speed(encoder));

    ASSERT_EQ(SCHULTZ_OK, schultz_video_encoder_set_speed(encoder, 0u));
    ASSERT_EQ(0u, schultz_video_encoder_speed(encoder));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_encoder_set_speed(encoder, 4u));
    ASSERT_EQ(4u, schultz_video_encoder_speed(encoder));

    /* Above the most there is comes back as the most there is. */
    ASSERT_EQ(SCHULTZ_OK, schultz_video_encoder_set_speed(encoder, 500u));
    ASSERT_EQ((uint32_t)SCHULTZ_VIDEO_SPEED_MOST,
              schultz_video_encoder_speed(encoder));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_set_speed(NULL, 4u));
    ASSERT_EQ(0u, schultz_video_encoder_speed(NULL));

    schultz_video_encoder_destroy(encoder);
    PASS();
}

/* How long twenty pictures take at a given speed, in seconds. */
static double seconds_at_speed(uint32_t codec, uint32_t speed)
{
    schultz_video_encoder *encoder = NULL;
    uint32_t *argb = (uint32_t *)malloc(WIDE * TALL * sizeof(uint32_t));
    const void *bytes = NULL;
    uint64_t length = 0u;
    clock_t started;
    uint32_t i;

    if (argb == NULL ||
        schultz_video_encoder_create(codec, WIDE, TALL, 30u, 400000u,
                                     &encoder) != SCHULTZ_OK) {
        free(argb);
        return 0.0;
    }
    schultz_video_encoder_set_speed(encoder, speed);
    started = clock();
    for (i = 0; i < 20u; i++) {
        draw_picture(argb, i);
        if (schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                              (uint64_t)(i + 1u) * 33000000u)
                != SCHULTZ_OK) {
            continue;
        }
        while (schultz_video_encoder_read_packet(encoder, &bytes, &length,
                                                 NULL, NULL) == SCHULTZ_OK) {
            /* read it out so the next picture can be encoded */
        }
    }
    {
        double spent = (double)(clock() - started) / (double)CLOCKS_PER_SEC;

        schultz_video_encoder_destroy(encoder);
        free(argb);
        return spent;
    }
}

/*
 * The setting does something, and what it does is the point of having it.
 *
 * VP9 rather than VP8, because VP8 in real time mode ignores it: measured,
 * VP8 takes the same six milliseconds a picture whatever it is asked for,
 * while VP9 ranges over sixteenfold. Asserting only twice as slow, against a
 * measured fifteen, leaves room for a busy machine.
 */
TEST asking_vp9_to_go_slower_makes_it_slower(void)
{
    double quick = seconds_at_speed(SCHULTZ_VIDEO_CODEC_VP9,
                                    SCHULTZ_VIDEO_SPEED_MOST);
    double careful = seconds_at_speed(SCHULTZ_VIDEO_CODEC_VP9, 0u);

    ASSERT(quick > 0.0);
    ASSERT(careful > quick * 2.0);
    PASS();
}

/* ---------------------------------------------------------- and in VP9 */

/*
 * The same loop in the other codec. VP9 is the default of the two and
 * makes smaller packets for the same picture; what it
 * costs in processor is a question for whoever is running it, not something
 * a test can settle.
 */
TEST a_picture_survives_the_round_trip_in_vp9(void)
{
    schultz_video_encoder *encoder = NULL;
    schultz_video_decoder *decoder = NULL;
    uint32_t *argb = (uint32_t *)malloc(WIDE * TALL * sizeof(uint32_t));
    const void *bytes = NULL;
    const uint32_t *back = NULL;
    uint64_t length = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t middle;
    int green;

    ASSERT(argb != NULL);
    fill_picture(argb, 0xFF208820u);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP9, WIDE,
                                           TALL, 30u, 800000u, &encoder));
    ASSERT_EQ(SCHULTZ_VIDEO_CODEC_VP9,
              schultz_video_encoder_codec(encoder));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_decoder_create(SCHULTZ_VIDEO_CODEC_VP9,
                                           &decoder));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                                33000000u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_read_packet(encoder, &bytes, &length,
                                                NULL, NULL));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_decoder_write_packet(decoder, bytes, length));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_decoder_read_frame(decoder, &back, &width,
                                               &height));
    ASSERT_EQ((uint32_t)WIDE, width);
    ASSERT_EQ((uint32_t)TALL, height);

    middle = back[(TALL / 2u) * WIDE + WIDE / 2u];
    green = (int)((middle >> 8) & 0xFFu);
    ASSERT_EQ(0xFFu, (middle >> 24) & 0xFFu);
    ASSERT(green > 0x88 - 14 && green < 0x88 + 14);

    schultz_video_decoder_destroy(decoder);
    schultz_video_encoder_destroy(encoder);
    free(argb);
    PASS();
}

/* A codec that is neither is refused rather than guessed at, on both ends. */
TEST a_codec_that_is_neither_is_refused(void)
{
    schultz_video_encoder *encoder = (schultz_video_encoder *)0x1;
    schultz_video_decoder *decoder = (schultz_video_decoder *)0x1;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_NONE, WIDE,
                                           TALL, 30u, 400000u, &encoder));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_encoder_create(99u, WIDE, TALL, 30u, 400000u,
                                           &encoder));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_decoder_create(99u, &decoder));
    ASSERT_EQ(SCHULTZ_VIDEO_CODEC_NONE, schultz_video_encoder_codec(NULL));
    PASS();
}

/*
 * A VP9 packet is not a VP8 packet. Decoding one with the other is refused
 * rather than turned into a picture of noise, which is what a receiver told
 * the wrong codec would otherwise show.
 */
TEST the_wrong_decoder_refuses_the_packet(void)
{
    schultz_video_encoder *encoder = NULL;
    schultz_video_decoder *decoder = NULL;
    uint32_t *argb = (uint32_t *)malloc(WIDE * TALL * sizeof(uint32_t));
    const void *bytes = NULL;
    uint64_t length = 0u;

    ASSERT(argb != NULL);
    draw_picture(argb, 3u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_create(SCHULTZ_VIDEO_CODEC_VP9, WIDE,
                                           TALL, 30u, 400000u, &encoder));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_decoder_create(SCHULTZ_VIDEO_CODEC_VP8,
                                           &decoder));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_write_frame(encoder, argb, WIDE, TALL,
                                                33000000u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_video_encoder_read_packet(encoder, &bytes, &length,
                                                NULL, NULL));
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_video_decoder_write_packet(decoder, bytes, length));

    schultz_video_decoder_destroy(decoder);
    schultz_video_encoder_destroy(encoder);
    free(argb);
    PASS();
}

/* ------------------------------------------------------------ and sound */

/* Twenty milliseconds at forty eight thousand, which is one Opus packet. */
enum { PACKET_FRAMES = 960u };

/* A tone, so the encoder has something with structure to code. */
static void fill_tone(float *samples, uint64_t frames, uint32_t channels,
                      uint64_t from)
{
    uint64_t i;
    uint32_t c;

    for (i = 0; i < frames; i++) {
        /* A rough sine without the maths library: a triangle sounds like
         * something and compresses like something, which is all this needs. */
        uint64_t phase = (from + i) % 120u;
        float value = ((float)phase / 60.0f) - 1.0f;

        for (c = 0; c < channels; c++) {
            samples[i * channels + c] = value * 0.5f;
        }
    }
}

TEST a_sound_encoder_refuses_what_it_should(void)
{
    schultz_audio *audio = NULL;
    schultz_handle encoder = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_audio_create(&audio));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_encoder_create(NULL, 1u, 24000u, &encoder));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_encoder_create(audio, 1u, 24000u, NULL));
    /* Opus takes one channel or two, and nothing else. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_encoder_create(audio, 3u, 24000u, &encoder));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_encoder_create(audio, 1u, 0u, &encoder));

    /* And a handle that names nothing is refused rather than followed. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_audio_encoder_write(audio, SCHULTZ_HANDLE_NONE, NULL,
                                          0u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_audio_encoder_destroy(audio, SCHULTZ_HANDLE_NONE));

    schultz_audio_destroy(audio);
    PASS();
}

/*
 * Opus codes fixed lengths of time, so samples are held until there are
 * enough. Fewer than a packet's worth produces nothing, and that is not an
 * error: it is the ordinary answer when a microphone hands over whatever it
 * had.
 */
TEST samples_are_held_until_there_are_enough_for_a_packet(void)
{
    schultz_audio *audio = NULL;
    schultz_handle encoder = SCHULTZ_HANDLE_NONE;
    float *samples = (float *)malloc(PACKET_FRAMES * 2u * sizeof(float));
    const void *bytes = NULL;
    uint64_t length = 0u;
    uint64_t when = 99u;

    ASSERT(samples != NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_create(&audio));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_create(audio, 1u, 24000u, &encoder));

    /* Half a packet: nothing yet. */
    fill_tone(samples, PACKET_FRAMES / 2u, 1u, 0u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_write(audio, encoder, samples,
                                          PACKET_FRAMES / 2u));
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED,
              schultz_audio_encoder_read_packet(audio, encoder, &bytes,
                                                &length, &when));

    /* The other half, and now there is one. */
    fill_tone(samples, PACKET_FRAMES / 2u, 1u, PACKET_FRAMES / 2u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_write(audio, encoder, samples,
                                          PACKET_FRAMES / 2u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_read_packet(audio, encoder, &bytes,
                                                &length, &when));
    ASSERT(bytes != NULL);
    ASSERT(length > 0u);
    /* The first packet starts at the beginning of the sound. */
    ASSERT_EQ(0u, when);

    /* And there is not another until more is written. */
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED,
              schultz_audio_encoder_read_packet(audio, encoder, &bytes,
                                                &length, &when));

    ASSERT_EQ(SCHULTZ_OK, schultz_audio_encoder_destroy(audio, encoder));
    schultz_audio_destroy(audio);
    free(samples);
    PASS();
}

/* Each packet says how far into the sound it starts, twenty milliseconds
 * further on than the one before it. */
TEST each_packet_says_where_in_the_sound_it_starts(void)
{
    schultz_audio *audio = NULL;
    schultz_handle encoder = SCHULTZ_HANDLE_NONE;
    float *samples = (float *)malloc(PACKET_FRAMES * 3u * sizeof(float));
    const void *bytes = NULL;
    uint64_t length = 0u;
    uint64_t when = 0u;

    ASSERT(samples != NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_create(&audio));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_create(audio, 1u, 24000u, &encoder));

    fill_tone(samples, PACKET_FRAMES * 3u, 1u, 0u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_write(audio, encoder, samples,
                                          PACKET_FRAMES * 3u));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_read_packet(audio, encoder, &bytes,
                                                &length, &when));
    ASSERT_EQ(0u, when);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_read_packet(audio, encoder, &bytes,
                                                &length, &when));
    ASSERT_EQ(20000000u, when);         /* twenty milliseconds */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_read_packet(audio, encoder, &bytes,
                                                &length, &when));
    ASSERT_EQ(40000000u, when);
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED,
              schultz_audio_encoder_read_packet(audio, encoder, &bytes,
                                                &length, &when));

    ASSERT_EQ(SCHULTZ_OK, schultz_audio_encoder_destroy(audio, encoder));
    schultz_audio_destroy(audio);
    free(samples);
    PASS();
}

/* How many bytes a second of sound costs at a given target. */
static uint64_t sound_bytes_at(uint32_t bitrate)
{
    schultz_audio *audio = NULL;
    schultz_handle encoder = SCHULTZ_HANDLE_NONE;
    float *samples = (float *)malloc(PACKET_FRAMES * sizeof(float));
    const void *bytes = NULL;
    uint64_t length = 0u;
    uint64_t total = 0u;
    uint32_t i;

    if (samples == NULL || schultz_audio_create(&audio) != SCHULTZ_OK) {
        free(samples);
        return 0u;
    }
    if (schultz_audio_encoder_create(audio, 1u, bitrate, &encoder)
            != SCHULTZ_OK) {
        schultz_audio_destroy(audio);
        free(samples);
        return 0u;
    }
    for (i = 0; i < 50u; i++) {         /* fifty packets is one second */
        fill_tone(samples, PACKET_FRAMES, 1u, (uint64_t)i * PACKET_FRAMES);
        schultz_audio_encoder_write(audio, encoder, samples, PACKET_FRAMES);
        while (schultz_audio_encoder_read_packet(audio, encoder, &bytes,
                                                 &length, NULL)
                   == SCHULTZ_OK) {
            total += length;
        }
    }
    schultz_audio_encoder_destroy(audio, encoder);
    schultz_audio_destroy(audio);
    free(samples);
    return total;
}

TEST the_sound_target_bitrate_governs_how_much_comes_out(void)
{
    uint64_t lean = sound_bytes_at(8000u);
    uint64_t rich = sound_bytes_at(96000u);

    ASSERT(lean > 0u);
    ASSERT(rich > 0u);
    /* Twelve times the target. Opus honours it closely, so this is a much
     * wider margin than the video side needs. */
    ASSERT(rich > lean * 3u);
    PASS();
}

TEST the_sound_bitrate_can_be_changed(void)
{
    schultz_audio *audio = NULL;
    schultz_handle encoder = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_audio_create(&audio));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_create(audio, 2u, 24000u, &encoder));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_set_bitrate(audio, encoder, 48000u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_encoder_set_bitrate(audio, encoder, 0u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_audio_encoder_set_bitrate(audio, SCHULTZ_HANDLE_NONE,
                                                48000u));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_encoder_destroy(audio, encoder));
    schultz_audio_destroy(audio);
    PASS();
}

/*
 * The whole sound loop: samples in, one Opus packet, samples back. No file,
 * no device, no network -- the same shape as the video round trip above.
 */
TEST sound_survives_the_round_trip(void)
{
    schultz_audio *audio = NULL;
    schultz_handle encoder = SCHULTZ_HANDLE_NONE;
    schultz_handle decoder = SCHULTZ_HANDLE_NONE;
    float *samples = (float *)malloc(PACKET_FRAMES * sizeof(float));
    const void *bytes = NULL;
    const float *back = NULL;
    uint64_t length = 0u;
    uint64_t frames = 0u;

    ASSERT(samples != NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_create(&audio));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_create(audio, 1u, 32000u, &encoder));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_packet_decoder_create(audio, 1u, &decoder));

    /* Nothing written, so nothing to read. */
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED,
              schultz_audio_packet_decoder_read(audio, decoder, &back,
                                                &frames));

    fill_tone(samples, PACKET_FRAMES, 1u, 0u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_write(audio, encoder, samples,
                                          PACKET_FRAMES));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_encoder_read_packet(audio, encoder, &bytes,
                                                &length, NULL));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_packet_decoder_write(audio, decoder, bytes,
                                                 length));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_packet_decoder_read(audio, decoder, &back,
                                                &frames));
    ASSERT(back != NULL);
    /* Twenty milliseconds went in and twenty comes back. */
    ASSERT_EQ((uint64_t)PACKET_FRAMES, frames);

    /* And it is sound rather than silence: Opus is lossy, so this asks
     * whether anything survived, not whether it matches. */
    {
        uint64_t i;
        double loudest = 0.0;

        for (i = 0; i < frames; i++) {
            double value = (back[i] < 0.0f) ? -back[i] : back[i];

            if (value > loudest) {
                loudest = value;
            }
        }
        ASSERT(loudest > 0.05);
    }

    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_packet_decoder_destroy(audio, decoder));
    ASSERT_EQ(SCHULTZ_OK, schultz_audio_encoder_destroy(audio, encoder));
    schultz_audio_destroy(audio);
    free(samples);
    PASS();
}

/* A lost packet is covered rather than dropped, which is what keeps a call
 * going through a bad moment instead of clicking. */
TEST a_lost_packet_is_covered(void)
{
    schultz_audio *audio = NULL;
    schultz_handle decoder = SCHULTZ_HANDLE_NONE;
    const float *back = NULL;
    uint64_t frames = 0u;

    ASSERT_EQ(SCHULTZ_OK, schultz_audio_create(&audio));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_packet_decoder_create(audio, 1u, &decoder));

    /* Nothing, which is how a caller says one was lost. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_packet_decoder_write(audio, decoder, NULL, 0u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_audio_packet_decoder_read(audio, decoder, &back,
                                                &frames));
    ASSERT(back != NULL);
    ASSERT(frames > 0u);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_audio_packet_decoder_create(audio, 5u, &decoder));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_audio_packet_decoder_write(audio, SCHULTZ_HANDLE_NONE,
                                                 NULL, 0u));
    schultz_audio_destroy(audio);
    PASS();
}

SUITE(sound_encoder)
{
    RUN_TEST(sound_survives_the_round_trip);
    RUN_TEST(a_lost_packet_is_covered);
    RUN_TEST(a_sound_encoder_refuses_what_it_should);
    RUN_TEST(samples_are_held_until_there_are_enough_for_a_packet);
    RUN_TEST(each_packet_says_where_in_the_sound_it_starts);
    RUN_TEST(the_sound_target_bitrate_governs_how_much_comes_out);
    RUN_TEST(the_sound_bitrate_can_be_changed);
}

SUITE(encoder)
{
    RUN_TEST(an_encoder_refuses_what_it_should);
    RUN_TEST(a_picture_the_wrong_size_is_refused);
    RUN_TEST(a_picture_that_goes_back_in_time_is_refused);
    RUN_TEST(the_first_packet_is_a_keyframe_and_carries_its_time);
    RUN_TEST(asking_for_a_keyframe_makes_the_next_one_a_keyframe);
    RUN_TEST(the_target_bitrate_governs_how_much_comes_out);
    RUN_TEST(the_bitrate_can_be_changed_and_is_reported);
    RUN_TEST(a_decoder_refuses_what_it_should);
    RUN_TEST(a_picture_survives_the_round_trip);
    RUN_TEST(a_run_of_pictures_goes_through_and_comes_back);
    RUN_TEST(the_speed_starts_at_eight_and_reads_back);
    RUN_TEST(asking_vp9_to_go_slower_makes_it_slower);
    RUN_TEST(a_picture_survives_the_round_trip_in_vp9);
    RUN_TEST(a_codec_that_is_neither_is_refused);
    RUN_TEST(the_wrong_decoder_refuses_the_packet);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    /* The sound encoder needs the sound system, which opens a device. The
     * dummy driver plays to nowhere, so this wants no sound card. */
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(encoder);
    RUN_SUITE(sound_encoder);
    GREATEST_MAIN_END();
}
