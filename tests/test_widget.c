/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_widget.c - the widget interface, the paint walk, and the tier 1
 * widgets that only draw.
 *
 * Appearance is asserted by inspecting the draw commands a widget emitted,
 * with no window and no rasterizer. That is the seam the architecture was
 * built around, and it means these tests say exactly what was drawn rather
 * than that something was.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_widgets.h"

#define FONT_PATH "assets/fonts/DejaVuSans.ttf"

/* ------------------------------------------------------------- fixture */

typedef struct {
    schultz_tree        *tree;
    schultz_arena        arena;
    schultz_draw_list    list;
    schultz_font_system *fonts;
    schultz_glyph_cache *glyphs;
    schultz_handle       font;
} widget_fixture;

static int32_t fixture_setup(widget_fixture *f)
{
    int32_t result;

    memset(f, 0, sizeof(*f));
    result = schultz_tree_create(&f->tree);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(f->tree, schultz_rect_make(0, 0, 800, 600));
    result = schultz_arena_init(&f->arena, 0);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_draw_list_init(&f->list, &f->arena, 0);
}

static void fixture_teardown(widget_fixture *f)
{
    schultz_arena_free(&f->arena);
    schultz_tree_destroy(f->tree);
    if (f->glyphs != NULL) {
        schultz_glyph_cache_destroy(f->glyphs);
    }
    if (f->fonts != NULL) {
        schultz_font_system_destroy(f->fonts);
    }
}

/* Adds a real font system, for the text widgets that need one. */
static int32_t fixture_add_fonts(widget_fixture *f, float size)
{
    int32_t result = schultz_font_system_create(&f->fonts);

    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_glyph_cache_create(f->fonts, &f->glyphs);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_font_load_file(f->fonts, FONT_PATH, size, &f->font);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_font_system(f->tree, f->fonts);
    return SCHULTZ_OK;
}

/* Resolves, then paints everything, and returns how many commands came out. */
static uint32_t paint_all(widget_fixture *f)
{
    schultz_arena_reset(&f->arena);
    schultz_draw_list_init(&f->list, &f->arena, 0);
    schultz_tree_resolve_styles(f->tree);
    schultz_widget_paint_tree(f->tree, &f->list, &f->arena,
                              schultz_rect_make(0, 0, 0, 0));
    return schultz_draw_list_count(&f->list);
}

/* Paints only what touches a region. */
static uint32_t paint_dirty(widget_fixture *f, schultz_rect dirty)
{
    schultz_arena_reset(&f->arena);
    schultz_draw_list_init(&f->list, &f->arena, 0);
    schultz_tree_resolve_styles(f->tree);
    schultz_widget_paint_tree(f->tree, &f->list, &f->arena, dirty);
    return schultz_draw_list_count(&f->list);
}

/* Index of the first command of a kind, or -1. */
static int32_t find_kind(widget_fixture *f, uint32_t kind)
{
    uint32_t i;
    for (i = 0; i < schultz_draw_list_count(&f->list); i++) {
        if (schultz_draw_list_at(&f->list, i)->kind == kind) {
            return (int32_t)i;
        }
    }
    return -1;
}

static uint32_t count_kind(widget_fixture *f, uint32_t kind)
{
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < schultz_draw_list_count(&f->list); i++) {
        if (schultz_draw_list_at(&f->list, i)->kind == kind) {
            n++;
        }
    }
    return n;
}

static void set_color(schultz_tree *t, schultz_handle n, uint32_t prop,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    schultz_node_set_style_property(t, n, prop,
        schultz_value_color(schultz_color_rgba(r, g, b, a)));
}

/* ----------------------------------------------------- the widget binding */

TEST a_node_with_no_widget_paints_nothing(void)
{
    widget_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_node_create(f.tree, schultz_tree_root(f.tree), &node);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 50, 50));

    ASSERT_EQ(0u, paint_all(&f));
    ASSERT_EQ(NULL, schultz_node_widget(f.tree, node));
    ASSERT_EQ(NULL, schultz_node_widget_data(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

/* The vtable is what identifies a widget type, so it must round trip. */
TEST setting_a_widget_records_its_type_and_data(void)
{
    widget_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree,
                                schultz_tree_root(f.tree), &node));
    ASSERT(schultz_node_widget(f.tree, node) != NULL);
    ASSERT_EQ((uint32_t)SCHULTZ_ROLE_GROUP,
              schultz_node_get_role(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

TEST widget_accessors_reject_stale_handles(void)
{
    widget_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &node);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_destroy(f.tree, node));

    ASSERT_EQ(NULL, schultz_node_widget(f.tree, node));
    ASSERT_EQ(NULL, schultz_node_widget_data(f.tree, node));
    ASSERT_EQ(0, schultz_node_clips_children(f.tree, node));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_set_widget(f.tree, node, NULL, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_set_clips_children(f.tree, node, 1));

    fixture_teardown(&f);
    PASS();
}

TEST the_paint_walk_rejects_bad_arguments(void)
{
    widget_fixture f;
    schultz_rect none = schultz_rect_make(0, 0, 0, 0);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_widget_paint_tree(NULL, &f.list, &f.arena, none));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_widget_paint_tree(f.tree, NULL, &f.arena, none));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_widget_paint_tree(f.tree, &f.list, NULL, none));

    fixture_teardown(&f);
    PASS();
}

/* ---------------------------------------------------------- the paint walk */

