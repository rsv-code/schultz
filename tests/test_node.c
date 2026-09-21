/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_node.c - widget tree, invalidation, and the accessibility schema.
 *
 * The invalidation tests carry the most weight in this file. An invalidation
 * bug does not fail loudly: it leaves stale pixels on screen, or it repaints
 * everything and quietly discards the only advantage retained mode has. Both
 * are hard to notice by looking. So each case here states the change made and
 * asserts the exact region that must be repainted because of it.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_layout.h"
#include "schultz_node.h"
/* For the selection owner, which detaching has to give up along with the
 * overlay list and the ticking list. */
#include "schultz_widget.h"

/* A tree with a viewport, so dirty regions are clipped to something real. */
static int32_t tree_setup(schultz_tree **tree)
{
    int32_t result = schultz_tree_create(tree);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_tree_set_viewport(*tree,
                                     schultz_rect_make(0, 0, 800, 600));
}

/* Builds root -> panel(100,100,200,200) -> button(20,20,60,30). */
static int32_t build_sample(schultz_tree *tree, schultz_handle *panel,
                            schultz_handle *button)
{
    int32_t result = schultz_node_create(tree, schultz_tree_root(tree),
                                         panel);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_node_set_bounds(tree, *panel,
                                     schultz_rect_make(100, 100, 200, 200));
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_node_create(tree, *panel, button);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_node_set_bounds(tree, *button,
                                   schultz_rect_make(20, 20, 60, 30));
}

/* ------------------------------------------------------------- structure */

TEST tree_starts_with_only_a_root(void)
{
    schultz_tree *tree;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT(schultz_tree_root(tree) != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(1u, schultz_tree_node_count(tree));
    ASSERT_EQ(0u, schultz_node_child_count(tree, schultz_tree_root(tree)));
    ASSERT_EQ((uint32_t)SCHULTZ_ROLE_WINDOW,
              schultz_node_get_role(tree, schultz_tree_root(tree)));

    schultz_tree_destroy(tree);
    PASS();
}

TEST children_keep_insertion_order(void)
{
    schultz_tree *tree;
    schultz_handle root;
    schultz_handle a;
    schultz_handle b;
    schultz_handle c;
    schultz_handle got = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    root = schultz_tree_root(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, root, &a));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, root, &b));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, root, &c));

    ASSERT_EQ(3u, schultz_node_child_count(tree, root));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, root, 0, &got));
    ASSERT_EQ(a, got);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, root, 2, &got));
    ASSERT_EQ(c, got);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_node_child_at(tree, root, 3, &got));

    schultz_tree_destroy(tree);
    PASS();
}

