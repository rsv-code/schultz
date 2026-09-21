/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_image_internal.h
 * @brief The one thing the renderer needs from an image that nothing else
 *        should see.
 *
 * A decoded image is a ThorVG picture. Saying so in the public header would
 * put a backend type in the toolkit's own interface, so it is said here
 * instead, and only the ThorVG backend includes this.
 */

#ifndef SCHULTZ_IMAGE_INTERNAL_H
#define SCHULTZ_IMAGE_INTERNAL_H

#include <thorvg_capi.h>

#include "schultz_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Returns a drawable copy of a loaded image.
 *
 * A copy, because a canvas takes ownership of what is added to it and drops
 * everything each frame, while the table holds its picture for the life of the
 * table. The copy shares the decoded pixels rather than decoding again.
 *
 * @param table The table holding it. NULL yields NULL.
 * @param image A handle from one of the load calls.
 * @return A picture the caller hands to a canvas, or NULL when there is no
 *         such image.
 */
Tvg_Paint schultz_image_picture(const schultz_image_table *table,
                                schultz_handle image);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_IMAGE_INTERNAL_H */