TEST children_paint_after_their_parent(void)
{
    widget_fixture f;
    schultz_handle parent;
    schultz_handle child;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree,
                                schultz_tree_root(f.tree), &parent));
    schultz_node_set_bounds(f.tree, parent, schultz_rect_make(0, 0, 100, 100));
    set_color(f.tree, parent, SCHULTZ_PROP_BACKGROUND, 1, 0, 0, 255);

    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree, parent, &child));
    schultz_node_set_bounds(f.tree, child, schultz_rect_make(10, 10, 20, 20));
    set_color(f.tree, child, SCHULTZ_PROP_BACKGROUND, 2, 0, 0, 255);

    ASSERT_EQ(2u, paint_all(&f));
    /* Parent first, so the child draws over it. */
    ASSERT_EQ(1u, schultz_draw_list_at(&f.list, 0)->as.fill_round_rect.paint.as.color.r);
    ASSERT_EQ(2u, schultz_draw_list_at(&f.list, 1)->as.fill_round_rect.paint.as.color.r);

    fixture_teardown(&f);
    PASS();
}

TEST later_siblings_paint_over_earlier_ones(void)
{
    widget_fixture f;
    schultz_handle root;
    schultz_handle first;
    schultz_handle second;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    root = schultz_tree_root(f.tree);
    schultz_panel_create(f.tree, root, &first);
    schultz_panel_create(f.tree, root, &second);
    schultz_node_set_bounds(f.tree, first, schultz_rect_make(0, 0, 50, 50));
    schultz_node_set_bounds(f.tree, second, schultz_rect_make(0, 0, 50, 50));
    set_color(f.tree, first, SCHULTZ_PROP_BACKGROUND, 1, 0, 0, 255);
    set_color(f.tree, second, SCHULTZ_PROP_BACKGROUND, 2, 0, 0, 255);

    ASSERT_EQ(2u, paint_all(&f));
    ASSERT_EQ(1u, schultz_draw_list_at(&f.list, 0)->as.fill_round_rect.paint.as.color.r);
    ASSERT_EQ(2u, schultz_draw_list_at(&f.list, 1)->as.fill_round_rect.paint.as.color.r);

    fixture_teardown(&f);
    PASS();
}

TEST a_hidden_subtree_paints_nothing(void)
{
    widget_fixture f;
    schultz_handle parent;
    schultz_handle child;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &parent);
    schultz_node_set_bounds(f.tree, parent, schultz_rect_make(0, 0, 100, 100));
    set_color(f.tree, parent, SCHULTZ_PROP_BACKGROUND, 1, 0, 0, 255);
    schultz_panel_create(f.tree, parent, &child);
    schultz_node_set_bounds(f.tree, child, schultz_rect_make(0, 0, 20, 20));
    set_color(f.tree, child, SCHULTZ_PROP_BACKGROUND, 2, 0, 0, 255);
    ASSERT_EQ(2u, paint_all(&f));

    /* Hiding the parent removes the child from the frame too. */
    schultz_node_set_state(f.tree, parent, SCHULTZ_STATE_ENABLED);
    ASSERT_EQ(0u, paint_all(&f));

    fixture_teardown(&f);
    PASS();
}

/*
 * Culling. This is what keeps a one pixel change from rebuilding the whole
 * command list, so it is asserted by position rather than by count alone.
 */
TEST a_node_outside_the_dirty_rect_is_skipped(void)
{
    widget_fixture f;
    schultz_handle near_node;
    schultz_handle far_node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &near_node);
    schultz_node_set_bounds(f.tree, near_node,
                            schultz_rect_make(0, 0, 50, 50));
    set_color(f.tree, near_node, SCHULTZ_PROP_BACKGROUND, 1, 0, 0, 255);

    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &far_node);
    schultz_node_set_bounds(f.tree, far_node,
                            schultz_rect_make(400, 400, 50, 50));
    set_color(f.tree, far_node, SCHULTZ_PROP_BACKGROUND, 2, 0, 0, 255);

    /* Everything, when the dirty rect is empty. */
    ASSERT_EQ(2u, paint_all(&f));

    /* Only the one that overlaps. */
    ASSERT_EQ(1u, paint_dirty(&f, schultz_rect_make(0, 0, 60, 60)));
    ASSERT_EQ(1u, schultz_draw_list_at(&f.list, 0)->as.fill_round_rect.paint.as.color.r);

    ASSERT_EQ(1u, paint_dirty(&f, schultz_rect_make(390, 390, 80, 80)));
    ASSERT_EQ(2u, schultz_draw_list_at(&f.list, 0)->as.fill_round_rect.paint.as.color.r);

    /* And neither, when the region touches nothing. */
    ASSERT_EQ(0u, paint_dirty(&f, schultz_rect_make(700, 500, 10, 10)));

    fixture_teardown(&f);
    PASS();
}

/* A container with no size of its own must not cull its children away. */
TEST an_unsized_parent_does_not_cull_its_children(void)
{
    widget_fixture f;
    schultz_handle group;
    schultz_handle child;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_node_create(f.tree, schultz_tree_root(f.tree), &group);
    /* left with zero bounds on purpose */
    schultz_panel_create(f.tree, group, &child);
    schultz_node_set_bounds(f.tree, child, schultz_rect_make(10, 10, 40, 40));
    set_color(f.tree, child, SCHULTZ_PROP_BACKGROUND, 9, 0, 0, 255);

    ASSERT_EQ(1u, paint_dirty(&f, schultz_rect_make(0, 0, 100, 100)));

    fixture_teardown(&f);
    PASS();
}

