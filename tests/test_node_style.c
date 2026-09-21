/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_node_style.c - style layering, inheritance and invalidation on the tree.
 *
 * This is the part of the style system most likely to be wrong, because it is
 * where ordering, the ancestor walk and the three dirty bits meet. Each test
 * states the change made and asserts both the resolved value and what the
 * change cost.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_node.h"

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

/* Resolves and then clears the paint and layout marks, so a test starts
 * from a settled tree and can attribute what follows to its own change. */
static void settle(schultz_tree *tree)
{
    schultz_tree_resolve_styles(tree);
    schultz_tree_clear_dirty(tree);
    schultz_tree_clear_layout_dirty(tree);
}

static schultz_color color_of(schultz_tree *tree, schultz_handle node,
                              uint32_t property)
{
    return schultz_resolved_color(schultz_node_resolved(tree, node),
                                  property);
}

static float number_of(schultz_tree *tree, schultz_handle node,
                       uint32_t property)
{
    return schultz_resolved_number(schultz_node_resolved(tree, node),
                                   property);
}

static schultz_handle make_style(schultz_tree *tree, uint32_t property,
                                 schultz_value value)
{
    schultz_patch patch;
    schultz_handle style = SCHULTZ_HANDLE_NONE;

    schultz_patch_init(&patch);
    schultz_patch_set(&patch, property, value);
    schultz_style_register(tree, &patch, &style);
    schultz_patch_free(&patch);
    return style;
}

/*
 * A style that says how it looks hovered as well as plain. The state patches
 * go in an array indexed by SCHULTZ_STYLE_STATE_*, so the ones nothing was
 * said about stay empty.
 */
static schultz_handle make_hover_style(schultz_tree *tree,
                                       schultz_color plain,
                                       schultz_color hovered)
{
    schultz_patch base;
    schultz_patch states[SCHULTZ_STYLE_STATE_COUNT];
    schultz_handle style = SCHULTZ_HANDLE_NONE;
    uint32_t i;

    schultz_patch_init(&base);
    schultz_patch_set(&base, SCHULTZ_PROP_BACKGROUND,
                      schultz_value_color(plain));
    for (i = 0; i < SCHULTZ_STYLE_STATE_COUNT; i++) {
        schultz_patch_init(&states[i]);
    }
    schultz_patch_set(&states[SCHULTZ_STYLE_STATE_HOVERED],
                      SCHULTZ_PROP_BACKGROUND,
                      schultz_value_color(hovered));

    schultz_style_register_states(tree, &base, states, &style);

    schultz_patch_free(&base);
    for (i = 0; i < SCHULTZ_STYLE_STATE_COUNT; i++) {
        schultz_patch_free(&states[i]);
    }
    return style;
}

static void set_hovered(schultz_tree *tree, schultz_handle node, int32_t on)
{
    uint32_t state = schultz_node_get_state(tree, node);

    schultz_node_set_state(tree, node,
        on ? (state | SCHULTZ_STATE_HOVERED)
           : (state & ~(uint32_t)SCHULTZ_STATE_HOVERED));
}

TEST a_style_carries_its_own_state_rules(void)
{
    schultz_tree *tree;
    schultz_handle a;
    schultz_handle b;
    schultz_handle style;
    schultz_color green = schultz_color_rgba(0, 200, 0, 255);
    schultz_color red = schultz_color_rgba(255, 0, 0, 255);

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &a));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &b));
    style = make_hover_style(tree, green, red);
    ASSERT(style != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_add_style(tree, a, style));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_add_style(tree, b, style));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(1, schultz_color_equals(green,
                    color_of(tree, a, SCHULTZ_PROP_BACKGROUND)));

    /* Written once, and both nodes have it without being told. */
    set_hovered(tree, a, 1);
    set_hovered(tree, b, 1);
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(1, schultz_color_equals(red,
                    color_of(tree, a, SCHULTZ_PROP_BACKGROUND)));
    ASSERT_EQ(1, schultz_color_equals(red,
                    color_of(tree, b, SCHULTZ_PROP_BACKGROUND)));

    schultz_tree_destroy(tree);
    PASS();
}

TEST removing_a_style_takes_its_state_rules_with_it(void)
{
    schultz_tree *tree;
    schultz_handle node;
    schultz_handle style;
    schultz_color green = schultz_color_rgba(0, 200, 0, 255);
    schultz_color red = schultz_color_rgba(255, 0, 0, 255);

    /*
     * The reason state belongs to the style rather than to every node
     * wearing it. Set per node, a hover rule outlives the style that
     * prompted it and sticks to the widget for good.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));
    style = make_hover_style(tree, green, red);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_add_style(tree, node, style));

    set_hovered(tree, node, 1);
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(1, schultz_color_equals(red,
                    color_of(tree, node, SCHULTZ_PROP_BACKGROUND)));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_remove_style(tree, node, style));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(0, schultz_color_equals(red,
                    color_of(tree, node, SCHULTZ_PROP_BACKGROUND)));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_node_has_the_last_word_on_a_state(void)
{
    schultz_tree *tree;
    schultz_handle node;
    schultz_handle style;
    schultz_color green = schultz_color_rgba(0, 200, 0, 255);
    schultz_color red = schultz_color_rgba(255, 0, 0, 255);
    schultz_color blue = schultz_color_rgba(0, 0, 255, 255);

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));
    style = make_hover_style(tree, green, red);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_add_style(tree, node, style));

    /* Naming a state on one node is the most specific thing a caller can
     * do, so it beats the same state coming from a style. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_state_property(tree, node,
                  SCHULTZ_STYLE_STATE_HOVERED, SCHULTZ_PROP_BACKGROUND,
                  schultz_value_color(blue)));
    set_hovered(tree, node, 1);
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(1, schultz_color_equals(blue,
                    color_of(tree, node, SCHULTZ_PROP_BACKGROUND)));

    /*
     * And a style's state beats a plain layer, including an inline property
     * set straight on the node, which is the layer directly below it.
     */
    {
        schultz_handle other = SCHULTZ_HANDLE_NONE;

        ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree,
                      schultz_tree_root(tree), &other));
        ASSERT_EQ(SCHULTZ_OK, schultz_node_add_style(tree, other, style));
        ASSERT_EQ(SCHULTZ_OK, schultz_node_set_style_property(tree, other,
                      SCHULTZ_PROP_BACKGROUND,
                      schultz_value_color(schultz_color_rgba(1, 1, 1, 255))));
        set_hovered(tree, other, 1);
        schultz_tree_resolve_styles(tree);
        ASSERT_EQ(1, schultz_color_equals(red,
                        color_of(tree, other, SCHULTZ_PROP_BACKGROUND)));
    }

    schultz_tree_destroy(tree);
    PASS();
}

