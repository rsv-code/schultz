/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_text.c - font loading, shaping, measurement, wrapping, direction.
 *
 * These assert relationships rather than exact pixel values. A shaped width
 * depends on the font build and the rasterizer version, so pinning it to a
 * number would produce a test that fails on an upgrade without anything being
 * wrong. What must hold is that wider text measures wider, that shaping is
 * stable, and that breaking happens where Unicode says it may.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "schultz_font.h"
#include "schultz_font_builtin.h"
#include "schultz_text.h"

#define FONT_PATH "assets/fonts/DejaVuSans.ttf"

typedef struct {
    schultz_font_system *system;
    schultz_arena        arena;
    schultz_handle       font;
} text_fixture;

/*
 * A font system does not start empty: it loads the faces compiled into the
 * library, so a program that says nothing about fonts still draws text. So
 * the counting tests measure what they loaded rather than what exists, and
 * this is the number they measure from.
 */
static uint32_t builtin_fonts(void)
{
    schultz_font_system *system;
    uint32_t count;

    if (schultz_font_system_create(&system) != SCHULTZ_OK) {
        return 0u;
    }
    count = schultz_font_count(system);
    schultz_font_system_destroy(system);
    return count;
}

static int32_t fixture_setup(text_fixture *f, float size_px)
{
    int32_t result = schultz_font_system_create(&f->system);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_arena_init(&f->arena, 0);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_font_load_file(f->system, FONT_PATH, size_px, &f->font);
}

static void fixture_teardown(text_fixture *f)
{
    schultz_arena_free(&f->arena);
    schultz_font_system_destroy(f->system);
}

/* ------------------------------------------------------------------ fonts */

TEST font_loads_from_file(void)
{
    text_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT(f.font != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(builtin_fonts() + 1u, schultz_font_count(f.system));
    ASSERT_EQ(16.0f, schultz_font_size(f.system, f.font));

    fixture_teardown(&f);
    PASS();
}

TEST font_load_rejects_bad_arguments(void)
{
    schultz_font_system *system;
    schultz_handle font = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_font_load_file(system, NULL, 16.0f, &font));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_font_load_file(system, FONT_PATH, 0.0f, &font));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_font_load_file(system, FONT_PATH, -5.0f, &font));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_font_load_file(system, FONT_PATH, 16.0f, NULL));
    /* Nothing loaded, so nothing but what the library brought with it. */
    ASSERT_EQ(builtin_fonts(), schultz_font_count(system));

    schultz_font_system_destroy(system);
    PASS();
}

TEST a_font_system_brings_its_own_faces(void)
{
    schultz_font_system *system;
    schultz_handle body;
    schultz_handle title;
    schultz_handle mono;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));

    /* One per theme font slot, all different, all loaded. */
    body  = schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_BODY);
    title = schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_TITLE);
    mono  = schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_MONO);
    ASSERT(body != SCHULTZ_HANDLE_NONE);
    ASSERT(title != SCHULTZ_HANDLE_NONE);
    ASSERT(mono != SCHULTZ_HANDLE_NONE);
    ASSERT(body != title);
    ASSERT(body != mono);
    ASSERT(title != mono);
    /*
     * Every compiled in face is loaded, not only the ones that fill a slot.
     * More faces than slots, because the two oblique Sans faces complete the
     * family without being a theme font of their own.
     */
    ASSERT_EQ(schultz_font_builtin_count(), schultz_font_count(system));
    ASSERT(schultz_font_builtin_count() > (uint32_t)SCHULTZ_TOKEN_FONT_COUNT);

    /* A token that names no slot answers nothing. */
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_COUNT));
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_font_builtin(NULL, SCHULTZ_TOKEN_FONT_BODY));

    schultz_font_system_destroy(system);
    PASS();
}

/*
 * The family tests below. A Bold button sets a wish rather than a handle, so
 * what has to hold is that the wish finds a real face, that the face is the
 * one asked for, and that asking again gets back where it started.
 */

TEST the_built_in_family_is_complete(void)
{
    schultz_font_system *system;
    schultz_handle body;
    schultz_handle bold;
    schultz_handle italic;
    schultz_handle both;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    body = schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_BODY);

    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, body, 1, 0, &bold));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, body, 0, 1, &italic));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, body, 1, 1, &both));

    /* Four different faces, one for each corner of the family. */
    ASSERT(bold != body);
    ASSERT(italic != body);
    ASSERT(both != body);
    ASSERT(bold != italic);
    ASSERT(bold != both);
    ASSERT(italic != both);

    /* And each is the face it was asked for, by its own account. */
    ASSERT_EQ(0, schultz_font_is_bold(system, body));
    ASSERT_EQ(0, schultz_font_is_italic(system, body));
    ASSERT_EQ(1, schultz_font_is_bold(system, bold));
    ASSERT_EQ(0, schultz_font_is_italic(system, bold));
    ASSERT_EQ(0, schultz_font_is_bold(system, italic));
    ASSERT_EQ(1, schultz_font_is_italic(system, italic));
    ASSERT_EQ(1, schultz_font_is_bold(system, both));
    ASSERT_EQ(1, schultz_font_is_italic(system, both));

    schultz_font_system_destroy(system);
    PASS();
}

TEST the_bold_body_face_is_the_title_face(void)
{
    schultz_font_system *system;
    schultz_handle bold;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    /*
     * The family table is a second way of reaching the faces already loaded,
     * not a second copy of them. The bold member of the body family and the
     * title slot are the same face, so they are the same handle.
     */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_font_at_style(
                  system, schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_BODY),
                  1, 0, &bold));
    ASSERT_EQ(schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_TITLE), bold);

    schultz_font_system_destroy(system);
    PASS();
}

TEST asking_for_the_style_a_face_already_has_gives_it_back(void)
{
    schultz_font_system *system;
    schultz_handle body;
    schultz_handle same;
    schultz_handle both;
    schultz_handle back;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    body = schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_BODY);

    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, body, 0, 0, &same));
    ASSERT_EQ(body, same);

    /* And the way back is the same road. Turning both off returns the face
     * the family was entered from, not merely something plain. */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, body, 1, 1, &both));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, both, 0, 0, &back));
    ASSERT_EQ(body, back);

    schultz_font_system_destroy(system);
    PASS();
}

TEST a_style_lookup_keeps_the_size(void)
{
    schultz_font_system *system;
    schultz_handle body;
    schultz_handle large;
    schultz_handle bold;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    body = schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_BODY);

    /*
     * The built in faces are loaded at one size, so a bold face at 32 does
     * not exist yet. Asking has to make one rather than quietly answer with
     * the 16 pixel bold face, which would draw a heading at body size.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(system, body, 32.0f, &large));
    ASSERT_IN_RANGE(32.0f, schultz_font_size(system, large), 0.01f);

    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, large, 1, 1, &bold));
    ASSERT_IN_RANGE(32.0f, schultz_font_size(system, bold), 0.01f);
    ASSERT_EQ(1, schultz_font_is_bold(system, bold));
    ASSERT_EQ(1, schultz_font_is_italic(system, bold));
    ASSERT(bold != large);

    schultz_font_system_destroy(system);
    PASS();
}

TEST a_family_missing_a_member_answers_with_what_it_has(void)
{
    schultz_font_system *system;
    schultz_handle mono;
    schultz_handle asked;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    mono = schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_MONO);

    /*
     * Only the regular mono face is compiled in. Nothing is synthesized, so
     * the answer is the face itself: readable, and honest about what it is.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, mono, 1, 0, &asked));
    ASSERT_EQ(mono, asked);
    ASSERT_EQ(0, schultz_font_is_bold(system, asked));

    schultz_font_system_destroy(system);
    PASS();
}

TEST bold_italic_falls_back_to_bold_when_that_is_all_there_is(void)
{
    schultz_font_system *system;
    schultz_handle body;
    schultz_handle both;
    schultz_handle asked;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    body = schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_BODY);

    /* Take the bold oblique member away, leaving a family of three. */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, body, 1, 1, &both));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_unload(system, both));

    /*
     * Half of what was asked for beats none of it, and weight carries
     * emphasis further than slant, so what comes back is bold and upright.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, body, 1, 1, &asked));
    ASSERT_EQ(1, schultz_font_is_bold(system, asked));
    ASSERT_EQ(0, schultz_font_is_italic(system, asked));

    schultz_font_system_destroy(system);
    PASS();
}

TEST bold_italic_falls_back_to_italic_when_bold_is_gone(void)
{
    schultz_font_system *system;
    schultz_handle body;
    schultz_handle both;
    schultz_handle bold;
    schultz_handle asked;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    body = schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_BODY);

    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, body, 1, 1, &both));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, body, 1, 0, &bold));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_unload(system, both));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_unload(system, bold));

    /* Nothing bold left anywhere in the family, so the slant is what is
     * left to say it with. */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, body, 1, 1, &asked));
    ASSERT_EQ(0, schultz_font_is_bold(system, asked));
    ASSERT_EQ(1, schultz_font_is_italic(system, asked));

    schultz_font_system_destroy(system);
    PASS();
}