TEST destroying_a_node_takes_its_subtree(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));
    ASSERT_EQ(3u, schultz_tree_node_count(tree));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_destroy(tree, panel));
    ASSERT_EQ(1u, schultz_tree_node_count(tree));
    ASSERT_EQ(0u, schultz_node_child_count(tree, schultz_tree_root(tree)));

    /* Both handles are stale, not merely unreachable. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_get_bounds(tree, panel, &bounds));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_get_bounds(tree, button, &bounds));

    schultz_tree_destroy(tree);
    PASS();
}

TEST the_root_cannot_be_destroyed_or_reparented(void)
{
    schultz_tree *tree;
    schultz_handle child;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &child));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_node_destroy(tree, schultz_tree_root(tree)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_node_set_parent(tree, schultz_tree_root(tree), child));

    schultz_tree_destroy(tree);
    PASS();
}

TEST reparenting_into_a_descendant_is_refused(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));

    /* Would make the panel its own grandchild. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_node_set_parent(tree, panel, button));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_node_set_parent(tree, panel, panel));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_detached_node_is_alive_but_out_of_the_tree(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;
    schultz_handle parent = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));
    ASSERT_EQ(1, schultz_node_is_attached(tree, button));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_parent(tree, button, SCHULTZ_HANDLE_NONE));

    /* Out of the tree, and the panel no longer counts it. */
    ASSERT_EQ(0, schultz_node_is_attached(tree, button));
    ASSERT_EQ(0u, schultz_node_child_count(tree, panel));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_parent(tree, button, &parent));
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE, parent);

    /* Still alive: the handle resolves and what it was told is still there. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_name(tree, button, "Still here"));
    ASSERT_STR_EQ("Still here", schultz_node_get_name(tree, button));

    /* And it goes back. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_parent(tree, button, panel));
    ASSERT_EQ(1, schultz_node_is_attached(tree, button));
    ASSERT_EQ(1u, schultz_node_child_count(tree, panel));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_detached_subtree_goes_with_its_top(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;
    schultz_handle caption;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, button, &caption));
    ASSERT_EQ(1, schultz_node_is_attached(tree, caption));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_parent(tree, button, SCHULTZ_HANDLE_NONE));

    /* The child kept its parent, and its parent is out of the tree. */
    ASSERT_EQ(0, schultz_node_is_attached(tree, caption));
    ASSERT_EQ(1u, schultz_node_child_count(tree, button));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_node_can_be_created_with_no_parent(void)
{
    schultz_tree *tree;
    schultz_handle loose = SCHULTZ_HANDLE_NONE;
    schultz_handle panel;
    schultz_handle button;
    int32_t result;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_create(tree, SCHULTZ_HANDLE_NONE, &loose));
    ASSERT(loose != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(0, schultz_node_is_attached(tree, loose));

    result = schultz_node_set_parent(tree, loose, panel);
    ASSERT_EQ(SCHULTZ_OK, result);
    ASSERT_EQ(1, schultz_node_is_attached(tree, loose));

    /* A parent that is named but does not resolve is still a mistake. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_create(tree, (schultz_handle)0xdeadbeefu, &loose));

    schultz_tree_destroy(tree);
    PASS();
}

TEST changing_a_detached_node_dirties_nothing(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;
    schultz_rect region;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));

    /* Leaving the tree repaints the area it occupied, which is the point. */
    schultz_tree_clear_dirty(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_parent(tree, button, SCHULTZ_HANDLE_NONE));
    ASSERT_EQ(1, schultz_tree_is_dirty(tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &region));
    /* Where it was: (100,100) + (20,20), 60 by 30. */
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(120, 120, 60, 30),
                                     region));

    /*
     * After that, nothing it does reaches the window. It still holds the
     * bounds it had, so without the attachment test this would repaint a
     * rectangle of live pixels every time a host touched a removed widget.
     */
    schultz_tree_clear_dirty(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_invalidate(tree, button));
    ASSERT_EQ(0, schultz_tree_is_dirty(tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, button,
                                        schultz_rect_make(0, 0, 10, 10)));
    ASSERT_EQ(0, schultz_tree_is_dirty(tree));

    /* Putting it back repaints where it landed. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_parent(tree, button, panel));
    ASSERT_EQ(1, schultz_tree_is_dirty(tree));

    schultz_tree_destroy(tree);
    PASS();
}

TEST adding_or_moving_a_node_asks_for_a_new_layout(void)
{
    schultz_tree *tree;
    schultz_handle root;
    schultz_handle left;
    schultz_handle right;
    schultz_handle child;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    root = schultz_tree_root(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, root, &left));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, root, &right));
    schultz_tree_clear_layout_dirty(tree);
    ASSERT_EQ(0, schultz_node_layout_dirty(tree, root));

    /* A new child means the parent has to place its children again. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, left, &child));
    ASSERT_EQ(1, schultz_node_layout_dirty(tree, left));
    ASSERT_EQ(1, schultz_node_layout_dirty(tree, root));

    schultz_tree_clear_layout_dirty(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_parent(tree, child, right));
    /* Both panes changed: one lost a child, the other gained one. */
    ASSERT_EQ(1, schultz_node_layout_dirty(tree, left));
    ASSERT_EQ(1, schultz_node_layout_dirty(tree, right));

    schultz_tree_clear_layout_dirty(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_destroy(tree, child));
    ASSERT_EQ(1, schultz_node_layout_dirty(tree, right));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_node_put_back_is_laid_out_again(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));
    schultz_tree_clear_layout_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_parent(tree, button, SCHULTZ_HANDLE_NONE));
    schultz_tree_clear_layout_dirty(tree);

    /*
     * A detached subtree is never walked, so nothing else would arrange it.
     * Coming back has to ask for that itself.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_parent(tree, button, panel));
    ASSERT_EQ(1, schultz_node_layout_dirty(tree, button));
    ASSERT_EQ(1, schultz_node_layout_dirty(tree, panel));

    schultz_tree_destroy(tree);
    PASS();
}

TEST detaching_gives_up_the_selection_and_the_overlay(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_set_selection_owner(tree, button));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_push_overlay(tree, panel, 0));
    ASSERT_EQ(1u, schultz_tree_overlay_count(tree));

    /* The owner is inside the subtree, not its top. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_parent(tree, panel, SCHULTZ_HANDLE_NONE));

    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_tree_selection_owner(tree));
    ASSERT_EQ(0u, schultz_tree_overlay_count(tree));

    schultz_tree_destroy(tree);
    PASS();
}

TEST absolute_bounds_accumulate_ancestor_offsets(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_absolute_bounds(tree, button, &bounds));
    /* 100 + 20, 100 + 20, keeping its own size. */
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(120, 120, 60, 30),
                                     bounds));

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------------- invalidation */

