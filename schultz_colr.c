/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_colr.c
 * @brief Drawing a colour glyph that the font describes as a picture.
 *
 * The description is a tree. A node is one of: a stack of layers, a shape to
 * fill, a colour or a gradient to fill it with, a movement or a scaling to
 * apply to what is under it, or two subtrees combined. Walking the tree
 * builds the same shapes and scenes the painter builds for anything else,
 * and the result is rasterized once and kept, because a glyph is drawn far
 * more often than it changes.
 *
 * Everything below the root is in the font's own units with the y axis
 * pointing up, which is what FreeType hands over when the root transform is
 * left out. One matrix at the end turns that into pixels with the y axis
 * pointing down, so the arithmetic in between stays in the units the font
 * was drawn in.
 */

#include "schultz_colr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include FT_COLOR_H
#include FT_OUTLINE_H

#include <thorvg_capi.h>

/*
 * How deep a drawing may nest before this gives up. The deepest in the face
 * that prompted this was ten; the limit is here so a font that refers to
 * itself in a circle stops rather than running out of stack.
 */
#define SCHULTZ_COLR_DEPTH 48u

/* How many colour stops one gradient may have. The most in that same face
 * was thirteen. */
#define SCHULTZ_COLR_STOPS 64u

/** @brief What the walk needs to hand down to itself. */
typedef struct {
    FT_Face   face;      /**< The face being drawn from. */
    FT_Color *palette;   /**< The colours the drawing refers to by number. */
    uint16_t  colors;    /**< How many of them there are. */
    /*
     * The area the drawing is allowed to reach, in the font's own units. A
     * colour or a gradient can appear with no shape under it, which means
     * "everywhere", and everywhere is this.
     */
    float     left;
    float     bottom;
    float     right;
    float     top;
} schultz_colr_walk;

static Tvg_Paint schultz_colr_build(schultz_colr_walk *walk,
                                    FT_OpaquePaint node, uint32_t depth);

/* ------------------------------------------------------------- outlines */

/*
 * FreeType reports an outline by calling back as it walks it, which is the
 * same shape as building a path. The only conversion is the curve: a
 * TrueType outline bends with one control point and a path takes two, and
 * the two thirds rule below is the exact equivalent rather than an
 * approximation.
 */
/*
 * The path being built, and where the pen is. The pen is tracked here
 * because turning a one control point curve into a two control point one
 * needs to know where the curve starts, and a path does not report that
 * back.
 */
typedef struct {
    Tvg_Paint shape;
    float     x;
    float     y;
} schultz_colr_pen;

static int schultz_colr_move(const FT_Vector *to, void *context)
{
    schultz_colr_pen *pen = (schultz_colr_pen *)context;

    tvg_shape_close(pen->shape);
    pen->x = (float)to->x;
    pen->y = (float)to->y;
    tvg_shape_move_to(pen->shape, pen->x, pen->y);
    return 0;
}

static int schultz_colr_line(const FT_Vector *to, void *context)
{
    schultz_colr_pen *pen = (schultz_colr_pen *)context;

    pen->x = (float)to->x;
    pen->y = (float)to->y;
    tvg_shape_line_to(pen->shape, pen->x, pen->y);
    return 0;
}

static int schultz_colr_conic(const FT_Vector *control, const FT_Vector *to,
                              void *context)
{
    schultz_colr_pen *pen = (schultz_colr_pen *)context;
    float px = (float)control->x;
    float py = (float)control->y;
    float tx = (float)to->x;
    float ty = (float)to->y;
    float cx1 = pen->x + (2.0f / 3.0f) * (px - pen->x);
    float cy1 = pen->y + (2.0f / 3.0f) * (py - pen->y);
    float cx2 = tx + (2.0f / 3.0f) * (px - tx);
    float cy2 = ty + (2.0f / 3.0f) * (py - ty);

    tvg_shape_cubic_to(pen->shape, cx1, cy1, cx2, cy2, tx, ty);
    pen->x = tx;
    pen->y = ty;
    return 0;
}

static int schultz_colr_cubic(const FT_Vector *c1, const FT_Vector *c2,
                              const FT_Vector *to, void *context)
{
    schultz_colr_pen *pen = (schultz_colr_pen *)context;

    tvg_shape_cubic_to(pen->shape, (float)c1->x, (float)c1->y,
                       (float)c2->x, (float)c2->y, (float)to->x,
                       (float)to->y);
    pen->x = (float)to->x;
    pen->y = (float)to->y;
    return 0;
}

