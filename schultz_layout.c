/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_layout.c
 * @brief The layout protocol and the built in panes.
 *
 * Every pane here follows the same shape. Measure asks each child what it
 * wants, given whatever the pane can offer it, and combines those answers.
 * Arrange divides the rectangle it was given and hands each child a final
 * rectangle, recursing through schultz_layout_arrange.
 *
 * No pane allocates. A pane that needs per child working values keeps them on
 * the child, or recomputes them, so a layout pass cannot fail for want of
 * memory and cannot trigger a collection on the C host.
 */

#include "schultz_layout.h"

#include "schultz_layout_internal.h"

#include <stddef.h>

#include "schultz_node.h"

int32_t schultz_layout_is_unbounded(float available)
{
    return (available < 0.0f) ? 1 : 0;
}

int32_t schultz_layout_params_default(schultz_layout_params *out_params)
{
    if (out_params == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    out_params->grow        = SCHULTZ_GROW_NEVER;
    out_params->align       = SCHULTZ_ALIGN_STRETCH;
    out_params->row         = 0;
    out_params->column      = 0;
    out_params->row_span    = 1;
    out_params->column_span = 1;
    out_params->slot        = SCHULTZ_SLOT_CENTER;
    out_params->x           = 0.0f;
    out_params->y           = 0.0f;
    return SCHULTZ_OK;
}

int32_t schultz_size_hints_default(schultz_size_hints *out_hints)
{
    if (out_hints == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    out_hints->min_width   = 0.0f;
    out_hints->pref_width  = -1.0f; /* compute it */
    out_hints->max_width   = -1.0f; /* no maximum */
    out_hints->min_height  = 0.0f;
    out_hints->pref_height = -1.0f;
    out_hints->max_height  = -1.0f;
    return SCHULTZ_OK;
}

/* ---------------------------------------------------------------- helpers */

/* Clamps one axis against a minimum and an optional maximum. */
static float schultz_layout_clamp(float value, float minimum, float maximum)
{
    if (maximum >= 0.0f && value > maximum) {
        value = maximum;
    }
    if (value < minimum) {
        value = minimum;
    }
    return (value > 0.0f) ? value : 0.0f;
}

/* Subtracts padding from an available size, leaving unbounded unbounded. */
static float schultz_layout_inset(float available, float amount)
{
    if (schultz_layout_is_unbounded(available)) {
        return available;
    }
    available -= amount;
    return (available > 0.0f) ? available : 0.0f;
}

/** @brief The bounds a node put on one axis: never below, never above. */
typedef struct {
    float least; /**< Smallest it may be. Zero when nothing was said. */
    float most;  /**< Largest it may be, or negative for no limit. */
} schultz_layout_limits;

/* What a node said its width may never go below or above. */
static schultz_layout_limits schultz_layout_width_limits(schultz_tree *tree,
                                                         schultz_handle node)
{
    schultz_layout_limits limits;
    schultz_size_hints hints;

    if (schultz_node_size_hints(tree, node, &hints) != SCHULTZ_OK) {
        limits.least = 0.0f;
        limits.most  = -1.0f;
        return limits;
    }
    limits.least = hints.min_width;
    limits.most  = hints.max_width;
    return limits;
}

/* The same about a height. */
static schultz_layout_limits schultz_layout_height_limits(schultz_tree *tree,
                                                          schultz_handle node)
{
    schultz_layout_limits limits;
    schultz_size_hints hints;

    if (schultz_node_size_hints(tree, node, &hints) != SCHULTZ_OK) {
        limits.least = 0.0f;
        limits.most  = -1.0f;
        return limits;
    }
    limits.least = hints.min_height;
    limits.most  = hints.max_height;
    return limits;
}

/*
 * Places a child across the cross axis according to its alignment.
 *
 * A minimum and a maximum bind here as well as at measure. Measuring is where
 * a node says how big it would like to be, and clamping there is enough while
 * the answer is what gets used; stretching does not use it. A child told
 * never to exceed two hundred, in a column six hundred wide, came out six
 * hundred, and nothing reported it. A maximum a pane may quietly exceed is
 * not a maximum.
 *
 * Preferred size is deliberately left alone. It says what a node would like
 * when nothing else decides, and a stretching parent is something else
 * deciding; a node that means "never wider than this" has min and max to say
 * so, and now they work.
 *
 * The offset is worked out after the clamping, so a centred child that was
 * cut down to its maximum is still centred in what it was offered.
 */
static void schultz_layout_align(uint32_t align, float available,
                                 float natural, schultz_layout_limits limits,
                                 float *out_offset, float *out_size)
{
    float size;

    switch (align) {
    case SCHULTZ_ALIGN_START:
    case SCHULTZ_ALIGN_CENTER:
    case SCHULTZ_ALIGN_END:
        size = (natural < available) ? natural : available;
        break;
    case SCHULTZ_ALIGN_STRETCH:
    default:
        size = available;
        break;
    }

    size = schultz_layout_clamp(size, limits.least, limits.most);

    switch (align) {
    case SCHULTZ_ALIGN_CENTER:
        *out_offset = (available - size) * 0.5f;
        break;
    case SCHULTZ_ALIGN_END:
        *out_offset = available - size;
        break;
    case SCHULTZ_ALIGN_START:
    case SCHULTZ_ALIGN_STRETCH:
    default:
        *out_offset = 0.0f;
        break;
    }
    if (*out_offset < 0.0f) {
        *out_offset = 0.0f;
    }
    *out_size = size;
}

/* Skips children that are not visible: they take part in no layout. */
static int32_t schultz_layout_counts(schultz_tree *tree, schultz_handle node,
                                     uint32_t *out_visible)
{
    uint32_t count = schultz_node_child_count(tree, node);
    uint32_t visible = 0;
    uint32_t i;

    for (i = 0; i < count; i++) {
        schultz_handle child;
        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK) {
            continue;
        }
        if (schultz_node_get_state(tree, child) & SCHULTZ_STATE_VISIBLE) {
            visible++;
        }
    }
    *out_visible = visible;
    return SCHULTZ_OK;
}

/*
 * Whether a pane should place this child at all.
 *
 * Every pane in this file asks before it measures or arranges anything, so
 * this is the one place the rule lives.
 *
 * Hidden children are the obvious case. The others are the nodes something
 * else already places, which a pane must leave where they are:
 *
 *   - Overlays. A dialog, a menu and a popover are children of the root, and
 *     they are placed against the window or against whatever they were opened
 *     from, not by whatever pane happens to be above them. Without this a
 *     pane on the root arranges a dialog into the flow, where it stops
 *     covering the window and stops answering, and the only rule preventing
 *     that was that the root must not have a pane, which is not a rule
 *     anything enforced or that a host could guess.
 *
 *   - Anything marked with schultz_node_set_places_itself. A toast is the
 *     case in the toolkit: it sits against an edge of the window and stacks
 *     with the other toasts, and it is not an overlay, because it never takes
 *     input and it comes and goes in whatever order its timers run out rather
 *     than in the stack order the overlay list keeps. A host can mark its own
 *     nodes the same way.
 *
 * Two questions rather than one, because the answers are kept in two places:
 * an overlay is a row in a list on the tree, and this is a flag on the node.
 */
static int32_t schultz_layout_child_visible(schultz_tree *tree,
                                            schultz_handle child)
{
    uint32_t overlays = schultz_tree_overlay_count(tree);
    uint32_t i;

    if (!(schultz_node_get_state(tree, child) & SCHULTZ_STATE_VISIBLE)) {
        return 0;
    }
    if (schultz_node_places_itself(tree, child)) {
        return 0;
    }
    for (i = 0; i < overlays; i++) {
        schultz_handle at = SCHULTZ_HANDLE_NONE;

        if (schultz_tree_overlay_at(tree, i, &at, NULL) == SCHULTZ_OK &&
            at == child) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------- callback */

int32_t schultz_layout_measure(schultz_tree *tree, schultz_handle node,
                               float avail_w, float avail_h,
                               schultz_size *out_size)
{
    const schultz_pane_vtable *pane;
    schultz_size_hints hints;
    schultz_size size;
    int32_t result;

    if (tree == NULL || out_size == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_size_hints(tree, node, &hints);
    if (result != SCHULTZ_OK) {
        return result;
    }

    size = schultz_size_make(0.0f, 0.0f);
    pane = schultz_node_get_pane(tree, node);

    if (hints.pref_width >= 0.0f && hints.pref_height >= 0.0f) {
        /* Fully specified: no need to consult the pane at all. */
        size = schultz_size_make(hints.pref_width, hints.pref_height);
    } else {
        /*
         * A width that is already decided is the width to measure against.
         * Height often depends on it: a paragraph asked how tall it is has
         * to be told how wide it will be, or it reports one long line and
         * then wraps into five when it is painted, overlapping whatever was
         * placed beneath it. The same holds for a fixed height and a width
         * that follows from it.
         */
        if (hints.pref_width >= 0.0f) {
            avail_w = hints.pref_width;
        }
        if (hints.pref_height >= 0.0f) {
            avail_h = hints.pref_height;
        }
        /* A pane with no measure reports nothing, the same as no pane. */
        if (pane != NULL && pane->measure != NULL) {
            result = pane->measure(tree, node, avail_w, avail_h, &size);
            if (result != SCHULTZ_OK) {
                return result;
            }
        }
        /* An explicit preferred size overrides what the pane computed. */
        if (hints.pref_width >= 0.0f) {
            size.width = hints.pref_width;
        }
        if (hints.pref_height >= 0.0f) {
            size.height = hints.pref_height;
        }
    }

    size.width  = schultz_layout_clamp(size.width, hints.min_width,
                                       hints.max_width);
    size.height = schultz_layout_clamp(size.height, hints.min_height,
                                       hints.max_height);
    *out_size = size;
    return SCHULTZ_OK;
}

int32_t schultz_layout_arrange(schultz_tree *tree, schultz_handle node,
                               schultz_rect rect)
{
    const schultz_pane_vtable *pane;
    int32_t result;

    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_set_bounds(tree, node, rect);
    if (result != SCHULTZ_OK) {
        return result;
    }

    pane = schultz_node_get_pane(tree, node);
    /*
     * A pane with no arrange places nothing, which is what a pane that only
     * exists to report a size wants. Treated the same as no pane at all
     * rather than called, because a designated initialiser leaves a slot it
     * does not mention NULL and calling that is a crash with no clue in it.
     */
    if (pane == NULL || pane->arrange == NULL) {
        return SCHULTZ_OK; /* a leaf places nothing */
    }
    return pane->arrange(tree, node, rect);
}


/* ------------------------------------------------------------- FlowPane
 *
 * Children run across in rows and start a new row when the next one will not
 * fit. That makes its height depend on the width it is offered, which is the
 * case the measure signature exists for and the same shape as wrapping text.
 */

/*
 * Walks the children a row at a time, calling back once per row with where
 * that row starts, how many children are on it and how tall it is. Measuring
 * and arranging are the same walk with different work per row, so they share
 * it rather than each getting the wrapping subtly wrong.
 */
/** @brief Told about one finished row: where it starts, how big it is. */
typedef void (*schultz_flow_row_fn)(void *context, uint32_t first,
                                    uint32_t last, float width, float height,
                                    float y);

static void schultz_flow_walk(schultz_tree *tree, schultz_handle node,
                              float inner_w, float inner_h, float gap,
                              schultz_flow_row_fn row_fn, void *context)
{
    uint32_t count = schultz_node_child_count(tree, node);
    uint32_t first = 0;
    float row_w = 0.0f;
    float row_h = 0.0f;
    float y = 0.0f;
    uint32_t on_row = 0;
    uint32_t i;

    for (i = 0; i < count; i++) {
        schultz_handle child;
        schultz_size size;
        float step;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_layout_child_visible(tree, child)) {
            continue;
        }
        /*
         * Measured against the row's full width rather than what is left of
         * it. A child that wraps starts a new row, where it has the whole
         * width, so measuring it against the remainder would size it for a
         * place it is not going to end up.
         */
        if (schultz_layout_measure(tree, child, inner_w, inner_h, &size)
                != SCHULTZ_OK) {
            continue;
        }
        step = (on_row == 0u) ? size.width : gap + size.width;

        if (on_row > 0u && !schultz_layout_is_unbounded(inner_w) &&
            row_w + step > inner_w) {
            /* It does not fit, so the row ends here and a new one begins. */
            row_fn(context, first, i, row_w, row_h, y);
            y += row_h + gap;
            first  = i;
            row_w  = size.width;
            row_h  = size.height;
            on_row = 1u;
            continue;
        }

        row_w += step;
        if (size.height > row_h) {
            row_h = size.height;
        }
        on_row++;
    }

    if (on_row > 0u) {
        row_fn(context, first, count, row_w, row_h, y);
    }
}

/** What measuring a flow pane accumulates as the rows go by. */
typedef struct {
    float widest; /**< The widest row, which is the pane's natural width. */
    float bottom; /**< Where the last row ends, which is its height. */
} schultz_flow_extent;

static void schultz_flow_extent_row(void *context, uint32_t first,
                                    uint32_t last, float width, float height,
                                    float y)
{
    schultz_flow_extent *extent = (schultz_flow_extent *)context;

    (void)first;
    (void)last;
    if (width > extent->widest) {
        extent->widest = width;
    }
    extent->bottom = y + height;
}

static int32_t schultz_flow_measure(schultz_tree *tree, schultz_handle node,
                                    float avail_w, float avail_h,
                                    schultz_size *out_size)
{
    schultz_flow_extent extent = { 0.0f, 0.0f };
    float padding = 0.0f;
    float gap = 0.0f;

    schultz_node_get_spacing(tree, node, &padding, &gap);
    schultz_flow_walk(tree, node,
                      schultz_layout_inset(avail_w, padding * 2.0f),
                      schultz_layout_inset(avail_h, padding * 2.0f), gap,
                      schultz_flow_extent_row, &extent);

    *out_size = schultz_size_make(extent.widest + padding * 2.0f,
                                  extent.bottom + padding * 2.0f);
    return SCHULTZ_OK;
}

/** What arranging a flow pane needs to hand each row. */
typedef struct {
    schultz_tree  *tree;    /**< The tree being arranged. */
    schultz_handle node;    /**< The flow pane itself. */
    float          padding; /**< Inset from the pane's own edges. */
    float          gap;     /**< Between children, and between rows. */
    float          inner_w; /**< Width a row has to fill. */
    float          inner_h; /**< Height children are measured against. */
} schultz_flow_place;

static void schultz_flow_place_row(void *context, uint32_t first,
                                   uint32_t last, float width, float height,
                                   float y)
{
    schultz_flow_place *place = (schultz_flow_place *)context;
    float x = place->padding;
    uint32_t i;

    (void)width;
    for (i = first; i < last; i++) {
        schultz_handle child;
        schultz_size size;
        schultz_layout_params params;
        float offset;
        float used;

        if (schultz_node_child_at(place->tree, place->node, i, &child)
                != SCHULTZ_OK ||
            !schultz_layout_child_visible(place->tree, child)) {
            continue;
        }
        if (schultz_layout_measure(place->tree, child, place->inner_w,
                                   place->inner_h, &size) != SCHULTZ_OK) {
            continue;
        }
        if (schultz_node_get_layout_params(place->tree, child, &params)
                != SCHULTZ_OK) {
            schultz_layout_params_default(&params);
        }
        /*
         * A child is as wide as it asked to be and is aligned within the
         * row's height, which is what makes a row of mixed heights line up
         * along its middle or its baseline edge rather than all stretching.
         */
        schultz_layout_align(params.align, height, size.height,
                             schultz_layout_height_limits(place->tree, child),
                             &offset, &used);
        schultz_layout_arrange(place->tree, child,
            schultz_rect_make(x, place->padding + y + offset, size.width,
                              used));
        x += size.width + place->gap;
    }
}

static int32_t schultz_flow_arrange(schultz_tree *tree, schultz_handle node,
                                    schultz_rect rect)
{
    schultz_flow_place place;
    float padding = 0.0f;
    float gap = 0.0f;

    schultz_node_get_spacing(tree, node, &padding, &gap);

    place.tree    = tree;
    place.node    = node;
    place.padding = padding;
    place.gap     = gap;
    place.inner_w = rect.width - padding * 2.0f;
    place.inner_h = rect.height - padding * 2.0f;
    if (place.inner_w < 0.0f) { place.inner_w = 0.0f; }
    if (place.inner_h < 0.0f) { place.inner_h = 0.0f; }

    schultz_flow_walk(tree, node, place.inner_w, place.inner_h, gap,
                      schultz_flow_place_row, &place);
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_flow_vtable = {
    schultz_flow_measure, schultz_flow_arrange
};

const schultz_pane_vtable *schultz_pane_flow(void)
{
    return &schultz_flow_vtable;
}

/* ------------------------------------------------------------ GridPane
 *
 * Sizing runs in three steps on each axis, and both axes use the same code
 * with a flag saying which one is being worked on.
 *
 *   1. Fixed tracks take their value. Content tracks take the largest
 *      preferred size among the single track children in them.
 *   2. A child spanning several tracks that does not fit grows those tracks
 *      evenly, so a wide cell widens its columns rather than overflowing.
 *   3. Whatever is left over is shared among the weighted tracks in
 *      proportion to their weights. With unbounded space there is nothing to
 *      share, so weighted tracks stay at their content size.
 *
 * Nothing here allocates: the per track working arrays are fixed size, capped
 * at SCHULTZ_GRID_MAX_TRACKS.
 */

/* Number of tracks on one axis, defaulting to one when none are defined. */
static uint32_t schultz_grid_count(schultz_tree *tree, schultz_handle node,
                                   int32_t rows)
{
    uint32_t count = 0;
    schultz_node_get_grid_tracks(tree, node, rows, &count);
    return (count == 0u) ? 1u : count;
}

/*
 * Computes the size of every track on one axis. `available` is the space
 * inside the padding and gaps, or negative when unbounded.
 */
static void schultz_grid_size_tracks(schultz_tree *tree, schultz_handle node,
                                     int32_t rows, float available,
                                     float cross_available,
                                     float *sizes, uint32_t count)
{
    const schultz_grid_track *tracks;
    uint32_t track_count = 0;
    uint32_t child_count = schultz_node_child_count(tree, node);
    float used = 0.0f;
    float total_weight = 0.0f;
    uint32_t i;

    tracks = schultz_node_get_grid_tracks(tree, node, rows, &track_count);

    for (i = 0; i < count; i++) {
        sizes[i] = 0.0f;
        if (tracks != NULL && i < track_count &&
            tracks[i].kind == SCHULTZ_TRACK_FIXED) {
            sizes[i] = (tracks[i].value > 0.0f) ? tracks[i].value : 0.0f;
        }
    }

    /* Step 1: single track children set the content tracks. */
    for (i = 0; i < child_count; i++) {
        schultz_handle child;
        schultz_layout_params params;
        schultz_size size;
        uint32_t index;
        uint32_t span;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_layout_child_visible(tree, child) ||
            schultz_node_get_layout_params(tree, child, &params)
                != SCHULTZ_OK) {
            continue;
        }
        index = rows ? params.row : params.column;
        span  = rows ? params.row_span : params.column_span;
        if (span < 1u) { span = 1u; }
        if (index >= count || span != 1u) {
            continue;
        }
        if (tracks != NULL && index < track_count &&
            tracks[index].kind != SCHULTZ_TRACK_CONTENT) {
            continue;
        }
        if (schultz_layout_measure(tree, child,
                                   rows ? cross_available : -1.0f,
                                   rows ? -1.0f : cross_available,
                                   &size) != SCHULTZ_OK) {
            continue;
        }
        {
            float want = rows ? size.height : size.width;
            if (want > sizes[index]) {
                sizes[index] = want;
            }
        }
    }

    /* Step 2: spanning children widen their tracks if they do not fit. */
    for (i = 0; i < child_count; i++) {
        schultz_handle child;
        schultz_layout_params params;
        schultz_size size;
        uint32_t index;
        uint32_t span;
        uint32_t t;
        float covered = 0.0f;
        float want;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_layout_child_visible(tree, child) ||
            schultz_node_get_layout_params(tree, child, &params)
                != SCHULTZ_OK) {
            continue;
        }
        index = rows ? params.row : params.column;
        span  = rows ? params.row_span : params.column_span;
        if (span < 1u) { span = 1u; }
        if (span == 1u || index >= count) {
            continue;
        }
        if (index + span > count) {
            span = count - index;
        }
        if (schultz_layout_measure(tree, child,
                                   rows ? cross_available : -1.0f,
                                   rows ? -1.0f : cross_available,
                                   &size) != SCHULTZ_OK) {
            continue;
        }
        for (t = 0; t < span; t++) {
            covered += sizes[index + t];
        }
        want = rows ? size.height : size.width;
        if (want > covered) {
            float extra = (want - covered) / (float)span;
            for (t = 0; t < span; t++) {
                sizes[index + t] += extra;
            }
        }
    }

    /* Step 3: weighted tracks share what is left. */
    if (schultz_layout_is_unbounded(available)) {
        return;
    }
    for (i = 0; i < count; i++) {
        used += sizes[i];
        if (tracks != NULL && i < track_count &&
            tracks[i].kind == SCHULTZ_TRACK_WEIGHTED &&
            tracks[i].value > 0.0f) {
            total_weight += tracks[i].value;
        }
    }
    if (total_weight <= 0.0f || available <= used) {
        return;
    }
    {
        float spare = available - used;
        for (i = 0; i < count; i++) {
            if (tracks != NULL && i < track_count &&
                tracks[i].kind == SCHULTZ_TRACK_WEIGHTED &&
                tracks[i].value > 0.0f) {
                sizes[i] += spare * (tracks[i].value / total_weight);
            }
        }
    }
}

static float schultz_grid_sum(const float *sizes, uint32_t count, float gap)
{
    float total = 0.0f;
    uint32_t i;

    for (i = 0; i < count; i++) {
        total += sizes[i];
    }
    if (count > 1u) {
        total += gap * (float)(count - 1u);
    }
    return total;
}

static int32_t schultz_grid_measure(schultz_tree *tree, schultz_handle node,
                                    float avail_w, float avail_h,
                                    schultz_size *out_size)
{
    float columns[SCHULTZ_GRID_MAX_TRACKS];
    float rows[SCHULTZ_GRID_MAX_TRACKS];
    uint32_t column_count = schultz_grid_count(tree, node, 0);
    uint32_t row_count = schultz_grid_count(tree, node, 1);
    float padding = 0.0f;
    float gap = 0.0f;
    float inner_w;
    float inner_h;

    schultz_node_get_spacing(tree, node, &padding, &gap);
    inner_w = schultz_layout_inset(avail_w, padding * 2.0f);
    inner_h = schultz_layout_inset(avail_h, padding * 2.0f);

    schultz_grid_size_tracks(tree, node, 0, -1.0f, inner_h, columns,
                             column_count);
    schultz_grid_size_tracks(tree, node, 1, -1.0f, inner_w, rows, row_count);

    *out_size = schultz_size_make(
        schultz_grid_sum(columns, column_count, gap) + padding * 2.0f,
        schultz_grid_sum(rows, row_count, gap) + padding * 2.0f);
    return SCHULTZ_OK;
}

static int32_t schultz_grid_arrange(schultz_tree *tree, schultz_handle node,
                                    schultz_rect rect)
{
    float columns[SCHULTZ_GRID_MAX_TRACKS];
    float rows[SCHULTZ_GRID_MAX_TRACKS];
    float column_offsets[SCHULTZ_GRID_MAX_TRACKS];
    float row_offsets[SCHULTZ_GRID_MAX_TRACKS];
    uint32_t column_count = schultz_grid_count(tree, node, 0);
    uint32_t row_count = schultz_grid_count(tree, node, 1);
    uint32_t child_count = schultz_node_child_count(tree, node);
    float padding = 0.0f;
    float gap = 0.0f;
    float inner_w;
    float inner_h;
    float cursor;
    uint32_t i;

    schultz_node_get_spacing(tree, node, &padding, &gap);
    inner_w = rect.width - padding * 2.0f;
    inner_h = rect.height - padding * 2.0f;
    if (inner_w < 0.0f) { inner_w = 0.0f; }
    if (inner_h < 0.0f) { inner_h = 0.0f; }

    /* Gaps are not available to the tracks themselves. */
    schultz_grid_size_tracks(tree, node, 0,
                             inner_w - gap * (float)(column_count > 1u
                                                     ? column_count - 1u : 0u),
                             inner_h, columns, column_count);
    schultz_grid_size_tracks(tree, node, 1,
                             inner_h - gap * (float)(row_count > 1u
                                                     ? row_count - 1u : 0u),
                             inner_w, rows, row_count);

    cursor = padding;
    for (i = 0; i < column_count; i++) {
        column_offsets[i] = cursor;
        cursor += columns[i] + gap;
    }
    cursor = padding;
    for (i = 0; i < row_count; i++) {
        row_offsets[i] = cursor;
        cursor += rows[i] + gap;
    }

    for (i = 0; i < child_count; i++) {
        schultz_handle child;
        schultz_layout_params params;
        schultz_size size;
        uint32_t column;
        uint32_t row;
        uint32_t column_span;
        uint32_t row_span;
        float cell_w = 0.0f;
        float cell_h = 0.0f;
        float x_offset;
        float y_offset;
        float w;
        float h;
        uint32_t t;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_layout_child_visible(tree, child) ||
            schultz_node_get_layout_params(tree, child, &params)
                != SCHULTZ_OK) {
            continue;
        }

        column = params.column;
        row    = params.row;
        if (column >= column_count || row >= row_count) {
            continue; /* outside the grid: not placed */
        }
        column_span = (params.column_span < 1u) ? 1u : params.column_span;
        row_span    = (params.row_span < 1u) ? 1u : params.row_span;
        if (column + column_span > column_count) {
            column_span = column_count - column;
        }
        if (row + row_span > row_count) {
            row_span = row_count - row;
        }

        for (t = 0; t < column_span; t++) {
            cell_w += columns[column + t];
        }
        cell_w += gap * (float)(column_span - 1u);
        for (t = 0; t < row_span; t++) {
            cell_h += rows[row + t];
        }
        cell_h += gap * (float)(row_span - 1u);

        if (schultz_layout_measure(tree, child, cell_w, cell_h, &size)
                != SCHULTZ_OK) {
            continue;
        }
        schultz_layout_align(params.align, cell_w, size.width,
                             schultz_layout_width_limits(tree, child),
                             &x_offset, &w);
        schultz_layout_align(params.align, cell_h, size.height,
                             schultz_layout_height_limits(tree, child),
                             &y_offset, &h);

        schultz_layout_arrange(tree, child,
                               schultz_rect_make(column_offsets[column] +
                                                     x_offset,
                                                 row_offsets[row] + y_offset,
                                                 w, h));
    }
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_grid_vtable = {
    schultz_grid_measure, schultz_grid_arrange
};

const schultz_pane_vtable *schultz_pane_grid(void)
{
    return &schultz_grid_vtable;
}

/* ------------------------------------------------------------ box panes
 *
 * VBox and HBox are the same algorithm with the axes swapped, so both are
 * written once against a "main" and "cross" axis and the caller says which is
 * which.
 */

static int32_t schultz_box_measure(schultz_tree *tree, schultz_handle node,
                                   float avail_w, float avail_h,
                                   schultz_size *out_size, int32_t vertical)
{
    uint32_t count = schultz_node_child_count(tree, node);
    uint32_t visible = 0;
    float padding = 0.0f;
    float gap = 0.0f;
    float main_total = 0.0f;
    float cross_max = 0.0f;
    float child_w;
    float child_h;
    uint32_t i;

    schultz_node_get_spacing(tree, node, &padding, &gap);
    schultz_layout_counts(tree, node, &visible);

    child_w = schultz_layout_inset(avail_w, padding * 2.0f);
    child_h = schultz_layout_inset(avail_h, padding * 2.0f);

    for (i = 0; i < count; i++) {
        schultz_handle child;
        schultz_size child_size;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_layout_child_visible(tree, child)) {
            continue;
        }
        /*
         * The main axis is offered unbounded space: a child should report
         * what it naturally wants, and the box decides afterwards how much it
         * actually gets. The cross axis is offered what the box has, which is
         * what lets wrapping text measure correctly inside a column.
         */
        if (schultz_layout_measure(tree, child,
                                   vertical ? child_w : -1.0f,
                                   vertical ? -1.0f : child_h,
                                   &child_size) != SCHULTZ_OK) {
            continue;
        }

        if (vertical) {
            main_total += child_size.height;
            if (child_size.width > cross_max) {
                cross_max = child_size.width;
            }
        } else {
            main_total += child_size.width;
            if (child_size.height > cross_max) {
                cross_max = child_size.height;
            }
        }
    }

    if (visible > 1u) {
        main_total += gap * (float)(visible - 1u);
    }
    main_total += padding * 2.0f;
    cross_max  += padding * 2.0f;

    *out_size = vertical ? schultz_size_make(cross_max, main_total)
                         : schultz_size_make(main_total, cross_max);
    return SCHULTZ_OK;
}

static int32_t schultz_box_arrange(schultz_tree *tree, schultz_handle node,
                                   schultz_rect rect, int32_t vertical)
{
    uint32_t count = schultz_node_child_count(tree, node);
    float padding = 0.0f;
    float gap = 0.0f;
    float inner_main;
    float inner_cross;
    float used = 0.0f;
    uint32_t visible = 0;
    uint32_t growers = 0;
    uint32_t top_priority = SCHULTZ_GROW_NEVER;
    float share = 0.0f;
    float cursor;
    uint32_t i;

    schultz_node_get_spacing(tree, node, &padding, &gap);

    inner_main  = (vertical ? rect.height : rect.width) - padding * 2.0f;
    inner_cross = (vertical ? rect.width : rect.height) - padding * 2.0f;
    if (inner_main < 0.0f)  { inner_main = 0.0f; }
    if (inner_cross < 0.0f) { inner_cross = 0.0f; }

    /*
     * First pass: measure every visible child at its natural main axis size,
     * and find the highest grow priority present. Leftover space is shared
     * only among children at that priority, which is the coarse three level
     * model rather than proportional weights.
     */
    for (i = 0; i < count; i++) {
        schultz_handle child;
        schultz_size size;
        schultz_layout_params params;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_layout_child_visible(tree, child)) {
            continue;
        }
        if (schultz_layout_measure(tree, child,
                                   vertical ? inner_cross : -1.0f,
                                   vertical ? -1.0f : inner_cross,
                                   &size) != SCHULTZ_OK) {
            continue;
        }
        used += vertical ? size.height : size.width;
        visible++;

        if (schultz_node_get_layout_params(tree, child, &params)
                == SCHULTZ_OK) {
            if (params.grow > top_priority) {
                top_priority = params.grow;
                growers = 0;
            }
            if (params.grow == top_priority &&
                top_priority != SCHULTZ_GROW_NEVER) {
                growers++;
            }
        }
    }

    if (visible > 1u) {
        used += gap * (float)(visible - 1u);
    }

    if (growers > 0u && inner_main > used) {
        share = (inner_main - used) / (float)growers;
    }

    /* Second pass: place them. */
    cursor = padding;
    for (i = 0; i < count; i++) {
        schultz_handle child;
        schultz_size size;
        schultz_layout_params params;
        float main_size;
        float cross_offset;
        float cross_size;
        schultz_rect child_rect;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_layout_child_visible(tree, child)) {
            continue;
        }
        if (schultz_layout_measure(tree, child,
                                   vertical ? inner_cross : -1.0f,
                                   vertical ? -1.0f : inner_cross,
                                   &size) != SCHULTZ_OK) {
            continue;
        }
        if (schultz_node_get_layout_params(tree, child, &params)
                != SCHULTZ_OK) {
            schultz_layout_params_default(&params);
        }

        main_size = vertical ? size.height : size.width;
        if (params.grow == top_priority &&
            top_priority != SCHULTZ_GROW_NEVER) {
            main_size += share;
        }

        schultz_layout_align(params.align, inner_cross,
                             vertical ? size.width : size.height,
                             vertical
                                 ? schultz_layout_width_limits(tree, child)
                                 : schultz_layout_height_limits(tree, child),
                             &cross_offset, &cross_size);

        if (vertical) {
            child_rect = schultz_rect_make(padding + cross_offset, cursor,
                                           cross_size, main_size);
        } else {
            child_rect = schultz_rect_make(cursor, padding + cross_offset,
                                           main_size, cross_size);
        }

        schultz_layout_arrange(tree, child, child_rect);
        cursor += main_size + gap;
    }
    return SCHULTZ_OK;
}