TEST style_lookup_rejects_bad_arguments(void)
{
    schultz_font_system *system;
    schultz_handle body;
    schultz_handle got = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    body = schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_BODY);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_font_at_style(NULL, body, 1, 0, &got));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_font_at_style(system, body, 1, 0, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_font_at_style(system, SCHULTZ_HANDLE_NONE, 1, 0, &got));

    /* Any nonzero counts as a yes, not only one. */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(system, body, 7, -3, &got));
    ASSERT_EQ(1, schultz_font_is_bold(system, got));
    ASSERT_EQ(1, schultz_font_is_italic(system, got));

    /* Neither predicate invents an answer for a font that is not there. */
    ASSERT_EQ(0, schultz_font_is_bold(NULL, body));
    ASSERT_EQ(0, schultz_font_is_italic(system, SCHULTZ_HANDLE_NONE));

    schultz_font_system_destroy(system);
    PASS();
}

TEST the_built_in_mono_face_really_is_fixed_pitch(void)
{
    schultz_font_system *system;
    schultz_arena arena;
    schultz_size narrow;
    schultz_size wide;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 0));

    /*
     * The whole reason to ask for mono is that columns line up, so this
     * checks the face rather than the handle: the narrowest letter and the
     * widest must measure the same.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(system,
        schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_MONO), "i", -1,
        &arena, &narrow));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(system,
        schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_MONO), "m", -1,
        &arena, &wide));
    ASSERT_EQ(narrow.width, wide.width);

    /* And the body face is not, or the mono slot would be pointless. */
    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(system,
        schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_BODY), "i", -1,
        &arena, &narrow));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(system,
        schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_BODY), "m", -1,
        &arena, &wide));
    ASSERT(wide.width > narrow.width);

    schultz_arena_free(&arena);
    schultz_font_system_destroy(system);
    PASS();
}

TEST a_built_in_face_can_be_had_at_another_size(void)
{
    schultz_font_system *system;
    schultz_arena arena;
    schultz_handle mono;
    schultz_handle big = SCHULTZ_HANDLE_NONE;
    schultz_handle again = SCHULTZ_HANDLE_NONE;
    schultz_size small_m;
    schultz_size big_m;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 0));
    mono = schultz_font_builtin(system, SCHULTZ_TOKEN_FONT_MONO);

    /*
     * A built in face has no file to read again, so this is the path that
     * loads it from the bytes it already has. Every widget goes through
     * schultz_font_at_size to honour font.size, so without it the built in
     * faces would silently draw at one size only.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(system, mono, 32.0f, &big));
    ASSERT(big != mono);
    ASSERT_EQ(32.0f, schultz_font_size(system, big));
    ASSERT_EQ(schultz_font_builtin_count() + 1u,
              schultz_font_count(system));

    /* Asked for twice, loaded once, and it is a family rather than a chain. */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(system, mono, 32.0f, &again));
    ASSERT_EQ(big, again);
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(system, big, 16.0f, &again));
    ASSERT_EQ(mono, again);
    ASSERT_EQ(schultz_font_builtin_count() + 1u,
              schultz_font_count(system));

    /* Twice the size, twice the advance, so it really is a bigger face. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_measure(system, mono, "m", -1, &arena, &small_m));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_measure(system, big, "m", -1, &arena, &big_m));
    ASSERT(big_m.width > small_m.width);

    schultz_arena_free(&arena);
    schultz_font_system_destroy(system);
    PASS();
}

TEST a_face_can_be_handed_over_as_bytes(void)
{
    schultz_font_system *system;
    schultz_arena arena;
    unsigned char *bytes = NULL;
    long length = 0;
    FILE *file;
    schultz_handle font = SCHULTZ_HANDLE_NONE;
    schultz_handle big = SCHULTZ_HANDLE_NONE;
    schultz_size from_bytes;
    schultz_size from_file;
    schultz_handle path_font = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 0));

    file = fopen(FONT_PATH, "rb");
    ASSERT(file != NULL);
    ASSERT_EQ(0, fseek(file, 0, SEEK_END));
    length = ftell(file);
    ASSERT(length > 0);
    ASSERT_EQ(0, fseek(file, 0, SEEK_SET));
    bytes = (unsigned char *)malloc((size_t)length);
    ASSERT(bytes != NULL);
    ASSERT_EQ((size_t)length, fread(bytes, 1u, (size_t)length, file));
    fclose(file);

    ASSERT_EQ(SCHULTZ_OK, schultz_font_load_memory(system, bytes,
                                                   (size_t)length, 16.0f,
                                                   &font));
    /* Copied, so the caller's buffer is its own business from here. */
    memset(bytes, 0, (size_t)length);
    free(bytes);

    ASSERT_EQ(16.0f, schultz_font_size(system, font));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_load_file(system, FONT_PATH, 16.0f,
                                                 &path_font));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_measure(system, font, "Hello", -1, &arena,
                                   &from_bytes));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_measure(system, path_font, "Hello", -1, &arena,
                                   &from_file));
    ASSERT_EQ(from_file.width, from_bytes.width);

    /* And it can be had at another size, from the copy it kept. */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(system, font, 24.0f, &big));
    ASSERT(big != font);
    ASSERT_EQ(24.0f, schultz_font_size(system, big));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_font_load_memory(system, NULL, 16u, 16.0f, &font));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_font_load_memory(system, "not a font", 0u, 16.0f,
                                       &font));
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_font_load_memory(system, "not a font", 10u, 16.0f,
                                       &font));

    schultz_arena_free(&arena);
    schultz_font_system_destroy(system);
    PASS();
}

TEST font_load_rejects_a_missing_file(void)
{
    schultz_font_system *system;
    schultz_handle font = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_font_load_file(system, "no/such/font.ttf", 16.0f,
                                     &font));
    schultz_font_system_destroy(system);
    PASS();
}

TEST font_load_rejects_a_file_that_is_not_a_font(void)
{
    schultz_font_system *system;
    schultz_handle font = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_font_load_file(system, "Makefile", 16.0f, &font));
    schultz_font_system_destroy(system);
    PASS();
}

TEST unloaded_font_handle_goes_stale(void)
{
    text_fixture f;
    schultz_font_metrics metrics;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_unload(f.system, f.font));
    ASSERT_EQ(builtin_fonts(), schultz_font_count(f.system));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_font_get_metrics(f.system, f.font, &metrics));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_font_unload(f.system, f.font));
    ASSERT_EQ(0.0f, schultz_font_size(f.system, f.font));

    fixture_teardown(&f);
    PASS();
}

