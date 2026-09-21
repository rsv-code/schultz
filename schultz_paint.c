/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * schultz_paint.c - draw command list and the painter interface.
 */

#include "schultz_paint.h"

#include <string.h>

enum {
    SCHULTZ_DRAW_DEFAULT_CHUNK_CAPACITY = 128
};

int32_t schultz_draw_list_init(schultz_draw_list *list, schultz_arena *arena,
                          uint32_t chunk_capacity)
{
    if (list == NULL || arena == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    list->arena          = arena;
    list->first          = NULL;
    list->last           = NULL;
    list->count          = 0;
    list->chunk_capacity = (chunk_capacity == 0)
                               ? SCHULTZ_DRAW_DEFAULT_CHUNK_CAPACITY
                               : chunk_capacity;
    list->overflowed     = 0;
    return SCHULTZ_OK;
}

void schultz_draw_list_reset(schultz_draw_list *list)
{
    schultz_draw_chunk *chunk;

    if (list == NULL) {
        return;
    }

    for (chunk = list->first; chunk != NULL; chunk = chunk->next) {
        chunk->count = 0;
    }
    list->last       = list->first;
    list->count      = 0;
    list->overflowed = 0;
}

uint32_t schultz_draw_list_count(const schultz_draw_list *list)
{
    return (list == NULL) ? 0 : list->count;
}

int32_t schultz_draw_list_overflowed(const schultz_draw_list *list)
{
    return (list == NULL) ? 0 : list->overflowed;
}

const schultz_draw_cmd *schultz_draw_list_at(const schultz_draw_list *list, uint32_t index)
{
    const schultz_draw_chunk *chunk;

    if (list == NULL || index >= list->count) {
        return NULL;
    }

    for (chunk = list->first; chunk != NULL; chunk = chunk->next) {
        if (index < chunk->count) {
            return &chunk->commands[index];
        }
        index -= chunk->count;
    }
    return NULL;
}

/*
 * Returns a slot for one more command, taking a fresh chunk from the arena
 * when the current one is full. Reuses chunks left over from a previous
 * frame before asking the arena for more.
 */
static schultz_draw_cmd *schultz_draw_list_append(schultz_draw_list *list)
{
    schultz_draw_chunk *chunk;
    size_t bytes;

    if (list->last != NULL && list->last->count < list->last->capacity) {
        chunk = list->last;
    } else if (list->last != NULL && list->last->next != NULL) {
        chunk       = list->last->next;
        chunk->count = 0;
        list->last  = chunk;
    } else {
        bytes = sizeof(schultz_draw_chunk) +
                (size_t)list->chunk_capacity * sizeof(schultz_draw_cmd);
        chunk = (schultz_draw_chunk *)schultz_arena_alloc(list->arena, bytes, 0);
        if (chunk == NULL) {
            list->overflowed = 1;
            return NULL;
        }
        chunk->next     = NULL;
        chunk->count    = 0;
        chunk->capacity = list->chunk_capacity;

        if (list->first == NULL) {
            list->first = chunk;
        } else {
            list->last->next = chunk;
        }
        list->last = chunk;
    }

    list->count++;
    return &chunk->commands[chunk->count++];
}

/* Every append funnels through here so the null and overflow handling is
 * written once. */
static int32_t schultz_draw_emit(schultz_draw_list *list, uint32_t kind,
                            schultz_draw_cmd **out_cmd)
{
    schultz_draw_cmd *cmd;

    if (list == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    cmd = schultz_draw_list_append(list);
    if (cmd == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    memset(cmd, 0, sizeof(*cmd));
    cmd->kind = kind;
    *out_cmd  = cmd;
    return SCHULTZ_OK;
}

int32_t schultz_draw_list_replay_one(schultz_draw_list *list,
                                     const schultz_draw_cmd *cmd)
{
    schultz_draw_cmd *slot;
    int32_t result;

    if (cmd == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_draw_emit(list, cmd->kind, &slot);
    if (result != SCHULTZ_OK) {
        return result;
    }
    *slot = *cmd;
    return SCHULTZ_OK;
}

int32_t schultz_draw_fill_rect(schultz_draw_list *list, schultz_rect rect,
                               schultz_paint paint)
{
    schultz_draw_cmd *cmd;
    int32_t result = schultz_draw_emit(list, SCHULTZ_DRAW_FILL_RECT, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.fill_rect.rect  = rect;
    cmd->as.fill_rect.paint = paint;
    return SCHULTZ_OK;
}

int32_t schultz_draw_stroke_rect(schultz_draw_list *list, schultz_rect rect,
                                 schultz_stroke stroke)
{
    schultz_draw_cmd *cmd;
    int32_t result = schultz_draw_emit(list, SCHULTZ_DRAW_STROKE_RECT, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.stroke_rect.rect  = rect;
    cmd->as.stroke_rect.stroke = stroke;
    return SCHULTZ_OK;
}

int32_t schultz_draw_fill_round_rect(schultz_draw_list *list,
                                     schultz_rect rect, schultz_paint paint,
                                     float radius)
{
    schultz_draw_cmd *cmd;
    int32_t result = schultz_draw_emit(list, SCHULTZ_DRAW_FILL_ROUND_RECT, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.fill_round_rect.rect   = rect;
    cmd->as.fill_round_rect.paint  = paint;
    cmd->as.fill_round_rect.radius = radius;
    return SCHULTZ_OK;
}

int32_t schultz_draw_stroke_round_rect(schultz_draw_list *list,
                                       schultz_rect rect,
                                       schultz_stroke stroke, float radius)
{
    schultz_draw_cmd *cmd;
    int32_t result = schultz_draw_emit(list, SCHULTZ_DRAW_STROKE_ROUND_RECT, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.stroke_round_rect.rect   = rect;
    cmd->as.stroke_round_rect.stroke = stroke;
    cmd->as.stroke_round_rect.radius = radius;
    return SCHULTZ_OK;
}

int32_t schultz_draw_fill_ellipse(schultz_draw_list *list, schultz_rect rect,
                                  schultz_paint paint)
{
    schultz_draw_cmd *cmd;
    int32_t result = schultz_draw_emit(list, SCHULTZ_DRAW_FILL_ELLIPSE, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.fill_ellipse.rect  = rect;
    cmd->as.fill_ellipse.paint = paint;
    return SCHULTZ_OK;
}

int32_t schultz_draw_stroke_ellipse(schultz_draw_list *list,
                                    schultz_rect rect, schultz_stroke stroke)
{
    schultz_draw_cmd *cmd;
    int32_t result = schultz_draw_emit(list, SCHULTZ_DRAW_STROKE_ELLIPSE,
                                       &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.stroke_ellipse.rect  = rect;
    cmd->as.stroke_ellipse.stroke = stroke;
    return SCHULTZ_OK;
}

int32_t schultz_draw_line(schultz_draw_list *list, schultz_point from,
                          schultz_point to, schultz_stroke stroke)
{
    schultz_draw_cmd *cmd;
    int32_t result = schultz_draw_emit(list, SCHULTZ_DRAW_LINE, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.line.from  = from;
    cmd->as.line.to    = to;
    cmd->as.line.stroke = stroke;
    return SCHULTZ_OK;
}

int32_t schultz_draw_fill_polygon(schultz_draw_list *list,
                                  const schultz_point *points, uint32_t count,
                                  schultz_paint paint, uint32_t rule)
{
    schultz_draw_cmd *cmd;
    schultz_point *copy;
    int32_t result;

    if (list == NULL || points == NULL || count == 0 ||
        rule > SCHULTZ_FILL_EVEN_ODD) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    /*
     * The points are copied because the command list outlives the caller's
     * stack frame. The copy lives in the same arena as the commands, so it
     * dies with them at reset.
     */
    copy = (schultz_point *)schultz_arena_alloc(list->arena,
                                      (size_t)count * sizeof(*copy),
                                      _Alignof(schultz_point));
    if (copy == NULL) {
        list->overflowed = 1;
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, points, (size_t)count * sizeof(*copy));

    result = schultz_draw_emit(list, SCHULTZ_DRAW_FILL_POLYGON, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.fill_polygon.points = copy;
    cmd->as.fill_polygon.count  = count;
    cmd->as.fill_polygon.paint  = paint;
    cmd->as.fill_polygon.rule   = rule;
    return SCHULTZ_OK;
}
int32_t schultz_draw_stroke_polygon(schultz_draw_list *list,
                                    const schultz_point *points, uint32_t count,
                                    schultz_stroke stroke, int32_t closed)
{
    schultz_draw_cmd *cmd;
    schultz_point *copy;
    int32_t result;

    /* Two points is the least that has a direction to be drawn along. */
    if (list == NULL || points == NULL || count < 2u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    copy = (schultz_point *)schultz_arena_alloc(list->arena,
                                      (size_t)count * sizeof(*copy),
                                      _Alignof(schultz_point));
    if (copy == NULL) {
        list->overflowed = 1;
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, points, (size_t)count * sizeof(*copy));

    result = schultz_draw_emit(list, SCHULTZ_DRAW_STROKE_POLYGON, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.stroke_polygon.points = copy;
    cmd->as.stroke_polygon.count  = count;
    cmd->as.stroke_polygon.stroke = stroke;
    cmd->as.stroke_polygon.closed = closed ? 1u : 0u;
    return SCHULTZ_OK;
}


int32_t schultz_draw_image(schultz_draw_list *list, schultz_handle image, schultz_rect source,
                      schultz_rect dest, uint8_t opacity)
{
    schultz_draw_cmd *cmd;
    int32_t result;

    if (image == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    result = schultz_draw_emit(list, SCHULTZ_DRAW_IMAGE, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.image.image   = image;
    cmd->as.image.source  = source;
    cmd->as.image.dest    = dest;
    cmd->as.image.opacity = opacity;
    return SCHULTZ_OK;
}

/*
 * Checks a path and works out how much room the copy needs.
 *
 * A quadratic becomes a cubic, so the copy can hold one more point per
 * quadratic than the caller passed. Everything else is copied as it stands.
 */
static int32_t schultz_path_measure(const uint8_t *steps, uint32_t step_count,
                                    uint32_t point_count,
                                    uint32_t *out_points)
{
    uint32_t wanted = 0u;
    uint32_t stored = 0u;
    uint32_t i;

    if (steps == NULL || step_count == 0u ||
        steps[0] != (uint8_t)SCHULTZ_PATH_MOVE) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < step_count; i++) {
        uint32_t step = steps[i];

        if (step > (uint32_t)SCHULTZ_PATH_QUAD) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
        wanted += schultz_path_step_points(step);
        stored += (step == (uint32_t)SCHULTZ_PATH_QUAD)
                      ? 3u : schultz_path_step_points(step);
    }
    /*
     * Exactly, not at least. A path with points left over is a path where the
     * two arrays disagree, and drawing it would quietly use the wrong ones.
     */
    if (wanted != point_count) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_points = stored;
    return SCHULTZ_OK;
}

/*
 * Copies a path, raising every quadratic to the cubic that draws the same
 * curve. A quadratic with control Q between ends P0 and P2 is the cubic whose
 * controls are a third of the way from each end towards Q:
 *
 *     C1 = P0 + 2/3 (Q - P0)      C2 = P2 + 2/3 (Q - P2)
 *
 * That is exact, not an approximation: every quadratic is a cubic already.
 */
static void schultz_path_copy(const uint8_t *steps, uint32_t step_count,
                              const schultz_point *points, uint8_t *out_steps,
                              schultz_point *out_points)
{
    schultz_point pen = { 0.0f, 0.0f };
    uint32_t from = 0u;
    uint32_t to = 0u;
    uint32_t i;

    for (i = 0; i < step_count; i++) {
        uint32_t step = steps[i];

        if (step == (uint32_t)SCHULTZ_PATH_QUAD) {
            schultz_point control = points[from];
            schultz_point end = points[from + 1u];

            out_steps[i] = (uint8_t)SCHULTZ_PATH_CURVE;
            out_points[to++] = schultz_point_make(
                pen.x + 2.0f / 3.0f * (control.x - pen.x),
                pen.y + 2.0f / 3.0f * (control.y - pen.y));
            out_points[to++] = schultz_point_make(
                end.x + 2.0f / 3.0f * (control.x - end.x),
                end.y + 2.0f / 3.0f * (control.y - end.y));
            out_points[to++] = end;
            from += 2u;
            pen = end;
            continue;
        }

        out_steps[i] = (uint8_t)step;
        {
            uint32_t n = schultz_path_step_points(step);
            uint32_t j;

            for (j = 0; j < n; j++) {
                out_points[to++] = points[from++];
            }
            if (n > 0u) {
                pen = out_points[to - 1u];
            }
        }
    }
}

/* Both path emitters check, allocate and copy the same way. */
static int32_t schultz_draw_path(schultz_draw_list *list, uint32_t kind,
                                 const uint8_t *steps, uint32_t step_count,
                                 const schultz_point *points,
                                 uint32_t point_count, uint8_t **out_steps,
                                 schultz_point **out_points,
                                 uint32_t *out_point_count,
                                 schultz_draw_cmd **out_cmd)
{
    uint8_t *step_copy;
    schultz_point *point_copy;
    uint32_t stored = 0u;
    int32_t result;

    if (list == NULL || points == NULL || point_count == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_path_measure(steps, step_count, point_count, &stored);
    if (result != SCHULTZ_OK) {
        return result;
    }

    step_copy = (uint8_t *)schultz_arena_alloc(list->arena, step_count,
                                               _Alignof(uint8_t));
    point_copy = (schultz_point *)schultz_arena_alloc(list->arena,
                     (size_t)stored * sizeof(*point_copy),
                     _Alignof(schultz_point));
    if (step_copy == NULL || point_copy == NULL) {
        list->overflowed = 1;
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    schultz_path_copy(steps, step_count, points, step_copy, point_copy);

    result = schultz_draw_emit(list, kind, out_cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    *out_steps       = step_copy;
    *out_points      = point_copy;
    *out_point_count = stored;
    return SCHULTZ_OK;
}

int32_t schultz_draw_fill_path(schultz_draw_list *list, const uint8_t *steps,
                               uint32_t step_count,
                               const schultz_point *points,
                               uint32_t point_count, schultz_paint paint,
                               uint32_t rule)
{
    schultz_draw_cmd *cmd = NULL;
    uint8_t *step_copy = NULL;
    schultz_point *point_copy = NULL;
    uint32_t stored = 0u;
    int32_t result;

    if (rule > (uint32_t)SCHULTZ_FILL_EVEN_ODD) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_draw_path(list, SCHULTZ_DRAW_FILL_PATH, steps,
                               step_count, points, point_count, &step_copy,
                               &point_copy, &stored, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.fill_path.steps       = step_copy;
    cmd->as.fill_path.step_count  = step_count;
    cmd->as.fill_path.points      = point_copy;
    cmd->as.fill_path.point_count = stored;
    cmd->as.fill_path.paint       = paint;
    cmd->as.fill_path.rule        = rule;
    return SCHULTZ_OK;
}

int32_t schultz_draw_stroke_path(schultz_draw_list *list,
                                 const uint8_t *steps, uint32_t step_count,
                                 const schultz_point *points,
                                 uint32_t point_count, schultz_stroke stroke)
{
    schultz_draw_cmd *cmd = NULL;
    uint8_t *step_copy = NULL;
    schultz_point *point_copy = NULL;
    uint32_t stored = 0u;
    int32_t result = schultz_draw_path(list, SCHULTZ_DRAW_STROKE_PATH, steps,
                                       step_count, points, point_count,
                                       &step_copy, &point_copy, &stored,
                                       &cmd);

    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.stroke_path.steps       = step_copy;
    cmd->as.stroke_path.step_count  = step_count;
    cmd->as.stroke_path.points      = point_copy;
    cmd->as.stroke_path.point_count = stored;
    cmd->as.stroke_path.stroke      = stroke;
    return SCHULTZ_OK;
}

int32_t schultz_draw_glyph_run(schultz_draw_list *list, schultz_handle font,
                          const schultz_glyph *glyphs, uint32_t count,
                          schultz_paint paint)
{
    schultz_draw_cmd *cmd;
    schultz_glyph *copy;
    int32_t result;

    if (list == NULL || glyphs == NULL || count == 0 ||
        font == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    copy = (schultz_glyph *)schultz_arena_alloc(list->arena,
                                      (size_t)count * sizeof(*copy),
                                      _Alignof(schultz_glyph));
    if (copy == NULL) {
        list->overflowed = 1;
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, glyphs, (size_t)count * sizeof(*copy));

    result = schultz_draw_emit(list, SCHULTZ_DRAW_GLYPH_RUN, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.glyph_run.font   = font;
    cmd->as.glyph_run.glyphs = copy;
    cmd->as.glyph_run.count  = count;
    cmd->as.glyph_run.paint  = paint;
    return SCHULTZ_OK;
}

int32_t schultz_draw_clip_begin(schultz_draw_list *list, schultz_rect rect)
{
    schultz_draw_cmd *cmd;
    int32_t result = schultz_draw_emit(list, SCHULTZ_DRAW_CLIP_BEGIN, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.clip_begin.rect = rect;
    return SCHULTZ_OK;
}

int32_t schultz_draw_clip_end(schultz_draw_list *list)
{
    schultz_draw_cmd *cmd;
    return schultz_draw_emit(list, SCHULTZ_DRAW_CLIP_END, &cmd);
}

int32_t schultz_draw_offset_begin(schultz_draw_list *list, float dx, float dy)
{
    schultz_draw_cmd *cmd;
    int32_t result = schultz_draw_emit(list, SCHULTZ_DRAW_OFFSET_BEGIN, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.offset_begin.dx = dx;
    cmd->as.offset_begin.dy = dy;
    return SCHULTZ_OK;
}

int32_t schultz_draw_glyph_outline(schultz_draw_list *list,
                                   schultz_handle font,
                                   const schultz_glyph *glyphs,
                                   uint32_t count, schultz_stroke stroke)
{
    schultz_draw_cmd *cmd;
    schultz_glyph *copy;
    int32_t result;

    if (list == NULL || glyphs == NULL || count == 0 ||
        font == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    copy = (schultz_glyph *)schultz_arena_alloc(list->arena,
                                      (size_t)count * sizeof(*copy),
                                      _Alignof(schultz_glyph));
    if (copy == NULL) {
        list->overflowed = 1;
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, glyphs, (size_t)count * sizeof(*copy));

    result = schultz_draw_emit(list, SCHULTZ_DRAW_GLYPH_OUTLINE, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.glyph_outline.font   = font;
    cmd->as.glyph_outline.glyphs = copy;
    cmd->as.glyph_outline.count  = count;
    cmd->as.glyph_outline.stroke = stroke;
    return SCHULTZ_OK;
}

int32_t schultz_draw_rotation_begin(schultz_draw_list *list, float degrees,
                                   float cx, float cy)
{
    schultz_draw_cmd *cmd;
    int32_t result = schultz_draw_emit(list, SCHULTZ_DRAW_ROTATION_BEGIN,
                                       &cmd);

    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.rotation_begin.degrees = degrees;
    cmd->as.rotation_begin.cx      = cx;
    cmd->as.rotation_begin.cy      = cy;
    return SCHULTZ_OK;
}

int32_t schultz_draw_rotation_end(schultz_draw_list *list)
{
    schultz_draw_cmd *cmd;

    return schultz_draw_emit(list, SCHULTZ_DRAW_ROTATION_END, &cmd);
}

int32_t schultz_draw_offset_end(schultz_draw_list *list)
{
    schultz_draw_cmd *cmd;
    return schultz_draw_emit(list, SCHULTZ_DRAW_OFFSET_END, &cmd);
}

int32_t schultz_draw_device_pixels_begin(schultz_draw_list *list)
{
    schultz_draw_cmd *cmd;
    return schultz_draw_emit(list, SCHULTZ_DRAW_DEVICE_PIXELS_BEGIN, &cmd);
}

int32_t schultz_draw_device_pixels_end(schultz_draw_list *list)
{
    schultz_draw_cmd *cmd;
    return schultz_draw_emit(list, SCHULTZ_DRAW_DEVICE_PIXELS_END, &cmd);
}

int32_t schultz_draw_group_begin(schultz_draw_list *list, float opacity,
                                schultz_shadow shadow)
{
    schultz_draw_cmd *cmd;
    int32_t result = schultz_draw_emit(list, SCHULTZ_DRAW_GROUP_BEGIN, &cmd);
    if (result != SCHULTZ_OK) {
        return result;
    }
    cmd->as.group_begin.opacity = opacity;
    cmd->as.group_begin.shadow  = shadow;
    return SCHULTZ_OK;
}

int32_t schultz_draw_group_end(schultz_draw_list *list)
{
    schultz_draw_cmd *cmd;
    return schultz_draw_emit(list, SCHULTZ_DRAW_GROUP_END, &cmd);
}

static int32_t schultz_painter_vtable_complete(const schultz_painter_vtable *v)
{
    return (v->begin && v->end && v->fill_rect && v->stroke_rect &&
            v->fill_round_rect && v->stroke_round_rect &&
            v->fill_ellipse && v->stroke_ellipse && v->line &&
            v->fill_polygon && v->stroke_polygon &&
            v->fill_path && v->stroke_path && v->image &&
            v->glyph_run && v->clip_begin &&
            v->clip_end && v->offset_begin && v->offset_end &&
            v->rotation_begin && v->rotation_end && v->glyph_outline &&
            v->device_pixels_begin && v->device_pixels_end &&
            v->group_begin && v->group_end) ? 1 : 0;
}

static int32_t schultz_painter_call(const schultz_painter *painter,
                                   const schultz_draw_cmd *cmd)
{
    const schultz_painter_vtable *v = painter->vtable;
    void *ctx = painter->context;

    switch (cmd->kind) {
    case SCHULTZ_DRAW_FILL_RECT:         return v->fill_rect(ctx, cmd);
    case SCHULTZ_DRAW_STROKE_RECT:       return v->stroke_rect(ctx, cmd);
    case SCHULTZ_DRAW_FILL_ROUND_RECT:   return v->fill_round_rect(ctx, cmd);
    case SCHULTZ_DRAW_STROKE_ROUND_RECT: return v->stroke_round_rect(ctx, cmd);
    case SCHULTZ_DRAW_FILL_ELLIPSE:      return v->fill_ellipse(ctx, cmd);
    case SCHULTZ_DRAW_STROKE_ELLIPSE:    return v->stroke_ellipse(ctx, cmd);
    case SCHULTZ_DRAW_LINE:              return v->line(ctx, cmd);
    case SCHULTZ_DRAW_FILL_POLYGON:         return v->fill_polygon(ctx, cmd);
    case SCHULTZ_DRAW_STROKE_POLYGON:       return v->stroke_polygon(ctx, cmd);
    case SCHULTZ_DRAW_FILL_PATH:            return v->fill_path(ctx, cmd);
    case SCHULTZ_DRAW_STROKE_PATH:          return v->stroke_path(ctx, cmd);
    case SCHULTZ_DRAW_IMAGE:             return v->image(ctx, cmd);
    case SCHULTZ_DRAW_GLYPH_RUN:         return v->glyph_run(ctx, cmd);
    case SCHULTZ_DRAW_CLIP_BEGIN:         return v->clip_begin(ctx, cmd);
    case SCHULTZ_DRAW_CLIP_END:          return v->clip_end(ctx);
    case SCHULTZ_DRAW_OFFSET_BEGIN:    return v->offset_begin(ctx, cmd);
    case SCHULTZ_DRAW_OFFSET_END:     return v->offset_end(ctx);
    case SCHULTZ_DRAW_GLYPH_OUTLINE:     return v->glyph_outline(ctx, cmd);
    case SCHULTZ_DRAW_ROTATION_BEGIN:     return v->rotation_begin(ctx, cmd);
    case SCHULTZ_DRAW_ROTATION_END:      return v->rotation_end(ctx);
    case SCHULTZ_DRAW_DEVICE_PIXELS_BEGIN:
        return v->device_pixels_begin(ctx);
    case SCHULTZ_DRAW_DEVICE_PIXELS_END:
        return v->device_pixels_end(ctx);
    case SCHULTZ_DRAW_GROUP_BEGIN:        return v->group_begin(ctx, cmd);
    case SCHULTZ_DRAW_GROUP_END:         return v->group_end(ctx);
    default:                        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
}

int32_t schultz_draw_list_play(const schultz_draw_list *list, const schultz_painter *painter,
                          schultz_rect bounds)
{
    const schultz_draw_chunk *chunk;
    int32_t result;
    int32_t end_result;

    if (list == NULL || painter == NULL || painter->vtable == NULL ||
        !schultz_painter_vtable_complete(painter->vtable)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    result = painter->vtable->begin(painter->context, bounds);
    if (result != SCHULTZ_OK) {
        return result;
    }

    for (chunk = list->first; chunk != NULL; chunk = chunk->next) {
        uint32_t i;
        for (i = 0; i < chunk->count; i++) {
            result = schultz_painter_call(painter, &chunk->commands[i]);
            if (result != SCHULTZ_OK) {
                /* Still let the backend unwind. */
                painter->vtable->end(painter->context);
                return result;
            }
        }
    }

    end_result = painter->vtable->end(painter->context);
    return end_result;
}
