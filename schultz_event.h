/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_event.h
 * @brief Event routing: hit testing, hover, press, focus, and overlays.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 *
 * Raw input arrives from the platform layer and is turned into toolkit events
 * aimed at a specific node. The routing state that makes that possible, which
 * node is hovered, which is pressed, which holds focus, lives here rather
 * than on the tree, because it is a property of the session rather than of
 * the structure.
 *
 * **The toolkit never owns a callback.** It stores one function the host
 * installs at startup, plus an opaque token per node. When an event fires,
 * the toolkit calls that function with the token and the host decides what
 * it means. That is what lets a Java host and a C host with a garbage
 * collector both drive the same toolkit without it tracing anything.
 */

#ifndef SCHULTZ_EVENT_H
#define SCHULTZ_EVENT_H

#include "schultz_node.h"

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


/** @brief What happened. */
enum {
    SCHULTZ_EVENT_MOUSE_ENTER = 1, /**< Pointer entered this node. */
    SCHULTZ_EVENT_MOUSE_LEAVE,     /**< Pointer left this node. */
    SCHULTZ_EVENT_MOUSE_MOVE,      /**< Pointer moved over this node. */
    SCHULTZ_EVENT_MOUSE_DOWN,      /**< A button went down on this node. */
    SCHULTZ_EVENT_MOUSE_UP,        /**< A button came up. */
    SCHULTZ_EVENT_CLICK,           /**< Press and release on the same node. */
    SCHULTZ_EVENT_DRAG,            /**< Pointer moved while captured. */
    SCHULTZ_EVENT_SCROLL,          /**< Wheel or trackpad scroll. */
    SCHULTZ_EVENT_FOCUS_GAINED,    /**< This node took keyboard focus. */
    SCHULTZ_EVENT_FOCUS_LOST,      /**< This node lost keyboard focus. */
    SCHULTZ_EVENT_KEY_DOWN,        /**< A key went down, to the focused node. */
    SCHULTZ_EVENT_KEY_UP,          /**< A key came up. */
    SCHULTZ_EVENT_TEXT_INPUT,      /**< Committed text, to the focused node. */
    SCHULTZ_EVENT_TEXT_EDITING,    /**< In progress IME composition. */
    SCHULTZ_EVENT_DISMISS,         /**< An overlay should close itself. */
    SCHULTZ_EVENT_CONTEXT_MENU,    /**< The secondary button asked for a menu. */
    SCHULTZ_EVENT_FILES_CHOSEN,    /**< A file dialog came back with paths. */
    SCHULTZ_EVENT_FILES_CANCELLED, /**< A file dialog was dismissed. */
    /**
     * The window changed size. `position` carries the new width and height in
     * pixels. The tree's viewport and root bounds have already been set to
     * match; what a host still owns is whatever it sized by hand.
     */
    SCHULTZ_EVENT_WINDOW_RESIZED
};

/**
 * @brief No span was pressed, which is what every event but a click on styled
 *        text reports.
 */
#define SCHULTZ_SPAN_NONE 0xFFFFFFFFu

/** @brief Mouse buttons, as a bit set for held state and an index for
 *         individual events. */
enum {
    SCHULTZ_BUTTON_LEFT = 1,   /**< Primary button. */
    SCHULTZ_BUTTON_MIDDLE,     /**< Wheel button. */
    SCHULTZ_BUTTON_RIGHT       /**< Secondary button, which opens menus. */
};

/**
 * @brief What a handler returns to say it took the event.
 *
 * Zero means "not mine, keep going" and every failure is negative, so a
 * positive value is free for this. A widget returns it to stop an event
 * reaching the host, and a host returns it to stop further routing.
 */
enum {
    SCHULTZ_EVENT_CONSUMED = 1
};

/**
 * @brief Keys, independent of the platform.
 *
 * A key that produces a character is named by the Unicode codepoint it types
 * with no modifiers held, so `A` is 0x61 and space is 0x20. This is what SDL,
 * GLFW and the browsers all do, and it means a host testing for a shortcut
 * letter can write the character rather than look up a constant.
 *
 * Keys that produce no character are named above the Unicode range, which
 * ends at 0x10FFFF, so the two sets can never collide. Return, escape, tab,
 * backspace and delete belong here: they have old ASCII control codes, but
 * those values are an accident of teletype history and say nothing useful.
 *
 * The platform layer translates. Nothing above it sees a platform key code.
 */