TEST metrics_are_sane_and_scale_with_size(void)
{
    text_fixture small;
    text_fixture large;
    schultz_font_metrics ms;
    schultz_font_metrics ml;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&small, 12.0f));
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&large, 48.0f));

    ASSERT_EQ(SCHULTZ_OK, schultz_font_get_metrics(small.system, small.font,
                                                   &ms));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_get_metrics(large.system, large.font,
                                                   &ml));

    ASSERT(ms.ascent > 0.0f);
    ASSERT(ms.descent > 0.0f);
    ASSERT(ms.line_height > 0.0f);

    /*
     * Deliberately not asserting line_height >= ascent + descent. Fonts may
     * specify negative leading, and DejaVu does: measured here, ascent plus
     * descent exceeds line height by one pixel at 12, 32 and 48 px, and
     * matches exactly at 16. Layout must use line_height for baseline to
     * baseline spacing rather than deriving it from ascent and descent.
     */
    ASSERT(ms.line_height >= ms.ascent);

    ASSERT(ml.ascent > ms.ascent);
    ASSERT(ml.descent > ms.descent);
    ASSERT(ml.line_height > ms.line_height);
    ASSERT(ml.underline_thickness > 0.0f);

    fixture_teardown(&small);
    fixture_teardown(&large);
    PASS();
}

/* ---------------------------------------------------------------- shaping */

TEST shaping_produces_one_glyph_per_simple_letter(void)
{
    text_fixture f;
    schultz_text_run run;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_shape(f.system, f.font, "Schultz", -1,
                                 SCHULTZ_DIR_AUTO, &f.arena, &run));

    ASSERT_EQ(7u, run.count);
    ASSERT(run.glyphs != NULL);
    ASSERT(run.width > 0.0f);
    ASSERT(run.height > 0.0f);
    ASSERT_EQ(SCHULTZ_DIR_LTR, run.direction);

    fixture_teardown(&f);
    PASS();
}

TEST shaped_glyphs_advance_left_to_right(void)
{
    text_fixture f;
    schultz_text_run run;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_shape(f.system, f.font, "abcdef", -1,
                                 SCHULTZ_DIR_LTR, &f.arena, &run));

    ASSERT_EQ(6u, run.count);
    ASSERT_EQ(0.0f, run.glyphs[0].x);
    for (i = 1; i < run.count; i++) {
        ASSERT(run.glyphs[i].x > run.glyphs[i - 1].x);
    }
    ASSERT(run.width > run.glyphs[run.count - 1].x);

    fixture_teardown(&f);
    PASS();
}

TEST empty_string_shapes_to_nothing(void)
{
    text_fixture f;
    schultz_text_run run;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_shape(f.system, f.font, "", -1, SCHULTZ_DIR_AUTO,
                                 &f.arena, &run));
    ASSERT_EQ(0u, run.count);
    ASSERT_EQ(0.0f, run.width);
    /* Height is still the font's line height: an empty line occupies space. */
    ASSERT(run.height > 0.0f);

    fixture_teardown(&f);
    PASS();
}

TEST shaping_honors_an_explicit_length(void)
{
    text_fixture f;
    schultz_text_run whole;
    schultz_text_run part;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_shape(f.system, f.font, "abcdef", -1,
                                 SCHULTZ_DIR_LTR, &f.arena, &whole));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_shape(f.system, f.font, "abcdef", 3,
                                 SCHULTZ_DIR_LTR, &f.arena, &part));

    ASSERT_EQ(3u, part.count);
    ASSERT(part.width < whole.width);

    fixture_teardown(&f);
    PASS();
}

TEST shaping_rejects_bad_arguments(void)
{
    text_fixture f;
    schultz_text_run run;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_text_shape(f.system, f.font, NULL, -1,
                                 SCHULTZ_DIR_AUTO, &f.arena, &run));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_text_shape(f.system, f.font, "x", -1, SCHULTZ_DIR_AUTO,
                                 NULL, &run));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_text_shape(f.system, f.font, "x", -1, SCHULTZ_DIR_AUTO,
                                 &f.arena, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_text_shape(f.system, SCHULTZ_HANDLE_NONE, "x", -1,
                                 SCHULTZ_DIR_AUTO, &f.arena, &run));

    fixture_teardown(&f);
    PASS();
}

/*
 * Kerning is the visible proof that shaping ran rather than a per character
 * advance loop being summed. "AV" kerns tighter than the two glyphs measured
 * apart, in any font with kern data, and DejaVu has it.
 */
TEST shaping_applies_kerning(void)
{
    text_fixture f;
    schultz_size pair;
    schultz_size a;
    schultz_size v;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 64.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(f.system, f.font, "AV", -1,
                                               &f.arena, &pair));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(f.system, f.font, "A", -1,
                                               &f.arena, &a));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(f.system, f.font, "V", -1,
                                               &f.arena, &v));

    ASSERT(pair.width < a.width + v.width);

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------ measurement */

TEST measurement_grows_with_text_and_size(void)
{
    text_fixture small;
    text_fixture large;
    schultz_size short_text;
    schultz_size long_text;
    schultz_size big_text;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&small, 16.0f));
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&large, 32.0f));

    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(small.system, small.font,
                                               "Hi", -1, &small.arena,
                                               &short_text));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(small.system, small.font,
                                               "Hi there", -1, &small.arena,
                                               &long_text));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(large.system, large.font,
                                               "Hi", -1, &large.arena,
                                               &big_text));

    ASSERT(long_text.width > short_text.width);
    ASSERT(big_text.width > short_text.width);
    ASSERT(big_text.height > short_text.height);

    fixture_teardown(&small);
    fixture_teardown(&large);
    PASS();
}

TEST measurement_is_repeatable(void)
{
    text_fixture f;
    schultz_size first;
    schultz_size second;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(f.system, f.font,
                                               "layout depends on this", -1,
                                               &f.arena, &first));
    schultz_arena_reset(&f.arena);
    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(f.system, f.font,
                                               "layout depends on this", -1,
                                               &f.arena, &second));
    ASSERT_EQ(first.width, second.width);
    ASSERT_EQ(first.height, second.height);

    fixture_teardown(&f);
    PASS();
}

/* -------------------------------------------------------------- direction */

TEST base_direction_of_latin_is_ltr(void)
{
    ASSERT_EQ(SCHULTZ_DIR_LTR, schultz_text_base_direction("Hello", -1));
    PASS();
}

/* Hebrew: the paragraph direction must come out right to left. */
TEST base_direction_of_hebrew_is_rtl(void)
{
    ASSERT_EQ(SCHULTZ_DIR_RTL,
              schultz_text_base_direction("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d",
                                          -1));
    PASS();
}

/* Arabic, likewise. */
TEST base_direction_of_arabic_is_rtl(void)
{
    ASSERT_EQ(SCHULTZ_DIR_RTL,
              schultz_text_base_direction("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7",
                                          -1));
    PASS();
}

/*
 * Rule P2 of the bidirectional algorithm: the paragraph direction comes from
 * the first strong character, so leading digits and punctuation do not decide
 * it.
 */
TEST base_direction_uses_the_first_strong_character(void)
{
    ASSERT_EQ(SCHULTZ_DIR_RTL,
              schultz_text_base_direction("123 \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d",
                                          -1));
    ASSERT_EQ(SCHULTZ_DIR_LTR,
              schultz_text_base_direction("123 abc", -1));
    PASS();
}

TEST base_direction_of_neutral_text_is_ltr(void)
{
    ASSERT_EQ(SCHULTZ_DIR_LTR, schultz_text_base_direction("12345 !?", -1));
    ASSERT_EQ(SCHULTZ_DIR_LTR, schultz_text_base_direction("", -1));
    ASSERT_EQ(SCHULTZ_DIR_LTR, schultz_text_base_direction(NULL, -1));
    PASS();
}