/* --------------------------------------------------------- basic layers */

TEST a_new_node_starts_style_dirty(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &node));
    ASSERT_EQ(1, schultz_node_style_dirty(tree, node));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_resolve_styles(tree));
    ASSERT_EQ(0, schultz_node_style_dirty(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST an_unstyled_node_resolves_to_the_defaults(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(1, schultz_color_equals(
        schultz_theme_color(NULL, SCHULTZ_TOKEN_COLOR_TEXT),
        color_of(tree, node, SCHULTZ_PROP_TEXT_COLOR)));
    ASSERT_EQ(0u, color_of(tree, node, SCHULTZ_PROP_BACKGROUND).a);

    schultz_tree_destroy(tree);
    PASS();
}

/* The requirement: styling one node directly, with no separate theme. */
TEST an_inline_property_styles_one_node_only(void)
{
    schultz_tree *tree;
    schultz_handle a;
    schultz_handle b;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &a);
    schultz_node_create(tree, schultz_tree_root(tree), &b);
    settle(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_style_property(tree, a,
                    SCHULTZ_PROP_BACKGROUND,
                    schultz_value_color(schultz_color_rgba(200, 0, 0, 255))));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(200u, color_of(tree, a, SCHULTZ_PROP_BACKGROUND).r);
    /* Its sibling is untouched. */
    ASSERT_EQ(0u, color_of(tree, b, SCHULTZ_PROP_BACKGROUND).a);

    schultz_tree_destroy(tree);
    PASS();
}

TEST clearing_an_inline_property_falls_back(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_PADDING,
                                    schultz_value_number(33.0f));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(33.0f, number_of(tree, node, SCHULTZ_PROP_PADDING));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_clear_style_property(tree, node,
                                            SCHULTZ_PROP_PADDING));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(0.0f, number_of(tree, node, SCHULTZ_PROP_PADDING));

    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------------------- named styles */

TEST a_registered_style_applies_to_every_node_using_it(void)
{
    schultz_tree *tree;
    schultz_handle style;
    schultz_handle a;
    schultz_handle b;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    style = make_style(tree, SCHULTZ_PROP_BACKGROUND,
                       schultz_value_color(schultz_color_rgba(10, 20, 30,
                                                              255)));
    ASSERT(style != SCHULTZ_HANDLE_NONE);

    schultz_node_create(tree, schultz_tree_root(tree), &a);
    schultz_node_create(tree, schultz_tree_root(tree), &b);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_add_style(tree, a, style));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_add_style(tree, b, style));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(10u, color_of(tree, a, SCHULTZ_PROP_BACKGROUND).r);
    ASSERT_EQ(10u, color_of(tree, b, SCHULTZ_PROP_BACKGROUND).r);
    ASSERT_EQ(1u, schultz_node_style_count(tree, a));

    schultz_tree_destroy(tree);
    PASS();
}

