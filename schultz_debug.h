/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_debug.h
 * @brief Visual debugging aids.
 *
 * Internal header.
 *
 * Invalidation bugs are invisible by construction: too little marking leaves
 * stale pixels that look plausible, and too much marking looks perfectly
 * correct while quietly repainting the whole window every frame. Neither
 * shows up in a screenshot. Tinting the region that is actually being
 * repainted makes both failures obvious at a glance, which is why the roadmap
 * asks for this before the third widget exists rather than after the first
 * bug report.
 *
 * **Not part of the host facing interface.** A development aid. A host binds
 * to schultz_api.h; this header is the toolkit's own and may change without
 * notice.
 */

#ifndef SCHULTZ_DEBUG_H
#define SCHULTZ_DEBUG_H

#include "schultz_node.h"
#include "schultz_paint.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Appends a tinted overlay marking the tree's dirty region.
 *
 * Draws a translucent fill plus a solid outline over the area that will be
 * repainted this frame. Call it last, after the scene, so the tint sits on
 * top of what it describes.
 *
 * Nothing is appended when the tree is clean.
 *
 * @param list  The draw list to append to. Must not be NULL.
 * @param tree  The tree whose dirty region should be shown. Must not be NULL.
 * @param color The tint. Its alpha is used for the fill; the outline is drawn
 *              at full opacity so a very thin region is still visible.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when either pointer is
 *         NULL, or an append error.
 */
int32_t schultz_debug_tint_dirty(schultz_draw_list *list,
                                 const schultz_tree *tree,
                                 schultz_color color);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_DEBUG_H */