enum {
    SCHULTZ_KEY_UNKNOWN   = 0,        /**< Not one of the keys named here. */
    SCHULTZ_KEY_SPACE     = 0x20,     /**< The space bar. */

    SCHULTZ_KEY_RETURN    = 0x110000, /**< Enter or return. */
    SCHULTZ_KEY_ESCAPE,               /**< Escape. */
    SCHULTZ_KEY_TAB,                  /**< Tab. */
    SCHULTZ_KEY_BACKSPACE,            /**< Delete backwards. */
    SCHULTZ_KEY_DELETE,               /**< Delete forwards. */
    SCHULTZ_KEY_LEFT,                 /**< Left arrow. */
    SCHULTZ_KEY_RIGHT,                /**< Right arrow. */
    SCHULTZ_KEY_UP,                   /**< Up arrow. */
    SCHULTZ_KEY_DOWN,                 /**< Down arrow. */
    SCHULTZ_KEY_HOME,                 /**< Home. */
    SCHULTZ_KEY_END,                  /**< End. */
    SCHULTZ_KEY_PAGE_UP,              /**< Page up. */
    SCHULTZ_KEY_PAGE_DOWN             /**< Page down. */
};

/** @brief Keyboard modifiers, as a bit set. */
enum {
    SCHULTZ_MOD_SHIFT = 1u << 0, /**< Either shift key. */
    SCHULTZ_MOD_CTRL  = 1u << 1, /**< Either control key. */
    SCHULTZ_MOD_ALT   = 1u << 2, /**< Either alt or option key. */
    SCHULTZ_MOD_SUPER = 1u << 3  /**< Command, Windows, or super. */
};

/**
 * @brief What is driving the pointer.
 *
 * A touch screen produces the same press, drag and release a mouse does, so
 * without this a widget cannot tell a finger from a cursor. It matters where
 * the right gesture differs: text is selected by dragging with a mouse, and
 * by holding still and then dragging with a finger.
 */
enum {
    SCHULTZ_POINTER_MOUSE = 0, /**< A mouse, trackpad or pen. */
    SCHULTZ_POINTER_TOUCH      /**< A finger on a touch screen. */
};

/**
 * @brief One routed event.
 *
 * The struct is passed to the host callback by pointer and is valid only for
 * the duration of that call. Anything the host needs afterwards must be
 * copied, `text` included.
 */
typedef struct {
    uint32_t       type;      /**< One of the SCHULTZ_EVENT_* values. */
    schultz_handle target;    /**< The node this event is aimed at. */
    uint64_t       token;     /**< The host token registered on the target. */
    schultz_point  position;  /**< Pointer position in window coordinates. */
    schultz_point  local;     /**< Pointer position relative to the target. */
    uint32_t       button;    /**< Which button, for press and release. */
    uint32_t       modifiers; /**< Bit set of SCHULTZ_MOD_* values. */
    uint32_t       key;       /**< One of SCHULTZ_KEY_*, for key events. */
    const char    *text;      /**< NUL terminated UTF-8, or NULL. */
    float          scroll_x;  /**< Horizontal scroll, positive is right. */
    float          scroll_y;  /**< Vertical scroll, positive is down. */
    /**
     * How many times in a row this node was pressed: 1 for a single click, 2
     * for a double, 3 for a triple, and on upward. Counted only for the
     * button events, and only when the host supplies a clock with
     * schultz_events_set_time. Zero everywhere else.
     */
    uint32_t       click_count;
    /**
     * What moved the pointer: SCHULTZ_POINTER_MOUSE or SCHULTZ_POINTER_TOUCH.
     * A finger and a mouse produce the same events, and a widget that wants
     * to treat them differently has nothing else to tell them apart by.
     */
    uint32_t       source;
    /**
     * Set on a backspace or delete that removes one codepoint rather than
     * one character as a person sees them.
     *
     * A phone does not send key presses. It keeps a text field of its own,
     * watches it change, and spells the change out afterwards as one
     * backspace for every codepoint that went. One tap on its delete key
     * over an emoji written as a base and a modifier therefore arrives here
     * as two. Taking a whole character for each of those would take the
     * emoji and whatever stood before it.
     *
     * So a key marked this way removes exactly one codepoint, and the run
     * of them the platform sends removes exactly what the platform removed.
     * A key from a real keyboard is not marked, and takes the whole
     * character, which is what every editor does.
     */
    uint32_t       one_codepoint;
    /**
     * Which styled stretch of the target's text was pressed, counting from
     * zero, or SCHULTZ_SPAN_NONE when the press landed on none.
     *
     * Set on SCHULTZ_EVENT_CLICK, and only for a widget whose text carries
     * spans that were marked clickable. Everywhere else it is
     * SCHULTZ_SPAN_NONE.
     *
     * It names the span as the list stood when the press happened. A host
     * that rebuilds its spans before it reads the event should go by
     * `span_tag` instead, which is its own and means whatever it decided.
     */
    uint32_t       span;
    /** The `tag` of that span, or zero when there was none. */
    uint64_t       span_tag;
} schultz_event;

