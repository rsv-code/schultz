/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_camera.c - finding a camera, refusing what is not there, and the node
 * that shows one.
 *
 * Headless, and on any machine. SDL's dummy camera driver is asked for before
 * anything opens a device, and it offers no cameras at all, which is exactly
 * the case a desktop with no webcam presents and the one worth pinning down:
 * every call has to answer sensibly rather than fail.
 *
 * The tests that need a real camera are in here too, in a second suite that
 * only runs when SCHULTZ_LIVE_CAMERA is set in the environment. `make test`
 * never sets it, so nothing here ever switches a camera on by itself.
 * scripts/livetest/camera.sh is what sets it; see that script for why this is
 * kept out of the ordinary run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>

#include "greatest.h"
#include "schultz_camera.h"
#include "schultz_image.h"
#include "schultz_widget.h"

typedef struct {
    schultz_tree        *tree;
    schultz_image_table *images;
    schultz_arena        arena;
    schultz_draw_list    list;
} camera_fixture;

static int32_t fixture_setup(camera_fixture *f)
{
    int32_t result;

    memset(f, 0, sizeof(*f));
    result = schultz_tree_create(&f->tree);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(f->tree, schultz_rect_make(0, 0, 640, 480));
    result = schultz_image_table_create(&f->images);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_image_table(f->tree, f->images);
    result = schultz_arena_init(&f->arena, 0);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_draw_list_init(&f->list, &f->arena, 0);
}

static void fixture_teardown(camera_fixture *f)
{
    schultz_arena_free(&f->arena);
    schultz_tree_destroy(f->tree);
    schultz_image_table_destroy(f->images);
}

static uint32_t paint_all(camera_fixture *f)
{
    schultz_arena_reset(&f->arena);
    schultz_draw_list_init(&f->list, &f->arena, 0);
    schultz_tree_resolve_styles(f->tree);
    schultz_widget_paint_tree(f->tree, &f->list, &f->arena,
                              schultz_rect_make(0, 0, 0, 0));
    return schultz_draw_list_count(&f->list);
}

/* ---------------------------------------------------------------- the list */

/*
 * No cameras is an ordinary answer. It is the one this suite can guarantee,
 * and the one most likely to be got wrong: a machine with no webcam must not
 * make any of this fail.
 */
TEST a_machine_with_no_camera_reports_none(void)
{
    ASSERT_EQ(0u, schultz_camera_count());
    ASSERT_EQ(0u, schultz_camera_device(0u));
    ASSERT_EQ(0u, schultz_camera_device(7u));
    PASS();
}

TEST asking_about_a_camera_that_is_not_there_is_answered(void)
{
    char name[64];

    memset(name, 'x', sizeof(name));
    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_camera_name(1234u, name, sizeof(name)));
    /* Answered, and the buffer left empty rather than as it was. */
    ASSERT_EQ('\0', name[0]);
    ASSERT_EQ(SCHULTZ_CAMERA_FACING_UNKNOWN, schultz_camera_facing(1234u));
    PASS();
}

TEST a_name_needs_somewhere_to_put_it(void)
{
    char name[8];

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_camera_name(1u, NULL, sizeof(name)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_camera_name(1u, name, 0u));
    PASS();
}

/* ------------------------------------------------------------ opening one */

TEST opening_a_camera_that_is_not_there_is_refused(void)
{
    schultz_camera *camera = (schultz_camera *)0x1;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_camera_open(1u, 640u, 480u, NULL));

    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_camera_open(0u, 640u, 480u, &camera));
    ASSERT_EQ(NULL, camera);

    camera = (schultz_camera *)0x1;
    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_camera_open(1234u, 640u, 480u, &camera));
    ASSERT_EQ(NULL, camera);
    PASS();
}

/* Every accessor has to answer for a camera that was never opened. */
TEST nothing_is_allowed_without_a_camera(void)
{
    const uint32_t *pixels = NULL;
    uint32_t width = 99u;
    uint32_t height = 99u;

    ASSERT_EQ(SCHULTZ_CAMERA_REFUSED, schultz_camera_permission(NULL));
    ASSERT_EQ(0u, schultz_camera_frames_taken(NULL));

    ASSERT_EQ(SCHULTZ_OK, schultz_camera_size(NULL, &width, &height));
    ASSERT_EQ(0u, width);
    ASSERT_EQ(0u, height);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_camera_size(NULL, NULL, &height));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_camera_frame(NULL, &pixels, NULL, NULL, NULL));
    ASSERT_EQ(NULL, pixels);

    /* Closing nothing is allowed, because it makes a teardown path simpler. */
    schultz_camera_close(NULL);
    PASS();
}

