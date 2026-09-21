/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_paint.h
 * @brief Draw command list and the painter interface.
 *
 * Internal header.
 *
 * Widgets never talk to a rasterizer. A paint pass appends commands to a draw
 * list, and a backend consumes that list through the painter interface. The
 * indirection is what lets the same widget code run against ThorVG, a test
 * fake, or anything added later.
 *
 * Command storage comes from an arena, in chunks, so a frame's worth of
 * commands costs no malloc once the arena has settled and nothing has to be
 * reallocated while the list is being built.
 *
 * **Not part of the host facing interface.** The draw command list a widget
 * written in C emits into. A host binds to schultz_api.h; this header is the
 * toolkit's own and may change without notice.
 */

#ifndef SCHULTZ_PAINT_H
#define SCHULTZ_PAINT_H

#include "schultz_arena.h"
#include "schultz_geom.h"

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


/** @brief Discriminator for schultz_draw_cmd. Zero is never a valid kind. */
enum {
    SCHULTZ_DRAW_FILL_RECT = 1,     /**< Filled rectangle. */
    SCHULTZ_DRAW_STROKE_RECT,       /**< Rectangle outline. */
    SCHULTZ_DRAW_FILL_ROUND_RECT,   /**< Filled rounded rectangle. */
    SCHULTZ_DRAW_STROKE_ROUND_RECT, /**< Rounded rectangle outline. */
    SCHULTZ_DRAW_FILL_ELLIPSE,      /**< Filled ellipse in a rectangle. */
    SCHULTZ_DRAW_STROKE_ELLIPSE,    /**< Ellipse outline. */
    SCHULTZ_DRAW_LINE,              /**< Straight line segment. */
    SCHULTZ_DRAW_FILL_POLYGON,         /**< Filled polygon. */
    SCHULTZ_DRAW_STROKE_POLYGON,       /**< Outlined polygon or polyline. */
    SCHULTZ_DRAW_FILL_PATH,         /**< Filled path, curves and all. */
    SCHULTZ_DRAW_STROKE_PATH,       /**< Outlined path, curves and all. */
    SCHULTZ_DRAW_IMAGE,             /**< Image blit. */
    SCHULTZ_DRAW_GLYPH_RUN,         /**< Run of positioned glyphs. */
    SCHULTZ_DRAW_GLYPH_OUTLINE,     /**< Run of positioned glyphs, stroked. */
    SCHULTZ_DRAW_CLIP_BEGIN,         /**< Narrow the clip region. */
    SCHULTZ_DRAW_CLIP_END,          /**< Restore the previous clip region. */
    SCHULTZ_DRAW_OFFSET_BEGIN,    /**< Offset subsequent drawing. */
    SCHULTZ_DRAW_OFFSET_END,     /**< Restore the previous offset. */
    SCHULTZ_DRAW_ROTATION_BEGIN,     /**< Turn what follows about a point. */
    SCHULTZ_DRAW_ROTATION_END,      /**< Restore the previous rotation. */
    SCHULTZ_DRAW_DEVICE_PIXELS_BEGIN,/**< Draw in screen pixels from here. */
    SCHULTZ_DRAW_DEVICE_PIXELS_END, /**< Go back to the toolkit's units. */
    SCHULTZ_DRAW_GROUP_BEGIN,        /**< Draw what follows as one picture. */
    SCHULTZ_DRAW_GROUP_END          /**< Finish it and blend it in. */
};

/**
 * @brief One positioned glyph.
 *
 * The text pipeline produces these; the painter turns them into quads.
 */
typedef struct {
    uint32_t glyph_id; /**< Font specific glyph index, not a character code. */
    float    x;        /**< Pen origin x, in pixels. */
    float    y;        /**< Pen origin y, on the baseline. */
} schultz_glyph;

/**
 * @brief One drawing operation.
 *
 * Read `kind` first, then the matching member of the union. Any other member
 * is meaningless.
 */
