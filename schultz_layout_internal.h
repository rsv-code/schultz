/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_layout_internal.h
 * @brief The size a node may take, as the layout engine reads it.
 *
 * Internal to the layout engine and the node store. A host names each of
 * these one at a time, through schultz_node_set_min_size and the two calls
 * beside it; measuring wants all six together, and that is what this is for.
 *
 * It is not part of the interface on purpose. A call that took all six at
 * once could not set one without being handed the other five, so every
 * caller read them back first, and a caller that did not silently replaced
 * settings it had never heard of.
 */

#ifndef SCHULTZ_LAYOUT_INTERNAL_H
#define SCHULTZ_LAYOUT_INTERNAL_H

#include "schultz_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Minimum, preferred and maximum size along both axes.
 *
 * A preferred size of a negative value means "ask the pane", which is the
 * usual case: a container derives its preferred size from its children. A
 * maximum of a negative value means no maximum.
 *
 * Preferred is a wish; minimum and maximum are rules. A preferred size says
 * how big a node would like to be when nothing else decides, and a pane is
 * entitled to decide otherwise: a box pane stretches its children across the
 * cross axis by default. A minimum and a maximum bind wherever a node is
 * placed, not only where it is measured.
 */
typedef struct {
    float min_width;   /**< Never smaller than this. Zero for no minimum. */
    float pref_width;  /**< Wanted width, or negative to compute it. */
    float max_width;   /**< Never larger than this, or negative for none. */
    float min_height;  /**< Never smaller than this. */
    float pref_height; /**< Wanted height, or negative to compute it. */
    float max_height;  /**< Never larger than this, or negative for none. */
} schultz_size_hints;

/**
 * @brief Fills in permissive defaults: no minimum, computed preferred size,
 *        no maximum.
 *
 * @param out_hints Receives the defaults. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when out_hints is NULL.
 */
int32_t schultz_size_hints_default(schultz_size_hints *out_hints);

/**
 * @brief Reads every size a node may take, in one go.
 *
 * What measuring needs, and the reason this header exists.
 *
 * @param tree      The tree holding the node. Must not be NULL.
 * @param node      The node to query.
 * @param out_hints Receives the sizes. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when out_hints is NULL, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_size_hints(const schultz_tree *tree, schultz_handle node,
                                schultz_size_hints *out_hints);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_LAYOUT_INTERNAL_H */
