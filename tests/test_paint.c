/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_paint.c - draw command list and painter callback.
 *
 * The exit criterion for this phase is that a command list built by hand
 * plays back correctly. These tests assert the commands a caller produced and
 * the order a backend sees them in, using the recording fake painter.
 */

#include <string.h>

#include "greatest.h"
#include "fake_painter.h"
#include "schultz_paint.h"

/* Every test gets a fresh arena and list. */
typedef struct {
    schultz_arena     arena;
    schultz_draw_list list;
} paint_fixture;

static int32_t fixture_setup(paint_fixture *fixture, uint32_t chunk_capacity)
{
    int32_t result = schultz_arena_init(&fixture->arena, 4096);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_draw_list_init(&fixture->list, &fixture->arena, chunk_capacity);
}

static void fixture_teardown(paint_fixture *fixture)
{
    schultz_arena_free(&fixture->arena);
}

TEST list_starts_empty(void)
{
    paint_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    ASSERT_EQ(0u, schultz_draw_list_count(&f.list));
    ASSERT_EQ(NULL, schultz_draw_list_at(&f.list, 0));
    ASSERT_EQ(0, schultz_draw_list_overflowed(&f.list));

    fixture_teardown(&f);
    PASS();
}

TEST init_rejects_null_arguments(void)
{
    schultz_arena arena;
    schultz_draw_list list;

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 1024));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT, schultz_draw_list_init(NULL, &arena, 0));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT, schultz_draw_list_init(&list, NULL, 0));
    schultz_arena_free(&arena);
    PASS();
}

TEST fill_rect_records_its_arguments(void)
{
    paint_fixture f;
    const schultz_draw_cmd *cmd;
    schultz_rect rect = schultz_rect_make(1, 2, 3, 4);
    schultz_color color = schultz_color_rgba(10, 20, 30, 40);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_rect(&f.list, rect, schultz_paint_solid(color)));
    ASSERT_EQ(1u, schultz_draw_list_count(&f.list));

    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT(cmd != NULL);
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_FILL_RECT, cmd->kind);
    ASSERT_EQ(1, schultz_rect_equals(rect, cmd->as.fill_rect.rect));
    ASSERT_EQ(1, schultz_color_equals(color, cmd->as.fill_rect.paint.as.color));

    fixture_teardown(&f);
    PASS();
}

TEST every_command_kind_round_trips(void)
{
    paint_fixture f;
    schultz_point points[3];
    schultz_glyph glyphs[2];
    schultz_color color = schultz_color_rgba(1, 2, 3, 4);
    schultz_rect rect = schultz_rect_make(0, 0, 10, 10);
    uint32_t i;
    static const uint32_t expected[] = {
        SCHULTZ_DRAW_FILL_RECT, SCHULTZ_DRAW_STROKE_RECT, SCHULTZ_DRAW_FILL_ROUND_RECT,
        SCHULTZ_DRAW_STROKE_ROUND_RECT, SCHULTZ_DRAW_LINE,
        SCHULTZ_DRAW_FILL_POLYGON,
        SCHULTZ_DRAW_IMAGE, SCHULTZ_DRAW_GLYPH_RUN, SCHULTZ_DRAW_CLIP_BEGIN,
        SCHULTZ_DRAW_CLIP_END, SCHULTZ_DRAW_OFFSET_BEGIN, SCHULTZ_DRAW_OFFSET_END,
        SCHULTZ_DRAW_GROUP_BEGIN, SCHULTZ_DRAW_GROUP_END
    };

    points[0] = schultz_point_make(0, 0);
    points[1] = schultz_point_make(10, 0);
    points[2] = schultz_point_make(5, 10);
    glyphs[0].glyph_id = 7; glyphs[0].x = 1; glyphs[0].y = 2;
    glyphs[1].glyph_id = 8; glyphs[1].x = 3; glyphs[1].y = 4;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));

    ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_rect(&f.list, rect, schultz_paint_solid(color)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_stroke_rect(&f.list, rect, schultz_stroke_solid(color, 2.0f)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_round_rect(&f.list, rect, schultz_paint_solid(color), 4.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_stroke_round_rect(&f.list, rect, schultz_stroke_solid(color, 2.0f), 4.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_line(&f.list, points[0], points[1], schultz_stroke_solid(color, 1.0f)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_polygon(&f.list, points, 3,
                              schultz_paint_solid(color),
                              SCHULTZ_FILL_NONZERO));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_image(&f.list, 42, rect, rect, 255));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_glyph_run(&f.list, 99, glyphs, 2,
                                          schultz_paint_solid(color)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_clip_begin(&f.list, rect));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_clip_end(&f.list));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_offset_begin(&f.list, 5.0f, 6.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_offset_end(&f.list));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_group_begin(&f.list, 0.5f,
                                                  schultz_shadow_none()));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_group_end(&f.list));

    ASSERT_EQ(14u, schultz_draw_list_count(&f.list));
    for (i = 0; i < 14u; i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        ASSERT(cmd != NULL);
        ASSERT_EQ(expected[i], cmd->kind);
    }

    fixture_teardown(&f);
    PASS();
}

