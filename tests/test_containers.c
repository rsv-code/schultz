/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_containers.c - Canvas and the tier 3 containers: GroupBox, SplitPane
 * and TabView.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_widgets.h"

#define FONT_PATH "assets/fonts/DejaVuSans.ttf"

typedef struct {
    schultz_tree        *tree;
    schultz_events      *events;
    schultz_arena        arena;
    schultz_draw_list    list;
    schultz_font_system *fonts;
    schultz_glyph_cache *glyphs;
    schultz_handle       font;
    schultz_theme        theme;
} box_fixture;

static int32_t fixture_setup(box_fixture *f)
{
    int32_t result;

    memset(f, 0, sizeof(*f));
    result = schultz_tree_create(&f->tree);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(f->tree, schultz_rect_make(0, 0, 800, 600));
    result = schultz_font_system_create(&f->fonts);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_glyph_cache_create(f->fonts, &f->glyphs);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_font_load_file(f->fonts, FONT_PATH, 16.0f, &f->font);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_font_system(f->tree, f->fonts);
    schultz_theme_init(&f->theme);
    schultz_theme_set_font(&f->theme, SCHULTZ_TOKEN_FONT_BODY, f->font);
    schultz_tree_set_theme(f->tree, &f->theme);

    result = schultz_events_create(f->tree, &f->events);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_arena_init(&f->arena, 0);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_draw_list_init(&f->list, &f->arena, 0);
}

static void fixture_teardown(box_fixture *f)
{
    schultz_arena_free(&f->arena);
    schultz_events_destroy(f->events);
    schultz_tree_destroy(f->tree);
    schultz_glyph_cache_destroy(f->glyphs);
    schultz_font_system_destroy(f->fonts);
}

static uint32_t paint_all(box_fixture *f)
{
    schultz_arena_reset(&f->arena);
    schultz_draw_list_init(&f->list, &f->arena, 0);
    schultz_tree_resolve_styles(f->tree);
    schultz_widget_paint_tree(f->tree, &f->list, &f->arena,
                              schultz_rect_make(0, 0, 0, 0));
    return schultz_draw_list_count(&f->list);
}

static uint32_t count_kind(box_fixture *f, uint32_t kind)
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

static void place(box_fixture *f, schultz_handle node, float x, float y,
                  float w, float h)
{
    schultz_tree_resolve_styles(f->tree);
    schultz_node_set_bounds(f->tree, node, schultz_rect_make(x, y, w, h));
    schultz_layout_arrange(f->tree, node, schultz_rect_make(x, y, w, h));
}

/* --------------------------------------------------------------- Canvas */

TEST a_canvas_keeps_what_was_recorded_into_it(void)
{
    box_fixture f;
    schultz_handle canvas;
    schultz_point tri[3];

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_create(f.tree,
                    schultz_tree_root(f.tree), &canvas));
    ASSERT_EQ(0u, schultz_canvas_count(f.tree, canvas));

    tri[0] = schultz_point_make(0, 0);
    tri[1] = schultz_point_make(10, 0);
    tri[2] = schultz_point_make(5, 10);

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    schultz_canvas_fill_rect(f.tree, canvas, schultz_rect_make(0, 0, 5, 5),
        schultz_paint_solid(schultz_color_rgba(1, 2, 3, 255)), 0.0f);
    schultz_canvas_stroke_rect(f.tree, canvas, schultz_rect_make(0, 0, 5, 5),
        schultz_stroke_solid(schultz_color_rgba(1, 2, 3, 255), 1.0f), 0.0f);
    schultz_canvas_fill_ellipse(f.tree, canvas, schultz_rect_make(0, 0, 4, 4),
        schultz_paint_solid(schultz_color_rgba(1, 2, 3, 255)));
    schultz_canvas_line(f.tree, canvas, tri[0], tri[1],
        schultz_stroke_solid(schultz_color_rgba(1, 2, 3, 255), 1.0f));
    schultz_canvas_fill_polygon(f.tree, canvas, tri, 3,
        schultz_paint_solid(schultz_color_rgba(1, 2, 3, 255)),
        SCHULTZ_FILL_NONZERO);
    schultz_canvas_text(f.tree, canvas, f.font, "42", 0.0f, 10.0f,
                        schultz_paint_solid(schultz_color_rgba(1, 2, 3, 255)));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));

    ASSERT_EQ(6u, schultz_canvas_count(f.tree, canvas));

    /* Recording again clears what was there rather than adding to it. */
    schultz_canvas_begin(f.tree, canvas);
    schultz_canvas_fill_rect(f.tree, canvas, schultz_rect_make(0, 0, 1, 1),
        schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255)), 0.0f);
    schultz_canvas_end(f.tree, canvas);
    ASSERT_EQ(1u, schultz_canvas_count(f.tree, canvas));

    fixture_teardown(&f);
    PASS();
}

TEST a_canvas_replays_its_drawing_where_it_was_laid_out(void)
{
    box_fixture f;
    schultz_handle canvas;
    uint32_t i;
    int32_t saw_transform = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_canvas_create(f.tree, schultz_tree_root(f.tree), &canvas);
    place(&f, canvas, 30.0f, 40.0f, 100.0f, 100.0f);

    schultz_canvas_begin(f.tree, canvas);
    schultz_canvas_fill_rect(f.tree, canvas, schultz_rect_make(0, 0, 10, 10),
        schultz_paint_solid(schultz_color_rgba(7, 7, 7, 255)), 0.0f);
    schultz_canvas_end(f.tree, canvas);

    paint_all(&f);
    /* Clipped to the canvas, and shifted to where it sits. */
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_CLIP_BEGIN));
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_CLIP_END));
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);

        if (cmd->kind == SCHULTZ_DRAW_OFFSET_BEGIN) {
            saw_transform = 1;
            ASSERT_EQ(30.0f, cmd->as.offset_begin.dx);
            ASSERT_EQ(40.0f, cmd->as.offset_begin.dy);
        }
    }
    ASSERT(saw_transform);

    fixture_teardown(&f);
    PASS();
}

TEST a_canvas_refuses_drawing_outside_a_recording(void)
{
    box_fixture f;
    schultz_handle canvas;
    schultz_handle panel;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_canvas_create(f.tree, schultz_tree_root(f.tree), &canvas);
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    /* Not recording: nothing is kept, so a half built picture cannot show. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_fill_rect(f.tree, canvas,
                  schultz_rect_make(0, 0, 1, 1),
                  schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255)),
                  0.0f));
    ASSERT_EQ(0u, schultz_canvas_count(f.tree, canvas));

    /* And a node that is not a canvas is refused throughout. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_begin(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE, schultz_canvas_end(f.tree, panel));
    ASSERT_EQ(0u, schultz_canvas_count(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_canvas_create(f.tree, schultz_tree_root(f.tree), NULL));

    fixture_teardown(&f);
    PASS();
}

TEST canvas_text_needs_a_font_system(void)
{
    box_fixture f;
    schultz_handle canvas;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_canvas_create(f.tree, schultz_tree_root(f.tree), &canvas);
    schultz_tree_set_font_system(f.tree, NULL);

    schultz_canvas_begin(f.tree, canvas);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_canvas_text(f.tree, canvas, f.font, "x", 0, 0,
                                  schultz_paint_solid(
                                      schultz_color_rgba(0, 0, 0, 255))));
    schultz_canvas_end(f.tree, canvas);
    ASSERT_EQ(0u, schultz_canvas_count(f.tree, canvas));

    fixture_teardown(&f);
    PASS();
}

/*
 * A canvas that has been asked for screen pixels brackets its drawing with
 * the command that switches the unit, and one that has not does not. That
 * bracket is the whole mechanism: everything between it is measured in
 * pixels of the screen rather than in units the toolkit scaled.
 */
TEST a_pixel_exact_canvas_brackets_its_drawing(void)
{
    box_fixture f;
    schultz_handle canvas;
    uint32_t i;
    uint32_t pushes = 0;
    uint32_t pops = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_canvas_create(f.tree, schultz_tree_root(f.tree), &canvas);
    schultz_node_set_bounds(f.tree, canvas, schultz_rect_make(0, 0, 100, 50));
    schultz_canvas_begin(f.tree, canvas);
    schultz_canvas_fill_rect(f.tree, canvas, schultz_rect_make(0, 0, 1, 1),
        schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255)), 0.0f);
    schultz_canvas_end(f.tree, canvas);

    /* Not asked for: the drawing goes out in the toolkit's own units. */
    paint_all(&f);
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        if (schultz_draw_list_at(&f.list, i)->kind ==
                SCHULTZ_DRAW_DEVICE_PIXELS_BEGIN) {
            pushes++;
        }
    }
    ASSERT_EQ(0u, pushes);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_canvas_set_pixel_exact(f.tree, canvas, 1));
    paint_all(&f);
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        uint32_t kind = schultz_draw_list_at(&f.list, i)->kind;

        if (kind == SCHULTZ_DRAW_DEVICE_PIXELS_BEGIN) { pushes++; }
        if (kind == SCHULTZ_DRAW_DEVICE_PIXELS_END)  { pops++; }
    }
    ASSERT_EQ(1u, pushes);
    ASSERT_EQ(1u, pops);

    /* And a node that is not a canvas is refused, as everywhere else. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_set_pixel_exact(f.tree,
                  schultz_tree_root(f.tree), 1));

    fixture_teardown(&f);
    PASS();
}

/*
 * The canvas answers in screen pixels so that a host never has to multiply
 * by the scale itself, which is the step everyone gets wrong.
 */
