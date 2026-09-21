/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_node.h
 * @brief Retained widget tree, invalidation, and the accessibility schema.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 *
 * The tree is the persistent structure a retained mode toolkit is built
 * around: nodes hold their own state between frames, and a change marks a
 * region dirty rather than triggering an immediate repaint. Getting that
 * marking right is the hardest correctness problem in the design. Marking too
 * little leaves stale pixels on screen; marking too much repaints everything
 * and gives up the only advantage retained mode has.
 *
 * Two dirty bits per node carry that:
 *
 *   - `dirty` means this node's own pixels are wrong.
 *   - `subtree_dirty` means this node or something under it is dirty, so a
 *     paint walk can skip a clean subtree without descending into it.
 *
 * Alongside those, the tree accumulates the union of every dirty rectangle in
 * window coordinates, which is what the presentation layer uploads.
 *
 * Every node also carries accessibility fields. They are modelled on
 * AccessKit's node schema but owned by Schultz, so the bridge translates
 * rather than the tree depending on a library. Retrofitting this shape later
 * would mean touching every widget.
 */

#ifndef SCHULTZ_NODE_H
#define SCHULTZ_NODE_H

#include "schultz_geom.h"
#include "schultz_layout.h"
#include "schultz_style.h"

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


/** @brief Opaque widget tree. Create with schultz_tree_create. */
typedef struct schultz_tree schultz_tree;

/** @brief Interaction and presentation state, as a bit set. */
enum {
    SCHULTZ_STATE_VISIBLE  = 1u << 0, /**< Painted and hit tested. */
    SCHULTZ_STATE_ENABLED  = 1u << 1, /**< Accepts input. */
    SCHULTZ_STATE_HOVERED  = 1u << 2, /**< Pointer is over it. */
    SCHULTZ_STATE_FOCUSED  = 1u << 3, /**< Holds keyboard focus. */
    SCHULTZ_STATE_PRESSED  = 1u << 4, /**< Being activated. */
    SCHULTZ_STATE_CHECKED  = 1u << 5, /**< Checkbox or radio is on. */
    SCHULTZ_STATE_SELECTED = 1u << 6  /**< Selected within a list. */
};

/** @brief Default state for a new node: visible and enabled. */
enum {
    SCHULTZ_STATE_DEFAULT = SCHULTZ_STATE_VISIBLE | SCHULTZ_STATE_ENABLED
};

/**
 * @brief What a node is, for assistive technology.
 *
 * Modelled on AccessKit's roles. Every platform accessibility API wants this
 * classification, so it lives on the node rather than in the bridge.
 */
enum {
    SCHULTZ_ROLE_UNKNOWN = 0, /**< Not yet classified. */
    SCHULTZ_ROLE_WINDOW,      /**< Top level container. */
    SCHULTZ_ROLE_GROUP,       /**< Container with no semantics of its own. */
    SCHULTZ_ROLE_LABEL,       /**< Static text. */
    SCHULTZ_ROLE_BUTTON,      /**< Activates on click. */
    SCHULTZ_ROLE_CHECKBOX,    /**< Two or three state toggle. */
    SCHULTZ_ROLE_RADIO,       /**< One of a mutually exclusive set. */
    SCHULTZ_ROLE_TEXT_INPUT,  /**< Editable text. */
    SCHULTZ_ROLE_LIST,        /**< Container of list items. */
    SCHULTZ_ROLE_LIST_ITEM,   /**< One entry in a list. */
    SCHULTZ_ROLE_IMAGE,       /**< Picture, with a name as alternative text. */
    SCHULTZ_ROLE_SCROLL_VIEW, /**< Clipped, scrollable viewport. */
    SCHULTZ_ROLE_SLIDER,      /**< Continuous value in a range. */
    SCHULTZ_ROLE_MENU,        /**< Menu or context menu. */
    SCHULTZ_ROLE_MENU_ITEM,   /**< One entry in a menu. */
    SCHULTZ_ROLE_PROGRESS     /**< How far along a task is. */
};

/**
 * @brief The pointer shape shown over a node.
 *
 * A node with the default shape inherits nothing: the platform layer looks at
 * whatever the pointer is over and shows that node's shape, so a container
 * saying nothing simply lets its own default stand.
 */
enum {
    SCHULTZ_CURSOR_DEFAULT = 0, /**< The ordinary arrow. */
    SCHULTZ_CURSOR_TEXT,        /**< An I-beam, over editable text. */
    SCHULTZ_CURSOR_POINTER,     /**< A hand, over something that activates. */
    SCHULTZ_CURSOR_RESIZE_H,    /**< Left and right, over a vertical divider. */
    SCHULTZ_CURSOR_RESIZE_V,    /**< Up and down, over a horizontal divider. */

    SCHULTZ_CURSOR_COUNT        /**< How many shapes there are. */
};

