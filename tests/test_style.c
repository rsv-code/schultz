/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_style.c - tokens, the property table, patches and resolution.
 *
 * These cover schultz_style.c on its own, with no tree. The node level
 * behaviour, which is where the layering and invalidation live, is in
 * test_node_style.c.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_style.h"

/* --------------------------------------------------------- token table */

TEST every_color_token_has_a_default(void)
{
    uint32_t i;

    for (i = 0; i < SCHULTZ_TOKEN_COLOR_COUNT; i++) {
        schultz_color c = schultz_theme_color(NULL, i);
        /* Not the magenta that signals an unknown token. */
        ASSERT(!(c.r == 255u && c.g == 0u && c.b == 255u && c.a == 255u));
    }
    PASS();
}

TEST every_number_token_has_a_sane_default(void)
{
    uint32_t i;

    for (i = 0; i < SCHULTZ_TOKEN_NUMBER_COUNT; i++) {
        /*
         * Zero is a real answer for exactly one token: structure is square.
         * Naming the exception keeps the check strong enough to catch a
         * token that was added and never given a default, which is what
         * relaxing this to "not negative" would let through.
         */
        if (i == SCHULTZ_TOKEN_RADIUS_STRUCTURE) {
            ASSERT_EQ(0.0f, schultz_theme_number(NULL, i));
            continue;
        }
        ASSERT(schultz_theme_number(NULL, i) > 0.0f);
    }
    PASS();
}

TEST an_unknown_token_is_loudly_wrong(void)
{
    schultz_color c = schultz_theme_color(NULL, SCHULTZ_TOKEN_COLOR_COUNT);

    ASSERT_EQ(255u, c.r);
    ASSERT_EQ(0u, c.g);
    ASSERT_EQ(255u, c.b);
    ASSERT_EQ(0.0f, schultz_theme_number(NULL, SCHULTZ_TOKEN_NUMBER_COUNT));
    ASSERT_EQ(SCHULTZ_HANDLE_NONE,
              schultz_theme_font(NULL, SCHULTZ_TOKEN_FONT_COUNT));
    PASS();
}

TEST the_transparent_token_is_transparent(void)
{
    ASSERT_EQ(0u, schultz_theme_color(NULL,
                        SCHULTZ_TOKEN_COLOR_TRANSPARENT).a);
    PASS();
}

TEST the_control_height_meets_the_touch_target_minimum(void)
{
    /* An accessibility floor from both mobile platforms, not a style choice. */
    ASSERT(schultz_theme_number(NULL, SCHULTZ_TOKEN_CONTROL_HEIGHT) >= 44.0f);
    PASS();
}

/* An empty theme is valid and means "defaults throughout". */
TEST an_empty_theme_resolves_to_the_defaults(void)
{
    schultz_theme theme;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, schultz_theme_init(&theme));
    for (i = 0; i < SCHULTZ_TOKEN_COLOR_COUNT; i++) {
        ASSERT_EQ(1, schultz_color_equals(schultz_theme_color(NULL, i),
                                          schultz_theme_color(&theme, i)));
    }
    for (i = 0; i < SCHULTZ_TOKEN_NUMBER_COUNT; i++) {
        ASSERT_EQ(schultz_theme_number(NULL, i),
                  schultz_theme_number(&theme, i));
    }
    PASS();
}

TEST a_theme_overrides_only_what_it_sets(void)
{
    schultz_theme theme;
    schultz_color before;

    ASSERT_EQ(SCHULTZ_OK, schultz_theme_init(&theme));
    before = schultz_theme_color(&theme, SCHULTZ_TOKEN_COLOR_SURFACE);

    ASSERT_EQ(SCHULTZ_OK, schultz_theme_set_color(&theme,
                    SCHULTZ_TOKEN_COLOR_ACCENT,
                    schultz_color_rgba(1, 2, 3, 255)));
    ASSERT_EQ(SCHULTZ_OK, schultz_theme_set_number(&theme,
                    SCHULTZ_TOKEN_SPACE_MD, 21.0f));

    ASSERT_EQ(1u, schultz_theme_color(&theme,
                        SCHULTZ_TOKEN_COLOR_ACCENT).r);
    ASSERT_EQ(21.0f, schultz_theme_number(&theme, SCHULTZ_TOKEN_SPACE_MD));
    /* Untouched tokens still read their defaults. */
    ASSERT_EQ(1, schultz_color_equals(before,
                    schultz_theme_color(&theme, SCHULTZ_TOKEN_COLOR_SURFACE)));
    PASS();
}

