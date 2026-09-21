/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_video.c - demuxing, decoding and showing video.
 *
 * These read two small clips from tests/assets: two seconds of 320x240 at
 * fifteen frames a second, one VP8 and one VP9. Small enough to commit, long
 * enough that a decoder which stops after the first keyframe fails.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>

#include "greatest.h"
#include "schultz_audio.h"
#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_image.h"
#include "schultz_style.h"
#include "schultz_video.h"
#include "schultz_widget.h"
#include "schultz_widgets.h"

#define CLIP_VP8 "tests/assets/clip-vp8.webm"
#define CLIP_VP9 "tests/assets/clip-vp9.webm"
#define CLIP_RED "tests/assets/clip-red.webm"
#define CLIP_SOUND "tests/assets/clip-sound.webm"
#define FONT_PATH  "assets/fonts/DejaVuSans.ttf"

/*
 * Where the tree's clock has got to. It only ever rises, so two runs in one
 * test carry on rather than starting again, which is what a frame loop does.
 * Reset by the fixture, so one test's time is never another's.
 */
static uint64_t clock_ms;

typedef struct {
    schultz_tree        *tree;
    schultz_image_table *images;
    schultz_audio       *audio;   /**< The dummy driver; see main. */
    /*
     * Fonts, a theme and an event source, so the control row can be laid out
     * and clicked. A button with no font measures to nothing and there is
     * nowhere to click.
     */
    schultz_font_system *fonts;
    schultz_glyph_cache *glyphs;
    schultz_handle       font;
    schultz_theme        theme;
    schultz_events      *events;
    schultz_arena        arena;
    schultz_draw_list    list;
} video_fixture;

static int32_t fixture_setup(video_fixture *f)
{
    int32_t result;

    memset(f, 0, sizeof(*f));
    clock_ms = 0u;
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
    /*
     * A sound system, so a film with an Opus track can be heard. It plays to
     * nowhere through SDL's dummy driver, which still consumes what it is
     * given at the rate a real one would, so it is a clock as well.
     */
    if (schultz_audio_create(&f->audio) == SCHULTZ_OK) {
        schultz_tree_set_audio(f->tree, f->audio);
    }

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

static void fixture_teardown(video_fixture *f)
{
    schultz_arena_free(&f->arena);
    schultz_events_destroy(f->events);
    schultz_tree_destroy(f->tree);
    schultz_glyph_cache_destroy(f->glyphs);
    schultz_font_system_destroy(f->fonts);
    schultz_image_table_destroy(f->images);
    if (f->audio != NULL) {
        schultz_audio_destroy(f->audio);
    }
}

/* The whole of a clip, as the host would hand it over. */
static unsigned char *slurp(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    unsigned char *bytes;
    long size;

    *out_size = 0u;
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    bytes = (unsigned char *)malloc((size_t)size);
    if (bytes == NULL || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)size;
    return bytes;
}

/* Runs the tree's clock forward, which is what decodes. */
static void run_for(video_fixture *f, uint32_t ms, uint32_t step_ms)
{
    uint32_t at;

    for (at = 0; at < ms; at += step_ms) {
        clock_ms += step_ms;
        schultz_tree_advance(f->tree, clock_ms);
    }
}

/* A node fed a whole clip and played, decoding wherever it is asked to. */
static schultz_handle play_clip_on(video_fixture *f, const char *path,
                                   uint32_t threads)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    size_t size = 0u;
    unsigned char *bytes = slurp(path, &size);

    if (bytes == NULL) {
        return SCHULTZ_HANDLE_NONE;
    }
    if (schultz_video_create(f->tree, schultz_tree_root(f->tree), threads,
                             &node) != SCHULTZ_OK) {
        free(bytes);
        return SCHULTZ_HANDLE_NONE;
    }
    schultz_node_set_bounds(f->tree, node, schultz_rect_make(0, 0, 320, 240));
    schultz_video_write(f->tree, node, bytes, size);
    free(bytes);
    schultz_video_play(f->tree, node);
    return node;
}

/* The common case: decoding on whichever thread advances the tree. */
static schultz_handle play_clip(video_fixture *f, const char *path)
{
    return play_clip_on(f, path, 0u);
}

/* ------------------------------------------------------------ the fixtures */

TEST the_clips_are_where_the_tests_expect_them(void)
{
    size_t size = 0u;
    unsigned char *bytes = slurp(CLIP_VP8, &size);

    ASSERT(bytes != NULL);
    ASSERT(size > 1000u);
    /* Every WebM starts with the EBML header, so this is a file and not a
     * placeholder somebody committed by accident. */
    ASSERT_EQ(0x1Au, bytes[0]);
    ASSERT_EQ(0x45u, bytes[1]);
    ASSERT_EQ(0xDFu, bytes[2]);
    ASSERT_EQ(0xA3u, bytes[3]);
    free(bytes);

    bytes = slurp(CLIP_VP9, &size);
    ASSERT(bytes != NULL);
    ASSERT(size > 1000u);
    free(bytes);
    PASS();
}

/* ------------------------------------------------------------------ demux */

TEST a_clip_reports_its_codec_and_size(void)
{
    video_fixture f;
    schultz_handle vp8;
    schultz_handle vp9;
    uint32_t width = 0u;
    uint32_t height = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));

    vp8 = play_clip(&f, CLIP_VP8);
    ASSERT(vp8 != SCHULTZ_HANDLE_NONE);
    run_for(&f, 100u, 33u);
    ASSERT_EQ((uint32_t)SCHULTZ_VIDEO_CODEC_VP8,
              schultz_video_codec(f.tree, vp8));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_size(f.tree, vp8, &width, &height));
    ASSERT_EQ(320u, width);
    ASSERT_EQ(240u, height);

    vp9 = play_clip(&f, CLIP_VP9);
    ASSERT(vp9 != SCHULTZ_HANDLE_NONE);
    run_for(&f, 100u, 33u);
    ASSERT_EQ((uint32_t)SCHULTZ_VIDEO_CODEC_VP9,
              schultz_video_codec(f.tree, vp9));

    fixture_teardown(&f);
    PASS();
}

