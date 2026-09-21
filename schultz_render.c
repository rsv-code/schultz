/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * schultz_render.c - rendering a widget, or a window, into memory.
 *
 * Nothing new happens here. The tree, the paint walk and the ThorVG backend
 * all already do their jobs against a buffer; this is the one call that puts
 * them together and hands the result back.
 */

#include "schultz_render.h"

#include <stdlib.h>
#include <string.h>

#include "schultz_arena.h"
#include "schultz_paint.h"
#include "schultz_thorvg.h"
#include "schultz_widget.h"

int32_t schultz_render_size(const schultz_tree *tree, schultz_handle node,
                            float scale, uint32_t *out_width,
                            uint32_t *out_height)
{
    schultz_rect bounds;

    if (tree == NULL || out_width == NULL || out_height == NULL ||
        scale <= 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (node == SCHULTZ_HANDLE_NONE) {
        node = schultz_tree_root(tree);
    }
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* Rounded outward, so a node ending on a fraction is not cut off. */
    *out_width  = (uint32_t)(bounds.width * scale + 0.999f);
    *out_height = (uint32_t)(bounds.height * scale + 0.999f);
    return SCHULTZ_OK;
}

int32_t schultz_render_to_buffer(schultz_tree *tree, schultz_handle node,
                                 float scale,
                                 const schultz_render_options *options,
                                 uint32_t *pixels, uint32_t width,
                                 uint32_t height, uint32_t stride)
{
    schultz_thorvg *backend = NULL;
    schultz_painter painter;
    schultz_arena arena;
    schultz_draw_list list;
    schultz_rect bounds;
    int32_t result;

    if (tree == NULL || options == NULL || pixels == NULL || scale <= 0.0f ||
        width == 0u || height == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (stride == 0u) {
        stride = width;
    }
    if (stride < width) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (node == SCHULTZ_HANDLE_NONE) {
        node = schultz_tree_root(tree);
    }
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }

    /*
     * The engine counts its callers, so starting it here is safe whether or
     * not a window is already running on it.
     */
    result = schultz_thorvg_engine_init(0);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_thorvg_create(pixels, width, height, stride, &backend);
    if (result != SCHULTZ_OK) {
        schultz_thorvg_engine_term();
        return result;
    }
    schultz_thorvg_set_scale(backend, scale);
    schultz_thorvg_set_fonts(backend, options->fonts, options->glyphs);
    schultz_thorvg_set_resources(backend, options->resources);
    schultz_thorvg_set_images(backend, options->images);
    schultz_thorvg_painter(backend, &painter);

    result = schultz_arena_init(&arena, 0);
    if (result == SCHULTZ_OK) {
        result = schultz_draw_list_init(&list, &arena, 0);
    }
    if (result != SCHULTZ_OK) {
        schultz_thorvg_destroy(backend);
        schultz_thorvg_engine_term();
        return result;
    }

    /*
     * The background is drawn rather than written into the buffer first,
     * because the rasterizer clears what it is given before it draws. In
     * logical units, since everything handed over is multiplied by the scale.
     */
    if (options->background.a > 0u) {
        schultz_draw_fill_rect(&list,
            schultz_rect_make(0.0f, 0.0f, (float)width / scale,
                              (float)height / scale),
            schultz_paint_solid(options->background));
    }

    /*
     * The node lands at the buffer's top left whatever its position in the
     * tree, so rendering one widget gives that widget and nothing around it.
     */
    schultz_draw_offset_begin(&list, -bounds.x, -bounds.y);
    result = schultz_widget_paint_subtree(tree, node, &list, &arena,
                                          schultz_rect_make(0, 0, 0, 0));
    schultz_draw_offset_end(&list);

    if (result == SCHULTZ_OK) {
        result = schultz_draw_list_overflowed(&list)
            ? SCHULTZ_ERR_OUT_OF_MEMORY
            : schultz_draw_list_play(&list, &painter,
                                     schultz_rect_make(0, 0, 0, 0));
    }

    schultz_arena_free(&arena);
    schultz_thorvg_destroy(backend);
    schultz_thorvg_engine_term();
    return result;
}

/*
 * How many bytes a buffer that size needs, or zero when it cannot have one.
 *
 * The same rule schultz_image.c applies before it writes a picture, repeated
 * here on purpose rather than shared. The point of asking is when it is
 * asked: encode refuses a size it cannot describe, and by then the pixels
 * have been allocated and drawn, which is the whole cost the refusal exists
 * to avoid. A caller chooses the scale, so this is the one number in a render
 * that somebody outside picks, and a wrong one should cost nothing.
 *
 * 65536 a side matches the limit in the picture writers, so a render that
 * gets past this is never refused later for its size.
 */
static size_t schultz_render_bytes(uint32_t width, uint32_t height)
{
    size_t total;

    if (width == 0u || height == 0u ||
        width > 65536u || height > 65536u) {
        return 0u;
    }
    /* Checked rather than trusted: on a 32-bit build the product of two
     * values this size wraps, and a wrapped size allocates a small buffer
     * that the render then fills as though it were large. */
    if ((size_t)width > SIZE_MAX / (size_t)height) {
        return 0u;
    }
    total = (size_t)width * (size_t)height;
    if (total > SIZE_MAX / sizeof(uint32_t)) {
        return 0u;
    }
    return total * sizeof(uint32_t);
}

int32_t schultz_render_encode(schultz_tree *tree, schultz_handle node,
                              float scale,
                              const schultz_render_options *options,
                              uint32_t format, const void **out_bytes,
                              uint64_t *out_length)
{
    uint32_t width = 0u;
    uint32_t height = 0u;
    size_t bytes;
    uint32_t *pixels;
    int32_t result;

    if (out_bytes == NULL || out_length == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_bytes  = NULL;
    *out_length = 0u;
    if (options == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_render_size(tree, node, scale, &width, &height);
    if (result != SCHULTZ_OK) {
        return result;
    }
    bytes = schultz_render_bytes(width, height);
    if (bytes == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    pixels = (uint32_t *)malloc(bytes);
    if (pixels == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_render_to_buffer(tree, node, scale, options, pixels,
                                      width, height, 0u);
    if (result == SCHULTZ_OK) {
        result = schultz_image_encode(pixels, width, height, 0u, format,
                                      out_bytes, out_length);
    }
    free(pixels);
    return result;
}
