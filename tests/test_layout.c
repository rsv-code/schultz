/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_layout.c - the layout protocol and the built in panes.
 *
 * Two things get asserted throughout. First, the arithmetic: exact
 * rectangles, because a pane that is nearly right is wrong. Second, the
 * unbounded available size, which every pane must answer for. That case is
 * not exotic: a context menu sizes to its widest item with no cap, and a
 * scroll view offers its content unbounded height.
 */

#include "greatest.h"
#include "schultz_layout.h"
#include "schultz_node.h"

/* Builds a leaf with a fixed preferred size. */
static int32_t add_leaf(schultz_tree *tree, schultz_handle parent,
                        float w, float h, schultz_handle *out)
{
    int32_t result = schultz_node_create(tree, parent, out);

    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_node_set_pref_size(tree, *out, w, h);
}

static int32_t set_grow(schultz_tree *tree, schultz_handle node,
                        uint32_t grow)
{
    schultz_layout_params params;
    int32_t result = schultz_node_get_layout_params(tree, node, &params);

    if (result != SCHULTZ_OK) {
        return result;
    }
    params.grow = grow;
    return schultz_node_set_layout_params(tree, node, &params);
}

static int32_t set_align(schultz_tree *tree, schultz_handle node,
                         uint32_t align)
{
    schultz_layout_params params;
    int32_t result = schultz_node_get_layout_params(tree, node, &params);

    if (result != SCHULTZ_OK) {
        return result;
    }
    params.align = align;
    return schultz_node_set_layout_params(tree, node, &params);
}

static schultz_rect bounds_of(schultz_tree *tree, schultz_handle node)
{
    schultz_rect r = schultz_rect_make(-1, -1, -1, -1);
    schultz_node_get_bounds(tree, node, &r);
    return r;
}

/* ------------------------------------------------------------- protocol */

TEST unbounded_is_any_negative_available_size(void)
{
    ASSERT_EQ(1, schultz_layout_is_unbounded(-1.0f));
    ASSERT_EQ(1, schultz_layout_is_unbounded(-0.5f));
    ASSERT_EQ(0, schultz_layout_is_unbounded(0.0f));
    ASSERT_EQ(0, schultz_layout_is_unbounded(100.0f));
    PASS();
}

TEST a_leaf_measures_to_its_preferred_size(void)
{
    schultz_tree *tree;
    schultz_handle leaf;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, schultz_tree_root(tree), 80, 24,
                                   &leaf));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, leaf, 500, 500,
                                                 &size));
    ASSERT_EQ(80.0f, size.width);
    ASSERT_EQ(24.0f, size.height);

    /* And the same when nothing is available: a preferred size is a want. */
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, leaf, -1, -1, &size));
    ASSERT_EQ(80.0f, size.width);
    ASSERT_EQ(24.0f, size.height);

    schultz_tree_destroy(tree);
    PASS();
}

TEST size_hints_clamp_the_measured_size(void)
{
    schultz_tree *tree;
    schultz_handle leaf;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, schultz_tree_root(tree), 80, 24,
                                   &leaf));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_pref_size(tree, leaf, 80.0f,
                                                     24.0f));
    /* A minimum and a maximum are rules; a preference is not. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_min_size(tree, leaf, 100.0f,
                                                    SCHULTZ_SIZE_UNSET));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_max_size(tree, leaf,
                                                    SCHULTZ_SIZE_UNSET,
                                                    20.0f));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, leaf, 500, 500,
                                                 &size));
    ASSERT_EQ(100.0f, size.width);
    ASSERT_EQ(20.0f, size.height);

    schultz_tree_destroy(tree);
    PASS();
}

TEST arrange_sets_bounds_and_recurses(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_handle leaf;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_pane(tree, box,
                                                schultz_pane_vbox()));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 40, 10, &leaf));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, box,
                                        schultz_rect_make(5, 6, 200, 100)));

    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(5, 6, 200, 100),
                                     bounds_of(tree, box)));
    /* The child's rectangle is relative to the box, not the window. */
    ASSERT_EQ(0.0f, bounds_of(tree, leaf).x);
    ASSERT_EQ(0.0f, bounds_of(tree, leaf).y);

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------------------- VBox */

TEST vbox_stacks_children_with_gaps(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_handle a;
    schultz_handle b;
    schultz_handle c;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_vbox());
    schultz_node_set_spacing(tree, box, 10.0f, 5.0f);
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 20, &a));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 30, &b));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 40, &c));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, box,
                                        schultz_rect_make(0, 0, 200, 300)));

    /* padding 10, then 20, gap 5, then 30, gap 5, then 40. */
    ASSERT_EQ(10.0f, bounds_of(tree, a).y);
    ASSERT_EQ(35.0f, bounds_of(tree, b).y);
    ASSERT_EQ(70.0f, bounds_of(tree, c).y);
    ASSERT_EQ(20.0f, bounds_of(tree, a).height);
    ASSERT_EQ(40.0f, bounds_of(tree, c).height);

    schultz_tree_destroy(tree);
    PASS();
}

TEST vbox_measures_to_the_sum_plus_gaps_and_padding(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_handle a;
    schultz_handle b;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_vbox());
    schultz_node_set_spacing(tree, box, 10.0f, 6.0f);
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 20, &a));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 70, 30, &b));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, box, 500, 500,
                                                 &size));
    /* Height 20 + 6 + 30 + 2*10 padding. Width: widest child + padding. */
    ASSERT_EQ(76.0f, size.height);
    ASSERT_EQ(90.0f, size.width);

    schultz_tree_destroy(tree);
    PASS();
}

TEST vbox_shares_leftover_space_among_growers(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_handle fixed;
    schultz_handle grow_a;
    schultz_handle grow_b;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_vbox());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 20, &fixed));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 20, &grow_a));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 20, &grow_b));
    ASSERT_EQ(SCHULTZ_OK, set_grow(tree, grow_a, SCHULTZ_GROW_ALWAYS));
    ASSERT_EQ(SCHULTZ_OK, set_grow(tree, grow_b, SCHULTZ_GROW_ALWAYS));

    /* 200 tall, 60 used, 140 spare shared between the two growers. */
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, box,
                                        schultz_rect_make(0, 0, 100, 200)));

    ASSERT_EQ(20.0f, bounds_of(tree, fixed).height);
    ASSERT_EQ(90.0f, bounds_of(tree, grow_a).height);
    ASSERT_EQ(90.0f, bounds_of(tree, grow_b).height);
    ASSERT_EQ(20.0f, bounds_of(tree, grow_a).y);
    ASSERT_EQ(110.0f, bounds_of(tree, grow_b).y);

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * The three level model: leftover space goes only to the highest priority
 * present. A MAYBE child gets nothing while an ALWAYS child exists, and
 * everything when one does not.
 */