TEST rtl_text_shapes_and_measures(void)
{
    text_fixture f;
    schultz_text_run run;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_shape(f.system, f.font,
                                 "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", -1,
                                 SCHULTZ_DIR_AUTO, &f.arena, &run));

    ASSERT_EQ(SCHULTZ_DIR_RTL, run.direction);
    ASSERT(run.count > 0u);
    ASSERT(run.width > 0.0f);

    fixture_teardown(&f);
    PASS();
}

/* ---------------------------------------------------------------- wrapping */

TEST wrap_of_short_text_is_one_line(void)
{
    text_fixture f;
    const schultz_text_line *lines;
    uint32_t count = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_wrap(f.system, f.font, "short", -1, 1000.0f,
                                &f.arena, &lines, &count));
    ASSERT_EQ(1u, count);
    ASSERT_EQ(0u, lines[0].start);
    ASSERT_EQ(5u, lines[0].end);

    fixture_teardown(&f);
    PASS();
}

TEST wrap_splits_when_the_width_is_small(void)
{
    text_fixture f;
    const schultz_text_line *lines;
    uint32_t count = 0;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_wrap(f.system, f.font,
                                "the quick brown fox jumps over the lazy dog",
                                -1, 80.0f, &f.arena, &lines, &count));
    ASSERT(count > 1u);

    /* Lines must tile the input in order, with no gaps and no overlap. */
    ASSERT_EQ(0u, lines[0].start);
    for (i = 1; i < count; i++) {
        ASSERT_EQ(lines[i - 1].end, lines[i].start);
    }
    ASSERT_EQ(43u, lines[count - 1].end);

    fixture_teardown(&f);
    PASS();
}

TEST wrap_breaks_at_spaces_not_mid_word(void)
{
    text_fixture f;
    const schultz_text_line *lines;
    uint32_t count = 0;
    uint32_t i;
    const char *text = "alpha beta gamma delta";

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_wrap(f.system, f.font, text, -1, 90.0f, &f.arena,
                                &lines, &count));
    ASSERT(count > 1u);

    /*
     * Every line after the first must begin at a word start, which here means
     * the byte before it is a space.
     */
    for (i = 1; i < count; i++) {
        ASSERT_EQ(' ', text[lines[i].start - 1]);
    }
    /* And every line fits, which is what wrapping is for. */
    for (i = 0; i < count; i++) {
        ASSERT(lines[i].width <= 90.0f);
    }

    fixture_teardown(&f);
    PASS();
}

/*
 * A run with no break opportunity in it is broken where it stops fitting. It
 * is a last resort, but a run left to overflow leaves the box it was given
 * and cannot be read at all, which is worse than an awkward break.
 */
TEST wrap_breaks_an_overlong_word_at_the_edge(void)
{
    text_fixture f;
    const schultz_text_line *lines;
    uint32_t count = 0;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_wrap(f.system, f.font, "supercalifragilistic", -1,
                                40.0f, &f.arena, &lines, &count));
    ASSERT(count > 1u);
    /* Every piece fits, they run in order, and nothing is lost. */
    for (i = 0; i < count; i++) {
        ASSERT(lines[i].width <= 40.0f);
        ASSERT(lines[i].end > lines[i].start);
        if (i > 0u) {
            ASSERT_EQ(lines[i - 1u].end, lines[i].start);
        }
    }
    ASSERT_EQ(0u, lines[0].start);
    ASSERT_EQ(20u, lines[count - 1u].end);

    fixture_teardown(&f);
    PASS();
}

/* Narrower than a single character, it still makes progress one at a time. */
TEST wrap_makes_progress_when_nothing_fits(void)
{
    text_fixture f;
    const schultz_text_line *lines;
    uint32_t count = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_wrap(f.system, f.font, "abcd", -1,
                                            1.0f, &f.arena, &lines, &count));
    ASSERT_EQ(4u, count);
    ASSERT_EQ(4u, lines[3].end);

    fixture_teardown(&f);
    PASS();
}

TEST wrap_honors_a_mandatory_break(void)
{
    text_fixture f;
    const schultz_text_line *lines;
    uint32_t count = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_wrap(f.system, f.font, "one\ntwo", -1, 1000.0f,
                                &f.arena, &lines, &count));
    ASSERT_EQ(2u, count);
    ASSERT_EQ(0u, lines[0].start);
    ASSERT_EQ(4u, lines[1].start);

    fixture_teardown(&f);
    PASS();
}

TEST wrap_with_unbounded_width_only_breaks_on_hard_breaks(void)
{
    text_fixture f;
    const schultz_text_line *lines;
    uint32_t count = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_wrap(f.system, f.font,
                                "a very long line that never wraps", -1,
                                -1.0f, &f.arena, &lines, &count));
    ASSERT_EQ(1u, count);

    fixture_teardown(&f);
    PASS();
}

TEST wrap_of_empty_text_produces_no_lines(void)
{
    text_fixture f;
    const schultz_text_line *lines = NULL;
    uint32_t count = 99;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_wrap(f.system, f.font, "", -1, 100.0f, &f.arena,
                                &lines, &count));
    ASSERT_EQ(0u, count);

    fixture_teardown(&f);
    PASS();
}

/*
 * A hard break belongs to neither line. Leaving it on the first one would put
 * a character no font draws into the shaped run and would let a caret sit
 * inside the break.
 */
TEST wrapping_leaves_the_break_character_out_of_both_lines(void)
{
    text_fixture f;
    const schultz_text_line *lines;
    uint32_t count = 0;
    const char *text = "ab\ncd";

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_wrap(f.system, f.font, text, -1, -1.0f,
                                            &f.arena, &lines, &count));
    ASSERT_EQ(2u, count);
    ASSERT_EQ(0u, lines[0].start);
    ASSERT_EQ(2u, lines[0].end);   /* stops before the newline */
    ASSERT_EQ(3u, lines[1].start); /* and the next line starts after it */
    ASSERT_EQ(5u, lines[1].end);

    fixture_teardown(&f);
    PASS();
}

/* A Windows line ending is two bytes and neither belongs to a line. */
TEST wrapping_leaves_out_a_two_byte_line_ending(void)
{
    text_fixture f;
    const schultz_text_line *lines;
    uint32_t count = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_wrap(f.system, f.font, "ab\r\ncd", -1,
                                            -1.0f, &f.arena, &lines, &count));
    ASSERT_EQ(2u, count);
    ASSERT_EQ(2u, lines[0].end);
    ASSERT_EQ(4u, lines[1].start);

    fixture_teardown(&f);
    PASS();
}

/*
 * The loop that wraps only looks at break opportunities, and the word being
 * typed at the end of a paragraph has none after it yet. Without checking the
 * tail as well, that word sits past the edge until a space is pressed.
 */
TEST the_last_word_wraps_before_a_space_follows_it(void)
{
    text_fixture f;
    const schultz_text_line *lines;
    uint32_t count = 0;
    schultz_size measured;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(f.system, f.font, "aaa bbb",
                                               -1, &f.arena, &measured));

    /* Room for the first word and a little more, but not for both. */
    ASSERT_EQ(SCHULTZ_OK, schultz_text_wrap(f.system, f.font, "aaa bbb", -1,
                                            measured.width * 0.7f, &f.arena,
                                            &lines, &count));
    ASSERT_EQ(2u, count);
    ASSERT_EQ(0u, lines[0].start);
    ASSERT_EQ(4u, lines[1].start);

    /* And there is still no trailing space in the text. */
    ASSERT_EQ(7u, lines[1].end);

    fixture_teardown(&f);
    PASS();
}

/*
 * A face is bound to one pixel size, so a font handle names a face and a size
 * together. Asking for another size is what lets a style's font.size mean
 * anything and what lets a page be rendered at print resolution with its
 * glyphs rasterized there rather than blown up from screen size.
 */
