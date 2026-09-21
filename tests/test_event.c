/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_event.c - hit testing, hover, press, focus, capture and overlays.
 *
 * Everything here runs through a fake host callback that records the events
 * it receives. That is the seam the architecture was designed around: routing
 * is asserted by inspecting what a host would have been told, with no window
 * and no input device.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_event.h"
#include "schultz_widget.h"

/* ------------------------------------------------------- fake host */

enum { FAKE_MAX = 64 };

typedef struct {
    uint32_t       types[FAKE_MAX];
    schultz_handle targets[FAKE_MAX];
    uint64_t       tokens[FAKE_MAX];
    schultz_point  locals[FAKE_MAX];
    char           text[FAKE_MAX][32];
    uint32_t       count;
} fake_host;

static int32_t fake_callback(void *context, const schultz_event *event)
{
    fake_host *host = (fake_host *)context;

    if (host->count < FAKE_MAX) {
        host->types[host->count]   = event->type;
        host->targets[host->count] = event->target;
        host->tokens[host->count]  = event->token;
        host->locals[host->count]  = event->local;
        host->text[host->count][0] = '\0';
        if (event->text != NULL) {
            size_t n = strlen(event->text);
            if (n > 31u) { n = 31u; }
            memcpy(host->text[host->count], event->text, n);
            host->text[host->count][n] = '\0';
        }
    }
    host->count++;
    return SCHULTZ_OK;
}

static void fake_reset(fake_host *host)
{
    memset(host, 0, sizeof(*host));
}

/* Index of the first event of a given type, or -1. */
static int32_t fake_find(const fake_host *host, uint32_t type)
{
    uint32_t i;
    for (i = 0; i < host->count && i < FAKE_MAX; i++) {
        if (host->types[i] == type) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t fake_saw(const fake_host *host, uint32_t type,
                        schultz_handle target)
{
    uint32_t i;
    for (i = 0; i < host->count && i < FAKE_MAX; i++) {
        if (host->types[i] == type && host->targets[i] == target) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ fixture */

typedef struct {
    schultz_tree   *tree;
    schultz_events *events;
    fake_host       host;
    schultz_handle  panel;   /* 100,100 200x200 */
    schultz_handle  button;  /* 20,20 60x30 inside the panel */
} fixture;

static int32_t fixture_setup(fixture *f)
{
    int32_t result = schultz_tree_create(&f->tree);

    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(f->tree, schultz_rect_make(0, 0, 800, 600));

    result = schultz_events_create(f->tree, &f->events);
    if (result != SCHULTZ_OK) {
        return result;
    }
    fake_reset(&f->host);
    schultz_events_set_callback(f->events, fake_callback, &f->host);

    result = schultz_node_create(f->tree, schultz_tree_root(f->tree),
                                 &f->panel);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_set_bounds(f->tree, f->panel,
                            schultz_rect_make(100, 100, 200, 200));
    schultz_node_set_token(f->tree, f->panel, 111u);

    result = schultz_node_create(f->tree, f->panel, &f->button);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_set_bounds(f->tree, f->button,
                            schultz_rect_make(20, 20, 60, 30));
    schultz_node_set_token(f->tree, f->button, 222u);
    schultz_node_set_actions(f->tree, f->button,
                             SCHULTZ_ACTION_CLICK | SCHULTZ_ACTION_FOCUS);

    fake_reset(&f->host);
    return SCHULTZ_OK;
}

static void fixture_teardown(fixture *f)
{
    schultz_events_destroy(f->events);
    schultz_tree_destroy(f->tree);
}

/* -------------------------------------------------------- hit testing */

TEST hit_test_finds_the_deepest_node(void)
{
    fixture f;
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));

    /* Inside the button, which sits at absolute 120,120 to 180,150. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(f.events,
                                        schultz_point_make(130, 130), &hit));
    ASSERT_EQ(f.button, hit);

    /* Inside the panel but outside the button. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(f.events,
                                        schultz_point_make(250, 250), &hit));
    ASSERT_EQ(f.panel, hit);

    /* Outside everything but the root, which covers nothing by default. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(f.events,
                                        schultz_point_make(700, 500), &hit));
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, hit);

    fixture_teardown(&f);
    PASS();
}

/*
 * Clipping decides reachability, and only clipping does.
 *
 * The two halves of this rule pull in opposite directions, so both are here.
 * A child hanging outside a plain container is still drawn and still has to
 * be clickable. A child hanging outside a container that clips is not drawn
 * at all, and must not be clickable, or it steals presses meant for whatever
 * is drawn where it happens to lie.
 */
TEST clipping_decides_whether_a_child_outside_can_be_hit(void)
{
    fixture f;
    schultz_handle box = SCHULTZ_HANDLE_NONE;
    schultz_handle outside = SCHULTZ_HANDLE_NONE;
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree,
                              schultz_tree_root(f.tree), &box));
    schultz_node_set_bounds(f.tree, box, schultz_rect_make(400, 400, 50, 50));

    /* A child whose bounds fall well outside its parent. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree, box, &outside));
    schultz_node_set_bounds(f.tree, outside,
                            schultz_rect_make(100, 100, 40, 40));

    /* Not clipping: the child overflows and stays reachable, which is what
     * a tooltip or a focus ring hanging off its owner relies on. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(f.events,
                                        schultz_point_make(520, 520), &hit));
    ASSERT_EQ(outside, hit);

    /* Clipping: the same child is drawn nowhere, so it answers nowhere. */
    schultz_node_set_clips_children(f.tree, box, 1);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(f.events,
                                        schultz_point_make(520, 520), &hit));
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, hit);

    /* And a point inside the clipping parent still reaches it. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(f.events,
                                        schultz_point_make(420, 420), &hit));
    ASSERT_EQ(box, hit);

    fixture_teardown(&f);
    PASS();
}

TEST hit_test_prefers_the_last_child(void)
{
    fixture f;
    schultz_handle under;
    schultz_handle over;
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree, f.panel, &under));
    schultz_node_set_bounds(f.tree, under, schultz_rect_make(0, 0, 100, 100));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree, f.panel, &over));
    schultz_node_set_bounds(f.tree, over, schultz_rect_make(0, 0, 100, 100));

    /* Both cover the point; the later child paints on top and wins. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(f.events,
                                        schultz_point_make(150, 150), &hit));
    ASSERT_EQ(over, hit);

    fixture_teardown(&f);
    PASS();
}

TEST hit_test_skips_hidden_subtrees(void)
{
    fixture f;
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_state(f.tree, f.panel,
                                                 SCHULTZ_STATE_ENABLED));

    /* Hiding the panel hides the button inside it too. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(f.events,
                                        schultz_point_make(130, 130), &hit));
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, hit);

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------ hover routing */

