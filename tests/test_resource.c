/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_resource.c - registered gradients and dash patterns, and the paint
 * value that lets a style hold either a colour or a gradient.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_node.h"
#include "schultz_resource.h"
#include "schultz_widgets.h"

/* ------------------------------------------------------------- registry */

static void two_stops(schultz_gradient_stop *stops)
{
    stops[0].offset = 0.0f;
    stops[0].color  = schultz_color_rgba(255, 0, 0, 255);
    stops[1].offset = 1.0f;
    stops[1].color  = schultz_color_rgba(0, 0, 255, 255);
}

TEST a_registered_gradient_comes_back_as_it_went_in(void)
{
    schultz_resource_table *table = NULL;
    schultz_gradient_stop stops[2];
    schultz_handle handle = SCHULTZ_HANDLE_NONE;
    const schultz_gradient *back = NULL;

    two_stops(stops);
    ASSERT_EQ(SCHULTZ_OK, schultz_resource_table_create(&table));
    ASSERT_EQ(SCHULTZ_OK, schultz_gradient_linear(table,
                    schultz_point_make(0.0f, 0.0f),
                    schultz_point_make(1.0f, 1.0f), stops, 2, &handle));
    ASSERT(handle != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(SCHULTZ_OK, schultz_gradient_get(table, handle, &back));
    ASSERT_EQ((uint32_t)SCHULTZ_GRADIENT_LINEAR, back->kind);
    ASSERT_EQ(2u, back->count);
    ASSERT_EQ(0.0f, back->from.x);
    ASSERT_EQ(1.0f, back->to.y);
    ASSERT_EQ(255u, back->stops[0].color.r);
    ASSERT_EQ(255u, back->stops[1].color.b);

    schultz_resource_table_destroy(table);
    PASS();
}

TEST a_radial_gradient_keeps_its_centre_and_radius(void)
{
    schultz_resource_table *table = NULL;
    schultz_gradient_stop stops[2];
    schultz_handle handle = SCHULTZ_HANDLE_NONE;
    const schultz_gradient *back = NULL;

    two_stops(stops);
    ASSERT_EQ(SCHULTZ_OK, schultz_resource_table_create(&table));
    ASSERT_EQ(SCHULTZ_OK, schultz_gradient_radial(table,
                    schultz_point_make(0.5f, 0.5f), 0.75f, stops, 2,
                    &handle));
    ASSERT_EQ(SCHULTZ_OK, schultz_gradient_get(table, handle, &back));
    ASSERT_EQ((uint32_t)SCHULTZ_GRADIENT_RADIAL, back->kind);
    ASSERT_EQ(0.5f, back->from.x);
    ASSERT_EQ(0.75f, back->radius);

    schultz_resource_table_destroy(table);
    PASS();
}

TEST the_registry_refuses_what_it_cannot_hold(void)
{
    schultz_resource_table *table = NULL;
    schultz_gradient_stop stops[SCHULTZ_GRADIENT_STOPS_MAX + 2];
    schultz_handle handle = SCHULTZ_HANDLE_NONE;
    float lengths[2] = { 4.0f, 2.0f };
    float bad[2] = { 4.0f, 0.0f };
    uint32_t i;

    for (i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
        stops[i].offset = (float)i;
        stops[i].color  = schultz_color_rgba(0, 0, 0, 255);
    }
    ASSERT_EQ(SCHULTZ_OK, schultz_resource_table_create(&table));

    /* One stop is not a gradient, and too many will not fit. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_gradient_linear(table, schultz_point_make(0, 0),
                                      schultz_point_make(1, 1), stops, 1,
                                      &handle));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_gradient_linear(table, schultz_point_make(0, 0),
                                      schultz_point_make(1, 1), stops,
                                      SCHULTZ_GRADIENT_STOPS_MAX + 1,
                                      &handle));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_gradient_radial(table, schultz_point_make(0, 0), 0.0f,
                                      stops, 2, &handle));

    /* A dash length of zero would never advance. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_dash_register(table, bad, 2, &handle));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_dash_register(table, lengths, 1, &handle));

    schultz_resource_table_destroy(table);
    PASS();
}

TEST a_handle_of_one_kind_is_not_the_other(void)
{
    schultz_resource_table *table = NULL;
    schultz_gradient_stop stops[2];
    schultz_handle gradient = SCHULTZ_HANDLE_NONE;
    schultz_handle dash = SCHULTZ_HANDLE_NONE;
    const schultz_gradient *back = NULL;
    const schultz_dash *pattern = NULL;
    float lengths[2] = { 6.0f, 3.0f };

    two_stops(stops);
    ASSERT_EQ(SCHULTZ_OK, schultz_resource_table_create(&table));
    ASSERT_EQ(SCHULTZ_OK, schultz_gradient_linear(table,
                    schultz_point_make(0, 0), schultz_point_make(1, 0),
                    stops, 2, &gradient));
    ASSERT_EQ(SCHULTZ_OK, schultz_dash_register(table, lengths, 2, &dash));

    ASSERT_EQ(SCHULTZ_OK, schultz_dash_get(table, dash, &pattern));
    ASSERT_EQ(2u, pattern->count);
    ASSERT_EQ(6.0f, pattern->lengths[0]);

    /* Asking for one as the other is a bad handle, not a wrong answer. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_gradient_get(table, dash, &back));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_dash_get(table, gradient, &pattern));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_gradient_get(table, SCHULTZ_HANDLE_NONE, &back));

    schultz_resource_table_destroy(table);
    PASS();
}

/* ------------------------------------------------------- paint in a style */

TEST a_style_property_holds_a_gradient_in_place_of_a_colour(void)
{
    schultz_tree *tree = NULL;
    schultz_resource_table *table = NULL;
    schultz_gradient_stop stops[2];
    schultz_handle gradient = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    const schultz_resolved_style *style;
    schultz_paint paint;

    two_stops(stops);
    ASSERT_EQ(SCHULTZ_OK, schultz_resource_table_create(&table));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_gradient_linear(table,
                    schultz_point_make(0, 0), schultz_point_make(0, 1),
                    stops, 2, &gradient));

    schultz_panel_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BACKGROUND,
                                    schultz_value_gradient(gradient));
    schultz_tree_resolve_styles(tree);

    style = schultz_node_resolved(tree, node);
    paint = schultz_resolved_paint(style, SCHULTZ_PROP_BACKGROUND);
    ASSERT_EQ((uint32_t)SCHULTZ_PAINT_GRADIENT, paint.kind);
    ASSERT_EQ(gradient, paint.as.gradient);
    ASSERT_FALSE(schultz_paint_is_invisible(paint));

    /* A caller that can only take a colour gets nothing, not a guess. */
    ASSERT_EQ(0u, schultz_resolved_color(style,
                                         SCHULTZ_PROP_BACKGROUND).a);

    /* Setting a plain colour afterwards puts it back. */
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BACKGROUND,
        schultz_value_color(schultz_color_rgba(9, 8, 7, 255)));
    schultz_tree_resolve_styles(tree);
    paint = schultz_resolved_paint(schultz_node_resolved(tree, node),
                                   SCHULTZ_PROP_BACKGROUND);
    ASSERT_EQ((uint32_t)SCHULTZ_PAINT_SOLID, paint.kind);
    ASSERT_EQ(9u, paint.as.color.r);

    schultz_tree_destroy(tree);
    schultz_resource_table_destroy(table);
    PASS();
}

