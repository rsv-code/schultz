/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_clipboard.h
 * @brief Clipboard shapes that are not one platform's own.
 *
 * Not part of the host facing interface. Most of what a clipboard needs is
 * the platform's business, and each one has its own file. What is here is the part that is plain byte work: building
 * the wrapper Windows puts round markup, which is fiddly enough to be worth
 * testing and does not need Windows to test.
 */

#ifndef SCHULTZ_CLIPBOARD_H
#define SCHULTZ_CLIPBOARD_H

#include "schultz.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wraps markup the way the Windows clipboard wants it.
 *
 * Windows carries rich text in a format of its own, registered under the name
 * "HTML Format". It is the markup with a header in front of it, and the header
 * is a short list of byte offsets saying where the document starts and ends
 * and which part of it was actually copied.
 *
 * The awkward part, and the reason this is worth its own function: **the
 * offsets count from the first byte of the result, header included**, so how
 * long the header is depends on the numbers and the numbers depend on how
 * long the header is. The way out is the one the format's own specification
 * gives: write each number in a fixed width padded with leading zeros, so the
 * header's length is known before any of the numbers are.
 *
 * Which part was copied is found from the comments the markup already
 * carries, `<!--StartFragment-->` and `<!--EndFragment-->`. Markup without
 * them is wrapped whole, with the fragment taken as the entire document.
 *
 * @param html       The markup, NUL terminated. Must not be NULL.
 * @param out_bytes  Receives the wrapped bytes, NUL terminated. Must not be
 *                   NULL. Owned by the toolkit and valid until the next call.
 * @param out_length Receives how many bytes, not counting the terminator.
 *                   Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_clipboard_windows_html(const char *html,
                                       const char **out_bytes,
                                       uint64_t *out_length);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_CLIPBOARD_H */