static int32_t schultz_vbox_measure(schultz_tree *tree, schultz_handle node,
                                    float avail_w, float avail_h,
                                    schultz_size *out_size)
{
    return schultz_box_measure(tree, node, avail_w, avail_h, out_size, 1);
}

static int32_t schultz_vbox_arrange(schultz_tree *tree, schultz_handle node,
                                    schultz_rect rect)
{
    return schultz_box_arrange(tree, node, rect, 1);
}

static int32_t schultz_hbox_measure(schultz_tree *tree, schultz_handle node,
                                    float avail_w, float avail_h,
                                    schultz_size *out_size)
{
    return schultz_box_measure(tree, node, avail_w, avail_h, out_size, 0);
}

static int32_t schultz_hbox_arrange(schultz_tree *tree, schultz_handle node,
                                    schultz_rect rect)
{
    return schultz_box_arrange(tree, node, rect, 0);
}

static const schultz_pane_vtable schultz_vbox_vtable = {
    schultz_vbox_measure, schultz_vbox_arrange
};

static const schultz_pane_vtable schultz_hbox_vtable = {
    schultz_hbox_measure, schultz_hbox_arrange
};

const schultz_pane_vtable *schultz_pane_vbox(void)
{
    return &schultz_vbox_vtable;
}

const schultz_pane_vtable *schultz_pane_hbox(void)
{
    return &schultz_hbox_vtable;
}