TEST a_clipping_node_brackets_its_children(void)
{
    widget_fixture f;
    schultz_handle clipper;
    schultz_handle child;
    int32_t push;
    int32_t pop;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &clipper);
    schultz_node_set_bounds(f.tree, clipper,
                            schultz_rect_make(10, 20, 100, 80));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_clips_children(f.tree, clipper, 1));
    ASSERT_EQ(1, schultz_node_clips_children(f.tree, clipper));

    schultz_panel_create(f.tree, clipper, &child);
    schultz_node_set_bounds(f.tree, child, schultz_rect_make(0, 0, 500, 500));
    set_color(f.tree, child, SCHULTZ_PROP_BACKGROUND, 5, 0, 0, 255);

    paint_all(&f);
    push = find_kind(&f, SCHULTZ_DRAW_CLIP_BEGIN);
    pop  = find_kind(&f, SCHULTZ_DRAW_CLIP_END);
    ASSERT(push >= 0);
    ASSERT(pop > push);
    /* The clip is the clipping node's own bounds. */
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(10, 20, 100, 80),
                    schultz_draw_list_at(&f.list, (uint32_t)push)
                        ->as.clip_begin.rect));
    /* And the child was emitted between the two. */
    ASSERT(find_kind(&f, SCHULTZ_DRAW_FILL_ROUND_RECT) > push);
    ASSERT(find_kind(&f, SCHULTZ_DRAW_FILL_ROUND_RECT) < pop);

    fixture_teardown(&f);
    PASS();
}

TEST clips_balance_across_nesting(void)
{
    widget_fixture f;
    schultz_handle outer;
    schultz_handle inner;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &outer);
    schultz_node_set_bounds(f.tree, outer, schultz_rect_make(0, 0, 200, 200));
    schultz_node_set_clips_children(f.tree, outer, 1);
    schultz_panel_create(f.tree, outer, &inner);
    schultz_node_set_bounds(f.tree, inner, schultz_rect_make(0, 0, 100, 100));
    schultz_node_set_clips_children(f.tree, inner, 1);

    paint_all(&f);
    ASSERT_EQ(2u, count_kind(&f, SCHULTZ_DRAW_CLIP_BEGIN));
    ASSERT_EQ(2u, count_kind(&f, SCHULTZ_DRAW_CLIP_END));

    fixture_teardown(&f);
    PASS();
}

/* --------------------------------------------------------------- Panel */

TEST a_panel_paints_nothing_until_it_is_styled(void)
{
    widget_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &node);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 50, 50));

    /* Transparent background and zero border width by default. */
    ASSERT_EQ(0u, paint_all(&f));

    fixture_teardown(&f);
    PASS();
}

TEST a_panel_fills_and_strokes_from_its_resolved_style(void)
{
    widget_fixture f;
    schultz_handle node;
    const schultz_draw_cmd *fill;
    const schultz_draw_cmd *stroke;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &node);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(5, 6, 60, 40));
    set_color(f.tree, node, SCHULTZ_PROP_BACKGROUND, 10, 20, 30, 255);
    set_color(f.tree, node, SCHULTZ_PROP_BORDER_COLOR, 40, 50, 60, 255);
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(3.0f));
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_CORNER_RADIUS,
                                    schultz_value_number(7.0f));

    ASSERT_EQ(2u, paint_all(&f));
    fill = schultz_draw_list_at(&f.list, 0);
    stroke = schultz_draw_list_at(&f.list, 1);

    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_FILL_ROUND_RECT, fill->kind);
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(5, 6, 60, 40),
                                     fill->as.fill_round_rect.rect));
    ASSERT_EQ(10u, fill->as.fill_round_rect.paint.as.color.r);
    ASSERT_EQ(7.0f, fill->as.fill_round_rect.radius);

    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_STROKE_ROUND_RECT, stroke->kind);
    ASSERT_EQ(40u, stroke->as.stroke_round_rect.stroke.paint.as.color.r);
    ASSERT_EQ(3.0f, stroke->as.stroke_round_rect.stroke.width);

    fixture_teardown(&f);
    PASS();
}

/* A panel paints in window coordinates, not its own. */
TEST a_nested_panel_paints_at_its_absolute_position(void)
{
    widget_fixture f;
    schultz_handle outer;
    schultz_handle inner;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &outer);
    schultz_node_set_bounds(f.tree, outer, schultz_rect_make(100, 50, 200,
                                                             200));
    schultz_panel_create(f.tree, outer, &inner);
    schultz_node_set_bounds(f.tree, inner, schultz_rect_make(10, 20, 30, 40));
    set_color(f.tree, inner, SCHULTZ_PROP_BACKGROUND, 1, 1, 1, 255);

    ASSERT_EQ(1u, paint_all(&f));
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(110, 70, 30, 40),
                    schultz_draw_list_at(&f.list, 0)->as.fill_round_rect.rect));

    fixture_teardown(&f);
    PASS();
}

TEST a_panel_follows_a_theme_change(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_theme theme;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &node);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 50, 50));
    /* A token, not a literal, so it follows the theme. */
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_BACKGROUND,
                    schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));
    paint_all(&f);
    ASSERT_EQ(schultz_theme_color(NULL, SCHULTZ_TOKEN_COLOR_ACCENT).r,
              schultz_draw_list_at(&f.list, 0)->as.fill_round_rect.paint.as.color.r);

    schultz_theme_init(&theme);
    schultz_theme_set_color(&theme, SCHULTZ_TOKEN_COLOR_ACCENT,
                            schultz_color_rgba(99, 0, 0, 255));
    schultz_tree_set_theme(f.tree, &theme);

    paint_all(&f);
    ASSERT_EQ(99u,
              schultz_draw_list_at(&f.list, 0)->as.fill_round_rect.paint.as.color.r);

    fixture_teardown(&f);
    PASS();
}

