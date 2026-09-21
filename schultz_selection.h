/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_selection.h
 * @brief Selecting text across more than one widget.
 *
 * A selection normally belongs to one widget: press in a paragraph, drag into
 * the next, and only the first is selected. A selection area changes that for
 * the subtree underneath it. A drag inside the area runs from wherever it
 * started to wherever it is now, across as many widgets as it passes over,
 * and a copy takes the lot in reading order.
 *
 * It is opt in. A host wraps the part of its tree where selecting is the
 * right meaning for a drag, and everywhere else behaves exactly as before.
 * That matters because a drag usually already means something -- scroll,
 * reorder, move a card -- and only the host knows which.
 *
 * Areas nest. An area inside another keeps its own selection: the outer one
 * cannot reach into it and it cannot reach out. That is how a text field
 * inside a selectable page behaves correctly.
 */

#ifndef SCHULTZ_SELECTION_H
#define SCHULTZ_SELECTION_H

#include "schultz_render.h"
#include "schultz_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif

/**
 * @brief Makes a selection area.
 *
 * The node itself draws nothing and takes no room of its own. Give it a pane
 * and children like any other container.
 *
 * @param tree     The tree to build in. Must not be NULL.
 * @param parent   The node to hang it from.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_selection_area_create(schultz_tree *tree,
                                      schultz_handle parent,
                                      schultz_handle *out_node);

/**
 * @brief Sets where the selection starts and where it now ends.
 *
 * The two ends each name a node and a byte offset inside it. They may be the
 * same node, and they may be given in either order: dragging backwards is an
 * ordinary thing to do, so the area works out which comes first in reading
 * order rather than asking the caller to.
 *
 * Every selectable node in the area is then told its share, which is all of
 * its text, part of it, or none.
 *
 * @param tree     The tree holding the area. Must not be NULL.
 * @param area     A node from schultz_selection_area_create.
 * @param from     The node the selection started in.
 * @param from_at  The byte offset it started at.
 * @param to       The node it now ends in.
 * @param to_at    The byte offset it now ends at.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the area is not one
 *         or when either end is not inside it.
 */
int32_t schultz_selection_area_set_range(schultz_tree *tree,
                                         schultz_handle area,
                                         schultz_handle from, uint32_t from_at,
                                         schultz_handle to, uint32_t to_at);

/**
 * @brief Where the selection starts and ends, in reading order.
 *
 * Named for the question rather than as the mirror of the setter, because
 * what comes back is sorted and what went in need not have been.
 *
 * @param tree       The tree holding the area. NULL yields an error.
 * @param area       A node from schultz_selection_area_create.
 * @param out_from   Receives the earlier node. May be NULL.
 * @param out_from_at Receives its byte offset. May be NULL.
 * @param out_to     Receives the later node. May be NULL.
 * @param out_to_at  Receives its byte offset. May be NULL.
 * @return SCHULTZ_OK when there is a selection, SCHULTZ_ERR_UNAVAILABLE when
 *         there is none, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_selection_area_ends(const schultz_tree *tree,
                                    schultz_handle area,
                                    schultz_handle *out_from,
                                    uint32_t *out_from_at,
                                    schultz_handle *out_to,
                                    uint32_t *out_to_at);

/**
 * @brief The nearest selection area above a node, or none.
 *
 * A widget asks this to find out whether it owns its own selection or takes
 * part in somebody else's. A label inside an area lets a press go past it so
 * that the area can run the drag; the same label on its own handles the press
 * itself, as it always did.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node The node to look above.
 * @return The nearest ancestor area, or SCHULTZ_HANDLE_NONE when there is
 *         none. A node that is itself an area is not its own area.
 */
schultz_handle schultz_selection_area_of(const schultz_tree *tree,
                                         schultz_handle node);

/**
 * @brief Puts the selected text on the clipboard.
 *
 * The pieces in reading order, with a line break between the text of one
 * widget and the next, because two paragraphs pasted together with nothing
 * between them read as one.
 *
 * When the selection holds a picture and render options are given, the
 * picture is offered beside the text as image/png and image/bmp, built only
 * if something asks. Pass NULL for the options to offer text alone.
 *
 * @param tree The tree holding the area. Must not be NULL.
 * @param area A node from schultz_selection_area_create.
 * @param options How to render a selected picture, or NULL for text alone.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, SCHULTZ_ERR_UNAVAILABLE
 *         when nothing is selected, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_selection_area_copy(schultz_tree *tree, schultz_handle area,
                                    const schultz_render_options *options);

/**
 * @brief The selected text, as one string.
 *
 * What a copy would put on the clipboard, for a host that wants it without
 * involving the clipboard at all.
 *
 * @param tree The tree holding the area. NULL yields NULL.
 * @param area A node from schultz_selection_area_create.
 * @return The text, NUL terminated, or NULL when nothing is selected. Owned
 *         by the area and valid until the selection changes.
 */
