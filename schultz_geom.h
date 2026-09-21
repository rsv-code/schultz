/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_geom.h
 * @brief Geometry and color primitives.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 *
 * These types cross the host boundary by value, which is allowed for exactly
 * this shape of struct: small, and holding nothing but fixed width scalars. A
 * paint names its gradient by handle rather than by pointer for the same
 * reason. The rule, and why it was relaxed from forbidding by value at all,
 * is stated once in schultz.h.
 *
 * Coordinates are floats in pixels. Display scale is applied at render
 * time, so the same layout serves desktop high DPI and mobile densities.
 */

#ifndef SCHULTZ_GEOM_H
#define SCHULTZ_GEOM_H

#include "schultz.h"

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


/** @brief A position in pixels. */
typedef struct {
    float x; /**< Horizontal position, increasing to the right. */
    float y; /**< Vertical position, increasing downward. */
} schultz_point;

/** @brief An axis aligned rectangle in pixels. */
typedef struct {
    float x;      /**< Left edge. */
    float y;      /**< Top edge. */
    float width;  /**< Extent to the right of x. */
    float height; /**< Extent below y. */
} schultz_rect;

/** @brief A two dimensional extent in pixels. */
typedef struct {
    float width;  /**< Horizontal extent. */
    float height; /**< Vertical extent. */
} schultz_size;

/** @brief A non premultiplied RGBA color, 8 bits per channel. */
typedef struct {
    uint8_t r; /**< Red, 0 to 255. */
    uint8_t g; /**< Green, 0 to 255. */
    uint8_t b; /**< Blue, 0 to 255. */
    uint8_t a; /**< Alpha, 0 transparent to 255 opaque. */
} schultz_color;

/**
 * @brief Builds a point.
 *
 * @param x Horizontal position in pixels.
 * @param y Vertical position in pixels.
 * @return A point with the given coordinates.
 */
schultz_point schultz_point_make(float x, float y);

/**
 * @brief Builds a size.
 *
 * @param width  Horizontal extent in pixels.
 * @param height Vertical extent in pixels.
 * @return A size with the given extents. Negative values are stored as given
 *         and are treated as empty by schultz_rect_is_empty.
 */
schultz_size schultz_size_make(float width, float height);

/**
 * @brief Builds a rectangle.
 *
 * @param x      Left edge in pixels.
 * @param y      Top edge in pixels.
 * @param width  Extent to the right of x.
 * @param height Extent below y.
 * @return A rectangle with the given origin and extents.
 */
schultz_rect schultz_rect_make(float x, float y, float width, float height);

/**
 * @brief Builds a color from four channel values.
 *
 * @param r Red channel, 0 to 255.
 * @param g Green channel, 0 to 255.
 * @param b Blue channel, 0 to 255.
 * @param a Alpha channel, 0 transparent to 255 opaque.
 * @return A color with the given channels.
 */
schultz_color schultz_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/** @brief What a fill or a stroke is made of. */
enum {
    SCHULTZ_PAINT_SOLID = 0, /**< One colour everywhere. */
    SCHULTZ_PAINT_GRADIENT   /**< A registered gradient, by handle. */
};

/**
 * @brief A fill or a stroke: a colour, or a gradient named by handle.
 *
 * A gradient is more data than fits in a style value and is usually shared,
 * so it is registered once and referred to. Its geometry is expressed in the
 * 0 to 1 range of whatever shape it is painting, so one gradient serves a
 * button and a window alike.
 */
typedef struct {
    uint32_t kind; /**< SCHULTZ_PAINT_SOLID or SCHULTZ_PAINT_GRADIENT. */
    union {
        schultz_color  color;    /**< Valid when kind is SOLID. */
        schultz_handle gradient; /**< Valid when kind is GRADIENT. */
    } as;          /**< Read `kind` first, then the matching member. */
} schultz_paint;

/** @brief How the ends of a stroked line are finished. */
enum {
    SCHULTZ_CAP_BUTT = 0, /**< Stops flat at the end point. */
    SCHULTZ_CAP_ROUND,    /**< A half circle past the end point. */
    SCHULTZ_CAP_SQUARE    /**< A half square past the end point. */
};

/** @brief How two stroked segments meet at a corner. */
enum {
    SCHULTZ_JOIN_MITER = 0, /**< Extended to a point. */
    SCHULTZ_JOIN_ROUND,     /**< Rounded off. */
    SCHULTZ_JOIN_BEVEL      /**< Cut flat across. */
};