TEST moving_over_a_node_sends_enter_and_sets_the_flag(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_mouse_move(f.events,
                                        schultz_point_make(130, 130), 0));

    ASSERT_EQ(f.button, schultz_events_hovered(f.events));
    ASSERT(fake_saw(&f.host, SCHULTZ_EVENT_MOUSE_ENTER, f.button));
    ASSERT(schultz_node_get_state(f.tree, f.button) & SCHULTZ_STATE_HOVERED);

    fixture_teardown(&f);
    PASS();
}

TEST moving_away_sends_leave_before_enter(void)
{
    fixture f;
    int32_t leave;
    int32_t enter;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_mouse_move(f.events, schultz_point_make(130, 130), 0);
    fake_reset(&f.host);

    /* From the button out to the panel. */
    schultz_events_mouse_move(f.events, schultz_point_make(250, 250), 0);

    leave = fake_find(&f.host, SCHULTZ_EVENT_MOUSE_LEAVE);
    enter = fake_find(&f.host, SCHULTZ_EVENT_MOUSE_ENTER);
    ASSERT(leave >= 0);
    ASSERT(enter >= 0);
    ASSERT(leave < enter); /* the old node hears first */
    ASSERT_EQ(f.button, f.host.targets[leave]);
    ASSERT_EQ(f.panel, f.host.targets[enter]);

    ASSERT_EQ(0u, schultz_node_get_state(f.tree, f.button) &
                      SCHULTZ_STATE_HOVERED);
    ASSERT(schultz_node_get_state(f.tree, f.panel) & SCHULTZ_STATE_HOVERED);

    fixture_teardown(&f);
    PASS();
}

TEST staying_on_one_node_does_not_re_enter(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_mouse_move(f.events, schultz_point_make(130, 130), 0);
    fake_reset(&f.host);

    schultz_events_mouse_move(f.events, schultz_point_make(135, 135), 0);
    ASSERT_EQ(-1, fake_find(&f.host, SCHULTZ_EVENT_MOUSE_ENTER));
    ASSERT_EQ(-1, fake_find(&f.host, SCHULTZ_EVENT_MOUSE_LEAVE));
    ASSERT(fake_find(&f.host, SCHULTZ_EVENT_MOUSE_MOVE) >= 0);

    fixture_teardown(&f);
    PASS();
}