/* ----------------------------------------------------------- Separator */

TEST a_horizontal_separator_measures_thin_and_draws_across(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_size size;
    const schultz_draw_cmd *cmd;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_separator_create(f.tree,
                    schultz_tree_root(f.tree),
                    SCHULTZ_ORIENT_HORIZONTAL, &node));
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(2.0f));
    set_color(f.tree, node, SCHULTZ_PROP_BORDER_COLOR, 7, 7, 7, 255);
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, 300, -1,
                                                 &size));
    ASSERT_EQ(0.0f, size.width);   /* the parent stretches it */
    ASSERT_EQ(2.0f, size.height);

    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 40, 200, 2));
    paint_all(&f);
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_LINE));
    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT_EQ(0.0f, cmd->as.line.from.x);
    ASSERT_EQ(200.0f, cmd->as.line.to.x);
    ASSERT_EQ(41.0f, cmd->as.line.from.y);   /* down the middle */
    ASSERT_EQ(41.0f, cmd->as.line.to.y);

    fixture_teardown(&f);
    PASS();
}

TEST a_vertical_separator_swaps_the_axes(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_size size;
    const schultz_draw_cmd *cmd;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_separator_create(f.tree, schultz_tree_root(f.tree),
                             SCHULTZ_ORIENT_VERTICAL, &node);
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(2.0f));
    set_color(f.tree, node, SCHULTZ_PROP_BORDER_COLOR, 7, 7, 7, 255);
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, -1, 300,
                                                 &size));
    ASSERT_EQ(2.0f, size.width);
    ASSERT_EQ(0.0f, size.height);

    schultz_node_set_bounds(f.tree, node, schultz_rect_make(10, 0, 2, 100));
    paint_all(&f);
    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT_EQ(11.0f, cmd->as.line.from.x);
    ASSERT_EQ(0.0f, cmd->as.line.from.y);
    ASSERT_EQ(100.0f, cmd->as.line.to.y);

    fixture_teardown(&f);
    PASS();
}

TEST an_invisible_separator_emits_nothing(void)
{
    widget_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_separator_create(f.tree, schultz_tree_root(f.tree),
                             SCHULTZ_ORIENT_HORIZONTAL, &node);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 100, 1));
    /* A transparent border colour means nothing to draw. */
    set_color(f.tree, node, SCHULTZ_PROP_BORDER_COLOR, 0, 0, 0, 0);

    ASSERT_EQ(0u, paint_all(&f));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------- Ellipse */

TEST an_ellipse_fills_and_strokes_its_bounds(void)
{
    widget_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_ellipse_create(f.tree,
                                schultz_tree_root(f.tree), &node));
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(20, 30, 80, 40));
    set_color(f.tree, node, SCHULTZ_PROP_BACKGROUND, 1, 2, 3, 255);
    set_color(f.tree, node, SCHULTZ_PROP_BORDER_COLOR, 4, 5, 6, 255);
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(2.0f));

    ASSERT_EQ(2u, paint_all(&f));
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_FILL_ELLIPSE,
              schultz_draw_list_at(&f.list, 0)->kind);
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(20, 30, 80, 40),
                    schultz_draw_list_at(&f.list, 0)->as.fill_ellipse.rect));
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_STROKE_ELLIPSE,
              schultz_draw_list_at(&f.list, 1)->kind);
    ASSERT_EQ(2.0f,
              schultz_draw_list_at(&f.list, 1)->as.stroke_ellipse.stroke.width);

    fixture_teardown(&f);
    PASS();
}

TEST an_unstyled_ellipse_emits_nothing(void)
{
    widget_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_ellipse_create(f.tree, schultz_tree_root(f.tree), &node);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 40, 40));
    ASSERT_EQ(0u, paint_all(&f));

    fixture_teardown(&f);
    PASS();
}

/* ---------------------------------------------------------------- Line */

TEST a_line_draws_between_its_points_offset_by_its_bounds(void)
{
    widget_fixture f;
    schultz_handle node;
    const schultz_draw_cmd *cmd;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_line_create(f.tree,
                    schultz_tree_root(f.tree), schultz_point_make(0, 0),
                    schultz_point_make(40, 30), &node));
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(100, 200, 40, 30));
    set_color(f.tree, node, SCHULTZ_PROP_BORDER_COLOR, 9, 9, 9, 255);
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(4.0f));

    ASSERT_EQ(1u, paint_all(&f));
    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT_EQ(100.0f, cmd->as.line.from.x);
    ASSERT_EQ(200.0f, cmd->as.line.from.y);
    ASSERT_EQ(140.0f, cmd->as.line.to.x);
    ASSERT_EQ(230.0f, cmd->as.line.to.y);
    ASSERT_EQ(4.0f, cmd->as.line.stroke.width);

    fixture_teardown(&f);
    PASS();
}

TEST a_line_measures_to_hold_its_points(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_line_create(f.tree, schultz_tree_root(f.tree),
                        schultz_point_make(5, 60), schultz_point_make(70, 10),
                        &node);
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, -1, -1,
                                                 &size));
    ASSERT_EQ(70.0f, size.width);
    ASSERT_EQ(60.0f, size.height);

    fixture_teardown(&f);
    PASS();
}