/**
 * @brief One step of a path: what to do next.
 *
 * A path is two arrays read together: these steps, and the points they use.
 * Each step takes a fixed number of points off the front of the point array,
 * so the two arrays are different lengths and both counts have to be given.
 *
 *   SCHULTZ_PATH_CLOSE   no points
 *   SCHULTZ_PATH_MOVE    one, where the new subpath starts
 *   SCHULTZ_PATH_LINE    one, where the line ends
 *   SCHULTZ_PATH_CURVE   three: two controls, then where the curve ends
 *   SCHULTZ_PATH_QUAD    two: one control, then where the curve ends
 *
 * A path begins with a move. A second move starts a second subpath, which is
 * how a shape gets a hole in it.
 */
enum {
    /** Join back to where this subpath started. */
    SCHULTZ_PATH_CLOSE = 0,
    /** Lift the pen and put it down somewhere else. */
    SCHULTZ_PATH_MOVE,
    /** A straight segment from where the pen is. */
    SCHULTZ_PATH_LINE,
    /** A cubic curve, the kind fonts and vector formats are drawn with. */
    SCHULTZ_PATH_CURVE,
    /**
     * A quadratic curve, which has one control point rather than two. It is
     * raised to a cubic when it is recorded, exactly and with no loss, so
     * this value never reaches a backend and never comes back out of a
     * command list.
     */
    SCHULTZ_PATH_QUAD
};

/** @brief How an arc is finished off at its two ends. */
enum {
    /** Left open: the curve and nothing else. This is the one to stroke. */
    SCHULTZ_ARC_OPEN = 0,
    /** Closed through the centre, so it comes out a wedge of a pie. */
    SCHULTZ_ARC_PIE,
    /** Closed straight across, so it comes out the piece a chord cuts off. */
    SCHULTZ_ARC_CHORD
};

/** @brief How much room an arc's path can ever need. */
enum {
    /** A move, four curves, two lines and a close. */
    SCHULTZ_ARC_STEPS_MAX = 8,
    /** One for the move, three for each curve, one for the line to a centre. */
    SCHULTZ_ARC_POINTS_MAX = 16
};

/**
 * @brief Builds the path of an arc, a pie wedge or a chord.
 *
 * No rasterizer this toolkit uses has an arc of its own, so one is built out
 * of cubic curves, split so that no curve turns more than a quarter. That is
 * the usual approximation and it is accurate to well under a pixel at any
 * size a screen has.
 *
 * Angles are in **degrees**, measured from three o'clock and running
 * clockwise, which is the direction y grows in. A sweep of 360 or more is
 * clamped to a whole turn; a sweep of zero produces nothing.
 *
 * The two arrays are the caller's, and must hold at least
 * SCHULTZ_ARC_STEPS_MAX and SCHULTZ_ARC_POINTS_MAX. Both are small enough to
 * live on the stack. What comes back goes straight to schultz_draw_fill_path
 * or schultz_draw_stroke_path.
 *
 * @param box              The rectangle the whole ellipse is inscribed in,
 *                         not just the part the arc covers.
 * @param start_degrees    Where the arc begins.
 * @param sweep_degrees    How far it turns. Negative turns anticlockwise.
 * @param ends             SCHULTZ_ARC_OPEN, _PIE or _CHORD.
 * @param out_steps        Receives the steps. Must not be NULL.
 * @param out_step_count   Receives how many steps. Must not be NULL.
 * @param out_points       Receives the points. Must not be NULL.
 * @param out_point_count  Receives how many points. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when a pointer is NULL,
 *         the box is empty, or `ends` is none of the three.
 */
int32_t schultz_arc_path(schultz_rect box, float start_degrees,
                         float sweep_degrees, uint32_t ends,
                         uint8_t *out_steps, uint32_t *out_step_count,
                         schultz_point *out_points,
                         uint32_t *out_point_count);

/**
 * @brief How many points one path step uses.
 *
 * @param step One of the SCHULTZ_PATH_* values.
 * @return 0, 1, 2 or 3. An unknown step reports 0.
 */
uint32_t schultz_path_step_points(uint32_t step);

/** @brief Which parts of a self crossing shape count as inside it. */
enum {
    /**
     * Count the crossings, adding for one direction and subtracting for the
     * other. Anything that does not come back to zero is inside. A star drawn
     * in one continuous stroke comes out solid.
     */
    SCHULTZ_FILL_NONZERO = 0,
    /**
     * Count the crossings and take the odd ones as inside. The same star
     * comes out with a hole in the middle, which is the other thing a person
     * might have meant.
     */
    SCHULTZ_FILL_EVEN_ODD
};

