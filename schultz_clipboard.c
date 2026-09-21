/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_clipboard.c
 * @brief The clipboard work that is plain bytes rather than a platform.
 */

#include "schultz_clipboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Where the copied part of a document begins and ends. */
#define SCHULTZ_CLIPBOARD_FRAGMENT_START "<!--StartFragment-->"
#define SCHULTZ_CLIPBOARD_FRAGMENT_END   "<!--EndFragment-->"

/*
 * Ten digits for every offset, which is what makes this possible at all.
 *
 * The numbers count from the front of the whole thing, so they cannot be
 * known until the header they sit in has a length. Fixing that length first,
 * by padding every number out to the same width, turns the problem round: the
 * header is laid down, then measured, then filled in.
 */
#define SCHULTZ_CLIPBOARD_DIGITS 10

static char  *schultz_clipboard_held = NULL;
static size_t schultz_clipboard_room = 0u;

int32_t schultz_clipboard_windows_html(const char *html,
                                       const char **out_bytes,
                                       uint64_t *out_length)
{
    /*
     * Version 0.9 rather than 1.0. Both are current; 0.9 is what every
     * version of Windows has understood since the format was introduced, and
     * nothing here uses anything 1.0 added.
     */
    static const char *const shape =
        "Version:0.9\r\n"
        "StartHTML:%0*u\r\n"
        "EndHTML:%0*u\r\n"
        "StartFragment:%0*u\r\n"
        "EndFragment:%0*u\r\n";
    size_t header;
    size_t body;
    size_t total;
    const char *start;
    const char *end;
    unsigned fragment_start;
    unsigned fragment_end;

    if (out_bytes == NULL || out_length == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_bytes  = NULL;
    *out_length = 0u;
    if (html == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    body = strlen(html);

    /* How long the header will be, with every number at its full width. */
    header = (size_t)snprintf(NULL, 0, shape,
                              SCHULTZ_CLIPBOARD_DIGITS, 0u,
                              SCHULTZ_CLIPBOARD_DIGITS, 0u,
                              SCHULTZ_CLIPBOARD_DIGITS, 0u,
                              SCHULTZ_CLIPBOARD_DIGITS, 0u);
    total = header + body;

    /*
     * Where the copied part sits, from the comments the markup carries. The
     * offsets point at the start of the opening comment and just past the end
     * of the closing one, which is what a reader expects to find between
     * them.
     */
    start = strstr(html, SCHULTZ_CLIPBOARD_FRAGMENT_START);
    end   = strstr(html, SCHULTZ_CLIPBOARD_FRAGMENT_END);
    if (start != NULL && end != NULL && end > start) {
        fragment_start = (unsigned)(header + (size_t)(start - html) +
                                    strlen(SCHULTZ_CLIPBOARD_FRAGMENT_START));
        fragment_end   = (unsigned)(header + (size_t)(end - html));
    } else {
        /* No markers, so the whole document is the fragment. */
        fragment_start = (unsigned)header;
        fragment_end   = (unsigned)total;
    }

    if (total + 1u > schultz_clipboard_room) {
        char *bigger = (char *)realloc(schultz_clipboard_held, total + 1u);

        if (bigger == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        schultz_clipboard_held = bigger;
        schultz_clipboard_room = total + 1u;
    }
    /*
     * The header first, at exactly the length it was measured at, then the
     * markup after it. snprintf writes its own terminator over the first byte
     * of the markup, so the markup is copied in afterwards rather than before.
     */
    snprintf(schultz_clipboard_held, header + 1u, shape,
             SCHULTZ_CLIPBOARD_DIGITS, (unsigned)header,
             SCHULTZ_CLIPBOARD_DIGITS, (unsigned)total,
             SCHULTZ_CLIPBOARD_DIGITS, fragment_start,
             SCHULTZ_CLIPBOARD_DIGITS, fragment_end);
    memcpy(schultz_clipboard_held + header, html, body);
    schultz_clipboard_held[total] = '\0';

    *out_bytes  = schultz_clipboard_held;
    *out_length = (uint64_t)total;
    return SCHULTZ_OK;
}