TEST a_canvas_reports_its_size_in_screen_pixels(void)
{
    box_fixture f;
    schultz_handle canvas;
    uint32_t w = 0;
    uint32_t h = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_canvas_create(f.tree, schultz_tree_root(f.tree), &canvas);
    schultz_node_set_bounds(f.tree, canvas, schultz_rect_make(0, 0, 100, 50));

    /* Nothing set a scale, which is the truth on an ordinary monitor. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_canvas_pixel_size(f.tree, canvas, &w, &h));
    ASSERT_EQ(100u, w);
    ASSERT_EQ(50u, h);

    /* A phone that fits three of its pixels into one unit. */
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_set_pixel_scale(f.tree, 3.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_canvas_pixel_size(f.tree, canvas, &w, &h));
    ASSERT_EQ(300u, w);
    ASSERT_EQ(150u, h);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_canvas_pixel_size(f.tree, canvas, NULL, &h));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_pixel_size(f.tree, schultz_tree_root(f.tree),
                                        &w, &h));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------- GroupBox */

TEST a_group_box_leaves_a_gap_for_its_title(void)
{
    box_fixture f;
    schultz_handle group;
    schultz_handle content;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_group_box_create(f.tree,
                    schultz_tree_root(f.tree), "Options", &group));
    content = schultz_group_box_content(f.tree, group);
    ASSERT(content != SCHULTZ_HANDLE_NONE);
    ASSERT_STR_EQ("Options", schultz_node_get_name(f.tree, group));

    place(&f, group, 0, 0, 200, 100);
    paint_all(&f);

    /*
     * The border is five segments, not a rectangle: the top edge is in two
     * pieces with the title between them, then both sides and the bottom.
     */
    ASSERT_EQ(5u, count_kind(&f, SCHULTZ_DRAW_LINE));
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN));

    fixture_teardown(&f);
    PASS();
}

TEST a_group_box_sizes_itself_around_its_contents(void)
{
    box_fixture f;
    schultz_handle group;
    schultz_handle content;
    schultz_handle inner;
    schultz_size small;
    schultz_size large;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_group_box_create(f.tree, schultz_tree_root(f.tree), "T", &group);
    content = schultz_group_box_content(f.tree, group);
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, group, -1, -1, &small);

    schultz_panel_create(f.tree, content, &inner);
    schultz_node_set_style_property(f.tree, content, SCHULTZ_PROP_PREF_HEIGHT,
                                    schultz_value_number(120.0f));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, group, -1, -1, &large);

    ASSERT(large.height > small.height);
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_group_box_content(f.tree, inner));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------ SplitPane */

TEST a_split_pane_divides_its_room_between_two_halves(void)
{
    box_fixture f;
    schultz_handle split;
    schultz_rect left;
    schultz_rect right;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_split_pane_create(f.tree,
                    schultz_tree_root(f.tree), SCHULTZ_ORIENT_HORIZONTAL,
                    &split));
    ASSERT_EQ(0.5f, schultz_split_pane_position(f.tree, split));

    place(&f, split, 0, 0, 200, 100);
    schultz_node_absolute_bounds(f.tree,
        schultz_split_pane_half(f.tree, split, 0), &left);
    schultz_node_absolute_bounds(f.tree,
        schultz_split_pane_half(f.tree, split, 1), &right);

    ASSERT(left.width > 90.0f && left.width < 100.0f);
    ASSERT(right.x > left.x + left.width);
    ASSERT_EQ(100.0f, left.height);

    /* Moving the divider gives one half more and the other less. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_split_pane_set_position(f.tree, split, 0.25f));
    place(&f, split, 0, 0, 200, 100);
    schultz_node_absolute_bounds(f.tree,
        schultz_split_pane_half(f.tree, split, 0), &left);
    ASSERT(left.width < 60.0f);
    ASSERT_EQ(0.25f, schultz_split_pane_position(f.tree, split));

    fixture_teardown(&f);
    PASS();
}

TEST dragging_a_split_divider_moves_it(void)
{
    box_fixture f;
    schultz_handle split;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_split_pane_create(f.tree, schultz_tree_root(f.tree),
                              SCHULTZ_ORIENT_HORIZONTAL, &split);
    place(&f, split, 0, 0, 200, 100);

    /* Press on the divider itself, which sits in the middle. */
    schultz_events_mouse_move(f.events, schultz_point_make(99, 50), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(99, 50),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_move(f.events, schultz_point_make(150, 50), 0);
    ASSERT(schultz_split_pane_position(f.tree, split) > 0.6f);
    schultz_events_mouse_button(f.events, schultz_point_make(150, 50),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    /* Past the end clamps rather than running off. */
    schultz_split_pane_set_position(f.tree, split, 0.5f);
    place(&f, split, 0, 0, 200, 100);
    schultz_events_mouse_move(f.events, schultz_point_make(99, 50), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(99, 50),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_move(f.events, schultz_point_make(9999, 50), 0);
    ASSERT_EQ(1.0f, schultz_split_pane_position(f.tree, split));

    fixture_teardown(&f);
    PASS();
}

TEST pressing_a_split_half_does_not_move_the_divider(void)
{
    box_fixture f;
    schultz_handle split;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_split_pane_create(f.tree, schultz_tree_root(f.tree),
                              SCHULTZ_ORIENT_HORIZONTAL, &split);
    place(&f, split, 0, 0, 200, 100);

    schultz_events_mouse_move(f.events, schultz_point_make(20, 50), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(20, 50),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_move(f.events, schultz_point_make(180, 50), 0);
    ASSERT_EQ(0.5f, schultz_split_pane_position(f.tree, split));
    schultz_events_mouse_button(f.events, schultz_point_make(180, 50),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    fixture_teardown(&f);
    PASS();
}

/* Every container that hands back a node to fill gives that node a pane. */
TEST every_content_node_measures_what_is_inside_it(void)
{
    box_fixture f;
    schultz_handle group;
    schultz_handle split;
    schultz_handle view;
    schultz_handle tabs;
    schultz_handle page = SCHULTZ_HANDLE_NONE;
    schultz_handle inner = SCHULTZ_HANDLE_NONE;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));

    /* A group box measures around what it holds. */
    schultz_group_box_create(f.tree, schultz_tree_root(f.tree), "T", &group);
    schultz_button_create(f.tree, schultz_group_box_content(f.tree, group),
                          "Wide caption here", &inner);
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, group, -1, -1, &size);
    ASSERT(size.width > 100.0f);

    /* So does a split pane's half. */
    schultz_split_pane_create(f.tree, schultz_tree_root(f.tree),
                              SCHULTZ_ORIENT_HORIZONTAL, &split);
    schultz_button_create(f.tree, schultz_split_pane_half(f.tree, split, 0),
                          "Wide caption here", &inner);
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, split, -1, -1, &size);
    ASSERT(size.width > 100.0f);

    /* And a scroll view's content, which is what tells it there is more. */
    schultz_scroll_view_create(f.tree, schultz_tree_root(f.tree), &view);
    schultz_button_create(f.tree, schultz_scroll_view_content(f.tree, view),
                          "Wide caption here", &inner);
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, view, -1, -1, &size);
    ASSERT(size.width > 100.0f);

    /*
     * And a tab view's page, which used to be the exception on the grounds
     * that an application arranges it however it likes. So does a dialog's
     * content and a group box's, and both of those hold a stack pane, so the
     * exception only made the odd one out: a page with no pane arranges
     * nothing, and everything put in it keeps the empty bounds it was born
     * with and never appears. An application that wants its own pane sets
     * one, exactly as it does for every other content node.
     */
    schultz_tab_view_create(f.tree, schultz_tree_root(f.tree), &tabs);
    schultz_tab_view_add(f.tree, tabs, "One", &page);
    ASSERT(schultz_node_get_pane(f.tree, page) != NULL);
    schultz_button_create(f.tree, page, "Wide caption here", &inner);
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, tabs, -1, -1, &size);
    ASSERT(size.width > 100.0f);

    fixture_teardown(&f);
    PASS();
}

/* Counts clicks on one node, since this suite routes input but has no host
 * recorder of its own. */
static schultz_handle scroll_watched;
static int32_t scroll_clicks;

static int32_t scroll_heard(void *context, const schultz_event *event)
{
    (void)context;
    if (event->type == SCHULTZ_EVENT_CLICK && event->target == scroll_watched) {
        scroll_clicks++;
    }
    return SCHULTZ_OK;
}

/* A scroll view whose content is taller than it is, and the clock a tick
 * needs. Returns its vertical bar, which is where the offset is read. */
static schultz_handle scroller(box_fixture *f, schultz_handle *out_view,
                               schultz_handle *out_row)
{
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle content;
    schultz_handle filler = SCHULTZ_HANDLE_NONE;

    schultz_node_set_bounds(f->tree, schultz_tree_root(f->tree),
                            schultz_rect_make(0, 0, 300, 200));
    schultz_scroll_view_create(f->tree, schultz_tree_root(f->tree), &view);
    schultz_node_set_bounds(f->tree, view, schultz_rect_make(0, 0, 300, 200));
    content = schultz_scroll_view_content(f->tree, view);
    schultz_node_set_pane(f->tree, content, schultz_pane_vbox());
    schultz_button_create(f->tree, content, "A row", out_row);
    schultz_node_set_pref_size(f->tree, *out_row, 260.0f, 60.0f);
    schultz_panel_create(f->tree, content, &filler);
    schultz_node_set_pref_size(f->tree, filler, 260.0f, 800.0f);
    schultz_tree_resolve_styles(f->tree);
    schultz_layout_run(f->tree);
    *out_view = view;
    return schultz_scroll_view_bar(f->tree, view, SCHULTZ_ORIENT_VERTICAL);
}