/* The outline of one glyph as a shape, in the font's own units. */
static Tvg_Paint schultz_colr_outline(FT_Face face, FT_UInt glyph)
{
    static const FT_Outline_Funcs funcs = {
        schultz_colr_move, schultz_colr_line, schultz_colr_conic,
        schultz_colr_cubic, 0, 0
    };
    schultz_colr_pen pen;

    /*
     * Unscaled, because the one matrix at the end does the scaling. Asking
     * for it at size here and scaling again there would apply it twice.
     */
    if (FT_Load_Glyph(face, glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING)
            != 0) {
        return NULL;
    }
    if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        return NULL;
    }
    pen.shape = tvg_shape_new();
    pen.x = 0.0f;
    pen.y = 0.0f;
    if (pen.shape == NULL) {
        return NULL;
    }
    if (FT_Outline_Decompose(&face->glyph->outline, &funcs, &pen) != 0) {
        tvg_paint_unref(pen.shape, true);
        return NULL;
    }
    tvg_shape_close(pen.shape);
    /*
     * A glyph's contours wind so that the outer one and the holes inside it
     * run opposite ways, which is what the non-zero rule reads to leave the
     * holes empty.
     */
    tvg_shape_set_fill_rule(pen.shape, TVG_FILL_RULE_NON_ZERO);
    return pen.shape;
}

void *schultz_colr_glyph_outline(FT_Face face, FT_UInt glyph)
{
    return (face == NULL) ? NULL : (void *)schultz_colr_outline(face, glyph);
}

/* --------------------------------------------------------------- colour */

/* One colour by the number the drawing refers to it with, times the alpha
 * that reference carries. The last entry stands for "whatever the text
 * colour is", which for an emoji is nothing sensible, so it comes out black
 * the way a foreground colour would if none were set. */
static void schultz_colr_color(const schultz_colr_walk *walk,
                               FT_ColorIndex index, uint8_t *out_rgba)
{
    FT_Color entry;
    float alpha = (float)index.alpha / 16384.0f;

    if (index.palette_index < walk->colors && walk->palette != NULL) {
        entry = walk->palette[index.palette_index];
    } else {
        entry.red = 0u;
        entry.green = 0u;
        entry.blue = 0u;
        entry.alpha = 255u;
    }
    if (alpha < 0.0f) { alpha = 0.0f; }
    if (alpha > 1.0f) { alpha = 1.0f; }
    out_rgba[0] = entry.red;
    out_rgba[1] = entry.green;
    out_rgba[2] = entry.blue;
    out_rgba[3] = (uint8_t)((float)entry.alpha * alpha + 0.5f);
}

/* The stops along a gradient, in the order the drawing gives them. */
static uint32_t schultz_colr_stops(schultz_colr_walk *walk,
                                   FT_ColorStopIterator iterator,
                                   Tvg_Color_Stop *out_stops)
{
    FT_ColorStop stop;
    uint32_t count = 0u;

    while (count < SCHULTZ_COLR_STOPS &&
           FT_Get_Colorline_Stops(walk->face, &stop, &iterator)) {
        uint8_t rgba[4];

        schultz_colr_color(walk, stop.color, rgba);
        out_stops[count].offset = (float)stop.stop_offset / 65536.0f;
        out_stops[count].r = rgba[0];
        out_stops[count].g = rgba[1];
        out_stops[count].b = rgba[2];
        out_stops[count].a = rgba[3];
        count++;
    }
    return count;
}

static Tvg_Stroke_Fill schultz_colr_spread(FT_PaintExtend extend)
{
    switch (extend) {
    case FT_COLR_PAINT_EXTEND_REPEAT:  return TVG_STROKE_FILL_REPEAT;
    case FT_COLR_PAINT_EXTEND_REFLECT: return TVG_STROKE_FILL_REFLECT;
    default:                           return TVG_STROKE_FILL_PAD;
    }
}

/*
 * A gradient runs from one point to another, and a third point says which
 * way "across" is, so that it can be sheared. A path's gradient has only the
 * two, so the run is projected onto the line at right angles to the third:
 * the same colours in the same places, with the shear folded into where the
 * end point lands.
 */