/**
 * @brief The single entry point the host installs to receive every event.
 *
 * @param host_context Opaque pointer given at installation.
 * @param event        The event. Valid only for this call.
 * @return SCHULTZ_OK to let routing continue, or SCHULTZ_EVENT_CONSUMED to
 *         take the event.
 */
typedef int32_t (*schultz_event_callback_fn)(void *host_context,
                                       const schultz_event *event);

/** @brief Opaque routing state. Create with schultz_events_create. */
typedef struct schultz_events schultz_events;

/**
 * @brief Creates routing state over a tree.
 *
 * @param tree       The tree to route into. Must not be NULL and must outlive
 *                   the routing state.
 * @param out_events Receives the new state. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when either pointer is
 *         NULL, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_events_create(schultz_tree *tree,
                              schultz_events **out_events);

/**
 * @brief Destroys routing state. The tree is untouched.
 *
 * @param events The state to destroy. NULL is accepted and does nothing.
 */
void schultz_events_destroy(schultz_events *events);

/**
 * @brief Installs the host's callback function.
 *
 * Call once at startup. Until it is installed, events are routed and state is
 * updated but nothing is delivered.
 *
 * **Across a language boundary, use the queue instead.** This one takes a
 * function pointer, so a bound language pays an upcall for every mouse
 * movement. schultz_events_set_queue and schultz_events_drain deliver the
 * same events as one downcall a frame. Nothing is lost by choosing the
 * queue: it records everything a callback would have been told.
 *
 * @param events       The routing state. Must not be NULL.
 * @param callback     The host's entry point, or NULL to stop delivery.
 * @param host_context Passed back to callback unchanged. May be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when events is NULL.
 */
int32_t schultz_events_set_callback(schultz_events *events,
                                    schultz_event_callback_fn callback,
                                    void *host_context);

/* ---------------------------------------------------------- hit testing */

/**
 * @brief Finds the deepest node under a point.
 *
 * Overlays are searched first, topmost down, then the content tree. Within a
 * subtree the last child is tried first, because later children paint on top.
 * Nodes that are not visible are skipped entirely, along with their subtrees.
 *
 * A disabled node is still returned: it blocks what is behind it rather than
 * letting clicks fall through, which is what a user expects from a greyed out
 * control.
 *
 * @param events   The routing state. Must not be NULL.
 * @param point    The position in window coordinates.
 * @param out_node Receives the node, or SCHULTZ_HANDLE_NONE when the point
 *                 hits nothing. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_events_hit_test(schultz_events *events, schultz_point point,
                                schultz_handle *out_node);

/* --------------------------------------------------------------- input */