/*
 * The caller's arrays are stack locals in real paint code, so the list must
 * own a copy by the time the caller returns.
 */
TEST fill_polygon_copies_the_points(void)
{
    paint_fixture f;
    const schultz_draw_cmd *cmd;
    schultz_point points[3];

    points[0] = schultz_point_make(1, 1);
    points[1] = schultz_point_make(2, 2);
    points[2] = schultz_point_make(3, 3);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_polygon(&f.list, points, 3,
                                       schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255)),
                                       SCHULTZ_FILL_NONZERO));

    /* Scribble over the caller's array. */
    memset(points, 0, sizeof(points));

    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT(cmd != NULL);
    ASSERT(cmd->as.fill_polygon.points != points);
    ASSERT_EQ(3u, cmd->as.fill_polygon.count);
    ASSERT_EQ(1.0f, cmd->as.fill_polygon.points[0].x);
    ASSERT_EQ(3.0f, cmd->as.fill_polygon.points[2].y);

    fixture_teardown(&f);
    PASS();
}

TEST glyph_run_copies_the_glyphs(void)
{
    paint_fixture f;
    const schultz_draw_cmd *cmd;
    schultz_glyph glyphs[2];

    glyphs[0].glyph_id = 11; glyphs[0].x = 1.5f; glyphs[0].y = 2.5f;
    glyphs[1].glyph_id = 12; glyphs[1].x = 3.5f; glyphs[1].y = 4.5f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_glyph_run(&f.list, 5, glyphs, 2,
                                       schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255))));
    memset(glyphs, 0, sizeof(glyphs));

    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT(cmd != NULL);
    ASSERT(cmd->as.glyph_run.glyphs != glyphs);
    ASSERT_EQ(11u, cmd->as.glyph_run.glyphs[0].glyph_id);
    ASSERT_EQ(4.5f, cmd->as.glyph_run.glyphs[1].y);
    ASSERT_EQ((schultz_handle)5, cmd->as.glyph_run.font);

    fixture_teardown(&f);
    PASS();
}

TEST emitters_reject_bad_arguments(void)
{
    paint_fixture f;
    schultz_point points[1];
    schultz_glyph glyphs[1];
    schultz_color color = schultz_color_rgba(0, 0, 0, 255);
    schultz_rect rect = schultz_rect_make(0, 0, 1, 1);

    points[0] = schultz_point_make(0, 0);
    memset(glyphs, 0, sizeof(glyphs));

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT, schultz_draw_fill_rect(NULL, rect, schultz_paint_solid(color)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_fill_polygon(&f.list, NULL, 3,
                                        schultz_paint_solid(color),
                                        SCHULTZ_FILL_NONZERO));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_fill_polygon(&f.list, points, 0,
                                        schultz_paint_solid(color),
                                        SCHULTZ_FILL_NONZERO));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_glyph_run(&f.list, 1, NULL, 1,
                                     schultz_paint_solid(color)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_glyph_run(&f.list, SCHULTZ_HANDLE_NONE, glyphs, 1,
                                     schultz_paint_solid(color)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_image(&f.list, SCHULTZ_HANDLE_NONE, rect, rect, 255));
    ASSERT_EQ(0u, schultz_draw_list_count(&f.list));

    fixture_teardown(&f);
    PASS();
}