TEST the_event_carries_the_token_and_local_coordinates(void)
{
    fixture f;
    int32_t index;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_mouse_move(f.events, schultz_point_make(130, 140), 0);

    index = fake_find(&f.host, SCHULTZ_EVENT_MOUSE_ENTER);
    ASSERT(index >= 0);
    ASSERT_EQ(222u, f.host.tokens[index]);
    /* The button is at absolute 120,120, so 130,140 is 10,20 inside it. */
    ASSERT_EQ(10.0f, f.host.locals[index].x);
    ASSERT_EQ(20.0f, f.host.locals[index].y);

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------ press and click */

TEST press_and_release_on_the_same_node_is_a_click(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    ASSERT_EQ(f.button, schultz_events_captured(f.events));
    ASSERT(schultz_node_get_state(f.tree, f.button) & SCHULTZ_STATE_PRESSED);

    schultz_events_mouse_button(f.events, schultz_point_make(135, 135),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    ASSERT(fake_saw(&f.host, SCHULTZ_EVENT_MOUSE_DOWN, f.button));
    ASSERT(fake_saw(&f.host, SCHULTZ_EVENT_MOUSE_UP, f.button));
    ASSERT(fake_saw(&f.host, SCHULTZ_EVENT_CLICK, f.button));
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_events_captured(f.events));
    ASSERT_EQ(0u, schultz_node_get_state(f.tree, f.button) &
                      SCHULTZ_STATE_PRESSED);

    fixture_teardown(&f);
    PASS();
}

/*
 * Dragging off a button and releasing must not activate it. The release still
 * goes to the pressed node, but no click is produced.
 */
TEST releasing_elsewhere_is_not_a_click(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    fake_reset(&f.host);

    schultz_events_mouse_button(f.events, schultz_point_make(700, 500),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    ASSERT(fake_saw(&f.host, SCHULTZ_EVENT_MOUSE_UP, f.button));
    ASSERT_EQ(-1, fake_find(&f.host, SCHULTZ_EVENT_CLICK));

    fixture_teardown(&f);
    PASS();
}

TEST motion_while_pressed_is_a_drag_on_the_captured_node(void)
{
    fixture f;
    int32_t index;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    fake_reset(&f.host);

    /* Far outside the button, but capture keeps the events on it. */
    schultz_events_mouse_move(f.events, schultz_point_make(700, 500), 0);

    index = fake_find(&f.host, SCHULTZ_EVENT_DRAG);
    ASSERT(index >= 0);
    ASSERT_EQ(f.button, f.host.targets[index]);
    /* And no hover churn while captured. */
    ASSERT_EQ(-1, fake_find(&f.host, SCHULTZ_EVENT_MOUSE_LEAVE));

    fixture_teardown(&f);
    PASS();
}

TEST scroll_goes_to_the_node_under_the_pointer(void)
{
    fixture f;
    int32_t index;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_scroll(f.events, schultz_point_make(250, 250), 0.0f,
                          -3.0f);

    index = fake_find(&f.host, SCHULTZ_EVENT_SCROLL);
    ASSERT(index >= 0);
    ASSERT_EQ(f.panel, f.host.targets[index]);

    fixture_teardown(&f);
    PASS();
}

/* --------------------------------------------------------------- focus */

TEST focus_emits_lost_then_gained(void)
{
    fixture f;
    int32_t lost;
    int32_t gained;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_node_set_actions(f.tree, f.panel, SCHULTZ_ACTION_FOCUS);

    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.panel));
    fake_reset(&f.host);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.button));

    lost   = fake_find(&f.host, SCHULTZ_EVENT_FOCUS_LOST);
    gained = fake_find(&f.host, SCHULTZ_EVENT_FOCUS_GAINED);
    ASSERT(lost >= 0 && gained >= 0);
    ASSERT(lost < gained);
    ASSERT_EQ(f.panel, f.host.targets[lost]);
    ASSERT_EQ(f.button, f.host.targets[gained]);

    ASSERT_EQ(f.button, schultz_events_focus(f.events));
    ASSERT(schultz_node_get_state(f.tree, f.button) & SCHULTZ_STATE_FOCUSED);
    ASSERT_EQ(0u, schultz_node_get_state(f.tree, f.panel) &
                      SCHULTZ_STATE_FOCUSED);

    fixture_teardown(&f);
    PASS();
}

TEST pressing_a_focusable_node_focuses_it(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_events_focus(f.events));

    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    ASSERT_EQ(f.button, schultz_events_focus(f.events));

    fixture_teardown(&f);
    PASS();
}

TEST focus_moves_in_tree_order_and_wraps(void)
{
    fixture f;
    schultz_handle a;
    schultz_handle b;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    /* Tree order: panel, button, a, b. Only focusables take part. */
    schultz_node_set_actions(f.tree, f.panel, SCHULTZ_ACTION_FOCUS);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree, f.panel, &a));
    schultz_node_set_actions(f.tree, a, SCHULTZ_ACTION_FOCUS);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree, f.panel, &b));
    schultz_node_set_actions(f.tree, b, SCHULTZ_ACTION_FOCUS);

    ASSERT_EQ(SCHULTZ_OK, schultz_events_focus_move(f.events, 1));
    ASSERT_EQ(f.panel, schultz_events_focus(f.events));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_focus_move(f.events, 1));
    ASSERT_EQ(f.button, schultz_events_focus(f.events));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_focus_move(f.events, 1));
    ASSERT_EQ(a, schultz_events_focus(f.events));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_focus_move(f.events, 1));
    ASSERT_EQ(b, schultz_events_focus(f.events));

    /* Past the last, back to the first. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_focus_move(f.events, 1));
    ASSERT_EQ(f.panel, schultz_events_focus(f.events));

    /* And backwards wraps the other way. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_focus_move(f.events, 0));
    ASSERT_EQ(b, schultz_events_focus(f.events));

    fixture_teardown(&f);
    PASS();
}

TEST focus_skips_disabled_and_hidden_nodes(void)
{
    fixture f;
    schultz_handle disabled;
    schultz_handle hidden;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree, f.panel, &disabled));
    schultz_node_set_actions(f.tree, disabled, SCHULTZ_ACTION_FOCUS);
    schultz_node_set_state(f.tree, disabled, SCHULTZ_STATE_VISIBLE);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree, f.panel, &hidden));
    schultz_node_set_actions(f.tree, hidden, SCHULTZ_ACTION_FOCUS);
    schultz_node_set_state(f.tree, hidden, SCHULTZ_STATE_ENABLED);

    /* Only the button qualifies, so moving forward twice stays on it. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_focus_move(f.events, 1));
    ASSERT_EQ(f.button, schultz_events_focus(f.events));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_focus_move(f.events, 1));
    ASSERT_EQ(f.button, schultz_events_focus(f.events));

    fixture_teardown(&f);
    PASS();
}

TEST focus_move_reports_exhausted_when_nothing_is_focusable(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_node_set_actions(f.tree, f.button, SCHULTZ_ACTION_CLICK);
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED, schultz_events_focus_move(f.events, 1));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------ keyboard */

