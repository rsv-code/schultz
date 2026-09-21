/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_text_widgets.c - scrolling and text entry: ScrollBar, ScrollView,
 * TextField, TextArea, and the selectable text block a Label becomes when it
 * is asked to be one.
 *
 * Editing is asserted on the buffer and the selection, appearance on the draw
 * commands, and routing on what a host would have been told. A fake clipboard
 * stands in for the platform's, so cut, copy and paste are tested without one.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_widgets.h"

#define FONT_PATH "assets/fonts/DejaVuSans.ttf"

/* ------------------------------------------------------------- fixture */

/** A clipboard that lives in this process, so the tests need no platform. */
typedef struct {
    char text[256];
    int32_t written;
} fake_clipboard;

static const void *fake_clipboard_take(void *context, const char *format,
                                       uint64_t *out_length)
{
    fake_clipboard *board = (fake_clipboard *)context;

    *out_length = 0u;
    if (strcmp(format, SCHULTZ_CLIPBOARD_TEXT) != 0 ||
        board->text[0] == '\0') {
        return NULL;
    }
    *out_length = (uint64_t)strlen(board->text);
    return board->text;
}

static int32_t fake_clipboard_holds(void *context, const char *format)
{
    fake_clipboard *board = (fake_clipboard *)context;

    return (strcmp(format, SCHULTZ_CLIPBOARD_TEXT) == 0 &&
            board->text[0] != '\0') ? 1 : 0;
}

/*
 * Text is produced here and now rather than when something pastes, which is
 * what the real one does for a text only offer too. These tests are about
 * the widgets, so the stored string is what they look at.
 */
static int32_t fake_clipboard_offer(void *context,
                                    const char *const *formats,
                                    uint32_t count,
                                    schultz_clipboard_make_fn make,
                                    void *make_context)
{
    fake_clipboard *board = (fake_clipboard *)context;
    uint64_t length = 0u;
    const void *bytes;
    size_t n;

    if (count == 0u || strcmp(formats[0], SCHULTZ_CLIPBOARD_TEXT) != 0) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    bytes = make(make_context, SCHULTZ_CLIPBOARD_TEXT, &length);
    if (bytes == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    n = (size_t)length;
    if (n > sizeof(board->text) - 1u) {
        n = sizeof(board->text) - 1u;
    }
    memcpy(board->text, bytes, n);
    board->text[n] = '\0';
    board->written++;
    return SCHULTZ_OK;
}

/* Records the value a scroll bar reported, for the callback tests. */
static void record_change(void *context, schultz_tree *tree,
                          schultz_handle bar, float value)
{
    (void)tree;
    (void)bar;
    *(float *)context = value;
}

typedef struct {
    schultz_tree        *tree;
    schultz_events      *events;
    schultz_arena        arena;
    schultz_draw_list    list;
    schultz_font_system *fonts;
    schultz_glyph_cache *glyphs;
    schultz_handle       font;
    schultz_theme        theme;
    fake_clipboard       board;
} text_fixture;

static int32_t fixture_setup(text_fixture *f)
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
    schultz_tree_set_clipboard(f->tree, fake_clipboard_offer,
                               fake_clipboard_take, fake_clipboard_holds,
                               &f->board);

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

static void fixture_teardown(text_fixture *f)
{
    schultz_arena_free(&f->arena);
    schultz_events_destroy(f->events);
    schultz_tree_destroy(f->tree);
    schultz_glyph_cache_destroy(f->glyphs);
    schultz_font_system_destroy(f->fonts);
}

static uint32_t paint_all(text_fixture *f)
{
    schultz_arena_reset(&f->arena);
    schultz_draw_list_init(&f->list, &f->arena, 0);
    schultz_tree_resolve_styles(f->tree);
    schultz_widget_paint_tree(f->tree, &f->list, &f->arena,
                              schultz_rect_make(0, 0, 0, 0));
    return schultz_draw_list_count(&f->list);
}

static uint32_t count_kind(text_fixture *f, uint32_t kind)
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

static void place(text_fixture *f, schultz_handle node, float x, float y,
                  float w, float h)
{
    schultz_tree_resolve_styles(f->tree);
    schultz_layout_arrange(f->tree, node, schultz_rect_make(x, y, w, h));
    schultz_node_set_bounds(f->tree, node, schultz_rect_make(x, y, w, h));
    schultz_layout_arrange(f->tree, node, schultz_rect_make(x, y, w, h));
}

/* Presses and releases at a point, at a given moment in milliseconds. */
static void click_at(text_fixture *f, schultz_point at, uint64_t when)
{
    schultz_events_set_time(f->events, when);
    schultz_events_mouse_move(f->events, at, 0);
    schultz_events_mouse_button(f->events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_button(f->events, at, SCHULTZ_BUTTON_LEFT, 0, 0);
}

/* Sends a key to the focused node. */
static void key(text_fixture *f, uint32_t code, uint32_t modifiers)
{
    schultz_events_key(f->events, code, modifiers, 1);
    schultz_events_key(f->events, code, modifiers, 0);
}

/* ------------------------------------------------------------ ScrollBar */

TEST a_scroll_bar_clamps_to_what_there_is_to_scroll(void)
{
    text_fixture f;
    schultz_handle bar;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_bar_create(f.tree,
                    schultz_tree_root(f.tree), SCHULTZ_ORIENT_VERTICAL,
                    &bar));

    /* Nothing to scroll until the content is larger than the viewport. */
    ASSERT_EQ(0.0f, schultz_scroll_bar_maximum(f.tree, bar));
    schultz_scroll_bar_set_value(f.tree, bar, 50.0f);
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, bar));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_scroll_bar_set_range(f.tree, bar, 500.0f, 100.0f));
    ASSERT_EQ(400.0f, schultz_scroll_bar_maximum(f.tree, bar));
    schultz_scroll_bar_set_value(f.tree, bar, 5000.0f);
    ASSERT_EQ(400.0f, schultz_scroll_bar_value(f.tree, bar));
    schultz_scroll_bar_set_value(f.tree, bar, -5.0f);
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, bar));

    /* A shorter document pulls the value back with it. */
    schultz_scroll_bar_set_value(f.tree, bar, 400.0f);
    schultz_scroll_bar_set_range(f.tree, bar, 150.0f, 100.0f);
    ASSERT_EQ(50.0f, schultz_scroll_bar_value(f.tree, bar));

    fixture_teardown(&f);
    PASS();
}

TEST a_scroll_bar_rejects_bad_arguments(void)
{
    text_fixture f;
    schultz_handle bar;
    schultz_handle panel;
    schultz_handle none = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_scroll_bar_create(f.tree, schultz_tree_root(f.tree),
                              SCHULTZ_ORIENT_HORIZONTAL, &bar);
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_scroll_bar_create(f.tree, schultz_tree_root(f.tree), 7,
                                        &none));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_scroll_bar_create(f.tree, schultz_tree_root(f.tree), 0,
                                        NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_scroll_bar_set_range(f.tree, bar, -1.0f, 1.0f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_scroll_bar_set_range(f.tree, panel, 1.0f, 1.0f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_scroll_bar_set_value(f.tree, panel, 1.0f));
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, panel));
    ASSERT_EQ(0.0f, schultz_scroll_bar_maximum(f.tree, panel));

    fixture_teardown(&f);
    PASS();
}

TEST a_scroll_bar_thumb_shrinks_as_the_content_grows(void)
{
    text_fixture f;
    schultz_handle bar;
    float small_thumb = 0.0f;
    float large_thumb = 0.0f;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_scroll_bar_create(f.tree, schultz_tree_root(f.tree),
                              SCHULTZ_ORIENT_VERTICAL, &bar);
    schultz_node_set_bounds(f.tree, bar, schultz_rect_make(0, 0, 12, 200));

    schultz_scroll_bar_set_range(f.tree, bar, 400.0f, 200.0f);
    paint_all(&f);
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_FILL_ROUND_RECT) {
            small_thumb = cmd->as.fill_round_rect.rect.height;
        }
    }

    schultz_scroll_bar_set_range(f.tree, bar, 4000.0f, 200.0f);
    paint_all(&f);
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_FILL_ROUND_RECT) {
            large_thumb = cmd->as.fill_round_rect.rect.height;
        }
    }

    ASSERT(large_thumb < small_thumb);
    /* Still grabbable, however long the document is. */
    ASSERT(large_thumb >= 20.0f);

    fixture_teardown(&f);
    PASS();
}

TEST dragging_a_scroll_bar_keeps_the_grab_point(void)
{
    text_fixture f;
    schultz_handle bar;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_scroll_bar_create(f.tree, schultz_tree_root(f.tree),
                              SCHULTZ_ORIENT_VERTICAL, &bar);
    schultz_node_set_bounds(f.tree, bar, schultz_rect_make(0, 0, 12, 200));
    schultz_scroll_bar_set_range(f.tree, bar, 400.0f, 200.0f);

    /*
     * Grab the top of the thumb and drag half the free room down. The thumb
     * covers half the track, so half its free room is half the way through.
     */
    schultz_events_mouse_move(f.events, schultz_point_make(6, 2), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(6, 2),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, bar));

    schultz_events_mouse_move(f.events, schultz_point_make(6, 52), 0);
    ASSERT_EQ(100.0f, schultz_scroll_bar_value(f.tree, bar));

    schultz_events_mouse_button(f.events, schultz_point_make(6, 52),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    fixture_teardown(&f);
    PASS();
}

TEST pressing_a_scroll_bar_track_pages(void)
{
    text_fixture f;
    schultz_handle bar;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_scroll_bar_create(f.tree, schultz_tree_root(f.tree),
                              SCHULTZ_ORIENT_VERTICAL, &bar);
    schultz_node_set_bounds(f.tree, bar, schultz_rect_make(0, 0, 12, 200));
    schultz_scroll_bar_set_range(f.tree, bar, 1000.0f, 200.0f);

    /* Below the thumb: one viewport forward. */
    schultz_events_mouse_move(f.events, schultz_point_make(6, 190), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(6, 190),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    ASSERT_EQ(200.0f, schultz_scroll_bar_value(f.tree, bar));
    schultz_events_mouse_button(f.events, schultz_point_make(6, 190),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    fixture_teardown(&f);
    PASS();
}

TEST scroll_bar_keys_move_it(void)
{
    text_fixture f;
    schultz_handle bar;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_scroll_bar_create(f.tree, schultz_tree_root(f.tree),
                              SCHULTZ_ORIENT_VERTICAL, &bar);
    schultz_node_set_bounds(f.tree, bar, schultz_rect_make(0, 0, 12, 200));
    schultz_scroll_bar_set_range(f.tree, bar, 1000.0f, 200.0f);
    schultz_events_set_focus(f.events, bar);

    key(&f, SCHULTZ_KEY_END, 0);
    ASSERT_EQ(800.0f, schultz_scroll_bar_value(f.tree, bar));
    key(&f, SCHULTZ_KEY_HOME, 0);
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, bar));
    key(&f, SCHULTZ_KEY_PAGE_DOWN, 0);
    ASSERT_EQ(200.0f, schultz_scroll_bar_value(f.tree, bar));
    key(&f, SCHULTZ_KEY_UP, 0);
    ASSERT_EQ(180.0f, schultz_scroll_bar_value(f.tree, bar));

    fixture_teardown(&f);
    PASS();
}

/* ----------------------------------------------------------- ScrollView */

/* Gives a scroll view a content larger than the space it is given. */
static schultz_handle tall_content(text_fixture *f, schultz_handle view,
                                   float width, float height)
{
    schultz_handle content = schultz_scroll_view_content(f->tree, view);

    schultz_node_set_style_property(f->tree, content,
                                    SCHULTZ_PROP_PREF_WIDTH,
                                    schultz_value_number(width));
    schultz_node_set_style_property(f->tree, content,
                                    SCHULTZ_PROP_PREF_HEIGHT,
                                    schultz_value_number(height));
    return content;
}

TEST a_scroll_view_shows_a_bar_only_when_it_is_needed(void)
{
    text_fixture f;
    schultz_handle view;
    schultz_handle bar_x;
    schultz_handle bar_y;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_create(f.tree,
                    schultz_tree_root(f.tree), &view));
    bar_x = schultz_scroll_view_bar(f.tree, view, SCHULTZ_ORIENT_HORIZONTAL);
    bar_y = schultz_scroll_view_bar(f.tree, view, SCHULTZ_ORIENT_VERTICAL);
    ASSERT(bar_x != SCHULTZ_HANDLE_NONE && bar_y != SCHULTZ_HANDLE_NONE);

    /* Content that fits: no bars. */
    tall_content(&f, view, 50.0f, 50.0f);
    place(&f, view, 0, 0, 200, 200);
    ASSERT_FALSE(schultz_node_is_visible(f.tree, bar_x));
    ASSERT_FALSE(schultz_node_is_visible(f.tree, bar_y));

    /* Taller than the view: only the vertical one. */
    tall_content(&f, view, 50.0f, 900.0f);
    place(&f, view, 0, 0, 200, 200);
    ASSERT_FALSE(schultz_node_is_visible(f.tree, bar_x));
    ASSERT(schultz_node_is_visible(f.tree, bar_y));

    tall_content(&f, view, 900.0f, 900.0f);
    place(&f, view, 0, 0, 200, 200);
    ASSERT(schultz_node_is_visible(f.tree, bar_x));
    ASSERT(schultz_node_is_visible(f.tree, bar_y));

    fixture_teardown(&f);
    PASS();
}