TEST grow_priority_is_winner_takes_all(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_handle maybe;
    schultz_handle always;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_vbox());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 20, &maybe));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 20, &always));
    ASSERT_EQ(SCHULTZ_OK, set_grow(tree, maybe, SCHULTZ_GROW_MAYBE));
    ASSERT_EQ(SCHULTZ_OK, set_grow(tree, always, SCHULTZ_GROW_ALWAYS));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, box,
                                        schultz_rect_make(0, 0, 100, 200)));
    ASSERT_EQ(20.0f, bounds_of(tree, maybe).height);
    ASSERT_EQ(180.0f, bounds_of(tree, always).height);

    /* Remove the ALWAYS child and the MAYBE child now takes the space. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_destroy(tree, always));
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, box,
                                        schultz_rect_make(0, 0, 100, 200)));
    ASSERT_EQ(200.0f, bounds_of(tree, maybe).height);

    schultz_tree_destroy(tree);
    PASS();
}

TEST vbox_skips_hidden_children(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_handle a;
    schultz_handle hidden;
    schultz_handle c;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_vbox());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 20, &a));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 20, &hidden));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 20, &c));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_state(tree, hidden,
                                                 SCHULTZ_STATE_ENABLED));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, box, 500, 500,
                                                 &size));
    ASSERT_EQ(40.0f, size.height); /* two children, not three */

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, box,
                                        schultz_rect_make(0, 0, 100, 200)));
    ASSERT_EQ(0.0f, bounds_of(tree, a).y);
    ASSERT_EQ(20.0f, bounds_of(tree, c).y); /* directly after a */

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_maximum_binds_where_a_child_is_placed(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_handle capped;
    schultz_handle pinned;
    schultz_handle loose;

    /*
     * Stretching fills the cross axis, which is the default and usually
     * right. A maximum has to survive it: one that a pane may quietly exceed
     * is not a maximum, and nothing reports the excess.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_vbox());

    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 400, 10, &capped));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_max_size(tree, capped, 200.0f,
                                                    SCHULTZ_SIZE_UNSET));

    /* The way to say "this wide, whatever the column thinks". */
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 40, 10, &pinned));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_min_size(tree, pinned, 150.0f,
                                                    SCHULTZ_SIZE_UNSET));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_max_size(tree, pinned, 150.0f,
                                                    SCHULTZ_SIZE_UNSET));

    /* And a child that said nothing still fills, as it always has. */
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 40, 10, &loose));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, box,
                                        schultz_rect_make(0, 0, 600, 200)));

    ASSERT_EQ(200.0f, bounds_of(tree, capped).width);
    ASSERT_EQ(150.0f, bounds_of(tree, pinned).width);
    ASSERT_EQ(600.0f, bounds_of(tree, loose).width);

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_capped_child_is_still_aligned_in_what_it_was_offered(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_handle middle;
    schultz_handle right;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_vbox());

    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 400, 10, &middle));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_max_size(tree, middle, 200.0f,
                                                    SCHULTZ_SIZE_UNSET));
    set_align(tree, middle, SCHULTZ_ALIGN_CENTER);

    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 400, 10, &right));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_max_size(tree, right, 200.0f,
                                                    SCHULTZ_SIZE_UNSET));
    set_align(tree, right, SCHULTZ_ALIGN_END);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, box,
                                        schultz_rect_make(0, 0, 600, 200)));

    /* Cut to its maximum, then placed in the six hundred it was offered. */
    ASSERT_EQ(200.0f, bounds_of(tree, middle).width);
    ASSERT_EQ(200.0f, bounds_of(tree, middle).x);
    ASSERT_EQ(200.0f, bounds_of(tree, right).width);
    ASSERT_EQ(400.0f, bounds_of(tree, right).x);

    schultz_tree_destroy(tree);
    PASS();
}

TEST vbox_cross_axis_alignment(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_handle stretch;
    schultz_handle start;
    schultz_handle center;
    schultz_handle end;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_vbox());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 40, 10, &stretch));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 40, 10, &start));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 40, 10, &center));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 40, 10, &end));
    set_align(tree, stretch, SCHULTZ_ALIGN_STRETCH);
    set_align(tree, start, SCHULTZ_ALIGN_START);
    set_align(tree, center, SCHULTZ_ALIGN_CENTER);
    set_align(tree, end, SCHULTZ_ALIGN_END);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, box,
                                        schultz_rect_make(0, 0, 100, 200)));

    ASSERT_EQ(100.0f, bounds_of(tree, stretch).width); /* fills */
    ASSERT_EQ(0.0f, bounds_of(tree, start).x);
    ASSERT_EQ(40.0f, bounds_of(tree, start).width);
    ASSERT_EQ(30.0f, bounds_of(tree, center).x);       /* (100-40)/2 */
    ASSERT_EQ(60.0f, bounds_of(tree, end).x);          /* 100-40 */

    schultz_tree_destroy(tree);
    PASS();
}

/* VBox's answer for unbounded height: report the full natural stack. */
TEST vbox_with_unbounded_height_reports_its_natural_height(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_handle a;
    schultz_handle b;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_vbox());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 100, &a));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 50, 100, &b));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, box, 200, -1.0f,
                                                 &size));
    ASSERT_EQ(200.0f, size.height);
    ASSERT_EQ(50.0f, size.width);

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------------------- HBox */

