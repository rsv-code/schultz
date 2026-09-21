/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_node.c
 * @brief Retained widget tree, invalidation, and the accessibility schema.
 */

#include "schultz_font.h"
#include "schultz_layout_internal.h"
#include "schultz_node.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "schultz_handle.h"
#include "schultz_layout.h"
#include "schultz_widget.h"

/** @brief One node in the widget tree. */
typedef struct {
    schultz_handle  self;           /**< This node's own handle. */
    schultz_handle  parent;         /**< Parent, or SCHULTZ_HANDLE_NONE. */
    schultz_handle *children;       /**< Child handles, in paint order. */
    uint32_t        child_count;    /**< Children in use. */
    uint32_t        child_capacity; /**< Slots allocated in children. */

    schultz_rect    bounds;         /**< Position and size, parent relative. */
    uint32_t        state;          /**< Bit set of SCHULTZ_STATE_* values. */

    uint32_t        dirty;          /**< This node's pixels are wrong. */
    uint32_t        subtree_dirty;  /**< This node or a descendant is dirty. */

    const schultz_pane_vtable *pane; /**< Layout behaviour, or NULL if leaf. */
    schultz_size_hints    hints;     /**< Min, preferred and maximum sizes. */
    schultz_layout_params params;    /**< Read by this node's parent pane. */
    float           padding;        /**< Inset from this pane's own edges. */
    float           gap;            /**< Space between adjacent children. */
    schultz_grid_track *columns;    /**< GridPane columns, owned, or NULL. */
    uint32_t        column_count;   /**< How many columns. */
    schultz_grid_track *rows;       /**< GridPane rows, owned, or NULL. */
    uint32_t        row_count;      /**< How many rows. */

    uint32_t        role;           /**< One of SCHULTZ_ROLE_*. */
    char           *name;           /**< Accessible name, owned, or NULL. */
    char           *value;          /**< Accessible value, owned, or NULL. */
    uint32_t        actions;        /**< Bit set of SCHULTZ_ACTION_*. */
    uint64_t        token;          /**< Opaque host listener key. */

    const void     *widget;         /**< Widget vtable, or NULL. Not owned. */
    void           *widget_data;    /**< Widget state, owned by the node. */
    uint32_t        clips_children; /**< Nonzero when children are clipped. */
    uint32_t        fills_viewport; /**< Nonzero to track the window's size. */
    uint32_t        keeps_focus;    /**< Nonzero when a press leaves focus. */
    uint32_t        anchored;       /**< Nonzero to sit beside anchor. */
    uint32_t        placed;         /**< The side it actually ended up on. */
    float           anchor_gap;     /**< Clearance from the anchor. */
    uint32_t        anchor_centred; /**< Centre on the anchor across it. */
    uint32_t        anchor_flips;   /**< Go to the other side rather than
                                         be nudged over the anchor. */
    /**
     * Nonzero when something other than a pane decides where this node
     * goes, so no pane should place it. Zero, which is what a node is born
     * with, means a pane places it like any other child.
     */
    uint32_t        places_itself;
    schultz_rect    anchor;         /**< What it sits beside. */
    uint32_t        placement;      /**< One of SCHULTZ_PLACE_*. */
    /** The safe area it was last placed against, so it is placed again when
     *  that changes and not on every frame. */
    schultz_rect    anchor_window;
    uint32_t        pointer_off;    /**< Nonzero when it is not a hit target. */
    float           paint_margin;   /**< How far its painting exceeds bounds. */
    /**
     * How far this node's shadow reaches past its bounds, or zero when it
     * casts none. Worked out when the style resolves, because that is where
     * the shadow's distance and blur are known, and read when anything in
     * the subtree is marked.
     */
    float           shadow_reach;
    schultz_point   scroll;         /**< How far its children are shifted. */
    uint32_t        cursor;         /**< Pointer shape shown over it. */

    schultz_handle *styles;         /**< Ordered style handles, or NULL. */
    uint32_t        style_count;    /**< Styles applied. */
    uint32_t        style_capacity; /**< Slots allocated in styles. */
    schultz_patch   inline_patch;   /**< Properties set directly on the node. */
    schultz_patch   state_patches[SCHULTZ_STYLE_STATE_COUNT]; /**< Per state. */
    schultz_theme  *theme;          /**< Subtree theme, owned, or NULL. */
    schultz_resolved_style resolved; /**< What drawing code reads. */
    uint32_t        style_dirty;    /**< The resolved struct is stale. */
    uint32_t        layout_dirty;   /**< Measure and arrange must run again. */
    uint32_t        layout_below;   /**< Something in the subtree is layout dirty. */
    uint32_t        style_below;    /**< Something in the subtree is style dirty. */
} schultz_node;

/** @brief Every node, plus the accumulated dirty state for the frame. */
/** @brief A registered, immutable, reference counted style. */
typedef struct {
    schultz_patch patch;     /**< The properties it sets. */
    /**
     * What it sets in each interaction state, indexed by
     * SCHULTZ_STYLE_STATE_*. Empty patches for a style that has no state
     * rules, which is most of them.
     */
    schultz_patch states[SCHULTZ_STYLE_STATE_COUNT];
    uint32_t      references;/**< How many nodes use it. */
} schultz_style_object;

/* Defined with the rest of the style code, and needed by tree teardown. */
static void schultz_style_free(schultz_style_object *style);

/** @brief One registered overlay layer. */
typedef struct {
    schultz_handle node;     /**< The overlay's root node. */
    int32_t        captures; /**< Nonzero when it swallows outside presses. */
} schultz_overlay;

/** @brief Every node, the overlay layers, and the frame's dirty state. */
struct schultz_tree {
    schultz_handle_table nodes;        /**< Maps a handle to a schultz_node. */
    schultz_handle       root;         /**< The permanent root node. */
    schultz_overlay     *overlays;     /**< Ordered list, topmost last. */
    uint32_t             overlay_count;/**< Overlays in use. */
    uint32_t             overlay_capacity; /**< Slots allocated. */
    schultz_rect         viewport;     /**< Clips every dirty region. */
    float                pixel_scale;  /**< Screen pixels per unit. */
    /*
     * What needs repainting, as a few rectangles rather than one. See
     * schultz_tree_add_dirty and docs/repainting.md.
     */
    schultz_rect         dirty[SCHULTZ_TREE_DIRTY_PARTS];
    uint32_t             dirty_count;
    schultz_handle_table styles;       /**< Registered style objects. */
    void                *fonts;        /**< Font system for text widgets. */
    void                *images;       /**< Image table for image widgets. */
    void                *glyphs;       /**< Rasterized glyphs, for text. */
    void                *resources;    /**< Gradients and dash patterns. */
    void                *audio;        /**< Sound system for video widgets. */
    /*
     * The nodes asking to be ticked, and when they last were. Kept as a list
     * rather than a flag on every node so advancing costs what is animating
     * rather than what exists.
     */
    schultz_handle      *animating;          /**< Nodes asking to be ticked. */
    uint32_t             animating_count;    /**< How many there are. */
    uint32_t             animating_capacity; /**< How many fit. */
    uint64_t             last_tick;    /**< When advance last ran. */
    uint32_t             ticked;       /**< Set once a starting point exists. */
    /*
     * The node whose text is currently selected. Copy is aimed here rather
     * than at whatever holds focus, because a block of prose is selectable
     * without being a tab stop, which is how a browser behaves.
     */
    schultz_handle       selection;    /**< Node owning the text selection. */
    /*
     * Where the pointer was last seen, so a widget that appears near it can
     * find it. Kept on the tree rather than asked of the event router,
     * because a widget is ticked with the tree and has no way to reach the
     * router at all.
     */
    schultz_point        pointer;      /**< Last position, in tree space. */
    uint32_t             pointer_seen; /**< Nonzero once one has been seen. */
    schultz_clipboard_offer_fn  clip_offer; /**< Host clipboard writer. */
    schultz_clipboard_take_fn   clip_take;  /**< Host clipboard reader. */
    schultz_clipboard_holds_fn  clip_holds; /**< Asks without fetching. */
    void                       *clip_context; /**< Passed to both. */
    schultz_theme        theme;        /**< Tokens the tree resolves through. */
};

/* ------------------------------------------------------------- resolution */

static schultz_node *schultz_node_get(const schultz_tree *tree,
                                      schultz_handle handle)
{
    void *object = NULL;

    if (tree == NULL) {
        return NULL;
    }
    if (schultz_handle_table_lookup(&tree->nodes, handle, &object)
            != SCHULTZ_OK) {
        return NULL;
    }
    return (schultz_node *)object;
}

/* Style marking is defined further down, next to the rest of the style code,
 * but geometry and state changes above need it. */
static void schultz_node_mark_style(schultz_tree *tree, schultz_node *node);
static void schultz_node_mark_style_below(schultz_tree *tree,
                                          schultz_node *node);
static void schultz_node_mark_style_subtree(schultz_tree *tree,
                                            schultz_node *node);

/* ------------------------------------------------------------ dirty marks */

/* The smallest rectangle covering both. */
static schultz_rect schultz_rect_cover(schultz_rect a, schultz_rect b)
{
    float left   = (a.x < b.x) ? a.x : b.x;
    float top    = (a.y < b.y) ? a.y : b.y;
    float right  = (schultz_rect_right(a) > schultz_rect_right(b))
                       ? schultz_rect_right(a) : schultz_rect_right(b);
    float bottom = (schultz_rect_bottom(a) > schultz_rect_bottom(b))
                       ? schultz_rect_bottom(a) : schultz_rect_bottom(b);

    return schultz_rect_make(left, top, right - left, bottom - top);
}

static float schultz_rect_area(schultz_rect r)
{
    return r.width * r.height;
}

/* How much is wasted by covering both with one rectangle. */
static float schultz_rect_waste(schultz_rect a, schultz_rect b)
{
    return schultz_rect_area(schultz_rect_cover(a, b)) -
           schultz_rect_area(a) - schultz_rect_area(b);
}

/*
 * Adds a rectangle to what needs repainting.
 *
 * A few rectangles rather than one, and the whole of why is in
 * docs/repainting.md. The short version: one rectangle means two small
 * changes at opposite corners repaint everything between them, and measured
 * on the demo that was 483,000 pixels a frame for 4,100 pixels of change.
 *
 * Two rules, and the second is the one that keeps this honest.
 *
 * **Merge when it is nearly free.** Two rectangles close together cost less
 * as one, because a rectangle is not free: measured, about 0.17 ms of fixed
 * cost against about 0.000005 ms a pixel. Anything under a few thousand
 * wasted pixels is cheaper merged, and SCHULTZ_TREE_DIRTY_WASTE is that line.
 *
 * **Never exceed the list.** When it is full, the pair that wastes least is
 * merged to make room. That is what bounds the worst case: however scattered
 * a frame gets, this degrades to the single rectangle it used to be rather
 * than to something worse.
 */
static void schultz_tree_add_dirty(schultz_tree *tree, schultz_rect rect)
{
    schultz_rect clipped = schultz_rect_intersect(tree->viewport, rect);
    uint32_t i;
    uint32_t best = 0u;
    float least = 0.0f;

    if (schultz_rect_is_empty(clipped)) {
        return;
    }

    /*
     * Into whichever it costs least to join, when that is cheap enough. A
     * rectangle already covering the new one costs nothing, which is the
     * common case: a node repainting in the same place every frame.
     */
    for (i = 0; i < tree->dirty_count; i++) {
        float waste = schultz_rect_waste(tree->dirty[i], clipped);

        if (i == 0u || waste < least) {
            least = waste;
            best  = i;
        }
    }
    if (tree->dirty_count > 0u && least <= SCHULTZ_TREE_DIRTY_WASTE) {
        tree->dirty[best] = schultz_rect_cover(tree->dirty[best], clipped);
        return;
    }

    if (tree->dirty_count < SCHULTZ_TREE_DIRTY_PARTS) {
        tree->dirty[tree->dirty_count] = clipped;
        tree->dirty_count++;
        return;
    }

    /*
     * Full. The two that waste least by becoming one make room, and the new
     * rectangle takes the slot that frees up.
     */
    {
        uint32_t keep = 0u;
        uint32_t drop = 1u;
        uint32_t j;

        least = schultz_rect_waste(tree->dirty[0], tree->dirty[1]);
        for (i = 0; i < tree->dirty_count; i++) {
            for (j = i + 1u; j < tree->dirty_count; j++) {
                float waste = schultz_rect_waste(tree->dirty[i],
                                                 tree->dirty[j]);

                if (waste < least) {
                    least = waste;
                    keep  = i;
                    drop  = j;
                }
            }
        }
        tree->dirty[keep] = schultz_rect_cover(tree->dirty[keep],
                                               tree->dirty[drop]);
        tree->dirty[drop] = clipped;
    }
}