TEST a_gradient_reaches_the_draw_command(void)
{
    schultz_tree *tree = NULL;
    schultz_resource_table *table = NULL;
    schultz_arena arena;
    schultz_draw_list list;
    schultz_gradient_stop stops[2];
    schultz_handle gradient = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    const schultz_draw_cmd *cmd;

    two_stops(stops);
    ASSERT_EQ(SCHULTZ_OK, schultz_resource_table_create(&table));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    schultz_gradient_linear(table, schultz_point_make(0, 0),
                            schultz_point_make(1, 0), stops, 2, &gradient);

    schultz_panel_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_bounds(tree, node, schultz_rect_make(0, 0, 40, 20));
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BACKGROUND,
                                    schultz_value_gradient(gradient));
    schultz_tree_resolve_styles(tree);
    schultz_widget_paint_tree(tree, &list, &arena,
                              schultz_rect_make(0, 0, 0, 0));

    ASSERT(schultz_draw_list_count(&list) > 0u);
    cmd = schultz_draw_list_at(&list, 0);
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_FILL_ROUND_RECT, cmd->kind);
    ASSERT_EQ((uint32_t)SCHULTZ_PAINT_GRADIENT,
              cmd->as.fill_round_rect.paint.kind);
    ASSERT_EQ(gradient, cmd->as.fill_round_rect.paint.as.gradient);

    schultz_arena_free(&arena);
    schultz_tree_destroy(tree);
    schultz_resource_table_destroy(table);
    PASS();
}

