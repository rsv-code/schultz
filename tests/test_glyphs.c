/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_glyphs.c - rasterized glyph cache.
 *
 * Glyph indices are font specific, so these tests get them from shaping
 * rather than hardcoding numbers that would only hold for one font build.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_text.h"

#define FONT_PATH "assets/fonts/DejaVuSans.ttf"

typedef struct {
    schultz_font_system *system;
    schultz_glyph_cache *cache;
    schultz_arena        arena;
    schultz_handle       font;
} glyph_fixture;

static int32_t fixture_setup(glyph_fixture *f)
{
    int32_t result = schultz_font_system_create(&f->system);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_glyph_cache_create(f->system, &f->cache);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_arena_init(&f->arena, 0);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_font_load_file(f->system, FONT_PATH, 24.0f, &f->font);
}

static void fixture_teardown(glyph_fixture *f)
{
    schultz_arena_free(&f->arena);
    schultz_glyph_cache_destroy(f->cache);
    schultz_font_system_destroy(f->system);
}

/* Shapes text and returns the glyph index at the given position. */
static uint32_t glyph_id_of(glyph_fixture *f, const char *utf8, uint32_t index)
{
    schultz_text_run run;
    if (schultz_text_shape(f->system, f->font, utf8, -1, SCHULTZ_DIR_LTR,
                           &f->arena, &run) != SCHULTZ_OK) {
        return 0;
    }
    return (index < run.count) ? run.glyphs[index].glyph_id : 0;
}

TEST cache_starts_empty(void)
{
    glyph_fixture f;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(0u, schultz_glyph_cache_count(f.cache));

    fixture_teardown(&f);
    PASS();
}

TEST create_rejects_null_arguments(void)
{
    schultz_font_system *system;
    schultz_glyph_cache *cache = NULL;

    ASSERT_EQ(SCHULTZ_OK, schultz_font_system_create(&system));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_glyph_cache_create(NULL, &cache));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_glyph_cache_create(system, NULL));
    schultz_font_system_destroy(system);
    PASS();
}

TEST rasterizes_a_glyph_with_sane_extents(void)
{
    glyph_fixture f;
    const schultz_glyph_bitmap *bitmap = NULL;
    uint32_t id;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    id = glyph_id_of(&f, "A", 0);
    ASSERT(id != 0u);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_glyph_cache_get(f.cache, f.font, id, &bitmap));
    ASSERT(bitmap != NULL);
    ASSERT(bitmap->bitmap != NULL);
    ASSERT(bitmap->width > 0u);
    ASSERT(bitmap->height > 0u);
    ASSERT(bitmap->pitch >= bitmap->width);
    /* A capital letter sits above the baseline. */
    ASSERT(bitmap->bearing_y > 0);
    ASSERT_EQ(1u, schultz_glyph_cache_count(f.cache));

    fixture_teardown(&f);
    PASS();
}

TEST the_bitmap_has_ink_in_it(void)
{
    glyph_fixture f;
    const schultz_glyph_bitmap *bitmap = NULL;
    uint32_t row;
    uint32_t col;
    uint32_t ink = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_glyph_cache_get(f.cache, f.font,
                                                  glyph_id_of(&f, "M", 0),
                                                  &bitmap));
    for (row = 0; row < bitmap->height; row++) {
        for (col = 0; col < bitmap->width; col++) {
            if (bitmap->bitmap[row * bitmap->pitch + col] != 0) {
                ink++;
            }
        }
    }
    ASSERT(ink > 0u);

    fixture_teardown(&f);
    PASS();
}

TEST a_repeated_request_is_served_from_the_cache(void)
{
    glyph_fixture f;
    const schultz_glyph_bitmap *first = NULL;
    const schultz_glyph_bitmap *second = NULL;
    uint32_t id;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    id = glyph_id_of(&f, "g", 0);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_glyph_cache_get(f.cache, f.font, id, &first));
    ASSERT_EQ(1u, schultz_glyph_cache_count(f.cache));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_glyph_cache_get(f.cache, f.font, id, &second));
    ASSERT_EQ(1u, schultz_glyph_cache_count(f.cache));
    ASSERT_EQ(first, second);

    fixture_teardown(&f);
    PASS();
}

TEST a_blank_glyph_has_no_bitmap(void)
{
    glyph_fixture f;
    const schultz_glyph_bitmap *bitmap = NULL;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    /* The space in "a b" is the second glyph. */
    ASSERT_EQ(SCHULTZ_OK, schultz_glyph_cache_get(f.cache, f.font,
                                                  glyph_id_of(&f, "a b", 1),
                                                  &bitmap));
    ASSERT_EQ(NULL, bitmap->bitmap);
    ASSERT_EQ(0u, bitmap->width);
    ASSERT_EQ(0u, bitmap->height);

    fixture_teardown(&f);
    PASS();
}