TEST the_same_face_can_be_had_at_another_size(void)
{
    text_fixture f;
    schultz_handle bigger = SCHULTZ_HANDLE_NONE;
    schultz_handle again = SCHULTZ_HANDLE_NONE;
    schultz_font_metrics small;
    schultz_font_metrics large;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(16.0f, schultz_font_size(f.system, f.font));

    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(f.system, f.font, 48.0f,
                                               &bigger));
    ASSERT(bigger != f.font);
    ASSERT_EQ(48.0f, schultz_font_size(f.system, bigger));

    /* Bigger in every way, which is what a different size means. */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_get_metrics(f.system, f.font, &small));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_get_metrics(f.system, bigger, &large));
    ASSERT(large.line_height > small.line_height);
    ASSERT(large.ascent > small.ascent);

    /* Asked for twice, loaded once. */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(f.system, f.font, 48.0f,
                                               &again));
    ASSERT_EQ(bigger, again);
    ASSERT_EQ(builtin_fonts() + 2u, schultz_font_count(f.system));

    /* The size it already is comes straight back. */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(f.system, f.font, 16.0f,
                                               &again));
    ASSERT_EQ(f.font, again);
    /* And from the other end, so it is a family rather than a chain. */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(f.system, bigger, 16.0f,
                                               &again));
    ASSERT_EQ(f.font, again);
    ASSERT_EQ(builtin_fonts() + 2u, schultz_font_count(f.system));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_font_at_size(f.system, f.font, 0.0f, &again));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_font_at_size(f.system, (schultz_handle)999, 16.0f,
                                   &again));

    fixture_teardown(&f);
    PASS();
}


/* ---------------------------------------------------------------- emoji */

/*
 * No text face carries emoji, so a string that mixes the two is drawn by two
 * faces. These assert the seam: that shaping notices, that it hands each
 * glyph the face that has it, and that a sequence meant to be one picture
 * stays one picture rather than being split at the joiners inside it.
 */

/* A thumbs up, a family of four joined by zero width joiners, and a flag. */
#define EMOJI_THUMB  "\xF0\x9F\x91\x8D"
#define EMOJI_FAMILY "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9" \
                     "\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D" \
                     "\xF0\x9F\x91\xA6"
#define EMOJI_FLAG   "\xF0\x9F\x87\xA8\xF0\x9F\x87\xA6"

/* How many faces a shaped run takes to draw. */
static uint32_t face_changes(const schultz_text_run *run)
{
    schultz_handle last = SCHULTZ_HANDLE_NONE;
    uint32_t runs = 0u;
    uint32_t i;

    for (i = 0u; i < run->count; i++) {
        if (run->fonts == NULL) {
            return 1u;
        }
        if (run->fonts[i] != last) {
            runs++;
            last = run->fonts[i];
        }
    }
    return runs;
}

TEST a_font_system_starts_with_the_emoji_face_set(void)
{
    text_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));

    /* Set up by the font system, not by the host: emoji work out of the
     * box, the same way text does. */
    ASSERT_EQ(schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_EMOJI),
              schultz_font_emoji(f.system));

    /* The text face has letters and no emoji; the emoji face the reverse. */
    ASSERT(schultz_font_has_glyph(f.system, f.font, 'A'));
    ASSERT_FALSE(schultz_font_has_glyph(f.system, f.font, 0x1F44Du));
    ASSERT(schultz_font_has_glyph(f.system,
        schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_EMOJI), 0x1F44Du));

    /* A host may replace it, or say there is none at all. */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_set_emoji(f.system, f.font));
    ASSERT_EQ(f.font, schultz_font_emoji(f.system));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_set_emoji(f.system,
                                                    SCHULTZ_HANDLE_NONE));
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_font_emoji(f.system));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_font_set_emoji(f.system, 12345u));

    fixture_teardown(&f);
    PASS();
}

TEST plain_text_still_shapes_as_one_run(void)
{
    text_fixture f;
    schultz_text_run run;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_shape(f.system, f.font, "Hello", -1,
                                             SCHULTZ_DIR_AUTO, &f.arena,
                                             &run));
    ASSERT_EQ(5u, run.count);
    /* Every glyph from the face that was asked for, and one draw command. */
    ASSERT_EQ(1u, face_changes(&run));
    ASSERT(run.fonts != NULL);
    ASSERT_EQ(f.font, run.fonts[0]);

    fixture_teardown(&f);
    PASS();
}

TEST an_emoji_is_shaped_by_the_emoji_face(void)
{
    text_fixture f;
    schultz_text_run run;
    schultz_handle emoji;
    uint32_t i;
    uint32_t from_emoji = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    emoji = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_EMOJI);
    ASSERT(emoji != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(SCHULTZ_OK, schultz_text_shape(f.system, f.font,
        "Hi " EMOJI_THUMB " there", -1, SCHULTZ_DIR_AUTO, &f.arena, &run));

    /* Letters, then the emoji, then letters again: three draw commands. */
    ASSERT_EQ(3u, face_changes(&run));
    ASSERT(run.fonts != NULL);
    for (i = 0u; i < run.count; i++) {
        if (run.fonts[i] != f.font) {
            from_emoji++;
        }
    }
    ASSERT_EQ(1u, from_emoji);

    fixture_teardown(&f);
    PASS();
}

TEST a_joined_sequence_stays_one_picture(void)
{
    text_fixture f;
    schultz_text_run run;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));

    /*
     * The joiners between the people are ordinary characters the text face
     * does have. Splitting per character would give them to it and draw four
     * separate people, so the split is made per grapheme cluster instead.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_text_shape(f.system, f.font, EMOJI_FAMILY,
                                             -1, SCHULTZ_DIR_AUTO, &f.arena,
                                             &run));
    ASSERT_EQ(1u, run.count);

    /* Two regional indicators are one flag, for the same reason. */
    ASSERT_EQ(SCHULTZ_OK, schultz_text_shape(f.system, f.font, EMOJI_FLAG, -1,
                                             SCHULTZ_DIR_AUTO, &f.arena,
                                             &run));
    ASSERT_EQ(1u, run.count);

    fixture_teardown(&f);
    PASS();
}

TEST without_an_emoji_face_nothing_is_split(void)
{
    text_fixture f;
    schultz_text_run run;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_set_emoji(f.system,
                                                    SCHULTZ_HANDLE_NONE));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_shape(f.system, f.font,
        "Hi " EMOJI_THUMB, -1, SCHULTZ_DIR_AUTO, &f.arena, &run));

    /* One face for all of it, whatever it can and cannot draw. */
    ASSERT_EQ(1u, face_changes(&run));

    fixture_teardown(&f);
    PASS();
}

TEST the_caret_steps_over_a_whole_emoji(void)
{
    static const char *text = "hi " EMOJI_FAMILY "!";
    uint32_t length = (uint32_t)strlen(text);
    uint32_t at = 0u;
    uint32_t steps = 0u;

    /* h, i, space, the whole family, and the mark: five places to stand. */
    while (at < length && steps < 40u) {
        at = schultz_text_next_cluster(text, length, at);
        steps++;
    }
    ASSERT_EQ(5u, steps);
    ASSERT_EQ(length, at);

    steps = 0u;
    while (at > 0u && steps < 40u) {
        at = schultz_text_prev_cluster(text, length, at);
        steps++;
    }
    ASSERT_EQ(5u, steps);
    ASSERT_EQ(0u, at);

    /* A skin tone is part of the hand it is on, not a character beside it. */
    {
        static const char *toned = EMOJI_THUMB "\xF0\x9F\x8F\xBD";

        ASSERT_EQ(8u, schultz_text_next_cluster(toned, 8u, 0u));
        ASSERT_EQ(0u, schultz_text_prev_cluster(toned, 8u, 8u));
    }

    /* And an empty string stands still rather than running off either end. */
    ASSERT_EQ(0u, schultz_text_next_cluster("", 0u, 0u));
    ASSERT_EQ(0u, schultz_text_prev_cluster("", 0u, 0u));
    ASSERT_EQ(0u, schultz_text_next_cluster(NULL, 0u, 0u));
    PASS();
}