typedef struct {
    uint32_t kind; /**< One of the SCHULTZ_DRAW_* constants. */
    union {
        /** Valid when kind is SCHULTZ_DRAW_FILL_RECT. */
        struct { schultz_rect rect; schultz_paint paint; }              fill_rect;
        /** Valid when kind is SCHULTZ_DRAW_STROKE_RECT. */
        struct { schultz_rect rect; schultz_stroke stroke; }            stroke_rect;
        /** Valid when kind is SCHULTZ_DRAW_FILL_ROUND_RECT. */
        struct { schultz_rect rect; schultz_paint paint;
                 float radius; }                                        fill_round_rect;
        /** Valid when kind is SCHULTZ_DRAW_STROKE_ROUND_RECT. */
        struct { schultz_rect rect; schultz_stroke stroke;
                 float radius; }                                        stroke_round_rect;
        /** Valid when kind is SCHULTZ_DRAW_FILL_ELLIPSE. */
        struct { schultz_rect rect; schultz_paint paint; }              fill_ellipse;
        /** Valid when kind is SCHULTZ_DRAW_STROKE_ELLIPSE. */
        struct { schultz_rect rect; schultz_stroke stroke; }            stroke_ellipse;
        /** Valid when kind is SCHULTZ_DRAW_LINE. */
        struct { schultz_point from; schultz_point to;
                 schultz_stroke stroke; }                               line;
        /** Valid when kind is SCHULTZ_DRAW_FILL_POLYGON. Points live in the
         *  arena and are valid until the arena is reset. `rule` is one of
         *  the SCHULTZ_FILL_* values and decides what a self crossing
         *  outline encloses. */
        struct { const schultz_point *points; uint32_t count;
                 schultz_paint paint; uint32_t rule; }          fill_polygon;
        /**
         * Valid when kind is SCHULTZ_DRAW_STROKE_POLYGON. Points live in the
         * arena, as they do for a fill. `closed` joins the last point back
         * to the first, which is the difference between an outline and a
         * line through a series of points.
         */
        struct { const schultz_point *points; uint32_t count;
                 schultz_stroke stroke; uint32_t closed; }   stroke_polygon;
        /**
         * Valid when kind is SCHULTZ_DRAW_FILL_PATH. Steps and points live in
         * the arena and are valid until it is reset. The two arrays are read
         * together: each step takes the points it needs off the front of the
         * second. Every step here is CLOSE, MOVE, LINE or CURVE; a quadratic
         * was raised to a cubic on the way in.
         */
        struct { const uint8_t *steps; uint32_t step_count;
                 const schultz_point *points; uint32_t point_count;
                 schultz_paint paint; uint32_t rule; }                fill_path;
        /** Valid when kind is SCHULTZ_DRAW_STROKE_PATH. Read as above. */
        struct { const uint8_t *steps; uint32_t step_count;
                 const schultz_point *points; uint32_t point_count;
                 schultz_stroke stroke; }                           stroke_path;
        /** Valid when kind is SCHULTZ_DRAW_IMAGE. */
        struct { schultz_handle image; schultz_rect source;
                 schultz_rect dest; uint8_t opacity; }                  image;
        /**
         * Valid when kind is SCHULTZ_DRAW_GLYPH_RUN. Glyphs live in the
         * arena and are valid until the arena is reset.
         *
         * A letter takes `paint` whole. An emoji brings its own colours and
         * takes only how solid `paint` is, because there is no sense in
         * which a yellow face is the text colour.
         */
        struct { schultz_handle font; const schultz_glyph *glyphs;
                 uint32_t count; schultz_paint paint; }                 glyph_run;
        /**
         * Valid when kind is SCHULTZ_DRAW_GLYPH_OUTLINE. The same glyphs a
         * run carries, drawn from the font's outlines and stroked rather
         * than filled. A glyph with no outline is left out.
         */
        struct { schultz_handle font; const schultz_glyph *glyphs;
                 uint32_t count; schultz_stroke stroke; }      glyph_outline;
        /** Valid when kind is SCHULTZ_DRAW_CLIP_BEGIN. */
        struct { schultz_rect rect; }                                   clip_begin;
        /** Valid when kind is SCHULTZ_DRAW_OFFSET_BEGIN. */
        struct { float dx; float dy; }                                  offset_begin;
        /**
         * Valid when kind is SCHULTZ_DRAW_ROTATION_BEGIN. Degrees, clockwise,
         * about a centre in the coordinates in force at this point.
         */
        struct { float degrees; float cx; float cy; }             rotation_begin;
        /**
         * Valid when kind is SCHULTZ_DRAW_GROUP_BEGIN. Everything up to the
         * matching end is drawn as one picture, and that picture is blended
         * in at this opacity rather than each piece being blended at it
         * separately. The difference shows wherever the pieces overlap: two
         * half transparent shapes on top of each other are darker where they
         * meet when they are faded one at a time, and are not when they are
         * faded together.
         */
        struct { float opacity; schultz_shadow shadow; }            group_begin;
    } as; /**< Per kind payload. */
} schultz_draw_cmd;