/* Commands must stay in order and stay intact across a chunk boundary. */
TEST list_spans_chunks_in_order(void)
{
    paint_fixture f;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 4)); /* tiny chunks on purpose */

    for (i = 0; i < 50u; i++) {
        schultz_rect rect = schultz_rect_make((float)i, 0, 1, 1);
        ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_rect(&f.list, rect,
                                           schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255))));
    }

    ASSERT_EQ(50u, schultz_draw_list_count(&f.list));
    for (i = 0; i < 50u; i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        ASSERT(cmd != NULL);
        ASSERT_EQ((float)i, cmd->as.fill_rect.rect.x);
    }
    ASSERT_EQ(NULL, schultz_draw_list_at(&f.list, 50));

    fixture_teardown(&f);
    PASS();
}

TEST reset_empties_the_list_and_reuses_chunks(void)
{
    paint_fixture f;
    uint32_t blocks_after_first_frame;
    uint32_t frame;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 8));

    for (i = 0; i < 40u; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_rect(&f.list,
                                           schultz_rect_make(0, 0, 1, 1),
                                           schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255))));
    }
    schultz_draw_list_reset(&f.list);
    ASSERT_EQ(0u, schultz_draw_list_count(&f.list));
    ASSERT_EQ(NULL, schultz_draw_list_at(&f.list, 0));

    blocks_after_first_frame = schultz_arena_block_count(&f.arena);

    /* Later frames must not take new memory from the arena. */
    for (frame = 0; frame < 10u; frame++) {
        for (i = 0; i < 40u; i++) {
            ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_rect(&f.list,
                                               schultz_rect_make(0, 0, 1, 1),
                                               schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255))));
        }
        ASSERT_EQ(40u, schultz_draw_list_count(&f.list));
        ASSERT_EQ(blocks_after_first_frame, schultz_arena_block_count(&f.arena));
        schultz_draw_list_reset(&f.list);
    }

    fixture_teardown(&f);
    PASS();
}

TEST play_delivers_commands_in_order(void)
{
    paint_fixture f;
    fake_painter fake;
    schultz_painter painter;
    schultz_rect bounds = schultz_rect_make(0, 0, 800, 600);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 4));
    fake_painter_init(&fake, &painter);

    ASSERT_EQ(SCHULTZ_OK, schultz_draw_clip_begin(&f.list, schultz_rect_make(0, 0, 100, 100)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_rect(&f.list, schultz_rect_make(1, 1, 2, 2),
                                       schultz_paint_solid(schultz_color_rgba(255, 0, 0, 255))));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_line(&f.list, schultz_point_make(0, 0),
                                  schultz_point_make(9, 9),
                                  schultz_stroke_solid(schultz_color_rgba(0, 255, 0, 255), 1.0f)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_clip_end(&f.list));

    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&f.list, &painter, bounds));

    ASSERT_EQ(1u, fake.begin_calls);
    ASSERT_EQ(1u, fake.end_calls);
    ASSERT_EQ(1, schultz_rect_equals(bounds, fake.bounds));
    ASSERT_EQ(4u, fake.count);
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_CLIP_BEGIN, fake_painter_kind_at(&fake, 0));
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_FILL_RECT, fake_painter_kind_at(&fake, 1));
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_LINE, fake_painter_kind_at(&fake, 2));
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_CLIP_END, fake_painter_kind_at(&fake, 3));

    /* Clip pushes and pops must balance. */
    ASSERT_EQ(0, fake.clip_depth);
    ASSERT_EQ(1, fake.max_clip_depth);

    fixture_teardown(&f);
    PASS();
}

TEST play_on_an_empty_list_still_brackets_the_frame(void)
{
    paint_fixture f;
    fake_painter fake;
    schultz_painter painter;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    fake_painter_init(&fake, &painter);

    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&f.list, &painter,
                                       schultz_rect_make(0, 0, 10, 10)));
    ASSERT_EQ(1u, fake.begin_calls);
    ASSERT_EQ(1u, fake.end_calls);
    ASSERT_EQ(0u, fake.count);

    fixture_teardown(&f);
    PASS();
}

