/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_voice.c - cleaning up microphone sound.
 *
 * No device and no sound system: this is arithmetic over buffers, which is
 * what makes it testable at all. What is checked is that the four things it
 * claims to do can be seen in the samples that come out, not that they sound
 * good -- that is a judgement no assertion makes.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "schultz_voice.h"

/* Twenty milliseconds at forty eight thousand, which is what a call uses. */
enum { FRAMES = 960u, RATE = 48000u };

/* How loud a buffer is, root mean square, which is the only measure of
 * loudness that survives a signal changing sign. */
static double loudness(const float *samples, uint32_t frames)
{
    double total = 0.0;
    uint32_t i;

    for (i = 0; i < frames; i++) {
        total += (double)samples[i] * (double)samples[i];
    }
    return sqrt(total / (double)frames);
}

/* A steady hiss, which is what noise suppression is meant to remove. */
static void fill_hiss(float *samples, uint32_t frames, uint32_t seed)
{
    uint32_t i;
    uint32_t state = seed * 2654435761u + 1u;

    for (i = 0; i < frames; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        samples[i] = ((float)(state & 0xFFFFu) / 32768.0f - 1.0f) * 0.05f;
    }
}

/* A tone, which is what a voice is closer to than hiss is. */
static void fill_tone(float *samples, uint32_t frames, uint32_t from)
{
    uint32_t i;

    for (i = 0; i < frames; i++) {
        uint32_t phase = (from + i) % 240u;

        samples[i] = ((float)phase / 120.0f - 1.0f) * 0.4f;
    }
}

TEST it_refuses_what_it_should(void)
{
    schultz_voice *voice = (schultz_voice *)0x1;
    float samples[FRAMES];

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_voice_create(FRAMES, RATE, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_voice_create(0u, RATE, &voice));
    ASSERT_EQ(NULL, voice);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_voice_create(FRAMES, 0u, &voice));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_voice_clean(NULL, samples, NULL, FRAMES));
    ASSERT_EQ(0, schultz_voice_speaking(NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_voice_set_noise_removal(NULL, 1));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT, schultz_voice_set_gain(NULL, 1));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_voice_set_detection(NULL, 1));
    schultz_voice_destroy(NULL);
    PASS();
}

/*
 * The frame length is fixed when it is created, because the echo canceller
 * keeps a running picture of the room that only makes sense at one size.
 * A different length is refused rather than quietly padded.
 */
TEST a_frame_of_the_wrong_length_is_refused(void)
{
    schultz_voice *voice = NULL;
    float samples[FRAMES];

    ASSERT_EQ(SCHULTZ_OK, schultz_voice_create(FRAMES, RATE, &voice));
    memset(samples, 0, sizeof(samples));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_voice_clean(voice, samples, NULL, FRAMES / 2u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_voice_clean(voice, samples, NULL, FRAMES * 2u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_voice_clean(voice, samples, NULL, FRAMES));
    schultz_voice_destroy(voice);
    PASS();
}

/*
 * Noise suppression, seen rather than asserted about: a steady hiss fed in
 * for long enough to be learned comes out quieter than it went in. It takes
 * a while -- the suppressor has to hear the noise before it can tell it from
 * anything else -- so this runs a couple of seconds of it.
 */
TEST a_steady_hiss_is_quieter_coming_out(void)
{
    schultz_voice *voice = NULL;
    float *samples = (float *)malloc(FRAMES * sizeof(float));
    double went_in;
    double came_out;
    uint32_t i;

    ASSERT(samples != NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_create(FRAMES, RATE, &voice));
    /* Gain would fight the measurement by turning the quiet hiss back up. */
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_gain(voice, 0));

    for (i = 0; i < 100u; i++) {        /* two seconds */
        fill_hiss(samples, FRAMES, i);
        if (i == 99u) {
            went_in = loudness(samples, FRAMES);
        }
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_voice_clean(voice, samples, NULL, FRAMES));
    }
    came_out = loudness(samples, FRAMES);

    ASSERT(went_in > 0.0);
    ASSERT(came_out < went_in);
    schultz_voice_destroy(voice);
    free(samples);
    PASS();
}