/** @brief Forward declaration so a chunk can chain to the next one. */
typedef struct schultz_draw_chunk schultz_draw_chunk;

/** @brief A fixed run of commands. Lists chain these rather than realloc. */
struct schultz_draw_chunk {
    schultz_draw_chunk *next;        /**< Next chunk, or NULL. */
    uint32_t            count;       /**< Commands used in this chunk. */
    uint32_t            capacity;    /**< Commands this chunk can hold. */
    schultz_draw_cmd    commands[];  /**< The commands themselves. */
};

/** @brief An ordered list of draw commands backed by an arena. */
typedef struct {
    schultz_arena      *arena;          /**< Where chunks come from. */
    schultz_draw_chunk *first;          /**< First chunk, or NULL. */
    schultz_draw_chunk *last;           /**< Chunk being appended to. */
    uint32_t            count;          /**< Total commands in the list. */
    uint32_t            chunk_capacity; /**< Commands per new chunk. */
    int32_t             overflowed;     /**< Nonzero once an append failed. */
} schultz_draw_list;

/**
 * @brief Prepares a draw list that appends into an arena.
 *
 * @param list           The list to initialize. Must not be NULL.
 * @param arena          The arena chunks are taken from. Must not be NULL and
 *                       must outlive the list.
 * @param chunk_capacity Commands per chunk. Pass 0 to accept the default.
 *                       Affects only how often a chunk is taken from the
 *                       arena, never the list's capacity, which is unbounded.
 * @return SCHULTZ_OK on success, or SCHULTZ_ERR_INVALID_ARGUMENT when list or
 *         arena is NULL.
 */
int32_t schultz_draw_list_init(schultz_draw_list *list, schultz_arena *arena,
                               uint32_t chunk_capacity);

/**
 * @brief Drops every command from the list.
 *
 * Chunks already taken from the arena are kept and reused, so a list reset
 * each frame settles and stops asking for memory. This does not reset the
 * arena; the frame loop owns that. Pointers previously returned by
 * schultz_draw_list_at become invalid.
 *
 * @param list The list to reset. NULL is accepted and does nothing.
 */
void schultz_draw_list_reset(schultz_draw_list *list);

/**
 * @brief Counts commands in the list.
 *
 * @param list The list to query. NULL yields 0.
 * @return The number of commands appended since the last reset.
 */
uint32_t schultz_draw_list_count(const schultz_draw_list *list);

/**
 * @brief Returns the command at an index.
 *
 * Walks the chunk chain, so this is meant for tests and tooling. A backend
 * should use schultz_draw_list_play instead.
 *
 * @param list  The list to read. NULL yields NULL.
 * @param index Zero based position in append order.
 * @return A pointer to the command, valid until the list is reset or the
 *         arena is reset, or NULL when index is out of range.
 */
const schultz_draw_cmd *schultz_draw_list_at(const schultz_draw_list *list,
                                             uint32_t index);

/**
 * @brief Reports whether any append has failed.
 *
 * Lets a caller build a whole frame and check once at the end rather than
 * testing the result of every append. Cleared by schultz_draw_list_reset.
 *
 * @param list The list to query. NULL yields 0.
 * @return Nonzero when at least one append could not allocate.
 */