TEST hbox_is_vbox_with_the_axes_swapped(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_handle a;
    schultz_handle b;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_hbox());
    schultz_node_set_spacing(tree, box, 0.0f, 8.0f);
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 30, 20, &a));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 40, 25, &b));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, box, 500, 500,
                                                 &size));
    ASSERT_EQ(78.0f, size.width);  /* 30 + 8 + 40 */
    ASSERT_EQ(25.0f, size.height); /* the taller child */

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, box,
                                        schultz_rect_make(0, 0, 300, 50)));
    ASSERT_EQ(0.0f, bounds_of(tree, a).x);
    ASSERT_EQ(38.0f, bounds_of(tree, b).x);
    ASSERT_EQ(50.0f, bounds_of(tree, a).height); /* stretched */

    schultz_tree_destroy(tree);
    PASS();
}

TEST hbox_with_unbounded_width_sums_its_children(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_handle a;
    schultz_handle b;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_hbox());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 120, 20, &a));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 80, 20, &b));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, box, -1.0f, 100,
                                                 &size));
    ASSERT_EQ(200.0f, size.width);

    schultz_tree_destroy(tree);
    PASS();
}

/* ----------------------------------------------------------- StackPane */

TEST stack_gives_every_child_the_same_rect(void)
{
    schultz_tree *tree;
    schultz_handle stack;
    schultz_handle back;
    schultz_handle front;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &stack));
    schultz_node_set_pane(tree, stack, schultz_pane_stack());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, stack, 50, 50, &back));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, stack, 20, 20, &front));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, stack,
                                        schultz_rect_make(0, 0, 200, 100)));
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(0, 0, 200, 100),
                                     bounds_of(tree, back)));
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(0, 0, 200, 100),
                                     bounds_of(tree, front)));

    schultz_tree_destroy(tree);
    PASS();
}

TEST stack_measures_to_its_largest_child(void)
{
    schultz_tree *tree;
    schultz_handle stack;
    schultz_handle wide;
    schultz_handle tall;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &stack));
    schultz_node_set_pane(tree, stack, schultz_pane_stack());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, stack, 200, 10, &wide));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, stack, 10, 300, &tall));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, stack, -1, -1, &size));
    ASSERT_EQ(200.0f, size.width);
    ASSERT_EQ(300.0f, size.height);

    schultz_tree_destroy(tree);
    PASS();
}

TEST stack_centres_a_child_when_asked(void)
{
    schultz_tree *tree;
    schultz_handle stack;
    schultz_handle badge;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &stack));
    schultz_node_set_pane(tree, stack, schultz_pane_stack());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, stack, 40, 20, &badge));
    set_align(tree, badge, SCHULTZ_ALIGN_CENTER);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, stack,
                                        schultz_rect_make(0, 0, 200, 100)));
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(80, 40, 40, 20),
                                     bounds_of(tree, badge)));

    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------------------- absolute Pane */

TEST absolute_places_children_where_told(void)
{
    schultz_tree *tree;
    schultz_handle pane;
    schultz_handle child;
    schultz_layout_params params;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &pane));
    schultz_node_set_pane(tree, pane, schultz_pane_absolute());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, pane, 30, 40, &child));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_layout_params(tree, child,
                                                         &params));
    params.x = 55.0f;
    params.y = 66.0f;
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_layout_params(tree, child,
                                                         &params));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, pane,
                                        schultz_rect_make(0, 0, 500, 500)));
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(55, 66, 30, 40),
                                     bounds_of(tree, child)));

    schultz_tree_destroy(tree);
    PASS();
}

TEST absolute_measures_to_the_union_of_its_children(void)
{
    schultz_tree *tree;
    schultz_handle pane;
    schultz_handle a;
    schultz_handle b;
    schultz_layout_params params;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &pane));
    schultz_node_set_pane(tree, pane, schultz_pane_absolute());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, pane, 30, 40, &a));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, pane, 20, 10, &b));

    schultz_node_get_layout_params(tree, a, &params);
    params.x = 10.0f; params.y = 10.0f;
    schultz_node_set_layout_params(tree, a, &params);
    schultz_node_get_layout_params(tree, b, &params);
    params.x = 100.0f; params.y = 5.0f;
    schultz_node_set_layout_params(tree, b, &params);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, pane, -1, -1, &size));
    ASSERT_EQ(120.0f, size.width);  /* 100 + 20 */
    ASSERT_EQ(50.0f, size.height);  /* 10 + 40 */

    schultz_tree_destroy(tree);
    PASS();
}

/* --------------------------------------------------------- BorderPane */

TEST border_gives_edges_their_size_and_the_centre_the_rest(void)
{
    schultz_tree *tree;
    schultz_handle border;
    schultz_handle top;
    schultz_handle bottom;
    schultz_handle left;
    schultz_handle center;
    schultz_layout_params params;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &border));
    schultz_node_set_pane(tree, border, schultz_pane_border());

    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, border, 0, 30, &top));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, border, 0, 20, &bottom));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, border, 60, 0, &left));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, border, 0, 0, &center));

    schultz_node_get_layout_params(tree, top, &params);
    params.slot = SCHULTZ_SLOT_TOP;
    schultz_node_set_layout_params(tree, top, &params);
    params.slot = SCHULTZ_SLOT_BOTTOM;
    schultz_node_set_layout_params(tree, bottom, &params);
    params.slot = SCHULTZ_SLOT_LEFT;
    schultz_node_set_layout_params(tree, left, &params);
    params.slot = SCHULTZ_SLOT_CENTER;
    schultz_node_set_layout_params(tree, center, &params);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, border,
                                        schultz_rect_make(0, 0, 400, 300)));

    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(0, 0, 400, 30),
                                     bounds_of(tree, top)));
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(0, 280, 400, 20),
                                     bounds_of(tree, bottom)));
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(0, 30, 60, 250),
                                     bounds_of(tree, left)));
    /* Whatever is left: x 60, y 30, width 340, height 250. */
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(60, 30, 340, 250),
                                     bounds_of(tree, center)));

    schultz_tree_destroy(tree);
    PASS();
}

/* ----------------------------------------------------------- nesting */