/*
 * Voice detection. Silence is not speech and a tone is, which is the whole
 * of what a caller needs from it.
 */
TEST silence_is_not_speech_and_a_tone_is(void)
{
    schultz_voice *voice = NULL;
    float *samples = (float *)malloc(FRAMES * sizeof(float));
    uint32_t i;
    int32_t on_silence;
    int32_t on_tone;

    ASSERT(samples != NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_create(FRAMES, RATE, &voice));
    /* Off by default, so this is the one test that asks for it. */
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_detection(voice, 1));

    for (i = 0; i < 50u; i++) {
        memset(samples, 0, FRAMES * sizeof(float));
        schultz_voice_clean(voice, samples, NULL, FRAMES);
    }
    on_silence = schultz_voice_speaking(voice);

    for (i = 0; i < 50u; i++) {
        fill_tone(samples, FRAMES, i * FRAMES);
        schultz_voice_clean(voice, samples, NULL, FRAMES);
    }
    on_tone = schultz_voice_speaking(voice);

    ASSERT_EQ(0, on_silence);
    ASSERT_EQ(1, on_tone);

    schultz_voice_destroy(voice);
    free(samples);
    PASS();
}

/* With detection off it always says yes, which is the safe answer for a
 * caller that sends whatever it is told is speech. */
TEST with_detection_off_everything_counts_as_speech(void)
{
    schultz_voice *voice = NULL;
    float *samples = (float *)malloc(FRAMES * sizeof(float));
    uint32_t i;

    ASSERT(samples != NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_create(FRAMES, RATE, &voice));
    /* Off is the default; saying so explicitly is what this test is about. */
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_detection(voice, 0));

    for (i = 0; i < 20u; i++) {
        memset(samples, 0, FRAMES * sizeof(float));
        schultz_voice_clean(voice, samples, NULL, FRAMES);
    }
    ASSERT_EQ(1, schultz_voice_speaking(voice));

    schultz_voice_destroy(voice);
    free(samples);
    PASS();
}

/*
 * Echo cancellation. The microphone hears exactly what the speaker played,
 * which is the worst case and the easiest to measure: given both halves, what
 * comes out is quieter than what went in.
 */
TEST what_the_speaker_played_is_taken_back_out(void)
{
    schultz_voice *voice = NULL;
    float *mic = (float *)malloc(FRAMES * sizeof(float));
    float *played = (float *)malloc(FRAMES * sizeof(float));
    double went_in = 0.0;
    double came_out = 0.0;
    uint32_t i;

    ASSERT(mic != NULL);
    ASSERT(played != NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_create(FRAMES, RATE, &voice));
    /* Both would change the loudness for their own reasons. */
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_gain(voice, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_noise_removal(voice, 0));

    for (i = 0; i < 200u; i++) {        /* four seconds to learn the room */
        fill_tone(played, FRAMES, i * FRAMES);
        memcpy(mic, played, FRAMES * sizeof(float));
        if (i == 199u) {
            went_in = loudness(mic, FRAMES);
        }
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_voice_clean(voice, mic, played, FRAMES));
    }
    came_out = loudness(mic, FRAMES);

    ASSERT(went_in > 0.0);
    ASSERT(came_out < went_in);

    schultz_voice_destroy(voice);
    free(mic);
    free(played);
    PASS();
}

/* Every switch answers with what it was set to, and the four start where the
 * header says they do. */
