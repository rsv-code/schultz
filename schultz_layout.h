/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_layout.h
 * @brief The layout protocol and the built in panes.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 *
 * Two operations make up the whole required interface:
 *
 *   measure(node, avail_w, avail_h) -> size
 *   arrange(node, rect)
 *
 * The parent decides constraints and position; the child decides its own
 * size. Neither does the other's job, which is what keeps layout a single
 * recursive walk with no solver and no relaxation loop.
 *
 * Because a parent asks for a preferred size **given an available width**,
 * and a paragraph's height depends on the width it is given, wrapping text
 * falls out of the ordering with no special case. That is the reason the
 * signature takes an available size at all.
 *
 * Dispatch is a per node vtable, so an application can add its own pane types
 * without modifying Schultz.
 */

#ifndef SCHULTZ_LAYOUT_H
#define SCHULTZ_LAYOUT_H

#include "schultz_geom.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Everything declared below is part of the host facing interface, so it is
 * marked visible. The toolkit is compiled with -fvisibility=hidden, which
 * hides everything by default: that is what stops a shared library built
 * from it exporting the whole of FreeType, libpng and zlib alongside, where
 * they would meet the copies already loaded by whatever is hosting it.
 *
 * A pragma rather than an attribute on each declaration, because there are
 * some hundreds of them and one pair of lines per header says the same thing.
 */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif


/**
 * @brief Forward declaration of the widget tree.
 *
 * Layout does not need the tree's definition, and the tree needs these types,
 * so declaring it here breaks what would otherwise be an include cycle.
 */
typedef struct schultz_tree schultz_tree;

/**
 * @brief Reports whether an available size means "no limit".
 *
 * Any negative available size is unbounded. This is not an edge case: a
 * context menu wants to be as wide as its widest item with no cap from a
 * parent, a vertical scroll view gives its content unbounded height, and most
 * mobile screens scroll. **Every pane must have a defined answer for it**, and
 * each built in pane documents its own.
 *
 * @param available An available width or height, in pixels.
 * @return 1 when the size is unbounded, 0 when it is a real limit.
 */
int32_t schultz_layout_is_unbounded(float available);

/**
 * @brief How eagerly a child takes leftover space along the main axis.
 *
 * Three coarse levels rather than proportional weights, following the JavaFX
 * model. Leftover space is shared equally among the children at the highest
 * priority present; children below that priority get none. This covers the
 * large majority of real cases, and when genuine two to one splitting is
 * needed a FlexPane can be added as another pane type.
 */
enum {
    SCHULTZ_GROW_NEVER = 0, /**< Stays at its preferred size. */
    SCHULTZ_GROW_MAYBE,     /**< Grows only if no ALWAYS child is present. */
    SCHULTZ_GROW_ALWAYS     /**< Shares the leftover space. */
};

/** @brief Cross axis placement of a child within the space it is given. */
enum {
    SCHULTZ_ALIGN_STRETCH = 0, /**< Fill the cross axis. */
    SCHULTZ_ALIGN_START,       /**< Top, or left. */
    SCHULTZ_ALIGN_CENTER,      /**< Centred. */
    SCHULTZ_ALIGN_END          /**< Bottom, or right. */
};

/** @brief Which slot of a BorderPane a child occupies. */
enum {
    SCHULTZ_SLOT_CENTER = 0, /**< Takes whatever the edges leave. */
    SCHULTZ_SLOT_TOP,        /**< Full width, its preferred height. */
    SCHULTZ_SLOT_BOTTOM,     /**< Full width, its preferred height. */
    SCHULTZ_SLOT_LEFT,       /**< Its preferred width, remaining height. */
    SCHULTZ_SLOT_RIGHT       /**< Its preferred width, remaining height. */
};

/** @brief How one grid row or column is sized. */
enum {
    /** As large as the largest cell in it. The default. */
    SCHULTZ_TRACK_CONTENT = 0,
    /** Exactly the given number of pixels. */
    SCHULTZ_TRACK_FIXED,
    /** Shares the leftover space in proportion to the given weight. */
    SCHULTZ_TRACK_WEIGHTED
};

/** @brief The largest number of rows or columns a GridPane may have. */
enum {
    SCHULTZ_GRID_MAX_TRACKS = 32
};

/** @brief One row or column definition. */
typedef struct {
    uint32_t kind;  /**< One of SCHULTZ_TRACK_*. */
    float    value; /**< Size for FIXED, weight for WEIGHTED, unused else. */
} schultz_grid_track;