TEST a_dash_pattern_and_the_stroke_detail_reach_the_command(void)
{
    schultz_tree *tree = NULL;
    schultz_resource_table *table = NULL;
    schultz_arena arena;
    schultz_draw_list list;
    schultz_handle dash = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    float lengths[2] = { 5.0f, 3.0f };
    const schultz_draw_cmd *cmd;
    uint32_t i;
    int32_t found = 0;

    ASSERT_EQ(SCHULTZ_OK, schultz_resource_table_create(&table));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_dash_register(table, lengths, 2, &dash));

    schultz_panel_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_bounds(tree, node, schultz_rect_make(0, 0, 40, 20));
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BORDER_COLOR,
        schultz_value_color(schultz_color_rgba(1, 2, 3, 255)));
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(2.0f));
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BORDER_DASH,
                                    schultz_value_dash(dash));
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BORDER_CAP,
                                    schultz_value_number(SCHULTZ_CAP_ROUND));
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BORDER_JOIN,
                                    schultz_value_number(SCHULTZ_JOIN_BEVEL));
    schultz_node_set_style_property(tree, node,
                                    SCHULTZ_PROP_BORDER_DASH_OFFSET,
                                    schultz_value_number(4.0f));
    schultz_node_set_style_property(tree, node,
                                    SCHULTZ_PROP_BORDER_MITER_LIMIT,
                                    schultz_value_number(2.5f));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(dash, schultz_resolved_dash(schultz_node_resolved(tree, node),
                                          SCHULTZ_PROP_BORDER_DASH));

    schultz_widget_paint_tree(tree, &list, &arena,
                              schultz_rect_make(0, 0, 0, 0));
    for (i = 0; i < schultz_draw_list_count(&list); i++) {
        cmd = schultz_draw_list_at(&list, i);
        if (cmd->kind != SCHULTZ_DRAW_STROKE_ROUND_RECT) {
            continue;
        }
        found = 1;
        ASSERT_EQ(dash, cmd->as.stroke_round_rect.stroke.dash);
        ASSERT_EQ((uint32_t)SCHULTZ_CAP_ROUND,
                  cmd->as.stroke_round_rect.stroke.cap);
        ASSERT_EQ((uint32_t)SCHULTZ_JOIN_BEVEL,
                  cmd->as.stroke_round_rect.stroke.join);
        /* The two that used to be settable only on a stroke built by hand. */
        ASSERT_EQ(4.0f, cmd->as.stroke_round_rect.stroke.dash_offset);
        ASSERT_EQ(2.5f, cmd->as.stroke_round_rect.stroke.miter_limit);
    }
    ASSERT(found);

    schultz_arena_free(&arena);
    schultz_tree_destroy(tree);
    schultz_resource_table_destroy(table);
    PASS();
}

TEST paint_and_stroke_values_are_what_they_say(void)
{
    schultz_paint solid = schultz_paint_solid(schultz_color_rgba(1, 2, 3, 4));
    schultz_paint clear = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 0));
    schultz_paint gradient = schultz_paint_gradient((schultz_handle)7);
    schultz_stroke stroke = schultz_stroke_solid(
        schultz_color_rgba(5, 6, 7, 255), 3.0f);
    schultz_stroke fancy = schultz_stroke_make(gradient, 1.0f);

    ASSERT_EQ((uint32_t)SCHULTZ_PAINT_SOLID, solid.kind);
    ASSERT_EQ(2u, solid.as.color.g);
    ASSERT_EQ((uint32_t)SCHULTZ_PAINT_GRADIENT, gradient.kind);
    ASSERT_EQ((schultz_handle)7, gradient.as.gradient);

    ASSERT_FALSE(schultz_paint_is_invisible(solid));
    ASSERT(schultz_paint_is_invisible(clear));
    /* A gradient decides its own transparency through its stops. */
    ASSERT_FALSE(schultz_paint_is_invisible(gradient));

    ASSERT_EQ(3.0f, stroke.width);
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE, stroke.dash);
    ASSERT_EQ((uint32_t)SCHULTZ_CAP_BUTT, stroke.cap);
    ASSERT_EQ((uint32_t)SCHULTZ_JOIN_MITER, stroke.join);
    ASSERT_EQ((uint32_t)SCHULTZ_PAINT_GRADIENT, fancy.paint.kind);

    PASS();
}