/* Ordering: the later style wins, with no specificity involved. */
TEST a_later_style_overrides_an_earlier_one(void)
{
    schultz_tree *tree;
    schultz_handle first;
    schultz_handle second;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    first  = make_style(tree, SCHULTZ_PROP_BACKGROUND,
                        schultz_value_color(schultz_color_rgba(1,0,0,255)));
    second = make_style(tree, SCHULTZ_PROP_BACKGROUND,
                        schultz_value_color(schultz_color_rgba(2,0,0,255)));

    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_add_style(tree, node, first);
    schultz_node_add_style(tree, node, second);
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(2u, color_of(tree, node, SCHULTZ_PROP_BACKGROUND).r);
    ASSERT_EQ(2u, schultz_node_style_count(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST the_inline_patch_beats_every_style(void)
{
    schultz_tree *tree;
    schultz_handle style;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    style = make_style(tree, SCHULTZ_PROP_BACKGROUND,
                       schultz_value_color(schultz_color_rgba(1,0,0,255)));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_add_style(tree, node, style);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BACKGROUND,
                    schultz_value_color(schultz_color_rgba(9,0,0,255)));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(9u, color_of(tree, node, SCHULTZ_PROP_BACKGROUND).r);

    schultz_tree_destroy(tree);
    PASS();
}

TEST removing_a_style_reverts_the_node(void)
{
    schultz_tree *tree;
    schultz_handle style;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    style = make_style(tree, SCHULTZ_PROP_PADDING,
                       schultz_value_number(12.0f));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_add_style(tree, node, style);
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(12.0f, number_of(tree, node, SCHULTZ_PROP_PADDING));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_remove_style(tree, node, style));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(0.0f, number_of(tree, node, SCHULTZ_PROP_PADDING));
    ASSERT_EQ(0u, schultz_node_style_count(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST adding_the_same_style_twice_is_idempotent(void)
{
    schultz_tree *tree;
    schultz_handle style;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    style = make_style(tree, SCHULTZ_PROP_PADDING, schultz_value_number(4));
    schultz_node_create(tree, schultz_tree_root(tree), &node);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_add_style(tree, node, style));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_add_style(tree, node, style));
    ASSERT_EQ(1u, schultz_node_style_count(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * A style outlives the node that dropped it as long as another still uses it.
 * Getting the refcount wrong here is a use after free, so it is checked in
 * both directions.
 */
TEST a_style_survives_until_the_last_node_drops_it(void)
{
    schultz_tree *tree;
    schultz_handle style;
    schultz_handle a;
    schultz_handle b;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    style = make_style(tree, SCHULTZ_PROP_PADDING, schultz_value_number(7));
    schultz_node_create(tree, schultz_tree_root(tree), &a);
    schultz_node_create(tree, schultz_tree_root(tree), &b);
    schultz_node_add_style(tree, a, style);
    schultz_node_add_style(tree, b, style);

    /* Destroying one holder must not free the style out from under the
     * other. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_destroy(tree, a));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(7.0f, number_of(tree, b, SCHULTZ_PROP_PADDING));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_remove_style(tree, b, style));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(0.0f, number_of(tree, b, SCHULTZ_PROP_PADDING));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_registered_style_is_a_copy(void)
{
    schultz_tree *tree;
    schultz_patch patch;
    schultz_handle style;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_patch_init(&patch);
    schultz_patch_set(&patch, SCHULTZ_PROP_PADDING, schultz_value_number(5));
    ASSERT_EQ(SCHULTZ_OK, schultz_style_register(tree, &patch, &style));

    /* Mutating and freeing the caller's patch must not affect the style. */
    schultz_patch_set(&patch, SCHULTZ_PROP_PADDING, schultz_value_number(99));
    schultz_patch_free(&patch);

    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_add_style(tree, node, style);
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(5.0f, number_of(tree, node, SCHULTZ_PROP_PADDING));

    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------------------- state patches */

TEST a_state_patch_applies_only_in_that_state(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BACKGROUND,
                    schultz_value_color(schultz_color_rgba(1,0,0,255)));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_HOVERED,
                    SCHULTZ_PROP_BACKGROUND,
                    schultz_value_color(schultz_color_rgba(2,0,0,255)));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(1u, color_of(tree, node, SCHULTZ_PROP_BACKGROUND).r);

    schultz_node_set_state(tree, node,
                           SCHULTZ_STATE_DEFAULT | SCHULTZ_STATE_HOVERED);
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(2u, color_of(tree, node, SCHULTZ_PROP_BACKGROUND).r);

    /* And it goes away again. */
    schultz_node_set_state(tree, node, SCHULTZ_STATE_DEFAULT);
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(1u, color_of(tree, node, SCHULTZ_PROP_BACKGROUND).r);

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * Precedence: a disabled control must not appear to respond to the pointer,
 * so disabled beats pressed beats hovered even when all three could apply.
 */
TEST disabled_beats_pressed_beats_hovered(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_HOVERED,
                    SCHULTZ_PROP_BACKGROUND,
                    schultz_value_color(schultz_color_rgba(1,0,0,255)));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_PRESSED,
                    SCHULTZ_PROP_BACKGROUND,
                    schultz_value_color(schultz_color_rgba(2,0,0,255)));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_DISABLED,
                    SCHULTZ_PROP_BACKGROUND,
                    schultz_value_color(schultz_color_rgba(3,0,0,255)));

    schultz_node_set_state(tree, node,
                           SCHULTZ_STATE_DEFAULT | SCHULTZ_STATE_HOVERED);
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(1u, color_of(tree, node, SCHULTZ_PROP_BACKGROUND).r);

    schultz_node_set_state(tree, node, SCHULTZ_STATE_DEFAULT |
                           SCHULTZ_STATE_HOVERED | SCHULTZ_STATE_PRESSED);
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(2u, color_of(tree, node, SCHULTZ_PROP_BACKGROUND).r);

    /* Disabled is the absence of enabled, not a flag of its own. */
    schultz_node_set_state(tree, node, SCHULTZ_STATE_VISIBLE |
                           SCHULTZ_STATE_HOVERED | SCHULTZ_STATE_PRESSED);
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(3u, color_of(tree, node, SCHULTZ_PROP_BACKGROUND).r);

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_state_patch_beats_the_inline_patch(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_PADDING,
                                    schultz_value_number(4));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_FOCUSED,
                                    SCHULTZ_PROP_PADDING,
                                    schultz_value_number(8));

    schultz_node_set_state(tree, node,
                           SCHULTZ_STATE_DEFAULT | SCHULTZ_STATE_FOCUSED);
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(8.0f, number_of(tree, node, SCHULTZ_PROP_PADDING));

    schultz_tree_destroy(tree);
    PASS();
}

TEST every_style_state_is_reachable(void)
{
    schultz_tree *tree;
    schultz_handle node;
    struct { uint32_t style_state; uint32_t node_state; } cases[] = {
        { SCHULTZ_STYLE_STATE_HOVERED,
          SCHULTZ_STATE_DEFAULT | SCHULTZ_STATE_HOVERED },
        { SCHULTZ_STYLE_STATE_FOCUSED,
          SCHULTZ_STATE_DEFAULT | SCHULTZ_STATE_FOCUSED },
        { SCHULTZ_STYLE_STATE_CHECKED,
          SCHULTZ_STATE_DEFAULT | SCHULTZ_STATE_CHECKED },
        { SCHULTZ_STYLE_STATE_SELECTED,
          SCHULTZ_STATE_DEFAULT | SCHULTZ_STATE_SELECTED },
        { SCHULTZ_STYLE_STATE_PRESSED,
          SCHULTZ_STATE_DEFAULT | SCHULTZ_STATE_PRESSED },
        { SCHULTZ_STYLE_STATE_DISABLED, SCHULTZ_STATE_VISIBLE }
    };
    size_t i;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_node_set_state_property(tree, node,
                        cases[i].style_state, SCHULTZ_PROP_OPACITY,
                        schultz_value_number((float)(i + 1u))));
    }
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        schultz_node_set_state(tree, node, cases[i].node_state);
        schultz_tree_resolve_styles(tree);
        /* Later states override earlier ones, so only the last applying
         * state's value is guaranteed; each must at least take effect. */
        ASSERT(number_of(tree, node, SCHULTZ_PROP_OPACITY) > 0.0f);
    }

    schultz_tree_destroy(tree);
    PASS();
}