TEST keys_and_text_go_to_the_focused_node(void)
{
    fixture f;
    int32_t index;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_set_focus(f.events, f.button);
    fake_reset(&f.host);

    ASSERT_EQ(SCHULTZ_OK, schultz_events_key(f.events, 65u, SCHULTZ_MOD_CTRL,
                                             1));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_text_input(f.events, "hi"));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_text_editing(f.events, "composing",
                                                      3));

    ASSERT(fake_saw(&f.host, SCHULTZ_EVENT_KEY_DOWN, f.button));
    index = fake_find(&f.host, SCHULTZ_EVENT_TEXT_INPUT);
    ASSERT(index >= 0);
    ASSERT_STR_EQ("hi", f.host.text[index]);
    index = fake_find(&f.host, SCHULTZ_EVENT_TEXT_EDITING);
    ASSERT(index >= 0);
    ASSERT_STR_EQ("composing", f.host.text[index]);

    fixture_teardown(&f);
    PASS();
}

TEST keys_go_nowhere_when_nothing_has_focus(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_key(f.events, 65u, 0, 1));
    ASSERT_EQ(0u, f.host.count);

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------ overlays */

TEST overlays_are_hit_tested_before_the_content_tree(void)
{
    fixture f;
    schultz_handle menu;
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree,
                                              schultz_tree_root(f.tree),
                                              &menu));
    /* Deliberately over the button. */
    schultz_node_set_bounds(f.tree, menu, schultz_rect_make(100, 100, 100,
                                                            100));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_push_overlay(f.tree, menu, 1));

    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(f.events,
                                        schultz_point_make(130, 130), &hit));
    ASSERT_EQ(menu, hit);

    fixture_teardown(&f);
    PASS();
}

TEST the_topmost_overlay_wins(void)
{
    fixture f;
    schultz_handle lower;
    schultz_handle upper;
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree,
                                              schultz_tree_root(f.tree),
                                              &lower));
    schultz_node_set_bounds(f.tree, lower, schultz_rect_make(0, 0, 400, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree,
                                              schultz_tree_root(f.tree),
                                              &upper));
    schultz_node_set_bounds(f.tree, upper, schultz_rect_make(0, 0, 400, 400));
    schultz_tree_push_overlay(f.tree, lower, 0);
    schultz_tree_push_overlay(f.tree, upper, 0);

    ASSERT_EQ(2u, schultz_tree_overlay_count(f.tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(f.events,
                                        schultz_point_make(50, 50), &hit));
    ASSERT_EQ(upper, hit);

    fixture_teardown(&f);
    PASS();
}

/*
 * Input capture. Clicking away from a menu must close it and must not also
 * activate whatever sits underneath.
 */
TEST pressing_outside_a_capturing_overlay_dismisses_and_consumes(void)
{
    fixture f;
    schultz_handle menu;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree,
                                              schultz_tree_root(f.tree),
                                              &menu));
    schultz_node_set_bounds(f.tree, menu, schultz_rect_make(400, 400, 100,
                                                            100));
    schultz_node_set_token(f.tree, menu, 999u);
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_push_overlay(f.tree, menu, 1));

    /* Press on the button, which is outside the menu. */
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0);

    ASSERT(fake_saw(&f.host, SCHULTZ_EVENT_DISMISS, menu));
    /* The button underneath heard nothing. */
    ASSERT_EQ(-1, fake_find(&f.host, SCHULTZ_EVENT_MOUSE_DOWN));
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_events_captured(f.events));

    fixture_teardown(&f);
    PASS();
}

TEST pressing_inside_a_capturing_overlay_is_delivered_normally(void)
{
    fixture f;
    schultz_handle menu;
    schultz_handle item;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree,
                                              schultz_tree_root(f.tree),
                                              &menu));
    schultz_node_set_bounds(f.tree, menu, schultz_rect_make(400, 400, 100,
                                                            100));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree, menu, &item));
    schultz_node_set_bounds(f.tree, item, schultz_rect_make(0, 0, 100, 20));
    schultz_tree_push_overlay(f.tree, menu, 1);

    schultz_events_mouse_button(f.events, schultz_point_make(450, 405),
                                SCHULTZ_BUTTON_LEFT, 1, 0);

    ASSERT_EQ(-1, fake_find(&f.host, SCHULTZ_EVENT_DISMISS));
    ASSERT(fake_saw(&f.host, SCHULTZ_EVENT_MOUSE_DOWN, item));

    fixture_teardown(&f);
    PASS();
}

TEST a_non_capturing_overlay_lets_presses_through(void)
{
    fixture f;
    schultz_handle tooltip;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree,
                                              schultz_tree_root(f.tree),
                                              &tooltip));
    schultz_node_set_bounds(f.tree, tooltip, schultz_rect_make(400, 400, 50,
                                                               50));
    schultz_tree_push_overlay(f.tree, tooltip, 0);

    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0);

    ASSERT_EQ(-1, fake_find(&f.host, SCHULTZ_EVENT_DISMISS));
    ASSERT(fake_saw(&f.host, SCHULTZ_EVENT_MOUSE_DOWN, f.button));

    fixture_teardown(&f);
    PASS();
}

