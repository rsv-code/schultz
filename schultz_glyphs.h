/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_glyphs.h
 * @brief Rasterized glyph cache.
 *
 * Internal header.
 *
 * Rasterizing a glyph is expensive and the same glyphs recur constantly, so
 * every rasterized glyph is kept, keyed by its font and glyph index. A cache
 * entry holds the rasterized pixels plus the offsets needed to place them
 * against the pen position.
 *
 * Entries are individually allocated rather than packed into one large atlas
 * texture. Packing exists to let a GPU draw many glyphs with one texture
 * binding, and Schultz rasterizes on the CPU, so it would buy locality but not
 * batching, so it is not worth what it costs until something draws on a GPU.
 *
 * **Not part of the host facing interface.** Rasterized glyphs, kept for the
 * renderer. A host binds to schultz_api.h; this header is the toolkit's own
 * and may change without notice.
 */

#ifndef SCHULTZ_GLYPHS_H
#define SCHULTZ_GLYPHS_H

#include "schultz_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Everything declared below is part of the host facing interface, so it is
 * marked visible. The toolkit is compiled with -fvisibility=hidden, which
 * hides everything by default: that is what stops a shared library built
 * from it exporting the whole of FreeType, libpng and zlib alongside, where
 * they would meet the copies already loaded by whatever is hosting it.
 *
 * A pragma rather than an attribute on each declaration, because there are
 * some hundreds of them and one pair of lines per header says the same thing.
 */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif


/** @brief Opaque glyph cache. Create with schultz_glyph_cache_create. */
typedef struct schultz_glyph_cache schultz_glyph_cache;

/**
 * @brief What the bytes of a rasterized glyph mean.
 *
 * A letter is a shape with no colour of its own, so it rasterizes to
 * coverage: how much of each pixel the shape covered. The colour arrives
 * later, from the style, and the painter multiplies the two. That is what
 * lets one face draw text in any colour.
 *
 * An emoji is a picture and brings its own colours, so there is nothing for
 * the style to say. It rasterizes to finished pixels and the painter copies
 * them, using the style's colour only for how solid the whole thing is.
 */
enum {
    /** One byte per pixel: how much of the pixel the shape covered. */
    SCHULTZ_GLYPH_COVERAGE = 0,
    /** Four bytes per pixel, blue green red alpha, already multiplied. */
    SCHULTZ_GLYPH_COLOR
};

/**
 * @brief One rasterized glyph.
 *
 * The bitmap is `height` rows of `pitch` bytes, and `format` says what a
 * pixel in them is. A glyph with no outline, such as a space, has a NULL
 * bitmap and zero extents but still has valid bearings.
 */
typedef struct {
    const unsigned char *bitmap;    /**< Pixel rows, or NULL when blank. */
    uint32_t             width;     /**< Bitmap width in pixels. */
    uint32_t             height;    /**< Bitmap height in pixels. */
    uint32_t             pitch;     /**< Bytes per row, at least width. */
    int32_t              bearing_x; /**< Pen to left edge, rightward positive. */
    int32_t              bearing_y; /**< Baseline to top edge, upward positive. */
    uint32_t             format;    /**< SCHULTZ_GLYPH_COVERAGE or _COLOR. */
} schultz_glyph_bitmap;

/**
 * @brief Creates a glyph cache over a font system.
 *
 * @param system   The font system whose fonts will be rasterized. Must not be
 *                 NULL and must outlive the cache.
 * @param out_cache Receives the new cache. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when either pointer is
 *         NULL, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_glyph_cache_create(schultz_font_system *system,
                                   schultz_glyph_cache **out_cache);

/**
 * @brief Destroys a cache and every bitmap it holds.
 *
 * @param cache The cache to destroy. NULL is accepted and does nothing.
 */
void schultz_glyph_cache_destroy(schultz_glyph_cache *cache);

/**
 * @brief Returns a rasterized glyph, rasterizing it on first request.
 *
 * @param cache    The cache to look in. Must not be NULL.
 * @param font     The font the glyph index belongs to.
 * @param glyph_id The font specific glyph index, as produced by shaping. This
 *                 is not a character code.
 * @param out_glyph Receives a pointer to the cached bitmap. Must not be NULL.
 *                  The pointer stays valid until the cache is destroyed.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer,
 *         SCHULTZ_ERR_INVALID_HANDLE when the font is not loaded, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_glyph_cache_get(schultz_glyph_cache *cache,
                                schultz_handle font, uint32_t glyph_id,
                                const schultz_glyph_bitmap **out_glyph);

/**
 * @brief One glyph's outline, for drawing it at an angle.
 *
 * The cache holds every letter rasterized upright, which is right until
 * something turns it: turning a picture of a letter resamples pixels that
 * have already been drawn. This hands back the outline instead, so the
 * rasterizer can turn it before any pixel exists.
 *
 * Nothing is cached here. An outline is wanted per frame by the only thing
 * that asks for one, and caching it per angle is the other design, not this
 * one.
 *
 * The shape is in the font's own units with y pointing up, and `out_scale`
 * is what one of those units is worth in pixels at this font's size. Returns
 * `void *` so this header need not include the rasterizer; it is a
 * `Tvg_Paint`.
 *
 * @param cache     The cache, for the font system behind it. Must not be NULL.
 * @param font      A loaded font.
 * @param glyph_id  The glyph index, not a character code.
 * @param out_scale Receives pixels per font unit. Must not be NULL.
 * @return The shape, which the caller owns, or NULL when the glyph has no
 *         outline, such as a bitmap emoji.
 */
void *schultz_glyph_outline(schultz_glyph_cache *cache, schultz_handle font,
                            uint32_t glyph_id, float *out_scale);

/**
 * @brief Drops every entry belonging to one font.
 *
 * Call this when a font is unloaded, otherwise its entries stay cached under
 * a handle that can never be resolved again.
 *
 * @param cache The cache to purge. NULL is accepted and does nothing.
 * @param font  The font whose entries should be dropped.
 */
void schultz_glyph_cache_purge_font(schultz_glyph_cache *cache,
                                    schultz_handle font);

/**
 * @brief Counts cached glyphs.
 *
 * @param cache The cache to query. NULL yields 0.
 * @return The number of rasterized glyphs currently held.
 */
uint32_t schultz_glyph_cache_count(const schultz_glyph_cache *cache);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_GLYPHS_H */