/* ----------------------------------------------------------- the preview */

TEST a_preview_with_no_camera_draws_nothing(void)
{
    camera_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_camera_preview_create(f.tree, schultz_tree_root(f.tree),
                                            NULL, &node));
    ASSERT(node != SCHULTZ_HANDLE_NONE);
    schultz_node_set_bounds(f.tree, node,
                            schultz_rect_make(0, 0, 320, 240));

    /* No camera, no clock: a preview showing nothing costs nothing. */
    ASSERT_EQ(0, schultz_node_animating(f.tree, node));

    schultz_tree_advance(f.tree, 16u);
    schultz_tree_advance(f.tree, 32u);
    ASSERT_EQ(0u, paint_all(&f));

    fixture_teardown(&f);
    PASS();
}

/* The bounds are the node's, whatever it is showing or not showing. */
TEST a_preview_keeps_the_bounds_it_was_given(void)
{
    camera_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_camera_preview_create(f.tree, schultz_tree_root(f.tree),
                                            NULL, &node));
    schultz_node_set_bounds(f.tree, node,
                            schultz_rect_make(10, 20, 300, 200));
    schultz_tree_advance(f.tree, 16u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_absolute_bounds(f.tree, node, &bounds));
    ASSERT_EQ(10, (int)bounds.x);
    ASSERT_EQ(20, (int)bounds.y);
    ASSERT_EQ(300, (int)bounds.width);
    ASSERT_EQ(200, (int)bounds.height);

    fixture_teardown(&f);
    PASS();
}

TEST a_preview_can_be_pointed_somewhere_else(void)
{
    camera_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle plain = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_camera_preview_create(f.tree, schultz_tree_root(f.tree),
                                            NULL, &node));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_camera_preview_set_camera(f.tree, node, NULL));

    /* And a node that is not a preview says so rather than taking it. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_create(f.tree, schultz_tree_root(f.tree), &plain));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_camera_preview_set_camera(f.tree, plain, NULL));

    fixture_teardown(&f);
    PASS();
}

TEST a_preview_needs_somewhere_to_put_the_handle(void)
{
    camera_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_camera_preview_create(f.tree, schultz_tree_root(f.tree),
                                            NULL, NULL));
    fixture_teardown(&f);
    PASS();
}

/* ----------------------------------------------- with a camera really there */

/*
 * These open the machine's camera, which on most machines turns a light on.
 * Nothing runs them unless SCHULTZ_LIVE_CAMERA is set, and each skips rather
 * than fails when there is no camera to open.
 */

/* Waits for the answer, or gives up. Permission can take a person's attention
 * on a phone, so there is no answer that means "immediately". */
static uint32_t wait_for_permission(schultz_camera *camera, uint32_t ms)
{
    uint32_t waited;
    uint32_t state = SCHULTZ_CAMERA_WAITING;

    for (waited = 0; waited < ms; waited += 50u) {
        state = schultz_camera_permission(camera);
        if (state != SCHULTZ_CAMERA_WAITING) {
            break;
        }
        SDL_Delay(50);
    }
    return state;
}

TEST a_real_camera_is_listed_with_a_name(void)
{
    uint64_t device;
    char name[128];

    if (schultz_camera_count() == 0u) {
        SKIPm("no camera on this machine");
    }
    device = schultz_camera_device(0u);
    ASSERT(device != 0u);
    ASSERT_EQ(SCHULTZ_OK, schultz_camera_name(device, name, sizeof(name)));
    ASSERT(strlen(name) > 0u);
    printf("      camera: %s\n", name);
    PASS();
}