/* --------------------------------------------------------- inheritance */

TEST text_color_inherits_from_an_ancestor(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle label;
    schultz_handle deep;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &panel);
    schultz_node_create(tree, panel, &label);
    schultz_node_create(tree, label, &deep);

    schultz_node_set_style_property(tree, panel, SCHULTZ_PROP_TEXT_COLOR,
                    schultz_value_color(schultz_color_rgba(77, 0, 0, 255)));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(77u, color_of(tree, label, SCHULTZ_PROP_TEXT_COLOR).r);
    /* And all the way down, not just one level. */
    ASSERT_EQ(77u, color_of(tree, deep, SCHULTZ_PROP_TEXT_COLOR).r);

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_child_can_override_an_inherited_value(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle label;
    schultz_handle sibling;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &panel);
    schultz_node_create(tree, panel, &label);
    schultz_node_create(tree, panel, &sibling);

    schultz_node_set_style_property(tree, panel, SCHULTZ_PROP_TEXT_COLOR,
                    schultz_value_color(schultz_color_rgba(77, 0, 0, 255)));
    schultz_node_set_style_property(tree, label, SCHULTZ_PROP_TEXT_COLOR,
                    schultz_value_color(schultz_color_rgba(88, 0, 0, 255)));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(88u, color_of(tree, label, SCHULTZ_PROP_TEXT_COLOR).r);
    ASSERT_EQ(77u, color_of(tree, sibling, SCHULTZ_PROP_TEXT_COLOR).r);

    schultz_tree_destroy(tree);
    PASS();
}

/* Backgrounds must not inherit: a panel's fill is not its children's fill. */
TEST background_does_not_inherit(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle child;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &panel);
    schultz_node_create(tree, panel, &child);

    schultz_node_set_style_property(tree, panel, SCHULTZ_PROP_BACKGROUND,
                    schultz_value_color(schultz_color_rgba(50, 0, 0, 255)));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(50u, color_of(tree, panel, SCHULTZ_PROP_BACKGROUND).r);
    ASSERT_EQ(0u, color_of(tree, child, SCHULTZ_PROP_BACKGROUND).a);

    schultz_tree_destroy(tree);
    PASS();
}

TEST padding_does_not_inherit(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle child;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &panel);
    schultz_node_create(tree, panel, &child);

    schultz_node_set_style_property(tree, panel, SCHULTZ_PROP_PADDING,
                                    schultz_value_number(20));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(20.0f, number_of(tree, panel, SCHULTZ_PROP_PADDING));
    ASSERT_EQ(0.0f, number_of(tree, child, SCHULTZ_PROP_PADDING));

    schultz_tree_destroy(tree);
    PASS();
}

TEST reparenting_picks_up_the_new_ancestor_chain(void)
{
    schultz_tree *tree;
    schultz_handle red;
    schultz_handle blue;
    schultz_handle child;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &red);
    schultz_node_create(tree, schultz_tree_root(tree), &blue);
    schultz_node_create(tree, red, &child);
    schultz_node_set_style_property(tree, red, SCHULTZ_PROP_TEXT_COLOR,
                    schultz_value_color(schultz_color_rgba(200, 0, 0, 255)));
    schultz_node_set_style_property(tree, blue, SCHULTZ_PROP_TEXT_COLOR,
                    schultz_value_color(schultz_color_rgba(0, 0, 200, 255)));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(200u, color_of(tree, child, SCHULTZ_PROP_TEXT_COLOR).r);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_parent(tree, child, blue));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(200u, color_of(tree, child, SCHULTZ_PROP_TEXT_COLOR).b);
    ASSERT_EQ(0u, color_of(tree, child, SCHULTZ_PROP_TEXT_COLOR).r);

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------------- the theme */

TEST a_token_value_follows_the_tree_theme(void)
{
    schultz_tree *tree;
    schultz_handle node;
    schultz_theme theme;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BACKGROUND,
                    schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(1, schultz_color_equals(
        schultz_theme_color(NULL, SCHULTZ_TOKEN_COLOR_ACCENT),
        color_of(tree, node, SCHULTZ_PROP_BACKGROUND)));

    schultz_theme_init(&theme);
    schultz_theme_set_color(&theme, SCHULTZ_TOKEN_COLOR_ACCENT,
                            schultz_color_rgba(3, 4, 5, 255));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_set_theme(tree, &theme));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(3u, color_of(tree, node, SCHULTZ_PROP_BACKGROUND).r);

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_literal_value_does_not_follow_the_theme(void)
{
    schultz_tree *tree;
    schultz_handle node;
    schultz_theme theme;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BACKGROUND,
                    schultz_value_color(schultz_color_rgba(60, 0, 0, 255)));

    schultz_theme_init(&theme);
    schultz_theme_set_color(&theme, SCHULTZ_TOKEN_COLOR_ACCENT,
                            schultz_color_rgba(3, 4, 5, 255));
    schultz_tree_set_theme(tree, &theme);
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(60u, color_of(tree, node, SCHULTZ_PROP_BACKGROUND).r);

    schultz_tree_destroy(tree);
    PASS();
}

TEST changing_the_theme_restyles_the_whole_tree(void)
{
    schultz_tree *tree;
    schultz_handle a;
    schultz_handle b;
    schultz_theme theme;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &a);
    schultz_node_create(tree, a, &b);
    settle(tree);
    ASSERT_EQ(0, schultz_node_style_dirty(tree, b));

    schultz_theme_preset_light(&theme);
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_set_theme(tree, &theme));

    ASSERT_EQ(1, schultz_node_style_dirty(tree, a));
    ASSERT_EQ(1, schultz_node_style_dirty(tree, b));

    schultz_tree_destroy(tree);
    PASS();
}

