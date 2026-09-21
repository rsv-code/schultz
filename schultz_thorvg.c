/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_thorvg.c
 * @brief ThorVG implementation of the painter interface.
 *
 * Each draw command becomes a ThorVG shape added to the canvas. The canvas is
 * cleared at begin and drawn at end, so one play of a command list is one
 * frame.
 *
 * Two pieces of painter state are tracked here rather than in ThorVG, because
 * the command list expresses them as stacks:
 *
 *   - the translation stack, applied to coordinates as shapes are built
 *   - the clip stack, kept as an intersected rectangle
 *
 * ThorVG clips a paint against another paint, so the current clip rectangle is
 * turned into a clipper shape and attached to each shape it applies to.
 */

#include "schultz_thorvg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <thorvg_capi.h>

#include "schultz_glyphs.h"
#include "schultz_image_internal.h"
#include "schultz_resource.h"

enum {
    /* Deep enough for any sane widget tree; overflow is reported, not
     * silently ignored, so a runaway paint pass is visible. */
    SCHULTZ_TVG_STACK_MAX = 64
};

/** @brief Canvas, target buffer, and the two painter stacks. */
struct schultz_thorvg {
    Tvg_Canvas canvas; /**< ThorVG software canvas targeting buffer. */
    uint32_t  *buffer; /**< Destination pixels, owned by the caller. */
    uint32_t   width;  /**< Buffer width in pixels. */
    uint32_t   height; /**< Buffer height in pixels. */
    uint32_t   stride; /**< Pixels per row. */

    /** Saved translations, innermost last. */
    schultz_point offsets[SCHULTZ_TVG_STACK_MAX];
    /** Scales saved by device_pixels_begin, with their offsets. */
    float         scales[SCHULTZ_TVG_STACK_MAX];
    /** Offsets saved beside them, since both change together. */
    schultz_point scale_offsets[SCHULTZ_TVG_STACK_MAX];
    /** Entries used in scales. */
    uint32_t      scale_depth;
    /** Entries used in offsets. */
    uint32_t      offset_depth;
    /** Running sum of the offsets now in force. */
    schultz_point offset;

    /** Saved rotations, innermost last. */
    Tvg_Matrix   rotations[SCHULTZ_TVG_STACK_MAX];
    /** Entries used in rotations. */
    uint32_t     rotation_depth;
    /**
     * The turn in force, in device pixels, as a product of every rotation
     * begun and not yet ended. Identity until one starts, which is why
     * `rotated` is kept beside it rather than comparing against identity.
     */
    Tvg_Matrix   rotation;
    /** Nonzero while `rotation` is anything other than identity. */
    int32_t      rotated;

    /** Saved clip rectangles, innermost last. */
    schultz_rect clips[SCHULTZ_TVG_STACK_MAX];
    /** Entries used in clips. Zero means no clipper is attached. */
    uint32_t     clip_depth;
    /** Running intersection of the clips now in force. */
    schultz_rect clip;

    /**
     * Groups being built, innermost last. A group is a ThorVG scene: shapes
     * go into it rather than onto the canvas, and when it closes it is
     * blended in as one picture at the opacity it was opened with.
     */
    Tvg_Paint    scenes[SCHULTZ_TVG_STACK_MAX];
    /** The opacity each of those was opened with, 0 to 255. */
    uint8_t      scene_opacity[SCHULTZ_TVG_STACK_MAX];
    /** The shadow each was opened with. A clear colour means none. */
    schultz_shadow scene_shadow[SCHULTZ_TVG_STACK_MAX];
    /** Entries used in scenes. Zero means shapes go straight on the canvas. */
    uint32_t     scene_depth;

    /** Set when the frame repaints everything, so the buffer may be cleared. */
    int32_t              full_repaint;
    /** Set when a stack overflowed, so end() can fail the frame. */
    int32_t overflowed;

    /** Font system for glyph run commands, or NULL. Not owned. */
    schultz_font_system *fonts;
    /** Glyph cache for glyph run commands, or NULL. Not owned. */
    schultz_glyph_cache *glyphs;
    /** Gradients and dash patterns a style may refer to, or NULL. */
    schultz_resource_table *resources;
    /** Decoded images a draw command may name, or NULL. */
    schultz_image_table    *images;
    /**
     * Device pixels per logical unit. One for a screen at its own resolution;
     * larger for a page rendered at print resolution or a thumbnail rendered
     * small. Everything handed to ThorVG is multiplied by it, and glyphs are
     * rasterized at the scaled size rather than blown up from screen size.
     */
    float                   scale;

    /** Scratch ARGB buffer a glyph run is composited into before upload. */
    uint32_t *run_pixels;
    /** Words allocated in run_pixels. */
    size_t    run_capacity;
};

