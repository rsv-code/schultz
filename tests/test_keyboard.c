/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_keyboard.c - the keyboard the toolkit draws for a machine that has no
 * other way to type.
 *
 * What is asserted is what a field ends up holding, because that is the whole
 * point of the widget: a key press has to arrive as though it came from a
 * real keyboard. The keys are reached the way a screen reader reaches them,
 * which needs no pointer and no layout pass.
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
    schultz_events      *events;
    schultz_font_system *fonts;
    schultz_glyph_cache *glyphs;
    schultz_handle       font;
    schultz_theme        theme;
    schultz_handle       keyboard;
    schultz_handle       field;
} keyboard_fixture;

static int32_t fixture_setup(keyboard_fixture *f)
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
    result = schultz_text_field_create(f->tree, schultz_tree_root(f->tree),
                                       "", &f->field);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_keyboard_create(f->tree, schultz_tree_root(f->tree),
                                     f->events, &f->keyboard);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /* Typing goes to whatever holds focus, which is the field in every test
     * here. */
    return schultz_events_set_focus(f->events, f->field);
}

static void fixture_teardown(keyboard_fixture *f)
{
    schultz_events_destroy(f->events);
    schultz_tree_destroy(f->tree);
    schultz_glyph_cache_destroy(f->glyphs);
    schultz_font_system_destroy(f->fonts);
}

/* One key, by where it sits: which set, which row, which key along it. */
static schultz_handle key_at(keyboard_fixture *f, uint32_t set, uint32_t row,
                             uint32_t index)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    if (schultz_node_child_at(f->tree, f->keyboard, set, &node) != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    if (schultz_node_child_at(f->tree, node, row, &node) != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    if (schultz_node_child_at(f->tree, node, index, &node) != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    return node;
}

/*
 * One emoji key, by its place in the list. The faces are not rows of keys
 * like the other sets: they are three to a column in a field that scrolls
 * sideways, so they are reached through the view that scrolls them.
 */
static schultz_handle emoji_at(keyboard_fixture *f, uint32_t index)
{
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    if (schultz_node_child_at(f->tree, f->keyboard, 3u, &node) != SCHULTZ_OK ||
        schultz_node_child_at(f->tree, node, 0u, &view) != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    node = schultz_scroll_view_content(f->tree, view);
    if (schultz_node_child_at(f->tree, node, index / 3u, &node)
            != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    if (schultz_node_child_at(f->tree, node, index % 3u, &node)
            != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    return node;
}

/*
 * Lays the keyboard out somewhere real, which pressing a key with a pointer
 * needs: a key with no bounds is a key the pointer cannot land on.
 */
static void lay_out(keyboard_fixture *f)
{
    schultz_node_set_bounds(f->tree, schultz_tree_root(f->tree),
                            schultz_rect_make(0, 0, 800, 600));
    schultz_node_set_bounds(f->tree, f->keyboard,
                            schultz_rect_make(0, 400, 800, 200));
    /* The keyboard itself, the way the window does it: bounds alone are not
     * a layout, and the root has no pane to carry the mark downward. */
    schultz_node_invalidate_layout(f->tree, f->keyboard);
    schultz_tree_resolve_styles(f->tree);
    schultz_layout_run(f->tree);
}

/*
 * Presses a key the way a finger does, through the router, which is the path
 * that decides focus. Pressing through the accessibility action below skips
 * that decision, so both are worth having.
 */
static int32_t tap(keyboard_fixture *f, schultz_handle key)
{
    schultz_rect at;
    schultz_point middle;
    int32_t result;

    if (schultz_node_absolute_bounds(f->tree, key, &at) != SCHULTZ_OK ||
        at.width <= 0.0f || at.height <= 0.0f) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    middle = schultz_point_make(at.x + at.width / 2.0f,
                                at.y + at.height / 2.0f);
    /*
     * A key consumes its press, and consumed is a positive answer rather
     * than a failure. Only a negative one is something going wrong.
     */
    result = schultz_events_mouse_button(f->events, middle,
                                         SCHULTZ_BUTTON_LEFT, 1, 0u);
    if (result < 0) {
        return result;
    }
    result = schultz_events_mouse_button(f->events, middle,
                                         SCHULTZ_BUTTON_LEFT, 0, 0u);
    return (result < 0) ? result : SCHULTZ_OK;
}

/* Presses it the way an assistive technology would, which needs no pointer. */
static int32_t press(keyboard_fixture *f, schultz_handle key)
{
    return schultz_events_perform(f->events, key, SCHULTZ_ACTION_CLICK);
}

/* Which set is showing, read off the visible flags rather than from inside. */
static uint32_t showing(keyboard_fixture *f)
{
    uint32_t i;

    for (i = 0u; i < 4u; i++) {
        schultz_handle set = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(f->tree, f->keyboard, i, &set)
                != SCHULTZ_OK) {
            continue;
        }
        if (schultz_node_get_state(f->tree, set) & SCHULTZ_STATE_VISIBLE) {
            return i;
        }
    }
    return 99u;
}

/* --------------------------------------------------------------- tests */

TEST a_key_types_into_the_focused_field(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 0u, 0u))); /* q */
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 1u, 0u))); /* a */
    ASSERT_STR_EQ("qa", schultz_text_get(f.tree, f.field));
    fixture_teardown(&f);
    PASS();
}

