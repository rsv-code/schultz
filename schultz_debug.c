/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_debug.c
 * @brief Visual debugging aids.
 */

#include "schultz_debug.h"

int32_t schultz_debug_tint_dirty(schultz_draw_list *list,
                                 const schultz_tree *tree,
                                 schultz_color color)
{
    schultz_rect region;
    schultz_color outline;
    int32_t result;

    if (list == NULL || tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    result = schultz_tree_dirty_region(tree, &region);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (schultz_rect_is_empty(region)) {
        return SCHULTZ_OK;
    }

    result = schultz_draw_fill_rect(list, region, schultz_paint_solid(color));
    if (result != SCHULTZ_OK) {
        return result;
    }

    /*
     * The outline is opaque so a one pixel tall region is still visible,
     * which a translucent fill alone would not be.
     */
    outline = schultz_color_rgba(color.r, color.g, color.b, 255);
    return schultz_draw_stroke_rect(list, region, schultz_stroke_solid(outline, 2.0f));
}