/* One turn of a frame loop: move the clock, let anything animating run. */
static uint64_t scroll_clock = 1000u;

static void scroll_advance(box_fixture *f, uint32_t ms)
{
    scroll_clock += ms;
    schultz_events_set_time(f->events, scroll_clock);
    schultz_tree_advance(f->tree, scroll_clock);
}

/* Drags a finger down the middle of the view, in steps, with the clock
 * running between them the way a real loop does. */
static void scroll_finger(box_fixture *f, float from_y, float to_y,
                          int32_t steps)
{
    int32_t i;

    schultz_events_set_pointer_source(f->events, SCHULTZ_POINTER_TOUCH);
    schultz_events_mouse_move(f->events, schultz_point_make(120, from_y), 0);
    schultz_events_mouse_button(f->events, schultz_point_make(120, from_y),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    for (i = 1; i <= steps; i++) {
        float y = from_y + (to_y - from_y) * (float)i / (float)steps;

        scroll_advance(f, 16u);
        schultz_events_mouse_move(f->events, schultz_point_make(120, y), 0);
    }
    scroll_advance(f, 16u);
    schultz_events_mouse_button(f->events, schultz_point_make(120, to_y),
                                SCHULTZ_BUTTON_LEFT, 0, 0);
    schultz_events_set_pointer_source(f->events, SCHULTZ_POINTER_MOUSE);
}

TEST a_finger_drags_the_content_with_it(void)
{
    box_fixture f;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_handle bar;

    /*
     * A phone has no wheel, and the wheel was the only thing that moved a
     * scroll view. The only other way was the bar itself, eight units wide
     * against a documented touch target minimum of forty four.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    bar = scroller(&f, &view, &row);
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, bar));

    /* Dragged up a hundred, so a hundred further down the content. */
    scroll_finger(&f, 150.0f, 50.0f, 8);
    ASSERT_EQ(100.0f, schultz_scroll_bar_value(f.tree, bar));

    fixture_teardown(&f);
    PASS();
}

/*
 * A page holding a list, which is itself a scroll view. Returns the page's
 * bar; the list and its own bar come back through the out parameters.
 */
static schultz_handle page_with_list(box_fixture *f, uint32_t rows,
                                     schultz_handle *out_list,
                                     schultz_handle *out_list_bar)
{
    schultz_handle page = SCHULTZ_HANDLE_NONE;
    schultz_handle content;
    schultz_handle filler = SCHULTZ_HANDLE_NONE;
    schultz_handle inner = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    uint32_t i;

    schultz_node_set_bounds(f->tree, schultz_tree_root(f->tree),
                            schultz_rect_make(0, 0, 300, 200));
    schultz_scroll_view_create(f->tree, schultz_tree_root(f->tree), &page);
    schultz_node_set_bounds(f->tree, page, schultz_rect_make(0, 0, 300, 200));
    content = schultz_scroll_view_content(f->tree, page);
    schultz_node_set_pane(f->tree, content, schultz_pane_vbox());

    schultz_list_view_create(f->tree, content, out_list);
    for (i = 0; i < rows; i++) {
        schultz_list_view_add(f->tree, *out_list, &row);
    }
    schultz_node_set_pref_size(f->tree, *out_list, 260.0f, 100.0f);

    schultz_panel_create(f->tree, content, &filler);
    schultz_node_set_pref_size(f->tree, filler, 260.0f, 800.0f);
    schultz_tree_resolve_styles(f->tree);
    schultz_layout_run(f->tree);

    schultz_node_child_at(f->tree, *out_list, 0, &inner);
    *out_list_bar = schultz_scroll_view_bar(f->tree, inner,
                                            SCHULTZ_ORIENT_VERTICAL);
    return schultz_scroll_view_bar(f->tree, page, SCHULTZ_ORIENT_VERTICAL);
}

TEST a_list_with_nothing_to_scroll_lets_the_page_scroll(void)
{
    box_fixture f;
    schultz_handle list = SCHULTZ_HANDLE_NONE;
    schultz_handle list_bar = SCHULTZ_HANDLE_NONE;
    schultz_handle page_bar;

    /*
     * A list view is a scroll view inside another node, so a finger on one
     * of its rows reaches the list before it reaches the page. It used to
     * take the drag whether or not it had anywhere to go, and a page with a
     * short list on it could not be scrolled by touching the list.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    page_bar = page_with_list(&f, 2u, &list, &list_bar);
    ASSERT_EQ(0.0f, schultz_scroll_bar_maximum(f.tree, list_bar));

    /* Dragged up fifty, over the list, so the page moves fifty. */
    scroll_finger(&f, 80.0f, 30.0f, 8);
    ASSERT_EQ(50.0f, schultz_scroll_bar_value(f.tree, page_bar));
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, list_bar));

    fixture_teardown(&f);
    PASS();
}

TEST a_list_that_can_scroll_keeps_the_drag_to_itself(void)
{
    box_fixture f;
    schultz_handle list = SCHULTZ_HANDLE_NONE;
    schultz_handle list_bar = SCHULTZ_HANDLE_NONE;
    schultz_handle page_bar;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    page_bar = page_with_list(&f, 40u, &list, &list_bar);
    ASSERT(schultz_scroll_bar_maximum(f.tree, list_bar) > 50.0f);

    /* The list has somewhere to go, so it goes and the page stays. */
    scroll_finger(&f, 80.0f, 30.0f, 8);
    ASSERT_EQ(50.0f, schultz_scroll_bar_value(f.tree, list_bar));
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, page_bar));

    /* At the end of the list the page takes over, rather than the gesture
     * dying against the last row. */
    schultz_scroll_bar_set_value(f.tree, list_bar,
        schultz_scroll_bar_maximum(f.tree, list_bar));
    scroll_finger(&f, 80.0f, 30.0f, 8);
    ASSERT_EQ(50.0f, schultz_scroll_bar_value(f.tree, page_bar));

    fixture_teardown(&f);
    PASS();
}

TEST a_flick_carries_on_after_the_finger_lifts(void)
{
    box_fixture f;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_handle bar;
    float lifted;
    float landed;
    int32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    bar = scroller(&f, &view, &row);

    scroll_finger(&f, 150.0f, 50.0f, 8);
    lifted = schultz_scroll_bar_value(f.tree, bar);

    /* Left to run, it keeps going and then stops of its own accord. */
    for (i = 0; i < 400; i++) {
        scroll_advance(&f, 16u);
    }
    landed = schultz_scroll_bar_value(f.tree, bar);
    ASSERT(landed > lifted);
    ASSERT_FALSE(schultz_node_animating(f.tree, view));

    /* And it stays there rather than drifting on. */
    for (i = 0; i < 60; i++) {
        scroll_advance(&f, 16u);
    }
    ASSERT_EQ(landed, schultz_scroll_bar_value(f.tree, bar));

    fixture_teardown(&f);
    PASS();
}

TEST a_scroll_that_starts_on_a_row_does_not_press_it(void)
{
    box_fixture f;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_handle bar;

    /*
     * The reason the router had to learn about taps. Content that scrolls
     * with the finger carries the row it started on along underneath it, so
     * the row is still what the release lands on and the old rule about
     * dragging off a button never fires.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    bar = scroller(&f, &view, &row);

    /* A tap on the row presses it and scrolls nothing. */
    scroll_watched = row;
    scroll_clicks = 0;
    schultz_events_set_callback(f.events, scroll_heard, NULL);
    scroll_finger(&f, 30.0f, 32.0f, 2);
    ASSERT_EQ(1, scroll_clicks);
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, bar));

    /* A drag from the same place scrolls and presses nothing. */
    schultz_scroll_view_scroll_to(f.tree, view,
                                  schultz_point_make(0.0f, 0.0f));
    scroll_clicks = 0;
    scroll_finger(&f, 30.0f, -70.0f, 8);
    ASSERT_EQ(0, scroll_clicks);
    ASSERT(schultz_scroll_bar_value(f.tree, bar) > 0.0f);

    fixture_teardown(&f);
    PASS();
}

TEST a_cursor_does_not_drag_the_content(void)
{
    box_fixture f;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_handle bar;

    /* A mouse has a wheel and a bar. Dragging the content with a cursor
     * would take the gesture away from selecting text. */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    bar = scroller(&f, &view, &row);

    schultz_events_mouse_move(f.events, schultz_point_make(120, 150), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(120, 150),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_move(f.events, schultz_point_make(120, 50), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(120, 50),
                                SCHULTZ_BUTTON_LEFT, 0, 0);
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, bar));

    /* The wheel still does, exactly as before. */
    schultz_events_scroll(f.events, schultz_point_make(120, 100), 0.0f, 1.0f);
    ASSERT(schultz_scroll_bar_value(f.tree, bar) > 0.0f);

    fixture_teardown(&f);
    PASS();
}

