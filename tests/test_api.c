/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_api.c - the host facing interface.
 *
 * This file includes schultz_api.h and nothing else on purpose. If a host
 * could not build against that one header, neither can this, and the failure
 * shows up here rather than in somebody's binding generator.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_api.h"

/* ------------------------------------------------------- the surface */

TEST the_umbrella_header_is_enough_to_build_a_screen(void)
{
    schultz_tree *tree = NULL;
    schultz_events *events = NULL;
    schultz_handle panel = SCHULTZ_HANDLE_NONE;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    /*
     * Tree, widget, style, layout, routing and geometry, with no header but
     * schultz_api.h in scope.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(tree,
                                               schultz_tree_root(tree),
                                               &panel));
    schultz_node_set_pane(tree, panel, schultz_pane_vbox());
    schultz_node_set_style_property(tree, panel, SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_SURFACE));
    ASSERT_EQ(SCHULTZ_OK, schultz_button_create(tree, panel, "Go", &button));

    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    schultz_node_set_bounds(tree, panel, schultz_rect_make(0, 0, 100, 50));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(tree, panel, &bounds));
    ASSERT_EQ(100.0f, bounds.width);

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

/* The slack is a function now, because a public header carries no macros. */
TEST the_repaint_slack_is_readable_without_a_macro(void)
{
    ASSERT(schultz_paint_slack() > 0.0f);
    PASS();
}

/* ------------------------------------------------------------- queue */

typedef struct {
    schultz_tree   *tree;
    schultz_events *events;
    schultz_handle  button;
} api_fixture;

static int32_t fixture_setup(api_fixture *f)
{
    int32_t result;

    memset(f, 0, sizeof(*f));
    result = schultz_tree_create(&f->tree);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(f->tree, schultz_rect_make(0, 0, 200, 200));
    result = schultz_panel_create(f->tree, schultz_tree_root(f->tree),
                                  &f->button);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_set_bounds(f->tree, f->button,
                            schultz_rect_make(0, 0, 100, 50));
    schultz_node_set_actions(f->tree, f->button,
                             SCHULTZ_ACTION_CLICK | SCHULTZ_ACTION_FOCUS);
    schultz_node_set_token(f->tree, f->button, 42u);
    return schultz_events_create(f->tree, &f->events);
}

static void fixture_teardown(api_fixture *f)
{
    schultz_events_destroy(f->events);
    schultz_tree_destroy(f->tree);
}

static uint32_t count_of(const schultz_event *events, uint32_t count,
                         uint32_t type)
{
    uint32_t i;
    uint32_t n = 0;

    for (i = 0; i < count; i++) {
        if (events[i].type == type) {
            n++;
        }
    }
    return n;
}

TEST nothing_is_recorded_until_the_queue_is_asked_for(void)
{
    api_fixture f;
    const schultz_event *drained = NULL;
    uint32_t count = 99u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_mouse_move(f.events, schultz_point_make(50, 25), 0);

    ASSERT_EQ(0u, schultz_events_pending(f.events));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_drain(f.events, &drained, &count));
    ASSERT_EQ(NULL, drained);
    ASSERT_EQ(0u, count);

    fixture_teardown(&f);
    PASS();
}

TEST a_frame_of_events_comes_back_in_one_call(void)
{
    api_fixture f;
    const schultz_event *drained = NULL;
    uint32_t count = 0;
    schultz_point at = schultz_point_make(50, 25);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_queue(f.events, 1));

    schultz_events_mouse_move(f.events, at, 0);
    schultz_events_mouse_button(f.events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_button(f.events, at, SCHULTZ_BUTTON_LEFT, 0, 0);

    ASSERT(schultz_events_pending(f.events) > 0u);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_drain(f.events, &drained, &count));
    ASSERT(drained != NULL);

    /* Everything a callback would have been told, in the order it happened. */
    ASSERT_EQ(1u, count_of(drained, count, SCHULTZ_EVENT_MOUSE_ENTER));
    ASSERT_EQ(1u, count_of(drained, count, SCHULTZ_EVENT_MOUSE_DOWN));
    ASSERT_EQ(1u, count_of(drained, count, SCHULTZ_EVENT_MOUSE_UP));
    ASSERT_EQ(1u, count_of(drained, count, SCHULTZ_EVENT_CLICK));

    /* Carrying the same details a callback would have had. */
    {
        uint32_t i;
        int32_t saw = 0;

        for (i = 0; i < count; i++) {
            if (drained[i].type == SCHULTZ_EVENT_CLICK) {
                ASSERT_EQ(f.button, drained[i].target);
                ASSERT_EQ(42u, (uint32_t)drained[i].token);
                ASSERT_EQ(50.0f, drained[i].position.x);
                saw = 1;
            }
        }
        ASSERT(saw);
    }

    /* Drained means empty: the next frame starts clean. */
    ASSERT_EQ(0u, schultz_events_pending(f.events));
    schultz_events_mouse_move(f.events, schultz_point_make(51, 25), 0);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_drain(f.events, &drained, &count));
    ASSERT_EQ(1u, count_of(drained, count, SCHULTZ_EVENT_MOUSE_MOVE));

    fixture_teardown(&f);
    PASS();
}