TEST theme_setters_reject_bad_arguments(void)
{
    schultz_theme theme;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT, schultz_theme_init(NULL));
    ASSERT_EQ(SCHULTZ_OK, schultz_theme_init(&theme));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_theme_set_color(NULL, 0, schultz_color_rgba(0,0,0,0)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_theme_set_color(&theme, SCHULTZ_TOKEN_COLOR_COUNT,
                                      schultz_color_rgba(0,0,0,0)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_theme_set_number(&theme, SCHULTZ_TOKEN_NUMBER_COUNT,
                                       1.0f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_theme_set_font(&theme, SCHULTZ_TOKEN_FONT_COUNT, 1));
    PASS();
}

TEST the_light_preset_differs_from_the_defaults(void)
{
    schultz_theme light;
    schultz_color dark_window;
    schultz_color light_window;

    ASSERT_EQ(SCHULTZ_OK, schultz_theme_preset_light(&light));
    dark_window  = schultz_theme_color(NULL, SCHULTZ_TOKEN_COLOR_WINDOW);
    light_window = schultz_theme_color(&light, SCHULTZ_TOKEN_COLOR_WINDOW);

    ASSERT_EQ(0, schultz_color_equals(dark_window, light_window));
    ASSERT(light_window.r > dark_window.r);  /* and in the right direction */

    /* The preset touches colours only, so numbers keep their defaults. */
    ASSERT_EQ(schultz_theme_number(NULL, SCHULTZ_TOKEN_SPACE_MD),
              schultz_theme_number(&light, SCHULTZ_TOKEN_SPACE_MD));
    PASS();
}

/* ------------------------------------------------------- property table */

TEST every_property_is_fully_described(void)
{
    uint32_t i;

    for (i = 0; i < SCHULTZ_PROP_COUNT; i++) {
        uint32_t kind = schultz_property_kind(i);
        ASSERT(schultz_property_name(i) != NULL);
        ASSERT(kind == SCHULTZ_VALUE_COLOR || kind == SCHULTZ_VALUE_NUMBER ||
               kind == SCHULTZ_VALUE_FONT || kind == SCHULTZ_VALUE_DASH);
    }
    ASSERT_EQ(NULL, schultz_property_name(SCHULTZ_PROP_COUNT));
    PASS();
}

/*
 * The table that keeps a colour change from causing a relayout. Getting a row
 * wrong here quietly discards the main advantage of retained mode, so the
 * classification is asserted explicitly rather than assumed.
 */
TEST paint_properties_do_not_affect_layout(void)
{
    ASSERT_EQ(0, schultz_property_affects_layout(SCHULTZ_PROP_BACKGROUND));
    ASSERT_EQ(0, schultz_property_affects_layout(SCHULTZ_PROP_BORDER_COLOR));
    ASSERT_EQ(0, schultz_property_affects_layout(SCHULTZ_PROP_TEXT_COLOR));
    ASSERT_EQ(0, schultz_property_affects_layout(SCHULTZ_PROP_CORNER_RADIUS));
    ASSERT_EQ(0, schultz_property_affects_layout(SCHULTZ_PROP_OPACITY));
    ASSERT_EQ(0, schultz_property_affects_layout(
                     SCHULTZ_PROP_FOCUS_RING_COLOR));
    PASS();
}

TEST layout_properties_are_marked_as_such(void)
{
    ASSERT_EQ(1, schultz_property_affects_layout(SCHULTZ_PROP_PADDING));
    ASSERT_EQ(1, schultz_property_affects_layout(SCHULTZ_PROP_GAP));
    ASSERT_EQ(1, schultz_property_affects_layout(SCHULTZ_PROP_FONT_SIZE));
    ASSERT_EQ(1, schultz_property_affects_layout(SCHULTZ_PROP_FONT));
    ASSERT_EQ(1, schultz_property_affects_layout(SCHULTZ_PROP_MIN_WIDTH));
    ASSERT_EQ(1, schultz_property_affects_layout(SCHULTZ_PROP_MAX_HEIGHT));
    ASSERT_EQ(1, schultz_property_affects_layout(SCHULTZ_PROP_BORDER_WIDTH));
    PASS();
}

/* The inheriting set is short on purpose; guard it against drift. */
TEST only_text_and_font_properties_inherit(void)
{
    uint32_t i;
    uint32_t inheriting = 0;

    ASSERT_EQ(1, schultz_property_inherits(SCHULTZ_PROP_TEXT_COLOR));
    ASSERT_EQ(1, schultz_property_inherits(SCHULTZ_PROP_FONT));
    ASSERT_EQ(1, schultz_property_inherits(SCHULTZ_PROP_FONT_SIZE));
    /* Prose settings inherit too, so a page sets them once on its container. */
    ASSERT_EQ(1, schultz_property_inherits(SCHULTZ_PROP_TEXT_ALIGN));
    ASSERT_EQ(1, schultz_property_inherits(SCHULTZ_PROP_LINE_SPACING));
    ASSERT_EQ(0, schultz_property_inherits(SCHULTZ_PROP_BACKGROUND));
    ASSERT_EQ(0, schultz_property_inherits(SCHULTZ_PROP_PADDING));

    for (i = 0; i < SCHULTZ_PROP_COUNT; i++) {
        if (schultz_property_inherits(i)) {
            inheriting++;
        }
    }
    ASSERT_EQ(5u, inheriting);
    PASS();
}

TEST out_of_range_properties_report_safely(void)
{
    ASSERT_EQ(0, schultz_property_affects_layout(SCHULTZ_PROP_COUNT));
    ASSERT_EQ(0, schultz_property_inherits(SCHULTZ_PROP_COUNT));
    ASSERT_EQ((uint32_t)SCHULTZ_VALUE_NUMBER,
              schultz_property_kind(SCHULTZ_PROP_COUNT));
    PASS();
}

/* ---------------------------------------------------------------- patch */

TEST a_patch_starts_empty(void)
{
    schultz_patch patch;
    schultz_value value;

    ASSERT_EQ(SCHULTZ_OK, schultz_patch_init(&patch));
    ASSERT_EQ(0u, schultz_patch_count(&patch));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_patch_get(&patch, SCHULTZ_PROP_BACKGROUND, &value));

    schultz_patch_free(&patch);
    PASS();
}