TEST panes_nest_without_special_cases(void)
{
    schultz_tree *tree;
    schultz_handle column;
    schultz_handle row;
    schultz_handle a;
    schultz_handle b;
    schultz_handle footer;
    schultz_rect r;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &column));
    schultz_node_set_pane(tree, column, schultz_pane_vbox());

    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, column, &row));
    schultz_node_set_pane(tree, row, schultz_pane_hbox());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, row, 50, 20, &a));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, row, 50, 20, &b));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, column, 0, 30, &footer));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, column,
                                        schultz_rect_make(0, 0, 300, 200)));

    /* The row sits at the top at its natural height. */
    r = bounds_of(tree, row);
    ASSERT_EQ(0.0f, r.y);
    ASSERT_EQ(20.0f, r.height);
    /* Its children are relative to it, side by side. */
    ASSERT_EQ(0.0f, bounds_of(tree, a).x);
    ASSERT_EQ(50.0f, bounds_of(tree, b).x);
    /* The footer follows the row. */
    ASSERT_EQ(20.0f, bounds_of(tree, footer).y);

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * Every pane must answer the unbounded case rather than producing a garbage
 * size, because a scroll view or a popup will ask.
 */
TEST every_pane_answers_unbounded(void)
{
    const schultz_pane_vtable *panes[] = {
        NULL, NULL, NULL, NULL, NULL, NULL
    };
    schultz_tree *tree;
    schultz_handle node;
    schultz_handle child;
    schultz_size size;
    size_t i;

    panes[0] = schultz_pane_vbox();
    panes[1] = schultz_pane_hbox();
    panes[2] = schultz_pane_stack();
    panes[3] = schultz_pane_absolute();
    panes[4] = schultz_pane_border();
    panes[5] = schultz_pane_grid();

    for (i = 0; i < sizeof(panes) / sizeof(panes[0]); i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_node_create(tree, schultz_tree_root(tree), &node));
        schultz_node_set_pane(tree, node, panes[i]);
        ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, node, 40, 30, &child));

        /* Unbounded on both axes must still produce a finite, sane size. */
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_layout_measure(tree, node, -1.0f, -1.0f, &size));
        ASSERT(size.width >= 0.0f);
        ASSERT(size.height >= 0.0f);
        ASSERT(size.width < 1.0e6f);
        ASSERT(size.height < 1.0e6f);

        schultz_tree_destroy(tree);
    }
    PASS();
}

TEST an_empty_pane_measures_to_its_padding(void)
{
    schultz_tree *tree;
    schultz_handle box;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_vbox());
    schultz_node_set_spacing(tree, box, 7.0f, 5.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, box, 100, 100, &size));
    ASSERT_EQ(14.0f, size.width);
    ASSERT_EQ(14.0f, size.height);

    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------------------------ GridPane */

/* Places a child in a grid cell. */
static int32_t set_cell(schultz_tree *tree, schultz_handle node,
                        uint32_t column, uint32_t row,
                        uint32_t column_span, uint32_t row_span)
{
    schultz_layout_params params;
    int32_t result = schultz_node_get_layout_params(tree, node, &params);

    if (result != SCHULTZ_OK) {
        return result;
    }
    params.column      = column;
    params.row         = row;
    params.column_span = column_span;
    params.row_span    = row_span;
    return schultz_node_set_layout_params(tree, node, &params);
}

TEST grid_content_tracks_size_to_their_largest_cell(void)
{
    schultz_tree *tree;
    schultz_handle grid;
    schultz_handle a;
    schultz_handle b;
    schultz_handle c;
    schultz_grid_track cols[2];
    schultz_grid_track rows[2];
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &grid));
    schultz_node_set_pane(tree, grid, schultz_pane_grid());
    cols[0].kind = SCHULTZ_TRACK_CONTENT; cols[0].value = 0.0f;
    cols[1].kind = SCHULTZ_TRACK_CONTENT; cols[1].value = 0.0f;
    rows[0].kind = SCHULTZ_TRACK_CONTENT; rows[0].value = 0.0f;
    rows[1].kind = SCHULTZ_TRACK_CONTENT; rows[1].value = 0.0f;
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_grid_tracks(tree, grid, cols, 2,
                                                       rows, 2));

    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 40, 10, &a));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 70, 10, &b));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 20, 25, &c));
    ASSERT_EQ(SCHULTZ_OK, set_cell(tree, a, 0, 0, 1, 1));
    ASSERT_EQ(SCHULTZ_OK, set_cell(tree, b, 1, 0, 1, 1));
    ASSERT_EQ(SCHULTZ_OK, set_cell(tree, c, 0, 1, 1, 1));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, grid, -1, -1, &size));
    /* Columns 40 and 70; rows 10 and 25. */
    ASSERT_EQ(110.0f, size.width);
    ASSERT_EQ(35.0f, size.height);

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * The reason GridPane exists: cells line up across rows. Two nested HBoxes
 * could not guarantee this.
 */
TEST grid_columns_align_across_rows(void)
{
    schultz_tree *tree;
    schultz_handle grid;
    schultz_handle narrow;
    schultz_handle wide;
    schultz_handle below;
    schultz_grid_track cols[2];
    schultz_grid_track rows[2];

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &grid));
    schultz_node_set_pane(tree, grid, schultz_pane_grid());
    cols[0].kind = SCHULTZ_TRACK_CONTENT; cols[0].value = 0.0f;
    cols[1].kind = SCHULTZ_TRACK_CONTENT; cols[1].value = 0.0f;
    rows[0].kind = SCHULTZ_TRACK_CONTENT; rows[0].value = 0.0f;
    rows[1].kind = SCHULTZ_TRACK_CONTENT; rows[1].value = 0.0f;
    schultz_node_set_grid_tracks(tree, grid, cols, 2, rows, 2);

    /* A narrow cell in row 0 and a wide one in row 1, same column. */
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 20, 10, &narrow));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 90, 10, &below));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 30, 10, &wide));
    set_cell(tree, narrow, 0, 0, 1, 1);
    set_cell(tree, below, 0, 1, 1, 1);
    set_cell(tree, wide, 1, 0, 1, 1);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, grid,
                                        schultz_rect_make(0, 0, 300, 200)));

    /* Column 0 is 90 wide because of the widest cell, so column 1 starts
     * at 90 in both rows. */
    ASSERT_EQ(90.0f, bounds_of(tree, wide).x);
    ASSERT_EQ(0.0f, bounds_of(tree, narrow).x);
    ASSERT_EQ(0.0f, bounds_of(tree, below).x);
    ASSERT_EQ(90.0f, bounds_of(tree, narrow).width);

    schultz_tree_destroy(tree);
    PASS();
}