int32_t schultz_draw_list_overflowed(const schultz_draw_list *list);

/**
 * @brief Appends a filled rectangle.
 *
 * @param list  The list to append to. Must not be NULL.
 * @param rect  The rectangle to fill, in pixels.
 * @param paint What to fill with: a colour or a gradient.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when a chunk could not be taken.
 */
int32_t schultz_draw_fill_rect(schultz_draw_list *list, schultz_rect rect,
                               schultz_paint paint);

/**
 * @brief Appends a rectangle outline.
 *
 * @param list  The list to append to. Must not be NULL.
 * @param rect  The rectangle to outline, in pixels.
 * @param stroke What the outline is drawn with: its paint, width, dash
 *               pattern, cap and join.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when a chunk could not be taken.
 */
int32_t schultz_draw_stroke_rect(schultz_draw_list *list, schultz_rect rect,
                                 schultz_stroke stroke);

/**
 * @brief Appends a filled rounded rectangle.
 *
 * @param list   The list to append to. Must not be NULL.
 * @param rect   The rectangle to fill, in pixels.
 * @param paint  What to fill with.
 * @param radius Corner radius in pixels.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when a chunk could not be taken.
 */
int32_t schultz_draw_fill_round_rect(schultz_draw_list *list,
                                     schultz_rect rect, schultz_paint paint,
                                     float radius);

/**
 * @brief Appends a rounded rectangle outline.
 *
 * @param list   The list to append to. Must not be NULL.
 * @param rect   The rectangle to outline, in pixels.
 * @param stroke What the outline is drawn with.
 * @param radius Corner radius in pixels.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when a chunk could not be taken.
 */
int32_t schultz_draw_stroke_round_rect(schultz_draw_list *list,
                                       schultz_rect rect,
                                       schultz_stroke stroke, float radius);

/**
 * @brief Appends a filled ellipse inscribed in a rectangle.
 *
 * @param list  The list to append to. Must not be NULL.
 * @param rect  The rectangle the ellipse fills, in pixels. A square
 *              gives a circle.
 * @param paint What to fill with.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_draw_fill_ellipse(schultz_draw_list *list, schultz_rect rect,
                                  schultz_paint paint);

/**
 * @brief Appends an ellipse outline inscribed in a rectangle.
 *
 * @param list  The list to append to. Must not be NULL.
 * @param rect  The rectangle the ellipse fills, in pixels.
 * @param stroke What the outline is drawn with.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_draw_stroke_ellipse(schultz_draw_list *list,
                                    schultz_rect rect,
                                    schultz_stroke stroke);

/**
 * @brief Appends a straight line segment.
 *
 * @param list  The list to append to. Must not be NULL.
 * @param from  Start point, in pixels.
 * @param to    End point, in pixels.
 * @param stroke What the line is drawn with.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when a chunk could not be taken.
 */
int32_t schultz_draw_line(schultz_draw_list *list, schultz_point from,
                          schultz_point to, schultz_stroke stroke);

/**
 * @brief Appends a filled polygon.
 *
 * The points are copied into the arena, so the caller's array may be a stack
 * local and need not outlive the call.
 *
 * @param list   The list to append to. Must not be NULL.
 * @param points The vertices, in order. Must not be NULL. The polygon is
 *               implicitly closed from the last point back to the first.
 * @param count  Number of vertices. Must be greater than zero.
 * @param paint  What to fill with.
 * @param rule   SCHULTZ_FILL_NONZERO or SCHULTZ_FILL_EVEN_ODD. Only tells
 *               them apart when the outline crosses itself.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list or points is
 *         NULL, count is zero, or the rule is neither of the two, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when the copy or the chunk could not be
 *         allocated.
 */
int32_t schultz_draw_fill_polygon(schultz_draw_list *list,
                                  const schultz_point *points, uint32_t count,
                                  schultz_paint paint, uint32_t rule);

