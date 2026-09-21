/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_geom.c - geometry primitives.
 */

#include <stddef.h>

#include "greatest.h"
#include "schultz_geom.h"

TEST rect_is_empty_when_an_extent_is_not_positive(void)
{
    ASSERT_EQ(0, schultz_rect_is_empty(schultz_rect_make(0, 0, 10, 10)));
    ASSERT_EQ(1, schultz_rect_is_empty(schultz_rect_make(0, 0, 0, 10)));
    ASSERT_EQ(1, schultz_rect_is_empty(schultz_rect_make(0, 0, 10, 0)));
    ASSERT_EQ(1, schultz_rect_is_empty(schultz_rect_make(0, 0, -5, 10)));
    PASS();
}

TEST rect_edges(void)
{
    schultz_rect rect = schultz_rect_make(10, 20, 30, 40);
    ASSERT_EQ(40.0f, schultz_rect_right(rect));
    ASSERT_EQ(60.0f, schultz_rect_bottom(rect));
    PASS();
}

TEST rect_equals_compares_all_fields(void)
{
    schultz_rect a = schultz_rect_make(1, 2, 3, 4);
    ASSERT_EQ(1, schultz_rect_equals(a, schultz_rect_make(1, 2, 3, 4)));
    ASSERT_EQ(0, schultz_rect_equals(a, schultz_rect_make(9, 2, 3, 4)));
    ASSERT_EQ(0, schultz_rect_equals(a, schultz_rect_make(1, 9, 3, 4)));
    ASSERT_EQ(0, schultz_rect_equals(a, schultz_rect_make(1, 2, 9, 4)));
    ASSERT_EQ(0, schultz_rect_equals(a, schultz_rect_make(1, 2, 3, 9)));
    PASS();
}

TEST intersect_of_overlapping_rects(void)
{
    schultz_rect a = schultz_rect_make(0, 0, 100, 100);
    schultz_rect b = schultz_rect_make(50, 50, 100, 100);
    schultz_rect hit = schultz_rect_intersect(a, b);

    ASSERT_EQ(1, schultz_rect_equals(hit, schultz_rect_make(50, 50, 50, 50)));
    PASS();
}

TEST intersect_is_commutative(void)
{
    schultz_rect a = schultz_rect_make(10, 5, 40, 60);
    schultz_rect b = schultz_rect_make(20, 20, 100, 10);

    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_intersect(a, b),
                                schultz_rect_intersect(b, a)));
    PASS();
}

TEST intersect_of_disjoint_rects_is_empty(void)
{
    schultz_rect a = schultz_rect_make(0, 0, 10, 10);
    schultz_rect b = schultz_rect_make(50, 50, 10, 10);

    ASSERT_EQ(1, schultz_rect_is_empty(schultz_rect_intersect(a, b)));
    PASS();
}

TEST touching_rects_do_not_intersect(void)
{
    schultz_rect a = schultz_rect_make(0, 0, 10, 10);
    schultz_rect b = schultz_rect_make(10, 0, 10, 10);

    ASSERT_EQ(1, schultz_rect_is_empty(schultz_rect_intersect(a, b)));
    PASS();
}

TEST intersect_with_contained_rect_returns_it(void)
{
    schultz_rect outer = schultz_rect_make(0, 0, 100, 100);
    schultz_rect inner = schultz_rect_make(25, 25, 10, 10);

    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_intersect(outer, inner), inner));
    PASS();
}

/*
 * Left and top edges belong to the rect, right and bottom do not, so two
 * adjacent rects never both claim a point on their shared edge.
 */
TEST contains_point_owns_the_left_and_top_edges_only(void)
{
    schultz_rect rect = schultz_rect_make(10, 10, 20, 20);

    ASSERT_EQ(1, schultz_rect_contains_point(rect, schultz_point_make(10, 10)));
    ASSERT_EQ(1, schultz_rect_contains_point(rect, schultz_point_make(29.9f, 29.9f)));
    ASSERT_EQ(0, schultz_rect_contains_point(rect, schultz_point_make(30, 20)));
    ASSERT_EQ(0, schultz_rect_contains_point(rect, schultz_point_make(20, 30)));
    ASSERT_EQ(0, schultz_rect_contains_point(rect, schultz_point_make(9.9f, 20)));
    PASS();
}

TEST empty_rect_contains_nothing(void)
{
    schultz_rect empty = schultz_rect_make(10, 10, 0, 0);
    ASSERT_EQ(0, schultz_rect_contains_point(empty, schultz_point_make(10, 10)));
    PASS();
}

TEST color_equals_compares_all_channels(void)
{
    schultz_color c = schultz_color_rgba(1, 2, 3, 4);
    ASSERT_EQ(1, schultz_color_equals(c, schultz_color_rgba(1, 2, 3, 4)));
    ASSERT_EQ(0, schultz_color_equals(c, schultz_color_rgba(9, 2, 3, 4)));
    ASSERT_EQ(0, schultz_color_equals(c, schultz_color_rgba(1, 2, 3, 9)));
    PASS();
}

TEST size_and_point_constructors(void)
{
    schultz_size size = schultz_size_make(3.5f, 4.5f);
    schultz_point point = schultz_point_make(-1.0f, 2.0f);

    ASSERT_EQ(3.5f, size.width);
    ASSERT_EQ(4.5f, size.height);
    ASSERT_EQ(-1.0f, point.x);
    ASSERT_EQ(2.0f, point.y);
    PASS();
}

/*
 * Regression tests for the trailing-stripe bug. A dirty region that moves by
 * a fractional amount each frame is handed to integer pixel APIs, and
 * truncating there loses part of the far edge. The loss accumulates until a
 * whole column of the previous frame survives on screen, which looks like a
 * comb of stripes behind a moving object.
 */
