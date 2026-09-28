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
#include "schultz_camera_internal.h"
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
              schultz_camera_open(1u, 640u, 480u, 0u, NULL));

    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_camera_open(0u, 640u, 480u, 0u, &camera));
    ASSERT_EQ(NULL, camera);

    camera = (schultz_camera *)0x1;
    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_camera_open(1234u, 640u, 480u, 0u, &camera));
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
    ASSERT_EQ(0u, schultz_camera_frames_skipped(NULL));

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
                                              640u, 480u, 0u, &camera));
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

/*
 * Asking for a picture gives the newest one, not the oldest waiting.
 *
 * SDL keeps eight and hands them over in arrival order, which is right for
 * something recording and wrong for something showing: once those eight are
 * full it drops every new picture and keeps the stale ones, so taking one per
 * turn leaves a preview a quarter of a second behind and keeps it there. The
 * queue is emptied instead, and the ones passed over are counted.
 *
 * Made to happen here by not asking for a while, which is what a program too
 * slow for the camera does by accident.
 */
TEST asking_late_gives_the_newest_picture_and_counts_the_rest(void)
{
    schultz_camera *camera = NULL;
    const uint32_t *pixels = NULL;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t tries;

    if (schultz_camera_count() == 0u) {
        SKIPm("no camera on this machine");
    }
    ASSERT_EQ(SCHULTZ_OK, schultz_camera_open(schultz_camera_device(0u),
                                              0u, 0u, 0u, &camera));
    if (wait_for_permission(camera, 5000u) != SCHULTZ_CAMERA_ALLOWED) {
        schultz_camera_close(camera);
        SKIPm("the machine did not allow the camera to be used");
    }
    /* One to prove it is running, and to start from a known count. */
    for (tries = 0; tries < 400u; tries++) {
        if (schultz_camera_frame(camera, &pixels, &width, &height, NULL)
                == SCHULTZ_OK) {
            break;
        }
        SDL_Delay(20);
    }
    if (width == 0u) {
        schultz_camera_close(camera);
        SKIPm("the camera did not produce a picture");
    }
    ASSERT_EQ(0u, schultz_camera_frames_skipped(camera));

    /* Long enough for several more to arrive with nobody taking them. */
    SDL_Delay(400);
    ASSERT_EQ(SCHULTZ_OK, schultz_camera_frame(camera, &pixels, &width,
                                               &height, NULL));
    /* One call, and everything that piled up behind it is accounted for. */
    ASSERT(schultz_camera_frames_skipped(camera) > 0u);
    /* And the queue is empty again, so the next one waits for a new picture. */
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED,
              schultz_camera_frame(camera, &pixels, &width, &height, NULL));

    schultz_camera_close(camera);
    PASS();
}

/*
 * Closing a camera that a preview is still showing.
 *
 * A preview holds its camera by pointer and reads it on every turn, so this
 * used to leave it pointing at freed memory -- read on the turn after the
 * call, which is the turn after a stop button. What it read was the SDL
 * camera handle, which then went into SDL and was locked as a mutex; taking
 * a lock out of freed memory hangs rather than faults, so it showed as the
 * whole program stopping rather than as a crash.
 *
 * A camera now knows which previews are showing it and tells them when it
 * goes. What was last on screen stays there; the preview simply stops asking
 * for more.
 *
 * Runs without a camera: the dummy driver opens nothing, so the preview is
 * pointed at a camera that is NULL and the interesting half -- destroying a
 * preview that is registered, and closing with viewers attached -- is
 * exercised by the live suite.
 */
TEST closing_a_camera_leaves_its_previews_alone(void)
{
    camera_fixture f;
    schultz_camera *camera = NULL;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint64_t clock_ms = 0u;
    uint32_t drawn = 0u;
    uint32_t tries;

    if (schultz_camera_count() == 0u) {
        SKIPm("no camera on this machine");
    }
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_camera_open(schultz_camera_device(0u),
                                              0u, 0u, 0u, &camera));
    if (wait_for_permission(camera, 5000u) != SCHULTZ_CAMERA_ALLOWED) {
        schultz_camera_close(camera);
        fixture_teardown(&f);
        SKIPm("the machine did not allow the camera to be used");
    }
    ASSERT_EQ(SCHULTZ_OK,
              schultz_camera_preview_create(f.tree, schultz_tree_root(f.tree),
                                            camera, &node));
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 320, 240));

    /* Run until it has a picture up, so there is something to keep. */
    for (tries = 0; tries < 200u && drawn == 0u; tries++) {
        clock_ms += 16u;
        schultz_tree_advance(f.tree, clock_ms);
        drawn = paint_all(&f);
        SDL_Delay(20);
    }
    if (drawn == 0u) {
        schultz_camera_close(camera);
        fixture_teardown(&f);
        SKIPm("the camera did not produce a picture");
    }

    /* The stop button. */
    schultz_camera_close(camera);

    /*
     * Every turn after it. Without the fix the first of these reads the
     * freed camera; with it the preview does nothing at all, and what it was
     * last showing is still on the screen.
     */
    clock_ms += 16u;
    schultz_tree_advance(f.tree, clock_ms);
    clock_ms += 16u;
    schultz_tree_advance(f.tree, clock_ms);
    ASSERT_EQ(drawn, paint_all(&f));

    fixture_teardown(&f);
    PASS();
}

