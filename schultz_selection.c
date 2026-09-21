/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_selection.c
 * @brief The selection area: one selection over many widgets.
 *
 * The area holds two ends, each a node and a byte offset. Everything else in
 * here is about turning that pair into "how much of you is selected" for each
 * widget underneath, which the widgets then paint themselves.
 *
 * Reading order is tree order: a node comes before its children, and children
 * come in the order they were added. That is the same order the paint walk
 * uses, so what a person sees selected is what a walk in this order covers.
 */

#include "schultz_selection.h"

#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_image.h"
#include "schultz_resource.h"
#include "schultz_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief An area's two ends. */
typedef struct {
    schultz_handle from;    /**< Where the selection started. */
    uint32_t       from_at; /**< Byte offset within it. */
    schultz_handle to;      /**< Where it ends now, which moves as it drags. */
    uint32_t       to_at;   /**< Byte offset within that. */
    uint32_t       live;    /**< Nonzero when there is a selection at all. */
    uint32_t       dragging;/**< Nonzero between a press and its release. */
    char          *taken;   /**< The last text handed out, owned here. */
    char          *marked;  /**< The last markup handed out, owned here. */
    uint64_t       budget;  /**< Most bytes a produced result may reach. */
    int32_t        cut;     /**< Nonzero when the budget stopped the last one. */
} schultz_selection_data;

static const schultz_widget_vtable schultz_selection_area_widget;

/* The area's own state, or NULL when the handle is not an area. */
static schultz_selection_data *schultz_selection_of(const schultz_tree *tree,
                                                    schultz_handle area)
{
    if (schultz_node_widget(tree, area) != &schultz_selection_area_widget) {
        return NULL;
    }
    return (schultz_selection_data *)schultz_node_widget_data(tree, area);
}

/* What a node offers a selection, or NULL when it takes no part. */
static const schultz_selectable_vtable *schultz_selection_part(
    const schultz_tree *tree, schultz_handle node)
{
    const schultz_widget_vtable *widget = schultz_node_widget(tree, node);

    if (widget == NULL || widget->selectable == NULL) {
        return NULL;
    }
    /*
     * The three that make a participant. A widget missing any of them cannot
     * be told its share, so it is treated as taking no part rather than as a
     * half participant that behaves strangely.
     */
    if (widget->selectable->length == NULL ||
        widget->selectable->set_range == NULL) {
        return NULL;
    }
    return widget->selectable;
}

/*
 * Walks a subtree in reading order, stopping at a nested area.
 *
 * A nested area owns its own selection, so the outer one must not reach into
 * it. Stopping the walk at its root is the whole of that rule: the inner
 * area's participants are never told anything by the outer one.
 */
typedef void (*schultz_selection_visit_fn)(schultz_tree *tree,
                                           schultz_handle node, void *context);

static void schultz_selection_walk(schultz_tree *tree, schultz_handle node,
                                   schultz_handle root,
                                   schultz_selection_visit_fn visit,
                                   void *context)
{
    uint32_t count;
    uint32_t i;

    if (node != root &&
        schultz_node_widget(tree, node) == &schultz_selection_area_widget) {
        return; /* an area inside an area keeps its own selection */
    }
    visit(tree, node, context);

    count = schultz_node_child_count(tree, node);
    for (i = 0; i < count; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(tree, node, i, &child) == SCHULTZ_OK) {
            schultz_selection_walk(tree, child, root, visit, context);
        }
    }
}

/* ------------------------------------------------------- reading order */

/** @brief Finds which of two nodes comes first, and whether both are here. */
typedef struct {
    schultz_handle one;
    schultz_handle two;
    schultz_handle first; /**< Whichever was met first. */
    uint32_t       found; /**< How many of the two were met. */
} schultz_selection_order;

static void schultz_selection_order_visit(schultz_tree *tree,
                                          schultz_handle node, void *context)
{
    schultz_selection_order *order = (schultz_selection_order *)context;

    (void)tree;
    if (node != order->one && node != order->two) {
        return;
    }
    if (order->first == SCHULTZ_HANDLE_NONE) {
        order->first = node;
    }
    /*
     * Counted rather than flagged, because the two ends are the same node
     * whenever a selection sits inside one paragraph, which is most of them.
     */
    order->found += (order->one == order->two) ? 2u : 1u;
}

/* ------------------------------------------------------- telling each one */

/** @brief The state of a walk that is handing out shares. */
typedef struct {
    schultz_handle first;
    uint32_t       first_at;
    schultz_handle last;
    uint32_t       last_at;
    int32_t        started; /**< Nonzero once the first end has been met. */
    int32_t        ended;   /**< Nonzero once the last end has been passed. */
} schultz_selection_share;

static void schultz_selection_share_visit(schultz_tree *tree,
                                          schultz_handle node, void *context)
{
    schultz_selection_share *share = (schultz_selection_share *)context;
    const schultz_selectable_vtable *part = schultz_selection_part(tree, node);
    uint32_t length;
    uint32_t low = 0u;
    uint32_t high = 0u;

    if (part == NULL) {
        return;
    }
    length = part->length(tree, node);

    if (!share->started && node == share->first) {
        share->started = 1;
        low  = share->first_at;
        high = (node == share->last) ? share->last_at : length;
        if (node == share->last) {
            share->ended = 1;
        }
    } else if (share->started && !share->ended) {
        low  = 0u;
        high = (node == share->last) ? share->last_at : length;
        if (node == share->last) {
            share->ended = 1;
        }
    }

    /*
     * Clamped here rather than trusted, because the offsets came from a
     * caller and the text may have changed since they were worked out.
     */
    if (low > length) {
        low = length;
    }
    if (high > length) {
        high = length;
    }
    if (high < low) {
        high = low;
    }
    part->set_range(tree, node, low, high);
}

