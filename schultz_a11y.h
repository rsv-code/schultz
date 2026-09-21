/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_a11y.h
 * @brief The bridge from the widget tree to a screen reader.
 *
 * **Not part of the host facing interface.** A host binds to schultz_api.h;
 * accessibility is switched on through schultz_window_options and needs no
 * calls of its own. This header is the toolkit's own.
 *
 * A toolkit draws pixels. A screen reader cannot see pixels, so something has
 * to say in words what is on screen: this is a button, it says "Save", it has
 * focus now. Every platform has its own API for that and no two agree, so
 * Schultz describes its tree once, here, and AccessTunnel presents that
 * description to whichever platform it is running on.
 *
 * Nothing above this file sees an AccessTunnel type. The node schema lives on
 * Schultz's own nodes, as role, name, value, state and actions, and is
 * translated here.
 *
 * ## What a caller has to do
 *
 * Three things, once a frame, and the frame driver already does all three:
 *
 *   - schultz_a11y_update pushes the tree when something changed.
 *   - schultz_a11y_pump answers whatever the platform is asking.
 *   - schultz_a11y_drain turns what it asked for into real input.
 *
 * Forgetting the first is the failure that matters, and it is silent: the
 * application works, and a screen reader describes a window that stopped
 * being true several clicks ago.
 *
 * ## Threading
 *
 * A platform accessibility API calls in on threads the toolkit did not
 * create. Requests are queued here under a lock and drained on the thread
 * that runs the frame, so no widget is ever touched from a thread it does not
 * expect, and the live tree is never walked from a foreign one.
 */

#ifndef SCHULTZ_A11Y_H
#define SCHULTZ_A11Y_H

#include "schultz_event.h"
#include "schultz_node.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque bridge state. Create with schultz_a11y_create. */
typedef struct schultz_a11y schultz_a11y;

/** @brief What to tell the platform about the application. */
typedef struct {
    /** The application's name, as a user would recognize it. NULL is "". */
    const char *app_name;
    /** Where the window is on screen, in pixels. */
    schultz_rect window;
    /** Nonzero when the window has focus at creation. */
    int32_t      focused;
    /**
     * The platform's own window, as SDL_Window *, or NULL.
     *
     * Accessibility on Windows, macOS, iOS and Android attaches to a native
     * window or view rather than announcing itself the way Linux does, and
     * this is the only way to reach one from here. Linux ignores it.
     */
    void        *native_window;
} schultz_a11y_options;

/**
 * @brief Starts the bridge and publishes the tree for the first time.
 *
 * Succeeds whether or not an assistive technology is running: on Linux there
 * may be no accessibility bus, and the right behaviour then is to sit quietly
 * rather than to fail an application's startup.
 *
 * @param tree      The tree to describe. Must not be NULL.
 * @param events    The router, used to deliver what is asked for and to read
 *                  which node has focus. Must not be NULL.
 * @param options   Application details, or NULL for defaults.
 * @param out_a11y  Receives the bridge. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_a11y_create(schultz_tree *tree, schultz_events *events,
                            const schultz_a11y_options *options,
                            schultz_a11y **out_a11y);

/**
 * @brief Stops the bridge and leaves the platform.
 *
 * @param a11y The bridge. NULL is accepted and does nothing.
 */
void schultz_a11y_destroy(schultz_a11y *a11y);

/**
 * @brief Publishes the tree, when anything a reader could notice changed.
 *
 * Cheap to call every frame: it does nothing when the tree is clean and the
 * focus has not moved.
 *
 * @param a11y The bridge. NULL is accepted and does nothing.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_a11y_update(schultz_a11y *a11y);

/**
 * @brief Answers whatever the platform has waiting, without blocking.
 *
 * @param a11y The bridge. NULL is accepted and does nothing.
 * @return SCHULTZ_OK.
 */
int32_t schultz_a11y_pump(schultz_a11y *a11y);

/**
 * @brief Carries out what an assistive technology asked for.
 *
 * Runs on the calling thread, which is the frame's, and turns each queued
 * request into the same event a hand would have produced.
 *
 * @param a11y The bridge. NULL is accepted and does nothing.
 * @return How many requests were carried out.
 */
uint32_t schultz_a11y_drain(schultz_a11y *a11y);

/**
 * @brief Tells the platform where the window is, after it moved or resized.
 *
 * @param a11y   The bridge. NULL is accepted and does nothing.
 * @param window The window's rectangle on screen, in pixels.
 * @return SCHULTZ_OK.
 */
int32_t schultz_a11y_set_window(schultz_a11y *a11y, schultz_rect window);

/**
 * @brief Tells the platform whether the window has focus.
 *
 * A screen reader needs this to know whether what it is reading is what the
 * user is looking at.
 *
 * @param a11y    The bridge. NULL is accepted and does nothing.
 * @param focused Nonzero when the window has focus.
 * @return SCHULTZ_OK.
 */
int32_t schultz_a11y_set_focused(schultz_a11y *a11y, int32_t focused);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_A11Y_H */