TEST popping_and_destroying_overlays_keeps_the_list_correct(void)
{
    fixture f;
    schultz_handle a;
    schultz_handle b;
    schultz_handle popped = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree,
                                              schultz_tree_root(f.tree), &a));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree,
                                              schultz_tree_root(f.tree), &b));
    schultz_tree_push_overlay(f.tree, a, 1);
    schultz_tree_push_overlay(f.tree, b, 1);
    ASSERT_EQ(2u, schultz_tree_overlay_count(f.tree));

    /* Registering the same node twice is refused. */
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED, schultz_tree_push_overlay(f.tree, b, 1));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_pop_overlay(f.tree, &popped));
    ASSERT_EQ(b, popped);
    ASSERT_EQ(1u, schultz_tree_overlay_count(f.tree));

    /* Destroying an overlay must remove it from the layer list. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_destroy(f.tree, a));
    ASSERT_EQ(0u, schultz_tree_overlay_count(f.tree));
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED, schultz_tree_pop_overlay(f.tree, NULL));

    fixture_teardown(&f);
    PASS();
}

TEST routing_without_a_callback_still_updates_state(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_callback(f.events, NULL, NULL));

    schultz_events_mouse_move(f.events, schultz_point_make(130, 130), 0);
    ASSERT_EQ(f.button, schultz_events_hovered(f.events));
    ASSERT_EQ(0u, f.host.count);

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------- pointer transparency */

TEST a_node_that_is_not_hit_testable_is_skipped(void)
{
    fixture f;
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(1, schultz_node_hit_testable(f.tree, f.button));

    schultz_events_hit_test(f.events, schultz_point_make(130, 130), &hit);
    ASSERT_EQ(f.button, hit);

    /* The pointer now falls through to whatever is behind it. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_hit_testable(f.tree, f.button, 0));
    ASSERT_EQ(0, schultz_node_hit_testable(f.tree, f.button));
    schultz_events_hit_test(f.events, schultz_point_make(130, 130), &hit);
    ASSERT_EQ(f.panel, hit);

    fixture_teardown(&f);
    PASS();
}

TEST children_of_a_transparent_node_are_still_reachable(void)
{
    fixture f;
    schultz_handle inner = SCHULTZ_HANDLE_NONE;
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree, f.button, &inner));
    schultz_node_set_bounds(f.tree, inner, schultz_rect_make(0, 0, 10, 10));
    schultz_node_set_hit_testable(f.tree, f.button, 0);

    /* Inside the inner node, which is inside the transparent one. */
    schultz_events_hit_test(f.events, schultz_point_make(125, 125), &hit);
    ASSERT_EQ(inner, hit);

    fixture_teardown(&f);
    PASS();
}

TEST the_flag_rejects_a_stale_handle(void)
{
    fixture f;
    schultz_handle gone;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree, f.panel, &gone));
    schultz_node_destroy(f.tree, gone);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_set_hit_testable(f.tree, gone, 0));
    ASSERT_EQ(0, schultz_node_hit_testable(f.tree, gone));

    fixture_teardown(&f);
    PASS();
}


TEST performing_an_action_is_the_same_as_doing_it_by_hand(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));

    /*
     * An assistive technology asking the button to press has to arrive as the
     * event a real press produces. If the two took different paths they would
     * drift, and the accessible path is the one nobody notices breaking.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_perform(f.events, f.button,
                                                 SCHULTZ_ACTION_CLICK));
    ASSERT_EQ(1u, f.host.count);
    ASSERT_EQ(SCHULTZ_EVENT_CLICK, f.host.types[0]);
    ASSERT_EQ(f.button, f.host.targets[0]);
    ASSERT_EQ(222u, f.host.tokens[0]);

    ASSERT_EQ(SCHULTZ_OK, schultz_events_perform(f.events, f.button,
                                                 SCHULTZ_ACTION_FOCUS));
    ASSERT_EQ(f.button, schultz_events_focus(f.events));

    fixture_teardown(&f);
    PASS();
}

TEST an_action_a_node_never_offered_is_refused(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));

    /* The button declares click and focus, and nothing else. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_events_perform(f.events, f.button,
                                     SCHULTZ_ACTION_INCREMENT));
    ASSERT_EQ(0u, f.host.count);

    /* The panel declares nothing at all. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_events_perform(f.events, f.panel,
                                     SCHULTZ_ACTION_CLICK));
    ASSERT_EQ(0u, f.host.count);

    /* A handle that no longer resolves declares nothing either. */
    schultz_node_destroy(f.tree, f.button);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_events_perform(f.events, f.button,
                                     SCHULTZ_ACTION_CLICK));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_events_perform(NULL, f.panel, SCHULTZ_ACTION_CLICK));

    fixture_teardown(&f);
    PASS();
}

TEST the_router_lets_go_of_a_node_taken_out_of_the_tree(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));

    /* Focused, hovered and pressed, all on the button. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.button));
    schultz_events_mouse_move(f.events, schultz_point_make(150, 140), 0);
    schultz_events_mouse_button(f.events, schultz_point_make(150, 140),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    ASSERT_EQ(f.button, schultz_events_focus(f.events));
    ASSERT_EQ(f.button, schultz_events_hovered(f.events));
    ASSERT_EQ(f.button, schultz_events_captured(f.events));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_parent(f.tree, f.button, SCHULTZ_HANDLE_NONE));

    /* All three answer nothing, without waiting for the next event. */
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_events_focus(f.events));
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_events_hovered(f.events));
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_events_captured(f.events));

    /* And the flags went with them, so putting it back is not pre-lit. */
    fake_reset(&f.host);
    schultz_events_key(f.events, SCHULTZ_KEY_SPACE, 0, 1);
    ASSERT_EQ(0u, schultz_node_get_state(f.tree, f.button) &
                      (SCHULTZ_STATE_FOCUSED | SCHULTZ_STATE_HOVERED |
                       SCHULTZ_STATE_PRESSED));
    ASSERT_EQ(0, fake_saw(&f.host, SCHULTZ_EVENT_CLICK, f.button));

    fixture_teardown(&f);
    PASS();
}

