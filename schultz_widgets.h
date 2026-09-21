/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_widgets.h
 * @brief The built in widgets.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 *
 * Every constructor creates a node, installs a pane and a widget vtable,
 * allocates whatever state the widget needs, and fills in the accessibility
 * role and actions. Nothing stops an application assembling the same thing by
 * hand: a constructor is the short path, not a privileged one.
 *
 * These widgets name no colours. Everything they draw comes from the node's
 * resolved style, so a theme or a per node override restyles them without
 * their knowing.
 */

#ifndef SCHULTZ_WIDGETS_H
#define SCHULTZ_WIDGETS_H

#include "schultz_widget.h"

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


/**
 * @brief Creates a panel: a background, a border and a corner radius.
 *
 * The plainest widget there is, and the one every container is built from. It
 * paints `background`, then strokes `border.color` at `border.width`, both
 * rounded by `corner.radius`. A fully transparent background paints nothing,
 * which is the default, so a panel is invisible until it is styled.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_panel_create(schultz_tree *tree, schultz_handle parent,
                             schultz_handle *out_node);

/** @brief Which way a widget runs, for the ones that have an axis. */
enum {
    SCHULTZ_ORIENT_HORIZONTAL = 0, /**< Across: wide, and thin vertically. */
    SCHULTZ_ORIENT_VERTICAL        /**< Down: tall, and thin horizontally. */
};

/**
 * @brief Creates a label: static text drawn in `text.color`.
 *
 * The text is copied. Measuring shapes it against the width it is offered,
 * so a label's height depends on its width, which is the case the whole text
 * pipeline and the measure signature were built around. Wrapping is on by
 * default.
 *
 * A label needs the tree's font system, set with
 * schultz_tree_set_font_system. Without one it measures to nothing and draws
 * nothing rather than guessing at a font.
 *
 * The font comes from the resolved `font` property. Note that a loaded font
 * is bound to a pixel size, so `font.size` is currently advisory: it does not
 * re-load the face. That is a known seam.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param text     The text, NUL terminated and copied. NULL means empty.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_label_create(schultz_tree *tree, schultz_handle parent,
                             const char *text, schultz_handle *out_node);

/**
 * @brief Says which stretches of a label's text are not set the way its
 *        style says.
 *
 * A bold phrase inside a sentence, a term in the code face, a word with a
 * background or a line through it. Anything no span covers is drawn the way
 * it was before, so a plain label needs none of this.
 *
 * The list is copied, link strings and all, so the caller may free or reuse
 * whatever it built them from. Spans covering nothing, or bytes past the end
 * of the text, are dropped.
 *
 * Setting the text clears the spans, because a span is byte offsets into the
 * words that were there and means nothing once they change. Set the text
 * first, then the spans.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_label_create.
 * @param spans The spans. May be NULL when count is zero.
 * @param count How many spans.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         label, SCHULTZ_ERR_INVALID_ARGUMENT for NULL spans with a nonzero
 *         count, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_label_set_spans(schultz_tree *tree, schultz_handle node,
                                const schultz_span *spans, uint32_t count);

/**
 * @brief The spans a label is carrying.
 *
 * @param tree      The tree holding the node. Must not be NULL.
 * @param node      A node created by schultz_label_create.
 * @param out_count Receives how many there are, or NULL if not wanted.
 * @return The spans, owned by the label and valid until they are set again
 *         or the text changes, or NULL when there are none.
 */
const schultz_span *schultz_label_spans(const schultz_tree *tree,
                                        schultz_handle node,
                                        uint32_t *out_count);

/**
 * @brief Replaces a label's text.
 *
 * Marks the node for both layout and paint, since the extent may have
 * changed, and updates its accessible name to match.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_label_create.
 * @param text The new text, copied. NULL means empty.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         label, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_label_set_text(schultz_tree *tree, schultz_handle node,
                               const char *text);

/**
 * @brief Returns a label's text.
 *
 * @param tree The tree holding the node. NULL yields NULL.
 * @param node A node created by schultz_label_create.
 * @return The text, owned by the label, or NULL when the node is not a label.
 */
const char *schultz_label_text(const schultz_tree *tree,
                               schultz_handle node);

/**
 * @brief Turns a label's wrapping on or off.
 *
 * With wrapping off the label is one line however narrow it is given, which
 * is what a button caption or a table cell wants.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_label_create.
 * @param wrap Nonzero to wrap at the offered width.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         label.
 */
int32_t schultz_label_set_wrap(schultz_tree *tree, schultz_handle node,
                               int32_t wrap);

/**
 * @brief Turns a label's ellipsis on or off.
 *
 * Off, which is how a label starts, a line too long for the box it is given
 * is drawn anyway and runs past the edge, over whatever is beside it.
 *
 * On, the line is cut where it stops fitting and the cut is marked with an
 * ellipsis, so the reader can see that there is more. Nothing is drawn
 * outside the box either way, and a box too narrow even for the ellipsis
 * draws nothing at all.
 *
 * This is what a caption, a table cell or a list row wants: the text is
 * whatever the application put there, and the width is whatever the layout
 * had left. A paragraph wants wrapping instead.
 *
 * A label still asks for the width its whole text needs, so turning this on
 * never makes a label narrower. It only decides what happens when the layout
 * cannot give it that width.
 *
 * Right to left text is not cut, because its end is the start of the string.
 * It is held inside the box by clipping alone.
 *
 * @param tree      The tree holding the node. Must not be NULL.
 * @param node      A node created by schultz_label_create.
 * @param ellipsize Nonzero to cut and mark, zero to let the text overflow.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         label.
 */
int32_t schultz_label_set_ellipsize(schultz_tree *tree, schultz_handle node,
                                    int32_t ellipsize);

/**
 * @brief Lets a label's text be selected and copied.
 *
 * Off by default, because turning it on everywhere would change what a click
 * on a caption does. A form's labels are not meant to be dragged across; a
 * paragraph of prose is.
 *
 * A selectable label is still not focusable, so it does not join the tab
 * order. Copy reaches it through the tree's selection owner instead; see
 * schultz_tree_set_selection_owner.
 *
 * With a mouse, a press places one end and a drag moves the other, a double
 * click takes a word and a triple click takes the whole text. With a finger,
 * a press held still for a moment takes the word under it and puts a grip on
 * each end to move them with, so that an ordinary drag is left free to mean
 * scroll.
 *
 * Control with C copies the selection and control with A takes all of it.
 *
 * @param tree       The tree holding the node. Must not be NULL.
 * @param node       A node created by schultz_label_create.
 * @param selectable Nonzero to allow selection, zero to disallow it and drop
 *                   whatever was selected.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         label.
 */
int32_t schultz_label_set_selectable(schultz_tree *tree, schultz_handle node,
                                     int32_t selectable);

/**
 * @brief Reports whether a label's text can be selected.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_label_create.
 * @return 1 when the text can be selected, 0 otherwise.
 */
int32_t schultz_label_selectable(const schultz_tree *tree,
                                 schultz_handle node);

/**
 * @brief Selects part of a label's text.
 *
 * Offsets are in bytes into the label's UTF-8 text, and are clamped to its
 * length. Passing the same value twice selects nothing. Selecting anything
 * takes the tree's selection away from whatever held it.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_label_create.
 * @param start One end of the selection, in bytes.
 * @param end   The other end, in bytes.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         label, or SCHULTZ_ERR_INVALID_ARGUMENT when it is not selectable.
 */
int32_t schultz_label_set_selection(schultz_tree *tree, schultz_handle node,
                                    uint32_t start, uint32_t end);

/**
 * @brief Reports what part of a label's text is selected.
 *
 * The two offsets come back in order however the selection was made, so
 * start is never greater than end.
 *
 * @param tree      The tree holding the node.
 * @param node      A node created by schultz_label_create.
 * @param out_start Receives the first selected byte. Must not be NULL.
 * @param out_end   Receives one past the last. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL output or a
 *         node that is not a label, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_label_selection(const schultz_tree *tree, schultz_handle node,
                                uint32_t *out_start, uint32_t *out_end);

/**
 * @brief Puts a label's selected text on the clipboard.
 *
 * What control with C does, exposed so that a menu item can do it too.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_label_create.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         label, or SCHULTZ_ERR_INVALID_ARGUMENT when nothing is selected.
 */
int32_t schultz_label_copy_selection(schultz_tree *tree, schultz_handle node);

/**
 * @brief Creates a separator: a single rule in `border.color`.
 *
 * Measures thin on its own axis and to nothing on the other, so a horizontal
 * separator in a VBox takes a line's worth of height and the full width.
 *
 * @param tree        The tree to create in. Must not be NULL.
 * @param parent      The parent node. Must name a live node.
 * @param orientation SCHULTZ_ORIENT_HORIZONTAL or _VERTICAL.
 * @param out_node    Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_separator_create(schultz_tree *tree, schultz_handle parent,
                                 uint32_t orientation,
                                 schultz_handle *out_node);

/**
 * @brief Creates an ellipse filling the node's bounds.
 *
 * Fills with `background` and strokes with `border.color` at `border.width`.
 * A square node gives a circle; there is no separate circle widget because
 * there is no separate behaviour.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_ellipse_create(schultz_tree *tree, schultz_handle parent,
                               schultz_handle *out_node);

/**
 * @brief Creates a line between two points inside the node's bounds.
 *
 * The points are in the node's own space, so they move with it. Strokes with
 * `border.color` at `border.width`.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param from     One end, relative to the node's origin.
 * @param to       The other end.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_line_create(schultz_tree *tree, schultz_handle parent,
                            schultz_point from, schultz_point to,
                            schultz_handle *out_node);

/**
 * @brief Moves a line's endpoints.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_line_create.
 * @param from One end, relative to the node's origin.
 * @param to   The other end.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         line.
 */
int32_t schultz_line_set_points(schultz_tree *tree, schultz_handle node,
                                schultz_point from, schultz_point to);

/**
 * @brief Creates a filled and stroked polygon.
 *
 * The points are copied and are in the node's own space. Fills with
 * `background` and strokes with `border.color`.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param points   The vertices, in order. Must not be NULL.
 * @param count    How many vertices. Must be at least two.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_polygon_create(schultz_tree *tree, schultz_handle parent,
                               const schultz_point *points, uint32_t count,
                               schultz_handle *out_node);

/**
 * @brief Replaces a path's points.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A node created by schultz_polygon_create.
 * @param points The new vertices. Must not be NULL.
 * @param count  How many. Must be at least two.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         when the node is not a path, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_polygon_set_points(schultz_tree *tree, schultz_handle node,
                                   const schultz_point *points, uint32_t count);

/* -------------------------------------------------------------- controls */

/**
 * @brief Creates a button: a box with a centred caption.
 *
 * The caption is a Label child, so it is styled and measured like any other
 * text. The button sizes itself around it, with padding from the resolved
 * `padding` property.
 *
 * A click reaches the host; the button does not consume it, because pressing
 * a button is exactly the thing the host wants to hear about. Space and enter
 * activate it, which the router does for any node declaring
 * SCHULTZ_ACTION_CLICK.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param text     The caption, copied. NULL means empty.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_button_create(schultz_tree *tree, schultz_handle parent,
                              const char *text, schultz_handle *out_node);

/**
 * @brief Returns a button's caption node.
 *
 * Use it to change the caption with schultz_label_set_text, or to style the
 * text on its own.
 *
 * Every button in the toolkit answers, not only a plain one: a toggle button,
 * a menu button and a split menu button are all made as buttons and keep the
 * caption a button is made with.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node A button, made by schultz_button_create,
 *             schultz_toggle_button_create, schultz_menu_button_create or
 *             schultz_split_menu_button_create.
 * @return The Label child, or SCHULTZ_HANDLE_NONE when the node is not one of
 *         those.
 */
schultz_handle schultz_button_label(const schultz_tree *tree,
                                    schultz_handle node);

/**
 * @brief Creates a hyperlink: a button drawn as a link.
 *
 * Text in the accent colour with no border or background, underlined while
 * the pointer is over it, and focusable and keyboard activated like any other
 * button.
 *
 * The toolkit opens nothing. The click reaches the host through the event
 * queue with the node's token, and what the link points at is the host's
 * business.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param text     The caption, copied.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_hyperlink_create(schultz_tree *tree, schultz_handle parent,
                                 const char *text, schultz_handle *out_node);

/**
 * @brief Replaces a hyperlink's caption.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_hyperlink_create.
 * @param text The new caption, copied.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_hyperlink_set_text(schultz_tree *tree, schultz_handle node,
                                   const char *text);

/**
 * @brief Marks a link as followed, which draws it in the muted colour.
 *
 * Only the host knows whether the target was reached, so only the host can
 * set this.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    A node created by schultz_hyperlink_create.
 * @param visited Nonzero for followed, zero to go back to unvisited.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_hyperlink_set_visited(schultz_tree *tree, schultz_handle node,
                                      int32_t visited);

/**
 * @brief Reports whether a link has been marked as followed.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_hyperlink_create.
 * @return 1 when visited, 0 otherwise.
 */
int32_t schultz_hyperlink_is_visited(const schultz_tree *tree,
                                     schultz_handle node);

/**
 * @brief Creates a toggle button: a button that stays pressed.
 *
 * Drawn as a button and selected rather than checked, since the selected
 * state is what the toolkit paints in the accent everywhere else.
 *
 * A group number works the way schultz_radio_create's does: only equality
 * matters, and a group is the siblings sharing a parent and that number.
 * Selecting one clears the rest, which is what makes a row of them a
 * segmented control. **Group 0 means ungrouped**, and an ungrouped toggle
 * button turns on and off freely.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param text     The caption, copied.
 * @param group    Group number, or 0 for ungrouped.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_toggle_button_create(schultz_tree *tree,
                                     schultz_handle parent, const char *text,
                                     uint32_t group,
                                     schultz_handle *out_node);

/**
 * @brief Selects a toggle button, clearing the rest of its group.
 *
 * An ungrouped button flips instead, since it has no group to be exclusive
 * within. A grouped button that is already selected does not turn off, the
 * way a radio does not.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_toggle_button_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_toggle_button_select(schultz_tree *tree, schultz_handle node);

/**
 * @brief Reports whether a toggle button is on.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_toggle_button_create.
 * @return 1 when selected, 0 otherwise.
 */
int32_t schultz_toggle_button_is_selected(const schultz_tree *tree,
                                          schultz_handle node);

/** @brief What a button in a bar is for, which decides where it sits. */
enum {
    SCHULTZ_BUTTON_ROLE_OTHER = 0, /**< Anything with no standard place. */
    SCHULTZ_BUTTON_ROLE_OK,        /**< Accepts. */
    SCHULTZ_BUTTON_ROLE_CANCEL,    /**< Dismisses. */
    SCHULTZ_BUTTON_ROLE_YES,       /**< Accepts, in a question. */
    SCHULTZ_BUTTON_ROLE_NO,        /**< Declines, in a question. */
    SCHULTZ_BUTTON_ROLE_APPLY,     /**< Commits without dismissing. */
    SCHULTZ_BUTTON_ROLE_HELP,      /**< Explains. */
    SCHULTZ_BUTTON_ROLE_LEFT,      /**< Pinned left, outside the ordering. */
    SCHULTZ_BUTTON_ROLE_COUNT
};

/** @brief Which platform's convention a bar arranges itself by. */
enum {
    SCHULTZ_BUTTON_ORDER_WINDOWS = 0, /**< OK then Cancel. */
    SCHULTZ_BUTTON_ORDER_MACOS,       /**< Cancel then OK. */
    SCHULTZ_BUTTON_ORDER_LINUX        /**< Cancel then OK, Help at the left. */
};

/** @brief The largest number of buttons one bar arranges. */
enum {
    SCHULTZ_BUTTON_BAR_MAX = 16
};

/** @brief The convention used when a bar is not told otherwise. */
#define SCHULTZ_BUTTON_ORDER_DEFAULT SCHULTZ_BUTTON_ORDER_LINUX