/**
 * @brief Routes a pointer motion.
 *
 * Updates the hovered node, emitting leave and enter as it changes, and sets
 * or clears SCHULTZ_STATE_HOVERED, which marks the affected nodes dirty.
 * While a button is held, motion goes to the captured node as a drag instead.
 *
 * @param events    The routing state. Must not be NULL.
 * @param point     The new position in window coordinates.
 * @param modifiers Bit set of SCHULTZ_MOD_* values.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_events_mouse_move(schultz_events *events, schultz_point point,
                                  uint32_t modifiers);

/**
 * @brief Routes a mouse button press or release.
 *
 * A press captures the node under the pointer, so the matching release goes
 * to it wherever the pointer has travelled. A release over the same node also
 * produces a click. A press outside a capturing overlay dismisses that
 * overlay and is consumed rather than reaching what is underneath.
 *
 * A press also decides where focus is. On a focusable node it moves there; on
 * anything else, empty space or a label or a plain container, focus goes
 * away. That is what closes an on-screen keyboard, which follows focus and is
 * managed nowhere else, and it is what a desktop does too: click the empty
 * canvas and the caret leaves the field. The secondary button is the
 * exception, because it asks for a context menu rather than pressing what is
 * under it.
 *
 * A finger has to have stayed roughly still as well, or the release is not a
 * click. Content that scrolls with a finger carries the row it started on
 * along underneath it, so that row is still what the release lands on, and
 * without this every scroll would also press whatever it began on. A mouse is
 * unaffected: pressing a large button, moving the cursor inside it and
 * letting go is a click, and a cursor does not scroll by dragging.
 *
 * @param events    The routing state. Must not be NULL.
 * @param point     The position in window coordinates.
 * @param button    One of the SCHULTZ_BUTTON_* values.
 * @param down      Nonzero for a press, zero for a release.
 * @param modifiers Bit set of SCHULTZ_MOD_* values.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_events_mouse_button(schultz_events *events,
                                    schultz_point point, uint32_t button,
                                    int32_t down, uint32_t modifiers);

/**
 * @brief Routes a scroll to the node under the pointer.
 *
 * @param events   The routing state. Must not be NULL.
 * @param point    The position in window coordinates.
 * @param scroll_x Horizontal amount, positive is right.
 * @param scroll_y Vertical amount, positive is down.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_events_scroll(schultz_events *events, schultz_point point,
                              float scroll_x, float scroll_y);

/**
 * @brief Routes a key press or release to the focused node.
 *
 * Does nothing when nothing has focus.
 *
 * @param events    The routing state. Must not be NULL.
 * @param key       One of the SCHULTZ_KEY_* values.
 * @param modifiers Bit set of SCHULTZ_MOD_* values.
 * @param down      Nonzero for a press, zero for a release.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_events_key(schultz_events *events, uint32_t key,
                           uint32_t modifiers, int32_t down);

/**
 * @brief Routes committed text to the focused node.
 *
 * This is what an IME produces when a composition is accepted, and what a
 * plain keystroke produces directly.
 *
 * @param events The routing state. Must not be NULL.
 * @param utf8   The text, NUL terminated. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_events_text_input(schultz_events *events, const char *utf8);

/**
 * @brief Routes an in progress IME composition to the focused node.
 *
 * The text is provisional and will be replaced, either by a further editing
 * event or by a committed text input event. A text field is expected to show
 * it inline, usually underlined.
 *
 * @param events The routing state. Must not be NULL.
 * @param utf8   The composition so far, NUL terminated. Must not be NULL.
 * @param cursor Cursor position within the composition, in bytes.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_events_text_editing(schultz_events *events, const char *utf8,
                                    int32_t cursor);

/* --------------------------------------------------------------- focus */

/**
 * @brief Moves keyboard focus to a node.
 *
 * Emits focus lost on the previous holder and focus gained on the new one,
 * and maintains SCHULTZ_STATE_FOCUSED, which marks both dirty. Passing
 * SCHULTZ_HANDLE_NONE clears focus.
 *
 * @param events The routing state. Must not be NULL.
 * @param node   The node to focus, or SCHULTZ_HANDLE_NONE.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE when the node is not live.
 */
int32_t schultz_events_set_focus(schultz_events *events, schultz_handle node);