/** @brief What assistive technology may ask a node to do, as a bit set. */
enum {
    SCHULTZ_ACTION_CLICK     = 1u << 0, /**< Activate the node. */
    SCHULTZ_ACTION_FOCUS     = 1u << 1, /**< Give it keyboard focus. */
    SCHULTZ_ACTION_INCREMENT = 1u << 2, /**< Step a value up. */
    SCHULTZ_ACTION_DECREMENT = 1u << 3, /**< Step a value down. */
    SCHULTZ_ACTION_SET_VALUE = 1u << 4, /**< Replace the value. */
    SCHULTZ_ACTION_SCROLL    = 1u << 5  /**< Scroll the node into view. */
};

/**
 * @brief Creates an empty tree with a root node.
 *
 * @param out_tree Receives the new tree. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when out_tree is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_tree_create(schultz_tree **out_tree);

/**
 * @brief Destroys a tree and every node in it.
 *
 * @param tree The tree to destroy. NULL is accepted and does nothing.
 */
void schultz_tree_destroy(schultz_tree *tree);

/**
 * @brief Returns the tree's root node.
 *
 * The root exists for the life of the tree and cannot be destroyed.
 *
 * @param tree The tree to query. NULL yields SCHULTZ_HANDLE_NONE.
 * @return The root handle.
 */
schultz_handle schultz_tree_root(const schultz_tree *tree);

/**
 * @brief Sets the tree's viewport, which bounds every dirty region.
 *
 * @param tree     The tree to configure. Must not be NULL.
 * @param viewport The window area in pixels. Dirty regions are clipped
 *                 to this, so a node moved off screen does not inflate the
 *                 repaint area.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when tree is NULL.
 */
int32_t schultz_tree_set_viewport(schultz_tree *tree, schultz_rect viewport);

/**
 * @brief Reads the visible area.
 *
 * An overlay needs it to keep itself on screen, and it is the one rectangle
 * that is not any node's bounds.
 *
 * @param tree         The tree to query. Must not be NULL.
 * @param out_viewport Receives the viewport. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer.
 */
int32_t schultz_tree_get_viewport(const schultz_tree *tree,
                                  schultz_rect *out_viewport);

/**
 * @brief Records how many screen pixels one unit covers.
 *
 * The frame driver sets this from the screen. It is here so that a widget
 * which has to answer in real pixels, a canvas being the one that does, can
 * work it out without being handed the frame.
 *
 * @param tree  The tree to configure. Must not be NULL.
 * @param scale Screen pixels per unit. Must be greater than zero.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_tree_set_pixel_scale(schultz_tree *tree, float scale);

/**
 * @brief Returns how many screen pixels one unit covers.
 *
 * One when nothing set it, which is the truth on a screen with nothing to
 * scale and the safe answer anywhere else.
 *
 * @param tree The tree to query. NULL yields 1.
 * @return The scale. Never zero or negative.
 */
float schultz_tree_pixel_scale(const schultz_tree *tree);

/**
 * @brief Creates a node, parented to another node.
 *
 * The new node is visible and enabled, with empty bounds, so it paints
 * nothing until it is given a size.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent to attach to. Must name a live node, or
 *                 SCHULTZ_HANDLE_NONE for a node that starts out of the tree.
 *                 See schultz_node_set_parent for what that means.
 * @param out_node Receives the new node handle. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer,
 *         SCHULTZ_ERR_INVALID_HANDLE when the parent is named but not live,
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_node_create(schultz_tree *tree, schultz_handle parent,
                            schultz_handle *out_node);

/**
 * @brief Destroys a node and its entire subtree.
 *
 * The area the subtree occupied is marked dirty, because those pixels must be
 * repainted by whatever is now behind them. Every handle in the subtree goes
 * stale.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node The node to destroy. Destroying the root is refused.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not live,
 *         or SCHULTZ_ERR_INVALID_ARGUMENT when asked to destroy the root.
 */
int32_t schultz_node_destroy(schultz_tree *tree, schultz_handle node);

/**
 * @brief Moves a node to a new parent, or takes it out of the tree.
 *
 * Both the old and the new position are marked dirty, and both panes are told
 * to place their children again.
 *
 * Passing SCHULTZ_HANDLE_NONE detaches the node: it keeps everything it has,
 * including its children, its styles and whatever state its widget is holding
 * such as a scroll offset or a selection, and it stops being part of the
 * tree. A detached node is not laid out, not painted, not hit tested and not
 * read by a screen reader, it is not ticked, and changing it does not mark
 * any part of the window for repainting. Handing it a parent again puts it
 * back, laid out and repainted where it lands.
 *
 * This is what a language binding needs for a `remove` that leaves a widget
 * alive and re-addable, with `destroy` as the call that ends it. Ask
 * schultz_node_is_attached to tell the two apart.
 *
 * Detaching gives up the two things that name what is on screen: the text
 * selection, and a place in the overlay list. Those are asked for again
 * rather than restored, because a node that was an overlay before is not
 * necessarily one afterwards. Everything the node asked for itself it keeps,
 * so a widget that was animating carries on where it left off when it goes
 * back in.
 *
 * @param tree   The tree holding both nodes. Must not be NULL.
 * @param node   The node to move. The root cannot be reparented.
 * @param parent The new parent, or SCHULTZ_HANDLE_NONE for no parent. Must
 *               not be the node itself or one of its descendants, which
 *               would form a cycle.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not live or
 *         the parent is named but not live, SCHULTZ_ERR_INVALID_ARGUMENT for
 *         the root or a cycle, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_node_set_parent(schultz_tree *tree, schultz_handle node,
                                schultz_handle parent);

/**
 * @brief Reports whether a node is in the tree.
 *
 * True when following the node's parents arrives at the root. False for a
 * node detached with schultz_node_set_parent, for anything under one, and for
 * a handle that no longer names a live node.
 *
 * This is the difference between a widget that exists and a widget that is on
 * screen, and it is the question a binding's `isAttached` is really asking.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return 1 when the node is in the tree, 0 otherwise.
 */
