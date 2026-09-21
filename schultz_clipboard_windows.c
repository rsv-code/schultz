/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_clipboard_windows.c
 * @brief The clipboard, spoken to Windows directly.
 *
 * Everywhere else the toolkit reaches the clipboard through SDL, which takes
 * a media type and hands it to the platform. That works on X11, on Wayland
 * and on macOS, because each of those carries a media type as it is given.
 *
 * Windows does not. Rich text lives in a format registered under the name
 * "HTML Format" rather than in one of the numbered formats SDL knows, and
 * SDL's own rule for deciding what is text is a prefix test: anything
 * beginning with "text" counts, so "text/html" is taken for the clipboard's
 * plain text and the first such type wins. Offering markup through SDL on
 * Windows therefore either loses it or puts a page of tags where a text box
 * expects words.
 *
 * So this file talks to the clipboard itself. It is only Windows, it is
 * installed in place of the SDL one on that platform, and if SDL ever grows
 * the format this can go away.
 *
 * The header the markup needs is built in schultz_clipboard.c, where it can
 * be tested without Windows.
 */

#include "schultz_clipboard_windows.h"

#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include <SDL3/SDL.h>

#include "schultz_clipboard.h"

/*
 * The registered formats, asked for once.
 *
 * Windows hands back the same number to every program that asks for a name,
 * which is how two programs share a format they both understand. Asking again
 * is cheap but pointless, so each is remembered.
 */
static UINT schultz_windows_html_format(void)
{
    static UINT format = 0;

    if (format == 0) {
        format = RegisterClipboardFormatA("HTML Format");
    }
    return format;
}

static UINT schultz_windows_png_format(void)
{
    static UINT format = 0;

    if (format == 0) {
        format = RegisterClipboardFormatA("PNG");
    }
    return format;
}

/* Copies bytes into the movable memory the clipboard takes ownership of. */
static int32_t schultz_windows_put(UINT format, const void *bytes,
                                   size_t length)
{
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, length);
    void *at;

    if (block == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    at = GlobalLock(block);
    if (at == NULL) {
        GlobalFree(block);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    memcpy(at, bytes, length);
    GlobalUnlock(block);
    if (SetClipboardData(format, block) == NULL) {
        /* Still ours, because the clipboard did not take it. */
        GlobalFree(block);
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    return SCHULTZ_OK;
}

/*
 * UTF-8 in, UTF-16 out, which is what the clipboard's text format is.
 *
 * Only this one format is written: Windows makes the older narrow ones from
 * it by itself, so writing them as well would be work for nothing.
 */
static int32_t schultz_windows_put_text(const char *utf8, size_t length)
{
    int wide = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)length, NULL, 0);
    HGLOBAL block;
    wchar_t *at;
    int32_t result = SCHULTZ_OK;

    if (wide <= 0) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    block = GlobalAlloc(GMEM_MOVEABLE, ((size_t)wide + 1u) * sizeof(wchar_t));
    if (block == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    at = (wchar_t *)GlobalLock(block);
    if (at == NULL) {
        GlobalFree(block);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)length, at, wide);
    at[wide] = L'\0';
    GlobalUnlock(block);
    if (SetClipboardData(CF_UNICODETEXT, block) == NULL) {
        GlobalFree(block);
        result = SCHULTZ_ERR_UNAVAILABLE;
    }
    return result;
}

/* Which of the offered formats is the one asked for, or none. */
static const void *schultz_windows_ask(schultz_clipboard_make_fn make,
                                       void *context,
                                       const char *const *formats,
                                       uint32_t count, const char *wanted,
                                       uint64_t *out_length)
{
    uint32_t i;

    *out_length = 0u;
    for (i = 0; i < count; i++) {
        if (strcmp(formats[i], wanted) == 0) {
            return make(context, wanted, out_length);
        }
    }
    return NULL;
}

