/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_thorvg.h
 * @brief ThorVG implementation of the painter interface.
 *
 * Internal header. This is the backend that turns a draw command list into
 * pixels, using ThorVG's software rasterizer. It renders into a CPU buffer the
 * caller owns; getting that buffer onto a screen is schultz_sdl.h's job.
 *
 * One backend serves every target. There is no GPU path to keep in sync.
 *
 * **Not part of the host facing interface.** The rasterizer backend. A host
 * binds to schultz_api.h; this header is the toolkit's own and may change
 * without notice.
 */

#ifndef SCHULTZ_THORVG_H
#define SCHULTZ_THORVG_H

#include "schultz_glyphs.h"
#include "schultz_paint.h"
#include "schultz_image.h"
#include "schultz_resource.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque backend state. Create with schultz_thorvg_create. */
typedef struct schultz_thorvg schultz_thorvg;

/**
 * @brief Starts the ThorVG engine.
 *
 * Call once before creating any backend, and pair with
 * schultz_thorvg_engine_term.
 *
 * @param threads Worker threads for the rasterizer. Pass 0 to rasterize on
 *                the calling thread, which is what bring up and deterministic
 *                tests want.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when the engine refused
 *         to start.
 */
int32_t schultz_thorvg_engine_init(uint32_t threads);

/**
 * @brief Stops the ThorVG engine.
 *
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when the engine refused
 *         to stop.
 */
int32_t schultz_thorvg_engine_term(void);

/**
 * @brief Creates a backend that rasterizes into a caller owned buffer.
 *
 * The buffer is not copied and must stay valid and unmoved for the life of
 * the backend. Pixels are written as premultiplied ARGB8888, which is what
 * SDL's SDL_PIXELFORMAT_ARGB8888 expects on a little endian machine.
 *
 * @param buffer  Destination pixels, at least stride * height words. Must not
 *                be NULL.
 * @param width   Buffer width in pixels. Must be greater than zero.
 * @param height  Buffer height in pixels. Must be greater than zero.
 * @param stride  Distance between rows, in pixels, not bytes. Must be at
 *                least width.
 * @param out_backend Receives the new backend. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad argument, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when the canvas could not be created.
 */
int32_t schultz_thorvg_create(uint32_t *buffer, uint32_t width,
                              uint32_t height, uint32_t stride,
                              schultz_thorvg **out_backend);

/**
 * @brief Destroys a backend and its canvas.
 *
 * The pixel buffer belongs to the caller and is not freed.
 *
 * @param backend The backend to destroy. NULL is accepted and does nothing.
 */
void schultz_thorvg_destroy(schultz_thorvg *backend);

/**
 * @brief Points the canvas at a different buffer.
 *
 * A window that has been resized has a new buffer of a new size, and the
 * canvas has to be told or it keeps rasterizing into the old one.
 *
 * @param backend The backend to retarget. Must not be NULL.
 * @param buffer  The new destination pixels, owned by the caller.
 * @param width   Its width in pixels.
 * @param height  Its height in pixels.
 * @param stride  Pixels per row, not bytes.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_thorvg_set_target(schultz_thorvg *backend, uint32_t *buffer,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride);

/**
 * @brief Gives the backend what it needs to draw text.
 *
 * Until this is called, a glyph run command fails rather than drawing
 * nothing, so missing text is reported instead of silently absent.
 *
 * @param backend The backend to configure. Must not be NULL.
 * @param system  The font system glyph run commands name fonts in. Must not
 *                be NULL and must outlive the backend.
 * @param cache   The glyph cache to rasterize through. Must not be NULL and
 *                must outlive the backend.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when any pointer is
 *         NULL.
 */
int32_t schultz_thorvg_set_fonts(schultz_thorvg *backend,
                                 schultz_font_system *system,
                                 schultz_glyph_cache *cache);

/**
 * @brief Fills in a painter that draws through this backend.
 *
 * @param backend     The backend to bind. Must not be NULL.
 * @param out_painter Receives the vtable and context. Must not be NULL. It
 *                    borrows the backend, so the backend must outlive it.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when either pointer is
 *         NULL.
 */
int32_t schultz_thorvg_painter(schultz_thorvg *backend,
                               schultz_painter *out_painter);

/**
 * @brief Tells the backend where registered gradients and dashes live.
 *
 * A draw command names a gradient or a dash pattern by handle, and this is
 * where the backend looks it up. Without a table, a gradient draws nothing
 * and a dash pattern is ignored, which is what a caller using only solid
 * colours wants.
 *
 * @param backend The backend to configure. Must not be NULL.
 * @param table   The table, or NULL to remove it. Not owned, and must outlive
 *                the backend.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when backend is NULL.
 */
int32_t schultz_thorvg_set_resources(schultz_thorvg *backend,
                                     schultz_resource_table *table);

/**
 * @brief Sets how many buffer pixels there are to a toolkit unit.
 *
 * One by default, which is a screen at its own resolution. Larger renders the
 * same tree at higher resolution without laying it out again: a page at print
 * resolution, or a thumbnail scaled the other way. Glyphs are rasterized at
 * the scaled size rather than blown up from screen size, so text stays as
 * sharp as everything around it.
 *
 * @param backend The backend to configure. Must not be NULL.
 * @param scale   Buffer pixels per toolkit unit. Must be greater than zero.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_thorvg_set_scale(schultz_thorvg *backend, float scale);

/**
 * @brief Tells the backend where loaded images live.
 *
 * A draw command names an image by handle, and this is where the backend looks
 * it up. Without a table an image draws nothing, which is what a caller that
 * never loads one wants.
 *
 * @param backend The backend to configure. Must not be NULL.
 * @param table   The table, or NULL to remove it. Not owned, and must outlive
 *                the backend.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when backend is NULL.
 */
int32_t schultz_thorvg_set_images(schultz_thorvg *backend,
                                  schultz_image_table *table);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_THORVG_H */
