/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_clock.c - the tree's clock, and the two things that needed it: a
 * Lottie animation that plays itself and a tooltip that waits for the pointer
 * to rest.
 *
 * Time is pushed in rather than read, so these tests move the clock by hand
 * and never wait for anything.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_image.h"
#include "schultz_widgets.h"

#define LOTTIE_PATH "assets/images/pulse.json"

typedef struct {
    schultz_tree        *tree;
    schultz_events      *events;
    schultz_image_table *images;
    schultz_handle       animation;
} clock_fixture;

static int32_t fixture_setup(clock_fixture *f)
{
    int32_t result;

    memset(f, 0, sizeof(*f));
    result = schultz_tree_create(&f->tree);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(f->tree, schultz_rect_make(0, 0, 300, 200));
    result = schultz_image_table_create(&f->images);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_image_table(f->tree, f->images);
    result = schultz_image_load_animation(f->images, LOTTIE_PATH,
                                          &f->animation);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_events_create(f->tree, &f->events);
}

static void fixture_teardown(clock_fixture *f)
{
    schultz_events_destroy(f->events);
    schultz_tree_destroy(f->tree);
    schultz_image_table_destroy(f->images);
}

/* ---------------------------------------------------------------- clock */

TEST nothing_ticks_until_it_asks_to(void)
{
    clock_fixture f;
    schultz_handle view;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_lottie_create(f.tree,
                    schultz_tree_root(f.tree), f.animation, &view));

    /* Created paused, so the clock has nothing to do. */
    ASSERT_FALSE(schultz_node_animating(f.tree, view));
    ASSERT_EQ(0u, schultz_tree_advance(f.tree, 1000u));
    ASSERT_EQ(0u, schultz_tree_advance(f.tree, 2000u));

    ASSERT_EQ(SCHULTZ_OK, schultz_lottie_play(f.tree, view, 1));
    ASSERT(schultz_node_animating(f.tree, view));

    /* Playing again after a pause leaves one entry, not two. */
    schultz_lottie_play(f.tree, view, 1);
    ASSERT_EQ(1u, schultz_tree_advance(f.tree, 2100u));

    ASSERT_EQ(SCHULTZ_OK, schultz_lottie_play(f.tree, view, 0));
    ASSERT_FALSE(schultz_node_animating(f.tree, view));
    ASSERT_EQ(0u, schultz_tree_advance(f.tree, 3000u));

    fixture_teardown(&f);
    PASS();
}

TEST a_node_out_of_the_tree_is_not_ticked(void)
{
    clock_fixture f;
    schultz_handle view;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_lottie_create(f.tree,
                    schultz_tree_root(f.tree), f.animation, &view));
    ASSERT_EQ(SCHULTZ_OK, schultz_lottie_play(f.tree, view, 1));
    schultz_tree_advance(f.tree, 1000u);
    ASSERT_EQ(1u, schultz_tree_advance(f.tree, 1100u));

    /*
     * The ticking list reaches a node without walking the tree, so it is the
     * one way a node taken out of the tree could carry on running.
     */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_parent(f.tree, view, SCHULTZ_HANDLE_NONE));
    ASSERT_EQ(0u, schultz_tree_advance(f.tree, 1200u));
    ASSERT_EQ(0u, schultz_tree_advance(f.tree, 1300u));

    /* It never stopped asking, so putting it back starts it again. */
    ASSERT(schultz_node_animating(f.tree, view));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_parent(f.tree, view, schultz_tree_root(f.tree)));
    ASSERT_EQ(1u, schultz_tree_advance(f.tree, 1400u));

    fixture_teardown(&f);
    PASS();
}

/* The first call has no previous time to measure from, so nothing moves. */
TEST the_first_advance_only_sets_the_starting_point(void)
{
    clock_fixture f;
    schultz_handle view;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_lottie_create(f.tree, schultz_tree_root(f.tree), f.animation,
                          &view);
    schultz_lottie_play(f.tree, view, 1);

    ASSERT_EQ(0u, schultz_tree_advance(f.tree, 5000u));
    ASSERT_EQ(0.0f, schultz_lottie_frame(f.tree, view));

    ASSERT_EQ(1u, schultz_tree_advance(f.tree, 5100u));
    ASSERT(schultz_lottie_frame(f.tree, view) > 0.0f);

    /* A clock that went backwards is treated as no time having passed. */
    {
        float at = schultz_lottie_frame(f.tree, view);

        schultz_tree_advance(f.tree, 1000u);
        ASSERT_EQ(at, schultz_lottie_frame(f.tree, view));
    }

    ASSERT_EQ(0u, schultz_tree_advance(NULL, 0u));
    fixture_teardown(&f);
    PASS();
}

/* ----------------------------------------------------------- LottieView */