TEST a_key_leaves_focus_on_the_field(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 0u, 0u)));
    /* A key that took focus would end the edit it exists to serve. */
    ASSERT_EQ(f.field, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

TEST backspace_takes_the_last_letter_back(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 0u, 0u))); /* q */
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 0u, 1u))); /* w */
    /* Last key on the third row of the letters. */
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 2u, 8u)));
    ASSERT_STR_EQ("q", schultz_text_get(f.tree, f.field));
    fixture_teardown(&f);
    PASS();
}

TEST shift_gives_one_capital_and_then_lower_case_again(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(0u, showing(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 2u, 0u))); /* shift */
    ASSERT_EQ(1u, showing(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 1u, 0u, 0u))); /* Q */
    /* One capital is what shift means, so the letters come back by
     * themselves. */
    ASSERT_EQ(0u, showing(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 0u, 1u))); /* w */
    ASSERT_STR_EQ("Qw", schultz_text_get(f.tree, f.field));
    fixture_teardown(&f);
    PASS();
}

TEST shift_twice_holds_the_capitals_down(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 2u, 0u))); /* shift */
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 1u, 2u, 0u))); /* and again */
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 1u, 0u, 0u))); /* Q */
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 1u, 0u, 1u))); /* W */
    ASSERT_STR_EQ("QW", schultz_text_get(f.tree, f.field));
    ASSERT_EQ(1u, showing(&f));
    fixture_teardown(&f);
    PASS();
}

TEST the_numbers_key_reaches_the_digits(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    /* First key on the bottom row of the letters. */
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 3u, 0u)));
    ASSERT_EQ(2u, showing(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 2u, 0u, 0u))); /* 1 */
    ASSERT_STR_EQ("1", schultz_text_get(f.tree, f.field));
    fixture_teardown(&f);
    PASS();
}

TEST an_emoji_key_enters_the_whole_emoji(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 3u, 1u))); /* emoji */
    ASSERT_EQ(3u, showing(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, emoji_at(&f, 0u)));
    /* All four bytes of it, and nothing chopped in half. */
    ASSERT_STR_EQ("\xF0\x9F\x98\x80",
                  schultz_text_get(f.tree, f.field));
    fixture_teardown(&f);
    PASS();
}

