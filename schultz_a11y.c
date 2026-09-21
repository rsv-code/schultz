/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * schultz_a11y.c - the bridge from the widget tree to a screen reader.
 *
 * Everything AccessTunnel needs stays in this file. Above it the tree carries
 * Schultz's own schema, as role, name, value, state and actions; below it is
 * whichever platform accessibility API is running. Nothing else in the
 * toolkit includes an AccessTunnel header.
 */

#include "schultz_a11y.h"
#include "schultz_selection.h"

#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "access_tunnel.h"
#include "schultz_a11y_backend.h"
#include "access_tunnel_node.h"
#include "access_tunnel_node_props.h"
#include "access_tunnel_tree_update.h"
#include "schultz_widget.h"
#include "schultz_widgets.h"

#include <wordbreak.h>

/** How many requests may be waiting to be carried out. */
#define SCHULTZ_A11Y_QUEUE 32u

/** How many lines of one text field are published as runs. */
#define SCHULTZ_A11Y_RUNS 256u

/*
 * How many nodes' run identifiers are remembered for a selection area to name.
 *
 * One entry per label that published runs. Past this a label's runs are still
 * published and still readable; an area's selection simply cannot name them,
 * so the selection falls back to being absent rather than wrong.
 */
#define SCHULTZ_A11Y_RUN_OWNERS 128u

/*
 * The toolkit's version, as a screen reader would report it. Built from the
 * constants in schultz.h rather than typed again, so the two cannot drift.
 */
#define SCHULTZ_A11Y_STR2(x) #x
#define SCHULTZ_A11Y_STR(x)  SCHULTZ_A11Y_STR2(x)
#define SCHULTZ_A11Y_VERSION \
    SCHULTZ_A11Y_STR(SCHULTZ_VERSION_MAJOR) "." \
    SCHULTZ_A11Y_STR(SCHULTZ_VERSION_MINOR) "." \
    SCHULTZ_A11Y_STR(SCHULTZ_VERSION_PATCH)

/** One thing an assistive technology asked for, waiting for the frame. */
typedef struct {
    schultz_handle node;   /**< What to act on. */
    uint32_t       action; /**< A SCHULTZ_ACTION_* value, or 0 for reveal. */
    uint32_t       reveal; /**< Nonzero for scroll into view. */
    /*
     * A value to set, when the action is one that carries one. The request
     * this came from is borrowed and gone by the time the frame runs, so the
     * text is copied rather than pointed at.
     */
    uint32_t       has_number; /**< Nonzero when number is meaningful. */
    double         number;     /**< For a slider or a scroll bar. */
    char          *text;       /**< Owned, or NULL. For a text field. */
} schultz_a11y_request;

/** @brief Everything the bridge holds between frames. */
struct schultz_a11y {
    schultz_tree               *tree;    /**< The tree being described. */
    schultz_events             *events;  /**< Where requests are delivered. */
    schultz_a11y_backend *backend;      /**< The platform side. */

    SDL_Mutex           *lock;                       /**< Guards the queue. */
    schultz_a11y_request queue[SCHULTZ_A11Y_QUEUE];  /**< Waiting requests. */
    uint32_t             queued;  /**< How many are waiting. */
    uint32_t             dropped; /**< Requests lost to a full queue. */

    access_tunnel_node_id next_run; /**< Next id to hand a text run. */
    /*
     * Which runs each node published, so that a selection area can name a run
     * inside one of its labels.
     *
     * Only the first identifier and the count are kept, because a node's runs
     * are handed out together and therefore run consecutively. The byte each
     * run starts at is worked out again from the text, which is cheaper than
     * remembering it and cannot drift from what was published.
     *
     * Children are pushed before their parent, so by the time an area is
     * built every label beneath it is already in here.
     */
    struct {
        schultz_handle        node;  /**< Whose runs these are. */
        access_tunnel_node_id first; /**< The first run's identifier. */
        uint32_t              count; /**< How many it published. */
    } runs_of[SCHULTZ_A11Y_RUN_OWNERS];
    uint32_t runs_known; /**< Entries used in runs_of. */
    schultz_handle focus;   /**< What was published as focused. */
    uint32_t       serial;  /**< Bumped when the tree is republished. */
    int32_t        started; /**< Nonzero once the first tree went out. */
};

/*
 * The root of the published tree. Schultz's own root is a node like any
 * other, but a platform wants a window at the top, so the root is published
 * as one and every other node hangs off it unchanged.
 */
#define SCHULTZ_A11Y_ROOT ((access_tunnel_node_id)1)

/*
 * A node's identifier, which must never be reused: an assistive technology
 * caches these hard, and a recycled one produces bugs that look like the
 * reader inventing information.
 *
 * A Schultz handle is exactly this already. It carries a slot index in its
 * low half and a generation in its high half, and the generation is bumped
 * when the slot is released, so a rebuilt node in the same slot is a
 * different handle. Nothing has to be invented here.
 *
 * One is reserved for the window, and the root maps onto it.
 */
static access_tunnel_node_id schultz_a11y_id(const schultz_a11y *a11y,
                                             schultz_handle node)
{
    if (node == schultz_tree_root(a11y->tree)) {
        return SCHULTZ_A11Y_ROOT;
    }
    return (access_tunnel_node_id)node;
}