TEST grid_fixed_tracks_take_their_value(void)
{
    schultz_tree *tree;
    schultz_handle grid;
    schultz_handle cell;
    schultz_grid_track cols[2];
    schultz_grid_track rows[1];

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &grid));
    schultz_node_set_pane(tree, grid, schultz_pane_grid());
    cols[0].kind = SCHULTZ_TRACK_FIXED;   cols[0].value = 120.0f;
    cols[1].kind = SCHULTZ_TRACK_CONTENT; cols[1].value = 0.0f;
    rows[0].kind = SCHULTZ_TRACK_FIXED;   rows[0].value = 44.0f;
    schultz_node_set_grid_tracks(tree, grid, cols, 2, rows, 1);

    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 10, 10, &cell));
    set_cell(tree, cell, 1, 0, 1, 1);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, grid,
                                        schultz_rect_make(0, 0, 400, 200)));
    /* Column 0 is fixed at 120 even though nothing is in it. */
    ASSERT_EQ(120.0f, bounds_of(tree, cell).x);
    ASSERT_EQ(44.0f, bounds_of(tree, cell).height);

    schultz_tree_destroy(tree);
    PASS();
}

TEST grid_weighted_tracks_share_the_leftover(void)
{
    schultz_tree *tree;
    schultz_handle grid;
    schultz_handle a;
    schultz_handle b;
    schultz_grid_track cols[2];
    schultz_grid_track rows[1];

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &grid));
    schultz_node_set_pane(tree, grid, schultz_pane_grid());
    /* One part to two parts of the 300 available. */
    cols[0].kind = SCHULTZ_TRACK_WEIGHTED; cols[0].value = 1.0f;
    cols[1].kind = SCHULTZ_TRACK_WEIGHTED; cols[1].value = 2.0f;
    rows[0].kind = SCHULTZ_TRACK_CONTENT;  rows[0].value = 0.0f;
    schultz_node_set_grid_tracks(tree, grid, cols, 2, rows, 1);

    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 10, 10, &a));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 10, 10, &b));
    set_cell(tree, a, 0, 0, 1, 1);
    set_cell(tree, b, 1, 0, 1, 1);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, grid,
                                        schultz_rect_make(0, 0, 300, 100)));
    ASSERT_EQ(100.0f, bounds_of(tree, a).width);
    ASSERT_EQ(200.0f, bounds_of(tree, b).width);
    ASSERT_EQ(100.0f, bounds_of(tree, b).x);

    schultz_tree_destroy(tree);
    PASS();
}

TEST grid_spanning_cell_covers_its_tracks_and_the_gap(void)
{
    schultz_tree *tree;
    schultz_handle grid;
    schultz_handle span;
    schultz_handle single;
    schultz_grid_track cols[2];
    schultz_grid_track rows[2];

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &grid));
    schultz_node_set_pane(tree, grid, schultz_pane_grid());
    schultz_node_set_spacing(tree, grid, 0.0f, 10.0f);
    cols[0].kind = SCHULTZ_TRACK_FIXED; cols[0].value = 50.0f;
    cols[1].kind = SCHULTZ_TRACK_FIXED; cols[1].value = 70.0f;
    rows[0].kind = SCHULTZ_TRACK_CONTENT; rows[0].value = 0.0f;
    rows[1].kind = SCHULTZ_TRACK_CONTENT; rows[1].value = 0.0f;
    schultz_node_set_grid_tracks(tree, grid, cols, 2, rows, 2);

    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 10, 10, &span));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 10, 10, &single));
    set_cell(tree, span, 0, 0, 2, 1);   /* spans both columns */
    set_cell(tree, single, 0, 1, 1, 1);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, grid,
                                        schultz_rect_make(0, 0, 400, 200)));
    /* 50 + gap 10 + 70 = 130. */
    ASSERT_EQ(130.0f, bounds_of(tree, span).width);
    ASSERT_EQ(50.0f, bounds_of(tree, single).width);

    schultz_tree_destroy(tree);
    PASS();
}

TEST grid_grows_tracks_for_an_oversized_spanning_cell(void)
{
    schultz_tree *tree;
    schultz_handle grid;
    schultz_handle span;
    schultz_grid_track cols[2];
    schultz_grid_track rows[1];
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &grid));
    schultz_node_set_pane(tree, grid, schultz_pane_grid());
    cols[0].kind = SCHULTZ_TRACK_CONTENT; cols[0].value = 0.0f;
    cols[1].kind = SCHULTZ_TRACK_CONTENT; cols[1].value = 0.0f;
    rows[0].kind = SCHULTZ_TRACK_CONTENT; rows[0].value = 0.0f;
    schultz_node_set_grid_tracks(tree, grid, cols, 2, rows, 1);

    /* Nothing else sizes the columns, so this 200 wide cell must. */
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 200, 10, &span));
    set_cell(tree, span, 0, 0, 2, 1);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, grid, -1, -1, &size));
    ASSERT_EQ(200.0f, size.width); /* 100 into each column */

    schultz_tree_destroy(tree);
    PASS();
}

TEST grid_ignores_a_cell_outside_its_tracks(void)
{
    schultz_tree *tree;
    schultz_handle grid;
    schultz_handle stray;
    schultz_grid_track cols[1];
    schultz_grid_track rows[1];

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &grid));
    schultz_node_set_pane(tree, grid, schultz_pane_grid());
    cols[0].kind = SCHULTZ_TRACK_CONTENT; cols[0].value = 0.0f;
    rows[0].kind = SCHULTZ_TRACK_CONTENT; rows[0].value = 0.0f;
    schultz_node_set_grid_tracks(tree, grid, cols, 1, rows, 1);

    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 10, 10, &stray));
    set_cell(tree, stray, 5, 5, 1, 1);

    /* Must not crash or read past the track arrays. */
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, grid,
                                        schultz_rect_make(0, 0, 100, 100)));

    schultz_tree_destroy(tree);
    PASS();
}