int32_t schultz_node_is_attached(const schultz_tree *tree,
                                 schultz_handle node);

/**
 * @brief Returns a node's parent.
 *
 * @param tree       The tree holding the node. Must not be NULL.
 * @param node       The node to query.
 * @param out_parent Receives the parent, or SCHULTZ_HANDLE_NONE for the root
 *                   and for a detached node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when out_parent is NULL,
 *         or SCHULTZ_ERR_INVALID_HANDLE when the node is not live.
 */
int32_t schultz_node_parent(const schultz_tree *tree, schultz_handle node,
                            schultz_handle *out_parent);

/**
 * @brief Counts a node's immediate children.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return The number of children, or 0 when the node is not live.
 */
uint32_t schultz_node_child_count(const schultz_tree *tree,
                                  schultz_handle node);

/**
 * @brief Returns one of a node's children by position.
 *
 * Children are kept in insertion order, which is paint order: later children
 * paint on top of earlier ones.
 *
 * @param tree      The tree holding the node. Must not be NULL.
 * @param node      The parent to query.
 * @param index     Zero based position among the children.
 * @param out_child Receives the child handle. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when out_child is NULL or
 *         index is out of range, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_child_at(const schultz_tree *tree, schultz_handle node,
                              uint32_t index, schultz_handle *out_child);

/**
 * @brief Sets a node's bounds, relative to its parent.
 *
 * Both the area vacated and the area newly occupied are marked dirty. Marking
 * only the new one is the classic invalidation bug: the old position keeps
 * showing whatever was last painted there.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   The node to move or resize.
 * @param bounds The new bounds, in the parent's coordinate space.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not
 *         live.
 */
int32_t schultz_node_set_bounds(schultz_tree *tree, schultz_handle node,
                                schultz_rect bounds);

/**
 * @brief Reads a node's bounds, relative to its parent.
 *
 * @param tree       The tree holding the node. Must not be NULL.
 * @param node       The node to query.
 * @param out_bounds Receives the bounds. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when out_bounds is NULL,
 *         or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_get_bounds(const schultz_tree *tree, schultz_handle node,
                                schultz_rect *out_bounds);

/**
 * @brief Computes a node's bounds in window coordinates.
 *
 * Walks to the root accumulating parent offsets.
 *
 * @param tree       The tree holding the node. Must not be NULL.
 * @param node       The node to locate.
 * @param out_bounds Receives the absolute bounds. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when out_bounds is NULL,
 *         or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_absolute_bounds(const schultz_tree *tree,
                                     schultz_handle node,
                                     schultz_rect *out_bounds);

/**
 * @brief Replaces a node's state flags.
 *
 * Marks the node dirty when the flags actually change, and not otherwise, so
 * a redundant set costs no repaint. Clearing SCHULTZ_STATE_VISIBLE marks the
 * area the node occupied, because it must be repainted without it.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  The node to update.
 * @param state A bit set of SCHULTZ_STATE_* values.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_state(schultz_tree *tree, schultz_handle node,
                               uint32_t state);

/**
 * @brief Reads a node's state flags.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return The bit set, or 0 when the node is not live.
 */
uint32_t schultz_node_get_state(const schultz_tree *tree,
                                schultz_handle node);

/**
 * @brief Reports whether a node and all its ancestors are visible.
 *
 * A node inside a hidden parent is not drawn even if its own visible flag is
 * set, so this is the question paint and hit testing actually ask.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query. A node that is not live yields 0, because a
 *             node that does not exist cannot be visible.
 * @return 1 when the node is effectively visible, 0 otherwise.
 */
int32_t schultz_node_is_visible(const schultz_tree *tree,
                                schultz_handle node);

/**
 * @brief Reports whether a node accepts input, itself and all the way up.
 *
 * Disabled is inherited, the way being hidden is: a node inside a disabled
 * one is disabled too, so turning a panel off turns off everything on it.
 *
 * A disabled node still stops the pointer reaching whatever is behind it. It
 * simply does nothing with what it stops: no hover, no press, no click, and
 * no key. That is what a disabled control looks like everywhere else, and it
 * is why this is not done by leaving the node out of hit testing.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return 1 when it and every node above it are enabled, 0 otherwise.
 */
int32_t schultz_node_is_enabled(const schultz_tree *tree,
                                schultz_handle node);