/**
 * @brief Per child layout properties, interpreted by the parent pane.
 *
 * These do not live as fields on every node. HBox wants a grow flag, GridPane
 * wants a row and column, an absolute Pane wants coordinates: putting all of
 * them on the node would mean a struct full of values that are ignored, and
 * users setting a property that silently does nothing. Instead each node
 * carries one of these, and a parent reads only the member matching its own
 * kind. A child under a pane that does not use its member gets that pane's
 * documented default.
 */
typedef struct {
    uint32_t grow;  /**< Box panes: one of SCHULTZ_GROW_*. */
    uint32_t align; /**< Box and stack panes: one of SCHULTZ_ALIGN_*. */

    uint32_t row;         /**< GridPane: zero based row. */
    uint32_t column;      /**< GridPane: zero based column. */
    uint32_t row_span;    /**< GridPane: rows covered, at least one. */
    uint32_t column_span; /**< GridPane: columns covered, at least one. */

    uint32_t slot;        /**< BorderPane: one of SCHULTZ_SLOT_*. */

    float x;              /**< Absolute Pane: x in the parent's space. */
    float y;              /**< Absolute Pane: y in the parent's space. */
} schultz_layout_params;

/**
 * @brief Fills in the defaults for per child layout properties.
 *
 * Grow never, stretch across the cross axis, grid cell 0,0 spanning one
 * track, the centre slot, and the origin.
 *
 * @param out_params Receives the defaults. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when out_params is
 *         NULL.
 */
int32_t schultz_layout_params_default(schultz_layout_params *out_params);

/**
 * @brief Nothing said here: the toolkit decides.
 *
 * What to pass schultz_node_set_min_size, schultz_node_set_pref_size and
 * schultz_node_set_max_size for an axis you have no opinion about. It reads
 * as the sensible thing in each: compute a preferred size from the content,
 * impose no minimum, impose no maximum.
 *
 * One value rather than three, because three would have to be remembered
 * apart. Zero is a real size everywhere and never means "work it out": a
 * node told it is zero high is zero high, and asking for a width and passing
 * zero for the height is how a label came out invisible once already.
 */
#define SCHULTZ_SIZE_UNSET (-1.0f)

/**
 * @brief The interface a pane implements.
 *
 * Both entries are required. A node with no pane is a leaf: it measures to
 * its preferred size hints and arranges nothing.
 */
typedef struct {
    /**
     * Reports the size this node wants, given the space available.
     *
     * Either available size may be unbounded; see
     * schultz_layout_is_unbounded. The returned size is clamped against the
     * node's size hints by the caller, so an implementation need not do it.
     */
    int32_t (*measure)(schultz_tree *tree, schultz_handle node,
                       float avail_w, float avail_h, schultz_size *out_size);
    /**
     * Assigns final rectangles to children and recurses.
     *
     * The rectangle given is in the node's own parent relative space, and the
     * rectangles written into children are relative to this node.
     */
    int32_t (*arrange)(schultz_tree *tree, schultz_handle node,
                       schultz_rect rect);
} schultz_pane_vtable;

/**
 * @brief Measures a node, honouring its pane and its size hints.
 *
 * Dispatches to the node's pane when it has one, then clamps the result
 * against the node's minimum and maximum. A node with no pane reports its
 * preferred size hints, or zero where they are not set.
 *
 * @param tree     The tree holding the node. Must not be NULL.
 * @param node     The node to measure.
 * @param avail_w  Available width, or negative for unbounded.
 * @param avail_h  Available height, or negative for unbounded.
 * @param out_size Receives the measured size. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer, or
 *         SCHULTZ_ERR_INVALID_HANDLE when the node is not live.
 */
int32_t schultz_layout_measure(schultz_tree *tree, schultz_handle node,
                               float avail_w, float avail_h,
                               schultz_size *out_size);

/**
 * @brief Assigns a node its final rectangle and lays out its subtree.
 *
 * Sets the node's bounds, which marks the affected regions dirty, then
 * hands over to its pane to place the children.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node The node to place.
 * @param rect The final rectangle, in the parent's coordinate space.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL tree, or
 *         SCHULTZ_ERR_INVALID_HANDLE when the node is not live.
 */
int32_t schultz_layout_arrange(schultz_tree *tree, schultz_handle node,
                               schultz_rect rect);

/* ------------------------------------------------------------ built in panes
 *
 * Each returns a vtable with static storage duration, so the pointer stays
 * valid for the life of the program and may be handed straight to
 * schultz_node_set_pane.
 */