/* Schultz's roles, in AccessTunnel's vocabulary. */
static access_tunnel_role schultz_a11y_role(uint32_t role)
{
    switch (role) {
    case SCHULTZ_ROLE_WINDOW:      return ACCESS_TUNNEL_ROLE_WINDOW;
    case SCHULTZ_ROLE_GROUP:       return ACCESS_TUNNEL_ROLE_GROUP;
    case SCHULTZ_ROLE_LABEL:       return ACCESS_TUNNEL_ROLE_LABEL;
    case SCHULTZ_ROLE_BUTTON:      return ACCESS_TUNNEL_ROLE_BUTTON;
    case SCHULTZ_ROLE_CHECKBOX:    return ACCESS_TUNNEL_ROLE_CHECK_BOX;
    case SCHULTZ_ROLE_RADIO:       return ACCESS_TUNNEL_ROLE_RADIO_BUTTON;
    case SCHULTZ_ROLE_TEXT_INPUT:  return ACCESS_TUNNEL_ROLE_TEXT_INPUT;
    case SCHULTZ_ROLE_LIST:        return ACCESS_TUNNEL_ROLE_LIST;
    case SCHULTZ_ROLE_LIST_ITEM:   return ACCESS_TUNNEL_ROLE_LIST_ITEM;
    case SCHULTZ_ROLE_IMAGE:       return ACCESS_TUNNEL_ROLE_IMAGE;
    case SCHULTZ_ROLE_SCROLL_VIEW: return ACCESS_TUNNEL_ROLE_SCROLL_VIEW;
    case SCHULTZ_ROLE_SLIDER:      return ACCESS_TUNNEL_ROLE_SLIDER;
    case SCHULTZ_ROLE_MENU:        return ACCESS_TUNNEL_ROLE_MENU;
    case SCHULTZ_ROLE_MENU_ITEM:   return ACCESS_TUNNEL_ROLE_MENU_ITEM;
    case SCHULTZ_ROLE_PROGRESS:    return ACCESS_TUNNEL_ROLE_PROGRESS_INDICATOR;
    default:
        /*
         * A node whose role was never set is almost always a container put
         * there for layout. Unknown is a real role that a reader will
         * announce; a generic container is one it walks through silently,
         * which is what a layout box deserves.
         */
        return ACCESS_TUNNEL_ROLE_GENERIC_CONTAINER;
    }
}

/* Schultz's actions, in AccessTunnel's vocabulary. */
static void schultz_a11y_actions(access_tunnel_node *out, uint32_t actions)
{
    if (actions & SCHULTZ_ACTION_CLICK) {
        access_tunnel_node_add_action(out, ACCESS_TUNNEL_ACTION_CLICK);
    }
    if (actions & SCHULTZ_ACTION_FOCUS) {
        access_tunnel_node_add_action(out, ACCESS_TUNNEL_ACTION_FOCUS);
    }
    if (actions & SCHULTZ_ACTION_INCREMENT) {
        access_tunnel_node_add_action(out, ACCESS_TUNNEL_ACTION_INCREMENT);
    }
    if (actions & SCHULTZ_ACTION_DECREMENT) {
        access_tunnel_node_add_action(out, ACCESS_TUNNEL_ACTION_DECREMENT);
    }
    /*
     * Without this a reader never offers to set the value, because it goes by
     * the actions a node advertises. The bridge acts on the request when one
     * arrives, so the way in worked and nothing ever came in.
     */
    if (actions & SCHULTZ_ACTION_SET_VALUE) {
        access_tunnel_node_add_action(out, ACCESS_TUNNEL_ACTION_SET_VALUE);
    }
    if (actions & SCHULTZ_ACTION_SCROLL) {
        access_tunnel_node_add_action(out,
                                      ACCESS_TUNNEL_ACTION_SCROLL_INTO_VIEW);
    }
}

/*
 * A rectangle for the platform.
 *
 * These are in the toolkit's units, and what the platform wants depends on
 * which one it is. UIKit and AppKit place an element in the view's own
 * coordinates, which are the same units the toolkit works in when it is
 * scaling to the screen, so those need nothing. AT-SPI and UI Automation want
 * screen pixels, which differ by the scale on a screen that has one.
 *
 * Nothing is applied here, so a Linux or Windows desktop that scales would
 * put a screen reader's highlight in the wrong place. That is a real gap, and
 * it is left alone rather than guessed at because the conversion belongs in
 * each backend and only two of the five want it. Apple's platforms, which is
 * where this is being used, are correct as they stand, and so is any screen
 * with nothing to scale.
 */
static access_tunnel_rect schultz_a11y_rect(schultz_rect r)
{
    access_tunnel_rect out;

    out.x0 = (double)r.x;
    out.y0 = (double)r.y;
    out.x1 = (double)(r.x + r.width);
    out.y1 = (double)(r.y + r.height);
    return out;
}

/*
 * True when a child is its parent's own caption rather than content.
 *
 * A button is a node with a label inside it, and both carry the same words.
 * Publishing both makes a screen reader say "Theme, Theme": once for the
 * control and once for the text inside it. The caption is not separate
 * content, so it is left out and the control keeps the name.
 *
 * Matching on the text rather than on a flag is deliberate: a label that
 * happens to repeat its parent's name is not worth announcing twice either.
 */
static int32_t schultz_a11y_is_caption(const schultz_a11y *a11y,
                                       schultz_handle parent,
                                       schultz_handle child)
{
    const char *outer;
    const char *inner;

    if (schultz_node_get_role(a11y->tree, child) != SCHULTZ_ROLE_LABEL) {
        return 0;
    }
    outer = schultz_node_get_name(a11y->tree, parent);
    inner = schultz_node_get_name(a11y->tree, child);
    if (outer == NULL || inner == NULL || outer[0] == '\0') {
        return 0;
    }
    return (strcmp(outer, inner) == 0) ? 1 : 0;
}

/*
 * A text field's contents, as an assistive technology needs them.
 *
 * A field is not one string to a screen reader. It is a sequence of text
 * runs, and each run carries the byte length of every character in it, so
 * that a reader can move a caret by character and by word and know where it
 * ended up. Publishing only the whole string, as this used to, lets a reader
 * read a field but not navigate inside it.
 *
 * The lengths cannot be worked out from the text by the accessibility layer:
 * a character is the smallest unit the editor lets you select, and only the
 * editor knows what that is. Schultz steps its caret over whole UTF-8
 * characters, so that is what is published, and the two agree by
 * construction.
 *
 * One run per hard line. A soft wrapped line is a property of the layout
 * rather than of the text, and a reader asking for "the next line" means the
 * next real one.
 *
 * Returns how many runs were published, and fills out_ids with their
 * identifiers.
 */