TEST get_rejects_a_font_that_is_not_loaded(void)
{
    glyph_fixture f;
    const schultz_glyph_bitmap *bitmap = NULL;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_glyph_cache_get(f.cache, SCHULTZ_HANDLE_NONE, 1,
                                      &bitmap));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_glyph_cache_get(f.cache, f.font, 1, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_glyph_cache_get(NULL, f.font, 1, &bitmap));

    fixture_teardown(&f);
    PASS();
}

/*
 * The table grows and rehashes past its initial capacity. Every glyph must
 * still resolve afterwards, which is what a broken probe chain would break.
 */
TEST cache_survives_growth(void)
{
    glyph_fixture f;
    const schultz_glyph_bitmap *bitmap = NULL;
    uint32_t id;
    uint32_t stored = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));

    for (id = 1; id < 600u; id++) {
        if (schultz_glyph_cache_get(f.cache, f.font, id, &bitmap)
                == SCHULTZ_OK) {
            stored++;
        }
    }
    ASSERT(stored > 300u);
    ASSERT_EQ(stored, schultz_glyph_cache_count(f.cache));

    /* Every one of them must still be found after the rehashing. */
    for (id = 1; id < 600u; id++) {
        const schultz_glyph_bitmap *again = NULL;
        if (schultz_glyph_cache_get(f.cache, f.font, id, &again)
                == SCHULTZ_OK) {
            ASSERT(again != NULL);
        }
    }
    ASSERT_EQ(stored, schultz_glyph_cache_count(f.cache));

    fixture_teardown(&f);
    PASS();
}

TEST two_fonts_do_not_share_entries(void)
{
    glyph_fixture f;
    schultz_handle other;
    const schultz_glyph_bitmap *small = NULL;
    const schultz_glyph_bitmap *large = NULL;
    uint32_t id;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_font_load_file(f.system, FONT_PATH, 64.0f, &other));

    id = glyph_id_of(&f, "W", 0);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_glyph_cache_get(f.cache, f.font, id, &small));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_glyph_cache_get(f.cache, other, id, &large));

    ASSERT_EQ(2u, schultz_glyph_cache_count(f.cache));
    ASSERT(large->height > small->height);

    fixture_teardown(&f);
    PASS();
}

TEST purge_drops_only_one_font(void)
{
    glyph_fixture f;
    schultz_handle other;
    const schultz_glyph_bitmap *bitmap = NULL;
    uint32_t id;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_font_load_file(f.system, FONT_PATH, 64.0f, &other));

    id = glyph_id_of(&f, "Q", 0);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_glyph_cache_get(f.cache, f.font, id, &bitmap));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_glyph_cache_get(f.cache, other, id, &bitmap));
    ASSERT_EQ(2u, schultz_glyph_cache_count(f.cache));

    schultz_glyph_cache_purge_font(f.cache, f.font);
    ASSERT_EQ(1u, schultz_glyph_cache_count(f.cache));

    /* The survivor must still be reachable. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_glyph_cache_get(f.cache, other, id, &bitmap));
    ASSERT_EQ(1u, schultz_glyph_cache_count(f.cache));

    schultz_glyph_cache_purge_font(f.cache, other);
    ASSERT_EQ(0u, schultz_glyph_cache_count(f.cache));
    schultz_glyph_cache_purge_font(NULL, other);

    fixture_teardown(&f);
    PASS();
}


/*
 * A letter has no colour of its own and rasterizes to coverage, which the
 * painter tints. An emoji is a picture and rasterizes to finished pixels,
 * which the painter copies. The cache says which it handed back, and getting
 * that wrong would draw an emoji as a solid block of the text colour.
 */