/**
 * @brief Lays out whatever in the tree needs it, and nothing that does not.
 *
 * A host running a frame loop calls this once a frame, after resolving styles
 * and before painting. It walks down from the root looking for nodes marked
 * as needing layout, arranges each at the bounds it already has, and stops
 * there: arranging a node lays out everything beneath it, so a change deep in
 * the tree costs the subtree it happened in rather than the window.
 *
 * A node with no pane places nothing, so the walk carries on past it into
 * whatever is below. That is what makes a page of absolutely positioned
 * children work: it has no pane, and the panes inside it are found and run.
 *
 * Nothing is left marked afterwards.
 *
 * @param tree The tree to lay out. Must not be NULL.
 * @return How many nodes were arranged, which is zero when nothing had
 *         changed and the whole call cost one flag test.
 */
uint32_t schultz_layout_run(schultz_tree *tree);

/**
 * @brief Where an overlay sits relative to whatever it belongs to.
 *
 * An overlay that would hang off the window is nudged back inside rather than
 * flipped to the other side: moving a little is less surprising than jumping.
 */
enum {
    SCHULTZ_PLACE_BELOW = 0, /**< Under it, left edges lined up. */
    SCHULTZ_PLACE_ABOVE,     /**< Over it. */
    SCHULTZ_PLACE_RIGHT,     /**< Beside it, to the right. */
    SCHULTZ_PLACE_LEFT,      /**< Beside it, to the left. */
    SCHULTZ_PLACE_OVER       /**< On top of it, corners lined up. */
};

/**
 * @brief Keeps a node beside something, and inside the window.
 *
 * What a menu and a popover want: sit under the button that opened me, and
 * if that would hang me off an edge, nudge me back in. Layout does it every
 * pass rather than once when the node is shown, so the node stays where it
 * belongs when the window changes size. On a phone that matters, because a
 * rotation is a resize and it does not dismiss anything.
 *
 * The node keeps its own size, measured from its contents. Only where it
 * sits is decided here.
 *
 * @param tree      The tree holding the node. Must not be NULL.
 * @param node      The node to place. Must name a live node.
 * @param anchor    What to sit beside, in the same space as the node's own
 *                  bounds.
 * @param placement One of SCHULTZ_PLACE_*.
 * @param gap       Clearance between the node and its anchor, in the
 *                  placement's own direction. Zero sits flush against it.
 * @param centred   Nonzero centres the node on the anchor across the axis
 *                  the placement did not decide: above and below centre it
 *                  across the anchor's width, left and right across its
 *                  height. Zero lines their leading edges up, which is what
 *                  a menu under a button wants.
 * @param flips     Nonzero sends the node to the opposite side when the side
 *                  asked for does not fit and the other one does, rather
 *                  than nudging it back inside. A tooltip and a popover want
 *                  this: nudged, they come to rest on top of the thing they
 *                  are explaining. A menu does not, because a submenu that
 *                  jumps above the row that opened it is disorienting.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_anchored(schultz_tree *tree, schultz_handle node,
                                  schultz_rect anchor, uint32_t placement,
                                  float gap, int32_t centred, int32_t flips);

/**
 * @brief Which side an anchored node actually ended up on.
 *
 * The placement it was given, unless it was allowed to flip and did, in
 * which case the opposite one. What a node needs to know to draw anything
 * that points back at its anchor.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_PLACE_BELOW.
 * @param node The node to query.
 * @return One of SCHULTZ_PLACE_*.
 */
uint32_t schultz_node_anchor_placement(const schultz_tree *tree,
                                       schultz_handle node);

/**
 * @brief Stops keeping a node beside something.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node The node to release. Must name a live node.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_clear_anchored(schultz_tree *tree, schultz_handle node);

/**
 * @brief Says that something other than a pane decides where a node goes.
 *
 * A pane places its children. A few nodes are placed by something else and
 * have to be left out of that, or the pane above them drags them into the row
 * or column with everything else and they stop being where they belong. A
 * toast is the case in the toolkit: it sits against the edge of the window
 * and stacks with the other toasts, wherever it happens to hang in the tree.
 *
 * Set this on any node an application places itself. Without it, such a node
 * only works while no pane sits above it, which is a rule nothing enforces
 * and nobody can guess: a host that gives its root a pane finds its own
 * hand placed nodes quietly swept into the flow.
 *
 * An overlay does not need this. A dialog, a menu and a popover are already
 * left alone, because the overlay list says the same thing about them.
 *
 * A node like this is still measured when something asks it for a size, and
 * it is drawn and hit tested as usual. Only placing it is somebody else's
 * job.
 *
 * @param tree          The tree holding the node. Must not be NULL.
 * @param node          The node to mark. Must name a live node.
 * @param places_itself Nonzero to keep panes off it, zero to let a pane place
 *                      it again, which is how a node starts.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_places_itself(schultz_tree *tree,
                                       schultz_handle node,
                                       int32_t places_itself);

/**
 * @brief Reports whether a node is placed by something other than a pane.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return 1 when no pane places it, 0 otherwise.
 */