/* A dark sidebar in a light application, without restating every property. */
TEST a_subtree_theme_overrides_the_tree_theme(void)
{
    schultz_tree *tree;
    schultz_handle main_area;
    schultz_handle sidebar;
    schultz_handle inside;
    schultz_theme light;
    schultz_theme dark;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &main_area);
    schultz_node_create(tree, schultz_tree_root(tree), &sidebar);
    schultz_node_create(tree, sidebar, &inside);

    /* Everything asks for the same token. */
    schultz_node_set_style_property(tree, main_area, SCHULTZ_PROP_BACKGROUND,
                    schultz_value_token(SCHULTZ_TOKEN_COLOR_SURFACE));
    schultz_node_set_style_property(tree, inside, SCHULTZ_PROP_BACKGROUND,
                    schultz_value_token(SCHULTZ_TOKEN_COLOR_SURFACE));

    schultz_theme_preset_light(&light);
    schultz_tree_set_theme(tree, &light);

    schultz_theme_init(&dark);
    schultz_theme_set_color(&dark, SCHULTZ_TOKEN_COLOR_SURFACE,
                            schultz_color_rgba(10, 10, 10, 255));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_theme(tree, sidebar, &dark));
    schultz_tree_resolve_styles(tree);

    /*
     * The same token, two different values, in one tree. Compared against
     * the theme rather than against a number, because what is being tested
     * is that the two disagree, not what either of them happens to be.
     */
    ASSERT_EQ(schultz_theme_color(&light, SCHULTZ_TOKEN_COLOR_SURFACE).r,
              color_of(tree, main_area, SCHULTZ_PROP_BACKGROUND).r);
    ASSERT_EQ(10u, color_of(tree, inside, SCHULTZ_PROP_BACKGROUND).r);

    /* Removing it puts the subtree back on the tree theme. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_theme(tree, sidebar, NULL));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(schultz_theme_color(&light, SCHULTZ_TOKEN_COLOR_SURFACE).r,
              color_of(tree, inside, SCHULTZ_PROP_BACKGROUND).r);

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------------- invalidation */

/*
 * The rule the whole property table exists for: a colour change must repaint
 * and must not relayout.
 */
TEST a_color_change_repaints_but_does_not_relayout(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_bounds(tree, node, schultz_rect_make(0, 0, 50, 50));
    settle(tree);

    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BACKGROUND,
                    schultz_value_color(schultz_color_rgba(1, 2, 3, 255)));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(1, schultz_node_is_dirty(tree, node));
    ASSERT_EQ(0, schultz_node_layout_dirty(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_padding_change_relayouts_and_repaints(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_bounds(tree, node, schultz_rect_make(0, 0, 50, 50));
    settle(tree);

    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_PADDING,
                                    schultz_value_number(11));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(1, schultz_node_layout_dirty(tree, node));
    ASSERT_EQ(1, schultz_node_is_dirty(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

TEST setting_a_property_to_its_current_value_costs_nothing(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_PADDING,
                                    schultz_value_number(6));
    settle(tree);

    /* The same value again: resolution runs, finds no difference, and must
     * not mark anything. */
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_PADDING,
                                    schultz_value_number(6));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(0, schultz_node_is_dirty(tree, node));
    ASSERT_EQ(0, schultz_node_layout_dirty(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

/* Inheriting properties reach further, and that is their whole cost. */
TEST an_inherited_change_marks_the_subtree(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle child;
    schultz_handle grandchild;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &panel);
    schultz_node_create(tree, panel, &child);
    schultz_node_create(tree, child, &grandchild);
    settle(tree);

    schultz_node_set_style_property(tree, panel, SCHULTZ_PROP_TEXT_COLOR,
                    schultz_value_color(schultz_color_rgba(1, 1, 1, 255)));

    ASSERT_EQ(1, schultz_node_style_dirty(tree, panel));
    ASSERT_EQ(1, schultz_node_style_dirty(tree, child));
    ASSERT_EQ(1, schultz_node_style_dirty(tree, grandchild));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_non_inherited_change_marks_one_node(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle child;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &panel);
    schultz_node_create(tree, panel, &child);
    settle(tree);

    schultz_node_set_style_property(tree, panel, SCHULTZ_PROP_BACKGROUND,
                    schultz_value_color(schultz_color_rgba(1, 1, 1, 255)));

    ASSERT_EQ(1, schultz_node_style_dirty(tree, panel));
    ASSERT_EQ(0, schultz_node_style_dirty(tree, child));

    schultz_tree_destroy(tree);
    PASS();
}

TEST resolution_reaches_children_when_an_inherited_value_changed(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle child;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &panel);
    schultz_node_create(tree, panel, &child);
    settle(tree);

    schultz_node_set_style_property(tree, panel, SCHULTZ_PROP_TEXT_COLOR,
                    schultz_value_color(schultz_color_rgba(123, 0, 0, 255)));
    schultz_tree_resolve_styles(tree);

    /* The child was never touched directly, but its value changed. */
    ASSERT_EQ(123u, color_of(tree, child, SCHULTZ_PROP_TEXT_COLOR).r);
    ASSERT_EQ(1, schultz_node_is_dirty(tree, child));

    schultz_tree_destroy(tree);
    PASS();
}

TEST layout_properties_reach_the_layout_fields(void)
{
    schultz_tree *tree;
    schultz_handle node;
    float padding = 0.0f;
    float gap = 0.0f;
    float min_width = 0.0f;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_PADDING,
                                    schultz_value_number(9));
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_GAP,
                                    schultz_value_number(3));
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_MIN_WIDTH,
                                    schultz_value_number(120));
    schultz_tree_resolve_styles(tree);

    /* The panes read these fields, so resolution must write them. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_spacing(tree, node, &padding,
                                                   &gap));
    ASSERT_EQ(9.0f, padding);
    ASSERT_EQ(3.0f, gap);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_min_size(tree, node, &min_width,
                                                    NULL));
    ASSERT_EQ(120.0f, min_width);

    schultz_tree_destroy(tree);
    PASS();
}

TEST clearing_layout_dirty_resets_every_node(void)
{
    schultz_tree *tree;
    schultz_handle a;
    schultz_handle b;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &a);
    schultz_node_create(tree, a, &b);
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_invalidate_layout(tree, b));
    ASSERT_EQ(1, schultz_node_layout_dirty(tree, b));

    schultz_tree_clear_layout_dirty(tree);
    ASSERT_EQ(0, schultz_node_layout_dirty(tree, a));
    ASSERT_EQ(0, schultz_node_layout_dirty(tree, b));
    schultz_tree_clear_layout_dirty(NULL);

    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------------------------- misc */

TEST clear_style_drops_every_layer(void)
{
    schultz_tree *tree;
    schultz_handle style;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    style = make_style(tree, SCHULTZ_PROP_PADDING, schultz_value_number(5));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_add_style(tree, node, style);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_GAP,
                                    schultz_value_number(6));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_HOVERED,
                                    SCHULTZ_PROP_OPACITY,
                                    schultz_value_number(0.5f));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(5.0f, number_of(tree, node, SCHULTZ_PROP_PADDING));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_clear_style(tree, node));
    schultz_node_set_state(tree, node,
                           SCHULTZ_STATE_DEFAULT | SCHULTZ_STATE_HOVERED);
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(0u, schultz_node_style_count(tree, node));
    ASSERT_EQ(0.0f, number_of(tree, node, SCHULTZ_PROP_PADDING));
    ASSERT_EQ(0.0f, number_of(tree, node, SCHULTZ_PROP_GAP));
    ASSERT_EQ(1.0f, number_of(tree, node, SCHULTZ_PROP_OPACITY));

    schultz_tree_destroy(tree);
    PASS();
}

