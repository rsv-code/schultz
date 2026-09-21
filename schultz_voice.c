/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_voice.c
 * @brief Cleaning up microphone sound, over speexdsp.
 *
 * Thin, and deliberately so. speexdsp has been doing this since 2002 and its
 * echo canceller is the one most open source calls use; what is left for this
 * file is the shape of the interface and the one conversion speexdsp needs.
 *
 * **The conversion.** speexdsp works in sixteen bit samples and everything
 * else here is float. The two buffers that holds are the whole of this file's
 * state beyond speexdsp's own.
 *
 * **The order.** Echo first, then the rest. Subtracting what the speaker
 * played has to happen before noise suppression decides what is noise, or the
 * far end's voice is learned as background and removed along with the fan.
 */

#include "schultz_voice.h"

#include <stdlib.h>
#include <string.h>

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

/** What one of these carries. */
struct schultz_voice {
    SpeexEchoState       *echo;
    SpeexPreprocessState *clean;
    uint32_t              frames;
    uint32_t              rate;
    /*
     * The sixteen bit buffers speexdsp works in. Three of them: what the
     * microphone heard, what the speaker played, and where the echo canceller
     * puts its answer.
     */
    int16_t              *mic;
    int16_t              *played;
    int16_t              *out;
    int32_t               speaking;
    int32_t               detecting;
    int32_t               removing_echo;
    int32_t               removing_noise;
    int32_t               gaining;
    schultz_voice_tuning  tuning;
};

/*
 * Bringing a value into range rather than refusing it.
 *
 * Refusing would mean a caller that nudged a slider one step too far got an
 * error instead of the strongest setting there is, which is not what a person
 * moving a slider means.
 */
static float schultz_voice_within(float value, float low, float high)
{
    if (value < low)  { return low; }
    if (value > high) { return high; }
    return value;
}

/* Hands the whole tuning to speexdsp. Called whenever any of it changes. */
static void schultz_voice_apply(schultz_voice *voice)
{
    spx_int32_t value;
    float level;

    value = (spx_int32_t)voice->tuning.noise_removal_db;
    speex_preprocess_ctl(voice->clean, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,
                         &value);
    value = (spx_int32_t)voice->tuning.echo_removal_db;
    speex_preprocess_ctl(voice->clean, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS,
                         &value);
    value = (spx_int32_t)voice->tuning.echo_removal_talking_db;
    speex_preprocess_ctl(voice->clean,
                         SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &value);
    value = (spx_int32_t)voice->tuning.gain_ceiling_db;
    speex_preprocess_ctl(voice->clean, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN,
                         &value);
    value = (spx_int32_t)voice->tuning.detection_start;
    speex_preprocess_ctl(voice->clean, SPEEX_PREPROCESS_SET_PROB_START,
                         &value);
    value = (spx_int32_t)voice->tuning.detection_continue;
    speex_preprocess_ctl(voice->clean, SPEEX_PREPROCESS_SET_PROB_CONTINUE,
                         &value);
    /*
     * The one that is not in the same units. speexdsp counts loudness in
     * sample values from zero to 32768; this interface counts it as a
     * fraction of full scale, because that is what its samples are.
     */
    level = voice->tuning.gain_target * 32768.0f;
    speex_preprocess_ctl(voice->clean, SPEEX_PREPROCESS_SET_AGC_LEVEL,
                         &level);
}

/* Float to sixteen bit, clamped. A sample outside the range is a caller's
 * mistake rather than something to wrap around. */
static void schultz_voice_to_short(const float *from, int16_t *to,
                                   uint32_t frames)
{
    uint32_t i;

    for (i = 0; i < frames; i++) {
        float value = from[i] * 32767.0f;

        if (value > 32767.0f)  { value = 32767.0f; }
        if (value < -32768.0f) { value = -32768.0f; }
        to[i] = (int16_t)value;
    }
}

static void schultz_voice_to_float(const int16_t *from, float *to,
                                   uint32_t frames)
{
    uint32_t i;

    for (i = 0; i < frames; i++) {
        to[i] = (float)from[i] / 32768.0f;
    }
}

