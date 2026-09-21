/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_event.c
 * @brief Event routing: hit testing, hover, press, focus, and overlays.
 */

#include "schultz_event.h"

#include "schultz_widget.h"

#include <stdlib.h>
#include <string.h>

/** @brief Which node is hovered, pressed and focused for this session. */
struct schultz_events {
    schultz_tree       *tree;         /**< The tree being routed into. */
    /*
     * Telling one press from two needs a clock and a little patience about
     * where the pointer is. Both thresholds are the usual ones: half a second
     * and a few logical units, which is about as far as a hand moves while
     * clicking twice on purpose.
     */
    uint64_t            now;          /**< What the host last said the time is. */
    uint64_t            last_press;   /**< When the last press landed. */
    schultz_point       press_at;     /**< Where it landed. */
    schultz_handle      press_node;   /**< What it landed on. */
    uint32_t            click_count;  /**< How many in a row, from 1. */
    /** Registered keyboard shortcuts, checked when nothing else wanted the key. */
    struct {
        uint32_t       key;       /**< Which key, or 0 for an empty slot. */
        uint32_t       modifiers; /**< Which modifiers must be held. */
        schultz_handle node;      /**< What it activates. */
    } accelerators[SCHULTZ_ACCELERATORS_MAX];
    uint32_t            accelerator_count; /**< Slots in use. */
    /*
     * The queue a host across a language boundary reads instead of taking a
     * callback per event. Text is copied into a buffer of its own, because
     * the pointer an event carries is only good for the length of one call.
     */
    schultz_event      *queue;         /**< Recorded events, or NULL. */
    uint32_t            queue_count;   /**< How many are recorded. */
    uint32_t            queue_size;    /**< How many fit. */
    char               *queue_text;    /**< Their text, back to back. */
    uint32_t            text_used;     /**< Bytes of it in use. */
    uint32_t            text_size;     /**< Bytes allocated. */
    uint32_t            queue_on;      /**< Nonzero while recording. */
    uint32_t            queue_taken;   /**< Set once drained, until reused. */
    schultz_event_callback_fn callback;     /**< Host entry point, or NULL. */
    void               *host_context; /**< Passed to callback unchanged. */

    schultz_handle      hovered;      /**< Node under the pointer. */
    schultz_handle      captured;     /**< Node holding the press. */
    schultz_handle      focused;      /**< Node holding keyboard focus. */
    uint32_t            source;       /**< SCHULTZ_POINTER_* of the pointer. */
    /** Nonzero while a delete key means one codepoint, not one
     *  character. See schultz_events_set_delete_by_codepoint. */
    uint32_t            one_codepoint;
    uint32_t            buttons;      /**< Bit set of held buttons. */
};