/**
 * @brief Sets the pointer shape shown over a node.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   The node to change.
 * @param cursor One of the SCHULTZ_CURSOR_* values.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for an unknown shape, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_cursor(schultz_tree *tree, schultz_handle node,
                                uint32_t cursor);

/**
 * @brief Returns the pointer shape shown over a node.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_CURSOR_DEFAULT.
 * @param node The node to query.
 * @return One of the SCHULTZ_CURSOR_* values.
 */
uint32_t schultz_node_cursor(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Marks a node's own area as needing repaint.
 *
 * Use this when something that affects only appearance changes, such as a
 * colour or a label.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node The node to invalidate.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_invalidate(schultz_tree *tree, schultz_handle node);

/**
 * @brief Reports whether a node is itself marked dirty.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return 1 when the node's own pixels need repainting, 0 otherwise.
 */
int32_t schultz_node_is_dirty(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Reports whether a node or anything beneath it is dirty.
 *
 * A paint walk tests this to decide whether to descend, which is what lets a
 * clean subtree cost nothing.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return 1 when the subtree contains a dirty node, 0 otherwise.
 */
int32_t schultz_node_subtree_dirty(const schultz_tree *tree,
                                   schultz_handle node);

/**
 * @brief How many separate areas the tree keeps track of repainting.
 *
 * Enough that scattered changes stay apart, few enough that the fixed cost of
 * a rectangle does not add up. Past this they are merged, so the worst case
 * is the single rectangle this used to be rather than anything worse. See
 * docs/repainting.md.
 */
#define SCHULTZ_TREE_DIRTY_PARTS 8u

/**
 * @brief How many wasted pixels are worth paying to avoid a rectangle.
 *
 * Two areas close together cost less as one. Measured on this toolkit, a
 * rectangle costs about 0.17 ms of fixed work and about five nanoseconds a
 * pixel, so anything under a few thousand wasted pixels is cheaper merged.
 */
#define SCHULTZ_TREE_DIRTY_WASTE 4096.0f

/**
 * @brief Reports whether anything in the tree needs repainting.
 *
 * @param tree The tree to query. NULL yields 0.
 * @return 1 when a repaint is needed, 0 when the frame can be skipped.
 */
int32_t schultz_tree_is_dirty(const schultz_tree *tree);

/**
 * @brief How many separate areas need repainting.
 *
 * The repainting a tree asks for is a few rectangles rather than one, because
 * one means two small changes at opposite corners repaint everything between
 * them. docs/repainting.md explains it and gives the measurements.
 *
 * Zero means nothing changed.
 *
 * @param tree The tree to query. NULL yields zero.
 * @return How many, at most SCHULTZ_TREE_DIRTY_PARTS.
 */
uint32_t schultz_tree_dirty_count(const schultz_tree *tree);

/**
 * @brief Returns one of the areas that need repainting.
 *
 * They do not overlap in any way worth relying on: a caller that paints each
 * in turn may paint a pixel twice, which costs a little and is never wrong.
 *
 * @param tree       The tree to query. Must not be NULL.
 * @param index      From zero to schultz_tree_dirty_count minus one.
 * @param out_region Receives it. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_EXHAUSTED when there is no such area.
 */
int32_t schultz_tree_dirty_at(const schultz_tree *tree, uint32_t index,
                              schultz_rect *out_region);

/**
 * @brief Returns the union of every dirty region, in window coordinates.
 *
 * This is the rectangle the rasterizer should be restricted to and the
 * presentation layer should upload.
 *
 * @param tree       The tree to query. Must not be NULL.
 * @param out_region Receives the union, clipped to the viewport. Must not be
 *                   NULL. An empty rectangle means nothing needs repainting.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when either pointer is
 *         NULL.
 */
int32_t schultz_tree_dirty_region(const schultz_tree *tree,
                                  schultz_rect *out_region);

/**
 * @brief Clears every dirty mark in the tree.
 *
 * Call once a frame has been painted and presented. Clearing before painting
 * loses the repaint.
 *
 * @param tree The tree to clear. NULL is accepted and does nothing.
 */
void schultz_tree_clear_dirty(schultz_tree *tree);

/**
 * @brief Counts live nodes, including the root.
 *
 * @param tree The tree to query. NULL yields 0.
 * @return The number of nodes.
 */
uint32_t schultz_tree_node_count(const schultz_tree *tree);

/* ------------------------------------------------------------- style */

/**
 * @brief The interaction states a node may carry a style patch for.
 *
 * Applied in this order during resolution, so a later one overrides an
 * earlier one. That is why disabled sits last: a disabled control must not
 * appear to respond to the pointer.
 */
enum {
    SCHULTZ_STYLE_STATE_HOVERED = 0, /**< The pointer is over the node. */
    SCHULTZ_STYLE_STATE_FOCUSED,     /**< The node holds keyboard focus. */
    SCHULTZ_STYLE_STATE_CHECKED,     /**< A toggle is on. */
    SCHULTZ_STYLE_STATE_SELECTED,    /**< Selected within a list. */
    SCHULTZ_STYLE_STATE_PRESSED,     /**< The node is being activated. */
    SCHULTZ_STYLE_STATE_DISABLED,    /**< The node cannot be used. */

    SCHULTZ_STYLE_STATE_COUNT        /**< How many state patches exist. */
};

/**
 * @brief Registers a reusable style and returns a handle for it.
 *
 * The patch is copied, so the caller's may be released afterwards. A style is
 * immutable once registered: to change one, register another. Styles are
 * reference counted and released when the last node using one drops it, or
 * when the tree is destroyed.
 *
 * @param tree      The tree to register with. Must not be NULL.
 * @param patch     The properties this style sets. Must not be NULL.
 * @param out_style Receives the style handle. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_style_register(schultz_tree *tree, const schultz_patch *patch,
                               schultz_handle *out_style);

/**
 * @brief Registers a reusable style that also says how it looks in each
 *        interaction state.
 *
 * The same as schultz_style_register, plus the hovered, pressed and disabled
 * looks that go with the plain one. Those belong to the style rather than to
 * the nodes wearing it, so a rule written once is shared by every node that
 * takes the style, and dropping the style with schultz_node_remove_style
 * takes its state rules away too. Set them on the node with
 * schultz_node_set_state_property and neither is true: the rule is copied per
 * node, and removing the style leaves it behind.
 *
 * Where it lands in the cascade: above every layer that has no state, so a
 * hover colour beats the plain one whether that came from a style or from the
 * node; below the node's own state patches, so naming a state on one node
 * still wins, the way an inline property beats a style.
 *
 * Every patch is copied, so the caller's may be released afterwards.
 *
 * @param tree      The tree to register with. Must not be NULL.
 * @param patch     The properties this style sets whatever state a node is
 *                  in. Must not be NULL.
 * @param states    SCHULTZ_STYLE_STATE_COUNT patches, indexed by the
 *                  SCHULTZ_STYLE_STATE_* values, or NULL for a style with no
 *                  state rules. An empty patch means that state adds nothing.
 * @param out_style Receives the style handle. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_style_register_states(schultz_tree *tree,
                                      const schultz_patch *patch,
                                      const schultz_patch *states,
                                      schultz_handle *out_style);

/**
 * @brief Adds a style to a node's ordered list.
 *
 * Later styles override earlier ones. Adding a style already on the node
 * moves nothing and is not an error.
 *
 * @param tree  The tree holding both. Must not be NULL.
 * @param node  The node to style.
 * @param style A style registered with schultz_style_register.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when either is not live, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_node_add_style(schultz_tree *tree, schultz_handle node,
                               schultz_handle style);

/**
 * @brief Removes a style from a node's list.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  The node to update.
 * @param style The style to remove.
 * @return SCHULTZ_OK whether or not it was present, or
 *         SCHULTZ_ERR_INVALID_HANDLE when the node is not live.
 */
int32_t schultz_node_remove_style(schultz_tree *tree, schultz_handle node,
                                  schultz_handle style);

/**
 * @brief Counts the styles applied to a node.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return How many styles are in its list.
 */
uint32_t schultz_node_style_count(const schultz_tree *tree,
                                  schultz_handle node);

/**
 * @brief Sets one property directly on a node.
 *
 * This is the inline patch: styling one node without registering a style or
 * touching the theme. It sits above the node's style list, so it wins.
 *
 * @param tree     The tree holding the node. Must not be NULL.
 * @param node     The node to style.
 * @param property One of the SCHULTZ_PROP_* values.
 * @param value    What to set it to, literal or a token reference.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for an out of range
 *         property, SCHULTZ_ERR_INVALID_HANDLE, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_node_set_style_property(schultz_tree *tree,
                                        schultz_handle node,
                                        uint32_t property,
                                        schultz_value value);

/**
 * @brief Removes one inline property from a node.
 *
 * The property then resolves from the layers beneath again.
 *
 * @param tree     The tree holding the node. Must not be NULL.
 * @param node     The node to update.
 * @param property The property to unset.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_clear_style_property(schultz_tree *tree,
                                          schultz_handle node,
                                          uint32_t property);

/**
 * @brief Sets one property that applies only in a given interaction state.
 *
 * State patches sit above everything else, so a hover colour overrides both
 * the node's inline style and its style list.
 *
 * @param tree     The tree holding the node. Must not be NULL.
 * @param node     The node to style.
 * @param state    One of the SCHULTZ_STYLE_STATE_* values.
 * @param property One of the SCHULTZ_PROP_* values.
 * @param value    What to set it to.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for an out of range state
 *         or property, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_node_set_state_property(schultz_tree *tree,
                                        schultz_handle node, uint32_t state,
                                        uint32_t property,
                                        schultz_value value);

/**
 * @brief Drops every style, inline property and state property from a node.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node The node to strip.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_clear_style(schultz_tree *tree, schultz_handle node);

/**
 * @brief Installs the theme the tree resolves tokens through.
 *
 * The theme is copied. Every node is marked style dirty, since any of them
 * may resolve a token that changed.
 *
 * @param tree  The tree to update. Must not be NULL.
 * @param theme The theme to install, or NULL to fall back to the built in
 *              token values throughout.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when tree is NULL.
 */
int32_t schultz_tree_set_theme(schultz_tree *tree,
                               const schultz_theme *theme);

/**
 * @brief Gives one subtree its own theme.
 *
 * Resolution walks up to the nearest ancestor carrying a theme and falls back
 * to the tree's. This is what makes a dark sidebar in a light application
 * expressible without restating every property.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  The node whose subtree gets the theme.
 * @param theme The theme to install, copied, or NULL to remove one.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_node_set_theme(schultz_tree *tree, schultz_handle node,
                               const schultz_theme *theme);

/**
 * @brief Returns a node's fully resolved style.
 *
 * This is what drawing code reads, and the only thing it should read. The
 * pointer is valid until the node is destroyed; its contents change whenever
 * the node is resolved.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node The node to query.
 * @return The resolved style, or NULL when the node is not live.
 */
const schultz_resolved_style *schultz_node_resolved(const schultz_tree *tree,
                                                    schultz_handle node);

/**
 * @brief Recomputes the resolved style of every node that needs it.
 *
 * Walks the tree in pre-order, resolving nodes marked style dirty and any
 * node beneath one whose inherited properties changed. Comparing the old
 * resolved style with the new one is what decides whether a node needs a
 * relayout or only a repaint.
 *
 * Call once per frame, before layout and paint.
 *
 * @param tree The tree to resolve. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when tree is NULL, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_tree_resolve_styles(schultz_tree *tree);

/**
 * @brief Reports whether a node's resolved style is stale.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return 1 when the node must be resolved again, 0 otherwise.
 */
int32_t schultz_node_style_dirty(const schultz_tree *tree,
                                 schultz_handle node);

/**
 * @brief Reports whether a node, or anything under it, needs laying out.
 *
 * Set by resolution when a layout property changed, and by anything else that
 * invalidates geometry. It reaches the ancestors as well as the node itself,
 * so asking the root is how a host finds out whether laying anything out is
 * worth the time, without walking the tree to look.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return 1 when layout must run again somewhere at or below it, 0 otherwise.
 */
int32_t schultz_node_layout_dirty(const schultz_tree *tree,
                                  schultz_handle node);

/**
 * @brief Marks a node as needing layout again.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node The node to mark.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_invalidate_layout(schultz_tree *tree,
                                       schultz_handle node);

/**
 * @brief Clears every layout dirty mark in the tree.
 *
 * Call after a layout pass, in the same way schultz_tree_clear_dirty is
 * called after painting.
 *
 * @param tree The tree to clear. NULL is accepted and does nothing.
 */
void schultz_tree_clear_layout_dirty(schultz_tree *tree);

/* ---------------------------------------------------------- overlays */

/**
 * @brief Registers a node as an overlay layer.
 *
 * A context menu is not a child of the thing it pops from. It must escape
 * that parent's bounds and clipping, size itself to its own content, and draw
 * above everything. Forcing that into the content tree means fighting it, so
 * the root instead owns an ordered list of overlays. Each is its own layout
 * root, positioned by anchor logic rather than by a parent pane.
 *
 * The node should be a child of the root. Its placement is worked out in
 * window coordinates and converted into the root's before it is stored, so a
 * root that has been inset for a camera notch places overlays correctly
 * rather than that much too low.
 *
 * An overlay is kept inside the safe area, which is the root's own box: one
 * nudged under a notch is on screen and cannot be read. A node that covers
 * the window instead, such as a dialog's scrim, says so with
 * schultz_node_set_fills_viewport and gets the whole window including the
 * notch.
 *
 * Overlays are searched first during hit testing, topmost down.
 *
 * @param tree     The tree to register with. Must not be NULL.
 * @param node     The node to raise to an overlay layer.
 * @param captures Nonzero when a press outside this overlay should dismiss it
 *                 and be consumed rather than reaching what is underneath,
 *                 which is what a menu wants.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, SCHULTZ_ERR_OUT_OF_MEMORY,
 *         or SCHULTZ_ERR_EXHAUSTED when the node is already an overlay.
 */
int32_t schultz_tree_push_overlay(schultz_tree *tree, schultz_handle node,
                                  int32_t captures);

/**
 * @brief Removes the topmost overlay from the list.
 *
 * The node itself is untouched; destroying or hiding it is the caller's
 * business.
 *
 * @param tree    The tree to pop from. Must not be NULL.
 * @param out_node Receives the removed overlay, or NULL if not wanted.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or SCHULTZ_ERR_EXHAUSTED
 *         when there are no overlays.
 */
int32_t schultz_tree_pop_overlay(schultz_tree *tree,
                                 schultz_handle *out_node);

/**
 * @brief Counts the overlay layers.
 *
 * @param tree The tree to query. NULL yields 0.
 * @return How many overlays are registered.
 */
uint32_t schultz_tree_overlay_count(const schultz_tree *tree);

/**
 * @brief Returns one overlay by position.
 *
 * @param tree       The tree to query. Must not be NULL.
 * @param index      Zero is the bottom overlay; the highest index is topmost.
 * @param out_node   Receives the overlay node. Must not be NULL.
 * @param out_captures Receives whether it captures input, or NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when index is out of
 *         range.
 */
int32_t schultz_tree_overlay_at(const schultz_tree *tree, uint32_t index,
                                schultz_handle *out_node,
                                int32_t *out_captures);

/* ------------------------------------------------------- host binding */

/**
 * @brief Attaches the host's opaque token to a node.
 *
 * The toolkit never interprets this. It is handed back with every event
 * aimed at the node, and the host uses it to find whatever listener it
 * registered. Storing a token rather than a callback is what keeps the
 * toolkit from having to trace host memory.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  The node to tag.
 * @param token Any value meaningful to the host. Zero means no listener.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_token(schultz_tree *tree, schultz_handle node,
                               uint64_t token);

/**
 * @brief Reads a node's host token.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return The token, or 0 when none is set.
 */
uint64_t schultz_node_get_token(const schultz_tree *tree,
                                schultz_handle node);

/* ------------------------------------------------------------ layout */

/**
 * @brief Gives a node a pane, which decides how its children are placed.
 *
 * A node with no pane is a leaf: it measures to its preferred size hints and
 * places no children.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   The node to configure.
 * @param vtable The pane implementation, or NULL to make the node a leaf.
 *               The vtable must outlive the node; the built in panes have
 *               static storage duration.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_pane(schultz_tree *tree, schultz_handle node,
                              const schultz_pane_vtable *vtable);

/**
 * @brief Returns the pane a node uses.
 *
 * @param tree The tree holding the node. NULL yields NULL.
 * @param node The node to query.
 * @return The pane vtable, or NULL when the node is a leaf.
 */
const schultz_pane_vtable *schultz_node_get_pane(const schultz_tree *tree,
                                                 schultz_handle node);

/**
 * @brief Sets how big a node would like to be.
 *
 * A wish, not a rule. It says what to be when nothing else decides, and a
 * pane is entitled to decide otherwise: a box pane stretches its children
 * across the cross axis by default, which is why a row of buttons comes out
 * the same height. For a size that holds whatever the pane thinks, say so
 * with schultz_node_set_min_size and schultz_node_set_max_size.
 *
 * Both axes are set. SCHULTZ_SIZE_UNSET on either leaves that one to be
 * measured from the content, which is what a wrapped label wants: a width to
 * wrap inside, and whatever height that turns out to need.
 *
 * Marks the node dirty, since a size change can alter what is painted.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   The node to size.
 * @param width  Wanted width, or SCHULTZ_SIZE_UNSET to measure it.
 * @param height Wanted height, or SCHULTZ_SIZE_UNSET to measure it.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_pref_size(schultz_tree *tree, schultz_handle node,
                                   float width, float height);

/**
 * @brief Sets the size a node may never go below.
 *
 * A rule, unlike a preferred size. It binds wherever the node is placed, so
 * a pane that would have squeezed it smaller does not.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   The node to constrain.
 * @param width  Smallest width, or SCHULTZ_SIZE_UNSET for no minimum.
 * @param height Smallest height, or SCHULTZ_SIZE_UNSET for no minimum.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_min_size(schultz_tree *tree, schultz_handle node,
                                  float width, float height);

/**
 * @brief Sets the size a node may never go above.
 *
 * A rule, unlike a preferred size, and the one that answers "this wide,
 * whatever the column thinks". A maximum on its own caps a stretched child
 * without pinning it; a maximum equal to a minimum pins it exactly.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   The node to constrain.
 * @param width  Largest width, or SCHULTZ_SIZE_UNSET for no maximum.
 * @param height Largest height, or SCHULTZ_SIZE_UNSET for no maximum.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_max_size(schultz_tree *tree, schultz_handle node,
                                  float width, float height);

/**
 * @brief Reads back the size a node would like to be.
 *
 * @param tree       The tree holding the node. Must not be NULL.
 * @param node       The node to query.
 * @param out_width  Receives the width, or SCHULTZ_SIZE_UNSET. May be NULL.
 * @param out_height Receives the height, or SCHULTZ_SIZE_UNSET. May be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_get_pref_size(const schultz_tree *tree,
                                   schultz_handle node, float *out_width,
                                   float *out_height);

/**
 * @brief Reads back the size a node may never go below.
 *
 * @param tree       The tree holding the node. Must not be NULL.
 * @param node       The node to query.
 * @param out_width  Receives the width. Zero means no minimum. May be NULL.
 * @param out_height Receives the height. Zero means no minimum. May be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_get_min_size(const schultz_tree *tree,
                                  schultz_handle node, float *out_width,
                                  float *out_height);

/**
 * @brief Reads back the size a node may never go above.
 *
 * @param tree       The tree holding the node. Must not be NULL.
 * @param node       The node to query.
 * @param out_width  Receives the width, or SCHULTZ_SIZE_UNSET. May be NULL.
 * @param out_height Receives the height, or SCHULTZ_SIZE_UNSET. May be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_get_max_size(const schultz_tree *tree,
                                  schultz_handle node, float *out_width,
                                  float *out_height);

/**
 * @brief Sets the per child layout properties the parent pane reads.
 *
 * Which members matter depends on the parent's kind: a box reads grow and
 * align, a grid reads row and column, an absolute pane reads x and y. A
 * member the parent does not use is ignored.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   The child to configure.
 * @param params The new properties. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when params is NULL, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_layout_params(schultz_tree *tree,
                                       schultz_handle node,
                                       const schultz_layout_params *params);

/**
 * @brief Reads a node's per child layout properties.
 *
 * @param tree       The tree holding the node. Must not be NULL.
 * @param node       The node to query.
 * @param out_params Receives the properties. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when out_params is NULL,
 *         or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_get_layout_params(const schultz_tree *tree,
                                       schultz_handle node,
                                       schultz_layout_params *out_params);

/**
 * @brief Sets the padding inside a pane and the gap between its children.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    The pane to configure.
 * @param padding Space between the pane's edge and its children, on all four
 *                sides. Negative values are treated as zero.
 * @param gap     Space between adjacent children. Negative is treated as
 *                zero. Panes that do not stack children ignore it.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_spacing(schultz_tree *tree, schultz_handle node,
                                 float padding, float gap);

/**
 * @brief Defines a GridPane's rows and columns.
 *
 * The definitions are copied. Counts above SCHULTZ_GRID_MAX_TRACKS are
 * clamped to it. Passing zero counts clears the definitions, after which the
 * grid derives a single content sized track per axis.
 *
 * @param tree         The tree holding the node. Must not be NULL.
 * @param node         The grid to configure.
 * @param columns      Column definitions, or NULL when column_count is zero.
 * @param column_count How many columns.
 * @param rows         Row definitions, or NULL when row_count is zero.
 * @param row_count    How many rows.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_node_set_grid_tracks(schultz_tree *tree, schultz_handle node,
                                     const schultz_grid_track *columns,
                                     uint32_t column_count,
                                     const schultz_grid_track *rows,
                                     uint32_t row_count);

/**
 * @brief Returns a grid's column or row definitions.
 *
 * @param tree      The tree holding the node. Must not be NULL.
 * @param node      The grid to query.
 * @param want_rows Nonzero for the rows, zero for the columns.
 * @param out_count Receives how many there are. Must not be NULL.
 * @return The definitions, owned by the node, or NULL when none are set.
 */
const schultz_grid_track *schultz_node_get_grid_tracks(
    const schultz_tree *tree, schultz_handle node, int32_t want_rows,
    uint32_t *out_count);

/**
 * @brief Reads a pane's padding and gap.
 *
 * @param tree        The tree holding the node. Must not be NULL.
 * @param node        The node to query.
 * @param out_padding Receives the padding, or NULL if not wanted.
 * @param out_gap     Receives the gap, or NULL if not wanted.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_get_spacing(const schultz_tree *tree,
                                 schultz_handle node, float *out_padding,
                                 float *out_gap);

/* -------------------------------------------------- accessibility schema */

/**
 * @brief Sets what a node is, for assistive technology.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node The node to classify.
 * @param role One of the SCHULTZ_ROLE_* values.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_role(schultz_tree *tree, schultz_handle node,
                              uint32_t role);

/**
 * @brief Reads a node's role.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_ROLE_UNKNOWN.
 * @param node The node to query.
 * @return The role.
 */
uint32_t schultz_node_get_role(const schultz_tree *tree,
                               schultz_handle node);

/**
 * @brief Sets a node's accessible name.
 *
 * This is what a screen reader announces: a button's label, an image's
 * alternative text. The string is copied.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node The node to name.
 * @param name A NUL terminated string, or NULL to clear the name.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_node_set_name(schultz_tree *tree, schultz_handle node,
                              const char *name);

/**
 * @brief Reads a node's accessible name.
 *
 * @param tree The tree holding the node. NULL yields NULL.
 * @param node The node to query.
 * @return The name, owned by the node and valid until it is changed or the
 *         node is destroyed, or NULL when unset.
 */
const char *schultz_node_get_name(const schultz_tree *tree,
                                  schultz_handle node);

/**
 * @brief Sets a node's accessible value.
 *
 * The current contents of a text field, or a slider's position rendered as
 * text. The string is copied.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  The node to update.
 * @param value A NUL terminated string, or NULL to clear the value.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_node_set_value(schultz_tree *tree, schultz_handle node,
                               const char *value);

/**
 * @brief Reads a node's accessible value.
 *
 * @param tree The tree holding the node. NULL yields NULL.
 * @param node The node to query.
 * @return The value, owned by the node, or NULL when unset.
 */
const char *schultz_node_get_value(const schultz_tree *tree,
                                   schultz_handle node);

/**
 * @brief Sets which actions a node supports.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    The node to update.
 * @param actions A bit set of SCHULTZ_ACTION_* values.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_actions(schultz_tree *tree, schultz_handle node,
                                 uint32_t actions);

/**
 * @brief Reads which actions a node supports.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return The bit set of supported actions.
 */
uint32_t schultz_node_get_actions(const schultz_tree *tree,
                                  schultz_handle node);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_NODE_H */