TEST moving_a_line_invalidates_it(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_handle other;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_line_create(f.tree, schultz_tree_root(f.tree),
                        schultz_point_make(0, 0), schultz_point_make(10, 10),
                        &node);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 10, 10));
    set_color(f.tree, node, SCHULTZ_PROP_BORDER_COLOR, 1, 1, 1, 255);
    schultz_tree_resolve_styles(f.tree);
    schultz_tree_clear_dirty(f.tree);
    schultz_tree_clear_layout_dirty(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_line_set_points(f.tree, node,
                    schultz_point_make(1, 2), schultz_point_make(30, 40)));
    ASSERT_EQ(1, schultz_node_is_dirty(f.tree, node));
    ASSERT_EQ(1, schultz_node_layout_dirty(f.tree, node));

    paint_all(&f);
    ASSERT_EQ(1.0f, schultz_draw_list_at(&f.list, 0)->as.line.from.x);
    ASSERT_EQ(30.0f, schultz_draw_list_at(&f.list, 0)->as.line.to.x);

    /* And the accessor refuses a node that is not a line. */
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &other);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_line_set_points(f.tree, other,
                    schultz_point_make(0, 0), schultz_point_make(1, 1)));

    fixture_teardown(&f);
    PASS();
}

/* ---------------------------------------------------------------- Path */

TEST a_path_fills_its_points_offset_by_its_bounds(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_point pts[3];
    const schultz_draw_cmd *cmd;

    pts[0] = schultz_point_make(0, 0);
    pts[1] = schultz_point_make(20, 0);
    pts[2] = schultz_point_make(10, 30);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_polygon_create(f.tree,
                    schultz_tree_root(f.tree), pts, 3, &node));
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(50, 60, 20, 30));
    set_color(f.tree, node, SCHULTZ_PROP_BACKGROUND, 3, 3, 3, 255);

    ASSERT_EQ(1u, paint_all(&f));
    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_FILL_POLYGON, cmd->kind);
    ASSERT_EQ(3u, cmd->as.fill_polygon.count);
    ASSERT_EQ(50.0f, cmd->as.fill_polygon.points[0].x);
    ASSERT_EQ(70.0f, cmd->as.fill_polygon.points[1].x);
    ASSERT_EQ(90.0f, cmd->as.fill_polygon.points[2].y);

    fixture_teardown(&f);
    PASS();
}

TEST a_path_copies_the_points_it_is_given(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_point pts[3];

    pts[0] = schultz_point_make(0, 0);
    pts[1] = schultz_point_make(20, 0);
    pts[2] = schultz_point_make(10, 30);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_polygon_create(f.tree, schultz_tree_root(f.tree), pts, 3, &node);
    /* Scribble over the caller's array. */
    memset(pts, 0, sizeof(pts));
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 20, 30));
    set_color(f.tree, node, SCHULTZ_PROP_BACKGROUND, 3, 3, 3, 255);

    paint_all(&f);
    ASSERT_EQ(20.0f,
              schultz_draw_list_at(&f.list, 0)->as.fill_polygon.points[1].x);

    fixture_teardown(&f);
    PASS();
}

TEST a_path_measures_to_the_extent_of_its_points(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_point pts[3];
    schultz_size size;

    pts[0] = schultz_point_make(0, 5);
    pts[1] = schultz_point_make(45, 0);
    pts[2] = schultz_point_make(12, 33);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_polygon_create(f.tree, schultz_tree_root(f.tree), pts, 3, &node);
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, -1, -1,
                                                 &size));
    ASSERT_EQ(45.0f, size.width);
    ASSERT_EQ(33.0f, size.height);

    fixture_teardown(&f);
    PASS();
}

TEST replacing_a_paths_points_invalidates_it(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_handle other;
    schultz_point pts[3];
    schultz_point wider[3];

    pts[0] = schultz_point_make(0, 0);
    pts[1] = schultz_point_make(10, 0);
    pts[2] = schultz_point_make(5, 10);
    wider[0] = schultz_point_make(0, 0);
    wider[1] = schultz_point_make(90, 0);
    wider[2] = schultz_point_make(45, 80);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_polygon_create(f.tree, schultz_tree_root(f.tree), pts, 3, &node);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 10, 10));
    set_color(f.tree, node, SCHULTZ_PROP_BACKGROUND, 3, 3, 3, 255);
    schultz_tree_resolve_styles(f.tree);
    schultz_tree_clear_dirty(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_polygon_set_points(f.tree, node, wider, 3));
    ASSERT_EQ(1, schultz_node_is_dirty(f.tree, node));
    paint_all(&f);
    ASSERT_EQ(90.0f,
              schultz_draw_list_at(&f.list, 0)->as.fill_polygon.points[1].x);

    /* Rejections. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_polygon_set_points(f.tree, node, NULL, 3));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_polygon_set_points(f.tree, node, wider, 1));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &other);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_polygon_set_points(f.tree, other, wider, 3));

    fixture_teardown(&f);
    PASS();
}

TEST path_creation_rejects_too_few_points(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_point one = { 0.0f, 0.0f };

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_polygon_create(f.tree, schultz_tree_root(f.tree), &one, 1,
                                     &node));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_polygon_create(f.tree, schultz_tree_root(f.tree), NULL, 3,
                                     &node));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------ together */

/*
 * The point of the whole chunk: widgets in a pane, laid out and painted from
 * the tree, with nothing drawn by hand.
 */