TEST play_spans_chunks(void)
{
    paint_fixture f;
    fake_painter fake;
    schultz_painter painter;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 3));
    fake_painter_init(&fake, &painter);

    for (i = 0; i < 25u; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_rect(&f.list,
                                           schultz_rect_make((float)i, 0, 1, 1),
                                           schultz_paint_solid(
                                               schultz_color_rgba(0, 0, 0,
                                                                  255))));
    }
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&f.list, &painter,
                                       schultz_rect_make(0, 0, 10, 10)));

    ASSERT_EQ(25u, fake.count);
    for (i = 0; i < 25u; i++) {
        ASSERT_EQ((float)i, fake.commands[i].as.fill_rect.rect.x);
    }

    fixture_teardown(&f);
    PASS();
}

/* A backend error stops playback but must still let the backend unwind. */
TEST play_stops_at_a_backend_error_and_still_ends(void)
{
    paint_fixture f;
    fake_painter fake;
    schultz_painter painter;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    fake_painter_init(&fake, &painter);
    fake.fail_at     = 2;
    fake.fail_result = SCHULTZ_ERR_OUT_OF_MEMORY;

    for (i = 0; i < 6u; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_rect(&f.list,
                                           schultz_rect_make(0, 0, 1, 1),
                                           schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255))));
    }

    ASSERT_EQ(SCHULTZ_ERR_OUT_OF_MEMORY,
              schultz_draw_list_play(&f.list, &painter,
                                schultz_rect_make(0, 0, 10, 10)));
    ASSERT_EQ(2u, fake.count);
    ASSERT_EQ(1u, fake.end_calls);

    fixture_teardown(&f);
    PASS();
}

TEST play_rejects_an_incomplete_vtable(void)
{
    paint_fixture f;
    fake_painter fake;
    schultz_painter painter;
    schultz_painter_vtable broken;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    fake_painter_init(&fake, &painter);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_list_play(NULL, &painter, schultz_rect_make(0, 0, 1, 1)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_list_play(&f.list, NULL, schultz_rect_make(0, 0, 1, 1)));

    painter.vtable = NULL;
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_list_play(&f.list, &painter, schultz_rect_make(0, 0, 1, 1)));

    /* A vtable with one hole must be rejected, not silently skipped. */
    broken = fake_painter_vtable;
    broken.glyph_run = NULL;
    painter.vtable = &broken;
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_list_play(&f.list, &painter, schultz_rect_make(0, 0, 1, 1)));

    /* Including the newest pair, which a backend written before groups
     * existed would be missing. */
    broken = fake_painter_vtable;
    broken.group_begin = NULL;
    painter.vtable = &broken;
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_list_play(&f.list, &painter, schultz_rect_make(0, 0, 1, 1)));
    broken = fake_painter_vtable;
    broken.group_end = NULL;
    painter.vtable = &broken;
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_list_play(&f.list, &painter, schultz_rect_make(0, 0, 1, 1)));

    fixture_teardown(&f);
    PASS();
}

TEST transform_pushes_and_pops_balance(void)
{
    paint_fixture f;
    fake_painter fake;
    schultz_painter painter;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    fake_painter_init(&fake, &painter);

    ASSERT_EQ(SCHULTZ_OK, schultz_draw_offset_begin(&f.list, 10.0f, 20.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_offset_begin(&f.list, 1.0f, 2.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_offset_end(&f.list));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_offset_end(&f.list));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&f.list, &painter,
                                       schultz_rect_make(0, 0, 10, 10)));

    ASSERT_EQ(0, fake.transform_depth);
    ASSERT_EQ(10.0f, fake.commands[0].as.offset_begin.dx);
    ASSERT_EQ(20.0f, fake.commands[0].as.offset_begin.dy);

    fixture_teardown(&f);
    PASS();
}

/*
 * The two attributes a stroke gained after the first five. Both have defaults
 * that every caller gets for free, which is the point of the constructor.
 */
/*
 * A group carries its opacity to the backend and nests like the other pairs.
 *
 * The opacity is the whole payload: a backend that loses it draws the subtree
 * solid, which looks like the property doing nothing rather than like a bug.
 */