/**
 * @brief Appends a polygon outline, or a line through a series of points.
 *
 * The points are copied into the arena, so a stack local array is fine.
 *
 * @param list   The list to append to. Must not be NULL.
 * @param points The vertices, in order. Must not be NULL.
 * @param count  How many. Must be at least two.
 * @param stroke What the outline is drawn with.
 * @param closed Nonzero joins the last point back to the first, making an
 *               outline; zero leaves it open, making a polyline.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_draw_stroke_polygon(schultz_draw_list *list,
                                    const schultz_point *points, uint32_t count,
                                    schultz_stroke stroke, int32_t closed);

/**
 * @brief Appends a filled path, which may curve and may have holes.
 *
 * The counterpart to schultz_draw_fill_polygon for anything a straight sided
 * outline cannot say. Both arrays are copied into the arena, so stack local
 * arrays are fine.
 *
 * A quadratic step is raised to a cubic here, so what the backend sees is
 * always one of CLOSE, MOVE, LINE and CURVE.
 *
 * @param list        The list to append to. Must not be NULL.
 * @param steps       SCHULTZ_PATH_* values, in order. Must not be NULL, and
 *                    must begin with SCHULTZ_PATH_MOVE.
 * @param step_count  How many steps. Must be greater than zero.
 * @param points      The points the steps use. Must not be NULL.
 * @param point_count How many points. Must be exactly the number the steps
 *                    together ask for, which schultz_path_step_points gives.
 * @param paint       What to fill with.
 * @param rule        SCHULTZ_FILL_NONZERO or SCHULTZ_FILL_EVEN_ODD.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when an argument is NULL,
 *         a count is zero, a step is not one of the five, the first step is
 *         not a move, the rule is neither of the two, or the points do not
 *         add up, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_draw_fill_path(schultz_draw_list *list, const uint8_t *steps,
                               uint32_t step_count,
                               const schultz_point *points,
                               uint32_t point_count, schultz_paint paint,
                               uint32_t rule);

/**
 * @brief Appends a stroked path, which may curve and may have holes.
 *
 * A subpath that was closed is stroked all the way round; one that was not
 * stops at its last point, with the stroke's cap on each end.
 *
 * @param list        The list to append to. Must not be NULL.
 * @param steps       SCHULTZ_PATH_* values, in order. Must not be NULL, and
 *                    must begin with SCHULTZ_PATH_MOVE.
 * @param step_count  How many steps. Must be greater than zero.
 * @param points      The points the steps use. Must not be NULL.
 * @param point_count How many the steps together ask for.
 * @param stroke      What the outline is drawn with.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT as above, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_draw_stroke_path(schultz_draw_list *list,
                                 const uint8_t *steps, uint32_t step_count,
                                 const schultz_point *points,
                                 uint32_t point_count, schultz_stroke stroke);

/**
 * @brief Appends an image blit.
 *
 * @param list    The list to append to. Must not be NULL.
 * @param image   Handle of the image to draw. Must not be
 *                SCHULTZ_HANDLE_NONE. Resolved by the backend, not here.
 * @param source  Region of the image to read, in image pixels.
 * @param dest    Region to draw into, in pixels. Scaling is implied
 *                when the two differ in size.
 * @param opacity Blend opacity, 0 transparent to 255 opaque.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL or image
 *         is SCHULTZ_HANDLE_NONE, or SCHULTZ_ERR_OUT_OF_MEMORY when a chunk
 *         could not be taken.
 */
int32_t schultz_draw_image(schultz_draw_list *list, schultz_handle image,
                           schultz_rect source, schultz_rect dest,
                           uint8_t opacity);

/**
 * @brief Appends a run of positioned glyphs.
 *
 * The glyphs are copied into the arena, so the caller's array may be a stack
 * local and need not outlive the call.
 *
 * @param list   The list to append to. Must not be NULL.
 * @param font   Handle of the font to draw with. Must not be
 *               SCHULTZ_HANDLE_NONE. Resolved by the backend, not here.
 * @param glyphs The positioned glyphs. Must not be NULL. These come from the
 *               shaping stage, already laid out.
 * @param count  Number of glyphs. Must be greater than zero.
 * @param paint  What to fill the letters with. A gradient is measured across
 *               the run's own bounds, so one gradient reads the same whatever
 *               the run says.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list or glyphs is
 *         NULL, count is zero, or font is SCHULTZ_HANDLE_NONE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when the copy or the chunk could not be
 *         allocated.
 */