TEST widgets_compose_in_a_pane(void)
{
    widget_fixture f;
    schultz_handle column;
    schultz_handle top;
    schultz_handle rule;
    schultz_handle bottom;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &column);
    schultz_node_set_pane(f.tree, column, schultz_pane_vbox());
    schultz_node_set_spacing(f.tree, column, 10.0f, 4.0f);
    /* The container paints too, so the count below covers all three panels. */
    set_color(f.tree, column, SCHULTZ_PROP_BACKGROUND, 9, 9, 9, 255);

    schultz_panel_create(f.tree, column, &top);
    schultz_node_set_style_property(f.tree, top, SCHULTZ_PROP_PREF_HEIGHT,
                                    schultz_value_number(20.0f));
    set_color(f.tree, top, SCHULTZ_PROP_BACKGROUND, 1, 0, 0, 255);

    ASSERT_EQ(SCHULTZ_OK, schultz_separator_create(f.tree, column,
                    SCHULTZ_ORIENT_HORIZONTAL, &rule));
    schultz_node_set_style_property(f.tree, rule, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(1.0f));
    set_color(f.tree, rule, SCHULTZ_PROP_BORDER_COLOR, 2, 0, 0, 255);

    schultz_panel_create(f.tree, column, &bottom);
    schultz_node_set_style_property(f.tree, bottom, SCHULTZ_PROP_PREF_HEIGHT,
                                    schultz_value_number(30.0f));
    set_color(f.tree, bottom, SCHULTZ_PROP_BACKGROUND, 3, 0, 0, 255);

    schultz_tree_resolve_styles(f.tree);
    schultz_layout_arrange(f.tree, column,
                           schultz_rect_make(0, 0, 200, 200));

    /* Stacked with the padding and gaps the column was given. */
    {
        schultz_rect r;
        schultz_node_get_bounds(f.tree, top, &r);
        ASSERT_EQ(10.0f, r.y);
        schultz_node_get_bounds(f.tree, rule, &r);
        ASSERT_EQ(34.0f, r.y);      /* 10 + 20 + 4 */
        schultz_node_get_bounds(f.tree, bottom, &r);
        ASSERT_EQ(39.0f, r.y);      /* 34 + 1 + 4 */
    }

    /* Three fills and one rule, in tree order. */
    paint_all(&f);
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_LINE));
    ASSERT_EQ(3u, count_kind(&f, SCHULTZ_DRAW_FILL_ROUND_RECT));

    fixture_teardown(&f);
    PASS();
}

/* ---------------------------------------------------------------- Label */

TEST a_label_keeps_and_returns_its_text(void)
{
    widget_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_label_create(f.tree,
                    schultz_tree_root(f.tree), "Save", &node));
    ASSERT_STR_EQ("Save", schultz_label_text(f.tree, node));
    /* The text is what a screen reader announces. */
    ASSERT_STR_EQ("Save", schultz_node_get_name(f.tree, node));
    ASSERT_EQ((uint32_t)SCHULTZ_ROLE_LABEL,
              schultz_node_get_role(f.tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_text(f.tree, node, "Cancel"));
    ASSERT_STR_EQ("Cancel", schultz_label_text(f.tree, node));
    ASSERT_STR_EQ("Cancel", schultz_node_get_name(f.tree, node));

    /* NULL means empty rather than a crash. */
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_text(f.tree, node, NULL));
    ASSERT_STR_EQ("", schultz_label_text(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

TEST label_accessors_reject_other_widgets(void)
{
    widget_fixture f;
    schultz_handle panel;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    ASSERT_EQ(NULL, schultz_label_text(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_label_set_text(f.tree, panel, "no"));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_label_set_wrap(f.tree, panel, 1));

    fixture_teardown(&f);
    PASS();
}

/* Without a font system a label reports nothing rather than guessing. */
TEST a_label_without_a_font_system_measures_to_nothing(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_label_create(f.tree, schultz_tree_root(f.tree), "Hello", &node);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 200, 40));
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, 200, -1,
                                                 &size));
    ASSERT_EQ(0.0f, size.width);
    ASSERT_EQ(0u, paint_all(&f));

    fixture_teardown(&f);
    PASS();
}

TEST a_label_measures_to_its_text(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_size shortish;
    schultz_size longer;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, fixture_add_fonts(&f, 16.0f));
    schultz_label_create(f.tree, schultz_tree_root(f.tree), "Hi", &node);
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_FONT,
                                    schultz_value_font(f.font));
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, -1, -1,
                                                 &shortish));
    ASSERT(shortish.width > 0.0f);
    ASSERT(shortish.height > 0.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_text(f.tree, node,
                                                 "Hi there, world"));
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, -1, -1,
                                                 &longer));
    ASSERT(longer.width > shortish.width);
    /* One line either way, so the same height. */
    ASSERT_EQ(shortish.height, longer.height);

    fixture_teardown(&f);
    PASS();
}

/*
 * The case the measure signature exists for: a label's height depends on the
 * width it is offered.
 */
TEST a_wrapped_label_gets_taller_as_it_gets_narrower(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_size wide;
    schultz_size narrow;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, fixture_add_fonts(&f, 16.0f));
    schultz_label_create(f.tree, schultz_tree_root(f.tree),
                         "the quick brown fox jumps over the lazy dog",
                         &node);
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_FONT,
                                    schultz_value_font(f.font));
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, 600, -1,
                                                 &wide));
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, 100, -1,
                                                 &narrow));

    ASSERT(narrow.height > wide.height);
    ASSERT(narrow.width < wide.width);

    /* Turning wrapping off makes it one line however narrow. */
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_wrap(f.tree, node, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, 100, -1,
                                                 &narrow));
    ASSERT_EQ(wide.height, narrow.height);

    fixture_teardown(&f);
    PASS();
}