/* Propagates subtree_dirty from a node up to the root. */
static void schultz_node_mark_ancestors(schultz_tree *tree,
                                        schultz_node *node)
{
    schultz_node *walk = node;

    while (walk != NULL) {
        if (walk->subtree_dirty) {
            /* Already marked, so everything above it is marked too. */
            return;
        }
        walk->subtree_dirty = 1;
        walk = schultz_node_get(tree, walk->parent);
    }
}

/*
 * Absolute bounds of a node, accumulating every ancestor's origin, and
 * whether the node is in the tree at all.
 *
 * Both answers come out of the same walk, because they are the same walk:
 * adding up the ancestors' origins is also what discovers whether the chain
 * ends at the root or at a node that was detached from it. Pass NULL for
 * out_attached when only the rectangle is wanted.
 */
static schultz_rect schultz_node_absolute(const schultz_tree *tree,
                                          const schultz_node *node,
                                          int32_t *out_attached)
{
    schultz_rect bounds = node->bounds;
    const schultz_node *top = node;
    const schultz_node *walk = schultz_node_get(tree, node->parent);

    /*
     * Scrolling is a shift of where an ancestor's children sit, not a change
     * to their bounds, so it is folded in here. Painting, hit testing and the
     * dirty region all ask this one function where a node is, which is what
     * keeps them agreeing: there is no second place doing the arithmetic.
     */
    while (walk != NULL) {
        bounds.x += walk->bounds.x - walk->scroll.x;
        bounds.y += walk->bounds.y - walk->scroll.y;
        top  = walk;
        walk = schultz_node_get(tree, walk->parent);
    }
    if (out_attached != NULL) {
        *out_attached = (int32_t)(top->self == tree->root);
    }
    return bounds;
}

/*
 * Marks a node's own area dirty and propagates the subtree flag upward. Only
 * the node's own rectangle is added: a descendant that extends outside its
 * parent contributes its own rectangle when it is marked.
 */
/*
 * Marks a node as needing layout and tells every ancestor that something
 * below it does. Without the second half a host would have to ask every node
 * in the tree whether anything had changed, which is the question it wants to
 * ask the root exactly once.
 */
static void schultz_node_mark_layout(schultz_tree *tree, schultz_node *node)
{
    const schultz_node *walk;

    node->layout_dirty = 1;
    walk = schultz_node_get(tree, node->parent);
    while (walk != NULL) {
        ((schultz_node *)walk)->layout_below = 1;
        walk = schultz_node_get(tree, walk->parent);
    }
}

/*
 * Tells every ancestor that something below it needs resolving, which is what
 * lets the resolve walk skip a clean branch instead of visiting every node in
 * the tree every frame. The mirror of layout_below, and it walks all the way
 * to the root rather than stopping at the first ancestor already marked: the
 * saving is not worth an invariant that has to hold while the walk is also
 * clearing the flag on its way back up.
 */
static void schultz_node_mark_style_below(schultz_tree *tree,
                                          schultz_node *node)
{
    const schultz_node *walk = schultz_node_get(tree, node->parent);

    while (walk != NULL) {
        ((schultz_node *)walk)->style_below = 1;
        walk = schultz_node_get(tree, walk->parent);
    }
}

static void schultz_node_mark(schultz_tree *tree, schultz_node *node)
{
    int32_t attached = 0;
    schultz_rect bounds;

    node->dirty = 1;
    schultz_node_mark_ancestors(tree, node);
    bounds = schultz_node_absolute(tree, node, &attached);
    /*
     * A detached node has no place on screen, and the bounds above are
     * whatever it was last given by the parent it no longer has. Adding that
     * rectangle would repaint a piece of the window where nothing changed,
     * every time a host touched a widget it had taken out of the tree. The
     * flags above are still set, so putting it back marks it properly.
     */
    if (!attached) {
        return;
    }
    /*
     * The margin is what a widget paints outside its own bounds, a focus ring
     * being the case that made it necessary. Invalidating only the bounds
     * leaves the part that pokes out unrepainted until something else happens
     * to cover it.
     */
    /*
     * A shadow is cast by the whole subtree drawn as one picture, so any
     * change inside that subtree changes the shadow, and the shadow lies
     * outside the bounds of whatever changed. Repainting only the piece that
     * changed leaves the old shadow on screen.
     *
     * So a change inside a node that casts a shadow dirties the whole of that
     * node, plus the reach of its shadow. The outermost one wins, since a
     * shadow inside a shadow is the outer one's picture changing too.
     *
     * Nothing like this is needed for opacity on its own: a faded subtree
     * composes correctly from whatever the culling left in it, because
     * anything that reaches the repainted area is in there by definition.
     * A shadow is different because it moves pixels outside that area.
     */
    {
        schultz_node *up = node;
        schultz_node *caster = NULL;

        while (up != NULL) {
            if (up->shadow_reach > 0.0f) {
                caster = up;
            }
            up = schultz_node_get(tree, up->parent);
        }
        if (caster != NULL && caster != node) {
            int32_t whole = 0;
            schultz_rect cover = schultz_node_absolute(tree, caster, &whole);

            if (whole) {
                /*
                 * Both, not just the caster's own rectangle. A child may
                 * hang outside its parent, and the shadow is cast by the
                 * picture rather than by the parent's bounds, so the part of
                 * the shadow that child casts lies outside the parent too.
                 */
                bounds = schultz_rect_cover(cover, bounds);
                node   = caster;
            }
        }
    }
    {
        float reach = (node->shadow_reach > node->paint_margin)
                          ? node->shadow_reach : node->paint_margin;

        schultz_tree_add_dirty(tree, schultz_rect_expand(bounds, reach));
    }
}

/*
 * Marks a whole subtree dirty. Needed whenever an ancestor moves or is
 * removed, because every descendant's absolute position changed with it.
 */
static void schultz_node_mark_subtree(schultz_tree *tree, schultz_node *node)
{
    uint32_t i;

    schultz_node_mark(tree, node);
    for (i = 0; i < node->child_count; i++) {
        schultz_node *child = schultz_node_get(tree, node->children[i]);
        if (child != NULL) {
            schultz_node_mark_subtree(tree, child);
        }
    }
}

/* True when candidate is node or one of node's descendants. Defined below. */
static int32_t schultz_node_contains(const schultz_tree *tree,
                                     schultz_handle node,
                                     schultz_handle candidate);

/* ------------------------------------------------------ child list helpers */

static int32_t schultz_node_add_child(schultz_node *parent,
                                      schultz_handle child)
{
    if (parent->child_count == parent->child_capacity) {
        uint32_t capacity = (parent->child_capacity == 0)
                                ? 4u : parent->child_capacity * 2u;
        schultz_handle *children = (schultz_handle *)realloc(
            parent->children, (size_t)capacity * sizeof(*children));
        if (children == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        parent->children       = children;
        parent->child_capacity = capacity;
    }
    parent->children[parent->child_count++] = child;
    return SCHULTZ_OK;
}

static void schultz_node_drop_child(schultz_node *parent,
                                    schultz_handle child)
{
    uint32_t i;

    for (i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child) {
            /* Shift the rest down: paint order must be preserved. */
            memmove(&parent->children[i], &parent->children[i + 1],
                    (size_t)(parent->child_count - i - 1) *
                        sizeof(*parent->children));
            parent->child_count--;
            return;
        }
    }
}

/* --------------------------------------------------------- tree lifecycle */