/* Hands every participant in the area its share of the current range. */
static void schultz_selection_apply(schultz_tree *tree, schultz_handle area,
                                    schultz_selection_data *data)
{
    schultz_selection_share share;

    share.started = 0;
    share.ended   = 0;
    if (!data->live) {
        /* Nothing selected: every participant is told so, which is the same
         * walk with a range nobody is inside. */
        share.first    = SCHULTZ_HANDLE_NONE;
        share.first_at = 0u;
        share.last     = SCHULTZ_HANDLE_NONE;
        share.last_at  = 0u;
    } else {
        schultz_selection_order order;

        order.one   = data->from;
        order.two   = data->to;
        order.first = SCHULTZ_HANDLE_NONE;
        order.found = 0u;
        schultz_selection_walk(tree, area, area,
                               schultz_selection_order_visit, &order);

        if (order.first == data->to && data->from != data->to) {
            /* Dragged backwards, so the end that moves comes first. */
            share.first    = data->to;
            share.first_at = data->to_at;
            share.last     = data->from;
            share.last_at  = data->from_at;
        } else {
            share.first    = data->from;
            share.first_at = data->from_at;
            share.last     = data->to;
            share.last_at  = data->to_at;
        }
        /* One node, dragged right to left: the offsets need sorting too. */
        if (share.first == share.last && share.first_at > share.last_at) {
            uint32_t swap = share.first_at;

            share.first_at = share.last_at;
            share.last_at  = swap;
        }
    }
    schultz_selection_walk(tree, area, area, schultz_selection_share_visit,
                           &share);
}

/* ------------------------------------------------------------- the widget */

/* ------------------------------------------------------- what is under it */

/** @brief The participant a window point is over, or nearest to. */
typedef struct {
    schultz_point  at;       /**< In window coordinates. */
    schultz_handle found;    /**< The best one so far. */
    schultz_rect   bounds;   /**< Its rectangle. */
    float          away;     /**< How far the point is from that rectangle. */
} schultz_selection_under;

/* How far a point lies outside a rectangle. Zero when it is inside. */
static float schultz_selection_away(schultz_rect rect, schultz_point at)
{
    float dx = 0.0f;
    float dy = 0.0f;

    if (at.x < rect.x) {
        dx = rect.x - at.x;
    } else if (at.x > schultz_rect_right(rect)) {
        dx = at.x - schultz_rect_right(rect);
    }
    if (at.y < rect.y) {
        dy = rect.y - at.y;
    } else if (at.y > schultz_rect_bottom(rect)) {
        dy = at.y - schultz_rect_bottom(rect);
    }
    /*
     * Weighted towards the vertical, because a page of prose is stacked and
     * a pointer that has run off the right hand edge of a line still means
     * that line rather than the one below it.
     */
    return dy * 8.0f + dx;
}

static void schultz_selection_under_visit(schultz_tree *tree,
                                          schultz_handle node, void *context)
{
    schultz_selection_under *under = (schultz_selection_under *)context;
    const schultz_selectable_vtable *part = schultz_selection_part(tree, node);
    schultz_rect bounds;
    float away;

    /*
     * Taking no part means having no length, which is how a label that was
     * never made selectable declines. Without this check a drag would stop
     * on one, because it is still a label and still has the vtable.
     */
    if (part == NULL || part->offset_at == NULL ||
        part->length(tree, node) == 0u) {
        return;
    }
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return;
    }
    away = schultz_selection_away(bounds, under->at);
    /*
     * Nearest wins, and ties go to the later one, which is deeper or further
     * along in reading order. Directly over something is a distance of zero,
     * so the common case falls out of the same comparison.
     */
    if (under->found == SCHULTZ_HANDLE_NONE || away <= under->away) {
        under->found  = node;
        under->bounds = bounds;
        under->away   = away;
    }
}

/*
 * Which participant a window point means, and where inside it.
 *
 * Nearest rather than only what is underneath. A drag that runs past the last
 * paragraph, into a gap between two, or out of the area altogether still
 * means something: the end of what it ran past. Freezing the selection where
 * the pointer last met a widget loses everything the drag crossed on the way,
 * which is what a person sees as the selection stopping for no reason.
 */