TEST a_finger_dragging_from_a_field_scrolls_the_page(void)
{
    text_fixture f;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle content = SCHULTZ_HANDLE_NONE;
    schultz_handle field = SCHULTZ_HANDLE_NONE;
    schultz_handle bar_y = SCHULTZ_HANDLE_NONE;
    schultz_rect at;
    schultz_point start;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_create(f.tree,
                    schultz_tree_root(f.tree), &view));
    bar_y = schultz_scroll_view_bar(f.tree, view, SCHULTZ_ORIENT_VERTICAL);
    content = schultz_scroll_view_content(f.tree, view);
    schultz_node_set_pane(f.tree, content, schultz_pane_vbox());
    ASSERT_EQ(SCHULTZ_OK, schultz_text_field_create(f.tree, content,
                                                    "some words", &field));
    schultz_node_set_pref_size(f.tree, field, 180.0f, 30.0f);
    {   /* Something below it, so there is somewhere to scroll to. */
        schultz_handle filler = SCHULTZ_HANDLE_NONE;

        ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree, content, &filler));
        schultz_node_set_pref_size(f.tree, filler, 180.0f, 800.0f);
    }
    place(&f, view, 0, 0, 200, 200);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, field, &at));
    start = schultz_point_make(at.x + at.width / 2.0f,
                               at.y + at.height / 2.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_pointer_source(f.events,
                                                    SCHULTZ_POINTER_TOUCH));
    /*
     * A finger that starts inside the field and moves is scrolling the page,
     * which is what a browser does and what Android does. The field must
     * neither take focus nor swallow the press, or there is no way to scroll
     * a form that has a field near the top of it.
     */
    schultz_events_mouse_button(f.events, start, SCHULTZ_BUTTON_LEFT, 1, 0u);
    schultz_events_mouse_move(f.events,
        schultz_point_make(start.x, start.y - 40.0f), 0u);
    schultz_events_mouse_move(f.events,
        schultz_point_make(start.x, start.y - 80.0f), 0u);
    schultz_events_mouse_button(f.events,
        schultz_point_make(start.x, start.y - 80.0f), SCHULTZ_BUTTON_LEFT, 0,
        0u);
    ASSERT(schultz_scroll_bar_value(f.tree, bar_y) > 0.0f);
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_events_focus(f.events));

    /* A tap in the same place is a person asking to type, and it focuses. */
    schultz_scroll_view_scroll_to(f.tree, view, schultz_point_make(0, 0));
    place(&f, view, 0, 0, 200, 200);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, field, &at));
    start = schultz_point_make(at.x + at.width / 2.0f,
                               at.y + at.height / 2.0f);
    schultz_events_mouse_button(f.events, start, SCHULTZ_BUTTON_LEFT, 1, 0u);
    schultz_events_mouse_button(f.events, start, SCHULTZ_BUTTON_LEFT, 0, 0u);
    ASSERT_EQ(field, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

TEST a_wheel_over_a_sideways_view_turns_it_sideways(void)
{
    text_fixture f;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle bar_x = SCHULTZ_HANDLE_NONE;
    schultz_handle bar_y = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_create(f.tree,
                    schultz_tree_root(f.tree), &view));
    bar_x = schultz_scroll_view_bar(f.tree, view, SCHULTZ_ORIENT_HORIZONTAL);
    bar_y = schultz_scroll_view_bar(f.tree, view, SCHULTZ_ORIENT_VERTICAL);

    /* Wide and short: there is nowhere to go downwards. */
    tall_content(&f, view, 900.0f, 50.0f);
    place(&f, view, 0, 0, 200, 200);
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, bar_x));

    /*
     * The wheel turns the only way the view can go. A wheel that did
     * nothing at all reads as a view that cannot scroll, and a row of emoji
     * on a desktop is exactly that case.
     */
    schultz_events_scroll(f.events, schultz_point_make(100, 100), 0.0f, 1.0f);
    ASSERT(schultz_scroll_bar_value(f.tree, bar_x) > 0.0f);
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, bar_y));

    /* Tall as well, and the wheel goes back to meaning down. */
    tall_content(&f, view, 900.0f, 900.0f);
    place(&f, view, 0, 0, 200, 200);
    schultz_scroll_view_scroll_to(f.tree, view, schultz_point_make(0, 0));
    schultz_events_scroll(f.events, schultz_point_make(100, 100), 0.0f, 1.0f);
    ASSERT(schultz_scroll_bar_value(f.tree, bar_y) > 0.0f);
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, bar_x));
    fixture_teardown(&f);
    PASS();
}

TEST a_view_told_to_hide_its_bars_keeps_them_away(void)
{
    text_fixture f;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle bar_x = SCHULTZ_HANDLE_NONE;
    schultz_handle viewport = SCHULTZ_HANDLE_NONE;
    schultz_rect inner;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_create(f.tree,
                    schultz_tree_root(f.tree), &view));
    bar_x = schultz_scroll_view_bar(f.tree, view, SCHULTZ_ORIENT_HORIZONTAL);
    ASSERT(schultz_scroll_view_shows_bars(f.tree, view)); /* on, to start */

    /* Wider than the view, so a bar would ordinarily appear. */
    tall_content(&f, view, 900.0f, 50.0f);
    place(&f, view, 0, 0, 200, 200);
    ASSERT(schultz_node_is_visible(f.tree, bar_x));

    /*
     * Hidden, and the height it was taking goes back to the content: on a
     * touch panel the bar is a strip nobody aims at, and three rows of
     * emoji have nothing to spare.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_set_bars(f.tree, view, 0));
    ASSERT_FALSE(schultz_scroll_view_shows_bars(f.tree, view));
    place(&f, view, 0, 0, 200, 200);
    ASSERT_FALSE(schultz_node_is_visible(f.tree, bar_x));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_parent(f.tree,
        schultz_scroll_view_content(f.tree, view), &viewport));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(f.tree, viewport, &inner));
    ASSERT_EQ(200.0f, inner.height);

    /* And back again, for a window with a pointer in it. */
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_set_bars(f.tree, view, 1));
    ASSERT(schultz_scroll_view_shows_bars(f.tree, view));
    place(&f, view, 0, 0, 200, 200);
    ASSERT(schultz_node_is_visible(f.tree, bar_x));
    fixture_teardown(&f);
    PASS();
}

TEST scrolling_moves_the_content_and_not_its_bounds(void)
{
    text_fixture f;
    schultz_handle view;
    schultz_handle content;
    schultz_rect before;
    schultz_rect after;
    schultz_rect own;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_scroll_view_create(f.tree, schultz_tree_root(f.tree), &view);
    content = tall_content(&f, view, 50.0f, 900.0f);
    place(&f, view, 10, 20, 200, 200);

    schultz_node_absolute_bounds(f.tree, content, &before);
    schultz_node_get_bounds(f.tree, content, &own);

    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_scroll_to(f.tree, view,
                                   schultz_point_make(0.0f, 120.0f)));

    schultz_node_absolute_bounds(f.tree, content, &after);
    ASSERT_EQ(before.y - 120.0f, after.y);

    /* Its own bounds never moved, so no layout was needed. */
    {
        schultz_rect still;
        schultz_node_get_bounds(f.tree, content, &still);
        ASSERT_EQ(own.y, still.y);
    }

    fixture_teardown(&f);
    PASS();
}

TEST hit_testing_follows_the_scrolled_content(void)
{
    text_fixture f;
    schultz_handle view;
    schultz_handle content;
    schultz_handle button;
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_scroll_view_create(f.tree, schultz_tree_root(f.tree), &view);
    content = tall_content(&f, view, 100.0f, 900.0f);
    schultz_button_create(f.tree, content, "Deep", &button);
    place(&f, view, 0, 0, 200, 200);
    schultz_node_set_bounds(f.tree, button,
                            schultz_rect_make(0, 300, 100, 30));

    /* Off the bottom of the view to begin with. */
    schultz_events_hit_test(f.events, schultz_point_make(50, 100), &hit);
    ASSERT(hit != button);

    schultz_scroll_view_scroll_to(f.tree, view,
                                  schultz_point_make(0.0f, 250.0f));
    schultz_events_hit_test(f.events, schultz_point_make(50, 60), &hit);
    ASSERT_EQ(button, hit);

    fixture_teardown(&f);
    PASS();
}

TEST the_wheel_scrolls_a_view(void)
{
    text_fixture f;
    schultz_handle view;
    schultz_handle bar_y;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_scroll_view_create(f.tree, schultz_tree_root(f.tree), &view);
    tall_content(&f, view, 50.0f, 900.0f);
    place(&f, view, 0, 0, 200, 200);
    bar_y = schultz_scroll_view_bar(f.tree, view, SCHULTZ_ORIENT_VERTICAL);

    schultz_events_scroll(f.events, schultz_point_make(50, 50), 0.0f, 2.0f);
    ASSERT(schultz_scroll_bar_value(f.tree, bar_y) > 0.0f);

    /* Scrolling back past the top stops at the top. */
    schultz_events_scroll(f.events, schultz_point_make(50, 50), 0.0f, -99.0f);
    ASSERT_EQ(0.0f, schultz_scroll_bar_value(f.tree, bar_y));

    fixture_teardown(&f);
    PASS();
}

TEST scroll_view_accessors_reject_other_widgets(void)
{
    text_fixture f;
    schultz_handle panel;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_scroll_view_content(f.tree, panel));
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_scroll_view_bar(f.tree, panel, 0));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_scroll_view_scroll_to(f.tree, panel,
                                            schultz_point_make(0, 0)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_scroll_view_create(f.tree, schultz_tree_root(f.tree),
                                         NULL));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------ TextField */

/* Builds a focused field ready to be typed into. */
static schultz_handle focused_field(text_fixture *f, const char *text)
{
    schultz_handle field = SCHULTZ_HANDLE_NONE;

    schultz_text_field_create(f->tree, schultz_tree_root(f->tree), text,
                              &field);
    place(f, field, 0, 0, 200, 30);
    schultz_events_set_focus(f->events, field);
    return field;
}