TEST an_animation_loads_and_reports_how_long_it_runs(void)
{
    clock_fixture f;
    float frames = 0.0f;
    float duration = 0.0f;
    schultz_size size;
    schultz_handle still = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_frames(f.images, f.animation,
                                               &frames, &duration));
    ASSERT_EQ(30.0f, frames);
    ASSERT_EQ(1.0f, duration);
    ASSERT_EQ(SCHULTZ_OK, schultz_image_size(f.images, f.animation, &size));
    ASSERT_EQ(64.0f, size.width);

    /* A still picture reports no frames, which is how the two are told apart. */
    ASSERT_EQ(SCHULTZ_OK, schultz_image_load_file(f.images,
                    "assets/images/checker.png", &still));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_frames(f.images, still, &frames,
                                               &duration));
    ASSERT_EQ(0.0f, frames);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_set_frame(f.images, still, 1.0f));

    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_image_load_animation(f.images, "no/such/file.json",
                                           &still));
    fixture_teardown(&f);
    PASS();
}

/*
 * Frames advance by however long has passed, not one per tick, so playback
 * runs at the speed it was authored at whatever rate the host draws.
 */
TEST playback_follows_the_clock_and_not_the_frame_rate(void)
{
    clock_fixture f;
    schultz_handle slow;
    schultz_handle fast;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_lottie_create(f.tree, schultz_tree_root(f.tree), f.animation,
                          &slow);
    schultz_lottie_create(f.tree, schultz_tree_root(f.tree), f.animation,
                          &fast);
    schultz_lottie_set_looping(f.tree, slow, 0);
    schultz_lottie_set_looping(f.tree, fast, 0);
    schultz_lottie_play(f.tree, slow, 1);
    schultz_tree_advance(f.tree, 0u);

    /* Half a second in two long frames, and in twenty short ones. */
    schultz_tree_advance(f.tree, 250u);
    schultz_tree_advance(f.tree, 500u);
    schultz_lottie_play(f.tree, slow, 0);

    schultz_lottie_play(f.tree, fast, 1);
    for (i = 1; i <= 20u; i++) {
        schultz_tree_advance(f.tree, 500u + i * 25u);
    }

    /* Both are half way through, within a frame of each other. */
    {
        float a = schultz_lottie_frame(f.tree, slow);
        float b = schultz_lottie_frame(f.tree, fast);
        float gap = (a > b) ? a - b : b - a;

        ASSERT(a > 13.0f && a < 16.0f);
        ASSERT(gap < 1.0f);
    }

    fixture_teardown(&f);
    PASS();
}

TEST an_animation_that_does_not_loop_stops_at_the_end(void)
{
    clock_fixture f;
    schultz_handle view;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_lottie_create(f.tree, schultz_tree_root(f.tree), f.animation,
                          &view);
    ASSERT_EQ(SCHULTZ_OK, schultz_lottie_set_looping(f.tree, view, 0));
    schultz_lottie_play(f.tree, view, 1);
    schultz_tree_advance(f.tree, 0u);

    /* Well past the end. */
    schultz_tree_advance(f.tree, 5000u);
    ASSERT_EQ(29.0f, schultz_lottie_frame(f.tree, view));
    ASSERT_FALSE(schultz_lottie_is_playing(f.tree, view));
    /* And it took itself off the clock, so it costs nothing from here. */
    ASSERT_FALSE(schultz_node_animating(f.tree, view));

    fixture_teardown(&f);
    PASS();
}

TEST a_looping_animation_wraps_rather_than_resetting(void)
{
    clock_fixture f;
    schultz_handle view;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_lottie_create(f.tree, schultz_tree_root(f.tree), f.animation,
                          &view);
    schultz_lottie_play(f.tree, view, 1);
    schultz_tree_advance(f.tree, 0u);

    /*
     * One and a quarter times round in a single very slow frame. Resetting to
     * zero would lose the quarter; wrapping keeps it.
     */
    schultz_tree_advance(f.tree, 1250u);
    {
        float at = schultz_lottie_frame(f.tree, view);

        ASSERT(at > 5.0f && at < 10.0f);
    }
    ASSERT(schultz_lottie_is_playing(f.tree, view));

    fixture_teardown(&f);
    PASS();
}

TEST an_animation_can_be_scrubbed_by_hand(void)
{
    clock_fixture f;
    schultz_handle view;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_lottie_create(f.tree, schultz_tree_root(f.tree), f.animation,
                          &view);

    ASSERT_EQ(SCHULTZ_OK, schultz_lottie_seek(f.tree, view, 12.5f));
    ASSERT_EQ(12.5f, schultz_lottie_frame(f.tree, view));
    /* Clamped at both ends. */
    schultz_lottie_seek(f.tree, view, -5.0f);
    ASSERT_EQ(0.0f, schultz_lottie_frame(f.tree, view));
    schultz_lottie_seek(f.tree, view, 900.0f);
    ASSERT_EQ(29.0f, schultz_lottie_frame(f.tree, view));

    /* Scrubbing does not start it. */
    ASSERT_FALSE(schultz_lottie_is_playing(f.tree, view));

    fixture_teardown(&f);
    PASS();
}