static Tvg_Gradient schultz_colr_linear(schultz_colr_walk *walk,
                                        const FT_PaintLinearGradient *from)
{
    Tvg_Color_Stop stops[SCHULTZ_COLR_STOPS];
    Tvg_Gradient gradient;
    uint32_t count;
    float x0 = (float)from->p0.x;
    float y0 = (float)from->p0.y;
    float x1 = (float)from->p1.x;
    float y1 = (float)from->p1.y;
    float rx = (float)from->p2.x - x0;
    float ry = (float)from->p2.y - y0;
    float dx = x1 - x0;
    float dy = y1 - y0;
    float across = rx * rx + ry * ry;

    if (across > 0.0f) {
        float along = (dx * rx + dy * ry) / across;

        x1 -= along * rx;
        y1 -= along * ry;
    }

    count = schultz_colr_stops(walk, from->colorline.color_stop_iterator,
                               stops);
    if (count == 0u) {
        return NULL;
    }
    gradient = tvg_linear_gradient_new();
    if (gradient == NULL) {
        return NULL;
    }
    tvg_linear_gradient_set(gradient, x0, y0, x1, y1);
    tvg_gradient_set_spread(gradient, schultz_colr_spread(
                                          from->colorline.extend));
    tvg_gradient_set_color_stops(gradient, stops, count);
    return gradient;
}

/*
 * Two circles, with the colours running from the first to the second. That
 * is the same thing a path's radial gradient describes with a centre, a
 * radius, and a focus with a radius of its own, so the two map across
 * directly.
 */
static Tvg_Gradient schultz_colr_radial(schultz_colr_walk *walk,
                                        const FT_PaintRadialGradient *from)
{
    Tvg_Color_Stop stops[SCHULTZ_COLR_STOPS];
    Tvg_Gradient gradient;
    uint32_t count = schultz_colr_stops(
        walk, from->colorline.color_stop_iterator, stops);

    if (count == 0u) {
        return NULL;
    }
    gradient = tvg_radial_gradient_new();
    if (gradient == NULL) {
        return NULL;
    }
    tvg_radial_gradient_set(gradient, (float)from->c1.x, (float)from->c1.y,
                            (float)from->r1, (float)from->c0.x,
                            (float)from->c0.y, (float)from->r0);
    tvg_gradient_set_spread(gradient, schultz_colr_spread(
                                          from->colorline.extend));
    tvg_gradient_set_color_stops(gradient, stops, count);
    return gradient;
}

/* Multiplies b into a, both being the plain two by three kind. */
static void schultz_colr_times(Tvg_Matrix *a, const Tvg_Matrix *b)
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
 * Sees past the movements and scalings that sit between a shape and the
 * colour going into it, collecting them as it goes.
 *
 * A transform above a gradient moves the gradient, not the shape: the shape
 * is already where it belongs and what is being placed is where the colours
 * run from and to. So the transforms are gathered here and handed to the
 * gradient, rather than being applied to anything.
 */