TEST a_patch_stores_and_returns_values(void)
{
    schultz_patch patch;
    schultz_value value;

    ASSERT_EQ(SCHULTZ_OK, schultz_patch_init(&patch));
    ASSERT_EQ(SCHULTZ_OK, schultz_patch_set(&patch, SCHULTZ_PROP_BACKGROUND,
                    schultz_value_color(schultz_color_rgba(1, 2, 3, 4))));
    ASSERT_EQ(SCHULTZ_OK, schultz_patch_set(&patch, SCHULTZ_PROP_PADDING,
                    schultz_value_number(7.5f)));

    ASSERT_EQ(2u, schultz_patch_count(&patch));
    ASSERT_EQ(SCHULTZ_OK, schultz_patch_get(&patch, SCHULTZ_PROP_BACKGROUND,
                                            &value));
    ASSERT_EQ((uint8_t)SCHULTZ_SOURCE_LITERAL, value.source);
    ASSERT_EQ(1u, value.literal.color.r);
    ASSERT_EQ(SCHULTZ_OK, schultz_patch_get(&patch, SCHULTZ_PROP_PADDING,
                                            &value));
    ASSERT_EQ(7.5f, value.literal.number);

    schultz_patch_free(&patch);
    PASS();
}

/* Setting the same property twice must replace, not accumulate. */
TEST setting_a_property_twice_replaces_it(void)
{
    schultz_patch patch;
    schultz_value value;

    ASSERT_EQ(SCHULTZ_OK, schultz_patch_init(&patch));
    schultz_patch_set(&patch, SCHULTZ_PROP_PADDING, schultz_value_number(1));
    schultz_patch_set(&patch, SCHULTZ_PROP_PADDING, schultz_value_number(2));

    ASSERT_EQ(1u, schultz_patch_count(&patch));
    ASSERT_EQ(SCHULTZ_OK, schultz_patch_get(&patch, SCHULTZ_PROP_PADDING,
                                            &value));
    ASSERT_EQ(2.0f, value.literal.number);

    schultz_patch_free(&patch);
    PASS();
}