static int32_t schultz_node_new(schultz_tree *tree, schultz_handle parent,
                                schultz_handle *out_node)
{
    schultz_node *node;
    int32_t result;
    uint32_t i;

    node = (schultz_node *)calloc(1, sizeof(*node));
    if (node == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    node->parent = parent;
    node->state  = SCHULTZ_STATE_DEFAULT;
    node->role   = SCHULTZ_ROLE_UNKNOWN;
    schultz_size_hints_default(&node->hints);
    schultz_layout_params_default(&node->params);
    schultz_patch_init(&node->inline_patch);
    for (i = 0; i < SCHULTZ_STYLE_STATE_COUNT; i++) {
        schultz_patch_init(&node->state_patches[i]);
    }
    /* A new node starts stale, so it resolves before it is first painted. */
    node->style_dirty  = 1;
    node->layout_dirty = 1;

    result = schultz_handle_table_insert(&tree->nodes, node, &node->self);
    if (result != SCHULTZ_OK) {
        free(node);
        return result;
    }

    *out_node = node->self;
    return SCHULTZ_OK;
}

static void schultz_node_free(schultz_node *node)
{
    if (node == NULL) {
        return;
    }
    free(node->children);
    free(node->name);
    free(node->value);
    free(node->columns);
    free(node->rows);
    if (node->widget != NULL && node->widget_data != NULL) {
        const schultz_widget_vtable *v =
            (const schultz_widget_vtable *)node->widget;
        if (v->destroy != NULL) {
            v->destroy(node->widget_data);
        }
    }
    free(node->styles);
    free(node->theme);
    schultz_patch_free(&node->inline_patch);
    {
        uint32_t i;
        for (i = 0; i < SCHULTZ_STYLE_STATE_COUNT; i++) {
            schultz_patch_free(&node->state_patches[i]);
        }
    }
    free(node);
}

int32_t schultz_tree_create(schultz_tree **out_tree)
{
    schultz_tree *tree;
    int32_t result;

    if (out_tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    tree = (schultz_tree *)calloc(1, sizeof(*tree));
    if (tree == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    result = schultz_handle_table_init(&tree->nodes, 32);
    if (result != SCHULTZ_OK) {
        free(tree);
        return result;
    }
    result = schultz_handle_table_init(&tree->styles, 8);
    if (result != SCHULTZ_OK) {
        schultz_handle_table_free(&tree->nodes);
        free(tree);
        return result;
    }
    schultz_theme_init(&tree->theme);

    /* An unbounded viewport until one is set, so nothing is clipped away. */
    tree->viewport = schultz_rect_make(-1.0e9f, -1.0e9f, 2.0e9f, 2.0e9f);

    result = schultz_node_new(tree, SCHULTZ_HANDLE_NONE, &tree->root);
    if (result != SCHULTZ_OK) {
        schultz_handle_table_free(&tree->nodes);
        free(tree);
        return result;
    }
    schultz_node_get(tree, tree->root)->role = SCHULTZ_ROLE_WINDOW;

    *out_tree = tree;
    return SCHULTZ_OK;
}

void schultz_tree_destroy(schultz_tree *tree)
{
    uint32_t i;

    if (tree == NULL) {
        return;
    }
    for (i = 0; i < tree->nodes.capacity; i++) {
        if (tree->nodes.slots[i].live) {
            schultz_node_free((schultz_node *)tree->nodes.slots[i].object);
        }
    }
    schultz_handle_table_free(&tree->nodes);

    for (i = 0; i < tree->styles.capacity; i++) {
        if (tree->styles.slots[i].live) {
            schultz_style_object *style =
                (schultz_style_object *)tree->styles.slots[i].object;
            schultz_style_free(style);
        }
    }
    schultz_handle_table_free(&tree->styles);
    free(tree->animating);

    free(tree->overlays);
    free(tree);
}

int32_t schultz_tree_push_overlay(schultz_tree *tree, schultz_handle node,
                                  int32_t captures)
{
    uint32_t i;

    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (schultz_node_get(tree, node) == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    for (i = 0; i < tree->overlay_count; i++) {
        if (tree->overlays[i].node == node) {
            return SCHULTZ_ERR_EXHAUSTED; /* already an overlay */
        }
    }

    if (tree->overlay_count == tree->overlay_capacity) {
        uint32_t capacity = (tree->overlay_capacity == 0)
                                ? 4u : tree->overlay_capacity * 2u;
        schultz_overlay *grown = (schultz_overlay *)realloc(
            tree->overlays, (size_t)capacity * sizeof(*grown));
        if (grown == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        tree->overlays         = grown;
        tree->overlay_capacity = capacity;
    }

    tree->overlays[tree->overlay_count].node     = node;
    tree->overlays[tree->overlay_count].captures = captures;
    tree->overlay_count++;
    return SCHULTZ_OK;
}

int32_t schultz_tree_pop_overlay(schultz_tree *tree, schultz_handle *out_node)
{
    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (tree->overlay_count == 0u) {
        return SCHULTZ_ERR_EXHAUSTED;
    }
    tree->overlay_count--;
    if (out_node != NULL) {
        *out_node = tree->overlays[tree->overlay_count].node;
    }
    return SCHULTZ_OK;
}

uint32_t schultz_tree_overlay_count(const schultz_tree *tree)
{
    return (tree == NULL) ? 0 : tree->overlay_count;
}

int32_t schultz_tree_overlay_at(const schultz_tree *tree, uint32_t index,
                                schultz_handle *out_node,
                                int32_t *out_captures)
{
    if (tree == NULL || out_node == NULL || index >= tree->overlay_count) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_node = tree->overlays[index].node;
    if (out_captures != NULL) {
        *out_captures = tree->overlays[index].captures;
    }
    return SCHULTZ_OK;
}

int32_t schultz_node_set_token(schultz_tree *tree, schultz_handle handle,
                               uint64_t token)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    node->token = token;
    return SCHULTZ_OK;
}

uint64_t schultz_node_get_token(const schultz_tree *tree,
                                schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? 0u : node->token;
}

schultz_handle schultz_tree_root(const schultz_tree *tree)
{
    return (tree == NULL) ? SCHULTZ_HANDLE_NONE : tree->root;
}

int32_t schultz_tree_set_viewport(schultz_tree *tree, schultz_rect viewport)
{
    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    tree->viewport = viewport;
    return SCHULTZ_OK;
}

int32_t schultz_tree_set_pixel_scale(schultz_tree *tree, float scale)
{
    if (tree == NULL || scale <= 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    tree->pixel_scale = scale;
    return SCHULTZ_OK;
}

float schultz_tree_pixel_scale(const schultz_tree *tree)
{
    if (tree == NULL || tree->pixel_scale <= 0.0f) {
        return 1.0f;
    }
    return tree->pixel_scale;
}

int32_t schultz_tree_get_viewport(const schultz_tree *tree,
                                  schultz_rect *out_viewport)
{
    if (tree == NULL || out_viewport == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_viewport = tree->viewport;
    return SCHULTZ_OK;
}

int32_t schultz_node_create(schultz_tree *tree, schultz_handle parent_handle,
                            schultz_handle *out_node)
{
    schultz_node *parent;
    schultz_handle handle = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (tree == NULL || out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * SCHULTZ_HANDLE_NONE asks for a node with no parent, which is a node
     * that exists and is not in the tree. Anything else has to name a live
     * node, because a handle that does not resolve is a mistake rather than
     * a request.
     */
    parent = schultz_node_get(tree, parent_handle);
    if (parent == NULL && parent_handle != SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }

    result = schultz_node_new(tree, parent_handle, &handle);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (parent_handle == SCHULTZ_HANDLE_NONE) {
        *out_node = handle;
        return SCHULTZ_OK;
    }

    /* The parent pointer may have moved if the table grew during insert. */
    parent = schultz_node_get(tree, parent_handle);
    result = schultz_node_add_child(parent, handle);
    if (result == SCHULTZ_OK) {
        /* A new node is born style dirty, and it was not attached to
         * anything when that was set, so the ancestors hear about it here. */
        schultz_node_mark_style_below(tree, schultz_node_get(tree, handle));
        /*
         * The parent has a child it did not have, so whatever pane it is
         * running has to place everything again. Without this a widget added
         * after the first layout keeps the empty bounds it was born with and
         * never appears.
         */
        schultz_node_mark_layout(tree, parent);
    }
    if (result != SCHULTZ_OK) {
        schultz_node_free(schultz_node_get(tree, handle));
        schultz_handle_table_remove(&tree->nodes, handle);
        return result;
    }

    *out_node = handle;
    return SCHULTZ_OK;
}

/* Recursively destroys a subtree without touching dirty state. */
static void schultz_node_destroy_recursive(schultz_tree *tree,
                                           schultz_handle handle)
{
    schultz_node *node = schultz_node_get(tree, handle);
    uint32_t i;

    if (node == NULL) {
        return;
    }
    for (i = 0; i < node->child_count; i++) {
        schultz_node_destroy_recursive(tree, node->children[i]);
    }
    schultz_handle_table_remove(&tree->nodes, handle);
    schultz_node_free(node);
}

/*
 * Gives up the two things a node holds on the tree that only mean anything
 * while it is on screen: naming it as the owner of the text selection, and
 * standing it in the overlay list.
 *
 * Both are flat lists on the tree rather than links in it, so nothing about
 * losing a parent reaches them. A node leaving the tree, whether it is being
 * destroyed or only taken out, has to be dropped from them by hand, or copy
 * is aimed at something invisible and the overlay list names a node that is
 * not there.
 *
 * The ticking list is deliberately not touched. That one is the node's own
 * request rather than a place in what is on screen, so a widget that was
 * animating is still animating when it is put back;
 * schultz_tree_advance is what leaves it alone in the meantime.
 */
static void schultz_node_drop_overlay(schultz_tree *tree,
                                      schultz_handle handle)
{
    schultz_node *node = schultz_node_get(tree, handle);
    uint32_t i;

    if (node == NULL) {
        return;
    }
    for (i = 0; i < tree->overlay_count; i++) {
        if (tree->overlays[i].node == handle) {
            memmove(&tree->overlays[i], &tree->overlays[i + 1],
                    (size_t)(tree->overlay_count - i - 1) *
                        sizeof(*tree->overlays));
            tree->overlay_count--;
            break;
        }
    }
    for (i = 0; i < node->child_count; i++) {
        schultz_node_drop_overlay(tree, node->children[i]);
    }
}

static void schultz_node_leave_tree(schultz_tree *tree, schultz_handle handle)
{
    /*
     * The owner may be anywhere inside the subtree, and one walk upward from
     * it answers that for the whole subtree at once, so this is asked here
     * rather than once per node below.
     */
    if (schultz_node_contains(tree, handle, tree->selection)) {
        tree->selection = SCHULTZ_HANDLE_NONE;
    }
    schultz_node_drop_overlay(tree, handle);
}

/*
 * Drops a subtree from the ticking list, for nodes that are being destroyed.
 *
 * Their handles would otherwise sit in the list for the life of the tree.
 * Nothing goes wrong, because a stale handle names no widget and is skipped,
 * but the list only ever grows, and an application that builds and throws
 * away animated widgets pays for every one it ever had.
 */
static void schultz_node_stop_ticking(schultz_tree *tree,
                                      schultz_handle handle)
{
    schultz_node *node = schultz_node_get(tree, handle);
    uint32_t i;

    if (node == NULL) {
        return;
    }
    for (i = 0; i < tree->animating_count; i++) {
        if (tree->animating[i] == handle) {
            /* Order does not matter here, so the last entry fills the gap. */
            tree->animating[i] = tree->animating[tree->animating_count - 1u];
            tree->animating_count--;
            break;
        }
    }
    for (i = 0; i < node->child_count; i++) {
        schultz_node_stop_ticking(tree, node->children[i]);
    }
}

int32_t schultz_node_destroy(schultz_tree *tree, schultz_handle handle)
{
    schultz_node *node;
    schultz_node *parent;

    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (handle == tree->root) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node = schultz_node_get(tree, handle);
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }

    /*
     * Mark before destroying: the area the subtree occupied has to be
     * repainted by whatever is behind it, and after the nodes are gone their
     * rectangles cannot be computed.
     */
    schultz_node_mark_subtree(tree, node);

    parent = schultz_node_get(tree, node->parent);
    if (parent != NULL) {
        schultz_node_drop_child(parent, handle);
        schultz_node_mark_ancestors(tree, parent);
        schultz_node_mark_layout(tree, parent);
    }

    schultz_node_leave_tree(tree, handle);
    schultz_node_stop_ticking(tree, handle);

    schultz_node_destroy_recursive(tree, handle);
    return SCHULTZ_OK;
}

/* True when candidate is node or one of node's descendants. */
static int32_t schultz_node_contains(const schultz_tree *tree,
                                     schultz_handle node,
                                     schultz_handle candidate)
{
    const schultz_node *walk = schultz_node_get(tree, candidate);

    while (walk != NULL) {
        if (walk->self == node) {
            return 1;
        }
        walk = schultz_node_get(tree, walk->parent);
    }
    return 0;
}

int32_t schultz_node_set_parent(schultz_tree *tree, schultz_handle handle,
                                schultz_handle new_parent_handle)
{
    schultz_node *node;
    schultz_node *old_parent;
    schultz_node *new_parent;
    int32_t result;

    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (handle == tree->root) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node = schultz_node_get(tree, handle);
    new_parent = schultz_node_get(tree, new_parent_handle);
    /*
     * SCHULTZ_HANDLE_NONE asks for no parent at all, which takes the node out
     * of the tree and leaves it alive. Any other handle has to resolve.
     */
    if (node == NULL ||
        (new_parent == NULL && new_parent_handle != SCHULTZ_HANDLE_NONE)) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* Reparenting a node under itself would build a cycle. */
    if (schultz_node_contains(tree, handle, new_parent_handle)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (node->parent == new_parent_handle) {
        return SCHULTZ_OK;
    }

    /*
     * The new parent takes it before the old one lets go. That order is what
     * makes a failure here leave the tree exactly as it was: the only step
     * that can fail is the one that grows a child list, and until it has
     * succeeded nothing has been moved. Doing it the other way round would
     * mean an out of memory in the middle of a move, with the node already
     * out of its old parent and nowhere to put it back that keeps its place
     * among its siblings, which is paint order.
     *
     * Being in two child lists for the length of this block is harmless:
     * node->parent still names the old one, and that is what every walk
     * reads.
     */
    if (new_parent != NULL) {
        result = schultz_node_add_child(new_parent, handle);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }

    /* The area it is leaving must be repainted, and it is still there. */
    schultz_node_mark_subtree(tree, node);

    old_parent = schultz_node_get(tree, node->parent);
    if (old_parent != NULL) {
        schultz_node_drop_child(old_parent, handle);
        schultz_node_mark_ancestors(tree, old_parent);
        /* The pane it was in has one child fewer and must place the rest. */
        schultz_node_mark_layout(tree, old_parent);
    }

    if (new_parent != NULL) {
        /* The pane it is joining has one child more. */
        schultz_node_mark_layout(tree, new_parent);
    } else {
        /*
         * Out of the tree. It gives up the selection and the overlay list for
         * the same reason a destroyed node does: both name what is on screen,
         * and this node no longer is. What it asked for itself, such as being
         * ticked, it keeps, so putting it back restores it as it was.
         */
        schultz_node_leave_tree(tree, handle);
    }
    node->parent = new_parent_handle;

    /*
     * And the area it is arriving in, which for a detached node is nowhere:
     * the mark is skipped because the node is no longer in the tree, and the
     * flags it sets are what make it repaint when it is put back.
     */
    schultz_node_mark_subtree(tree, node);
    /* A new parent means new inherited values and possibly a new theme. */
    schultz_node_mark_style_subtree(tree, node);
    /*
     * Its own layout too. A detached subtree is never walked, so nothing
     * would otherwise arrange it when it comes back.
     */
    schultz_node_mark_layout(tree, node);
    return SCHULTZ_OK;
}

int32_t schultz_node_is_attached(const schultz_tree *tree,
                                 schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    int32_t attached = 0;

    if (node == NULL) {
        return 0;
    }
    schultz_node_absolute(tree, node, &attached);
    return attached;
}

int32_t schultz_node_parent(const schultz_tree *tree, schultz_handle handle,
                            schultz_handle *out_parent)
{
    const schultz_node *node;

    if (out_parent == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node = schultz_node_get(tree, handle);
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    *out_parent = node->parent;
    return SCHULTZ_OK;
}

uint32_t schultz_node_child_count(const schultz_tree *tree,
                                  schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? 0 : node->child_count;
}

int32_t schultz_node_child_at(const schultz_tree *tree, schultz_handle handle,
                              uint32_t index, schultz_handle *out_child)
{
    const schultz_node *node;

    if (out_child == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node = schultz_node_get(tree, handle);
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (index >= node->child_count) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_child = node->children[index];
    return SCHULTZ_OK;
}

/* ------------------------------------------------------------- geometry */

int32_t schultz_node_set_bounds(schultz_tree *tree, schultz_handle handle,
                                schultz_rect bounds)
{
    schultz_node *node;

    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node = schultz_node_get(tree, handle);
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (schultz_rect_equals(node->bounds, bounds)) {
        return SCHULTZ_OK;
    }

    /*
     * Both rectangles must be marked. Marking only the new one leaves
     * whatever was last painted at the old position on screen, which is the
     * classic invalidation bug.
     */
    schultz_node_mark_subtree(tree, node);
    node->bounds = bounds;
    schultz_node_mark_subtree(tree, node);
    return SCHULTZ_OK;
}

int32_t schultz_node_get_bounds(const schultz_tree *tree,
                                schultz_handle handle,
                                schultz_rect *out_bounds)
{
    const schultz_node *node;

    if (out_bounds == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node = schultz_node_get(tree, handle);
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    *out_bounds = node->bounds;
    return SCHULTZ_OK;
}

int32_t schultz_node_absolute_bounds(const schultz_tree *tree,
                                     schultz_handle handle,
                                     schultz_rect *out_bounds)
{
    const schultz_node *node;

    if (out_bounds == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node = schultz_node_get(tree, handle);
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    *out_bounds = schultz_node_absolute(tree, node, NULL);
    return SCHULTZ_OK;
}

/* ---------------------------------------------------------------- state */

int32_t schultz_node_set_state(schultz_tree *tree, schultz_handle handle,
                               uint32_t state)
{
    schultz_node *node;

    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node = schultz_node_get(tree, handle);
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (node->state == state) {
        return SCHULTZ_OK; /* no change, so no repaint */
    }

    /*
     * Mark before and after. Losing visibility means the old area must be
     * repainted without this node; gaining it means the new area must be
     * painted with it.
     */
    schultz_node_mark_subtree(tree, node);
    node->state = state;
    schultz_node_mark_subtree(tree, node);
    /* State selects which state patches apply, so the style is now stale. */
    schultz_node_mark_style(tree, node);
    return SCHULTZ_OK;
}

uint32_t schultz_node_get_state(const schultz_tree *tree,
                                schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? 0 : node->state;
}

int32_t schultz_node_is_visible(const schultz_tree *tree,
                                schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);

    /*
     * A node that does not exist is not visible. Without this the walk below
     * would fall straight through and report visible, because the loop that
     * terminates at the root also terminates immediately on a stale handle.
     */
    if (node == NULL) {
        return 0;
    }

    while (node != NULL) {
        if ((node->state & SCHULTZ_STATE_VISIBLE) == 0) {
            return 0;
        }
        node = schultz_node_get(tree, node->parent);
    }
    return 1;
}

int32_t schultz_node_is_enabled(const schultz_tree *tree,
                                schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);

    /*
     * Inherited from the ancestors the same way being visible is, and for the
     * same reason: turning a panel off has to turn off everything on it, or
     * a form disabled while it saves still answers every button in it.
     *
     * A node that does not exist is not enabled, for the reason the visible
     * walk gives: the loop that stops at the root also stops immediately on a
     * stale handle, and would report enabled.
     */
    if (node == NULL) {
        return 0;
    }

    while (node != NULL) {
        if ((node->state & SCHULTZ_STATE_ENABLED) == 0) {
            return 0;
        }
        node = schultz_node_get(tree, node->parent);
    }
    return 1;
}

/* ----------------------------------------------------------- invalidation */

int32_t schultz_node_invalidate(schultz_tree *tree, schultz_handle handle)
{
    schultz_node *node;

    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node = schultz_node_get(tree, handle);
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_node_mark(tree, node);
    return SCHULTZ_OK;
}

int32_t schultz_node_is_dirty(const schultz_tree *tree, schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? 0 : (int32_t)(node->dirty != 0);
}

int32_t schultz_node_subtree_dirty(const schultz_tree *tree,
                                   schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? 0 : (int32_t)(node->subtree_dirty != 0);
}

int32_t schultz_tree_is_dirty(const schultz_tree *tree)
{
    if (tree == NULL) {
        return 0;
    }
    return (int32_t)(tree->dirty_count != 0u);
}

int32_t schultz_tree_dirty_region(const schultz_tree *tree,
                                  schultz_rect *out_region)
{
    if (tree == NULL || out_region == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (tree->dirty_count == 0u) {
        *out_region = schultz_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
        return SCHULTZ_OK;
    }
    {
        uint32_t i;

        *out_region = tree->dirty[0];
        for (i = 1u; i < tree->dirty_count; i++) {
            *out_region = schultz_rect_cover(*out_region, tree->dirty[i]);
        }
    }
    return SCHULTZ_OK;
}

uint32_t schultz_tree_dirty_count(const schultz_tree *tree)
{
    return (tree == NULL) ? 0u : tree->dirty_count;
}

int32_t schultz_tree_dirty_at(const schultz_tree *tree, uint32_t index,
                              schultz_rect *out_region)
{
    if (tree == NULL || out_region == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (index >= tree->dirty_count) {
        return SCHULTZ_ERR_EXHAUSTED;
    }
    *out_region = tree->dirty[index];
    return SCHULTZ_OK;
}

void schultz_tree_clear_dirty(schultz_tree *tree)
{
    uint32_t i;

    if (tree == NULL) {
        return;
    }
    for (i = 0; i < tree->nodes.capacity; i++) {
        if (tree->nodes.slots[i].live) {
            schultz_node *node = (schultz_node *)tree->nodes.slots[i].object;
            node->dirty         = 0;
            node->subtree_dirty = 0;
        }
    }
    tree->dirty_count = 0u;
}

uint32_t schultz_tree_node_count(const schultz_tree *tree)
{
    return (tree == NULL) ? 0 : schultz_handle_table_count(&tree->nodes);
}


/* ------------------------------------------------------------ widget */

int32_t schultz_node_set_widget(schultz_tree *tree, schultz_handle handle,
                                const schultz_widget_vtable *vtable,
                                void *data)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* Replacing a widget releases what the previous one was using. */
    if (node->widget != NULL && node->widget_data != NULL) {
        const schultz_widget_vtable *old =
            (const schultz_widget_vtable *)node->widget;
        if (old->destroy != NULL && node->widget_data != data) {
            old->destroy(node->widget_data);
        }
    }
    node->widget      = vtable;
    node->widget_data = data;
    schultz_node_mark(tree, node);
    return SCHULTZ_OK;
}

void *schultz_node_widget_data(const schultz_tree *tree,
                               schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? NULL : node->widget_data;
}

const schultz_widget_vtable *schultz_node_widget(const schultz_tree *tree,
                                                 schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? NULL
                          : (const schultz_widget_vtable *)node->widget;
}

/*
 * Puts the faces compiled into the library into any font slot the tree's
 * theme left empty.
 *
 * A theme names a font per slot and starts with none of them filled, so a
 * program that never mentions fonts used to draw nothing at all: a label with
 * no face returns before it shapes anything, and the screen came up blank.
 * The faces are in the library, so the tree fills the gaps from them and text
 * works whether or not a host has said anything about it.
 *
 * Only the tree's own copy is touched, never the caller's struct. And only
 * empty slots, so a host that sets one face and leaves the others gets its
 * face where it asked for it and a working one everywhere else.
 *
 * Run from both setters, because a host may hand over the theme or the font
 * system first and only the second of the two can finish the job.
 */
static void schultz_tree_fill_fonts(schultz_tree *tree)
{
    uint32_t token;

    if (tree->fonts == NULL) {
        return;
    }
    for (token = 0u; token < SCHULTZ_TOKEN_FONT_COUNT; token++) {
        schultz_handle face;

        if (schultz_theme_font(&tree->theme, token) != SCHULTZ_HANDLE_NONE) {
            continue;
        }
        face = schultz_font_builtin(
            (const schultz_font_system *)tree->fonts, token);
        if (face != SCHULTZ_HANDLE_NONE) {
            schultz_theme_set_font(&tree->theme, token, face);
        }
    }
}

int32_t schultz_tree_set_font_system(schultz_tree *tree, void *fonts)
{
    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    tree->fonts = fonts;
    schultz_tree_fill_fonts(tree);
    {
        schultz_node *root = schultz_node_get(tree, tree->root);

        if (root != NULL) {
            schultz_node_mark_style_subtree(tree, root);
        }
    }
    return SCHULTZ_OK;
}

void *schultz_tree_font_system(const schultz_tree *tree)
{
    return (tree == NULL) ? NULL : tree->fonts;
}

int32_t schultz_node_set_animating(schultz_tree *tree, schultz_handle handle,
                                   int32_t animating)
{
    uint32_t i;

    if (schultz_node_get(tree, handle) == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    for (i = 0; i < tree->animating_count; i++) {
        if (tree->animating[i] == handle) {
            if (!animating) {
                tree->animating[i] =
                    tree->animating[tree->animating_count - 1u];
                tree->animating_count--;
            }
            return SCHULTZ_OK;
        }
    }
    if (!animating) {
        return SCHULTZ_OK;
    }
    if (tree->animating_count == tree->animating_capacity) {
        uint32_t capacity = (tree->animating_capacity == 0u)
                                ? 4u : tree->animating_capacity * 2u;
        schultz_handle *grown = (schultz_handle *)realloc(
            tree->animating, (size_t)capacity * sizeof(*grown));

        if (grown == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        tree->animating          = grown;
        tree->animating_capacity = capacity;
    }
    tree->animating[tree->animating_count++] = handle;
    return SCHULTZ_OK;
}

int32_t schultz_node_animating(const schultz_tree *tree, schultz_handle handle)
{
    uint32_t i;

    if (tree == NULL) {
        return 0;
    }
    for (i = 0; i < tree->animating_count; i++) {
        if (tree->animating[i] == handle) {
            return 1;
        }
    }
    return 0;
}

static void schultz_node_apply_anchor(schultz_tree *tree, schultz_node *node);

/* Both defined with the anchor code, and both needed by the layout pass. */
static void schultz_node_place_absolute(schultz_tree *tree,
                                        schultz_node *node, schultz_rect at);
static schultz_rect schultz_tree_safe_rect(const schultz_tree *tree);

/* True when a node is one of the tree's raised overlays. */
static int32_t schultz_node_is_overlay(const schultz_tree *tree,
                                       schultz_handle node)
{
    uint32_t count = schultz_tree_overlay_count(tree);
    uint32_t i;

    for (i = 0; i < count; i++) {
        schultz_handle at = SCHULTZ_HANDLE_NONE;

        if (schultz_tree_overlay_at(tree, i, &at, NULL) == SCHULTZ_OK &&
            at == node) {
            return 1;
        }
    }
    return 0;
}

/*
 * Recursive half of the layout pass. It lives here rather than in
 * schultz_layout.c because it reads the two dirty bits directly: whether this
 * node needs arranging, and whether anything below it does. Everything else
 * about layout is in schultz_layout.c; this is the part that needs to see
 * inside a node.
 */
static uint32_t schultz_layout_run_node(schultz_tree *tree,
                                        schultz_node *node)
{
    uint32_t arranged = 0;
    uint32_t i;
    int32_t  placed = 0;

    /*
     * A node that covers the window takes its size from the window rather
     * than from a parent, and the window can change size at any moment: a
     * phone being turned, a keyboard arriving, a band appearing across the
     * top of the screen. Nothing would otherwise tell it, because its parent
     * did not place it and a resize is aimed at the root.
     *
     * Done before the dirty check, since the node has not been marked and the
     * whole point is that it does not know yet.
     */
    if (node->fills_viewport &&
        !schultz_rect_equals(schultz_node_absolute(tree, node, NULL),
                             tree->viewport)) {
        /*
         * The whole window and not the safe area, because covering
         * everything is the point: a dialog's scrim has to reach under the
         * notch and behind the home indicator, or the screen shows a strip
         * of the application either side of a modal one. Compared and placed
         * in window coordinates, since that is what it is being given.
         */
        schultz_node_place_absolute(tree, node, tree->viewport);
    }
    /*
     * An anchored node is placed against the safe area, so it is placed again
     * when that changes. Only then: measuring a menu costs real work and
     * nothing about where it belongs has moved otherwise. The safe area
     * rather than the window, because a keyboard arriving changes the first
     * without always changing the second.
     */
    if (node->anchored &&
        !schultz_rect_equals(node->anchor_window,
                             schultz_tree_safe_rect(tree))) {
        schultz_node_apply_anchor(tree, node);
    }
    if (!node->layout_dirty && !node->layout_below) {
        return 0u;
    }

    /*
     * A node that needs laying out cannot always do it: a leaf whose size
     * changed is placed by whatever pane holds it, not by itself. So the
     * decision is made at the pane, which runs when it is marked or when
     * anything under it is.
     *
     * Its own bounds, because bounds are relative to the parent and this node
     * is not moving; only what is inside it is.
     */
    if (schultz_node_get_pane(tree, node->self) != NULL) {
        schultz_layout_arrange(tree, node->self, node->bounds);
        arranged = 1u;
        placed   = 1;
    }

    for (i = 0; i < node->child_count; i++) {
        schultz_node *child = schultz_node_get(tree, node->children[i]);

        if (child == NULL) {
            continue;
        }
        /*
         * Arranging above ran every pane beneath it, so those children are
         * done. A child with no pane was given a place but nothing inside it
         * was laid out, and a pane may well be sitting in there: a tab page
         * holds its content that way. So the walk carries on through it.
         */
        /*
         * An overlay is not part of the flow. A dialog is a child of the
         * root, so a pane on the root arranges it along with everything
         * else, and it stops covering the window it is supposed to be
         * covering. It gets its own pass regardless, which is where a node
         * that takes its size from the window puts that size back.
         */
        if (placed && schultz_node_get_pane(tree, child->self) != NULL &&
            !schultz_node_is_overlay(tree, child->self)) {
            continue;
        }
        arranged += schultz_layout_run_node(tree, child);
    }
    return arranged;
}

uint32_t schultz_layout_run(schultz_tree *tree)
{
    schultz_node *root;
    uint32_t arranged;

    if (tree == NULL) {
        return 0u;
    }
    root = schultz_node_get(tree, tree->root);
    if (root == NULL) {
        return 0u;
    }
    arranged = schultz_layout_run_node(tree, root);
    schultz_tree_clear_layout_dirty(tree);
    return arranged;
}

uint32_t schultz_tree_advance(schultz_tree *tree, uint64_t now_ms)
{
    uint32_t elapsed;
    uint32_t changed = 0;
    uint32_t i;

    if (tree == NULL) {
        return 0u;
    }
    if (!tree->ticked) {
        /* The first call only sets the starting point: no time has passed. */
        tree->last_tick = now_ms;
        tree->ticked    = 1u;
        return 0u;
    }
    /* A clock that went backwards is treated as no time having passed. */
    elapsed = (now_ms > tree->last_tick)
                  ? (uint32_t)(now_ms - tree->last_tick) : 0u;
    tree->last_tick = now_ms;

    /*
     * Walked backwards, so a widget that stops animating during its own tick
     * removes itself without the walk skipping whatever took its place.
     */
    for (i = tree->animating_count; i > 0u; i--) {
        schultz_handle handle = tree->animating[i - 1u];
        const schultz_widget_vtable *widget = schultz_node_widget(tree,
                                                                  handle);

        if (widget == NULL || widget->tick == NULL) {
            continue;
        }
        /*
         * This is the one list that reaches a node without walking the tree,
         * so it is the one place a node taken out of the tree would carry on
         * working. A detached widget stops here and starts again where it
         * left off when it is put back, because its own request to be ticked
         * is left alone.
         */
        if (!schultz_node_is_attached(tree, handle)) {
            continue;
        }
        if (widget->tick(tree, handle, now_ms, elapsed) != 0) {
            /*
             * A tick returning nonzero means the node needs painting again,
             * which is the tick's whole contract. Marking it here rather than
             * leaving each widget to remember is what makes an animation
             * smooth instead of redrawing only when something else nearby
             * happens to change.
             */
            schultz_node_invalidate(tree, handle);
            changed++;
        }
    }
    return changed;
}

int32_t schultz_tree_set_pointer(schultz_tree *tree, schultz_point point)
{
    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    tree->pointer      = point;
    tree->pointer_seen = 1u;
    return SCHULTZ_OK;
}

int32_t schultz_tree_pointer(const schultz_tree *tree,
                             schultz_point *out_point)
{
    if (tree == NULL || out_point == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (!tree->pointer_seen) {
        return SCHULTZ_ERR_EXHAUSTED;
    }
    *out_point = tree->pointer;
    return SCHULTZ_OK;
}

int32_t schultz_tree_set_image_table(schultz_tree *tree, void *images)
{
    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    tree->images = images;
    return SCHULTZ_OK;
}

void *schultz_tree_image_table(const schultz_tree *tree)
{
    return (tree == NULL) ? NULL : tree->images;
}

int32_t schultz_tree_set_glyph_cache(schultz_tree *tree, void *glyphs)
{
    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    tree->glyphs = glyphs;
    return SCHULTZ_OK;
}

void *schultz_tree_glyph_cache(const schultz_tree *tree)
{
    return (tree == NULL) ? NULL : tree->glyphs;
}

int32_t schultz_tree_set_resources(schultz_tree *tree, void *resources)
{
    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    tree->resources = resources;
    return SCHULTZ_OK;
}

void *schultz_tree_resources(const schultz_tree *tree)
{
    return (tree == NULL) ? NULL : tree->resources;
}

int32_t schultz_tree_set_audio(schultz_tree *tree, void *audio)
{
    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    tree->audio = audio;
    return SCHULTZ_OK;
}

void *schultz_tree_audio(const schultz_tree *tree)
{
    return (tree == NULL) ? NULL : tree->audio;
}

int32_t schultz_tree_set_clipboard(schultz_tree *tree,
                                   schultz_clipboard_offer_fn offer,
                                   schultz_clipboard_take_fn take,
                                   schultz_clipboard_holds_fn holds,
                                   void *context)
{
    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    tree->clip_offer   = offer;
    tree->clip_take    = take;
    tree->clip_holds   = holds;
    tree->clip_context = context;
    return SCHULTZ_OK;
}

int32_t schultz_tree_clipboard_offer(schultz_tree *tree,
                                     const char *const *formats,
                                     uint32_t count,
                                     schultz_clipboard_make_fn make,
                                     void *make_context)
{
    if (tree == NULL || tree->clip_offer == NULL || formats == NULL ||
        count == 0u || make == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * Refused rather than trimmed. The layer underneath carries a fixed
     * number of formats and used to drop the rest without saying so, which
     * is the worst of the three possible answers: the caller believes it
     * offered something it did not, and only finds out when a paste into one
     * particular program comes back empty.
     */
    if (count > (uint32_t)SCHULTZ_CLIPBOARD_MAX) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return tree->clip_offer(tree->clip_context, formats, count, make,
                            make_context);
}

const void *schultz_tree_clipboard_take(const schultz_tree *tree,
                                        const char *format,
                                        uint64_t *out_length)
{
    if (out_length == NULL) {
        return NULL;
    }
    *out_length = 0u;
    if (tree == NULL || tree->clip_take == NULL || format == NULL) {
        return NULL;
    }
    return tree->clip_take(tree->clip_context, format, out_length);
}

int32_t schultz_tree_clipboard_holds(const schultz_tree *tree,
                                     const char *format)
{
    if (tree == NULL || format == NULL) {
        return 0;
    }
    if (tree->clip_holds != NULL) {
        return tree->clip_holds(tree->clip_context, format) ? 1 : 0;
    }
    /*
     * No cheap way to ask, so ask the expensive way. A host that installs a
     * take without a holds gets a correct answer rather than a refusal.
     */
    if (tree->clip_take != NULL) {
        uint64_t length = 0u;

        return (tree->clip_take(tree->clip_context, format, &length) != NULL)
                   ? 1 : 0;
    }
    return 0;
}

/*
 * Text, which is nearly every clipboard there has ever been.
 *
 * The bytes a take hands back are not promised to be NUL terminated, because
 * a picture has no reason to be. Text from this toolkit always is, and every
 * platform clipboard terminates its text as well, but a caller that trusted
 * that and met one that did not would read off the end. So this copies into a
 * buffer it terminates itself.
 */
const char *schultz_tree_clipboard_read(const schultz_tree *tree)
{
    static char *held = NULL;
    static size_t room = 0u;
    const void *bytes;
    uint64_t length = 0u;

    bytes = schultz_tree_clipboard_take(tree, SCHULTZ_CLIPBOARD_TEXT,
                                        &length);
    if (bytes == NULL || length == 0u) {
        return NULL;
    }
    /*
     * What the platform handed back, as a uint64_t. Refused before a
     * terminator is added to it: at the top of the range that sum wraps to
     * nothing, the buffer is made that size, and the copy below still writes
     * the length that was reported.
     */
    if (length > (uint64_t)SIZE_MAX - 1u) {
        return NULL;
    }
    if (length + 1u > (uint64_t)room) {
        char *bigger = (char *)realloc(held, (size_t)length + 1u);

        if (bigger == NULL) {
            return NULL;
        }
        held = bigger;
        room = (size_t)length + 1u;
    }
    memcpy(held, bytes, (size_t)length);
    held[length] = '\0';
    return held;
}

int32_t schultz_tree_set_selection_owner(schultz_tree *tree,
                                         schultz_handle node)
{
    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    tree->selection = node;
    return SCHULTZ_OK;
}

schultz_handle schultz_tree_selection_owner(const schultz_tree *tree)
{
    return (tree == NULL) ? SCHULTZ_HANDLE_NONE : tree->selection;
}

/* Hands back the one string a text-only offer was made from. */
static const void *schultz_clipboard_make_text(void *context,
                                               const char *format,
                                               uint64_t *out_length)
{
    const char *utf8 = (const char *)context;

    (void)format;
    *out_length = (uint64_t)strlen(utf8);
    return utf8;
}

int32_t schultz_tree_clipboard_write(schultz_tree *tree, const char *utf8)
{
    static const char *const formats[] = { SCHULTZ_CLIPBOARD_TEXT };
    static char *held = NULL;

    if (tree == NULL || utf8 == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * Kept, because the bytes are produced later than this call and the
     * caller's string is usually a local. One copy is held at a time, which
     * is all a clipboard ever holds.
     */
    {
        size_t length = strlen(utf8);
        char *copy = (char *)realloc(held, length + 1u);

        if (copy == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        memcpy(copy, utf8, length + 1u);
        held = copy;
    }
    return schultz_tree_clipboard_offer(tree, formats, 1u,
                                        schultz_clipboard_make_text, held);
}

/*
 * What the last write of bytes put up, kept because the platform asks for the
 * bytes later than the call that offered them. One set at a time, which is
 * all a clipboard ever holds: each write frees what the one before it left.
 */
static struct {
    uint32_t count;
    char    *formats[SCHULTZ_CLIPBOARD_MAX];
    void    *bytes[SCHULTZ_CLIPBOARD_MAX];
    uint64_t lengths[SCHULTZ_CLIPBOARD_MAX];
} schultz_clipboard_kept;

static void schultz_clipboard_kept_clear(void)
{
    uint32_t i;

    for (i = 0; i < schultz_clipboard_kept.count; i++) {
        free(schultz_clipboard_kept.formats[i]);
        free(schultz_clipboard_kept.bytes[i]);
    }
    memset(&schultz_clipboard_kept, 0, sizeof(schultz_clipboard_kept));
}

/* Hands back whichever of the kept formats was asked for. */
static const void *schultz_clipboard_make_bytes(void *context,
                                                const char *format,
                                                uint64_t *out_length)
{
    uint32_t i;

    (void)context;
    for (i = 0; i < schultz_clipboard_kept.count; i++) {
        if (strcmp(format, schultz_clipboard_kept.formats[i]) == 0) {
            *out_length = schultz_clipboard_kept.lengths[i];
            return schultz_clipboard_kept.bytes[i];
        }
    }
    return NULL;
}

int32_t schultz_tree_clipboard_write_bytes(
    schultz_tree *tree, const schultz_clipboard_entry *entries,
    uint32_t count)
{
    const char *formats[SCHULTZ_CLIPBOARD_MAX];
    uint32_t i;

    if (tree == NULL || entries == NULL || count == 0u ||
        count > (uint32_t)SCHULTZ_CLIPBOARD_MAX) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < count; i++) {
        if (entries[i].format == NULL || entries[i].bytes == NULL ||
            entries[i].length == 0u) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
        /*
         * The length arrives as a uint64_t and has to become a size_t to be
         * allocated. On a 32-bit build the two are different sizes, and a
         * length that does not fit would otherwise be truncated into a small
         * allocation that the copy below then overruns.
         */
        if (entries[i].length > (uint64_t)SIZE_MAX) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
    }
    /*
     * Checked before anything is thrown away, so a refused write leaves what
     * was on the clipboard where it was rather than emptying it.
     */
    schultz_clipboard_kept_clear();
    for (i = 0; i < count; i++) {
        size_t length = (size_t)entries[i].length;
        size_t room = strlen(entries[i].format) + 1u;

        schultz_clipboard_kept.formats[i] = (char *)malloc(room);
        schultz_clipboard_kept.bytes[i] = malloc(length);
        if (schultz_clipboard_kept.formats[i] == NULL ||
            schultz_clipboard_kept.bytes[i] == NULL) {
            /* Counted up to here, so the clear frees exactly what was made. */
            schultz_clipboard_kept.count = i + 1u;
            schultz_clipboard_kept_clear();
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        memcpy(schultz_clipboard_kept.formats[i], entries[i].format, room);
        memcpy(schultz_clipboard_kept.bytes[i], entries[i].bytes, length);
        schultz_clipboard_kept.lengths[i] = entries[i].length;
        formats[i] = schultz_clipboard_kept.formats[i];
    }
    schultz_clipboard_kept.count = count;
    return schultz_tree_clipboard_offer(tree, formats, count,
                                        schultz_clipboard_make_bytes, NULL);
}

int32_t schultz_node_set_clips_children(schultz_tree *tree,
                                        schultz_handle handle, int32_t clips)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    node->clips_children = clips ? 1u : 0u;
    schultz_node_mark(tree, node);
    return SCHULTZ_OK;
}

int32_t schultz_node_set_places_itself(schultz_tree *tree,
                                       schultz_handle handle,
                                       int32_t places_itself)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    node->places_itself = places_itself ? 1u : 0u;
    /*
     * The pane above it now has a different set of children to arrange, one
     * way or the other, so it has to run again.
     */
    return schultz_node_invalidate_layout(tree, handle);
}

int32_t schultz_node_places_itself(const schultz_tree *tree,
                                   schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);

    return (node == NULL) ? 0 : (int32_t)(node->places_itself != 0u);
}

int32_t schultz_node_set_keeps_focus(schultz_tree *tree, schultz_handle node,
                                     int32_t keeps)
{
    schultz_node *target = schultz_node_get(tree, node);

    if (target == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    target->keeps_focus = keeps ? 1u : 0u;
    return SCHULTZ_OK;
}

int32_t schultz_node_keeps_focus(const schultz_tree *tree,
                                 schultz_handle node)
{
    const schultz_node *target = schultz_node_get(tree, node);

    return (target == NULL) ? 0 : (int32_t)(target->keeps_focus != 0u);
}

int32_t schultz_node_set_fills_viewport(schultz_tree *tree,
                                        schultz_handle handle, int32_t fills)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    node->fills_viewport = fills ? 1u : 0u;
    return SCHULTZ_OK;
}

/*
 * Puts a node at a rectangle given in window coordinates.
 *
 * schultz_node_set_bounds takes a rectangle in the parent's space, and
 * everything that places itself against the window works in the window's. On
 * a desktop the two are the same, because the root sits at the origin and
 * covers everything. On a phone the root is given the safe area instead, so
 * it starts below the notch, and handing it an absolute rectangle moved
 * every overlay down by the height of the notch: menus opened below the
 * button, popovers landed on top of what they pointed at, and a toast meant
 * for the bottom of the screen went off the end of it.
 *
 * So the conversion happens here, once, rather than at each place that knows
 * where it wants to be.
 */
static void schultz_node_place_absolute(schultz_tree *tree,
                                        schultz_node *node, schultz_rect at)
{
    schultz_rect here = schultz_node_absolute(tree, node, NULL);
    float dx = here.x - node->bounds.x;
    float dy = here.y - node->bounds.y;

    schultz_node_set_bounds(tree, node->self,
        schultz_rect_make(at.x - dx, at.y - dy, at.width, at.height));
}

int32_t schultz_tree_safe_area(const schultz_tree *tree,
                               schultz_rect *out_area)
{
    const schultz_node *root;

    if (tree == NULL || out_area == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    root = schultz_node_get(tree, tree->root);
    /*
     * A root with no bounds has not been told where content may go, which is
     * every tree before the first layout and any host that sets a viewport
     * and nothing else. The whole window is the right answer then: it is what
     * the toolkit did before there was a safe area at all.
     */
    if (root == NULL || schultz_rect_is_empty(root->bounds)) {
        *out_area = tree->viewport;
    } else {
        *out_area = root->bounds;
    }
    return SCHULTZ_OK;
}

/* The same, as a value, for the placement code below. */
static schultz_rect schultz_tree_safe_rect(const schultz_tree *tree)
{
    schultz_rect area;

    schultz_tree_safe_area(tree, &area);
    return area;
}

/* Where one placement puts a node of this size beside this anchor. */
static schultz_rect schultz_node_anchor_rect(const schultz_node *node,
                                             uint32_t placement,
                                             schultz_size size)
{
    schultz_rect anchor = node->anchor;
    float gap = node->anchor_gap;
    schultz_rect at;

    switch (placement) {
    case SCHULTZ_PLACE_ABOVE:
        at = schultz_rect_make(anchor.x, anchor.y - size.height - gap,
                               size.width, size.height);
        break;
    case SCHULTZ_PLACE_RIGHT:
        at = schultz_rect_make(anchor.x + anchor.width + gap, anchor.y,
                               size.width, size.height);
        break;
    case SCHULTZ_PLACE_LEFT:
        at = schultz_rect_make(anchor.x - size.width - gap, anchor.y,
                               size.width, size.height);
        break;
    case SCHULTZ_PLACE_OVER:
        at = schultz_rect_make(anchor.x, anchor.y, size.width, size.height);
        break;
    case SCHULTZ_PLACE_BELOW:
    default:
        at = schultz_rect_make(anchor.x, anchor.y + anchor.height + gap,
                               size.width, size.height);
        break;
    }

    /*
     * Centred on the anchor across the axis the placement did not decide.
     * Above and below decide a top, so centring is across the width; left and
     * right decide a side, so it is across the height.
     */
    if (node->anchor_centred) {
        if (placement == SCHULTZ_PLACE_LEFT ||
            placement == SCHULTZ_PLACE_RIGHT) {
            at.y = anchor.y + (anchor.height - size.height) * 0.5f;
        } else {
            at.x = anchor.x + (anchor.width - size.width) * 0.5f;
        }
    }
    return at;
}

/* The side opposite a placement, for a node allowed to flip. */
static uint32_t schultz_node_other_side(uint32_t placement)
{
    switch (placement) {
    case SCHULTZ_PLACE_ABOVE: return SCHULTZ_PLACE_BELOW;
    case SCHULTZ_PLACE_BELOW: return SCHULTZ_PLACE_ABOVE;
    case SCHULTZ_PLACE_LEFT:  return SCHULTZ_PLACE_RIGHT;
    case SCHULTZ_PLACE_RIGHT: return SCHULTZ_PLACE_LEFT;
    default:                  return placement;
    }
}

/* Whether a rectangle is wholly inside the window along the placement axis. */
static int32_t schultz_node_side_fits(schultz_rect at, schultz_rect viewport,
                                      uint32_t placement)
{
    if (placement == SCHULTZ_PLACE_LEFT ||
        placement == SCHULTZ_PLACE_RIGHT) {
        return (at.x >= viewport.x &&
                at.x + at.width <= viewport.x + viewport.width) ? 1 : 0;
    }
    return (at.y >= viewport.y &&
            at.y + at.height <= viewport.y + viewport.height) ? 1 : 0;
}

/*
 * Puts an anchored node beside its anchor, and back inside the window if that
 * would hang it off an edge.
 *
 * Nudged rather than flipped by default: for a menu, moving a little is less
 * surprising than jumping, and a submenu that leaps above the row that opened
 * it is genuinely disorienting.
 *
 * A node that asks to flip gets the other rule, because for a tooltip and a
 * popover nudging is not enough: nudged, they end up sitting on top of the
 * thing they are explaining, which is the one place they must not be. Such a
 * node goes to the other side only when the side it asked for does not fit
 * and the other one does, so it stays where it was asked to be whenever that
 * is possible at all.
 */
static void schultz_node_apply_anchor(schultz_tree *tree, schultz_node *node)
{
    /* The safe area rather than the whole window: an overlay nudged under a
     * camera notch is on screen and unreadable, which is worse than one that
     * moved a little further to stay clear of it. */
    schultz_rect viewport = schultz_tree_safe_rect(tree);
    uint32_t placement = node->placement;
    schultz_size size;
    schultz_rect at;

    if (schultz_layout_measure(tree, node->self, -1.0f, -1.0f, &size)
            != SCHULTZ_OK) {
        return;
    }
    at = schultz_node_anchor_rect(node, placement, size);

    if (node->anchor_flips &&
        !schultz_node_side_fits(at, viewport, placement)) {
        uint32_t other = schultz_node_other_side(placement);
        schultz_rect swapped = schultz_node_anchor_rect(node, other, size);

        if (other != placement &&
            schultz_node_side_fits(swapped, viewport, other)) {
            placement = other;
            at = swapped;
        }
    }

    if (at.x + at.width > viewport.x + viewport.width) {
        at.x = viewport.x + viewport.width - at.width;
    }
    if (at.y + at.height > viewport.y + viewport.height) {
        at.y = viewport.y + viewport.height - at.height;
    }
    if (at.x < viewport.x) { at.x = viewport.x; }
    if (at.y < viewport.y) { at.y = viewport.y; }

    node->placed        = placement;
    node->anchor_window = viewport;
    schultz_node_place_absolute(tree, node, at);
}

int32_t schultz_node_set_anchored(schultz_tree *tree, schultz_handle handle,
                                  schultz_rect anchor, uint32_t placement,
                                  float gap, int32_t centred, int32_t flips)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    node->anchored       = 1u;
    node->anchor         = anchor;
    node->placement      = placement;
    node->placed         = placement;
    node->anchor_gap     = (gap > 0.0f) ? gap : 0.0f;
    node->anchor_centred = centred ? 1u : 0u;
    node->anchor_flips   = flips ? 1u : 0u;
    /* Now, so that a node shown this frame is in the right place this frame
     * rather than one frame late. */
    schultz_node_apply_anchor(tree, node);
    return SCHULTZ_OK;
}

uint32_t schultz_node_anchor_placement(const schultz_tree *tree,
                                       schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);

    return (node == NULL) ? SCHULTZ_PLACE_BELOW : node->placed;
}

int32_t schultz_node_clear_anchored(schultz_tree *tree, schultz_handle handle)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    node->anchored = 0u;
    return SCHULTZ_OK;
}

int32_t schultz_node_fills_viewport(const schultz_tree *tree,
                                    schultz_handle handle)
{
    const schultz_node *node = schultz_node_get((schultz_tree *)tree, handle);

    return (node == NULL) ? 0 : (int32_t)(node->fills_viewport != 0u);
}

int32_t schultz_node_clips_children(const schultz_tree *tree,
                                    schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? 0 : (int32_t)(node->clips_children != 0u);
}

int32_t schultz_node_set_hit_testable(schultz_tree *tree,
                                      schultz_handle handle, int32_t testable)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    node->pointer_off = testable ? 0u : 1u;
    return SCHULTZ_OK;
}

int32_t schultz_node_hit_testable(const schultz_tree *tree,
                                  schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? 0 : (int32_t)(node->pointer_off == 0u);
}

int32_t schultz_node_set_paint_margin(schultz_tree *tree,
                                      schultz_handle handle, float margin)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (margin < 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node->paint_margin = margin;
    schultz_node_mark(tree, node);
    return SCHULTZ_OK;
}

float schultz_node_paint_margin(const schultz_tree *tree,
                                schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? 0.0f : node->paint_margin;
}

int32_t schultz_node_set_scroll_offset(schultz_tree *tree,
                                       schultz_handle handle,
                                       schultz_point offset)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (node->scroll.x == offset.x && node->scroll.y == offset.y) {
        return SCHULTZ_OK;
    }
    node->scroll = offset;
    /*
     * Every descendant moved, so every descendant's old and new area has to
     * be repainted. No layout runs: that is the whole point of scrolling by
     * an offset rather than by moving bounds.
     */
    schultz_node_mark_subtree(tree, node);
    return SCHULTZ_OK;
}

int32_t schultz_node_set_cursor(schultz_tree *tree, schultz_handle handle,
                                uint32_t cursor)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (cursor >= SCHULTZ_CURSOR_COUNT) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    node->cursor = cursor;
    return SCHULTZ_OK;
}

uint32_t schultz_node_cursor(const schultz_tree *tree, schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? (uint32_t)SCHULTZ_CURSOR_DEFAULT : node->cursor;
}

schultz_point schultz_node_scroll_offset(const schultz_tree *tree,
                                         schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);

    return (node == NULL) ? schultz_point_make(0.0f, 0.0f) : node->scroll;
}

/* ------------------------------------------------------------- style */

static void schultz_node_mark_style(schultz_tree *tree, schultz_node *node)
{
    node->style_dirty = 1;
    schultz_node_mark_style_below(tree, node);
    schultz_node_mark_ancestors(tree, node);
}

/*
 * Marks a whole subtree style dirty. Needed when an inherited property
 * changes, because every descendant may have been resolving through it, and
 * when a theme changes, because every token lookup may now differ.
 */
static void schultz_node_mark_style_subtree(schultz_tree *tree,
                                            schultz_node *node)
{
    uint32_t i;

    schultz_node_mark_style(tree, node);
    for (i = 0; i < node->child_count; i++) {
        schultz_node *child = schultz_node_get(tree, node->children[i]);
        if (child != NULL) {
            schultz_node_mark_style_subtree(tree, child);
        }
    }
}

/* Marks style stale, widening to the subtree when the property inherits. */
static void schultz_node_mark_property(schultz_tree *tree, schultz_node *node,
                                       uint32_t property)
{
    if (schultz_property_inherits(property)) {
        schultz_node_mark_style_subtree(tree, node);
    } else {
        schultz_node_mark_style(tree, node);
    }
}

static schultz_style_object *schultz_style_get(const schultz_tree *tree,
                                               schultz_handle handle)
{
    void *object = NULL;

    if (tree == NULL ||
        schultz_handle_table_lookup(&tree->styles, handle, &object)
            != SCHULTZ_OK) {
        return NULL;
    }
    return (schultz_style_object *)object;
}

/* Releases a style and every patch in it. */
static void schultz_style_free(schultz_style_object *style)
{
    uint32_t i;

    if (style == NULL) {
        return;
    }
    schultz_patch_free(&style->patch);
    for (i = 0; i < SCHULTZ_STYLE_STATE_COUNT; i++) {
        schultz_patch_free(&style->states[i]);
    }
    free(style);
}

int32_t schultz_style_register(schultz_tree *tree, const schultz_patch *patch,
                               schultz_handle *out_style)
{
    return schultz_style_register_states(tree, patch, NULL, out_style);
}

int32_t schultz_style_register_states(schultz_tree *tree,
                                      const schultz_patch *patch,
                                      const schultz_patch *states,
                                      schultz_handle *out_style)
{
    schultz_style_object *style;
    int32_t result;
    uint32_t i;

    if (tree == NULL || patch == NULL || out_style == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    style = (schultz_style_object *)calloc(1, sizeof(*style));
    if (style == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    schultz_patch_init(&style->patch);
    for (i = 0; i < SCHULTZ_STYLE_STATE_COUNT; i++) {
        schultz_patch_init(&style->states[i]);
    }

    /* Copied, so the caller may release theirs and cannot mutate ours. */
    result = schultz_patch_merge(&style->patch, patch);
    for (i = 0; result == SCHULTZ_OK && states != NULL &&
                i < SCHULTZ_STYLE_STATE_COUNT; i++) {
        result = schultz_patch_merge(&style->states[i], &states[i]);
    }
    if (result != SCHULTZ_OK) {
        schultz_style_free(style);
        return result;
    }

    result = schultz_handle_table_insert(&tree->styles, style, out_style);
    if (result != SCHULTZ_OK) {
        schultz_style_free(style);
        return result;
    }
    return SCHULTZ_OK;
}

/* Drops one reference, freeing the style when the last node lets go. */
static void schultz_style_release(schultz_tree *tree, schultz_handle handle)
{
    schultz_style_object *style = schultz_style_get(tree, handle);

    if (style == NULL) {
        return;
    }
    if (style->references > 0u) {
        style->references--;
    }
    if (style->references == 0u) {
        schultz_handle_table_remove(&tree->styles, handle);
        schultz_style_free(style);
    }
}

int32_t schultz_node_add_style(schultz_tree *tree, schultz_handle handle,
                               schultz_handle style_handle)
{
    schultz_node *node = schultz_node_get(tree, handle);
    schultz_style_object *style = schultz_style_get(tree, style_handle);
    uint32_t i;

    if (node == NULL || style == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    for (i = 0; i < node->style_count; i++) {
        if (node->styles[i] == style_handle) {
            return SCHULTZ_OK; /* already applied */
        }
    }

    if (node->style_count == node->style_capacity) {
        uint32_t capacity = (node->style_capacity == 0)
                                ? 2u : node->style_capacity * 2u;
        schultz_handle *grown = (schultz_handle *)realloc(
            node->styles, (size_t)capacity * sizeof(*grown));
        if (grown == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        node->styles         = grown;
        node->style_capacity = capacity;
    }

    node->styles[node->style_count++] = style_handle;
    style->references++;

    /*
     * A style may set an inherited property, so widen to the subtree when it
     * does rather than guessing.
     */
    for (i = 0; i < style->patch.count; i++) {
        if (schultz_property_inherits(style->patch.entries[i].property)) {
            schultz_node_mark_style_subtree(tree, node);
            return SCHULTZ_OK;
        }
    }
    schultz_node_mark_style(tree, node);
    return SCHULTZ_OK;
}

int32_t schultz_node_remove_style(schultz_tree *tree, schultz_handle handle,
                                  schultz_handle style_handle)
{
    schultz_node *node = schultz_node_get(tree, handle);
    uint32_t i;

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    for (i = 0; i < node->style_count; i++) {
        if (node->styles[i] != style_handle) {
            continue;
        }
        memmove(&node->styles[i], &node->styles[i + 1],
                (size_t)(node->style_count - i - 1) * sizeof(*node->styles));
        node->style_count--;
        schultz_style_release(tree, style_handle);
        schultz_node_mark_style_subtree(tree, node);
        return SCHULTZ_OK;
    }
    return SCHULTZ_OK;
}

uint32_t schultz_node_style_count(const schultz_tree *tree,
                                  schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? 0 : node->style_count;
}

int32_t schultz_node_set_style_property(schultz_tree *tree,
                                        schultz_handle handle,
                                        uint32_t property,
                                        schultz_value value)
{
    schultz_node *node = schultz_node_get(tree, handle);
    int32_t result;

    if (property >= SCHULTZ_PROP_COUNT) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    result = schultz_patch_set(&node->inline_patch, property, value);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_mark_property(tree, node, property);
    return SCHULTZ_OK;
}

int32_t schultz_node_clear_style_property(schultz_tree *tree,
                                          schultz_handle handle,
                                          uint32_t property)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_patch_unset(&node->inline_patch, property);
    schultz_node_mark_property(tree, node, property);
    return SCHULTZ_OK;
}

int32_t schultz_node_set_state_property(schultz_tree *tree,
                                        schultz_handle handle, uint32_t state,
                                        uint32_t property,
                                        schultz_value value)
{
    schultz_node *node = schultz_node_get(tree, handle);
    int32_t result;

    if (state >= SCHULTZ_STYLE_STATE_COUNT ||
        property >= SCHULTZ_PROP_COUNT) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    result = schultz_patch_set(&node->state_patches[state], property, value);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_mark_property(tree, node, property);
    return SCHULTZ_OK;
}

int32_t schultz_node_clear_style(schultz_tree *tree, schultz_handle handle)
{
    schultz_node *node = schultz_node_get(tree, handle);
    uint32_t i;

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    for (i = 0; i < node->style_count; i++) {
        schultz_style_release(tree, node->styles[i]);
    }
    node->style_count = 0;
    schultz_patch_clear(&node->inline_patch);
    for (i = 0; i < SCHULTZ_STYLE_STATE_COUNT; i++) {
        schultz_patch_clear(&node->state_patches[i]);
    }
    schultz_node_mark_style_subtree(tree, node);
    return SCHULTZ_OK;
}

int32_t schultz_tree_set_theme(schultz_tree *tree, const schultz_theme *theme)
{
    schultz_node *root;

    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (theme == NULL) {
        schultz_theme_init(&tree->theme);
    } else {
        tree->theme = *theme;
    }
    schultz_tree_fill_fonts(tree);
    root = schultz_node_get(tree, tree->root);
    if (root != NULL) {
        schultz_node_mark_style_subtree(tree, root);
    }
    return SCHULTZ_OK;
}

int32_t schultz_node_set_theme(schultz_tree *tree, schultz_handle handle,
                               const schultz_theme *theme)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (theme == NULL) {
        free(node->theme);
        node->theme = NULL;
    } else {
        if (node->theme == NULL) {
            node->theme = (schultz_theme *)malloc(sizeof(*node->theme));
            if (node->theme == NULL) {
                return SCHULTZ_ERR_OUT_OF_MEMORY;
            }
        }
        *node->theme = *theme;
    }
    /* Every descendant resolves through this theme, so all of them restyle. */
    schultz_node_mark_style_subtree(tree, node);
    return SCHULTZ_OK;
}

/* The nearest theme at or above a node, falling back to the tree's. */
static const schultz_theme *schultz_node_effective_theme(
    const schultz_tree *tree, const schultz_node *node)
{
    const schultz_node *walk = node;

    while (walk != NULL) {
        if (walk->theme != NULL) {
            return walk->theme;
        }
        walk = schultz_node_get(tree, walk->parent);
    }
    return &tree->theme;
}

const schultz_resolved_style *schultz_node_resolved(const schultz_tree *tree,
                                                    schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? NULL : &node->resolved;
}

/* Which state patches apply, given a node's state flags. */
static int32_t schultz_state_patch_applies(uint32_t style_state,
                                           uint32_t state)
{
    switch (style_state) {
    case SCHULTZ_STYLE_STATE_HOVERED:
        return (state & SCHULTZ_STATE_HOVERED) ? 1 : 0;
    case SCHULTZ_STYLE_STATE_FOCUSED:
        return (state & SCHULTZ_STATE_FOCUSED) ? 1 : 0;
    case SCHULTZ_STYLE_STATE_CHECKED:
        return (state & SCHULTZ_STATE_CHECKED) ? 1 : 0;
    case SCHULTZ_STYLE_STATE_SELECTED:
        return (state & SCHULTZ_STATE_SELECTED) ? 1 : 0;
    case SCHULTZ_STYLE_STATE_PRESSED:
        return (state & SCHULTZ_STATE_PRESSED) ? 1 : 0;
    case SCHULTZ_STYLE_STATE_DISABLED:
        /* Disabled is the absence of enabled, not a flag of its own. */
        return (state & SCHULTZ_STATE_ENABLED) ? 0 : 1;
    default:
        return 0;
    }
}

/*
 * Resolves one node against its parent's already resolved style, then sets
 * the dirty bits the change actually costs.
 */
static void schultz_node_resolve_one(schultz_tree *tree, schultz_node *node,
                                     const schultz_node *parent)
{
    const schultz_theme *theme = schultz_node_effective_theme(tree, node);
    schultz_resolved_style previous = node->resolved;
    schultz_resolved_style next;
    uint32_t i;
    uint32_t j;

    schultz_resolved_defaults(&next, theme);

    /*
     * Inherited properties come from the parent's resolved style, which is
     * already final because the walk is pre-order. That keeps inheritance to
     * one step per node rather than a walk to the root.
     */
    if (parent != NULL) {
        for (i = 0; i < SCHULTZ_PROP_COUNT; i++) {
            if (schultz_property_inherits(i)) {
                next.values[i] = parent->resolved.values[i];
            }
        }
    }

    for (i = 0; i < node->style_count; i++) {
        schultz_style_object *style = schultz_style_get(tree, node->styles[i]);
        if (style != NULL) {
            schultz_resolved_apply(&next, &style->patch, theme);
        }
    }
    schultz_resolved_apply(&next, &node->inline_patch, theme);

    /*
     * State last, and in two rounds: what the styles say about a state, then
     * what this node says about it.
     *
     * Above the layers that have no state, because a hover colour is only
     * useful if it beats the plain one, and the plain one may have come from
     * a style or from the node itself. Below the node's own state patches,
     * because naming a state on one node is the most specific thing a caller
     * can do and has to win, exactly as an inline property beats a style.
     */
    for (j = 0; j < node->style_count; j++) {
        schultz_style_object *style = schultz_style_get(tree, node->styles[j]);

        if (style == NULL) {
            continue;
        }
        for (i = 0; i < SCHULTZ_STYLE_STATE_COUNT; i++) {
            if (schultz_state_patch_applies(i, node->state)) {
                schultz_resolved_apply(&next, &style->states[i], theme);
            }
        }
    }

    for (i = 0; i < SCHULTZ_STYLE_STATE_COUNT; i++) {
        if (schultz_state_patch_applies(i, node->state)) {
            schultz_resolved_apply(&next, &node->state_patches[i], theme);
        }
    }

    node->resolved = next;

    /*
     * The property table decides what a change costs. A colour change
     * repaints and nothing more; a padding change must also relayout.
     */
    if (schultz_resolved_differs(&previous, &next)) {
        if (schultz_resolved_layout_differs(&previous, &next)) {
            schultz_node_mark_layout(tree, node);
            /* Layout properties reach the panes through these fields. */
            node->padding = schultz_resolved_number(&next,
                                                    SCHULTZ_PROP_PADDING);
            node->gap     = schultz_resolved_number(&next, SCHULTZ_PROP_GAP);
            node->hints.min_width   = schultz_resolved_number(&next,
                                            SCHULTZ_PROP_MIN_WIDTH);
            node->hints.min_height  = schultz_resolved_number(&next,
                                            SCHULTZ_PROP_MIN_HEIGHT);
            node->hints.pref_width  = schultz_resolved_number(&next,
                                            SCHULTZ_PROP_PREF_WIDTH);
            node->hints.pref_height = schultz_resolved_number(&next,
                                            SCHULTZ_PROP_PREF_HEIGHT);
            node->hints.max_width   = schultz_resolved_number(&next,
                                            SCHULTZ_PROP_MAX_WIDTH);
            node->hints.max_height  = schultz_resolved_number(&next,
                                            SCHULTZ_PROP_MAX_HEIGHT);
        }
        /*
         * Before marking, because marking reads it. A shadow reaches its
         * distance away plus the spread of its blur, and the rasterizer's
         * blur is three box passes, so three times the blur covers it with a
         * unit of slack for rounding.
         */
        {
            float was = node->shadow_reach;
            float want = 0.0f;

            if (schultz_resolved_color(&next,
                                       SCHULTZ_PROP_SHADOW_COLOR).a > 0u) {
                want = schultz_resolved_number(&next,
                           SCHULTZ_PROP_SHADOW_DISTANCE) +
                       schultz_resolved_number(&next,
                           SCHULTZ_PROP_SHADOW_BLUR) * 3.0f + 1.0f;
            }
            /*
             * Marked with whichever reach is larger, because a shadow that
             * has just been taken away still has to be painted over where it
             * used to be. Then the new one is kept, for the next change.
             */
            node->shadow_reach = (was > want) ? was : want;
            schultz_node_mark(tree, node);
            node->shadow_reach = want;
        }
    }
    node->style_dirty = 0;
}

/*
 * Pre-order walk. A node resolves when it is marked, or when an ancestor
 * resolved and may have changed an inherited property.
 */
static void schultz_tree_resolve_walk(schultz_tree *tree, schultz_node *node,
                                      schultz_node *parent, int32_t forced)
{
    int32_t resolved_here = 0;
    uint32_t i;

    /*
     * Nothing stale here, nothing stale below, and no ancestor resolved into
     * this branch, so it is already current. This is the whole point of
     * style_below: an idle frame on a large tree touches the root and stops.
     */
    if (!node->style_dirty && !node->style_below && !forced) {
        return;
    }

    if (node->style_dirty || forced) {
        schultz_node_resolve_one(tree, node, parent);
        resolved_here = 1;
    }

    for (i = 0; i < node->child_count; i++) {
        schultz_node *child = schultz_node_get(tree, node->children[i]);
        if (child != NULL) {
            schultz_tree_resolve_walk(tree, child, node, resolved_here);
        }
    }
    node->style_below = 0;
}

int32_t schultz_tree_resolve_styles(schultz_tree *tree)
{
    schultz_node *root;

    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    root = schultz_node_get(tree, tree->root);
    if (root == NULL) {
        return SCHULTZ_OK;
    }
    schultz_tree_resolve_walk(tree, root, NULL, 0);
    return SCHULTZ_OK;
}

int32_t schultz_node_style_dirty(const schultz_tree *tree,
                                 schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? 0 : (int32_t)(node->style_dirty != 0);
}

int32_t schultz_node_layout_dirty(const schultz_tree *tree,
                                  schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return 0;
    }
    /*
     * The node itself, or anything under it. Asking the root is how a host
     * finds out whether laying anything out is worth the time.
     */
    return (int32_t)(node->layout_dirty != 0u || node->layout_below != 0u);
}

int32_t schultz_node_invalidate_layout(schultz_tree *tree,
                                       schultz_handle handle)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_node_mark_layout(tree, node);
    schultz_node_mark(tree, node);
    return SCHULTZ_OK;
}

void schultz_tree_clear_layout_dirty(schultz_tree *tree)
{
    uint32_t i;

    if (tree == NULL) {
        return;
    }
    for (i = 0; i < tree->nodes.capacity; i++) {
        if (tree->nodes.slots[i].live) {
            schultz_node *node =
                (schultz_node *)tree->nodes.slots[i].object;

            node->layout_dirty = 0;
            node->layout_below = 0;
        }
    }
}

/* ------------------------------------------------------------ layout */

int32_t schultz_node_set_pane(schultz_tree *tree, schultz_handle handle,
                              const schultz_pane_vtable *vtable)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    node->pane = vtable;
    /* A different pane places the children differently, and measures to a
     * different size, so both this node and the one above it are stale. */
    schultz_node_mark_layout(tree, node);
    schultz_node_mark(tree, node);
    return SCHULTZ_OK;
}

const schultz_pane_vtable *schultz_node_get_pane(const schultz_tree *tree,
                                                 schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? NULL : node->pane;
}

/*
 * A size is two style properties, one per axis, and setting one pair is what
 * every one of these does.
 *
 * Writing the properties is what lets a style or a theme have an opinion
 * about a size, and writing the cached fields beside them is what lets a
 * caller that never resolves still see its own change. A pair at a time
 * rather than all six: a call that took every size at once could not set one
 * without being handed the other five, so a caller either read them back
 * first or quietly replaced settings it had never heard of.
 *
 * SCHULTZ_SIZE_UNSET is a negative number, and negative already meant
 * "nothing said" for a preferred size and a maximum. A minimum says the same
 * thing with zero, so it is mapped here rather than asking a caller to
 * remember which is which.
 */
static int32_t schultz_node_set_size_pair(schultz_tree *tree,
                                          schultz_handle handle,
                                          uint32_t width_property,
                                          uint32_t height_property,
                                          float width, float height,
                                          float *out_width, float *out_height)
{
    schultz_node *node = schultz_node_get(tree, handle);
    int32_t result;

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    result = schultz_patch_set(&node->inline_patch, width_property,
                               schultz_value_number(width));
    if (result == SCHULTZ_OK) {
        result = schultz_patch_set(&node->inline_patch, height_property,
                                   schultz_value_number(height));
    }
    if (result != SCHULTZ_OK) {
        return result;
    }
    *out_width  = width;
    *out_height = height;

    schultz_node_mark_style(tree, node);
    schultz_node_mark_layout(tree, node);
    schultz_node_mark(tree, node);
    return SCHULTZ_OK;
}

int32_t schultz_node_set_pref_size(schultz_tree *tree, schultz_handle handle,
                                   float width, float height)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_node_set_size_pair(tree, handle, SCHULTZ_PROP_PREF_WIDTH,
                                      SCHULTZ_PROP_PREF_HEIGHT,
                                      width, height,
                                      &node->hints.pref_width,
                                      &node->hints.pref_height);
}

int32_t schultz_node_set_min_size(schultz_tree *tree, schultz_handle handle,
                                  float width, float height)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* No minimum is zero here, whatever the caller spelled it. */
    if (width < 0.0f) {
        width = 0.0f;
    }
    if (height < 0.0f) {
        height = 0.0f;
    }
    return schultz_node_set_size_pair(tree, handle, SCHULTZ_PROP_MIN_WIDTH,
                                      SCHULTZ_PROP_MIN_HEIGHT,
                                      width, height,
                                      &node->hints.min_width,
                                      &node->hints.min_height);
}

int32_t schultz_node_set_max_size(schultz_tree *tree, schultz_handle handle,
                                  float width, float height)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_node_set_size_pair(tree, handle, SCHULTZ_PROP_MAX_WIDTH,
                                      SCHULTZ_PROP_MAX_HEIGHT,
                                      width, height,
                                      &node->hints.max_width,
                                      &node->hints.max_height);
}

/* Hands back one pair, skipping whichever the caller did not ask for. */
static int32_t schultz_node_size_pair(const schultz_tree *tree,
                                      schultz_handle handle, float width,
                                      float height, float *out_width,
                                      float *out_height)
{
    if (schultz_node_get(tree, handle) == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (out_width != NULL) {
        *out_width = width;
    }
    if (out_height != NULL) {
        *out_height = height;
    }
    return SCHULTZ_OK;
}

int32_t schultz_node_get_pref_size(const schultz_tree *tree,
                                   schultz_handle handle, float *out_width,
                                   float *out_height)
{
    const schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_node_size_pair(tree, handle, node->hints.pref_width,
                                  node->hints.pref_height, out_width,
                                  out_height);
}

int32_t schultz_node_get_min_size(const schultz_tree *tree,
                                  schultz_handle handle, float *out_width,
                                  float *out_height)
{
    const schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_node_size_pair(tree, handle, node->hints.min_width,
                                  node->hints.min_height, out_width,
                                  out_height);
}

int32_t schultz_node_get_max_size(const schultz_tree *tree,
                                  schultz_handle handle, float *out_width,
                                  float *out_height)
{
    const schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_node_size_pair(tree, handle, node->hints.max_width,
                                  node->hints.max_height, out_width,
                                  out_height);
}

int32_t schultz_node_size_hints(const schultz_tree *tree,
                                schultz_handle handle,
                                schultz_size_hints *out_hints)
{
    const schultz_node *node;

    if (out_hints == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node = schultz_node_get(tree, handle);
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    *out_hints = node->hints;
    return SCHULTZ_OK;
}

int32_t schultz_node_set_layout_params(schultz_tree *tree,
                                       schultz_handle handle,
                                       const schultz_layout_params *params)
{
    schultz_node *node;

    if (params == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node = schultz_node_get(tree, handle);
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    node->params = *params;
    /*
     * Every one of these is read by the pane above, not by this node, so the
     * parent has to place its children again. Without it an alignment or a
     * grid cell changed after the first layout sat in the struct doing
     * nothing until something else happened to dirty the tree, and then took
     * effect all at once. That looks like the call being ignored, because
     * from the outside it was.
     */
    schultz_node_mark_layout(tree, node);
    schultz_node_mark(tree, node);
    return SCHULTZ_OK;
}

int32_t schultz_node_get_layout_params(const schultz_tree *tree,
                                       schultz_handle handle,
                                       schultz_layout_params *out_params)
{
    const schultz_node *node;

    if (out_params == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    node = schultz_node_get(tree, handle);
    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    *out_params = node->params;
    return SCHULTZ_OK;
}

int32_t schultz_node_set_spacing(schultz_tree *tree, schultz_handle handle,
                                 float padding, float gap)
{
    schultz_node *node = schultz_node_get(tree, handle);
    int32_t result;

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    padding = (padding > 0.0f) ? padding : 0.0f;
    gap     = (gap > 0.0f) ? gap : 0.0f;

    result = schultz_patch_set(&node->inline_patch, SCHULTZ_PROP_PADDING,
                               schultz_value_number(padding));
    if (result == SCHULTZ_OK) {
        result = schultz_patch_set(&node->inline_patch, SCHULTZ_PROP_GAP,
                                   schultz_value_number(gap));
    }
    if (result != SCHULTZ_OK) {
        return result;
    }

    node->padding = padding;
    node->gap     = gap;
    schultz_node_mark_style(tree, node);
    schultz_node_mark_layout(tree, node);
    schultz_node_mark(tree, node);
    return SCHULTZ_OK;
}

/* Replaces one owned track array with a clamped copy of source. */
static int32_t schultz_node_set_tracks(schultz_grid_track **slot,
                                       uint32_t *slot_count,
                                       const schultz_grid_track *source,
                                       uint32_t count)
{
    schultz_grid_track *copy = NULL;

    if (count > (uint32_t)SCHULTZ_GRID_MAX_TRACKS) {
        count = (uint32_t)SCHULTZ_GRID_MAX_TRACKS;
    }
    if (source != NULL && count > 0u) {
        copy = (schultz_grid_track *)malloc((size_t)count * sizeof(*copy));
        if (copy == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        memcpy(copy, source, (size_t)count * sizeof(*copy));
    } else {
        count = 0u;
    }

    free(*slot);
    *slot       = copy;
    *slot_count = count;
    return SCHULTZ_OK;
}

int32_t schultz_node_set_grid_tracks(schultz_tree *tree, schultz_handle handle,
                                     const schultz_grid_track *columns,
                                     uint32_t column_count,
                                     const schultz_grid_track *rows,
                                     uint32_t row_count)
{
    schultz_node *node = schultz_node_get(tree, handle);
    int32_t result;

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    result = schultz_node_set_tracks(&node->columns, &node->column_count,
                                     columns, column_count);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_node_set_tracks(&node->rows, &node->row_count, rows,
                                     row_count);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /* The grid's own pane reads these, so the grid places again. */
    schultz_node_mark_layout(tree, node);
    schultz_node_mark(tree, node);
    return SCHULTZ_OK;
}

const schultz_grid_track *schultz_node_get_grid_tracks(
    const schultz_tree *tree, schultz_handle handle, int32_t want_rows,
    uint32_t *out_count)
{
    const schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL || out_count == NULL) {
        if (out_count != NULL) {
            *out_count = 0;
        }
        return NULL;
    }
    *out_count = want_rows ? node->row_count : node->column_count;
    return want_rows ? node->rows : node->columns;
}

int32_t schultz_node_get_spacing(const schultz_tree *tree,
                                 schultz_handle handle, float *out_padding,
                                 float *out_gap)
{
    const schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (out_padding != NULL) {
        *out_padding = node->padding;
    }
    if (out_gap != NULL) {
        *out_gap = node->gap;
    }
    return SCHULTZ_OK;
}

/* -------------------------------------------------- accessibility schema */

int32_t schultz_node_set_role(schultz_tree *tree, schultz_handle handle,
                              uint32_t role)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    node->role = role;
    return SCHULTZ_OK;
}

uint32_t schultz_node_get_role(const schultz_tree *tree,
                               schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? (uint32_t)SCHULTZ_ROLE_UNKNOWN : node->role;
}

/* Replaces an owned string with a copy of source, or NULL. */
static int32_t schultz_node_set_string(char **slot, const char *source)
{
    char *copy = NULL;

    if (source != NULL) {
        size_t length = strlen(source) + 1u;
        copy = (char *)malloc(length);
        if (copy == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        memcpy(copy, source, length);
    }
    free(*slot);
    *slot = copy;
    return SCHULTZ_OK;
}

int32_t schultz_node_set_name(schultz_tree *tree, schultz_handle handle,
                              const char *name)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_node_set_string(&node->name, name);
}

const char *schultz_node_get_name(const schultz_tree *tree,
                                  schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? NULL : node->name;
}

int32_t schultz_node_set_value(schultz_tree *tree, schultz_handle handle,
                               const char *value)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_node_set_string(&node->value, value);
}

const char *schultz_node_get_value(const schultz_tree *tree,
                                   schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? NULL : node->value;
}

int32_t schultz_node_set_actions(schultz_tree *tree, schultz_handle handle,
                                 uint32_t actions)
{
    schultz_node *node = schultz_node_get(tree, handle);

    if (node == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    node->actions = actions;
    return SCHULTZ_OK;
}

uint32_t schultz_node_get_actions(const schultz_tree *tree,
                                  schultz_handle handle)
{
    const schultz_node *node = schultz_node_get(tree, handle);
    return (node == NULL) ? 0 : node->actions;
}