/* And the other way round: the preview goes while the camera is still open. */
TEST destroying_a_preview_before_its_camera_is_safe(void)
{
    camera_fixture f;
    schultz_camera *camera = NULL;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    if (schultz_camera_count() == 0u) {
        SKIPm("no camera on this machine");
    }
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_camera_open(schultz_camera_device(0u),
                                              0u, 0u, 0u, &camera));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_camera_preview_create(f.tree, schultz_tree_root(f.tree),
                                            camera, &node));
    schultz_tree_advance(f.tree, 16u);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_destroy(f.tree, node));
    /* The camera's list must no longer name that preview's memory. */
    schultz_camera_close(camera);

    fixture_teardown(&f);
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
                                              640u, 480u, 0u, &camera));
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

/* ------------------------------------------------- turning a picture upright */

/*
 * A picture whose every pixel says where it came from, so a turn that moves
 * one to the wrong place cannot pass. The value is the source column in the
 * high half and the source row in the low half.
 */
static void fill_marked(uint32_t *pixels, uint32_t w, uint32_t h,
                        uint32_t pitch_words)
{
    uint32_t x;
    uint32_t y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            pixels[y * pitch_words + x] = (x << 16) | y;
        }
    }
}

TEST a_frame_is_turned_the_way_the_camera_says(void)
{
    /* Three wide and two tall, so a quarter turn cannot be mistaken for no
     * turn: the shape changes as well as the contents. */
    static uint32_t from[2 * 3];
    static uint32_t into[3 * 2];
    const uint32_t w = 3u;
    const uint32_t h = 2u;
    const uint32_t pitch = 3u * sizeof(uint32_t);

    fill_marked(from, w, h, 3u);

    /* No turn: straight across. */
    memset(into, 0, sizeof(into));
    schultz_camera_upright(from, w, h, pitch, 0u, into);
    ASSERT_EQ(from[0], into[0]);
    ASSERT_EQ(from[5], into[5]);

    /*
     * A quarter clockwise. The picture becomes two wide and three tall, and
     * the pixel from the top left corner lands in the top right one.
     */
    memset(into, 0, sizeof(into));
    schultz_camera_upright(from, w, h, pitch, 1u, into);
    ASSERT_EQ((0u << 16) | 0u, into[0 * 2u + 1u]);   /* source (0,0) */
    ASSERT_EQ((2u << 16) | 0u, into[2 * 2u + 1u]);   /* source (2,0) */
    ASSERT_EQ((0u << 16) | 1u, into[0 * 2u + 0u]);   /* source (0,1) */

    /* Upside down: every corner swaps with the one opposite it. */
    memset(into, 0, sizeof(into));
    schultz_camera_upright(from, w, h, pitch, 2u, into);
    ASSERT_EQ((0u << 16) | 0u, into[1 * 3u + 2u]);
    ASSERT_EQ((2u << 16) | 1u, into[0 * 3u + 0u]);

    /* Three quarters clockwise, which is a quarter the other way. */
    memset(into, 0, sizeof(into));
    schultz_camera_upright(from, w, h, pitch, 3u, into);
    ASSERT_EQ((0u << 16) | 0u, into[2 * 2u + 0u]);   /* source (0,0) */
    ASSERT_EQ((2u << 16) | 0u, into[0 * 2u + 0u]);   /* source (2,0) */

    PASS();
}

TEST a_turn_keeps_every_pixel_exactly_once(void)
{
    static uint32_t from[4 * 5];
    static uint32_t into[4 * 5];
    static uint32_t seen[4 * 5];
    uint32_t quarters;
    uint32_t i;

    fill_marked(from, 4u, 5u, 4u);

    /*
     * Whatever the turn, the same twenty pixels come out, each once. That is
     * what catches an index that runs off the end or lands twice, which a
     * corner check on its own can miss.
     */
    for (quarters = 0u; quarters < 4u; quarters++) {
        memset(into, 0xFF, sizeof(into));
        memset(seen, 0, sizeof(seen));
        schultz_camera_upright(from, 4u, 5u, 4u * sizeof(uint32_t),
                               quarters, into);
        for (i = 0; i < 4u * 5u; i++) {
            uint32_t x = into[i] >> 16;
            uint32_t y = into[i] & 0xFFFFu;

            ASSERT(x < 4u);
            ASSERT(y < 5u);
            seen[y * 4u + x]++;
        }
        for (i = 0; i < 4u * 5u; i++) {
            ASSERT_EQ(1u, seen[i]);
        }
    }
    PASS();
}