TEST grid_track_count_is_clamped_to_the_maximum(void)
{
    schultz_tree *tree;
    schultz_handle grid;
    schultz_grid_track many[SCHULTZ_GRID_MAX_TRACKS + 8];
    const schultz_grid_track *stored;
    uint32_t count = 0;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &grid));
    schultz_node_set_pane(tree, grid, schultz_pane_grid());

    for (i = 0; i < SCHULTZ_GRID_MAX_TRACKS + 8u; i++) {
        many[i].kind = SCHULTZ_TRACK_CONTENT;
        many[i].value = 0.0f;
    }
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_grid_tracks(tree, grid, many,
                                           SCHULTZ_GRID_MAX_TRACKS + 8u,
                                           NULL, 0));

    stored = schultz_node_get_grid_tracks(tree, grid, 0, &count);
    ASSERT(stored != NULL);
    ASSERT_EQ((uint32_t)SCHULTZ_GRID_MAX_TRACKS, count);

    schultz_tree_destroy(tree);
    PASS();
}

/* Unbounded space means weighted tracks have nothing to share. */
TEST grid_weighted_tracks_collapse_when_unbounded(void)
{
    schultz_tree *tree;
    schultz_handle grid;
    schultz_handle cell;
    schultz_grid_track cols[2];
    schultz_grid_track rows[1];
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &grid));
    schultz_node_set_pane(tree, grid, schultz_pane_grid());
    cols[0].kind = SCHULTZ_TRACK_CONTENT;  cols[0].value = 0.0f;
    cols[1].kind = SCHULTZ_TRACK_WEIGHTED; cols[1].value = 1.0f;
    rows[0].kind = SCHULTZ_TRACK_CONTENT;  rows[0].value = 0.0f;
    schultz_node_set_grid_tracks(tree, grid, cols, 2, rows, 1);

    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, grid, 60, 10, &cell));
    set_cell(tree, cell, 0, 0, 1, 1);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, grid, -1, -1, &size));
    /* Only the content column contributes. */
    ASSERT_EQ(60.0f, size.width);

    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------------------------- FlowPane */

/* A pane of same sized children, so wrapping is easy to reason about. */
static int32_t flow_of(schultz_tree *tree, uint32_t count, float w, float h,
                       schultz_handle *out_pane)
{
    uint32_t i;
    int32_t result = schultz_node_create(tree, schultz_tree_root(tree),
                                         out_pane);

    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_set_pane(tree, *out_pane, schultz_pane_flow());
    for (i = 0; i < count; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;

        result = add_leaf(tree, *out_pane, w, h, &child);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    return SCHULTZ_OK;
}

TEST a_flow_pane_starts_a_new_row_when_one_is_full(void)
{
    schultz_tree *tree;
    schultz_handle pane;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    /* Six children of 40, so three fit across 130 with a gap of 5. */
    ASSERT_EQ(SCHULTZ_OK, flow_of(tree, 6u, 40.0f, 20.0f, &pane));
    schultz_node_set_spacing(tree, pane, 0.0f, 5.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, pane, 130.0f, -1.0f,
                                                 &size));
    /* Three across: 40 + 5 + 40 + 5 + 40. Two rows: 20 + 5 + 20. */
    ASSERT_EQ(130.0f, size.width);
    ASSERT_EQ(45.0f, size.height);

    /*
     * Narrower means more rows and a taller pane, which is the whole point.
     * Two across now: 40 + 5 + 40. Three rows: 20 + 5 + 20 + 5 + 20.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, pane, 85.0f, -1.0f,
                                                 &size));
    ASSERT_EQ(85.0f, size.width);
    ASSERT_EQ(70.0f, size.height);

    schultz_tree_destroy(tree);
    PASS();
}

/* Nothing has to wrap when there is no limit, so nothing does. */
TEST an_unbounded_flow_pane_puts_everything_on_one_row(void)
{
    schultz_tree *tree;
    schultz_handle pane;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, flow_of(tree, 4u, 30.0f, 10.0f, &pane));
    schultz_node_set_spacing(tree, pane, 0.0f, 5.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, pane, -1.0f, -1.0f,
                                                 &size));
    ASSERT_EQ(4.0f * 30.0f + 3.0f * 5.0f, size.width);
    ASSERT_EQ(10.0f, size.height);

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_flow_pane_places_its_children_row_by_row(void)
{
    schultz_tree *tree;
    schultz_handle pane;
    schultz_handle child = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, flow_of(tree, 4u, 40.0f, 20.0f, &pane));
    schultz_node_set_spacing(tree, pane, 10.0f, 5.0f);

    /* Padding of ten, so 100 of usable width: two across. */
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, pane,
                    schultz_rect_make(0, 0, 120.0f, 200.0f)));

    schultz_node_child_at(tree, pane, 0, &child);
    schultz_node_get_bounds(tree, child, &bounds);
    ASSERT_EQ(10.0f, bounds.x);
    ASSERT_EQ(10.0f, bounds.y);

    schultz_node_child_at(tree, pane, 1, &child);
    schultz_node_get_bounds(tree, child, &bounds);
    ASSERT_EQ(10.0f + 40.0f + 5.0f, bounds.x);
    ASSERT_EQ(10.0f, bounds.y);

    /* The third wrapped, so it is back at the left and a row down. */
    schultz_node_child_at(tree, pane, 2, &child);
    schultz_node_get_bounds(tree, child, &bounds);
    ASSERT_EQ(10.0f, bounds.x);
    ASSERT_EQ(10.0f + 20.0f + 5.0f, bounds.y);

    schultz_tree_destroy(tree);
    PASS();
}

/* Mixed heights line up within their row rather than every child stretching. */
TEST a_flow_row_aligns_children_within_its_height(void)
{
    schultz_tree *tree;
    schultz_handle pane = SCHULTZ_HANDLE_NONE;
    schultz_handle tall = SCHULTZ_HANDLE_NONE;
    schultz_handle shortish = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &pane));
    schultz_node_set_pane(tree, pane, schultz_pane_flow());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, pane, 20.0f, 40.0f, &tall));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, pane, 20.0f, 10.0f, &shortish));
    ASSERT_EQ(SCHULTZ_OK, set_align(tree, shortish, SCHULTZ_ALIGN_CENTER));

    schultz_layout_arrange(tree, pane, schultz_rect_make(0, 0, 200.0f,
                                                         100.0f));

    /* The row is as tall as the tallest child. */
    schultz_node_get_bounds(tree, tall, &bounds);
    ASSERT_EQ(0.0f, bounds.y);
    ASSERT_EQ(40.0f, bounds.height);

    /* And the short one sits in the middle of it, at its own height. */
    schultz_node_get_bounds(tree, shortish, &bounds);
    ASSERT_EQ(10.0f, bounds.height);
    ASSERT_EQ(15.0f, bounds.y);

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * A child wider than the pane goes on a row of its own and overflows, in the
 * same way as a word too long for a line: there is nowhere narrower for it.
 */