/*
 * Bytes that are not a WebM file must not be taken for one. The node stays
 * silent rather than showing anything, and above all does not crash on the
 * first packet it invents.
 */
TEST bytes_that_are_not_webm_show_nothing(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    unsigned char rubbish[4096];
    uint32_t i;

    for (i = 0; i < sizeof(rubbish); i++) {
        rubbish[i] = (unsigned char)(i * 7u + 13u);
    }

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_write(f.tree, node, rubbish,
                                              sizeof(rubbish)));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    run_for(&f, 500u, 33u);

    ASSERT_EQ(0u, schultz_video_frames_shown(f.tree, node));
    ASSERT_EQ((uint32_t)SCHULTZ_VIDEO_CODEC_NONE,
              schultz_video_codec(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

/* A file cut in half is a file that has not finished arriving, not an error.
 * What matters is that it decodes what it has, and does not read past the
 * end of it. */
TEST a_truncated_clip_decodes_what_arrived(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    size_t size = 0u;
    unsigned char *bytes = slurp(CLIP_VP8, &size);

    ASSERT(bytes != NULL);
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 320, 240));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_write(f.tree, node, bytes, size / 2u));
    free(bytes);
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    run_for(&f, 3000u, 33u);

    /* Half a clip is still a clip: it opened, and it drew something. */
    ASSERT_EQ((uint32_t)SCHULTZ_VIDEO_CODEC_VP8,
              schultz_video_codec(f.tree, node));
    ASSERT(schultz_video_frames_shown(f.tree, node) > 0u);

    fixture_teardown(&f);
    PASS();
}

/* ----------------------------------------------------------------- decode */

TEST a_vp8_clip_decodes_more_than_one_frame(void)
{
    video_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = play_clip(&f, CLIP_VP8);
    ASSERT(node != SCHULTZ_HANDLE_NONE);

    run_for(&f, 2200u, 33u);
    /*
     * Two seconds at fifteen a second is thirty frames. Not asserting thirty
     * exactly: the clock is stepped in 33 ms jumps and the last one may land
     * either side of the end. More than one is the thing that matters, since
     * one would mean the keyframe decoded and nothing after it.
     */
    ASSERT(schultz_video_frames_shown(f.tree, node) > 20u);

    fixture_teardown(&f);
    PASS();
}

TEST a_vp9_clip_decodes_more_than_one_frame(void)
{
    video_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = play_clip(&f, CLIP_VP9);
    ASSERT(node != SCHULTZ_HANDLE_NONE);

    run_for(&f, 2200u, 33u);
    ASSERT(schultz_video_frames_shown(f.tree, node) > 20u);

    fixture_teardown(&f);
    PASS();
}

/* A film plays at its own speed, not the display's. Half the clip's length
 * should be about half its frames, which is what proves the timestamps are
 * being read rather than a frame being shown per turn. */
TEST a_clip_plays_at_its_own_speed(void)
{
    video_fixture f;
    schultz_handle node;
    uint64_t halfway;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = play_clip(&f, CLIP_VP8);
    ASSERT(node != SCHULTZ_HANDLE_NONE);

    run_for(&f, 1000u, 33u);
    halfway = schultz_video_frames_shown(f.tree, node);
    /* One second of fifteen a second, with a little slack either way. */
    ASSERT(halfway >= 10u);
    ASSERT(halfway <= 20u);

    fixture_teardown(&f);
    PASS();
}

