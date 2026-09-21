/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_a11y_backend.h
 * @brief The one part of accessibility that differs by platform.
 *
 * Describing a tree is the same everywhere: schultz_a11y.c walks the widgets
 * and builds an access_tunnel_tree_update, and that code compiles anywhere.
 * Handing the update to the platform is not the same everywhere. Linux
 * publishes over D-Bus and has to be pumped; Windows answers a window
 * message; macOS and iOS answer methods on a view; Android answers through
 * Java. Those five have different lifetimes, different attachment points and
 * different ideas of what focus means.
 *
 * So the platform half is behind these six functions. Exactly one file
 * implements them per build, chosen in the Makefile from the target name,
 * and a do nothing implementation covers a target that is not wired up yet.
 *
 * **Not part of the host facing interface.** A host binds to schultz_api.h.
 */

#ifndef SCHULTZ_A11Y_BACKEND_H
#define SCHULTZ_A11Y_BACKEND_H

#include "access_tunnel.h"
#include "access_tunnel_tree_update.h"
#include "schultz.h"
#include "schultz_geom.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque platform state. Created by schultz_a11y_backend_create. */
typedef struct schultz_a11y_backend schultz_a11y_backend;

/**
 * @brief Where a backend delivers what an assistive technology asked for.
 *
 * Every adapter in AccessTunnel uses this shape, so the bridge's own handler
 * installs into any of them without translation. Called on whatever thread
 * the platform chose, which is why the bridge queues rather than acts.
 */
typedef void (*schultz_a11y_backend_action_fn)(
    const access_tunnel_action_request *request, void *userdata);

/**
 * @brief Asks the bridge to build a complete tree, right now.
 *
 * iOS is pull rather than push: an adapter sits asleep until VoiceOver
 * starts, and then asks for the whole tree at once. No other platform needs
 * this, and the ones that do not simply never call it.
 *
 * @param out_update Receives a complete update the caller must free.
 * @param userdata   The pointer given at creation.
 * @return true when an update was produced.
 */
typedef bool (*schultz_a11y_backend_build_fn)(
    access_tunnel_tree_update *out_update, void *userdata);

/** @brief What a backend needs to attach itself to the platform. */
typedef struct {
    /** The application's name, as a user would recognize it. Never NULL. */
    const char *app_name;
    /** The toolkit's name, for platforms that publish it. */
    const char *toolkit_name;
    /** The toolkit's version, for platforms that publish it. */
    const char *toolkit_version;
    /** Where to deliver action requests. */
    schultz_a11y_backend_action_fn action;
    /** Passed to the action callback. */
    void *action_userdata;
    /** How to ask for a whole tree. Used by pull based platforms only. */
    schultz_a11y_backend_build_fn build;
    /** Passed to the build callback. */
    void *build_userdata;
    /** Nonzero when the window has focus at creation. */
    int32_t focused;
    /** Where the window is on screen, in pixels. */
    schultz_rect window;
    /**
     * The platform's own window, as SDL_Window *, or NULL.
     *
     * Every backend but the Linux one needs it: accessibility on Windows,
     * macOS, iOS and Android attaches to a native window or view, and this
     * is the only way to reach one from here.
     */
    void *native_window;
} schultz_a11y_backend_config;

/**
 * @brief Attaches to the platform and publishes the first tree.
 *
 * Must succeed whether or not an assistive technology is running. There may
 * be no accessibility bus, no screen reader and nothing listening, and the
 * right behaviour then is to sit quietly rather than to fail startup.
 *
 * @param config      How to attach. Must not be NULL.
 * @param initial     The first complete tree. Must not be NULL. Borrowed:
 *                    the caller frees it afterwards.
 * @param out_backend Receives the backend. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_a11y_backend_create(
    const schultz_a11y_backend_config *config,
    const access_tunnel_tree_update *initial,
    schultz_a11y_backend **out_backend);

/**
 * @brief Detaches from the platform.
 *
 * @param backend The backend. NULL is accepted and does nothing.
 */
void schultz_a11y_backend_destroy(schultz_a11y_backend *backend);

/**
 * @brief Hands the platform a new tree.
 *
 * @param backend The backend. NULL is accepted and does nothing.
 * @param update  The update. Borrowed.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_a11y_backend_update(schultz_a11y_backend *backend,
                                    const access_tunnel_tree_update *update);

/**
 * @brief Gives the platform a moment to talk, on platforms that need one.
 *
 * Linux accessibility is a D-Bus connection the application owns, so
 * somebody has to read it; the frame driver calls this every frame. Nothing
 * else works that way, and everywhere else this does nothing.
 *
 * @param backend The backend. NULL is accepted and does nothing.
 * @return SCHULTZ_OK.
 */
int32_t schultz_a11y_backend_pump(schultz_a11y_backend *backend);

/**
 * @brief Tells the platform where the window is.
 *
 * Only platforms that publish a window in their own right need this. Where
 * accessibility hangs off a view, the view already knows where it is, and
 * this does nothing.
 *
 * @param backend The backend. NULL is accepted and does nothing.
 * @param window  The window rectangle in screen pixels.
 * @return SCHULTZ_OK.
 */
int32_t schultz_a11y_backend_set_window(schultz_a11y_backend *backend,
                                        schultz_rect window);

/**
 * @brief Tells the platform whether the window has focus.
 *
 * This is the outbound half of focus: whether the application is the one the
 * user is typing into. It is not where a screen reader's cursor is, which
 * travels the other way and arrives as an action request.
 *
 * @param backend The backend. NULL is accepted and does nothing.
 * @param focused Nonzero when the window has focus.
 * @return SCHULTZ_OK.
 */
int32_t schultz_a11y_backend_set_focused(schultz_a11y_backend *backend,
                                         int32_t focused);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_A11Y_BACKEND_H */