int32_t schultz_draw_glyph_run(schultz_draw_list *list, schultz_handle font,
                               const schultz_glyph *glyphs, uint32_t count,
                               schultz_paint paint);

/**
 * @brief Starts a clip, narrowing the drawable region.
 *
 * Must be balanced by schultz_draw_clip_end. The backend intersects this with
 * the clip already in effect.
 *
 * @param list The list to append to. Must not be NULL.
 * @param rect The new clip region, in pixels.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when a chunk could not be taken.
 */
int32_t schultz_draw_clip_begin(schultz_draw_list *list, schultz_rect rect);

/**
 * @brief Ends a clip, restoring the previous clip region.
 *
 * @param list The list to append to. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when a chunk could not be taken.
 */
int32_t schultz_draw_clip_end(schultz_draw_list *list);

/**
 * @brief Appends a translation that offsets subsequent drawing.
 *
 * Must be balanced by schultz_draw_offset_end. Offsets accumulate with any
 * translation already in effect.
 *
 * @param list The list to append to. Must not be NULL.
 * @param dx   Horizontal offset in pixels.
 * @param dy   Vertical offset in pixels.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when a chunk could not be taken.
 */
int32_t schultz_draw_offset_begin(schultz_draw_list *list, float dx,
                                    float dy);

/**
 * @brief Ends an offset, restoring the previous one.
 *
 * @param list The list to append to. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when a chunk could not be taken.
 */
int32_t schultz_draw_offset_end(schultz_draw_list *list);

/**
 * @brief Starts a group.
 *
 * Everything drawn until the matching schultz_draw_group_end is composed into
 * one picture, and that picture is blended in as a whole.
 *
 * This is what makes a subtree fade as one thing. Without it, fading a panel
 * and its contents means fading each of them, and everywhere they overlap is
 * darker than it should be, because the same pixel was blended twice.
 *
 * A group also casts the shadow, for the same reason: the subtree makes one
 * picture, and one picture casts one shadow. Without that, a card with a
 * title and a button would cast three shadows that overlap.
 *
 * @param list    The list to add to. Must not be NULL.
 * @param opacity 0 for invisible, 1 for solid. Values outside are clamped by
 *                whatever draws them.
 * @param shadow  The shadow the group casts. schultz_shadow_none for none,
 *                which is also what a fully transparent colour means.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_draw_group_begin(schultz_draw_list *list, float opacity,
                                schultz_shadow shadow);

/**
 * @brief Finishes the group started by schultz_draw_group_begin.
 *
 * @param list The list to add to. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_draw_group_end(schultz_draw_list *list);

/**
 * @brief Draws what follows in screen pixels rather than toolkit units.
 *
 * The toolkit usually draws everything larger on a screen that packs more
 * pixels into the same space, so one unit covers several of them. Between
 * this and its matching end, one unit is one pixel of the screen in front of
 * the user, and a line one wide is one pixel wide.
 *
 * Where the drawing sits is unaffected: the position built up by any
 * enclosing transforms still means what it meant. Only the size of a unit
 * inside changes.
 *
 * On a screen with nothing to scale this does nothing, because a unit is
 * already a pixel there.
 *
 * Must be balanced by schultz_draw_device_pixels_end.
 *
 * @param list The list to append to. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when a chunk could not be taken.
 */
int32_t schultz_draw_device_pixels_begin(schultz_draw_list *list);

