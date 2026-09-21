/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_colr.h
 * @brief Drawing a colour glyph that the font describes as a picture.
 *
 * Internal header.
 *
 * An emoji is not a letter with a colour applied to it. Newer emoji fonts
 * describe each one as a small drawing: shapes filled with flat colours or
 * gradients, moved and scaled, clipped to outlines, and layered over one
 * another. OpenType calls that description a COLR version 1 table.
 *
 * FreeType reads the description and hands it over, but does not draw it,
 * because drawing it needs gradients and clipping and blending -- a vector
 * engine. The toolkit already carries one for everything else it draws, so
 * this walks the description and builds the same kind of scene the painter
 * builds, then rasterizes it into the pixels the glyph cache keeps.
 *
 * Nothing here is reached for a plain letter, or for the older kind of
 * colour font that stores flat layers or pictures: FreeType finishes both of
 * those itself.
 *
 * **Not part of the host facing interface.** A host binds to schultz_api.h.
 */

#ifndef SCHULTZ_COLR_H
#define SCHULTZ_COLR_H

#include <stdint.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "schultz.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether a glyph is described as a drawing rather than an outline.
 *
 * True only for the newer colour format, the one FreeType will not draw on
 * its own. A face whose colour glyphs are flat layers or pictures answers no
 * here, and FreeType rasterizes those the ordinary way.
 *
 * @param face  The face to ask. NULL yields zero.
 * @param glyph The glyph index.
 * @return Nonzero when schultz_colr_render can draw it.
 */
int32_t schultz_colr_has_drawing(FT_Face face, FT_UInt glyph);

/**
 * @brief One glyph's outline as a shape the rasterizer can draw.
 *
 * The shape comes back in the font's own units, with y pointing up the way a
 * font measures it. The caller puts a matrix on it to get pixels, which is
 * also where a rotation goes: turning the outline before it is rasterized is
 * what keeps text at an angle sharp, rather than turning a picture of it.
 *
 * Returns `void *` rather than the rasterizer's own paint type so this header
 * does not have to include the rasterizer. It is a `Tvg_Paint`.
 *
 * Not every glyph has one. A bitmap emoji is a picture with no outline at
 * all, and answers NULL.
 *
 * @param face  The face to load from. Must not be NULL.
 * @param glyph The glyph index.
 * @return The shape, which the caller owns and must hand to the canvas or
 *         release, or NULL when the glyph has no outline.
 */
void *schultz_colr_glyph_outline(FT_Face face, FT_UInt glyph);

/**
 * @brief Draws one such glyph at the face's current size.
 *
 * The pixels come back blue, green, red, alpha, with the colour channels
 * already multiplied by the alpha, which is the arrangement FreeType uses
 * for the other colour formats and therefore the one the glyph cache and the
 * painter already expect.
 *
 * @param face       The face, at the size the glyph is wanted. Must not be
 *                   NULL.
 * @param glyph      The glyph index.
 * @param out_pixels Receives the pixels, allocated here. The caller owns
 *                   them and frees them with free(). Must not be NULL.
 * @param out_width  Receives the width in pixels. Must not be NULL.
 * @param out_height Receives the height in pixels. Must not be NULL.
 * @param out_left   Receives the pen to left edge distance. Must not be NULL.
 * @param out_top    Receives the baseline to top edge distance, upward
 *                   positive. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when the glyph is not one
 *         of these or an argument is NULL, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_colr_render(FT_Face face, FT_UInt glyph,
                            unsigned char **out_pixels, uint32_t *out_width,
                            uint32_t *out_height, int32_t *out_left,
                            int32_t *out_top);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_COLR_H */