/* What a node has until something asks for a shadow. */
TEST a_shadow_starts_as_no_shadow(void)
{
    schultz_shadow shadow = schultz_shadow_none();

    /* Clear, which is what the backend takes as "draw nothing". */
    ASSERT_EQ(0u, shadow.color.a);
    /* Below, which is where a shadow goes when nobody says otherwise. */
    ASSERT_EQ(180.0f, shadow.angle);
    ASSERT_EQ(0.0f, shadow.distance);
    ASSERT_EQ(0.0f, shadow.blur);
    PASS();
}

TEST group_pushes_and_pops_balance(void)
{
    paint_fixture f;
    fake_painter fake;
    schultz_painter painter;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    fake_painter_init(&fake, &painter);

    ASSERT_EQ(SCHULTZ_OK, schultz_draw_group_begin(&f.list, 0.5f,
                                                  schultz_shadow_none()));
    {
        schultz_shadow shadow = schultz_shadow_none();

        shadow.color    = schultz_color_rgba(0, 0, 0, 128);
        shadow.angle    = 180.0f;
        shadow.distance = 4.0f;
        shadow.blur     = 2.0f;
        ASSERT_EQ(SCHULTZ_OK, schultz_draw_group_begin(&f.list, 0.25f,
                                                      shadow));
    }
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_group_end(&f.list));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_group_end(&f.list));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&f.list, &painter,
                                       schultz_rect_make(0, 0, 10, 10)));

    ASSERT_EQ(0, fake.group_depth);
    ASSERT_EQ(2, fake.max_group_depth);
    ASSERT_EQ(0.5f, fake.commands[0].as.group_begin.opacity);
    ASSERT_EQ(0.25f, fake.commands[1].as.group_begin.opacity);
    /* And the shadow rides along with it. */
    ASSERT_EQ(0u, fake.commands[0].as.group_begin.shadow.color.a);
    ASSERT_EQ(128u, fake.commands[1].as.group_begin.shadow.color.a);
    ASSERT_EQ(180.0f, fake.commands[1].as.group_begin.shadow.angle);
    ASSERT_EQ(4.0f, fake.commands[1].as.group_begin.shadow.distance);
    ASSERT_EQ(2.0f, fake.commands[1].as.group_begin.shadow.blur);
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_GROUP_END, fake.commands[2].kind);

    fixture_teardown(&f);
    PASS();
}

TEST a_stroke_carries_its_offset_and_its_miter_limit(void)
{
    paint_fixture f;
    const schultz_draw_cmd *cmd;
    schultz_stroke stroke = schultz_stroke_solid(
        schultz_color_rgba(1, 2, 3, 255), 2.0f);

    /* What a stroke starts out as, before anyone says otherwise. */
    ASSERT_EQ(0.0f, stroke.dash_offset);
    ASSERT_EQ(4.0f, stroke.miter_limit);

    stroke.dash_offset = 7.5f;
    stroke.miter_limit = 1.5f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_stroke_rect(&f.list,
                              schultz_rect_make(0, 0, 10, 10), stroke));

    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT(cmd != NULL);
    ASSERT_EQ(7.5f, cmd->as.stroke_rect.stroke.dash_offset);
    ASSERT_EQ(1.5f, cmd->as.stroke_rect.stroke.miter_limit);

    fixture_teardown(&f);
    PASS();
}

/* The rule rides on the command, and anything that is not one of the two is
 * refused rather than quietly treated as nonzero. */
TEST a_filled_path_records_its_fill_rule(void)
{
    paint_fixture f;
    const schultz_draw_cmd *cmd;
    schultz_paint paint = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));
    schultz_point points[3];

    points[0] = schultz_point_make(1, 1);
    points[1] = schultz_point_make(2, 2);
    points[2] = schultz_point_make(3, 3);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_polygon(&f.list, points, 3, paint,
                                                    SCHULTZ_FILL_EVEN_ODD));
    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT(cmd != NULL);
    ASSERT_EQ((uint32_t)SCHULTZ_FILL_EVEN_ODD, cmd->as.fill_polygon.rule);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_fill_polygon(&f.list, points, 3, paint, 99u));
    ASSERT_EQ(1u, schultz_draw_list_count(&f.list));

    fixture_teardown(&f);
    PASS();
}

/* A run carries a paint rather than a colour, so a gradient survives the trip
 * to the backend the way it does for every shape. */