TEST unsetting_removes_a_property(void)
{
    schultz_patch patch;
    schultz_value value;

    ASSERT_EQ(SCHULTZ_OK, schultz_patch_init(&patch));
    schultz_patch_set(&patch, SCHULTZ_PROP_PADDING, schultz_value_number(1));
    schultz_patch_set(&patch, SCHULTZ_PROP_GAP, schultz_value_number(2));

    ASSERT_EQ(SCHULTZ_OK, schultz_patch_unset(&patch, SCHULTZ_PROP_PADDING));
    ASSERT_EQ(1u, schultz_patch_count(&patch));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_patch_get(&patch, SCHULTZ_PROP_PADDING, &value));
    /* The survivor is still reachable, so the shift did not corrupt it. */
    ASSERT_EQ(SCHULTZ_OK, schultz_patch_get(&patch, SCHULTZ_PROP_GAP,
                                            &value));
    ASSERT_EQ(2.0f, value.literal.number);

    /* Unsetting something absent is not an error. */
    ASSERT_EQ(SCHULTZ_OK, schultz_patch_unset(&patch, SCHULTZ_PROP_OPACITY));

    schultz_patch_free(&patch);
    PASS();
}

TEST a_patch_grows_past_its_initial_capacity(void)
{
    schultz_patch patch;
    schultz_value value;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, schultz_patch_init(&patch));
    for (i = 0; i < SCHULTZ_PROP_COUNT; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_patch_set(&patch, i,
                        schultz_value_number((float)i)));
    }
    ASSERT_EQ((uint32_t)SCHULTZ_PROP_COUNT, schultz_patch_count(&patch));

    /* Every one still readable after the reallocations. */
    for (i = 0; i < SCHULTZ_PROP_COUNT; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_patch_get(&patch, i, &value));
        ASSERT_EQ((float)i, value.literal.number);
    }

    schultz_patch_free(&patch);
    PASS();
}

TEST merging_copies_settings_without_clearing_others(void)
{
    schultz_patch dest;
    schultz_patch source;
    schultz_value value;

    schultz_patch_init(&dest);
    schultz_patch_init(&source);
    schultz_patch_set(&dest, SCHULTZ_PROP_PADDING, schultz_value_number(1));
    schultz_patch_set(&dest, SCHULTZ_PROP_GAP, schultz_value_number(2));
    schultz_patch_set(&source, SCHULTZ_PROP_GAP, schultz_value_number(9));
    schultz_patch_set(&source, SCHULTZ_PROP_OPACITY,
                      schultz_value_number(0.5f));

    ASSERT_EQ(SCHULTZ_OK, schultz_patch_merge(&dest, &source));
    ASSERT_EQ(3u, schultz_patch_count(&dest));

    schultz_patch_get(&dest, SCHULTZ_PROP_PADDING, &value);
    ASSERT_EQ(1.0f, value.literal.number);  /* untouched */
    schultz_patch_get(&dest, SCHULTZ_PROP_GAP, &value);
    ASSERT_EQ(9.0f, value.literal.number);  /* overwritten */
    schultz_patch_get(&dest, SCHULTZ_PROP_OPACITY, &value);
    ASSERT_EQ(0.5f, value.literal.number);  /* added */

    schultz_patch_free(&dest);
    schultz_patch_free(&source);
    PASS();
}

TEST patch_operations_reject_bad_arguments(void)
{
    schultz_patch patch;
    schultz_value value;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT, schultz_patch_init(NULL));
    ASSERT_EQ(SCHULTZ_OK, schultz_patch_init(&patch));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_patch_set(NULL, 0, schultz_value_number(1)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_patch_set(&patch, SCHULTZ_PROP_COUNT,
                                schultz_value_number(1)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_patch_get(NULL, 0, &value));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_patch_get(&patch, 0, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT, schultz_patch_merge(&patch, NULL));
    ASSERT_EQ(0u, schultz_patch_count(NULL));

    /* Safe on NULL and safe twice. */
    schultz_patch_free(NULL);
    schultz_patch_clear(NULL);
    schultz_patch_free(&patch);
    schultz_patch_free(&patch);
    PASS();
}