TEST a_text_field_holds_and_replaces_its_text(void)
{
    text_fixture f;
    schultz_handle field;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "hello");

    ASSERT_STR_EQ("hello", schultz_text_get(f.tree, field));
    ASSERT_STR_EQ("hello", schultz_node_get_value(f.tree, field));
    ASSERT_EQ((uint32_t)SCHULTZ_ROLE_TEXT_INPUT,
              schultz_node_get_role(f.tree, field));
    ASSERT_EQ((uint32_t)SCHULTZ_CURSOR_TEXT,
              schultz_node_cursor(f.tree, field));

    ASSERT_EQ(SCHULTZ_OK, schultz_text_set(f.tree, field, "goodbye"));
    ASSERT_STR_EQ("goodbye", schultz_text_get(f.tree, field));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_set(f.tree, field, NULL));
    ASSERT_STR_EQ("", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST typing_inserts_at_the_caret(void)
{
    text_fixture f;
    schultz_handle field;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "");

    schultz_events_text_input(f.events, "ab");
    schultz_events_text_input(f.events, "c");
    ASSERT_STR_EQ("abc", schultz_text_get(f.tree, field));

    /* Back one, then insert in the middle. */
    key(&f, SCHULTZ_KEY_LEFT, 0);
    schultz_events_text_input(f.events, "X");
    ASSERT_STR_EQ("abXc", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST backspace_and_delete_remove_one_character(void)
{
    text_fixture f;
    schultz_handle field;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "abcd");

    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    ASSERT_STR_EQ("abc", schultz_text_get(f.tree, field));

    key(&f, SCHULTZ_KEY_HOME, 0);
    key(&f, SCHULTZ_KEY_DELETE, 0);
    ASSERT_STR_EQ("bc", schultz_text_get(f.tree, field));

    /* At the ends there is nothing to remove and nothing breaks. */
    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    ASSERT_STR_EQ("bc", schultz_text_get(f.tree, field));
    key(&f, SCHULTZ_KEY_END, 0);
    key(&f, SCHULTZ_KEY_DELETE, 0);
    ASSERT_STR_EQ("bc", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

/*
 * The caret steps over whole characters. Splitting a multi byte character
 * would leave bytes no font can draw, so this is checked with text that is
 * not ASCII.
 */
TEST the_caret_steps_over_whole_characters(void)
{
    text_fixture f;
    schultz_handle field;
    uint32_t caret = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    /* Three characters, six bytes: each is two bytes in UTF-8. */
    field = focused_field(&f, "\xc3\xa9\xc3\xa8\xc3\xaa");

    key(&f, SCHULTZ_KEY_HOME, 0);
    key(&f, SCHULTZ_KEY_RIGHT, 0);
    schultz_text_selection(f.tree, field, NULL, &caret);
    ASSERT_EQ(2u, caret);

    key(&f, SCHULTZ_KEY_RIGHT, 0);
    schultz_text_selection(f.tree, field, NULL, &caret);
    ASSERT_EQ(4u, caret);

    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    ASSERT_STR_EQ("\xc3\xa9\xc3\xaa", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST shift_extends_the_selection_and_typing_replaces_it(void)
{
    text_fixture f;
    schultz_handle field;
    uint32_t anchor = 0;
    uint32_t caret = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "hello");

    key(&f, SCHULTZ_KEY_HOME, 0);
    key(&f, SCHULTZ_KEY_RIGHT, SCHULTZ_MOD_SHIFT);
    key(&f, SCHULTZ_KEY_RIGHT, SCHULTZ_MOD_SHIFT);
    schultz_text_selection(f.tree, field, &anchor, &caret);
    ASSERT_EQ(0u, anchor);
    ASSERT_EQ(2u, caret);

    schultz_events_text_input(f.events, "j");
    ASSERT_STR_EQ("jllo", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST control_arrows_move_by_word(void)
{
    text_fixture f;
    schultz_handle field;
    uint32_t caret = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "one two three");

    key(&f, SCHULTZ_KEY_HOME, 0);
    key(&f, SCHULTZ_KEY_RIGHT, SCHULTZ_MOD_CTRL);
    schultz_text_selection(f.tree, field, NULL, &caret);
    ASSERT_EQ(3u, caret);

    key(&f, SCHULTZ_KEY_RIGHT, SCHULTZ_MOD_CTRL);
    schultz_text_selection(f.tree, field, NULL, &caret);
    ASSERT_EQ(7u, caret);

    key(&f, SCHULTZ_KEY_LEFT, SCHULTZ_MOD_CTRL);
    schultz_text_selection(f.tree, field, NULL, &caret);
    ASSERT_EQ(4u, caret);

    /* Control with backspace removes the word before the caret. */
    key(&f, SCHULTZ_KEY_BACKSPACE, SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("two three", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST select_all_cut_and_paste_use_the_clipboard(void)
{
    text_fixture f;
    schultz_handle field;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "copy me");

    key(&f, 'a', SCHULTZ_MOD_CTRL);
    key(&f, 'c', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("copy me", f.board.text);
    ASSERT_STR_EQ("copy me", schultz_text_get(f.tree, field));

    key(&f, 'a', SCHULTZ_MOD_CTRL);
    key(&f, 'x', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("", schultz_text_get(f.tree, field));

    key(&f, 'v', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("copy me", schultz_text_get(f.tree, field));
    key(&f, 'v', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("copy mecopy me", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST editing_works_without_a_clipboard(void)
{
    text_fixture f;
    schultz_handle field;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    /* Take the clipboard away: everything else must still work. */
    schultz_tree_set_clipboard(f.tree, NULL, NULL, NULL, NULL);
    field = focused_field(&f, "text");

    key(&f, 'a', SCHULTZ_MOD_CTRL);
    key(&f, 'c', SCHULTZ_MOD_CTRL);
    key(&f, 'v', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("text", schultz_text_get(f.tree, field));

    schultz_events_text_input(f.events, "!");
    ASSERT_STR_EQ("!", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST undo_takes_back_a_run_of_typing_in_one_step(void)
{
    text_fixture f;
    schultz_handle field;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "start");

    schultz_events_text_input(f.events, "a");
    schultz_events_text_input(f.events, "b");
    schultz_events_text_input(f.events, "c");
    ASSERT_STR_EQ("startabc", schultz_text_get(f.tree, field));

    /* One step, not three: a run of typing is one edit. */
    key(&f, 'z', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("start", schultz_text_get(f.tree, field));

    key(&f, 'z', SCHULTZ_MOD_CTRL | SCHULTZ_MOD_SHIFT);
    ASSERT_STR_EQ("startabc", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST undo_separates_typing_from_deleting(void)
{
    text_fixture f;
    schultz_handle field;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "");

    schultz_events_text_input(f.events, "word");
    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    ASSERT_STR_EQ("wor", schultz_text_get(f.tree, field));

    key(&f, 'z', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("word", schultz_text_get(f.tree, field));
    key(&f, 'z', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("", schultz_text_get(f.tree, field));

    /* Nothing left to take back, and asking again is not an error. */
    key(&f, 'z', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("", schultz_text_get(f.tree, field));

    key(&f, 'y', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("word", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST a_new_edit_after_an_undo_drops_what_was_undone(void)
{
    text_fixture f;
    schultz_handle field;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "");

    schultz_events_text_input(f.events, "one");
    key(&f, SCHULTZ_KEY_END, 0);
    schultz_events_text_input(f.events, "two");
    key(&f, 'z', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("one", schultz_text_get(f.tree, field));

    key(&f, SCHULTZ_KEY_END, 0);
    schultz_events_text_input(f.events, "three");
    ASSERT_STR_EQ("onethree", schultz_text_get(f.tree, field));

    /* Redo cannot reach the branch that was abandoned. */
    key(&f, 'y', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("onethree", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST a_click_puts_the_caret_where_it_landed(void)
{
    text_fixture f;
    schultz_handle field;
    uint32_t start = 0;
    uint32_t end = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "abcdefgh");

    /* Far left is the start; far right is the end. */
    schultz_events_mouse_move(f.events, schultz_point_make(1, 15), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(1, 15),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_text_selection(f.tree, field, NULL, &start);
    ASSERT_EQ(0u, start);

    /* Drag to the far right: everything is selected. */
    schultz_events_mouse_move(f.events, schultz_point_make(195, 15), 0);
    schultz_text_selection(f.tree, field, NULL, &end);
    ASSERT_EQ(8u, end);
    schultz_events_mouse_button(f.events, schultz_point_make(195, 15),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    ASSERT_STR_EQ("", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST a_text_field_paints_its_text_selection_and_caret(void)
{
    text_fixture f;
    schultz_handle field;
    uint32_t plain;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "abc");
    schultz_events_set_focus(f.events, SCHULTZ_HANDLE_NONE);

    paint_all(&f);
    plain = count_kind(&f, SCHULTZ_DRAW_FILL_RECT);
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN));
    /* No focus, so no caret. */
    ASSERT_EQ(0u, plain);

    schultz_events_set_focus(f.events, field);
    paint_all(&f);
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_FILL_RECT));

    /* Selecting everything adds the band behind it. */
    schultz_text_set_selection(f.tree, field, 0u, 3u);
    paint_all(&f);
    ASSERT_EQ(2u, count_kind(&f, SCHULTZ_DRAW_FILL_RECT));

    /* Text is clipped to the content box. */
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_CLIP_BEGIN));
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_CLIP_END));

    fixture_teardown(&f);
    PASS();
}

TEST an_in_progress_composition_is_shown_but_not_committed(void)
{
    text_fixture f;
    schultz_handle field;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "");

    schultz_events_text_editing(f.events, "nihon", 0);
    /* Shown, but the buffer is untouched until it is committed. */
    ASSERT_STR_EQ("", schultz_text_get(f.tree, field));
    paint_all(&f);
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN));

    schultz_events_text_input(f.events, "\xe6\x97\xa5\xe6\x9c\xac");
    ASSERT_STR_EQ("\xe6\x97\xa5\xe6\x9c\xac",
                  schultz_text_get(f.tree, field));

    /* The composition is gone once committed. */
    paint_all(&f);
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN));

    fixture_teardown(&f);
    PASS();
}

TEST a_disabled_field_ignores_typing(void)
{
    text_fixture f;
    schultz_handle field;
    uint32_t state;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "locked");
    state = schultz_node_get_state(f.tree, field);
    schultz_node_set_state(f.tree, field,
                           state & ~(uint32_t)SCHULTZ_STATE_ENABLED);

    schultz_events_text_input(f.events, "x");
    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    ASSERT_STR_EQ("locked", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST text_accessors_reject_other_widgets(void)
{
    text_fixture f;
    schultz_handle panel;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ(NULL, schultz_text_get(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_text_set(f.tree, panel, "x"));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_text_set_selection(f.tree, panel, 0, 0));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_text_selection(f.tree, panel, NULL, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_text_field_create(f.tree, schultz_tree_root(f.tree),
                                        "x", NULL));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------- TextArea */

/*
 * A box that got taller with every line typed would push whatever is under it
 * down the screen, so an area stays the height it asked for and its text
 * scrolls inside it. Growing is something to ask for.
 */
TEST a_text_area_stays_put_unless_it_is_told_to_grow(void)
{
    text_fixture f;
    schultz_handle area;
    schultz_size few;
    schultz_size many;
    schultz_size grown;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_area_create(f.tree,
                    schultz_tree_root(f.tree), "one", &area));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, area, 300, -1, &few);

    schultz_text_set(f.tree, area, "one\ntwo\nthree\nfour\nfive\nsix");
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, area, 300, -1, &many);
    ASSERT_EQ(few.height, many.height);

    ASSERT_EQ(SCHULTZ_OK, schultz_text_area_set_grows(f.tree, area, 1));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, area, 300, -1, &grown);
    ASSERT(grown.height > many.height);

    /* Growing never shrinks it below the height it asked for. */
    schultz_text_set(f.tree, area, "one");
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, area, 300, -1, &grown);
    ASSERT_EQ(few.height, grown.height);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_text_area_set_grows(f.tree, schultz_tree_root(f.tree),
                                          1));

    fixture_teardown(&f);
    PASS();
}

/* Typing past the bottom scrolls the text rather than resizing the box. */
TEST typing_past_the_bottom_of_an_area_scrolls_it(void)
{
    text_fixture f;
    schultz_handle area;
    uint32_t i;
    float first_line_y = 0.0f;
    float later_first_line_y = 0.0f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree), "one", &area);
    place(&f, area, 0, 0, 300, 70);
    schultz_events_set_focus(f.events, area);

    paint_all(&f);
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_GLYPH_RUN) {
            first_line_y = cmd->as.glyph_run.glyphs[0].y;
            break;
        }
    }

    for (i = 0; i < 6u; i++) {
        key(&f, SCHULTZ_KEY_RETURN, 0);
        schultz_events_text_input(f.events, "x");
    }
    paint_all(&f);
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_GLYPH_RUN) {
            later_first_line_y = cmd->as.glyph_run.glyphs[0].y;
            break;
        }
    }

    /* The first line drawn has moved up, which is what scrolling looks like. */
    ASSERT(later_first_line_y < first_line_y);

    fixture_teardown(&f);
    PASS();
}

/*
 * A box the height of a field does not say "this is where a paragraph goes",
 * so an area asks for several lines of room before it holds several.
 */
TEST a_text_area_starts_several_lines_tall(void)
{
    text_fixture f;
    schultz_handle area;
    schultz_handle field;
    schultz_size empty;
    schultz_size one_line;
    schultz_size taller;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree), "", &area);
    schultz_text_field_create(f.tree, schultz_tree_root(f.tree), "", &field);
    schultz_tree_resolve_styles(f.tree);

    schultz_layout_measure(f.tree, area, 300, -1, &empty);
    schultz_layout_measure(f.tree, field, 300, -1, &one_line);
    ASSERT(empty.height > one_line.height * 2.0f);

    /* Asking for more gives more; asking for none is refused. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_area_set_visible_lines(f.tree, area, 8));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, area, 300, -1, &taller);
    ASSERT(taller.height > empty.height);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_text_area_set_visible_lines(f.tree, area, 0));
    /* A field is not an area, and neither is a panel. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_text_area_set_visible_lines(f.tree, field, 3));

    fixture_teardown(&f);
    PASS();
}

TEST enter_breaks_a_line_in_an_area_but_not_a_field(void)
{
    text_fixture f;
    schultz_handle area;
    schultz_handle field;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree), "a", &area);
    place(&f, area, 0, 0, 200, 100);
    schultz_events_set_focus(f.events, area);
    key(&f, SCHULTZ_KEY_RETURN, 0);
    schultz_events_text_input(f.events, "b");
    ASSERT_STR_EQ("a\nb", schultz_text_get(f.tree, area));

    field = focused_field(&f, "a");
    key(&f, SCHULTZ_KEY_RETURN, 0);
    ASSERT_STR_EQ("a", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST up_and_down_move_between_lines_and_keep_the_column(void)
{
    text_fixture f;
    schultz_handle area;
    uint32_t caret = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree),
                             "alpha\nbeta\ngamma", &area);
    place(&f, area, 0, 0, 300, 200);
    schultz_events_set_focus(f.events, area);

    /* Caret starts at the end, on "gamma". */
    key(&f, SCHULTZ_KEY_UP, 0);
    schultz_text_selection(f.tree, area, NULL, &caret);
    /* "beta" is shorter than the column, so the caret lands at its end. */
    ASSERT_EQ(10u, caret);

    key(&f, SCHULTZ_KEY_HOME, 0);
    schultz_text_selection(f.tree, area, NULL, &caret);
    ASSERT_EQ(6u, caret);

    key(&f, SCHULTZ_KEY_DOWN, 0);
    schultz_text_selection(f.tree, area, NULL, &caret);
    ASSERT_EQ(11u, caret);

    fixture_teardown(&f);
    PASS();
}

TEST an_area_paints_one_run_per_line(void)
{
    text_fixture f;
    schultz_handle area;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree),
                             "one\ntwo\nthree", &area);
    place(&f, area, 0, 0, 300, 200);

    paint_all(&f);
    ASSERT_EQ(3u, count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN));

    fixture_teardown(&f);
    PASS();
}

TEST an_area_wraps_at_its_own_width(void)
{
    text_fixture f;
    schultz_handle area;
    uint32_t wide;
    uint32_t narrow;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree),
        "the quick brown fox jumps over the lazy dog", &area);

    place(&f, area, 0, 0, 400, 200);
    paint_all(&f);
    wide = count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN);

    place(&f, area, 0, 0, 90, 200);
    paint_all(&f);
    narrow = count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN);

    ASSERT(narrow > wide);

    fixture_teardown(&f);
    PASS();
}

TEST an_empty_editor_paints_only_its_box(void)
{
    text_fixture f;
    schultz_handle field;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_field_create(f.tree, schultz_tree_root(f.tree), "", &field);
    place(&f, field, 0, 0, 200, 30);

    paint_all(&f);
    ASSERT_EQ(0u, count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN));
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_FILL_ROUND_RECT));

    fixture_teardown(&f);
    PASS();
}

TEST an_editor_with_no_size_paints_without_trouble(void)
{
    text_fixture f;
    schultz_handle field;
    schultz_handle area;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_field_create(f.tree, schultz_tree_root(f.tree), "hi",
                              &field);
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree), "hi", &area);
    schultz_node_set_bounds(f.tree, field, schultz_rect_make(0, 0, 0, 0));
    schultz_node_set_bounds(f.tree, area, schultz_rect_make(0, 0, 0, 0));

    paint_all(&f);

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------- the node itself */

/*
 * The offset is what a scroll view drives, but it is a plain node property
 * and works on any node, which is what makes a custom scrolling container
 * possible without reaching inside the toolkit.
 */
TEST a_scroll_offset_moves_children_and_not_the_node(void)
{
    text_fixture f;
    schultz_handle panel;
    schultz_handle child;
    schultz_rect before;
    schultz_rect after;
    schultz_point read;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_panel_create(f.tree, panel, &child);
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(10, 20, 100,
                                                             100));
    schultz_node_set_bounds(f.tree, child, schultz_rect_make(5, 5, 40, 40));

    read = schultz_node_scroll_offset(f.tree, panel);
    ASSERT_EQ(0.0f, read.x);
    ASSERT_EQ(0.0f, read.y);
    schultz_node_absolute_bounds(f.tree, child, &before);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_scroll_offset(f.tree, panel,
                                    schultz_point_make(7.0f, 11.0f)));
    read = schultz_node_scroll_offset(f.tree, panel);
    ASSERT_EQ(7.0f, read.x);
    ASSERT_EQ(11.0f, read.y);

    schultz_node_absolute_bounds(f.tree, child, &after);
    ASSERT_EQ(before.x - 7.0f, after.x);
    ASSERT_EQ(before.y - 11.0f, after.y);

    /* The node that scrolls does not move itself. */
    schultz_node_absolute_bounds(f.tree, panel, &after);
    ASSERT_EQ(10.0f, after.x);

    fixture_teardown(&f);
    PASS();
}

TEST the_node_flags_reject_bad_values(void)
{
    text_fixture f;
    schultz_handle panel;
    schultz_handle gone;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &gone);
    schultz_node_destroy(f.tree, gone);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_node_set_cursor(f.tree, panel, 999u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_cursor(f.tree, panel, SCHULTZ_CURSOR_POINTER));
    ASSERT_EQ((uint32_t)SCHULTZ_CURSOR_POINTER,
              schultz_node_cursor(f.tree, panel));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_set_cursor(f.tree, gone, SCHULTZ_CURSOR_TEXT));
    ASSERT_EQ((uint32_t)SCHULTZ_CURSOR_DEFAULT,
              schultz_node_cursor(f.tree, gone));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_set_scroll_offset(f.tree, gone,
                                             schultz_point_make(1, 1)));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------- the three fixes */

/*
 * A bar moves for many reasons and the content has to follow every one of
 * them. Dragging the thumb used to move the bar and leave the text behind,
 * because only the wheel pushed the offset through.
 */
