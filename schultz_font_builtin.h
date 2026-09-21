/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_font_builtin.h
 * @brief The faces compiled into the library.
 *
 * Internal. The bytes themselves are generated from assets/fonts by
 * scripts/embed_fonts.sh; this is how the font system reaches them.
 *
 * The faces are DejaVu, unmodified, and they keep their own license. See
 * THIRD_PARTY_NOTICES.md.
 */

#ifndef SCHULTZ_FONT_BUILTIN_H
#define SCHULTZ_FONT_BUILTIN_H

#include <stddef.h>
#include <stdint.h>

#include "schultz_style.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How many faces are compiled into the library.
 *
 * @return The number of faces, which is at least one.
 */
uint32_t schultz_font_builtin_count(void);

/**
 * @brief One compiled in face, by position.
 *
 * The bytes are static and read only, so they outlive any font made from
 * them and are never copied or freed.
 *
 * Some of these faces fill a theme font slot and some do not. The two oblique
 * Sans faces are here to complete the Sans family, so that asking for the
 * italic member of a family has an answer without the host loading a file;
 * they fill no slot, and report SCHULTZ_TOKEN_FONT_COUNT.
 *
 * @param index     Which face, from zero up to schultz_font_builtin_count.
 * @param out_size  Receives the length in bytes. Must not be NULL.
 * @param out_token Receives the theme font slot this face fills, or
 *                  SCHULTZ_TOKEN_FONT_COUNT for one that fills none. Must
 *                  not be NULL.
 * @return The face bytes, or NULL for an index past the end.
 */
const unsigned char *schultz_font_builtin_face(uint32_t index,
                                               size_t *out_size,
                                               uint32_t *out_token);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_FONT_BUILTIN_H */