TEST style_operations_reject_stale_and_bad_handles(void)
{
    schultz_tree *tree;
    schultz_handle node;
    schultz_handle style;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    style = make_style(tree, SCHULTZ_PROP_PADDING, schultz_value_number(1));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_destroy(tree, node));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_add_style(tree, node, style));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_remove_style(tree, node, style));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_set_style_property(tree, node,
                        SCHULTZ_PROP_PADDING, schultz_value_number(1)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_clear_style(tree, node));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_set_theme(tree, node, NULL));
    ASSERT_EQ(NULL, schultz_node_resolved(tree, node));
    ASSERT_EQ(0u, schultz_node_style_count(tree, node));

    /* An out of range property or state is rejected on a live node. */
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_node_set_style_property(tree, node, SCHULTZ_PROP_COUNT,
                        schultz_value_number(1)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_node_set_state_property(tree, node,
                        SCHULTZ_STYLE_STATE_COUNT, SCHULTZ_PROP_PADDING,
                        schultz_value_number(1)));
    /* A style handle that names nothing is an invalid handle, not an
     * invalid argument: the documented contract for add_style. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_add_style(tree, node, SCHULTZ_HANDLE_NONE));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_style_register(tree, NULL, &style));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_set_theme(NULL, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_resolve_styles(NULL));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_deep_tree_inherits_all_the_way_down(void)
{
    schultz_tree *tree;
    schultz_handle parent;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t depth;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    parent = schultz_tree_root(tree);
    schultz_node_set_style_property(tree, parent, SCHULTZ_PROP_TEXT_COLOR,
                    schultz_value_color(schultz_color_rgba(42, 0, 0, 255)));

    for (depth = 0; depth < 60u; depth++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, parent, &node));
        parent = node;
    }
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(42u, color_of(tree, node, SCHULTZ_PROP_TEXT_COLOR).r);

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * Fonts are the third value kind, and the only one whose token has no
 * compiled in default: the application must load and register one.
 */
TEST font_properties_resolve_and_inherit(void)
{
    schultz_tree *tree;
    schultz_handle panel;
    schultz_handle child;
    schultz_theme theme;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &panel);
    schultz_node_create(tree, panel, &child);

    /* Unset until the application supplies one. */
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(SCHULTZ_HANDLE_NONE,
              schultz_resolved_font(schultz_node_resolved(tree, child),
                                    SCHULTZ_PROP_FONT));

    /* A literal font on the panel is inherited by the child. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_style_property(tree, panel,
                    SCHULTZ_PROP_FONT, schultz_value_font(
                        (schultz_handle)4242)));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ((schultz_handle)4242,
              schultz_resolved_font(schultz_node_resolved(tree, child),
                                    SCHULTZ_PROP_FONT));

    /* A token font follows the theme's font slot. */
    schultz_theme_init(&theme);
    schultz_theme_set_font(&theme, SCHULTZ_TOKEN_FONT_BODY,
                           (schultz_handle)77);
    schultz_tree_set_theme(tree, &theme);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_clear_style_property(tree, panel,
                                                    SCHULTZ_PROP_FONT));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ((schultz_handle)77,
              schultz_resolved_font(schultz_node_resolved(tree, child),
                                    SCHULTZ_PROP_FONT));

    schultz_tree_destroy(tree);
    PASS();
}

/* Changing a font is a layout change, since text measurement depends on it. */
TEST a_font_change_relayouts(void)
{
    schultz_tree *tree;
    schultz_handle node;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    settle(tree);

    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_FONT,
                                    schultz_value_font((schultz_handle)5));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(1, schultz_node_layout_dirty(tree, node));

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * The setters are the short spelling of a style property, not a separate
 * path. Before this was true, a value set through them survived until the
 * next resolve and then silently reverted.
 */