TEST dragging_a_view_bar_moves_its_content(void)
{
    text_fixture f;
    schultz_handle view;
    schultz_handle content;
    schultz_handle bar_y;
    schultz_rect before;
    schultz_rect after;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_scroll_view_create(f.tree, schultz_tree_root(f.tree), &view);
    content = tall_content(&f, view, 50.0f, 900.0f);
    place(&f, view, 0, 0, 200, 200);
    bar_y = schultz_scroll_view_bar(f.tree, view, SCHULTZ_ORIENT_VERTICAL);
    schultz_node_absolute_bounds(f.tree, content, &before);

    /* Grab the thumb and drag it down the track. */
    schultz_events_mouse_move(f.events, schultz_point_make(194, 4), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(194, 4),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_move(f.events, schultz_point_make(194, 60), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(194, 60),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    ASSERT(schultz_scroll_bar_value(f.tree, bar_y) > 0.0f);
    schultz_node_absolute_bounds(f.tree, content, &after);
    ASSERT_EQ(before.y - schultz_scroll_bar_value(f.tree, bar_y), after.y);

    /* And so does setting the bar directly, without going through the view. */
    schultz_scroll_bar_set_value(f.tree, bar_y, 300.0f);
    schultz_node_absolute_bounds(f.tree, content, &after);
    ASSERT_EQ(before.y - 300.0f, after.y);

    fixture_teardown(&f);
    PASS();
}

TEST a_bar_used_alone_reports_that_it_moved(void)
{
    text_fixture f;
    schultz_handle bar;
    schultz_handle panel;
    float seen = -1.0f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_scroll_bar_create(f.tree, schultz_tree_root(f.tree),
                              SCHULTZ_ORIENT_VERTICAL, &bar);
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_scroll_bar_set_range(f.tree, bar, 400.0f, 100.0f);
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_bar_on_change(f.tree, bar,
                                                       record_change, &seen));

    schultz_scroll_bar_set_value(f.tree, bar, 120.0f);
    ASSERT_EQ(120.0f, seen);

    /* A value that does not move reports nothing. */
    seen = -1.0f;
    schultz_scroll_bar_set_value(f.tree, bar, 120.0f);
    ASSERT_EQ(-1.0f, seen);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_scroll_bar_on_change(f.tree, panel, record_change,
                                           &seen));

    fixture_teardown(&f);
    PASS();
}

/*
 * Where the text sits has to be settled before it is drawn. Correcting it
 * afterwards left the frame being built showing the old position, so the
 * character just typed stayed off the end of the box until something else
 * forced a repaint.
 */
TEST the_character_just_typed_is_visible(void)
{
    text_fixture f;
    schultz_handle field;
    schultz_rect box;
    uint32_t i;
    float caret = -1.0f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "");
    schultz_node_absolute_bounds(f.tree, field, &box);

    /* Type well past the end of the box. */
    for (i = 0; i < 60u; i++) {
        schultz_events_text_input(f.events, "m");
    }
    paint_all(&f);

    /* The caret is the last thin filled rectangle drawn. */
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);

        if (cmd->kind == SCHULTZ_DRAW_FILL_RECT &&
            cmd->as.fill_rect.rect.width <= 2.0f) {
            caret = cmd->as.fill_rect.rect.x;
        }
    }
    ASSERT(caret >= 0.0f);
    /* Inside the box, on this very frame. */
    ASSERT(caret >= box.x);
    ASSERT(caret <= box.x + box.width);

    fixture_teardown(&f);
    PASS();
}

TEST a_double_click_selects_a_word_and_a_triple_the_line(void)
{
    text_fixture f;
    schultz_handle field;
    schultz_rect box;
    schultz_point at;
    uint32_t low = 0;
    uint32_t high = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "one two three");
    schultz_node_absolute_bounds(f.tree, field, &box);
    /* Somewhere inside the middle word. */
    at = schultz_point_make(box.x + 40.0f, box.y + box.height * 0.5f);

    click_at(&f, at, 1000u);
    schultz_text_selection(f.tree, field, &low, &high);
    ASSERT_EQ(low, high); /* one press only places the caret */

    click_at(&f, at, 1100u);
    schultz_text_selection(f.tree, field, &low, &high);
    ASSERT_EQ(4u, low);
    ASSERT_EQ(7u, high); /* "two" */

    click_at(&f, at, 1200u);
    schultz_text_selection(f.tree, field, &low, &high);
    ASSERT_EQ(0u, low);
    ASSERT_EQ(13u, high); /* the whole line */

    fixture_teardown(&f);
    PASS();
}

/* Too slow, or too far away, and it is a fresh single click. */
TEST two_slow_or_distant_presses_are_two_single_clicks(void)
{
    text_fixture f;
    schultz_handle field;
    schultz_rect box;
    schultz_point at;
    uint32_t low = 0;
    uint32_t high = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "one two three");
    schultz_node_absolute_bounds(f.tree, field, &box);
    at = schultz_point_make(box.x + 40.0f, box.y + box.height * 0.5f);

    click_at(&f, at, 1000u);
    click_at(&f, at, 3000u);
    schultz_text_selection(f.tree, field, &low, &high);
    ASSERT_EQ(low, high);

    click_at(&f, at, 3050u);
    click_at(&f, schultz_point_make(at.x + 40.0f, at.y), 3100u);
    schultz_text_selection(f.tree, field, &low, &high);
    ASSERT_EQ(low, high);

    fixture_teardown(&f);
    PASS();
}

/* With no clock at all, every press is a single click and nothing breaks. */
TEST without_a_clock_every_press_is_a_single_click(void)
{
    text_fixture f;
    schultz_handle field;
    schultz_rect box;
    schultz_point at;
    uint32_t low = 0;
    uint32_t high = 0;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "one two three");
    schultz_node_absolute_bounds(f.tree, field, &box);
    at = schultz_point_make(box.x + 40.0f, box.y + box.height * 0.5f);

    for (i = 0; i < 3u; i++) {
        schultz_events_mouse_move(f.events, at, 0);
        schultz_events_mouse_button(f.events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
        schultz_events_mouse_button(f.events, at, SCHULTZ_BUTTON_LEFT, 0, 0);
    }
    schultz_text_selection(f.tree, field, &low, &high);
    ASSERT_EQ(low, high);

    fixture_teardown(&f);
    PASS();
}

TEST a_double_click_selects_one_line_of_an_area(void)
{
    text_fixture f;
    schultz_handle area;
    schultz_rect box;
    schultz_point at;
    uint32_t low = 0;
    uint32_t high = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree),
                             "alpha\nbeta\ngamma", &area);
    place(&f, area, 0, 0, 300, 200);
    schultz_events_set_focus(f.events, area);
    schultz_node_absolute_bounds(f.tree, area, &box);

    /* On the second line: three presses take that line, not the lot. */
    at = schultz_point_make(box.x + 15.0f, box.y + 25.0f);
    click_at(&f, at, 1000u);
    click_at(&f, at, 1080u);
    click_at(&f, at, 1160u);
    schultz_text_selection(f.tree, area, &low, &high);
    ASSERT_EQ(6u, low);
    ASSERT_EQ(10u, high);

    fixture_teardown(&f);
    PASS();
}

/* ----------------------------------------------------- password field */

TEST a_password_field_holds_the_real_text(void)
{
    schultz_tree *tree;
    schultz_handle field = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_password_field_create(tree,
        schultz_tree_root(tree), "hunter2", &field));

    /* Masked on screen, and the value is still the value. */
    ASSERT_EQ(SCHULTZ_PASSWORD_MASK, schultz_text_field_mask(tree, field));
    ASSERT_STR_EQ("hunter2", schultz_text_get(tree, field));

    schultz_tree_destroy(tree);
    PASS();
}

TEST clearing_the_mask_turns_it_back_into_a_text_field(void)
{
    schultz_tree *tree;
    schultz_handle field = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_password_field_create(tree,
        schultz_tree_root(tree), "secret", &field));

    ASSERT_EQ(SCHULTZ_OK, schultz_text_field_set_mask(tree, field, 0u));
    ASSERT_EQ(0u, schultz_text_field_mask(tree, field));
    ASSERT_STR_EQ("secret", schultz_text_get(tree, field));

    /* And back on again, with a different character. */
    ASSERT_EQ(SCHULTZ_OK, schultz_text_field_set_mask(tree, field, (uint32_t)'*'));
    ASSERT_EQ((uint32_t)'*', schultz_text_field_mask(tree, field));

    schultz_tree_destroy(tree);
    PASS();
}

/* A text area has wrapped lines, and a page of dots is not a password. */
TEST a_text_area_refuses_to_be_masked(void)
{
    schultz_tree *tree;
    schultz_handle area = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_area_create(tree,
        schultz_tree_root(tree), "notes", &area));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_text_field_set_mask(tree, area, SCHULTZ_PASSWORD_MASK));
    ASSERT_EQ(0u, schultz_text_field_mask(tree, area));

    schultz_tree_destroy(tree);
    PASS();
}

/* Copying a masked field must not put the text on the clipboard. */
TEST a_masked_field_refuses_to_copy_but_still_cuts(void)
{
    text_fixture f;
    schultz_handle field = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_password_field_create(f.tree,
        schultz_tree_root(f.tree), "hunter2", &field));
    schultz_node_set_bounds(f.tree, field, schultz_rect_make(0, 0, 200, 30));
    schultz_events_set_focus(f.events, field);
    schultz_text_set_selection(f.tree, field, 0u, 7u);

    key(&f, (uint32_t)'c', SCHULTZ_MOD_CTRL);
    ASSERT_EQ(0, f.board.written);
    ASSERT_STR_EQ("hunter2", schultz_text_get(f.tree, field));

    /* Cut still edits, it just does not hand the text over. */
    key(&f, (uint32_t)'x', SCHULTZ_MOD_CTRL);
    ASSERT_EQ(0, f.board.written);
    ASSERT_STR_EQ("", schultz_text_get(f.tree, field));

    /* With the mask off, copy works as it always did. */
    schultz_text_set(f.tree, field, "plain");
    schultz_text_field_set_mask(f.tree, field, 0u);
    schultz_text_set_selection(f.tree, field, 0u, 7u);
    key(&f, (uint32_t)'c', SCHULTZ_MOD_CTRL);
    ASSERT_EQ(1, f.board.written);
    ASSERT_STR_EQ("plain", f.board.text);

    fixture_teardown(&f);
    PASS();
}


/* ------------------------------------------------- selectable text blocks */

/* A wrapped, selectable paragraph placed at a known size. */
static schultz_handle prose(text_fixture *f, const char *text, float w,
                            float h)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    schultz_label_create(f->tree, schultz_tree_root(f->tree), text, &node);
    schultz_label_set_wrap(f->tree, node, 1);
    schultz_label_set_selectable(f->tree, node, 1);
    place(f, node, 0.0f, 0.0f, w, h);
    return node;
}

TEST a_label_is_not_selectable_until_it_is_asked_to_be(void)
{
    text_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t start = 9u;
    uint32_t end = 9u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_label_create(f.tree, schultz_tree_root(f.tree), "hello", &node);

    /* Off by default: turning it on everywhere would change what a click on
     * a caption does. */
    ASSERT_EQ(0, schultz_label_selectable(f.tree, node));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_label_set_selection(f.tree, node, 0u, 5u));

    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_selectable(f.tree, node, 1));
    ASSERT_EQ(1, schultz_label_selectable(f.tree, node));
    ASSERT_EQ(SCHULTZ_CURSOR_TEXT, schultz_node_cursor(f.tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_selection(f.tree, node, 0u, 5u));
    ASSERT_EQ(SCHULTZ_OK, schultz_label_selection(f.tree, node, &start, &end));
    ASSERT_EQ(0u, start);
    ASSERT_EQ(5u, end);

    fixture_teardown(&f);
    PASS();
}

TEST dragging_across_a_label_selects_what_was_crossed(void)
{
    text_fixture f;
    schultz_handle node;
    uint32_t start = 0u;
    uint32_t end = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two three four", 400.0f, 40.0f);

    schultz_events_mouse_move(f.events, schultz_point_make(2.0f, 8.0f), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(2.0f, 8.0f),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_move(f.events, schultz_point_make(60.0f, 8.0f), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(60.0f, 8.0f),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    ASSERT_EQ(SCHULTZ_OK, schultz_label_selection(f.tree, node, &start, &end));
    ASSERT(end > start);

    /* The node holding a selection is where copy is aimed, since a block of
     * prose never takes focus. */
    ASSERT_EQ(node, schultz_tree_selection_owner(f.tree));

    fixture_teardown(&f);
    PASS();
}

TEST copy_reaches_a_selection_that_never_took_focus(void)
{
    text_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two three", 400.0f, 40.0f);
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_selection(f.tree, node, 4u, 7u));

    /* Nothing holds focus, which is the whole point: the key still arrives. */
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_events_focus(f.events));
    schultz_events_key(f.events, 'c', SCHULTZ_MOD_CTRL, 1);

    ASSERT_EQ(1, f.board.written);
    ASSERT_STR_EQ("two", f.board.text);

    fixture_teardown(&f);
    PASS();
}

TEST control_a_takes_the_whole_block(void)
{
    text_fixture f;
    schultz_handle node;
    uint32_t start = 9u;
    uint32_t end = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two", 400.0f, 40.0f);
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_selection(f.tree, node, 0u, 1u));

    schultz_events_key(f.events, 'a', SCHULTZ_MOD_CTRL, 1);
    ASSERT_EQ(SCHULTZ_OK, schultz_label_selection(f.tree, node, &start, &end));
    ASSERT_EQ(0u, start);
    ASSERT_EQ(7u, end);

    fixture_teardown(&f);
    PASS();
}

TEST a_second_block_takes_the_selection_from_the_first(void)
{
    text_fixture f;
    schultz_handle first;
    schultz_handle second;
    uint32_t start = 9u;
    uint32_t end = 9u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    first = prose(&f, "first block", 400.0f, 40.0f);
    second = prose(&f, "second block", 400.0f, 40.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_selection(f.tree, first, 0u, 5u));
    ASSERT_EQ(first, schultz_tree_selection_owner(f.tree));

    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_selection(f.tree, second, 0u, 6u));
    ASSERT_EQ(second, schultz_tree_selection_owner(f.tree));

    /* Two highlights at once would be a lie about what copy would take. */
    ASSERT_EQ(SCHULTZ_OK, schultz_label_selection(f.tree, first, &start,
                                                  &end));
    ASSERT_EQ(start, end);

    fixture_teardown(&f);
    PASS();
}

TEST a_selection_is_painted_behind_the_text(void)
{
    text_fixture f;
    schultz_handle node;
    uint32_t plain;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two three", 400.0f, 40.0f);

    paint_all(&f);
    plain = count_kind(&f, SCHULTZ_DRAW_FILL_RECT);

    schultz_label_set_selection(f.tree, node, 0u, 3u);
    paint_all(&f);
    ASSERT_EQ(plain + 1u, count_kind(&f, SCHULTZ_DRAW_FILL_RECT));

    fixture_teardown(&f);
    PASS();
}

