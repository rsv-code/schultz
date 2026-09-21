/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * fake_painter.h - recording painter for tests.
 *
 * Header only, with static functions, because each test binary is a single
 * translation unit. Include it and use fake_painter_init.
 *
 * This is the seam the architecture was designed around: paint behavior is
 * asserted by inspecting the commands a widget produced, with no rasterizer
 * and no window. It is meant to be reused by every test that needs a painter.
 */

#ifndef FAKE_PAINTER_H
#define FAKE_PAINTER_H

#include <string.h>

#include "schultz_paint.h"

enum { FAKE_PAINTER_MAX = 256 };

typedef struct {
    schultz_draw_cmd commands[FAKE_PAINTER_MAX];
    uint32_t    count;
    uint32_t    begin_calls;
    uint32_t    end_calls;
    schultz_rect     bounds;

    int32_t     clip_depth;
    int32_t     max_clip_depth;
    int32_t     transform_depth;
    int32_t     rotation_depth;
    int32_t     group_depth;
    int32_t     max_group_depth;

    /* Set to make the Nth recorded command fail, to test error paths. */
    int32_t     fail_at;
    int32_t     fail_result;
} fake_painter;

static int32_t fake_painter_record(void *context, const schultz_draw_cmd *cmd)
{
    fake_painter *fake = (fake_painter *)context;

    if (fake->fail_at >= 0 && (int32_t)fake->count == fake->fail_at) {
        return fake->fail_result;
    }
    if (fake->count < FAKE_PAINTER_MAX) {
        fake->commands[fake->count] = *cmd;
    }
    fake->count++;
    return SCHULTZ_OK;
}

static int32_t fake_painter_begin(void *context, schultz_rect bounds)
{
    fake_painter *fake = (fake_painter *)context;
    fake->begin_calls++;
    fake->bounds = bounds;
    return SCHULTZ_OK;
}

static int32_t fake_painter_end(void *context)
{
    fake_painter *fake = (fake_painter *)context;
    fake->end_calls++;
    return SCHULTZ_OK;
}

static int32_t fake_painter_push_clip(void *context, const schultz_draw_cmd *cmd)
{
    fake_painter *fake = (fake_painter *)context;
    int32_t result = fake_painter_record(context, cmd);
    if (result == SCHULTZ_OK) {
        fake->clip_depth++;
        if (fake->clip_depth > fake->max_clip_depth) {
            fake->max_clip_depth = fake->clip_depth;
        }
    }
    return result;
}

static int32_t fake_painter_pop_clip(void *context)
{
    fake_painter *fake = (fake_painter *)context;
    schultz_draw_cmd cmd;
    int32_t result;

    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = SCHULTZ_DRAW_CLIP_END;
    result = fake_painter_record(context, &cmd);
    if (result == SCHULTZ_OK) {
        fake->clip_depth--;
    }
    return result;
}

static int32_t fake_painter_push_rotation(void *context,
                                          const schultz_draw_cmd *cmd)
{
    fake_painter *fake = (fake_painter *)context;
    int32_t result = fake_painter_record(context, cmd);
    if (result == SCHULTZ_OK) {
        fake->rotation_depth++;
    }
    return result;
}

static int32_t fake_painter_pop_rotation(void *context)
{
    fake_painter *fake = (fake_painter *)context;
    schultz_draw_cmd cmd;
    int32_t result;

    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = SCHULTZ_DRAW_ROTATION_END;
    result = fake_painter_record(context, &cmd);
    if (result == SCHULTZ_OK) {
        fake->rotation_depth--;
    }
    return result;
}

static int32_t fake_painter_push_transform(void *context,
                                           const schultz_draw_cmd *cmd)
{
    fake_painter *fake = (fake_painter *)context;
    int32_t result = fake_painter_record(context, cmd);
    if (result == SCHULTZ_OK) {
        fake->transform_depth++;
    }
    return result;
}

static int32_t fake_painter_pop_transform(void *context)
{
    fake_painter *fake = (fake_painter *)context;
    schultz_draw_cmd cmd;
    int32_t result;

    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = SCHULTZ_DRAW_OFFSET_END;
    result = fake_painter_record(context, &cmd);
    if (result == SCHULTZ_OK) {
        fake->transform_depth--;
    }
    return result;
}

/* A group, recorded with its depth kept so a test can check it balances. */
static int32_t fake_painter_push_group(void *context,
                                       const schultz_draw_cmd *cmd)
{
    fake_painter *fake = (fake_painter *)context;
    int32_t result = fake_painter_record(context, cmd);
    if (result == SCHULTZ_OK) {
        fake->group_depth++;
        if (fake->group_depth > fake->max_group_depth) {
            fake->max_group_depth = fake->group_depth;
        }
    }
    return result;
}

static int32_t fake_painter_pop_group(void *context)
{
    fake_painter *fake = (fake_painter *)context;
    schultz_draw_cmd cmd;
    int32_t result;

    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = SCHULTZ_DRAW_GROUP_END;
    result = fake_painter_record(context, &cmd);
    if (result == SCHULTZ_OK) {
        fake->group_depth--;
    }
    return result;
}

/* The unit a canvas may switch to. Recorded like any other command. */
static int32_t fake_painter_push_device_pixels(void *context)
{
    schultz_draw_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = SCHULTZ_DRAW_DEVICE_PIXELS_BEGIN;
    return fake_painter_record(context, &cmd);
}

static int32_t fake_painter_pop_device_pixels(void *context)
{
    schultz_draw_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = SCHULTZ_DRAW_DEVICE_PIXELS_END;
    return fake_painter_record(context, &cmd);
}

static const schultz_painter_vtable fake_painter_vtable = {
    fake_painter_begin,
    fake_painter_end,
    fake_painter_record, /* fill_rect */
    fake_painter_record, /* stroke_rect */
    fake_painter_record, /* fill_round_rect */
    fake_painter_record, /* stroke_round_rect */
    fake_painter_record, /* fill_ellipse */
    fake_painter_record, /* stroke_ellipse */
    fake_painter_record, /* line */
    fake_painter_record, /* fill_polygon */
    fake_painter_record, /* stroke_polygon */
    fake_painter_record, /* fill_path */
    fake_painter_record, /* stroke_path */
    fake_painter_record, /* image */
    fake_painter_record, /* glyph_run */
    fake_painter_record, /* glyph_outline */
    fake_painter_push_clip,
    fake_painter_pop_clip,
    fake_painter_push_transform,
    fake_painter_pop_transform,
    fake_painter_push_rotation,
    fake_painter_pop_rotation,
    fake_painter_push_group,
    fake_painter_pop_group,
    fake_painter_push_device_pixels,
    fake_painter_pop_device_pixels
};

static void fake_painter_init(fake_painter *fake, schultz_painter *painter)
{
    memset(fake, 0, sizeof(*fake));
    fake->fail_at     = -1;
    fake->fail_result = SCHULTZ_OK;
    painter->vtable   = &fake_painter_vtable;
    painter->context  = fake;
}

/* Kind of the Nth command the painter saw, or 0 when out of range. */
static uint32_t fake_painter_kind_at(const fake_painter *fake, uint32_t index)
{
    if (index >= fake->count || index >= FAKE_PAINTER_MAX) {
        return 0;
    }
    return fake->commands[index].kind;
}

#endif /* FAKE_PAINTER_H */