/*
 * A text face covers a great many emoji as plain outlines: DejaVu draws the
 * smileys, the hearts, the aeroplane and the tick. Choosing a face on
 * coverage alone therefore drew some emoji in colour and the rest as thin
 * black line art in the same line. Which face has the colours is the
 * question that sorts them.
 */
TEST an_emoji_the_text_face_also_has_is_still_drawn_in_colour(void)
{
    text_fixture f;
    schultz_text_run run;
    schultz_handle emoji;
    /* Upside-down face, grinning face, white smiling face, heavy black
     * heart, aeroplane. DejaVu has an outline for every one of them. */
    static const uint32_t both[] = { 0x1F643u, 0x1F600u, 0x263Au, 0x2764u,
                                     0x2708u };
    static const char *text[] = { "\xF0\x9F\x99\x83", "\xF0\x9F\x98\x80",
                                  "\xE2\x98\xBA", "\xE2\x9D\xA4",
                                  "\xE2\x9C\x88" };
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    emoji = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_EMOJI);

    for (i = 0u; i < 5u; i++) {
        /* Both faces have the character, which is the whole difficulty. */
        ASSERT(schultz_font_has_glyph(f.system, f.font, both[i]));
        ASSERT(schultz_font_has_glyph(f.system, emoji, both[i]));
        /* Only one of them has it in colour. */
        ASSERT_FALSE(schultz_font_has_color_glyph(f.system, f.font, both[i]));
        ASSERT(schultz_font_has_color_glyph(f.system, emoji, both[i]));

        ASSERT_EQ(SCHULTZ_OK, schultz_text_shape(f.system, f.font, text[i],
            -1, SCHULTZ_DIR_LTR, &f.arena, &run));
        ASSERT_EQ(1u, run.count);
        ASSERT(run.fonts != NULL);
        ASSERT_EQ(emoji, run.fonts[0]);
    }

    fixture_teardown(&f);
    PASS();
}

TEST letters_and_digits_stay_with_the_text_face(void)
{
    text_fixture f;
    schultz_text_run run;
    schultz_handle emoji;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    emoji = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_EMOJI);

    /*
     * The emoji face carries the digits, the hash and the asterisk for the
     * keycap sequences they take part in, and draws them in colour even on
     * their own. So "which face has the colours" is not enough on its own:
     * by that question alone a year would come out as coloured tiles.
     */
    ASSERT(schultz_font_has_glyph(f.system, emoji, '0'));
    ASSERT(schultz_font_has_color_glyph(f.system, emoji, '0'));

    /* What keeps them where they belong is that a digit is not a picture. */

    ASSERT_EQ(SCHULTZ_OK, schultz_text_shape(f.system, f.font, "2026 Hi",
        -1, SCHULTZ_DIR_LTR, &f.arena, &run));
    ASSERT_EQ(7u, run.count);
    for (i = 0u; i < run.count; i++) {
        ASSERT_EQ(f.font, run.fonts[i]);
    }

    fixture_teardown(&f);
    PASS();
}

TEST a_character_can_say_which_way_it_is_meant(void)
{
    text_fixture f;
    schultz_text_run run;
    schultz_handle emoji;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    emoji = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_EMOJI);

    /* A heart on its own is a picture, because the emoji face has it. */
    ASSERT_EQ(SCHULTZ_OK, schultz_text_shape(f.system, f.font,
        "\xE2\x9D\xA4", -1, SCHULTZ_DIR_LTR, &f.arena, &run));
    ASSERT_EQ(emoji, run.fonts[0]);

    /* Followed by the character that asks for text, it is a letter again. */
    ASSERT_EQ(SCHULTZ_OK, schultz_text_shape(f.system, f.font,
        "\xE2\x9D\xA4\xEF\xB8\x8E", -1, SCHULTZ_DIR_LTR, &f.arena, &run));
    ASSERT(run.count > 0u);
    ASSERT_EQ(f.font, run.fonts[0]);

    /*
     * And the other way for a keycap, which is a digit the text face has and
     * would otherwise keep: the character asking for a picture wins.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_text_shape(f.system, f.font,
        "1\xEF\xB8\x8F\xE2\x83\xA3", -1, SCHULTZ_DIR_LTR, &f.arena,
        &run));
    ASSERT(run.count > 0u);
    ASSERT_EQ(emoji, run.fonts[0]);

    fixture_teardown(&f);
    PASS();
}

/* ----------------------------------------------------------------- pieces */

/*
 * A piece is a stretch of a string set in one face. These check the three
 * things a piece has to do: change the glyphs, change what the text measures,
 * and change where it breaks. Colour and underline never reach this layer,
 * because none of them move a glyph.
 */

/* The whole string in one face, said the long way round. */
static schultz_text_piece whole(const char *text, schultz_handle font)
{
    schultz_text_piece piece;

    piece.start = 0u;
    piece.end   = (uint32_t)strlen(text);
    piece.font  = font;
    return piece;
}

TEST pieces_naming_one_face_match_the_plain_call(void)
{
    text_fixture f;
    const char *text = "The quick brown fox";
    schultz_text_piece piece;
    schultz_size plain;
    schultz_size pieced;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    piece = whole(text, f.font);

    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(f.system, f.font, text, -1,
                                               &f.arena, &plain));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_measure_pieces(f.system, &piece, 1u, f.font, text,
                                          -1, &f.arena, &pieced));
    /*
     * Saying what the call already assumed changes nothing at all. This is
     * the case every label in the toolkit is, so it is the one that must not
     * drift.
     */
    ASSERT_IN_RANGE(plain.width, pieced.width, 0.01f);
    ASSERT_IN_RANGE(plain.height, pieced.height, 0.01f);

    fixture_teardown(&f);
    PASS();
}

TEST text_with_no_pieces_measures_as_it_always_did(void)
{
    text_fixture f;
    const char *text = "The quick brown fox";
    schultz_size plain;
    schultz_size none;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));

    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(f.system, f.font, text, -1,
                                               &f.arena, &plain));
    /* NULL and zero both mean the same thing, and both mean the old way. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_measure_pieces(f.system, NULL, 0u, f.font, text, -1,
                                          &f.arena, &none));
    ASSERT_IN_RANGE(plain.width, none.width, 0.01f);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_measure_pieces(f.system, NULL, 7u, f.font, text, -1,
                                          &f.arena, &none));
    ASSERT_IN_RANGE(plain.width, none.width, 0.01f);

    fixture_teardown(&f);
    PASS();
}

TEST a_bold_piece_changes_the_face_its_glyphs_come_from(void)
{
    text_fixture f;
    const char *text = "one two";
    schultz_text_piece piece;
    schultz_handle body;
    schultz_handle bold;
    schultz_text_run run;
    uint32_t i;
    uint32_t from_bold = 0u;
    uint32_t from_body = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    body = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_BODY);
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(f.system, body, 1, 0, &bold));

    /* "one" bold, the rest not. */
    piece.start = 0u;
    piece.end   = 3u;
    piece.font  = bold;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_shape_pieces(f.system, &piece, 1u, body, text, -1,
                                        SCHULTZ_DIR_LTR, &f.arena, &run));
    ASSERT(run.count > 0u);
    ASSERT(run.fonts != NULL);

    for (i = 0u; i < run.count; i++) {
        if (run.fonts[i] == bold) {
            from_bold++;
            /* Every bold glyph came from inside the piece. */
            ASSERT(run.clusters[i] < 3u);
        } else if (run.fonts[i] == body) {
            from_body++;
            ASSERT(run.clusters[i] >= 3u);
        }
    }
    ASSERT_EQ(3u, from_bold);
    ASSERT(from_body > 0u);

    fixture_teardown(&f);
    PASS();
}