TEST every_switch_reads_back_what_it_was_set_to(void)
{
    schultz_voice *voice = NULL;

    ASSERT_EQ(SCHULTZ_OK, schultz_voice_create(FRAMES, RATE, &voice));

    /* Three on, detection off, as documented. */
    ASSERT_EQ(1, schultz_voice_echo_removal(voice));
    ASSERT_EQ(1, schultz_voice_noise_removal(voice));
    ASSERT_EQ(1, schultz_voice_gain(voice));
    ASSERT_EQ(0, schultz_voice_detection(voice));

    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_echo_removal(voice, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_noise_removal(voice, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_gain(voice, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_detection(voice, 1));
    ASSERT_EQ(0, schultz_voice_echo_removal(voice));
    ASSERT_EQ(0, schultz_voice_noise_removal(voice));
    ASSERT_EQ(0, schultz_voice_gain(voice));
    ASSERT_EQ(1, schultz_voice_detection(voice));

    /* And they answer for nothing at all rather than following it. */
    ASSERT_EQ(0, schultz_voice_echo_removal(NULL));
    ASSERT_EQ(0, schultz_voice_noise_removal(NULL));
    ASSERT_EQ(0, schultz_voice_gain(NULL));
    ASSERT_EQ(0, schultz_voice_detection(NULL));

    schultz_voice_destroy(voice);
    PASS();
}

/*
 * Turning echo cancellation off means what was played is ignored, even when
 * it is handed over. The microphone heard exactly the speaker, so with it on
 * the result is quieter and with it off it is not.
 */
TEST echo_cancellation_can_be_switched_off(void)
{
    schultz_voice *voice = NULL;
    float *mic = (float *)malloc(FRAMES * sizeof(float));
    float *played = (float *)malloc(FRAMES * sizeof(float));
    double went_in = 0.0;
    double came_out = 0.0;
    uint32_t i;

    ASSERT(mic != NULL);
    ASSERT(played != NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_create(FRAMES, RATE, &voice));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_gain(voice, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_noise_removal(voice, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_echo_removal(voice, 0));

    for (i = 0; i < 200u; i++) {
        fill_tone(played, FRAMES, i * FRAMES);
        memcpy(mic, played, FRAMES * sizeof(float));
        if (i == 199u) {
            went_in = loudness(mic, FRAMES);
        }
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_voice_clean(voice, mic, played, FRAMES));
    }
    came_out = loudness(mic, FRAMES);

    /* Nothing was taken out, so what is left is what went in. */
    ASSERT(went_in > 0.0);
    ASSERT(came_out > went_in * 0.9);

    schultz_voice_destroy(voice);
    free(mic);
    free(played);
    PASS();
}

/* The defaults are speexdsp's own, and they are what a new voice starts on. */
TEST the_tuning_starts_on_the_defaults(void)
{
    schultz_voice *voice = NULL;
    schultz_voice_tuning fresh;
    schultz_voice_tuning now;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_voice_tuning_default(NULL));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_tuning_default(&fresh));
    ASSERT_EQ(-40, (int)fresh.echo_removal_db);
    ASSERT_EQ(-15, (int)fresh.echo_removal_talking_db);
    ASSERT_EQ(-15, (int)fresh.noise_removal_db);
    ASSERT_EQ(30, (int)fresh.gain_ceiling_db);
    ASSERT_EQ(35, (int)fresh.detection_start);
    ASSERT_EQ(20, (int)fresh.detection_continue);
    ASSERT(fresh.gain_target > 0.24f && fresh.gain_target < 0.25f);

    ASSERT_EQ(SCHULTZ_OK, schultz_voice_create(FRAMES, RATE, &voice));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_get_tuning(voice, &now));
    ASSERT_EQ((int)fresh.echo_removal_db, (int)now.echo_removal_db);
    ASSERT_EQ((int)fresh.noise_removal_db, (int)now.noise_removal_db);
    ASSERT_EQ((int)fresh.gain_ceiling_db, (int)now.gain_ceiling_db);
    ASSERT_EQ((int)fresh.detection_start, (int)now.detection_start);

    schultz_voice_destroy(voice);
    PASS();
}

/*
 * What is set comes back, and what is out of range comes back corrected
 * rather than refused. A reduction given as a positive number is the case
 * that matters: speexdsp silently negates it, so this has to as well or the
 * getter would lie about what is in force.
 */