TEST emoji_switched_off_cannot_be_reached(void)
{
    keyboard_fixture f;
    schultz_handle opens;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT(schultz_keyboard_offers_emoji(f.tree, f.keyboard));
    ASSERT_EQ(SCHULTZ_OK, schultz_keyboard_set_emoji(f.tree, f.keyboard, 0));
    ASSERT_FALSE(schultz_keyboard_offers_emoji(f.tree, f.keyboard));

    /* The key that opens them goes with them, or it would sit there doing
     * nothing. */
    opens = key_at(&f, 0u, 3u, 1u);
    ASSERT_FALSE(schultz_node_get_state(f.tree, opens) &
                 SCHULTZ_STATE_VISIBLE);
    ASSERT_EQ(SCHULTZ_OK, press(&f, opens));
    ASSERT_EQ(0u, showing(&f));
    fixture_teardown(&f);
    PASS();
}

TEST a_number_field_is_offered_digits_and_no_emoji(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ((uint32_t)SCHULTZ_INPUT_TEXT,
              schultz_keyboard_input_type(f.tree, f.keyboard));
    ASSERT_EQ(SCHULTZ_OK, schultz_keyboard_set_input_type(f.tree, f.keyboard,
                                                          SCHULTZ_INPUT_NUMBER));
    ASSERT_EQ((uint32_t)SCHULTZ_INPUT_NUMBER,
              schultz_keyboard_input_type(f.tree, f.keyboard));
    ASSERT_EQ(2u, showing(&f));
    ASSERT_FALSE(schultz_keyboard_offers_emoji(f.tree, f.keyboard));
    fixture_teardown(&f);
    PASS();
}

TEST a_masked_field_is_a_password_whether_or_not_it_was_told(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ((uint32_t)SCHULTZ_INPUT_TEXT,
              schultz_text_field_input_type(f.tree, f.field));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_field_set_mask(f.tree, f.field,
                                                      SCHULTZ_PASSWORD_MASK));
    ASSERT_EQ((uint32_t)SCHULTZ_INPUT_PASSWORD,
              schultz_text_field_input_type(f.tree, f.field));
    fixture_teardown(&f);
    PASS();
}

TEST the_window_and_the_field_both_have_to_agree(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    /* The window says no. The field saying yes does not overrule it. */
    ASSERT_EQ(SCHULTZ_OK, schultz_keyboard_set_emoji(f.tree, f.keyboard, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_keyboard_set_input_type(f.tree, f.keyboard,
                                                          SCHULTZ_INPUT_TEXT));
    ASSERT_FALSE(schultz_keyboard_offers_emoji(f.tree, f.keyboard));
    /* And with the window saying yes, the field still decides. */
    ASSERT_EQ(SCHULTZ_OK, schultz_keyboard_set_emoji(f.tree, f.keyboard, 1));
    ASSERT(schultz_keyboard_offers_emoji(f.tree, f.keyboard));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_keyboard_set_input_type(f.tree, f.keyboard,
                                              SCHULTZ_INPUT_PASSWORD));
    ASSERT_FALSE(schultz_keyboard_offers_emoji(f.tree, f.keyboard));
    fixture_teardown(&f);
    PASS();
}

/* ------------------------------- pressed with a pointer, as a finger does */

TEST a_tapped_key_types_into_the_field(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    lay_out(&f);
    ASSERT_EQ(SCHULTZ_OK, tap(&f, key_at(&f, 0u, 0u, 0u))); /* q */
    ASSERT_EQ(SCHULTZ_OK, tap(&f, key_at(&f, 0u, 1u, 0u))); /* a */
    ASSERT_STR_EQ("qa", schultz_text_get(f.tree, f.field));
    fixture_teardown(&f);
    PASS();
}