const char *schultz_selection_area_text(schultz_tree *tree,
                                        schultz_handle area);

/**
 * @brief The most bytes a copy out of this area may produce.
 *
 * Bytes rather than characters, because once a picture can be in a selection
 * a character count stops describing what producing it costs. A page of text
 * is kilobytes; one screenshot is megabytes.
 *
 * The limit is on producing a result, not on what may be highlighted.
 * Highlighting a long transcript is a walk; building the string, or the
 * picture beside it, is the part that can hurt.
 *
 * Twenty megabytes by default, which text never comes near: that is around
 * twenty million characters, or a hundred thousand chat messages.
 *
 * @param tree  The tree holding the area. Must not be NULL.
 * @param area  A node from schultz_selection_area_create.
 * @param bytes The limit. Zero means no limit at all.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_selection_area_set_limit(schultz_tree *tree,
                                         schultz_handle area, uint64_t bytes);

/**
 * @brief The limit a copy out of this area stops at.
 *
 * @param tree The tree holding the area. NULL yields 0.
 * @param area A node from schultz_selection_area_create.
 * @return The limit in bytes, or 0 when there is none.
 */
uint64_t schultz_selection_area_limit(const schultz_tree *tree,
                                      schultz_handle area);

/**
 * @brief Whether the last copy stopped early because of the limit.
 *
 * Named for the question, because the answer is about what happened rather
 * than about what was set. A host that gets a yes here has something to say
 * to the person: what they copied is not all of what they selected.
 *
 * @param tree The tree holding the area. NULL yields 0.
 * @param area A node from schultz_selection_area_create.
 * @return Nonzero when the last text or copy was cut short.
 */
int32_t schultz_selection_area_was_cut(const schultz_tree *tree,
                                       schultz_handle area);

/**
 * @brief The selected picture, written out as a file, or nothing.
 *
 * A selection may cover a picture as well as text. Plain text has nowhere to
 * put one, so it is offered separately and produced only when something asks
 * for a format that can hold it.
 *
 * The picture is rendered from the node rather than taken from anywhere,
 * which is what lets a drawn canvas be copied as well as a loaded picture.
 *
 * @param tree       The tree holding the area. Must not be NULL.
 * @param area       A node from schultz_selection_area_create.
 * @param format     SCHULTZ_IMAGE_PNG or SCHULTZ_IMAGE_BMP.
 * @param options    How to render it: the fonts, glyphs, images and
 *                   resources a draw needs. Must not be NULL, because only
 *                   the host has them.
 * @param out_bytes  Receives the file's bytes. Must not be NULL. Owned by the
 *                   toolkit and valid until the next picture is written.
 * @param out_length Receives how many bytes. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_UNAVAILABLE when no picture is selected or
 *         the result would pass the limit, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_selection_area_picture(schultz_tree *tree,
                                       schultz_handle area, uint32_t format,
                                       const schultz_render_options *options,
                                       const void **out_bytes,
                                       uint64_t *out_length);

/**
 * @brief The selected content as HTML, keeping its formatting.
 *
 * What a copy offers beside the plain text, and what a word processor takes
 * when it is given the choice. Each widget's share becomes a paragraph
 * carrying its own size, colour and, when the face is a code face, a
 * monospace family. A picture in the range is written into the document
 * itself, so one format carries the whole selection rather than the text and
 * one picture arriving separately.
 *
 * The styles come from each widget's resolved style rather than from the
 * theme, so a host that set a colour on one paragraph keeps it in the paste.
 *
 * @param tree The tree holding the area. NULL yields NULL.
 * @param area A node from schultz_selection_area_create.
 * @return The markup, NUL terminated, or NULL when nothing is selected. Owned
 *         by the area and valid until the selection changes.
 */
const char *schultz_selection_area_html(schultz_tree *tree,
                                        schultz_handle area);

/**
 * @brief Takes the selection away, leaving nothing selected anywhere in it.
 *
 * @param tree The tree holding the area. Must not be NULL.
 * @param area A node from schultz_selection_area_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_selection_area_clear(schultz_tree *tree, schultz_handle area);

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_SELECTION_H */