TEST the_tuning_reads_back_corrected(void)
{
    schultz_voice *voice = NULL;
    schultz_voice_tuning want;
    schultz_voice_tuning now;

    ASSERT_EQ(SCHULTZ_OK, schultz_voice_create(FRAMES, RATE, &voice));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_tuning_default(&want));

    want.noise_removal_db = -25.0f;
    want.detection_start  = 60.0f;
    want.gain_target      = 0.5f;
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_tuning(voice, &want));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_get_tuning(voice, &now));
    ASSERT_EQ(-25, (int)now.noise_removal_db);
    ASSERT_EQ(60, (int)now.detection_start);
    ASSERT(now.gain_target > 0.49f && now.gain_target < 0.51f);

    /* A reduction written the wrong way round is taken as meant. */
    want.noise_removal_db = 20.0f;
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_tuning(voice, &want));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_get_tuning(voice, &now));
    ASSERT_EQ(-20, (int)now.noise_removal_db);

    /* And what cannot be meant is brought into range. */
    want.gain_target     = 4.0f;
    want.detection_start = -10.0f;
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_tuning(voice, &want));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_get_tuning(voice, &now));
    ASSERT_EQ(1, (int)now.gain_target);
    ASSERT_EQ(0, (int)now.detection_start);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_voice_set_tuning(NULL, &want));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_voice_set_tuning(voice, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_voice_get_tuning(voice, NULL));

    schultz_voice_destroy(voice);
    PASS();
}

/* Turning noise removal up removes more of it, which is the whole point of
 * the number being there. */
TEST turning_noise_removal_up_removes_more(void)
{
    schultz_voice *voice = NULL;
    float *samples = (float *)malloc(FRAMES * sizeof(float));
    schultz_voice_tuning tuning;
    double gentle;
    double heavy;
    uint32_t i;

    ASSERT(samples != NULL);

    ASSERT_EQ(SCHULTZ_OK, schultz_voice_create(FRAMES, RATE, &voice));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_gain(voice, 0));
    for (i = 0; i < 100u; i++) {
        fill_hiss(samples, FRAMES, i);
        schultz_voice_clean(voice, samples, NULL, FRAMES);
    }
    gentle = loudness(samples, FRAMES);
    schultz_voice_destroy(voice);

    ASSERT_EQ(SCHULTZ_OK, schultz_voice_create(FRAMES, RATE, &voice));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_gain(voice, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_tuning_default(&tuning));
    tuning.noise_removal_db = -40.0f;
    ASSERT_EQ(SCHULTZ_OK, schultz_voice_set_tuning(voice, &tuning));
    for (i = 0; i < 100u; i++) {
        fill_hiss(samples, FRAMES, i);
        schultz_voice_clean(voice, samples, NULL, FRAMES);
    }
    heavy = loudness(samples, FRAMES);
    schultz_voice_destroy(voice);

    ASSERT(gentle > 0.0);
    ASSERT(heavy < gentle);

    free(samples);
    PASS();
}

SUITE(voice)
{
    RUN_TEST(every_switch_reads_back_what_it_was_set_to);
    RUN_TEST(echo_cancellation_can_be_switched_off);
    RUN_TEST(the_tuning_starts_on_the_defaults);
    RUN_TEST(the_tuning_reads_back_corrected);
    RUN_TEST(turning_noise_removal_up_removes_more);
    RUN_TEST(it_refuses_what_it_should);
    RUN_TEST(a_frame_of_the_wrong_length_is_refused);
    RUN_TEST(a_steady_hiss_is_quieter_coming_out);
    RUN_TEST(silence_is_not_speech_and_a_tone_is);
    RUN_TEST(with_detection_off_everything_counts_as_speech);
    RUN_TEST(what_the_speaker_played_is_taken_back_out);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(voice);
    GREATEST_MAIN_END();
}