TEST a_node_out_of_the_tree_cannot_be_focused_or_performed(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_parent(f.tree, f.button, SCHULTZ_HANDLE_NONE));

    /* It still declares the actions; being out of the tree is what stops it. */
    ASSERT(schultz_node_get_actions(f.tree, f.button) & SCHULTZ_ACTION_CLICK);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_events_perform(f.events, f.button,
                                     SCHULTZ_ACTION_CLICK));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_events_set_focus(f.events, f.button));
    ASSERT_EQ(0, fake_saw(&f.host, SCHULTZ_EVENT_CLICK, f.button));

    /* Back in the tree it works again. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_parent(f.tree, f.button, f.panel));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_events_perform(f.events, f.button,
                                     SCHULTZ_ACTION_CLICK));
    ASSERT_EQ(1, fake_saw(&f.host, SCHULTZ_EVENT_CLICK, f.button));

    fixture_teardown(&f);
    PASS();
}

/* Turns a node off, leaving its other state flags alone. */
static void set_disabled(fixture *f, schultz_handle node, int32_t off)
{
    uint32_t state = schultz_node_get_state(f->tree, node);

    schultz_node_set_state(f->tree, node,
        off ? (state & ~(uint32_t)SCHULTZ_STATE_ENABLED)
            : (state | SCHULTZ_STATE_ENABLED));
}

TEST a_press_on_nothing_focusable_takes_focus_away(void)
{
    fixture f;
    schultz_handle label = SCHULTZ_HANDLE_NONE;

    /*
     * A press decides where focus is rather than only being able to move it.
     * This is what closes an on-screen keyboard: the keyboard follows focus
     * and nothing manages it separately, so a field that never lost focus
     * never let it go, and the only way to shut it was to press something
     * else focusable.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.button));
    ASSERT_EQ(f.button, schultz_events_focus(f.events));

    /* The panel holds the button and is not focusable itself. */
    schultz_events_mouse_button(f.events, schultz_point_make(110, 110),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_events_focus(f.events));
    schultz_events_mouse_button(f.events, schultz_point_make(110, 110),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    /* A node with no widget at all, outside everything. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.button));
    schultz_events_mouse_button(f.events, schultz_point_make(700, 500),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_events_focus(f.events));
    schultz_events_mouse_button(f.events, schultz_point_make(700, 500),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    /* And a press on something focusable still moves focus to it. */
    schultz_events_mouse_button(f.events, schultz_point_make(150, 140),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    ASSERT_EQ(f.button, schultz_events_focus(f.events));
    schultz_events_mouse_button(f.events, schultz_point_make(150, 140),
                                SCHULTZ_BUTTON_LEFT, 0, 0);

    /* Losing focus this way is announced, so a host hears about it. */
    fake_reset(&f.host);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree, f.panel, &label));
    schultz_events_mouse_button(f.events, schultz_point_make(110, 110),
                                SCHULTZ_BUTTON_LEFT, 1, 0);
    ASSERT_EQ(1, fake_saw(&f.host, SCHULTZ_EVENT_FOCUS_LOST, f.button));

    fixture_teardown(&f);
    PASS();
}

TEST a_right_press_leaves_focus_alone(void)
{
    fixture f;

    /* The secondary button asks for a context menu rather than pressing what
     * is under it, so it does not decide focus either. */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.button));
    schultz_events_mouse_button(f.events, schultz_point_make(110, 110),
                                SCHULTZ_BUTTON_RIGHT, 1, 0);
    ASSERT_EQ(f.button, schultz_events_focus(f.events));

    fixture_teardown(&f);
    PASS();
}

