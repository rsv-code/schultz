/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_voice.h
 * @brief Cleaning up microphone sound before it is sent.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 *
 * A microphone hears more than the person in front of it. It hears the room,
 * the fan, and -- the one that ruins a call -- whatever the speakers are
 * playing, which on a call is the other person's voice coming back at them a
 * fraction of a second late. None of that is the encoder's problem: Opus will
 * faithfully encode a room full of echo. It has to be taken out first.
 *
 * This is that step, and it is four things:
 *
 *   - **Echo cancellation.** What went to the speaker is subtracted from what
 *     the microphone heard. This is the one that matters most and the one
 *     that needs the most from the caller: it has to be given both halves.
 *   - **Noise suppression.** Steady background sound -- a fan, traffic, a
 *     hum -- is learned and removed.
 *   - **Automatic gain.** A quiet talker is brought up and a loud one is
 *     brought down, so the far end does not reach for the volume.
 *   - **Voice detection.** Whether anybody is speaking right now, which is
 *     what lets a caller stop sending during a silence.
 *
 * The first three are on by default, because a host that did not want them
 * would not have created one of these.
 *
 * **Voice detection is off by default, and that is upstream's advice.**
 * speexdsp prints a warning when it is turned on: "The VAD has been replaced
 * by a hack pending a complete rewrite". It works well enough to tell a tone
 * from silence and there is a test here that shows it, but it is not what the
 * other three are and it should not be switched on without knowing that. With
 * it off, schultz_voice_speaking always answers yes, which is the safe answer
 * for a caller that sends whatever it is told is speech.
 *
 * **It works a frame at a time.** The frame size is fixed when the voice is
 * created and every call has to use it, because the echo canceller keeps a
 * running picture of the room that only makes sense at one size. Twenty
 * milliseconds is the usual choice, which is 960 sample frames at 48000 and
 * exactly what one Opus packet holds.
 *
 * **Mono.** Echo cancellation on a stereo microphone is a different and much
 * harder problem, and a call has one voice in it.
 *
 * **None of this happens unless you ask for it.** The microphone itself is
 * schultz_audio_recorder_* over in schultz_audio.h, and it has never heard of
 * any of this: it hands over exactly what it captured. Cleaning up is a
 * separate step a host puts in between, which is why there is no way to turn
 * it off -- not doing it is the default.
 *
 * Each of the four has a switch, and the strengths behind them are in
 * schultz_voice_tuning. The defaults are speexdsp's own and are what most
 * callers should leave alone.
 */

#ifndef SCHULTZ_VOICE_H
#define SCHULTZ_VOICE_H

#include <stdint.h>

#include "schultz.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif

/** @brief One microphone's worth of cleaning up. */
typedef struct schultz_voice schultz_voice;

/**
 * @brief Creates one.
 *
 * @param frames    How many sample frames each call will carry. 960 at 48000
 *                  is twenty milliseconds and is what a call uses. Must not
 *                  be zero.
 * @param rate      Samples a second. Must not be zero.
 * @param out_voice Receives it. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_OUT_OF_MEMORY
 *         or SCHULTZ_ERR_UNAVAILABLE.
 */
int32_t schultz_voice_create(uint32_t frames, uint32_t rate,
                             schultz_voice **out_voice);

/**
 * @brief Closes one. Safe on NULL.
 *
 * @param voice The one to close.
 */
void schultz_voice_destroy(schultz_voice *voice);

/**
 * @brief Cleans one frame of microphone sound, in place.
 *
 * Float samples, mono, at the rate and length given when it was created.
 *
 * `played` is what went to the speaker at the same moment, and is what makes
 * echo cancellation possible: without it there is nothing to subtract, and
 * passing NULL turns that part off for this frame while the rest still runs.
 * A caller that has it should hand over the samples it actually wrote to the
 * sound system, delayed by nothing -- lining the two up is this call's job,
 * not the caller's.
 *
 * @param voice  The voice. Must not be NULL.
 * @param mic    What the microphone heard, replaced by what to send. Must not
 *               be NULL.
 * @param played What went to the speaker, or NULL for none.
 * @param frames How many sample frames. Must be what it was created with.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_voice_clean(schultz_voice *voice, float *mic,
                            const float *played, uint64_t frames);

/**
 * @brief Reports whether somebody was speaking in the last frame cleaned.
 *
 * What a caller uses to stop sending during a silence, which saves most of
 * the bandwidth on most calls. Always true unless schultz_voice_set_detection
 * has turned detection on.
 *
 * @param voice The voice. NULL yields zero.
 * @return Nonzero while somebody is speaking.
 */
int32_t schultz_voice_speaking(const schultz_voice *voice);