TEST every_content_node_arranges_what_is_put_in_it(void)
{
    box_fixture f;
    schultz_handle tabs = SCHULTZ_HANDLE_NONE;
    schultz_handle page = SCHULTZ_HANDLE_NONE;
    schultz_handle acc = SCHULTZ_HANDLE_NONE;
    schultz_handle section = SCHULTZ_HANDLE_NONE;
    schultz_handle split = SCHULTZ_HANDLE_NONE;
    schultz_handle group = SCHULTZ_HANDLE_NONE;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle root;

    /*
     * One rule, asserted in one place: a call that hands back a node to put
     * things in gives back a node that places them. The failure it guards
     * against is silent, because a node with no pane arranges nothing and
     * everything inside it keeps the empty bounds it was born with. Nothing
     * errors and nothing appears.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    root = schultz_tree_root(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_create(f.tree, root, &tabs));
    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_add(f.tree, tabs, "One", &page));
    ASSERT(schultz_node_get_pane(f.tree, page) != NULL);

    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_create(f.tree, root, &acc));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_accordion_add(f.tree, acc, "One", &section));
    ASSERT(schultz_node_get_pane(f.tree, section) != NULL);

    ASSERT_EQ(SCHULTZ_OK, schultz_split_pane_create(f.tree, root,
                                    SCHULTZ_ORIENT_HORIZONTAL, &split));
    ASSERT(schultz_node_get_pane(f.tree,
               schultz_split_pane_half(f.tree, split, 0)) != NULL);
    ASSERT(schultz_node_get_pane(f.tree,
               schultz_split_pane_half(f.tree, split, 1)) != NULL);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_group_box_create(f.tree, root, "Group", &group));
    ASSERT(schultz_node_get_pane(f.tree,
               schultz_group_box_content(f.tree, group)) != NULL);

    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_create(f.tree, root, &view));
    ASSERT(schultz_node_get_pane(f.tree,
               schultz_scroll_view_content(f.tree, view)) != NULL);

    fixture_teardown(&f);
    PASS();
}

TEST a_tab_page_shows_what_is_put_in_it(void)
{
    box_fixture f;
    schultz_handle tabs = SCHULTZ_HANDLE_NONE;
    schultz_handle page = SCHULTZ_HANDLE_NONE;
    schultz_handle inner = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_node_set_bounds(f.tree, schultz_tree_root(f.tree),
                            schultz_rect_make(0, 0, 400, 300));
    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_create(f.tree,
                            schultz_tree_root(f.tree), &tabs));
    schultz_node_set_bounds(f.tree, tabs, schultz_rect_make(0, 0, 400, 300));
    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_add(f.tree, tabs, "One", &page));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_button_create(f.tree, page, "Press me", &inner));

    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);

    /* Somewhere real, rather than nowhere at nothing by nothing. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_absolute_bounds(f.tree, inner, &bounds));
    ASSERT(bounds.width > 0.0f);
    ASSERT(bounds.height > 0.0f);

    fixture_teardown(&f);
    PASS();
}

TEST split_pane_accessors_reject_other_widgets(void)
{
    box_fixture f;
    schultz_handle panel;
    schultz_handle none = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_split_pane_half(f.tree, panel, 0));
    ASSERT_EQ(0.0f, schultz_split_pane_position(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_split_pane_set_position(f.tree, panel, 0.5f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_split_pane_create(f.tree, schultz_tree_root(f.tree), 9,
                                        &none));

    fixture_teardown(&f);
    PASS();
}

/* -------------------------------------------------------------- TabView */

TEST a_tab_view_shows_one_page_at_a_time(void)
{
    box_fixture f;
    schultz_handle tabs;
    schultz_handle first = SCHULTZ_HANDLE_NONE;
    schultz_handle second = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_create(f.tree,
                    schultz_tree_root(f.tree), &tabs));
    ASSERT_EQ(0u, schultz_tab_view_count(f.tree, tabs));

    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_add(f.tree, tabs, "One", &first));
    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_add(f.tree, tabs, "Two", &second));
    ASSERT_EQ(2u, schultz_tab_view_count(f.tree, tabs));
    ASSERT_EQ(0u, schultz_tab_view_selected(f.tree, tabs));

    ASSERT(schultz_node_is_visible(f.tree, first));
    ASSERT_FALSE(schultz_node_is_visible(f.tree, second));

    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_select(f.tree, tabs, 1));
    ASSERT_EQ(1u, schultz_tab_view_selected(f.tree, tabs));
    ASSERT_FALSE(schultz_node_is_visible(f.tree, first));
    ASSERT(schultz_node_is_visible(f.tree, second));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tab_view_select(f.tree, tabs, 9));

    fixture_teardown(&f);
    PASS();
}

TEST clicking_a_tab_shows_its_page(void)
{
    box_fixture f;
    schultz_handle tabs;
    schultz_handle first = SCHULTZ_HANDLE_NONE;
    schultz_handle second = SCHULTZ_HANDLE_NONE;
    schultz_handle strip;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    schultz_rect at;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_tab_view_create(f.tree, schultz_tree_root(f.tree), &tabs);
    schultz_tab_view_add(f.tree, tabs, "One", &first);
    schultz_tab_view_add(f.tree, tabs, "Two", &second);
    place(&f, tabs, 0, 0, 300, 200);

    /* The strip is the tab view's first child; its second button is "Two". */
    schultz_node_child_at(f.tree, tabs, 0, &strip);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, strip, 1, &button));
    schultz_node_absolute_bounds(f.tree, button, &at);

    {
        schultz_point centre = schultz_point_make(at.x + at.width * 0.5f,
                                                  at.y + at.height * 0.5f);
        schultz_events_mouse_move(f.events, centre, 0);
        schultz_events_mouse_button(f.events, centre, SCHULTZ_BUTTON_LEFT, 1,
                                    0);
        schultz_events_mouse_button(f.events, centre, SCHULTZ_BUTTON_LEFT, 0,
                                    0);
    }

    ASSERT_EQ(1u, schultz_tab_view_selected(f.tree, tabs));

    fixture_teardown(&f);
    PASS();
}

TEST tab_view_accessors_reject_other_widgets(void)
{
    box_fixture f;
    schultz_handle panel;
    schultz_handle page = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ(0u, schultz_tab_view_count(f.tree, panel));
    ASSERT_EQ(0u, schultz_tab_view_selected(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_tab_view_add(f.tree, panel, "x", &page));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_tab_view_select(f.tree, panel, 0));

    fixture_teardown(&f);
    PASS();
}

TEST a_tab_view_holds_only_so_many(void)
{
    box_fixture f;
    schultz_handle tabs;
    schultz_handle page = SCHULTZ_HANDLE_NONE;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_tab_view_create(f.tree, schultz_tree_root(f.tree), &tabs);
    for (i = 0; i < 16u; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_add(f.tree, tabs, "T", &page));
    }
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED,
              schultz_tab_view_add(f.tree, tabs, "T", &page));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------- reveal */

/* Builds a scroll view holding a tall column of fixed height rows. */
static schultz_handle reveal_setup(schultz_tree *tree, schultz_handle *rows,
                                   uint32_t count)
{
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle content;
    uint32_t i;

    if (schultz_scroll_view_create(tree, schultz_tree_root(tree), &view)
            != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    schultz_node_set_bounds(tree, view, schultz_rect_make(0, 0, 100, 100));
    content = schultz_scroll_view_content(tree, view);
    schultz_node_set_pane(tree, content, schultz_pane_vbox());

    for (i = 0; i < count; i++) {
        if (schultz_node_create(tree, content, &rows[i]) != SCHULTZ_OK) {
            return SCHULTZ_HANDLE_NONE;
        }
        schultz_node_set_pref_size(tree, rows[i], 80.0f, 50.0f);
    }
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);
    return view;
}

/*
 * A row scrolled out of sight must not be clickable.
 *
 * A scroll view clips its children and then moves them by the scroll offset,
 * so a row scrolled past the top keeps bounds that are simply somewhere else
 * on screen. The painter clips it away and the user never sees it. If hit
 * testing does not clip too, that invisible row sits over whatever is drawn
 * above the scroll view and swallows every press meant for it. What the user
 * sees is a tab strip or a header that has stopped working, with nothing on
 * screen to explain why.
 */