TEST a_disabled_node_stops_the_pointer_and_does_nothing_with_it(void)
{
    fixture f;
    schultz_handle hit = SCHULTZ_HANDLE_NONE;
    schultz_point at = schultz_point_make(150, 140);

    /*
     * A plain button has no behaviour of its own to suppress: reporting the
     * click is its whole job, and the report comes from the router. So the
     * router is where a disabled one has to be stopped.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    set_disabled(&f, f.button, 1);

    /* Still the hit, so nothing behind it is reached. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_hit_test(f.events, at, &hit));
    ASSERT_EQ(f.button, hit);

    fake_reset(&f.host);
    schultz_events_mouse_move(f.events, at, 0);
    schultz_events_mouse_button(f.events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_button(f.events, at, SCHULTZ_BUTTON_LEFT, 0, 0);

    ASSERT_EQ(0, fake_saw(&f.host, SCHULTZ_EVENT_CLICK, f.button));
    ASSERT_EQ(0, fake_saw(&f.host, SCHULTZ_EVENT_MOUSE_DOWN, f.button));
    ASSERT_EQ(0u, schultz_node_get_state(f.tree, f.button) &
                      (SCHULTZ_STATE_HOVERED | SCHULTZ_STATE_PRESSED));
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_events_hovered(f.events));

    /* And it works again when it is turned back on. */
    set_disabled(&f, f.button, 0);
    fake_reset(&f.host);
    schultz_events_mouse_move(f.events, at, 0);
    schultz_events_mouse_button(f.events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_button(f.events, at, SCHULTZ_BUTTON_LEFT, 0, 0);
    ASSERT_EQ(1, fake_saw(&f.host, SCHULTZ_EVENT_CLICK, f.button));

    fixture_teardown(&f);
    PASS();
}

TEST disabled_is_inherited_from_whatever_is_above(void)
{
    fixture f;
    schultz_point at = schultz_point_make(150, 140);

    /* Turning a panel off turns off everything on it, or a form disabled
     * while it saves still answers every button in it. */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT(schultz_node_is_enabled(f.tree, f.button));
    set_disabled(&f, f.panel, 1);
    ASSERT_FALSE(schultz_node_is_enabled(f.tree, f.button));

    fake_reset(&f.host);
    schultz_events_mouse_move(f.events, at, 0);
    schultz_events_mouse_button(f.events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_button(f.events, at, SCHULTZ_BUTTON_LEFT, 0, 0);
    ASSERT_EQ(0, fake_saw(&f.host, SCHULTZ_EVENT_CLICK, f.button));

    fixture_teardown(&f);
    PASS();
}

TEST a_node_disabled_after_it_was_focused_gives_focus_up(void)
{
    fixture f;

    /*
     * Tab never reaches a disabled control, but one that already held focus
     * when it was turned off keeps it, and the space bar would press it.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.button));
    ASSERT_EQ(f.button, schultz_events_focus(f.events));

    set_disabled(&f, f.button, 1);
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_events_focus(f.events));

    fake_reset(&f.host);
    schultz_events_key(f.events, SCHULTZ_KEY_SPACE, 0, 1);
    ASSERT_EQ(0, fake_saw(&f.host, SCHULTZ_EVENT_CLICK, f.button));

    /* Nor can it be given focus while it is off. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_events_set_focus(f.events, f.button));

    fixture_teardown(&f);
    PASS();
}

TEST a_disabled_node_cannot_be_activated_through_accessibility(void)
{
    fixture f;

    /*
     * The accessibility layer already tells the platform when a node is
     * disabled, so a screen reader announces it as unavailable. Without this
     * it could then press it, and the two halves of the same conversation
     * would disagree.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    set_disabled(&f, f.button, 1);

    fake_reset(&f.host);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_events_perform(f.events, f.button,
                                     SCHULTZ_ACTION_CLICK));
    ASSERT_EQ(0, fake_saw(&f.host, SCHULTZ_EVENT_CLICK, f.button));

    set_disabled(&f, f.button, 0);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_events_perform(f.events, f.button,
                                     SCHULTZ_ACTION_CLICK));
    ASSERT_EQ(1, fake_saw(&f.host, SCHULTZ_EVENT_CLICK, f.button));

    fixture_teardown(&f);
    PASS();
}

/*
 * The two halves of keeping focus. A control that acts on whatever is being
 * edited has to stop both of them: taking focus, and clearing it.
 */

TEST a_node_that_keeps_focus_does_not_take_it_either(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_node_set_actions(f.tree, f.panel, SCHULTZ_ACTION_FOCUS);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.panel));

    /* Without the flag, pressing a focusable node moves focus to it. That is
     * the ordinary rule and it stays. */
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0u);
    ASSERT_EQ(f.button, schultz_events_focus(f.events));
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 0, 0u);

    /*
     * With it, the press leaves focus alone even though the node could hold
     * it. This is a toolbar's Bold button over a text area: reachable by tab
     * like any control, and pressing it must leave the caret in the text it
     * is about to embolden.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.panel));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_keeps_focus(f.tree, f.button, 1));
    ASSERT(schultz_node_keeps_focus(f.tree, f.button));
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0u);
    ASSERT_EQ(f.panel, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

TEST a_finger_that_drags_off_a_field_does_not_focus_it(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_pointer_source(f.events,
                                                    SCHULTZ_POINTER_TOUCH));
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_events_focus(f.events));

    /*
     * Down on something focusable, then away: that is the page being
     * scrolled, and focusing what the finger happened to start on would put
     * a keyboard over whatever the person was reading.
     */
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0u);
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_events_focus(f.events));
    schultz_events_mouse_move(f.events, schultz_point_make(130, 220), 0u);
    schultz_events_mouse_button(f.events, schultz_point_make(130, 220),
                                SCHULTZ_BUTTON_LEFT, 0, 0u);
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_events_focus(f.events));

    /* A finger that stays still has tapped, and a tap is a choice. */
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0u);
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 0, 0u);
    ASSERT_EQ(f.button, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

TEST a_cursor_still_focuses_on_the_way_down(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    /*
     * A cursor decides on the press, which is what a desktop does and what
     * dragging from a field to select text needs: the caret has to land
     * before the drag can extend anything.
     */
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0u);
    ASSERT_EQ(f.button, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

TEST a_finger_tapping_the_background_still_ends_the_edit(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.button));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_pointer_source(f.events,
                                                    SCHULTZ_POINTER_TOUCH));
    /* Tapping something that cannot hold focus is how a person says they
     * are done, and it has to keep working now that the answer comes on the
     * release. */
    schultz_events_mouse_button(f.events, schultz_point_make(290, 290),
                                SCHULTZ_BUTTON_LEFT, 1, 0u);
    schultz_events_mouse_button(f.events, schultz_point_make(290, 290),
                                SCHULTZ_BUTTON_LEFT, 0, 0u);
    ASSERT_EQ(SCHULTZ_HANDLE_NONE, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

TEST a_press_between_children_keeps_focus_too(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_node_set_actions(f.tree, f.panel, SCHULTZ_ACTION_FOCUS);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.panel));

    /*
     * The panel keeps focus. The press lands on a child of it that can
     * neither hold focus nor keeps it -- a key cap, or the gap between two
     * keys -- so the answer has to come from the panel above it.
     */
    schultz_node_set_actions(f.tree, f.panel, 0u);
    schultz_node_set_actions(f.tree, f.button, SCHULTZ_ACTION_CLICK);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_keeps_focus(f.tree, f.panel, 1));
    ASSERT_FALSE(schultz_node_keeps_focus(f.tree, f.button));

    /* Somewhere else holds focus, and a press inside the panel must leave
     * it there. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.panel));
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0u);
    ASSERT_EQ(f.panel, schultz_events_focus(f.events));
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 0, 0u);

    /* And so must one that lands on the panel itself, between its children. */
    schultz_events_mouse_button(f.events, schultz_point_make(290, 290),
                                SCHULTZ_BUTTON_LEFT, 1, 0u);
    ASSERT_EQ(f.panel, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

TEST a_child_that_can_hold_focus_still_takes_it(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    /* The container keeps focus; the field inside it is focusable. Pressing
     * the field has to put the caret in it, or a form inside such a
     * container could never be filled in. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_keeps_focus(f.tree, f.panel, 1));
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0u);
    ASSERT_EQ(f.button, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

TEST a_node_that_keeps_focus_does_not_clear_it_either(void)
{
    fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_node_set_actions(f.tree, f.panel, SCHULTZ_ACTION_FOCUS);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, f.panel));

    /* The other half, on a node that cannot hold focus at all: a key on the
     * on-screen keyboard. */
    schultz_node_set_actions(f.tree, f.button, SCHULTZ_ACTION_CLICK);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_keeps_focus(f.tree, f.button, 1));
    schultz_events_mouse_button(f.events, schultz_point_make(130, 130),
                                SCHULTZ_BUTTON_LEFT, 1, 0u);
    ASSERT_EQ(f.panel, schultz_events_focus(f.events));
    fixture_teardown(&f);
    PASS();
}