TEST a_new_tree_is_clean(void)
{
    schultz_tree *tree;
    schultz_rect region;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(0, schultz_tree_is_dirty(tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &region));
    ASSERT_EQ(1, schultz_rect_is_empty(region));

    schultz_tree_destroy(tree);
    PASS();
}

TEST invalidating_a_node_dirties_exactly_its_area(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;
    schultz_rect region;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));
    schultz_tree_clear_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_invalidate(tree, button));
    ASSERT_EQ(1, schultz_tree_is_dirty(tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &region));
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(120, 120, 60, 30),
                                     region));

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * The flag that lets a paint walk skip work: a dirty leaf must mark every
 * ancestor's subtree flag, and no sibling's.
 */
TEST dirty_propagates_to_ancestors_only(void)
{
    schultz_tree *tree;
    schultz_handle root;
    schultz_handle panel;
    schultz_handle button;
    schultz_handle sibling;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    root = schultz_tree_root(tree);
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, root, &sibling));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, sibling,
                                        schultz_rect_make(400, 400, 50, 50)));
    schultz_tree_clear_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_invalidate(tree, button));

    ASSERT_EQ(1, schultz_node_is_dirty(tree, button));
    ASSERT_EQ(1, schultz_node_subtree_dirty(tree, button));
    /* The panel itself does not need repainting, but its subtree does. */
    ASSERT_EQ(0, schultz_node_is_dirty(tree, panel));
    ASSERT_EQ(1, schultz_node_subtree_dirty(tree, panel));
    ASSERT_EQ(1, schultz_node_subtree_dirty(tree, root));
    /* The sibling subtree stays clean, so a paint walk can skip it. */
    ASSERT_EQ(0, schultz_node_is_dirty(tree, sibling));
    ASSERT_EQ(0, schultz_node_subtree_dirty(tree, sibling));

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * The classic invalidation bug. Moving a node must dirty the area it left as
 * well as the area it arrived in; marking only the new one leaves a copy of
 * the node painted at the old position.
 */
TEST moving_a_node_dirties_both_the_old_and_new_area(void)
{
    schultz_tree *tree;
    schultz_handle node;
    schultz_rect region;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, node,
                                        schultz_rect_make(10, 10, 40, 40)));
    schultz_tree_clear_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, node,
                                        schultz_rect_make(200, 300, 40, 40)));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &region));
    /* Union of (10,10,40,40) and (200,300,40,40). */
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(10, 10, 230, 330),
                                     region));

    schultz_tree_destroy(tree);
    PASS();
}

TEST shrinking_a_node_dirties_the_area_it_vacated(void)
{
    schultz_tree *tree;
    schultz_handle node;
    schultz_rect region;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, node,
                                        schultz_rect_make(0, 0, 200, 200)));
    schultz_tree_clear_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, node,
                                        schultz_rect_make(0, 0, 50, 50)));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &region));
    /* The full former area, not just the smaller new one. */
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(0, 0, 200, 200),
                                     region));

    schultz_tree_destroy(tree);
    PASS();
}

TEST setting_the_same_bounds_dirties_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, node,
                                        schultz_rect_make(10, 10, 40, 40)));
    schultz_tree_clear_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, node,
                                        schultz_rect_make(10, 10, 40, 40)));
    ASSERT_EQ(0, schultz_tree_is_dirty(tree));

    schultz_tree_destroy(tree);
    PASS();
}

