/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * schultz_geom.c - geometry and color primitives.
 */

#include "schultz_geom.h"

#include <math.h>
#include <stddef.h>

schultz_point schultz_point_make(float x, float y)
{
    schultz_point point;
    point.x = x;
    point.y = y;
    return point;
}

schultz_size schultz_size_make(float width, float height)
{
    schultz_size size;
    size.width  = width;
    size.height = height;
    return size;
}

schultz_rect schultz_rect_make(float x, float y, float width, float height)
{
    schultz_rect rect;
    rect.x      = x;
    rect.y      = y;
    rect.width  = width;
    rect.height = height;
    return rect;
}

schultz_color schultz_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    schultz_color color;
    color.r = r;
    color.g = g;
    color.b = b;
    color.a = a;
    return color;
}

schultz_rect schultz_rect_pixel_bounds(schultz_rect rect, float slack)
{
    float left;
    float top;
    float right;
    float bottom;

    if (schultz_rect_is_empty(rect)) {
        return schultz_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
    }

    left   = floorf(rect.x - slack);
    top    = floorf(rect.y - slack);
    right  = ceilf(schultz_rect_right(rect) + slack);
    bottom = ceilf(schultz_rect_bottom(rect) + slack);

    return schultz_rect_make(left, top, right - left, bottom - top);
}

float schultz_rect_right(schultz_rect rect)
{
    return rect.x + rect.width;
}

float schultz_rect_bottom(schultz_rect rect)
{
    return rect.y + rect.height;
}

int32_t schultz_rect_is_empty(schultz_rect rect)
{
    return (rect.width > 0.0f && rect.height > 0.0f) ? 0 : 1;
}

int32_t schultz_rect_equals(schultz_rect a, schultz_rect b)
{
    return (a.x == b.x && a.y == b.y &&
            a.width == b.width && a.height == b.height) ? 1 : 0;
}

schultz_rect schultz_rect_expand(schultz_rect rect, float amount)
{
    schultz_rect grown = schultz_rect_make(rect.x - amount, rect.y - amount,
                                           rect.width + amount * 2.0f,
                                           rect.height + amount * 2.0f);

    if (grown.width < 0.0f) {
        grown.x    += grown.width * 0.5f;
        grown.width = 0.0f;
    }
    if (grown.height < 0.0f) {
        grown.y     += grown.height * 0.5f;
        grown.height = 0.0f;
    }
    return grown;
}

int32_t schultz_rect_contains_point(schultz_rect rect, schultz_point point)
{
    if (schultz_rect_is_empty(rect)) {
        return 0;
    }
    /* Left and top edges are inside, right and bottom are outside, so
     * adjacent rects do not both claim a point on their shared edge. */
    return (point.x >= rect.x && point.x < schultz_rect_right(rect) &&
            point.y >= rect.y && point.y < schultz_rect_bottom(rect)) ? 1 : 0;
}