TEST lottie_accessors_reject_other_widgets(void)
{
    clock_fixture f;
    schultz_handle panel;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ(0.0f, schultz_lottie_frame(f.tree, panel));
    ASSERT_EQ(0, schultz_lottie_is_playing(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_lottie_play(f.tree, panel, 1));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_lottie_seek(f.tree, panel, 1.0f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_lottie_set_looping(f.tree, panel, 1));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_lottie_create(f.tree, schultz_tree_root(f.tree),
                                    f.animation, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_set_animating(f.tree, (schultz_handle)999, 1));

    fixture_teardown(&f);
    PASS();
}

/* -------------------------------------------------------------- Tooltip */

TEST a_tooltip_waits_for_the_pointer_to_rest(void)
{
    clock_fixture f;
    schultz_handle button;
    schultz_handle tip;
    schultz_point over;
    schultz_point away;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Hover",
                          &button);
    schultz_node_set_bounds(f.tree, button, schultz_rect_make(10, 10, 80,
                                                              30));
    ASSERT_EQ(SCHULTZ_OK, schultz_tooltip_create(f.tree, "Explains itself",
                                                 &tip));
    ASSERT_EQ(SCHULTZ_OK, schultz_tooltip_watch(f.tree, tip, button, 500u));
    ASSERT(schultz_node_animating(f.tree, tip));

    over = schultz_point_make(50, 25);
    away = schultz_point_make(200, 150);
    schultz_tree_advance(f.tree, 0u);

    /* Time passing with the pointer elsewhere shows nothing. */
    schultz_events_mouse_move(f.events, away, 0);
    schultz_tree_advance(f.tree, 2000u);
    ASSERT_FALSE(schultz_popup_is_open(f.tree, tip));

    /* Hovering, but not for long enough yet. */
    schultz_events_mouse_move(f.events, over, 0);
    schultz_tree_advance(f.tree, 2300u);
    ASSERT_FALSE(schultz_popup_is_open(f.tree, tip));

    /* And now it has rested long enough. */
    schultz_tree_advance(f.tree, 2600u);
    ASSERT(schultz_popup_is_open(f.tree, tip));

    /* Leaving takes it away again, and the wait starts over. */
    schultz_events_mouse_move(f.events, away, 0);
    schultz_tree_advance(f.tree, 2700u);
    ASSERT_FALSE(schultz_popup_is_open(f.tree, tip));

    schultz_events_mouse_move(f.events, over, 0);
    schultz_tree_advance(f.tree, 2900u);
    ASSERT_FALSE(schultz_popup_is_open(f.tree, tip));
    schultz_tree_advance(f.tree, 3300u);
    ASSERT(schultz_popup_is_open(f.tree, tip));

    fixture_teardown(&f);
    PASS();
}

TEST a_tooltip_can_stop_watching(void)
{
    clock_fixture f;
    schultz_handle button;
    schultz_handle tip;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Hover",
                          &button);
    schultz_node_set_bounds(f.tree, button, schultz_rect_make(0, 0, 80, 30));
    schultz_tooltip_create(f.tree, "Gone", &tip);
    schultz_tooltip_watch(f.tree, tip, button, 100u);

    schultz_tree_advance(f.tree, 0u);
    schultz_events_mouse_move(f.events, schultz_point_make(40, 15), 0);
    schultz_tree_advance(f.tree, 500u);
    ASSERT(schultz_popup_is_open(f.tree, tip));

    ASSERT_EQ(SCHULTZ_OK, schultz_tooltip_watch(f.tree, tip,
                                                SCHULTZ_HANDLE_NONE, 0u));
    ASSERT_FALSE(schultz_popup_is_open(f.tree, tip));
    ASSERT_FALSE(schultz_node_animating(f.tree, tip));

    fixture_teardown(&f);
    PASS();
}

SUITE(animation)
{
    RUN_TEST(nothing_ticks_until_it_asks_to);
    RUN_TEST(a_node_out_of_the_tree_is_not_ticked);
    RUN_TEST(the_first_advance_only_sets_the_starting_point);
    RUN_TEST(an_animation_loads_and_reports_how_long_it_runs);
    RUN_TEST(playback_follows_the_clock_and_not_the_frame_rate);
    RUN_TEST(an_animation_that_does_not_loop_stops_at_the_end);
    RUN_TEST(a_looping_animation_wraps_rather_than_resetting);
    RUN_TEST(an_animation_can_be_scrubbed_by_hand);
    RUN_TEST(lottie_accessors_reject_other_widgets);
    RUN_TEST(a_tooltip_waits_for_the_pointer_to_rest);
    RUN_TEST(a_tooltip_can_stop_watching);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(animation);
    GREATEST_MAIN_END();
}