static int32_t schultz_colr_through(schultz_colr_walk *walk,
                                    FT_OpaquePaint node, Tvg_Matrix *matrix,
                                    FT_COLR_Paint *out_paint)
{
    uint32_t depth = 0u;

    while (depth < SCHULTZ_COLR_DEPTH) {
        FT_COLR_Paint paint;
        Tvg_Matrix step;
        float cx;
        float cy;
        float e11 = 1.0f;
        float e12 = 0.0f;
        float e21 = 0.0f;
        float e22 = 1.0f;

        if (!FT_Get_Paint(walk->face, node, &paint)) {
            return 0;
        }
        switch (paint.format) {
        case FT_COLR_PAINTFORMAT_TRANSFORM:
            step.e11 = (float)paint.u.transform.affine.xx / 65536.0f;
            step.e21 = (float)paint.u.transform.affine.yx / 65536.0f;
            step.e12 = (float)paint.u.transform.affine.xy / 65536.0f;
            step.e22 = (float)paint.u.transform.affine.yy / 65536.0f;
            step.e13 = (float)paint.u.transform.affine.dx / 65536.0f;
            step.e23 = (float)paint.u.transform.affine.dy / 65536.0f;
            step.e31 = 0.0f;
            step.e32 = 0.0f;
            step.e33 = 1.0f;
            schultz_colr_times(matrix, &step);
            node = paint.u.transform.paint;
            break;

        case FT_COLR_PAINTFORMAT_TRANSLATE:
            step.e11 = 1.0f; step.e12 = 0.0f;
            step.e13 = (float)paint.u.translate.dx / 65536.0f;
            step.e21 = 0.0f; step.e22 = 1.0f;
            step.e23 = (float)paint.u.translate.dy / 65536.0f;
            step.e31 = 0.0f; step.e32 = 0.0f; step.e33 = 1.0f;
            schultz_colr_times(matrix, &step);
            node = paint.u.translate.paint;
            break;

        case FT_COLR_PAINTFORMAT_SCALE:
        case FT_COLR_PAINTFORMAT_ROTATE:
        case FT_COLR_PAINTFORMAT_SKEW:
            if (paint.format == FT_COLR_PAINTFORMAT_SCALE) {
                e11 = (float)paint.u.scale.scale_x / 65536.0f;
                e22 = (float)paint.u.scale.scale_y / 65536.0f;
                cx = (float)paint.u.scale.center_x / 65536.0f;
                cy = (float)paint.u.scale.center_y / 65536.0f;
                node = paint.u.scale.paint;
            } else if (paint.format == FT_COLR_PAINTFORMAT_ROTATE) {
                float turns = (float)paint.u.rotate.angle / 65536.0f;
                float radians = turns * 3.14159265358979f;

                e11 = cosf(radians);
                e12 = -sinf(radians);
                e21 = sinf(radians);
                e22 = cosf(radians);
                cx = (float)paint.u.rotate.center_x / 65536.0f;
                cy = (float)paint.u.rotate.center_y / 65536.0f;
                node = paint.u.rotate.paint;
            } else {
                e12 = -tanf((float)paint.u.skew.x_skew_angle / 65536.0f
                            * 3.14159265358979f);
                e21 = tanf((float)paint.u.skew.y_skew_angle / 65536.0f
                           * 3.14159265358979f);
                cx = (float)paint.u.skew.center_x / 65536.0f;
                cy = (float)paint.u.skew.center_y / 65536.0f;
                node = paint.u.skew.paint;
            }
            step.e11 = e11; step.e12 = e12;
            step.e21 = e21; step.e22 = e22;
            step.e13 = cx - (e11 * cx + e12 * cy);
            step.e23 = cy - (e21 * cx + e22 * cy);
            step.e31 = 0.0f; step.e32 = 0.0f; step.e33 = 1.0f;
            schultz_colr_times(matrix, &step);
            break;

        default:
            *out_paint = paint;
            return 1;
        }
        depth++;
    }
    return 0;
}

/*
 * Fills a shape straight away when what goes in it is a colour or a
 * gradient, which is the ordinary case and the cheap one. Anything else is
 * a drawing in its own right and has to be built and then clipped, which the
 * caller does.
 */