/**
 * @brief Everything about a stroke but the shape it follows.
 *
 * Passed by value so the eight drawing calls that stroke something keep one
 * parameter rather than five, and so adding to it later touches none of them.
 *
 * Build one with schultz_stroke_solid or schultz_stroke_make rather than by
 * assigning the fields, so a field added later starts out sensible instead of
 * holding whatever was on the stack.
 */
typedef struct {
    schultz_paint  paint;      /**< What the stroke is drawn with. */
    float          width;      /**< How thick, in pixels. */
    schultz_handle dash;       /**< A registered dash pattern, or none. */
    uint32_t       cap;        /**< One of the SCHULTZ_CAP_* values. */
    uint32_t       join;       /**< One of the SCHULTZ_JOIN_* values. */
    /**
     * How far along the pattern to start, in pixels. Zero starts on the
     * first mark. Drawing the same shape twice at two offsets is how a
     * dashed outline is made to look like it is moving, and it is why this
     * sits on the stroke rather than on the registered pattern: one pattern
     * serves every offset.
     *
     * Ignored when `dash` names nothing.
     */
    float          dash_offset;
    /**
     * How far a mitred corner may reach past the corner itself, as a
     * multiple of the stroke width. A sharp enough angle sends a mitre off
     * towards infinity, so past this limit the corner is cut flat instead.
     * Four by default, which is what SVG and the rasterizer both start from.
     *
     * Ignored unless `join` is SCHULTZ_JOIN_MITER.
     */
    float          miter_limit;
} schultz_stroke;

/**
 * @brief A shadow cast by a node and everything under it.
 *
 * The whole subtree is drawn once, and that picture is what casts the shadow,
 * so a card with a title and a button on it casts one shadow in the shape of
 * the card rather than three overlapping ones.
 */
typedef struct {
    /**
     * The shadow's colour, alpha included. Alpha zero means no shadow at all
     * and costs nothing, which is why it is the default on every node.
     */
    schultz_color color;
    /**
     * Which way it falls, in degrees clockwise, with 0 directly above the
     * node. So 90 is to its right, 180 below it, and 270 to its left. Below
     * is the usual answer, which makes 180 the usual angle.
     *
     * Clockwise, and measured from above, to agree with the rotation a draw
     * list can push. One meaning for degrees across the toolkit.
     *
     * A node that is itself turned keeps its shadow falling the same way on
     * screen, because a light source does not turn with the thing it lights.
     */
    float angle;
    /** How far along that angle the shadow sits, in logical units. */
    float distance;
    /**
     * How soft the edge is. Zero gives a hard edged copy of the shape, which
     * is a real shadow and not a special case. Larger spreads it further, and
     * spreading costs blur work over a larger area.
     */
    float blur;
} schultz_shadow;

/**
 * @brief A shadow that does not appear.
 *
 * What a node has until something says otherwise.
 *
 * @return A shadow whose colour is fully transparent.
 */
schultz_shadow schultz_shadow_none(void);

/**
 * @brief Makes a paint that is one flat colour.
 *
 * @param color The colour.
 * @return The paint.
 */
schultz_paint schultz_paint_solid(schultz_color color);

/**
 * @brief Makes a paint that is a registered gradient.
 *
 * @param gradient A handle from schultz_gradient_linear or _radial.
 * @return The paint.
 */
schultz_paint schultz_paint_gradient(schultz_handle gradient);

/**
 * @brief Makes an ordinary stroke: one colour, no dashes, butt and miter.
 *
 * The dash offset starts at zero and the miter limit at four.
 *
 * @param color The colour.
 * @param width How thick, in pixels.
 * @return The stroke.
 */
schultz_stroke schultz_stroke_solid(schultz_color color, float width);

/**
 * @brief Makes a stroke from a paint, so it can be a gradient.
 *
 * Every field this does not name takes the same default schultz_stroke_solid
 * gives it: no dash, butt cap, mitre join, zero offset, limit of four.
 *
 * @param paint What to stroke with.
 * @param width How thick, in pixels.
 * @return The stroke.
 */
schultz_stroke schultz_stroke_make(schultz_paint paint, float width);