TEST pixel_bounds_rounds_outward_not_toward_zero(void)
{
    schultz_rect box = schultz_rect_pixel_bounds(
        schultz_rect_make(105.3f, 250.0f, 92.6f, 40.0f), 0.0f);

    /* Left floors to 105, right ceils from 197.9 to 198. */
    ASSERT_EQ(105.0f, box.x);
    ASSERT_EQ(198.0f, schultz_rect_right(box));
    ASSERT_EQ(93.0f, box.width);
    PASS();
}

TEST pixel_bounds_always_contains_the_input(void)
{
    float offsets[] = { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 0.99f };
    size_t i;
    size_t j;

    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        for (j = 0; j < sizeof(offsets) / sizeof(offsets[0]); j++) {
            schultz_rect input =
                schultz_rect_make(10.0f + offsets[i], 20.0f + offsets[j],
                                  30.0f + offsets[j], 40.0f + offsets[i]);
            schultz_rect box = schultz_rect_pixel_bounds(input, 0.0f);

            ASSERT(box.x <= input.x);
            ASSERT(box.y <= input.y);
            ASSERT(schultz_rect_right(box) >= schultz_rect_right(input));
            ASSERT(schultz_rect_bottom(box) >= schultz_rect_bottom(input));
        }
    }
    PASS();
}

TEST pixel_bounds_applies_slack_on_every_side(void)
{
    schultz_rect box = schultz_rect_pixel_bounds(
        schultz_rect_make(10.0f, 20.0f, 30.0f, 40.0f), 1.0f);

    ASSERT_EQ(9.0f, box.x);
    ASSERT_EQ(19.0f, box.y);
    ASSERT_EQ(41.0f, schultz_rect_right(box));
    ASSERT_EQ(61.0f, schultz_rect_bottom(box));
    PASS();
}

TEST pixel_bounds_of_an_already_aligned_rect_is_unchanged(void)
{
    schultz_rect input = schultz_rect_make(10.0f, 20.0f, 30.0f, 40.0f);
    ASSERT_EQ(1, schultz_rect_equals(input,
                                     schultz_rect_pixel_bounds(input, 0.0f)));
    PASS();
}

TEST pixel_bounds_of_an_empty_rect_is_empty(void)
{
    ASSERT_EQ(1, schultz_rect_is_empty(
        schultz_rect_pixel_bounds(schultz_rect_make(5, 5, 0, 0), 1.0f)));
    ASSERT_EQ(1, schultz_rect_is_empty(
        schultz_rect_pixel_bounds(schultz_rect_make(5, 5, -3, 10), 1.0f)));
    PASS();
}

TEST expanding_a_rectangle_moves_every_edge(void)
{
    schultz_rect r = schultz_rect_make(10, 20, 30, 40);
    schultz_rect grown = schultz_rect_expand(r, 5.0f);
    schultz_rect same = schultz_rect_expand(r, 0.0f);
    schultz_rect shrunk = schultz_rect_expand(r, -5.0f);

    ASSERT_EQ(5.0f, grown.x);
    ASSERT_EQ(15.0f, grown.y);
    ASSERT_EQ(40.0f, grown.width);
    ASSERT_EQ(50.0f, grown.height);

    ASSERT_EQ(r.x, same.x);
    ASSERT_EQ(r.width, same.width);

    /* Negative shrinks, which is what an inset wants. */
    ASSERT_EQ(15.0f, shrunk.x);
    ASSERT_EQ(20.0f, shrunk.width);

    PASS();
}

/* Shrinking past nothing gives an empty rectangle, never a negative size. */
TEST shrinking_past_nothing_gives_an_empty_rectangle(void)
{
    schultz_rect r = schultz_rect_make(10, 20, 4, 6);
    schultz_rect gone = schultz_rect_expand(r, -10.0f);

    ASSERT_EQ(0.0f, gone.width);
    ASSERT_EQ(0.0f, gone.height);
    ASSERT(schultz_rect_is_empty(gone));
    /* Collapsed to the middle, so it stays where the rectangle was. */
    ASSERT_EQ(12.0f, gone.x);
    ASSERT_EQ(23.0f, gone.y);

    PASS();
}

SUITE(geom)
{
    RUN_TEST(expanding_a_rectangle_moves_every_edge);
    RUN_TEST(shrinking_past_nothing_gives_an_empty_rectangle);
    RUN_TEST(rect_is_empty_when_an_extent_is_not_positive);
    RUN_TEST(rect_edges);
    RUN_TEST(rect_equals_compares_all_fields);
    RUN_TEST(intersect_of_overlapping_rects);
    RUN_TEST(intersect_is_commutative);
    RUN_TEST(intersect_of_disjoint_rects_is_empty);
    RUN_TEST(touching_rects_do_not_intersect);
    RUN_TEST(intersect_with_contained_rect_returns_it);
    RUN_TEST(contains_point_owns_the_left_and_top_edges_only);
    RUN_TEST(empty_rect_contains_nothing);
    RUN_TEST(color_equals_compares_all_channels);
    RUN_TEST(size_and_point_constructors);
    RUN_TEST(pixel_bounds_rounds_outward_not_toward_zero);
    RUN_TEST(pixel_bounds_always_contains_the_input);
    RUN_TEST(pixel_bounds_applies_slack_on_every_side);
    RUN_TEST(pixel_bounds_of_an_already_aligned_rect_is_unchanged);
    RUN_TEST(pixel_bounds_of_an_empty_rect_is_empty);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(geom);
    GREATEST_MAIN_END();
}