TEST a_flow_pane_gives_an_oversized_child_its_own_row(void)
{
    schultz_tree *tree;
    schultz_handle pane = SCHULTZ_HANDLE_NONE;
    schultz_handle small = SCHULTZ_HANDLE_NONE;
    schultz_handle huge = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &pane));
    schultz_node_set_pane(tree, pane, schultz_pane_flow());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, pane, 20.0f, 10.0f, &small));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, pane, 300.0f, 10.0f, &huge));

    schultz_layout_arrange(tree, pane, schultz_rect_make(0, 0, 100.0f,
                                                         100.0f));

    schultz_node_get_bounds(tree, small, &bounds);
    ASSERT_EQ(0.0f, bounds.y);
    schultz_node_get_bounds(tree, huge, &bounds);
    ASSERT_EQ(0.0f, bounds.x);
    ASSERT_EQ(10.0f, bounds.y);
    ASSERT_EQ(300.0f, bounds.width);

    schultz_tree_destroy(tree);
    PASS();
}

TEST an_empty_flow_pane_measures_its_padding(void)
{
    schultz_tree *tree;
    schultz_handle pane = SCHULTZ_HANDLE_NONE;
    schultz_handle hidden = SCHULTZ_HANDLE_NONE;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &pane));
    schultz_node_set_pane(tree, pane, schultz_pane_flow());
    schultz_node_set_spacing(tree, pane, 6.0f, 4.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, pane, 100.0f, -1.0f,
                                                 &size));
    ASSERT_EQ(12.0f, size.width);
    ASSERT_EQ(12.0f, size.height);

    /* A hidden child takes no room and starts no row. */
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, pane, 30.0f, 30.0f, &hidden));
    schultz_node_set_state(tree, hidden, 0u);
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, pane, 100.0f, -1.0f,
                                                 &size));
    ASSERT_EQ(12.0f, size.height);

    /* And arranging one with nothing in it is not an error. */
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(tree, pane,
                    schultz_rect_make(0, 0, 100.0f, 100.0f)));

    schultz_tree_destroy(tree);
    PASS();
}

/* --------------------------------------------------------- the pass */

/*
 * The pass a host runs once a frame. It finds what needs arranging and
 * arranges it, rather than the host walking the tree to work that out.
 */
TEST the_layout_pass_arranges_only_what_changed(void)
{
    schultz_tree *tree;
    schultz_handle box = SCHULTZ_HANDLE_NONE;
    schultz_handle a = SCHULTZ_HANDLE_NONE;
    schultz_handle b = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &box));
    schultz_node_set_pane(tree, box, schultz_pane_vbox());
    schultz_node_set_bounds(tree, box, schultz_rect_make(0, 0, 100, 100));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 40, 20, &a));
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 40, 30, &b));

    /* Something changed, so one node is arranged: the pane holding them. */
    ASSERT_EQ(1u, schultz_layout_run(tree));
    schultz_node_get_bounds(tree, b, &bounds);
    ASSERT_EQ(20.0f, bounds.y);

    /* Nothing has changed since, so the whole pass is a flag test. */
    ASSERT_EQ(0u, schultz_layout_run(tree));

    /* A size hint deep inside marks it again, and the pane runs again. */
    schultz_node_set_pref_size(tree, a, SCHULTZ_SIZE_UNSET, 50.0f);
    ASSERT_EQ(1u, schultz_layout_run(tree));
    schultz_node_get_bounds(tree, b, &bounds);
    ASSERT_EQ(50.0f, bounds.y);

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * A node with no pane places nothing, so the pass carries on past it. That is
 * what makes a page of absolutely positioned children work: it has no pane,
 * and the panes inside it are still found.
 */
TEST the_layout_pass_looks_past_a_node_with_no_pane(void)
{
    schultz_tree *tree;
    schultz_handle page = SCHULTZ_HANDLE_NONE;
    schultz_handle box = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &page));
    schultz_node_set_bounds(tree, page, schultz_rect_make(0, 0, 200, 200));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, page, &box));
    schultz_node_set_pane(tree, box, schultz_pane_vbox());
    schultz_node_set_bounds(tree, box, schultz_rect_make(10, 20, 100, 100));
    schultz_node_set_spacing(tree, box, 5.0f, 0.0f);
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, box, 40, 20, &child));

    ASSERT_EQ(1u, schultz_layout_run(tree));

    /* The column ran, and the page it sits on was left where it was. */
    schultz_node_get_bounds(tree, child, &bounds);
    ASSERT_EQ(5.0f, bounds.x);
    schultz_node_get_bounds(tree, page, &bounds);
    ASSERT_EQ(0.0f, bounds.x);
    ASSERT_EQ(200.0f, bounds.width);

    schultz_tree_destroy(tree);
    PASS();
}

/* Arranging a node lays out what is under it, so the walk stops there. */
TEST the_layout_pass_stops_at_the_node_it_arranges(void)
{
    schultz_tree *tree;
    schultz_handle outer = SCHULTZ_HANDLE_NONE;
    schultz_handle inner = SCHULTZ_HANDLE_NONE;
    schultz_handle leaf = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &outer));
    schultz_node_set_pane(tree, outer, schultz_pane_vbox());
    schultz_node_set_bounds(tree, outer, schultz_rect_make(0, 0, 100, 100));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, outer, &inner));
    schultz_node_set_pane(tree, inner, schultz_pane_hbox());
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, inner, 30, 20, &leaf));

    /* Both are marked, but arranging the outer one covers the inner one. */
    ASSERT_EQ(1u, schultz_layout_run(tree));
    ASSERT_EQ(0u, schultz_layout_run(tree));

    ASSERT_EQ(0u, schultz_layout_run(NULL));

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * Arranging a pane runs every pane beneath it, but only through nodes that
 * have one. A node with no pane, such as a tab page, is placed and left
 * alone, so the walk has to carry on through it to reach what is inside.
 */