TEST replacing_the_text_drops_a_selection_into_the_old_string(void)
{
    text_fixture f;
    schultz_handle node;
    uint32_t start = 9u;
    uint32_t end = 9u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "a much longer piece of text", 400.0f, 40.0f);
    schultz_label_set_selection(f.tree, node, 2u, 20u);

    /* Offsets into a string that has been freed would be read on the next
     * paint, so replacing the text has to drop them. */
    schultz_label_set_text(f.tree, node, "hi");
    ASSERT_EQ(SCHULTZ_OK, schultz_label_selection(f.tree, node, &start, &end));
    ASSERT_EQ(0u, start);
    ASSERT_EQ(0u, end);
    paint_all(&f);

    fixture_teardown(&f);
    PASS();
}

TEST destroying_the_owner_leaves_no_selection_behind(void)
{
    text_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two", 400.0f, 40.0f);
    schultz_label_set_selection(f.tree, node, 0u, 3u);
    ASSERT_EQ(node, schultz_tree_selection_owner(f.tree));

    schultz_node_destroy(f.tree, node);
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_tree_selection_owner(f.tree));

    /* A key that would have been routed to the owner must not follow a
     * handle that no longer resolves. */
    schultz_events_key(f.events, 'c', SCHULTZ_MOD_CTRL, 1);
    ASSERT_EQ(0, f.board.written);

    fixture_teardown(&f);
    PASS();
}

TEST a_finger_selects_by_holding_still_and_not_by_dragging(void)
{
    text_fixture f;
    schultz_handle node;
    uint32_t start = 9u;
    uint32_t end = 9u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two three", 400.0f, 40.0f);

    schultz_events_set_pointer_source(f.events, SCHULTZ_POINTER_TOUCH);
    schultz_events_mouse_move(f.events, schultz_point_make(30.0f, 8.0f), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(30.0f, 8.0f),
                                SCHULTZ_BUTTON_LEFT, 1, 0);

    /* A finger that moves straight away is scrolling, not selecting. */
    ASSERT_EQ(SCHULTZ_OK, schultz_label_selection(f.tree, node, &start, &end));
    ASSERT_EQ(start, end);

    schultz_tree_advance(f.tree, 1000u);
    schultz_tree_advance(f.tree, 1000u + 500u);

    ASSERT_EQ(SCHULTZ_OK, schultz_label_selection(f.tree, node, &start, &end));
    ASSERT(end > start);
    ASSERT_EQ(node, schultz_tree_selection_owner(f.tree));

    fixture_teardown(&f);
    PASS();
}

TEST a_finger_that_moves_off_never_starts_a_selection(void)
{
    text_fixture f;
    schultz_handle node;
    uint32_t start = 9u;
    uint32_t end = 9u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two three", 400.0f, 40.0f);

    schultz_events_set_pointer_source(f.events, SCHULTZ_POINTER_TOUCH);
    schultz_events_mouse_move(f.events, schultz_point_make(30.0f, 8.0f), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(30.0f, 8.0f),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_move(f.events, schultz_point_make(30.0f, 60.0f), 0);

    schultz_tree_advance(f.tree, 1000u);
    schultz_tree_advance(f.tree, 1000u + 500u);

    ASSERT_EQ(SCHULTZ_OK, schultz_label_selection(f.tree, node, &start, &end));
    ASSERT_EQ(start, end);

    fixture_teardown(&f);
    PASS();
}

TEST a_mouse_does_not_wait_and_never_shows_grips(void)
{
    text_fixture f;
    schultz_handle node;
    uint32_t ellipses;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two three", 400.0f, 40.0f);

    schultz_events_set_pointer_source(f.events, SCHULTZ_POINTER_MOUSE);
    schultz_events_mouse_move(f.events, schultz_point_make(2.0f, 8.0f), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(2.0f, 8.0f),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_move(f.events, schultz_point_make(60.0f, 8.0f), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(60.0f, 8.0f),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    paint_all(&f);
    ellipses = count_kind(&f, SCHULTZ_DRAW_FILL_ELLIPSE);
    ASSERT_EQ(0u, ellipses);
    ASSERT_EQ(0.0f, schultz_node_paint_margin(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

TEST a_touch_selection_gets_a_grip_on_each_end(void)
{
    text_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two three", 400.0f, 40.0f);

    schultz_events_set_pointer_source(f.events, SCHULTZ_POINTER_TOUCH);
    schultz_events_mouse_move(f.events, schultz_point_make(30.0f, 8.0f), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(30.0f, 8.0f),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_tree_advance(f.tree, 1000u);
    schultz_tree_advance(f.tree, 1000u + 500u);

    paint_all(&f);
    ASSERT_EQ(2u, count_kind(&f, SCHULTZ_DRAW_FILL_ELLIPSE));

    /* A grip is drawn below the text, which is outside the node's bounds and
     * so has to be declared or it is painted once and never repainted. */
    ASSERT(schultz_node_paint_margin(f.tree, node) > 0.0f);

    fixture_teardown(&f);
    PASS();
}

TEST centred_text_sits_in_the_middle_of_its_width(void)
{
    text_fixture f;
    schultz_handle node;
    float left;
    float centred;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "short", 400.0f, 40.0f);

    paint_all(&f);
    left = 0.0f;
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_GLYPH_RUN) {
            left = cmd->as.glyph_run.glyphs[0].x;
        }
    }

    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_TEXT_ALIGN,
        schultz_value_number((float)SCHULTZ_TEXT_ALIGN_CENTER));
    paint_all(&f);
    centred = 0.0f;
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_GLYPH_RUN) {
            centred = cmd->as.glyph_run.glyphs[0].x;
        }
    }

    ASSERT(centred > left);

    fixture_teardown(&f);
    PASS();
}

TEST line_spacing_makes_a_block_taller_without_rewrapping_it(void)
{
    text_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_size tight;
    schultz_size airy;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_label_create(f.tree, schultz_tree_root(f.tree),
                         "a paragraph long enough to take several lines when "
                         "it is given a narrow column to sit in", &node);
    schultz_label_set_wrap(f.tree, node, 1);

    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, 200.0f, -1.0f,
                                                 &tight));

    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_LINE_SPACING,
                                    schultz_value_number(2.0f));
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, 200.0f, -1.0f,
                                                 &airy));

    /* Same lines, more room between them. */
    ASSERT(airy.height > tight.height);
    ASSERT_EQ(tight.width, airy.width);

    fixture_teardown(&f);
    PASS();
}

TEST justified_lines_reach_both_edges_except_the_last(void)
{
    text_fixture f;
    schultz_handle node;
    float first_end = 0.0f;
    float last_end = 0.0f;
    uint32_t runs = 0;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f,
                 "a paragraph long enough to take several lines when it is "
                 "given a narrow column to sit in", 200.0f, 200.0f);
    schultz_node_set_style_property(f.tree, node, SCHULTZ_PROP_TEXT_ALIGN,
        schultz_value_number((float)SCHULTZ_TEXT_ALIGN_JUSTIFY));
    paint_all(&f);

    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_GLYPH_RUN) {
            float end = cmd->as.glyph_run.glyphs[cmd->as.glyph_run.count - 1u].x;
            if (runs == 0u) {
                first_end = end;
            }
            last_end = end;
            runs++;
        }
    }

    ASSERT(runs > 1u);
    /* Every line but the last is stretched to the column, and the last keeps
     * its natural length, which is what stops a two word final line being
     * spread across the page. */
    ASSERT(first_end > 150.0f);
    ASSERT(last_end < first_end);

    fixture_teardown(&f);
    PASS();
}

TEST label_selection_accessors_reject_other_widgets(void)
{
    text_fixture f;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    uint32_t start = 0u;
    uint32_t end = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "press", &button);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_label_set_selectable(f.tree, button, 1));
    ASSERT_EQ(0, schultz_label_selectable(f.tree, button));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_label_set_selection(f.tree, button, 0u, 1u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_label_selection(f.tree, button, &start, &end));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_label_copy_selection(f.tree, button));

    fixture_teardown(&f);
    PASS();
}

TEST copying_nothing_puts_nothing_on_the_clipboard(void)
{
    text_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two", 400.0f, 40.0f);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_label_copy_selection(f.tree, node));
    ASSERT_EQ(0, f.board.written);

    schultz_label_set_selection(f.tree, node, 0u, 3u);
    ASSERT_EQ(SCHULTZ_OK, schultz_label_copy_selection(f.tree, node));
    ASSERT_STR_EQ("one", f.board.text);

    fixture_teardown(&f);
    PASS();
}

TEST turning_selection_off_drops_what_was_selected(void)
{
    text_fixture f;
    schultz_handle node;
    uint32_t start = 9u;
    uint32_t end = 9u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two", 400.0f, 40.0f);
    schultz_label_set_selection(f.tree, node, 0u, 3u);

    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_selectable(f.tree, node, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_label_selection(f.tree, node, &start, &end));
    ASSERT_EQ(start, end);
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_tree_selection_owner(f.tree));
    ASSERT_EQ(SCHULTZ_CURSOR_DEFAULT, schultz_node_cursor(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

TEST what_is_selected_is_reported_for_a_reader(void)
{
    text_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two three", 400.0f, 40.0f);

    /* A label's name is its whole text, so the value is the part of it that
     * is selected. */
    ASSERT_EQ(NULL, schultz_node_get_value(f.tree, node));
    schultz_label_set_selection(f.tree, node, 4u, 7u);
    ASSERT_STR_EQ("two", schultz_node_get_value(f.tree, node));
    schultz_label_set_selection(f.tree, node, 4u, 4u);
    ASSERT_EQ(NULL, schultz_node_get_value(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

TEST a_label_that_is_not_selectable_ignores_the_pointer(void)
{
    text_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t start = 9u;
    uint32_t end = 9u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_label_create(f.tree, schultz_tree_root(f.tree), "one two", &node);
    place(&f, node, 0.0f, 0.0f, 400.0f, 40.0f);

    schultz_events_mouse_move(f.events, schultz_point_make(2.0f, 8.0f), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(2.0f, 8.0f),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_move(f.events, schultz_point_make(60.0f, 8.0f), 0);

    ASSERT_EQ(SCHULTZ_OK, schultz_label_selection(f.tree, node, &start, &end));
    ASSERT_EQ(start, end);
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_tree_selection_owner(f.tree));

    fixture_teardown(&f);
    PASS();
}


TEST a_grip_sits_on_the_line_its_end_belongs_to(void)
{
    text_fixture f;
    schultz_handle node;
    float first_x = 0.0f;
    float first_y = 0.0f;
    float second_x = 0.0f;
    float second_y = 0.0f;
    uint32_t grips = 0u;
    uint32_t start = 0u;
    uint32_t end = 0u;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    /*
     * At this width the first row is "alpha bravo " and the second starts at
     * byte 12, which is also where the first row ends. An offset at a wrap
     * point belongs to both rows, and the opening grip belongs to the later
     * one.
     */
    node = prose(&f, "alpha bravo charlie delta echo foxtrot", 120.0f,
                 100.0f);

    schultz_events_set_pointer_source(f.events, SCHULTZ_POINTER_TOUCH);
    schultz_events_mouse_move(f.events, schultz_point_make(20.0f, 28.0f), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(20.0f, 28.0f),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_tree_advance(f.tree, 1000u);
    schultz_tree_advance(f.tree, 1000u + 500u);

    ASSERT_EQ(SCHULTZ_OK, schultz_label_selection(f.tree, node, &start, &end));
    ASSERT_EQ(12u, start);

    paint_all(&f);
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);

        if (cmd->kind != SCHULTZ_DRAW_FILL_ELLIPSE) {
            continue;
        }
        if (grips == 0u) {
            first_x = cmd->as.fill_ellipse.rect.x;
            first_y = cmd->as.fill_ellipse.rect.y;
        } else {
            second_x = cmd->as.fill_ellipse.rect.x;
            second_y = cmd->as.fill_ellipse.rect.y;
        }
        grips++;
    }

    ASSERT_EQ(2u, grips);
    /* Both ends of one word are on one line, so both grips are too. Taking
     * the first row that contains the offset puts the opening grip at the far
     * right of the line above. */
    ASSERT_EQ(first_y, second_y);
    ASSERT(first_x < second_x);

    fixture_teardown(&f);
    PASS();
}


/*
 * A family emoji is five characters joined by zero width joiners and one
 * picture on screen. The caret has to treat it as the one thing it looks
 * like: crossing it is one press, and deleting it takes the whole thing
 * rather than leaving three people and a joiner behind.
 */
TEST the_caret_treats_an_emoji_as_one_character(void)
{
    text_fixture f;
    schultz_handle field;
    uint32_t caret = 0;
    /* 25 bytes: four people and three joiners. */
    static const char *family =
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6";

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    /*
     * Split before the b on purpose: a hex escape swallows the letter after
     * it, so "\xA6b" would be one bad character rather than an emoji and a b.
     */
    field = focused_field(&f, "a\xF0\x9F\x91\xA8\xE2\x80\x8D"
                              "\xF0\x9F\x91\xA9\xE2\x80\x8D"
                              "\xF0\x9F\x91\xA7\xE2\x80\x8D"
                              "\xF0\x9F\x91\xA6" "b");

    key(&f, SCHULTZ_KEY_HOME, 0);
    key(&f, SCHULTZ_KEY_RIGHT, 0);
    schultz_text_selection(f.tree, field, NULL, &caret);
    ASSERT_EQ(1u, caret);

    /* One press crosses the whole family, not one of its five parts. */
    key(&f, SCHULTZ_KEY_RIGHT, 0);
    schultz_text_selection(f.tree, field, NULL, &caret);
    ASSERT_EQ(26u, caret);

    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    ASSERT_STR_EQ("ab", schultz_text_get(f.tree, field));

    /* And forward delete does the same from the other side. */
    schultz_text_set(f.tree, field, family);
    schultz_text_set_selection(f.tree, field, 0u, 0u);
    key(&f, SCHULTZ_KEY_DELETE, 0);
    ASSERT_STR_EQ("", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST a_line_with_an_emoji_in_it_is_drawn_by_two_faces(void)
{
    text_fixture f;
    schultz_handle label = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));

    /*
     * A draw command names one font, so a line the text face cannot draw on
     * its own becomes more than one command: letters, emoji, letters.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_label_create(f.tree,
        schultz_tree_root(f.tree), "Hi \xF0\x9F\x91\x8D there", &label));
    place(&f, label, 0.0f, 0.0f, 300.0f, 24.0f);
    paint_all(&f);
    ASSERT_EQ(3u, count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN));

    /* Plain text is still one, the way it always was. */
    schultz_label_set_text(f.tree, label, "Hi there");
    place(&f, label, 0.0f, 0.0f, 300.0f, 24.0f);
    paint_all(&f);
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN));

    fixture_teardown(&f);
    PASS();
}


/*
 * A phone reports no key presses. It keeps a text field of its own, watches
 * it change, and spells the change out afterwards as one backspace for every
 * codepoint that went. One tap over an emoji written as a base and a
 * modifier arrives as two backspaces, and taking a whole character for each
 * of those takes the emoji and whatever stood before it.
 */
TEST a_spelled_out_delete_takes_only_what_the_platform_took(void)
{
    text_fixture f;
    schultz_handle field;
    /* Dark sunglasses: U+1F576 and the character that says to draw it as a
     * picture. Two codepoints, one emoji. */
    static const char *glasses = "\xF0\x9F\x95\xB6\xEF\xB8\x8F";

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "");

    schultz_events_set_delete_by_codepoint(f.events, 1);
    schultz_text_set(f.tree, field, "a");
    schultz_events_text_input(f.events, glasses);
    ASSERT_STR_EQ("a\xF0\x9F\x95\xB6\xEF\xB8\x8F",
                  schultz_text_get(f.tree, field));

    /* The two the platform would send for its one tap. */
    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    ASSERT_STR_EQ("a", schultz_text_get(f.tree, field));

    /* An emoji of one codepoint is one backspace, and still just the one. */
    schultz_text_set(f.tree, field, "a\xF0\x9F\x98\x8E");
    schultz_text_set_selection(f.tree, field, 5u, 5u);
    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    ASSERT_STR_EQ("a", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

TEST a_real_key_press_still_takes_the_whole_character(void)
{
    text_fixture f;
    schultz_handle field;
    static const char *family =
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7";

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    field = focused_field(&f, "");

    /* Not set, which is a keyboard with keys on it. */
    schultz_text_set(f.tree, field, "a");
    schultz_events_text_input(f.events, family);
    schultz_events_text_input(f.events, "\xF0\x9F\x95\xB6\xEF\xB8\x8F");

    /* One press each, whatever the emoji is spelled with. */
    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    ASSERT_STR_EQ("a\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
                  "\xE2\x80\x8D\xF0\x9F\x91\xA7",
                  schultz_text_get(f.tree, field));
    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    ASSERT_STR_EQ("a", schultz_text_get(f.tree, field));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------ rich text */

/*
 * A span says a stretch of a label is not set the way the label is. These
 * check each thing a span can say, on its own and alongside the others, and
 * that saying them in another order changes nothing.
 */

/* A span marking [start, end) and nothing else. */
static schultz_span span_over(uint32_t start, uint32_t end)
{
    schultz_span span;

    memset(&span, 0, sizeof(span));
    span.start = start;
    span.end   = end;
    return span;
}

/* How many glyph runs a paint produced, and how many carry one colour. */
static uint32_t runs_in_colour(text_fixture *f, schultz_color want)
{
    uint32_t found = 0u;
    uint32_t i;

    for (i = 0; i < schultz_draw_list_count(&f->list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f->list, i);

        if (cmd->kind == SCHULTZ_DRAW_GLYPH_RUN &&
            schultz_color_equals(cmd->as.glyph_run.paint.as.color, want)) {
            found++;
        }
    }
    return found;
}

/* The first filled rectangle painted in a colour, or a zero rectangle. */
static schultz_rect rect_in_colour(text_fixture *f, schultz_color want)
{
    uint32_t i;

    for (i = 0; i < schultz_draw_list_count(&f->list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f->list, i);

        if (cmd->kind == SCHULTZ_DRAW_FILL_RECT &&
            schultz_color_equals(cmd->as.fill_rect.paint.as.color, want)) {
            return cmd->as.fill_rect.rect;
        }
    }
    return schultz_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
}

TEST a_label_with_no_spans_paints_what_it_always_did(void)
{
    text_fixture f;
    schultz_handle node;
    uint32_t plain;
    uint32_t emptied;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two three", 400.0f, 40.0f);

    plain = paint_all(&f);
    ASSERT(plain > 0u);

    /* Saying there are no spans is not the same call as never saying it,
     * and it has to come out the same. Every label in the toolkit is this
     * case, so it is the one that must not drift. */
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, NULL, 0u));
    emptied = paint_all(&f);
    ASSERT_EQ(plain, emptied);

    fixture_teardown(&f);
    PASS();
}

TEST a_coloured_span_is_painted_in_its_own_colour(void)
{
    text_fixture f;
    schultz_handle node;
    schultz_span span;
    schultz_color red = schultz_color_rgba(200u, 0u, 0u, 255u);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two", 400.0f, 40.0f);

    paint_all(&f);
    ASSERT_EQ(0u, runs_in_colour(&f, red));

    span = span_over(0u, 3u);
    span.color = red;
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, &span, 1u));

    /*
     * A draw command names one paint, so the line has to come out as more
     * than one command once part of it is another colour.
     */
    paint_all(&f);
    ASSERT_EQ(1u, runs_in_colour(&f, red));
    ASSERT(count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN) > 1u);

    fixture_teardown(&f);
    PASS();
}

