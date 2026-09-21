/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_setters.c - a setter given the value it already holds does no work.
 *
 * Laying out is the expensive half of a turn, and a host that pushes its
 * state at the tree every turn calls these setters every turn, nearly always
 * with the value they already hold. Each test here does the same two things:
 * set a value, then set it again and check that nothing was asked of layout,
 * then set a different value and check that something was.
 *
 * The second half matters as much as the first. A gate that never lets
 * anything through would pass the first check and break the widget.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "schultz_node.h"
#include "schultz_widgets.h"

/* Has anything asked for a layout since the last time this was cleared? */
#define FRESH(tree) schultz_tree_clear_layout_dirty(tree)
#define ASKED(tree, node) schultz_node_layout_dirty(tree, node)

/* ------------------------------------------------------------------ label */

TEST a_label_set_to_the_words_it_already_shows_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_label_create(tree, schultz_tree_root(tree),
                                               "00:00", &node));

    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_text(tree, node, "00:00"));
    ASSERT_EQ(0, ASKED(tree, node));

    /* A clock that has moved on has to be measured again. */
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_text(tree, node, "00:01"));
    ASSERT_EQ(1, ASKED(tree, node));

    /* And the words are the ones that were set, so the gate did not eat the
     * change it let through. */
    ASSERT_STR_EQ("00:01", schultz_label_text(tree, node));

    /* A label wraps when it is made, so asking it to wrap asks nothing. */
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_wrap(tree, node, 1));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_wrap(tree, node, 0));
    ASSERT_EQ(1, ASKED(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------------------------- the shapes */

TEST a_line_set_to_where_it_already_is_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_point from = schultz_point_make(0.0f, 0.0f);
    schultz_point to = schultz_point_make(10.0f, 10.0f);
    schultz_point along = schultz_point_make(10.0f, 11.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_line_create(tree, schultz_tree_root(tree),
                                              from, to, &node));

    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_line_set_points(tree, node, from, to));
    ASSERT_EQ(0, ASKED(tree, node));

    /* One end moved by a pixel is still a move. */
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_line_set_points(tree, node, from, along));
    ASSERT_EQ(1, ASKED(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_polygon_set_to_the_points_it_already_has_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_point three[3];
    schultz_point moved[3];
    schultz_point four[4];

    three[0] = schultz_point_make(0.0f, 0.0f);
    three[1] = schultz_point_make(10.0f, 0.0f);
    three[2] = schultz_point_make(5.0f, 8.0f);
    memcpy(moved, three, sizeof(three));
    moved[2] = schultz_point_make(5.0f, 9.0f);
    memcpy(four, three, sizeof(three));
    four[3] = schultz_point_make(0.0f, 8.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_polygon_create(tree, schultz_tree_root(tree), three, 3u,
                                     &node));

    /* The same vertices, in a different array. It is the values that count,
     * not where they were handed over from. */
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_polygon_set_points(tree, node, three, 3u));
    ASSERT_EQ(0, ASKED(tree, node));

    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_polygon_set_points(tree, node, moved, 3u));
    ASSERT_EQ(1, ASKED(tree, node));

    /* More of them, with the first three unchanged: still a change. */
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_polygon_set_points(tree, node, four, 4u));
    ASSERT_EQ(1, ASKED(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

/* --------------------------------------------------------------- the rows */

TEST a_button_bar_set_to_what_it_already_is_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_button_bar_create(tree, schultz_tree_root(tree),
                                        &node));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_button_bar_set_order(tree, node,
                                           SCHULTZ_BUTTON_ORDER_LINUX));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_button_bar_set_order(tree, node,
                                           SCHULTZ_BUTTON_ORDER_LINUX));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_button_bar_set_order(tree, node,
                                           SCHULTZ_BUTTON_ORDER_WINDOWS));
    ASSERT_EQ(1, ASKED(tree, node));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_button_bar_set_uniform_width(tree, node, 1));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_button_bar_set_uniform_width(tree, node, 1));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_button_bar_set_uniform_width(tree, node, 0));
    ASSERT_EQ(1, ASKED(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_toolbar_told_the_same_thing_twice_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_toolbar_create(tree, schultz_tree_root(tree),
                                     SCHULTZ_ORIENT_HORIZONTAL, &node));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_toolbar_set_overflow_enabled(tree, node, 1));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_toolbar_set_overflow_enabled(tree, node, 1));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_toolbar_set_overflow_enabled(tree, node, 0));
    ASSERT_EQ(1, ASKED(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

/* --------------------------------------------------------------- the text */

TEST a_text_field_told_the_same_thing_twice_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle area = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_field_create(tree, schultz_tree_root(tree), "",
                                        &node));

    ASSERT_EQ(SCHULTZ_OK, schultz_text_field_set_mask(tree, node, 0x2022u));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_text_field_set_mask(tree, node, 0x2022u));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_text_field_set_mask(tree, node, 0u));
    ASSERT_EQ(1, ASKED(tree, node));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_area_create(tree, schultz_tree_root(tree), "",
                                       &area));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_area_set_visible_lines(tree, area, 4u));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_area_set_visible_lines(tree, area, 4u));
    ASSERT_EQ(0, ASKED(tree, area));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_area_set_visible_lines(tree, area, 6u));
    ASSERT_EQ(1, ASKED(tree, area));

    ASSERT_EQ(SCHULTZ_OK, schultz_text_area_set_grows(tree, area, 1));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_text_area_set_grows(tree, area, 1));
    ASSERT_EQ(0, ASKED(tree, area));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_text_area_set_grows(tree, area, 0));
    ASSERT_EQ(1, ASKED(tree, area));

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------------- the furniture */