TEST a_label_emits_a_glyph_run_per_line(void)
{
    widget_fixture f;
    schultz_handle node;
    const schultz_draw_cmd *cmd;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, fixture_add_fonts(&f, 16.0f));
    schultz_label_create(f.tree, schultz_tree_root(f.tree), "Hello", &node);
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_FONT,
                                    schultz_value_font(f.font));
    set_color(f.tree, node, SCHULTZ_PROP_TEXT_COLOR, 200, 100, 50, 255);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(30, 40, 200, 20));

    ASSERT_EQ(1u, paint_all(&f));
    cmd = schultz_draw_list_at(&f.list, 0);
    ASSERT_EQ((uint32_t)SCHULTZ_DRAW_GLYPH_RUN, cmd->kind);
    ASSERT_EQ(5u, cmd->as.glyph_run.count);
    ASSERT_EQ(f.font, cmd->as.glyph_run.font);
    ASSERT_EQ(200u, cmd->as.glyph_run.paint.as.color.r);
    /* Positioned at the label, not at the origin. */
    ASSERT(cmd->as.glyph_run.glyphs[0].x >= 30.0f);
    ASSERT(cmd->as.glyph_run.glyphs[0].y > 40.0f);

    fixture_teardown(&f);
    PASS();
}

TEST a_wrapped_label_emits_one_run_per_line(void)
{
    widget_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, fixture_add_fonts(&f, 16.0f));
    schultz_label_create(f.tree, schultz_tree_root(f.tree),
                         "the quick brown fox jumps over the lazy dog",
                         &node);
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_FONT,
                                    schultz_value_font(f.font));
    set_color(f.tree, node, SCHULTZ_PROP_TEXT_COLOR, 1, 1, 1, 255);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 100, 200));

    ASSERT(paint_all(&f) > 1u);
    ASSERT_EQ(schultz_draw_list_count(&f.list),
              count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN));

    /* Each line sits below the one before it. */
    {
        const schultz_draw_cmd *first = schultz_draw_list_at(&f.list, 0);
        const schultz_draw_cmd *second = schultz_draw_list_at(&f.list, 1);
        ASSERT(second->as.glyph_run.glyphs[0].y >
               first->as.glyph_run.glyphs[0].y);
    }

    fixture_teardown(&f);
    PASS();
}

TEST an_empty_label_emits_nothing(void)
{
    widget_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, fixture_add_fonts(&f, 16.0f));
    schultz_label_create(f.tree, schultz_tree_root(f.tree), "", &node);
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_FONT,
                                    schultz_value_font(f.font));
    set_color(f.tree, node, SCHULTZ_PROP_TEXT_COLOR, 1, 1, 1, 255);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 100, 20));

    ASSERT_EQ(0u, paint_all(&f));

    fixture_teardown(&f);
    PASS();
}

TEST a_labels_colour_inherits_from_its_parent(void)
{
    widget_fixture f;
    schultz_handle panel;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, fixture_add_fonts(&f, 16.0f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 200, 100));
    /* Both inherit, so setting them on the panel reaches the label. */
    set_color(f.tree, panel, SCHULTZ_PROP_TEXT_COLOR, 77, 88, 99, 255);
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_FONT,
                                    schultz_value_font(f.font));

    schultz_label_create(f.tree, panel, "Inherited", &node);
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 200, 20));

    ASSERT_EQ(1u, paint_all(&f));
    ASSERT_EQ(77u,
              schultz_draw_list_at(&f.list, 0)->as.glyph_run.paint.as.color.r);

    fixture_teardown(&f);
    PASS();
}

/*
 * What actually gets repainted is a little larger than the rectangle asked
 * for, because antialiased coverage reaches past a shape's geometry. Culling
 * has to use the same widened rectangle: a node lying entirely inside the
 * slack would otherwise be skipped while the pixels it covers are painted
 * over by whatever is behind it, which erases it.
 */
TEST a_node_just_outside_the_dirty_rectangle_still_paints(void)
{
    widget_fixture f;
    schultz_handle behind;
    schultz_handle sliver;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &behind);
    schultz_node_set_bounds(f.tree, behind, schultz_rect_make(0, 0, 200, 200));
    set_color(f.tree, behind, SCHULTZ_PROP_BACKGROUND, 10, 10, 10, 255);

    /* Its bottom edge is exactly the dirty rectangle's top edge. */
    schultz_panel_create(f.tree, behind, &sliver);
    schultz_node_set_bounds(f.tree, sliver, schultz_rect_make(20, 20, 60, 30));
    set_color(f.tree, sliver, SCHULTZ_PROP_BACKGROUND, 20, 20, 20, 255);

    paint_dirty(&f, schultz_rect_make(0, 50, 200, 100));

    /* The one behind it repaints, so the one on top has to as well. */
    ASSERT_EQ(2u, count_kind(&f, SCHULTZ_DRAW_FILL_ROUND_RECT));

    /* Well clear of the region, it is still skipped. */
    schultz_node_set_bounds(f.tree, sliver, schultz_rect_make(20, 0, 60, 20));
    paint_dirty(&f, schultz_rect_make(0, 50, 200, 100));
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_FILL_ROUND_RECT));

    fixture_teardown(&f);
    PASS();
}

/*
 * The face comes from `font` and the size from `font.size`, always. Before
 * that was true a font handle carried its own size and `font.size` resolved
 * to a number nothing read.
 */