TEST a_span_background_is_painted_behind_the_words(void)
{
    text_fixture f;
    schultz_handle node;
    schultz_span span;
    schultz_color wash = schultz_color_rgba(0u, 90u, 160u, 255u);
    schultz_rect behind;
    uint32_t i;
    int32_t first_glyph = -1;
    int32_t the_rect = -1;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two", 400.0f, 40.0f);

    span = span_over(4u, 7u);
    span.background = wash;
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, &span, 1u));
    paint_all(&f);

    behind = rect_in_colour(&f, wash);
    ASSERT(behind.width > 0.0f);
    ASSERT(behind.height > 0.0f);

    /* Behind means before: the words are drawn over it, not under it. */
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);

        if (the_rect < 0 && cmd->kind == SCHULTZ_DRAW_FILL_RECT &&
            schultz_color_equals(cmd->as.fill_rect.paint.as.color, wash)) {
            the_rect = (int32_t)i;
        }
        if (first_glyph < 0 && cmd->kind == SCHULTZ_DRAW_GLYPH_RUN) {
            first_glyph = (int32_t)i;
        }
    }
    ASSERT(the_rect >= 0);
    ASSERT(first_glyph >= 0);
    ASSERT(the_rect < first_glyph);

    fixture_teardown(&f);
    PASS();
}

TEST an_underline_sits_below_a_strikethrough(void)
{
    text_fixture f;
    schultz_handle node;
    schultz_span span;
    schultz_color ink = schultz_color_rgba(10u, 120u, 30u, 255u);
    schultz_rect under;
    schultz_rect through;
    uint32_t plain_rects;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two", 400.0f, 40.0f);
    paint_all(&f);
    plain_rects = count_kind(&f, SCHULTZ_DRAW_FILL_RECT);

    span = span_over(0u, 3u);
    span.underline = 1u;
    span.color = ink;
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, &span, 1u));
    paint_all(&f);
    ASSERT_EQ(plain_rects + 1u, count_kind(&f, SCHULTZ_DRAW_FILL_RECT));
    under = rect_in_colour(&f, ink);
    ASSERT(under.width > 0.0f);
    ASSERT(under.height >= 1.0f);

    span.underline     = 0u;
    span.strikethrough = 1u;
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, &span, 1u));
    paint_all(&f);
    ASSERT_EQ(plain_rects + 1u, count_kind(&f, SCHULTZ_DRAW_FILL_RECT));
    through = rect_in_colour(&f, ink);
    ASSERT(through.width > 0.0f);

    /*
     * Down the screen is larger, so a line under the words sits at a greater
     * y than one through them. Both come from the face rather than from a
     * fraction, and this is what says they were not swapped.
     */
    ASSERT(under.y > through.y);

    fixture_teardown(&f);
    PASS();
}

TEST both_rules_can_be_drawn_at_once(void)
{
    text_fixture f;
    schultz_handle node;
    schultz_span span;
    uint32_t plain_rects;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two", 400.0f, 40.0f);
    paint_all(&f);
    plain_rects = count_kind(&f, SCHULTZ_DRAW_FILL_RECT);

    span = span_over(0u, 7u);
    span.underline     = 1u;
    span.strikethrough = 1u;
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, &span, 1u));
    paint_all(&f);
    ASSERT_EQ(plain_rects + 2u, count_kind(&f, SCHULTZ_DRAW_FILL_RECT));

    fixture_teardown(&f);
    PASS();
}

TEST a_bold_span_is_drawn_in_the_bold_face(void)
{
    text_fixture f;
    schultz_handle node;
    schultz_span span;
    schultz_handle bold = SCHULTZ_HANDLE_NONE;
    uint32_t in_bold = 0u;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two", 400.0f, 40.0f);
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(f.fonts, f.font, 1, 0, &bold));
    ASSERT(bold != f.font);

    span = span_over(0u, 3u);
    span.bold = 1u;
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, &span, 1u));
    paint_all(&f);

    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);

        if (cmd->kind == SCHULTZ_DRAW_GLYPH_RUN &&
            cmd->as.glyph_run.font == bold) {
            in_bold += cmd->as.glyph_run.count;
        }
    }
    /* Three letters bold, and they came from the bold face rather than from
     * the same face drawn some other way. */
    ASSERT_EQ(3u, in_bold);

    fixture_teardown(&f);
    PASS();
}

TEST a_larger_span_makes_the_label_taller(void)
{
    text_fixture f;
    schultz_handle node;
    schultz_span span;
    schultz_size plain;
    schultz_size grown;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_label_create(f.tree, schultz_tree_root(f.tree), "one two", &node);
    /* Measuring reads the resolved style to find the face. */
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, 400.0f, 400.0f,
                                               &plain));
    ASSERT(plain.height > 0.0f);
    span = span_over(0u, 3u);
    span.size = 40.0f;
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, &span, 1u));
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, node, 400.0f, 400.0f,
                                               &grown));

    /* A label measured as though every line matched the first would cut the
     * big word off at the bottom. */
    ASSERT(grown.height > plain.height);
    ASSERT(grown.width > plain.width);

    fixture_teardown(&f);
    PASS();
}

TEST spans_in_any_order_paint_the_same(void)
{
    text_fixture f;
    schultz_handle node;
    schultz_span forward[3];
    schultz_span backward[3];
    uint32_t first;
    uint32_t again;
    uint32_t glyph_runs;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "aaa bbb ccc ddd", 400.0f, 60.0f);

    forward[0] = span_over(0u, 3u);
    forward[0].bold = 1u;
    forward[1] = span_over(4u, 7u);
    forward[1].color = schultz_color_rgba(200u, 0u, 0u, 255u);
    forward[2] = span_over(8u, 11u);
    forward[2].underline = 1u;

    backward[0] = forward[2];
    backward[1] = forward[1];
    backward[2] = forward[0];

    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, forward, 3u));
    first = paint_all(&f);
    glyph_runs = count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN);

    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, backward, 3u));
    again = paint_all(&f);

    /*
     * Spans describe the text rather than act on it in turn, so the order a
     * program happened to build them in must not show on screen.
     */
    ASSERT_EQ(first, again);
    ASSERT_EQ(glyph_runs, count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN));

    fixture_teardown(&f);
    PASS();
}

TEST setting_the_text_clears_the_spans(void)
{
    text_fixture f;
    schultz_handle node;
    schultz_span span;
    uint32_t count = 99u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two", 400.0f, 40.0f);

    span = span_over(0u, 3u);
    span.bold = 1u;
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, &span, 1u));
    ASSERT(schultz_label_spans(f.tree, node, &count) != NULL);
    ASSERT_EQ(1u, count);

    /*
     * A span is byte offsets into the words that were there. Kept across a
     * change of words it would mark whatever now sits at those bytes.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_text(f.tree, node, "something"));
    ASSERT_EQ(NULL, schultz_label_spans(f.tree, node, &count));
    ASSERT_EQ(0u, count);

    fixture_teardown(&f);
    PASS();
}

TEST spans_that_mark_nothing_are_dropped(void)
{
    text_fixture f;
    schultz_handle node;
    schultz_span spans[3];
    const schultz_span *kept;
    uint32_t count = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two", 400.0f, 40.0f);

    spans[0] = span_over(3u, 3u);   /* covers nothing */
    spans[1] = span_over(90u, 99u); /* past the end of the text */
    spans[2] = span_over(0u, 400u); /* runs off the end, and is cut to fit */
    spans[2].bold = 1u;
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, spans, 3u));

    kept = schultz_label_spans(f.tree, node, &count);
    ASSERT_EQ(1u, count);
    ASSERT(kept != NULL);
    ASSERT_EQ(0u, kept[0].start);
    ASSERT_EQ(7u, kept[0].end);

    fixture_teardown(&f);
    PASS();
}

TEST the_span_setter_refuses_what_it_should(void)
{
    text_fixture f;
    schultz_handle node;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    schultz_span span;
    uint32_t count = 7u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = prose(&f, "one two", 400.0f, 40.0f);
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "press", &button);
    span = span_over(0u, 3u);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_label_set_spans(f.tree, button, &span, 1u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_label_set_spans(f.tree, node, NULL, 2u));
    ASSERT_EQ(NULL, schultz_label_spans(f.tree, button, &count));
    ASSERT_EQ(0u, count);

    /* Saying nothing is allowed, and means nothing is styled. */
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(f.tree, node, NULL, 0u));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------- pressable spans */

/*
 * A span can be pressed without being a link, which is what a keyword in a
 * code editor is. The press arrives as an ordinary click naming which
 * stretch it landed on, so these read the event the host would have been
 * given.
 */

/* The click the host was told about, or NULL when it was told nothing. */
static const schultz_event *last_click(text_fixture *f)
{
    const schultz_event *queued = NULL;
    const schultz_event *found = NULL;
    uint32_t count = 0u;
    uint32_t i;

    if (schultz_events_drain(f->events, &queued, &count) != SCHULTZ_OK) {
        return NULL;
    }
    for (i = 0u; i < count; i++) {
        if (queued[i].type == SCHULTZ_EVENT_CLICK) {
            found = &queued[i];
        }
    }
    return found;
}

/* A label of pressable prose, with the first word marked. */
static schultz_handle pressable(text_fixture *f, uint32_t selectable,
                                uint64_t tag)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_span span;

    schultz_label_create(f->tree, schultz_tree_root(f->tree),
                         "press me please", &node);
    schultz_label_set_wrap(f->tree, node, 1);
    schultz_label_set_selectable(f->tree, node, selectable);
    place(f, node, 0.0f, 0.0f, 400.0f, 40.0f);

    memset(&span, 0, sizeof(span));
    span.start     = 0u;
    span.end       = 5u;       /* "press" */
    span.clickable = 1u;
    span.tag       = tag;
    span.underline = 1u;
    schultz_label_set_spans(f->tree, node, &span, 1u);
    schultz_tree_resolve_styles(f->tree);
    return node;
}

