/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_clipboard_windows.h
 * @brief The clipboard, spoken to Windows directly.
 *
 * Installed in place of the SDL one on Windows, because Windows carries rich
 * text in a registered format SDL has no code for. See the source for the
 * whole of the reason.
 *
 * Not part of the host facing interface, the same as the accessibility
 * backends are not: the window installs these and nothing else calls them.
 */

#ifndef SCHULTZ_CLIPBOARD_WINDOWS_H
#define SCHULTZ_CLIPBOARD_WINDOWS_H

#include "schultz_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Puts a set of formats on the Windows clipboard.
 *
 * Matches schultz_clipboard_offer_fn. The context is the window handle from
 * schultz_windows_clipboard_owner, or NULL to let the calling task own the
 * clipboard instead.
 *
 * Formats are written richest first, which is the order Windows asks for:
 * markup, then words, then a picture. That is the opposite of the order they
 * arrive in, where plain text leads.
 *
 * @param context      A window handle, or NULL.
 * @param formats      The media types on offer.
 * @param count        How many.
 * @param make         Produces the bytes of whichever is asked for.
 * @param make_context Passed to make, unchanged.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_UNAVAILABLE when the clipboard would not open.
 */
int32_t schultz_windows_clipboard_offer(void *context,
                                        const char *const *formats,
                                        uint32_t count,
                                        schultz_clipboard_make_fn make,
                                        void *make_context);

/**
 * @brief Takes one format off the Windows clipboard.
 *
 * Matches schultz_clipboard_take_fn.
 *
 * @param context    A window handle, or NULL.
 * @param format     The media type wanted.
 * @param out_length Receives how many bytes.
 * @return The bytes, or NULL when the clipboard holds none in that format.
 *         Owned here and valid until the next call.
 */
const void *schultz_windows_clipboard_take(void *context, const char *format,
                                           uint64_t *out_length);

/**
 * @brief Says whether the Windows clipboard holds a format.
 *
 * Matches schultz_clipboard_holds_fn.
 *
 * @param context Ignored.
 * @param format  The media type to ask about.
 * @return Nonzero when it is there.
 */
int32_t schultz_windows_clipboard_holds(void *context, const char *format);

/**
 * @brief The window handle to own the clipboard with.
 *
 * Windows would rather a window owned the clipboard than the bare task, and
 * the handle lives inside SDL's window.
 *
 * @param window The SDL_Window, as a void pointer.
 * @return The handle, or NULL.
 */
void *schultz_windows_clipboard_owner(void *window);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_CLIPBOARD_WINDOWS_H */