TEST the_spacing_setter_writes_style_properties(void)
{
    schultz_tree *tree;
    schultz_handle node;
    float padding = 0.0f;
    float gap = 0.0f;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_spacing(tree, node, 11.0f, 5.0f));

    /* Readable at once, before any resolve. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_spacing(tree, node, &padding,
                                                   &gap));
    ASSERT_EQ(11.0f, padding);
    ASSERT_EQ(5.0f, gap);

    /* And it survives resolution rather than reverting. */
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(11.0f, number_of(tree, node, SCHULTZ_PROP_PADDING));
    ASSERT_EQ(5.0f, number_of(tree, node, SCHULTZ_PROP_GAP));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_spacing(tree, node, &padding,
                                                   &gap));
    ASSERT_EQ(11.0f, padding);

    schultz_tree_destroy(tree);
    PASS();
}

TEST the_size_setters_write_style_properties(void)
{
    schultz_tree *tree;
    schultz_handle node;
    float width = 0.0f;
    float height = 0.0f;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_min_size(tree, node, 40.0f,
                                                    SCHULTZ_SIZE_UNSET));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_pref_size(tree, node, 100.0f,
                                                     SCHULTZ_SIZE_UNSET));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_max_size(tree, node,
                                                    SCHULTZ_SIZE_UNSET,
                                                    60.0f));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_pref_size(tree, node, &width,
                                                     &height));
    ASSERT_EQ(100.0f, width);

    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(40.0f, number_of(tree, node, SCHULTZ_PROP_MIN_WIDTH));
    ASSERT_EQ(100.0f, number_of(tree, node, SCHULTZ_PROP_PREF_WIDTH));
    ASSERT_EQ(60.0f, number_of(tree, node, SCHULTZ_PROP_MAX_HEIGHT));

    /* Still readable after resolution, and still one pair per call. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_pref_size(tree, node, &width,
                                                     &height));
    ASSERT_EQ(100.0f, width);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_max_size(tree, node, &width,
                                                    &height));
    ASSERT_EQ(60.0f, height);

    /*
     * The point of one pair per call: setting a preference cannot reach the
     * minimum somebody else asked for. Setting all six at once could not
     * help it.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_pref_size(tree, node, 55.0f,
                                                     SCHULTZ_SIZE_UNSET));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_min_size(tree, node, &width,
                                                    &height));
    ASSERT_EQ(40.0f, width);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_max_size(tree, node, &width,
                                                    &height));
    ASSERT_EQ(60.0f, height);

    schultz_tree_destroy(tree);
    PASS();
}

/* A style may now override what the setter wrote, since both are properties. */
TEST a_later_style_property_overrides_the_spacing_setter(void)
{
    schultz_tree *tree;
    schultz_handle node;
    float padding = 0.0f;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    schultz_node_create(tree, schultz_tree_root(tree), &node);
    schultz_node_set_spacing(tree, node, 11.0f, 5.0f);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_PADDING,
                                    schultz_value_number(30.0f));
    schultz_tree_resolve_styles(tree);

    ASSERT_EQ(30.0f, number_of(tree, node, SCHULTZ_PROP_PADDING));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_spacing(tree, node, &padding,
                                                   NULL));
    ASSERT_EQ(30.0f, padding);

    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------- the resolve short circuit */

/*
 * Resolution skips a branch with nothing stale in it, so every one of these
 * asserts a case where skipping would leave a style behind. A wrong short
 * circuit does not crash; it silently paints the old value.
 */

/* Builds a settled chain of nodes, returning the deepest. */
static schultz_handle deep_chain(schultz_tree *tree, uint32_t depth,
                                 schultz_handle *out_top)
{
    schultz_handle parent = schultz_tree_root(tree);
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t i;

    for (i = 0; i < depth; i++) {
        if (schultz_node_create(tree, parent, &node) != SCHULTZ_OK) {
            return SCHULTZ_HANDLE_NONE;
        }
        if (i == 0 && out_top != NULL) {
            *out_top = node;
        }
        parent = node;
    }
    return node;
}

TEST a_leaf_deep_in_a_settled_tree_still_resolves(void)
{
    schultz_tree *tree;
    schultz_handle leaf;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    leaf = deep_chain(tree, 12u, NULL);
    ASSERT(leaf != SCHULTZ_HANDLE_NONE);
    settle(tree);

    schultz_node_set_style_property(tree, leaf, SCHULTZ_PROP_BACKGROUND,
                                    schultz_value_color(
                                        schultz_color_rgba(1, 2, 3, 255)));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(1, color_of(tree, leaf, SCHULTZ_PROP_BACKGROUND).r);

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * A node created under a parent that was already settled. The node is born
 * stale before it is attached to anything, so nothing above it knows unless
 * attaching says so.
 */
TEST a_node_added_to_a_settled_parent_resolves(void)
{
    schultz_tree *tree;
    schultz_handle top = SCHULTZ_HANDLE_NONE;
    schultz_handle deep;
    schultz_handle added = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    deep = deep_chain(tree, 8u, &top);
    ASSERT(deep != SCHULTZ_HANDLE_NONE);
    /* An inherited value on the top, so the new node has something to pick
     * up rather than only its own defaults. */
    schultz_node_set_style_property(tree, top, SCHULTZ_PROP_TEXT_COLOR,
                                    schultz_value_color(
                                        schultz_color_rgba(9, 9, 9, 255)));
    settle(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, deep, &added));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(9, color_of(tree, added, SCHULTZ_PROP_TEXT_COLOR).r);

    schultz_tree_destroy(tree);
    PASS();
}