TEST a_row_scrolled_out_of_sight_is_not_clickable(void)
{
    schultz_tree *tree;
    schultz_events *events;
    schultz_handle rows[10];
    schultz_handle view;
    schultz_handle above;
    schultz_handle hit = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));

    /* Something above the scroll view, the way a header or a tab strip is. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &above));
    schultz_node_set_bounds(tree, above, schultz_rect_make(0, 0, 400, 30));

    /* The scroll view is created after it, so it is the later sibling and
     * would win a hit test on any overlap. */
    view = reveal_setup(tree, rows, 10u);
    ASSERT(view != SCHULTZ_HANDLE_NONE);
    schultz_node_set_bounds(tree, view, schultz_rect_make(0, 40, 100, 100));
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);

    /* Before scrolling, the header answers for its own area. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(events,
                                        schultz_point_make(50, 15), &hit));
    ASSERT_EQ(above, hit);

    /* Scroll far enough that a row's bounds land over the header. */
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_scroll_to(tree, view,
                                        schultz_point_make(0.0f, 200.0f)));
    schultz_layout_run(tree);

    /*
     * Row 3 now spans y -10 to 40: scrolled off the top of a view that
     * starts at 40, so none of it is drawn, and it lies right across the
     * header at 0 to 30. That is the precondition, not the bug.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(tree, rows[3],
                                                       &bounds));
    ASSERT(bounds.y < 40.0f);
    ASSERT(bounds.y + bounds.height > 15.0f);

    /* The header still answers. The invisible row does not. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(events,
                                        schultz_point_make(50, 15), &hit));
    ASSERT_EQ(above, hit);

    /* And a row that is genuinely visible is still reachable, so the fix
     * has not simply switched hit testing off inside the scroll view. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(events,
                                        schultz_point_make(50, 60), &hit));
    ASSERT(hit != SCHULTZ_HANDLE_NONE);
    ASSERT(hit != above);

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

TEST revealing_a_node_below_the_viewport_scrolls_down(void)
{
    schultz_tree *tree;
    schultz_handle rows[10];
    schultz_handle view;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));
    view = reveal_setup(tree, rows, 10u);
    ASSERT(view != SCHULTZ_HANDLE_NONE);

    /* Row 5 sits at y 250 in a viewport 100 tall, so it is out of sight. */
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_reveal(tree, view, rows[5]));
    schultz_node_absolute_bounds(tree, rows[5], &bounds);
    /* Its far edge is now at the viewport's far edge, and no further. */
    ASSERT_EQ(50.0f, bounds.y);
    ASSERT_EQ(100.0f, bounds.y + bounds.height);

    schultz_tree_destroy(tree);
    PASS();
}

TEST revealing_a_node_above_the_viewport_scrolls_back(void)
{
    schultz_tree *tree;
    schultz_handle rows[10];
    schultz_handle view;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));
    view = reveal_setup(tree, rows, 10u);
    ASSERT(view != SCHULTZ_HANDLE_NONE);

    schultz_scroll_view_scroll_to(tree, view, schultz_point_make(0.0f,
                                                                300.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_reveal(tree, view, rows[1]));
    schultz_node_absolute_bounds(tree, rows[1], &bounds);
    /* Its near edge is brought to the viewport's near edge. */
    ASSERT_EQ(0.0f, bounds.y);

    schultz_tree_destroy(tree);
    PASS();
}

TEST revealing_a_node_already_in_view_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle rows[10];
    schultz_handle view;
    schultz_handle bar;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));
    view = reveal_setup(tree, rows, 10u);
    ASSERT(view != SCHULTZ_HANDLE_NONE);
    bar = schultz_scroll_view_bar(tree, view, SCHULTZ_ORIENT_VERTICAL);

    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_reveal(tree, view, rows[0]));
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(tree, bar));

    schultz_tree_destroy(tree);
    PASS();
}

/* The target need not be a direct child of the content. */
TEST revealing_reaches_a_node_nested_inside_a_row(void)
{
    schultz_tree *tree;
    schultz_handle rows[10];
    schultz_handle view;
    schultz_handle inner = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));
    view = reveal_setup(tree, rows, 10u);
    ASSERT(view != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, rows[7], &inner));
    schultz_node_set_bounds(tree, inner, schultz_rect_make(0, 10, 20, 20));

    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_reveal(tree, view, inner));
    schultz_node_absolute_bounds(tree, inner, &bounds);
    ASSERT(bounds.y >= 0.0f);
    ASSERT(bounds.y + bounds.height <= 100.0f);

    schultz_tree_destroy(tree);
    PASS();
}

/* A node taller than the viewport is shown from its near edge. */
TEST revealing_a_node_taller_than_the_view_shows_its_start(void)
{
    schultz_tree *tree;
    schultz_handle rows[4];
    schultz_handle view;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));
    view = reveal_setup(tree, rows, 4u);
    ASSERT(view != SCHULTZ_HANDLE_NONE);

    /* Three times the viewport, so revealing it has to scroll. */
    schultz_node_set_pref_size(tree, rows[2], 80.0f, 300.0f);
    schultz_layout_run(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_reveal(tree, view, rows[2]));
    schultz_node_absolute_bounds(tree, rows[2], &bounds);
    ASSERT_EQ(0.0f, bounds.y);

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------------- accordion */

TEST an_accordion_opens_and_closes_a_section(void)
{
    schultz_tree *tree;
    schultz_handle acc = SCHULTZ_HANDLE_NONE;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_handle header = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_create(tree,
        schultz_tree_root(tree), &acc));
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_add(tree, acc, "Details", &body));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, acc, 0, &header));

    /* Sections start closed, so the body is not drawn. */
    ASSERT_EQ(0, schultz_accordion_is_expanded(tree, header));
    ASSERT_EQ(0, schultz_node_is_visible(tree, body));

    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_expand(tree, acc, header, 1));
    ASSERT_EQ(1, schultz_accordion_is_expanded(tree, header));
    ASSERT_EQ(1, schultz_node_is_visible(tree, body));

    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_expand(tree, acc, header, 0));
    ASSERT_EQ(0, schultz_node_is_visible(tree, body));

    schultz_tree_destroy(tree);
    PASS();
}

TEST single_expand_closes_whichever_section_was_open(void)
{
    schultz_tree *tree;
    schultz_handle acc = SCHULTZ_HANDLE_NONE;
    schultz_handle first = SCHULTZ_HANDLE_NONE;
    schultz_handle second = SCHULTZ_HANDLE_NONE;
    schultz_handle body = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_create(tree,
        schultz_tree_root(tree), &acc));
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_add(tree, acc, "One", &body));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, acc, 0, &first));
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_add(tree, acc, "Two", &body));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, acc, 2, &second));

    /* Many at once by default. */
    schultz_accordion_expand(tree, acc, first, 1);
    schultz_accordion_expand(tree, acc, second, 1);
    ASSERT_EQ(1, schultz_accordion_is_expanded(tree, first));
    ASSERT_EQ(1, schultz_accordion_is_expanded(tree, second));

    /* One at a time from here on. */
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_set_single_expand(tree, acc, 1));
    schultz_accordion_expand(tree, acc, first, 1);
    ASSERT_EQ(1, schultz_accordion_is_expanded(tree, first));
    ASSERT_EQ(0, schultz_accordion_is_expanded(tree, second));

    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------------------------ toolbar */

TEST a_toolbar_collapses_what_does_not_fit(void)
{
    schultz_tree *tree;
    schultz_handle bar = SCHULTZ_HANDLE_NONE;
    schultz_handle items[5];
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 800, 600));
    ASSERT_EQ(SCHULTZ_OK, schultz_toolbar_create(tree, schultz_tree_root(tree),
        SCHULTZ_ORIENT_HORIZONTAL, &bar));
    for (i = 0; i < 5u; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_toolbar_add(tree, bar, "Item",
                                                  &items[i]));
        /* Fixed widths, since this suite has no font system. */
        schultz_node_set_pref_size(tree, items[i], 60.0f, 24.0f);
    }

    /* Wide enough for all five. */
    schultz_node_set_bounds(tree, bar, schultz_rect_make(0, 0, 400, 32));
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);
    ASSERT_EQ(0u, schultz_toolbar_overflow_count(tree, bar));
    ASSERT_EQ(1, schultz_node_is_visible(tree, items[4]));

    /*
     * Too narrow: the tail collapses. Setting bounds marks the node for
     * repaint but not for layout, since arrange sets bounds itself and would
     * otherwise mark what it had just finished laying out. So a host that
     * resizes a container asks for the relayout it wants.
     */
    schultz_node_set_bounds(tree, bar, schultz_rect_make(0, 0, 180, 32));
    schultz_node_invalidate_layout(tree, bar);
    schultz_layout_run(tree);
    ASSERT(schultz_toolbar_overflow_count(tree, bar) > 0u);
    ASSERT_EQ(0, schultz_node_is_visible(tree, items[4]));
    /* The first one always stays, however narrow it gets. */
    ASSERT_EQ(1, schultz_node_is_visible(tree, items[0]));

    /* Wide again: everything comes back. */
    schultz_node_set_bounds(tree, bar, schultz_rect_make(0, 0, 400, 32));
    schultz_node_invalidate_layout(tree, bar);
    schultz_layout_run(tree);
    ASSERT_EQ(0u, schultz_toolbar_overflow_count(tree, bar));
    ASSERT_EQ(1, schultz_node_is_visible(tree, items[4]));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_toolbar_that_may_not_overflow_keeps_everything_shown(void)
{
    schultz_tree *tree;
    schultz_handle bar = SCHULTZ_HANDLE_NONE;
    schultz_handle item = SCHULTZ_HANDLE_NONE;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 800, 600));
    ASSERT_EQ(SCHULTZ_OK, schultz_toolbar_create(tree, schultz_tree_root(tree),
        SCHULTZ_ORIENT_HORIZONTAL, &bar));
    schultz_toolbar_set_overflow_enabled(tree, bar, 0);
    for (i = 0; i < 5u; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_toolbar_add(tree, bar, "Item", &item));
        schultz_node_set_pref_size(tree, item, 60.0f, 24.0f);
    }
    schultz_node_set_bounds(tree, bar, schultz_rect_make(0, 0, 100, 32));
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);

    ASSERT_EQ(0u, schultz_toolbar_overflow_count(tree, bar));
    ASSERT_EQ(1, schultz_node_is_visible(tree, item));

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------------- status bar */