/* ------------------------------------------------------------ StackPane */

static int32_t schultz_stack_measure(schultz_tree *tree, schultz_handle node,
                                     float avail_w, float avail_h,
                                     schultz_size *out_size)
{
    uint32_t count = schultz_node_child_count(tree, node);
    float padding = 0.0f;
    float widest = 0.0f;
    float tallest = 0.0f;
    uint32_t i;

    schultz_node_get_spacing(tree, node, &padding, NULL);
    avail_w = schultz_layout_inset(avail_w, padding * 2.0f);
    avail_h = schultz_layout_inset(avail_h, padding * 2.0f);

    for (i = 0; i < count; i++) {
        schultz_handle child;
        schultz_size size;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_layout_child_visible(tree, child)) {
            continue;
        }
        if (schultz_layout_measure(tree, child, avail_w, avail_h, &size)
                != SCHULTZ_OK) {
            continue;
        }
        if (size.width > widest)   { widest = size.width; }
        if (size.height > tallest) { tallest = size.height; }
    }

    *out_size = schultz_size_make(widest + padding * 2.0f,
                                  tallest + padding * 2.0f);
    return SCHULTZ_OK;
}

static int32_t schultz_stack_arrange(schultz_tree *tree, schultz_handle node,
                                     schultz_rect rect)
{
    uint32_t count = schultz_node_child_count(tree, node);
    float padding = 0.0f;
    float inner_w;
    float inner_h;
    uint32_t i;

    schultz_node_get_spacing(tree, node, &padding, NULL);
    inner_w = rect.width - padding * 2.0f;
    inner_h = rect.height - padding * 2.0f;
    if (inner_w < 0.0f) { inner_w = 0.0f; }
    if (inner_h < 0.0f) { inner_h = 0.0f; }

    for (i = 0; i < count; i++) {
        schultz_handle child;
        schultz_size size;
        schultz_layout_params params;
        float x_offset;
        float y_offset;
        float w;
        float h;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_layout_child_visible(tree, child)) {
            continue;
        }
        if (schultz_layout_measure(tree, child, inner_w, inner_h, &size)
                != SCHULTZ_OK) {
            continue;
        }
        if (schultz_node_get_layout_params(tree, child, &params)
                != SCHULTZ_OK) {
            schultz_layout_params_default(&params);
        }

        /* Every child gets the same rectangle, aligned within it. */
        schultz_layout_align(params.align, inner_w, size.width,
                             schultz_layout_width_limits(tree, child),
                             &x_offset, &w);
        schultz_layout_align(params.align, inner_h, size.height,
                             schultz_layout_height_limits(tree, child),
                             &y_offset, &h);

        schultz_layout_arrange(tree, child,
                               schultz_rect_make(padding + x_offset,
                                                 padding + y_offset, w, h));
    }
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_stack_vtable = {
    schultz_stack_measure, schultz_stack_arrange
};