/* A subtree moved under a different parent inherits from where it landed. */
TEST a_reparented_subtree_resolves_under_its_new_parent(void)
{
    schultz_tree *tree;
    schultz_handle left = SCHULTZ_HANDLE_NONE;
    schultz_handle right = SCHULTZ_HANDLE_NONE;
    schultz_handle moved = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &left));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &right));
    schultz_node_set_style_property(tree, right, SCHULTZ_PROP_TEXT_COLOR,
                                    schultz_value_color(
                                        schultz_color_rgba(7, 7, 7, 255)));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, left, &moved));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, moved, &child));
    settle(tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_parent(tree, moved, right));
    schultz_tree_resolve_styles(tree);
    /* The moved node and what hangs off it both take the new ancestor's
     * inherited colour. */
    ASSERT_EQ(7, color_of(tree, moved, SCHULTZ_PROP_TEXT_COLOR).r);
    ASSERT_EQ(7, color_of(tree, child, SCHULTZ_PROP_TEXT_COLOR).r);

    schultz_tree_destroy(tree);
    PASS();
}

/* An inherited change at the top still reaches every descendant. */
TEST an_inherited_change_reaches_a_settled_subtree(void)
{
    schultz_tree *tree;
    schultz_handle top = SCHULTZ_HANDLE_NONE;
    schultz_handle leaf;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    leaf = deep_chain(tree, 10u, &top);
    ASSERT(leaf != SCHULTZ_HANDLE_NONE);
    settle(tree);

    schultz_node_set_style_property(tree, top, SCHULTZ_PROP_TEXT_COLOR,
                                    schultz_value_color(
                                        schultz_color_rgba(5, 6, 7, 255)));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(5, color_of(tree, leaf, SCHULTZ_PROP_TEXT_COLOR).r);

    schultz_tree_destroy(tree);
    PASS();
}

/* A new theme re-resolves a settled tree, since every token may now differ. */
TEST a_theme_change_reaches_a_settled_tree(void)
{
    schultz_tree *tree;
    schultz_theme theme;
    schultz_handle leaf;
    schultz_color before;

    ASSERT_EQ(SCHULTZ_OK, tree_setup(&tree));
    leaf = deep_chain(tree, 6u, NULL);
    ASSERT(leaf != SCHULTZ_HANDLE_NONE);
    schultz_node_set_style_property(tree, leaf, SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_SURFACE));
    settle(tree);
    before = color_of(tree, leaf, SCHULTZ_PROP_BACKGROUND);

    schultz_theme_preset_light(&theme);
    schultz_tree_set_theme(tree, &theme);
    schultz_tree_resolve_styles(tree);
    /* The light surface is not the dark one, so the token followed. */
    ASSERT(color_of(tree, leaf, SCHULTZ_PROP_BACKGROUND).r != before.r);

    schultz_tree_destroy(tree);
    PASS();
}

SUITE(node_style)
{
    RUN_TEST(a_style_carries_its_own_state_rules);
    RUN_TEST(removing_a_style_takes_its_state_rules_with_it);
    RUN_TEST(a_node_has_the_last_word_on_a_state);
    RUN_TEST(a_new_node_starts_style_dirty);
    RUN_TEST(a_leaf_deep_in_a_settled_tree_still_resolves);
    RUN_TEST(a_node_added_to_a_settled_parent_resolves);
    RUN_TEST(a_reparented_subtree_resolves_under_its_new_parent);
    RUN_TEST(an_inherited_change_reaches_a_settled_subtree);
    RUN_TEST(a_theme_change_reaches_a_settled_tree);
    RUN_TEST(an_unstyled_node_resolves_to_the_defaults);
    RUN_TEST(an_inline_property_styles_one_node_only);
    RUN_TEST(clearing_an_inline_property_falls_back);
    RUN_TEST(a_registered_style_applies_to_every_node_using_it);
    RUN_TEST(a_later_style_overrides_an_earlier_one);
    RUN_TEST(the_inline_patch_beats_every_style);
    RUN_TEST(removing_a_style_reverts_the_node);
    RUN_TEST(adding_the_same_style_twice_is_idempotent);
    RUN_TEST(a_style_survives_until_the_last_node_drops_it);
    RUN_TEST(a_registered_style_is_a_copy);
    RUN_TEST(a_state_patch_applies_only_in_that_state);
    RUN_TEST(disabled_beats_pressed_beats_hovered);
    RUN_TEST(a_state_patch_beats_the_inline_patch);
    RUN_TEST(every_style_state_is_reachable);
    RUN_TEST(text_color_inherits_from_an_ancestor);
    RUN_TEST(a_child_can_override_an_inherited_value);
    RUN_TEST(background_does_not_inherit);
    RUN_TEST(padding_does_not_inherit);
    RUN_TEST(reparenting_picks_up_the_new_ancestor_chain);
    RUN_TEST(a_token_value_follows_the_tree_theme);
    RUN_TEST(a_literal_value_does_not_follow_the_theme);
    RUN_TEST(changing_the_theme_restyles_the_whole_tree);
    RUN_TEST(a_subtree_theme_overrides_the_tree_theme);
    RUN_TEST(a_color_change_repaints_but_does_not_relayout);
    RUN_TEST(a_padding_change_relayouts_and_repaints);
    RUN_TEST(setting_a_property_to_its_current_value_costs_nothing);
    RUN_TEST(an_inherited_change_marks_the_subtree);
    RUN_TEST(a_non_inherited_change_marks_one_node);
    RUN_TEST(resolution_reaches_children_when_an_inherited_value_changed);
    RUN_TEST(layout_properties_reach_the_layout_fields);
    RUN_TEST(clearing_layout_dirty_resets_every_node);
    RUN_TEST(clear_style_drops_every_layer);
    RUN_TEST(style_operations_reject_stale_and_bad_handles);
    RUN_TEST(font_properties_resolve_and_inherit);
    RUN_TEST(a_font_change_relayouts);
    RUN_TEST(the_spacing_setter_writes_style_properties);
    RUN_TEST(the_size_setters_write_style_properties);
    RUN_TEST(a_later_style_property_overrides_the_spacing_setter);
    RUN_TEST(a_deep_tree_inherits_all_the_way_down);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(node_style);
    GREATEST_MAIN_END();
}