TEST a_flashed_message_reverts_when_its_time_is_up(void)
{
    schultz_tree *tree;
    schultz_handle bar = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_status_bar_create(tree,
        schultz_tree_root(tree), &bar));

    ASSERT_EQ(SCHULTZ_OK, schultz_status_bar_set_message(tree, bar, "Ready"));
    ASSERT_STR_EQ("Ready", schultz_status_bar_message(tree, bar));

    ASSERT_EQ(SCHULTZ_OK, schultz_status_bar_flash(tree, bar, "Saved", 500u));
    ASSERT_STR_EQ("Saved", schultz_status_bar_message(tree, bar));

    /* The first advance only sets the clock's starting point. */
    schultz_tree_advance(tree, 0u);
    schultz_tree_advance(tree, 200u);
    ASSERT_STR_EQ("Saved", schultz_status_bar_message(tree, bar));

    schultz_tree_advance(tree, 900u);
    ASSERT_STR_EQ("Ready", schultz_status_bar_message(tree, bar));

    schultz_tree_destroy(tree);
    PASS();
}

/* Setting the standing message during a flash does not cut the flash short. */
TEST setting_the_message_during_a_flash_changes_what_it_reverts_to(void)
{
    schultz_tree *tree;
    schultz_handle bar = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_status_bar_create(tree,
        schultz_tree_root(tree), &bar));
    schultz_status_bar_set_message(tree, bar, "Ready");
    schultz_status_bar_flash(tree, bar, "Saved", 500u);
    schultz_status_bar_set_message(tree, bar, "Idle");

    ASSERT_STR_EQ("Saved", schultz_status_bar_message(tree, bar));
    schultz_tree_advance(tree, 0u);
    schultz_tree_advance(tree, 900u);
    ASSERT_STR_EQ("Idle", schultz_status_bar_message(tree, bar));

    schultz_tree_destroy(tree);
    PASS();
}

/* ----------------------------------------------------------- list view */

/* A list of fixed height rows, laid out and ready to click. */
static schultz_handle list_setup(schultz_tree *tree, uint32_t rows)
{
    schultz_handle list = SCHULTZ_HANDLE_NONE;
    uint32_t i;

    if (schultz_list_view_create(tree, schultz_tree_root(tree), &list)
            != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    schultz_node_set_bounds(tree, list, schultz_rect_make(0, 0, 200, 100));
    for (i = 0; i < rows; i++) {
        schultz_handle row = SCHULTZ_HANDLE_NONE;

        if (schultz_list_view_add(tree, list, &row) != SCHULTZ_OK) {
            return SCHULTZ_HANDLE_NONE;
        }
        schultz_node_set_pref_size(tree, row, 180.0f, 20.0f);
    }
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);
    return list;
}

TEST a_list_view_holds_rows_and_hands_them_back(void)
{
    schultz_tree *tree;
    schultz_handle list;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    list = list_setup(tree, 5u);
    ASSERT(list != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(5u, schultz_list_view_count(tree, list));
    ASSERT(schultz_list_view_row(tree, list, 0) != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_list_view_row(tree, list, 9u));

    ASSERT_EQ(SCHULTZ_OK, schultz_list_view_remove(tree, list, 1u));
    ASSERT_EQ(4u, schultz_list_view_count(tree, list));

    ASSERT_EQ(SCHULTZ_OK, schultz_list_view_clear(tree, list));
    ASSERT_EQ(0u, schultz_list_view_count(tree, list));

    schultz_tree_destroy(tree);
    PASS();
}

TEST single_selection_replaces_what_was_chosen(void)
{
    schultz_tree *tree;
    schultz_handle list;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    list = list_setup(tree, 5u);
    ASSERT(list != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(-1, schultz_list_view_selected(tree, list));
    ASSERT_EQ(SCHULTZ_OK, schultz_list_view_select(tree, list, 2u));
    ASSERT_EQ(2, schultz_list_view_selected(tree, list));
    ASSERT_EQ(1u, schultz_list_view_selected_count(tree, list));

    schultz_list_view_select(tree, list, 4u);
    ASSERT_EQ(0, schultz_list_view_is_selected(tree, list, 2u));
    ASSERT_EQ(1, schultz_list_view_is_selected(tree, list, 4u));
    ASSERT_EQ(1u, schultz_list_view_selected_count(tree, list));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_range_selects_everything_between_its_ends(void)
{
    schultz_tree *tree;
    schultz_handle list;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    list = list_setup(tree, 8u);
    ASSERT(list != SCHULTZ_HANDLE_NONE);

    /* A range needs multiple mode. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_list_view_select_range(tree, list, 1u, 3u));

    schultz_list_view_set_selection_mode(tree, list,
                                         SCHULTZ_SELECT_MULTIPLE);
    ASSERT_EQ(SCHULTZ_OK, schultz_list_view_select_range(tree, list, 2u, 5u));
    ASSERT_EQ(4u, schultz_list_view_selected_count(tree, list));
    ASSERT_EQ(2, schultz_list_view_selected(tree, list));
    ASSERT_EQ(1, schultz_list_view_is_selected(tree, list, 5u));
    ASSERT_EQ(0, schultz_list_view_is_selected(tree, list, 6u));

    /* Backwards ends work the same way. */
    ASSERT_EQ(SCHULTZ_OK, schultz_list_view_select_range(tree, list, 6u, 4u));
    ASSERT_EQ(3u, schultz_list_view_selected_count(tree, list));
    ASSERT_EQ(4, schultz_list_view_selected_at(tree, list, 0u));
    ASSERT_EQ(6, schultz_list_view_selected_at(tree, list, 2u));
    ASSERT_EQ(-1, schultz_list_view_selected_at(tree, list, 3u));

    schultz_tree_destroy(tree);
    PASS();
}

/* Removing a selected row takes its selection with it. */
TEST removing_a_row_takes_its_selection_with_it(void)
{
    schultz_tree *tree;
    schultz_handle list;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    list = list_setup(tree, 4u);
    ASSERT(list != SCHULTZ_HANDLE_NONE);

    schultz_list_view_set_selection_mode(tree, list,
                                         SCHULTZ_SELECT_MULTIPLE);
    schultz_list_view_select_range(tree, list, 0u, 2u);
    ASSERT_EQ(3u, schultz_list_view_selected_count(tree, list));

    ASSERT_EQ(SCHULTZ_OK, schultz_list_view_remove(tree, list, 1u));
    ASSERT_EQ(3u, schultz_list_view_count(tree, list));
    ASSERT_EQ(2u, schultz_list_view_selected_count(tree, list));

    schultz_tree_destroy(tree);
    PASS();
}

TEST the_arrow_keys_walk_a_list(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle list;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));
    list = list_setup(tree, 6u);
    ASSERT(list != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    schultz_events_set_focus(events, list);
    schultz_list_view_select(tree, list, 0u);

    schultz_events_key(events, SCHULTZ_KEY_DOWN, 0u, 1);
    ASSERT_EQ(1, schultz_list_view_selected(tree, list));
    schultz_events_key(events, SCHULTZ_KEY_DOWN, 0u, 1);
    ASSERT_EQ(2, schultz_list_view_selected(tree, list));
    schultz_events_key(events, SCHULTZ_KEY_UP, 0u, 1);
    ASSERT_EQ(1, schultz_list_view_selected(tree, list));

    schultz_events_key(events, SCHULTZ_KEY_END, 0u, 1);
    ASSERT_EQ(5, schultz_list_view_selected(tree, list));
    /* And it stops at the end rather than running off it. */
    schultz_events_key(events, SCHULTZ_KEY_DOWN, 0u, 1);
    ASSERT_EQ(5, schultz_list_view_selected(tree, list));

    schultz_events_key(events, SCHULTZ_KEY_HOME, 0u, 1);
    ASSERT_EQ(0, schultz_list_view_selected(tree, list));
    schultz_events_key(events, SCHULTZ_KEY_UP, 0u, 1);
    ASSERT_EQ(0, schultz_list_view_selected(tree, list));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

TEST a_list_that_may_not_be_selected_ignores_the_keys(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle list;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    list = list_setup(tree, 4u);
    ASSERT(list != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    schultz_events_set_focus(events, list);

    schultz_list_view_select(tree, list, 1u);
    ASSERT_EQ(SCHULTZ_OK, schultz_list_view_set_selection_mode(tree, list,
        SCHULTZ_SELECT_NONE));
    /* Turning selection off clears what was chosen. */
    ASSERT_EQ(-1, schultz_list_view_selected(tree, list));

    schultz_events_key(events, SCHULTZ_KEY_DOWN, 0u, 1);
    ASSERT_EQ(-1, schultz_list_view_selected(tree, list));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

/* ----------------------------------------------------------- tree view */

TEST a_tree_view_nests_rows_and_reports_depth(void)
{
    schultz_tree *tree;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle root_row = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;
    schultz_handle grand = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_create(tree,
        schultz_tree_root(tree), &view));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view,
        SCHULTZ_HANDLE_NONE, &root_row));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view, root_row,
                                                &child));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view, child, &grand));

    ASSERT_EQ(0u, schultz_tree_view_depth(tree, root_row));
    ASSERT_EQ(1u, schultz_tree_view_depth(tree, child));
    ASSERT_EQ(2u, schultz_tree_view_depth(tree, grand));

    schultz_tree_destroy(tree);
    PASS();
}