/**
 * @brief Appends a run of glyphs drawn as outlines and stroked.
 *
 * The same glyphs schultz_draw_glyph_run takes, drawn the other way: from the
 * font's outlines, with a stroke round each letter rather than a fill inside
 * it. A glyph that is a picture rather than an outline is left out, because
 * there is nothing to stroke.
 *
 * @param list   The list to append to. Must not be NULL.
 * @param font   Handle of the font to draw with. Must not be
 *               SCHULTZ_HANDLE_NONE.
 * @param glyphs The positioned glyphs. Must not be NULL.
 * @param count  Number of glyphs. Must be greater than zero.
 * @param stroke What the outlines are drawn with.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_draw_glyph_outline(schultz_draw_list *list,
                                   schultz_handle font,
                                   const schultz_glyph *glyphs,
                                   uint32_t count, schultz_stroke stroke);

/**
 * @brief Appends a rotation, turning everything drawn after it.
 *
 * Must be balanced by schultz_draw_rotation_end. Rotations nest with each
 * other and with offsets: an offset started inside a rotation moves along the
 * turned axes, which is what makes the two compose the way a person expects.
 *
 * The angle is in **degrees**, clockwise, because y grows downward. The
 * centre is a point in the coordinates in force here, so turning a drawing
 * about its own middle is one call rather than an offset either side of it.
 *
 * This turns what is drawn and nothing else. The node keeps its upright
 * bounds, is laid out the same and is hit tested the same.
 *
 * A clip started inside a rotation is the upright box around the turned
 * rectangle rather than the turned rectangle itself, so it lets through a
 * little more than it names. See schultz_draw_clip_begin.
 *
 * @param list    The list to append to. Must not be NULL.
 * @param degrees How far to turn, clockwise.
 * @param cx      The centre to turn about, along x.
 * @param cy      The centre to turn about, along y.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_draw_rotation_begin(schultz_draw_list *list, float degrees,
                                   float cx, float cy);

/**
 * @brief Ends a rotation, restoring the previous one.
 *
 * @param list The list to append to. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when list is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_draw_rotation_end(schultz_draw_list *list);

/**
 * @brief Restores the unit saved by the matching device_pixels_begin.
 *
 * @param list The list to append to. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_draw_device_pixels_end(schultz_draw_list *list);

/**
 * @brief Appends a command that already exists.
 *
 * Copies the command itself. Anything a command points at, such as a path's
 * points or a run's glyphs, is not copied, so that memory has to outlive the
 * list being appended to. This is what lets a retained list be replayed into
 * a frame's list without shaping or copying anything again.
 *
 * @param list The list to append to. Must not be NULL.
 * @param cmd  The command to copy in. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when a chunk could not be taken.
 */
int32_t schultz_draw_list_replay_one(schultz_draw_list *list,
                                     const schultz_draw_cmd *cmd);

/**
 * @brief The painter interface a rendering backend implements.
 *
 * Every entry must be non-NULL. schultz_draw_list_play rejects an incomplete
 * vtable rather than silently skipping drawing, because a missing entry would
 * otherwise show up as content that never appears.
 *
 * Each command handler receives the whole command; read the union member that
 * matches its kind. Handlers return SCHULTZ_OK to continue, or any error to
 * abort the frame.
 */