/* Moving a parent must repaint every descendant's old and new area too. */
TEST moving_a_parent_dirties_its_children(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;
    schultz_rect region;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));
    schultz_tree_clear_dirty(tree);

    /* Panel 100,100 -> 400,100. Button rides along, 120,120 -> 420,120. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, panel,
                                        schultz_rect_make(400, 100, 200, 200)));

    ASSERT_EQ(1, schultz_node_is_dirty(tree, button));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &region));
    /* Union of old panel (100,100,200,200) and new panel (400,100,200,200). */
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(100, 100, 500, 200),
                                     region));

    schultz_tree_destroy(tree);
    PASS();
}

TEST hiding_a_node_dirties_the_area_it_occupied(void)
{
    schultz_tree *tree;
    schultz_handle node;
    schultz_rect region;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, node,
                                        schultz_rect_make(60, 70, 80, 90)));
    schultz_tree_clear_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_state(tree, node,
                                                 SCHULTZ_STATE_ENABLED));
    ASSERT_EQ(1, schultz_tree_is_dirty(tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &region));
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(60, 70, 80, 90),
                                     region));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_state_change_that_changes_nothing_dirties_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));
    schultz_tree_clear_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_state(tree, node,
                                                 SCHULTZ_STATE_DEFAULT));
    ASSERT_EQ(0, schultz_tree_is_dirty(tree));

    /* But a real change, such as hover, must dirty it. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_state(tree, node,
                                     SCHULTZ_STATE_DEFAULT |
                                     SCHULTZ_STATE_HOVERED));
    ASSERT_EQ(1, schultz_node_is_dirty(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST destroying_a_node_dirties_the_area_it_occupied(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;
    schultz_rect region;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));
    schultz_tree_clear_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_destroy(tree, panel));

    ASSERT_EQ(1, schultz_tree_is_dirty(tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &region));
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(100, 100, 200, 200),
                                     region));
    /* The root must know something under it changed. */
    ASSERT_EQ(1, schultz_node_subtree_dirty(tree, schultz_tree_root(tree)));

    schultz_tree_destroy(tree);
    PASS();
}

TEST reparenting_dirties_both_positions(void)
{
    schultz_tree *tree;
    schultz_handle root;
    schultz_handle left;
    schultz_handle right;
    schultz_handle child;
    schultz_rect region;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    root = schultz_tree_root(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, root, &left));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, left,
                                        schultz_rect_make(0, 0, 100, 100)));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, root, &right));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, right,
                                        schultz_rect_make(500, 0, 100, 100)));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, left, &child));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, child,
                                        schultz_rect_make(10, 10, 20, 20)));
    schultz_tree_clear_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_parent(tree, child, right));

    ASSERT_EQ(1u, schultz_node_child_count(tree, right));
    ASSERT_EQ(0u, schultz_node_child_count(tree, left));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &region));
    /* Union of (10,10,20,20) under left and (510,10,20,20) under right. */
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(10, 10, 520, 20),
                                     region));

    schultz_tree_destroy(tree);
    PASS();
}

TEST the_dirty_region_is_the_union_of_every_change(void)
{
    schultz_tree *tree;
    schultz_handle a;
    schultz_handle b;
    schultz_rect region;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &a));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, a,
                                        schultz_rect_make(10, 20, 30, 40)));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &b));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, b,
                                        schultz_rect_make(500, 400, 60, 50)));
    schultz_tree_clear_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_invalidate(tree, a));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_invalidate(tree, b));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &region));
    /* (10,20)-(40,60) unioned with (500,400)-(560,450). */
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(10, 20, 550, 430),
                                     region));

    schultz_tree_destroy(tree);
    PASS();
}

/* A node moved off screen must not inflate the repaint area. */
TEST the_dirty_region_is_clipped_to_the_viewport(void)
{
    schultz_tree *tree;
    schultz_handle node;
    schultz_rect region;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, node,
                                        schultz_rect_make(700, 500, 500, 500)));
    schultz_tree_clear_dirty(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_invalidate(tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &region));
    ASSERT_EQ(1, schultz_rect_equals(schultz_rect_make(700, 500, 100, 100),
                                     region));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_node_entirely_off_screen_dirties_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, node,
                                        schultz_rect_make(2000, 2000, 50, 50)));
    schultz_tree_clear_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_invalidate(tree, node));
    ASSERT_EQ(0, schultz_tree_is_dirty(tree));

    schultz_tree_destroy(tree);
    PASS();
}