int32_t schultz_windows_clipboard_offer(void *context,
                                        const char *const *formats,
                                        uint32_t count,
                                        schultz_clipboard_make_fn make,
                                        void *make_context)
{
    const void *bytes;
    uint64_t length = 0u;
    int32_t result = SCHULTZ_OK;
    /*
     * Whether anything at all went on.
     *
     * This window carries markup, plain text and image/png, and nothing else.
     * An offer of only formats it does not know used to empty the clipboard,
     * put nothing back, and report success: the person pressed copy, the
     * clipboard lost what it had, and no error said so. Counted here so the
     * caller is told instead.
     */
    int32_t carried = 0;

    if (formats == NULL || count == 0u || make == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (!OpenClipboard((HWND)context)) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    EmptyClipboard();

    /*
     * Richest first, which is what Windows asks for: a program pasting takes
     * the first format it recognises, and formats are enumerated in the order
     * they were put on. So the markup goes before the words, which is the
     * opposite of the order they are offered in, where plain text has to lead
     * because the layer underneath takes the first text type as the text.
     */
    bytes = schultz_windows_ask(make, make_context, formats, count,
                                SCHULTZ_CLIPBOARD_HTML, &length);
    if (bytes != NULL) {
        const char *wrapped = NULL;
        uint64_t wrapped_length = 0u;

        if (schultz_clipboard_windows_html((const char *)bytes, &wrapped,
                                           &wrapped_length) == SCHULTZ_OK) {
            /*
             * The terminator goes on too. What the clipboard hands back is
             * the block it was given, and a block may be larger than what was
             * put in it, so a reader with no terminator to stop at reads
             * whatever the allocator left behind.
             */
            if (schultz_windows_put(schultz_windows_html_format(), wrapped,
                                    (size_t)wrapped_length + 1u)
                    != SCHULTZ_OK) {
                result = SCHULTZ_ERR_UNAVAILABLE;
            } else {
                carried = 1;
            }
        }
    }

    bytes = schultz_windows_ask(make, make_context, formats, count,
                                SCHULTZ_CLIPBOARD_TEXT, &length);
    if (bytes != NULL) {
        if (schultz_windows_put_text((const char *)bytes, (size_t)length)
                != SCHULTZ_OK) {
            result = SCHULTZ_ERR_UNAVAILABLE;
        } else {
            carried = 1;
        }
    }

    /*
     * A picture, when one is offered at all. It is offered only when the
     * selection is a picture and nothing else, so this is never a stray
     * picture pulled out of a document.
     */
    bytes = schultz_windows_ask(make, make_context, formats, count,
                                "image/png", &length);
    if (bytes != NULL &&
        schultz_windows_put(schultz_windows_png_format(), bytes,
                            (size_t)length) == SCHULTZ_OK) {
        carried = 1;
    }

    CloseClipboard();
    /*
     * Nothing went on, so the clipboard is empty and it is this call that
     * emptied it. Said plainly rather than reported as success, because a
     * caller that offered only formats this window cannot carry has not
     * copied anything and needs to know.
     */
    if (!carried && result == SCHULTZ_OK) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    return result;
}

const void *schultz_windows_clipboard_take(void *context, const char *format,
                                           uint64_t *out_length)
{
    /*
     * Kept until the next call, so the answer can be handed back as a plain
     * pointer. The same contract every other clipboard in the toolkit has.
     */
    static void *held = NULL;
    static size_t room = 0u;
    HANDLE block;
    UINT which;
    int32_t text = 0;

    *out_length = 0u;
    if (format == NULL) {
        return NULL;
    }
    if (strcmp(format, SCHULTZ_CLIPBOARD_TEXT) == 0) {
        which = CF_UNICODETEXT;
        text  = 1;
    } else if (strcmp(format, SCHULTZ_CLIPBOARD_HTML) == 0) {
        which = schultz_windows_html_format();
    } else if (strcmp(format, "image/png") == 0) {
        which = schultz_windows_png_format();
    } else {
        /*
         * A bitmap is offered but not taken back. The clipboard's bitmap
         * format is a file without its first fourteen bytes, so handing it
         * back under a name that means a file would be handing back something
         * that is not one. Nothing in the toolkit reads a bitmap anyway.
         */
        return NULL;
    }
    if (!IsClipboardFormatAvailable(which) || !OpenClipboard((HWND)context)) {
        return NULL;
    }
    block = GetClipboardData(which);
    if (block != NULL) {
        const void *at = GlobalLock(block);

        if (at != NULL) {
            size_t length;

            size_t room_in_block = (size_t)GlobalSize(block);

            if (text) {
                int bytes = WideCharToMultiByte(CP_UTF8, 0,
                                                (const wchar_t *)at, -1,
                                                NULL, 0, NULL, NULL);

                length = (bytes > 0) ? (size_t)bytes - 1u : 0u;
            } else if (which == schultz_windows_html_format()) {
                /*
                 * Markup is written with a terminator, and the block it sits
                 * in may be larger than it. Stopping at the terminator is
                 * what keeps the allocator's leftovers out of the answer.
                 */
                const char *chars = (const char *)at;

                length = 0u;
                while (length < room_in_block && chars[length] != '\0') {
                    length++;
                }
            } else {
                length = room_in_block;
            }
            if (length > 0u && length + 1u > room) {
                void *bigger = realloc(held, length + 1u);

                if (bigger != NULL) {
                    held = bigger;
                    room = length + 1u;
                }
            }
            if (length > 0u && length + 1u <= room) {
                if (text) {
                    WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)at, -1,
                                        (char *)held, (int)room, NULL, NULL);
                } else {
                    memcpy(held, at, length);
                    ((char *)held)[length] = '\0';
                }
                *out_length = (uint64_t)length;
            }
            GlobalUnlock(block);
        }
    }
    CloseClipboard();
    return (*out_length == 0u) ? NULL : held;
}

int32_t schultz_windows_clipboard_holds(void *context, const char *format)
{
    (void)context;
    if (format == NULL) {
        return 0;
    }
    if (strcmp(format, SCHULTZ_CLIPBOARD_TEXT) == 0) {
        return IsClipboardFormatAvailable(CF_UNICODETEXT) ? 1 : 0;
    }
    if (strcmp(format, SCHULTZ_CLIPBOARD_HTML) == 0) {
        return IsClipboardFormatAvailable(schultz_windows_html_format())
                   ? 1 : 0;
    }
    if (strcmp(format, "image/png") == 0) {
        return IsClipboardFormatAvailable(schultz_windows_png_format())
                   ? 1 : 0;
    }
    if (strcmp(format, "image/bmp") == 0) {
        return IsClipboardFormatAvailable(CF_DIB) ? 1 : 0;
    }
    return 0;
}

void *schultz_windows_clipboard_owner(void *window)
{
    SDL_PropertiesID props;

    if (window == NULL) {
        return NULL;
    }
    props = SDL_GetWindowProperties((SDL_Window *)window);
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                  NULL);
}