static int32_t schultz_colr_fill(schultz_colr_walk *walk, Tvg_Paint shape,
                                 FT_OpaquePaint node)
{
    FT_COLR_Paint paint;
    Tvg_Matrix matrix;
    Tvg_Gradient gradient = NULL;

    matrix.e11 = 1.0f; matrix.e12 = 0.0f; matrix.e13 = 0.0f;
    matrix.e21 = 0.0f; matrix.e22 = 1.0f; matrix.e23 = 0.0f;
    matrix.e31 = 0.0f; matrix.e32 = 0.0f; matrix.e33 = 1.0f;

    if (!schultz_colr_through(walk, node, &matrix, &paint)) {
        return 0;
    }
    if (paint.format == FT_COLR_PAINTFORMAT_LINEAR_GRADIENT) {
        gradient = schultz_colr_linear(walk, &paint.u.linear_gradient);
    } else if (paint.format == FT_COLR_PAINTFORMAT_RADIAL_GRADIENT) {
        gradient = schultz_colr_radial(walk, &paint.u.radial_gradient);
    }
    if (gradient != NULL) {
        tvg_gradient_set_transform(gradient, &matrix);
        tvg_shape_set_gradient(shape, gradient);
        return 1;
    }
    if (paint.format == FT_COLR_PAINTFORMAT_SOLID) {
        uint8_t rgba[4];

        schultz_colr_color(walk, paint.u.solid.color, rgba);
        tvg_shape_set_fill_color(shape, rgba[0], rgba[1], rgba[2], rgba[3]);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------- the walk */

/* Applies a matrix on top of whatever the node already carries. */
static void schultz_colr_apply(Tvg_Paint paint, const Tvg_Matrix *matrix)
{
    Tvg_Matrix current;
    Tvg_Matrix combined;

    if (tvg_paint_get_transform(paint, &current) != TVG_RESULT_SUCCESS) {
        tvg_paint_set_transform(paint, matrix);
        return;
    }
    combined.e11 = matrix->e11 * current.e11 + matrix->e12 * current.e21;
    combined.e12 = matrix->e11 * current.e12 + matrix->e12 * current.e22;
    combined.e13 = matrix->e11 * current.e13 + matrix->e12 * current.e23
                   + matrix->e13;
    combined.e21 = matrix->e21 * current.e11 + matrix->e22 * current.e21;
    combined.e22 = matrix->e21 * current.e12 + matrix->e22 * current.e22;
    combined.e23 = matrix->e21 * current.e13 + matrix->e22 * current.e23
                   + matrix->e23;
    combined.e31 = 0.0f;
    combined.e32 = 0.0f;
    combined.e33 = 1.0f;
    tvg_paint_set_transform(paint, &combined);
}

/* A movement, a scaling or a shear, each of which is a matrix around a
 * centre that may not be the origin. */
static Tvg_Paint schultz_colr_moved(schultz_colr_walk *walk,
                                    FT_OpaquePaint child, uint32_t depth,
                                    float e11, float e12, float e21,
                                    float e22, float cx, float cy)
{
    Tvg_Paint paint = schultz_colr_build(walk, child, depth + 1u);
    Tvg_Matrix matrix;

    if (paint == NULL) {
        return NULL;
    }
    matrix.e11 = e11;
    matrix.e12 = e12;
    matrix.e21 = e21;
    matrix.e22 = e22;
    /* Around the centre: move it to the origin, transform, move it back. */
    matrix.e13 = cx - (e11 * cx + e12 * cy);
    matrix.e23 = cy - (e21 * cx + e22 * cy);
    matrix.e31 = 0.0f;
    matrix.e32 = 0.0f;
    matrix.e33 = 1.0f;
    schultz_colr_apply(paint, &matrix);
    return paint;
}

static Tvg_Paint schultz_colr_build(schultz_colr_walk *walk,
                                    FT_OpaquePaint node, uint32_t depth)
{
    FT_COLR_Paint paint;

    if (depth > SCHULTZ_COLR_DEPTH) {
        return NULL;
    }
    if (!FT_Get_Paint(walk->face, node, &paint)) {
        return NULL;
    }

    switch (paint.format) {
    case FT_COLR_PAINTFORMAT_COLR_LAYERS: {
        Tvg_Paint scene = tvg_scene_new();
        FT_OpaquePaint child;
        uint32_t added = 0u;

        if (scene == NULL) {
            return NULL;
        }
        child.p = NULL;
        while (FT_Get_Paint_Layers(walk->face,
                                   &paint.u.colr_layers.layer_iterator,
                                   &child)) {
            Tvg_Paint layer = schultz_colr_build(walk, child, depth + 1u);

            if (layer != NULL) {
                tvg_scene_add(scene, layer);
                added++;
            }
            child.p = NULL;
        }
        if (added == 0u) {
            tvg_paint_unref(scene, true);
            return NULL;
        }
        return scene;
    }

    case FT_COLR_PAINTFORMAT_GLYPH: {
        /*
         * A shape, and what goes inside it. When that is a colour or a
         * gradient the shape is simply filled. When it is a drawing, the
         * shape becomes the edge the drawing is cut to.
         */
        Tvg_Paint shape = schultz_colr_outline(walk->face,
                                               paint.u.glyph.glyphID);
        Tvg_Paint inside;

        if (shape == NULL) {
            return NULL;
        }
        if (schultz_colr_fill(walk, shape, paint.u.glyph.paint)) {
            return shape;
        }
        inside = schultz_colr_build(walk, paint.u.glyph.paint, depth + 1u);
        if (inside == NULL) {
            tvg_paint_unref(shape, true);
            return NULL;
        }
        tvg_paint_set_clip(inside, shape);
        return inside;
    }

    case FT_COLR_PAINTFORMAT_COLR_GLYPH: {
        /* Another glyph's drawing, used whole. No root transform this time:
         * the one the caller applies already covers it. */
        FT_OpaquePaint other;

        other.p = NULL;
        if (!FT_Get_Color_Glyph_Paint(walk->face, paint.u.colr_glyph.glyphID,
                                      FT_COLOR_NO_ROOT_TRANSFORM, &other)) {
            return NULL;
        }
        return schultz_colr_build(walk, other, depth + 1u);
    }

    case FT_COLR_PAINTFORMAT_TRANSFORM: {
        Tvg_Paint child = schultz_colr_build(walk, paint.u.transform.paint,
                                             depth + 1u);
        Tvg_Matrix matrix;

        if (child == NULL) {
            return NULL;
        }
        matrix.e11 = (float)paint.u.transform.affine.xx / 65536.0f;
        matrix.e21 = (float)paint.u.transform.affine.yx / 65536.0f;
        matrix.e12 = (float)paint.u.transform.affine.xy / 65536.0f;
        matrix.e22 = (float)paint.u.transform.affine.yy / 65536.0f;
        matrix.e13 = (float)paint.u.transform.affine.dx / 65536.0f;
        matrix.e23 = (float)paint.u.transform.affine.dy / 65536.0f;
        matrix.e31 = 0.0f;
        matrix.e32 = 0.0f;
        matrix.e33 = 1.0f;
        schultz_colr_apply(child, &matrix);
        return child;
    }

    case FT_COLR_PAINTFORMAT_TRANSLATE: {
        Tvg_Paint child = schultz_colr_build(walk, paint.u.translate.paint,
                                             depth + 1u);
        Tvg_Matrix matrix;

        if (child == NULL) {
            return NULL;
        }
        matrix.e11 = 1.0f;
        matrix.e12 = 0.0f;
        matrix.e13 = (float)paint.u.translate.dx / 65536.0f;
        matrix.e21 = 0.0f;
        matrix.e22 = 1.0f;
        matrix.e23 = (float)paint.u.translate.dy / 65536.0f;
        matrix.e31 = 0.0f;
        matrix.e32 = 0.0f;
        matrix.e33 = 1.0f;
        schultz_colr_apply(child, &matrix);
        return child;
    }

    case FT_COLR_PAINTFORMAT_ROTATE: {
        /* The angle arrives as degrees over a hundred and eighty, which is
         * what the format stores. */
        float turns = (float)paint.u.rotate.angle / 65536.0f;
        float radians = turns * 3.14159265358979f;
        float c = cosf(radians);
        float s = sinf(radians);

        return schultz_colr_moved(walk, paint.u.rotate.paint, depth,
            c, -s, s, c,
            (float)paint.u.rotate.center_x / 65536.0f,
            (float)paint.u.rotate.center_y / 65536.0f);
    }

    case FT_COLR_PAINTFORMAT_SKEW: {
        float ax = (float)paint.u.skew.x_skew_angle / 65536.0f
                   * 3.14159265358979f;
        float ay = (float)paint.u.skew.y_skew_angle / 65536.0f
                   * 3.14159265358979f;

        return schultz_colr_moved(walk, paint.u.skew.paint, depth,
            1.0f, -tanf(ax), tanf(ay), 1.0f,
            (float)paint.u.skew.center_x / 65536.0f,
            (float)paint.u.skew.center_y / 65536.0f);
    }

    case FT_COLR_PAINTFORMAT_SCALE:
        return schultz_colr_moved(walk, paint.u.scale.paint, depth,
            (float)paint.u.scale.scale_x / 65536.0f, 0.0f, 0.0f,
            (float)paint.u.scale.scale_y / 65536.0f,
            (float)paint.u.scale.center_x / 65536.0f,
            (float)paint.u.scale.center_y / 65536.0f);

    case FT_COLR_PAINTFORMAT_COMPOSITE: {
        /*
         * Two drawings put together. Only two ways of doing it appear in the
         * faces this was written for: keeping the source where the backdrop
         * is solid, and laying the source over the backdrop with a soft
         * light. The first is a mask and the second is a blend, and both are
         * things the vector engine already does.
         */
        Tvg_Paint source = schultz_colr_build(walk,
            paint.u.composite.source_paint, depth + 1u);
        Tvg_Paint backdrop = schultz_colr_build(walk,
            paint.u.composite.backdrop_paint, depth + 1u);
        Tvg_Paint scene;

        if (source == NULL) {
            if (backdrop != NULL) {
                tvg_paint_unref(backdrop, true);
            }
            return NULL;
        }
        if (backdrop == NULL) {
            return source;
        }
        if (paint.u.composite.composite_mode == FT_COLR_COMPOSITE_SRC_IN) {
            tvg_paint_set_mask_method(source, backdrop,
                                      TVG_MASK_METHOD_ALPHA);
            return source;
        }
        scene = tvg_scene_new();
        if (scene == NULL) {
            tvg_paint_unref(source, true);
            tvg_paint_unref(backdrop, true);
            return NULL;
        }
        if (paint.u.composite.composite_mode
                == FT_COLR_COMPOSITE_SOFT_LIGHT) {
            tvg_paint_set_blend_method(source, TVG_BLEND_METHOD_SOFTLIGHT);
        }
        tvg_scene_add(scene, backdrop);
        tvg_scene_add(scene, source);
        return scene;
    }

    case FT_COLR_PAINTFORMAT_SOLID:
    case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
    case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT: {
        /*
         * A colour or a gradient with no shape under it covers everything
         * the drawing is allowed to reach. It turns up as one side of a
         * pair being combined, where the other side is what trims it back
         * to a shape: a flag is a gradient cut to the outline beside it.
         */
        Tvg_Paint shape = tvg_shape_new();

        if (shape == NULL) {
            return NULL;
        }
        tvg_shape_append_rect(shape, walk->left, walk->bottom,
                              walk->right - walk->left,
                              walk->top - walk->bottom, 0.0f, 0.0f, false);
        if (!schultz_colr_fill(walk, shape, node)) {
            tvg_paint_unref(shape, true);
            return NULL;
        }
        return shape;
    }

    default:
        /* A kind of node this does not know. Drawing nothing for it loses
         * one layer rather than the whole glyph. */
        return NULL;
    }
}

/* ------------------------------------------------------------- the face */

int32_t schultz_colr_has_drawing(FT_Face face, FT_UInt glyph)
{
    FT_OpaquePaint root;

    if (face == NULL || !FT_HAS_COLOR(face)) {
        return 0;
    }
    root.p = NULL;
    return FT_Get_Color_Glyph_Paint(face, glyph,
                                    FT_COLOR_INCLUDE_ROOT_TRANSFORM,
                                    &root) ? 1 : 0;
}

int32_t schultz_colr_render(FT_Face face, FT_UInt glyph,
                            unsigned char **out_pixels, uint32_t *out_width,
                            uint32_t *out_height, int32_t *out_left,
                            int32_t *out_top)
{
    schultz_colr_walk walk;
    FT_ClipBox box;
    FT_OpaquePaint root;
    FT_Palette_Data data;
    Tvg_Paint drawing;
    Tvg_Canvas canvas;
    Tvg_Matrix place;
    uint32_t *pixels;
    unsigned char *bytes;
    float scale;
    float left;
    float top;
    float right;
    float bottom;
    uint32_t width;
    uint32_t height;
    uint32_t i;
    int32_t result = SCHULTZ_ERR_OUT_OF_MEMORY;

    if (face == NULL || out_pixels == NULL || out_width == NULL ||
        out_height == NULL || out_left == NULL || out_top == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    root.p = NULL;
    if (!FT_Get_Color_Glyph_Paint(face, glyph, FT_COLOR_NO_ROOT_TRANSFORM,
                                  &root)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * Where on the page the drawing is allowed to reach. The font states it
     * per glyph, already in pixels at the size in use.
     */
    if (!FT_Get_Color_Glyph_ClipBox(face, glyph, &box)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    left   = (float)box.bottom_left.x / 64.0f;
    right  = (float)box.top_right.x / 64.0f;
    bottom = (float)box.bottom_left.y / 64.0f;
    top    = (float)box.top_right.y / 64.0f;
    if (right <= left || top <= bottom) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    width  = (uint32_t)(right - left + 0.5f);
    height = (uint32_t)(top - bottom + 0.5f);
    if (width == 0u || height == 0u || width > 4096u || height > 4096u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    walk.face = face;
    walk.palette = NULL;
    walk.colors = 0u;
    /*
     * The same box in the units the drawing is described in, for a fill that
     * has no shape of its own.
     */
    {
        float units = (face->size->metrics.x_ppem > 0)
                          ? ((float)face->units_per_EM
                             / (float)face->size->metrics.x_ppem)
                          : 1.0f;

        walk.left   = left * units;
        walk.right  = right * units;
        walk.bottom = bottom * units;
        walk.top    = top * units;
    }
    if (FT_Palette_Data_Get(face, &data) == 0 && data.num_palettes > 0u) {
        if (FT_Palette_Select(face, 0, &walk.palette) == 0) {
            walk.colors = data.num_palette_entries;
        } else {
            walk.palette = NULL;
        }
    }

    /*
     * The engine counts how many times it has been started, so starting it
     * here costs nothing when a window already has it running and works when
     * nothing else does.
     */
    if (tvg_engine_init(0) != TVG_RESULT_SUCCESS) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    pixels = (uint32_t *)calloc((size_t)width * height, sizeof(*pixels));
    canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_NONE);
    drawing = schultz_colr_build(&walk, root, 0u);
    if (pixels == NULL || canvas == NULL || drawing == NULL) {
        goto done;
    }
    if (tvg_swcanvas_set_target(canvas, pixels, width, width, height,
                                TVG_COLORSPACE_ARGB8888)
            != TVG_RESULT_SUCCESS) {
        goto done;
    }

    /*
     * From the font's units to this buffer's pixels. The y axis turns over
     * on the way, because a font measures upward from the baseline and a
     * buffer counts rows downward from the top.
     */
    scale = (face->units_per_EM > 0)
                ? ((float)face->size->metrics.x_ppem
                   / (float)face->units_per_EM)
                : 1.0f;
    place.e11 = scale;
    place.e12 = 0.0f;
    place.e13 = -left;
    place.e21 = 0.0f;
    place.e22 = -scale;
    place.e23 = top;
    place.e31 = 0.0f;
    place.e32 = 0.0f;
    place.e33 = 1.0f;
    schultz_colr_apply(drawing, &place);

    if (tvg_canvas_add(canvas, drawing) != TVG_RESULT_SUCCESS) {
        goto done;
    }
    drawing = NULL;  /* the canvas owns it now */
    if (tvg_canvas_draw(canvas, true) != TVG_RESULT_SUCCESS ||
        tvg_canvas_sync(canvas) != TVG_RESULT_SUCCESS) {
        goto done;
    }

    /*
     * The engine writes alpha, red, green, blue with the colours already
     * multiplied by the alpha. What the glyph cache keeps is blue, green,
     * red, alpha with the same multiplication, which is what FreeType
     * produces for the other colour formats, so only the order changes.
     */
    bytes = (unsigned char *)malloc((size_t)width * height * 4u);
    if (bytes == NULL) {
        goto done;
    }
    for (i = 0u; i < width * height; i++) {
        uint32_t p = pixels[i];

        bytes[i * 4u + 0u] = (unsigned char)(p & 0xffu);
        bytes[i * 4u + 1u] = (unsigned char)((p >> 8) & 0xffu);
        bytes[i * 4u + 2u] = (unsigned char)((p >> 16) & 0xffu);
        bytes[i * 4u + 3u] = (unsigned char)((p >> 24) & 0xffu);
    }
    *out_pixels = bytes;
    *out_width  = width;
    *out_height = height;
    *out_left   = (int32_t)(left < 0.0f ? left - 0.5f : left + 0.5f);
    *out_top    = (int32_t)(top < 0.0f ? top - 0.5f : top + 0.5f);
    result = SCHULTZ_OK;

done:
    if (drawing != NULL) {
        tvg_paint_unref(drawing, true);
    }
    if (canvas != NULL) {
        tvg_canvas_destroy(canvas);
    }
    free(pixels);
    tvg_engine_term();
    return result;
}