TEST the_layout_pass_reaches_a_pane_behind_a_plain_node(void)
{
    schultz_tree *tree;
    schultz_handle outer = SCHULTZ_HANDLE_NONE;
    schultz_handle plain = SCHULTZ_HANDLE_NONE;
    schultz_handle inner = SCHULTZ_HANDLE_NONE;
    schultz_handle leaf = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &outer));
    schultz_node_set_pane(tree, outer, schultz_pane_vbox());
    schultz_node_set_bounds(tree, outer, schultz_rect_make(0, 0, 100, 100));

    /* No pane, so its own children are nobody else's business. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, outer, &plain));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, plain, &inner));
    schultz_node_set_pane(tree, inner, schultz_pane_vbox());
    schultz_node_set_bounds(tree, inner, schultz_rect_make(0, 0, 60, 60));
    schultz_node_set_spacing(tree, inner, 4.0f, 0.0f);
    ASSERT_EQ(SCHULTZ_OK, add_leaf(tree, inner, 20, 10, &leaf));

    /* The outer column and the buried one: two panes, one pass. */
    ASSERT_EQ(2u, schultz_layout_run(tree));
    schultz_node_get_bounds(tree, leaf, &bounds);
    ASSERT_EQ(4.0f, bounds.x);
    ASSERT_EQ(4.0f, bounds.y);

    ASSERT_EQ(0u, schultz_layout_run(tree));

    schultz_tree_destroy(tree);
    PASS();
}


TEST a_decided_width_is_the_width_a_node_is_measured_against(void)
{
    schultz_tree *tree = NULL;
    schultz_handle pane = SCHULTZ_HANDLE_NONE;
    schultz_size wide;
    schultz_size narrow;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));

    /* Six boxes of 50 in a flow pane: one row at 400 wide, three at 100. */
    ASSERT_EQ(SCHULTZ_OK, flow_of(tree, 6u, 50.0f, 20.0f, &pane));
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, pane, 400.0f, -1.0f,
                                                 &wide));
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, pane, 100.0f, -1.0f,
                                                 &narrow));
    ASSERT(narrow.height > wide.height);

    /*
     * With the width already decided, the offered width is beside the point:
     * the node will be laid out at its preferred width, so that is the width
     * its height has to be worked out from. Measuring against the offered
     * width instead reports one row and then draws three, and whatever was
     * placed below is overlapped.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_pref_size(tree, pane, 100.0f,
                                                     SCHULTZ_SIZE_UNSET));

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(tree, pane, 400.0f, -1.0f,
                                                 &wide));
    ASSERT_EQ(100.0f, wide.width);
    ASSERT_EQ(narrow.height, wide.height);

    schultz_tree_destroy(tree);
    PASS();
}

SUITE(layout)
{
    RUN_TEST(the_layout_pass_arranges_only_what_changed);
    RUN_TEST(the_layout_pass_looks_past_a_node_with_no_pane);
    RUN_TEST(the_layout_pass_stops_at_the_node_it_arranges);
    RUN_TEST(the_layout_pass_reaches_a_pane_behind_a_plain_node);
    RUN_TEST(a_flow_pane_starts_a_new_row_when_one_is_full);
    RUN_TEST(an_unbounded_flow_pane_puts_everything_on_one_row);
    RUN_TEST(a_flow_pane_places_its_children_row_by_row);
    RUN_TEST(a_flow_row_aligns_children_within_its_height);
    RUN_TEST(a_flow_pane_gives_an_oversized_child_its_own_row);
    RUN_TEST(an_empty_flow_pane_measures_its_padding);
    RUN_TEST(unbounded_is_any_negative_available_size);
    RUN_TEST(a_leaf_measures_to_its_preferred_size);
    RUN_TEST(size_hints_clamp_the_measured_size);
    RUN_TEST(arrange_sets_bounds_and_recurses);
    RUN_TEST(vbox_stacks_children_with_gaps);
    RUN_TEST(vbox_measures_to_the_sum_plus_gaps_and_padding);
    RUN_TEST(vbox_shares_leftover_space_among_growers);
    RUN_TEST(grow_priority_is_winner_takes_all);
    RUN_TEST(vbox_skips_hidden_children);
    RUN_TEST(a_maximum_binds_where_a_child_is_placed);
    RUN_TEST(a_capped_child_is_still_aligned_in_what_it_was_offered);
    RUN_TEST(vbox_cross_axis_alignment);
    RUN_TEST(vbox_with_unbounded_height_reports_its_natural_height);
    RUN_TEST(hbox_is_vbox_with_the_axes_swapped);
    RUN_TEST(hbox_with_unbounded_width_sums_its_children);
    RUN_TEST(stack_gives_every_child_the_same_rect);
    RUN_TEST(stack_measures_to_its_largest_child);
    RUN_TEST(stack_centres_a_child_when_asked);
    RUN_TEST(absolute_places_children_where_told);
    RUN_TEST(absolute_measures_to_the_union_of_its_children);
    RUN_TEST(border_gives_edges_their_size_and_the_centre_the_rest);
    RUN_TEST(panes_nest_without_special_cases);
    RUN_TEST(every_pane_answers_unbounded);
    RUN_TEST(an_empty_pane_measures_to_its_padding);
    RUN_TEST(grid_content_tracks_size_to_their_largest_cell);
    RUN_TEST(grid_columns_align_across_rows);
    RUN_TEST(grid_fixed_tracks_take_their_value);
    RUN_TEST(grid_weighted_tracks_share_the_leftover);
    RUN_TEST(grid_spanning_cell_covers_its_tracks_and_the_gap);
    RUN_TEST(grid_grows_tracks_for_an_oversized_spanning_cell);
    RUN_TEST(grid_ignores_a_cell_outside_its_tracks);
    RUN_TEST(grid_track_count_is_clamped_to_the_maximum);
    RUN_TEST(grid_weighted_tracks_collapse_when_unbounded);
    RUN_TEST(a_decided_width_is_the_width_a_node_is_measured_against);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(layout);
    GREATEST_MAIN_END();
}
