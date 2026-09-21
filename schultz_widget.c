/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_widget.c
 * @brief The widget interface and the paint walk.
 */

#include "schultz_widget.h"

#include <stddef.h>

/*
 * The node's widget fields are private to schultz_node.c, so the accessors
 * live there and this file works through them. That keeps one owner for the
 * node struct rather than two files reaching into it.
 */

int32_t schultz_widget_paint_tree(schultz_tree *tree, schultz_draw_list *list,
                                  schultz_arena *arena, schultz_rect dirty);

/* Recursive half of the walk, so the entry point can validate once. */
static int32_t schultz_widget_paint_node(schultz_tree *tree,
                                         schultz_handle node,
                                         schultz_draw_list *list,
                                         schultz_arena *arena,
                                         const schultz_rect *dirty,
                                         uint32_t parts,
                                         int32_t cull)
{
    const schultz_widget_vtable *widget;
    schultz_rect bounds;
    uint32_t count;
    uint32_t i;
    int32_t clips;
    int32_t grouped;
    float   opacity;
    schultz_shadow shadow;
    const schultz_resolved_style *style;
    int32_t result;

    if (!(schultz_node_get_state(tree, node) & SCHULTZ_STATE_VISIBLE)) {
        return SCHULTZ_OK; /* a hidden subtree draws nothing at all */
    }
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }

    /*
     * Culling. A node whose bounds miss the dirty rectangle emits nothing,
     * and neither does anything beneath it, which is what keeps a small
     * change from rebuilding the whole list.
     *
     * A node with empty bounds is not culled: a container that has not been
     * given a size may still hold children that have been.
     *
     * The test is against the bounds plus whatever the widget paints outside
     * them, so a control whose focus ring is the only thing inside the dirty
     * region still repaints. The clip below uses the bounds themselves.
     */
    if (cull && !schultz_rect_is_empty(bounds)) {
        schultz_rect reach = schultz_rect_expand(
            bounds, schultz_node_paint_margin(tree, node));
        uint32_t part;
        int32_t touches = 0;

        for (part = 0; part < parts; part++) {
            if (!schultz_rect_is_empty(schultz_rect_intersect(reach,
                                                              dirty[part]))) {
                touches = 1;
                break;
            }
        }
        if (!touches) {
            return SCHULTZ_OK;
        }
    }

    /*
     * Opacity below one, or a shadow, makes this node and everything under
     * it one picture. Faded once, Fading each piece separately is not the same thing: two
     * half transparent shapes that overlap come out darker where they meet,
     * because the same pixel is blended twice. A whole card can be faded by
     * putting one number on it, which is the point of the property.
     *
     * It wraps the clip rather than sitting inside it, so the group is the
     * outermost thing a node does.
     */
    style   = schultz_node_resolved(tree, node);
    opacity = schultz_resolved_number(style, SCHULTZ_PROP_OPACITY);
    shadow.color    = schultz_resolved_color(style, SCHULTZ_PROP_SHADOW_COLOR);
    shadow.angle    = schultz_resolved_number(style,
                                              SCHULTZ_PROP_SHADOW_ANGLE);
    shadow.distance = schultz_resolved_number(style,
                                              SCHULTZ_PROP_SHADOW_DISTANCE);
    shadow.blur     = schultz_resolved_number(style, SCHULTZ_PROP_SHADOW_BLUR);

    /* A shadow needs the group for the same reason an opacity does: the
     * subtree has to be one picture before it can cast one shadow. */
    grouped = (opacity < 1.0f) || (shadow.color.a > 0u);
    if (grouped) {
        result = schultz_draw_group_begin(list, opacity, shadow);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }

    clips = schultz_node_clips_children(tree, node);
    if (clips) {
        result = schultz_draw_clip_begin(list, bounds);
        if (result != SCHULTZ_OK) {
            if (grouped) {
                schultz_draw_group_end(list);
            }
            return result;
        }
    }

    widget = schultz_node_widget(tree, node);
    if (widget != NULL && widget->paint != NULL) {
        result = widget->paint(tree, node, list, arena);
        if (result != SCHULTZ_OK) {
            if (clips) {
                schultz_draw_clip_end(list);
            }
            if (grouped) {
                schultz_draw_group_end(list);
            }
            return result;
        }
    }

    /* Children after the parent, in order, so later ones paint on top. */
    count = schultz_node_child_count(tree, node);
    for (i = 0; i < count; i++) {
        schultz_handle child;
        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK) {
            continue;
        }
        result = schultz_widget_paint_node(tree, child, list, arena, dirty,
                                           parts, cull);
        if (result != SCHULTZ_OK) {
            if (clips) {
                schultz_draw_clip_end(list);
            }
            if (grouped) {
                schultz_draw_group_end(list);
            }
            return result;
        }
    }

    if (clips) {
        result = schultz_draw_clip_end(list);
        if (result != SCHULTZ_OK) {
            if (grouped) {
                schultz_draw_group_end(list);
            }
            return result;
        }
    }
    if (grouped) {
        return schultz_draw_group_end(list);
    }
    return SCHULTZ_OK;
}

int32_t schultz_widget_paint_tree(schultz_tree *tree, schultz_draw_list *list,
                                  schultz_arena *arena, schultz_rect dirty)
{
    if (tree == NULL || list == NULL || arena == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return schultz_widget_paint_subtree(tree, schultz_tree_root(tree), list,
                                        arena, dirty);
}

int32_t schultz_widget_paint_tree_parts(schultz_tree *tree,
                                        schultz_draw_list *list,
                                        schultz_arena *arena,
                                        const schultz_rect *dirty,
                                        uint32_t parts)
{
    schultz_rect widened[SCHULTZ_TREE_DIRTY_PARTS];
    uint32_t i;

    if (tree == NULL || list == NULL || arena == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (dirty == NULL || parts == 0u) {
        /* Nothing named means everything, as it has always meant. */
        return schultz_widget_paint_node(tree, schultz_tree_root(tree), list,
                                         arena, NULL, 0u, 0);
    }
    if (parts > SCHULTZ_TREE_DIRTY_PARTS) {
        parts = SCHULTZ_TREE_DIRTY_PARTS;
    }
    /*
     * What actually gets repainted is a little larger than the rectangles
     * asked for: each is rounded out to whole pixels with a pixel of slack,
     * because antialiased coverage reaches past a shape's geometry. Culling
     * has to use those same widened rectangles. A node lying entirely inside
     * the slack would otherwise be skipped while the pixels it covers are
     * painted over by whatever is behind it, which erases it.
     */
    for (i = 0; i < parts; i++) {
        widened[i] = schultz_rect_pixel_bounds(dirty[i], schultz_paint_slack());
    }
    return schultz_widget_paint_node(tree, schultz_tree_root(tree), list,
                                     arena, widened, parts, 1);
}

int32_t schultz_widget_paint_subtree(schultz_tree *tree, schultz_handle node,
                                     schultz_draw_list *list,
                                     schultz_arena *arena, schultz_rect dirty)
{
    schultz_rect widened;

    if (tree == NULL || list == NULL || arena == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /* An empty dirty rectangle means paint everything. */
    if (schultz_rect_is_empty(dirty)) {
        return schultz_widget_paint_node(tree, node, list, arena, NULL, 0u, 0);
    }
    widened = schultz_rect_pixel_bounds(dirty, schultz_paint_slack());
    return schultz_widget_paint_node(tree, node, list, arena, &widened, 1u, 1);
}