/*
 * An event's text belongs to whoever produced it and is good for one call, so
 * the queue keeps its own copy. Scribbling over the original proves it.
 */
TEST the_queue_keeps_its_own_copy_of_any_text(void)
{
    api_fixture f;
    const schultz_event *drained = NULL;
    uint32_t count = 0;
    char typed[8];
    uint32_t i;
    int32_t saw = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_set_queue(f.events, 1);
    schultz_events_set_focus(f.events, f.button);

    memcpy(typed, "hello", 6);
    schultz_events_text_input(f.events, typed);
    memset(typed, 'x', sizeof(typed));

    schultz_events_drain(f.events, &drained, &count);
    for (i = 0; i < count; i++) {
        if (drained[i].type == SCHULTZ_EVENT_TEXT_INPUT) {
            ASSERT_STR_EQ("hello", drained[i].text);
            saw = 1;
        }
    }
    ASSERT(saw);

    fixture_teardown(&f);
    PASS();
}

/* The text buffer grows, and what was already recorded survives the move. */
TEST text_already_recorded_survives_the_buffer_growing(void)
{
    api_fixture f;
    const schultz_event *drained = NULL;
    uint32_t count = 0;
    uint32_t i;
    uint32_t seen = 0;
    char big[300];

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_set_queue(f.events, 1);
    schultz_events_set_focus(f.events, f.button);

    memset(big, 'a', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = '\0';

    schultz_events_text_input(f.events, "first");
    schultz_events_text_input(f.events, big);
    schultz_events_text_input(f.events, "last");

    schultz_events_drain(f.events, &drained, &count);
    for (i = 0; i < count; i++) {
        if (drained[i].type != SCHULTZ_EVENT_TEXT_INPUT) {
            continue;
        }
        if (seen == 0u) {
            ASSERT_STR_EQ("first", drained[i].text);
        } else if (seen == 1u) {
            ASSERT_EQ(sizeof(big) - 1u, strlen(drained[i].text));
        } else {
            ASSERT_STR_EQ("last", drained[i].text);
        }
        seen++;
    }
    ASSERT_EQ(3u, seen);

    fixture_teardown(&f);
    PASS();
}

/* Many more events than the queue first holds, so it grows and keeps order. */
TEST the_queue_grows_rather_than_dropping_events(void)
{
    api_fixture f;
    const schultz_event *drained = NULL;
    uint32_t count = 0;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_set_queue(f.events, 1);

    for (i = 0; i < 200u; i++) {
        schultz_events_mouse_move(f.events,
            schultz_point_make(10.0f + (float)i * 0.1f, 25), 0);
    }
    schultz_events_drain(f.events, &drained, &count);
    ASSERT(count >= 200u);
    ASSERT_EQ(200u, count_of(drained, count, SCHULTZ_EVENT_MOUSE_MOVE));

    /* In the order they happened. */
    {
        float last = -1.0f;

        for (i = 0; i < count; i++) {
            if (drained[i].type == SCHULTZ_EVENT_MOUSE_MOVE) {
                ASSERT(drained[i].position.x > last);
                last = drained[i].position.x;
            }
        }
    }

    fixture_teardown(&f);
    PASS();
}

/* A widget still gets first refusal: the queue watches, it does not consume. */
static int32_t seen_by_host;

static int32_t counting_callback(void *context, const schultz_event *event)
{
    (void)context;
    (void)event;
    seen_by_host++;
    return SCHULTZ_OK;
}

TEST the_queue_and_a_callback_see_the_same_events(void)
{
    api_fixture f;
    const schultz_event *drained = NULL;
    uint32_t count = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    seen_by_host = 0;
    schultz_events_set_queue(f.events, 1);
    schultz_events_set_callback(f.events, counting_callback, NULL);

    schultz_events_mouse_move(f.events, schultz_point_make(50, 25), 0);
    schultz_events_drain(f.events, &drained, &count);

    ASSERT_EQ((uint32_t)seen_by_host, count);

    fixture_teardown(&f);
    PASS();
}

TEST turning_the_queue_off_releases_it(void)
{
    api_fixture f;
    const schultz_event *drained = NULL;
    uint32_t count = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_events_set_queue(f.events, 1);
    schultz_events_mouse_move(f.events, schultz_point_make(50, 25), 0);
    ASSERT(schultz_events_pending(f.events) > 0u);

    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_queue(f.events, 0));
    ASSERT_EQ(0u, schultz_events_pending(f.events));
    schultz_events_mouse_move(f.events, schultz_point_make(60, 25), 0);
    schultz_events_drain(f.events, &drained, &count);
    ASSERT_EQ(0u, count);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_events_set_queue(NULL, 1));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_events_drain(f.events, NULL, &count));
    ASSERT_EQ(0u, schultz_events_pending(NULL));

    fixture_teardown(&f);
    PASS();
}