/**
 * @brief Creates a button bar: a row ordered by convention, not by insertion.
 *
 * A host adds its buttons in whatever order suits its code and each one says
 * what it is for. The bar puts them where the platform expects, so the same
 * dialog has Cancel on the correct side everywhere.
 *
 * Buttons sit against the right edge, except one given
 * SCHULTZ_BUTTON_ROLE_LEFT, which is pinned to the left and left out of the
 * ordering. Every button is sized to the widest unless that is turned off.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_button_bar_create(schultz_tree *tree, schultz_handle parent,
                                  schultz_handle *out_node);

/**
 * @brief Adds a button to a bar.
 *
 * @param tree       The tree holding the bar. Must not be NULL.
 * @param node       A node created by schultz_button_bar_create.
 * @param text       The caption, copied.
 * @param role       One of the SCHULTZ_BUTTON_ROLE_* values.
 * @param out_button Receives the new button, or NULL if it is not wanted.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_button_bar_add(schultz_tree *tree, schultz_handle node,
                               const char *text, uint32_t role,
                               schultz_handle *out_button);

/**
 * @brief Says what an existing node in a bar is for.
 *
 * This is how anything other than a plain button joins a bar: add it as a
 * child of the bar and then give it a role.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param button A child of a button bar.
 * @param role   One of the SCHULTZ_BUTTON_ROLE_* values.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for an unknown role, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_button_bar_set_role(schultz_tree *tree, schultz_handle button,
                                    uint32_t role);

/**
 * @brief Returns what a button in a bar is for.
 *
 * @param tree   The tree holding the node. NULL yields the other role.
 * @param button A child of a button bar.
 * @return One of the SCHULTZ_BUTTON_ROLE_* values.
 */
uint32_t schultz_button_bar_role(const schultz_tree *tree,
                                 schultz_handle button);

/**
 * @brief Chooses which platform's ordering a bar follows.
 *
 * @param tree  The tree holding the bar. Must not be NULL.
 * @param node  A node created by schultz_button_bar_create.
 * @param order One of the SCHULTZ_BUTTON_ORDER_* values.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_button_bar_set_order(schultz_tree *tree, schultz_handle node,
                                     uint32_t order);

/**
 * @brief Turns matching button widths on or off. On by default.
 *
 * @param tree The tree holding the bar. Must not be NULL.
 * @param node A node created by schultz_button_bar_create.
 * @param on   Nonzero to size every button to the widest.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_button_bar_set_uniform_width(schultz_tree *tree,
                                             schultz_handle node, int32_t on);

/**
 * @brief Creates a checkbox: a tick box with a caption beside it.
 *
 * Clicking it flips SCHULTZ_STATE_CHECKED on itself, and the click still
 * reaches the host so it can act on the new value. The box is a square of the
 * row height; its fill is `background`, its outline `border.color`, and the
 * tick `selection.color`, all of which change with the checked state through
 * the state patches the constructor installs.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param text     The caption, copied. NULL or empty means no caption.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_checkbox_create(schultz_tree *tree, schultz_handle parent,
                                const char *text, schultz_handle *out_node);

/**
 * @brief Creates a switch: a checkbox that draws as a sliding track.
 *
 * Identical to a checkbox in every way but its painting, which is what a
 * switch is: the same two state choice, shown as on or off rather than as
 * ticked or not.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param text     The caption, copied. NULL or empty means no caption.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_switch_create(schultz_tree *tree, schultz_handle parent,
                              const char *text, schultz_handle *out_node);

/**
 * @brief Creates a radio button, one of a mutually exclusive set.
 *
 * A group is the radios that share a parent and a group number, so two groups
 * on one screen need no coordination beyond sitting in different containers.
 * Clicking one selects it and clears the rest of its group; unlike a
 * checkbox, clicking the selected one changes nothing, because one of a set
 * is always chosen.
 *
 * Because the group is the parent, a radio taken out of the tree with
 * schultz_node_set_parent is in no group: selecting it clears nothing, and
 * nothing clears it. It keeps whatever it was set to, and putting it back
 * joins it to the group again for the next click but does not settle the
 * group as it stands. So a radio removed while selected comes back selected
 * beside whichever one was chosen in its absence, and both read as chosen
 * until one of them is clicked. Call schultz_radio_select on the one that
 * should win after putting it back.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param text     The caption, copied. NULL or empty means no caption.
 * @param group    Group number. Any value; only equality matters.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_radio_create(schultz_tree *tree, schultz_handle parent,
                             const char *text, uint32_t group,
                             schultz_handle *out_node);

/**
 * @brief Selects a radio and clears the rest of its group.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_radio_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         radio.
 */
int32_t schultz_radio_select(schultz_tree *tree, schultz_handle node);

/**
 * @brief Reports whether a checkbox, radio or switch is on.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A checkbox, radio or switch node.
 * @return 1 when checked, 0 when not or when the node is not a toggle.
 */