TEST clearing_dirty_resets_every_node(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;
    schultz_rect region;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));
    ASSERT_EQ(1, schultz_tree_is_dirty(tree));

    schultz_tree_clear_dirty(tree);

    ASSERT_EQ(0, schultz_tree_is_dirty(tree));
    ASSERT_EQ(0, schultz_node_is_dirty(tree, button));
    ASSERT_EQ(0, schultz_node_subtree_dirty(tree, panel));
    ASSERT_EQ(0, schultz_node_subtree_dirty(tree, schultz_tree_root(tree)));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &region));
    ASSERT_EQ(1, schultz_rect_is_empty(region));

    schultz_tree_destroy(tree);
    PASS();
}

/* -------------------------------------------------------------- visibility */

TEST visibility_is_inherited_from_ancestors(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle button;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, build_sample(tree, &panel, &button));
    ASSERT_EQ(1, schultz_node_is_visible(tree, button));

    /* Hiding the parent hides the child, whose own flag is untouched. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_state(tree, panel,
                                                 SCHULTZ_STATE_ENABLED));
    ASSERT_EQ(0, schultz_node_is_visible(tree, button));
    ASSERT(schultz_node_get_state(tree, button) & SCHULTZ_STATE_VISIBLE);

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------- accessibility schema */

TEST accessibility_fields_round_trip(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));

    ASSERT_EQ((uint32_t)SCHULTZ_ROLE_UNKNOWN,
              schultz_node_get_role(tree, node));
    ASSERT_EQ(NULL, schultz_node_get_name(tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_role(tree, node,
                                                SCHULTZ_ROLE_BUTTON));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_name(tree, node, "Save"));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_value(tree, node, "unsaved"));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_actions(tree, node,
                                       SCHULTZ_ACTION_CLICK |
                                       SCHULTZ_ACTION_FOCUS));

    ASSERT_EQ((uint32_t)SCHULTZ_ROLE_BUTTON,
              schultz_node_get_role(tree, node));
    ASSERT_STR_EQ("Save", schultz_node_get_name(tree, node));
    ASSERT_STR_EQ("unsaved", schultz_node_get_value(tree, node));
    ASSERT_EQ((uint32_t)(SCHULTZ_ACTION_CLICK | SCHULTZ_ACTION_FOCUS),
              schultz_node_get_actions(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST names_are_copied_and_replaceable(void)
{
    schultz_tree *tree;
    schultz_handle node;
    char buffer[16];

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));

    strcpy(buffer, "Original");
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_name(tree, node, buffer));
    /* Scribbling the caller's buffer must not change the stored name. */
    memset(buffer, 'x', sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    ASSERT_STR_EQ("Original", schultz_node_get_name(tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_name(tree, node, "Replaced"));
    ASSERT_STR_EQ("Replaced", schultz_node_get_name(tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_name(tree, node, NULL));
    ASSERT_EQ(NULL, schultz_node_get_name(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST stale_handles_are_rejected_everywhere(void)
{
    schultz_tree *tree;
    schultz_handle node;
    schultz_rect bounds;
    schultz_handle parent;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_destroy(tree, node));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_get_bounds(tree, node, &bounds));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_set_bounds(tree, node, bounds));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_invalidate(tree, node));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_parent(tree, node, &parent));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_set_role(tree, node, SCHULTZ_ROLE_BUTTON));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_set_name(tree, node, "gone"));
    ASSERT_EQ(0u, schultz_node_child_count(tree, node));
    ASSERT_EQ(0, schultz_node_is_visible(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_deep_tree_stays_consistent(void)
{
    schultz_tree *tree;
    schultz_handle parent;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;
    uint32_t depth;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    parent = schultz_tree_root(tree);

    /* 100 nested nodes, each offset by 1, so absolute x should reach 100. */
    for (depth = 0; depth < 100u; depth++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, parent, &node));
        ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(tree, node,
                                        schultz_rect_make(1, 1, 10, 10)));
        parent = node;
    }
    ASSERT_EQ(101u, schultz_tree_node_count(tree));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(tree, node, &bounds));
    ASSERT_EQ(100.0f, bounds.x);
    ASSERT_EQ(100.0f, bounds.y);

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * A host asks the root whether laying anything out is worth the time, so the
 * flag has to reach the root from wherever it was set. Without that a change
 * deep in the tree looks like no change at all, and whatever was invalidated
 * is never laid out.
 */