/* The span covering a byte, first match, as the label paints it. */
static const schultz_span *schultz_a11y_span_at(const schultz_span *spans,
                                                uint32_t count, uint32_t at)
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
 * How far a run starting at a byte may reach before the look changes: the end
 * of the span covering it, or the start of the next one, whichever comes
 * first. With no spans the answer is the end of the text, which is what this
 * did before there were any.
 */
static size_t schultz_a11y_run_limit(const schultz_span *spans,
                                     uint32_t count, uint32_t at,
                                     size_t length)
{
    const schultz_span *here = schultz_a11y_span_at(spans, count, at);
    size_t limit = length;
    uint32_t i;

    if (here != NULL) {
        return (here->end < limit) ? (size_t)here->end : limit;
    }
    for (i = 0u; i < count; i++) {
        if (spans[i].start > at && (size_t)spans[i].start < limit) {
            limit = spans[i].start;
        }
    }
    return limit;
}

/* Tells a run how its words look, so a reader can say so. */
static void schultz_a11y_say_look(access_tunnel_node *run,
                                  const schultz_span *span)
{
    access_tunnel_text_decoration line;

    if (span == NULL) {
        return;
    }
    if (span->bold) {
        /* Increments of a hundred, where four hundred is normal and seven
         * hundred is bold. That is the scale the property is defined on. */
        access_tunnel_node_set_font_weight(run, 700.0f);
    }
    if (span->italic) {
        access_tunnel_node_set_italic(run);
    }
    if (span->size > 0.0f) {
        access_tunnel_node_set_font_size(run, (double)span->size);
    }
    if (span->color.a != 0u) {
        access_tunnel_color ink;

        ink.red   = span->color.r;
        ink.green = span->color.g;
        ink.blue  = span->color.b;
        ink.alpha = span->color.a;
        access_tunnel_node_set_foreground_color(run, ink);
    }
    if (span->background.a != 0u) {
        access_tunnel_color wash;

        wash.red   = span->background.r;
        wash.green = span->background.g;
        wash.blue  = span->background.b;
        wash.alpha = span->background.a;
        access_tunnel_node_set_background_color(run, wash);
    }
    memset(&line, 0, sizeof(line));
    line.style = ACCESS_TUNNEL_TEXT_DECORATION_STYLE_SOLID;
    if (span->underline) {
        access_tunnel_node_set_underline(run, line);
    }
    if (span->strikethrough) {
        access_tunnel_node_set_strikethrough(run, line);
    }
}

static uint32_t schultz_a11y_text_runs(schultz_a11y *a11y,
                                       access_tunnel_tree_update *update,
                                       schultz_handle node, const char *text,
                                       schultz_rect bounds,
                                       access_tunnel_node_id *ids,
                                       uint32_t max_runs,
                                       uint32_t *out_starts)
{
    const schultz_widget_vtable *widget = schultz_node_widget(a11y->tree,
                                                             node);
    const schultz_span *spans = NULL;
    uint32_t span_count = 0u;
    uint32_t runs = 0;
    size_t at = 0;
    size_t length = (text == NULL) ? 0u : strlen(text);

    /*
     * A run is a stretch that reads the same throughout, so it stops wherever
     * the look changes as well as at the end of a line. Without that a reader
     * has no way to say that a phrase is bold, because the property belongs
     * to the run and the run would cover the whole paragraph.
     */
    if (widget != NULL && widget->selectable != NULL &&
        widget->selectable->spans != NULL) {
        spans = widget->selectable->spans(a11y->tree, node, &span_count);
    }

    while (runs < max_runs) {
        const char *line = text + at;
        size_t end = at;
        size_t bytes;
        size_t i;
        size_t chars = 0;
        uint8_t *lengths;
        uint8_t *starts = NULL;
        char *breaks;
        uint32_t words = 0;
        access_tunnel_node run;

        const schultz_span *look = schultz_a11y_span_at(spans, span_count,
                                                        (uint32_t)at);
        size_t limit = schultz_a11y_run_limit(spans, span_count,
                                              (uint32_t)at, length);

        /* The line, including the break that ends it: a reader counts that
         * break as one character and puts the caret before it. */
        while (end < limit && text[end] != '\n') {
            end++;
        }
        if (end < limit && text[end] == '\n') {
            end++;
        }
        bytes = end - at;

        access_tunnel_node_init(&run, ACCESS_TUNNEL_ROLE_TEXT_RUN);
        schultz_a11y_say_look(&run, look);
        {
            char *value = (char *)malloc(bytes + 1u);

            if (value == NULL) {
                access_tunnel_node_free(&run);
                return runs;
            }
            memcpy(value, line, bytes);
            value[bytes] = '\0';
            access_tunnel_node_set_value(&run, value);
            free(value);
        }

        /* One entry per character: the bytes it takes up. A byte whose top
         * two bits are 10 continues the character before it. */
        lengths = (uint8_t *)calloc(bytes + 1u, 1u);
        breaks  = (char *)calloc(bytes + 1u, 1u);
        if (lengths == NULL || breaks == NULL) {
            free(lengths); free(breaks);
            access_tunnel_node_free(&run);
            return runs;
        }
        for (i = 0; i < bytes; i++) {
            if (((unsigned char)line[i] & 0xC0u) == 0x80u && chars > 0u) {
                lengths[chars - 1u] = (uint8_t)(lengths[chars - 1u] + 1u);
            } else {
                lengths[chars++] = 1u;
            }
        }
        access_tunnel_node_set_character_lengths(&run, lengths, chars);

        /*
         * Where words begin, as character indices. libunibreak knows the
         * Unicode rules, and it is already here for line breaking, so word
         * boundaries come from the same place rather than from a second
         * guess about what a word is.
         */
        if (bytes > 0u) {
            starts = (uint8_t *)calloc(chars + 1u, sizeof(*starts));
            if (starts != NULL) {
                size_t byte = 0;
                size_t index = 0;

                set_wordbreaks_utf8((const utf8_t *)line, bytes, "", breaks);
                starts[words++] = 0u; /* a line always starts a word */
                for (index = 0; index < chars; index++) {
                    size_t next = byte + lengths[index];

                    /*
                     * A word start is an index into character_lengths, and
                     * the schema holds it in a byte. Past 255 characters
                     * there is nowhere to put one, so word navigation stops
                     * there rather than wrapping to a wrong position.
                     */
                    if (index + 1u > 255u) {
                        break;
                    }
                    if (next < bytes && breaks[next - 1u] == WORDBREAK_BREAK) {
                        starts[words++] = (uint8_t)(index + 1u);
                    }
                    byte = next;
                }
                access_tunnel_node_set_word_starts(&run, starts, words);
            }
        }
        free(lengths);
        free(breaks);
        free(starts);

        /*
         * The run sits where the field does. Per character rectangles would
         * let a reader highlight one word on screen; that needs the shaped
         * glyph positions and is not published yet.
         */
        access_tunnel_node_set_bounds(&run, schultz_a11y_rect(bounds));

        if (runs == 0u && a11y->runs_known < SCHULTZ_A11Y_RUN_OWNERS) {
            a11y->runs_of[a11y->runs_known].node  = node;
            a11y->runs_of[a11y->runs_known].first = a11y->next_run;
            a11y->runs_of[a11y->runs_known].count = 0u;
            a11y->runs_known++;
        }
        ids[runs] = a11y->next_run++;
        out_starts[runs] = (uint32_t)at;
        if (access_tunnel_tree_update_push_node(update, ids[runs], &run)
                != ACCESS_TUNNEL_OK) {
            access_tunnel_node_free(&run);
            return runs;
        }
        runs++;
        if (a11y->runs_known > 0u &&
            a11y->runs_of[a11y->runs_known - 1u].node == node) {
            a11y->runs_of[a11y->runs_known - 1u].count = runs;
        }

        at = end;
        if (at >= length) {
            break;
        }
    }
    return runs;
}


