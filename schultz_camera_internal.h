/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_camera_internal.h
 * @brief Turning a camera picture upright, apart from the camera.
 *
 * A phone camera is fitted one way round and the phone is held another, so
 * what the sensor reads is rarely what a person is looking at. SDL does not
 * turn the picture for you; it measures how far it is out and hands that over
 * with the frame, as SDL_PROP_SURFACE_ROTATION_FLOAT, leaving the turn to
 * whoever is drawing.
 *
 * The turn itself is arithmetic over a block of pixels, with no camera and no
 * platform in it. It is declared here so that it can be tested against known
 * pictures rather than against a device that has to be held a particular way
 * round.
 *
 * **Not part of the host facing interface.** A host binds to schultz_api.h.
 */

#ifndef SCHULTZ_CAMERA_INTERNAL_H
#define SCHULTZ_CAMERA_INTERNAL_H

#include "schultz_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Copies a camera picture into place, turning it upright on the way.
 *
 * The turn costs nothing on top of the copy that has to happen anyway: the
 * pixels are being moved from the camera's own rows into a packed block
 * either way, and a turn only changes where each one lands.
 *
 * @param from     The camera's pixels, ARGB8888. Must not be NULL.
 * @param width    Width of the picture as the camera gave it.
 * @param height   Height of the picture as the camera gave it.
 * @param pitch    Bytes per row in `from`, which a camera pads.
 * @param quarters Quarter turns clockwise to apply, 0 to 3.
 * @param into     Receives width by height pixels, packed, with the width
 *                 and height swapped for one and three quarter turns. Must
 *                 not be NULL and must have room for width * height of them.
 */
void schultz_camera_upright(const void *from, uint32_t width, uint32_t height,
                            uint32_t pitch, uint32_t quarters, uint32_t *into);

/**
 * @brief How far a frame says it is out, as quarter turns clockwise.
 *
 * @param degrees What the frame reported, which may be negative or above a
 *                full turn.
 * @return 0 to 3. Anything that is not a right angle answers 0, because a
 *         picture cannot be turned by part of one without resampling it, and
 *         a picture the wrong way up is better than a smeared one.
 */
uint32_t schultz_camera_quarters(float degrees);

/**
 * @brief The size a preview should ask to be, given the pictures arriving.
 *
 * A widget here cannot tell layout what shape it is: the vtable has no
 * measure, and a leaf takes its size hints and nothing else. So a preview
 * says what it wants by setting its own preferred size, and this is the rule
 * it uses.
 *
 * Declared apart from the camera so it can be checked against numbers rather
 * than against a phone held a particular way round.
 *
 * @param want_width The width the node actually has, which is not the width
 *                   it asked for: a column stretches a child across itself
 *                   and ignores the request. Anything not positive falls back
 *                   to the picture's own width, which is all there is to go
 *                   on before anything has been laid out.
 * @param frame_w    Width of the pictures arriving. Must not be zero.
 * @param frame_h    Height of them. Must not be zero.
 * @param out_width  Receives the width to ask for. Must not be NULL.
 * @param out_height Receives the height that width implies, from the
 *                   picture's own proportions. Must not be NULL.
 */
void schultz_camera_preview_size(float want_width, uint32_t frame_w,
                                 uint32_t frame_h, float *out_width,
                                 float *out_height);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_CAMERA_INTERNAL_H */