TEST a_tap_leaves_focus_on_the_field(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    lay_out(&f);
    /*
     * A press decides focus, and a key is not focusable, so without being
     * told otherwise the router would clear focus here. That would end the
     * edit the keyboard exists to serve, and the letter would arrive
     * nowhere.
     */
    ASSERT_EQ(SCHULTZ_OK, tap(&f, key_at(&f, 0u, 0u, 0u)));
    ASSERT_EQ(f.field, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

TEST tapping_the_emoji_key_keeps_the_field_being_edited(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    lay_out(&f);
    ASSERT_EQ(SCHULTZ_OK, tap(&f, key_at(&f, 0u, 3u, 1u))); /* emoji */
    ASSERT_EQ(3u, showing(&f));
    /* Focus still on the field, or a window would take the keyboard away
     * again the moment it opened the emoji. */
    ASSERT_EQ(f.field, schultz_events_focus(f.events));
    lay_out(&f);
    ASSERT_EQ(SCHULTZ_OK, tap(&f, emoji_at(&f, 0u)));
    ASSERT_STR_EQ("\xF0\x9F\x98\x80",
                  schultz_text_get(f.tree, f.field));
    fixture_teardown(&f);
    PASS();
}

TEST tapping_the_numbers_key_keeps_the_field_being_edited(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    lay_out(&f);
    ASSERT_EQ(SCHULTZ_OK, tap(&f, key_at(&f, 0u, 3u, 0u))); /* ?123 */
    ASSERT_EQ(2u, showing(&f));
    ASSERT_EQ(f.field, schultz_events_focus(f.events));
    lay_out(&f);
    ASSERT_EQ(SCHULTZ_OK, tap(&f, key_at(&f, 2u, 0u, 0u))); /* 1 */
    ASSERT_STR_EQ("1", schultz_text_get(f.tree, f.field));
    fixture_teardown(&f);
    PASS();
}

TEST tapping_shift_then_a_letter_gives_one_capital(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    lay_out(&f);
    ASSERT_EQ(SCHULTZ_OK, tap(&f, key_at(&f, 0u, 2u, 0u))); /* shift */
    lay_out(&f);
    ASSERT_EQ(SCHULTZ_OK, tap(&f, key_at(&f, 1u, 0u, 0u))); /* Q */
    lay_out(&f);
    ASSERT_EQ(SCHULTZ_OK, tap(&f, key_at(&f, 0u, 0u, 1u))); /* w */
    ASSERT_STR_EQ("Qw", schultz_text_get(f.tree, f.field));
    ASSERT_EQ(f.field, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

/* The view that scrolls the faces, and how far along it is. */
static schultz_handle emoji_view(keyboard_fixture *f)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle view = SCHULTZ_HANDLE_NONE;

    if (schultz_node_child_at(f->tree, f->keyboard, 3u, &node) != SCHULTZ_OK ||
        schultz_node_child_at(f->tree, node, 0u, &view) != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    return view;
}

static float scrolled_to(keyboard_fixture *f)
{
    return schultz_scroll_bar_value(f->tree,
        schultz_scroll_view_bar(f->tree, emoji_view(f),
                                SCHULTZ_ORIENT_HORIZONTAL));
}

/* Drags from a point, the way a finger or a cursor moves across the faces. */
static void drag_from(keyboard_fixture *f, schultz_point start, float by)
{
    schultz_events_mouse_button(f->events, start, SCHULTZ_BUTTON_LEFT, 1, 0u);
    /* In steps, because that is how a hand moves and how the view measures
     * what it is following. */
    schultz_events_mouse_move(f->events,
        schultz_point_make(start.x + by / 2.0f, start.y), 0u);
    schultz_events_mouse_move(f->events,
        schultz_point_make(start.x + by, start.y), 0u);
    schultz_events_mouse_button(f->events,
        schultz_point_make(start.x + by, start.y), SCHULTZ_BUTTON_LEFT, 0,
        0u);
}

/* Where the middle of one emoji key is, on screen. */
static schultz_point middle_of(keyboard_fixture *f, schultz_handle node)
{
    schultz_rect at;

    if (schultz_node_absolute_bounds(f->tree, node, &at) != SCHULTZ_OK) {
        return schultz_point_make(0.0f, 0.0f);
    }
    return schultz_point_make(at.x + at.width / 2.0f,
                              at.y + at.height / 2.0f);
}

/* A finger, rather than the cursor every other test here uses. */
static int32_t touch_at(keyboard_fixture *f, schultz_point where, int32_t down)
{
    int32_t result = schultz_events_mouse_button(f->events, where,
                                                 SCHULTZ_BUTTON_LEFT, down,
                                                 0u);

    return (result < 0) ? result : SCHULTZ_OK;
}

TEST a_finger_that_swipes_the_faces_enters_none_of_them(void)
{
    keyboard_fixture f;
    schultz_rect at;
    schultz_point start;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 3u, 1u))); /* emoji */
    lay_out(&f);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_pointer_source(f.events,
                                                    SCHULTZ_POINTER_TOUCH));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree,
                                                       emoji_at(&f, 0u), &at));
    start = schultz_point_make(at.x + at.width / 2.0f,
                               at.y + at.height / 2.0f);

    /*
     * Down on a face, then away: that is a swipe along the faces, and every
     * phone treats it as scrolling rather than as choosing the one the
     * finger happened to land on.
     */
    ASSERT_EQ(SCHULTZ_OK, touch_at(&f, start, 1));
    schultz_events_mouse_move(f.events,
        schultz_point_make(start.x - 60.0f, start.y), 0u);
    ASSERT_EQ(SCHULTZ_OK, touch_at(&f,
        schultz_point_make(start.x - 60.0f, start.y), 0));
    ASSERT_STR_EQ("", schultz_text_get(f.tree, f.field));

    /*
     * Down and up without moving is a tap, and enters the face. Back to the
     * start first: the swipe above moved the faces along, so the place the
     * finger was is a different face now.
     */
    schultz_scroll_view_scroll_to(f.tree, emoji_view(&f),
                                  schultz_point_make(0.0f, 0.0f));
    lay_out(&f);
    start = middle_of(&f, emoji_at(&f, 0u));
    ASSERT_EQ(SCHULTZ_OK, touch_at(&f, start, 1));
    ASSERT_EQ(SCHULTZ_OK, touch_at(&f, start, 0));
    ASSERT_STR_EQ("\xF0\x9F\x98\x80", schultz_text_get(f.tree, f.field));
    fixture_teardown(&f);
    PASS();
}