/*
 * The character index a byte offset falls on, within one run.
 *
 * A selection is expressed in characters, not bytes, so a caret Schultz holds
 * as a byte offset has to be counted into the same units the run published.
 * Every byte that does not continue the character before it starts one.
 */
static size_t schultz_a11y_char_index(const char *line, size_t bytes,
                                      size_t offset)
{
    size_t i;
    size_t index = 0;

    if (offset > bytes) {
        offset = bytes;
    }
    for (i = 0; i < offset; i++) {
        if (((unsigned char)line[i] & 0xC0u) != 0x80u) {
            index++;
        }
    }
    return index;
}

/*
 * Turns a byte offset in the whole text into a position inside one run.
 *
 * A selection names a run and an index within it, so an offset that Schultz
 * keeps against the entire string has to be found in whichever line holds it.
 */
static access_tunnel_text_position schultz_a11y_position(
    const char *text, const access_tunnel_node_id *ids, const uint32_t *starts,
    uint32_t runs, uint32_t offset)
{
    access_tunnel_text_position position;
    uint32_t i = 0;
    uint32_t begin;
    uint32_t end;

    while ((i + 1u) < runs && offset >= starts[i + 1u]) {
        i++;
    }
    begin = starts[i];
    end   = ((i + 1u) < runs) ? starts[i + 1u] : (uint32_t)strlen(text);
    if (offset < begin) {
        offset = begin;
    }
    position.node = ids[i];
    position.character_index =
        schultz_a11y_char_index(text + begin, (size_t)(end - begin),
                                (size_t)(offset - begin));
    return position;
}

/*
 * Turns a byte offset inside a node into a position naming one of the runs
 * that node published.
 *
 * The identifiers were handed out together and so run consecutively, and
 * where each run starts is worked out again from the text the same way it was
 * split in the first place. That keeps this in step with what was published
 * without a second table to go stale.
 */
static int32_t schultz_a11y_position_in(const schultz_a11y *a11y,
                                        schultz_handle node, const char *text,
                                        uint32_t offset,
                                        access_tunnel_text_position *out)
{
    access_tunnel_node_id ids[SCHULTZ_A11Y_RUNS];
    uint32_t starts[SCHULTZ_A11Y_RUNS];
    uint32_t runs = 0u;
    size_t at = 0;
    size_t length;
    uint32_t i;

    if (text == NULL) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    for (i = 0; i < a11y->runs_known; i++) {
        if (a11y->runs_of[i].node == node) {
            break;
        }
    }
    if (i == a11y->runs_known || a11y->runs_of[i].count == 0u) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    length = strlen(text);
    while (runs < a11y->runs_of[i].count && runs < SCHULTZ_A11Y_RUNS) {
        size_t end = at;

        while (end < length && text[end] != '\n') {
            end++;
        }
        if (end < length) {
            end++;
        }
        ids[runs]    = a11y->runs_of[i].first + runs;
        starts[runs] = (uint32_t)at;
        runs++;
        at = end;
        if (at >= length) {
            break;
        }
    }
    if (runs == 0u) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    *out = schultz_a11y_position(text, ids, starts, runs, offset);
    return SCHULTZ_OK;
}

/*
 * Publishes one node and everything under it.
 *
 * A node that is not effectively visible is left out entirely, along with its
 * children: describing something the user cannot see is worse than saying
 * nothing, because a reader will offer to move to it.
 */