TEST opening_a_row_shows_what_is_under_it(void)
{
    schultz_tree *tree;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle parent = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_create(tree,
        schultz_tree_root(tree), &view));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view,
        SCHULTZ_HANDLE_NONE, &parent));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view, parent, &child));

    /* Rows start closed, so what is under them is not drawn. */
    ASSERT_EQ(0, schultz_tree_view_is_expanded(tree, parent));
    ASSERT_EQ(0, schultz_node_is_visible(tree, child));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_expand(tree, view, parent, 1));
    ASSERT_EQ(1, schultz_tree_view_is_expanded(tree, parent));
    ASSERT_EQ(1, schultz_node_is_visible(tree, child));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_expand(tree, view, parent, 0));
    ASSERT_EQ(0, schultz_node_is_visible(tree, child));

    schultz_tree_destroy(tree);
    PASS();
}

/* Up and down walk what a reader can see, which is not the same as every row. */
TEST the_keys_walk_only_the_rows_that_are_showing(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle one = SCHULTZ_HANDLE_NONE;
    schultz_handle hidden = SCHULTZ_HANDLE_NONE;
    schultz_handle two = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_create(tree,
        schultz_tree_root(tree), &view));
    schultz_node_set_bounds(tree, view, schultz_rect_make(0, 0, 200, 200));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view,
        SCHULTZ_HANDLE_NONE, &one));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view, one, &hidden));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view,
        SCHULTZ_HANDLE_NONE, &two));
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    schultz_events_set_focus(events, view);
    schultz_tree_view_select(tree, view, one);
    ASSERT_EQ(one, schultz_tree_view_selected(tree, view));

    /* Closed, so down goes past the buried row to the next top level one. */
    schultz_events_key(events, SCHULTZ_KEY_DOWN, 0u, 1);
    ASSERT_EQ(two, schultz_tree_view_selected(tree, view));

    /* Opened, so now it stops on the buried row. */
    schultz_tree_view_select(tree, view, one);
    schultz_tree_view_expand(tree, view, one, 1);
    schultz_events_key(events, SCHULTZ_KEY_DOWN, 0u, 1);
    ASSERT_EQ(hidden, schultz_tree_view_selected(tree, view));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

/* Right opens then steps in; left closes then steps out. */
TEST right_and_left_each_do_two_jobs(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle parent = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_create(tree,
        schultz_tree_root(tree), &view));
    schultz_node_set_bounds(tree, view, schultz_rect_make(0, 0, 200, 200));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view,
        SCHULTZ_HANDLE_NONE, &parent));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view, parent, &child));
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    schultz_events_set_focus(events, view);
    schultz_tree_view_select(tree, view, parent);

    /* Closed: right opens it and leaves the choice where it was. */
    schultz_events_key(events, SCHULTZ_KEY_RIGHT, 0u, 1);
    ASSERT_EQ(1, schultz_tree_view_is_expanded(tree, parent));
    ASSERT_EQ(parent, schultz_tree_view_selected(tree, view));

    /* Open: right steps into it. */
    schultz_events_key(events, SCHULTZ_KEY_RIGHT, 0u, 1);
    ASSERT_EQ(child, schultz_tree_view_selected(tree, view));

    /* On a leaf: left steps back out to the parent. */
    schultz_events_key(events, SCHULTZ_KEY_LEFT, 0u, 1);
    ASSERT_EQ(parent, schultz_tree_view_selected(tree, view));

    /* On an open row: left closes it. */
    schultz_events_key(events, SCHULTZ_KEY_LEFT, 0u, 1);
    ASSERT_EQ(0, schultz_tree_view_is_expanded(tree, parent));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

/* The rest of the list and tree API, which a host reaches for less often. */
TEST the_quieter_list_and_tree_calls_do_what_they_say(void)
{
    schultz_tree *tree;
    schultz_handle list;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle one = SCHULTZ_HANDLE_NONE;
    schultz_handle two = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));

    list = list_setup(tree, 20u);
    ASSERT(list != SCHULTZ_HANDLE_NONE);
    schultz_list_view_set_selection_mode(tree, list, SCHULTZ_SELECT_MULTIPLE);
    schultz_list_view_select_range(tree, list, 0u, 3u);
    ASSERT_EQ(4u, schultz_list_view_selected_count(tree, list));

    /* Deselect takes one row out and leaves the rest alone. */
    ASSERT_EQ(SCHULTZ_OK, schultz_list_view_deselect(tree, list, 1u));
    ASSERT_EQ(3u, schultz_list_view_selected_count(tree, list));
    ASSERT_EQ(0, schultz_list_view_is_selected(tree, list, 1u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_list_view_deselect(tree, list, 99u));

    /* Scrolling to a row brings it into view without choosing it. */
    ASSERT_EQ(SCHULTZ_OK, schultz_list_view_scroll_to(tree, list, 18u));
    ASSERT_EQ(0, schultz_list_view_is_selected(tree, list, 18u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_list_view_scroll_to(tree, list, 99u));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_create(tree,
        schultz_tree_root(tree), &view));
    schultz_node_set_bounds(tree, view, schultz_rect_make(0, 0, 200, 100));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view,
        SCHULTZ_HANDLE_NONE, &one));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view,
        SCHULTZ_HANDLE_NONE, &two));
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_set_indent(tree, view, 24.0f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_view_set_indent(tree, view, -1.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_set_selection_mode(tree, view,
        SCHULTZ_SELECT_NONE));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_view_set_selection_mode(tree, view, 99u));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_scroll_to(tree, view, two));

    /* Removing the selected row leaves nothing selected. */
    schultz_tree_view_set_selection_mode(tree, view, SCHULTZ_SELECT_SINGLE);
    schultz_tree_view_select(tree, view, two);
    ASSERT_EQ(two, schultz_tree_view_selected(tree, view));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_remove(tree, view, two));
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_tree_view_selected(tree, view));

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * The disclosure column opens and closes; the rest of the row selects. The
 * press lands on whatever the host put in the row, so the two are told apart
 * by where the row is, not by where the pressed node is.
 */
TEST clicking_a_trees_text_selects_rather_than_toggling(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_rect strip;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 300));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 300));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_create(tree,
        schultz_tree_root(tree), &view));
    schultz_node_set_bounds(tree, view, schultz_rect_make(0, 0, 240, 200));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view,
        SCHULTZ_HANDLE_NONE, &row));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view, row, &child));
    /* Something wide enough to press well past the arrow. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, row, &label));
    schultz_node_set_pref_size(tree, label, 160.0f, 20.0f);
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);

    {
        schultz_handle header = SCHULTZ_HANDLE_NONE;

        ASSERT_EQ(SCHULTZ_OK, schultz_node_parent(tree, row, &header));
        ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(tree, header,
                                                           &strip));
    }

    /*
     * Just past the arrow is text, not the arrow: it selects and stays
     * closed. Twenty pixels in on purpose, because that is inside the band
     * the old hit test got wrong: it measured from the node the press landed
     * on rather than from the row, so the first stretch of the text counted
     * as the arrow.
     */
    {
        schultz_point at = schultz_point_make(strip.x + 20.0f,
                                              strip.y + 8.0f);

        schultz_events_mouse_move(events, at, 0);
        schultz_events_mouse_button(events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
        schultz_events_mouse_button(events, at, SCHULTZ_BUTTON_LEFT, 0, 0);
    }
    ASSERT_EQ(row, schultz_tree_view_selected(tree, view));
    ASSERT_EQ(0, schultz_tree_view_is_expanded(tree, row));

    /* The arrow itself still opens it. */
    {
        schultz_point at = schultz_point_make(strip.x + 6.0f,
                                              strip.y + 8.0f);

        schultz_events_mouse_move(events, at, 0);
        schultz_events_mouse_button(events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
        schultz_events_mouse_button(events, at, SCHULTZ_BUTTON_LEFT, 0, 0);
    }
    ASSERT_EQ(1, schultz_tree_view_is_expanded(tree, row));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

/* A setter that stores a value nothing reads is a setter that does nothing. */
TEST setting_a_trees_indent_moves_its_rows(void)
{
    schultz_tree *tree;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle parent = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;
    schultz_rect narrow;
    schultz_rect wide;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 300));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 300));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_create(tree,
        schultz_tree_root(tree), &view));
    schultz_node_set_bounds(tree, view, schultz_rect_make(0, 0, 240, 200));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view,
        SCHULTZ_HANDLE_NONE, &parent));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_add(tree, view, parent, &child));
    schultz_tree_view_expand(tree, view, parent, 1);
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(tree, child, &narrow));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_set_indent(tree, view, 48.0f));
    schultz_layout_run(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(tree, child, &wide));

    /* One level deep, so a bigger indent moves it further in by exactly the
     * difference. */
    ASSERT(wide.x > narrow.x);
    ASSERT_EQ(32.0f, wide.x - narrow.x);

    /* The top level row is at depth zero and does not move. */
    schultz_node_absolute_bounds(tree, parent, &wide);
    schultz_tree_view_set_indent(tree, view, 4.0f);
    schultz_layout_run(tree);
    schultz_node_absolute_bounds(tree, parent, &narrow);
    ASSERT_EQ(wide.x, narrow.x);

    schultz_tree_destroy(tree);
    PASS();
}