TEST the_registry_rejects_null_arguments(void)
{
    schultz_resource_table *table = NULL;
    schultz_gradient_stop stops[2];
    schultz_handle handle = SCHULTZ_HANDLE_NONE;
    const schultz_gradient *gradient = NULL;
    float lengths[2] = { 1.0f, 1.0f };

    two_stops(stops);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_resource_table_create(NULL));
    ASSERT_EQ(SCHULTZ_OK, schultz_resource_table_create(&table));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_gradient_linear(table, schultz_point_make(0, 0),
                                      schultz_point_make(1, 1), NULL, 2,
                                      &handle));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_dash_register(table, lengths, 2, NULL));
    /* No table means no such gradient, which is a bad handle. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_gradient_get(NULL, 1, &gradient));

    /* Destroying nothing is not an error. */
    schultz_resource_table_destroy(NULL);
    schultz_resource_table_destroy(table);
    PASS();
}

/*
 * The common pattern is one mark and one gap, and writing an array for two
 * numbers is what this saves. It has to be the same thing the array form
 * registers, not a second kind of pattern.
 */
TEST a_dash_pair_is_the_same_as_a_two_length_array(void)
{
    schultz_resource_table *table = NULL;
    schultz_handle from_pair = SCHULTZ_HANDLE_NONE;
    schultz_handle from_array = SCHULTZ_HANDLE_NONE;
    const schultz_dash *pair = NULL;
    const schultz_dash *array = NULL;
    float lengths[2];

    lengths[0] = 6.0f;
    lengths[1] = 3.0f;

    ASSERT_EQ(SCHULTZ_OK, schultz_resource_table_create(&table));
    ASSERT_EQ(SCHULTZ_OK, schultz_dash_pair(table, 6.0f, 3.0f, &from_pair));
    ASSERT_EQ(SCHULTZ_OK, schultz_dash_register(table, lengths, 2,
                                                &from_array));
    ASSERT_EQ(SCHULTZ_OK, schultz_dash_get(table, from_pair, &pair));
    ASSERT_EQ(SCHULTZ_OK, schultz_dash_get(table, from_array, &array));

    ASSERT_EQ(array->count, pair->count);
    ASSERT_EQ(array->lengths[0], pair->lengths[0]);
    ASSERT_EQ(array->lengths[1], pair->lengths[1]);

    /* And it refuses what the array form refuses. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_dash_pair(table, 0.0f, 3.0f, &from_pair));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_dash_pair(table, 6.0f, -1.0f, &from_pair));

    schultz_resource_table_destroy(table);
    PASS();
}

/*
 * A widget that says nothing about these two still gets sensible ones. Four
 * is the miter limit SVG and the rasterizer both start from, and a pattern
 * that says nothing starts at its beginning.
 */
TEST a_border_that_says_nothing_gets_the_usual_stroke_defaults(void)
{
    schultz_tree *tree = NULL;
    schultz_arena arena;
    schultz_draw_list list;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t i;
    int32_t found = 0;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));

    schultz_panel_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_bounds(tree, node, schultz_rect_make(0, 0, 40, 20));
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(2.0f));
    schultz_tree_resolve_styles(tree);
    schultz_widget_paint_tree(tree, &list, &arena,
                              schultz_rect_make(0, 0, 0, 0));

    for (i = 0; i < schultz_draw_list_count(&list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&list, i);

        if (cmd->kind != SCHULTZ_DRAW_STROKE_ROUND_RECT) {
            continue;
        }
        found = 1;
        ASSERT_EQ(0.0f, cmd->as.stroke_round_rect.stroke.dash_offset);
        ASSERT_EQ(4.0f, cmd->as.stroke_round_rect.stroke.miter_limit);
    }
    ASSERT(found);

    schultz_arena_free(&arena);
    schultz_tree_destroy(tree);
    PASS();
}

SUITE(resource)
{
    RUN_TEST(a_registered_gradient_comes_back_as_it_went_in);
    RUN_TEST(a_radial_gradient_keeps_its_centre_and_radius);
    RUN_TEST(the_registry_refuses_what_it_cannot_hold);
    RUN_TEST(a_handle_of_one_kind_is_not_the_other);
    RUN_TEST(a_style_property_holds_a_gradient_in_place_of_a_colour);
    RUN_TEST(a_gradient_reaches_the_draw_command);
    RUN_TEST(a_dash_pattern_and_the_stroke_detail_reach_the_command);
    RUN_TEST(a_dash_pair_is_the_same_as_a_two_length_array);
    RUN_TEST(a_border_that_says_nothing_gets_the_usual_stroke_defaults);
    RUN_TEST(paint_and_stroke_values_are_what_they_say);
    RUN_TEST(the_registry_rejects_null_arguments);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(resource);
    GREATEST_MAIN_END();
}