TEST a_glyph_run_records_a_gradient_paint(void)
{
    paint_fixture f;
    const schultz_draw_cmd *cmd;
    schultz_glyph glyphs[2];

    glyphs[0].glyph_id = 11; glyphs[0].x = 1.0f; glyphs[0].y = 2.0f;
    glyphs[1].glyph_id = 12; glyphs[1].x = 3.0f; glyphs[1].y = 4.0f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_glyph_run(&f.list, 5, glyphs, 2,
                              schultz_paint_gradient((schultz_handle)77)));

    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT(cmd != NULL);
    ASSERT_EQ((uint32_t)SCHULTZ_PAINT_GRADIENT, cmd->as.glyph_run.paint.kind);
    ASSERT_EQ((schultz_handle)77, cmd->as.glyph_run.paint.as.gradient);

    fixture_teardown(&f);
    PASS();
}

/*
 * A path is two arrays read together, and the one thing that can silently go
 * wrong is the counts disagreeing. So the round trip is asserted and every
 * way of getting it wrong is refused.
 */
TEST a_curved_path_records_its_steps_and_its_points(void)
{
    paint_fixture f;
    const schultz_draw_cmd *cmd;
    uint8_t steps[4];
    schultz_point points[6];
    schultz_paint ink = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));

    steps[0] = SCHULTZ_PATH_MOVE;
    steps[1] = SCHULTZ_PATH_LINE;
    steps[2] = SCHULTZ_PATH_CURVE;
    steps[3] = SCHULTZ_PATH_CLOSE;
    points[0] = schultz_point_make(1, 1);   /* move */
    points[1] = schultz_point_make(9, 1);   /* line */
    points[2] = schultz_point_make(9, 5);   /* curve control one */
    points[3] = schultz_point_make(5, 9);   /* curve control two */
    points[4] = schultz_point_make(1, 9);   /* curve end */

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_path(&f.list, steps, 4u, points,
                                                 5u, ink,
                                                 SCHULTZ_FILL_NONZERO));
    /* Scribble over both of the caller's arrays. */
    memset(steps, 0, sizeof(steps));
    memset(points, 0, sizeof(points));

    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT(cmd != NULL);
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_FILL_PATH, cmd->kind);
    ASSERT_EQ(4u, cmd->as.fill_path.step_count);
    ASSERT_EQ(5u, cmd->as.fill_path.point_count);
    ASSERT_EQ((uint8_t)SCHULTZ_PATH_MOVE, cmd->as.fill_path.steps[0]);
    ASSERT_EQ((uint8_t)SCHULTZ_PATH_CLOSE, cmd->as.fill_path.steps[3]);
    ASSERT_EQ(9.0f, cmd->as.fill_path.points[1].x);
    ASSERT_EQ(9.0f, cmd->as.fill_path.points[4].y);

    fixture_teardown(&f);
    PASS();
}

TEST a_path_whose_counts_disagree_is_refused(void)
{
    paint_fixture f;
    uint8_t steps[2];
    schultz_point points[4];
    schultz_paint ink = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));

    points[0] = schultz_point_make(1, 1);
    points[1] = schultz_point_make(2, 2);
    points[2] = schultz_point_make(3, 3);
    points[3] = schultz_point_make(4, 4);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));

    /* A curve wants three points and is given one. */
    steps[0] = SCHULTZ_PATH_MOVE;
    steps[1] = SCHULTZ_PATH_CURVE;
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_fill_path(&f.list, steps, 2u, points, 2u, ink,
                                     SCHULTZ_FILL_NONZERO));
    /* Points left over is just as wrong as too few: a move and a line want
     * two, and four were handed over. */
    steps[1] = SCHULTZ_PATH_LINE;
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_fill_path(&f.list, steps, 2u, points, 4u, ink,
                                     SCHULTZ_FILL_NONZERO));
    /* A path has to say where it starts. */
    steps[0] = SCHULTZ_PATH_LINE;
    steps[1] = SCHULTZ_PATH_LINE;
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_fill_path(&f.list, steps, 2u, points, 2u, ink,
                                     SCHULTZ_FILL_NONZERO));
    /* And a step it has never heard of is not guessed at. */
    steps[0] = SCHULTZ_PATH_MOVE;
    steps[1] = 99u;
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_fill_path(&f.list, steps, 2u, points, 2u, ink,
                                     SCHULTZ_FILL_NONZERO));

    ASSERT_EQ(0u, schultz_draw_list_count(&f.list));
    fixture_teardown(&f);
    PASS();
}