TEST pausing_holds_the_frame_and_playing_carries_on(void)
{
    video_fixture f;
    schultz_handle node;
    uint64_t at_pause;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = play_clip(&f, CLIP_VP8);
    ASSERT(node != SCHULTZ_HANDLE_NONE);

    run_for(&f, 400u, 33u);
    ASSERT(schultz_video_is_playing(f.tree, node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_pause(f.tree, node));
    ASSERT_EQ(0, schultz_video_is_playing(f.tree, node));

    at_pause = schultz_video_frames_shown(f.tree, node);
    ASSERT(at_pause > 0u);
    run_for(&f, 500u, 33u);
    ASSERT_EQ(at_pause, schultz_video_frames_shown(f.tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    run_for(&f, 500u, 33u);
    ASSERT(schultz_video_frames_shown(f.tree, node) > at_pause);

    fixture_teardown(&f);
    PASS();
}

/* A clip that is not looping stops at the end and holds the last picture.
 * One that is looping carries on past it. */
TEST looping_starts_again_and_not_looping_stops(void)
{
    video_fixture f;
    schultz_handle once;
    schultz_handle over;
    uint64_t once_at_end;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));

    once = play_clip(&f, CLIP_VP8);
    over = play_clip(&f, CLIP_VP8);
    ASSERT(once != SCHULTZ_HANDLE_NONE);
    ASSERT(over != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_looping(f.tree, over, 1));

    run_for(&f, 2500u, 33u);
    once_at_end = schultz_video_frames_shown(f.tree, once);
    ASSERT(once_at_end > 20u);

    run_for(&f, 2000u, 33u);
    ASSERT_EQ(once_at_end, schultz_video_frames_shown(f.tree, once));
    ASSERT(schultz_video_frames_shown(f.tree, over) > once_at_end);

    fixture_teardown(&f);
    PASS();
}

/* ----------------------------------------------------------- feeding bytes */

/* A stream arrives in pieces. The node must open once enough has come, not
 * on the first write and not never. */
TEST a_clip_written_in_pieces_still_plays(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    size_t size = 0u;
    unsigned char *bytes = slurp(CLIP_VP8, &size);
    size_t at;

    ASSERT(bytes != NULL);
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 320, 240));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));

    /* Nothing written yet, so nothing is waiting. */
    ASSERT_EQ(0u, schultz_video_queued(f.tree, node));

    /* Two hundred bytes at a time, with the clock running between, which is
     * what a socket looks like. */
    for (at = 0; at < size; at += 200u) {
        size_t piece = (size - at < 200u) ? size - at : 200u;

        ASSERT_EQ(SCHULTZ_OK, schultz_video_write(f.tree, node, bytes + at,
                                                  piece));
        clock_ms += 1u;
        schultz_tree_advance(f.tree, clock_ms);
    }
    free(bytes);

    /*
     * Written faster than it is read, so there is a backlog. This is the
     * number a host streaming from a socket watches to decide when to stop
     * reading ahead.
     */
    ASSERT(schultz_video_queued(f.tree, node) > 0u);

    run_for(&f, 2500u, 33u);
    ASSERT_EQ((uint32_t)SCHULTZ_VIDEO_CODEC_VP8,
              schultz_video_codec(f.tree, node));
    /*
     * Thirty exactly: two seconds at fifteen a second, every frame once. A
     * stream that is still arriving has to be reopened as it grows, and
     * reopening seeks back to the keyframe before where it had got to, so
     * getting more than thirty here would mean frames were being shown twice.
     */
    ASSERT_EQ(30u, schultz_video_frames_shown(f.tree, node));

    /* And once it has all been read, nothing is waiting again. */
    ASSERT_EQ(0u, schultz_video_queued(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

TEST writing_refuses_what_it_should(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle plain = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));

    /* A null with a length is a mistake; a null with no length is not. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_write(f.tree, node, NULL, 10u));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_write(f.tree, node, NULL, 0u));

    /* A node that is not a video node is not one. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree,
                              schultz_tree_root(f.tree), &plain));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_video_write(f.tree, plain, "xx", 2u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_video_play(f.tree, plain));
    ASSERT_EQ(0u, schultz_video_frames_shown(f.tree, plain));

    fixture_teardown(&f);
    PASS();
}

/* ----------------------------------------------------------------- drawing */

TEST a_video_node_draws_its_frame_and_keeps_its_bounds(void)
{
    video_fixture f;
    schultz_handle node;
    schultz_rect before;
    schultz_rect after;
    uint32_t i;
    uint32_t images = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = play_clip(&f, CLIP_VP8);
    ASSERT(node != SCHULTZ_HANDLE_NONE);
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, node, &before));

    run_for(&f, 300u, 33u);
    ASSERT(schultz_video_frames_shown(f.tree, node) > 0u);

    schultz_widget_paint_tree(f.tree, &f.list, &f.arena,
                              schultz_rect_make(0, 0, 0, 0));
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        if (schultz_draw_list_at(&f.list, i)->kind ==
                (uint32_t)SCHULTZ_DRAW_IMAGE) {
            images++;
        }
    }
    ASSERT_EQ(1u, images);

    /* Frames arriving must not move the node: layout never learns about
     * video at all. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, node, &after));
    ASSERT_EQ(before.x, after.x);
    ASSERT_EQ(before.y, after.y);
    ASSERT_EQ(before.width, after.width);
    ASSERT_EQ(before.height, after.height);

    fixture_teardown(&f);
    PASS();
}

TEST a_node_with_no_frame_yet_draws_nothing(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t i;
    uint32_t images = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 320, 240));
    schultz_tree_resolve_styles(f.tree);

    schultz_widget_paint_tree(f.tree, &f.list, &f.arena,
                              schultz_rect_make(0, 0, 0, 0));
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        if (schultz_draw_list_at(&f.list, i)->kind ==
                (uint32_t)SCHULTZ_DRAW_IMAGE) {
            images++;
        }
    }
    ASSERT_EQ(0u, images);

    fixture_teardown(&f);
    PASS();
}

/*
 * A film is thirty pictures a second and each one is registered in the image
 * table. If the one before it were not released the table would grow for as
 * long as the clip plays, which on an hour of video is a hundred thousand
 * handles. This is the test that catches that.
 */
TEST playing_does_not_grow_the_image_table(void)
{
    video_fixture f;
    schultz_handle node;
    uint32_t early;
    uint32_t late;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = play_clip(&f, CLIP_VP8);
    ASSERT(node != SCHULTZ_HANDLE_NONE);

    run_for(&f, 300u, 33u);
    early = schultz_image_count(f.images);
    ASSERT(schultz_video_frames_shown(f.tree, node) > 0u);

    run_for(&f, 1500u, 33u);
    late = schultz_image_count(f.images);
    ASSERT(schultz_video_frames_shown(f.tree, node) > 20u);

    /* Twenty more frames drawn, and not one more picture held. */
    ASSERT_EQ(early, late);

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------- a file, rather than a stream */

/* A node given a file reads it itself, and knows what it holds straight away
 * rather than after the first turn. */
TEST a_file_opened_by_name_plays(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t width = 0u;
    uint32_t height = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_VP9));
    ASSERT_EQ(SCHULTZ_VIDEO_CODEC_VP9, schultz_video_codec(f.tree, node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_size(f.tree, node, &width, &height));
    ASSERT_EQ(320u, width);
    ASSERT_EQ(240u, height);

    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    run_for(&f, 2200u, 33u);
    ASSERT(schultz_video_frames_shown(f.tree, node) > 20u);

    fixture_teardown(&f);
    PASS();
}

TEST opening_a_file_that_is_not_there_says_so(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_video_open_file(f.tree, node, "tests/assets/nope.webm"));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_video_open_file(f.tree, node, NULL));
    ASSERT_EQ(SCHULTZ_VIDEO_CODEC_NONE, schultz_video_codec(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

/* Two seconds of film, and the container says so. */
TEST a_file_says_how_long_it_is(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint64_t length;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_VP8));
    length = schultz_video_duration(f.tree, node);
    ASSERT(length >= 1900u);
    ASSERT(length <= 2200u);
    ASSERT_EQ(0u, schultz_video_position(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

/* A file can be moved about in because Schultz still has it. A stream cannot,
 * and says so rather than pretending. */
TEST a_file_can_seek_and_a_stream_cannot(void)
{
    video_fixture f;
    schultz_handle from_file = SCHULTZ_HANDLE_NONE;
    schultz_handle from_bytes;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &from_file));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, from_file, CLIP_VP8));
    ASSERT_EQ(1, schultz_video_can_seek(f.tree, from_file));

    from_bytes = play_clip(&f, CLIP_VP8);
    ASSERT(from_bytes != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(0, schultz_video_can_seek(f.tree, from_bytes));
    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_video_seek(f.tree, from_bytes, 1000u));

    fixture_teardown(&f);
    PASS();
}