TEST a_focused_tab_is_marked_by_its_line_and_never_by_a_box(void)
{
    box_fixture f;
    schultz_handle tabs;
    schultz_handle strip = SCHULTZ_HANDLE_NONE;
    schultz_handle first = SCHULTZ_HANDLE_NONE;
    schultz_handle second = SCHULTZ_HANDLE_NONE;
    schultz_handle page = SCHULTZ_HANDLE_NONE;
    uint32_t lines_when_unfocused;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_create(f.tree,
                    schultz_tree_root(f.tree), &tabs));
    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_add(f.tree, tabs, "One", &page));
    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_add(f.tree, tabs, "Two", &page));
    schultz_node_set_bounds(f.tree, tabs, schultz_rect_make(0, 0, 300, 120));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, tabs, 0u, &strip));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, strip, 0u, &first));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, strip, 1u, &second));

    paint_all(&f);
    lines_when_unfocused = count_kind(&f, SCHULTZ_DRAW_FILL_RECT);
    ASSERT_EQ(0u, count_kind(&f, SCHULTZ_DRAW_STROKE_ROUND_RECT));

    /*
     * A focus ring is a box drawn around a control, and a tab has no box. It
     * would put an outline back on the one tab that is supposed to be marked
     * by the line underneath it instead.
     */
    schultz_events_set_focus(f.events, first);
    paint_all(&f);
    ASSERT_EQ(0u, count_kind(&f, SCHULTZ_DRAW_STROKE_ROUND_RECT));
    ASSERT_EQ(lines_when_unfocused, count_kind(&f, SCHULTZ_DRAW_FILL_RECT));

    /*
     * Tabs are selected by being activated rather than by being reached, so
     * the keyboard can sit on a tab that is not the one showing. That gets a
     * line of its own, or there is nothing at all to say where it is.
     */
    schultz_events_set_focus(f.events, second);
    paint_all(&f);
    ASSERT_EQ(0u, count_kind(&f, SCHULTZ_DRAW_STROKE_ROUND_RECT));
    ASSERT_EQ(lines_when_unfocused + 1u,
              count_kind(&f, SCHULTZ_DRAW_FILL_RECT));

    fixture_teardown(&f);
    PASS();
}


TEST a_canvas_records_pictures_clips_and_offsets(void)
{
    box_fixture f;
    schultz_handle canvas = SCHULTZ_HANDLE_NONE;
    /* Any live handle will do: a canvas writes down what to draw, and what
     * an image is, is the renderer's business. */
    schultz_handle image = (schultz_handle)0x100000001u;
    schultz_point tri[3];
    schultz_stroke pen;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_create(f.tree,
                    schultz_tree_root(f.tree), &canvas));
    pen = schultz_stroke_solid(schultz_color_rgba(1, 2, 3, 255), 1.0f);
    tri[0] = schultz_point_make(0, 0);
    tri[1] = schultz_point_make(10, 0);
    tri[2] = schultz_point_make(5, 10);

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_stroke_ellipse(f.tree, canvas,
                    schultz_rect_make(0, 0, 8, 4), pen));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_image(f.tree, canvas, image,
                    schultz_rect_make(0, 0, 16, 16),
                    schultz_rect_make(0, 0, 32, 32), 255u));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_clip_begin(f.tree, canvas,
                    schultz_rect_make(0, 0, 10, 10)));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_offset_begin(f.tree, canvas,
                                                     4.0f, 4.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_offset_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_clip_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_stroke_polygon(f.tree, canvas, tri, 3u,
                                                        pen, 1));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_stroke_polygon(f.tree, canvas, tri, 3u,
                                                        pen, 0));
    /* An outline needs a direction, so one point is not a path. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_canvas_stroke_polygon(f.tree, canvas, tri, 1u, pen, 1));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));

    ASSERT_EQ(8u, schultz_canvas_count(f.tree, canvas));

    /* None of them records while the canvas is closed. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_stroke_ellipse(f.tree, canvas,
                    schultz_rect_make(0, 0, 8, 4), pen));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_image(f.tree, canvas, image,
                    schultz_rect_make(0, 0, 1, 1),
                    schultz_rect_make(0, 0, 1, 1), 255u));
    /* And no picture at all is refused even while it is recording. */
    schultz_canvas_begin(f.tree, canvas);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_canvas_image(f.tree, canvas, SCHULTZ_HANDLE_NONE,
                    schultz_rect_make(0, 0, 1, 1),
                    schultz_rect_make(0, 0, 1, 1), 255u));
    schultz_canvas_end(f.tree, canvas);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_clip_begin(f.tree, canvas,
                    schultz_rect_make(0, 0, 1, 1)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_clip_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_offset_begin(f.tree, canvas, 1.0f, 1.0f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_offset_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_stroke_polygon(f.tree, canvas, tri, 3u, pen, 1));

    /* Measuring needs no canvas at all, and an exact answer beats a guess
     * from a character count. */
    {
        schultz_size wide;
        schultz_size narrow;

        ASSERT_EQ(SCHULTZ_OK, schultz_canvas_measure_text(f.tree, f.font,
                        "wwwwwwww", &wide));
        ASSERT_EQ(SCHULTZ_OK, schultz_canvas_measure_text(f.tree, f.font,
                        "iiiiiiii", &narrow));
        ASSERT(wide.width > narrow.width);
        ASSERT(wide.height > 0.0f);
        ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
                  schultz_canvas_measure_text(f.tree, f.font, "x", NULL));
    }

    fixture_teardown(&f);
    PASS();
}

SUITE(containers)
{
    RUN_TEST(a_list_view_holds_rows_and_hands_them_back);
    RUN_TEST(the_quieter_list_and_tree_calls_do_what_they_say);
    RUN_TEST(single_selection_replaces_what_was_chosen);
    RUN_TEST(a_range_selects_everything_between_its_ends);
    RUN_TEST(removing_a_row_takes_its_selection_with_it);
    RUN_TEST(the_arrow_keys_walk_a_list);
    RUN_TEST(a_list_that_may_not_be_selected_ignores_the_keys);
    RUN_TEST(a_tree_view_nests_rows_and_reports_depth);
    RUN_TEST(setting_a_trees_indent_moves_its_rows);
    RUN_TEST(clicking_a_trees_text_selects_rather_than_toggling);
    RUN_TEST(opening_a_row_shows_what_is_under_it);
    RUN_TEST(the_keys_walk_only_the_rows_that_are_showing);
    RUN_TEST(right_and_left_each_do_two_jobs);
    RUN_TEST(an_accordion_opens_and_closes_a_section);
    RUN_TEST(single_expand_closes_whichever_section_was_open);
    RUN_TEST(a_toolbar_collapses_what_does_not_fit);
    RUN_TEST(a_toolbar_that_may_not_overflow_keeps_everything_shown);
    RUN_TEST(a_flashed_message_reverts_when_its_time_is_up);
    RUN_TEST(setting_the_message_during_a_flash_changes_what_it_reverts_to);
    RUN_TEST(a_row_scrolled_out_of_sight_is_not_clickable);
    RUN_TEST(revealing_a_node_below_the_viewport_scrolls_down);
    RUN_TEST(revealing_a_node_above_the_viewport_scrolls_back);
    RUN_TEST(revealing_a_node_already_in_view_does_nothing);
    RUN_TEST(revealing_reaches_a_node_nested_inside_a_row);
    RUN_TEST(revealing_a_node_taller_than_the_view_shows_its_start);
    RUN_TEST(a_canvas_keeps_what_was_recorded_into_it);
    RUN_TEST(a_canvas_records_pictures_clips_and_offsets);
    RUN_TEST(a_canvas_replays_its_drawing_where_it_was_laid_out);
    RUN_TEST(a_canvas_refuses_drawing_outside_a_recording);
    RUN_TEST(canvas_text_needs_a_font_system);
    RUN_TEST(a_pixel_exact_canvas_brackets_its_drawing);
    RUN_TEST(a_canvas_reports_its_size_in_screen_pixels);
    RUN_TEST(a_group_box_leaves_a_gap_for_its_title);
    RUN_TEST(a_group_box_sizes_itself_around_its_contents);
    RUN_TEST(a_split_pane_divides_its_room_between_two_halves);
    RUN_TEST(dragging_a_split_divider_moves_it);
    RUN_TEST(pressing_a_split_half_does_not_move_the_divider);
    RUN_TEST(every_content_node_measures_what_is_inside_it);
    RUN_TEST(a_finger_drags_the_content_with_it);
    RUN_TEST(a_flick_carries_on_after_the_finger_lifts);
    RUN_TEST(a_list_with_nothing_to_scroll_lets_the_page_scroll);
    RUN_TEST(a_list_that_can_scroll_keeps_the_drag_to_itself);
    RUN_TEST(a_scroll_that_starts_on_a_row_does_not_press_it);
    RUN_TEST(a_cursor_does_not_drag_the_content);
    RUN_TEST(every_content_node_arranges_what_is_put_in_it);
    RUN_TEST(a_tab_page_shows_what_is_put_in_it);
    RUN_TEST(split_pane_accessors_reject_other_widgets);
    RUN_TEST(a_tab_view_shows_one_page_at_a_time);
    RUN_TEST(a_focused_tab_is_marked_by_its_line_and_never_by_a_box);
    RUN_TEST(clicking_a_tab_shows_its_page);
    RUN_TEST(tab_view_accessors_reject_other_widgets);
    RUN_TEST(a_tab_view_holds_only_so_many);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(containers);
    GREATEST_MAIN_END();
}