/* --------------------------------------------------- custom widgets */

/*
 * A host writes a widget's appearance with a canvas and its behaviour with a
 * token, and never implements the C vtable. Nothing here reaches past
 * schultz_api.h, which is the whole point: the draw list and the arena that
 * vtable is written against are not a host's to touch.
 */
TEST a_host_builds_a_custom_widget_without_an_upcall(void)
{
    schultz_tree *tree = NULL;
    schultz_events *events = NULL;
    schultz_handle dial = SCHULTZ_HANDLE_NONE;
    schultz_point points[3];
    const schultz_event *drained = NULL;
    uint32_t count = 0;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_create(tree, schultz_tree_root(tree),
                                                &dial));
    schultz_node_set_bounds(tree, dial, schultz_rect_make(0, 0, 60, 60));

    /* Its look, recorded once and replayed every frame after this. */
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(tree, dial));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_ellipse(tree, dial,
        schultz_rect_make(0, 0, 60, 60),
        schultz_paint_solid(schultz_color_rgba(40, 40, 48, 255))));
    points[0] = schultz_point_make(30.0f, 30.0f);
    points[1] = schultz_point_make(52.0f, 22.0f);
    points[2] = schultz_point_make(30.0f, 26.0f);
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_polygon(tree, dial, points, 3,
        schultz_paint_solid(schultz_color_rgba(216, 139, 74, 255)),
        SCHULTZ_FILL_NONZERO));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(tree, dial));
    ASSERT_EQ(2u, schultz_canvas_count(tree, dial));

    /* Its behaviour: a token on the node, and the queue rather than a call. */
    schultz_node_set_token(tree, dial, 77);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_queue(events, 1));

    schultz_events_mouse_move(events, schultz_point_make(30.0f, 30.0f), 0u);
    schultz_events_mouse_button(events, schultz_point_make(30.0f, 30.0f),
                                SCHULTZ_BUTTON_LEFT, 1, 0u);
    schultz_events_mouse_button(events, schultz_point_make(30.0f, 30.0f),
                                SCHULTZ_BUTTON_LEFT, 0, 0u);

    ASSERT_EQ(SCHULTZ_OK, schultz_events_drain(events, &drained, &count));
    ASSERT(count > 0u);
    {
        uint32_t i;
        int32_t clicked = 0;

        for (i = 0; i < count; i++) {
            if (drained[i].type == SCHULTZ_EVENT_CLICK &&
                drained[i].token == 77) {
                clicked = 1;
            }
        }
        ASSERT(clicked);
    }

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

SUITE(api)
{
    RUN_TEST(the_umbrella_header_is_enough_to_build_a_screen);
    RUN_TEST(the_repaint_slack_is_readable_without_a_macro);
    RUN_TEST(nothing_is_recorded_until_the_queue_is_asked_for);
    RUN_TEST(a_frame_of_events_comes_back_in_one_call);
    RUN_TEST(the_queue_keeps_its_own_copy_of_any_text);
    RUN_TEST(text_already_recorded_survives_the_buffer_growing);
    RUN_TEST(the_queue_grows_rather_than_dropping_events);
    RUN_TEST(the_queue_and_a_callback_see_the_same_events);
    RUN_TEST(turning_the_queue_off_releases_it);
    RUN_TEST(a_host_builds_a_custom_widget_without_an_upcall);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(api);
    GREATEST_MAIN_END();
}