/* Seeking moves the position and the film carries on from there. */
TEST seeking_moves_the_position_and_plays_on(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint64_t after_seek;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_VP8));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    run_for(&f, 300u, 33u);
    ASSERT(schultz_video_position(f.tree, node) < 900u);

    ASSERT_EQ(SCHULTZ_OK, schultz_video_seek(f.tree, node, 1500u));
    after_seek = schultz_video_frames_shown(f.tree, node);

    /*
     * At or before what was asked for, never after. A seek lands on the
     * keyframe covering the point, which for these clips is one every third
     * of a second, so 1500 ms lands somewhere in the 1200s.
     */
    run_for(&f, 33u, 33u);
    ASSERT(schultz_video_position(f.tree, node) <= 1600u);
    ASSERT(schultz_video_position(f.tree, node) > 1000u);

    run_for(&f, 400u, 33u);
    ASSERT(schultz_video_frames_shown(f.tree, node) > after_seek);
    ASSERT(schultz_video_position(f.tree, node) > 1300u);

    fixture_teardown(&f);
    PASS();
}

/*
 * Seeking keeps sound and picture together.
 *
 * Asking to move to a point does not move the file to that point: it moves to
 * the keyframe at or before it, because that is the last place a decoder can
 * start. On an ordinary film that can be seconds earlier. Taking the time
 * that was asked for as where the film now is -- which is what this used to
 * do -- leaves every picture reading as seconds overdue against a sound track
 * that started earlier, so the pictures race ahead while the sound plays on.
 *
 * What says it is right is that the position afterwards is where the file
 * actually landed, which is at or before what was asked for and never after.
 */
TEST seeking_puts_the_clock_where_the_file_really_went(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint64_t at;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_SOUND));
    ASSERT_EQ(1, schultz_video_has_sound(f.tree, node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    run_for(&f, 200u, 33u);

    ASSERT_EQ(SCHULTZ_OK, schultz_video_seek(f.tree, node, 1500u));
    /* One turn is enough to read the first packet and learn where it is. */
    run_for(&f, 33u, 33u);
    at = schultz_video_position(f.tree, node);

    /*
     * At or before what was asked for. Before is the keyframe being earlier;
     * after would mean the clock had been set from the request rather than
     * from the file, which is the bug this is here for.
     */
    ASSERT(at <= 1600u);
    /* And it did move: this is well past where it was playing. */
    ASSERT(at > 1000u);

    fixture_teardown(&f);
    PASS();
}

/* Pausing holds the picture where it is; stopping winds back. */
TEST stopping_returns_to_the_beginning(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_VP9));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    run_for(&f, 900u, 33u);
    ASSERT(schultz_video_position(f.tree, node) > 500u);

    ASSERT_EQ(SCHULTZ_OK, schultz_video_stop(f.tree, node));
    ASSERT_EQ(0, schultz_video_is_playing(f.tree, node));
    ASSERT_EQ(0u, schultz_video_position(f.tree, node));

    /* And it plays again from the top rather than from where it was. */
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    run_for(&f, 300u, 33u);
    ASSERT(schultz_video_position(f.tree, node) < 900u);

    fixture_teardown(&f);
    PASS();
}

/* A file plays from its first picture to its last and stops there. */
TEST a_file_plays_all_the_way_to_the_end(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint64_t total;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_VP9));
    total = schultz_video_duration(f.tree, node);
    ASSERT(total > 1900u);

    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    /* Half again as long as the film, so the end is well past. */
    run_for(&f, (uint32_t)total + (uint32_t)total / 2u, 33u);

    /* Thirty frames is the whole of a two second film at fifteen a second. */
    ASSERT_EQ(30u, schultz_video_frames_shown(f.tree, node));
    /* And it holds the last one rather than starting again. */
    run_for(&f, 500u, 33u);
    ASSERT_EQ(30u, schultz_video_frames_shown(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------- with sound */

TEST a_clip_with_sound_says_so(void)
{
    video_fixture f;
    schultz_handle loud = SCHULTZ_HANDLE_NONE;
    schultz_handle quiet = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT(f.audio != NULL);

    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &loud));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, loud, CLIP_SOUND));
    ASSERT_EQ(1, schultz_video_has_sound(f.tree, loud));

    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &quiet));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, quiet, CLIP_VP8));
    ASSERT_EQ(0, schultz_video_has_sound(f.tree, quiet));

    fixture_teardown(&f);
    PASS();
}

/*
 * The sound is the clock. The device consumes what it is given at the rate it
 * would play it, so the position rises with real time rather than with how
 * fast the tree is advanced -- and advancing the tree far faster than real
 * time, as this does, must not run the film fast.
 */
TEST sound_drives_the_clock(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t turns;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 1u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_SOUND));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));

    /* Ten frames is two thirds of a second of film at fifteen a second. */
    for (turns = 0; turns < 4000u; turns++) {
        if (schultz_video_frames_shown(f.tree, node) >= 10u) {
            break;
        }
        clock_ms += 33u;
        schultz_tree_advance(f.tree, clock_ms);
        SDL_Delay(1);
    }
    ASSERT(schultz_video_frames_shown(f.tree, node) >= 10u);
    /*
     * The tree was advanced 33 ms a turn but only slept 1 ms, so reaching ten
     * frames on the tree's clock alone would take about twenty turns. Needing
     * many more is the proof that the sound is what is being counted.
     */
    ASSERT(turns > 200u);
    ASSERT(schultz_video_position(f.tree, node) > 400u);
    ASSERT(schultz_video_position(f.tree, node) < 1500u);

    fixture_teardown(&f);
    PASS();
}

/*
 * The sound has to be decoded further ahead than the pictures are.
 *
 * Two decoded pictures is eighty milliseconds at twenty four a second. A
 * decoder that stopped there gave the device that much and no more, and the
 * first time anything held the frame loop up for longer than that, the device
 * ran dry. A stream that has run dry stops moving, and the sound is the
 * clock, so the film lost exactly the time it was starved for.
 *
 * So this holds the loop up on purpose, for longer than the old buffer held
 * and less than the new one does, and asks afterwards whether the film is
 * still where real time says it should be.
 */