/**
 * @brief Tells the router what time it is, in milliseconds.
 *
 * Nothing in the toolkit knows the time, and telling one press from two needs
 * it. A host calls this before handing over each event, with that event's own
 * timestamp when the platform provides one.
 *
 * Without it every press counts as a single click, which is what a host that
 * does not care about double clicks wants anyway.
 *
 * @param events The routing state. Must not be NULL.
 * @param now    Milliseconds from any fixed point, so long as it only rises.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_events_set_time(schultz_events *events, uint64_t now);

/**
 * @brief Hands an event to the host without routing it through the tree.
 *
 * Everything the router raises belongs to a node and rises through the tree
 * on its way out. Some things do not: a file dialog comes back long after the
 * click that opened it, and belongs to no node at all. Those are posted here
 * instead, and reach the host exactly as any other event does, through the
 * queue and through the callback.
 *
 * No widget sees it, because there is no target to give it to.
 *
 * @param tree_event The event to hand over. Must not be NULL.
 * @param events     The routing state. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or whatever the host's
 *         callback returned.
 */
int32_t schultz_events_post(schultz_events *events,
                            const schultz_event *tree_event);

/**
 * @brief Starts recording events into a queue the host reads in one go.
 *
 * A callback is the right shape for a host written in C and the wrong shape
 * for one across a language boundary, where it becomes an upcall for every
 * mouse movement. A queue turns that into one downcall a frame: hand the
 * platform's events to the router as usual, then drain once and walk the
 * array.
 *
 * The queue records what a host would have been told and never consumes
 * anything, so a widget still sees every event first and the host's callback,
 * if one is installed, still runs. It grows as needed and is reused, so a host
 * that drains every frame stops allocating after the first few.
 *
 * @param events The routing state. Must not be NULL.
 * @param on     Nonzero to record, zero to stop and release the queue.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_events_set_queue(schultz_events *events, int32_t on);

/**
 * @brief Hands over everything recorded since the last drain.
 *
 * No copying: the array is the queue's own storage. It stays valid until the
 * next event is routed, which in a frame loop means until the next frame, so
 * a host drains and walks the array before touching the router again.
 *
 * @param events     The routing state. Must not be NULL.
 * @param out_events Receives the array, or NULL when nothing was recorded.
 *                   Must not be NULL.
 * @param out_count  Receives how many events it holds. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_events_drain(schultz_events *events,
                             const schultz_event **out_events,
                             uint32_t *out_count);

/**
 * @brief Counts what the queue is holding without taking it.
 *
 * @param events The routing state. NULL yields 0.
 * @return How many events are waiting.
 */
uint32_t schultz_events_pending(const schultz_events *events);

/** @brief The largest number of keyboard shortcuts one router may hold. */
enum {
    SCHULTZ_ACCELERATORS_MAX = 32
};

/**
 * @brief Registers a keyboard shortcut that activates a node.
 *
 * A shortcut fires wherever focus happens to be, so long as whatever holds
 * focus did not want the key itself: a text field typing control with V is
 * not also a paste command aimed at a menu. The node is then clicked exactly
 * as if it had been pressed, so a menu item and a button behave the same way
 * whether they were reached by hand or by shortcut.
 *
 * Registering the same key and modifiers again replaces what was there.
 *
 * @param events    The routing state. Must not be NULL.
 * @param key       One of the SCHULTZ_KEY_* values, or a character's
 *                  codepoint.
 * @param modifiers The modifiers that must be held, as SCHULTZ_MOD_* values.
 * @param node      The node to activate, or SCHULTZ_HANDLE_NONE to remove
 *                  the shortcut.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or SCHULTZ_ERR_EXHAUSTED
 *         when there is no room for another.
 */
int32_t schultz_events_add_accelerator(schultz_events *events, uint32_t key,
                                       uint32_t modifiers,
                                       schultz_handle node);

/**
 * @brief Returns the node holding keyboard focus.
 *
 * @param events The routing state. NULL yields SCHULTZ_HANDLE_NONE.
 * @return The focused node, or SCHULTZ_HANDLE_NONE.
 */
schultz_handle schultz_events_focus(const schultz_events *events);