TEST a_scroll_view_told_the_same_thing_twice_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_scroll_view_create(tree, schultz_tree_root(tree),
                                         &node));

    /* Bars are shown when one is made, so this is the value it holds. */
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_set_bars(tree, node, 1));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_scroll_view_set_bars(tree, node, 0));
    ASSERT_EQ(1, ASKED(tree, node));
    ASSERT_EQ(0, schultz_scroll_view_shows_bars(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_busy_indicator_set_to_its_own_size_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_busy_indicator_create(tree, schultz_tree_root(tree),
                                            &node));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_busy_indicator_set_size(tree, node, 32.0f));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_busy_indicator_set_size(tree, node, 32.0f));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_busy_indicator_set_size(tree, node, 48.0f));
    ASSERT_EQ(1, ASKED(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_popup_told_the_same_thing_twice_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_create(tree, 0, &node));

    ASSERT_EQ(SCHULTZ_OK, schultz_popup_set_arrow(tree, node, 1));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_set_arrow(tree, node, 1));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_set_arrow(tree, node, 0));
    ASSERT_EQ(1, ASKED(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

/* ----------------------------------------------------------- the pickers */

TEST a_date_picker_set_to_its_own_format_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_date_picker_create(tree, schultz_tree_root(tree),
                                         &node));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_date_picker_set_format(tree, node, SCHULTZ_DATE_MDY));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_date_picker_set_format(tree, node, SCHULTZ_DATE_MDY));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_date_picker_set_format(tree, node, SCHULTZ_DATE_ISO));
    ASSERT_EQ(1, ASKED(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_time_picker_told_the_same_thing_twice_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_time_picker_create(tree, schultz_tree_root(tree),
                                         &node));

    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_set_24_hour(tree, node, 1));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_set_24_hour(tree, node, 1));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_set_24_hour(tree, node, 0));
    ASSERT_EQ(1, ASKED(tree, node));

    /* Seconds are hidden when a picker is made, so asking for them to stay
     * hidden is asking for nothing. */
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_time_picker_set_show_seconds(tree, node, 0));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_time_picker_set_show_seconds(tree, node, 1));
    ASSERT_EQ(1, ASKED(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------------- the ones that open up */

TEST a_tree_view_told_to_open_what_is_open_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_tree_view_create(tree, schultz_tree_root(tree), &node));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_tree_view_add(tree, node, SCHULTZ_HANDLE_NONE, &row));

    /* Rows start closed. */
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_expand(tree, node, row, 0));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_expand(tree, node, row, 1));
    ASSERT_EQ(1, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_expand(tree, node, row, 1));
    ASSERT_EQ(0, ASKED(tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_set_indent(tree, node, 20.0f));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_set_indent(tree, node, 20.0f));
    ASSERT_EQ(0, ASKED(tree, node));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_view_set_indent(tree, node, 24.0f));
    ASSERT_EQ(1, ASKED(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST an_accordion_told_to_open_what_is_open_does_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_handle first = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_accordion_create(tree, schultz_tree_root(tree), &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_add(tree, node, "One", &body));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, node, 0, &first));

    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_expand(tree, node, first, 1));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_expand(tree, node, first, 1));
    ASSERT_EQ(0, ASKED(tree, node));
    ASSERT_EQ(1, schultz_accordion_is_expanded(tree, first));
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_expand(tree, node, first, 0));
    ASSERT_EQ(1, ASKED(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * The gate on an accordion cannot be an early return.
 *
 * Turning the one at a time rule on while two are open leaves the rule
 * broken, and it is the next expand that puts it right -- including an
 * expand of a section that is already open, which is the case a plain
 * "nothing changed, go home" would skip.
 */
TEST an_accordion_still_closes_the_others_when_nothing_moved(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_handle first = SCHULTZ_HANDLE_NONE;
    schultz_handle second = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_accordion_create(tree, schultz_tree_root(tree), &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_add(tree, node, "One", &body));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, node, 0, &first));
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_add(tree, node, "Two", &body));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, node, 2, &second));

    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_expand(tree, node, first, 1));
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_expand(tree, node, second, 1));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_accordion_set_single_expand(tree, node, 1));

    /* first is already open, so nothing about it moves -- but second has to
     * close, and the layout has to hear about it. */
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_accordion_expand(tree, node, first, 1));
    ASSERT_EQ(1, schultz_accordion_is_expanded(tree, first));
    ASSERT_EQ(0, schultz_accordion_is_expanded(tree, second));
    ASSERT_EQ(1, ASKED(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * And the tab view's gate cannot be an early return either: adding a tab
 * selects the one already selected, and that call is what hides the pages
 * the add did not make.
 */
TEST adding_a_tab_still_hides_the_pages_that_are_not_chosen(void)
{
    schultz_tree *tree;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle one = SCHULTZ_HANDLE_NONE;
    schultz_handle two = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_tab_view_create(tree, schultz_tree_root(tree), &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_add(tree, node, "One", &one));
    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_add(tree, node, "Two", &two));

    /* The first tab stays chosen, so the second page is hidden. */
    ASSERT_EQ(0u, schultz_tab_view_selected(tree, node));
    ASSERT(schultz_node_get_state(tree, one) & SCHULTZ_STATE_VISIBLE);
    ASSERT_EQ(0u, schultz_node_get_state(tree, two) &
                      (uint32_t)SCHULTZ_STATE_VISIBLE);

    /* Choosing the one already chosen asks for no layout. */
    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_select(tree, node, 0u));
    ASSERT_EQ(0, ASKED(tree, node));

    FRESH(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_tab_view_select(tree, node, 1u));
    ASSERT_EQ(1, ASKED(tree, node));
    ASSERT(schultz_node_get_state(tree, two) & SCHULTZ_STATE_VISIBLE);

    schultz_tree_destroy(tree);
    PASS();
}