TEST the_sound_survives_a_stall_in_the_frame_loop(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint64_t started;
    uint64_t ran;
    uint64_t at;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_SOUND));
    ASSERT_EQ(1, schultz_video_has_sound(f.tree, node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));

    started = SDL_GetTicks();
    while (SDL_GetTicks() - started < 500u) {
        clock_ms += 16u;
        schultz_tree_advance(f.tree, clock_ms);
        SDL_Delay(16);
    }

    /* The machine got busy. Nothing is advanced and nothing is written. */
    SDL_Delay(250);

    while (SDL_GetTicks() - started < 1200u) {
        clock_ms += 16u;
        schultz_tree_advance(f.tree, clock_ms);
        SDL_Delay(16);
    }
    ran = SDL_GetTicks() - started;
    at  = schultz_video_position(f.tree, node);

    /*
     * Within a tenth of a second of real time. A film whose sound ran out
     * during the stall comes back a quarter of a second short, which is what
     * this is here to catch.
     */
    ASSERT(at + 100u >= ran);
    ASSERT(at <= ran + 100u);

    fixture_teardown(&f);
    PASS();
}

/* Muting is not the volume set to zero: unmuting has to remember. */
TEST muting_keeps_the_volume_it_was_set_to(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_SOUND));

    ASSERT_EQ(1.0f, schultz_video_volume(f.tree, node));
    ASSERT_EQ(0, schultz_video_is_muted(f.tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_volume(f.tree, node, 0.4f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_muted(f.tree, node, 1));
    ASSERT_EQ(1, schultz_video_is_muted(f.tree, node));
    ASSERT_EQ(0.4f, schultz_video_volume(f.tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_muted(f.tree, node, 0));
    ASSERT_EQ(0, schultz_video_is_muted(f.tree, node));
    ASSERT_EQ(0.4f, schultz_video_volume(f.tree, node));

    /* And it is held to what a volume can be. */
    schultz_video_set_volume(f.tree, node, 4.0f);
    ASSERT_EQ(1.0f, schultz_video_volume(f.tree, node));
    schultz_video_set_volume(f.tree, node, -1.0f);
    ASSERT_EQ(0.0f, schultz_video_volume(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

/*
 * A host may build its widgets before it builds its sound system, and on a
 * good many it will: the window and its tree come first, and sound is set up
 * when something wants to make a noise. Reading the tree once when the node
 * was made turned that ordinary order into a silent film forever.
 */
TEST a_sound_system_set_after_the_node_is_still_found(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint64_t started;
    uint64_t at;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT(f.audio != NULL);

    /* The node first, with no sound system anywhere. */
    schultz_tree_set_audio(f.tree, NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_SOUND));
    ASSERT_EQ(0, schultz_video_has_sound(f.tree, node));

    /* And the sound system afterwards, which is the whole point. */
    schultz_tree_set_audio(f.tree, f.audio);
    ASSERT_EQ(1, schultz_video_has_sound(f.tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));

    /*
     * In real time, not by winding the clock on. Sound is the clock once
     * there is sound, and pictures are shown against where the sound has
     * actually got to, so a loop that advances the tree instantly leaves the
     * sound where it started and almost nothing is shown. That this test had
     * to change shape is itself the fix working: before it, this film ran on
     * the tree's clock because it believed it had no sound.
     */
    started = SDL_GetTicks();
    while (SDL_GetTicks() - started < 400u) {
        clock_ms += 16u;
        schultz_tree_advance(f.tree, clock_ms);
        SDL_Delay(16);
    }

    /* Still true once a stream exists, and the film got somewhere. */
    ASSERT_EQ(1, schultz_video_has_sound(f.tree, node));
    at = schultz_video_position(f.tree, node);
    ASSERT(at > 0u);
    ASSERT(schultz_video_frames_shown(f.tree, node) > 5u);

    fixture_teardown(&f);
    PASS();
}

/* A tree with no sound system plays the same film silently rather than
 * refusing it. */
TEST a_film_with_no_sound_system_still_plays(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_tree_set_audio(f.tree, NULL);
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_SOUND));
    ASSERT_EQ(0, schultz_video_has_sound(f.tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    run_for(&f, 1000u, 33u);
    ASSERT(schultz_video_frames_shown(f.tree, node) > 5u);

    fixture_teardown(&f);
    PASS();
}

/*
 * A film that is not playing and shows no controls draws its first picture
 * and then asks to be left alone. A node that kept being ticked would decode
 * nothing and show nothing, and still cost a turn of the frame loop for as
 * long as the window was open.
 */
TEST a_stopped_film_shows_one_picture_then_goes_quiet(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t turns;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_VP8));
    ASSERT_EQ(0, schultz_video_is_playing(f.tree, node));
    /* Opening is enough to get it ticked, without playing it. */
    ASSERT_EQ(1, schultz_node_animating(f.tree, node));

    for (turns = 0; turns < 200u; turns++) {
        if (schultz_video_frames_shown(f.tree, node) > 0u) {
            break;
        }
        clock_ms += 16u;
        schultz_tree_advance(f.tree, clock_ms);
    }
    ASSERT_EQ(1u, schultz_video_frames_shown(f.tree, node));
    ASSERT_EQ(0, schultz_node_animating(f.tree, node));

    /* And it stays on that picture rather than creeping forward. */
    run_for(&f, 1000u, 16u);
    ASSERT_EQ(1u, schultz_video_frames_shown(f.tree, node));

    /* Playing asks for the clock back. */
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    ASSERT_EQ(1, schultz_node_animating(f.tree, node));
    run_for(&f, 700u, 33u);
    ASSERT(schultz_video_frames_shown(f.tree, node) > 5u);

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------- controls */

/* Lays the node out where a real pane would, so its controls have bounds. */
static void place(video_fixture *f, schultz_handle node, float width,
                  float height)
{
    schultz_node_set_bounds(f->tree, node,
                            schultz_rect_make(0, 0, width, height));
    schultz_tree_resolve_styles(f->tree);
    schultz_layout_arrange(f->tree, node,
                           schultz_rect_make(0, 0, width, height));
}

/* Press and release in the middle of a node, which is a click. */
static void click(video_fixture *f, schultz_handle node)
{
    schultz_rect bounds;
    schultz_point at;

    schultz_node_absolute_bounds(f->tree, node, &bounds);
    at = schultz_point_make(bounds.x + bounds.width * 0.5f,
                            bounds.y + bounds.height * 0.5f);
    schultz_events_mouse_move(f->events, at, 0);
    schultz_events_mouse_button(f->events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_button(f->events, at, SCHULTZ_BUTTON_LEFT, 0, 0);
}


/* Counts the row's children, which is how many controls appeared. */
static uint32_t control_count(video_fixture *f, schultz_handle node)
{
    schultz_handle row = SCHULTZ_HANDLE_NONE;

    if (schultz_node_child_count(f->tree, node) == 0u ||
        schultz_node_child_at(f->tree, node, 0u, &row) != SCHULTZ_OK) {
        return 0u;
    }
    return schultz_node_child_count(f->tree, row);
}

/* None is the default, and asking for all of them puts six in the row. */
TEST controls_appear_only_when_they_are_asked_for(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_VIDEO_CONTROLS_NONE,
              schultz_video_controls(f.tree, node));
    ASSERT_EQ(0u, schultz_node_child_count(f.tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_controls(f.tree, node,
                              SCHULTZ_VIDEO_CONTROLS_ALL));
    ASSERT_EQ(SCHULTZ_VIDEO_CONTROLS_ALL,
              schultz_video_controls(f.tree, node));
    ASSERT_EQ(6u, control_count(&f, node));

    /* A smaller set rebuilds the row rather than adding to it. */
    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_controls(f.tree, node,
                              SCHULTZ_VIDEO_CONTROL_PLAY |
                              SCHULTZ_VIDEO_CONTROL_MUTE));
    ASSERT_EQ(2u, control_count(&f, node));
    ASSERT_EQ(1u, schultz_node_child_count(f.tree, node));

    /* And none takes the row away entirely. */
    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_controls(f.tree, node,
                              SCHULTZ_VIDEO_CONTROLS_NONE));
    ASSERT_EQ(0u, schultz_node_child_count(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

/* Finds the row's first child, which the tests below know the kind of. */
static schultz_handle control_at(video_fixture *f, schultz_handle node,
                                 uint32_t index)
{
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;

    if (schultz_node_child_at(f->tree, node, 0u, &row) != SCHULTZ_OK ||
        schultz_node_child_at(f->tree, row, index, &child) != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    return child;
}

/* The play button is a button, and clicking it plays and pauses. */
TEST the_play_button_plays_and_pauses(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle button;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_VP8));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_controls(f.tree, node,
                              SCHULTZ_VIDEO_CONTROL_PLAY));
    button = control_at(&f, node, 0u);
    ASSERT(button != SCHULTZ_HANDLE_NONE);
    place(&f, node, 320.0f, 240.0f);

    click(&f, button);
    ASSERT_EQ(1, schultz_video_is_playing(f.tree, node));
    click(&f, button);
    ASSERT_EQ(0, schultz_video_is_playing(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

/* The bar follows the film, and refuses a drag when the film is a stream. */
TEST the_position_bar_follows_the_film(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle bar;
    schultz_handle streamed;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_VP8));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_controls(f.tree, node,
                              SCHULTZ_VIDEO_CONTROL_POSITION));
    bar = control_at(&f, node, 0u);
    ASSERT(bar != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(0.0f, schultz_slider_value(f.tree, bar));

    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    run_for(&f, 1000u, 33u);
    /* About half of a two second film, as a thousandth of the whole. */
    ASSERT(schultz_slider_value(f.tree, bar) > 300.0f);
    ASSERT(schultz_slider_value(f.tree, bar) < 700.0f);

    /* Over a stream the same bar is there but takes no input. */
    streamed = play_clip(&f, CLIP_VP8);
    ASSERT(streamed != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_controls(f.tree, streamed,
                              SCHULTZ_VIDEO_CONTROL_POSITION));
    bar = control_at(&f, streamed, 0u);
    ASSERT(bar != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(0u,
              schultz_node_get_state(f.tree, bar) & SCHULTZ_STATE_ENABLED);

    fixture_teardown(&f);
    PASS();
}

/* Dragging the bar moves the film. */
TEST dragging_the_bar_moves_the_film(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle bar;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_VP9));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_controls(f.tree, node,
                              SCHULTZ_VIDEO_CONTROL_POSITION));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    run_for(&f, 200u, 33u);
    bar = control_at(&f, node, 0u);

    /* Three quarters of the way along a two second film, landing on the
     * keyframe at or before it. */
    schultz_slider_set_value(f.tree, bar, 750.0f);
    run_for(&f, 66u, 33u);
    ASSERT(schultz_video_position(f.tree, node) > 1000u);
    ASSERT(schultz_video_position(f.tree, node) <= 1600u);

    fixture_teardown(&f);
    PASS();
}

/* The mute button silences the film and gives the sound back. */
TEST the_mute_button_silences_and_restores(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle button;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_SOUND));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_controls(f.tree, node,
                              SCHULTZ_VIDEO_CONTROL_MUTE));
    button = control_at(&f, node, 0u);
    ASSERT(button != SCHULTZ_HANDLE_NONE);
    place(&f, node, 320.0f, 240.0f);

    click(&f, button);
    ASSERT_EQ(1, schultz_video_is_muted(f.tree, node));
    click(&f, button);
    ASSERT_EQ(0, schultz_video_is_muted(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

/*
 * A player at rest shows the opening picture rather than a hole. Decoding
 * starts when the film is opened, not when play is pressed.
 */
TEST a_film_not_yet_playing_shows_its_first_picture(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t turns;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 1u, &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_open_file(f.tree, node, CLIP_VP9));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_set_controls(f.tree, node,
                              SCHULTZ_VIDEO_CONTROLS_ALL));
    ASSERT_EQ(0, schultz_video_is_playing(f.tree, node));

    for (turns = 0; turns < 500u; turns++) {
        if (schultz_video_frames_shown(f.tree, node) > 0u) {
            break;
        }
        clock_ms += 16u;
        schultz_tree_advance(f.tree, clock_ms);
        SDL_Delay(1);
    }
    ASSERT_EQ(1u, schultz_video_frames_shown(f.tree, node));
    ASSERT_EQ(0, schultz_video_is_playing(f.tree, node));
    /* And it stays on that one, because the clock has not moved. */
    run_for(&f, 500u, 33u);
    ASSERT_EQ(1u, schultz_video_frames_shown(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------ on its own thread */

/*
 * Advances the tree until the film has shown as many frames as asked for,
 * and answers how many turns that took.
 *
 * A decode thread works when the operating system runs it, not when the tree
 * is advanced, so a test that steps the clock as fast as it can would ask for
 * pictures nobody has made yet. The short sleep is what gives the thread its
 * turn. The count of turns is the interesting part: the clock moves by
 * `step_ms` each time, so the turns taken say how much film time had to pass.
 */
static uint32_t turns_to_show(video_fixture *f, schultz_handle node,
                              uint64_t want, uint32_t step_ms)
{
    uint32_t turns;

    for (turns = 0; turns < 4000u; turns++) {
        if (schultz_video_frames_shown(f->tree, node) >= want) {
            break;
        }
        clock_ms += step_ms;
        schultz_tree_advance(f->tree, clock_ms);
        SDL_Delay(1);
    }
    return turns;
}

/*
 * The whole clip, decoded somewhere else, and still at its own speed. Thirty
 * frames at fifteen a second is two seconds of film, so at 33 ms a turn it
 * cannot arrive in fewer than about sixty turns however fast the thread is.
 */
TEST a_clip_decodes_on_its_own_thread(void)
{
    video_fixture f;
    schultz_handle node;
    uint32_t turns;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = play_clip_on(&f, CLIP_VP8, 1u);
    ASSERT(node != SCHULTZ_HANDLE_NONE);

    turns = turns_to_show(&f, node, 30u, 33u);
    ASSERT_EQ(30u, schultz_video_frames_shown(f.tree, node));
    ASSERT(turns >= 55u);
    ASSERT_EQ(SCHULTZ_VIDEO_CODEC_VP8, schultz_video_codec(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

/*
 * Pausing stops the clock, so nothing new is shown however far ahead the
 * thread has decoded.
 */
TEST pausing_a_threaded_clip_holds_the_frame(void)
{
    video_fixture f;
    schultz_handle node;
    uint64_t at_pause;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = play_clip_on(&f, CLIP_VP9, 1u);
    ASSERT(node != SCHULTZ_HANDLE_NONE);

    turns_to_show(&f, node, 5u, 33u);
    ASSERT_EQ(SCHULTZ_OK, schultz_video_pause(f.tree, node));
    at_pause = schultz_video_frames_shown(f.tree, node);
    ASSERT(at_pause >= 5u);

    run_for(&f, 1000u, 33u);
    SDL_Delay(50);
    run_for(&f, 33u, 33u);
    ASSERT_EQ(at_pause, schultz_video_frames_shown(f.tree, node));
    ASSERT_EQ(0, schultz_video_is_playing(f.tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    turns_to_show(&f, node, at_pause + 5u, 33u);
    ASSERT(schultz_video_frames_shown(f.tree, node) >= at_pause + 5u);

    fixture_teardown(&f);
    PASS();
}

/*
 * Destroying a node mid-film ends its thread. A thread asleep on a full set
 * of slots has to be woken to see it is meant to stop, and if it is not this
 * test never finishes.
 */
TEST destroying_a_playing_node_ends_its_thread(void)
{
    video_fixture f;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = play_clip_on(&f, CLIP_VP8, 1u);
    ASSERT(node != SCHULTZ_HANDLE_NONE);

    turns_to_show(&f, node, 3u, 33u);
    schultz_node_destroy(f.tree, node);
    ASSERT_EQ(NULL, schultz_node_widget(f.tree, node));

    fixture_teardown(&f);
    PASS();
}

/*
 * Asking for more than one hands the extra to the decoder rather than
 * starting a second reader, and the result is the same film.
 */
TEST asking_for_several_threads_still_plays_the_clip(void)
{
    video_fixture f;
    schultz_handle node;
    uint32_t width = 0u;
    uint32_t height = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    node = play_clip_on(&f, CLIP_VP9, 4u);
    ASSERT(node != SCHULTZ_HANDLE_NONE);

    turns_to_show(&f, node, 30u, 33u);
    ASSERT_EQ(30u, schultz_video_frames_shown(f.tree, node));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_size(f.tree, node, &width, &height));
    ASSERT_EQ(320u, width);
    ASSERT_EQ(240u, height);

    fixture_teardown(&f);
    PASS();
}

/*
 * A pushed stream lets go of what has been decoded, keeping the header and
 * everything from the oldest picture still waiting. Without it the buffer is
 * everything the host ever wrote, and a source that never ends grows until
 * the machine stops the program.
 */
TEST a_stream_lets_go_of_what_has_been_decoded(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    size_t size = 0u;
    unsigned char *bytes = slurp(CLIP_VP8, &size);
    size_t at;
    uint64_t most_held = 0u;

    ASSERT(bytes != NULL);
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 320, 240));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));

    /* Arriving in pieces, the way a socket delivers it. */
    for (at = 0; at < size; at += 200u) {
        size_t piece = (size - at < 200u) ? size - at : 200u;

        ASSERT_EQ(SCHULTZ_OK,
                  schultz_video_write(f.tree, node, bytes + at, piece));
        clock_ms += 1u;
        schultz_tree_advance(f.tree, clock_ms);
        if (schultz_video_held(f.tree, node) > most_held) {
            most_held = schultz_video_held(f.tree, node);
        }
    }
    free(bytes);

    /* And then played through, which is when most of it is finished with. */
    run_for(&f, 2500u, 33u);

    /* It really played, or holding little would prove nothing. */
    ASSERT(schultz_video_frames_shown(f.tree, node) > 0u);

    /*
     * The whole clip went through it, and it never held all of it at once.
     * A file reader holds nothing at all, which is what zero would mean.
     */
    /*
     * Every frame, so nothing was let go of that was still wanted. This is
     * the number that broke while this was being built: trimming by where
     * the demuxer had read rather than by what it had decoded lost ten of
     * these, then twenty.
     */
    ASSERT_EQ(30u, schultz_video_frames_shown(f.tree, node));

    /*
     * And it is holding less than it was given. This test writes the whole
     * clip before playing it, so the most it ever held is the whole thing;
     * what matters is that playing it back gave the memory up again. A
     * stream that arrives while it plays never reaches that peak at all.
     */
    ASSERT(most_held > 0u);
    ASSERT(schultz_video_held(f.tree, node) < (uint64_t)size);

    fixture_teardown(&f);
    PASS();
}

/*
 * The hard case, and the one every wrong version of the trimming broke.
 *
 * A stream is played until it runs dry, which makes the demuxer give up and
 * be built again from the header when more turns up. By then the middle may
 * have been let go of, so the rebuilt demuxer has to take up at a cluster
 * that is still held rather than hunt through bytes that are gone.
 */
TEST a_stream_that_runs_dry_carries_on_when_more_arrives(void)
{
    video_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    size_t size = 0u;
    unsigned char *bytes = slurp(CLIP_VP8, &size);
    size_t first = 0u;
    uint32_t shown_when_dry;
    uint64_t held_when_dry;

    ASSERT(bytes != NULL);
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 320, 240));

    /* The first part arrives, and is played until there is no more. */
    first = (size * 7u) / 8u;
    ASSERT_EQ(SCHULTZ_OK, schultz_video_write(f.tree, node, bytes, first));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    run_for(&f, 2500u, 33u);

    shown_when_dry = schultz_video_frames_shown(f.tree, node);
    held_when_dry  = schultz_video_held(f.tree, node);
    ASSERT(shown_when_dry > 0u);
    ASSERT(shown_when_dry < 30u);      /* not the whole clip yet */

    /*
     * It let go of what it had played while it was playing, so what follows
     * is a demuxer rebuilt against a buffer with a hole in it rather than a
     * whole one. That is the thing worth testing here.
     */
    ASSERT(held_when_dry < (uint64_t)first);

    /* The rest turns up, after it had given up on there being any. */
    ASSERT_EQ(SCHULTZ_OK, schultz_video_write(f.tree, node, bytes + first,
                                              size - first));
    free(bytes);
    run_for(&f, 2500u, 33u);

    /*
     * Every picture, once each. Fewer would mean the rebuilt demuxer took up
     * past something still to be shown; more would mean it took up behind
     * something already shown and played it twice.
     */
    ASSERT_EQ(30u, schultz_video_frames_shown(f.tree, node));
    ASSERT(schultz_video_frames_shown(f.tree, node) > shown_when_dry);
    ASSERT(schultz_video_held(f.tree, node) < (uint64_t)size);

    fixture_teardown(&f);
    PASS();
}

SUITE(video)
{
    RUN_TEST(a_stream_that_runs_dry_carries_on_when_more_arrives);
    RUN_TEST(a_stream_lets_go_of_what_has_been_decoded);
    RUN_TEST(the_clips_are_where_the_tests_expect_them);
    RUN_TEST(a_clip_reports_its_codec_and_size);
    RUN_TEST(bytes_that_are_not_webm_show_nothing);
    RUN_TEST(a_truncated_clip_decodes_what_arrived);
    RUN_TEST(a_vp8_clip_decodes_more_than_one_frame);
    RUN_TEST(a_vp9_clip_decodes_more_than_one_frame);
    RUN_TEST(a_clip_plays_at_its_own_speed);
    RUN_TEST(pausing_holds_the_frame_and_playing_carries_on);
    RUN_TEST(looping_starts_again_and_not_looping_stops);
    RUN_TEST(a_clip_written_in_pieces_still_plays);
    RUN_TEST(writing_refuses_what_it_should);
    RUN_TEST(a_video_node_draws_its_frame_and_keeps_its_bounds);
    RUN_TEST(a_node_with_no_frame_yet_draws_nothing);
    RUN_TEST(playing_does_not_grow_the_image_table);
    RUN_TEST(a_stopped_film_shows_one_picture_then_goes_quiet);
    RUN_TEST(controls_appear_only_when_they_are_asked_for);
    RUN_TEST(the_play_button_plays_and_pauses);
    RUN_TEST(the_position_bar_follows_the_film);
    RUN_TEST(dragging_the_bar_moves_the_film);
    RUN_TEST(the_mute_button_silences_and_restores);
    RUN_TEST(a_film_not_yet_playing_shows_its_first_picture);
    RUN_TEST(a_clip_with_sound_says_so);
    RUN_TEST(sound_drives_the_clock);
    RUN_TEST(the_sound_survives_a_stall_in_the_frame_loop);
    RUN_TEST(muting_keeps_the_volume_it_was_set_to);
    RUN_TEST(a_film_with_no_sound_system_still_plays);
    RUN_TEST(a_sound_system_set_after_the_node_is_still_found);
    RUN_TEST(a_file_opened_by_name_plays);
    RUN_TEST(opening_a_file_that_is_not_there_says_so);
    RUN_TEST(a_file_says_how_long_it_is);
    RUN_TEST(a_file_can_seek_and_a_stream_cannot);
    RUN_TEST(seeking_moves_the_position_and_plays_on);
    RUN_TEST(seeking_puts_the_clock_where_the_file_really_went);
    RUN_TEST(stopping_returns_to_the_beginning);
    RUN_TEST(a_file_plays_all_the_way_to_the_end);
    RUN_TEST(a_clip_decodes_on_its_own_thread);
    RUN_TEST(pausing_a_threaded_clip_holds_the_frame);
    RUN_TEST(destroying_a_playing_node_ends_its_thread);
    RUN_TEST(asking_for_several_threads_still_plays_the_clip);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    /* Before anything opens a device, and before SDL starts. */
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(video);
    GREATEST_MAIN_END();
}