TEST layout_dirty_reaches_the_root_from_any_depth(void)
{
    schultz_tree *tree = NULL;
    schultz_handle parent = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;
    schultz_handle grandchild = SCHULTZ_HANDLE_NONE;
    schultz_handle root;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    root = schultz_tree_root(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, root, &parent));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, parent, &child));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, child, &grandchild));

    schultz_tree_clear_layout_dirty(tree);
    ASSERT_FALSE(schultz_node_layout_dirty(tree, root));
    ASSERT_FALSE(schultz_node_layout_dirty(tree, grandchild));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_invalidate_layout(tree, grandchild));
    ASSERT(schultz_node_layout_dirty(tree, grandchild));
    ASSERT(schultz_node_layout_dirty(tree, child));
    ASSERT(schultz_node_layout_dirty(tree, parent));
    ASSERT(schultz_node_layout_dirty(tree, root));

    /* A sibling branch is untouched. */
    {
        schultz_handle other = SCHULTZ_HANDLE_NONE;

        schultz_tree_clear_layout_dirty(tree);
        schultz_node_create(tree, root, &other);
        schultz_tree_clear_layout_dirty(tree);
        schultz_node_invalidate_layout(tree, grandchild);
        ASSERT_FALSE(schultz_node_layout_dirty(tree, other));
    }

    schultz_tree_clear_layout_dirty(tree);
    ASSERT_FALSE(schultz_node_layout_dirty(tree, root));
    ASSERT_FALSE(schultz_node_layout_dirty(tree, grandchild));

    schultz_tree_destroy(tree);
    PASS();
}

/* A size hint set deep down has to reach the root the same way. */
TEST every_layout_setter_asks_for_a_new_layout(void)
{
    schultz_tree *tree = NULL;
    schultz_handle column = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;
    schultz_layout_params params;
    schultz_grid_track track;

    /*
     * A setter that changes where something goes and does not say so leaves
     * the change sitting in the struct until anything else happens to dirty
     * the tree, and then it lands all at once. From the outside the call was
     * ignored, which is the hardest kind of bug to place: the code is right
     * and the screen is wrong.
     *
     * One test over all of them, rather than one each, so a setter added
     * later has somewhere obvious to be added too.
     */
    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &column));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, column, &child));

    schultz_tree_clear_layout_dirty(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_layout_params(tree, child,
                                                         &params));
    params.align = SCHULTZ_ALIGN_START;
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_layout_params(tree, child,
                                                         &params));
    ASSERT(schultz_node_layout_dirty(tree, schultz_tree_root(tree)));

    schultz_tree_clear_layout_dirty(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_pane(tree, column,
                                                schultz_pane_vbox()));
    ASSERT(schultz_node_layout_dirty(tree, schultz_tree_root(tree)));

    schultz_tree_clear_layout_dirty(tree);
    track.kind  = SCHULTZ_TRACK_FIXED;
    track.value = 40.0f;
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_grid_tracks(tree, column, &track,
                                                       1u, &track, 1u));
    ASSERT(schultz_node_layout_dirty(tree, schultz_tree_root(tree)));

    schultz_tree_clear_layout_dirty(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_pref_size(tree, child, 40.0f,
                                                     20.0f));
    ASSERT(schultz_node_layout_dirty(tree, schultz_tree_root(tree)));

    schultz_tree_clear_layout_dirty(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_min_size(tree, child, 10.0f,
                                                    10.0f));
    ASSERT(schultz_node_layout_dirty(tree, schultz_tree_root(tree)));

    schultz_tree_clear_layout_dirty(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_max_size(tree, child, 80.0f,
                                                    80.0f));
    ASSERT(schultz_node_layout_dirty(tree, schultz_tree_root(tree)));

    schultz_tree_clear_layout_dirty(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_spacing(tree, column, 4.0f, 2.0f));
    ASSERT(schultz_node_layout_dirty(tree, schultz_tree_root(tree)));

    schultz_tree_destroy(tree);
    PASS();
}

