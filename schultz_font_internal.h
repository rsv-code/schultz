/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_font_internal.h
 * @brief Font representation shared between the font and text modules.
 *
 * Internal to the text pipeline. Nothing outside schultz_font.c and
 * schultz_text.c should include this; everything else names a font by handle.
 */

#ifndef SCHULTZ_FONT_INTERNAL_H
#define SCHULTZ_FONT_INTERNAL_H

#include <stddef.h>

#include <ft2build.h>
#include FT_COLOR_H
#include FT_FREETYPE_H
/* The OS/2 table, which is where a face says how it wants to be struck
 * through. FT_Get_Sfnt_Table and TT_OS2 both come from here. */
#include FT_TRUETYPE_TABLES_H

#include <hb.h>

#include "schultz_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One loaded face bound to one pixel size. */
typedef struct {
    FT_Face        face;    /**< FreeType face, owns rasterization. */
    hb_font_t     *hb_font; /**< HarfBuzz view of the same face. */
    /**
     * The face bytes. FreeType points into these for the life of the face,
     * so they have to outlive it whoever owns them.
     */
    const unsigned char *data;
    size_t         size;    /**< Length of data in bytes. */
    float          size_px; /**< Pixel size the face is bound to. */
    /**
     * Where it was loaded from, owned here, or NULL for a face handed over
     * as bytes. This is what lets the same face be loaded again at another
     * size; a face with no path is loaded again from its bytes instead.
     */
    char          *path;
    /**
     * Nonzero when data was allocated for this font and must be freed with
     * it. Zero for the faces compiled into the library, whose bytes are
     * static and shared by every size they are loaded at.
     */
    uint32_t       owns_data;
    /**
     * The family this face belongs to, copied from the face when it loads.
     * A Bold button sets bold on a span rather than naming a handle, and the
     * family is how the bold member of that span's face is found.
     *
     * Empty for a face that names no family. Such a face joins no family and
     * is only ever itself, which is the right answer: guessing that two
     * unnamed faces are related would substitute the wrong glyphs.
     */
    char           family[64];
    uint32_t       bold;    /**< Nonzero when this face is the bold member. */
    uint32_t       italic;  /**< Nonzero when this face is the slanted one. */
} schultz_font;

/**
 * @brief Resolves a font handle to its representation.
 *
 * @param system   The system holding the font. Must not be NULL.
 * @param handle   The font handle to resolve.
 * @param out_font Receives the font. Must not be NULL. The pointer is valid
 *                 until the font is unloaded.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when a pointer is NULL, or
 *         SCHULTZ_ERR_INVALID_HANDLE when the handle names no loaded font.
 */
int32_t schultz_font_resolve(const schultz_font_system *system,
                             schultz_handle handle, schultz_font **out_font);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_FONT_INTERNAL_H */