typedef struct {
    /** Called once before any command. Receives the frame bounds. */
    int32_t (*begin)(void *context, schultz_rect bounds);
    /** Called once after the last command, including after an abort. */
    int32_t (*end)(void *context);
    /** Fill a rectangle. Read cmd->as.fill_rect. */
    int32_t (*fill_rect)(void *context, const schultz_draw_cmd *cmd);
    /** Outline a rectangle. Read cmd->as.stroke_rect. */
    int32_t (*stroke_rect)(void *context, const schultz_draw_cmd *cmd);
    /** Fill a rounded rectangle. Read cmd->as.fill_round_rect. */
    int32_t (*fill_round_rect)(void *context, const schultz_draw_cmd *cmd);
    /** Outline a rounded rectangle. Read cmd->as.stroke_round_rect. */
    int32_t (*stroke_round_rect)(void *context, const schultz_draw_cmd *cmd);
    /** Stroke a line segment. Read cmd->as.line. */
    /** Fill an ellipse. Read cmd->as.fill_ellipse. */
    int32_t (*fill_ellipse)(void *context, const schultz_draw_cmd *cmd);
    /** Outline an ellipse. Read cmd->as.stroke_ellipse. */
    int32_t (*stroke_ellipse)(void *context, const schultz_draw_cmd *cmd);
    /** Stroke a line segment. Read cmd->as.line. */
    int32_t (*line)(void *context, const schultz_draw_cmd *cmd);
    /** Fill a closed polygon. Read cmd->as.fill_polygon. */
    int32_t (*fill_polygon)(void *context, const schultz_draw_cmd *cmd);
    /** Outlines a polygon, or a polyline when it is not closed. */
    int32_t (*stroke_polygon)(void *context, const schultz_draw_cmd *cmd);
    /** Fill a path that may curve. Read cmd->as.fill_path. */
    int32_t (*fill_path)(void *context, const schultz_draw_cmd *cmd);
    /** Stroke a path that may curve. Read cmd->as.stroke_path. */
    int32_t (*stroke_path)(void *context, const schultz_draw_cmd *cmd);
    /** Blit an image. Resolve cmd->as.image.image to a texture. */
    int32_t (*image)(void *context, const schultz_draw_cmd *cmd);
    /** Draw positioned glyphs. Resolve cmd->as.glyph_run.font. */
    int32_t (*glyph_run)(void *context, const schultz_draw_cmd *cmd);
    /** Stroke a run of glyphs. Read cmd->as.glyph_outline. */
    int32_t (*glyph_outline)(void *context, const schultz_draw_cmd *cmd);
    /** Narrow the clip to cmd->as.clip_begin.rect until the matching end. */
    int32_t (*clip_begin)(void *context, const schultz_draw_cmd *cmd);
    /** Restore the clip region saved by the matching clip_begin. */
    int32_t (*clip_end)(void *context);
    /** Shift by cmd->as.offset_begin until the matching end. */
    int32_t (*offset_begin)(void *context, const schultz_draw_cmd *cmd);
    /** Restore the offset saved by the matching offset_begin. */
    int32_t (*offset_end)(void *context);
    /** Turn what follows about a point. Read cmd->as.rotation_begin. */
    int32_t (*rotation_begin)(void *context, const schultz_draw_cmd *cmd);
    /** Restore the rotation saved by the matching rotation_begin. */
    int32_t (*rotation_end)(void *context);
    /** Starts a group; everything until group_end composes as one picture. */
    int32_t (*group_begin)(void *context, const schultz_draw_cmd *cmd);
    /** Finishes that group and blends it in at the opacity it was given. */
    int32_t (*group_end)(void *context);

    /** Draw what follows in screen pixels, whatever the toolkit's unit. */
    int32_t (*device_pixels_begin)(void *context);
    /** Restore the unit saved by the matching device_pixels_begin. */
    int32_t (*device_pixels_end)(void *context);
} schultz_painter_vtable;

/** @brief A backend: its function table plus its own state. */
typedef struct {
    const schultz_painter_vtable *vtable;  /**< Handlers. Must be complete. */
    void                         *context; /**< Passed to every handler. */
} schultz_painter;

/**
 * @brief Plays a command list into a backend.
 *
 * Calls begin, then one handler per command in append order, then end. Stops
 * at the first handler error and returns it, still calling end so the backend
 * can unwind.
 *
 * @param list    The commands to play. Must not be NULL. An empty list is
 *                valid and still brackets the frame with begin and end.
 * @param painter The backend to play into. Must not be NULL, and its vtable
 *                must be non-NULL with every entry filled in.
 * @param bounds  The frame bounds, passed through to begin. Typically the
 *                surface or dirty region being painted.
 * @return SCHULTZ_OK when every handler succeeded,
 *         SCHULTZ_ERR_INVALID_ARGUMENT for a NULL list or painter, a NULL
 *         vtable, an incomplete vtable, or an unrecognized command kind, or
 *         whatever error a handler returned.
 */
int32_t schultz_draw_list_play(const schultz_draw_list *list,
                               const schultz_painter *painter,
                               schultz_rect bounds);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_PAINT_H */