const schultz_pane_vtable *schultz_pane_stack(void)
{
    return &schultz_stack_vtable;
}

/* -------------------------------------------------------- absolute Pane */

static int32_t schultz_absolute_measure(schultz_tree *tree,
                                        schultz_handle node, float avail_w,
                                        float avail_h,
                                        schultz_size *out_size)
{
    uint32_t count = schultz_node_child_count(tree, node);
    float right = 0.0f;
    float bottom = 0.0f;
    uint32_t i;

    for (i = 0; i < count; i++) {
        schultz_handle child;
        schultz_size size;
        schultz_layout_params params;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_layout_child_visible(tree, child)) {
            continue;
        }
        if (schultz_layout_measure(tree, child, avail_w, avail_h, &size)
                != SCHULTZ_OK) {
            continue;
        }
        if (schultz_node_get_layout_params(tree, child, &params)
                != SCHULTZ_OK) {
            continue;
        }
        /* The union of the child extents, so the pane still reports a size. */
        if (params.x + size.width > right)   { right = params.x + size.width; }
        if (params.y + size.height > bottom) { bottom = params.y +
                                                        size.height; }
    }

    *out_size = schultz_size_make(right, bottom);
    return SCHULTZ_OK;
}

static int32_t schultz_absolute_arrange(schultz_tree *tree,
                                        schultz_handle node,
                                        schultz_rect rect)
{
    uint32_t count = schultz_node_child_count(tree, node);
    uint32_t i;

    for (i = 0; i < count; i++) {
        schultz_handle child;
        schultz_size size;
        schultz_layout_params params;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_layout_child_visible(tree, child)) {
            continue;
        }
        if (schultz_layout_measure(tree, child, rect.width, rect.height,
                                   &size) != SCHULTZ_OK) {
            continue;
        }
        if (schultz_node_get_layout_params(tree, child, &params)
                != SCHULTZ_OK) {
            schultz_layout_params_default(&params);
        }
        /*
         * Placed exactly where asked, at its preferred size. A child may
         * overflow the pane; clipping is a separate flag, not a layout
         * constraint.
         */
        schultz_layout_arrange(tree, child,
                               schultz_rect_make(params.x, params.y,
                                                 size.width, size.height));
    }
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_absolute_vtable = {
    schultz_absolute_measure, schultz_absolute_arrange
};