TEST a_labels_size_comes_from_the_style_not_the_font_handle(void)
{
    widget_fixture f;
    schultz_handle node;
    schultz_size small;
    schultz_size large;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, fixture_add_fonts(&f, 16.0f));
    schultz_label_create(f.tree, schultz_tree_root(f.tree), "Size", &node);
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_FONT,
                                    schultz_value_font(f.font));
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_FONT_SIZE,
                                    schultz_value_number(16.0f));
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, -1, -1,
                                                 &small));

    /* The same font handle, a larger size, and larger text. */
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_FONT_SIZE,
                                    schultz_value_number(40.0f));
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, -1, -1,
                                                 &large));
    ASSERT(large.width > small.width);
    ASSERT(large.height > small.height);

    /* And the run that is drawn uses the sized face, not the one set. */
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 200, 60));
    set_color(f.tree, node, SCHULTZ_PROP_TEXT_COLOR, 255, 255, 255, 255);
    ASSERT_EQ(1u, paint_all(&f));
    ASSERT(schultz_draw_list_at(&f.list, 0)->as.glyph_run.font != f.font);

    fixture_teardown(&f);
    PASS();
}

/*
 * The whole of the rotation design rests on this: a canvas turns what it
 * draws and nothing else. If a rotation moved the node's bounds, then layout,
 * the dirty rectangle and hit testing would all have to learn about angles,
 * and none of them do.
 */
TEST a_canvas_that_rotates_does_not_move_its_own_bounds(void)
{
    widget_fixture f;
    schultz_handle canvas;
    schultz_rect before;
    schultz_rect after;
    schultz_paint ink = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_create(f.tree,
                              schultz_tree_root(f.tree), &canvas));
    schultz_node_set_bounds(f.tree, canvas, schultz_rect_make(10, 20, 60, 40));
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, canvas,
                                                       &before));

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_rotation_begin(f.tree, canvas, 37.0f,
                                                       30.0f, 20.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas,
                              schultz_rect_make(0, 0, 200, 200), ink, 0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_rotation_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, canvas,
                                                       &after));
    ASSERT_EQ(before.x, after.x);
    ASSERT_EQ(before.y, after.y);
    ASSERT_EQ(before.width, after.width);
    ASSERT_EQ(before.height, after.height);

    /* And the commands really were recorded, so this is not measuring an
     * empty canvas. */
    ASSERT_EQ(3u, schultz_canvas_count(f.tree, canvas));

    fixture_teardown(&f);
    PASS();
}

SUITE(widget)
{
    RUN_TEST(a_canvas_that_rotates_does_not_move_its_own_bounds);
    RUN_TEST(a_labels_size_comes_from_the_style_not_the_font_handle);
    RUN_TEST(a_node_just_outside_the_dirty_rectangle_still_paints);
    RUN_TEST(a_node_with_no_widget_paints_nothing);
    RUN_TEST(setting_a_widget_records_its_type_and_data);
    RUN_TEST(widget_accessors_reject_stale_handles);
    RUN_TEST(the_paint_walk_rejects_bad_arguments);
    RUN_TEST(children_paint_after_their_parent);
    RUN_TEST(later_siblings_paint_over_earlier_ones);
    RUN_TEST(a_hidden_subtree_paints_nothing);
    RUN_TEST(a_node_outside_the_dirty_rect_is_skipped);
    RUN_TEST(an_unsized_parent_does_not_cull_its_children);
    RUN_TEST(a_clipping_node_brackets_its_children);
    RUN_TEST(clips_balance_across_nesting);
    RUN_TEST(a_panel_paints_nothing_until_it_is_styled);
    RUN_TEST(a_panel_fills_and_strokes_from_its_resolved_style);
    RUN_TEST(a_nested_panel_paints_at_its_absolute_position);
    RUN_TEST(a_panel_follows_a_theme_change);
    RUN_TEST(a_horizontal_separator_measures_thin_and_draws_across);
    RUN_TEST(a_vertical_separator_swaps_the_axes);
    RUN_TEST(an_invisible_separator_emits_nothing);
    RUN_TEST(an_ellipse_fills_and_strokes_its_bounds);
    RUN_TEST(an_unstyled_ellipse_emits_nothing);
    RUN_TEST(a_line_draws_between_its_points_offset_by_its_bounds);
    RUN_TEST(a_line_measures_to_hold_its_points);
    RUN_TEST(moving_a_line_invalidates_it);
    RUN_TEST(a_path_fills_its_points_offset_by_its_bounds);
    RUN_TEST(a_path_copies_the_points_it_is_given);
    RUN_TEST(a_path_measures_to_the_extent_of_its_points);
    RUN_TEST(replacing_a_paths_points_invalidates_it);
    RUN_TEST(path_creation_rejects_too_few_points);
    RUN_TEST(a_label_keeps_and_returns_its_text);
    RUN_TEST(label_accessors_reject_other_widgets);
    RUN_TEST(a_label_without_a_font_system_measures_to_nothing);
    RUN_TEST(a_label_measures_to_its_text);
    RUN_TEST(a_wrapped_label_gets_taller_as_it_gets_narrower);
    RUN_TEST(a_label_emits_a_glyph_run_per_line);
    RUN_TEST(a_wrapped_label_emits_one_run_per_line);
    RUN_TEST(an_empty_label_emits_nothing);
    RUN_TEST(a_labels_colour_inherits_from_its_parent);
    RUN_TEST(widgets_compose_in_a_pane);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(widget);
    GREATEST_MAIN_END();
}