TEST alignment_set_after_a_layout_takes_effect(void)
{
    schultz_tree *tree = NULL;
    schultz_handle column = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;
    schultz_layout_params params;
    schultz_rect bounds;

    /* The shape it was reported in: a settled tree, then one call. */
    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &column));
    schultz_node_set_pane(tree, column, schultz_pane_vbox());
    schultz_node_set_bounds(tree, column, schultz_rect_make(0, 0, 600, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, column, &child));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_pref_size(tree, child, 84.0f,
                                                     20.0f));

    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(tree, child, &bounds));
    ASSERT_EQ(600.0f, bounds.width);   /* stretched, as a column does */

    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_layout_params(tree, child,
                                                         &params));
    params.align = SCHULTZ_ALIGN_START;
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_layout_params(tree, child,
                                                         &params));

    /* One pass, and it is the width it asked for. */
    schultz_layout_run(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(tree, child, &bounds));
    ASSERT_EQ(84.0f, bounds.width);

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_size_hint_makes_the_root_report_layout_dirty(void)
{
    schultz_tree *tree = NULL;
    schultz_handle parent = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &parent);
    schultz_node_create(tree, parent, &child);
    schultz_tree_clear_layout_dirty(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_pref_size(tree, child, 120.0f,
                                                     SCHULTZ_SIZE_UNSET));
    ASSERT(schultz_node_layout_dirty(tree, schultz_tree_root(tree)));

    schultz_tree_clear_layout_dirty(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_spacing(tree, child, 4.0f, 2.0f));
    ASSERT(schultz_node_layout_dirty(tree, schultz_tree_root(tree)));

    schultz_tree_destroy(tree);
    PASS();
}

SUITE(node)
{
    RUN_TEST(layout_dirty_reaches_the_root_from_any_depth);
    RUN_TEST(every_layout_setter_asks_for_a_new_layout);
    RUN_TEST(alignment_set_after_a_layout_takes_effect);
    RUN_TEST(a_size_hint_makes_the_root_report_layout_dirty);
    RUN_TEST(tree_starts_with_only_a_root);
    RUN_TEST(children_keep_insertion_order);
    RUN_TEST(destroying_a_node_takes_its_subtree);
    RUN_TEST(the_root_cannot_be_destroyed_or_reparented);
    RUN_TEST(reparenting_into_a_descendant_is_refused);
    RUN_TEST(a_detached_node_is_alive_but_out_of_the_tree);
    RUN_TEST(a_detached_subtree_goes_with_its_top);
    RUN_TEST(a_node_can_be_created_with_no_parent);
    RUN_TEST(changing_a_detached_node_dirties_nothing);
    RUN_TEST(adding_or_moving_a_node_asks_for_a_new_layout);
    RUN_TEST(a_node_put_back_is_laid_out_again);
    RUN_TEST(detaching_gives_up_the_selection_and_the_overlay);
    RUN_TEST(absolute_bounds_accumulate_ancestor_offsets);
    RUN_TEST(a_new_tree_is_clean);
    RUN_TEST(invalidating_a_node_dirties_exactly_its_area);
    RUN_TEST(dirty_propagates_to_ancestors_only);
    RUN_TEST(moving_a_node_dirties_both_the_old_and_new_area);
    RUN_TEST(shrinking_a_node_dirties_the_area_it_vacated);
    RUN_TEST(setting_the_same_bounds_dirties_nothing);
    RUN_TEST(moving_a_parent_dirties_its_children);
    RUN_TEST(hiding_a_node_dirties_the_area_it_occupied);
    RUN_TEST(a_state_change_that_changes_nothing_dirties_nothing);
    RUN_TEST(destroying_a_node_dirties_the_area_it_occupied);
    RUN_TEST(reparenting_dirties_both_positions);
    RUN_TEST(the_dirty_region_is_the_union_of_every_change);
    RUN_TEST(the_dirty_region_is_clipped_to_the_viewport);
    RUN_TEST(a_node_entirely_off_screen_dirties_nothing);
    RUN_TEST(clearing_dirty_resets_every_node);
    RUN_TEST(visibility_is_inherited_from_ancestors);
    RUN_TEST(accessibility_fields_round_trip);
    RUN_TEST(names_are_copied_and_replaceable);
    RUN_TEST(stale_handles_are_rejected_everywhere);
    RUN_TEST(a_deep_tree_stays_consistent);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(node);
    GREATEST_MAIN_END();
}