const schultz_pane_vtable *schultz_pane_absolute(void)
{
    return &schultz_absolute_vtable;
}

/* ---------------------------------------------------------- BorderPane */

/* Finds the first visible child in a slot, or SCHULTZ_HANDLE_NONE. */
static schultz_handle schultz_border_slot(schultz_tree *tree,
                                          schultz_handle node, uint32_t slot)
{
    uint32_t count = schultz_node_child_count(tree, node);
    uint32_t i;

    for (i = 0; i < count; i++) {
        schultz_handle child;
        schultz_layout_params params;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_layout_child_visible(tree, child)) {
            continue;
        }
        if (schultz_node_get_layout_params(tree, child, &params)
                == SCHULTZ_OK && params.slot == slot) {
            return child;
        }
    }
    return SCHULTZ_HANDLE_NONE;
}

static int32_t schultz_border_measure(schultz_tree *tree, schultz_handle node,
                                      float avail_w, float avail_h,
                                      schultz_size *out_size)
{
    static const uint32_t slots[] = {
        SCHULTZ_SLOT_TOP, SCHULTZ_SLOT_BOTTOM, SCHULTZ_SLOT_LEFT,
        SCHULTZ_SLOT_RIGHT, SCHULTZ_SLOT_CENTER
    };
    schultz_size sizes[5];
    float padding = 0.0f;
    float width;
    float height;
    float middle_width;
    uint32_t i;

    schultz_node_get_spacing(tree, node, &padding, NULL);

    for (i = 0; i < 5u; i++) {
        schultz_handle child = schultz_border_slot(tree, node, slots[i]);
        sizes[i] = schultz_size_make(0.0f, 0.0f);
        if (child != SCHULTZ_HANDLE_NONE) {
            schultz_layout_measure(tree, child,
                                   schultz_layout_inset(avail_w,
                                                        padding * 2.0f),
                                   schultz_layout_inset(avail_h,
                                                        padding * 2.0f),
                                   &sizes[i]);
        }
    }

    /* Left, centre and right sit side by side; top and bottom span them. */
    middle_width = sizes[2].width + sizes[4].width + sizes[3].width;
    width = middle_width;
    if (sizes[0].width > width) { width = sizes[0].width; }
    if (sizes[1].width > width) { width = sizes[1].width; }

    height = sizes[4].height;
    if (sizes[2].height > height) { height = sizes[2].height; }
    if (sizes[3].height > height) { height = sizes[3].height; }
    height += sizes[0].height + sizes[1].height;

    *out_size = schultz_size_make(width + padding * 2.0f,
                                  height + padding * 2.0f);
    return SCHULTZ_OK;
}