TEST a_piece_only_has_to_cover_what_differs(void)
{
    text_fixture f;
    const char *text = "plain bold";
    schultz_text_piece piece;
    schultz_handle body;
    schultz_handle bold;
    schultz_text_run run;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    body = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_BODY);
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(f.system, body, 1, 0, &bold));

    /* Only the last word is described. The rest falls to the call's font. */
    piece.start = 6u;
    piece.end   = 10u;
    piece.font  = bold;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_shape_pieces(f.system, &piece, 1u, body, text, -1,
                                        SCHULTZ_DIR_LTR, &f.arena, &run));
    for (i = 0u; i < run.count; i++) {
        ASSERT_EQ((run.clusters[i] >= 6u) ? bold : body, run.fonts[i]);
    }

    fixture_teardown(&f);
    PASS();
}

TEST a_larger_piece_makes_the_line_taller(void)
{
    text_fixture f;
    const char *text = "small LARGE";
    schultz_text_piece piece;
    schultz_handle body;
    schultz_handle big;
    schultz_size plain;
    schultz_size mixed;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    body = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_BODY);
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(f.system, body, 32.0f, &big));

    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(f.system, body, text, -1,
                                               &f.arena, &plain));
    piece.start = 6u;
    piece.end   = 11u;
    piece.font  = big;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_measure_pieces(f.system, &piece, 1u, body, text, -1,
                                          &f.arena, &mixed));

    /*
     * The tallest face on the line decides the line, so one large word makes
     * the whole line taller, and wider, than the same words set small.
     */
    ASSERT(mixed.height > plain.height);
    ASSERT(mixed.width > plain.width);

    fixture_teardown(&f);
    PASS();
}

TEST the_order_pieces_are_given_in_does_not_matter(void)
{
    text_fixture f;
    const char *text = "alpha beta gamma";
    schultz_text_piece forward[2];
    schultz_text_piece backward[2];
    schultz_handle body;
    schultz_handle bold;
    schultz_handle italic;
    schultz_text_run one;
    schultz_text_run two;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    body = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_BODY);
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(f.system, body, 1, 0, &bold));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(f.system, body, 0, 1, &italic));

    forward[0].start = 0u;  forward[0].end = 5u;  forward[0].font = bold;
    forward[1].start = 11u; forward[1].end = 16u; forward[1].font = italic;
    backward[0] = forward[1];
    backward[1] = forward[0];

    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_shape_pieces(f.system, forward, 2u, body, text, -1,
                                        SCHULTZ_DIR_LTR, &f.arena, &one));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_shape_pieces(f.system, backward, 2u, body, text, -1,
                                        SCHULTZ_DIR_LTR, &f.arena, &two));

    /*
     * Pieces describe the text, they do not act on it in turn, so handing
     * them over in another order has to produce the same line. Otherwise the
     * order a program happened to build them in would show on screen.
     */
    ASSERT_EQ(one.count, two.count);
    ASSERT_IN_RANGE(one.width, two.width, 0.001f);
    ASSERT_IN_RANGE(one.height, two.height, 0.001f);
    for (i = 0u; i < one.count; i++) {
        ASSERT_EQ(one.fonts[i], two.fonts[i]);
        ASSERT_EQ(one.clusters[i], two.clusters[i]);
        ASSERT_EQ(one.glyphs[i].glyph_id, two.glyphs[i].glyph_id);
        ASSERT_IN_RANGE(one.glyphs[i].x, two.glyphs[i].x, 0.001f);
    }

    fixture_teardown(&f);
    PASS();
}

TEST three_pieces_in_any_order_give_the_same_line(void)
{
    text_fixture f;
    const char *text = "aaa bbb ccc ddd";
    schultz_handle body;
    schultz_handle bold;
    schultz_handle italic;
    schultz_handle big;
    schultz_text_piece set[3];
    schultz_text_piece shuffled[3];
    schultz_size first;
    schultz_size again;
    uint32_t order[6][3] = {
        {0u, 1u, 2u}, {0u, 2u, 1u}, {1u, 0u, 2u},
        {1u, 2u, 0u}, {2u, 0u, 1u}, {2u, 1u, 0u}
    };
    uint32_t which;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    body = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_BODY);
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(f.system, body, 1, 0, &bold));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(f.system, body, 1, 1, &italic));
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(f.system, body, 20.0f, &big));

    set[0].start = 0u;  set[0].end = 3u;  set[0].font = bold;
    set[1].start = 4u;  set[1].end = 7u;  set[1].font = italic;
    set[2].start = 8u;  set[2].end = 11u; set[2].font = big;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_measure_pieces(f.system, set, 3u, body, text, -1,
                                          &f.arena, &first));

    /* Every arrangement of the same three, measured the same. */
    for (which = 0u; which < 6u; which++) {
        shuffled[0] = set[order[which][0]];
        shuffled[1] = set[order[which][1]];
        shuffled[2] = set[order[which][2]];
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_text_measure_pieces(f.system, shuffled, 3u, body,
                                              text, -1, &f.arena, &again));
        ASSERT_IN_RANGE(first.width, again.width, 0.001f);
        ASSERT_IN_RANGE(first.height, again.height, 0.001f);
    }

    fixture_teardown(&f);
    PASS();
}

TEST an_empty_piece_says_nothing(void)
{
    text_fixture f;
    const char *text = "unchanged";
    schultz_text_piece empty[2];
    schultz_handle body;
    schultz_handle bold;
    schultz_size plain;
    schultz_size with_empty;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    body = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_BODY);
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(f.system, body, 1, 0, &bold));

    /* One piece covering nothing, and one describing text that is not
     * here. Neither may change a thing. */
    empty[0].start = 4u;  empty[0].end = 4u;  empty[0].font = bold;
    empty[1].start = 90u; empty[1].end = 99u; empty[1].font = bold;

    ASSERT_EQ(SCHULTZ_OK, schultz_text_measure(f.system, body, text, -1,
                                               &f.arena, &plain));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_measure_pieces(f.system, empty, 2u, body, text, -1,
                                          &f.arena, &with_empty));
    ASSERT_IN_RANGE(plain.width, with_empty.width, 0.01f);
    ASSERT_IN_RANGE(plain.height, with_empty.height, 0.01f);

    fixture_teardown(&f);
    PASS();
}

TEST a_caret_crosses_a_piece_boundary(void)
{
    text_fixture f;
    const char *text = "ab cd";
    schultz_text_piece piece;
    schultz_handle body;
    schultz_handle big;
    schultz_text_run run;
    uint32_t at;
    float previous = -1.0f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    body = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_BODY);
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(f.system, body, 28.0f, &big));

    piece.start = 3u;
    piece.end   = 5u;
    piece.font  = big;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_shape_pieces(f.system, &piece, 1u, body, text, -1,
                                        SCHULTZ_DIR_LTR, &f.arena, &run));

    /*
     * A caret walks forward across the whole line, boundary and all. If the
     * pieces were laid down separately rather than end to end, the caret
     * would jump backwards where the face changes.
     */
    for (at = 0u; at <= 5u; at++) {
        float x = schultz_text_caret_x(&run, at);

        ASSERT(x > previous);
        previous = x;
    }

    /* And clicking lands back on the byte the caret came from, on both
     * sides of the boundary. */
    for (at = 0u; at <= 5u; at++) {
        float x = schultz_text_caret_x(&run, at);

        ASSERT_EQ(at, schultz_text_caret_offset(&run, x, 5u));
    }

    fixture_teardown(&f);
    PASS();
}

TEST wrapping_breaks_earlier_when_a_piece_grows(void)
{
    text_fixture f;
    const char *text = "alpha beta gamma delta";
    schultz_text_piece piece;
    schultz_handle body;
    schultz_handle big;
    const schultz_text_line *plain_lines;
    const schultz_text_line *grown_lines;
    uint32_t plain_count = 0u;
    uint32_t grown_count = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    body = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_BODY);
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_size(f.system, body, 40.0f, &big));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_wrap(f.system, body, text, -1, 160.0f, &f.arena,
                                &plain_lines, &plain_count));
    ASSERT(plain_count > 0u);

    /* "gamma" at more than twice the size cannot sit where it did. */
    piece.start = 11u;
    piece.end   = 16u;
    piece.font  = big;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_wrap_pieces(f.system, &piece, 1u, body, text, -1,
                                       160.0f, &f.arena, &grown_lines,
                                       &grown_count));
    ASSERT(grown_count > plain_count);

    fixture_teardown(&f);
    PASS();
}