SUITE(setters)
{
    RUN_TEST(a_label_set_to_the_words_it_already_shows_does_nothing);
    RUN_TEST(a_line_set_to_where_it_already_is_does_nothing);
    RUN_TEST(a_polygon_set_to_the_points_it_already_has_does_nothing);
    RUN_TEST(a_button_bar_set_to_what_it_already_is_does_nothing);
    RUN_TEST(a_toolbar_told_the_same_thing_twice_does_nothing);
    RUN_TEST(a_text_field_told_the_same_thing_twice_does_nothing);
    RUN_TEST(a_scroll_view_told_the_same_thing_twice_does_nothing);
    RUN_TEST(a_busy_indicator_set_to_its_own_size_does_nothing);
    RUN_TEST(a_popup_told_the_same_thing_twice_does_nothing);
    RUN_TEST(a_date_picker_set_to_its_own_format_does_nothing);
    RUN_TEST(a_time_picker_told_the_same_thing_twice_does_nothing);
    RUN_TEST(a_tree_view_told_to_open_what_is_open_does_nothing);
    RUN_TEST(an_accordion_told_to_open_what_is_open_does_nothing);
    RUN_TEST(an_accordion_still_closes_the_others_when_nothing_moved);
    RUN_TEST(adding_a_tab_still_hides_the_pages_that_are_not_chosen);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(setters);
    GREATEST_MAIN_END();
}