SUITE(event)
{
    RUN_TEST(a_press_on_nothing_focusable_takes_focus_away);
    RUN_TEST(a_right_press_leaves_focus_alone);
    RUN_TEST(a_disabled_node_stops_the_pointer_and_does_nothing_with_it);
    RUN_TEST(disabled_is_inherited_from_whatever_is_above);
    RUN_TEST(a_node_disabled_after_it_was_focused_gives_focus_up);
    RUN_TEST(a_disabled_node_cannot_be_activated_through_accessibility);
    RUN_TEST(the_router_lets_go_of_a_node_taken_out_of_the_tree);
    RUN_TEST(a_node_out_of_the_tree_cannot_be_focused_or_performed);
    RUN_TEST(performing_an_action_is_the_same_as_doing_it_by_hand);
    RUN_TEST(an_action_a_node_never_offered_is_refused);
    RUN_TEST(a_node_that_is_not_hit_testable_is_skipped);
    RUN_TEST(children_of_a_transparent_node_are_still_reachable);
    RUN_TEST(the_flag_rejects_a_stale_handle);
    RUN_TEST(hit_test_finds_the_deepest_node);
    RUN_TEST(clipping_decides_whether_a_child_outside_can_be_hit);
    RUN_TEST(hit_test_prefers_the_last_child);
    RUN_TEST(hit_test_skips_hidden_subtrees);
    RUN_TEST(moving_over_a_node_sends_enter_and_sets_the_flag);
    RUN_TEST(moving_away_sends_leave_before_enter);
    RUN_TEST(staying_on_one_node_does_not_re_enter);
    RUN_TEST(the_event_carries_the_token_and_local_coordinates);
    RUN_TEST(press_and_release_on_the_same_node_is_a_click);
    RUN_TEST(releasing_elsewhere_is_not_a_click);
    RUN_TEST(motion_while_pressed_is_a_drag_on_the_captured_node);
    RUN_TEST(scroll_goes_to_the_node_under_the_pointer);
    RUN_TEST(focus_emits_lost_then_gained);
    RUN_TEST(pressing_a_focusable_node_focuses_it);
    RUN_TEST(focus_moves_in_tree_order_and_wraps);
    RUN_TEST(focus_skips_disabled_and_hidden_nodes);
    RUN_TEST(focus_move_reports_exhausted_when_nothing_is_focusable);
    RUN_TEST(keys_and_text_go_to_the_focused_node);
    RUN_TEST(keys_go_nowhere_when_nothing_has_focus);
    RUN_TEST(overlays_are_hit_tested_before_the_content_tree);
    RUN_TEST(the_topmost_overlay_wins);
    RUN_TEST(pressing_outside_a_capturing_overlay_dismisses_and_consumes);
    RUN_TEST(pressing_inside_a_capturing_overlay_is_delivered_normally);
    RUN_TEST(a_non_capturing_overlay_lets_presses_through);
    RUN_TEST(popping_and_destroying_overlays_keeps_the_list_correct);
    RUN_TEST(routing_without_a_callback_still_updates_state);
    RUN_TEST(a_node_that_keeps_focus_does_not_take_it_either);
    RUN_TEST(a_node_that_keeps_focus_does_not_clear_it_either);
    RUN_TEST(a_finger_that_drags_off_a_field_does_not_focus_it);
    RUN_TEST(a_cursor_still_focuses_on_the_way_down);
    RUN_TEST(a_finger_tapping_the_background_still_ends_the_edit);
    RUN_TEST(a_press_between_children_keeps_focus_too);
    RUN_TEST(a_child_that_can_hold_focus_still_takes_it);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(event);
    GREATEST_MAIN_END();
}