TEST pressing_a_span_says_which_one_it_was(void)
{
    text_fixture f;
    schultz_handle node;
    const schultz_event *click;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_queue(f.events, 1));
    node = pressable(&f, 1u, 4242u);
    (void)node;

    click_at(&f, schultz_point_make(4.0f, 8.0f), 10u);
    click = last_click(&f);
    ASSERT(click != NULL);
    ASSERT_EQ(0u, click->span);
    ASSERT_EQ((uint64_t)4242u, click->span_tag);

    fixture_teardown(&f);
    PASS();
}

TEST a_span_works_on_a_label_that_cannot_be_selected(void)
{
    text_fixture f;
    const schultz_event *click;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_queue(f.events, 1));
    /*
     * Prose a reader may not copy can still carry something to press, and a
     * caption is the ordinary case of that.
     */
    pressable(&f, 0u, 7u);

    click_at(&f, schultz_point_make(4.0f, 8.0f), 10u);
    click = last_click(&f);
    ASSERT(click != NULL);
    ASSERT_EQ(0u, click->span);
    ASSERT_EQ((uint64_t)7u, click->span_tag);

    fixture_teardown(&f);
    PASS();
}

TEST pressing_beside_a_span_says_nothing_was_pressed(void)
{
    text_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_span span;
    const schultz_event *click;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_queue(f.events, 1));
    schultz_label_create(f.tree, schultz_tree_root(f.tree), "press me", &node);
    schultz_label_set_wrap(f.tree, node, 1);
    place(&f, node, 0.0f, 0.0f, 400.0f, 40.0f);
    memset(&span, 0, sizeof(span));
    span.start     = 0u;
    span.end       = 5u;
    span.clickable = 1u;
    span.tag       = 9u;
    schultz_label_set_spans(f.tree, node, &span, 1u);
    schultz_tree_resolve_styles(f.tree);

    /*
     * Far to the right of the last word. The nearest byte is the end of the
     * text, so anything going by nearest offset would report the span that
     * ended there; the empty part of a line presses nothing.
     */
    click_at(&f, schultz_point_make(380.0f, 8.0f), 10u);
    click = last_click(&f);
    ASSERT(click != NULL);
    ASSERT_EQ((uint32_t)SCHULTZ_SPAN_NONE, click->span);
    ASSERT_EQ((uint64_t)0u, click->span_tag);

    fixture_teardown(&f);
    PASS();
}

TEST a_span_that_was_not_marked_is_not_pressable(void)
{
    text_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_span span;
    const schultz_event *click;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_queue(f.events, 1));
    schultz_label_create(f.tree, schultz_tree_root(f.tree), "press me", &node);
    schultz_label_set_wrap(f.tree, node, 1);
    place(&f, node, 0.0f, 0.0f, 400.0f, 40.0f);

    /*
     * A link that is shown rather than offered: it looks like a link and
     * copies as one, and pressing it does nothing. That is the whole reason
     * the flag is separate from the address.
     */
    memset(&span, 0, sizeof(span));
    span.start     = 0u;
    span.end       = 5u;
    span.link      = "https://example.org/";
    span.underline = 1u;
    schultz_label_set_spans(f.tree, node, &span, 1u);
    schultz_tree_resolve_styles(f.tree);

    click_at(&f, schultz_point_make(4.0f, 8.0f), 10u);
    click = last_click(&f);
    ASSERT(click != NULL);
    ASSERT_EQ((uint32_t)SCHULTZ_SPAN_NONE, click->span);

    fixture_teardown(&f);
    PASS();
}

TEST a_drag_across_a_span_does_not_press_it(void)
{
    text_fixture f;
    const schultz_event *click;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_queue(f.events, 1));
    pressable(&f, 1u, 5u);

    /*
     * Pressed and released inside the marked word, with a drag between them
     * that selected part of it. Both ends are over the span, so what has to
     * stop this being a press is that a selection was made, not that the
     * pointer ended up somewhere else.
     */
    schultz_events_set_time(f.events, 10u);
    schultz_events_mouse_move(f.events, schultz_point_make(2.0f, 8.0f), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(2.0f, 8.0f),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_move(f.events, schultz_point_make(24.0f, 8.0f), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(24.0f, 8.0f),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    /*
     * The node was still pressed and released, so the host still hears that
     * the label was clicked. What it must not hear is that the stretch the
     * drag began on was pressed.
     */
    click = last_click(&f);
    ASSERT(click != NULL);
    ASSERT_EQ((uint32_t)SCHULTZ_SPAN_NONE, click->span);
    ASSERT_EQ((uint64_t)0u, click->span_tag);

    fixture_teardown(&f);
    PASS();
}

TEST the_pointer_turns_to_a_hand_over_a_pressable_span(void)
{
    text_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = pressable(&f, 1u, 1u);

    /* Over the marked word. */
    schultz_events_mouse_move(f.events, schultz_point_make(4.0f, 8.0f), 0);
    ASSERT_EQ((uint32_t)SCHULTZ_CURSOR_POINTER,
              schultz_node_cursor(f.tree, node));

    /*
     * And back to the I-beam over the rest of it, because the words there
     * can still be taken even though there is nothing to press.
     */
    schultz_events_mouse_move(f.events, schultz_point_make(80.0f, 8.0f), 0);
    ASSERT_EQ((uint32_t)SCHULTZ_CURSOR_TEXT,
              schultz_node_cursor(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

TEST an_ordinary_click_still_says_no_span_was_pressed(void)
{
    text_fixture f;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    const schultz_event *click;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_queue(f.events, 1));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "press", &button);
    place(&f, button, 0.0f, 0.0f, 120.0f, 30.0f);
    schultz_tree_resolve_styles(f.tree);

    /* Every widget that is not styled text reports the same thing, so a
     * host may read the field without checking what it clicked. */
    click_at(&f, schultz_point_make(10.0f, 10.0f), 10u);
    click = last_click(&f);
    ASSERT(click != NULL);
    ASSERT_EQ((uint32_t)SCHULTZ_SPAN_NONE, click->span);
    ASSERT_EQ((uint64_t)0u, click->span_tag);

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------ editing the look */

/*
 * Spans on text that can be edited. What has to hold is that a stretch keeps
 * marking the same characters however the text around it moves: typing before
 * it pushes it along, typing inside it joins it, and deleting what it marked
 * takes it with them.
 */

/* An editable area with "one two three" and "two" marked bold. */
static schultz_handle marked_area(text_fixture *f)
{
    schultz_handle area = SCHULTZ_HANDLE_NONE;
    schultz_span span;

    schultz_text_area_create(f->tree, schultz_tree_root(f->tree),
                             "one two three", &area);
    place(f, area, 0, 0, 400, 70);
    schultz_events_set_focus(f->events, area);

    memset(&span, 0, sizeof(span));
    span.start = 4u;
    span.end   = 7u;
    span.bold  = 1u;
    schultz_text_field_set_spans(f->tree, area, &span, 1u);
    return area;
}

/* The one span the widget is carrying, or NULL. */
static const schultz_span *only_span(text_fixture *f, schultz_handle node,
                                     uint32_t want)
{
    uint32_t count = 0u;
    const schultz_span *spans = schultz_text_field_spans(f->tree, node,
                                                         &count);

    return (count == want) ? spans : NULL;
}

TEST typing_before_a_span_pushes_it_along(void)
{
    text_fixture f;
    schultz_handle area;
    const schultz_span *spans;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    area = marked_area(&f);

    /* At the very start, well before the marked word. */
    schultz_text_set_selection(f.tree, area, 0u, 0u);
    schultz_events_text_input(f.events, "XY");

    spans = only_span(&f, area, 1u);
    ASSERT(spans != NULL);
    ASSERT_EQ(6u, spans[0].start);
    ASSERT_EQ(9u, spans[0].end);

    fixture_teardown(&f);
    PASS();
}

TEST typing_inside_a_span_joins_it(void)
{
    text_fixture f;
    schultz_handle area;
    const schultz_span *spans;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    area = marked_area(&f);

    /* Between the "t" and the "w" of the bold word. */
    schultz_text_set_selection(f.tree, area, 5u, 5u);
    schultz_events_text_input(f.events, "Z");

    spans = only_span(&f, area, 1u);
    ASSERT(spans != NULL);
    ASSERT_EQ(4u, spans[0].start);
    ASSERT_EQ(8u, spans[0].end);

    fixture_teardown(&f);
    PASS();
}

TEST typing_at_the_end_of_a_span_carries_on_in_it(void)
{
    text_fixture f;
    schultz_handle area;
    const schultz_span *spans;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    area = marked_area(&f);

    /*
     * Right after the last letter of the bold word. Taking after the
     * character before the caret is what continues a word being typed.
     */
    schultz_text_set_selection(f.tree, area, 7u, 7u);
    schultz_events_text_input(f.events, "!");

    spans = only_span(&f, area, 1u);
    ASSERT(spans != NULL);
    ASSERT_EQ(4u, spans[0].start);
    ASSERT_EQ(8u, spans[0].end);

    fixture_teardown(&f);
    PASS();
}

TEST typing_at_the_start_of_a_span_stays_outside_it(void)
{
    text_fixture f;
    schultz_handle area;
    const schultz_span *spans;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    area = marked_area(&f);

    /* Immediately in front of the bold word: not part of it. */
    schultz_text_set_selection(f.tree, area, 4u, 4u);
    schultz_events_text_input(f.events, "Q");

    spans = only_span(&f, area, 1u);
    ASSERT(spans != NULL);
    ASSERT_EQ(5u, spans[0].start);
    ASSERT_EQ(8u, spans[0].end);

    fixture_teardown(&f);
    PASS();
}

TEST deleting_what_a_span_marked_takes_the_span_with_it(void)
{
    text_fixture f;
    schultz_handle area;
    uint32_t count = 99u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    area = marked_area(&f);

    /* Select the bold word and cut it out. */
    schultz_text_set_selection(f.tree, area, 4u, 7u);
    key(&f, SCHULTZ_KEY_BACKSPACE, 0);

    /* Nothing is marked, because the characters that were marked have gone.
     * A span left behind would mark whatever moved into their place. */
    schultz_text_field_spans(f.tree, area, &count);
    ASSERT_EQ(0u, count);

    fixture_teardown(&f);
    PASS();
}

TEST deleting_part_of_a_span_keeps_the_rest(void)
{
    text_fixture f;
    schultz_handle area;
    const schultz_span *spans;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    area = marked_area(&f);

    /* Take the first letter of the bold word only. */
    schultz_text_set_selection(f.tree, area, 4u, 5u);
    key(&f, SCHULTZ_KEY_BACKSPACE, 0);

    spans = only_span(&f, area, 1u);
    ASSERT(spans != NULL);
    ASSERT_EQ(4u, spans[0].start);
    ASSERT_EQ(6u, spans[0].end);

    fixture_teardown(&f);
    PASS();
}

TEST a_look_asked_for_before_there_is_text_is_given_to_it(void)
{
    text_fixture f;
    schultz_handle area = SCHULTZ_HANDLE_NONE;
    schultz_span look;
    const schultz_span *spans;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree), "ab", &area);
    place(&f, area, 0, 0, 400, 70);
    schultz_events_set_focus(f.events, area);

    /* A Bold button pressed with nothing selected. */
    schultz_text_set_selection(f.tree, area, 2u, 2u);
    memset(&look, 0, sizeof(look));
    look.bold = 1u;
    ASSERT_EQ(SCHULTZ_OK, schultz_text_field_set_typing(f.tree, area, &look));

    schultz_events_text_input(f.events, "cd");
    spans = only_span(&f, area, 1u);
    ASSERT(spans != NULL);
    ASSERT_EQ(2u, spans[0].start);
    ASSERT_EQ(4u, spans[0].end);
    ASSERT_EQ(1u, spans[0].bold);

    fixture_teardown(&f);
    PASS();
}

TEST moving_the_caret_forgets_a_look_that_was_asked_for(void)
{
    text_fixture f;
    schultz_handle area = SCHULTZ_HANDLE_NONE;
    schultz_span look;
    uint32_t count = 99u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree), "ab", &area);
    place(&f, area, 0, 0, 400, 70);
    schultz_events_set_focus(f.events, area);

    schultz_text_set_selection(f.tree, area, 2u, 2u);
    memset(&look, 0, sizeof(look));
    look.bold = 1u;
    ASSERT_EQ(SCHULTZ_OK, schultz_text_field_set_typing(f.tree, area, &look));

    /* Thought better of: the caret went somewhere else first. */
    key(&f, SCHULTZ_KEY_LEFT, 0);
    schultz_events_text_input(f.events, "Z");

    schultz_text_field_spans(f.tree, area, &count);
    ASSERT_EQ(0u, count);

    fixture_teardown(&f);
    PASS();
}

TEST marking_a_range_splits_what_was_there(void)
{
    text_fixture f;
    schultz_handle area;
    const schultz_span *spans;
    uint32_t count = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    area = marked_area(&f);

    /*
     * A different look over the middle of the bold word, which is what a
     * button pressed on a selection inside a marked phrase does. The old
     * span has to come apart round it.
     */
    {
        schultz_span look;

        memset(&look, 0, sizeof(look));
        look.italic = 1u;
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_text_field_set_span(f.tree, area, 5u, 6u, &look));
    }

    spans = schultz_text_field_spans(f.tree, area, &count);
    ASSERT_EQ(3u, count);
    ASSERT(spans != NULL);
    ASSERT_EQ(4u, spans[0].start);
    ASSERT_EQ(5u, spans[0].end);
    ASSERT_EQ(1u, spans[0].bold);
    ASSERT_EQ(5u, spans[1].start);
    ASSERT_EQ(6u, spans[1].end);
    ASSERT_EQ(1u, spans[1].italic);
    ASSERT_EQ(0u, spans[1].bold);
    ASSERT_EQ(6u, spans[2].start);
    ASSERT_EQ(7u, spans[2].end);
    ASSERT_EQ(1u, spans[2].bold);

    fixture_teardown(&f);
    PASS();
}

TEST clearing_a_range_strips_it_back_to_plain(void)
{
    text_fixture f;
    schultz_handle area;
    uint32_t count = 99u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    area = marked_area(&f);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_field_set_span(f.tree, area, 0u, 13u, NULL));
    schultz_text_field_spans(f.tree, area, &count);
    ASSERT_EQ(0u, count);

    fixture_teardown(&f);
    PASS();
}

TEST neighbours_that_say_the_same_thing_are_merged(void)
{
    text_fixture f;
    schultz_handle area = SCHULTZ_HANDLE_NONE;
    schultz_span look;
    const schultz_span *spans;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree), "abcdef",
                             &area);
    place(&f, area, 0, 0, 400, 70);

    memset(&look, 0, sizeof(look));
    look.bold = 1u;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_field_set_span(f.tree, area, 0u, 3u, &look));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_field_set_span(f.tree, area, 3u, 6u, &look));

    /*
     * Two touching stretches that look alike are one stretch. Without this a
     * paragraph typed a character at a time would end with one span per
     * character and the list would grow for the whole session.
     */
    spans = only_span(&f, area, 1u);
    ASSERT(spans != NULL);
    ASSERT_EQ(0u, spans[0].start);
    ASSERT_EQ(6u, spans[0].end);

    fixture_teardown(&f);
    PASS();
}