/*
 * Every quadratic is a cubic already, so raising one loses nothing. The two
 * controls land two thirds of the way from each end towards the quadratic's
 * single control, and the command list holds no quadratics at all.
 */
TEST a_quadratic_curve_is_recorded_as_a_cubic(void)
{
    paint_fixture f;
    const schultz_draw_cmd *cmd;
    uint8_t steps[2];
    schultz_point points[3];
    schultz_paint ink = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));

    steps[0] = SCHULTZ_PATH_MOVE;
    steps[1] = SCHULTZ_PATH_QUAD;
    points[0] = schultz_point_make(0.0f, 0.0f);   /* move */
    points[1] = schultz_point_make(30.0f, 0.0f);  /* the one control */
    points[2] = schultz_point_make(30.0f, 30.0f); /* end */

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_path(&f.list, steps, 2u, points,
                                                 3u, ink,
                                                 SCHULTZ_FILL_NONZERO));

    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT(cmd != NULL);
    /* Two steps still, but the second is a curve and it took three points. */
    ASSERT_EQ(2u, cmd->as.fill_path.step_count);
    ASSERT_EQ((uint8_t)SCHULTZ_PATH_CURVE, cmd->as.fill_path.steps[1]);
    ASSERT_EQ(4u, cmd->as.fill_path.point_count);

    /* C1 = P0 + 2/3 (Q - P0) = (20, 0). */
    ASSERT_EQ(20.0f, cmd->as.fill_path.points[1].x);
    ASSERT_EQ(0.0f, cmd->as.fill_path.points[1].y);
    /* C2 = P2 + 2/3 (Q - P2) = (30, 10). */
    ASSERT_EQ(30.0f, cmd->as.fill_path.points[2].x);
    ASSERT_EQ(10.0f, cmd->as.fill_path.points[2].y);
    /* And the end is where it was. */
    ASSERT_EQ(30.0f, cmd->as.fill_path.points[3].x);
    ASSERT_EQ(30.0f, cmd->as.fill_path.points[3].y);

    fixture_teardown(&f);
    PASS();
}

/* A second move starts a second subpath, which is how a shape gets a hole. */
TEST a_path_with_two_subpaths_keeps_them_apart(void)
{
    paint_fixture f;
    const schultz_draw_cmd *cmd;
    uint8_t steps[6];
    schultz_point points[4];
    schultz_stroke pen = schultz_stroke_solid(schultz_color_rgba(0, 0, 0, 255),
                                              1.0f);

    steps[0] = SCHULTZ_PATH_MOVE;
    steps[1] = SCHULTZ_PATH_LINE;
    steps[2] = SCHULTZ_PATH_CLOSE;
    steps[3] = SCHULTZ_PATH_MOVE;
    steps[4] = SCHULTZ_PATH_LINE;
    steps[5] = SCHULTZ_PATH_CLOSE;
    points[0] = schultz_point_make(0, 0);
    points[1] = schultz_point_make(10, 0);
    points[2] = schultz_point_make(0, 20);
    points[3] = schultz_point_make(10, 20);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_stroke_path(&f.list, steps, 6u, points,
                                                   4u, pen));

    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT(cmd != NULL);
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_STROKE_PATH, cmd->kind);
    ASSERT_EQ(6u, cmd->as.stroke_path.step_count);
    ASSERT_EQ(4u, cmd->as.stroke_path.point_count);
    /* Both moves survived, so there really are two subpaths. */
    ASSERT_EQ((uint8_t)SCHULTZ_PATH_MOVE, cmd->as.stroke_path.steps[0]);
    ASSERT_EQ((uint8_t)SCHULTZ_PATH_MOVE, cmd->as.stroke_path.steps[3]);
    ASSERT_EQ(20.0f, cmd->as.stroke_path.points[2].y);

    fixture_teardown(&f);
    PASS();
}