/**
 * @brief Moves focus to the next or previous focusable node.
 *
 * Focus order is tree order: a depth first walk, parents before children,
 * earlier siblings first. A node is focusable when it is effectively visible,
 * enabled, and declares SCHULTZ_ACTION_FOCUS, which reuses the accessibility
 * schema rather than inventing a second notion of focusability.
 *
 * The walk wraps around, so tabbing from the last focusable node reaches the
 * first.
 *
 * @param events  The routing state. Must not be NULL.
 * @param forward Nonzero to move forward, zero to move backward.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or SCHULTZ_ERR_EXHAUSTED
 *         when nothing in the tree is focusable.
 */
int32_t schultz_events_focus_move(schultz_events *events, int32_t forward);

/**
 * @brief Returns the tree this router is bound to.
 *
 * Lets the platform layer ask about the focused node without being handed the
 * tree separately.
 *
 * @param events The routing state. NULL yields NULL.
 * @return The tree, which the router does not own.
 */
schultz_tree *schultz_events_tree(const schultz_events *events);

/**
 * @brief Returns the node the pointer is currently over.
 *
 * @param events The routing state. NULL yields SCHULTZ_HANDLE_NONE.
 * @return The hovered node, or SCHULTZ_HANDLE_NONE.
 */
schultz_handle schultz_events_hovered(const schultz_events *events);

/**
 * @brief Says what is driving the pointer from here on.
 *
 * The platform layer sets this as it translates each event, before handing it
 * to the router, and every event the router sends afterwards carries it. It
 * stays as set until it is changed, because a run of events from one finger
 * all come from the same source.
 *
 * @param events The routing state. Must not be NULL.
 * @param source SCHULTZ_POINTER_MOUSE or SCHULTZ_POINTER_TOUCH.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_events_set_pointer_source(schultz_events *events,
                                          uint32_t source);

/**
 * @brief Says that a delete key removes one codepoint rather than a whole
 *        character.
 *
 * For a platform that has no keys to report and works out what changed
 * instead, sending one backspace for every codepoint it removed. See
 * `one_codepoint` on schultz_event for what goes wrong without this.
 *
 * A host that reports real key presses never calls this. One that translates
 * them from a platform text field sets it while that is where the keys are
 * coming from, and clears it if a real keyboard is attached.
 *
 * @param events The event system. Must not be NULL.
 * @param on     Nonzero to mark delete keys from here on.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_events_set_delete_by_codepoint(schultz_events *events,
                                               int32_t on);

/**
 * @brief Carries out one of a node's declared actions.
 *
 * The way something other than a hand asks a widget to act. An assistive
 * technology telling a screen reader user's button to press arrives here, and
 * is turned into the same event a real press produces, so that what a screen
 * reader can do and what a mouse can do cannot drift apart.
 *
 * The action is refused unless the node declares it in its accessibility
 * schema, which is what stops a caller reaching past what a widget offers.
 *
 * Increment and decrement are delivered as a right or left arrow key, because
 * that is the input a range widget already understands. A widget that answers
 * neither will not move.
 *
 * SCHULTZ_ACTION_SET_VALUE is not handled here: a value needs a payload, and
 * this call carries none. SCHULTZ_ACTION_SCROLL is not either, because
 * bringing a node into view belongs to the scroll view that holds it; see
 * schultz_scroll_view_reveal.
 *
 * @param events The routing state. Must not be NULL.
 * @param node   The node to act on. Must name a live node.
 * @param action One of the SCHULTZ_ACTION_* values, exactly one.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when the action is not one
 *         this handles or the node does not declare it, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_events_perform(schultz_events *events, schultz_handle node,
                               uint32_t action);

/**
 * @brief Returns the node holding the pointer capture.
 *
 * Set on press and cleared on release. While it is set, motion is delivered
 * to it as a drag no matter where the pointer goes.
 *
 * @param events The routing state. NULL yields SCHULTZ_HANDLE_NONE.
 * @return The captured node, or SCHULTZ_HANDLE_NONE.
 */
schultz_handle schultz_events_captured(const schultz_events *events);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_EVENT_H */