TEST a_rotation_that_is_not_a_right_angle_is_left_alone(void)
{
    ASSERT_EQ(0u, schultz_camera_quarters(0.0f));
    ASSERT_EQ(1u, schultz_camera_quarters(90.0f));
    ASSERT_EQ(2u, schultz_camera_quarters(180.0f));
    ASSERT_EQ(3u, schultz_camera_quarters(270.0f));

    /* The same turn said another way. */
    ASSERT_EQ(3u, schultz_camera_quarters(-90.0f));
    ASSERT_EQ(1u, schultz_camera_quarters(450.0f));
    ASSERT_EQ(0u, schultz_camera_quarters(360.0f));

    /*
     * Anything else is refused rather than rounded to the nearest corner. A
     * picture cannot be turned by part of a right angle without resampling
     * it, and one the wrong way up is better than one that is smeared.
     */
    ASSERT_EQ(0u, schultz_camera_quarters(45.0f));
    ASSERT_EQ(0u, schultz_camera_quarters(1.0f));
    PASS();
}

TEST the_sizes_a_camera_offers_can_be_read(void)
{
    uint32_t devices = schultz_camera_count();
    uint32_t i;

    /*
     * A machine with no camera answers nothing rather than failing, which is
     * what `make test` sees. The shape of the answer is what is checked here;
     * scripts/livetest/camera.sh is where a real camera is asked.
     */
    ASSERT_EQ(0u, schultz_camera_size_count(0u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_camera_size_at(0u, 0u, NULL, NULL, NULL));

    for (i = 0; i < devices; i++) {
        uint64_t device = schultz_camera_device(i);
        uint32_t sizes = schultz_camera_size_count(device);
        uint32_t n;

        /* One past the end is refused, whatever the count turned out to be. */
        ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
                  schultz_camera_size_at(device, sizes, NULL, NULL, NULL));

        for (n = 0; n < sizes; n++) {
            uint32_t w = 0u;
            uint32_t h = 0u;
            uint32_t rate = 0u;

            ASSERT_EQ(SCHULTZ_OK,
                      schultz_camera_size_at(device, n, &w, &h, &rate));
            /* A size a camera offers is a size it can produce. */
            ASSERT(w > 0u);
            ASSERT(h > 0u);
        }
    }
    PASS();
}

TEST a_preview_asks_to_be_the_shape_of_the_pictures(void)
{
    float w = 0.0f;
    float h = 0.0f;

    /*
     * The case that started this. A phone turns its pictures upright, so a
     * stream that is sixteen by nine on the wire arrives nine by sixteen. A
     * host that asked for 320 wide should be told it needs to be tall, not
     * left with a wide box holding a narrow picture.
     */
    schultz_camera_preview_size(320.0f, 1080u, 1920u, &w, &h);
    ASSERT_EQ(320.0f, w);            /* the host's width is not ours to move */
    ASSERT_EQ(569u, (uint32_t)(h + 0.5f));   /* 320 * 1920 / 1080 */

    /* A landscape picture in the same request stays short. */
    schultz_camera_preview_size(320.0f, 1920u, 1080u, &w, &h);
    ASSERT_EQ(320.0f, w);
    ASSERT_EQ(180u, (uint32_t)(h + 0.5f));

    /* Four by three, the shape a low resolution mode turns into. */
    schultz_camera_preview_size(320.0f, 360u, 480u, &w, &h);
    ASSERT_EQ(427u, (uint32_t)(h + 0.5f));   /* 320 * 480 / 360 */

    /* Nothing asked for leaves only the picture's own size to go on. */
    schultz_camera_preview_size(SCHULTZ_SIZE_UNSET, 640u, 480u, &w, &h);
    ASSERT_EQ(640.0f, w);
    ASSERT_EQ(480.0f, h);

    /* A picture with no size at all changes nothing, rather than dividing
     * by zero. */
    w = 11.0f;
    h = 22.0f;
    schultz_camera_preview_size(320.0f, 0u, 480u, &w, &h);
    ASSERT_EQ(11.0f, w);
    ASSERT_EQ(22.0f, h);
    PASS();
}

SUITE(camera_live)
{
    RUN_TEST(a_real_camera_is_listed_with_a_name);
    RUN_TEST(a_real_camera_gives_pictures);
    RUN_TEST(asking_late_gives_the_newest_picture_and_counts_the_rest);
    RUN_TEST(closing_a_camera_leaves_its_previews_alone);
    RUN_TEST(destroying_a_preview_before_its_camera_is_safe);
    RUN_TEST(a_preview_shows_what_a_real_camera_sees);
}

SUITE(camera)
{
    RUN_TEST(the_sizes_a_camera_offers_can_be_read);
    RUN_TEST(a_frame_is_turned_the_way_the_camera_says);
    RUN_TEST(a_preview_asks_to_be_the_shape_of_the_pictures);
    RUN_TEST(a_turn_keeps_every_pixel_exactly_once);
    RUN_TEST(a_rotation_that_is_not_a_right_angle_is_left_alone);
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