TEST a_finger_dragging_from_a_face_scrolls_the_field(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 3u, 1u))); /* emoji */
    lay_out(&f);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_pointer_source(f.events,
                                                    SCHULTZ_POINTER_TOUCH));
    ASSERT_EQ(0.0f, scrolled_to(&f));

    /*
     * The drag starts on a face rather than between two of them, which is
     * the only way anybody can realistically start one. The keys must let
     * the press through to the view above them, or there is no way to scroll
     * at all.
     */
    drag_from(&f, middle_of(&f, emoji_at(&f, 0u)), -80.0f);
    ASSERT(scrolled_to(&f) > 0.0f);
    ASSERT_STR_EQ("", schultz_text_get(f.tree, f.field));
    fixture_teardown(&f);
    PASS();
}

TEST a_cursor_drags_the_faces_as_a_finger_does(void)
{
    keyboard_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, press(&f, key_at(&f, 0u, 3u, 1u))); /* emoji */
    lay_out(&f);
    ASSERT(schultz_scroll_view_drag_scrolls(f.tree, emoji_view(&f)));

    /* The pointer is a cursor here, which ordinarily does not drag content.
     * Among the faces there is nothing to select and dragging is the whole
     * gesture, so this view is told otherwise. */
    drag_from(&f, middle_of(&f, emoji_at(&f, 0u)), -80.0f);
    ASSERT(scrolled_to(&f) > 0.0f);
    fixture_teardown(&f);
    PASS();
}