static int32_t schultz_a11y_push(schultz_a11y *a11y,
                                 access_tunnel_tree_update *update,
                                 schultz_handle node)
{
    access_tunnel_node out;
    access_tunnel_node_id *kids = NULL;
    uint32_t count = schultz_node_child_count(a11y->tree, node);
    uint32_t kept = 0;
    uint32_t state = schultz_node_get_state(a11y->tree, node);
    const char *name;
    const char *value;
    schultz_rect bounds;
    uint32_t i;
    int32_t result;

    if (!schultz_node_is_visible(a11y->tree, node)) {
        return SCHULTZ_OK;
    }

    /*
     * A text field's children are its own workings, and its published
     * children are the text runs instead. They are not walked at all: a node
     * that is pushed and then left with no parent orphans the update, and the
     * adapter answers a tree that has lost almost everything.
     */
    if (schultz_node_get_role(a11y->tree, node) == SCHULTZ_ROLE_TEXT_INPUT) {
        count = 0u;
    }

    if (count > 0u) {
        kids = (access_tunnel_node_id *)calloc(count, sizeof(*kids));
        if (kids == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
    }
    for (i = 0; i < count; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(a11y->tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_node_is_visible(a11y->tree, child) ||
            schultz_a11y_is_caption(a11y, node, child)) {
            continue;
        }
        kids[kept++] = schultz_a11y_id(a11y, child);
        result = schultz_a11y_push(a11y, update, child);
        if (result != SCHULTZ_OK) {
            free(kids);
            return result;
        }
    }

    access_tunnel_node_init(&out,
        (node == schultz_tree_root(a11y->tree))
            ? ACCESS_TUNNEL_ROLE_WINDOW
            : schultz_a11y_role(schultz_node_get_role(a11y->tree, node)));
    /*
     * A masked field is a password field, and saying so is what makes a
     * reader announce it as one and stop echoing what is typed into it.
     */
    if (schultz_node_get_role(a11y->tree, node) == SCHULTZ_ROLE_TEXT_INPUT &&
        schultz_text_field_mask(a11y->tree, node) != 0u) {
        access_tunnel_node_set_role(&out, ACCESS_TUNNEL_ROLE_PASSWORD_INPUT);
    }

    /*
     * A text field publishes its contents as text runs, so that a reader can
     * move inside it rather than only read it out. The runs become its
     * children, and its selection points into them.
     */
    if (schultz_node_get_role(a11y->tree, node) == SCHULTZ_ROLE_TEXT_INPUT) {
        uint32_t mask = schultz_text_field_mask(a11y->tree, node);
        char *hidden = NULL;
        const char *text;

        /*
         * A masked field must not put what it holds on the accessibility bus.
         * It is masked on screen precisely so that it cannot be read, and
         * anything listening can read this. What goes out is one bullet per
         * character, which keeps the length and every caret position honest
         * while the characters themselves never leave.
         */
        if (mask != 0u) {
            const char *real = schultz_text_get(a11y->tree, node);
            size_t chars = 0;
            size_t i;

            for (i = 0; real != NULL && real[i] != '\0'; i++) {
                if (((unsigned char)real[i] & 0xC0u) != 0x80u) {
                    chars++;
                }
            }
            hidden = (char *)malloc(chars * 3u + 1u);
            if (hidden != NULL) {
                for (i = 0; i < chars; i++) {
                    hidden[i * 3u]      = (char)0xE2; /* U+2022, a bullet */
                    hidden[i * 3u + 1u] = (char)0x80;
                    hidden[i * 3u + 2u] = (char)0xA2;
                }
                hidden[chars * 3u] = '\0';
            }
            text = hidden;
        } else {
            text = schultz_text_get(a11y->tree, node);
        }

        if (text != NULL) {
            access_tunnel_node_id run_ids[SCHULTZ_A11Y_RUNS];
            uint32_t run_at[SCHULTZ_A11Y_RUNS];
            /*
             * The runs are placed inside the field, so they need its box
             * before the node's own bounds are read further down. A field
             * whose bounds cannot be read has not been laid out, and an
             * empty rectangle is the honest answer for where its text is.
             */
            schultz_rect box;
            uint32_t runs;

            if (schultz_node_absolute_bounds(a11y->tree, node, &box)
                    != SCHULTZ_OK) {
                box = schultz_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
            }
            runs = schultz_a11y_text_runs(a11y, update, node, text, box,
                                          run_ids, SCHULTZ_A11Y_RUNS, run_at);
            if (runs > 0u) {
                uint32_t anchor = 0u;
                uint32_t caret = 0u;

                access_tunnel_node_set_children(&out, run_ids, (size_t)runs);
                if (schultz_text_selection(a11y->tree, node, &anchor, &caret)
                        == SCHULTZ_OK) {
                    access_tunnel_text_selection sel;

                    sel.anchor = schultz_a11y_position(text, run_ids, run_at,
                                                       runs, anchor);
                    sel.focus  = schultz_a11y_position(text, run_ids, run_at,
                                                       runs, caret);
                    access_tunnel_node_set_text_selection(&out, sel);
                }
                free(kids);
                kids = NULL;
                kept = 0u;
            }
        }
        free(hidden);
    }

    /*
     * A selectable label publishes its text the same way, and for the same
     * reason: a reader that can only be told the whole string can read it
     * out but cannot move through it or say what is selected.
     *
     * The selection goes on this node rather than on the runs, with its two
     * ends naming runs beneath it. That is the shape the schema is built for
     * and the shape every other toolkit on this model uses.
     *
     * A label that is not selectable publishes no runs. Its text still
     * reaches a reader as the node's value further down, which is all a
     * caption needs.
     */
    if (schultz_node_get_role(a11y->tree, node) == SCHULTZ_ROLE_LABEL &&
        schultz_label_selectable(a11y->tree, node)) {
        const char *text = schultz_node_get_name(a11y->tree, node);

        if (text != NULL && text[0] != '\0') {
            access_tunnel_node_id run_ids[SCHULTZ_A11Y_RUNS];
            uint32_t run_at[SCHULTZ_A11Y_RUNS];
            schultz_rect box;
            uint32_t runs;

            if (schultz_node_absolute_bounds(a11y->tree, node, &box)
                    != SCHULTZ_OK) {
                box = schultz_rect_make(0.0f, 0.0f, 0.0f, 0.0f);
            }
            runs = schultz_a11y_text_runs(a11y, update, node, text, box,
                                          run_ids, SCHULTZ_A11Y_RUNS, run_at);
            if (runs > 0u) {
                uint32_t start = 0u;
                uint32_t end = 0u;

                access_tunnel_node_set_children(&out, run_ids, (size_t)runs);
                if (schultz_label_selection(a11y->tree, node, &start, &end)
                        == SCHULTZ_OK) {
                    access_tunnel_text_selection sel;

                    sel.anchor = schultz_a11y_position(text, run_ids, run_at,
                                                       runs, start);
                    sel.focus  = schultz_a11y_position(text, run_ids, run_at,
                                                       runs, end);
                    access_tunnel_node_set_text_selection(&out, sel);
                }
                /*
                 * The runs are the label's children now, so whatever child
                 * list was gathered above is dropped: a label has no other
                 * children worth publishing, and keeping both would put the
                 * text in twice.
                 */
                free(kids);
                kids = NULL;
                kept = 0u;
            }
        }
    }

    /*
     * A selection area carries a selection whose two ends may be in two
     * different labels beneath it.
     *
     * This is the shape the schema is built for: a position names a run and
     * an index, the runs sit under the labels, and the labels sit under this
     * node, so a reader resolving either end against this container finds it
     * by walking up. Nothing had to be invented; what was needed was knowing
     * which identifiers the runs beneath were given, and children are
     * published before their parent, so by now they have them.
     */
    {
        schultz_handle from = SCHULTZ_HANDLE_NONE;
        schultz_handle to = SCHULTZ_HANDLE_NONE;
        uint32_t from_at = 0u;
        uint32_t to_at = 0u;

        if (schultz_selection_area_ends(a11y->tree, node, &from, &from_at,
                                        &to, &to_at) == SCHULTZ_OK) {
            access_tunnel_text_selection sel;

            if (schultz_a11y_position_in(a11y, from,
                    schultz_node_get_name(a11y->tree, from), from_at,
                    &sel.anchor) == SCHULTZ_OK &&
                schultz_a11y_position_in(a11y, to,
                    schultz_node_get_name(a11y->tree, to), to_at,
                    &sel.focus) == SCHULTZ_OK) {
                access_tunnel_node_set_text_selection(&out, sel);
            }
        }
    }

    /*
     * Where the words go depends on what the node is.
     *
     * For a control, the name is its label: the thing a reader says to
     * identify it. For a piece of static text, the words *are* the content,
     * and AccessKit's schema is explicit that a Label role carries them in
     * value rather than in label. Putting them in label instead publishes a
     * node with nothing to read, and every caption in the window goes silent
     * without anything failing.
     */
    name  = schultz_node_get_name(a11y->tree, node);
    value = schultz_node_get_value(a11y->tree, node);
    if (schultz_node_get_role(a11y->tree, node) == SCHULTZ_ROLE_LABEL) {
        if (name != NULL && name[0] != '\0') {
            access_tunnel_node_set_value(&out, name);
        }
    } else {
        if (name != NULL && name[0] != '\0') {
            access_tunnel_node_set_label(&out, name);
        }
        if (value != NULL && value[0] != '\0') {
            access_tunnel_node_set_value(&out, value);
        }
    }
    if (schultz_node_absolute_bounds(a11y->tree, node, &bounds) == SCHULTZ_OK) {
        access_tunnel_node_set_bounds(&out, schultz_a11y_rect(bounds));
    }
    /*
     * A toggle's state is a property, not part of its name. Saying "Play
     * sounds, checked" is the reader's job and its user's preference; baking
     * it into the label takes that choice away and reads twice.
     */
    if (schultz_node_get_actions(a11y->tree, node) & SCHULTZ_ACTION_CLICK) {
        uint32_t role = schultz_node_get_role(a11y->tree, node);

        if (role == SCHULTZ_ROLE_CHECKBOX || role == SCHULTZ_ROLE_RADIO) {
            access_tunnel_node_set_toggled(&out,
                (state & SCHULTZ_STATE_CHECKED) ? ACCESS_TUNNEL_TOGGLED_TRUE
                                                : ACCESS_TUNNEL_TOGGLED_FALSE);
        }
    }
    if (!(state & SCHULTZ_STATE_ENABLED)) {
        access_tunnel_node_set_disabled(&out);
    }
    schultz_a11y_actions(&out, schultz_node_get_actions(a11y->tree, node));

    if (kept > 0u) {
        access_tunnel_node_set_children(&out, kids, (size_t)kept);
    }
    free(kids);

    result = access_tunnel_tree_update_push_node(update,
                                                 schultz_a11y_id(a11y, node),
                                                 &out);
    if (result != ACCESS_TUNNEL_OK) {
        access_tunnel_node_free(&out);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    return SCHULTZ_OK;
}

/* Builds a complete update: every visible node, plus who has focus. */
static int32_t schultz_a11y_build(schultz_a11y *a11y,
                                  access_tunnel_tree_update *update)
{
    access_tunnel_tree_info info;
    schultz_handle focus = schultz_events_focus(a11y->events);
    int32_t result;

    /*
     * Something always has focus as far as a platform is concerned. With
     * nothing focused in the tree, the window itself is the honest answer.
     */
    /*
     * Run identifiers are handed out from zero each time the tree is
     * published. A Schultz handle always carries a non zero generation in its
     * high half, so a small number can never collide with one, and handing
     * them out in walk order makes a run keep its identifier for as long as
     * the tree keeps its shape.
     */
    a11y->next_run   = 2u;
    a11y->runs_known = 0u;

    access_tunnel_tree_update_init(update,
        (focus == SCHULTZ_HANDLE_NONE) ? SCHULTZ_A11Y_ROOT
                                       : schultz_a11y_id(a11y, focus));
    result = schultz_a11y_push(a11y, update, schultz_tree_root(a11y->tree));
    if (result != SCHULTZ_OK) {
        access_tunnel_tree_update_free(update);
        return result;
    }

    access_tunnel_tree_info_init(&info, SCHULTZ_A11Y_ROOT);
    access_tunnel_tree_info_set_toolkit_name(&info, "Schultz");
    access_tunnel_tree_info_set_toolkit_version(&info, SCHULTZ_A11Y_VERSION);
    access_tunnel_tree_update_set_tree(update, &info);
    access_tunnel_tree_info_free(&info);

    a11y->focus = focus;
    return SCHULTZ_OK;
}

/*
 * What an assistive technology asked for, called from inside the pump and
 * possibly on a thread this toolkit did not create. Nothing is done here but
 * writing it down; schultz_a11y_drain carries it out on the frame's thread.
 */
static void schultz_a11y_on_action(const access_tunnel_action_request *request,
                                   void *userdata)
{
    schultz_a11y *a11y = (schultz_a11y *)userdata;
    schultz_a11y_request queued;

    memset(&queued, 0, sizeof(queued));
    queued.node = (request->target_node == SCHULTZ_A11Y_ROOT)
                      ? schultz_tree_root(a11y->tree)
                      : (schultz_handle)request->target_node;

    switch (request->action) {
    case ACCESS_TUNNEL_ACTION_CLICK:
        queued.action = SCHULTZ_ACTION_CLICK; break;
    case ACCESS_TUNNEL_ACTION_FOCUS:
        queued.action = SCHULTZ_ACTION_FOCUS; break;
    case ACCESS_TUNNEL_ACTION_INCREMENT:
        queued.action = SCHULTZ_ACTION_INCREMENT; break;
    case ACCESS_TUNNEL_ACTION_DECREMENT:
        queued.action = SCHULTZ_ACTION_DECREMENT; break;
    case ACCESS_TUNNEL_ACTION_SCROLL_INTO_VIEW:
        queued.reveal = 1u; break;
    case ACCESS_TUNNEL_ACTION_SET_VALUE:
        /*
         * A value arrives as either a number or a string, tagged. Which one
         * says what kind of control the request was aimed at, so it is kept
         * and the widget is chosen from it at the other end.
         */
        queued.action = SCHULTZ_ACTION_SET_VALUE;
        if (!request->has_data) {
            return;
        }
        if (request->data.kind == ACCESS_TUNNEL_ACTION_DATA_NUMERIC_VALUE) {
            queued.has_number = 1u;
            queued.number     = request->data.as.numeric_value;
        } else if (request->data.kind == ACCESS_TUNNEL_ACTION_DATA_VALUE &&
                   request->data.as.value != NULL) {
            size_t n = strlen(request->data.as.value);

            queued.text = (char *)malloc(n + 1u);
            if (queued.text == NULL) {
                return;
            }
            memcpy(queued.text, request->data.as.value, n + 1u);
        } else {
            return; /* a value with nothing in it */
        }
        break;
    default:
        return; /* one this toolkit does not offer */
    }

    SDL_LockMutex(a11y->lock);
    if (a11y->queued < SCHULTZ_A11Y_QUEUE) {
        a11y->queue[a11y->queued++] = queued;
        queued.text = NULL; /* the queue owns it now */
    } else {
        a11y->dropped++;
    }
    SDL_UnlockMutex(a11y->lock);
    free(queued.text);
}

/*
 * The scroll view that holds a node, if any. Bringing something into view is
 * the enclosing view's job, not the node's, so the request has to be carried
 * up to whichever ancestor said it can scroll.
 */
static schultz_handle schultz_a11y_scroller(const schultz_a11y *a11y,
                                            schultz_handle node)
{
    schultz_handle walk = node;

    while (walk != SCHULTZ_HANDLE_NONE) {
        schultz_handle parent = SCHULTZ_HANDLE_NONE;

        if (schultz_node_get_actions(a11y->tree, walk) &
            SCHULTZ_ACTION_SCROLL) {
            return walk;
        }
        if (schultz_node_parent(a11y->tree, walk, &parent) != SCHULTZ_OK) {
            break;
        }
        walk = parent;
    }
    return SCHULTZ_HANDLE_NONE;
}

/*
 * Carries out a request to set a value.
 *
 * schultz_events_perform deliberately takes no payload, because a value is
 * specific to the kind of widget receiving it while every other action is
 * not. So this is the one action the bridge finishes itself, by calling the
 * widget's own setter.
 *
 * A slider and a scroll bar share a role, so the two are told apart by asking
 * each in turn: a setter refuses a node that is not its own widget, which
 * makes trying one and then the other exact rather than a guess.
 */
static void schultz_a11y_set_value(schultz_a11y *a11y,
                                   const schultz_a11y_request *request)
{
    if (!(schultz_node_get_actions(a11y->tree, request->node) &
          SCHULTZ_ACTION_SET_VALUE)) {
        return;
    }
    if (request->has_number) {
        float value = (float)request->number;

        if (schultz_slider_set_value(a11y->tree, request->node, value)
                != SCHULTZ_OK) {
            schultz_scroll_bar_set_value(a11y->tree, request->node, value);
        }
        return;
    }
    if (request->text != NULL) {
        schultz_text_set(a11y->tree, request->node, request->text);
    }
}

/*
 * Builds a whole tree on demand, for a platform that asks rather than is
 * told. iOS is the only one: its adapter sleeps until VoiceOver starts and
 * then wants everything at once.
 *
 * Called on whatever thread the platform chose. On iOS that is the main
 * thread, which is also the thread running the frame loop, so the widget
 * tree is walked by the thread that owns it. A platform that called this
 * from anywhere else would need a lock the update path does not have.
 */
static bool schultz_a11y_on_build(access_tunnel_tree_update *out_update,
                                  void *userdata)
{
    schultz_a11y *a11y = (schultz_a11y *)userdata;

    if (a11y == NULL || out_update == NULL) {
        return false;
    }
    return schultz_a11y_build(a11y, out_update) == SCHULTZ_OK;
}

int32_t schultz_a11y_create(schultz_tree *tree, schultz_events *events,
                            const schultz_a11y_options *options,
                            schultz_a11y **out_a11y)
{
    schultz_a11y *a11y;
    schultz_a11y_backend_config config;
    access_tunnel_tree_update update;
    int32_t result;

    if (tree == NULL || events == NULL || out_a11y == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    a11y = (schultz_a11y *)calloc(1, sizeof(*a11y));
    if (a11y == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    a11y->tree   = tree;
    a11y->events = events;
    a11y->focus  = SCHULTZ_HANDLE_NONE;
    a11y->lock   = SDL_CreateMutex();
    if (a11y->lock == NULL) {
        free(a11y);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    result = schultz_a11y_build(a11y, &update);
    if (result != SCHULTZ_OK) {
        SDL_DestroyMutex(a11y->lock);
        free(a11y);
        return result;
    }

    memset(&config, 0, sizeof(config));
    config.app_name = (options != NULL && options->app_name != NULL)
                          ? options->app_name : "Schultz";
    config.toolkit_name    = "Schultz";
    config.toolkit_version = SCHULTZ_A11Y_VERSION;
    config.action          = schultz_a11y_on_action;
    config.action_userdata = a11y;
    config.focused         = (options != NULL) ? (options->focused != 0) : 1;
    config.build           = schultz_a11y_on_build;
    config.build_userdata  = a11y;
    if (options != NULL) {
        config.window        = options->window;
        config.native_window = options->native_window;
    }

    /*
     * Succeeds whether or not anything is listening. There may be no
     * accessibility bus, no screen reader and nothing running at all, and the
     * right behaviour then is to sit quietly rather than to fail an
     * application's startup.
     */
    result = schultz_a11y_backend_create(&config, &update, &a11y->backend);
    access_tunnel_tree_update_free(&update);
    if (result != SCHULTZ_OK) {
        SDL_DestroyMutex(a11y->lock);
        free(a11y);
        return result;
    }

    a11y->started = 1;
    *out_a11y = a11y;
    return SCHULTZ_OK;
}

void schultz_a11y_destroy(schultz_a11y *a11y)
{
    if (a11y == NULL) {
        return;
    }
    schultz_a11y_backend_destroy(a11y->backend);
    SDL_DestroyMutex(a11y->lock);
    free(a11y);
}

int32_t schultz_a11y_update(schultz_a11y *a11y)
{
    access_tunnel_tree_update update;
    schultz_rect dirty;
    int32_t result;

    if (a11y == NULL || !a11y->started) {
        return SCHULTZ_OK;
    }
    /*
     * Republish when anything was marked for repaint, or when focus moved.
     *
     * This is the safe reading rather than the tight one: a hover that
     * repaints a button marks the tree dirty without changing a word of what
     * a reader would say, and that costs a rebuild. The opposite mistake is
     * worse and silent, because a reader then describes a window that stopped
     * being true, so the cheap side is the one to be wrong on until this is
     * measured.
     */
    if (schultz_tree_dirty_region(a11y->tree, &dirty) == SCHULTZ_OK &&
        schultz_rect_is_empty(dirty) &&
        schultz_events_focus(a11y->events) == a11y->focus) {
        return SCHULTZ_OK;
    }

    result = schultz_a11y_build(a11y, &update);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_a11y_backend_update(a11y->backend, &update);
    access_tunnel_tree_update_free(&update);
    a11y->serial++;
    return SCHULTZ_OK;
}

int32_t schultz_a11y_pump(schultz_a11y *a11y)
{
    return schultz_a11y_backend_pump((a11y != NULL) ? a11y->backend : NULL);
}

uint32_t schultz_a11y_drain(schultz_a11y *a11y)
{
    schultz_a11y_request taken[SCHULTZ_A11Y_QUEUE];
    uint32_t count;
    uint32_t i;

    if (a11y == NULL) {
        return 0u;
    }
    SDL_LockMutex(a11y->lock);
    count = a11y->queued;
    if (count > 0u) {
        memcpy(taken, a11y->queue, (size_t)count * sizeof(taken[0]));
        a11y->queued = 0u;
    }
    SDL_UnlockMutex(a11y->lock);

    for (i = 0; i < count; i++) {
        if (taken[i].action == SCHULTZ_ACTION_SET_VALUE) {
            schultz_a11y_set_value(a11y, &taken[i]);
            free(taken[i].text);
        } else if (taken[i].reveal) {
            schultz_handle view = schultz_a11y_scroller(a11y, taken[i].node);

            if (view != SCHULTZ_HANDLE_NONE) {
                schultz_scroll_view_reveal(a11y->tree, view, taken[i].node);
            }
        } else {
            schultz_events_perform(a11y->events, taken[i].node,
                                   taken[i].action);
        }
    }
    return count;
}

int32_t schultz_a11y_set_window(schultz_a11y *a11y, schultz_rect window)
{
    return schultz_a11y_backend_set_window(
        (a11y != NULL) ? a11y->backend : NULL, window);
}

int32_t schultz_a11y_set_focused(schultz_a11y *a11y, int32_t focused)
{
    return schultz_a11y_backend_set_focused(
        (a11y != NULL) ? a11y->backend : NULL, focused != 0);
}