static int32_t schultz_selection_at(schultz_tree *tree, schultz_handle area,
                                    schultz_point at,
                                    schultz_handle *out_node,
                                    uint32_t *out_offset)
{
    schultz_selection_under under;
    const schultz_selectable_vtable *part;
    schultz_point local;

    under.at    = at;
    under.found = SCHULTZ_HANDLE_NONE;
    under.away  = 0.0f;
    under.bounds = schultz_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
    schultz_selection_walk(tree, area, area, schultz_selection_under_visit,
                           &under);
    if (under.found == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    part = schultz_selection_part(tree, under.found);
    if (part == NULL || part->offset_at == NULL) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    /*
     * Pulled onto the widget before it is asked where inside it the point
     * falls. A point below it lands at its bottom, which reads as the end of
     * it, and one above lands at its top, which reads as the beginning.
     */
    local.x = at.x;
    local.y = at.y;
    if (local.x < under.bounds.x) {
        local.x = under.bounds.x;
    }
    if (local.x > schultz_rect_right(under.bounds)) {
        local.x = schultz_rect_right(under.bounds);
    }
    if (local.y < under.bounds.y) {
        local.y = under.bounds.y;
    }
    if (local.y > schultz_rect_bottom(under.bounds)) {
        local.y = schultz_rect_bottom(under.bounds);
    }
    *out_node   = under.found;
    *out_offset = part->offset_at(tree, under.found,
                                  schultz_point_make(local.x - under.bounds.x,
                                                     local.y - under.bounds.y));
    return SCHULTZ_OK;
}

/* ------------------------------------------------------------ the dragging */

/*
 * Takes the selection away from whoever had it.
 *
 * There is one selection in a window, the same as there is one on a desktop:
 * starting a new one anywhere ends the one before it. Without this two
 * highlights sit on screen at once and only one of them answers a copy, which
 * is exactly what it looks like when it goes wrong.
 *
 * The old owner may be another area or a single widget that owns its own
 * selection, and neither needs naming: an area is cleared as an area, and
 * anything else is told through the same interface a participant is told
 * with, which is that nothing in it is selected.
 */
static void schultz_selection_take(schultz_tree *tree, schultz_handle area)
{
    schultz_handle had = schultz_tree_selection_owner(tree);

    if (had != SCHULTZ_HANDLE_NONE && had != area) {
        if (schultz_selection_of(tree, had) != NULL) {
            schultz_selection_area_clear(tree, had);
        } else {
            const schultz_selectable_vtable *part =
                schultz_selection_part(tree, had);

            if (part != NULL) {
                part->set_range(tree, had, 0u, 0u);
            }
        }
    }
    schultz_tree_set_selection_owner(tree, area);
}

static int32_t schultz_selection_event(schultz_tree *tree, schultz_handle area,
                                       const schultz_event *event)
{
    schultz_selection_data *data = schultz_selection_of(tree, area);
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t offset = 0u;

    if (data == NULL) {
        return SCHULTZ_OK;
    }
    switch (event->type) {
    case SCHULTZ_EVENT_MOUSE_DOWN:
        /*
         * A finger is left to the widget underneath. Dragging a finger across
         * text is how a page is scrolled, so a touch selection starts with a
         * hold, and that gesture lives on the label. Selecting across widgets
         * by touch is not built yet and the label's own behaviour is better
         * than nothing in the meantime.
         */
        if (event->source == SCHULTZ_POINTER_TOUCH) {
            return SCHULTZ_OK;
        }
        if (schultz_selection_at(tree, area, event->position, &node, &offset)
                != SCHULTZ_OK) {
            /* Pressed on nothing selectable, which means: start again. */
            schultz_selection_area_clear(tree, area);
            return SCHULTZ_OK;
        }
        schultz_selection_take(tree, area);
        data->from     = node;
        data->from_at  = offset;
        data->to       = node;
        data->to_at    = offset;
        data->live     = 1u;
        data->dragging = 1u;
        schultz_tree_set_selection_owner(tree, area);
        schultz_selection_apply(tree, area, data);
        return SCHULTZ_EVENT_CONSUMED;

    case SCHULTZ_EVENT_DRAG:
        if (!data->dragging) {
            return SCHULTZ_OK;
        }
        /*
         * A drag that has wandered off every participant keeps the end where
         * it was rather than snapping somewhere arbitrary. Coming back over
         * one picks it up again.
         */
        if (schultz_selection_at(tree, area, event->position, &node, &offset)
                == SCHULTZ_OK) {
            data->to    = node;
            data->to_at = offset;
            schultz_selection_apply(tree, area, data);
        }
        return SCHULTZ_EVENT_CONSUMED;

    case SCHULTZ_EVENT_MOUSE_UP:
        data->dragging = 0u;
        return SCHULTZ_OK;

    case SCHULTZ_EVENT_KEY_DOWN:
        if ((event->modifiers & SCHULTZ_MOD_CTRL) != 0u &&
            (event->key == 'c' || event->key == 'C')) {
            /*
             * No options given, so the tree's own are used. A picture in the
             * selection goes with the text, which is what a person pressing
             * this key expects and what every other application does.
             */
            if (schultz_selection_area_copy(tree, area, NULL) == SCHULTZ_OK) {
                return SCHULTZ_EVENT_CONSUMED;
            }
        }
        return SCHULTZ_OK;

    default:
        return SCHULTZ_OK;
    }
}

static void schultz_selection_destroy(void *data)
{
    schultz_selection_data *area = (schultz_selection_data *)data;

    if (area != NULL) {
        free(area->taken);
        free(area->marked);
    }
    free(data);
}

static const schultz_widget_vtable schultz_selection_area_widget = {
    NULL,                          /* paint: an area draws nothing */
    schultz_selection_event,
    NULL,                          /* tick */
    schultz_selection_destroy,
    NULL,                          /* an area is not itself selectable */
    NULL                           /* and holds no text of its own */
};

/* ---------------------------------------------------------- the interface */

int32_t schultz_selection_area_create(schultz_tree *tree,
                                      schultz_handle parent,
                                      schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_selection_data *data;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_node = SCHULTZ_HANDLE_NONE;
    if (tree == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    data = (schultz_selection_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->from = SCHULTZ_HANDLE_NONE;
    data->to   = SCHULTZ_HANDLE_NONE;
    /*
     * Twenty megabytes. Text never comes near it, so for the common case this
     * is no limit at all; it is there for the format that carries a picture,
     * where one screenshot is already megabytes.
     */
    data->budget = 20u * 1024u * 1024u;
    /*
     * A group as far as a screen reader is concerned: it holds things and
     * means nothing of its own. It needs to be a published node rather than
     * an invisible one, because it is what carries a selection whose two ends
     * are in different labels beneath it.
     */
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_GROUP);
    schultz_node_set_widget(tree, node, &schultz_selection_area_widget, data);
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_selection_area_set_range(schultz_tree *tree,
                                         schultz_handle area,
                                         schultz_handle from, uint32_t from_at,
                                         schultz_handle to, uint32_t to_at)
{
    schultz_selection_data *data = schultz_selection_of(tree, area);
    schultz_selection_order order;

    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /*
     * Both ends have to be inside this area. An end somewhere else would
     * describe a range this area cannot walk, and silently keeping it would
     * leave a selection nothing ever paints.
     */
    order.one   = from;
    order.two   = to;
    order.first = SCHULTZ_HANDLE_NONE;
    order.found = 0u;
    schultz_selection_walk(tree, area, area, schultz_selection_order_visit,
                           &order);
    if (order.found < 2u) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }

    data->from    = from;
    data->from_at = from_at;
    data->to      = to;
    data->to_at   = to_at;
    data->live    = 1u;
    schultz_selection_apply(tree, area, data);
    return SCHULTZ_OK;
}

int32_t schultz_selection_area_ends(const schultz_tree *tree,
                                    schultz_handle area,
                                    schultz_handle *out_from,
                                    uint32_t *out_from_at,
                                    schultz_handle *out_to,
                                    uint32_t *out_to_at)
{
    schultz_selection_data *data = schultz_selection_of(tree, area);
    schultz_selection_order order;
    schultz_handle first;
    uint32_t first_at;
    schultz_handle last;
    uint32_t last_at;

    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (!data->live) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    order.one   = data->from;
    order.two   = data->to;
    order.first = SCHULTZ_HANDLE_NONE;
    order.found = 0u;
    schultz_selection_walk((schultz_tree *)tree, area, area,
                           schultz_selection_order_visit, &order);

    if (order.first == data->to && data->from != data->to) {
        first = data->to;    first_at = data->to_at;
        last  = data->from;  last_at  = data->from_at;
    } else {
        first = data->from;  first_at = data->from_at;
        last  = data->to;    last_at  = data->to_at;
    }
    if (first == last && first_at > last_at) {
        uint32_t swap = first_at;

        first_at = last_at;
        last_at  = swap;
    }
    if (out_from    != NULL) { *out_from    = first;    }
    if (out_from_at != NULL) { *out_from_at = first_at; }
    if (out_to      != NULL) { *out_to      = last;     }
    if (out_to_at   != NULL) { *out_to_at   = last_at;  }
    return SCHULTZ_OK;
}

/* ---------------------------------------------------------------- the text */

/** @brief Gathers each participant's share into one growing string. */
typedef struct {
    char    *text;   /**< Owned, NUL terminated, or NULL until the first. */
    size_t   used;   /**< Bytes in it, not counting the terminator. */
    size_t   room;   /**< Bytes allocated. */
    int32_t  failed; /**< Nonzero once an allocation did not come back. */
    int32_t  any;    /**< Nonzero once something has been added. */
    uint64_t budget; /**< Most it may reach, or zero for no limit. */
    int32_t  cut;    /**< Nonzero once the budget stopped it. */
} schultz_selection_gather;

static void schultz_selection_add(schultz_selection_gather *gather,
                                  const char *bytes, size_t length)
{
    if (gather->failed || gather->cut || length == 0u) {
        return;
    }
    /*
     * Stopping here rather than halfway through a block. A copy that ends
     * mid-sentence is worse than one that ends a paragraph early and says so.
     */
    if (gather->budget != 0u &&
        (uint64_t)(gather->used + length) > gather->budget) {
        gather->cut = 1;
        return;
    }
    if (gather->used + length + 1u > gather->room) {
        size_t want = (gather->used + length + 1u) * 2u;
        char *bigger = (char *)realloc(gather->text, want);

        if (bigger == NULL) {
            gather->failed = 1;
            return;
        }
        gather->text = bigger;
        gather->room = want;
    }
    memcpy(gather->text + gather->used, bytes, length);
    gather->used += length;
    gather->text[gather->used] = '\0';
}

static void schultz_selection_gather_visit(schultz_tree *tree,
                                           schultz_handle node, void *context)
{
    schultz_selection_gather *gather = (schultz_selection_gather *)context;
    const schultz_selectable_vtable *part = schultz_selection_part(tree, node);
    const char *text;
    uint32_t low = 0u;
    uint32_t high = 0u;

    /*
     * A picture adds nothing here. It is in the range and it will be carried
     * by a format that can hold one, but plain text has nowhere to put it.
     */
    if (part == NULL || part->text == NULL || part->range == NULL) {
        return;
    }
    if (part->range == NULL) {
        return;
    }
    part->range(tree, node, &low, &high);
    if (high <= low) {
        return;
    }
    text = part->text(tree, node);
    if (text == NULL) {
        return;
    }
    /*
     * The break and the text are weighed together, not one after the other.
     * Checking them apart lets the break through and then stops the text,
     * which ends the copy on a dangling separator.
     */
    {
        size_t need = (gather->any ? 1u : 0u) + (size_t)(high - low);

        if (gather->budget != 0u &&
            (uint64_t)(gather->used + need) > gather->budget) {
            gather->cut = 1;
            return;
        }
    }
    /*
     * A line break between one widget's text and the next. Two paragraphs
     * pasted together with nothing between them read as one sentence, which
     * is not what was on the screen.
     */
    if (gather->any) {
        schultz_selection_add(gather, "\n", 1u);
    }
    schultz_selection_add(gather, text + low, (size_t)(high - low));
    gather->any = 1;
}

/* One node, drawn and written out as a file; defined further down. */
static int32_t schultz_selection_picture_of(schultz_tree *tree,
                                            schultz_handle node,
                                            uint32_t format,
                                            const void **out_bytes,
                                            uint64_t *out_length);

/* ---------------------------------------------------------------- as html */

/* Appends one string, obeying the budget the gather is carrying. */
static void schultz_selection_say(schultz_selection_gather *gather,
                                  const char *text)
{
    schultz_selection_add(gather, text, strlen(text));
}

/*
 * Text with the four characters that mean something to a parser taken out.
 *
 * An ampersand and the two angle brackets would be read as markup, and a
 * double quote ends an attribute. Everything else goes through as it is,
 * including every byte of UTF-8, because the document says it is UTF-8.
 */
static void schultz_selection_escape(schultz_selection_gather *gather,
                                     const char *text, size_t length)
{
    size_t at = 0;
    size_t i;

    for (i = 0; i < length; i++) {
        const char *swap = NULL;

        switch (text[i]) {
        case '&':  swap = "&amp;";  break;
        case '<':  swap = "&lt;";   break;
        case '>':  swap = "&gt;";   break;
        case '"':  swap = "&quot;"; break;
        default:   break;
        }
        if (swap == NULL) {
            continue;
        }
        schultz_selection_add(gather, text + at, i - at);
        schultz_selection_say(gather, swap);
        at = i + 1u;
    }
    schultz_selection_add(gather, text + at, length - at);
}

/* Bytes as base64, which is how a picture travels inside a document. */
static void schultz_selection_base64(schultz_selection_gather *gather,
                                     const unsigned char *bytes, size_t length)
{
    static const char *const set =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char out[4];
    size_t i;

    for (i = 0; i + 2u < length; i += 3u) {
        uint32_t word = ((uint32_t)bytes[i] << 16) |
                        ((uint32_t)bytes[i + 1u] << 8) | bytes[i + 2u];

        out[0] = set[(word >> 18) & 0x3Fu];
        out[1] = set[(word >> 12) & 0x3Fu];
        out[2] = set[(word >> 6) & 0x3Fu];
        out[3] = set[word & 0x3Fu];
        schultz_selection_add(gather, out, 4u);
    }
    /* The last one or two bytes, padded out to four characters. */
    if (i < length) {
        uint32_t word = (uint32_t)bytes[i] << 16;
        size_t left = length - i;

        if (left > 1u) {
            word |= (uint32_t)bytes[i + 1u] << 8;
        }
        out[0] = set[(word >> 18) & 0x3Fu];
        out[1] = set[(word >> 12) & 0x3Fu];
        out[2] = (left > 1u) ? set[(word >> 6) & 0x3Fu] : '=';
        out[3] = '=';
        schultz_selection_add(gather, out, 4u);
    }
}

/*
 * The style of one participant, as the attribute a paragraph carries.
 *
 * Taken from the node's own resolved style rather than from the theme,
 * because a host that set a colour or a size on one paragraph meant it, and
 * reading the theme instead would quietly lose exactly the formatting this
 * format exists to carry.
 */
static void schultz_selection_style(schultz_tree *tree, schultz_handle node,
                                    schultz_selection_gather *gather)
{
    const schultz_resolved_style *style = schultz_node_resolved(tree, node);
    schultz_color colour = schultz_resolved_color(style,
                                                  SCHULTZ_PROP_TEXT_COLOR);
    float size = schultz_resolved_number(style, SCHULTZ_PROP_FONT_SIZE);
    schultz_handle face = schultz_resolved_font(style, SCHULTZ_PROP_FONT);
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    char line[160];

    snprintf(line, sizeof(line),
             " style=\"font-size:%.0fpx;color:#%02x%02x%02x",
             (double)size, colour.r, colour.g, colour.b);
    schultz_selection_say(gather, line);
    /*
     * Only the one distinction worth making without a family name to give.
     * The toolkit holds a face, not a name a document could ask for, so what
     * can be said honestly is whether it was a code face.
     */
    if (fonts != NULL && schultz_font_is_fixed_pitch(fonts, face)) {
        schultz_selection_say(gather, ";font-family:monospace");
    }
    schultz_selection_say(gather, "\"");
}

/* Whether a span asks for anything a style attribute would carry. */
static int32_t schultz_selection_span_styled(const schultz_span *span)
{
    return (span->color.a != 0u || span->background.a != 0u ||
            span->size > 0.0f) ? 1 : 0;
}

/*
 * The markup one span asks for.
 *
 * Bold and the rest get the tags a reader of the document would expect,
 * because a word processor understands `<strong>` and would have to be taught
 * a style attribute. Colour and size have no tag, so they go in one.
 */
static void schultz_selection_span_open(schultz_selection_gather *gather,
                                        const schultz_span *span)
{
    char line[96];

    if (span == NULL) {
        return;
    }
    if (span->link != NULL) {
        schultz_selection_say(gather, "<a href=\"");
        schultz_selection_escape(gather, span->link, strlen(span->link));
        schultz_selection_say(gather, "\">");
    }
    if (schultz_selection_span_styled(span)) {
        schultz_selection_say(gather, "<span style=\"");
        if (span->size > 0.0f) {
            snprintf(line, sizeof(line), "font-size:%.0fpx;",
                     (double)span->size);
            schultz_selection_say(gather, line);
        }
        if (span->color.a != 0u) {
            snprintf(line, sizeof(line), "color:#%02x%02x%02x;",
                     span->color.r, span->color.g, span->color.b);
            schultz_selection_say(gather, line);
        }
        if (span->background.a != 0u) {
            snprintf(line, sizeof(line), "background-color:#%02x%02x%02x;",
                     span->background.r, span->background.g,
                     span->background.b);
            schultz_selection_say(gather, line);
        }
        schultz_selection_say(gather, "\">");
    }
    if (span->bold) {
        schultz_selection_say(gather, "<strong>");
    }
    if (span->italic) {
        schultz_selection_say(gather, "<em>");
    }
    if (span->underline) {
        schultz_selection_say(gather, "<u>");
    }
    if (span->strikethrough) {
        schultz_selection_say(gather, "<s>");
    }
}

/* The same tags, shut in the order that closes them properly. */
static void schultz_selection_span_close(schultz_selection_gather *gather,
                                         const schultz_span *span)
{
    if (span == NULL) {
        return;
    }
    if (span->strikethrough) {
        schultz_selection_say(gather, "</s>");
    }
    if (span->underline) {
        schultz_selection_say(gather, "</u>");
    }
    if (span->italic) {
        schultz_selection_say(gather, "</em>");
    }
    if (span->bold) {
        schultz_selection_say(gather, "</strong>");
    }
    if (schultz_selection_span_styled(span)) {
        schultz_selection_say(gather, "</span>");
    }
    if (span->link != NULL) {
        schultz_selection_say(gather, "</a>");
    }
}

/* The span covering a byte, first match, as the label paints it. */
static const schultz_span *schultz_selection_span_at(
    const schultz_span *spans, uint32_t count, uint32_t at)
{
    uint32_t i;

    for (i = 0u; i < count; i++) {
        if (spans[i].start <= at && at < spans[i].end) {
            return &spans[i];
        }
    }
    return NULL;
}

/*
 * The selected words, with each span's markup around the part it covers.
 *
 * Walks the range one stretch at a time, where a stretch runs until the span
 * covering it ends or the next one begins. A node with no spans is one
 * stretch and comes out exactly as it did before.
 */
static void schultz_selection_say_words(schultz_selection_gather *gather,
                                        const char *text, uint32_t low,
                                        uint32_t high,
                                        const schultz_span *spans,
                                        uint32_t count)
{
    uint32_t at = low;

    if (spans == NULL || count == 0u) {
        schultz_selection_escape(gather, text + low, (size_t)(high - low));
        return;
    }
    while (at < high) {
        const schultz_span *span = schultz_selection_span_at(spans, count, at);
        uint32_t end = high;
        uint32_t i;

        if (span != NULL) {
            if (span->end < end) {
                end = span->end;
            }
        } else {
            /* Plain text, as far as the next span that starts. */
            for (i = 0u; i < count; i++) {
                if (spans[i].start > at && spans[i].start < end) {
                    end = spans[i].start;
                }
            }
        }
        schultz_selection_span_open(gather, span);
        schultz_selection_escape(gather, text + at, (size_t)(end - at));
        schultz_selection_span_close(gather, span);
        at = end;
    }
}

static void schultz_selection_html_visit(schultz_tree *tree,
                                         schultz_handle node, void *context)
{
    schultz_selection_gather *gather = (schultz_selection_gather *)context;
    const schultz_selectable_vtable *part = schultz_selection_part(tree, node);
    uint32_t low = 0u;
    uint32_t high = 0u;

    if (part == NULL || part->range == NULL) {
        return;
    }
    part->range(tree, node, &low, &high);
    if (high <= low) {
        return;
    }

    if (part->is_picture != NULL && part->is_picture(tree, node)) {
        const void *bytes = NULL;
        uint64_t length = 0u;

        /*
         * The picture goes inside the document rather than beside it, so one
         * format carries the whole selection. That is also what lets a
         * selection holding two pictures arrive as two pictures.
         */
        if (schultz_selection_picture_of(tree, node, SCHULTZ_IMAGE_PNG,
                                         &bytes, &length) != SCHULTZ_OK) {
            return;
        }
        schultz_selection_say(gather, "<img src=\"data:image/png;base64,");
        schultz_selection_base64(gather, (const unsigned char *)bytes,
                                 (size_t)length);
        schultz_selection_say(gather, "\">");
        gather->any = 1;
        return;
    }
    if (part->text != NULL) {
        const char *text = part->text(tree, node);

        if (text == NULL) {
            return;
        }
        const schultz_span *spans = NULL;
        uint32_t span_count = 0u;

        if (part->spans != NULL) {
            spans = part->spans(tree, node, &span_count);
        }
        schultz_selection_say(gather, "<p");
        schultz_selection_style(tree, node, gather);
        schultz_selection_say(gather, ">");
        schultz_selection_say_words(gather, text, low, high, spans,
                                    span_count);
        schultz_selection_say(gather, "</p>");
        gather->any = 1;
    }
}

const char *schultz_selection_area_html(schultz_tree *tree,
                                        schultz_handle area)
{
    schultz_selection_data *data = schultz_selection_of(tree, area);
    schultz_selection_gather gather;

    if (data == NULL || !data->live) {
        return NULL;
    }
    memset(&gather, 0, sizeof(gather));
    gather.budget = data->budget;
    schultz_selection_say(&gather,
        /*
         * The two comments round the body are not decoration. Windows carries
         * markup in a format that reports where the copied part starts and
         * ends twice over: once as a byte offset in a header, and once as
         * exactly these comments. Writing them here means the part that
         * builds that header only has to find them rather than work out the
         * shape of the document. Everywhere else they are ordinary comments
         * and are ignored.
         */
        "<html><head><meta charset=\"utf-8\"></head><body>"
        "<!--StartFragment-->");
    schultz_selection_walk(tree, area, area, schultz_selection_html_visit,
                           &gather);
    schultz_selection_say(&gather, "<!--EndFragment--></body></html>");
    data->cut = gather.cut;
    if (gather.failed || !gather.any) {
        free(gather.text);
        return NULL;
    }
    free(data->marked);
    data->marked = gather.text;
    return data->marked;
}

const char *schultz_selection_area_text(schultz_tree *tree,
                                        schultz_handle area)
{
    schultz_selection_data *data = schultz_selection_of(tree, area);
    schultz_selection_gather gather;

    if (data == NULL || !data->live) {
        return NULL;
    }
    memset(&gather, 0, sizeof(gather));
    gather.budget = data->budget;
    schultz_selection_walk(tree, area, area, schultz_selection_gather_visit,
                           &gather);
    data->cut = gather.cut;
    if (gather.failed || !gather.any) {
        free(gather.text);
        return NULL;
    }
    free(data->taken);
    data->taken = gather.text;
    return data->taken;
}

/*
 * What the clipboard asks for, when it asks.
 *
 * The formats are offered together and produced one at a time, so a paste
 * into a text box costs the text and nothing else, and a picture is only ever
 * built if something that can hold one asks for it.
 */
typedef struct {
    schultz_tree  *tree;
    schultz_handle area;
    const schultz_render_options *options; /**< NULL when text only. */
} schultz_selection_copy;

static const void *schultz_selection_make(void *context, const char *format,
                                          uint64_t *out_length)
{
    schultz_selection_copy *copy = (schultz_selection_copy *)context;
    const void *bytes = NULL;

    *out_length = 0u;
    if (strcmp(format, SCHULTZ_CLIPBOARD_TEXT) == 0) {
        const char *text = schultz_selection_area_text(copy->tree,
                                                       copy->area);

        if (text == NULL) {
            return NULL;
        }
        *out_length = (uint64_t)strlen(text);
        return text;
    }
    if (strcmp(format, SCHULTZ_CLIPBOARD_HTML) == 0) {
        const char *markup = schultz_selection_area_html(copy->tree,
                                                         copy->area);

        if (markup == NULL) {
            return NULL;
        }
        *out_length = (uint64_t)strlen(markup);
        return markup;
    }
    if (strcmp(format, "image/png") == 0 &&
        schultz_selection_area_picture(copy->tree, copy->area,
                                       SCHULTZ_IMAGE_PNG, copy->options,
                                       &bytes, out_length) == SCHULTZ_OK) {
        return bytes;
    }
    return NULL;
}

/* Kept because the clipboard produces long after the copy returns. */
static schultz_selection_copy schultz_selection_last;

int32_t schultz_selection_area_copy(schultz_tree *tree, schultz_handle area,
                                    const schultz_render_options *options)
{
    /*
     * The markup first, then plain text. Most descriptive first is what every
     * clipboard asks for and what a program pasting relies on: it takes the
     * first format it recognises, so the one that carries everything has to
     * come before the one that carries the words alone.
     *
     * Plain text is the fallback, for a text box that can hold nothing else
     * and for a person who asked for it by name.
     *
     * A picture on its own is offered only when the selection is a picture
     * and nothing else. Handing one out of a selection that also held words
     * offers a program a choice it cannot make sensibly: a document with
     * three pictures in it is not any one of them, and a paste that silently
     * became the first one is worse than no offer at all.
     */
    static const char *const with_words[] = {
        SCHULTZ_CLIPBOARD_HTML, SCHULTZ_CLIPBOARD_TEXT
    };
    static const char *const picture_alone[] = {
        SCHULTZ_CLIPBOARD_HTML, SCHULTZ_CLIPBOARD_TEXT, "image/png"
    };
    const char *text;
    const void *bytes = NULL;
    uint64_t length = 0u;
    int32_t has_picture;

    if (schultz_selection_of(tree, area) == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    text = schultz_selection_area_text(tree, area);
    has_picture = (schultz_selection_area_picture(tree, area,
                                                  SCHULTZ_IMAGE_PNG, options,
                                                  &bytes, &length)
                       == SCHULTZ_OK);
    if (text == NULL && !has_picture) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    schultz_selection_last.tree    = tree;
    schultz_selection_last.area    = area;
    schultz_selection_last.options = options;
    /*
     * A picture makes this an offer of three formats rather than one. The
     * paste target picks: a text box takes the text, a document editor takes
     * the picture, and neither had to be asked which it wanted.
     */
    /* A picture and no words at all: the selection is that picture. */
    if (has_picture && text == NULL) {
        return schultz_tree_clipboard_offer(tree, picture_alone, 3u,
                                            schultz_selection_make,
                                            &schultz_selection_last);
    }
    return schultz_tree_clipboard_offer(tree, with_words, 2u,
                                        schultz_selection_make,
                                        &schultz_selection_last);
}

/* ------------------------------------------------------------ the picture */

/** @brief Finds the first selected picture in reading order. */
typedef struct {
    schultz_handle found;
} schultz_selection_pick;

static void schultz_selection_pick_visit(schultz_tree *tree,
                                         schultz_handle node, void *context)
{
    schultz_selection_pick *pick = (schultz_selection_pick *)context;
    const schultz_selectable_vtable *part = schultz_selection_part(tree, node);
    uint32_t low = 0u;
    uint32_t high = 0u;

    if (pick->found != SCHULTZ_HANDLE_NONE || part == NULL ||
        part->is_picture == NULL || part->range == NULL) {
        return;
    }
    if (!part->is_picture(tree, node)) {
        return;
    }
    part->range(tree, node, &low, &high);
    if (high > low) {
        pick->found = node;
    }
}

/*
 * One node, drawn and written out as a file.
 *
 * Rendered rather than fetched, which is what lets a picture that was loaded
 * and a canvas that is drawn on the spot both answer the same way.
 */
static int32_t schultz_selection_picture_of(schultz_tree *tree,
                                            schultz_handle node,
                                            uint32_t format,
                                            const void **out_bytes,
                                            uint64_t *out_length)
{
    schultz_render_options options;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t *pixels;
    int32_t result;

    if (schultz_render_size(tree, node, 1.0f, &width, &height) != SCHULTZ_OK ||
        width == 0u || height == 0u) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    memset(&options, 0, sizeof(options));
    options.fonts     = (schultz_font_system *)schultz_tree_font_system(tree);
    options.glyphs    = (schultz_glyph_cache *)schultz_tree_glyph_cache(tree);
    options.images    = (schultz_image_table *)schultz_tree_image_table(tree);
    options.resources = (schultz_resource_table *)schultz_tree_resources(tree);
    options.background = schultz_color_rgba(0u, 0u, 0u, 0u);

    pixels = (uint32_t *)malloc((size_t)width * height * sizeof(*pixels));
    if (pixels == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_render_to_buffer(tree, node, 1.0f, &options, pixels,
                                      width, height, 0u);
    if (result == SCHULTZ_OK) {
        result = schultz_image_encode(pixels, width, height, 0u, format,
                                      out_bytes, out_length);
    }
    free(pixels);
    return result;
}

int32_t schultz_selection_area_picture(schultz_tree *tree,
                                       schultz_handle area, uint32_t format,
                                       const schultz_render_options *options,
                                       const void **out_bytes,
                                       uint64_t *out_length)
{
    schultz_selection_data *data = schultz_selection_of(tree, area);
    schultz_selection_pick pick;
    uint32_t width = 0u;
    uint32_t height = 0u;
    schultz_render_options mine;
    int32_t result;

    if (out_bytes == NULL || out_length == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_bytes  = NULL;
    *out_length = 0u;
    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (!data->live) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    pick.found = SCHULTZ_HANDLE_NONE;
    schultz_selection_walk(tree, area, area, schultz_selection_pick_visit,
                           &pick);
    if (pick.found == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    if (schultz_render_size(tree, pick.found, 1.0f, &width, &height)
            != SCHULTZ_OK || width == 0u || height == 0u) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    /*
     * The raw pixels are the biggest this will get, so the limit is checked
     * against them before any of them are asked for. Encoding shrinks it;
     * refusing after rendering twenty megabytes would have spent exactly the
     * cost the limit exists to avoid.
     */
    if (data->budget != 0u &&
        (uint64_t)width * (uint64_t)height * 4u > data->budget) {
        data->cut = 1;
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    /*
     * What a render needs, assembled from the tree when the caller did not
     * say. The tree holds all four, so a copy that comes from a keystroke
     * works without a host passing anything in; a caller that wants
     * different options still passes its own.
     */
    if (options == NULL) {
        memset(&mine, 0, sizeof(mine));
        mine.fonts     = (schultz_font_system *)schultz_tree_font_system(tree);
        mine.glyphs    = (schultz_glyph_cache *)schultz_tree_glyph_cache(tree);
        mine.images    = (schultz_image_table *)schultz_tree_image_table(tree);
        mine.resources =
            (schultz_resource_table *)schultz_tree_resources(tree);
        mine.background = schultz_color_rgba(0u, 0u, 0u, 0u);
        options = &mine;
    }
    /*
     * Rendered rather than fetched. A picture that was loaded and a canvas
     * that is drawn on the spot both answer this the same way, which is the
     * whole reason it is done from the node.
     *
     * The buffer, the render and the encode are one call because they are
     * the same three steps any caller wanting a picture of a node takes.
     * What is left here is the part that is about a selection: finding which
     * node the picture is, and refusing one larger than the budget.
     */
    result = schultz_render_encode(tree, pick.found, 1.0f, options, format,
                                   out_bytes, out_length);
    return result;
}

int32_t schultz_selection_area_set_limit(schultz_tree *tree,
                                         schultz_handle area, uint64_t bytes)
{
    schultz_selection_data *data = schultz_selection_of(tree, area);

    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data->budget = bytes;
    return SCHULTZ_OK;
}

uint64_t schultz_selection_area_limit(const schultz_tree *tree,
                                      schultz_handle area)
{
    const schultz_selection_data *data = schultz_selection_of(tree, area);

    return (data == NULL) ? 0u : data->budget;
}

int32_t schultz_selection_area_was_cut(const schultz_tree *tree,
                                       schultz_handle area)
{
    const schultz_selection_data *data = schultz_selection_of(tree, area);

    return (data == NULL) ? 0 : data->cut;
}

schultz_handle schultz_selection_area_of(const schultz_tree *tree,
                                         schultz_handle node)
{
    schultz_handle walk = node;

    if (tree == NULL) {
        return SCHULTZ_HANDLE_NONE;
    }
    for (;;) {
        schultz_handle parent = SCHULTZ_HANDLE_NONE;

        if (schultz_node_parent(tree, walk, &parent) != SCHULTZ_OK ||
            parent == SCHULTZ_HANDLE_NONE) {
            return SCHULTZ_HANDLE_NONE;
        }
        if (schultz_node_widget(tree, parent) ==
                &schultz_selection_area_widget) {
            return parent;
        }
        walk = parent;
    }
}

int32_t schultz_selection_area_clear(schultz_tree *tree, schultz_handle area)
{
    schultz_selection_data *data = schultz_selection_of(tree, area);

    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data->live    = 0u;
    data->from    = SCHULTZ_HANDLE_NONE;
    data->to      = SCHULTZ_HANDLE_NONE;
    data->from_at = 0u;
    data->to_at   = 0u;
    schultz_selection_apply(tree, area, data);
    return SCHULTZ_OK;
}