TEST missing_a_key_does_not_end_the_edit(void)
{
    keyboard_fixture f;
    schultz_rect first;
    schultz_rect second;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    lay_out(&f);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree,
                                                       key_at(&f, 0u, 0u, 0u),
                                                       &first));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree,
                                                       key_at(&f, 0u, 0u, 1u),
                                                       &second));
    ASSERT(second.x > first.x + first.width); /* there is a gap to aim at */

    /*
     * A finger between two keys has pressed the keyboard rather than a key.
     * Ending the edit there means missing a key by two pixels closes the
     * keyboard and throws the caret away.
     */
    schultz_events_mouse_button(f.events,
        schultz_point_make((first.x + first.width + second.x) / 2.0f,
                           first.y + first.height / 2.0f),
        SCHULTZ_BUTTON_LEFT, 1, 0u);
    ASSERT_EQ(f.field, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

TEST pressing_something_else_still_ends_the_edit(void)
{
    keyboard_fixture f;
    schultz_handle elsewhere = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_label_create(f.tree,
                                               schultz_tree_root(f.tree),
                                               "not a key", &elsewhere));
    schultz_node_set_bounds(f.tree, elsewhere,
                            schultz_rect_make(0, 0, 200, 40));
    lay_out(&f);
    /* The rule the keys are an exception to is still the rule everywhere
     * else: pressing the background is how a person says they are done. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_events_mouse_button(f.events,
                                          schultz_point_make(100.0f, 20.0f),
                                          SCHULTZ_BUTTON_LEFT, 1, 0u));
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

TEST a_set_that_comes_up_asks_to_be_drawn(void)
{
    keyboard_fixture f;
    schultz_rect at;
    schultz_rect dirty;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    lay_out(&f);
    /* A window repaints the region that says it changed and nothing else,
     * so a frame begins with that region cleared. */
    schultz_tree_clear_dirty(f.tree);
    ASSERT_FALSE(schultz_tree_is_dirty(f.tree));

    ASSERT_EQ(SCHULTZ_OK, tap(&f, key_at(&f, 0u, 3u, 1u))); /* emoji */
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);

    /*
     * The set that came up has to be in the region that is about to be
     * painted. Laid out but never marked, it is drawn into a part of the
     * buffer nobody copies, and the keyboard comes up blank with the page
     * showing through where it should be.
     */
    ASSERT(schultz_tree_is_dirty(f.tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(f.tree, &dirty));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree,
                                                       emoji_at(&f, 0u),
                                                       &at));
    ASSERT(at.width > 0.0f);
    ASSERT(at.height > 0.0f);
    ASSERT(dirty.x <= at.x);
    ASSERT(dirty.y <= at.y);
    ASSERT(dirty.x + dirty.width >= at.x + at.width);
    ASSERT(dirty.y + dirty.height >= at.y + at.height);
    fixture_teardown(&f);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_key_types_into_the_focused_field);
    RUN_TEST(a_key_leaves_focus_on_the_field);
    RUN_TEST(backspace_takes_the_last_letter_back);
    RUN_TEST(shift_gives_one_capital_and_then_lower_case_again);
    RUN_TEST(shift_twice_holds_the_capitals_down);
    RUN_TEST(the_numbers_key_reaches_the_digits);
    RUN_TEST(an_emoji_key_enters_the_whole_emoji);
    RUN_TEST(emoji_switched_off_cannot_be_reached);
    RUN_TEST(a_number_field_is_offered_digits_and_no_emoji);
    RUN_TEST(a_masked_field_is_a_password_whether_or_not_it_was_told);
    RUN_TEST(the_window_and_the_field_both_have_to_agree);
    RUN_TEST(a_tapped_key_types_into_the_field);
    RUN_TEST(a_tap_leaves_focus_on_the_field);
    RUN_TEST(tapping_the_emoji_key_keeps_the_field_being_edited);
    RUN_TEST(tapping_the_numbers_key_keeps_the_field_being_edited);
    RUN_TEST(tapping_shift_then_a_letter_gives_one_capital);
    RUN_TEST(a_finger_that_swipes_the_faces_enters_none_of_them);
    RUN_TEST(a_finger_dragging_from_a_face_scrolls_the_field);
    RUN_TEST(a_cursor_drags_the_faces_as_a_finger_does);
    RUN_TEST(missing_a_key_does_not_end_the_edit);
    RUN_TEST(pressing_something_else_still_ends_the_edit);
    RUN_TEST(a_set_that_comes_up_asks_to_be_drawn);
    GREATEST_MAIN_END();
}