int32_t schultz_toggle_checked(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Turns a checkbox, radio or switch on or off.
 *
 * This sets one node and does not clear a radio's group; use
 * schultz_radio_select for that.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    A checkbox, radio or switch node.
 * @param checked Nonzero to turn it on.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         toggle.
 */
int32_t schultz_toggle_set_checked(schultz_tree *tree, schultz_handle node,
                                   int32_t checked);

/**
 * @brief Creates a slider: a value dragged along a track.
 *
 * Pressing or dragging anywhere on it moves the thumb there. Left and down
 * step back, right and up step forward, home and end jump to the ends. Those
 * keys are consumed, so an arrow that moved a slider does not also move
 * focus. The value starts at @p value and is always inside the range.
 *
 * The track is drawn in `border.color`, the part behind the thumb in
 * `background`, and the thumb in `selection.color`.
 *
 * A vertical slider counts upward: the minimum is at the bottom, which is
 * the way a person reads a column. Up and right still step forward and down
 * and left still step back, so the keys mean the same thing either way round.
 *
 * @param tree        The tree to create in. Must not be NULL.
 * @param parent      The parent node. Must name a live node.
 * @param orientation SCHULTZ_ORIENT_HORIZONTAL or _VERTICAL.
 * @param minimum     Lowest value.
 * @param maximum     Highest value. Must not be below the minimum.
 * @param value       Starting value, clamped into the range.
 * @param out_node    Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when the range is
 *         backwards or the orientation is neither of the two, or an error as
 *         above.
 */
int32_t schultz_slider_create(schultz_tree *tree, schultz_handle parent,
                              uint32_t orientation, float minimum,
                              float maximum, float value,
                              schultz_handle *out_node);

/**
 * @brief Moves a slider to a value.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_slider_create.
 * @param value The new value, clamped into the range.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         slider.
 */
int32_t schultz_slider_set_value(schultz_tree *tree, schultz_handle node,
                                 float value);

/**
 * @brief Returns a slider's value.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_slider_create.
 * @return The value, or 0 when the node is not a slider.
 */
float schultz_slider_value(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Sets how far one arrow key moves a slider.
 *
 * The default is a hundredth of the range.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_slider_create.
 * @param step The step. Must be greater than zero.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a step of zero or
 *         less, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a slider.
 */
int32_t schultz_slider_set_step(schultz_tree *tree, schultz_handle node,
                                float step);

/**
 * @brief Creates a progress bar: a track with a filled portion.
 *
 * It draws and nothing else: it takes no focus and handles no events. The
 * track is `border.color` and the filled part `background`.
 *
 * A vertical bar fills from the bottom, the way a column fills.
 *
 * @param tree        The tree to create in. Must not be NULL.
 * @param parent      The parent node. Must name a live node.
 * @param orientation SCHULTZ_ORIENT_HORIZONTAL or _VERTICAL.
 * @param out_node    Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when the orientation is
 *         neither of the two, or an error as above.
 */
int32_t schultz_progress_bar_create(schultz_tree *tree, schultz_handle parent,
                                    uint32_t orientation,
                                    schultz_handle *out_node);

/**
 * @brief Sets how far along a progress bar is.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_progress_bar_create.
 * @param value 0 to 1, clamped. On an unmeasured bar this is where the
 *              marker sits rather than how full the bar is.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         progress bar.
 */
int32_t schultz_progress_bar_set_value(schultz_tree *tree,
                                       schultz_handle node, float value);

/**
 * @brief Returns a progress bar's value.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_progress_bar_create.
 * @return The value, or 0 when the node is not a progress bar.
 */
float schultz_progress_bar_value(const schultz_tree *tree,
                                 schultz_handle node);

/**
 * @brief Switches a progress bar between measured and unmeasured.
 *
 * Nothing in the toolkit knows what time it is, so an unmeasured bar does not
 * animate itself. It draws a marker covering a third of the track, positioned
 * by the value, and the host moves it the same way it moves anything else
 * that animates.
 *
 * @param tree          The tree holding the node. Must not be NULL.
 * @param node          A node created by schultz_progress_bar_create.
 * @param indeterminate Nonzero for a bar whose end is not known.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         progress bar.
 */
int32_t schultz_progress_bar_set_indeterminate(schultz_tree *tree,
                                               schultz_handle node,
                                               int32_t indeterminate);

/* ------------------------------------------------------------- scrolling */

/**
 * @brief Creates a scroll bar: a track with a draggable thumb.
 *
 * A real widget rather than painting inside a scroll view, so it can be
 * styled, focused and used on its own to scroll anything at all.
 *
 * The thumb's length is the fraction of the content that shows, floored so it
 * stays grabbable in a very long document. Dragging keeps the point that was
 * grabbed under the pointer; pressing the track beyond the thumb pages
 * towards the press. Arrows step, page keys page, home and end jump.
 *
 * The track is drawn in `border.color` and the thumb in `background`.
 *
 * @param tree        The tree to create in. Must not be NULL.
 * @param parent      The parent node. Must name a live node.
 * @param orientation SCHULTZ_ORIENT_HORIZONTAL or _VERTICAL.
 * @param out_node    Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_scroll_bar_create(schultz_tree *tree, schultz_handle parent,
                                  uint32_t orientation,
                                  schultz_handle *out_node);

/**
 * @brief Told whenever a scroll bar's value moves.
 *
 * @param context The pointer given when the callback was installed.
 * @param tree    The tree holding the bar.
 * @param bar     The bar that moved.
 * @param value   Its new offset.
 */
typedef void (*schultz_scroll_bar_changed_fn)(void *context,
                                              schultz_tree *tree,
                                              schultz_handle bar,
                                              float value);

/**
 * @brief Asks to be told when a scroll bar moves.
 *
 * Called for every reason a bar moves: a drag, a page, a key, or an
 * application setting the value. This is how a bar used on its own scrolls
 * something, and it is how a scroll view keeps its content and its bars in
 * step.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    A node created by schultz_scroll_bar_create.
 * @param changed The callback, or NULL to stop being told.
 * @param context Passed to it, unchanged.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         scroll bar.
 */
int32_t schultz_scroll_bar_on_change(schultz_tree *tree, schultz_handle node,
                                     schultz_scroll_bar_changed_fn changed,
                                     void *context);

/**
 * @brief Tells a scroll bar how much there is and how much of it shows.
 *
 * Both are in the same units as the value, which are the content's own. A
 * viewport at least as large as the content leaves nowhere to scroll, and the
 * bar clamps its value to match.
 *
 * @param tree     The tree holding the node. Must not be NULL.
 * @param node     A node created by schultz_scroll_bar_create.
 * @param content  Total length of what is being scrolled. Not negative.
 * @param viewport Length of the part that shows. Not negative.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a negative length, or
 *         SCHULTZ_ERR_INVALID_HANDLE when the node is not a scroll bar.
 */
int32_t schultz_scroll_bar_set_range(schultz_tree *tree, schultz_handle node,
                                     float content, float viewport);

/**
 * @brief Moves a scroll bar to an offset.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_scroll_bar_create.
 * @param value The offset, clamped between zero and the maximum.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         scroll bar.
 */
int32_t schultz_scroll_bar_set_value(schultz_tree *tree, schultz_handle node,
                                     float value);

/**
 * @brief Returns a scroll bar's offset.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_scroll_bar_create.
 * @return The offset, or 0 when the node is not a scroll bar.
 */
float schultz_scroll_bar_value(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Returns the furthest a scroll bar can be scrolled.
 *
 * The content length less the viewport length, never below zero.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_scroll_bar_create.
 * @return The maximum offset, or 0 when the node is not a scroll bar.
 */
float schultz_scroll_bar_maximum(const schultz_tree *tree,
                                 schultz_handle node);

/**
 * @brief Creates a scroll view: a window onto content larger than itself.
 *
 * The view owns four nodes. A viewport clips and offsets, the content sits
 * inside it and is what the application fills, and two scroll bars sit along
 * the bottom and right edges, each shown only when there is something to
 * scroll in its direction.
 *
 * Scrolling shifts where the content is drawn and hit tested rather than
 * moving its bounds, so it costs a repaint and never a relayout, whatever the
 * content is. Everything that scrolls moves the bars and the bars move the
 * content, so the two can never disagree.
 *
 * Three things move it. A mouse wheel, a bar dragged with either a cursor or
 * a finger, and a finger dragged across the content itself: the content
 * follows the finger, and a flick carries on and slows to a stop after the
 * finger lifts. A cursor deliberately does not drag the content, because it
 * has a wheel and a bar already and dragging is how a cursor selects text.
 *
 * A finger that starts on something inside the view presses it if it stays
 * still and scrolls if it moves, and the press is not also delivered: see
 * schultz_events_mouse_button.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_scroll_view_create(schultz_tree *tree, schultz_handle parent,
                                   schultz_handle *out_node);

/**
 * @brief Returns the node an application fills with the scrolled content.
 *
 * Give it a pane and children, or a size hint, and the view scrolls it.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node A node created by schultz_scroll_view_create.
 * @return The content node, or SCHULTZ_HANDLE_NONE when the node is not a
 *         scroll view.
 */
schultz_handle schultz_scroll_view_content(const schultz_tree *tree,
                                           schultz_handle node);

/**
 * @brief Returns one of a scroll view's two bars, to style or query it.
 *
 * @param tree        The tree holding the node. NULL yields
 *                    SCHULTZ_HANDLE_NONE.
 * @param node        A node created by schultz_scroll_view_create.
 * @param orientation SCHULTZ_ORIENT_HORIZONTAL or _VERTICAL.
 * @return The bar, or SCHULTZ_HANDLE_NONE when the node is not a scroll view.
 */
schultz_handle schultz_scroll_view_bar(const schultz_tree *tree,
                                       schultz_handle node,
                                       uint32_t orientation);

/**
 * @brief Scrolls the least distance that brings a node into view.
 *
 * Does nothing when the node is already inside, which is the usual answer
 * while walking a selection with the arrow keys. A node larger than the
 * viewport is shown from its near edge rather than its far one.
 *
 * The target may be any node under the view, not only a direct child of its
 * content, so a row built out of several nodes can name whichever one should
 * be brought into view.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A node created by schultz_scroll_view_create.
 * @param target The node to bring into view.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         scroll view or the target is not live.
 */
int32_t schultz_scroll_view_reveal(schultz_tree *tree, schultz_handle node,
                                   schultz_handle target);

/**
 * @brief Says whether this view may show its bars.
 *
 * Bars show by default, when the content is larger than the view. Turning
 * them off leaves the view scrolling exactly as it did -- a finger still
 * drags the content, a wheel still turns it -- and gives the strip of space
 * a bar was taking back to the content. That is what a view on a touch panel
 * wants, where nobody aims at a bar and every pixel of height is spoken for.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_scroll_view_create.
 * @param shown Nonzero to let the bars appear, zero to keep them away.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         scroll view.
 */
int32_t schultz_scroll_view_set_bars(schultz_tree *tree, schultz_handle node,
                                     int32_t shown);

/**
 * @brief Says whether a pointer that is not a finger may drag the content.
 *
 * A finger always drags. A cursor does not, because it has a wheel and a bar
 * already and dragging with one is how text is selected -- so turning this on
 * is for a view holding nothing selectable, where dragging is the whole
 * gesture. The keyboard's field of emoji is the case it exists for.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_scroll_view_create.
 * @param drags Nonzero to let any pointer drag the content.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         scroll view.
 */
int32_t schultz_scroll_view_set_drag_scrolls(schultz_tree *tree,
                                             schultz_handle node,
                                             int32_t drags);

/**
 * @brief Returns whether any pointer may drag this view's content.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_scroll_view_create.
 * @return Nonzero when a cursor drags it as a finger does.
 */
int32_t schultz_scroll_view_drag_scrolls(const schultz_tree *tree,
                                         schultz_handle node);

/**
 * @brief Returns whether this view may show its bars.
 *
 * Named for the question rather than as the mirror of the setter, because
 * schultz_scroll_view_bar already means one of the two bar nodes and one
 * word cannot mean both.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_scroll_view_create.
 * @return Nonzero when a bar appears once the content needs one.
 */
int32_t schultz_scroll_view_shows_bars(const schultz_tree *tree,
                                       schultz_handle node);

/**
 * @brief Scrolls a view to an offset.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A node created by schultz_scroll_view_create.
 * @param offset How far into the content to scroll, clamped on both axes.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         scroll view.
 */
int32_t schultz_scroll_view_scroll_to(schultz_tree *tree, schultz_handle node,
                                      schultz_point offset);

/* ---------------------------------------------------------- text entry */

/**
 * @brief Creates a text field: one line of editable text.
 *
 * The field edits itself. Typing inserts, backspace and delete remove, the
 * arrows move by a character and with control by a word, home and end go to
 * the ends, and shift with any of those extends the selection. Control with
 * A, C, X and V select all, copy, cut and paste, and control with Z takes an
 * edit back, with shift to put it forward again. A run of typing is one step
 * of that history rather than one step per keystroke.
 *
 * Cut, copy and paste need a clipboard; install one with
 * schultz_tree_set_clipboard. Without one, everything else still works.
 *
 * Enter is left alone, so a host can treat it as accepting the form.
 *
 * The text scrolls sideways to keep the caret in view. Selection is drawn in
 * `selection.color`, the text and caret in `text.color`.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param text     Starting text, copied. NULL means empty.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_text_field_create(schultz_tree *tree, schultz_handle parent,
                                  const char *text, schultz_handle *out_node);

/** @brief What kind of text a field holds. */
enum {
    SCHULTZ_INPUT_TEXT = 0,  /**< Anything. The default. */
    SCHULTZ_INPUT_NUMBER,    /**< Digits and what goes with them. */
    SCHULTZ_INPUT_EMAIL,     /**< An address. */
    SCHULTZ_INPUT_PASSWORD   /**< A secret, which a masked field already is. */
};

/**
 * @brief Says what kind of text this field holds.
 *
 * What it changes is the keyboard: a number field opens on the numbers, and
 * neither a number nor a password field offers emoji however the window is
 * set. It does not restrict what may be typed; a field that must hold only
 * digits is a number field.
 *
 * @param tree The tree holding the field. Must not be NULL.
 * @param node A node from schultz_text_field_create or one of its kin.
 * @param type One of the SCHULTZ_INPUT_* values.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_INVALID_ARGUMENT for a type that is not one of them.
 */
int32_t schultz_text_field_set_input_type(schultz_tree *tree,
                                          schultz_handle node, uint32_t type);

/**
 * @brief Returns what kind of text a field holds.
 *
 * A masked field answers SCHULTZ_INPUT_PASSWORD whether or not it was told
 * to, since that is what masking means.
 *
 * @param tree The tree holding the field. Must not be NULL.
 * @param node A node from schultz_text_field_create or one of its kin.
 * @return One of the SCHULTZ_INPUT_* values.
 */
uint32_t schultz_text_field_input_type(const schultz_tree *tree,
                                       schultz_handle node);

/** @brief The mask a password field uses: a bullet, U+2022. */
#define SCHULTZ_PASSWORD_MASK 0x2022u

/**
 * @brief Draws a mask character in place of each character a field holds.
 *
 * Everything else about the field is unchanged, so every text field accessor
 * works on a masked one. What is masked is the drawing only: the field still
 * holds the real text and schultz_text_get still returns it.
 *
 * **Cut and copy are refused while a mask is set**, so the text never reaches
 * the clipboard. Cut still removes the selection. Paste still works.
 *
 * @param tree      The tree holding the node. Must not be NULL.
 * @param node      A node created by schultz_text_field_create.
 * @param codepoint The character to draw, or 0 to show the text itself.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_INVALID_ARGUMENT when asked to mask a text area.
 */
int32_t schultz_text_field_set_mask(schultz_tree *tree, schultz_handle node,
                                    uint32_t codepoint);

/**
 * @brief Returns the mask character a field is drawing, or 0 for none.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_text_field_create.
 * @return The codepoint, or 0.
 */
uint32_t schultz_text_field_mask(const schultz_tree *tree,
                                 schultz_handle node);

/**
 * @brief Creates a password field: a text field with masking already on.
 *
 * The same widget as a text field, so it is created, read and written with
 * the text field calls. Turning the mask off with
 * schultz_text_field_set_mask turns it back into a plain field.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param text     Starting text, copied. NULL means empty.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_password_field_create(schultz_tree *tree,
                                      schultz_handle parent,
                                      const char *text,
                                      schultz_handle *out_node);

/**
 * @brief Creates a text area: wrapped, scrolling, editable text.
 *
 * Everything a text field does, plus wrapping at its own width, up and down
 * between lines keeping the column, home and end on the current line, and
 * enter inserting a line break.
 *
 * It is three lines tall and its text scrolls inside it, keeping the caret in
 * view. A box that got taller with every line typed would push whatever is
 * under it down the screen, so growing is something to ask for rather than
 * the default: see schultz_text_area_set_grows. Change the height with
 * schultz_text_area_set_visible_lines.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param text     Starting text, copied. NULL means empty.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_text_area_create(schultz_tree *tree, schultz_handle parent,
                                 const char *text, schultz_handle *out_node);

/**
 * @brief Returns what a text field or text area holds.
 *
 * @param tree The tree holding the node. NULL yields NULL.
 * @param node A text field or text area node.
 * @return The text, owned by the widget, or NULL when the node is neither.
 */
const char *schultz_text_get(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Replaces what a text field or text area holds.
 *
 * The replacement can be taken back like any other edit.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A text field or text area node.
 * @param text The new text, copied. NULL means empty.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is neither, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_text_set(schultz_tree *tree, schultz_handle node,
                         const char *text);

/**
 * @brief Says which stretches of a field's or an area's text are not set the
 *        way its style says.
 *
 * The same spans a Label takes, on text that can be edited. Everything moves
 * with the words as they are typed, pasted and deleted: a stretch marked bold
 * stays on the same characters, grows when something is typed inside it, and
 * goes when the characters it marked go.
 *
 * The list is copied, link strings and all. Spans covering nothing, or bytes
 * past the end of the text, are dropped.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node from schultz_text_field_create or _text_area_create.
 * @param spans The spans. May be NULL when count is zero.
 * @param count How many spans.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node holds no
 *         editable text, SCHULTZ_ERR_INVALID_ARGUMENT for NULL spans with a
 *         nonzero count, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_text_field_set_spans(schultz_tree *tree, schultz_handle node,
                                     const schultz_span *spans,
                                     uint32_t count);

/**
 * @brief The spans an editable text widget is carrying.
 *
 * @param tree      The tree holding the node. Must not be NULL.
 * @param node      A node holding editable text.
 * @param out_count Receives how many there are, or NULL if not wanted.
 * @return The spans, owned by the widget and valid until the text or the
 *         spans change, or NULL when there are none.
 */
const schultz_span *schultz_text_field_spans(const schultz_tree *tree,
                                             schultz_handle node,
                                             uint32_t *out_count);

/**
 * @brief Gives one stretch of the text a look of its own.
 *
 * What a Bold button does to a selection. A span the range lands in the
 * middle of is split, and neighbours that end up saying the same thing are
 * merged, so the list stays as short as what it describes.
 *
 * Pass NULL for `look` to strip the range back to the widget's own style.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node holding editable text.
 * @param from First byte.
 * @param to   One past the last byte. Cut to the end of the text.
 * @param look What that stretch should look like, or NULL for plain.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node holds no
 *         editable text, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_text_field_set_span(schultz_tree *tree, schultz_handle node,
                                    uint32_t from, uint32_t to,
                                    const schultz_span *look);

/**
 * @brief Says what the next thing typed should look like.
 *
 * What a Bold button does with nothing selected: there is no text to mark
 * yet, so the look is remembered and given to whatever is typed next.
 *
 * Without this, typing takes after the character before the caret, which is
 * what continues a bold word when more is added to the end of it.
 *
 * Moving the caret forgets it, because a look asked for in one place was not
 * asked for in another. Pass NULL to forget it now.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node holding editable text.
 * @param look What to type in, or NULL to go back to taking after the text.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node holds no
 *         editable text, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_text_field_set_typing(schultz_tree *tree, schultz_handle node,
                                      const schultz_span *look);

/**
 * @brief Sets how many lines tall a text area is.
 *
 * Three by default. A size hint on the node overrules it.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_text_area_create.
 * @param lines How many lines. Must be at least one.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for zero lines, or
 *         SCHULTZ_ERR_INVALID_HANDLE when the node is not a text area.
 */
int32_t schultz_text_area_set_visible_lines(schultz_tree *tree,
                                            schultz_handle node,
                                            uint32_t lines);

/**
 * @brief Lets a text area's height follow its text instead of scrolling.
 *
 * Off by default, because a box that gets taller with every line typed pushes
 * whatever is under it down the screen. Turn it on for the cases that want
 * it: a message composer that opens up as it is filled, or an area inside a
 * scroll view, where the view supplies the bars and does the scrolling.
 *
 * A growing area never shrinks below the height it asked for.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_text_area_create.
 * @param grows Nonzero to follow the text.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         text area.
 */
int32_t schultz_text_area_set_grows(schultz_tree *tree, schultz_handle node,
                                    int32_t grows);

/**
 * @brief Sets the selection, as two byte offsets.
 *
 * The anchor is the end that stays put and the caret is the end that moves,
 * so passing the same value for both places the caret and selects nothing.
 * Both are clamped to the text's length.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A text field or text area node.
 * @param anchor Byte offset of the fixed end.
 * @param caret  Byte offset of the moving end.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is neither.
 */
int32_t schultz_text_set_selection(schultz_tree *tree, schultz_handle node,
                                   uint32_t anchor, uint32_t caret);

/**
 * @brief Reads the selection.
 *
 * @param tree       The tree holding the node. Must not be NULL.
 * @param node       A text field or text area node.
 * @param out_anchor Receives the fixed end, or NULL if not wanted.
 * @param out_caret  Receives the moving end, or NULL if not wanted.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is neither.
 */
int32_t schultz_text_selection(const schultz_tree *tree, schultz_handle node,
                               uint32_t *out_anchor, uint32_t *out_caret);

/* --------------------------------------------------------------- Canvas */

/**
 * @brief Creates a canvas: a widget holding drawing the application records.
 *
 * The drawing is retained, not asked for each frame. Record into it when the
 * data changes and it is replayed for free thereafter, which is what a chart
 * wants: it redraws when its numbers change, not sixty times a second. From a
 * scripting language above, that means a burst of calls when something
 * happens and none at all in between.
 *
 * Coordinates are the canvas's own, with the origin at its top left, and the
 * drawing is clipped to its bounds.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_canvas_create(schultz_tree *tree, schultz_handle parent,
                              schultz_handle *out_node);

/**
 * @brief Draws this canvas in screen pixels rather than toolkit units.
 *
 * The toolkit usually draws everything larger on a screen that packs more
 * pixels into the same space, so a line one wide covers three of them on a
 * dense phone. That is right for a button and wrong for a drawing: a chart
 * that wants a one pixel rule, or a picture meant to land on exact pixels,
 * needs to reach the screen as it is.
 *
 * With this set, one unit inside this canvas is one pixel of the screen. The
 * canvas itself is still placed and sized like any other widget, so a canvas
 * a hundred units wide still occupies a hundred units of the layout; what
 * changes is that its own drawing space is then three hundred wide on a
 * three times screen. Ask schultz_canvas_pixel_size for that number rather
 * than working it out.
 *
 * Nothing changes on a screen with nothing to scale, where a unit is already
 * a pixel, or when the frame was told not to scale to the screen.
 *
 * @param tree  The tree the canvas is in. Must not be NULL.
 * @param node  A node created by schultz_canvas_create.
 * @param exact Nonzero to draw in screen pixels, zero for toolkit units.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_canvas_set_pixel_exact(schultz_tree *tree,
                                       schultz_handle node, int32_t exact);

/**
 * @brief The size of a canvas's drawing space in screen pixels.
 *
 * The same as its bounds on a screen with nothing to scale, and three times
 * them on a dense phone. This is the number to draw against when the canvas
 * is set to screen pixels, and the resolution a picture should be prepared
 * at either way.
 *
 * @param tree       The tree to query. Must not be NULL.
 * @param node       A node created by schultz_canvas_create.
 * @param out_width  Receives the width in pixels. Must not be NULL.
 * @param out_height Receives the height in pixels. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_canvas_pixel_size(const schultz_tree *tree,
                                  schultz_handle node, uint32_t *out_width,
                                  uint32_t *out_height);

/**
 * @brief Starts recording, clearing whatever was there.
 *
 * Every drawing call between this and schultz_canvas_end is kept. Calls made
 * outside that pair are refused, so a half finished recording cannot end up
 * on screen.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_canvas_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         canvas.
 */
int32_t schultz_canvas_begin(schultz_tree *tree, schultz_handle node);

/**
 * @brief Finishes recording and marks the canvas for repaint.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_canvas_create.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         canvas, or SCHULTZ_ERR_OUT_OF_MEMORY when the recording ran out of
 *         room part way through.
 */
int32_t schultz_canvas_end(schultz_tree *tree, schultz_handle node);

/**
 * @brief Starts a group: everything until the end becomes one picture.
 *
 * The reason to want one is that a drawing made of several pieces is often
 * one thing to the eye. A card is a rounded rectangle, a title and a button
 * glyph, and it should cast **one** shadow and fade as **one** picture.
 * Without a group it casts three shadows that overlap, and fading the three
 * separately makes every place they overlap darker than it should be,
 * because the same pixel is blended twice.
 *
 * Groups nest, so a group may hold other groups, each with its own fading and
 * its own shadow.
 *
 * **Why there is an end call.** A canvas is a stream of drawing calls rather
 * than a bag of objects, so which pieces belong to the group can only be said
 * as a range: everything between the two. And because groups nest, a start
 * inside a group means a group inside that group, so only the end can say
 * which one has finished. The end is also the moment the picture exists,
 * which is when the shadow can be worked out at all: a shadow is cast by a
 * finished shape.
 *
 * A group left open when schultz_canvas_end is reached is closed there, so a
 * drawing that returned early still appears rather than being thrown away.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    A canvas that is recording.
 * @param opacity 0 for invisible, 1 for solid.
 * @param shadow  The shadow the group casts, or schultz_shadow_none.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         canvas or is not recording, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_canvas_group_begin(schultz_tree *tree, schultz_handle node,
                                  float opacity, schultz_shadow shadow);

/**
 * @brief Finishes the group started by schultz_canvas_group_begin.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A canvas that is recording.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         canvas or is not recording, SCHULTZ_ERR_INVALID_ARGUMENT when no
 *         group is open, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_canvas_group_end(schultz_tree *tree, schultz_handle node);

/**
 * @brief Records a filled rectangle, rounded when a radius is given.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A canvas that is recording.
 * @param rect   The rectangle, in the canvas's own coordinates.
 * @param paint  What to fill with.
 * @param radius Corner radius, or zero for square corners.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         canvas or is not recording, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_canvas_fill_rect(schultz_tree *tree, schultz_handle node,
                                 schultz_rect rect, schultz_paint paint,
                                 float radius);

/**
 * @brief Records a rectangle outline.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A canvas that is recording.
 * @param rect   The rectangle, in the canvas's own coordinates.
 * @param stroke What the outline is drawn with.
 * @param radius Corner radius, or zero for square corners.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_stroke_rect(schultz_tree *tree, schultz_handle node,
                                   schultz_rect rect, schultz_stroke stroke,
                                   float radius);

/**
 * @brief Records a polygon outline, or a line through a series of points.
 *
 * The counterpart to schultz_canvas_fill_polygon. Drawing the segments one line
 * at a time is not the same thing: each line would end square at every
 * corner, and a thick outline shows it.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A canvas that is recording.
 * @param points The vertices, in order. Copied.
 * @param count  How many. Must be at least two.
 * @param stroke What the outline is drawn with.
 * @param closed Nonzero joins the last point back to the first, making a
 *               closed outline; zero leaves it open, making a polyline.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_stroke_polygon(schultz_tree *tree, schultz_handle node,
                                      const schultz_point *points,
                                      uint32_t count, schultz_stroke stroke,
                                      int32_t closed);

/**
 * @brief Measures a string, without drawing it.
 *
 * Text can be drawn onto a canvas, and laying anything out around it needs
 * its size. The toolkit shapes its own text, so this is the width the string
 * will actually take rather than an estimate.
 *
 * This does not record anything and does not need a canvas that is open.
 *
 * @param tree     A tree with a font system. Must not be NULL.
 * @param font     A loaded font.
 * @param utf8     The text, NUL terminated.
 * @param out_size Receives the width and the line height. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_canvas_measure_text(const schultz_tree *tree,
                                    schultz_handle font, const char *utf8,
                                    schultz_size *out_size);

/**
 * @brief Records an ellipse outline inscribed in a rectangle.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A canvas that is recording.
 * @param rect   The rectangle the ellipse is drawn inside.
 * @param stroke What the outline is drawn with.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_stroke_ellipse(schultz_tree *tree,
                                      schultz_handle node,
                                      schultz_rect rect,
                                      schultz_stroke stroke);

/**
 * @brief Records a picture, scaled from a part of it into a rectangle.
 *
 * The source rectangle picks a region of the image out, which is what a
 * sprite sheet or an atlas wants. To draw the whole picture, pass a source
 * rectangle covering it; a source with no width or height means the whole
 * image.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    A canvas that is recording.
 * @param image   A loaded image, from schultz_image_load_file.
 * @param source  The part of the image to take, in image pixels.
 * @param dest    Where to put it, in the canvas's own coordinates.
 * @param opacity 0 for invisible, 255 for opaque.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_image(schultz_tree *tree, schultz_handle node,
                             schultz_handle image, schultz_rect source,
                             schultz_rect dest, uint8_t opacity);

/**
 * @brief Narrows what the drawing after it may touch.
 *
 * Pairs with schultz_canvas_clip_end. Clips nest: each one narrows what the
 * one before it allowed, and never widens it.
 *
 * The node's own bounds already clip a canvas, so this is for masking part of
 * a drawing rather than for keeping it inside its widget.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A canvas that is recording.
 * @param rect What to keep, in the canvas's own coordinates.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_clip_begin(schultz_tree *tree, schultz_handle node,
                                 schultz_rect rect);

/**
 * @brief Restores the clip in force before the matching push.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A canvas that is recording.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_clip_end(schultz_tree *tree, schultz_handle node);

/**
 * @brief Shifts everything drawn after it.
 *
 * Pairs with schultz_canvas_offset_end, and they nest: an offset inside
 * another adds to it. Drawing a repeated shape at several places is what this
 * is for, without adding the position into every coordinate.
 *
 * This is a shift and nothing more. To turn what is drawn, use
 * schultz_canvas_rotation_begin, which nests with this one: an offset pushed
 * inside a rotation moves along the turned axes. There is no scale, because
 * schultz_canvas_set_pixel_exact already says what a unit is worth.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A canvas that is recording.
 * @param dx   How far to move along x.
 * @param dy   How far to move along y.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_offset_begin(schultz_tree *tree, schultz_handle node,
                                   float dx, float dy);

/**
 * @brief Restores the offset in force before the matching push.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A canvas that is recording.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_offset_end(schultz_tree *tree, schultz_handle node);

/**
 * @brief Turns everything drawn after it.
 *
 * Pairs with schultz_canvas_rotation_end, and they nest: a rotation inside
 * another adds to it, and an offset pushed inside a rotation moves along the
 * turned axes.
 *
 * The angle is in **degrees**, clockwise, because y grows downward. The
 * centre is in the canvas's own coordinates, so turning a drawing about its
 * own middle is one call rather than an offset either side of it.
 *
 * This turns the drawing, not the canvas. The node keeps its upright bounds,
 * takes the same space in the layout, clips to the same rectangle and is hit
 * tested the same. Nothing turned can escape the canvas.
 *
 * Shapes, images and text all turn. Text at an angle is drawn from the
 * font's outlines rather than from the cached upright pictures of each
 * letter, so it stays sharp, and it gives up hinting the way rotated text
 * does in every toolkit. An emoji has no outline to draw from and is turned
 * as the picture it is.
 *
 * A clip pushed inside a rotation is the upright box around the turned
 * rectangle rather than the turned rectangle itself, so it lets through a
 * little more than it names.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    A canvas that is recording.
 * @param degrees How far to turn, clockwise.
 * @param cx      The centre to turn about, along x.
 * @param cy      The centre to turn about, along y.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_rotation_begin(schultz_tree *tree, schultz_handle node,
                                     float degrees, float cx, float cy);

/**
 * @brief Restores the rotation in force before the matching push.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A canvas that is recording.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_rotation_end(schultz_tree *tree, schultz_handle node);

/**
 * @brief Records a filled ellipse.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A canvas that is recording.
 * @param rect  The rectangle the ellipse fills.
 * @param paint What to fill with.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_fill_ellipse(schultz_tree *tree, schultz_handle node,
                                    schultz_rect rect, schultz_paint paint);

/**
 * @brief Records a line.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A canvas that is recording.
 * @param from   One end, in the canvas's own coordinates.
 * @param to     The other end.
 * @param stroke What the line is drawn with.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_line(schultz_tree *tree, schultz_handle node,
                            schultz_point from, schultz_point to,
                            schultz_stroke stroke);

/**
 * @brief Records a filled polygon.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A canvas that is recording.
 * @param points The vertices, in order. Copied.
 * @param count  How many. Must be greater than zero.
 * @param paint  What to fill with.
 * @param rule   SCHULTZ_FILL_NONZERO or SCHULTZ_FILL_EVEN_ODD, which differ
 *               only where the outline crosses itself.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_fill_polygon(schultz_tree *tree, schultz_handle node,
                                    const schultz_point *points, uint32_t count,
                                    schultz_paint paint, uint32_t rule);

/**
 * @brief Records a filled path, which may curve and may have holes.
 *
 * The counterpart to schultz_canvas_fill_polygon for anything straight sides
 * cannot say. The two arrays are read together: each step takes the points it
 * needs off the front of the second, so their counts differ and both are
 * given. See SCHULTZ_PATH_MOVE and the values beside it.
 *
 * Both arrays are copied.
 *
 * @param tree        The tree holding the node. Must not be NULL.
 * @param node        A canvas that is recording.
 * @param steps       SCHULTZ_PATH_* values, beginning with a move.
 * @param step_count  How many steps.
 * @param points      The points the steps use.
 * @param point_count How many points the steps together ask for.
 * @param paint       What to fill with.
 * @param rule        SCHULTZ_FILL_NONZERO or SCHULTZ_FILL_EVEN_ODD.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_fill_path(schultz_tree *tree, schultz_handle node,
                                 const uint8_t *steps, uint32_t step_count,
                                 const schultz_point *points,
                                 uint32_t point_count, schultz_paint paint,
                                 uint32_t rule);

/**
 * @brief Records a stroked path, which may curve and may have holes.
 *
 * A subpath that ends in a close is stroked all the way round; one that does
 * not stops at its last point, wearing the stroke's cap at each end. That is
 * the difference between an outline and an open curve, and it is said in the
 * path rather than in a separate argument.
 *
 * @param tree        The tree holding the node. Must not be NULL.
 * @param node        A canvas that is recording.
 * @param steps       SCHULTZ_PATH_* values, beginning with a move.
 * @param step_count  How many steps.
 * @param points      The points the steps use.
 * @param point_count How many points the steps together ask for.
 * @param stroke      What the outline is drawn with.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_canvas_stroke_path(schultz_tree *tree, schultz_handle node,
                                   const uint8_t *steps, uint32_t step_count,
                                   const schultz_point *points,
                                   uint32_t point_count,
                                   schultz_stroke stroke);

/**
 * @brief Records text, shaped once and replayed thereafter.
 *
 * The position is the leading edge of the baseline, which is what the text
 * pipeline works in.
 *
 * @param tree  The tree holding the node. Must not be NULL, and must have a
 *              font system.
 * @param node  A canvas that is recording.
 * @param font  A loaded font.
 * @param utf8  The text, NUL terminated.
 * @param x     Where the text starts.
 * @param y     The baseline.
 * @param paint What to draw the letters with. A gradient is measured across
 *              the run's own bounds, so it reads the same whatever the words
 *              are. An emoji keeps its own colours and ignores a gradient.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when there is no font
 *         system, or an error as above.
 */
int32_t schultz_canvas_text(schultz_tree *tree, schultz_handle node,
                            schultz_handle font, const char *utf8, float x,
                            float y, schultz_paint paint);

/**
 * @brief Records text drawn as an outline rather than filled in.
 *
 * The letters are taken from the font's own outlines and stroked, so this is
 * a heading with a rule round it rather than a heading. It costs more than
 * schultz_canvas_text, which draws from pictures of each letter the toolkit
 * keeps, so it is for the few words that want it rather than for prose.
 *
 * A glyph that is a picture rather than an outline, such as an emoji, cannot
 * be stroked and is left out.
 *
 * @param tree   The tree holding the node. Must not be NULL, and must have a
 *               font system.
 * @param node   A canvas that is recording.
 * @param font   A loaded font.
 * @param utf8   The text, NUL terminated.
 * @param x      Where the text starts.
 * @param y      The baseline.
 * @param stroke What the outline is drawn with.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when there is no font
 *         system, or an error as above.
 */
int32_t schultz_canvas_stroke_text(schultz_tree *tree, schultz_handle node,
                                   schultz_handle font, const char *utf8,
                                   float x, float y, schultz_stroke stroke);

/**
 * @brief Counts what a canvas is holding.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_canvas_create.
 * @return How many drawing operations were recorded.
 */
uint32_t schultz_canvas_count(const schultz_tree *tree, schultz_handle node);

/* ----------------------------------------------------------- containers */

/**
 * @brief Creates a group box: a titled panel with its title set into the
 *        border.
 *
 * The border runs around everything below the middle of the title, with a gap
 * where the title sits, which is what makes it read as a heading rather than
 * a caption above a box.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param title    The heading, copied. NULL means empty.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_group_box_create(schultz_tree *tree, schultz_handle parent,
                                 const char *title, schultz_handle *out_node);

/**
 * @brief Returns the node an application fills with a group box's contents.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node A node created by schultz_group_box_create.
 * @return The content node, or SCHULTZ_HANDLE_NONE when the node is not a
 *         group box.
 */
schultz_handle schultz_group_box_content(const schultz_tree *tree,
                                         schultz_handle node);

/**
 * @brief Creates a split pane: two halves and a divider between them.
 *
 * The divider captures the pointer while it is dragged, which is the
 * behaviour a pane cannot express and the reason this is a widget rather than
 * a layout. It starts in the middle.
 *
 * @param tree        The tree to create in. Must not be NULL.
 * @param parent      The parent node. Must name a live node.
 * @param orientation SCHULTZ_ORIENT_HORIZONTAL puts the halves side by side;
 *                    _VERTICAL stacks them.
 * @param out_node    Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_split_pane_create(schultz_tree *tree, schultz_handle parent,
                                  uint32_t orientation,
                                  schultz_handle *out_node);

/**
 * @brief Returns one of a split pane's two halves, to fill it.
 *
 * @param tree  The tree holding the node. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node  A node created by schultz_split_pane_create.
 * @param which 0 for the left or top half, anything else for the other.
 * @return The half, or SCHULTZ_HANDLE_NONE when the node is not a split pane.
 */
schultz_handle schultz_split_pane_half(const schultz_tree *tree,
                                       schultz_handle node, uint32_t which);

/**
 * @brief Moves a split pane's divider.
 *
 * @param tree     The tree holding the node. Must not be NULL.
 * @param node     A node created by schultz_split_pane_create.
 * @param position Where the divider sits, 0 at one end and 1 at the other.
 *                 Clamped.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         split pane.
 */
int32_t schultz_split_pane_set_position(schultz_tree *tree,
                                        schultz_handle node, float position);

/**
 * @brief Returns where a split pane's divider sits.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_split_pane_create.
 * @return The position, 0 to 1, or 0 when the node is not a split pane.
 */
float schultz_split_pane_position(const schultz_tree *tree,
                                  schultz_handle node);

/**
 * @brief Creates a tab view: a strip of buttons and one page showing.
 *
 * Pages that are not showing are hidden rather than laid out, which is the
 * behaviour a pane cannot express.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_tab_view_create(schultz_tree *tree, schultz_handle parent,
                                schultz_handle *out_node);

/**
 * @brief Adds a tab and gives back its page.
 *
 * The first tab added is the one showing.
 *
 * A page is a container the application arranges however it likes, so it has
 * no pane of its own: give it one, or place its children by hand. That is
 * unlike the other containers here, whose content nodes hold one thing and
 * size themselves around it.
 *
 * @param tree     The tree holding the node. Must not be NULL.
 * @param node     A node created by schultz_tab_view_create.
 * @param title    What the tab's button says, copied.
 * @param out_page Receives the page to fill. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a tab
 *         view, SCHULTZ_ERR_EXHAUSTED when it already holds as many tabs as
 *         it can, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_tab_view_add(schultz_tree *tree, schultz_handle node,
                             const char *title, schultz_handle *out_page);

/**
 * @brief Shows one of a tab view's pages.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_tab_view_create.
 * @param index Which page, counting from zero.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when there is no such tab,
 *         or SCHULTZ_ERR_INVALID_HANDLE when the node is not a tab view.
 */
int32_t schultz_tab_view_select(schultz_tree *tree, schultz_handle node,
                                uint32_t index);

/**
 * @brief Returns which of a tab view's pages is showing.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_tab_view_create.
 * @return The index, or 0 when the node is not a tab view.
 */
uint32_t schultz_tab_view_selected(const schultz_tree *tree,
                                   schultz_handle node);

/**
 * @brief Counts a tab view's tabs.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_tab_view_create.
 * @return How many tabs it holds, or 0 when the node is not a tab view.
 */
uint32_t schultz_tab_view_count(const schultz_tree *tree,
                                schultz_handle node);

/* ------------------------------------------------------------- overlays */

/**
 * @brief Creates a menu, hidden until it is opened.
 *
 * A menu hangs off the root rather than off whatever opens it, because an
 * overlay is its own layout root and its coordinates are the window's. It
 * captures while open, so a press anywhere else closes it and does not reach
 * what is underneath. Escape closes it too.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_menu_create(schultz_tree *tree, schultz_handle *out_node);

/** @brief What kind of row a menu item is. */
enum {
    SCHULTZ_MENU_ITEM_PLAIN = 0, /**< A choice. Closes the menu. */
    SCHULTZ_MENU_ITEM_CHECK,     /**< A tick that turns on and off. */
    SCHULTZ_MENU_ITEM_RADIO,     /**< A dot, one of a set. */
    SCHULTZ_MENU_ITEM_SEPARATOR, /**< A rule. Not a choice. */
    SCHULTZ_MENU_ITEM_CUSTOM,    /**< A row holding whatever it is given. */
    SCHULTZ_MENU_ITEM_SUBMENU    /**< A row that opens another menu. */
};

/**
 * @brief Adds a row that draws a tick when it is on.
 *
 * Choosing it flips the tick and **leaves the menu open**, since these are
 * the rows a user sets several of at once. Its state is read and written with
 * schultz_menu_item_checked and schultz_menu_item_set_checked.
 *
 * @param tree     The tree holding the menu. Must not be NULL.
 * @param menu     A node created by schultz_menu_create.
 * @param text     What the row says, copied.
 * @param shortcut Shortcut text shown on the right, or NULL.
 * @param checked  Nonzero to start ticked.
 * @param out_item Receives the row. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_menu_add_check(schultz_tree *tree, schultz_handle menu,
                               const char *text, const char *shortcut,
                               int32_t checked, schultz_handle *out_item);

/**
 * @brief Adds a row that is one of a set.
 *
 * A set is the rows sharing a menu and a group number, the same rule
 * schultz_radio_create uses with a parent. Choosing one clears the rest and
 * leaves the menu open.
 *
 * @param tree     The tree holding the menu. Must not be NULL.
 * @param menu     A node created by schultz_menu_create.
 * @param text     What the row says, copied.
 * @param shortcut Shortcut text shown on the right, or NULL.
 * @param group    Group number. Any value; only equality matters.
 * @param out_item Receives the row. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_menu_add_radio(schultz_tree *tree, schultz_handle menu,
                               const char *text, const char *shortcut,
                               uint32_t group, schultz_handle *out_item);

/**
 * @brief Chooses one radio row and clears the rest of its set.
 *
 * @param tree The tree holding the row. Must not be NULL.
 * @param item A row created by schultz_menu_add_radio.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_menu_radio_select(schultz_tree *tree, schultz_handle item);

/**
 * @brief Adds a rule between groups of rows.
 *
 * It takes no focus, so keyboard navigation steps over it, and it is
 * invisible to the pointer.
 *
 * @param tree     The tree holding the menu. Must not be NULL.
 * @param menu     A node created by schultz_menu_create.
 * @param out_item Receives the row, or NULL if it is not wanted.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_menu_add_separator(schultz_tree *tree, schultz_handle menu,
                                   schultz_handle *out_item);

/**
 * @brief Adds a row holding whatever the host puts in it.
 *
 * The returned node holds one thing and sizes itself around it. The row lays
 * out and highlights like any other.
 *
 * @param tree        The tree holding the menu. Must not be NULL.
 * @param menu        A node created by schultz_menu_create.
 * @param out_content Receives the node to fill. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_menu_add_custom(schultz_tree *tree, schultz_handle menu,
                                schultz_handle *out_content);

/**
 * @brief Adds a row that opens another menu beside it.
 *
 * The returned menu is filled with these same calls, so submenus nest as
 * deep as an application wants. It opens after the pointer has rested on the
 * row, and on the right arrow key.
 *
 * @param tree        The tree holding the menu. Must not be NULL.
 * @param menu        A node created by schultz_menu_create.
 * @param text        What the row says, copied.
 * @param out_submenu Receives the new menu. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT,
 *         SCHULTZ_ERR_INVALID_HANDLE, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_menu_add_submenu(schultz_tree *tree, schultz_handle menu,
                                 const char *text,
                                 schultz_handle *out_submenu);

/**
 * @brief Returns what kind of row an item is.
 *
 * @param tree The tree holding the row. NULL yields -1.
 * @param item A row added to a menu.
 * @return One of the SCHULTZ_MENU_ITEM_* values, or -1 when it is not a row.
 */
int32_t schultz_menu_item_kind(const schultz_tree *tree, schultz_handle item);

/**
 * @brief Returns a menu row's caption node.
 *
 * The same idea as schultz_button_label, for the one part of the menu that is
 * not a button: change the caption with schultz_label_set_text, or style the
 * text on its own.
 *
 * A separator and a custom row have a caption node like any other row; it is
 * simply empty on a separator, and a custom row's own content sits beside it.
 *
 * @param tree The tree holding the row. NULL yields SCHULTZ_HANDLE_NONE.
 * @param item A row added to a menu.
 * @return The Label child, or SCHULTZ_HANDLE_NONE when the node is not a menu
 *         row.
 */
schultz_handle schultz_menu_item_label(const schultz_tree *tree,
                                       schultz_handle item);

/**
 * @brief Reports whether a check or radio row is on.
 *
 * @param tree The tree holding the row. NULL yields 0.
 * @param item A row added to a menu.
 * @return 1 when ticked, 0 otherwise.
 */
int32_t schultz_menu_item_checked(const schultz_tree *tree,
                                  schultz_handle item);

/**
 * @brief Turns a check or radio row on or off.
 *
 * This sets one row and does not clear a radio's set; use
 * schultz_menu_radio_select for that.
 *
 * @param tree    The tree holding the row. Must not be NULL.
 * @param item    A row added to a menu.
 * @param checked Nonzero to tick it.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_menu_item_set_checked(schultz_tree *tree, schultz_handle item,
                                      int32_t checked);

/**
 * @brief Adds a row to a menu.
 *
 * Choosing the row closes the menu, and the click still reaches the host,
 * which is what tells it what was chosen.
 *
 * @param tree     The tree holding the menu. Must not be NULL.
 * @param menu     A node created by schultz_menu_create.
 * @param text     What the row says, copied.
 * @param shortcut Shortcut text shown on the right, copied. NULL for none.
 *                 This is what the row says, not what the key does; register
 *                 the key itself with schultz_events_add_accelerator.
 * @param out_item Receives the row. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a menu,
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_menu_add(schultz_tree *tree, schultz_handle menu,
                         const char *text, const char *shortcut,
                         schultz_handle *out_item);

/**
 * @brief Opens a menu beside a rectangle.
 *
 * @param tree      The tree holding the menu. Must not be NULL.
 * @param menu      A node created by schultz_menu_create.
 * @param anchor    What to sit beside, in window coordinates.
 * @param placement One of the SCHULTZ_PLACE_* values.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_menu_open(schultz_tree *tree, schultz_handle menu,
                          schultz_rect anchor, uint32_t placement);

/**
 * @brief Opens a menu at a point, which is what a right click wants.
 *
 * @param tree  The tree holding the menu. Must not be NULL.
 * @param menu  A node created by schultz_menu_create.
 * @param point Where the corner of the menu goes, in window coordinates.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_menu_open_at(schultz_tree *tree, schultz_handle menu,
                             schultz_point point);

/**
 * @brief Opens a menu beside a node, which is what a menu button wants.
 *
 * @param tree      The tree holding both. Must not be NULL.
 * @param menu      A node created by schultz_menu_create.
 * @param anchor    The node to sit beside.
 * @param placement One of the SCHULTZ_PLACE_* values.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_menu_open_for(schultz_tree *tree, schultz_handle menu,
                              schultz_handle anchor, uint32_t placement);

/**
 * @brief Closes a menu.
 *
 * @param tree The tree holding the menu. Must not be NULL.
 * @param menu A node created by schultz_menu_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_menu_close(schultz_tree *tree, schultz_handle menu);

/**
 * @brief Reports whether a menu is showing.
 *
 * @param tree The tree holding the menu. NULL yields 0.
 * @param menu A node created by schultz_menu_create.
 * @return 1 when open, 0 otherwise.
 */
int32_t schultz_menu_is_open(const schultz_tree *tree, schultz_handle menu);

/**
 * @brief Creates a menu button: a button that opens a menu when clicked.
 *
 * The click never reaches the host; the menu is what it is for. Fill the menu
 * with the ordinary menu calls.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param text     The caption, copied.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_menu_button_create(schultz_tree *tree, schultz_handle parent,
                                   const char *text,
                                   schultz_handle *out_node);

/**
 * @brief Creates a split menu button: a button beside an arrow.
 *
 * Clicking the wide part reports a click to the host like an ordinary button.
 * Clicking the arrow opens the menu instead.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param text     The caption, copied.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_split_menu_button_create(schultz_tree *tree,
                                         schultz_handle parent,
                                         const char *text,
                                         schultz_handle *out_node);

/**
 * @brief Returns the menu a menu button owns, to fill it.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node A node created by either menu button call.
 * @return The menu, or SCHULTZ_HANDLE_NONE.
 */
schultz_handle schultz_menu_button_menu(const schultz_tree *tree,
                                        schultz_handle node);

/**
 * @brief Creates a menu bar: a row of titles, each dropping a menu.
 *
 * Clicking a title opens its menu. **Once one is open, moving along the bar
 * opens the next without a click**, and clicking the open one closes it.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_menu_bar_create(schultz_tree *tree, schultz_handle parent,
                                schultz_handle *out_node);

/**
 * @brief Adds a title to a menu bar and returns the menu it drops.
 *
 * @param tree     The tree holding the bar. Must not be NULL.
 * @param node     A node created by schultz_menu_bar_create.
 * @param text     The title, copied.
 * @param out_menu Receives the new menu. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_menu_bar_add(schultz_tree *tree, schultz_handle node,
                             const char *text, schultz_handle *out_menu);

/**
 * @brief Closes whichever of a bar's menus is showing.
 *
 * @param tree The tree holding the bar. Must not be NULL.
 * @param node A node created by schultz_menu_bar_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_menu_bar_close(schultz_tree *tree, schultz_handle node);

/**
 * @brief Creates a toolbar: a row of actions with consistent spacing.
 *
 * When the row is too narrow, the items that do not fit are hidden and a
 * trailing button appears that opens them as a menu. Each overflow row
 * carries the hidden item's own token, so a host hears the same thing
 * whichever way the action was reached.
 *
 * @param tree        The tree to create in. Must not be NULL.
 * @param parent      The parent node. Must name a live node.
 * @param orientation SCHULTZ_ORIENT_HORIZONTAL or _VERTICAL.
 * @param out_node    Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_toolbar_create(schultz_tree *tree, schultz_handle parent,
                               uint32_t orientation,
                               schultz_handle *out_node);

/**
 * @brief Adds a button to a toolbar.
 *
 * @param tree       The tree holding the toolbar. Must not be NULL.
 * @param node       A node created by schultz_toolbar_create.
 * @param text       The caption, copied.
 * @param out_button Receives the button, or NULL if it is not wanted.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_toolbar_add(schultz_tree *tree, schultz_handle node,
                            const char *text, schultz_handle *out_button);

/**
 * @brief Adds a rule between groups of toolbar items.
 *
 * @param tree     The tree holding the toolbar. Must not be NULL.
 * @param node     A node created by schultz_toolbar_create.
 * @param out_node Receives the rule, or NULL if it is not wanted.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_toolbar_add_separator(schultz_tree *tree, schultz_handle node,
                                      schultz_handle *out_node);

/**
 * @brief Turns collapsing into an overflow menu on or off. On by default.
 *
 * With it off, items that do not fit are simply clipped.
 *
 * @param tree The tree holding the toolbar. Must not be NULL.
 * @param node A node created by schultz_toolbar_create.
 * @param on   Nonzero to collapse what does not fit.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_toolbar_set_overflow_enabled(schultz_tree *tree,
                                             schultz_handle node, int32_t on);

/**
 * @brief Returns how many items are currently collapsed into the overflow.
 *
 * @param tree The tree holding the toolbar. NULL yields 0.
 * @param node A node created by schultz_toolbar_create.
 * @return The count, which is 0 when everything fits.
 */
uint32_t schultz_toolbar_overflow_count(const schultz_tree *tree,
                                        schultz_handle node);

/**
 * @brief Creates a status bar: a message that stretches, and sections.
 *
 * The message area takes whatever the sections leave, so the sections sit
 * against the far end.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_status_bar_create(schultz_tree *tree, schultz_handle parent,
                                  schultz_handle *out_node);

/**
 * @brief Sets the message a status bar shows when nothing is flashed.
 *
 * Setting it while a flash is showing replaces what the flash reverts to and
 * leaves the flash alone.
 *
 * @param tree The tree holding the bar. Must not be NULL.
 * @param node A node created by schultz_status_bar_create.
 * @param text The message, copied. NULL means empty.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_status_bar_set_message(schultz_tree *tree,
                                       schultz_handle node, const char *text);

/**
 * @brief Shows a message for a while, then goes back to the standing one.
 *
 * The countdown runs on the tree's clock, so a host that stops drawing frames
 * stops the revert with it.
 *
 * @param tree The tree holding the bar. Must not be NULL.
 * @param node A node created by schultz_status_bar_create.
 * @param text The temporary message, copied. NULL means empty.
 * @param ms   How long to show it. Must be greater than zero.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_status_bar_flash(schultz_tree *tree, schultz_handle node,
                                 const char *text, uint32_t ms);

/**
 * @brief Returns the message a status bar is showing right now.
 *
 * While a flash is up this is the flashed text, not the standing one.
 *
 * @param tree The tree holding the bar. NULL yields NULL.
 * @param node A node created by schultz_status_bar_create.
 * @return The text, or NULL when the node is not a status bar.
 */
const char *schultz_status_bar_message(const schultz_tree *tree,
                                       schultz_handle node);

/**
 * @brief Moves an existing node into a status bar as a fixed size section.
 *
 * @param tree  The tree holding the bar. Must not be NULL.
 * @param node  A node created by schultz_status_bar_create.
 * @param child The node to place. Must name a live node.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_status_bar_add_section(schultz_tree *tree,
                                       schultz_handle node,
                                       schultz_handle child);

/**
 * @brief Creates an accordion: a column of sections that open and close.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_accordion_create(schultz_tree *tree, schultz_handle parent,
                                 schultz_handle *out_node);

/**
 * @brief Adds a titled section and returns the node to fill.
 *
 * Sections start closed. The header returned by
 * schultz_node_child_at on the accordion is what
 * schultz_accordion_expand takes.
 *
 * @param tree        The tree holding the accordion. Must not be NULL.
 * @param node        A node created by schultz_accordion_create.
 * @param title       The header text, copied.
 * @param out_content Receives the node to fill. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_accordion_add(schultz_tree *tree, schultz_handle node,
                              const char *title, schultz_handle *out_content);

/**
 * @brief Opens or closes one section.
 *
 * With single expand on, opening one closes whichever was open.
 *
 * @param tree     The tree holding the accordion. Must not be NULL.
 * @param node     A node created by schultz_accordion_create.
 * @param header   The section's header node.
 * @param expanded Nonzero to open it.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_accordion_expand(schultz_tree *tree, schultz_handle node,
                                 schultz_handle header, int32_t expanded);

/**
 * @brief Reports whether a section is open.
 *
 * @param tree   The tree holding the section. NULL yields 0.
 * @param header The section's header node.
 * @return 1 when open, 0 otherwise.
 */
int32_t schultz_accordion_is_expanded(const schultz_tree *tree,
                                      schultz_handle header);

/**
 * @brief Chooses whether more than one section may be open. Off by default.
 *
 * @param tree The tree holding the accordion. Must not be NULL.
 * @param node A node created by schultz_accordion_create.
 * @param on   Nonzero to allow only one open at a time.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_accordion_set_single_expand(schultz_tree *tree,
                                            schultz_handle node, int32_t on);

/**
 * @brief Creates a popup: a panel that floats over the page, anchored to a
 *        node.
 *
 * This is the plain one, and the thing the others are made of. It looks like
 * a panel a step above the page, with square corners and no triangle, which
 * is what a dropdown or a menu wants. The date, time and colour pickers all
 * open one.
 *
 * A popover and a tooltip are popups too, made by the two calls below. They
 * differ only in how they are dressed; everything after they are made is
 * done with the popup calls, because they are popups. There is one set of
 * verbs here and several ways to start.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param captures Nonzero to close on a press outside and swallow it, which
 *                 is what a chooser wants; zero to let presses through.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_popup_create(schultz_tree *tree, int32_t captures,
                             schultz_handle *out_node);

/**
 * @brief Creates a popover: a popup that points at what it belongs to.
 *
 * A bubble rather than a panel. It carries a triangle aimed at the node it
 * was opened against, sits centred on that node, and is drawn in the colours
 * opposite the page so that it reads as being in front of the page rather
 * than part of it.
 *
 * Drive it with the popup calls: schultz_popup_open, _close, _content and
 * the rest all take one of these, because one of these is a popup.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param captures Nonzero to close on a press outside and swallow it; zero
 *                 to let presses through.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_popover_create(schultz_tree *tree, int32_t captures,
                               schultz_handle *out_node);

/**
 * @brief Creates a tooltip: a small non capturing panel holding one line.
 *
 * Show and hide it yourself with schultz_popup_open and _close, or hand it
 * a node to watch with schultz_tooltip_watch and let it decide.
 *
 * It is invisible to the pointer throughout, since it sits over the thing it
 * explains and one that took a click would make that thing unclickable. A
 * panel that can be clicked is a popover.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param text     What it says, copied.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_tooltip_create(schultz_tree *tree, const char *text,
                               schultz_handle *out_node);

/**
 * @brief Has a tooltip watch a node and show itself when the pointer rests.
 *
 * The tooltip appears once the pointer has been over the node for the delay,
 * and goes away as soon as it leaves. It reads the hover flag the router
 * already keeps, so watching costs nothing at the routing layer, and it needs
 * the tree's clock: a host that never calls schultz_tree_advance will never
 * see it appear.
 *
 * @param tree     The tree holding both. Must not be NULL.
 * @param node     A tooltip, or any popover.
 * @param anchor   The node to watch, or SCHULTZ_HANDLE_NONE to stop watching.
 * @param delay_ms How long the pointer must rest before it shows.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         popover, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_tooltip_watch(schultz_tree *tree, schultz_handle node,
                              schultz_handle anchor, uint32_t delay_ms);

/**
 * @brief Returns the node an application fills with a popover's contents.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node A popover or tooltip node.
 * @return The content node, or SCHULTZ_HANDLE_NONE when the node is neither.
 */
schultz_handle schultz_popup_content(const schultz_tree *tree,
                                       schultz_handle node);

/**
 * @brief Turns a popover's little triangle on or off.
 *
 * On for a popover, because a bubble that points at what it belongs to is
 * how a reader knows which control it is about. Off for a plain panel
 * dropped under a control, such as the calendar a date picker opens, where
 * the panel is plainly the field's and an arrow only adds noise.
 *
 * The triangle lives inside the popover's own bounds, on whichever side it
 * ends up on, so the content sits clear of it and nothing has to be told to
 * repaint a wider area.
 *
 * It also decides how the popover lines up. A bubble is centred on what it
 * points at, because the triangle has to come out of the middle of it; a
 * panel with no triangle lines up with the leading edge of the control, the
 * way a dropdown does.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_popup_create.
 * @param arrow Nonzero to draw one, zero for a plain box.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_popup_set_arrow(schultz_tree *tree, schultz_handle node,
                                  int32_t arrow);

/**
 * @brief How far a popover or a tooltip sits from what it belongs to.
 *
 * Four by default. One setting for the whole toolkit rather than one per
 * node, the way the toast's edge is, because it is a clearance rather than a
 * decision about one widget.
 *
 * @param gap The clearance. Zero sits flush; negative is refused.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_popup_set_gap(float gap);

/**
 * @brief Shows a popover beside something, where it belongs.
 *
 * Above what it was opened against and centred on it, with a little clearance
 * and its triangle pointing back down at it. That way the control being
 * talked about stays under the reader's eye instead of being covered by the
 * bubble talking about it.
 *
 * A tooltip goes below instead, out of the way of the pointer resting on the
 * widget, and lines up with the pointer rather than with the widget's corner.
 *
 * Either flips to the other side sooner than be cut off by the edge of the
 * window, and only then: a popover with room above it is always above.
 *
 * Use schultz_popup_open_at to name a side yourself.
 *
 * @param tree   The tree holding both. Must not be NULL.
 * @param node   A node created by schultz_popup_create.
 * @param anchor What it belongs to. Must name a live node.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_popup_open(schultz_tree *tree, schultz_handle node,
                             schultz_handle anchor);

/**
 * @brief Shows a popover on a side you name.
 *
 * For a panel that belongs somewhere in particular whatever the toolkit would
 * have chosen: the calendar a date picker drops under its field is below,
 * because that is where a dropdown goes.
 *
 * It still moves aside rather than off the screen, and still flips when the
 * side named does not fit and the other one does.
 *
 * @param tree      The tree holding both. Must not be NULL.
 * @param node      A node created by schultz_popup_create.
 * @param anchor    What it belongs to. Must name a live node.
 * @param placement One of SCHULTZ_PLACE_*.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_popup_open_at(schultz_tree *tree, schultz_handle node,
                                schultz_handle anchor, uint32_t placement);

/**
 * @brief Closes a popover or tooltip.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A popover or tooltip node.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_popup_close(schultz_tree *tree, schultz_handle node);

/**
 * @brief Reports whether a popover or tooltip is showing.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A popover or tooltip node.
 * @return 1 when open, 0 otherwise.
 */
int32_t schultz_popup_is_open(const schultz_tree *tree,
                                schultz_handle node);

/**
 * @brief Creates a dialog: a titled panel over a scrim covering the window.
 *
 * The scrim dims what is behind and swallows every press that misses the
 * panel, which is what modal means. A press beside the panel does not close
 * it: a dialog is asking a question, and clicking elsewhere is not an answer.
 * Escape does close it.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param title    The heading, copied. NULL means none.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, or an error as above.
 */
int32_t schultz_dialog_create(schultz_tree *tree, const char *title,
                              schultz_handle *out_node);

/**
 * @brief Returns the node an application fills with a dialog's contents.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node A node created by schultz_dialog_create.
 * @return The content node, or SCHULTZ_HANDLE_NONE when the node is not a
 *         dialog.
 */
schultz_handle schultz_dialog_content(const schultz_tree *tree,
                                      schultz_handle node);

/**
 * @brief Opens a dialog, covering the window.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_dialog_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_dialog_open(schultz_tree *tree, schultz_handle node);

/**
 * @brief Closes a dialog.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_dialog_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_dialog_close(schultz_tree *tree, schultz_handle node);

/**
 * @brief Reports whether a dialog is showing.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_dialog_create.
 * @return 1 when open, 0 otherwise.
 */
int32_t schultz_dialog_is_open(const schultz_tree *tree, schultz_handle node);

/* ------------------------------------------------------------- ComboBox */

/** @brief Which picture a message dialog shows beside its message. */
enum {
    SCHULTZ_DIALOG_ICON_NONE = 0,
    SCHULTZ_DIALOG_ICON_INFO,
    SCHULTZ_DIALOG_ICON_WARNING,
    SCHULTZ_DIALOG_ICON_ERROR,
    SCHULTZ_DIALOG_ICON_QUESTION
};

/** @brief Which set of buttons a message dialog offers. */
enum {
    SCHULTZ_DIALOG_OK = 0,      /**< One button that dismisses. */
    SCHULTZ_DIALOG_OK_CANCEL,   /**< Accept or dismiss. */
    SCHULTZ_DIALOG_YES_NO,      /**< A question with two answers. */
    SCHULTZ_DIALOG_YES_NO_CANCEL /**< A question that can be backed out of. */
};

/**
 * @brief Creates a message dialog: an icon, a message, and standard buttons.
 *
 * Built on schultz_dialog_create, so it hangs off the root and dims what is
 * behind it. The buttons are laid out by a button bar, so they sit where the
 * platform expects.
 *
 * Enter answers with OK and Escape with Cancel, or with OK when OK is the
 * only button there is.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param title    The dialog's title, copied.
 * @param icon     One of the SCHULTZ_DIALOG_ICON_* values.
 * @param buttons  One of the SCHULTZ_DIALOG_* button set values.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_message_dialog_create(schultz_tree *tree, const char *title,
                                      uint32_t icon, uint32_t buttons,
                                      schultz_handle *out_node);

/**
 * @brief Sets the message and the smaller line under it.
 *
 * An empty or NULL detail takes no room rather than leaving a blank line.
 *
 * @param tree    The tree holding the dialog. Must not be NULL.
 * @param node    A node created by any of the three dialog calls.
 * @param message The main line, copied.
 * @param detail  The second line, copied, or NULL for none.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_message_dialog_set_text(schultz_tree *tree,
                                        schultz_handle node,
                                        const char *message,
                                        const char *detail);

/**
 * @brief Shows the dialog and clears any previous answer.
 *
 * **Put the keyboard focus inside the dialog after opening it**, with
 * schultz_events_set_focus on one of its buttons. Enter and Escape reach the
 * dialog by rising from the focused node, so a dialog nothing inside is
 * focused in answers to neither. The toolkit cannot do this itself, because
 * focus lives on the event router rather than on the tree.
 *
 * @param tree The tree holding the dialog. Must not be NULL.
 * @param node A node created by any of the three dialog calls.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_message_dialog_open(schultz_tree *tree, schultz_handle node);

/**
 * @brief Returns which button answered the dialog.
 *
 * @param tree The tree holding the dialog. NULL yields
 *             SCHULTZ_BUTTON_ROLE_COUNT.
 * @param node A node created by any of the three dialog calls.
 * @return One of the SCHULTZ_BUTTON_ROLE_* values, or
 *         SCHULTZ_BUTTON_ROLE_COUNT while it is still unanswered.
 */
uint32_t schultz_message_dialog_result(const schultz_tree *tree,
                                       schultz_handle node);

/**
 * @brief Returns which icon a dialog was created with.
 *
 * @param tree The tree holding the dialog. NULL yields
 *             SCHULTZ_DIALOG_ICON_NONE.
 * @param node A node created by any of the three dialog calls.
 * @return One of the SCHULTZ_DIALOG_ICON_* values.
 */
uint32_t schultz_message_dialog_icon(const schultz_tree *tree,
                                     schultz_handle node);

/**
 * @brief Creates a message dialog holding one text field.
 *
 * OK does nothing while the field is empty, unless
 * schultz_text_input_dialog_set_allow_empty says otherwise.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param title    The dialog's title, copied.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_text_input_dialog_create(schultz_tree *tree,
                                         const char *title,
                                         schultz_handle *out_node);

/**
 * @brief Sets what the field starts with.
 *
 * @param tree The tree holding the dialog. Must not be NULL.
 * @param node A node created by schultz_text_input_dialog_create.
 * @param text The starting text, copied.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_text_input_dialog_set_value(schultz_tree *tree,
                                            schultz_handle node,
                                            const char *text);

/**
 * @brief Returns what the field holds.
 *
 * @param tree The tree holding the dialog. NULL yields NULL.
 * @param node A node created by schultz_text_input_dialog_create.
 * @return The text, owned by the field.
 */
const char *schultz_text_input_dialog_value(const schultz_tree *tree,
                                            schultz_handle node);

/**
 * @brief Allows an empty answer to be accepted. Off by default.
 *
 * @param tree The tree holding the dialog. Must not be NULL.
 * @param node A node created by schultz_text_input_dialog_create.
 * @param on   Nonzero to accept empty text.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_text_input_dialog_set_allow_empty(schultz_tree *tree,
                                                  schultz_handle node,
                                                  int32_t on);

/**
 * @brief Creates a message dialog holding one list of choices.
 *
 * The choices are shown in a combo box.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param title    The dialog's title, copied.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_choice_dialog_create(schultz_tree *tree, const char *title,
                                     schultz_handle *out_node);

/**
 * @brief Adds one choice to the end of the list.
 *
 * @param tree The tree holding the dialog. Must not be NULL.
 * @param node A node created by schultz_choice_dialog_create.
 * @param text The choice, copied.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_choice_dialog_add(schultz_tree *tree, schultz_handle node,
                                  const char *text);

/**
 * @brief Chooses which entry starts selected.
 *
 * @param tree  The tree holding the dialog. Must not be NULL.
 * @param node  A node created by schultz_choice_dialog_create.
 * @param index Zero based index into the choices.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_choice_dialog_select(schultz_tree *tree, schultz_handle node,
                                     uint32_t index);

/**
 * @brief Returns which choice is selected.
 *
 * @param tree The tree holding the dialog. NULL yields 0.
 * @param node A node created by schultz_choice_dialog_create.
 * @return The zero based index.
 */
uint32_t schultz_choice_dialog_selected(const schultz_tree *tree,
                                        schultz_handle node);

/**
 * @brief Creates a busy indicator: a spinning arc for work of unknown length.
 *
 * Beside schultz_progress_bar_create, which is for work whose length is
 * known. It asks to be ticked only while it is running, so a stopped one
 * costs nothing.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_busy_indicator_create(schultz_tree *tree,
                                      schultz_handle parent,
                                      schultz_handle *out_node);

/**
 * @brief Starts the arc turning.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_busy_indicator_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_busy_indicator_start(schultz_tree *tree, schultz_handle node);

/**
 * @brief Stops it, and stops it asking to be ticked.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_busy_indicator_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_busy_indicator_stop(schultz_tree *tree, schultz_handle node);

/**
 * @brief Reports whether the arc is turning.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_busy_indicator_create.
 * @return 1 while running, 0 otherwise.
 */
int32_t schultz_busy_indicator_is_running(const schultz_tree *tree,
                                          schultz_handle node);

/**
 * @brief Sets how big the arc is drawn.
 *
 * @param tree     The tree holding the node. Must not be NULL.
 * @param node     A node created by schultz_busy_indicator_create.
 * @param diameter Width and height in pixels. Must be above zero.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_busy_indicator_set_size(schultz_tree *tree,
                                        schultz_handle node, float diameter);

/** @brief Which edge toasts stack against. */
enum {
    SCHULTZ_TOAST_BOTTOM = 0, /**< Along the bottom, the usual place. */
    SCHULTZ_TOAST_TOP         /**< Along the top. */
};

/**
 * @brief Shows a brief message that takes itself away again.
 *
 * The toast hangs off the root, sits centred against whichever edge was
 * chosen, and never takes the pointer or the keyboard. Several showing at
 * once stack rather than overlapping.
 *
 * The countdown runs on the tree's clock, so a host that stops drawing
 * frames stops the toast going away with it.
 *
 * A toast whose time is up is hidden rather than destroyed, and the next
 * call reuses it, so showing a hundred toasts does not build a hundred
 * nodes.
 *
 * @param tree     The tree to show it in. Must not be NULL.
 * @param text     What it says, copied.
 * @param ms       How long to show it. Must be greater than zero.
 * @param out_node Receives the toast, or NULL if it is not wanted.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_toast_show(schultz_tree *tree, const char *text, uint32_t ms,
                           schultz_handle *out_node);

/**
 * @brief Takes a toast away before its time is up.
 *
 * @param tree The tree holding the toast. Must not be NULL.
 * @param node A toast returned by schultz_toast_show.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_toast_dismiss(schultz_tree *tree, schultz_handle node);

/**
 * @brief Reports whether a toast is still showing.
 *
 * @param tree The tree holding the toast. NULL yields 0.
 * @param node A toast returned by schultz_toast_show.
 * @return 1 while showing, 0 once its time is up.
 */
int32_t schultz_toast_is_showing(const schultz_tree *tree,
                                 schultz_handle node);

/**
 * @brief Chooses which edge toasts stack against.
 *
 * This is one setting for the whole toolkit rather than one per toast,
 * because toasts stack together and a stack has one direction.
 *
 * @param position SCHULTZ_TOAST_BOTTOM or SCHULTZ_TOAST_TOP.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_toast_set_position(uint32_t position);

/**
 * @brief Creates pagination: steps and numbers for moving between pages.
 *
 * First, previous, next and last around a run of numbered buttons, with the
 * current page marked. A step that cannot go anywhere is disabled.
 *
 * When there are more pages than there is room for numbers, the run follows
 * the current page, and the far ends keep naming the first and last page with
 * an ellipsis where the run was cut.
 *
 * The page count and the current page are the whole of its state. What is on
 * a page is the application's business; it listens for the change and shows
 * what it likes.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param pages    How many pages there are. Zero is treated as one.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_pagination_create(schultz_tree *tree, schultz_handle parent,
                                  uint32_t pages, schultz_handle *out_node);

/**
 * @brief Says how many pages there are now.
 *
 * A current page past the new end moves back to the last one.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_pagination_create.
 * @param pages How many pages. Zero is treated as one.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_pagination_set_page_count(schultz_tree *tree,
                                          schultz_handle node,
                                          uint32_t pages);

/**
 * @brief Moves to a page. An index past the end is clamped to the last.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_pagination_create.
 * @param index Zero based page number.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_pagination_set_current(schultz_tree *tree,
                                       schultz_handle node, uint32_t index);

/**
 * @brief Returns which page is showing, zero based.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_pagination_create.
 * @return The current page.
 */
uint32_t schultz_pagination_current(const schultz_tree *tree,
                                    schultz_handle node);

/**
 * @brief Returns how many pages there are.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_pagination_create.
 * @return The page count.
 */
uint32_t schultz_pagination_page_count(const schultz_tree *tree,
                                       schultz_handle node);

/** @brief How many rows may be chosen at once in a list or a tree. */
enum {
    SCHULTZ_SELECT_NONE = 0,  /**< Nothing may be chosen. */
    SCHULTZ_SELECT_SINGLE,    /**< One at a time. The default. */
    SCHULTZ_SELECT_MULTIPLE   /**< Any number, with control and shift. */
};

/**
 * @brief Creates a list view: a scrolling column of rows the host fills.
 *
 * It owns no data model and does not recycle rows. A row is a node, and it
 * exists for as long as it is in the list, which is what keeps it simple and
 * what puts a ceiling on how many rows are sensible. A few thousand is
 * comfortable and ten thousand still works; past that a list wants recycling,
 * which this is not.
 *
 * Clicking chooses. In multiple mode, control clicking toggles one and shift
 * clicking takes the range from the anchor, which is the last row chosen
 * without shift. The arrow keys, home, end and the page keys move the choice
 * and scroll it into view; space toggles in multiple mode.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_list_view_create(schultz_tree *tree, schultz_handle parent,
                                 schultz_handle *out_node);

/**
 * @brief Adds an empty row at the end and returns it to fill.
 *
 * The row holds one thing and sizes itself around it, so put a label, a
 * layout, or anything else in it.
 *
 * @param tree    The tree holding the list. Must not be NULL.
 * @param node    A node created by schultz_list_view_create.
 * @param out_row Receives the new row. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_list_view_add(schultz_tree *tree, schultz_handle node,
                              schultz_handle *out_row);

/**
 * @brief Removes one row and everything in it.
 *
 * The row takes its own selection with it, and the anchor goes back to the
 * start rather than pointing at whatever slid into the gap.
 *
 * @param tree  The tree holding the list. Must not be NULL.
 * @param node  A node created by schultz_list_view_create.
 * @param index Zero based row number.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_list_view_remove(schultz_tree *tree, schultz_handle node,
                                 uint32_t index);

/**
 * @brief Removes every row.
 *
 * @param tree The tree holding the list. Must not be NULL.
 * @param node A node created by schultz_list_view_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_list_view_clear(schultz_tree *tree, schultz_handle node);

/**
 * @brief Returns how many rows there are.
 *
 * @param tree The tree holding the list. NULL yields 0.
 * @param node A node created by schultz_list_view_create.
 * @return The row count.
 */
uint32_t schultz_list_view_count(const schultz_tree *tree,
                                 schultz_handle node);

/**
 * @brief Returns one row by position.
 *
 * @param tree  The tree holding the list. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node  A node created by schultz_list_view_create.
 * @param index Zero based row number.
 * @return The row, or SCHULTZ_HANDLE_NONE when there is none there.
 */
schultz_handle schultz_list_view_row(const schultz_tree *tree,
                                     schultz_handle node, uint32_t index);

/**
 * @brief Chooses how many rows may be selected at once.
 *
 * Setting it to none clears whatever was selected.
 *
 * @param tree The tree holding the list. Must not be NULL.
 * @param node A node created by schultz_list_view_create.
 * @param mode One of the SCHULTZ_SELECT_* values.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_list_view_set_selection_mode(schultz_tree *tree,
                                             schultz_handle node,
                                             uint32_t mode);

/**
 * @brief Selects one row, clearing the rest, and scrolls it into view.
 *
 * @param tree  The tree holding the list. Must not be NULL.
 * @param node  A node created by schultz_list_view_create.
 * @param index Zero based row number.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_list_view_select(schultz_tree *tree, schultz_handle node,
                                 uint32_t index);

/**
 * @brief Selects a run of rows, replacing whatever was selected.
 *
 * Multiple mode only. The first row becomes the anchor, so a following shift
 * click extends from there.
 *
 * @param tree  The tree holding the list. Must not be NULL.
 * @param node  A node created by schultz_list_view_create.
 * @param first One end of the run.
 * @param last  The other end. Either order works.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when out of range or not
 *         in multiple mode, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_list_view_select_range(schultz_tree *tree,
                                       schultz_handle node, uint32_t first,
                                       uint32_t last);

/**
 * @brief Clears one row's selection, leaving the rest alone.
 *
 * @param tree  The tree holding the list. Must not be NULL.
 * @param node  A node created by schultz_list_view_create.
 * @param index Zero based row number.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_list_view_deselect(schultz_tree *tree, schultz_handle node,
                                   uint32_t index);

/**
 * @brief Reports whether one row is selected.
 *
 * @param tree  The tree holding the list. NULL yields 0.
 * @param node  A node created by schultz_list_view_create.
 * @param index Zero based row number.
 * @return 1 when selected, 0 otherwise.
 */
int32_t schultz_list_view_is_selected(const schultz_tree *tree,
                                      schultz_handle node, uint32_t index);

/**
 * @brief Returns how many rows are selected.
 *
 * @param tree The tree holding the list. NULL yields 0.
 * @param node A node created by schultz_list_view_create.
 * @return The count.
 */
uint32_t schultz_list_view_selected_count(const schultz_tree *tree,
                                          schultz_handle node);

/**
 * @brief Returns the first selected row.
 *
 * @param tree The tree holding the list. NULL yields -1.
 * @param node A node created by schultz_list_view_create.
 * @return The zero based row number, or -1 when nothing is selected.
 */
int32_t schultz_list_view_selected(const schultz_tree *tree,
                                   schultz_handle node);

/**
 * @brief Returns the nth selected row, for reading a whole selection.
 *
 * @param tree The tree holding the list. NULL yields -1.
 * @param node A node created by schultz_list_view_create.
 * @param n    Which selected row to report, counting from zero.
 * @return The zero based row number, or -1 when there are fewer than that.
 */
int32_t schultz_list_view_selected_at(const schultz_tree *tree,
                                      schultz_handle node, uint32_t n);

/**
 * @brief Scrolls a row into view without selecting it.
 *
 * @param tree  The tree holding the list. Must not be NULL.
 * @param node  A node created by schultz_list_view_create.
 * @param index Zero based row number.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_list_view_scroll_to(schultz_tree *tree, schultz_handle node,
                                    uint32_t index);

/**
 * @brief Creates a tree view: a list view whose rows hold rows.
 *
 * The hierarchy is the node tree rather than a model beside it. A row is a
 * header and a column of child rows; collapsing hides the column, and
 * indentation comes from depth. A row with no children draws no triangle.
 *
 * Rows are named by handle rather than by position, because a row's position
 * changes whenever anything above it opens or closes.
 *
 * The up and down keys walk the rows a reader can see, skipping what is
 * closed. Right opens a closed row and steps into an open one; left closes an
 * open row and steps out of a closed one.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_tree_view_create(schultz_tree *tree, schultz_handle parent,
                                 schultz_handle *out_node);

/**
 * @brief Adds a row and returns the node to fill.
 *
 * Rows start closed. The returned node is what every other tree view call
 * takes back.
 *
 * @param tree       The tree holding the view. Must not be NULL.
 * @param node       A node created by schultz_tree_view_create.
 * @param parent_row The row to add under, or SCHULTZ_HANDLE_NONE for the top
 *                   level.
 * @param out_row    Receives the new row. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_tree_view_add(schultz_tree *tree, schultz_handle node,
                              schultz_handle parent_row,
                              schultz_handle *out_row);

/**
 * @brief Removes a row and everything under it.
 *
 * @param tree The tree holding the view. Must not be NULL.
 * @param node A node created by schultz_tree_view_create.
 * @param row  A row returned by schultz_tree_view_add.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_tree_view_remove(schultz_tree *tree, schultz_handle node,
                                 schultz_handle row);

/**
 * @brief Opens or closes a row.
 *
 * @param tree     The tree holding the view. Must not be NULL.
 * @param node     A node created by schultz_tree_view_create.
 * @param row      A row returned by schultz_tree_view_add.
 * @param expanded Nonzero to open it.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_tree_view_expand(schultz_tree *tree, schultz_handle node,
                                 schultz_handle row, int32_t expanded);

/**
 * @brief Reports whether a row is open.
 *
 * @param tree The tree holding the row. NULL yields 0.
 * @param row  A row returned by schultz_tree_view_add.
 * @return 1 when open, 0 otherwise.
 */
int32_t schultz_tree_view_is_expanded(const schultz_tree *tree,
                                      schultz_handle row);

/**
 * @brief Returns how far in a row sits, counting the top level as zero.
 *
 * @param tree The tree holding the row. NULL yields 0.
 * @param row  A row returned by schultz_tree_view_add.
 * @return The depth.
 */
uint32_t schultz_tree_view_depth(const schultz_tree *tree,
                                 schultz_handle row);

/**
 * @brief Sets how much each level of depth indents.
 *
 * @param tree   The tree holding the view. Must not be NULL.
 * @param node   A node created by schultz_tree_view_create.
 * @param pixels Indent per level. Must not be negative.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_tree_view_set_indent(schultz_tree *tree, schultz_handle node,
                                     float pixels);

/**
 * @brief Chooses how many rows may be selected at once.
 *
 * @param tree The tree holding the view. Must not be NULL.
 * @param node A node created by schultz_tree_view_create.
 * @param mode One of the SCHULTZ_SELECT_* values.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_tree_view_set_selection_mode(schultz_tree *tree,
                                             schultz_handle node,
                                             uint32_t mode);

/**
 * @brief Selects a row and scrolls it into view.
 *
 * @param tree The tree holding the view. Must not be NULL.
 * @param node A node created by schultz_tree_view_create.
 * @param row  A row returned by schultz_tree_view_add.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_tree_view_select(schultz_tree *tree, schultz_handle node,
                                 schultz_handle row);

/**
 * @brief Returns the selected row.
 *
 * @param tree The tree holding the view. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node A node created by schultz_tree_view_create.
 * @return The row, or SCHULTZ_HANDLE_NONE when nothing is selected.
 */
schultz_handle schultz_tree_view_selected(const schultz_tree *tree,
                                          schultz_handle node);

/**
 * @brief Scrolls a row into view without selecting it.
 *
 * @param tree The tree holding the view. Must not be NULL.
 * @param node A node created by schultz_tree_view_create.
 * @param row  A row returned by schultz_tree_view_add.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_tree_view_scroll_to(schultz_tree *tree, schultz_handle node,
                                    schultz_handle row);

/**
 * @brief Creates a number field: a text field with a down and an up button.
 *
 * The two steps sit to the right of the field, down then up, and each is a
 * square as tall as the row rather than half of one, so a finger can aim at
 * either. That makes the whole thing about twice as wide as the field alone.
 *
 * Typing is read when the field is left or when Enter is pressed, so a half
 * typed number is not fought with while it is being typed. Text that is not a
 * number puts the last good value back rather than guessing.
 *
 * Holding a step button repeats it after a short pause, so one press is one
 * step. The up and down arrow keys step it too.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param minimum  Lowest value it will take.
 * @param maximum  Highest value. Must not be below the minimum.
 * @param value    Where to start. Clamped into the range.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_number_field_create(schultz_tree *tree, schultz_handle parent,
                                    double minimum, double maximum,
                                    double value, schultz_handle *out_node);

/**
 * @brief Where a number field's two step buttons sit.
 */
enum {
    /** Down then up to the right of the field, each a square as tall as the
     *  row. Wide, and the default. */
    SCHULTZ_STEPS_BESIDE = 0,
    /** Up over the field and down under it, each as wide as the field. Tall
     *  instead of wide, for a row of these in a narrow space. */
    SCHULTZ_STEPS_ABOVE_BELOW
};

/**
 * @brief Moves the two step buttons beside the field or above and below it.
 *
 * Beside is the default and is what a number on its own wants. Above and
 * below is for several of them side by side, where three fields each twice
 * their own width will not fit: it trades the width for height and keeps the
 * buttons big enough to aim at. The time picker's popup uses it.
 *
 * A preferred width set on the number field means the whole widget, so the
 * same number is a much wider field above and below than it is beside.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_number_field_create.
 * @param where SCHULTZ_STEPS_BESIDE or SCHULTZ_STEPS_ABOVE_BELOW.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_INVALID_ARGUMENT for anything else.
 */
int32_t schultz_number_field_set_steps(schultz_tree *tree, schultz_handle node,
                                       uint32_t where);

/**
 * @brief Sets the value, clamped into the range.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_number_field_create.
 * @param value The new value.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_number_field_set_value(schultz_tree *tree,
                                       schultz_handle node, double value);

/**
 * @brief Returns the value.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_number_field_create.
 * @return The value.
 */
double schultz_number_field_value(const schultz_tree *tree,
                                  schultz_handle node);

/**
 * @brief Changes the range, pulling the value inside it if it now falls out.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    A node created by schultz_number_field_create.
 * @param minimum Lowest value.
 * @param maximum Highest value. Must not be below the minimum.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_number_field_set_range(schultz_tree *tree,
                                       schultz_handle node, double minimum,
                                       double maximum);

/**
 * @brief Sets how far one step moves the value. One by default.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_number_field_create.
 * @param step How far. Must be above zero.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_number_field_set_step(schultz_tree *tree, schultz_handle node,
                                      double step);

/**
 * @brief Sets how many decimal places are shown. None by default.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A node created by schultz_number_field_create.
 * @param places How many, up to nine.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_number_field_set_decimals(schultz_tree *tree,
                                          schultz_handle node,
                                          uint32_t places);

/**
 * @brief Runs off one end of the range onto the other. Off by default.
 *
 * Wanted for things that are circular, such as an hour or a hue, and wrong
 * for things that are not.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_number_field_create.
 * @param on   Nonzero to wrap.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_number_field_set_wrap(schultz_tree *tree, schultz_handle node,
                                      int32_t on);

/**
 * @brief Returns the text field inside, for styling or for focus.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node A node created by schultz_number_field_create.
 * @return The text field.
 */
schultz_handle schultz_number_field_text_field(const schultz_tree *tree,
                                               schultz_handle node);

/** @brief The order a date picker writes and reads its three numbers in. */
enum {
    SCHULTZ_DATE_ISO = 0, /**< Year, month, day, separated by hyphens. */
    SCHULTZ_DATE_DMY,     /**< Day, month, year, separated by slashes. */
    SCHULTZ_DATE_MDY      /**< Month, day, year, separated by slashes. */
};

/**
 * @brief Creates a date picker: a field beside a calendar in a popover.
 *
 * The date is three integers and never a `time_t`, so there is no epoch, no
 * timezone and no daylight saving anywhere in it. The calendar is proleptic
 * Gregorian.
 *
 * Typing is read when the field is left or Enter is pressed. Text that is not
 * a date, or names a day that month does not have, puts the date back.
 *
 * The starting date is the first of January 2026, because a widget with no
 * clock cannot know what today is. Set one.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_date_picker_create(schultz_tree *tree, schultz_handle parent,
                                   schultz_handle *out_node);

/**
 * @brief Sets the date. Months and days count from one.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_date_picker_create.
 * @param year  The year.
 * @param month 1 to 12.
 * @param day   1 to however many days that month has.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a day that month does
 *         not have, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_date_picker_set_date(schultz_tree *tree, schultz_handle node,
                                     int32_t year, int32_t month,
                                     int32_t day);

/**
 * @brief Reads the date. Any of the three outputs may be NULL.
 *
 * @param tree      The tree holding the node. Must not be NULL.
 * @param node      A node created by schultz_date_picker_create.
 * @param out_year  Receives the year, or NULL.
 * @param out_month Receives the month, or NULL.
 * @param out_day   Receives the day, or NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_date_picker_date(const schultz_tree *tree,
                                 schultz_handle node, int32_t *out_year,
                                 int32_t *out_month, int32_t *out_day);

/**
 * @brief Limits which dates may be chosen.
 *
 * Days outside the range are drawn disabled and refused on entry.
 *
 * @param tree      The tree holding the node. Must not be NULL.
 * @param node      A node created by schultz_date_picker_create.
 * @param min_year  Earliest year.
 * @param min_month Earliest month.
 * @param min_day   Earliest day.
 * @param max_year  Latest year.
 * @param max_month Latest month.
 * @param max_day   Latest day.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when the earliest is
 *         after the latest, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_date_picker_set_range(schultz_tree *tree, schultz_handle node,
                                      int32_t min_year, int32_t min_month,
                                      int32_t min_day, int32_t max_year,
                                      int32_t max_month, int32_t max_day);

/**
 * @brief Replaces the month names, so a host can be correct in any language.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_date_picker_create.
 * @param names Twelve names, January first. Copied.
 * @param count Must be 12.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_date_picker_set_month_names(schultz_tree *tree,
                                            schultz_handle node,
                                            const char *const *names,
                                            uint32_t count);

/**
 * @brief Replaces the day names.
 *
 * Always given Sunday first, whatever the first day of the week is set to.
 * Turning the row round is the widget's job, not the caller's.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_date_picker_create.
 * @param names Seven names, Sunday first. Copied.
 * @param count Must be 7.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_date_picker_set_day_names(schultz_tree *tree,
                                          schultz_handle node,
                                          const char *const *names,
                                          uint32_t count);

/**
 * @brief Chooses which weekday a row of the calendar starts on.
 *
 * Sunday and Monday are both right, depending on where you are.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    A node created by schultz_date_picker_create.
 * @param weekday 0 for Sunday through 6 for Saturday.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_date_picker_set_first_day(schultz_tree *tree,
                                          schultz_handle node,
                                          uint32_t weekday);

/**
 * @brief Chooses the order the three numbers are written and read in.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A node created by schultz_date_picker_create.
 * @param format One of the SCHULTZ_DATE_* values.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_date_picker_set_format(schultz_tree *tree,
                                       schultz_handle node, uint32_t format);

/**
 * @brief Creates a time picker: a field beside hour and minute steppers.
 *
 * The value is always kept as 24 hour time whatever is displayed, so a host
 * reading it back never has to work out which half of the day it meant.
 * Hours and minutes are circular, so their steppers run off one end onto the
 * other.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_time_picker_create(schultz_tree *tree, schultz_handle parent,
                                   schultz_handle *out_node);

/**
 * @brief Sets the time, as 24 hour whatever is being displayed.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A node created by schultz_time_picker_create.
 * @param hour   0 to 23.
 * @param minute 0 to 59.
 * @param second 0 to 59.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_time_picker_set_time(schultz_tree *tree, schultz_handle node,
                                     int32_t hour, int32_t minute,
                                     int32_t second);

/**
 * @brief Reads the time, as 24 hour. Any of the three outputs may be NULL.
 *
 * @param tree       The tree holding the node. Must not be NULL.
 * @param node       A node created by schultz_time_picker_create.
 * @param out_hour   Receives the hour, or NULL.
 * @param out_minute Receives the minute, or NULL.
 * @param out_second Receives the second, or NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_time_picker_time(const schultz_tree *tree,
                                 schultz_handle node, int32_t *out_hour,
                                 int32_t *out_minute, int32_t *out_second);

/**
 * @brief Chooses 24 hour or 12 hour display. 24 hour by default.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_time_picker_create.
 * @param on   Nonzero for 24 hour, zero for 12 hour with am and pm.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_time_picker_set_24_hour(schultz_tree *tree,
                                        schultz_handle node, int32_t on);

/**
 * @brief Shows seconds as well as hours and minutes. Off by default.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_time_picker_create.
 * @param on   Nonzero to show seconds.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_time_picker_set_show_seconds(schultz_tree *tree,
                                             schultz_handle node, int32_t on);

/**
 * @brief Creates a colour picker: a swatch that drops a picker.
 *
 * The popover holds a saturation and value square, a hue slider, an optional
 * alpha slider, and a hex field. The square is drawn as strips rather than
 * filled with a gradient, because the hue moving would need a new gradient
 * every time and a widget cannot register one.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param color    The colour to start on.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_color_picker_create(schultz_tree *tree, schultz_handle parent,
                                    schultz_color color,
                                    schultz_handle *out_node);

/**
 * @brief Sets the colour.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_color_picker_create.
 * @param color The new colour.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_color_picker_set_color(schultz_tree *tree,
                                       schultz_handle node,
                                       schultz_color color);

/**
 * @brief Returns the colour.
 *
 * @param tree The tree holding the node. NULL yields opaque black.
 * @param node A node created by schultz_color_picker_create.
 * @return The colour.
 */
schultz_color schultz_color_picker_color(const schultz_tree *tree,
                                         schultz_handle node);

/**
 * @brief Shows the alpha slider. Off by default, and alpha stays opaque.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_color_picker_create.
 * @param on   Nonzero to let alpha be chosen.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_color_picker_set_alpha_enabled(schultz_tree *tree,
                                               schultz_handle node,
                                               int32_t on);

/**
 * @brief Creates a combo box: a button showing a choice, and a menu of them.
 *
 * Choosing one of many is as common in a form as typing into one, which is
 * why this ships rather than being left to be composed. It is a button that
 * opens a menu and shows what was chosen, so it waits for the overlays it
 * needs rather than arriving with the rest of the controls.
 *
 * A click opens the menu below it and a click on a row chooses that row; up
 * and down move the choice without opening anything.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_combo_box_create(schultz_tree *tree, schultz_handle parent,
                                 schultz_handle *out_node);

/**
 * @brief Adds a choice.
 *
 * The first one added becomes the current choice, so a combo box is never
 * blank.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_combo_box_create.
 * @param text What the choice says, copied.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a combo
 *         box, SCHULTZ_ERR_EXHAUSTED when it already holds as many as it can,
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_combo_box_add(schultz_tree *tree, schultz_handle node,
                              const char *text);

/**
 * @brief Chooses one of a combo box's items.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_combo_box_create.
 * @param index Which item, counting from zero.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when there is no such
 *         item, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_combo_box_select(schultz_tree *tree, schultz_handle node,
                                 uint32_t index);

/**
 * @brief Returns which item a combo box is showing.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_combo_box_create.
 * @return The index, or 0 when the node is not a combo box.
 */
uint32_t schultz_combo_box_selected(const schultz_tree *tree,
                                    schultz_handle node);

/**
 * @brief Counts a combo box's items.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_combo_box_create.
 * @return How many items it holds, or 0 when the node is not a combo box.
 */
uint32_t schultz_combo_box_count(const schultz_tree *tree,
                                 schultz_handle node);

/**
 * @brief Returns the menu a combo box opens, to style or inspect it.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node A node created by schultz_combo_box_create.
 * @return The menu, or SCHULTZ_HANDLE_NONE when the node is not a combo box.
 */
schultz_handle schultz_combo_box_menu(const schultz_tree *tree,
                                      schultz_handle node);

/* ----------------------------------------------------------------- Icon */

/** @brief How a picture is fitted into the room a widget gives it. */
enum {
    SCHULTZ_FIT_CONTAIN = 0, /**< As large as fits, proportions kept. */
    SCHULTZ_FIT_COVER,       /**< Covers the box, proportions kept, clipped. */
    SCHULTZ_FIT_FILL,        /**< Stretched to the box, proportions ignored. */
    SCHULTZ_FIT_NONE         /**< Its own size, centred. */
};

/**
 * @brief Creates an icon: a widget that draws a loaded image.
 *
 * The same widget draws every format the toolkit loads, vector included: an
 * SVG is a picture like any other and scales cleanly to whatever size the
 * icon is given, so there is no separate widget for it.
 *
 * It measures to the image's natural size, which a size hint overrules, and
 * fits the picture into whatever room it ends up with. Contain by default,
 * which is the fit that never crops and never distorts.
 *
 * The tree needs an image table, set with schultz_tree_set_image_table.
 * Without one an icon measures to nothing and draws nothing rather than
 * guessing at a size.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param image    A handle from schultz_image_load_file or its siblings, or
 *                 SCHULTZ_HANDLE_NONE for an icon filled in later.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_icon_create(schultz_tree *tree, schultz_handle parent,
                            schultz_handle image, schultz_handle *out_node);

/**
 * @brief Changes which image an icon draws.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_icon_create.
 * @param image The new image, or SCHULTZ_HANDLE_NONE to draw none.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not an
 *         icon.
 */
int32_t schultz_icon_set_image(schultz_tree *tree, schultz_handle node,
                               schultz_handle image);

/**
 * @brief Lets a drag inside a selection area take this picture.
 *
 * Off by default, the same as a label: a picture is decoration until somebody
 * says it is content. A selected picture is carried by a copy only into a
 * format that can hold one, so it costs nothing in a plain text paste.
 *
 * @param tree       The tree holding the node. Must not be NULL.
 * @param node       A node created by schultz_icon_create.
 * @param selectable Nonzero to let it be selected.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not an
 *         icon.
 */
int32_t schultz_icon_set_selectable(schultz_tree *tree, schultz_handle node,
                                    int32_t selectable);

/**
 * @brief Whether a drag may take this picture.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_icon_create.
 * @return Nonzero when it may.
 */
int32_t schultz_icon_selectable(const schultz_tree *tree,
                                schultz_handle node);

/**
 * @brief Returns the image an icon draws.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_HANDLE_NONE.
 * @param node A node created by schultz_icon_create.
 * @return The image, or SCHULTZ_HANDLE_NONE when the node is not an icon.
 */
schultz_handle schultz_icon_image(const schultz_tree *tree,
                                  schultz_handle node);

/**
 * @brief Sets how an icon fits its picture into the room it has.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node created by schultz_icon_create.
 * @param fit  One of the SCHULTZ_FIT_* values.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for an unknown fit, or
 *         SCHULTZ_ERR_INVALID_HANDLE when the node is not an icon.
 */
int32_t schultz_icon_set_fit(schultz_tree *tree, schultz_handle node,
                             uint32_t fit);

/* ----------------------------------------------------------- LottieView */

/**
 * @brief Creates a view that plays a Lottie animation.
 *
 * It measures to the animation's natural size, which a size hint overrules,
 * and draws whichever frame the animation is on, stretched to the node. The
 * artwork is vector, so it is drawn at that size rather than resampled to it.
 *
 * It starts paused on the first frame and loops once played. Playing is what
 * asks the tree's clock to tick it, so a paused view costs nothing, and a host
 * that never calls schultz_tree_advance will see it sit still.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param image    A handle from schultz_image_load_animation.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_lottie_create(schultz_tree *tree, schultz_handle parent,
                              schultz_handle image, schultz_handle *out_node);

/**
 * @brief Starts or stops an animation.
 *
 * Frames advance by however long has passed rather than one per tick, so
 * playback runs at the speed it was authored at whatever rate the host draws.
 * A view that reaches the end without looping stops itself.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    A node created by schultz_lottie_create.
 * @param playing Nonzero to run.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         Lottie view, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_lottie_play(schultz_tree *tree, schultz_handle node,
                            int32_t playing);

/**
 * @brief Says whether an animation starts again at the end.
 *
 * Looping by default.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    A node created by schultz_lottie_create.
 * @param looping Nonzero to loop.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_lottie_set_looping(schultz_tree *tree, schultz_handle node,
                                   int32_t looping);

/**
 * @brief Moves an animation to a frame without playing it.
 *
 * Fractions are allowed, which is what makes a scrubbed animation smooth.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  A node created by schultz_lottie_create.
 * @param frame Which frame, counted from zero and clamped to what there is.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_lottie_seek(schultz_tree *tree, schultz_handle node,
                            float frame);

/**
 * @brief Returns which frame an animation is showing.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_lottie_create.
 * @return The frame, or 0 when the node is not a Lottie view.
 */
float schultz_lottie_frame(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Reports whether an animation is running.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node A node created by schultz_lottie_create.
 * @return 1 when playing, 0 otherwise.
 */
int32_t schultz_lottie_is_playing(const schultz_tree *tree,
                                  schultz_handle node);


/* ------------------------------------------------------ On-screen keyboard */

/**
 * @brief Builds a keyboard the toolkit draws itself.
 *
 * For a machine with a touch screen and no keyboard, where the platform has
 * none to offer either: SDL implements a screen keyboard for Android, iOS and
 * a few consoles, and for a Linux framebuffer it implements nothing, so a
 * text field there cannot be typed into without this.
 *
 * A press goes out through schultz_events_text_input and schultz_events_key,
 * which are the calls the platform layer makes for a real keyboard, so a
 * field cannot tell the two apart. Keys do not take focus: the field being
 * edited keeps it.
 *
 * Most hosts do not call this. schultz_window_set_keyboard asks the window to
 * put one up when a text widget takes focus, which is the ordinary way to get
 * one. This is here for a host that wants to place a keyboard itself.
 *
 * @param tree     The tree to build in. Must not be NULL.
 * @param parent   The node to build under.
 * @param events   Where presses are sent. Must not be NULL.
 * @param out_node Receives the keyboard. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT,
 *         SCHULTZ_ERR_INVALID_HANDLE or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_keyboard_create(schultz_tree *tree, schultz_handle parent,
                                schultz_events *events,
                                schultz_handle *out_node);

/**
 * @brief Says whether the keyboard offers its page of emoji.
 *
 * Offered by default, since the face that draws them is compiled into the
 * library whether or not anything uses it. Switching it off hides the emoji
 * page and the keys that open it, and is what an application wants where
 * emoji do not belong.
 *
 * A field may still refuse them whatever this says: see
 * schultz_text_field_set_input_type.
 *
 * @param tree    The tree holding the keyboard. Must not be NULL.
 * @param node    A node from schultz_keyboard_create.
 * @param offered Nonzero to offer emoji, zero to hide them.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE when the node is not a
 *         keyboard.
 */
int32_t schultz_keyboard_set_emoji(schultz_tree *tree, schultz_handle node,
                                   int32_t offered);

/**
 * @brief Tells the keyboard what kind of field it is serving.
 *
 * A number field opens on the digits, and neither a number nor a password
 * field is offered emoji whatever schultz_keyboard_set_emoji says. The two
 * answers are kept apart: an application says whether this window offers
 * emoji at all, a field says whether it accepts them, and either one saying
 * no is a no.
 *
 * A window driving its own keyboard calls this for you, from the focused
 * field's schultz_text_field_input_type.
 *
 * @param tree The tree holding the keyboard. Must not be NULL.
 * @param node A node from schultz_keyboard_create.
 * @param type One of the SCHULTZ_INPUT_* values.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_INVALID_ARGUMENT for a type that is not one of them.
 */
int32_t schultz_keyboard_set_input_type(schultz_tree *tree,
                                        schultz_handle node, uint32_t type);

/**
 * @brief Returns what the keyboard was last told it is serving.
 *
 * SCHULTZ_INPUT_TEXT until something says otherwise, which is what a
 * keyboard nobody has told anything shows.
 *
 * @param tree The tree holding the keyboard. Must not be NULL.
 * @param node A node from schultz_keyboard_create.
 * @return One of the SCHULTZ_INPUT_* values.
 */
uint32_t schultz_keyboard_input_type(const schultz_tree *tree,
                                     schultz_handle node);

/**
 * @brief Returns whether the keyboard offers emoji.
 *
 * @param tree The tree holding the keyboard. Must not be NULL.
 * @param node A node from schultz_keyboard_create.
 * @return Nonzero when emoji are offered.
 */
int32_t schultz_keyboard_offers_emoji(const schultz_tree *tree,
                                      schultz_handle node);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_WIDGETS_H */