TEST clearing_keeps_storage_but_drops_settings(void)
{
    schultz_patch patch;

    schultz_patch_init(&patch);
    schultz_patch_set(&patch, SCHULTZ_PROP_PADDING, schultz_value_number(1));
    schultz_patch_clear(&patch);
    ASSERT_EQ(0u, schultz_patch_count(&patch));

    /* And it is reusable afterwards. */
    ASSERT_EQ(SCHULTZ_OK, schultz_patch_set(&patch, SCHULTZ_PROP_GAP,
                                            schultz_value_number(3)));
    ASSERT_EQ(1u, schultz_patch_count(&patch));

    schultz_patch_free(&patch);
    PASS();
}

/* ----------------------------------------------------------- resolution */

TEST defaults_fill_every_property(void)
{
    schultz_resolved_style resolved;

    ASSERT_EQ(SCHULTZ_OK, schultz_resolved_defaults(&resolved, NULL));

    /* Text colour comes from its token, and is therefore opaque. */
    ASSERT_EQ(255u, schultz_resolved_color(&resolved,
                        SCHULTZ_PROP_TEXT_COLOR).a);
    /* Background defaults to transparent: a node paints nothing by default. */
    ASSERT_EQ(0u, schultz_resolved_color(&resolved,
                        SCHULTZ_PROP_BACKGROUND).a);
    ASSERT_EQ(1.0f, schultz_resolved_number(&resolved, SCHULTZ_PROP_OPACITY));
    ASSERT_EQ(16.0f, schultz_resolved_number(&resolved,
                        SCHULTZ_PROP_FONT_SIZE));
    /* An unset preferred size is negative, meaning "compute it". */
    ASSERT(schultz_resolved_number(&resolved, SCHULTZ_PROP_PREF_WIDTH) < 0.0f);
    PASS();
}

TEST defaults_follow_the_theme(void)
{
    schultz_theme theme;
    schultz_resolved_style resolved;

    schultz_theme_init(&theme);
    schultz_theme_set_color(&theme, SCHULTZ_TOKEN_COLOR_TEXT,
                            schultz_color_rgba(9, 8, 7, 255));
    schultz_theme_set_number(&theme, SCHULTZ_TOKEN_FONT_SIZE_BODY, 22.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_resolved_defaults(&resolved, &theme));
    ASSERT_EQ(9u, schultz_resolved_color(&resolved,
                        SCHULTZ_PROP_TEXT_COLOR).r);
    ASSERT_EQ(22.0f, schultz_resolved_number(&resolved,
                        SCHULTZ_PROP_FONT_SIZE));
    PASS();
}

TEST applying_a_patch_overwrites_only_its_properties(void)
{
    schultz_resolved_style resolved;
    schultz_patch patch;
    float radius_before;

    schultz_resolved_defaults(&resolved, NULL);
    radius_before = schultz_resolved_number(&resolved,
                                            SCHULTZ_PROP_CORNER_RADIUS);

    schultz_patch_init(&patch);
    schultz_patch_set(&patch, SCHULTZ_PROP_BACKGROUND,
                      schultz_value_color(schultz_color_rgba(5, 5, 5, 255)));
    ASSERT_EQ(SCHULTZ_OK, schultz_resolved_apply(&resolved, &patch, NULL));

    ASSERT_EQ(5u, schultz_resolved_color(&resolved,
                        SCHULTZ_PROP_BACKGROUND).r);
    ASSERT_EQ(radius_before, schultz_resolved_number(&resolved,
                        SCHULTZ_PROP_CORNER_RADIUS));

    schultz_patch_free(&patch);
    PASS();
}

/* The indirection that lets a style follow a theme change. */
TEST a_token_value_resolves_through_the_theme(void)
{
    schultz_resolved_style resolved;
    schultz_patch patch;
    schultz_theme theme;

    schultz_patch_init(&patch);
    schultz_patch_set(&patch, SCHULTZ_PROP_BACKGROUND,
                      schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));

    /* Under the defaults. */
    schultz_resolved_defaults(&resolved, NULL);
    schultz_resolved_apply(&resolved, &patch, NULL);
    ASSERT_EQ(1, schultz_color_equals(
        schultz_theme_color(NULL, SCHULTZ_TOKEN_COLOR_ACCENT),
        schultz_resolved_color(&resolved, SCHULTZ_PROP_BACKGROUND)));

    /* The same patch under a theme that redefines the token. */
    schultz_theme_init(&theme);
    schultz_theme_set_color(&theme, SCHULTZ_TOKEN_COLOR_ACCENT,
                            schultz_color_rgba(11, 22, 33, 255));
    schultz_resolved_defaults(&resolved, &theme);
    schultz_resolved_apply(&resolved, &patch, &theme);
    ASSERT_EQ(11u, schultz_resolved_color(&resolved,
                        SCHULTZ_PROP_BACKGROUND).r);

    schultz_patch_free(&patch);
    PASS();
}