TEST a_letter_rasterizes_to_coverage_and_an_emoji_to_colour(void)
{
    glyph_fixture f;
    schultz_text_run run;
    schultz_handle emoji;
    const schultz_glyph_bitmap *letter;
    const schultz_glyph_bitmap *picture;
    uint32_t ink = 0u;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    emoji = schultz_font_builtin(f.system, SCHULTZ_TOKEN_FONT_EMOJI);
    ASSERT(emoji != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(SCHULTZ_OK, schultz_glyph_cache_get(f.cache, f.font,
                                                  glyph_id_of(&f, "A", 0),
                                                  &letter));
    ASSERT_EQ((uint32_t)SCHULTZ_GLYPH_COVERAGE, letter->format);
    ASSERT_EQ(letter->width, letter->pitch);

    /* Shaped through the text face, which sends it on to the emoji one. */
    ASSERT_EQ(SCHULTZ_OK, schultz_text_shape(f.system, f.font,
        "\xF0\x9F\x91\x8D", -1, SCHULTZ_DIR_LTR, &f.arena, &run));
    ASSERT_EQ(1u, run.count);
    ASSERT(run.fonts != NULL);
    ASSERT(run.fonts[0] != f.font);

    ASSERT_EQ(SCHULTZ_OK, schultz_glyph_cache_get(f.cache, run.fonts[0],
                                                  run.glyphs[0].glyph_id,
                                                  &picture));
    ASSERT_EQ((uint32_t)SCHULTZ_GLYPH_COLOR, picture->format);
    /* Four bytes to a pixel rather than one. */
    ASSERT_EQ(picture->width * 4u, picture->pitch);
    ASSERT(picture->bitmap != NULL);

    /*
     * And it is a picture rather than a silhouette: somewhere in it the
     * colour channels differ from one another, which a tinted mask could
     * never produce. The whole bitmap is searched rather than its first row,
     * because a drawing sits inside the box the font reserves for it and the
     * top row of that box is usually empty.
     */
    for (i = 0u; i + 3u < picture->pitch * picture->height; i += 4u) {
        if (picture->bitmap[i] != picture->bitmap[i + 1u] ||
            picture->bitmap[i + 1u] != picture->bitmap[i + 2u]) {
            ink++;
        }
    }
    ASSERT(ink > 0u);

    /*
     * The face carries outlines rather than one fixed size of picture, so it
     * follows the text size instead of being resampled to it.
     */
    ASSERT(picture->width > 0u && picture->width < 200u);

    fixture_teardown(&f);
    PASS();
}


/*
 * The emoji face describes each glyph as a drawing rather than an outline,
 * which FreeType reads and does not draw. These check the drawing: that the
 * hard shapes come out with ink in them, that a gradient really is a
 * gradient rather than one flat colour, and that a picture built by
 * combining two others is not lost.
 */
TEST every_kind_of_drawing_comes_out_with_ink_in_it(void)
{
    glyph_fixture f;
    schultz_text_run run;
    uint32_t which;
    /* A flat face, a gradient (the rocket), one built by combining (the
     * flag), a joined sequence, and one from the newest release. */
    static const char *cases[] = {
        "\xF0\x9F\x98\x80",
        "\xF0\x9F\x9A\x80",
        "\xF0\x9F\x87\xA8\xF0\x9F\x87\xA6",
        "\xF0\x9F\x99\x82\xE2\x80\x8D\xE2\x86\x94\xEF\xB8\x8F",
        "\xF0\x9F\xAB\xA9"
    };

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));

    for (which = 0u; which < 5u; which++) {
        const schultz_glyph_bitmap *picture;
        uint32_t opaque = 0u;
        uint32_t shades = 0u;
        uint32_t seen[16];
        uint32_t i;

        ASSERT_EQ(SCHULTZ_OK, schultz_text_shape(f.system, f.font,
            cases[which], -1, SCHULTZ_DIR_LTR, &f.arena, &run));
        /* One picture, not the pieces of a sequence drawn side by side. */
        ASSERT_EQ(1u, run.count);
        ASSERT(run.fonts != NULL && run.fonts[0] != f.font);

        ASSERT_EQ(SCHULTZ_OK, schultz_glyph_cache_get(f.cache, run.fonts[0],
            run.glyphs[0].glyph_id, &picture));
        ASSERT_EQ((uint32_t)SCHULTZ_GLYPH_COLOR, picture->format);
        ASSERT(picture->bitmap != NULL);
        ASSERT(picture->width > 0u && picture->height > 0u);

        for (i = 0u; i + 3u < picture->pitch * picture->height; i += 4u) {
            uint32_t j;

            if (picture->bitmap[i + 3u] == 0u) {
                continue;
            }
            opaque++;
            /* Count how many different colours turn up, to a handful. */
            for (j = 0u; j < shades; j++) {
                if (seen[j] == (uint32_t)picture->bitmap[i]
                               + ((uint32_t)picture->bitmap[i + 1u] << 8)) {
                    break;
                }
            }
            if (j == shades && shades < 16u) {
                seen[shades] = (uint32_t)picture->bitmap[i]
                               + ((uint32_t)picture->bitmap[i + 1u] << 8);
                shades++;
            }
        }
        /* Something was drawn, and it was not one flat colour: even the
         * plainest of these has an outline and a fill. */
        ASSERT(opaque > 0u);
        ASSERT(shades > 1u);
    }

    fixture_teardown(&f);
    PASS();
}

SUITE(glyphs)
{
    RUN_TEST(cache_starts_empty);
    RUN_TEST(create_rejects_null_arguments);
    RUN_TEST(rasterizes_a_glyph_with_sane_extents);
    RUN_TEST(the_bitmap_has_ink_in_it);
    RUN_TEST(a_repeated_request_is_served_from_the_cache);
    RUN_TEST(a_blank_glyph_has_no_bitmap);
    RUN_TEST(get_rejects_a_font_that_is_not_loaded);
    RUN_TEST(cache_survives_growth);
    RUN_TEST(two_fonts_do_not_share_entries);
    RUN_TEST(purge_drops_only_one_font);
    RUN_TEST(a_letter_rasterizes_to_coverage_and_an_emoji_to_colour);
    RUN_TEST(every_kind_of_drawing_comes_out_with_ink_in_it);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(glyphs);
    GREATEST_MAIN_END();
}