/**
 * @brief Reports whether a paint would draw nothing at all.
 *
 * A fully transparent colour draws nothing; a gradient always draws
 * something, since its stops decide its own transparency.
 *
 * @param paint The paint to test.
 * @return 1 when nothing would be drawn, 0 otherwise.
 */
int32_t schultz_paint_is_invisible(schultz_paint paint);

/**
 * @brief Reports whether a rectangle encloses any area.
 *
 * A rectangle is empty when either extent is not greater than zero, which
 * includes negative extents.
 *
 * @param rect The rectangle to test, by value.
 * @return 1 when the rectangle is empty, 0 when it encloses area.
 */
int32_t schultz_rect_is_empty(schultz_rect rect);

/**
 * @brief Compares two rectangles field by field.
 *
 * This is an exact float comparison, not a tolerance comparison.
 *
 * @param a First rectangle, by value.
 * @param b Second rectangle, by value.
 * @return 1 when all four fields are equal, 0 otherwise.
 */
int32_t schultz_rect_equals(schultz_rect a, schultz_rect b);

/**
 * @brief Tests whether a point lies inside a rectangle.
 *
 * The left and top edges are inside and the right and bottom edges are
 * outside, so two adjacent rectangles never both claim a point on their
 * shared edge. An empty rectangle contains nothing.
 *
 * @param rect  The rectangle to test against, by value.
 * @param point The point to test, by value.
 * @return 1 when the point is inside, 0 otherwise.
 */
int32_t schultz_rect_contains_point(schultz_rect rect, schultz_point point);

/**
 * @brief Grows a rectangle by the same amount on every side.
 *
 * A negative amount shrinks it, and shrinking past nothing gives an empty
 * rectangle at the centre rather than a negative size.
 *
 * @param rect   The rectangle to grow.
 * @param amount How far to move each edge outward, in pixels.
 * @return The grown rectangle.
 */
schultz_rect schultz_rect_expand(schultz_rect rect, float amount);

/**
 * @brief How far past a dirty rectangle repainting actually reaches.
 *
 * Antialiased coverage extends past a shape's geometry, so every consumer of
 * a dirty rectangle rounds it out to whole pixels with this much slack. It is
 * named once so the region that gets repainted and the region that decides
 * what to repaint cannot drift apart.
 *
 * A function rather than a constant, because a public header carries no
 * macros: a host reading this across a foreign function interface should not
 * have to reproduce a number that only C can see.
 *
 * @return The slack, in pixels.
 */
float schultz_paint_slack(void);

/**
 * @brief Computes the overlap of two rectangles.
 *
 * Rectangles that merely touch along an edge do not overlap.
 *
 * @param a First rectangle, by value.
 * @param b Second rectangle, by value.
 * @return The overlapping region, or a zero sized rectangle at the origin
 *         when the two do not overlap. Test the result with
 *         schultz_rect_is_empty rather than comparing against a rectangle.
 */
schultz_rect schultz_rect_intersect(schultz_rect a, schultz_rect b);

/**
 * @brief Expands a rectangle outward to whole pixel boundaries.
 *
 * Anything that hands a rectangle to an integer pixel API must round outward,
 * never truncate. Truncating loses a fraction of a pixel on the far edge, and
 * for a region that moves by a fractional amount each frame that loss
 * accumulates and periodically leaves a column of the previous frame behind.
 *
 * @param rect  The rectangle to align, in pixels.
 * @param slack Extra margin in pixels, applied on every side before
 *              alignment. Pass at least 1 for anything antialiased, because
 *              coverage bleeds outside the geometric bounds.
 * @return The smallest whole pixel rectangle that contains the input expanded
 *         by slack. An empty input yields an empty result.
 */
schultz_rect schultz_rect_pixel_bounds(schultz_rect rect, float slack);

/**
 * @brief Returns the x coordinate just past the right edge.
 *
 * @param rect The rectangle, by value.
 * @return rect.x + rect.width.
 */
float schultz_rect_right(schultz_rect rect);

/**
 * @brief Returns the y coordinate just past the bottom edge.
 *
 * @param rect The rectangle, by value.
 * @return rect.y + rect.height.
 */
float schultz_rect_bottom(schultz_rect rect);

/**
 * @brief Compares two colors channel by channel.
 *
 * @param a First color, by value.
 * @param b Second color, by value.
 * @return 1 when all four channels are equal, 0 otherwise.
 */
int32_t schultz_color_equals(schultz_color a, schultz_color b);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_GEOM_H */
