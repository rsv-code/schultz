/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_render.h
 * @brief Rendering a widget, or a whole window, into memory.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 * The same tree that draws to a window draws to a buffer, at whatever
 * resolution is asked for, without being laid out again. That is one call,
 * and it is what a screenshot, a thumbnail, an image export and a printed
 * page all want.
 *
 * **Pagination is not here and is not planned.** Splitting a continuous tree
 * across page boundaries needs to know what may be broken, what must stay
 * together and what repeats as a heading, and those are document semantics a
 * widget toolkit has no opinion about. An application that knows its own
 * document lays out one page at a time and renders each with this.
 *
 * The print dialog and the spooler are not here either, being wholly platform
 * specific.
 */

#ifndef SCHULTZ_RENDER_H
#define SCHULTZ_RENDER_H

#include "schultz.h"
#include "schultz_font.h"
#include "schultz_geom.h"
#include "schultz_glyphs.h"
#include "schultz_image.h"
#include "schultz_node.h"
#include "schultz_resource.h"

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


/**
 * @brief What a render needs besides the tree itself.
 *
 * The tree knows its own fonts and images, but a render also needs the glyph
 * cache and the gradient table, which live beside the renderer rather than in
 * the tree. Zero initialize this and fill in what applies: a tree with no text
 * needs no fonts, and one with no gradients needs no resources.
 */
typedef struct {
    schultz_font_system    *fonts;      /**< Faces, for text. */
    schultz_glyph_cache    *glyphs;     /**< Rasterized glyphs, for text. */
    schultz_resource_table *resources;  /**< Gradients and dash patterns. */
    schultz_image_table    *images;     /**< Decoded images. */
    /**
     * What the buffer is cleared to first. A transparent colour leaves the
     * buffer transparent where nothing was drawn, which is what an icon
     * export wants; an opaque one is what a screenshot wants.
     */
    schultz_color           background;
} schultz_render_options;

/**
 * @brief How large a buffer a node needs at a given scale.
 *
 * Rounded outward to whole pixels, so the whole node fits.
 *
 * @param tree       The tree holding the node. Must not be NULL.
 * @param node       The node to measure. Its current bounds are used, so it
 *                   must have been laid out already.
 * @param scale      Pixels to render for each one the node measures. Two
 *                   gives a buffer twice the size. Must be above zero.
 * @param out_width  Receives the width in pixels. Must not be NULL.
 * @param out_height Receives the height in pixels. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_render_size(const schultz_tree *tree, schultz_handle node,
                            float scale, uint32_t *out_width,
                            uint32_t *out_height);

/**
 * @brief Renders a node and everything under it into a buffer.
 *
 * The node is drawn at the buffer's top left whatever its position in the
 * tree, so rendering one widget gives that widget and nothing around it.
 * Pixels are premultiplied ARGB8888, the same layout the window buffer and
 * schultz_image_set_pixels both use, so a render can be handed straight back
 * as an image.
 *
 * Text is rasterized at the scale asked for rather than scaled up from screen
 * size, so a page at print resolution is sharp.
 *
 * The caller need not have started the rendering engine; this starts it for
 * the duration of the call and the count it keeps means doing so alongside a
 * running window is safe.
 *
 * @param tree    The tree to render from. Must not be NULL.
 * @param node    The node to render. SCHULTZ_HANDLE_NONE means the root.
 * @param scale   Pixels to render for each one the node measures. Two gives
 *                a picture twice the size. Must be greater than zero.
 * @param options What to render with. Must not be NULL.
 * @param pixels  The buffer to render into. Must not be NULL.
 * @param width   Buffer width in pixels. Must be greater than zero.
 * @param height  Buffer height in pixels. Must be greater than zero.
 * @param stride  Pixels per row, which may exceed the width. Zero means the
 *                rows are packed.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad argument,
 *         SCHULTZ_ERR_INVALID_HANDLE when the node is not live, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_render_to_buffer(schultz_tree *tree, schultz_handle node,
                                 float scale,
                                 const schultz_render_options *options,
                                 uint32_t *pixels, uint32_t width,
                                 uint32_t height, uint32_t stride);

/**
 * @brief Renders a node straight to the bytes of a picture file.
 *
 * What schultz_render_size, a buffer of your own, schultz_render_to_buffer
 * and schultz_image_encode do together, in one call: the common case, which
 * is wanting a PNG of a widget and not wanting the pixels for anything else.
 *
 * Use the three calls separately when the pixels are the point: to render
 * into a buffer you already have, to hand the result back through
 * schultz_image_set_pixels, or to write more than one format from one render.
 *
 * **How large this gets is decided by the scale.** A node measuring a
 * thousand units on a side at a scale of one hundred asks for forty gigabytes
 * of pixels before a single byte is encoded, so the size is worked out and
 * refused first, before anything is allocated or drawn. Refusing afterwards
 * would have spent exactly what the refusal is for.
 *
 * @param tree       The tree to render from. Must not be NULL.
 * @param node       The node to render. SCHULTZ_HANDLE_NONE means the root.
 * @param scale      Pixels to render for each one the node measures. Two
 *                   gives a picture twice the size. Must be above zero.
 * @param options    What to render with. Must not be NULL.
 * @param format     SCHULTZ_IMAGE_PNG or SCHULTZ_IMAGE_BMP.
 * @param out_bytes  Receives the file's bytes. Must not be NULL. Owned by
 *                   the toolkit and valid until the next call to
 *                   schultz_image_encode or to this, so a caller wanting two
 *                   formats at once copies the first.
 * @param out_length Receives how many bytes. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad argument or a
 *         size too large to render, SCHULTZ_ERR_INVALID_HANDLE when the node
 *         is not live, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_render_encode(schultz_tree *tree, schultz_handle node,
                              float scale,
                              const schultz_render_options *options,
                              uint32_t format, const void **out_bytes,
                              uint64_t *out_length);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_RENDER_H */