int32_t schultz_voice_create(uint32_t frames, uint32_t rate,
                             schultz_voice **out_voice)
{
    schultz_voice *voice;
    int on = 1;
    int rate_now;

    if (out_voice == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_voice = NULL;
    if (frames == 0u || rate == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    voice = (schultz_voice *)calloc(1, sizeof(*voice));
    if (voice == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    voice->frames = frames;
    voice->rate   = rate;

    /*
     * Ten frames of tail, which at twenty milliseconds a frame is two tenths
     * of a second. That is how long an echo can take to come back around a
     * room and through the far end, and a tail shorter than the real echo
     * cancels nothing. Longer costs processor for no gain in a small room.
     */
    voice->echo = speex_echo_state_init((int)frames, (int)frames * 10);
    voice->clean = speex_preprocess_state_init((int)frames, (int)rate);
    voice->mic    = (int16_t *)calloc(frames, sizeof(int16_t));
    voice->played = (int16_t *)calloc(frames, sizeof(int16_t));
    voice->out    = (int16_t *)calloc(frames, sizeof(int16_t));
    if (voice->echo == NULL || voice->clean == NULL || voice->mic == NULL ||
        voice->played == NULL || voice->out == NULL) {
        schultz_voice_destroy(voice);
        return SCHULTZ_ERR_UNAVAILABLE;
    }

    rate_now = (int)rate;
    speex_echo_ctl(voice->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate_now);
    /*
     * The preprocessor is told about the echo canceller so it can use what
     * the canceller learned about the room to finish the job. Without this
     * the two work separately and the residual echo stays.
     */
    speex_preprocess_ctl(voice->clean, SPEEX_PREPROCESS_SET_ECHO_STATE,
                         voice->echo);

    schultz_voice_tuning_default(&voice->tuning);
    schultz_voice_apply(voice);
    voice->removing_echo  = 1;
    voice->removing_noise = 1;
    voice->gaining        = 1;

    /*
     * Three on. A host that did not want them would not have made one.
     *
     * Detection is the fourth and it stays off, because speexdsp says so
     * itself: turning it on prints "The VAD has been replaced by a hack
     * pending a complete rewrite" to the terminal. Something upstream
     * describes that way is not something to switch on for everybody by
     * default, and the warning alone would be a mystery in a host's log.
     */
    speex_preprocess_ctl(voice->clean, SPEEX_PREPROCESS_SET_DENOISE, &on);
    speex_preprocess_ctl(voice->clean, SPEEX_PREPROCESS_SET_AGC, &on);
    voice->detecting = 0;
    voice->speaking  = 1;

    *out_voice = voice;
    return SCHULTZ_OK;
}

void schultz_voice_destroy(schultz_voice *voice)
{
    if (voice == NULL) {
        return;
    }
    if (voice->clean != NULL) {
        speex_preprocess_state_destroy(voice->clean);
    }
    if (voice->echo != NULL) {
        speex_echo_state_destroy(voice->echo);
    }
    free(voice->mic);
    free(voice->played);
    free(voice->out);
    free(voice);
}

int32_t schultz_voice_clean(schultz_voice *voice, float *mic,
                            const float *played, uint64_t frames)
{
    int speaking;

    if (voice == NULL || mic == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (frames != (uint64_t)voice->frames) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_voice_to_short(mic, voice->mic, voice->frames);

    if (played != NULL && voice->removing_echo) {
        schultz_voice_to_short(played, voice->played, voice->frames);
        speex_echo_cancellation(voice->echo, voice->mic, voice->played,
                                voice->out);
        memcpy(voice->mic, voice->out,
               (size_t)voice->frames * sizeof(int16_t));
    }

    speaking = speex_preprocess_run(voice->clean, voice->mic);
    /*
     * With detection off speex still answers, and its answer is meaningless
     * because it was not asked to look. Saying yes is the safe answer for a
     * caller that sends whatever it is told is speech.
     */
    voice->speaking = voice->detecting ? (speaking != 0) : 1;

    schultz_voice_to_float(voice->mic, mic, voice->frames);
    return SCHULTZ_OK;
}

int32_t schultz_voice_speaking(const schultz_voice *voice)
{
    return (voice == NULL) ? 0 : voice->speaking;
}

int32_t schultz_voice_set_echo_removal(schultz_voice *voice, int32_t on)
{
    if (voice == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    voice->removing_echo = on ? 1 : 0;
    return SCHULTZ_OK;
}

int32_t schultz_voice_echo_removal(const schultz_voice *voice)
{
    return (voice == NULL) ? 0 : voice->removing_echo;
}

int32_t schultz_voice_set_noise_removal(schultz_voice *voice, int32_t on)
{
    int value = on ? 1 : 0;

    if (voice == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    speex_preprocess_ctl(voice->clean, SPEEX_PREPROCESS_SET_DENOISE, &value);
    voice->removing_noise = value;
    return SCHULTZ_OK;
}

int32_t schultz_voice_noise_removal(const schultz_voice *voice)
{
    return (voice == NULL) ? 0 : voice->removing_noise;
}

int32_t schultz_voice_set_gain(schultz_voice *voice, int32_t on)
{
    int value = on ? 1 : 0;

    if (voice == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    speex_preprocess_ctl(voice->clean, SPEEX_PREPROCESS_SET_AGC, &value);
    voice->gaining = value;
    return SCHULTZ_OK;
}

int32_t schultz_voice_gain(const schultz_voice *voice)
{
    return (voice == NULL) ? 0 : voice->gaining;
}

int32_t schultz_voice_set_detection(schultz_voice *voice, int32_t on)
{
    int value = on ? 1 : 0;

    if (voice == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    speex_preprocess_ctl(voice->clean, SPEEX_PREPROCESS_SET_VAD, &value);
    voice->detecting = value;
    if (!value) {
        voice->speaking = 1;
    }
    return SCHULTZ_OK;
}

int32_t schultz_voice_detection(const schultz_voice *voice)
{
    return (voice == NULL) ? 0 : voice->detecting;
}

int32_t schultz_voice_tuning_default(schultz_voice_tuning *out_tuning)
{
    if (out_tuning == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * speexdsp's own, read out of its preprocess.c rather than guessed at.
     * The gain target is its 8000 of 32768 written as the fraction this
     * interface counts in.
     */
    out_tuning->echo_removal_db         = -40.0f;
    out_tuning->echo_removal_talking_db = -15.0f;
    out_tuning->noise_removal_db        = -15.0f;
    out_tuning->gain_target             = 8000.0f / 32768.0f;
    out_tuning->gain_ceiling_db         = 30.0f;
    out_tuning->detection_start         = 35.0f;
    out_tuning->detection_continue      = 20.0f;
    return SCHULTZ_OK;
}

/* A reduction in decibels is negative by definition, and speexdsp takes the
 * absolute value and negates it whatever it is given. Doing the same here,
 * and keeping the result, is what lets the getter tell the truth. */
static float schultz_voice_reduction(float value)
{
    float size = (value < 0.0f) ? -value : value;

    return -schultz_voice_within(size, 0.0f, 100.0f);
}

int32_t schultz_voice_set_tuning(schultz_voice *voice,
                                 const schultz_voice_tuning *tuning)
{
    schultz_voice_tuning want;

    if (voice == NULL || tuning == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    want = *tuning;
    want.echo_removal_db = schultz_voice_reduction(want.echo_removal_db);
    want.echo_removal_talking_db =
        schultz_voice_reduction(want.echo_removal_talking_db);
    want.noise_removal_db = schultz_voice_reduction(want.noise_removal_db);
    want.gain_target = schultz_voice_within(want.gain_target, 0.0f, 1.0f);
    want.gain_ceiling_db =
        schultz_voice_within(want.gain_ceiling_db, 0.0f, 100.0f);
    want.detection_start =
        schultz_voice_within(want.detection_start, 0.0f, 100.0f);
    want.detection_continue =
        schultz_voice_within(want.detection_continue, 0.0f, 100.0f);

    voice->tuning = want;
    schultz_voice_apply(voice);
    return SCHULTZ_OK;
}

int32_t schultz_voice_get_tuning(const schultz_voice *voice,
                                 schultz_voice_tuning *out_tuning)
{
    if (voice == NULL || out_tuning == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_tuning = voice->tuning;
    return SCHULTZ_OK;
}