int32_t schultz_node_places_itself(const schultz_tree *tree,
                                   schultz_handle node);

/**
 * @brief Rows and columns, with cells that align across the whole grid.
 *
 * This is the pane the composable model needs. Nested rows do not align their
 * columns with each other, and no amount of nesting fixes that, so anything
 * genuinely tabular needs real tracks.
 *
 * Track sizes come from schultz_node_set_grid_tracks. A content track is as
 * large as the largest cell in it, a fixed track is exactly its value, and a
 * weighted track shares whatever is left over in proportion to its weight. A
 * child spanning several tracks that does not fit grows them evenly.
 *
 * **Unbounded** available space means weighted tracks have nothing to share,
 * so they collapse to their content size and the grid reports its natural
 * size. Rows and columns beyond SCHULTZ_GRID_MAX_TRACKS are ignored.
 *
 * @return The GridPane vtable.
 */
const schultz_pane_vtable *schultz_pane_grid(void);

/**
 * @brief A column: children stacked top to bottom.
 *
 * Measure sums the children's heights plus the gaps and takes the widest
 * child. **Unbounded height** is passed straight to the children, so a column
 * inside a scroll view reports its full natural height. **Unbounded width**
 * is likewise passed down, and the column reports its widest child.
 *
 * @return The VBox vtable.
 */
const schultz_pane_vtable *schultz_pane_vbox(void);

/**
 * @brief A row: children placed left to right.
 *
 * The mirror of VBox. Measure sums widths plus gaps and takes the tallest
 * child. **Unbounded width** is passed to the children and the row reports
 * the sum of their natural widths.
 *
 * @return The HBox vtable.
 */
const schultz_pane_vtable *schultz_pane_hbox(void);

/**
 * @brief Children share one rectangle, aligned within it.
 *
 * Measure returns the largest child size on each axis. **Unbounded** on
 * either axis is passed to the children, and the result is the maximum of
 * what they report. Overlays, badges and centred content all fall out of
 * this.
 *
 * @return The StackPane vtable.
 */
const schultz_pane_vtable *schultz_pane_stack(void);

/**
 * @brief Children at explicit coordinates.
 *
 * Each child is placed at the x and y in its layout params, at its preferred
 * size. Measure returns the union of the child extents, so an absolute pane
 * still reports a sensible size to whatever contains it. **Unbounded** is
 * passed to the children. Children may overflow the pane's bounds; clipping
 * is a separate concern, not a layout constraint.
 *
 * @return The absolute Pane vtable.
 */
const schultz_pane_vtable *schultz_pane_absolute(void);

/**
 * @brief Children run across in rows, starting a new row when one is full.
 *
 * The pane a tag list, a wrapping toolbar or a gallery of thumbnails wants.
 * Its height depends on the width it is offered, which is the case the
 * measure signature exists for and the same shape as wrapping text: narrower
 * means more rows and a taller pane.
 *
 * A child is as wide as it asks to be and is aligned within its row's height
 * by its own `align`, so a row of mixed heights lines up rather than every
 * child stretching. The gap sits between children on a row and between the
 * rows themselves.
 *
 * **Unbounded** width means nothing ever has to wrap, so everything lands on
 * one row and the pane reports the width that row needs. That is what a
 * flow pane inside a horizontal scroll view or a context menu asks for.
 *
 * A child wider than the pane is put on a row of its own and allowed to
 * overflow, in the same way as a word too long for a line: there is nowhere
 * narrower to put it.
 *
 * @return The FlowPane vtable.
 */
const schultz_pane_vtable *schultz_pane_flow(void);

/**
 * @brief Five slots: top, bottom, left, right and centre.
 *
 * Top and bottom span the full width at their preferred heights, left and
 * right take their preferred widths from what remains, and the centre takes
 * the rest. **Unbounded** on an axis makes the edge children report their
 * natural size and the centre contribute its own.
 *
 * @return The BorderPane vtable.
 */
const schultz_pane_vtable *schultz_pane_border(void);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_LAYOUT_H */