/**
 * @brief Turns echo cancellation on or off.
 *
 * On by default. With it off, whatever is passed as `played` to
 * schultz_voice_clean is ignored and the microphone is cleaned up without
 * subtracting anything.
 *
 * @param voice The voice. Must not be NULL.
 * @param on    Nonzero for on.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_voice_set_echo_removal(schultz_voice *voice, int32_t on);

/**
 * @brief Reports whether echo cancellation is on.
 *
 * @param voice The voice. NULL yields zero.
 * @return Nonzero when it is on.
 */
int32_t schultz_voice_echo_removal(const schultz_voice *voice);

/**
 * @brief Turns noise suppression on or off.
 *
 * @param voice The voice. Must not be NULL.
 * @param on    Nonzero for on.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_voice_set_noise_removal(schultz_voice *voice, int32_t on);

/**
 * @brief Reports whether noise suppression is on.
 *
 * @param voice The voice. NULL yields zero.
 * @return Nonzero when it is on.
 */
int32_t schultz_voice_noise_removal(const schultz_voice *voice);

/**
 * @brief Turns automatic gain on or off.
 *
 * This is the switch, not the level. How loud it aims for and how much it is
 * allowed to add are `gain_target` and `gain_ceiling_db` in
 * schultz_voice_tuning.
 *
 * @param voice The voice. Must not be NULL.
 * @param on    Nonzero for on.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_voice_set_gain(schultz_voice *voice, int32_t on);

/**
 * @brief Reports whether automatic gain is on.
 *
 * @param voice The voice. NULL yields zero.
 * @return Nonzero when it is on.
 */
int32_t schultz_voice_gain(const schultz_voice *voice);

/**
 * @brief Turns voice detection on or off.
 *
 * Off by default. See the note at the top of this file: speexdsp itself says
 * its detection is a placeholder, and it prints a warning when it is turned
 * on. With it off, schultz_voice_speaking always answers yes.
 *
 * @param voice The voice. Must not be NULL.
 * @param on    Nonzero for on.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_voice_set_detection(schultz_voice *voice, int32_t on);

/**
 * @brief Reports whether voice detection is on.
 *
 * @param voice The voice. NULL yields zero.
 * @return Nonzero when it is on.
 */
int32_t schultz_voice_detection(const schultz_voice *voice);

/**
 * @brief How hard each of the four pushes.
 *
 * The switches above say whether something happens; this says how much. The
 * defaults are speexdsp's own, are what most callers should leave alone, and
 * are what schultz_voice_tuning_default fills in.
 *
 * The two units are decibels and a fraction of full scale, and neither is a
 * knob to turn blind: -15 dB of noise removal is gentle and -30 is heavy
 * enough to hear the voice being chewed as well.
 */
typedef struct {
    /**
     * Most the residual echo is pushed down by, in decibels, negative.
     * Default -40. This is what is left after the canceller has done its
     * work, so it is a safety net rather than the main mechanism.
     */
    float echo_removal_db;
    /**
     * The same, for while the person at this end is talking. Default -15.
     * Less, on purpose: pushing hard here is what makes somebody sound
     * cut off when both people speak at once.
     */
    float echo_removal_talking_db;
    /**
     * Most the background noise is pushed down by, in decibels, negative.
     * Default -15. The one people actually reach for.
     */
    float noise_removal_db;
    /**
     * How loud automatic gain aims for, as a fraction of full scale from
     * zero to one. Default 0.244, which is speexdsp's 8000 out of 32768.
     */
    float gain_target;
    /**
     * Most gain it is allowed to add, in decibels. Default 30. A ceiling
     * matters: without one, a silent room gets its hiss amplified until it
     * hits the target.
     */
    float gain_ceiling_db;
    /**
     * How sure detection has to be before it calls something speech, as a
     * percentage from zero to a hundred. Default 35.
     */
    float detection_start;
    /**
     * How sure it has to stay to go on calling it speech. Default 20, lower
     * than the start on purpose: it makes detection hold through the quiet
     * parts of a word instead of chopping it up.
     */
    float detection_continue;
} schultz_voice_tuning;

/**
 * @brief Fills in the defaults.
 *
 * @param out_tuning Receives them. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when out_tuning is
 *         NULL.
 */
int32_t schultz_voice_tuning_default(schultz_voice_tuning *out_tuning);

/**
 * @brief Sets how hard each of the four pushes.
 *
 * Values outside what each one can mean are brought into range rather than
 * refused, and the range is what the field's own documentation says. What
 * comes back from schultz_voice_get_tuning is the corrected value, not what
 * was passed.
 *
 * @param voice  The voice. Must not be NULL.
 * @param tuning What to use. Must not be NULL.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_voice_set_tuning(schultz_voice *voice,
                                 const schultz_voice_tuning *tuning);

/**
 * @brief Reads back how hard each of the four pushes.
 *
 * @param voice      The voice. Must not be NULL.
 * @param out_tuning Receives it. Must not be NULL.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_voice_get_tuning(const schultz_voice *voice,
                                 schultz_voice_tuning *out_tuning);

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_VOICE_H */