static int32_t schultz_border_arrange(schultz_tree *tree, schultz_handle node,
                                      schultz_rect rect)
{
    schultz_handle top    = schultz_border_slot(tree, node, SCHULTZ_SLOT_TOP);
    schultz_handle bottom = schultz_border_slot(tree, node,
                                                SCHULTZ_SLOT_BOTTOM);
    schultz_handle left   = schultz_border_slot(tree, node, SCHULTZ_SLOT_LEFT);
    schultz_handle right  = schultz_border_slot(tree, node,
                                                SCHULTZ_SLOT_RIGHT);
    schultz_handle center = schultz_border_slot(tree, node,
                                                SCHULTZ_SLOT_CENTER);
    schultz_size size;
    float padding = 0.0f;
    float x;
    float y;
    float width;
    float height;

    schultz_node_get_spacing(tree, node, &padding, NULL);
    x      = padding;
    y      = padding;
    width  = rect.width - padding * 2.0f;
    height = rect.height - padding * 2.0f;
    if (width < 0.0f)  { width = 0.0f; }
    if (height < 0.0f) { height = 0.0f; }

    /* Top and bottom first: they claim full width at their natural height. */
    if (top != SCHULTZ_HANDLE_NONE &&
        schultz_layout_measure(tree, top, width, -1.0f, &size) == SCHULTZ_OK) {
        float h = (size.height < height) ? size.height : height;
        schultz_layout_arrange(tree, top,
                               schultz_rect_make(x, y, width, h));
        y      += h;
        height -= h;
    }
    if (bottom != SCHULTZ_HANDLE_NONE &&
        schultz_layout_measure(tree, bottom, width, -1.0f, &size)
            == SCHULTZ_OK) {
        float h = (size.height < height) ? size.height : height;
        schultz_layout_arrange(tree, bottom,
                               schultz_rect_make(x, y + height - h, width, h));
        height -= h;
    }

    /* Then left and right, from what the edges left. */
    if (left != SCHULTZ_HANDLE_NONE &&
        schultz_layout_measure(tree, left, -1.0f, height, &size)
            == SCHULTZ_OK) {
        float w = (size.width < width) ? size.width : width;
        schultz_layout_arrange(tree, left,
                               schultz_rect_make(x, y, w, height));
        x     += w;
        width -= w;
    }
    if (right != SCHULTZ_HANDLE_NONE &&
        schultz_layout_measure(tree, right, -1.0f, height, &size)
            == SCHULTZ_OK) {
        float w = (size.width < width) ? size.width : width;
        schultz_layout_arrange(tree, right,
                               schultz_rect_make(x + width - w, y, w, height));
        width -= w;
    }

    /* The centre takes everything that is left. */
    if (center != SCHULTZ_HANDLE_NONE) {
        schultz_layout_arrange(tree, center,
                               schultz_rect_make(x, y, width, height));
    }
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_border_vtable = {
    schultz_border_measure, schultz_border_arrange
};

const schultz_pane_vtable *schultz_pane_border(void)
{
    return &schultz_border_vtable;
}