/*
 * Rotations nest with offsets and with each other, and both stacks are the
 * painter's rather than the list's, so what the list has to get right is that
 * every push is paired and the commands arrive in order.
 */
TEST a_rotation_nests_inside_an_offset(void)
{
    paint_fixture f;
    fake_painter fake;
    schultz_painter painter;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    fake_painter_init(&fake, &painter);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_offset_begin(&f.list, 10.0f, 20.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_rotation_begin(&f.list, 30.0f, 5.0f,
                                                     6.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_rect(&f.list,
                              schultz_rect_make(0, 0, 4, 4),
                              schultz_paint_solid(
                                  schultz_color_rgba(0, 0, 0, 255))));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_rotation_end(&f.list));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_offset_end(&f.list));

    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&f.list, &painter,
                              schultz_rect_make(0, 0, 100, 100)));

    /* Both stacks came back to nothing, and the angle survived the trip. */
    ASSERT_EQ(0, fake.transform_depth);
    ASSERT_EQ(0, fake.rotation_depth);
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_OFFSET_BEGIN, fake.commands[0].kind);
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_ROTATION_BEGIN, fake.commands[1].kind);
    ASSERT_EQ(30.0f, fake.commands[1].as.rotation_begin.degrees);
    ASSERT_EQ(5.0f, fake.commands[1].as.rotation_begin.cx);
    ASSERT_EQ(6.0f, fake.commands[1].as.rotation_begin.cy);
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_ROTATION_END, fake.commands[3].kind);
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_OFFSET_END, fake.commands[4].kind);

    fixture_teardown(&f);
    PASS();
}

/* Two rotations deep and back out again, so a pop restores the one below
 * rather than clearing the lot. */
TEST popping_a_rotation_restores_what_was_there_before(void)
{
    paint_fixture f;
    fake_painter fake;
    schultz_painter painter;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 0));
    fake_painter_init(&fake, &painter);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_rotation_begin(&f.list, 10.0f, 0.0f,
                                                     0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_rotation_begin(&f.list, 20.0f, 1.0f,
                                                     1.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_rotation_end(&f.list));

    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&f.list, &painter,
                              schultz_rect_make(0, 0, 100, 100)));
    /* One left over, because only one of the two was popped. */
    ASSERT_EQ(1, fake.rotation_depth);
    ASSERT_EQ(20.0f, fake.commands[1].as.rotation_begin.degrees);

    fixture_teardown(&f);
    PASS();
}

SUITE(paint)
{
    RUN_TEST(list_starts_empty);
    RUN_TEST(init_rejects_null_arguments);
    RUN_TEST(fill_rect_records_its_arguments);
    RUN_TEST(every_command_kind_round_trips);
    RUN_TEST(fill_polygon_copies_the_points);
    RUN_TEST(glyph_run_copies_the_glyphs);
    RUN_TEST(emitters_reject_bad_arguments);
    RUN_TEST(list_spans_chunks_in_order);
    RUN_TEST(reset_empties_the_list_and_reuses_chunks);
    RUN_TEST(play_delivers_commands_in_order);
    RUN_TEST(play_on_an_empty_list_still_brackets_the_frame);
    RUN_TEST(play_spans_chunks);
    RUN_TEST(play_stops_at_a_backend_error_and_still_ends);
    RUN_TEST(play_rejects_an_incomplete_vtable);
    RUN_TEST(transform_pushes_and_pops_balance);
    RUN_TEST(a_shadow_starts_as_no_shadow);
    RUN_TEST(group_pushes_and_pops_balance);
    RUN_TEST(a_stroke_carries_its_offset_and_its_miter_limit);
    RUN_TEST(a_filled_path_records_its_fill_rule);
    RUN_TEST(a_glyph_run_records_a_gradient_paint);
    RUN_TEST(a_curved_path_records_its_steps_and_its_points);
    RUN_TEST(a_path_whose_counts_disagree_is_refused);
    RUN_TEST(a_quadratic_curve_is_recorded_as_a_cubic);
    RUN_TEST(a_path_with_two_subpaths_keeps_them_apart);
    RUN_TEST(a_rotation_nests_inside_an_offset);
    RUN_TEST(popping_a_rotation_restores_what_was_there_before);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(paint);
    GREATEST_MAIN_END();
}
