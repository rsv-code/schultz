/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_files_backend.h
 * @brief The one part of a file dialog that differs by platform.
 *
 * Asking for a file is the same everywhere: schultz_window.c takes a request
 * number, copies what the caller asked for, and turns the answer into an
 * event. Showing the dialog is not the same everywhere.
 *
 * On Linux, Windows, macOS and Android, SDL already has a backend, and this
 * seam is one call through to it. On iOS it does not: SDL 3.4.16 ships
 * dialog backends for android, cocoa, haiku, unix and windows, its build
 * selects the cocoa one with `elseif(MACOS)`, and iOS matches no branch at
 * all. What gets compiled instead is the dummy, which is three lines that
 * report failure without showing anything. That is why a file picker on iOS
 * looked like a dialog the person dismissed the instant it opened.
 *
 * So iOS answers here, with UIDocumentPickerViewController, and everything
 * above this line stays the same on all six platforms.
 *
 * **Not part of the host facing interface.** A host binds to schultz_api.h.
 */

#ifndef SCHULTZ_FILES_BACKEND_H
#define SCHULTZ_FILES_BACKEND_H

#include <SDL3/SDL.h>

#include "schultz.h"
#include "schultz_window.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Which of the three dialogs to show. */
typedef enum {
    SCHULTZ_FILES_OPEN,   /**< Choose one or more existing files. */
    SCHULTZ_FILES_SAVE,   /**< Choose where to write a new one. */
    SCHULTZ_FILES_FOLDER  /**< Choose a directory. */
} schultz_files_kind;

/**
 * @brief Where a backend delivers what the person chose.
 *
 * Called exactly once for every schultz_files_show, however it ended, and
 * possibly on a thread that is not the one the window loop runs on.
 *
 * @param userdata The pointer given to schultz_files_show.
 * @param paths    The chosen paths, NULL terminated, or NULL for none. An
 *                 empty list and NULL both mean nothing was chosen; the
 *                 caller treats them alike.
 * @param filter   Which filter was showing, or -1 when the platform did not
 *                 say.
 */
typedef void (*schultz_files_answer_fn)(void *userdata,
                                        const char *const *paths,
                                        int32_t filter);

/**
 * @brief Shows one file dialog and returns without waiting for an answer.
 *
 * The filters are the caller's copies and stay alive until after the answer
 * arrives, so a backend may point at their strings rather than copying them
 * again.
 *
 * @param kind         Which dialog to show.
 * @param window       The window to hang it off. May be NULL.
 * @param options      What the caller asked for, or NULL for every default.
 * @param filters      Copied filters, or NULL for none.
 * @param filter_count How many filters there are.
 * @param answer       Called once, later, with the result. Must not be NULL.
 * @param userdata     Passed back to the callback untouched.
 */
void schultz_files_show(schultz_files_kind kind, SDL_Window *window,
                        const schultz_file_options *options,
                        const schultz_file_filter *filters,
                        uint32_t filter_count,
                        schultz_files_answer_fn answer, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_FILES_BACKEND_H */