TEST a_piece_boundary_is_not_a_place_to_break(void)
{
    text_fixture f;
    /* One word, whose second half is bold. */
    const char *text = "ridiculous";
    schultz_text_piece piece;
    schultz_handle body;
    schultz_handle bold;
    const schultz_text_line *lines;
    uint32_t count = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    body = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_BODY);
    ASSERT_EQ(SCHULTZ_OK, schultz_font_at_style(f.system, body, 1, 0, &bold));

    piece.start = 5u;
    piece.end   = 10u;
    piece.font  = bold;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_wrap_pieces(f.system, &piece, 1u, body, text, -1,
                                       -1.0f, &f.arena, &lines, &count));
    /*
     * Where a line may break is a property of the writing, not of how it is
     * set. Half a word in another face is still one word, so an unbounded
     * width leaves it on one line.
     */
    ASSERT_EQ(1u, count);
    ASSERT_EQ(0u, lines[0].start);
    ASSERT_EQ(10u, lines[0].end);

    fixture_teardown(&f);
    PASS();
}

TEST piece_calls_reject_bad_arguments(void)
{
    text_fixture f;
    const char *text = "text";
    schultz_text_piece piece;
    schultz_text_run run;
    schultz_size size;
    const schultz_text_line *lines;
    uint32_t count = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f, 16.0f));
    piece = whole(text, f.font);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_text_shape_pieces(f.system, &piece, 1u, f.font, text, -1,
                                        SCHULTZ_DIR_LTR, NULL, &run));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_text_shape_pieces(f.system, &piece, 1u, f.font, text, -1,
                                        SCHULTZ_DIR_LTR, &f.arena, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_text_measure_pieces(f.system, &piece, 1u, f.font, text,
                                          -1, &f.arena, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_text_wrap_pieces(f.system, &piece, 1u, f.font, text, -1,
                                       100.0f, &f.arena, NULL, &count));

    /*
     * A face nothing can draw with is refused rather than measured as
     * nothing, because a line of zeroes wraps into lines no one can see.
     */
    piece.font = SCHULTZ_HANDLE_NONE;
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_text_shape_pieces(f.system, &piece, 1u,
                                        SCHULTZ_HANDLE_NONE, text, -1,
                                        SCHULTZ_DIR_LTR, &f.arena, &run));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_text_wrap_pieces(f.system, &piece, 1u,
                                       SCHULTZ_HANDLE_NONE, text, -1, 100.0f,
                                       &f.arena, &lines, &count));

    /* But a piece with no face of its own still draws, in the call's. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_measure_pieces(f.system, &piece, 1u, f.font, text,
                                          -1, &f.arena, &size));
    ASSERT(size.width > 0.0f);

    fixture_teardown(&f);
    PASS();
}

SUITE(text)
{
    RUN_TEST(the_same_face_can_be_had_at_another_size);
    RUN_TEST(the_last_word_wraps_before_a_space_follows_it);
    RUN_TEST(wrapping_leaves_the_break_character_out_of_both_lines);
    RUN_TEST(wrapping_leaves_out_a_two_byte_line_ending);
    RUN_TEST(font_loads_from_file);
    RUN_TEST(font_load_rejects_bad_arguments);
    RUN_TEST(a_font_system_brings_its_own_faces);
    RUN_TEST(the_built_in_mono_face_really_is_fixed_pitch);
    RUN_TEST(a_built_in_face_can_be_had_at_another_size);
    RUN_TEST(the_built_in_family_is_complete);
    RUN_TEST(the_bold_body_face_is_the_title_face);
    RUN_TEST(asking_for_the_style_a_face_already_has_gives_it_back);
    RUN_TEST(a_style_lookup_keeps_the_size);
    RUN_TEST(a_family_missing_a_member_answers_with_what_it_has);
    RUN_TEST(bold_italic_falls_back_to_bold_when_that_is_all_there_is);
    RUN_TEST(bold_italic_falls_back_to_italic_when_bold_is_gone);
    RUN_TEST(style_lookup_rejects_bad_arguments);
    RUN_TEST(pieces_naming_one_face_match_the_plain_call);
    RUN_TEST(text_with_no_pieces_measures_as_it_always_did);
    RUN_TEST(a_bold_piece_changes_the_face_its_glyphs_come_from);
    RUN_TEST(a_piece_only_has_to_cover_what_differs);
    RUN_TEST(a_larger_piece_makes_the_line_taller);
    RUN_TEST(the_order_pieces_are_given_in_does_not_matter);
    RUN_TEST(three_pieces_in_any_order_give_the_same_line);
    RUN_TEST(an_empty_piece_says_nothing);
    RUN_TEST(a_caret_crosses_a_piece_boundary);
    RUN_TEST(wrapping_breaks_earlier_when_a_piece_grows);
    RUN_TEST(a_piece_boundary_is_not_a_place_to_break);
    RUN_TEST(piece_calls_reject_bad_arguments);
    RUN_TEST(a_face_can_be_handed_over_as_bytes);
    RUN_TEST(font_load_rejects_a_missing_file);
    RUN_TEST(font_load_rejects_a_file_that_is_not_a_font);
    RUN_TEST(unloaded_font_handle_goes_stale);
    RUN_TEST(metrics_are_sane_and_scale_with_size);
    RUN_TEST(shaping_produces_one_glyph_per_simple_letter);
    RUN_TEST(shaped_glyphs_advance_left_to_right);
    RUN_TEST(empty_string_shapes_to_nothing);
    RUN_TEST(shaping_honors_an_explicit_length);
    RUN_TEST(shaping_rejects_bad_arguments);
    RUN_TEST(shaping_applies_kerning);
    RUN_TEST(measurement_grows_with_text_and_size);
    RUN_TEST(measurement_is_repeatable);
    RUN_TEST(base_direction_of_latin_is_ltr);
    RUN_TEST(base_direction_of_hebrew_is_rtl);
    RUN_TEST(base_direction_of_arabic_is_rtl);
    RUN_TEST(base_direction_uses_the_first_strong_character);
    RUN_TEST(base_direction_of_neutral_text_is_ltr);
    RUN_TEST(rtl_text_shapes_and_measures);
    RUN_TEST(wrap_of_short_text_is_one_line);
    RUN_TEST(wrap_splits_when_the_width_is_small);
    RUN_TEST(wrap_breaks_at_spaces_not_mid_word);
    RUN_TEST(wrap_breaks_an_overlong_word_at_the_edge);
    RUN_TEST(wrap_makes_progress_when_nothing_fits);
    RUN_TEST(wrap_honors_a_mandatory_break);
    RUN_TEST(wrap_with_unbounded_width_only_breaks_on_hard_breaks);
    RUN_TEST(wrap_of_empty_text_produces_no_lines);
    RUN_TEST(a_font_system_starts_with_the_emoji_face_set);
    RUN_TEST(plain_text_still_shapes_as_one_run);
    RUN_TEST(an_emoji_is_shaped_by_the_emoji_face);
    RUN_TEST(a_joined_sequence_stays_one_picture);
    RUN_TEST(without_an_emoji_face_nothing_is_split);
    RUN_TEST(the_caret_steps_over_a_whole_emoji);
    RUN_TEST(an_emoji_the_text_face_also_has_is_still_drawn_in_colour);
    RUN_TEST(letters_and_digits_stay_with_the_text_face);
    RUN_TEST(a_character_can_say_which_way_it_is_meant);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(text);
    GREATEST_MAIN_END();
}