int32_t schultz_events_create(schultz_tree *tree, schultz_events **out_events)
{
    schultz_events *events;

    if (tree == NULL || out_events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    events = (schultz_events *)calloc(1, sizeof(*events));
    if (events == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    events->tree = tree;
    *out_events = events;
    return SCHULTZ_OK;
}

void schultz_events_destroy(schultz_events *events)
{
    if (events != NULL) {
        free(events->queue);
        free(events->queue_text);
    }
    free(events);
}

int32_t schultz_events_set_callback(schultz_events *events,
                                    schultz_event_callback_fn callback,
                                    void *host_context)
{
    if (events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    events->callback     = callback;
    events->host_context = host_context;
    return SCHULTZ_OK;
}

/** How long after a press a second one still counts as a double click. */
#define SCHULTZ_MULTI_CLICK_MS 500u
/** How far the pointer may move between them, in logical units. */
#define SCHULTZ_MULTI_CLICK_SLOP 4.0f

/*
 * How far a finger may travel and still be a tap.
 *
 * A finger that moved was doing something else, almost always scrolling, and
 * on a touch screen the thing it started on is usually the thing being
 * scrolled: press a row, drag, and the row travels under the finger, so it is
 * still what the release lands on. Without this every scroll would also press
 * whatever it began on.
 *
 * A mouse is left alone. Pressing a large button, moving the cursor inside it
 * and letting go is a click everywhere, and a cursor does not scroll by
 * dragging.
 */
#define SCHULTZ_TAP_SLOP 10.0f

/* ------------------------------------------------------------- queue */

/*
 * Copies one event into the queue. Text comes with it, because the pointer an
 * event carries belongs to whatever produced it and is good only for the
 * length of the call.
 */
static void schultz_event_record(schultz_events *events,
                                 const schultz_event *event)
{
    schultz_event *slot;
    uint32_t length;

    if (!events->queue_on) {
        return;
    }
    if (events->queue_taken) {
        /* Whatever was handed out has been read by now: start again. */
        events->queue_count = 0u;
        events->text_used   = 0u;
        events->queue_taken = 0u;
    }

    if (events->queue_count == events->queue_size) {
        uint32_t size = (events->queue_size == 0u) ? 32u
                                                   : events->queue_size * 2u;
        schultz_event *grown = (schultz_event *)realloc(
            events->queue, (size_t)size * sizeof(*grown));

        if (grown == NULL) {
            return; /* dropped rather than crashed; the next one may fit */
        }
        events->queue      = grown;
        events->queue_size = size;
    }

    slot  = &events->queue[events->queue_count];
    *slot = *event;
    slot->text = NULL;

    length = (event->text == NULL) ? 0u : (uint32_t)strlen(event->text);
    if (length > 0u) {
        if (events->text_used + length + 1u > events->text_size) {
            uint32_t size = (events->text_size == 0u) ? 256u
                                                      : events->text_size;
            char *grown;

            while (size < events->text_used + length + 1u) {
                size *= 2u;
            }
            grown = (char *)realloc(events->queue_text, size);
            if (grown == NULL) {
                return;
            }
            /*
             * The buffer moved, so every text pointer already handed out
             * points into freed memory. They are all inside this buffer, so
             * they are all fixed by re-pointing them at their own offsets.
             */
            if (grown != events->queue_text) {
                uint32_t i;
                uint32_t offset = 0u;

                for (i = 0; i < events->queue_count; i++) {
                    if (events->queue[i].text != NULL) {
                        events->queue[i].text = grown + offset;
                        offset += (uint32_t)strlen(grown + offset) + 1u;
                    }
                }
            }
            events->queue_text = grown;
            events->text_size  = size;
        }
        memcpy(events->queue_text + events->text_used, event->text,
               length + 1u);
        slot->text = events->queue_text + events->text_used;
        events->text_used += length + 1u;
    }
    events->queue_count++;
}

int32_t schultz_events_post(schultz_events *events,
                            const schultz_event *tree_event)
{
    if (events == NULL || tree_event == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /* Recorded before the callback, so a host using both sees the same
     * events in the same order however it chooses to read them. */
    schultz_event_record(events, tree_event);
    if (events->callback == NULL) {
        return SCHULTZ_OK;
    }
    return events->callback(events->host_context, tree_event);
}

int32_t schultz_events_set_queue(schultz_events *events, int32_t on)
{
    if (events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    events->queue_on = on ? 1u : 0u;
    if (!on) {
        free(events->queue);
        free(events->queue_text);
        events->queue       = NULL;
        events->queue_text  = NULL;
        events->queue_count = 0u;
        events->queue_size  = 0u;
        events->text_used   = 0u;
        events->text_size   = 0u;
        events->queue_taken = 0u;
    }
    return SCHULTZ_OK;
}

int32_t schultz_events_drain(schultz_events *events,
                             const schultz_event **out_events,
                             uint32_t *out_count)
{
    if (events == NULL || out_events == NULL || out_count == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_events = (events->queue_count > 0u) ? events->queue : NULL;
    *out_count  = events->queue_count;
    /*
     * Emptied on the next event rather than here, so the array the host is
     * holding stays readable until it routes something again.
     */
    events->queue_taken = 1u;
    return SCHULTZ_OK;
}

uint32_t schultz_events_pending(const schultz_events *events)
{
    if (events == NULL || events->queue_taken) {
        return 0u;
    }
    return events->queue_count;
}

/* ---------------------------------------------------------- delivery */

/*
 * Fills in the parts of an event that are the same everywhere, offers it to
 * the target's widget, and then hands it to the host. A node with no token
 * still routes: state is updated either way, and the host simply has nothing
 * registered for it.
 *
 * The widget goes first. That is what lets built in behaviour work with no
 * host code at all: a checkbox toggles itself, and a text field will edit
 * itself. A widget that consumes an event stops it there; a button does not
 * consume its click, because the host still wants to hear about it.
 */
static int32_t schultz_event_send(schultz_events *events, uint32_t type,
                                  schultz_handle target, schultz_point point,
                                  uint32_t button, uint32_t modifiers,
                                  uint32_t key, const char *text,
                                  float scroll_x, float scroll_y)
{
    const schultz_widget_vtable *widget;
    schultz_handle walk;
    schultz_event event;
    schultz_rect bounds;

    if (target == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_OK;
    }

    memset(&event, 0, sizeof(event));
    event.type      = type;
    event.target    = target;
    event.token     = schultz_node_get_token(events->tree, target);
    event.position  = point;
    event.button    = button;
    event.modifiers = modifiers;
    event.key       = key;
    event.text      = text;
    event.scroll_x  = scroll_x;
    event.scroll_y  = scroll_y;
    event.source    = events->source;
    /*
     * Only on a key: it says how much one press of a delete key takes, and
     * nothing else has a delete key.
     */
    if (type == SCHULTZ_EVENT_KEY_DOWN || type == SCHULTZ_EVENT_KEY_UP) {
        event.one_codepoint = events->one_codepoint;
    }
    /* Only a press and what follows from it can be a second or third one. */
    if (type == SCHULTZ_EVENT_MOUSE_DOWN || type == SCHULTZ_EVENT_MOUSE_UP ||
        type == SCHULTZ_EVENT_CLICK) {
        event.click_count = events->click_count;
    }

    /* Local coordinates save every listener from walking to the root. */
    if (schultz_node_absolute_bounds(events->tree, target, &bounds)
            == SCHULTZ_OK) {
        event.local = schultz_point_make(point.x - bounds.x,
                                         point.y - bounds.y);
    } else {
        event.local = point;
    }

    /*
     * Which styled stretch was pressed, for text that is not all one piece.
     * Asked here rather than left to the widget because the widget cannot
     * reach the event that goes out: it is handed one to read. Filling it in
     * on the way past keeps a click one event, which is what every other
     * widget produces.
     */
    event.span     = SCHULTZ_SPAN_NONE;
    event.span_tag = 0u;
    if (type == SCHULTZ_EVENT_CLICK) {
        const schultz_widget_vtable *at =
            schultz_node_widget(events->tree, target);

        if (at != NULL && at->clickable_span != NULL) {
            uint32_t which = 0u;
            uint64_t tag = 0u;

            if (at->clickable_span(events->tree, target, event.local, &which,
                                   &tag)) {
                event.span     = which;
                event.span_tag = tag;
            }
        }
    }

    /*
     * The target's widget first, then its ancestors, until one takes the
     * event. A control made of nodes has to hear about what happens to the
     * nodes it is made of: a tab view owns the buttons in its strip, a combo
     * box owns the rows of its menu, and neither can be told any other way.
     * A widget that does not recognise an event returns zero and it carries
     * on up.
     */
    walk = target;
    while (walk != SCHULTZ_HANDLE_NONE) {
        schultz_handle parent = SCHULTZ_HANDLE_NONE;

        widget = schultz_node_widget(events->tree, walk);
        if (widget != NULL && widget->event != NULL) {
            int32_t result = widget->event(events->tree, walk, &event);

            if (result != SCHULTZ_OK) {
                return result;
            }
        }
        if (schultz_node_parent(events->tree, walk, &parent) != SCHULTZ_OK) {
            break;
        }
        walk = parent;
    }

    /*
     * Recorded before the callback, so a host using both sees the same events
     * in the same order however it chooses to read them.
     */
    schultz_event_record(events, &event);

    if (events->callback == NULL) {
        return SCHULTZ_OK;
    }
    return events->callback(events->host_context, &event);
}

/* -------------------------------------------------------- hit testing */

/*
 * Depth first, last child first, because later children paint on top and the
 * topmost thing under the pointer is what should receive the event.
 */
static schultz_handle schultz_hit_subtree(schultz_tree *tree,
                                          schultz_handle node,
                                          schultz_point point)
{
    schultz_rect bounds;
    uint32_t count;
    uint32_t i;

    if (!(schultz_node_get_state(tree, node) & SCHULTZ_STATE_VISIBLE)) {
        return SCHULTZ_HANDLE_NONE;
    }
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }

    /*
     * A node that clips its children hides whatever falls outside it, and
     * what cannot be seen must not be clickable either.
     *
     * This matters because a scroll view keeps its rows where they are and
     * moves them by the scroll offset, so a row scrolled off the top still
     * has bounds: they are simply somewhere the user cannot see. The painter
     * clips it away. Without this, hit testing does not, and that invisible
     * row lies over whatever is drawn above the scroll view and swallows
     * every press meant for it. What that looks like is a header or a tab
     * strip that has stopped working, with nothing on screen to explain it.
     *
     * Only for a parent that clips. One that does not keeps the behaviour
     * below, where a child hanging outside its parent is still reachable.
     */
    if (schultz_node_clips_children(tree, node) &&
        !schultz_rect_contains_point(bounds, point)) {
        return SCHULTZ_HANDLE_NONE;
    }

    count = schultz_node_child_count(tree, node);
    for (i = count; i > 0u; i--) {
        schultz_handle child;
        schultz_handle hit;

        if (schultz_node_child_at(tree, node, i - 1u, &child) != SCHULTZ_OK) {
            continue;
        }
        hit = schultz_hit_subtree(tree, child, point);
        if (hit != SCHULTZ_HANDLE_NONE) {
            return hit;
        }
    }

    /*
     * Children are tested before the node itself so the deepest match wins,
     * and a child that overflows its parent is still reachable.
     */
    if (schultz_node_hit_testable(tree, node) &&
        schultz_rect_contains_point(bounds, point)) {
        return node;
    }
    return SCHULTZ_HANDLE_NONE;
}

int32_t schultz_events_hit_test(schultz_events *events, schultz_point point,
                                schultz_handle *out_node)
{
    uint32_t overlays;
    uint32_t i;

    if (events == NULL || out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_node = SCHULTZ_HANDLE_NONE;

    /* Overlays first, topmost down. */
    overlays = schultz_tree_overlay_count(events->tree);
    for (i = overlays; i > 0u; i--) {
        schultz_handle overlay;
        schultz_handle hit;

        if (schultz_tree_overlay_at(events->tree, i - 1u, &overlay, NULL)
                != SCHULTZ_OK) {
            continue;
        }
        hit = schultz_hit_subtree(events->tree, overlay, point);
        if (hit != SCHULTZ_HANDLE_NONE) {
            *out_node = hit;
            return SCHULTZ_OK;
        }
    }

    *out_node = schultz_hit_subtree(events->tree,
                                    schultz_tree_root(events->tree), point);
    return SCHULTZ_OK;
}

/* True when node is inside the given overlay's subtree. */
static int32_t schultz_within(schultz_tree *tree, schultz_handle root,
                              schultz_handle node)
{
    schultz_handle walk = node;

    while (walk != SCHULTZ_HANDLE_NONE) {
        schultz_handle parent;
        if (walk == root) {
            return 1;
        }
        if (schultz_node_parent(tree, walk, &parent) != SCHULTZ_OK) {
            return 0;
        }
        walk = parent;
    }
    return 0;
}

/* Adds or removes one state flag, which marks the node dirty when it moves. */
static void schultz_event_set_flag(schultz_events *events,
                                   schultz_handle node, uint32_t flag,
                                   int32_t on)
{
    uint32_t state;

    if (node == SCHULTZ_HANDLE_NONE) {
        return;
    }
    state = schultz_node_get_state(events->tree, node);
    schultz_node_set_state(events->tree, node,
                           on ? (state | flag) : (state & ~flag));
}

/*
 * Lets go of a node the router can no longer send anything to.
 *
 * Focus, hover and pointer capture are the router's own memory of three
 * nodes, and they are the only way a node is reached without walking the
 * tree from the root. So they are the only way a node that a host detached
 * with schultz_node_set_parent could carry on taking input: a removed button
 * would still activate on the space bar, and a removed row would still take a
 * drag. This is called before anything is routed, so every entry point starts
 * from a router that only remembers nodes that are on screen.
 *
 * A destroyed node needs none of this, because its handle stops resolving and
 * everything downstream reads that as nothing. A detached node is alive, so
 * it has to be let go of deliberately, and so does a node that was focused
 * and then disabled: tab never reaches a disabled control, but one that
 * already had focus when it was turned off keeps it, and the space bar would
 * still press it.
 *
 * The state flags go with it, or a widget put back into the tree comes back
 * wearing a focus ring or a hover highlight it did not earn. No event is
 * sent: the host asked for this by taking the widget out, and telling it that
 * what it just removed is no longer focused says nothing it does not know.
 */
/*
 * Whether the router will deal with a node at all.
 *
 * Out of the tree, or disabled. The two are different sentences and the same
 * answer: there is nothing to send input to. Disabled is asked of the whole
 * chain, so a node inside a disabled panel is disabled too.
 */
static int32_t schultz_events_takes_input(const schultz_events *events,
                                          schultz_handle node)
{
    return (schultz_node_is_attached(events->tree, node) &&
            schultz_node_is_enabled(events->tree, node)) ? 1 : 0;
}

/*
 * What the pointer landed on, or nothing when what it landed on is disabled.
 *
 * The hit itself is left alone on purpose. A disabled control still stops the
 * pointer reaching whatever is behind it, which is what a disabled control
 * does everywhere: a greyed button on a panel does not quietly hand its
 * presses to the panel. It stops them and does nothing with them, and this is
 * where the doing nothing happens.
 */
/* Whether the pointer has moved far enough since the press that a release is
 * not a tap. Only ever true of a finger; see SCHULTZ_TAP_SLOP. */
static int32_t schultz_events_travelled(const schultz_events *events,
                                        schultz_point point)
{
    float dx = point.x - events->press_at.x;
    float dy = point.y - events->press_at.y;

    if (events->source != SCHULTZ_POINTER_TOUCH) {
        return 0;
    }
    if (dx < 0.0f) { dx = -dx; }
    if (dy < 0.0f) { dy = -dy; }
    return (dx > SCHULTZ_TAP_SLOP || dy > SCHULTZ_TAP_SLOP) ? 1 : 0;
}

static schultz_handle schultz_events_live(const schultz_events *events,
                                          schultz_handle hit)
{
    if (hit != SCHULTZ_HANDLE_NONE &&
        !schultz_node_is_enabled(events->tree, hit)) {
        return SCHULTZ_HANDLE_NONE;
    }
    return hit;
}

static void schultz_events_let_go(schultz_events *events)
{
    if (events->focused != SCHULTZ_HANDLE_NONE &&
        !schultz_events_takes_input(events, events->focused)) {
        schultz_event_set_flag(events, events->focused, SCHULTZ_STATE_FOCUSED,
                               0);
        events->focused = SCHULTZ_HANDLE_NONE;
    }
    if (events->hovered != SCHULTZ_HANDLE_NONE &&
        !schultz_events_takes_input(events, events->hovered)) {
        schultz_event_set_flag(events, events->hovered, SCHULTZ_STATE_HOVERED,
                               0);
        events->hovered = SCHULTZ_HANDLE_NONE;
    }
    if (events->captured != SCHULTZ_HANDLE_NONE &&
        !schultz_events_takes_input(events, events->captured)) {
        schultz_event_set_flag(events, events->captured,
                               SCHULTZ_STATE_PRESSED, 0);
        events->captured = SCHULTZ_HANDLE_NONE;
    }
}

/* --------------------------------------------------------------- mouse */

int32_t schultz_events_mouse_move(schultz_events *events, schultz_point point,
                                  uint32_t modifiers)
{
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    if (events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_events_let_go(events);
    /* Where a widget that appears near the pointer will look for it. */
    schultz_tree_set_pointer(events->tree, point);

    /* A held button captures the pointer: motion is a drag, not a hover. */
    if (events->captured != SCHULTZ_HANDLE_NONE) {
        return schultz_event_send(events, SCHULTZ_EVENT_DRAG,
                                  events->captured, point, 0, modifiers, 0,
                                  NULL, 0.0f, 0.0f);
    }

    schultz_events_hit_test(events, point, &hit);
    hit = schultz_events_live(events, hit);

    if (hit != events->hovered) {
        schultz_handle previous = events->hovered;

        schultz_event_set_flag(events, previous, SCHULTZ_STATE_HOVERED, 0);
        events->hovered = hit;
        schultz_event_set_flag(events, hit, SCHULTZ_STATE_HOVERED, 1);

        if (previous != SCHULTZ_HANDLE_NONE) {
            schultz_event_send(events, SCHULTZ_EVENT_MOUSE_LEAVE, previous,
                               point, 0, modifiers, 0, NULL, 0.0f, 0.0f);
        }
        if (hit != SCHULTZ_HANDLE_NONE) {
            schultz_event_send(events, SCHULTZ_EVENT_MOUSE_ENTER, hit, point,
                               0, modifiers, 0, NULL, 0.0f, 0.0f);
        }
    }

    return schultz_event_send(events, SCHULTZ_EVENT_MOUSE_MOVE, hit, point, 0,
                              modifiers, 0, NULL, 0.0f, 0.0f);
}

/*
 * Whether a node sits inside something that keeps focus.
 *
 * A press lands on whatever is under it, and between the keys of an
 * on-screen keyboard that is the keyboard itself rather than a key. The
 * whole of it has to behave the same way, or missing a key by two pixels
 * ends the edit the keyboard exists to serve.
 */
static int32_t schultz_event_inside_keeps_focus(schultz_events *events,
                                                schultz_handle node)
{
    schultz_handle walk = node;

    while (walk != SCHULTZ_HANDLE_NONE) {
        schultz_handle parent = SCHULTZ_HANDLE_NONE;

        if (schultz_node_keeps_focus(events->tree, walk)) {
            return 1;
        }
        if (schultz_node_parent(events->tree, walk, &parent) != SCHULTZ_OK) {
            break;
        }
        walk = parent;
    }
    return 0;
}

/*
 * Puts focus where a press says it goes.
 *
 * A press decides where focus is, rather than only being able to move it. On
 * something focusable it goes there; on anything else, empty space or a label
 * or a plain container, it goes away.
 *
 * Leaving focus alone is what kept an on-screen keyboard up. The keyboard is
 * a pure function of focus and nothing manages it separately, so a field that
 * never lost focus never let the keyboard go, and the only way to close it
 * was to press something else focusable. Pressing the background is how a
 * person says they are done, and it is what a desktop does too: click the
 * empty canvas and the caret leaves the field.
 *
 * A node that keeps focus is asked first, ahead of whether it is focusable,
 * because there are two ways to end an edit and it has to stop both:
 * clearing focus, and taking it. A Bold button over a text area is focusable,
 * so it is reachable by tab like every other control, and pressing it must
 * still leave the caret in the text it is about to embolden.
 *
 * **When this runs depends on the pointer.** A cursor decides on the way
 * down, which is what a desktop does and what dragging to select text needs.
 * A finger decides when it lifts, and only if it stayed still: a finger that
 * lands on a field and moves is scrolling the page, and focusing the field
 * would put a keyboard over the thing the person was trying to read. That is
 * what a browser does, and what Android does; iOS is the odd one out, and an
 * application there generally turns it off.
 */
static void schultz_events_take_focus(schultz_events *events,
                                      schultz_handle hit)
{
    if (hit != SCHULTZ_HANDLE_NONE &&
        schultz_node_keeps_focus(events->tree, hit)) {
        return; /* where it is now is where it stays */
    }
    if (hit != SCHULTZ_HANDLE_NONE &&
        (schultz_node_get_actions(events->tree, hit) &
         SCHULTZ_ACTION_FOCUS)) {
        schultz_events_set_focus(events, hit);
        return;
    }
    /*
     * Landing between the keys rather than on one. The gap belongs to the
     * keyboard, which keeps focus, so a finger that misses a key must not end
     * the edit the keyboard is serving. Asked after focusable, so a field
     * sitting inside something that keeps focus still takes it when pressed.
     */
    if (schultz_event_inside_keeps_focus(events, hit)) {
        return;
    }
    schultz_events_set_focus(events, SCHULTZ_HANDLE_NONE);
}

int32_t schultz_events_mouse_button(schultz_events *events,
                                    schultz_point point, uint32_t button,
                                    int32_t down, uint32_t modifiers)
{
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    if (events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_events_let_go(events);
    schultz_events_hit_test(events, point, &hit);

    if (down) {
        uint32_t overlays = schultz_tree_overlay_count(events->tree);

        /*
         * A press outside a capturing overlay closes it and goes no further.
         * Without this, clicking away from a menu would both dismiss it and
         * activate whatever happened to be underneath.
         */
        if (overlays > 0u) {
            schultz_handle top = SCHULTZ_HANDLE_NONE;
            int32_t captures = 0;

            if (schultz_tree_overlay_at(events->tree, overlays - 1u, &top,
                                        &captures) == SCHULTZ_OK &&
                captures && !schultz_within(events->tree, top, hit)) {
                return schultz_event_send(events, SCHULTZ_EVENT_DISMISS, top,
                                          point, button, modifiers, 0, NULL,
                                          0.0f, 0.0f);
            }
        }

        /*
         * A second press counts as a double only when it lands on the same
         * node, soon enough, and close enough. Any of the three failing makes
         * it a fresh single click.
         */
        {
            float dx = point.x - events->press_at.x;
            float dy = point.y - events->press_at.y;

            if (dx < 0.0f) { dx = -dx; }
            if (dy < 0.0f) { dy = -dy; }
            if (hit == events->press_node && events->last_press != 0u &&
                events->now - events->last_press <= SCHULTZ_MULTI_CLICK_MS &&
                dx <= SCHULTZ_MULTI_CLICK_SLOP &&
                dy <= SCHULTZ_MULTI_CLICK_SLOP) {
                events->click_count++;
            } else {
                events->click_count = 1u;
            }
            events->last_press = events->now;
            events->press_at   = point;
            events->press_node = hit;
        }

        /*
         * The secondary button asks for a context menu rather than pressing
         * what is under it, which is what it means everywhere.
         */
        /*
         * From here on it is the live hit, so a disabled node is pressed by
         * nothing, focused by nothing and told nothing. The raw hit was used
         * above, where the question was where the press landed rather than
         * what it should do: a press on a disabled row inside a menu is still
         * inside the menu and must not dismiss it.
         *
         * Capture is what makes the release silent as well. A click is only
         * sent when the press and the release are on the same node, and a
         * press that captured nothing has nothing for the release to match.
         */
        hit = schultz_events_live(events, hit);

        if (button == SCHULTZ_BUTTON_RIGHT) {
            return schultz_event_send(events, SCHULTZ_EVENT_CONTEXT_MENU, hit,
                                      point, button, modifiers, 0, NULL,
                                      0.0f, 0.0f);
        }

        events->buttons |= (1u << button);
        events->captured = hit;
        schultz_event_set_flag(events, hit, SCHULTZ_STATE_PRESSED, 1);

        /*
         * A finger decides focus when it lifts, not when it lands: see
         * schultz_events_take_focus. Anything else decides it here.
         */
        if (events->source != SCHULTZ_POINTER_TOUCH) {
            schultz_events_take_focus(events, hit);
        }

        return schultz_event_send(events, SCHULTZ_EVENT_MOUSE_DOWN, hit,
                                  point, button, modifiers, 0, NULL, 0.0f,
                                  0.0f);
    }

    {
        schultz_handle pressed = events->captured;

        events->buttons &= ~(1u << button);
        events->captured = SCHULTZ_HANDLE_NONE;
        schultz_event_set_flag(events, pressed, SCHULTZ_STATE_PRESSED, 0);

        /*
         * A finger that stayed still has tapped, and a tap is where it says
         * what it wanted. One that travelled was scrolling, and scrolling
         * says nothing about focus.
         */
        if (events->source == SCHULTZ_POINTER_TOUCH &&
            !schultz_events_travelled(events, point)) {
            schultz_handle under = SCHULTZ_HANDLE_NONE;

            schultz_events_hit_test(events, point, &under);
            schultz_events_take_focus(events, schultz_events_live(events,
                                                                  under));
        }

        schultz_event_send(events, SCHULTZ_EVENT_MOUSE_UP, pressed, point,
                           button, modifiers, 0, NULL, 0.0f, 0.0f);

        /*
         * A click is a press and release on the same node, not merely a
         * release: dragging off a button and letting go must not activate it.
         * A finger has to have stayed still as well, because content that
         * scrolls with it carries the node it started on along underneath it.
         */
        if (pressed != SCHULTZ_HANDLE_NONE && pressed == hit &&
            !schultz_events_travelled(events, point)) {
            schultz_event_send(events, SCHULTZ_EVENT_CLICK, pressed, point,
                               button, modifiers, 0, NULL, 0.0f, 0.0f);
        }
    }
    return SCHULTZ_OK;
}

/*
 * The wheel is aimed at whatever can act on it, not at whatever happens to be
 * under the pointer. A label inside a scroll view cannot scroll, so the event
 * walks up to the nearest node that says it can, which is the one bit of
 * bubbling the router does. Everything else is delivered where it landed.
 */
static schultz_handle schultz_event_scrollable(schultz_events *events,
                                               schultz_handle node)
{
    schultz_handle walk = node;

    while (walk != SCHULTZ_HANDLE_NONE) {
        schultz_handle parent = SCHULTZ_HANDLE_NONE;

        if (schultz_node_get_actions(events->tree, walk) &
            SCHULTZ_ACTION_SCROLL) {
            return walk;
        }
        if (schultz_node_parent(events->tree, walk, &parent) != SCHULTZ_OK) {
            break;
        }
        walk = parent;
    }
    return node;
}

int32_t schultz_events_scroll(schultz_events *events, schultz_point point,
                              float scroll_x, float scroll_y)
{
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    if (events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_events_let_go(events);
    schultz_events_hit_test(events, point, &hit);
    hit = schultz_events_live(events, hit);
    return schultz_event_send(events, SCHULTZ_EVENT_SCROLL,
                              schultz_event_scrollable(events, hit), point, 0,
                              0, 0, NULL, scroll_x, scroll_y);
}

/* ------------------------------------------------------------ keyboard */

/* The node a shortcut names, or none when no shortcut matches. */
static schultz_handle schultz_event_accelerator(schultz_events *events,
                                                uint32_t key,
                                                uint32_t modifiers)
{
    uint32_t i;

    for (i = 0; i < events->accelerator_count; i++) {
        if (events->accelerators[i].key == key &&
            events->accelerators[i].modifiers == modifiers) {
            return events->accelerators[i].node;
        }
    }
    return SCHULTZ_HANDLE_NONE;
}

/*
 * Space and enter activate whatever holds focus. This lives in the router
 * rather than in each widget for the same reason the accessibility actions
 * do: a node that says it can be clicked can be clicked, whether a widget
 * vtable is installed on it or not.
 */
static int32_t schultz_event_activates(schultz_events *events, uint32_t key)
{
    if (key != (uint32_t)SCHULTZ_KEY_SPACE &&
        key != (uint32_t)SCHULTZ_KEY_RETURN) {
        return 0;
    }
    return (schultz_node_get_actions(events->tree, events->focused) &
            SCHULTZ_ACTION_CLICK) ? 1 : 0;
}

int32_t schultz_events_key(schultz_events *events, uint32_t key,
                           uint32_t modifiers, int32_t down)
{
    schultz_handle focused;
    schultz_point origin = schultz_point_make(0.0f, 0.0f);
    int32_t result;

    if (events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_events_let_go(events);
    focused = events->focused;

    /*
     * The key goes out first. A widget that wants the key gets it: a text
     * field types a space rather than activating itself.
     */
    result = schultz_event_send(events,
                                down ? SCHULTZ_EVENT_KEY_DOWN
                                     : SCHULTZ_EVENT_KEY_UP,
                                focused, origin, 0, modifiers, key, NULL,
                                0.0f, 0.0f);
    if (result != SCHULTZ_OK) {
        return result;
    }

    /*
     * Then whatever holds the text selection, when that is not already the
     * node the key went to. A block of prose is selectable without being
     * focusable, so copy has to reach it some other way, and this is it. It
     * comes before shortcuts for the same reason the focused node does: the
     * thing holding the selection is more specific than a global accelerator.
     */
    if (down) {
        schultz_handle owner = schultz_tree_selection_owner(events->tree);

        if (owner != SCHULTZ_HANDLE_NONE && owner != focused) {
            result = schultz_event_send(events, SCHULTZ_EVENT_KEY_DOWN, owner,
                                        origin, 0, modifiers, key, NULL,
                                        0.0f, 0.0f);
            if (result != SCHULTZ_OK) {
                return result;
            }
        }
    }

    /*
     * A shortcut fires only when whatever holds focus did not want the key,
     * which is why this comes after the key has been offered: a text field
     * typing control with V is not also a paste command aimed at a menu.
     */
    if (down) {
        schultz_handle target = schultz_event_accelerator(events, key,
                                                          modifiers);

        if (target != SCHULTZ_HANDLE_NONE) {
            return schultz_event_send(events, SCHULTZ_EVENT_CLICK, target,
                                      origin, 0, modifiers, key, NULL, 0.0f,
                                      0.0f);
        }
    }

    if (!schultz_event_activates(events, key)) {
        return result;
    }

    /*
     * Press on the way down and click on the way up, which is the same shape
     * as the mouse, so a control's pressed styling works for both.
     */
    if (down) {
        schultz_event_set_flag(events, focused, SCHULTZ_STATE_PRESSED, 1);
        return SCHULTZ_OK;
    }
    schultz_event_set_flag(events, focused, SCHULTZ_STATE_PRESSED, 0);
    return schultz_event_send(events, SCHULTZ_EVENT_CLICK, focused, origin,
                              0, modifiers, key, NULL, 0.0f, 0.0f);
}

int32_t schultz_events_text_input(schultz_events *events, const char *utf8)
{
    if (events == NULL || utf8 == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_events_let_go(events);
    return schultz_event_send(events, SCHULTZ_EVENT_TEXT_INPUT,
                              events->focused,
                              schultz_point_make(0.0f, 0.0f), 0, 0, 0, utf8,
                              0.0f, 0.0f);
}

int32_t schultz_events_text_editing(schultz_events *events, const char *utf8,
                                    int32_t cursor)
{
    if (events == NULL || utf8 == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_events_let_go(events);
    /* The cursor rides in the key field: it is the only integer an editing
     * event needs, and adding a member for it would widen every event. */
    return schultz_event_send(events, SCHULTZ_EVENT_TEXT_EDITING,
                              events->focused,
                              schultz_point_make(0.0f, 0.0f), 0, 0,
                              (uint32_t)(cursor < 0 ? 0 : cursor), utf8, 0.0f,
                              0.0f);
}

/* --------------------------------------------------------------- focus */

int32_t schultz_events_add_accelerator(schultz_events *events, uint32_t key,
                                       uint32_t modifiers,
                                       schultz_handle node)
{
    uint32_t i;

    if (events == NULL || key == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < events->accelerator_count; i++) {
        if (events->accelerators[i].key == key &&
            events->accelerators[i].modifiers == modifiers) {
            events->accelerators[i].node = node;
            return SCHULTZ_OK;
        }
    }
    if (node == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_OK; /* removing one that was never there */
    }
    if (events->accelerator_count == SCHULTZ_ACCELERATORS_MAX) {
        return SCHULTZ_ERR_EXHAUSTED;
    }
    events->accelerators[events->accelerator_count].key       = key;
    events->accelerators[events->accelerator_count].modifiers = modifiers;
    events->accelerators[events->accelerator_count].node      = node;
    events->accelerator_count++;
    return SCHULTZ_OK;
}

int32_t schultz_events_set_time(schultz_events *events, uint64_t now)
{
    if (events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    events->now = now;
    return SCHULTZ_OK;
}

int32_t schultz_events_set_focus(schultz_events *events, schultz_handle node)
{
    schultz_handle previous;

    if (events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (node != SCHULTZ_HANDLE_NONE &&
        schultz_node_get_state(events->tree, node) == 0u) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /*
     * Focus is where the keyboard goes, so it has to name something the user
     * can see and use. A node that is alive but out of the tree is neither
     * drawn nor reachable by tab, and one that is disabled answers nothing;
     * focusing either would put every keystroke somewhere it does nothing.
     */
    if (node != SCHULTZ_HANDLE_NONE &&
        !schultz_events_takes_input(events, node)) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_events_let_go(events);
    if (events->focused == node) {
        return SCHULTZ_OK;
    }

    previous = events->focused;
    schultz_event_set_flag(events, previous, SCHULTZ_STATE_FOCUSED, 0);
    events->focused = node;
    schultz_event_set_flag(events, node, SCHULTZ_STATE_FOCUSED, 1);

    if (previous != SCHULTZ_HANDLE_NONE) {
        schultz_event_send(events, SCHULTZ_EVENT_FOCUS_LOST, previous,
                           schultz_point_make(0.0f, 0.0f), 0, 0, 0, NULL,
                           0.0f, 0.0f);
    }
    if (node != SCHULTZ_HANDLE_NONE) {
        schultz_event_send(events, SCHULTZ_EVENT_FOCUS_GAINED, node,
                           schultz_point_make(0.0f, 0.0f), 0, 0, 0, NULL,
                           0.0f, 0.0f);
    }
    return SCHULTZ_OK;
}

schultz_tree *schultz_events_tree(const schultz_events *events)
{
    return (events == NULL) ? NULL : events->tree;
}

int32_t schultz_events_set_pointer_source(schultz_events *events,
                                          uint32_t source)
{
    if (events == NULL || source > SCHULTZ_POINTER_TOUCH) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    events->source = source;
    return SCHULTZ_OK;
}

int32_t schultz_events_set_delete_by_codepoint(schultz_events *events,
                                               int32_t on)
{
    if (events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    events->one_codepoint = on ? 1u : 0u;
    return SCHULTZ_OK;
}

int32_t schultz_events_perform(schultz_events *events, schultz_handle node,
                               uint32_t action)
{
    schultz_point origin = schultz_point_make(0.0f, 0.0f);
    uint32_t key;

    if (events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * Only what the widget said it offers. The accessibility schema is the
     * one declaration of what a node can be asked to do, and honouring it
     * here means a caller cannot reach past it. It also covers a handle that
     * no longer resolves, which declares no actions at all.
     */
    if (action == 0u ||
        !(schultz_node_get_actions(events->tree, node) & action)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * And only what is in the tree. A screen reader holds on to what it was
     * told about, so it can ask for an action on a widget the application has
     * since taken out; a node that is not on screen must not answer.
     */
    /*
     * And only what accepts input. The accessibility layer already tells the
     * platform when a node is disabled, so a screen reader announces a button
     * as unavailable; without this it could then press it anyway, and the two
     * halves of the same conversation would disagree.
     */
    if (!schultz_events_takes_input(events, node)) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }

    switch (action) {
    case SCHULTZ_ACTION_CLICK:
        /* A consumed event is a handled one, not a failure. */
        schultz_event_send(events, SCHULTZ_EVENT_CLICK, node, origin, 0, 0, 0,
                           NULL, 0.0f, 0.0f);
        return SCHULTZ_OK;
    case SCHULTZ_ACTION_FOCUS:
        return schultz_events_set_focus(events, node);
    case SCHULTZ_ACTION_INCREMENT:
    case SCHULTZ_ACTION_DECREMENT:
        key = (action == SCHULTZ_ACTION_INCREMENT)
                  ? (uint32_t)SCHULTZ_KEY_RIGHT : (uint32_t)SCHULTZ_KEY_LEFT;
        schultz_event_send(events, SCHULTZ_EVENT_KEY_DOWN, node, origin, 0, 0,
                           key, NULL, 0.0f, 0.0f);
        return SCHULTZ_OK;
    default:
        break;
    }
    return SCHULTZ_ERR_INVALID_ARGUMENT;
}

/*
 * The three below answer for a node only while it can still be sent input.
 * The router lets go of one that cannot before it routes anything, but a host
 * may ask between disabling a widget, or taking it out of the tree, and the
 * next event arriving, and until then the field still holds it. Testing here
 * means the answer is never a widget the caller has already switched off.
 */
static schultz_handle schultz_events_remembered(const schultz_events *events,
                                                schultz_handle node)
{
    if (events == NULL || node == SCHULTZ_HANDLE_NONE ||
        !schultz_events_takes_input(events, node)) {
        return SCHULTZ_HANDLE_NONE;
    }
    return node;
}

schultz_handle schultz_events_focus(const schultz_events *events)
{
    return (events == NULL)
               ? SCHULTZ_HANDLE_NONE
               : schultz_events_remembered(events, events->focused);
}

schultz_handle schultz_events_hovered(const schultz_events *events)
{
    return (events == NULL)
               ? SCHULTZ_HANDLE_NONE
               : schultz_events_remembered(events, events->hovered);
}

schultz_handle schultz_events_captured(const schultz_events *events)
{
    return (events == NULL)
               ? SCHULTZ_HANDLE_NONE
               : schultz_events_remembered(events, events->captured);
}

static int32_t schultz_focusable(schultz_tree *tree, schultz_handle node)
{
    /* Asked of the whole chain, so a control on a disabled panel is skipped
     * along with the panel rather than only when it is disabled itself. */
    if (!schultz_node_is_enabled(tree, node)) {
        return 0;
    }
    if (!schultz_node_is_visible(tree, node)) {
        return 0;
    }
    return (schultz_node_get_actions(tree, node) & SCHULTZ_ACTION_FOCUS)
               ? 1 : 0;
}

/*
 * Flattens the tree into focus order, which is a depth first walk with
 * parents before children. Writes at most `capacity` entries and reports how
 * many nodes there were, so a caller can tell when it ran out of room.
 */
static uint32_t schultz_focus_collect(schultz_tree *tree, schultz_handle node,
                                      schultz_handle *out, uint32_t capacity,
                                      uint32_t written)
{
    uint32_t count;
    uint32_t i;

    if (!(schultz_node_get_state(tree, node) & SCHULTZ_STATE_VISIBLE)) {
        return written; /* a hidden subtree holds nothing focusable */
    }
    if (schultz_focusable(tree, node) && written < capacity) {
        out[written++] = node;
    }

    count = schultz_node_child_count(tree, node);
    for (i = 0; i < count; i++) {
        schultz_handle child;
        if (schultz_node_child_at(tree, node, i, &child) == SCHULTZ_OK) {
            written = schultz_focus_collect(tree, child, out, capacity,
                                            written);
        }
    }
    return written;
}

enum {
    /* Focus order is gathered into a fixed array so the walk allocates
     * nothing. A tree with more focusable nodes than this cycles through the
     * first SCHULTZ_FOCUS_MAX of them. */
    SCHULTZ_FOCUS_MAX = 256
};

int32_t schultz_events_focus_move(schultz_events *events, int32_t forward)
{
    schultz_handle order[SCHULTZ_FOCUS_MAX];
    uint32_t count;
    uint32_t current = 0;
    uint32_t found = 0;
    uint32_t i;

    if (events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_events_let_go(events);

    count = schultz_focus_collect(events->tree,
                                  schultz_tree_root(events->tree), order,
                                  SCHULTZ_FOCUS_MAX, 0);
    if (count == 0u) {
        return SCHULTZ_ERR_EXHAUSTED;
    }

    for (i = 0; i < count; i++) {
        if (order[i] == events->focused) {
            current = i;
            found = 1;
            break;
        }
    }

    if (!found) {
        /* Nothing focused yet: forward starts at the first, back at the last. */
        return schultz_events_set_focus(events,
                                        forward ? order[0] : order[count - 1u]);
    }

    /* Wrap around, so tabbing past the end reaches the beginning. */
    if (forward) {
        current = (current + 1u) % count;
    } else {
        current = (current == 0u) ? count - 1u : current - 1u;
    }
    return schultz_events_set_focus(events, order[current]);
}