/* A literal deliberately does not follow the theme. */
TEST a_literal_value_ignores_the_theme(void)
{
    schultz_resolved_style resolved;
    schultz_patch patch;
    schultz_theme theme;

    schultz_patch_init(&patch);
    schultz_patch_set(&patch, SCHULTZ_PROP_BACKGROUND,
                      schultz_value_color(schultz_color_rgba(7, 7, 7, 255)));

    schultz_theme_init(&theme);
    schultz_theme_set_color(&theme, SCHULTZ_TOKEN_COLOR_ACCENT,
                            schultz_color_rgba(11, 22, 33, 255));
    schultz_resolved_defaults(&resolved, &theme);
    schultz_resolved_apply(&resolved, &patch, &theme);

    ASSERT_EQ(7u, schultz_resolved_color(&resolved,
                        SCHULTZ_PROP_BACKGROUND).r);

    schultz_patch_free(&patch);
    PASS();
}

TEST later_patches_win(void)
{
    schultz_resolved_style resolved;
    schultz_patch first;
    schultz_patch second;

    schultz_resolved_defaults(&resolved, NULL);
    schultz_patch_init(&first);
    schultz_patch_init(&second);
    schultz_patch_set(&first, SCHULTZ_PROP_PADDING, schultz_value_number(4));
    schultz_patch_set(&first, SCHULTZ_PROP_GAP, schultz_value_number(4));
    schultz_patch_set(&second, SCHULTZ_PROP_PADDING, schultz_value_number(9));

    schultz_resolved_apply(&resolved, &first, NULL);
    schultz_resolved_apply(&resolved, &second, NULL);

    ASSERT_EQ(9.0f, schultz_resolved_number(&resolved, SCHULTZ_PROP_PADDING));
    /* What the later patch did not mention survives. */
    ASSERT_EQ(4.0f, schultz_resolved_number(&resolved, SCHULTZ_PROP_GAP));

    schultz_patch_free(&first);
    schultz_patch_free(&second);
    PASS();
}

TEST reading_a_property_of_the_wrong_kind_is_loudly_wrong(void)
{
    schultz_resolved_style resolved;
    schultz_color c;

    schultz_resolved_defaults(&resolved, NULL);
    /* Padding is a number, not a colour. */
    c = schultz_resolved_color(&resolved, SCHULTZ_PROP_PADDING);
    ASSERT_EQ(255u, c.r);
    ASSERT_EQ(0u, c.g);
    ASSERT_EQ(255u, c.b);

    ASSERT_EQ(0.0f, schultz_resolved_number(&resolved,
                        SCHULTZ_PROP_BACKGROUND));
    ASSERT_EQ(SCHULTZ_HANDLE_NONE,
              schultz_resolved_font(&resolved, SCHULTZ_PROP_PADDING));
    /* And NULL is handled rather than dereferenced. */
    ASSERT_EQ(0.0f, schultz_resolved_number(NULL, SCHULTZ_PROP_PADDING));
    PASS();
}

TEST comparison_separates_layout_changes_from_paint_changes(void)
{
    schultz_resolved_style a;
    schultz_resolved_style b;
    schultz_patch patch;

    schultz_resolved_defaults(&a, NULL);
    b = a;
    ASSERT_EQ(0, schultz_resolved_differs(&a, &b));
    ASSERT_EQ(0, schultz_resolved_layout_differs(&a, &b));

    /* A colour change differs, but not in layout. */
    schultz_patch_init(&patch);
    schultz_patch_set(&patch, SCHULTZ_PROP_BACKGROUND,
                      schultz_value_color(schultz_color_rgba(1, 1, 1, 255)));
    schultz_resolved_apply(&b, &patch, NULL);
    ASSERT_EQ(1, schultz_resolved_differs(&a, &b));
    ASSERT_EQ(0, schultz_resolved_layout_differs(&a, &b));

    /* A padding change differs in layout too. */
    schultz_patch_clear(&patch);
    schultz_patch_set(&patch, SCHULTZ_PROP_PADDING, schultz_value_number(99));
    schultz_resolved_apply(&b, &patch, NULL);
    ASSERT_EQ(1, schultz_resolved_layout_differs(&a, &b));

    schultz_patch_free(&patch);
    PASS();
}