static int32_t schultz_tvg_ok(Tvg_Result result)
{
    switch (result) {
    case TVG_RESULT_SUCCESS:            return SCHULTZ_OK;
    case TVG_RESULT_INVALID_ARGUMENT:   return SCHULTZ_ERR_INVALID_ARGUMENT;
    case TVG_RESULT_INSUFFICIENT_CONDITION:
                                        return SCHULTZ_ERR_INVALID_ARGUMENT;
    case TVG_RESULT_FAILED_ALLOCATION:  return SCHULTZ_ERR_OUT_OF_MEMORY;
    default:                            return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
}

int32_t schultz_thorvg_engine_init(uint32_t threads)
{
    return schultz_tvg_ok(tvg_engine_init((unsigned)threads));
}

int32_t schultz_thorvg_engine_term(void)
{
    return schultz_tvg_ok(tvg_engine_term());
}

int32_t schultz_thorvg_create(uint32_t *buffer, uint32_t width,
                              uint32_t height, uint32_t stride,
                              schultz_thorvg **out_backend)
{
    schultz_thorvg *backend;
    Tvg_Result result;

    if (buffer == NULL || out_backend == NULL || width == 0 || height == 0 ||
        stride < width) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    backend = (schultz_thorvg *)calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    /*
     * TVG_ENGINE_OPTION_NONE, not _DEFAULT. The default turns on ThorVG's own
     * partial rendering, which tracks which shapes changed between frames and
     * clears those regions before redrawing them. Schultz already computes a
     * dirty region from its tree and rebuilds the shape list every frame, so
     * ThorVG would see the whole previous scene as changed and clear it, then
     * redraw only the commands inside the dirty region. Everything else on
     * screen would be erased. _NONE leaves antialiasing on and the partial
     * tracking off, which is what a caller that owns its own dirty region
     * wants.
     */
    backend->canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_NONE);
    if (backend->canvas == NULL) {
        free(backend);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    /*
     * ARGB8888 premultiplied matches SDL_PIXELFORMAT_ARGB8888 on a little
     * endian machine, so the buffer uploads to a texture with no conversion.
     */
    result = tvg_swcanvas_set_target(backend->canvas, buffer, stride, width,
                                     height, TVG_COLORSPACE_ARGB8888);
    if (result != TVG_RESULT_SUCCESS) {
        tvg_canvas_destroy(backend->canvas);
        free(backend);
        return schultz_tvg_ok(result);
    }

    backend->buffer = buffer;
    backend->scale  = 1.0f;
    backend->width  = width;
    backend->height = height;
    backend->stride = stride;
    *out_backend    = backend;
    return SCHULTZ_OK;
}

void schultz_thorvg_destroy(schultz_thorvg *backend)
{
    if (backend == NULL) {
        return;
    }
    if (backend->canvas != NULL) {
        tvg_canvas_destroy(backend->canvas);
    }
    free(backend->run_pixels);
    free(backend);
}

int32_t schultz_thorvg_set_target(schultz_thorvg *backend, uint32_t *buffer,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride)
{
    Tvg_Result result;

    if (backend == NULL || buffer == NULL || width == 0u || height == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = tvg_swcanvas_set_target(backend->canvas, buffer, stride, width,
                                     height, TVG_COLORSPACE_ARGB8888);
    if (result != TVG_RESULT_SUCCESS) {
        return schultz_tvg_ok(result);
    }
    backend->buffer = buffer;
    backend->width  = width;
    backend->height = height;
    backend->stride = stride;
    return SCHULTZ_OK;
}

int32_t schultz_thorvg_set_fonts(schultz_thorvg *backend,
                                 schultz_font_system *system,
                                 schultz_glyph_cache *cache)
{
    if (backend == NULL || system == NULL || cache == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend->fonts  = system;
    backend->glyphs = cache;
    return SCHULTZ_OK;
}

/* ---------------------------------------------------------------- helpers */

/* Applies the current translation to a rectangle. */
int32_t schultz_thorvg_set_resources(schultz_thorvg *backend,
                                     schultz_resource_table *table)
{
    if (backend == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend->resources = table;
    return SCHULTZ_OK;
}

int32_t schultz_thorvg_set_scale(schultz_thorvg *backend, float scale)
{
    if (backend == NULL || scale <= 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend->scale = scale;
    return SCHULTZ_OK;
}

int32_t schultz_thorvg_set_images(schultz_thorvg *backend,
                                  schultz_image_table *table)
{
    if (backend == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend->images = table;
    return SCHULTZ_OK;
}

/* One logical length in device pixels. */
static float schultz_tvg_scale(const schultz_thorvg *backend, float length)
{
    return length * backend->scale;
}

/* One logical point in device pixels, with the current offset applied. */
static schultz_point schultz_tvg_point(const schultz_thorvg *backend, float x,
                                       float y)
{
    return schultz_point_make((x + backend->offset.x) * backend->scale,
                              (y + backend->offset.y) * backend->scale);
}

static schultz_rect schultz_tvg_place(const schultz_thorvg *backend,
                                      schultz_rect rect)
{
    schultz_point at = schultz_tvg_point(backend, rect.x, rect.y);

    return schultz_rect_make(at.x, at.y, rect.width * backend->scale,
                             rect.height * backend->scale);
}

/* The identity, which is what "no rotation" means. */
static Tvg_Matrix schultz_tvg_identity(void)
{
    Tvg_Matrix m;

    m.e11 = 1.0f; m.e12 = 0.0f; m.e13 = 0.0f;
    m.e21 = 0.0f; m.e22 = 1.0f; m.e23 = 0.0f;
    m.e31 = 0.0f; m.e32 = 0.0f; m.e33 = 1.0f;
    return m;
}

/* a becomes a times b, so b happens first and a happens to the result. */
static void schultz_tvg_times(Tvg_Matrix *a, const Tvg_Matrix *b)
{
    Tvg_Matrix out;

    out.e11 = a->e11 * b->e11 + a->e12 * b->e21;
    out.e12 = a->e11 * b->e12 + a->e12 * b->e22;
    out.e13 = a->e11 * b->e13 + a->e12 * b->e23 + a->e13;
    out.e21 = a->e21 * b->e11 + a->e22 * b->e21;
    out.e22 = a->e21 * b->e12 + a->e22 * b->e22;
    out.e23 = a->e21 * b->e13 + a->e22 * b->e23 + a->e23;
    out.e31 = 0.0f;
    out.e32 = 0.0f;
    out.e33 = 1.0f;
    *a = out;
}

/*
 * Attaches the current clip to a shape, when one is in effect. ThorVG clips a
 * paint against another paint, so the clip rectangle becomes a throwaway
 * shape owned by the clipped shape.
 */
static int32_t schultz_tvg_apply_clip(schultz_thorvg *backend, Tvg_Paint shape)
{
    Tvg_Paint clipper;

    if (backend->clip_depth == 0) {
        return SCHULTZ_OK;
    }

    clipper = tvg_shape_new();
    if (clipper == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    tvg_shape_append_rect(clipper, backend->clip.x, backend->clip.y,
                          backend->clip.width, backend->clip.height,
                          0.0f, 0.0f, true);
    return schultz_tvg_ok(tvg_paint_set_clip(shape, clipper));
}

/*
 * Creates a shape, turns it if a rotation is in force, applies the clip, and
 * adds it to the canvas.
 *
 * Every shape goes through here, which is why the rotation is applied here
 * and nowhere else. The composition matters: a picture has already placed
 * itself with a transform of its own, so the turn multiplies onto that rather
 * than replacing it.
 */
static int32_t schultz_tvg_finish(schultz_thorvg *backend, Tvg_Paint shape)
{
    int32_t result;

    if (backend->rotated) {
        Tvg_Matrix already;
        Tvg_Matrix turned = backend->rotation;

        if (tvg_paint_get_transform(shape, &already) == TVG_RESULT_SUCCESS) {
            schultz_tvg_times(&turned, &already);
        }
        tvg_paint_set_transform(shape, &turned);
    }
    result = schultz_tvg_apply_clip(backend, shape);
    if (result != SCHULTZ_OK) {
        tvg_paint_unref(shape, true);
        return result;
    }
    /*
     * Inside a group the shape belongs to the group's scene, not the canvas.
     * The scene reaches the canvas when the group closes, which is also what
     * keeps the drawing order right: nothing else is added to the canvas
     * between the group opening and closing.
     */
    if (backend->scene_depth > 0u) {
        return schultz_tvg_ok(tvg_scene_add(
            backend->scenes[backend->scene_depth - 1u], shape));
    }
    return schultz_tvg_ok(tvg_canvas_add(backend->canvas, shape));
}

/* ------------------------------------------------------------ vtable entries */

static int32_t schultz_tvg_begin(void *context, schultz_rect bounds)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;

    backend->offset_depth   = 0;
    backend->offset         = schultz_point_make(0.0f, 0.0f);
    backend->rotation_depth = 0;
    backend->rotation       = schultz_tvg_identity();
    backend->rotated        = 0;
    backend->scale_depth  = 0;
    backend->clip_depth   = 0;
    backend->scene_depth  = 0;
    backend->clip         = schultz_rect_make(0.0f, 0.0f,
                                              (float)backend->width,
                                              (float)backend->height);
    backend->overflowed   = 0;
    backend->full_repaint = schultz_rect_is_empty(bounds);

    /*
     * Restrict rasterization to the region being repainted. This is where the
     * dirty rectangle earns its keep: everything outside it is skipped.
     *
     * The viewport must be set before the shapes are removed. ThorVG accepts
     * a viewport change only on a synced canvas, and removing a shape leaves
     * the canvas in its painting state, where the request is ignored without
     * an error.
     */
    if (backend->full_repaint) {
        /*
         * A previous frame may have left a small viewport behind, and it
         * persists until it is replaced. Widen it back to the whole target.
         */
        tvg_canvas_set_viewport(backend->canvas, 0, 0,
                                (int32_t)backend->width,
                                (int32_t)backend->height);
    } else {
        /*
         * Round outward. Truncating here drops a fraction of a pixel on the
         * far edge, and a region that moves by a fractional amount each frame
         * accumulates that loss until a whole column of the previous frame
         * survives. One pixel of slack covers antialiased coverage, which
         * extends past the geometric bounds.
         */
        /*
         * The region arrives in the toolkit's units and a viewport is in
         * pixels of the buffer, so it is scaled like every other length. Left
         * unscaled it names a rectangle a third the size on a screen that
         * packs three pixels to the unit, and everything outside that keeps
         * whatever the buffer happened to hold: a black band around a picture
         * that is otherwise drawn correctly.
         */
        schultz_rect scaled = schultz_rect_make(
            bounds.x * backend->scale, bounds.y * backend->scale,
            bounds.width * backend->scale, bounds.height * backend->scale);
        schultz_rect box = schultz_rect_pixel_bounds(scaled,
                                                     schultz_paint_slack());
        tvg_canvas_set_viewport(backend->canvas, (int32_t)box.x,
                                (int32_t)box.y, (int32_t)box.width,
                                (int32_t)box.height);
    }

    /* Drop last frame's shapes. The canvas owns them. */
    tvg_canvas_remove(backend->canvas, NULL);

    return SCHULTZ_OK;
}

static int32_t schultz_tvg_end(void *context)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    Tvg_Result result;

    if (backend->overflowed) {
        return SCHULTZ_ERR_EXHAUSTED;
    }

    result = tvg_canvas_update(backend->canvas);
    if (result != TVG_RESULT_SUCCESS) {
        return schultz_tvg_ok(result);
    }
    /*
     * Clearing wipes the whole target, not just the viewport, so it is only
     * safe on a full repaint. A partial frame keeps the pixels outside its
     * region and redraws the region itself from the bottom up, which is what
     * makes the dirty rectangle worth computing.
     */
    result = tvg_canvas_draw(backend->canvas, backend->full_repaint != 0);
    if (result != TVG_RESULT_SUCCESS) {
        return schultz_tvg_ok(result);
    }
    return schultz_tvg_ok(tvg_canvas_sync(backend->canvas));
}

/*
 * Applies a paint as a shape's fill. A gradient's geometry is given in the 0
 * to 1 range of the shape it paints, so it is scaled to the bounds here and
 * one registered gradient serves a checkbox and a window alike.
 */
static int32_t schultz_tvg_apply_fill(schultz_thorvg *backend,
                                      Tvg_Paint shape, schultz_paint paint,
                                      schultz_rect bounds)
{
    const schultz_gradient *definition;
    Tvg_Gradient gradient;
    Tvg_Color_Stop stops[SCHULTZ_GRADIENT_STOPS_MAX];
    uint32_t i;

    if (paint.kind == SCHULTZ_PAINT_SOLID) {
        schultz_color c = paint.as.color;

        tvg_shape_set_fill_color(shape, c.r, c.g, c.b, c.a);
        return SCHULTZ_OK;
    }
    if (schultz_gradient_get(backend->resources, paint.as.gradient,
                             &definition) != SCHULTZ_OK) {
        /* An unknown gradient draws nothing rather than guessing a colour. */
        tvg_shape_set_fill_color(shape, 0u, 0u, 0u, 0u);
        return SCHULTZ_OK;
    }

    for (i = 0; i < definition->count; i++) {
        stops[i].offset = definition->stops[i].offset;
        stops[i].r      = definition->stops[i].color.r;
        stops[i].g      = definition->stops[i].color.g;
        stops[i].b      = definition->stops[i].color.b;
        stops[i].a      = definition->stops[i].color.a;
    }

    if (definition->kind == SCHULTZ_GRADIENT_RADIAL) {
        float side = (bounds.width > bounds.height) ? bounds.width
                                                    : bounds.height;

        gradient = tvg_radial_gradient_new();
        if (gradient == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        tvg_radial_gradient_set(gradient,
            bounds.x + definition->from.x * bounds.width,
            bounds.y + definition->from.y * bounds.height,
            definition->radius * side,
            bounds.x + definition->from.x * bounds.width,
            bounds.y + definition->from.y * bounds.height, 0.0f);
    } else {
        gradient = tvg_linear_gradient_new();
        if (gradient == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        tvg_linear_gradient_set(gradient,
            bounds.x + definition->from.x * bounds.width,
            bounds.y + definition->from.y * bounds.height,
            bounds.x + definition->to.x * bounds.width,
            bounds.y + definition->to.y * bounds.height);
    }
    tvg_gradient_set_color_stops(gradient, stops, definition->count);
    tvg_shape_set_gradient(shape, gradient);
    return SCHULTZ_OK;
}

/* The same for a stroke, plus its width, dash pattern, cap and join. */
static int32_t schultz_tvg_apply_stroke(schultz_thorvg *backend,
                                        Tvg_Paint shape,
                                        schultz_stroke stroke,
                                        schultz_rect bounds)
{
    static const Tvg_Stroke_Cap caps[] = {
        TVG_STROKE_CAP_BUTT, TVG_STROKE_CAP_ROUND, TVG_STROKE_CAP_SQUARE
    };
    static const Tvg_Stroke_Join joins[] = {
        TVG_STROKE_JOIN_MITER, TVG_STROKE_JOIN_ROUND, TVG_STROKE_JOIN_BEVEL
    };
    const schultz_dash *dash;

    if (stroke.paint.kind == SCHULTZ_PAINT_SOLID) {
        schultz_color c = stroke.paint.as.color;

        tvg_shape_set_stroke_color(shape, c.r, c.g, c.b, c.a);
    } else {
        const schultz_gradient *definition;
        Tvg_Gradient gradient = NULL;
        Tvg_Color_Stop stops[SCHULTZ_GRADIENT_STOPS_MAX];
        uint32_t i;

        if (schultz_gradient_get(backend->resources, stroke.paint.as.gradient,
                                 &definition) != SCHULTZ_OK) {
            tvg_shape_set_stroke_color(shape, 0u, 0u, 0u, 0u);
        } else {
            for (i = 0; i < definition->count; i++) {
                stops[i].offset = definition->stops[i].offset;
                stops[i].r      = definition->stops[i].color.r;
                stops[i].g      = definition->stops[i].color.g;
                stops[i].b      = definition->stops[i].color.b;
                stops[i].a      = definition->stops[i].color.a;
            }
            gradient = tvg_linear_gradient_new();
            if (gradient != NULL) {
                tvg_linear_gradient_set(gradient,
                    bounds.x + definition->from.x * bounds.width,
                    bounds.y + definition->from.y * bounds.height,
                    bounds.x + definition->to.x * bounds.width,
                    bounds.y + definition->to.y * bounds.height);
                tvg_gradient_set_color_stops(gradient, stops,
                                             definition->count);
                tvg_shape_set_stroke_gradient(shape, gradient);
            }
        }
    }

    tvg_shape_set_stroke_width(shape, schultz_tvg_scale(backend,
                                                        stroke.width));
    if (stroke.cap < sizeof(caps) / sizeof(caps[0])) {
        tvg_shape_set_stroke_cap(shape, caps[stroke.cap]);
    }
    if (stroke.join < sizeof(joins) / sizeof(joins[0])) {
        tvg_shape_set_stroke_join(shape, joins[stroke.join]);
    }
    if (stroke.miter_limit >= 0.0f) {
        tvg_shape_set_stroke_miterlimit(shape, stroke.miter_limit);
    }
    if (stroke.dash != SCHULTZ_HANDLE_NONE &&
        schultz_dash_get(backend->resources, stroke.dash, &dash)
            == SCHULTZ_OK) {
        /*
         * Scaled, like the width above. A dash pattern is a set of lengths in
         * the toolkit's units, and handing them over unscaled would leave the
         * marks the same size while the line they sit on doubled.
         */
        float lengths[SCHULTZ_DASH_MAX];
        uint32_t i;

        for (i = 0; i < dash->count && i < SCHULTZ_DASH_MAX; i++) {
            lengths[i] = schultz_tvg_scale(backend, dash->lengths[i]);
        }
        tvg_shape_set_stroke_dash(shape, lengths, i,
            schultz_tvg_scale(backend, stroke.dash_offset));
    }
    return SCHULTZ_OK;
}

static int32_t schultz_tvg_fill_rect(void *context,
                                     const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    schultz_rect r = schultz_tvg_place(backend, cmd->as.fill_rect.rect);
    Tvg_Paint shape = tvg_shape_new();

    if (shape == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    tvg_shape_append_rect(shape, r.x, r.y, r.width, r.height, 0.0f, 0.0f,
                          true);
    schultz_tvg_apply_fill(backend, shape, cmd->as.fill_rect.paint, r);
    return schultz_tvg_finish(backend, shape);
}

static int32_t schultz_tvg_stroke_rect(void *context,
                                       const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    schultz_rect r = schultz_tvg_place(backend, cmd->as.stroke_rect.rect);
    Tvg_Paint shape = tvg_shape_new();

    if (shape == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    tvg_shape_append_rect(shape, r.x, r.y, r.width, r.height, 0.0f, 0.0f,
                          true);
    schultz_tvg_apply_stroke(backend, shape, cmd->as.stroke_rect.stroke, r);
    return schultz_tvg_finish(backend, shape);
}

static int32_t schultz_tvg_fill_round_rect(void *context,
                                           const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    schultz_rect r = schultz_tvg_place(backend, cmd->as.fill_round_rect.rect);
    float radius = schultz_tvg_scale(backend,
                                     cmd->as.fill_round_rect.radius);
    Tvg_Paint shape = tvg_shape_new();

    if (shape == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    tvg_shape_append_rect(shape, r.x, r.y, r.width, r.height, radius, radius,
                          true);
    schultz_tvg_apply_fill(backend, shape, cmd->as.fill_round_rect.paint, r);
    return schultz_tvg_finish(backend, shape);
}

static int32_t schultz_tvg_stroke_round_rect(void *context,
                                             const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    schultz_rect r = schultz_tvg_place(backend,
                                       cmd->as.stroke_round_rect.rect);
    float radius = schultz_tvg_scale(backend,
                                     cmd->as.stroke_round_rect.radius);
    Tvg_Paint shape = tvg_shape_new();

    if (shape == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    tvg_shape_append_rect(shape, r.x, r.y, r.width, r.height, radius, radius,
                          true);
    schultz_tvg_apply_stroke(backend, shape,
                             cmd->as.stroke_round_rect.stroke, r);
    return schultz_tvg_finish(backend, shape);
}

/*
 * ThorVG draws a circle from a centre and two radii, so a rectangle becomes
 * its centre and half extents.
 */
static int32_t schultz_tvg_fill_ellipse(void *context,
                                        const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    schultz_rect r = schultz_tvg_place(backend, cmd->as.fill_ellipse.rect);
    Tvg_Paint shape = tvg_shape_new();

    if (shape == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    tvg_shape_append_circle(shape, r.x + r.width * 0.5f,
                            r.y + r.height * 0.5f, r.width * 0.5f,
                            r.height * 0.5f, true);
    schultz_tvg_apply_fill(backend, shape, cmd->as.fill_ellipse.paint, r);
    return schultz_tvg_finish(backend, shape);
}

static int32_t schultz_tvg_stroke_ellipse(void *context,
                                          const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    schultz_rect r = schultz_tvg_place(backend, cmd->as.stroke_ellipse.rect);
    Tvg_Paint shape = tvg_shape_new();

    if (shape == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    tvg_shape_append_circle(shape, r.x + r.width * 0.5f,
                            r.y + r.height * 0.5f, r.width * 0.5f,
                            r.height * 0.5f, true);
    schultz_tvg_apply_stroke(backend, shape, cmd->as.stroke_ellipse.stroke,
                             r);
    return schultz_tvg_finish(backend, shape);
}

static int32_t schultz_tvg_line(void *context, const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    Tvg_Paint shape = tvg_shape_new();
    schultz_point a = schultz_tvg_point(backend, cmd->as.line.from.x,
                                        cmd->as.line.from.y);
    schultz_point b = schultz_tvg_point(backend, cmd->as.line.to.x,
                                        cmd->as.line.to.y);
    float x0 = a.x;
    float y0 = a.y;
    float x1 = b.x;
    float y1 = b.y;

    if (shape == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    tvg_shape_move_to(shape, x0, y0);
    tvg_shape_line_to(shape, x1, y1);
    schultz_tvg_apply_stroke(backend, shape, cmd->as.line.stroke,
        schultz_rect_make((x0 < x1) ? x0 : x1, (y0 < y1) ? y0 : y1,
                          (x1 > x0) ? x1 - x0 : x0 - x1,
                          (y1 > y0) ? y1 - y0 : y0 - y1));
    return schultz_tvg_finish(backend, shape);
}

static int32_t schultz_tvg_fill_polygon(void *context,
                                        const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    const schultz_point *points = cmd->as.fill_polygon.points;
    uint32_t count = cmd->as.fill_polygon.count;
    schultz_rect bounds;
    Tvg_Paint shape;
    uint32_t i;

    if (count == 0) {
        return SCHULTZ_OK;
    }

    shape = tvg_shape_new();
    if (shape == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    {
        schultz_point at = schultz_tvg_point(backend, points[0].x,
                                             points[0].y);

        tvg_shape_move_to(shape, at.x, at.y);
    }
    for (i = 1; i < count; i++) {
        schultz_point at = schultz_tvg_point(backend, points[i].x,
                                             points[i].y);

        tvg_shape_line_to(shape, at.x, at.y);
    }
    tvg_shape_close(shape);

    /* A gradient on a path needs the path's own extent to scale into. */
    bounds = schultz_rect_make(points[0].x, points[0].y, 0.0f, 0.0f);
    for (i = 1; i < count; i++) {
        float right = bounds.x + bounds.width;
        float bottom = bounds.y + bounds.height;

        if (points[i].x < bounds.x) { bounds.x = points[i].x; }
        if (points[i].y < bounds.y) { bounds.y = points[i].y; }
        if (points[i].x > right)    { right = points[i].x; }
        if (points[i].y > bottom)   { bottom = points[i].y; }
        bounds.width  = right - bounds.x;
        bounds.height = bottom - bounds.y;
    }
    bounds = schultz_tvg_place(backend, bounds);
    tvg_shape_set_fill_rule(shape,
        (cmd->as.fill_polygon.rule == (uint32_t)SCHULTZ_FILL_EVEN_ODD)
            ? TVG_FILL_RULE_EVEN_ODD : TVG_FILL_RULE_NON_ZERO);
    schultz_tvg_apply_fill(backend, shape, cmd->as.fill_polygon.paint, bounds);
    return schultz_tvg_finish(backend, shape);
}
/*
 * A polygon's outline, or a line through a series of points when it is not
 * closed. The same shape either way; only the closing segment differs, and
 * that is what separates an outline from a polyline.
 */
static int32_t schultz_tvg_stroke_polygon(void *context,
                                          const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    const schultz_point *points = cmd->as.stroke_polygon.points;
    uint32_t count = cmd->as.stroke_polygon.count;
    schultz_rect bounds;
    Tvg_Paint shape;
    uint32_t i;

    if (count < 2u) {
        return SCHULTZ_OK;
    }

    shape = tvg_shape_new();
    if (shape == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    {
        schultz_point at = schultz_tvg_point(backend, points[0].x,
                                             points[0].y);

        tvg_shape_move_to(shape, at.x, at.y);
    }
    for (i = 1; i < count; i++) {
        schultz_point at = schultz_tvg_point(backend, points[i].x,
                                             points[i].y);

        tvg_shape_line_to(shape, at.x, at.y);
    }
    if (cmd->as.stroke_polygon.closed) {
        tvg_shape_close(shape);
    }

    /* A gradient on a stroke scales into the path's own extent, as a fill
     * does. */
    bounds = schultz_rect_make(points[0].x, points[0].y, 0.0f, 0.0f);
    for (i = 1; i < count; i++) {
        float right = bounds.x + bounds.width;
        float bottom = bounds.y + bounds.height;

        if (points[i].x < bounds.x) { bounds.x = points[i].x; }
        if (points[i].y < bounds.y) { bounds.y = points[i].y; }
        if (points[i].x > right)    { right = points[i].x; }
        if (points[i].y > bottom)   { bottom = points[i].y; }
        bounds.width  = right - bounds.x;
        bounds.height = bottom - bounds.y;
    }
    bounds = schultz_tvg_place(backend, bounds);
    schultz_tvg_apply_stroke(backend, shape, cmd->as.stroke_polygon.stroke,
                             bounds);
    return schultz_tvg_finish(backend, shape);
}


/*
 * Images still need the resource table that a later phase brings. Until a
 * handle can be resolved to decoded pixels this reports that the command was
 * understood but not drawn, rather than pretending to succeed.
 */
/*
 * Draws a loaded image into the destination rectangle. The picture handed
 * back is a copy, because the canvas owns whatever is added to it and drops
 * everything at the end of a frame, while the table keeps its own for as long
 * as it lives.
 */
static int32_t schultz_tvg_image(void *context, const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    schultz_rect dest = schultz_tvg_place(backend, cmd->as.image.dest);
    schultz_rect source = cmd->as.image.source;
    schultz_size natural;
    Tvg_Paint picture;

    if (backend->images == NULL || schultz_rect_is_empty(dest)) {
        return SCHULTZ_OK;
    }
    if (schultz_image_size(backend->images, cmd->as.image.image, &natural)
            != SCHULTZ_OK || natural.width <= 0.0f ||
        natural.height <= 0.0f) {
        /* An image that is not loaded draws nothing rather than a placeholder. */
        return SCHULTZ_OK;
    }
    picture = schultz_image_picture(backend->images, cmd->as.image.image);
    if (picture == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    /*
     * Placed with a transform rather than by resizing. Two reasons: resizing
     * keeps the picture's aspect ratio, so it can never fill a box that is a
     * different shape, and an animation's picture is a scene its animation is
     * driving, which resizing fights. A matrix does both jobs and touches
     * neither the picture nor the scene.
     */
    {
        Tvg_Matrix place;
        float scale_x;
        float scale_y;

        if (schultz_rect_is_empty(source)) {
            scale_x = dest.width / natural.width;
            scale_y = dest.height / natural.height;
            place.e13 = dest.x;
            place.e23 = dest.y;
        } else {
            /*
             * A region of the image, scaled so that region covers the
             * destination and shifted so its corner lands on it. What falls
             * outside is clipped below.
             */
            scale_x = dest.width / source.width;
            scale_y = dest.height / source.height;
            place.e13 = dest.x - source.x * scale_x;
            place.e23 = dest.y - source.y * scale_y;
        }
        place.e11 = scale_x;
        place.e12 = 0.0f;
        place.e21 = 0.0f;
        place.e22 = scale_y;
        place.e31 = 0.0f;
        place.e32 = 0.0f;
        place.e33 = 1.0f;
        tvg_paint_set_transform(picture, &place);
    }
    tvg_paint_set_opacity(picture, cmd->as.image.opacity);

    if (!schultz_rect_is_empty(source)) {
        Tvg_Paint clip = tvg_shape_new();

        if (clip == NULL) {
            tvg_paint_unref(picture, true);
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        tvg_shape_append_rect(clip, dest.x, dest.y, dest.width, dest.height,
                              0.0f, 0.0f, true);
        tvg_paint_set_clip(picture, clip);
    }
    return schultz_tvg_finish(backend, picture);
}

/* Grows the glyph run scratch buffer to hold at least `words` pixels. */
static int32_t schultz_tvg_reserve_run(schultz_thorvg *backend, size_t words)
{
    uint32_t *pixels;

    if (words <= backend->run_capacity) {
        return SCHULTZ_OK;
    }
    pixels = (uint32_t *)realloc(backend->run_pixels,
                                 words * sizeof(*pixels));
    if (pixels == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    backend->run_pixels   = pixels;
    backend->run_capacity = words;
    return SCHULTZ_OK;
}

/**
 * @brief Where a run's composited bitmap sits, and what kinds of glyph it holds.
 *
 * Worked out by one pass over the run before anything is drawn, because the
 * buffer has to be sized before it can be written to and because a gradient
 * needs to know whether there is an emoji to keep out of it.
 */
typedef struct {
    int32_t  min_x;        /**< Left edge, in device pixels. */
    int32_t  min_y;        /**< Top edge, in device pixels. */
    uint32_t width;        /**< How wide the composited bitmap is. */
    uint32_t height;       /**< How tall it is. */
    int32_t  has_letters;  /**< Set when a coverage glyph is in the run. */
    int32_t  has_pictures; /**< Set when a colour glyph is in the run. */
} schultz_tvg_run;

/** Which glyphs a composite pass should write. */
enum {
    SCHULTZ_TVG_RUN_LETTERS = 0, /**< Only the ones that arrive as coverage. */
    SCHULTZ_TVG_RUN_PICTURES,    /**< Only the ones that bring their colours. */
    SCHULTZ_TVG_RUN_EVERYTHING   /**< Both, which is the ordinary case. */
};

/* Where one glyph's bitmap lands, in device pixels before the run is placed. */
static void schultz_tvg_glyph_at(const schultz_glyph *glyph, float scale,
                                 const schultz_glyph_bitmap *bitmap,
                                 int32_t *out_left, int32_t *out_top)
{
    *out_left = (int32_t)(glyph->x * scale) + bitmap->bearing_x;
    *out_top  = (int32_t)(glyph->y * scale) - bitmap->bearing_y;
}

/* First pass: the bounding box of every glyph's bitmap, and what kinds there are. */
static int32_t schultz_tvg_run_measure(schultz_thorvg *backend,
                                       schultz_handle font,
                                       const schultz_glyph *glyphs,
                                       uint32_t count, float scale,
                                       uint32_t which,
                                       schultz_tvg_run *out_run)
{
    int32_t min_x = 0;
    int32_t min_y = 0;
    int32_t max_x = 0;
    int32_t max_y = 0;
    int32_t have_box = 0;
    uint32_t i;

    memset(out_run, 0, sizeof(*out_run));
    for (i = 0; i < count; i++) {
        const schultz_glyph_bitmap *bitmap;
        int32_t left;
        int32_t top;
        int32_t result = schultz_glyph_cache_get(backend->glyphs, font,
                                                 glyphs[i].glyph_id, &bitmap);

        if (result != SCHULTZ_OK) {
            return result;
        }
        if (bitmap->bitmap == NULL) {
            continue; /* blank glyph, such as a space */
        }
        if (bitmap->format == (uint32_t)SCHULTZ_GLYPH_COLOR) {
            out_run->has_pictures = 1;
            if (which == (uint32_t)SCHULTZ_TVG_RUN_LETTERS) {
                continue;
            }
        } else {
            out_run->has_letters = 1;
            if (which == (uint32_t)SCHULTZ_TVG_RUN_PICTURES) {
                continue;
            }
        }
        schultz_tvg_glyph_at(&glyphs[i], scale, bitmap, &left, &top);
        if (!have_box) {
            min_x = left;
            min_y = top;
            max_x = left + (int32_t)bitmap->width;
            max_y = top + (int32_t)bitmap->height;
            have_box = 1;
            continue;
        }
        if (left < min_x) { min_x = left; }
        if (top  < min_y) { min_y = top; }
        if (left + (int32_t)bitmap->width  > max_x) {
            max_x = left + (int32_t)bitmap->width;
        }
        if (top + (int32_t)bitmap->height > max_y) {
            max_y = top + (int32_t)bitmap->height;
        }
    }
    if (!have_box || max_x <= min_x || max_y <= min_y) {
        return SCHULTZ_OK; /* nothing to draw; width stays zero */
    }
    out_run->min_x  = min_x;
    out_run->min_y  = min_y;
    out_run->width  = (uint32_t)(max_x - min_x);
    out_run->height = (uint32_t)(max_y - min_y);
    return SCHULTZ_OK;
}

/*
 * Second pass: composite the wanted glyphs into the scratch buffer. ThorVG's
 * ARGB8888 is alpha premultiplied, so each channel is scaled by the final
 * alpha.
 *
 * A letter arrives as coverage and takes its colour from `color`. An emoji
 * arrives with its own colours and takes only how solid `color` is, because
 * there is no sense in which a yellow face is "the text colour".
 */
static int32_t schultz_tvg_run_compose(schultz_thorvg *backend,
                                       schultz_handle font,
                                       const schultz_glyph *glyphs,
                                       uint32_t count, float scale,
                                       const schultz_tvg_run *run,
                                       schultz_color color, uint32_t which)
{
    uint32_t i;

    memset(backend->run_pixels, 0,
           (size_t)run->width * run->height * sizeof(*backend->run_pixels));

    for (i = 0; i < count; i++) {
        const schultz_glyph_bitmap *bitmap;
        int32_t is_picture;
        int32_t left;
        int32_t top;
        uint32_t row;
        uint32_t col;
        int32_t result = schultz_glyph_cache_get(backend->glyphs, font,
                                                 glyphs[i].glyph_id, &bitmap);

        if (result != SCHULTZ_OK) {
            return result;
        }
        if (bitmap->bitmap == NULL) {
            continue;
        }
        is_picture = (bitmap->format == (uint32_t)SCHULTZ_GLYPH_COLOR);
        if (which == (uint32_t)SCHULTZ_TVG_RUN_LETTERS && is_picture) {
            continue;
        }
        if (which == (uint32_t)SCHULTZ_TVG_RUN_PICTURES && !is_picture) {
            continue;
        }

        schultz_tvg_glyph_at(&glyphs[i], scale, bitmap, &left, &top);
        left -= run->min_x;
        top  -= run->min_y;

        for (row = 0; row < bitmap->height; row++) {
            uint32_t *dest = backend->run_pixels +
                             (size_t)(top + (int32_t)row) * run->width + left;
            const unsigned char *src = bitmap->bitmap +
                                       (size_t)row * bitmap->pitch;

            if (is_picture) {
                /*
                 * FreeType hands these over blue green red alpha, already
                 * multiplied by their own alpha, which is the same order
                 * and the same convention the buffer wants. Only the run's
                 * own opacity has to be folded in.
                 */
                for (col = 0; col < bitmap->width; col++) {
                    const unsigned char *pixel = src + (size_t)col * 4u;
                    uint32_t a = pixel[3];

                    if (a == 0u) {
                        continue;
                    }
                    if (color.a == 255u) {
                        dest[col] = ((uint32_t)a << 24) |
                                    ((uint32_t)pixel[2] << 16) |
                                    ((uint32_t)pixel[1] << 8) |
                                    (uint32_t)pixel[0];
                    } else {
                        dest[col] =
                            ((a * color.a / 255u) << 24) |
                            (((uint32_t)pixel[2] * color.a / 255u) << 16) |
                            (((uint32_t)pixel[1] * color.a / 255u) << 8) |
                            ((uint32_t)pixel[0] * color.a / 255u);
                    }
                }
                continue;
            }

            for (col = 0; col < bitmap->width; col++) {
                uint32_t coverage = src[col];
                uint32_t alpha;

                if (coverage == 0) {
                    continue;
                }
                alpha = (coverage * color.a) / 255u;
                dest[col] = (alpha << 24) |
                            (((uint32_t)color.r * alpha / 255u) << 16) |
                            (((uint32_t)color.g * alpha / 255u) << 8) |
                            ((uint32_t)color.b * alpha / 255u);
            }
        }
    }
    return SCHULTZ_OK;
}

/*
 * Turns what is in the scratch buffer into a placed ThorVG picture.
 *
 * copy=true because run_pixels is reused by the next run in this frame, while
 * ThorVG does not rasterize until the canvas is drawn.
 */
static Tvg_Paint schultz_tvg_run_picture(schultz_thorvg *backend,
                                         const schultz_tvg_run *run,
                                         float scale)
{
    Tvg_Paint picture = tvg_picture_new();

    if (picture == NULL) {
        return NULL;
    }
    if (tvg_picture_load_raw(picture, backend->run_pixels, run->width,
                             run->height, TVG_COLORSPACE_ARGB8888, true)
            != TVG_RESULT_SUCCESS) {
        tvg_paint_unref(picture, true);
        return NULL;
    }
    tvg_paint_translate(picture,
                        (float)run->min_x + backend->offset.x * scale,
                        (float)run->min_y + backend->offset.y * scale);
    return picture;
}

/*
 * Draws one glyph from the font's outline rather than from its cached
 * picture, which is what keeps text sharp when it is turned.
 *
 * The cache holds each letter rasterized upright, and turning one of those
 * resamples pixels that have already been drawn: the result is soft on a page
 * that is sharp everywhere else. So at an angle the outline is fetched
 * instead and handed to the rasterizer as a path, which turns it before any
 * pixel exists. Hinting is given up, which is what happens to rotated text in
 * every toolkit and is why no toolkit rotates body text.
 *
 * Returns nonzero when it drew, so the caller can fall back for a glyph that
 * has no outline at all.
 */
static int32_t schultz_tvg_glyph_shape(schultz_thorvg *backend,
                                       schultz_handle font,
                                       const schultz_glyph *glyph,
                                       schultz_paint paint,
                                       const schultz_stroke *stroke)
{
    Tvg_Paint shape;
    Tvg_Matrix place;
    schultz_point pen;
    float units = 1.0f;
    float size;

    shape = (Tvg_Paint)schultz_glyph_outline(backend->glyphs, font,
                                             glyph->glyph_id, &units);
    if (shape == NULL) {
        return 0;
    }

    /*
     * From the font's units to device pixels. The y axis turns over on the
     * way, because a font measures upward from the baseline and a buffer
     * counts rows downward from the top. The pen goes in the last column,
     * which is where this glyph sits along the run.
     */
    pen = schultz_tvg_point(backend, glyph->x, glyph->y);
    place.e11 = units;
    place.e12 = 0.0f;
    place.e13 = pen.x;
    place.e21 = 0.0f;
    place.e22 = -units;
    place.e23 = pen.y;
    place.e31 = 0.0f;
    place.e32 = 0.0f;
    place.e33 = 1.0f;
    tvg_paint_set_transform(shape, &place);

    /*
     * A gradient on turned text is measured across the one glyph rather than
     * the whole run, which is the one thing this route does not match the
     * cached one on. A run's box is not known until every glyph is placed,
     * and holding the shapes back to find it would cost more than it is worth
     * for text at an angle.
     */
    size = schultz_tvg_scale(backend, schultz_font_size(backend->fonts, font));
    if (stroke != NULL) {
        /*
         * The outline is in font units and the matrix above shrinks it to
         * pixels, so a stroke width given in pixels has to be grown by the
         * same amount or it would come out hair thin.
         */
        schultz_stroke widened = *stroke;

        widened.width = (units > 0.0f) ? widened.width / units
                                       : widened.width;
        tvg_shape_set_fill_color(shape, 0u, 0u, 0u, 0u);
        schultz_tvg_apply_stroke(backend, shape, widened,
            schultz_rect_make(pen.x, pen.y - size, size, size));
    } else {
        schultz_tvg_apply_fill(backend, shape, paint,
            schultz_rect_make(pen.x, pen.y - size, size, size));
    }
    return (schultz_tvg_finish(backend, shape) == SCHULTZ_OK) ? 1 : 0;
}

/*
 * Draws a run of glyphs as one image rather than one image per glyph.
 *
 * The run's glyphs are composited into a single ARGB buffer sized to their
 * combined bounding box, then handed to ThorVG as one picture. That keeps the
 * canvas to one paint per run instead of one per character, which matters as
 * soon as a screen holds real text.
 *
 * A gradient cannot fill a picture, so it takes a second route: the letters
 * are composited in white, which leaves the buffer holding nothing but their
 * coverage, and that picture becomes an alpha mask over a rectangle filled
 * with the gradient. Emoji are kept out of that mask and drawn as themselves,
 * because a gradient laid over a picture would throw its colours away.
 */
static int32_t schultz_tvg_glyph_run(void *context,
                                     const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    const schultz_glyph *glyphs = cmd->as.glyph_run.glyphs;
    uint32_t count = cmd->as.glyph_run.count;
    schultz_paint paint = cmd->as.glyph_run.paint;
    schultz_handle font = cmd->as.glyph_run.font;
    float scale = backend->scale;
    int32_t is_gradient = (paint.kind != SCHULTZ_PAINT_SOLID);
    schultz_color color = is_gradient ? schultz_color_rgba(255u, 255u, 255u,
                                                           255u)
                                      : paint.as.color;
    schultz_tvg_run run;
    Tvg_Paint picture;
    int32_t result;

    if (backend->fonts == NULL || backend->glyphs == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (count == 0 || schultz_paint_is_invisible(paint)) {
        return SCHULTZ_OK;
    }

    /*
     * At any scale but one, the same face is used at the scaled size and the
     * glyphs are rasterized there. Blowing up a screen sized bitmap would put
     * blurred text on a page that is sharp everywhere else, and a glyph index
     * means the same thing at every size of one face, so the run itself needs
     * no reshaping.
     */
    if (scale != 1.0f) {
        schultz_handle sized = SCHULTZ_HANDLE_NONE;
        float size = schultz_font_size(backend->fonts, font);

        if (size > 0.0f &&
            schultz_font_at_size(backend->fonts, font, size * scale, &sized)
                == SCHULTZ_OK) {
            font = sized;
        }
    }

    /*
     * At an angle a letter is drawn from its outline, so the turn happens
     * before anything is rasterized and the result stays sharp. An emoji is
     * a picture either way, and turning a picture of a face is not the
     * mistake that turning a picture of a letter is, so those keep the route
     * below. Which is which is decided by what the cache holds, not by
     * whether an outline happened to be found, so the split is the same every
     * frame.
     */
    if (backend->rotated) {
        uint32_t i;

        for (i = 0; i < count; i++) {
            const schultz_glyph_bitmap *bitmap;

            result = schultz_glyph_cache_get(backend->glyphs, font,
                                             glyphs[i].glyph_id, &bitmap);
            if (result != SCHULTZ_OK) {
                return result;
            }
            if (bitmap->bitmap == NULL ||
                bitmap->format == (uint32_t)SCHULTZ_GLYPH_COLOR) {
                continue;
            }
            if (!schultz_tvg_glyph_shape(backend, font, &glyphs[i], paint,
                                         NULL)) {
                /* A letter with no outline is a face this cannot help with;
                 * it is left to the picture route with the emoji. */
                continue;
            }
        }
    }

    result = schultz_tvg_run_measure(backend, font, glyphs, count, scale,
                 backend->rotated ? (uint32_t)SCHULTZ_TVG_RUN_PICTURES
                                  : (uint32_t)SCHULTZ_TVG_RUN_EVERYTHING,
                 &run);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (run.width == 0u || run.height == 0u) {
        return SCHULTZ_OK;
    }
    /* Turned, the only thing left for the picture route is the emoji. */
    if (backend->rotated) {
        result = schultz_tvg_reserve_run(backend,
                                         (size_t)run.width * run.height);
        if (result != SCHULTZ_OK) {
            return result;
        }
        result = schultz_tvg_run_compose(backend, font, glyphs, count, scale,
                                         &run,
                                         schultz_color_rgba(255u, 255u, 255u,
                                                            255u),
                                         SCHULTZ_TVG_RUN_PICTURES);
        if (result != SCHULTZ_OK) {
            return result;
        }
        picture = schultz_tvg_run_picture(backend, &run, scale);
        if (picture == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        return schultz_tvg_finish(backend, picture);
    }
    result = schultz_tvg_reserve_run(backend,
                                    (size_t)run.width * run.height);
    if (result != SCHULTZ_OK) {
        return result;
    }

    if (!is_gradient) {
        result = schultz_tvg_run_compose(backend, font, glyphs, count, scale,
                                         &run, color,
                                         SCHULTZ_TVG_RUN_EVERYTHING);
        if (result != SCHULTZ_OK) {
            return result;
        }
        picture = schultz_tvg_run_picture(backend, &run, scale);
        if (picture == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        return schultz_tvg_finish(backend, picture);
    }

    /* The letters, as a mask over a rectangle carrying the gradient. */
    if (run.has_letters) {
        Tvg_Paint sheet;
        schultz_rect bounds = schultz_rect_make(
            (float)run.min_x + backend->offset.x * scale,
            (float)run.min_y + backend->offset.y * scale,
            (float)run.width, (float)run.height);

        result = schultz_tvg_run_compose(backend, font, glyphs, count, scale,
                                         &run, color,
                                         SCHULTZ_TVG_RUN_LETTERS);
        if (result != SCHULTZ_OK) {
            return result;
        }
        picture = schultz_tvg_run_picture(backend, &run, scale);
        if (picture == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        sheet = tvg_shape_new();
        if (sheet == NULL) {
            tvg_paint_unref(picture, true);
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        tvg_shape_append_rect(sheet, bounds.x, bounds.y, bounds.width,
                              bounds.height, 0.0f, 0.0f, true);
        /*
         * The gradient is measured across the run's own bounds, so a heading
         * reads the same whatever words are in it. The mask is owned by the
         * shape it masks, the way a clipper is.
         */
        schultz_tvg_apply_fill(backend, sheet, paint, bounds);
        tvg_paint_set_mask_method(sheet, picture, TVG_MASK_METHOD_ALPHA);
        result = schultz_tvg_finish(backend, sheet);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }

    /* The emoji, which keep their own colours and ignore the gradient. */
    if (run.has_pictures) {
        result = schultz_tvg_run_compose(backend, font, glyphs, count, scale,
                                         &run,
                                         schultz_color_rgba(255u, 255u, 255u,
                                                            255u),
                                         SCHULTZ_TVG_RUN_PICTURES);
        if (result != SCHULTZ_OK) {
            return result;
        }
        picture = schultz_tvg_run_picture(backend, &run, scale);
        if (picture == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        return schultz_tvg_finish(backend, picture);
    }
    return SCHULTZ_OK;
}

/*
 * A group: everything until the matching end composes into one picture.
 *
 * Opacity on a scene is applied to the composed result, which is the whole
 * point. Fading each shape separately double blends wherever two of them
 * overlap; fading the picture they make does not.
 */
static int32_t schultz_tvg_group_begin(void *context,
                                      const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    Tvg_Paint scene;
    float opacity = cmd->as.group_begin.opacity;

    if (backend->scene_depth >= SCHULTZ_TVG_STACK_MAX) {
        backend->overflowed = 1;
        return SCHULTZ_ERR_EXHAUSTED;
    }
    scene = tvg_scene_new();
    if (scene == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    if (opacity < 0.0f) {
        opacity = 0.0f;
    }
    if (opacity > 1.0f) {
        opacity = 1.0f;
    }
    backend->scenes[backend->scene_depth] = scene;
    backend->scene_opacity[backend->scene_depth] =
        (uint8_t)(opacity * 255.0f + 0.5f);
    backend->scene_shadow[backend->scene_depth] = cmd->as.group_begin.shadow;
    backend->scene_depth++;
    return SCHULTZ_OK;
}

static int32_t schultz_tvg_group_end(void *context)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    Tvg_Paint scene;
    uint8_t opacity;

    if (backend->scene_depth == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT; /* an end with no start */
    }
    backend->scene_depth--;
    scene   = backend->scenes[backend->scene_depth];
    opacity = backend->scene_opacity[backend->scene_depth];
    tvg_paint_set_opacity(scene, opacity);

    /*
     * The shadow is an effect on the finished scene, so it is added here
     * rather than when the group opened: it falls behind whatever the scene
     * turned out to contain.
     *
     * Distance and blur are lengths, so they scale with the screen like every
     * other length. The angle does not. Quality is ThorVG's count of blur
     * passes, and 100 is every pass it offers, which is what a shadow on a
     * user interface wants: a visible band in a soft edge is worse than the
     * passes cost.
     */
    {
        const schultz_shadow *shadow = &backend->scene_shadow[
            backend->scene_depth];

        if (shadow->color.a > 0u) {
            tvg_scene_add_effect_drop_shadow(
                scene, (int)shadow->color.r, (int)shadow->color.g,
                (int)shadow->color.b, (int)shadow->color.a,
                (double)shadow->angle,
                (double)(shadow->distance * backend->scale),
                (double)(shadow->blur * backend->scale), 100);
        }
    }

    /* Out to whatever encloses it, which is the next group or the canvas. */
    if (backend->scene_depth > 0u) {
        return schultz_tvg_ok(tvg_scene_add(
            backend->scenes[backend->scene_depth - 1u], scene));
    }
    return schultz_tvg_ok(tvg_canvas_add(backend->canvas, scene));
}

static int32_t schultz_tvg_clip_begin(void *context,
                                     const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    schultz_rect rect;

    if (backend->clip_depth >= SCHULTZ_TVG_STACK_MAX) {
        backend->overflowed = 1;
        return SCHULTZ_ERR_EXHAUSTED;
    }

    backend->clips[backend->clip_depth] = backend->clip;
    backend->clip_depth++;

    rect = schultz_tvg_place(backend, cmd->as.clip_begin.rect);
    if (backend->rotated) {
        /*
         * The clip stack is rectangles, and intersecting two turned ones does
         * not give a rectangle. So a clip started inside a rotation is the
         * upright box around the turned rectangle: it lets through a little
         * more than it names, never less, which is the safe direction to be
         * wrong in. The canvas's own clip starts outside any rotation and
         * is therefore exact.
         */
        schultz_point corner[4];
        float least_x;
        float least_y;
        float most_x;
        float most_y;
        uint32_t i;

        corner[0] = schultz_point_make(rect.x, rect.y);
        corner[1] = schultz_point_make(rect.x + rect.width, rect.y);
        corner[2] = schultz_point_make(rect.x, rect.y + rect.height);
        corner[3] = schultz_point_make(rect.x + rect.width,
                                       rect.y + rect.height);
        least_x = most_x = backend->rotation.e11 * corner[0].x +
                           backend->rotation.e12 * corner[0].y +
                           backend->rotation.e13;
        least_y = most_y = backend->rotation.e21 * corner[0].x +
                           backend->rotation.e22 * corner[0].y +
                           backend->rotation.e23;
        for (i = 1u; i < 4u; i++) {
            float x = backend->rotation.e11 * corner[i].x +
                      backend->rotation.e12 * corner[i].y +
                      backend->rotation.e13;
            float y = backend->rotation.e21 * corner[i].x +
                      backend->rotation.e22 * corner[i].y +
                      backend->rotation.e23;

            if (x < least_x) { least_x = x; }
            if (y < least_y) { least_y = y; }
            if (x > most_x)  { most_x  = x; }
            if (y > most_y)  { most_y  = y; }
        }
        rect = schultz_rect_make(least_x, least_y, most_x - least_x,
                                 most_y - least_y);
    }
    backend->clip = schultz_rect_intersect(backend->clip, rect);
    return SCHULTZ_OK;
}

static int32_t schultz_tvg_clip_end(void *context)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;

    if (backend->clip_depth == 0) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend->clip_depth--;
    backend->clip = backend->clips[backend->clip_depth];
    return SCHULTZ_OK;
}

/*
 * Turns everything that follows about a point.
 *
 * The centre is placed to device pixels the ordinary way, with the offset and
 * the scale but not with any rotation already in force. That is deliberate:
 * the new turn multiplies onto the right of the old one, so the old one is
 * applied to the result and the untouched centre is exactly what the new turn
 * should pivot about.
 *
 * Because the turn is applied last, to the finished device point, an offset
 * started inside it moves along the turned axes without anything here having
 * to know about it.
 */
/*
 * A run drawn as outlines and stroked. Nothing is cached, because a stroked
 * heading is a few words rather than a page of prose, and an outline is what
 * a stroke needs.
 */
static int32_t schultz_tvg_glyph_outline_run(void *context,
                                             const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    schultz_handle font = cmd->as.glyph_outline.font;
    schultz_stroke stroke = cmd->as.glyph_outline.stroke;
    float scale = backend->scale;
    uint32_t i;

    if (backend->fonts == NULL || backend->glyphs == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (cmd->as.glyph_outline.count == 0u ||
        schultz_paint_is_invisible(stroke.paint)) {
        return SCHULTZ_OK;
    }
    if (scale != 1.0f) {
        schultz_handle sized = SCHULTZ_HANDLE_NONE;
        float size = schultz_font_size(backend->fonts, font);

        if (size > 0.0f &&
            schultz_font_at_size(backend->fonts, font, size * scale, &sized)
                == SCHULTZ_OK) {
            font = sized;
        }
    }
    for (i = 0; i < cmd->as.glyph_outline.count; i++) {
        /* A glyph with no outline has nothing to stroke and is left out. */
        (void)schultz_tvg_glyph_shape(backend, font,
                                      &cmd->as.glyph_outline.glyphs[i],
                                      stroke.paint, &stroke);
    }
    return SCHULTZ_OK;
}

static int32_t schultz_tvg_rotation_begin(void *context,
                                         const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    schultz_point pivot;
    Tvg_Matrix turn;
    float radians;
    float c;
    float sn;

    if (backend->rotation_depth >= SCHULTZ_TVG_STACK_MAX) {
        backend->overflowed = 1;
        return SCHULTZ_ERR_EXHAUSTED;
    }
    backend->rotations[backend->rotation_depth] = backend->rotation;
    backend->rotation_depth++;

    pivot = schultz_tvg_point(backend, cmd->as.rotation_begin.cx,
                              cmd->as.rotation_begin.cy);
    radians = cmd->as.rotation_begin.degrees * 3.14159265358979f / 180.0f;
    c  = cosf(radians);
    sn = sinf(radians);

    /* Move the centre to the origin, turn, and put it back. */
    turn.e11 = c;
    turn.e12 = -sn;
    turn.e13 = pivot.x - c * pivot.x + sn * pivot.y;
    turn.e21 = sn;
    turn.e22 = c;
    turn.e23 = pivot.y - sn * pivot.x - c * pivot.y;
    turn.e31 = 0.0f;
    turn.e32 = 0.0f;
    turn.e33 = 1.0f;

    schultz_tvg_times(&backend->rotation, &turn);
    backend->rotated = 1;
    return SCHULTZ_OK;
}

static int32_t schultz_tvg_rotation_end(void *context)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;

    if (backend->rotation_depth == 0) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend->rotation_depth--;
    backend->rotation = backend->rotations[backend->rotation_depth];
    backend->rotated  = (backend->rotation_depth > 0) ? 1 : 0;
    return SCHULTZ_OK;
}

static int32_t schultz_tvg_offset_begin(void *context,
                                          const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;

    if (backend->offset_depth >= SCHULTZ_TVG_STACK_MAX) {
        backend->overflowed = 1;
        return SCHULTZ_ERR_EXHAUSTED;
    }

    backend->offsets[backend->offset_depth] = backend->offset;
    backend->offset_depth++;
    backend->offset.x += cmd->as.offset_begin.dx;
    backend->offset.y += cmd->as.offset_begin.dy;
    return SCHULTZ_OK;
}

static int32_t schultz_tvg_offset_end(void *context)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;

    if (backend->offset_depth == 0) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend->offset_depth--;
    backend->offset = backend->offsets[backend->offset_depth];
    return SCHULTZ_OK;
}

/*
 * Draw what follows in screen pixels.
 *
 * A point becomes (x + offset) * scale, so making one unit mean one pixel is
 * a matter of setting the scale to one. The offset cannot be left alone
 * though: it was accumulated in the toolkit's units and was going to be
 * multiplied by the old scale on the way out, so it is multiplied here
 * instead. Where the drawing sits is then unchanged and only the size of a
 * unit inside it differs, which is the whole point.
 *
 * On a screen with nothing to scale the scale is already one and this
 * changes nothing.
 */
static int32_t schultz_tvg_device_pixels_begin(void *context)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;

    if (backend->scale_depth >= SCHULTZ_TVG_STACK_MAX) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    backend->scales[backend->scale_depth]        = backend->scale;
    backend->scale_offsets[backend->scale_depth] = backend->offset;
    backend->scale_depth++;

    backend->offset = schultz_point_make(backend->offset.x * backend->scale,
                                         backend->offset.y * backend->scale);
    backend->scale  = 1.0f;
    return SCHULTZ_OK;
}

static int32_t schultz_tvg_device_pixels_end(void *context)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;

    if (backend->scale_depth == 0) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend->scale_depth--;
    backend->scale  = backend->scales[backend->scale_depth];
    backend->offset = backend->scale_offsets[backend->scale_depth];
    return SCHULTZ_OK;
}

/*
 * Walks a path's two arrays onto a ThorVG shape, and reports the bounding box
 * a gradient needs to scale into.
 *
 * ThorVG has the same four steps under different names, so this is a
 * translation rather than a construction. Everything is placed on the way
 * through, since a path's points are in the toolkit's units like any other.
 */
static void schultz_tvg_path_onto(schultz_thorvg *backend, Tvg_Paint shape,
                                  const uint8_t *steps, uint32_t step_count,
                                  const schultz_point *points,
                                  schultz_rect *out_bounds)
{
    schultz_point least = { 0.0f, 0.0f };
    schultz_point most = { 0.0f, 0.0f };
    int32_t have_box = 0;
    uint32_t from = 0u;
    uint32_t i;

    for (i = 0; i < step_count; i++) {
        uint32_t step = steps[i];
        uint32_t n = schultz_path_step_points(step);
        schultz_point at[3];
        uint32_t j;

        for (j = 0; j < n; j++) {
            at[j] = schultz_tvg_point(backend, points[from + j].x,
                                      points[from + j].y);
            if (!have_box) {
                least = at[j];
                most  = at[j];
                have_box = 1;
            } else {
                if (at[j].x < least.x) { least.x = at[j].x; }
                if (at[j].y < least.y) { least.y = at[j].y; }
                if (at[j].x > most.x)  { most.x  = at[j].x; }
                if (at[j].y > most.y)  { most.y  = at[j].y; }
            }
        }
        from += n;

        switch (step) {
        case SCHULTZ_PATH_MOVE:
            tvg_shape_move_to(shape, at[0].x, at[0].y);
            break;
        case SCHULTZ_PATH_LINE:
            tvg_shape_line_to(shape, at[0].x, at[0].y);
            break;
        case SCHULTZ_PATH_CURVE:
            tvg_shape_cubic_to(shape, at[0].x, at[0].y, at[1].x, at[1].y,
                               at[2].x, at[2].y);
            break;
        default:
            tvg_shape_close(shape);
            break;
        }
    }
    /*
     * The control points are in this box as well as the ends. That is wider
     * than the ink for a curve that bends inward, which only affects where a
     * gradient's stops land, and is what every other shape here does too.
     */
    *out_bounds = schultz_rect_make(least.x, least.y, most.x - least.x,
                                    most.y - least.y);
}

static int32_t schultz_tvg_fill_path(void *context,
                                     const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    schultz_rect bounds;
    Tvg_Paint shape = tvg_shape_new();

    if (shape == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    schultz_tvg_path_onto(backend, shape, cmd->as.fill_path.steps,
                          cmd->as.fill_path.step_count,
                          cmd->as.fill_path.points, &bounds);
    tvg_shape_set_fill_rule(shape,
        (cmd->as.fill_path.rule == (uint32_t)SCHULTZ_FILL_EVEN_ODD)
            ? TVG_FILL_RULE_EVEN_ODD : TVG_FILL_RULE_NON_ZERO);
    schultz_tvg_apply_fill(backend, shape, cmd->as.fill_path.paint, bounds);
    return schultz_tvg_finish(backend, shape);
}

static int32_t schultz_tvg_stroke_path(void *context,
                                       const schultz_draw_cmd *cmd)
{
    schultz_thorvg *backend = (schultz_thorvg *)context;
    schultz_rect bounds;
    Tvg_Paint shape = tvg_shape_new();

    if (shape == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    schultz_tvg_path_onto(backend, shape, cmd->as.stroke_path.steps,
                          cmd->as.stroke_path.step_count,
                          cmd->as.stroke_path.points, &bounds);
    schultz_tvg_apply_stroke(backend, shape, cmd->as.stroke_path.stroke,
                             bounds);
    return schultz_tvg_finish(backend, shape);
}

static const schultz_painter_vtable schultz_thorvg_vtable = {
    schultz_tvg_begin,
    schultz_tvg_end,
    schultz_tvg_fill_rect,
    schultz_tvg_stroke_rect,
    schultz_tvg_fill_round_rect,
    schultz_tvg_stroke_round_rect,
    schultz_tvg_fill_ellipse,
    schultz_tvg_stroke_ellipse,
    schultz_tvg_line,
    schultz_tvg_fill_polygon,
    schultz_tvg_stroke_polygon,
    schultz_tvg_fill_path,
    schultz_tvg_stroke_path,
    schultz_tvg_image,
    schultz_tvg_glyph_run,
    schultz_tvg_glyph_outline_run,
    schultz_tvg_clip_begin,
    schultz_tvg_clip_end,
    schultz_tvg_offset_begin,
    schultz_tvg_offset_end,
    schultz_tvg_rotation_begin,
    schultz_tvg_rotation_end,
    schultz_tvg_group_begin,
    schultz_tvg_group_end,
    schultz_tvg_device_pixels_begin,
    schultz_tvg_device_pixels_end
};

int32_t schultz_thorvg_painter(schultz_thorvg *backend,
                               schultz_painter *out_painter)
{
    if (backend == NULL || out_painter == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    out_painter->vtable  = &schultz_thorvg_vtable;
    out_painter->context = backend;
    return SCHULTZ_OK;
}