TEST a_real_camera_gives_pictures(void)
{
    schultz_camera *camera = NULL;
    const uint32_t *pixels = NULL;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint64_t when = 0u;
    uint32_t tries;
    int32_t got = 0;

    if (schultz_camera_count() == 0u) {
        SKIPm("no camera on this machine");
    }
    ASSERT_EQ(SCHULTZ_OK, schultz_camera_open(schultz_camera_device(0u),
                                              640u, 480u, &camera));
    ASSERT(camera != NULL);

    if (wait_for_permission(camera, 5000u) != SCHULTZ_CAMERA_ALLOWED) {
        schultz_camera_close(camera);
        SKIPm("the machine did not allow the camera to be used");
    }

    for (tries = 0; tries < 200u && !got; tries++) {
        if (schultz_camera_frame(camera, &pixels, &width, &height, &when)
                == SCHULTZ_OK) {
            got = 1;
            break;
        }
        SDL_Delay(20);
    }
    ASSERT(got);
    ASSERT(pixels != NULL);
    ASSERT(width > 0u);
    ASSERT(height > 0u);
    ASSERT(when > 0u);
    /* A camera picture is opaque, whatever the camera's own format was. */
    ASSERT_EQ(0xFFu, (pixels[0] >> 24) & 0xFFu);
    ASSERT_EQ(1u, schultz_camera_frames_taken(camera));

    /* The size is known once pictures are arriving, and matches them. */
    {
        uint32_t said_width = 0u;
        uint32_t said_height = 0u;

        ASSERT_EQ(SCHULTZ_OK, schultz_camera_size(camera, &said_width,
                                                  &said_height));
        ASSERT_EQ(width, said_width);
        ASSERT_EQ(height, said_height);
    }
    printf("      %ux%u, first picture at %llu ns\n", width, height,
           (unsigned long long)when);

    schultz_camera_close(camera);
    PASS();
}

TEST a_preview_shows_what_a_real_camera_sees(void)
{
    camera_fixture f;
    schultz_camera *camera = NULL;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t tries;
    uint64_t clock_ms = 0u;

    if (schultz_camera_count() == 0u) {
        SKIPm("no camera on this machine");
    }
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_camera_open(schultz_camera_device(0u),
                                              640u, 480u, &camera));
    if (wait_for_permission(camera, 5000u) != SCHULTZ_CAMERA_ALLOWED) {
        schultz_camera_close(camera);
        fixture_teardown(&f);
        SKIPm("the machine did not allow the camera to be used");
    }

    ASSERT_EQ(SCHULTZ_OK,
              schultz_camera_preview_create(f.tree, schultz_tree_root(f.tree),
                                            camera, &node));
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 320, 240));

    for (tries = 0; tries < 200u; tries++) {
        clock_ms += 16u;
        schultz_tree_advance(f.tree, clock_ms);
        if (paint_all(&f) > 0u) {
            break;
        }
        SDL_Delay(20);
    }
    /* One picture, drawn as one image command. */
    ASSERT_EQ(1u, paint_all(&f));

    schultz_camera_close(camera);
    fixture_teardown(&f);
    PASS();
}

SUITE(camera_live)
{
    RUN_TEST(a_real_camera_is_listed_with_a_name);
    RUN_TEST(a_real_camera_gives_pictures);
    RUN_TEST(a_preview_shows_what_a_real_camera_sees);
}

SUITE(camera)
{
    RUN_TEST(a_machine_with_no_camera_reports_none);
    RUN_TEST(asking_about_a_camera_that_is_not_there_is_answered);
    RUN_TEST(a_name_needs_somewhere_to_put_it);
    RUN_TEST(opening_a_camera_that_is_not_there_is_refused);
    RUN_TEST(nothing_is_allowed_without_a_camera);
    RUN_TEST(a_preview_with_no_camera_draws_nothing);
    RUN_TEST(a_preview_keeps_the_bounds_it_was_given);
    RUN_TEST(a_preview_can_be_pointed_somewhere_else);
    RUN_TEST(a_preview_needs_somewhere_to_put_the_handle);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    int32_t live = (getenv("SCHULTZ_LIVE_CAMERA") != NULL);

    if (!live) {
        /*
         * Before anything opens a device, and before SDL starts. The dummy
         * driver offers no cameras, which is what makes this run the same on
         * every machine and never switch a real camera on.
         */
        SDL_SetHint(SDL_HINT_CAMERA_DRIVER, "dummy");
    }
    GREATEST_MAIN_BEGIN();
    if (live) {
        RUN_SUITE(camera_live);
    } else {
        RUN_SUITE(camera);
    }
    GREATEST_MAIN_END();
}