TEST comparison_covers_every_property(void)
{
    schultz_resolved_style base;
    uint32_t i;

    schultz_resolved_defaults(&base, NULL);

    /* Changing any single property must be detected. */
    for (i = 0; i < SCHULTZ_PROP_COUNT; i++) {
        schultz_resolved_style changed = base;
        switch (schultz_property_kind(i)) {
        case SCHULTZ_VALUE_COLOR:
            changed.values[i].paint = schultz_paint_solid(
                schultz_color_rgba(0x12, 0x34, 0x56, 0x78));
            break;
        case SCHULTZ_VALUE_FONT:
            changed.values[i].font = (schultz_handle)999;
            break;
        case SCHULTZ_VALUE_DASH:
            changed.values[i].dash = (schultz_handle)999;
            break;
        default:
            changed.values[i].number =
                base.values[i].number + 123.5f;
            break;
        }
        ASSERT_EQ(1, schultz_resolved_differs(&base, &changed));
        ASSERT_EQ(schultz_property_affects_layout(i),
                  schultz_resolved_layout_differs(&base, &changed));
    }
    PASS();
}

TEST resolution_rejects_bad_arguments(void)
{
    schultz_patch patch;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_resolved_defaults(NULL, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_resolved_apply(NULL, NULL, NULL));
    ASSERT_EQ(1, schultz_resolved_differs(NULL, NULL));
    ASSERT_EQ(1, schultz_resolved_layout_differs(NULL, NULL));

    /* A NULL patch applies nothing rather than failing. */
    {
        schultz_resolved_style resolved;
        schultz_resolved_defaults(&resolved, NULL);
        ASSERT_EQ(SCHULTZ_OK, schultz_resolved_apply(&resolved, NULL, NULL));
    }
    schultz_patch_init(&patch);
    schultz_patch_free(&patch);
    PASS();
}

SUITE(style)
{
    RUN_TEST(every_color_token_has_a_default);
    RUN_TEST(every_number_token_has_a_sane_default);
    RUN_TEST(an_unknown_token_is_loudly_wrong);
    RUN_TEST(the_transparent_token_is_transparent);
    RUN_TEST(the_control_height_meets_the_touch_target_minimum);
    RUN_TEST(an_empty_theme_resolves_to_the_defaults);
    RUN_TEST(a_theme_overrides_only_what_it_sets);
    RUN_TEST(theme_setters_reject_bad_arguments);
    RUN_TEST(the_light_preset_differs_from_the_defaults);
    RUN_TEST(every_property_is_fully_described);
    RUN_TEST(paint_properties_do_not_affect_layout);
    RUN_TEST(layout_properties_are_marked_as_such);
    RUN_TEST(only_text_and_font_properties_inherit);
    RUN_TEST(out_of_range_properties_report_safely);
    RUN_TEST(a_patch_starts_empty);
    RUN_TEST(a_patch_stores_and_returns_values);
    RUN_TEST(setting_a_property_twice_replaces_it);
    RUN_TEST(unsetting_removes_a_property);
    RUN_TEST(a_patch_grows_past_its_initial_capacity);
    RUN_TEST(merging_copies_settings_without_clearing_others);
    RUN_TEST(patch_operations_reject_bad_arguments);
    RUN_TEST(clearing_keeps_storage_but_drops_settings);
    RUN_TEST(defaults_fill_every_property);
    RUN_TEST(defaults_follow_the_theme);
    RUN_TEST(applying_a_patch_overwrites_only_its_properties);
    RUN_TEST(a_token_value_resolves_through_the_theme);
    RUN_TEST(a_literal_value_ignores_the_theme);
    RUN_TEST(later_patches_win);
    RUN_TEST(reading_a_property_of_the_wrong_kind_is_loudly_wrong);
    RUN_TEST(comparison_separates_layout_changes_from_paint_changes);
    RUN_TEST(comparison_covers_every_property);
    RUN_TEST(resolution_rejects_bad_arguments);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(style);
    GREATEST_MAIN_END();
}