TEST spans_with_different_tags_are_not_merged(void)
{
    text_fixture f;
    schultz_handle area = SCHULTZ_HANDLE_NONE;
    schultz_span look;
    uint32_t count = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree), "abcdef",
                             &area);
    place(&f, area, 0, 0, 400, 70);

    memset(&look, 0, sizeof(look));
    look.clickable = 1u;
    look.tag = 1u;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_field_set_span(f.tree, area, 0u, 3u, &look));
    look.tag = 2u;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_field_set_span(f.tree, area, 3u, 6u, &look));

    /* Two things the program named separately are two things, however alike
     * they look on the screen. */
    schultz_text_field_spans(f.tree, area, &count);
    ASSERT_EQ(2u, count);

    fixture_teardown(&f);
    PASS();
}

TEST taking_back_an_edit_takes_back_the_look_as_well(void)
{
    text_fixture f;
    schultz_handle area;
    const schultz_span *spans;
    uint32_t count = 99u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    area = marked_area(&f);

    /* Cut the bold word out, which takes its span with it. */
    schultz_text_set_selection(f.tree, area, 4u, 7u);
    key(&f, SCHULTZ_KEY_BACKSPACE, 0);
    schultz_text_field_spans(f.tree, area, &count);
    ASSERT_EQ(0u, count);

    /*
     * Undo has to give back the words and the way they looked. Giving back
     * plain text would be a different document from the one that was there.
     */
    key(&f, 'z', SCHULTZ_MOD_CTRL);
    ASSERT_STR_EQ("one two three", schultz_text_get(f.tree, area));
    spans = only_span(&f, area, 1u);
    ASSERT(spans != NULL);
    ASSERT_EQ(4u, spans[0].start);
    ASSERT_EQ(7u, spans[0].end);
    ASSERT_EQ(1u, spans[0].bold);

    fixture_teardown(&f);
    PASS();
}

TEST setting_spans_refuses_a_widget_that_holds_no_text(void)
{
    text_fixture f;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    schultz_span look;
    uint32_t count = 7u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "press", &button);
    memset(&look, 0, sizeof(look));
    look.start = 0u;
    look.end   = 1u;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_text_field_set_spans(f.tree, button, &look, 1u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_text_field_set_span(f.tree, button, 0u, 1u, &look));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_text_field_set_typing(f.tree, button, &look));
    ASSERT_EQ(NULL, schultz_text_field_spans(f.tree, button, &count));
    ASSERT_EQ(0u, count);

    fixture_teardown(&f);
    PASS();
}

TEST an_area_draws_a_bold_span_in_the_bold_face(void)
{
    text_fixture f;
    schultz_handle bold = SCHULTZ_HANDLE_NONE;
    uint32_t in_bold = 0u;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    (void)marked_area(&f);
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(f.fonts, f.font, 1, 0, &bold));
    ASSERT(bold != f.font);

    paint_all(&f);
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);

        if (cmd->kind == SCHULTZ_DRAW_GLYPH_RUN &&
            cmd->as.glyph_run.font == bold) {
            in_bold += cmd->as.glyph_run.count;
        }
    }
    ASSERT_EQ(3u, in_bold);   /* "two" */

    fixture_teardown(&f);
    PASS();
}

TEST an_area_draws_a_coloured_span_in_its_own_colour(void)
{
    text_fixture f;
    schultz_handle area = SCHULTZ_HANDLE_NONE;
    schultz_span span;
    schultz_color red = schultz_color_rgba(200u, 0u, 0u, 255u);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree), "one two",
                             &area);
    place(&f, area, 0, 0, 400, 70);

    paint_all(&f);
    ASSERT_EQ(0u, runs_in_colour(&f, red));

    memset(&span, 0, sizeof(span));
    span.start = 0u;
    span.end   = 3u;
    span.color = red;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_field_set_spans(f.tree, area, &span, 1u));
    paint_all(&f);
    ASSERT_EQ(1u, runs_in_colour(&f, red));
    ASSERT(count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN) > 1u);

    fixture_teardown(&f);
    PASS();
}

TEST an_area_draws_the_rules_a_span_asks_for(void)
{
    text_fixture f;
    schultz_handle area = SCHULTZ_HANDLE_NONE;
    schultz_span span;
    uint32_t plain_rects;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_area_create(f.tree, schultz_tree_root(f.tree), "one two",
                             &area);
    place(&f, area, 0, 0, 400, 70);
    paint_all(&f);
    plain_rects = count_kind(&f, SCHULTZ_DRAW_FILL_RECT);

    memset(&span, 0, sizeof(span));
    span.start         = 0u;
    span.end           = 3u;
    span.underline     = 1u;
    span.strikethrough = 1u;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_field_set_spans(f.tree, area, &span, 1u));
    paint_all(&f);
    ASSERT_EQ(plain_rects + 2u, count_kind(&f, SCHULTZ_DRAW_FILL_RECT));

    fixture_teardown(&f);
    PASS();
}

TEST a_masked_field_is_drawn_plain_whatever_its_spans_say(void)
{
    text_fixture f;
    schultz_handle field = SCHULTZ_HANDLE_NONE;
    schultz_span span;
    schultz_color red = schultz_color_rgba(200u, 0u, 0u, 255u);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_field_create(f.tree, schultz_tree_root(f.tree), "secret",
                              &field);
    place(&f, field, 0, 0, 200, 30);

    memset(&span, 0, sizeof(span));
    span.start = 0u;
    span.end   = 3u;
    span.color = red;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_field_set_spans(f.tree, field, &span, 1u));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_field_set_mask(f.tree, field, 0x2022u));

    /*
     * The dots are not the text the spans describe, and colouring three of
     * them would say where the marked part of the secret is.
     */
    paint_all(&f);
    ASSERT_EQ(0u, runs_in_colour(&f, red));

    fixture_teardown(&f);
    PASS();
}

SUITE(text_widgets)
{
    RUN_TEST(an_area_draws_a_bold_span_in_the_bold_face);
    RUN_TEST(an_area_draws_a_coloured_span_in_its_own_colour);
    RUN_TEST(an_area_draws_the_rules_a_span_asks_for);
    RUN_TEST(a_masked_field_is_drawn_plain_whatever_its_spans_say);
    RUN_TEST(typing_before_a_span_pushes_it_along);
    RUN_TEST(typing_inside_a_span_joins_it);
    RUN_TEST(typing_at_the_end_of_a_span_carries_on_in_it);
    RUN_TEST(typing_at_the_start_of_a_span_stays_outside_it);
    RUN_TEST(deleting_what_a_span_marked_takes_the_span_with_it);
    RUN_TEST(deleting_part_of_a_span_keeps_the_rest);
    RUN_TEST(a_look_asked_for_before_there_is_text_is_given_to_it);
    RUN_TEST(moving_the_caret_forgets_a_look_that_was_asked_for);
    RUN_TEST(marking_a_range_splits_what_was_there);
    RUN_TEST(clearing_a_range_strips_it_back_to_plain);
    RUN_TEST(neighbours_that_say_the_same_thing_are_merged);
    RUN_TEST(spans_with_different_tags_are_not_merged);
    RUN_TEST(taking_back_an_edit_takes_back_the_look_as_well);
    RUN_TEST(setting_spans_refuses_a_widget_that_holds_no_text);
    RUN_TEST(pressing_a_span_says_which_one_it_was);
    RUN_TEST(a_span_works_on_a_label_that_cannot_be_selected);
    RUN_TEST(pressing_beside_a_span_says_nothing_was_pressed);
    RUN_TEST(a_span_that_was_not_marked_is_not_pressable);
    RUN_TEST(a_drag_across_a_span_does_not_press_it);
    RUN_TEST(the_pointer_turns_to_a_hand_over_a_pressable_span);
    RUN_TEST(an_ordinary_click_still_says_no_span_was_pressed);
    RUN_TEST(a_label_with_no_spans_paints_what_it_always_did);
    RUN_TEST(a_coloured_span_is_painted_in_its_own_colour);
    RUN_TEST(a_span_background_is_painted_behind_the_words);
    RUN_TEST(an_underline_sits_below_a_strikethrough);
    RUN_TEST(both_rules_can_be_drawn_at_once);
    RUN_TEST(a_bold_span_is_drawn_in_the_bold_face);
    RUN_TEST(a_larger_span_makes_the_label_taller);
    RUN_TEST(spans_in_any_order_paint_the_same);
    RUN_TEST(setting_the_text_clears_the_spans);
    RUN_TEST(spans_that_mark_nothing_are_dropped);
    RUN_TEST(the_span_setter_refuses_what_it_should);
    RUN_TEST(a_password_field_holds_the_real_text);
    RUN_TEST(clearing_the_mask_turns_it_back_into_a_text_field);
    RUN_TEST(a_text_area_refuses_to_be_masked);
    RUN_TEST(a_masked_field_refuses_to_copy_but_still_cuts);
    RUN_TEST(dragging_a_view_bar_moves_its_content);
    RUN_TEST(a_bar_used_alone_reports_that_it_moved);
    RUN_TEST(the_character_just_typed_is_visible);
    RUN_TEST(a_double_click_selects_a_word_and_a_triple_the_line);
    RUN_TEST(two_slow_or_distant_presses_are_two_single_clicks);
    RUN_TEST(without_a_clock_every_press_is_a_single_click);
    RUN_TEST(a_double_click_selects_one_line_of_an_area);
    RUN_TEST(a_scroll_offset_moves_children_and_not_the_node);
    RUN_TEST(the_node_flags_reject_bad_values);
    RUN_TEST(a_scroll_bar_clamps_to_what_there_is_to_scroll);
    RUN_TEST(a_scroll_bar_rejects_bad_arguments);
    RUN_TEST(a_scroll_bar_thumb_shrinks_as_the_content_grows);
    RUN_TEST(dragging_a_scroll_bar_keeps_the_grab_point);
    RUN_TEST(pressing_a_scroll_bar_track_pages);
    RUN_TEST(scroll_bar_keys_move_it);
    RUN_TEST(a_scroll_view_shows_a_bar_only_when_it_is_needed);
    RUN_TEST(a_finger_dragging_from_a_field_scrolls_the_page);
    RUN_TEST(a_wheel_over_a_sideways_view_turns_it_sideways);
    RUN_TEST(a_view_told_to_hide_its_bars_keeps_them_away);
    RUN_TEST(scrolling_moves_the_content_and_not_its_bounds);
    RUN_TEST(hit_testing_follows_the_scrolled_content);
    RUN_TEST(the_wheel_scrolls_a_view);
    RUN_TEST(scroll_view_accessors_reject_other_widgets);
    RUN_TEST(a_text_field_holds_and_replaces_its_text);
    RUN_TEST(typing_inserts_at_the_caret);
    RUN_TEST(backspace_and_delete_remove_one_character);
    RUN_TEST(the_caret_steps_over_whole_characters);
    RUN_TEST(shift_extends_the_selection_and_typing_replaces_it);
    RUN_TEST(control_arrows_move_by_word);
    RUN_TEST(select_all_cut_and_paste_use_the_clipboard);
    RUN_TEST(editing_works_without_a_clipboard);
    RUN_TEST(undo_takes_back_a_run_of_typing_in_one_step);
    RUN_TEST(undo_separates_typing_from_deleting);
    RUN_TEST(a_new_edit_after_an_undo_drops_what_was_undone);
    RUN_TEST(a_click_puts_the_caret_where_it_landed);
    RUN_TEST(a_text_field_paints_its_text_selection_and_caret);
    RUN_TEST(an_in_progress_composition_is_shown_but_not_committed);
    RUN_TEST(a_disabled_field_ignores_typing);
    RUN_TEST(text_accessors_reject_other_widgets);
    RUN_TEST(a_text_area_stays_put_unless_it_is_told_to_grow);
    RUN_TEST(typing_past_the_bottom_of_an_area_scrolls_it);
    RUN_TEST(a_text_area_starts_several_lines_tall);
    RUN_TEST(enter_breaks_a_line_in_an_area_but_not_a_field);
    RUN_TEST(up_and_down_move_between_lines_and_keep_the_column);
    RUN_TEST(an_area_paints_one_run_per_line);
    RUN_TEST(an_area_wraps_at_its_own_width);
    RUN_TEST(an_empty_editor_paints_only_its_box);
    RUN_TEST(an_editor_with_no_size_paints_without_trouble);
    RUN_TEST(a_label_is_not_selectable_until_it_is_asked_to_be);
    RUN_TEST(dragging_across_a_label_selects_what_was_crossed);
    RUN_TEST(copy_reaches_a_selection_that_never_took_focus);
    RUN_TEST(control_a_takes_the_whole_block);
    RUN_TEST(a_second_block_takes_the_selection_from_the_first);
    RUN_TEST(a_selection_is_painted_behind_the_text);
    RUN_TEST(replacing_the_text_drops_a_selection_into_the_old_string);
    RUN_TEST(destroying_the_owner_leaves_no_selection_behind);
    RUN_TEST(a_finger_selects_by_holding_still_and_not_by_dragging);
    RUN_TEST(a_finger_that_moves_off_never_starts_a_selection);
    RUN_TEST(a_mouse_does_not_wait_and_never_shows_grips);
    RUN_TEST(a_touch_selection_gets_a_grip_on_each_end);
    RUN_TEST(centred_text_sits_in_the_middle_of_its_width);
    RUN_TEST(line_spacing_makes_a_block_taller_without_rewrapping_it);
    RUN_TEST(justified_lines_reach_both_edges_except_the_last);
    RUN_TEST(label_selection_accessors_reject_other_widgets);
    RUN_TEST(copying_nothing_puts_nothing_on_the_clipboard);
    RUN_TEST(turning_selection_off_drops_what_was_selected);
    RUN_TEST(what_is_selected_is_reported_for_a_reader);
    RUN_TEST(a_label_that_is_not_selectable_ignores_the_pointer);
    RUN_TEST(a_grip_sits_on_the_line_its_end_belongs_to);
    RUN_TEST(the_caret_treats_an_emoji_as_one_character);
    RUN_TEST(a_spelled_out_delete_takes_only_what_the_platform_took);
    RUN_TEST(a_real_key_press_still_takes_the_whole_character);
    RUN_TEST(a_line_with_an_emoji_in_it_is_drawn_by_two_faces);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(text_widgets);
    GREATEST_MAIN_END();
}