schultz_rect schultz_rect_intersect(schultz_rect a, schultz_rect b)
{
    float left   = (a.x > b.x) ? a.x : b.x;
    float top    = (a.y > b.y) ? a.y : b.y;
    float right  = (schultz_rect_right(a) < schultz_rect_right(b))
                       ? schultz_rect_right(a) : schultz_rect_right(b);
    float bottom = (schultz_rect_bottom(a) < schultz_rect_bottom(b))
                       ? schultz_rect_bottom(a) : schultz_rect_bottom(b);

    if (right <= left || bottom <= top) {
        return schultz_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return schultz_rect_make(left, top, right - left, bottom - top);
}

/*
 * One pixel. Antialiased coverage reaches about that far past a shape's
 * geometry, and repainting slightly too much is invisible while repainting
 * slightly too little leaves the shape behind.
 */
float schultz_paint_slack(void)
{
    return 1.0f;
}

schultz_shadow schultz_shadow_none(void)
{
    schultz_shadow shadow;

    shadow.color    = schultz_color_rgba(0u, 0u, 0u, 0u);
    shadow.angle    = 180.0f;
    shadow.distance = 0.0f;
    shadow.blur     = 0.0f;
    return shadow;
}

schultz_paint schultz_paint_solid(schultz_color color)
{
    schultz_paint paint;

    paint.kind     = SCHULTZ_PAINT_SOLID;
    paint.as.color = color;
    return paint;
}

schultz_paint schultz_paint_gradient(schultz_handle gradient)
{
    schultz_paint paint;

    paint.kind        = SCHULTZ_PAINT_GRADIENT;
    paint.as.gradient = gradient;
    return paint;
}

schultz_stroke schultz_stroke_make(schultz_paint paint, float width)
{
    schultz_stroke stroke;

    stroke.paint = paint;
    stroke.width = width;
    stroke.dash  = SCHULTZ_HANDLE_NONE;
    stroke.cap   = SCHULTZ_CAP_BUTT;
    stroke.join  = SCHULTZ_JOIN_MITER;
    stroke.dash_offset = 0.0f;
    /* Four is what SVG starts from and what the rasterizer starts from. */
    stroke.miter_limit = 4.0f;
    return stroke;
}

schultz_stroke schultz_stroke_solid(schultz_color color, float width)
{
    return schultz_stroke_make(schultz_paint_solid(color), width);
}

int32_t schultz_paint_is_invisible(schultz_paint paint)
{
    return (paint.kind == SCHULTZ_PAINT_SOLID && paint.as.color.a == 0u)
               ? 1 : 0;
}

int32_t schultz_color_equals(schultz_color a, schultz_color b)
{
    return (a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a) ? 1 : 0;
}

uint32_t schultz_path_step_points(uint32_t step)
{
    switch (step) {
    case SCHULTZ_PATH_CLOSE: return 0u;
    case SCHULTZ_PATH_MOVE:  return 1u;
    case SCHULTZ_PATH_LINE:  return 1u;
    case SCHULTZ_PATH_CURVE: return 3u;
    case SCHULTZ_PATH_QUAD:  return 2u;
    default:                 return 0u;
    }
}

/*
 * One cubic along an ellipse, from `a0` to `a1` radians.
 *
 * A cubic cannot be an arc exactly, but over a quarter turn or less it is
 * close enough that the error is far below a pixel. The control points sit
 * along the tangents at each end, at a distance of
 *
 *     k = 4/3 * tan((a1 - a0) / 4)
 *
 * times the radius. That is the standard value, the one that makes the curve
 * meet the true arc at its middle as well as at its ends.
 */
static void schultz_arc_segment(schultz_point centre, float rx, float ry,
                                float a0, float a1, schultz_point *out_three)
{
    float k = 4.0f / 3.0f * tanf((a1 - a0) * 0.25f);
    float c0 = cosf(a0);
    float s0 = sinf(a0);
    float c1 = cosf(a1);
    float s1 = sinf(a1);

    out_three[0] = schultz_point_make(centre.x + rx * (c0 - k * s0),
                                      centre.y + ry * (s0 + k * c0));
    out_three[1] = schultz_point_make(centre.x + rx * (c1 + k * s1),
                                      centre.y + ry * (s1 - k * c1));
    out_three[2] = schultz_point_make(centre.x + rx * c1, centre.y + ry * s1);
}

int32_t schultz_arc_path(schultz_rect box, float start_degrees,
                         float sweep_degrees, uint32_t ends,
                         uint8_t *out_steps, uint32_t *out_step_count,
                         schultz_point *out_points, uint32_t *out_point_count)
{
    const float to_radians = 3.14159265358979f / 180.0f;
    schultz_point centre;
    float rx;
    float ry;
    float start;
    float sweep;
    float turned;
    uint32_t steps = 0u;
    uint32_t points = 0u;

    if (out_steps == NULL || out_step_count == NULL || out_points == NULL ||
        out_point_count == NULL || ends > SCHULTZ_ARC_CHORD ||
        schultz_rect_is_empty(box)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_step_count  = 0u;
    *out_point_count = 0u;

    /* A whole turn is as far as an arc can go; more would draw over itself. */
    sweep = sweep_degrees;
    if (sweep > 360.0f)  { sweep = 360.0f; }
    if (sweep < -360.0f) { sweep = -360.0f; }
    if (sweep == 0.0f) {
        return SCHULTZ_OK;
    }

    rx = box.width * 0.5f;
    ry = box.height * 0.5f;
    centre = schultz_point_make(box.x + rx, box.y + ry);
    start = start_degrees * to_radians;
    sweep = sweep * to_radians;

    out_steps[steps++] = (uint8_t)SCHULTZ_PATH_MOVE;
    out_points[points++] = schultz_point_make(centre.x + rx * cosf(start),
                                              centre.y + ry * sinf(start));

    /*
     * Split into quarters or less. Four segments cover a whole turn, which is
     * where SCHULTZ_ARC_STEPS_MAX comes from.
     */
    turned = 0.0f;
    while (turned != sweep) {
        float quarter = 3.14159265358979f * 0.5f;
        float left = sweep - turned;
        float piece = left;

        if (piece > quarter)  { piece = quarter; }
        if (piece < -quarter) { piece = -quarter; }

        schultz_arc_segment(centre, rx, ry, start + turned,
                            start + turned + piece, out_points + points);
        out_steps[steps++] = (uint8_t)SCHULTZ_PATH_CURVE;
        points += 3u;
        turned += piece;
    }

    /*
     * A pie goes home by way of the centre and a chord goes straight across,
     * which is the whole difference between the two. An open arc does
     * neither, so a stroke follows the curve and stops.
     */
    if (ends == (uint32_t)SCHULTZ_ARC_PIE) {
        out_steps[steps++] = (uint8_t)SCHULTZ_PATH_LINE;
        out_points[points++] = centre;
    }
    if (ends != (uint32_t)SCHULTZ_ARC_OPEN) {
        out_steps[steps++] = (uint8_t)SCHULTZ_PATH_CLOSE;
    }

    *out_step_count  = steps;
    *out_point_count = points;
    return SCHULTZ_OK;
}
