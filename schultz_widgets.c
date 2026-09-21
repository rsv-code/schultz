/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_widgets.c
 * @brief The built in widgets.
 */

#include "schultz_widgets.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_image.h"
#include "schultz_layout.h"
#include "schultz_selection.h"
#include "schultz_text.h"

/* ---------------------------------------------------------------- shared */

/*
 * Setters and layout.
 *
 * A setter that changes the size or shape of something has to ask for the
 * layout to run again, and laying out is the expensive half of a turn. That
 * is fine when a host calls a setter because something changed. It is a trap
 * when a host calls it every turn with the value it already holds, which is
 * an ordinary way to write a frame loop: push the current state at the tree
 * and let it work out the difference.
 *
 * So every setter below compares first, and asks for a layout only when what
 * it was given differs from what it is holding. Two of these were found the
 * expensive way: a button's caption, and schultz_icon_set_image, which was
 * laying the whole window out twice a frame for pictures that were all the
 * same size -- 23 milliseconds a frame of it, against well under one once the
 * size was checked.
 *
 * Two shapes of the same rule appear here. Most simply return when nothing
 * moved. A few -- schultz_tab_view_select and schultz_keyboard_show_set --
 * carry work their own creation path depends on, so they do that work either
 * way and gate only the request to lay out again.
 */

/*
 * Every widget here draws from the node's resolved style and names no colours
 * of its own, so a theme change or a per node override restyles all of them
 * without their knowing.
 */
static const schultz_resolved_style *schultz_widget_style(
    schultz_tree *tree, schultz_handle node)
{
    return schultz_node_resolved(tree, node);
}

/* Fills and strokes a rounded rectangle, skipping either when invisible. */
/*
 * A widget's outline, gathered from the style in one place: what it is drawn
 * with, how thick, and whether it is dashed and how its ends and corners are
 * finished.
 */
static schultz_stroke schultz_widget_stroke(
    const schultz_resolved_style *style)
{
    schultz_stroke stroke = schultz_stroke_make(
        schultz_resolved_paint(style, SCHULTZ_PROP_BORDER_COLOR),
        schultz_resolved_number(style, SCHULTZ_PROP_BORDER_WIDTH));

    stroke.dash = schultz_resolved_dash(style, SCHULTZ_PROP_BORDER_DASH);
    stroke.cap  = (uint32_t)schultz_resolved_number(style,
                                                    SCHULTZ_PROP_BORDER_CAP);
    stroke.join = (uint32_t)schultz_resolved_number(style,
                                                    SCHULTZ_PROP_BORDER_JOIN);
    stroke.dash_offset =
        schultz_resolved_number(style, SCHULTZ_PROP_BORDER_DASH_OFFSET);
    stroke.miter_limit =
        schultz_resolved_number(style, SCHULTZ_PROP_BORDER_MITER_LIMIT);
    return stroke;
}

static int32_t schultz_widget_draw_box(schultz_draw_list *list,
                                       const schultz_resolved_style *style,
                                       schultz_rect bounds)
{
    schultz_paint fill = schultz_resolved_paint(style,
                                                SCHULTZ_PROP_BACKGROUND);
    schultz_stroke stroke = schultz_widget_stroke(style);
    float radius = schultz_resolved_number(style, SCHULTZ_PROP_CORNER_RADIUS);
    int32_t result;

    if (!schultz_paint_is_invisible(fill)) {
        result = schultz_draw_fill_round_rect(list, bounds, fill, radius);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    if (!schultz_paint_is_invisible(stroke.paint) && stroke.width > 0.0f) {
        return schultz_draw_stroke_round_rect(list, bounds, stroke, radius);
    }
    return SCHULTZ_OK;
}

/*
 * Widgets whose measure is their own place nothing, so they share one empty
 * arrange. A widget with no pane at all would fall back to its size hints,
 * which is what Panel and Ellipse want.
 */
static int32_t schultz_leaf_arrange(schultz_tree *tree, schultz_handle node,
                                    schultz_rect rect)
{
    (void)tree;
    (void)node;
    (void)rect;
    return SCHULTZ_OK;
}

/* ---------------------------------------------------------------- Panel */

static int32_t schultz_panel_paint(schultz_tree *tree, schultz_handle node,
                                   schultz_draw_list *list,
                                   schultz_arena *arena)
{
    schultz_rect bounds;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    return schultz_widget_draw_box(list, schultz_widget_style(tree, node),
                                   bounds);
}

static const schultz_widget_vtable schultz_panel_widget = {
    .paint = schultz_panel_paint
};

int32_t schultz_panel_create(schultz_tree *tree, schultz_handle parent,
                             schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result = schultz_node_create(tree, parent, &node);

    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_set_widget(tree, node, &schultz_panel_widget, NULL);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_GROUP);
    *out_node = node;
    return SCHULTZ_OK;
}


/* ---------------------------------------------------------------- Label */

/** @brief A label's text, how it is laid out, and what is selected in it. */
typedef struct {
    char    *text;   /**< Owned copy, never NULL once created. */
    uint32_t length; /**< Bytes in text, not counting the terminator. */
    uint32_t wrap;   /**< Nonzero when the text wraps at the offered width. */
    /**
     * Nonzero cuts a line too long for the box and marks the cut with an
     * ellipsis, rather than letting the glyphs run past the edge.
     */
    uint32_t ellipsize;

    /*
     * Rich text. Which stretches of the text are not set the way the widget's
     * style says: a bold phrase, a term in the code face, a word with a
     * background. NULL and zero mean the label is all one face in one colour,
     * which is what nearly every label is.
     *
     * Owned, including a copy of every link string, because a caller has no
     * way to know how long the label will want them.
     */
    schultz_span *spans;
    uint32_t      span_count;

    /*
     * Selection. A label is not focusable, so the tree names whichever node
     * owns the selection and copy is routed there; see
     * schultz_tree_set_selection_owner.
     */
    uint32_t selectable; /**< Nonzero when the text can be selected. */
    uint32_t anchor;     /**< Byte offset the selection started at. */
    uint32_t caret;      /**< Byte offset the selection ends at. */
    uint32_t dragging;   /**< Nonzero while a drag is extending it. */
    /**
     * Nonzero when the press that just ended selected something by dragging.
     *
     * A click is a press and release on the same node, and the router lets a
     * mouse wobble between the two because a button pressed and released
     * with a shaky hand is still pressed. Dragging across words to select
     * them is that same shape and must not also press what it started on, so
     * the label remembers and says there was nothing under the pointer.
     */
    uint32_t dragged;

    /*
     * Touch. A finger cannot place a caret precisely and a drag on a touch
     * screen means scroll, so selecting starts with a hold rather than a
     * drag, and the two ends are then moved with grips large enough to hit.
     */
    uint32_t      grips;    /**< Nonzero when the grips are shown. */
    uint32_t      grip;     /**< Which grip is held: 0 none, 1 start, 2 end. */
    uint32_t      holding;  /**< Nonzero while a touch press is being timed. */
    uint64_t      held_ms;  /**< How long that press has lasted. */
    schultz_point held_at;  /**< Where it landed, in local coordinates. */
} schultz_label_data;

/** How long a finger must rest on text before it selects a word. */
#define SCHULTZ_LABEL_HOLD_MS 400u

/** How far a finger may stray during that hold and still be holding still. */
#define SCHULTZ_LABEL_HOLD_SLOP 10.0f

/** Radius of a touch grip, and how far below the text it sits. */
#define SCHULTZ_LABEL_GRIP 7.0f

/*
 * A span list, owned.
 *
 * A label holds one and so does an editable widget, and the rules for keeping
 * it are the same for both: link strings are copied because a caller cannot
 * know how long the widget will want them, a span covering nothing is dropped
 * rather than carried through layout and painting, and a span running past
 * the end of the text is cut to fit. Written once here so the two cannot
 * drift apart.
 */

/*
 * A copy of a string, or NULL for NULL. strdup is not in C11, and the one
 * place that needs it is a span's link travelling with the span.
 */
static char *schultz_span_copy_link(const char *from)
{
    size_t bytes;
    char  *copy;

    if (from == NULL) {
        return NULL;
    }
    bytes = strlen(from) + 1u;
    copy  = (char *)malloc(bytes);
    if (copy != NULL) {
        memcpy(copy, from, bytes);
    }
    return copy;
}

/* Lets go of a span list and every link string in it. */
static void schultz_spans_free(schultz_span **spans, uint32_t *count)
{
    uint32_t i;

    for (i = 0u; i < *count; i++) {
        free((void *)(uintptr_t)(*spans)[i].link);
    }
    free(*spans);
    *spans = NULL;
    *count = 0u;
}

/* Whether two span lists say the same thing, for a setter's gate. */
static int32_t schultz_spans_same(const schultz_span *a, uint32_t a_count,
                                  const schultz_span *b, uint32_t b_count)
{
    uint32_t i;

    if (a_count != b_count) {
        return 0;
    }
    for (i = 0u; i < a_count; i++) {
        if (a[i].start != b[i].start || a[i].end != b[i].end ||
            a[i].font != b[i].font || a[i].size != b[i].size ||
            a[i].bold != b[i].bold || a[i].italic != b[i].italic ||
            a[i].underline != b[i].underline ||
            a[i].strikethrough != b[i].strikethrough ||
            a[i].clickable != b[i].clickable || a[i].tag != b[i].tag ||
            !schultz_color_equals(a[i].color, b[i].color) ||
            !schultz_color_equals(a[i].background, b[i].background)) {
            return 0;
        }
        if ((a[i].link == NULL) != (b[i].link == NULL)) {
            return 0;
        }
        if (a[i].link != NULL && strcmp(a[i].link, b[i].link) != 0) {
            return 0;
        }
    }
    return 1;
}

/* Takes a copy of a span list, cut to fit text of `limit` bytes. */
static int32_t schultz_spans_store(schultz_span **spans, uint32_t *count,
                                   const schultz_span *from, uint32_t n,
                                   uint32_t limit)
{
    schultz_span *copy = NULL;
    uint32_t kept = 0u;
    uint32_t i;

    if (n > 0u) {
        copy = (schultz_span *)calloc(n, sizeof(*copy));
        if (copy == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
    }
    for (i = 0u; i < n; i++) {
        uint32_t end = (from[i].end > limit) ? limit : from[i].end;

        if (from[i].start >= end) {
            continue;
        }
        copy[kept]      = from[i];
        copy[kept].end  = end;
        copy[kept].link = NULL;
        if (from[i].link != NULL) {
            copy[kept].link = schultz_span_copy_link(from[i].link);
            if (copy[kept].link == NULL) {
                *spans = copy;
                *count = kept;
                schultz_spans_free(spans, count);
                return SCHULTZ_ERR_OUT_OF_MEMORY;
            }
        }
        kept++;
    }

    schultz_spans_free(spans, count);
    if (kept == 0u) {
        free(copy);
        return SCHULTZ_OK;
    }
    *spans = copy;
    *count = kept;
    return SCHULTZ_OK;
}

/*
 * The span covering a byte, or NULL where none does.
 *
 * The first match wins, which is what the text layer does when it decides
 * which face a byte is set in. The two have to agree, or a phrase would be
 * shaped in one span's face and painted in another's colour.
 */
static const schultz_span *schultz_spans_at(const schultz_span *spans,
                                            uint32_t count, uint32_t at)
{
    uint32_t i;

    for (i = 0u; i < count; i++) {
        if (spans[i].start <= at && at < spans[i].end) {
            return &spans[i];
        }
    }
    return NULL;
}

static void schultz_label_free_spans(schultz_label_data *label)
{
    schultz_spans_free(&label->spans, &label->span_count);
}

static void schultz_label_destroy(void *data)
{
    schultz_label_data *label = (schultz_label_data *)data;

    if (label != NULL) {
        schultz_label_free_spans(label);
        free(label->text);
        free(label);
    }
}

/*
 * The face a widget draws with, at the size its style asks for. The `font`
 * property names a face and `font.size` names the size, and a face is bound
 * to a size, so the two are put back together here. One place does this, so
 * every widget that draws text agrees about what the style meant.
 */
static schultz_handle schultz_widget_font(schultz_tree *tree,
                                          schultz_handle node)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_handle font = schultz_resolved_font(style, SCHULTZ_PROP_FONT);
    float size = schultz_resolved_number(style, SCHULTZ_PROP_FONT_SIZE);
    schultz_handle sized = SCHULTZ_HANDLE_NONE;

    if (fonts == NULL || font == SCHULTZ_HANDLE_NONE || size <= 0.0f) {
        return font;
    }
    if (schultz_font_at_size(fonts, font, size, &sized) != SCHULTZ_OK) {
        return font;
    }
    return sized;
}

/* The font a label draws with, or SCHULTZ_HANDLE_NONE when it has none. */
static schultz_handle schultz_label_font(schultz_tree *tree,
                                         schultz_handle node)
{
    return schultz_widget_font(tree, node);
}

/*
 * Draws a shaped line, which may take more than one face to draw.
 *
 * A draw command names one font, because a glyph is an index into a face and
 * means nothing without it. Shaping reports which face each glyph came from,
 * so this starts a new command wherever that changes. A line with no emoji
 * in it is one command, the same as it always was.
 */
static void schultz_widget_draw_glyphs(schultz_draw_list *list,
                                       schultz_handle font,
                                       const schultz_handle *fonts,
                                       const schultz_glyph *glyphs,
                                       uint32_t count, schultz_paint paint)
{
    uint32_t start = 0u;
    uint32_t i;

    if (count == 0u) {
        return;
    }
    if (fonts == NULL) {
        schultz_draw_glyph_run(list, font, glyphs, count, paint);
        return;
    }
    for (i = 1u; i <= count; i++) {
        if (i == count || fonts[i] != fonts[start]) {
            schultz_draw_glyph_run(list, fonts[start], glyphs + start,
                                   i - start, paint);
            start = i;
        }
    }
}

/* Distance along an axis, without pulling in a library call for it. */
static float schultz_label_distance(float a, float b)
{
    return (a > b) ? (a - b) : (b - a);
}

/** @brief One line of a label, placed in absolute coordinates. */
typedef struct {
    uint32_t             start;    /**< First byte of the line. */
    uint32_t             end;      /**< One past its last byte. */
    float                x;        /**< Leading edge. */
    float                width;    /**< Advance, after any justification. */
    float                top;      /**< Top of the line's box. */
    float                height;   /**< The line height in use. */
    float                baseline; /**< Where the glyphs sit. */
    const schultz_glyph *glyphs;   /**< Placed in absolute coordinates. */
    const uint32_t      *clusters; /**< Byte offset per glyph, line relative. */
    /** The face each glyph came from. Differs along the row where the text
     *  face had no glyph and the emoji face supplied one. */
    const schultz_handle *fonts;
    uint32_t             count;    /**< How many glyphs. */
} schultz_label_row;

/*
 * The line height a label uses: the font's, scaled by line.spacing. Body text
 * usually wants more air between lines than a caption does, and the font's
 * own figure is the caption's.
 */
static float schultz_label_line_height(const schultz_resolved_style *style,
                                       const schultz_font_metrics *metrics)
{
    float spacing = schultz_resolved_number(style, SCHULTZ_PROP_LINE_SPACING);

    return metrics->line_height * ((spacing > 0.0f) ? spacing : 1.0f);
}

/*
 * Spreads a line's slack across the gaps between its words, which is what
 * justification is. The shift a glyph gets is proportional to how many spaces
 * come before it, so the gaps grow evenly and the last glyph lands on the
 * trailing edge.
 *
 * Only for left to right lines. Glyphs come back in visual order, and for
 * right to left text that order runs against the byte offsets this counts
 * from, so the shifts would land on the wrong gaps.
 */
static void schultz_label_justify(schultz_glyph *placed,
                                  const uint32_t *clusters, uint32_t count,
                                  const char *line, uint32_t length,
                                  float extra)
{
    uint32_t spaces = 0u;
    uint32_t seen = 0u;
    uint32_t at = 0u;
    uint32_t i;

    for (i = 0u; i < length; i++) {
        if (line[i] == ' ') {
            spaces++;
        }
    }
    if (spaces == 0u) {
        return;
    }
    for (i = 0u; i < count; i++) {
        uint32_t upto = (clusters[i] > length) ? length : clusters[i];

        while (at < upto) {
            if (line[at] == ' ') {
                seen++;
            }
            at++;
        }
        placed[i].x += extra * (float)seen / (float)spaces;
    }
}

/** The mark left where a line was cut short, U+2026 HORIZONTAL ELLIPSIS. */
#define SCHULTZ_LABEL_ELLIPSIS "\xE2\x80\xA6"

/** Bytes in SCHULTZ_LABEL_ELLIPSIS, not counting the terminator. */
#define SCHULTZ_LABEL_ELLIPSIS_BYTES 3u

/*
 * Cuts a shaped line down to a width and marks the cut with an ellipsis.
 *
 * The shortened text is shaped again rather than having glyphs dropped off
 * the end of the run, because the ellipsis has to be shaped against the text
 * it follows and a cut can land in the middle of a ligature. Only the shaper
 * knows what the shortened string draws as.
 *
 * The cut point is the last glyph that starts inside the room left over once
 * the ellipsis is paid for. Glyphs come back in visual order and their x is a
 * leading edge, so for left to right text that glyph's byte offset is exactly
 * the longest prefix that still fits.
 *
 * Right to left text is left alone: its visual end is the start of the
 * string, so cutting a prefix would take the wrong end away. Such a line is
 * held inside its box by the clip in schultz_label_paint instead.
 *
 * The run is replaced in place. A line with no room even for the ellipsis
 * comes back with no glyphs, because a caption cut down to nothing still has
 * to stay inside its box.
 */
static void schultz_label_cut(schultz_font_system *fonts, schultz_handle font,
                              const char *line, uint32_t length, float width,
                              schultz_arena *arena, schultz_text_run *run)
{
    schultz_size mark;
    float room;
    uint32_t cut = 0u;
    uint32_t i;
    char *shortened;

    if (run->width <= width || run->direction == SCHULTZ_DIR_RTL) {
        return;
    }
    if (schultz_text_measure(fonts, font, SCHULTZ_LABEL_ELLIPSIS,
                             (int32_t)SCHULTZ_LABEL_ELLIPSIS_BYTES, arena,
                             &mark) != SCHULTZ_OK) {
        return;
    }

    room = width - mark.width;
    if (room < 0.0f) {
        run->count = 0u;
        run->width = 0.0f;
        return;
    }
    for (i = 0u; i < run->count; i++) {
        if (run->glyphs[i].x > room) {
            break;
        }
        cut = run->clusters[i];
    }
    if (cut > length) {
        cut = length;
    }

    shortened = (char *)schultz_arena_alloc(
        arena, (size_t)cut + SCHULTZ_LABEL_ELLIPSIS_BYTES + 1u, 1u);
    if (shortened == NULL) {
        return;
    }
    if (cut > 0u) {
        memcpy(shortened, line, cut);
    }
    memcpy(shortened + cut, SCHULTZ_LABEL_ELLIPSIS,
           SCHULTZ_LABEL_ELLIPSIS_BYTES);
    shortened[cut + SCHULTZ_LABEL_ELLIPSIS_BYTES] = '\0';

    if (schultz_text_shape(fonts, font, shortened,
                           (int32_t)(cut + SCHULTZ_LABEL_ELLIPSIS_BYTES),
                           SCHULTZ_DIR_AUTO, arena, run) != SCHULTZ_OK) {
        run->count = 0u;
        run->width = 0.0f;
    }
}

/*
 * Lays a label's text out into placed lines. Painting and hit testing both go
 * through this, because a selection drawn from one set of positions and
 * measured against another selects one thing and highlights a different one.
 *
 * Everything returned lives in the arena the caller passed.
 */
/*
 * The faces a label's spans ask for, as the text layer wants them.
 *
 * A span says bold, or a size, or a face; the family and the size table turn
 * that into a handle. Spans that change nothing about the face produce no
 * piece, so a label whose spans only set colours measures exactly as a plain
 * one does and pays nothing for having them.
 *
 * Returns how many pieces there are, which is zero when the text is all one
 * face however many spans there are.
 */
/*
 * The pieces that fall on one line, clipped to it and moved to the front.
 *
 * Shaping is done a line at a time, against the line's own bytes, so the
 * offsets a piece carries have to be rebased the same way. Returns how many
 * landed on the line.
 */
static uint32_t schultz_pieces_on_line(const schultz_text_piece *pieces,
                                          uint32_t count, uint32_t from,
                                          uint32_t to,
                                          schultz_text_piece *out)
{
    uint32_t kept = 0u;
    uint32_t i;

    for (i = 0u; i < count; i++) {
        uint32_t start = pieces[i].start;
        uint32_t end   = pieces[i].end;

        if (start < from) {
            start = from;
        }
        if (end > to) {
            end = to;
        }
        if (start >= end) {
            continue;
        }
        out[kept].start = start - from;
        out[kept].end   = end - from;
        out[kept].font  = pieces[i].font;
        kept++;
    }
    return kept;
}

/*
 * The faces a span list asks for, as the text layer wants them.
 *
 * A span says bold, or a size, or a face; the family and the size table turn
 * that into a handle. Spans that change nothing about the face produce no
 * piece, so text whose spans only set colours measures exactly as plain text
 * does and pays nothing for having them.
 */
/* Forward, because the text widget's helpers want it before it is defined. */
static uint32_t schultz_spans_pieces(schultz_font_system *fonts,
                                     const schultz_span *spans,
                                     uint32_t span_count,
                                     schultz_handle font,
                                     schultz_arena *arena,
                                     schultz_text_piece **out_pieces);

static uint32_t schultz_spans_pieces(schultz_font_system *fonts,
                                     const schultz_span *spans,
                                     uint32_t span_count,
                                     schultz_handle font,
                                     schultz_arena *arena,
                                     schultz_text_piece **out_pieces)
{
    schultz_text_piece *pieces;
    uint32_t count = 0u;
    uint32_t i;

    *out_pieces = NULL;
    if (span_count == 0u || fonts == NULL) {
        return 0u;
    }
    pieces = (schultz_text_piece *)schultz_arena_alloc(
        arena, (size_t)span_count * sizeof(*pieces),
        _Alignof(schultz_text_piece));
    if (pieces == NULL) {
        return 0u;
    }

    for (i = 0u; i < span_count; i++) {
        schultz_handle at = (spans[i].font != SCHULTZ_HANDLE_NONE)
                                ? spans[i].font : font;

        if (spans[i].size > 0.0f) {
            (void)schultz_font_at_size(fonts, at, spans[i].size, &at);
        }
        if (spans[i].bold || spans[i].italic) {
            (void)schultz_font_at_style(fonts, at, (int32_t)spans[i].bold,
                                        (int32_t)spans[i].italic, &at);
        }
        /*
         * Nothing about the face changed, so there is nothing for the text
         * layer to do differently here. Underline and colour are painted,
         * not shaped.
         */
        if (at == font) {
            continue;
        }
        pieces[count].start = spans[i].start;
        pieces[count].end   = spans[i].end;
        pieces[count].font  = at;
        count++;
    }
    if (count == 0u) {
        return 0u;
    }
    *out_pieces = pieces;
    return count;
}

static uint32_t schultz_label_rows(schultz_tree *tree, schultz_handle node,
                                   const schultz_label_data *label,
                                   schultz_rect bounds, schultz_arena *arena,
                                   schultz_label_row **out_rows)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_handle font = schultz_label_font(tree, node);
    schultz_font_metrics metrics;
    const schultz_text_line *lines;
    schultz_text_piece *pieces = NULL;
    schultz_text_piece *line_pieces = NULL;
    schultz_label_row *rows;
    uint32_t piece_count;
    uint32_t count = 0u;
    uint32_t align;
    float spacing;
    float top;
    uint32_t i;

    *out_rows = NULL;
    if (label == NULL || fonts == NULL || font == SCHULTZ_HANDLE_NONE ||
        label->text[0] == '\0' ||
        schultz_font_get_metrics(fonts, font, &metrics) != SCHULTZ_OK) {
        return 0u;
    }
    piece_count = schultz_spans_pieces(fonts, label->spans, label->span_count,
                                       font, arena, &pieces);
    if (schultz_text_wrap_pieces(fonts, pieces, piece_count, font,
                                 label->text, (int32_t)label->length,
                                 label->wrap ? bounds.width : -1.0f, arena,
                                 &lines, &count) != SCHULTZ_OK ||
        count == 0u) {
        return 0u;
    }
    rows = (schultz_label_row *)schultz_arena_alloc(
        arena, (size_t)count * sizeof(*rows), _Alignof(schultz_label_row));
    if (rows == NULL) {
        return 0u;
    }

    align = (uint32_t)schultz_resolved_number(style, SCHULTZ_PROP_TEXT_ALIGN);
    spacing = schultz_resolved_number(style, SCHULTZ_PROP_LINE_SPACING);
    if (spacing <= 0.0f) {
        spacing = 1.0f;
    }
    if (piece_count > 0u) {
        line_pieces = (schultz_text_piece *)schultz_arena_alloc(
            arena, (size_t)piece_count * sizeof(*line_pieces),
            _Alignof(schultz_text_piece));
        if (line_pieces == NULL) {
            return 0u;
        }
    }
    top = bounds.y;

    for (i = 0u; i < count; i++) {
        schultz_label_row *row = &rows[i];
        schultz_text_run run;
        schultz_glyph *placed = NULL;
        uint32_t length = lines[i].end - lines[i].start;
        float line_top = top;
        float extra = 0.0f;
        uint32_t on_line = 0u;
        uint32_t g;

        row->start    = lines[i].start;
        row->end      = lines[i].end;
        row->top      = line_top;
        /*
         * Each line is as tall as the tallest face on it, which wrapping
         * already worked out. With one face throughout every line is the
         * same height, as it always was.
         */
        row->height   = lines[i].height * spacing;
        row->baseline = line_top + metrics.ascent;
        row->glyphs   = NULL;
        row->clusters = NULL;
        row->fonts    = NULL;
        row->count    = 0u;
        row->width    = lines[i].width;
        row->x        = bounds.x;
        top          += row->height;

        if (line_pieces != NULL) {
            on_line = schultz_pieces_on_line(pieces, piece_count,
                                                lines[i].start, lines[i].end,
                                                line_pieces);
        }
        if (schultz_text_shape_pieces(fonts, line_pieces, on_line, font,
                                      label->text + lines[i].start,
                                      (int32_t)length, SCHULTZ_DIR_AUTO,
                                      arena, &run) != SCHULTZ_OK ||
            run.count == 0u) {
            continue;
        }
        /* Everything on the line sits on one baseline, and the tallest face
         * is what puts it where it is. */
        row->baseline = line_top + run.ascent;
        if (label->ellipsize) {
            schultz_label_cut(fonts, font, label->text + lines[i].start,
                              length, bounds.width, arena, &run);
            if (run.count == 0u) {
                row->width = 0.0f;
                continue;
            }
        }
        placed = (schultz_glyph *)schultz_arena_alloc(
            arena, (size_t)run.count * sizeof(*placed),
            _Alignof(schultz_glyph));
        if (placed == NULL) {
            continue;
        }
        for (g = 0u; g < run.count; g++) {
            placed[g] = run.glyphs[g];
        }

        /* The last line of a justified block keeps its natural width. */
        if (align == SCHULTZ_TEXT_ALIGN_JUSTIFY && (i + 1u) < count &&
            run.direction != SCHULTZ_DIR_RTL &&
            bounds.width > run.width) {
            extra = bounds.width - run.width;
            schultz_label_justify(placed, run.clusters, run.count,
                                  label->text + lines[i].start, length,
                                  extra);
        }

        row->width = run.width + extra;
        switch (align) {
        case SCHULTZ_TEXT_ALIGN_CENTER:
            row->x = bounds.x + (bounds.width - row->width) * 0.5f;
            break;
        case SCHULTZ_TEXT_ALIGN_END:
            row->x = bounds.x + bounds.width - row->width;
            break;
        default:
            row->x = bounds.x;
            break;
        }

        for (g = 0u; g < run.count; g++) {
            placed[g].x += row->x;
            placed[g].y  = row->baseline;
        }
        row->glyphs   = placed;
        row->clusters = run.clusters;
        row->fonts    = run.fonts;
        row->count    = run.count;
    }

    *out_rows = rows;
    return count;
}

/*
 * Which row an end of the selection sits on.
 *
 * An offset at a wrap point belongs to two rows at once: it is the end of one
 * and the start of the next. Which one is wanted depends on which end of the
 * selection is asking. The start of a selection sits at the beginning of the
 * following line, and the end of one sits at the end of the preceding line,
 * which is where a reader expects to see them. Taking the first match for
 * both puts the opening grip at the far right of the line above the text it
 * is supposed to be marking.
 *
 * Returns the row index, which is always a real one when count is not zero.
 */
static uint32_t schultz_label_row_for(const schultz_label_row *rows,
                                      uint32_t count, uint32_t offset,
                                      uint32_t leading)
{
    uint32_t i;

    if (leading) {
        for (i = 0u; i < count; i++) {
            if (offset < rows[i].end) {
                return i;
            }
        }
        return count - 1u;
    }
    for (i = count; i > 0u; i--) {
        if (offset > rows[i - 1u].start) {
            return i - 1u;
        }
    }
    return 0u;
}

/* Where a caret sits on a row, for a byte offset relative to the row. */
static float schultz_label_row_x(const schultz_label_row *row,
                                 uint32_t offset)
{
    uint32_t i;

    /*
     * Glyphs are in visual order, so scanning for the first that starts at or
     * after the offset gives a position for either direction. This is what
     * schultz_text_caret_x does, repeated here because these glyphs have
     * already been moved into place and justified.
     */
    for (i = 0u; i < row->count; i++) {
        if (row->clusters[i] >= offset) {
            return row->glyphs[i].x;
        }
    }
    return row->x + row->width;
}

/* The byte offset, relative to the row, nearest a position along it. */
static uint32_t schultz_label_row_offset(const schultz_label_row *row,
                                         float x)
{
    uint32_t best = row->end - row->start;
    float best_distance;
    uint32_t i;

    if (row->count == 0u) {
        return 0u;
    }
    best_distance = schultz_label_distance(x, row->x + row->width);
    for (i = 0u; i < row->count; i++) {
        float distance = schultz_label_distance(x, row->glyphs[i].x);

        if (distance < best_distance) {
            best_distance = distance;
            best = row->clusters[i];
        }
    }
    return best;
}

/* The byte offset a point in the label lands on. */
static uint32_t schultz_label_offset_at(schultz_tree *tree,
                                        schultz_handle node,
                                        const schultz_label_data *label,
                                        schultz_point local)
{
    schultz_arena scratch;
    schultz_label_row *rows = NULL;
    schultz_rect bounds;
    uint32_t count;
    uint32_t offset = 0u;
    uint32_t i;

    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        schultz_arena_init(&scratch, 0) != SCHULTZ_OK) {
        return 0u;
    }
    count = schultz_label_rows(tree, node, label, bounds, &scratch, &rows);
    if (count > 0u) {
        float y = bounds.y + local.y;

        /* Above the first line takes the first, below the last takes the
         * last, which is what dragging off the top or bottom should do. */
        i = 0u;
        while ((i + 1u) < count && y >= rows[i + 1u].top) {
            i++;
        }
        offset = rows[i].start +
                 schultz_label_row_offset(&rows[i], bounds.x + local.x);
    }
    schultz_arena_free(&scratch);
    return offset;
}

/*
 * Height depends on the width offered, which is the whole reason measure
 * takes an available size. An unbounded width means one line at its natural
 * length, which is what a popup or a table column asks for.
 */
static int32_t schultz_label_measure(schultz_tree *tree, schultz_handle node,
                                     float avail_w, float avail_h,
                                     schultz_size *out_size)
{
    const schultz_label_data *label =
        (const schultz_label_data *)schultz_node_widget_data(tree, node);
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_handle font = schultz_label_font(tree, node);
    schultz_font_metrics metrics;
    schultz_arena scratch;
    const schultz_text_line *lines;
    schultz_text_piece *pieces = NULL;
    uint32_t piece_count;
    uint32_t count = 0;
    float widest = 0.0f;
    uint32_t i;

    (void)avail_h;
    *out_size = schultz_size_make(0.0f, 0.0f);

    if (label == NULL || fonts == NULL || font == SCHULTZ_HANDLE_NONE ||
        label->text[0] == '\0') {
        return SCHULTZ_OK;
    }
    if (schultz_font_get_metrics(fonts, font, &metrics) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }

    /*
     * Measurement needs scratch for shaping. Layout runs outside the frame
     * arena's lifetime, so this uses its own and releases it here rather than
     * holding memory between passes.
     */
    if (schultz_arena_init(&scratch, 0) != SCHULTZ_OK) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    piece_count = schultz_spans_pieces(fonts, label->spans, label->span_count,
                                       font, &scratch, &pieces);
    if (schultz_text_wrap_pieces(fonts, pieces, piece_count, font,
                                 label->text, -1,
                                 label->wrap ? avail_w : -1.0f, &scratch,
                                 &lines, &count) == SCHULTZ_OK) {
        float spacing = schultz_resolved_number(
            schultz_widget_style(tree, node), SCHULTZ_PROP_LINE_SPACING);
        float tall = 0.0f;

        if (spacing <= 0.0f) {
            spacing = 1.0f;
        }
        for (i = 0; i < count; i++) {
            if (lines[i].width > widest) {
                widest = lines[i].width;
            }
            /*
             * Added up rather than multiplied by the count, because a line
             * carrying a larger face is taller than the rest and a label
             * measured as though every line matched the first would be cut
             * off at the bottom.
             */
            tall += lines[i].height * spacing;
        }
        *out_size = schultz_size_make(widest, tall);
    }

    schultz_arena_free(&scratch);
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_label_pane = {
    schultz_label_measure, schultz_leaf_arrange
};

/* The lower and upper ends of a label's selection, in byte offsets. */
static uint32_t schultz_label_low(const schultz_label_data *label)
{
    return (label->anchor < label->caret) ? label->anchor : label->caret;
}

static uint32_t schultz_label_high(const schultz_label_data *label)
{
    return (label->anchor > label->caret) ? label->anchor : label->caret;
}

/*
 * The band behind the selected part of one row. Drawn before the glyphs, so
 * the text sits on top of it rather than being covered by it.
 *
 * The band runs from the lower position to the higher one. For a line that
 * mixes directions the selected characters are not one continuous stretch on
 * screen, and this draws the span that covers them rather than a piece per
 * direction, which is the same simplification the text field makes.
 */
/*
 * Where a byte range starts and ends along a row, in absolute coordinates.
 *
 * The selection band, a span's background and the lines under and through it
 * all want the same two numbers, so they are worked out in one place. Returns
 * zero when none of the range is on this row, or when it comes to nothing.
 */
static int32_t schultz_label_span_edges(const schultz_label_row *row,
                                        uint32_t low, uint32_t high,
                                        float *out_left, float *out_right)
{
    float a;
    float b;

    if (high <= row->start || low >= row->end || row->count == 0u) {
        return 0;
    }
    a = schultz_label_row_x(row, (low > row->start) ? (low - row->start) : 0u);
    b = schultz_label_row_x(row, (high < row->end) ? (high - row->start)
                                                  : (row->end - row->start));
    *out_left  = (a < b) ? a : b;
    *out_right = (a < b) ? b : a;
    return (*out_right > *out_left) ? 1 : 0;
}

static void schultz_label_paint_band(schultz_draw_list *list,
                                     const schultz_label_row *row,
                                     uint32_t low, uint32_t high,
                                     schultz_paint paint)
{
    float left;
    float right;

    if (!schultz_label_span_edges(row, low, high, &left, &right)) {
        return;
    }
    schultz_draw_fill_rect(list,
        schultz_rect_make(left, row->top, right - left, row->height), paint);
}

/*
 * The span covering a byte, or NULL where none does.
 *
 * The first match wins, which is what the text layer does when it decides
 * which face a byte is set in. The two have to agree, or a phrase would be
 * shaped in one span's face and painted in another's colour.
 */
/* The colour one glyph on a row is drawn in. */
static schultz_color schultz_label_glyph_color(
    const schultz_label_data *label, const schultz_label_row *row,
    uint32_t glyph, schultz_color base)
{
    const schultz_span *span;

    /* Clusters are offsets into the line, and spans are offsets into the
     * whole text, so one has to be moved to meet the other. */
    span = schultz_spans_at(label->spans, label->span_count,
                            row->start + row->clusters[glyph]);
    if (span == NULL || span->color.a == 0u) {
        return base;
    }
    return span->color;
}

/* The face a byte on a row is set in. */
static schultz_handle schultz_label_row_font(const schultz_label_row *row,
                                             uint32_t offset,
                                             schultz_handle fallback)
{
    uint32_t i;

    if (row->fonts == NULL) {
        return fallback;
    }
    for (i = 0u; i < row->count; i++) {
        if (row->clusters[i] >= offset) {
            return row->fonts[i];
        }
    }
    return fallback;
}

/*
 * A row's glyphs, in the colours its spans ask for.
 *
 * A draw command names one face and one paint, so this starts a new one
 * wherever either changes. A label with no spans is one command per face, the
 * same as it always was.
 */
static void schultz_label_draw_row(schultz_draw_list *list,
                                   const schultz_label_data *label,
                                   const schultz_label_row *row,
                                   schultz_handle font, schultz_color base)
{
    uint32_t start = 0u;
    uint32_t i;

    if (row->count == 0u) {
        return;
    }
    if (label->span_count == 0u) {
        schultz_widget_draw_glyphs(list, font, row->fonts, row->glyphs,
                                   row->count, schultz_paint_solid(base));
        return;
    }
    for (i = 1u; i <= row->count; i++) {
        schultz_color here = schultz_label_glyph_color(label, row, start,
                                                       base);
        int32_t split = (i == row->count);

        if (!split) {
            schultz_handle a = (row->fonts != NULL) ? row->fonts[i] : font;
            schultz_handle b = (row->fonts != NULL) ? row->fonts[start] : font;

            split = (a != b) ||
                    !schultz_color_equals(
                        here, schultz_label_glyph_color(label, row, i, base));
        }
        if (split) {
            schultz_handle face = (row->fonts != NULL) ? row->fonts[start]
                                                       : font;

            schultz_draw_glyph_run(list, face, row->glyphs + start, i - start,
                                   schultz_paint_solid(here));
            start = i;
        }
    }
}

/* Whatever a span asks to be painted behind its text. */
static void schultz_label_paint_backgrounds(schultz_draw_list *list,
                                            const schultz_label_data *label,
                                            const schultz_label_row *row)
{
    uint32_t i;

    for (i = 0u; i < label->span_count; i++) {
        const schultz_span *span = &label->spans[i];

        if (span->background.a == 0u) {
            continue;
        }
        schultz_label_paint_band(list, row, span->start, span->end,
                                 schultz_paint_solid(span->background));
    }
}

/*
 * The lines under and through a span's text.
 *
 * Both come from the face the span is set in, so a phrase at twice the size
 * is underlined twice as far below the baseline and twice as thickly. A rule
 * thinner than a pixel would disappear at small sizes, so it is never less
 * than one.
 */
static void schultz_label_paint_rules(schultz_draw_list *list,
                                      const schultz_font_system *fonts,
                                      const schultz_label_data *label,
                                      const schultz_label_row *row,
                                      schultz_handle font, schultz_color base)
{
    uint32_t i;

    for (i = 0u; i < label->span_count; i++) {
        const schultz_span *span = &label->spans[i];
        schultz_font_metrics metrics;
        schultz_handle face;
        schultz_color ink;
        float left;
        float right;
        float thick;

        if (!span->underline && !span->strikethrough) {
            continue;
        }
        if (!schultz_label_span_edges(row, span->start, span->end, &left,
                                      &right)) {
            continue;
        }
        face = schultz_label_row_font(
            row, (span->start > row->start) ? (span->start - row->start) : 0u,
            font);
        if (schultz_font_get_metrics(fonts, face, &metrics) != SCHULTZ_OK) {
            continue;
        }
        ink = (span->color.a != 0u) ? span->color : base;

        if (span->underline) {
            thick = (metrics.underline_thickness < 1.0f)
                        ? 1.0f : metrics.underline_thickness;
            schultz_draw_fill_rect(list,
                schultz_rect_make(left,
                                  row->baseline + metrics.underline_position,
                                  right - left, thick),
                schultz_paint_solid(ink));
        }
        if (span->strikethrough) {
            thick = (metrics.strikeout_thickness < 1.0f)
                        ? 1.0f : metrics.strikeout_thickness;
            schultz_draw_fill_rect(list,
                schultz_rect_make(left,
                                  row->baseline - metrics.strikeout_position,
                                  right - left, thick),
                schultz_paint_solid(ink));
        }
    }
}

static int32_t schultz_label_paint(schultz_tree *tree, schultz_handle node,
                                   schultz_draw_list *list,
                                   schultz_arena *arena)
{
    const schultz_label_data *label =
        (const schultz_label_data *)schultz_node_widget_data(tree, node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_handle font = schultz_label_font(tree, node);
    schultz_color color = schultz_resolved_color(style,
                                                 SCHULTZ_PROP_TEXT_COLOR);
    schultz_label_row *rows = NULL;
    schultz_rect bounds;
    uint32_t count;
    uint32_t low;
    uint32_t high;
    uint32_t i;

    if (label == NULL || font == SCHULTZ_HANDLE_NONE ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    count = schultz_label_rows(tree, node, label, bounds, arena, &rows);
    if (count == 0u) {
        return SCHULTZ_OK;
    }

    low  = schultz_label_low(label);
    high = schultz_label_high(label);

    /*
     * A label that ellipsizes has promised to stay inside its box, and the
     * cut alone does not quite keep that promise: a line the shaper leaves
     * fractionally wide, and right to left text, which is not cut at all,
     * both still reach past the edge. The clip is what makes it certain.
     *
     * Only around the text. The touch grips hang below the last line on
     * purpose, and the node's paint margin already allows for them.
     */
    if (label->ellipsize) {
        schultz_draw_clip_begin(list, bounds);
    }

    /*
     * A span's own background goes down first, then the selection over it,
     * then the words. A selection has to be visible over a highlighted
     * phrase, and both have to be behind the text they are marking.
     */
    for (i = 0u; i < count; i++) {
        schultz_label_paint_backgrounds(list, label, &rows[i]);
    }

    if (label->selectable && high > low) {
        schultz_paint band = schultz_resolved_paint(style,
                                                    SCHULTZ_PROP_SELECTION_COLOR);

        for (i = 0u; i < count; i++) {
            schultz_label_paint_band(list, &rows[i], low, high, band);
        }
    }

    if (color.a != 0u) {
        schultz_font_system *fonts =
            (schultz_font_system *)schultz_tree_font_system(tree);

        for (i = 0u; i < count; i++) {
            schultz_label_draw_row(list, label, &rows[i], font, color);
            schultz_label_paint_rules(list, fonts, label, &rows[i], font,
                                      color);
        }
    }

    if (label->ellipsize) {
        schultz_draw_clip_end(list);
    }

    /*
     * The grips a finger moves the two ends with. They hang below the line
     * their end sits on, so the finger covers the grip rather than the text
     * it is placing, and the node's paint margin has to allow for them.
     */
    if (label->grips && high > low) {
        schultz_paint grip = schultz_resolved_paint(style,
                                                    SCHULTZ_PROP_SELECTION_COLOR);
        uint32_t ends[2];
        uint32_t e;

        ends[0] = low;
        ends[1] = high;
        for (e = 0u; e < 2u; e++) {
            i = schultz_label_row_for(rows, count, ends[e], (e == 0u));
            if (rows[i].count > 0u) {
                float x = schultz_label_row_x(&rows[i],
                                              ends[e] - rows[i].start);
                float y = rows[i].top + rows[i].height;

                schultz_draw_fill_ellipse(list,
                    schultz_rect_make(x - SCHULTZ_LABEL_GRIP, y,
                                      SCHULTZ_LABEL_GRIP * 2.0f,
                                      SCHULTZ_LABEL_GRIP * 2.0f), grip);
            }
        }
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_label_widget;

/*
 * Word stepping, defined with the editable text widget further down. A label
 * needs the same notion of a word for its double click, and there is no sense
 * in a second one that disagrees.
 */
static uint32_t schultz_word_next(const char *text, uint32_t length,
                                  uint32_t at);
static uint32_t schultz_word_prev(const char *text, uint32_t at);

/* A node's label state, or NULL when the node is not a label. */
static schultz_label_data *schultz_label_of(schultz_tree *tree,
                                            schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_label_widget) {
        return NULL;
    }
    return (schultz_label_data *)schultz_node_widget_data(tree, node);
}

/*
 * Records a selection change: repaints, and reports what is selected as the
 * node's accessible value. A label's name is its whole text, so the value is
 * the part of it that is currently selected, which is what a reader announces
 * when the selection moves.
 */
static void schultz_label_selection_changed(schultz_tree *tree,
                                            schultz_handle node,
                                            const schultz_label_data *label)
{
    uint32_t low = schultz_label_low(label);
    uint32_t high = schultz_label_high(label);

    if (high > low) {
        char *piece = (char *)malloc((size_t)(high - low) + 1u);

        if (piece != NULL) {
            memcpy(piece, label->text + low, high - low);
            piece[high - low] = '\0';
            schultz_node_set_value(tree, node, piece);
            free(piece);
        }
    } else {
        schultz_node_set_value(tree, node, NULL);
    }
    schultz_node_invalidate(tree, node);
}

/* Drops whatever a label had selected, and repaints it if anything changed. */
static void schultz_label_clear(schultz_tree *tree, schultz_handle node,
                                schultz_label_data *label)
{
    if (label->anchor == label->caret && !label->grips) {
        return;
    }
    label->anchor = label->caret;
    label->grips  = 0u;
    label->grip   = 0u;
    schultz_node_set_paint_margin(tree, node, 0.0f);
    schultz_label_selection_changed(tree, node, label);
}

/*
 * Takes the tree's selection away from whoever had it.
 *
 * The tree names the owner but does not know what a selection is made of, so
 * clearing the old one is done here. Only a label can own one today; a second
 * widget that can would have to be cleared here as well, which is the cost of
 * keeping the tree ignorant of what it is pointing at.
 */
static void schultz_label_take_selection(schultz_tree *tree,
                                         schultz_handle node)
{
    schultz_handle previous = schultz_tree_selection_owner(tree);

    if (previous != node && previous != SCHULTZ_HANDLE_NONE) {
        schultz_label_data *other = schultz_label_of(tree, previous);

        if (other != NULL) {
            schultz_label_clear(tree, previous, other);
        } else {
            /*
             * Not a label, so it may be a selection area holding a range
             * across several widgets. One selection at a time means that one
             * ends here too, or its highlight stays on screen behind this.
             */
            schultz_selection_area_clear(tree, previous);
        }
    }
    schultz_tree_set_selection_owner(tree, node);
}

/* Puts a label's selected text on the clipboard. */
static void schultz_label_copy(schultz_tree *tree,
                               const schultz_label_data *label)
{
    uint32_t low = schultz_label_low(label);
    uint32_t high = schultz_label_high(label);
    char *piece;

    if (high <= low) {
        return;
    }
    piece = (char *)malloc((size_t)(high - low) + 1u);
    if (piece == NULL) {
        return;
    }
    memcpy(piece, label->text + low, high - low);
    piece[high - low] = '\0';
    schultz_tree_clipboard_write(tree, piece);
    free(piece);
}

/* Turns the touch grips on, and makes room for them outside the bounds. */
static void schultz_label_show_grips(schultz_tree *tree, schultz_handle node,
                                     schultz_label_data *label)
{
    label->grips = 1u;
    schultz_node_set_paint_margin(tree, node, SCHULTZ_LABEL_GRIP * 2.0f);
}

/* Stops the hold timer, and with it the ticking that drives it. */
static void schultz_label_stop_hold(schultz_tree *tree, schultz_handle node,
                                    schultz_label_data *label)
{
    if (label->holding) {
        label->holding = 0u;
        label->held_ms = 0u;
        schultz_node_set_animating(tree, node, 0);
    }
}

/* Selects the word a byte offset lands in. */
static void schultz_label_select_word(schultz_label_data *label, uint32_t at)
{
    label->anchor = schultz_word_prev(label->text,
        schultz_word_next(label->text, label->length, at));
    label->caret = schultz_word_next(label->text, label->length,
                                     label->anchor);
}

/*
 * Which grip a press landed on, or 0 for neither. Measured against the grip's
 * own circle rather than the text, because the point of a grip is to be
 * bigger than the character it is moving.
 */
static uint32_t schultz_label_grip_at(schultz_tree *tree, schultz_handle node,
                                      const schultz_label_data *label,
                                      schultz_point local)
{
    schultz_arena scratch;
    schultz_label_row *rows = NULL;
    schultz_rect bounds;
    uint32_t ends[2];
    uint32_t hit = 0u;
    uint32_t count;
    uint32_t e;

    if (!label->grips ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        schultz_arena_init(&scratch, 0) != SCHULTZ_OK) {
        return 0u;
    }
    ends[0] = schultz_label_low(label);
    ends[1] = schultz_label_high(label);
    count = schultz_label_rows(tree, node, label, bounds, &scratch, &rows);

    for (e = 0u; e < 2u && hit == 0u && count > 0u; e++) {
        uint32_t i = schultz_label_row_for(rows, count, ends[e], (e == 0u));

        if (rows[i].count > 0u) {
            float x = schultz_label_row_x(&rows[i], ends[e] - rows[i].start);
            float y = rows[i].top + rows[i].height + SCHULTZ_LABEL_GRIP;

            if (schultz_label_distance(bounds.x + local.x, x) <=
                    SCHULTZ_LABEL_GRIP * 1.5f &&
                schultz_label_distance(bounds.y + local.y, y) <=
                    SCHULTZ_LABEL_GRIP * 1.5f) {
                hit = e + 1u;
            }
        }
    }
    schultz_arena_free(&scratch);
    return hit;
}

/* Moves one end of the selection, keeping the other where it is. */
static void schultz_label_move_end(schultz_tree *tree, schultz_handle node,
                                   schultz_label_data *label, uint32_t which,
                                   uint32_t to)
{
    uint32_t low = schultz_label_low(label);
    uint32_t high = schultz_label_high(label);

    if (which == 1u) {
        label->anchor = high;
        label->caret  = to;
    } else {
        label->anchor = low;
        label->caret  = to;
    }
    schultz_label_selection_changed(tree, node, label);
}

/*
 * Which clickable span a point lands on.
 *
 * The span's painted rectangle on its row rather than the nearest byte: a
 * press to the right of the last word is not a press on the last word, and
 * treating it as one would make the empty part of a line activate whatever
 * ended it.
 */
static int32_t schultz_label_clickable_span(schultz_tree *tree,
                                            schultz_handle node,
                                            schultz_point local,
                                            uint32_t *out_index,
                                            uint64_t *out_tag)
{
    const schultz_label_data *label =
        (const schultz_label_data *)schultz_node_widget_data(tree, node);
    schultz_arena scratch;
    schultz_label_row *rows = NULL;
    schultz_rect bounds;
    uint32_t count;
    uint32_t row;
    int32_t found = 0;

    if (label == NULL || label->span_count == 0u) {
        return 0;
    }
    /*
     * A finger that rested long enough to start selecting has not tapped,
     * however still it was held. The press that made a selection must not
     * also press what is under it.
     */
    if (label->grips || label->dragged) {
        return 0;
    }
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        schultz_arena_init(&scratch, 0) != SCHULTZ_OK) {
        return 0;
    }

    count = schultz_label_rows(tree, node, label, bounds, &scratch, &rows);
    for (row = 0u; row < count && !found; row++) {
        float x = bounds.x + local.x;
        float y = bounds.y + local.y;
        uint32_t i;

        if (y < rows[row].top || y >= rows[row].top + rows[row].height) {
            continue;
        }
        for (i = 0u; i < label->span_count; i++) {
            float left;
            float right;

            if (!label->spans[i].clickable) {
                continue;
            }
            if (!schultz_label_span_edges(&rows[row], label->spans[i].start,
                                          label->spans[i].end, &left,
                                          &right)) {
                continue;
            }
            if (x >= left && x < right) {
                *out_index = i;
                *out_tag   = label->spans[i].tag;
                found = 1;
                break;
            }
        }
    }
    schultz_arena_free(&scratch);
    return found;
}

/*
 * The pointer shape over a label, which says what a press would do before it
 * happens. A hand over a stretch that can be pressed, and otherwise whatever
 * the label would have shown anyway.
 */
static void schultz_label_hover(schultz_tree *tree, schultz_handle node,
                                const schultz_label_data *label,
                                schultz_point local)
{
    uint32_t which = 0u;
    uint64_t tag = 0u;
    uint32_t want;

    if (label->span_count == 0u) {
        return;
    }
    want = schultz_label_clickable_span(tree, node, local, &which, &tag)
               ? SCHULTZ_CURSOR_POINTER
               : (label->selectable ? SCHULTZ_CURSOR_TEXT
                                    : SCHULTZ_CURSOR_DEFAULT);
    if (schultz_node_cursor(tree, node) != want) {
        schultz_node_set_cursor(tree, node, want);
    }
}

static int32_t schultz_label_event(schultz_tree *tree, schultz_handle node,
                                   const schultz_event *event)
{
    schultz_label_data *label =
        (schultz_label_data *)schultz_node_widget_data(tree, node);

    if (label == NULL) {
        return SCHULTZ_OK;
    }
    /*
     * The pointer shape is decided whether or not the words can be selected,
     * because a stretch that can be pressed is worth showing on a label that
     * is only there to be read.
     */
    if (event->type == SCHULTZ_EVENT_MOUSE_MOVE ||
        event->type == SCHULTZ_EVENT_MOUSE_ENTER) {
        schultz_label_hover(tree, node, label, event->local);
    }
    if (!label->selectable) {
        return SCHULTZ_OK;
    }

    switch (event->type) {
    case SCHULTZ_EVENT_MOUSE_DOWN: {
        uint32_t at;

        label->grip = schultz_label_grip_at(tree, node, label, event->local);
        if (label->grip != 0u) {
            return SCHULTZ_EVENT_CONSUMED;
        }

        /*
         * Inside a selection area the drag belongs to the area, because a
         * selection that crosses widgets cannot be run by any one of them:
         * whatever takes the press is the only thing told about the motion
         * after it. So the press is let through and the area picks it up on
         * its way to the root.
         *
         * A finger is the exception. Touch selection starts with a hold
         * rather than a drag, and that hold lives here; selecting across
         * widgets by touch is not built yet.
         */
        if (event->source != SCHULTZ_POINTER_TOUCH &&
            schultz_selection_area_of(tree, node) != SCHULTZ_HANDLE_NONE) {
            return SCHULTZ_OK;
        }

        /*
         * A finger has to rest before it selects. Dragging a finger across
         * text is how a page is scrolled, so starting a selection on the
         * first movement would take that gesture away.
         */
        if (event->source == SCHULTZ_POINTER_TOUCH) {
            schultz_label_clear(tree, node, label);
            label->holding = 1u;
            label->held_ms = 0u;
            label->held_at = event->local;
            schultz_node_set_animating(tree, node, 1);
            return SCHULTZ_OK;
        }

        label->dragged = 0u;
        at = schultz_label_offset_at(tree, node, label, event->local);
        if (event->click_count >= 3u) {
            label->anchor = 0u;
            label->caret  = label->length;
        } else if (event->click_count == 2u) {
            schultz_label_select_word(label, at);
        } else {
            label->anchor = at;
            label->caret  = at;
        }
        label->dragging = 1u;
        schultz_label_take_selection(tree, node);
        schultz_label_selection_changed(tree, node, label);
        return SCHULTZ_EVENT_CONSUMED;
    }

    case SCHULTZ_EVENT_DRAG:
        /* The area is running this drag; see the press above. */
        if (label->grip == 0u && !label->dragging &&
            event->source != SCHULTZ_POINTER_TOUCH &&
            schultz_selection_area_of(tree, node) != SCHULTZ_HANDLE_NONE) {
            return SCHULTZ_OK;
        }
        if (label->grip != 0u) {
            schultz_label_move_end(tree, node, label, label->grip,
                schultz_label_offset_at(tree, node, label, event->local));
            return SCHULTZ_EVENT_CONSUMED;
        }
        if (label->holding) {
            /* Moved before the hold finished, so it was a drag after all. */
            if (schultz_label_distance(event->local.x, label->held_at.x) >
                    SCHULTZ_LABEL_HOLD_SLOP ||
                schultz_label_distance(event->local.y, label->held_at.y) >
                    SCHULTZ_LABEL_HOLD_SLOP) {
                schultz_label_stop_hold(tree, node, label);
            }
            return SCHULTZ_OK;
        }
        if (label->dragging) {
            label->caret = schultz_label_offset_at(tree, node, label,
                                                   event->local);
            schultz_label_selection_changed(tree, node, label);
            return SCHULTZ_EVENT_CONSUMED;
        }
        return SCHULTZ_OK;

    case SCHULTZ_EVENT_MOUSE_UP:
        /*
         * Worked out here rather than when the click arrives, because by then
         * the drag has been let go of and there would be nothing left to tell
         * a selection from a press.
         */
        label->dragged = (label->dragging && label->anchor != label->caret)
                             ? 1u : 0u;
        schultz_label_stop_hold(tree, node, label);
        label->dragging = 0u;
        label->grip     = 0u;
        return SCHULTZ_OK;

    case SCHULTZ_EVENT_KEY_DOWN:
        if (event->modifiers & SCHULTZ_MOD_CTRL) {
            switch (event->key) {
            case 'a':
                label->anchor = 0u;
                label->caret  = label->length;
                schultz_label_take_selection(tree, node);
                schultz_label_selection_changed(tree, node, label);
                return SCHULTZ_EVENT_CONSUMED;
            case 'c':
                schultz_label_copy(tree, label);
                return SCHULTZ_EVENT_CONSUMED;
            default:
                break;
            }
        }
        return SCHULTZ_OK;

    default:
        break;
    }
    return SCHULTZ_OK;
}

/*
 * Counts out the hold that starts a touch selection. Nothing else in a label
 * moves on its own, so the label is only ticked while a finger is down.
 */
static int32_t schultz_label_tick(schultz_tree *tree, schultz_handle node,
                                  uint64_t now_ms, uint32_t elapsed_ms)
{
    schultz_label_data *label =
        (schultz_label_data *)schultz_node_widget_data(tree, node);

    (void)now_ms;
    if (label == NULL || !label->holding) {
        return 0;
    }
    label->held_ms += elapsed_ms;
    if (label->held_ms < SCHULTZ_LABEL_HOLD_MS) {
        return 0;
    }

    schultz_label_stop_hold(tree, node, label);
    schultz_label_select_word(label,
        schultz_label_offset_at(tree, node, label, label->held_at));
    schultz_label_show_grips(tree, node, label);
    schultz_label_take_selection(tree, node);
    schultz_label_selection_changed(tree, node, label);
    return 1;
}

/* ------------------------------------------- taking part in a selection */

/*
 * How a label joins a selection that spans widgets.
 *
 * Almost nothing new: a label already keeps an anchor and a caret, paints
 * between them, and reports what is selected to a screen reader. A selection
 * area works out which labels are in the range and how much of each, then
 * sets those two offsets. So the four functions below are a thin skin over
 * what a label already did on its own.
 *
 * A label that was never made selectable reports no length, which is how it
 * declines without the area needing to know what a label is.
 */
static uint32_t schultz_label_select_length(schultz_tree *tree,
                                            schultz_handle node)
{
    const schultz_label_data *label = schultz_label_of(tree, node);

    if (label == NULL || !label->selectable) {
        return 0u;
    }
    return label->length;
}

static uint32_t schultz_label_select_offset_at(schultz_tree *tree,
                                               schultz_handle node,
                                               schultz_point local)
{
    schultz_label_data *label = schultz_label_of(tree, node);

    if (label == NULL || !label->selectable) {
        return 0u;
    }
    return schultz_label_offset_at(tree, node, label, local);
}

static void schultz_label_select_set_range(schultz_tree *tree,
                                           schultz_handle node,
                                           uint32_t low, uint32_t high)
{
    schultz_label_data *label = schultz_label_of(tree, node);

    if (label == NULL || !label->selectable) {
        return;
    }
    if (label->anchor == low && label->caret == high) {
        return; /* nothing moved, so nothing to repaint or announce */
    }
    label->anchor = low;
    label->caret  = high;
    schultz_label_selection_changed(tree, node, label);
}

static void schultz_label_select_range(schultz_tree *tree,
                                       schultz_handle node,
                                       uint32_t *out_low, uint32_t *out_high)
{
    const schultz_label_data *label = schultz_label_of(tree, node);

    *out_low  = 0u;
    *out_high = 0u;
    if (label == NULL || !label->selectable) {
        return;
    }
    *out_low  = schultz_label_low(label);
    *out_high = schultz_label_high(label);
}

static const char *schultz_label_select_text(schultz_tree *tree,
                                             schultz_handle node)
{
    const schultz_label_data *label = schultz_label_of(tree, node);

    return (label == NULL) ? NULL : label->text;
}

/* The spans, for a copy that carries the look of the words as well. */
static const schultz_span *schultz_label_select_spans(schultz_tree *tree,
                                                      schultz_handle node,
                                                      uint32_t *out_count)
{
    return schultz_label_spans(tree, node, out_count);
}

static const schultz_selectable_vtable schultz_label_selection_part = {
    schultz_label_select_length,
    schultz_label_select_offset_at,
    schultz_label_select_set_range,
    schultz_label_select_range,
    schultz_label_select_text,
    NULL,                         /* a label is text, not a picture */
    schultz_label_select_spans
};

static const schultz_widget_vtable schultz_label_widget = {
    .paint = schultz_label_paint, .event = schultz_label_event,
    .tick = schultz_label_tick, .destroy = schultz_label_destroy,
    .selectable = &schultz_label_selection_part,
    .clickable_span = schultz_label_clickable_span
};

/* Replaces a label's owned string with a copy of source. */
static int32_t schultz_label_store(schultz_label_data *label,
                                   const char *source)
{
    size_t length = (source == NULL) ? 0u : strlen(source);
    char *copy = (char *)malloc(length + 1u);

    if (copy == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    if (length > 0u) {
        memcpy(copy, source, length);
    }
    copy[length] = '\0';
    free(label->text);
    label->text   = copy;
    label->length = (uint32_t)length;
    return SCHULTZ_OK;
}

int32_t schultz_label_create(schultz_tree *tree, schultz_handle parent,
                             const char *text, schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_label_data *label;
    int32_t result = schultz_node_create(tree, parent, &node);

    if (result != SCHULTZ_OK) {
        return result;
    }
    label = (schultz_label_data *)calloc(1, sizeof(*label));
    if (label == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    label->wrap = 1u;
    result = schultz_label_store(label, text);
    if (result != SCHULTZ_OK) {
        free(label);
        schultz_node_destroy(tree, node);
        return result;
    }

    schultz_node_set_pane(tree, node, &schultz_label_pane);
    schultz_node_set_widget(tree, node, &schultz_label_widget, label);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_LABEL);
    /* The text is what a screen reader announces, so it is the name too. */
    schultz_node_set_name(tree, node, label->text);
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_label_set_spans(schultz_tree *tree, schultz_handle node,
                                const schultz_span *spans, uint32_t count)
{
    schultz_label_data *label;
    int32_t result;

    if (schultz_node_widget(tree, node) != &schultz_label_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (spans == NULL && count > 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    label = (schultz_label_data *)schultz_node_widget_data(tree, node);
    if (label == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /*
     * The same look as last time. A program that rebuilds its spans every
     * turn from a document it holds would otherwise remeasure the text on
     * every one of them, and measuring text is not cheap.
     */
    if (schultz_spans_same(label->spans, label->span_count, spans, count)) {
        return SCHULTZ_OK;
    }
    result = schultz_spans_store(&label->spans, &label->span_count, spans,
                                 count, label->length);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_invalidate_layout(tree, node);
    return schultz_node_invalidate(tree, node);
}

const schultz_span *schultz_label_spans(const schultz_tree *tree,
                                        schultz_handle node,
                                        uint32_t *out_count)
{
    const schultz_label_data *label;

    if (out_count != NULL) {
        *out_count = 0u;
    }
    if (schultz_node_widget(tree, node) != &schultz_label_widget) {
        return NULL;
    }
    label = (const schultz_label_data *)schultz_node_widget_data(tree, node);
    if (label == NULL) {
        return NULL;
    }
    if (out_count != NULL) {
        *out_count = label->span_count;
    }
    return label->spans;
}

int32_t schultz_label_set_text(schultz_tree *tree, schultz_handle node,
                               const char *text)
{
    schultz_label_data *label;
    int32_t result;

    if (schultz_node_widget(tree, node) != &schultz_label_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    label = (schultz_label_data *)schultz_node_widget_data(tree, node);
    if (label == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /*
     * The same words as last time. A clock, a frame count or a position
     * readout is set every turn and changes far less often than that, and
     * measuring text is not cheap.
     */
    if (label->text != NULL && text != NULL &&
        strcmp(label->text, text) == 0) {
        return SCHULTZ_OK;
    }
    result = schultz_label_store(label, text);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /* The offsets pointed into the string that has just been thrown away.
     * Spans are offsets too, and a span kept across a change of words marks
     * whatever now happens to sit at those bytes. */
    schultz_label_free_spans(label);
    label->grips  = 0u;
    label->grip   = 0u;
    label->anchor = 0u;
    label->caret  = 0u;
    schultz_node_set_paint_margin(tree, node, 0.0f);
    schultz_label_selection_changed(tree, node, label);
    schultz_node_set_name(tree, node, label->text);
    schultz_node_invalidate_layout(tree, node);
    return schultz_node_invalidate(tree, node);
}

const char *schultz_label_text(const schultz_tree *tree, schultz_handle node)
{
    const schultz_label_data *label;

    if (schultz_node_widget(tree, node) != &schultz_label_widget) {
        return NULL;
    }
    label = (const schultz_label_data *)schultz_node_widget_data(tree, node);
    return (label == NULL) ? NULL : label->text;
}

int32_t schultz_label_set_wrap(schultz_tree *tree, schultz_handle node,
                               int32_t wrap)
{
    schultz_label_data *label;

    if (schultz_node_widget(tree, node) != &schultz_label_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    label = (schultz_label_data *)schultz_node_widget_data(tree, node);
    if (label == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (label->wrap == (wrap ? 1u : 0u)) {
        return SCHULTZ_OK;
    }
    label->wrap = wrap ? 1u : 0u;
    schultz_node_invalidate_layout(tree, node);
    return schultz_node_invalidate(tree, node);
}

int32_t schultz_label_set_ellipsize(schultz_tree *tree, schultz_handle node,
                                    int32_t ellipsize)
{
    schultz_label_data *label;

    if (schultz_node_widget(tree, node) != &schultz_label_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    label = (schultz_label_data *)schultz_node_widget_data(tree, node);
    if (label == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    label->ellipsize = ellipsize ? 1u : 0u;
    /*
     * No layout invalidation. A label still asks for the width its whole text
     * needs, so what it is given does not change; only what it draws in the
     * width it ends up with does.
     */
    return schultz_node_invalidate(tree, node);
}

int32_t schultz_label_set_selectable(schultz_tree *tree, schultz_handle node,
                                     int32_t selectable)
{
    schultz_label_data *label = schultz_label_of(tree, node);

    if (label == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    label->selectable = selectable ? 1u : 0u;
    /*
     * The pointer says what a node will do before it is pressed, and text
     * that can be taken should look like it. A label that is not selectable
     * keeps the default arrow.
     */
    schultz_node_set_cursor(tree, node, label->selectable
                                            ? SCHULTZ_CURSOR_TEXT
                                            : SCHULTZ_CURSOR_DEFAULT);
    if (!label->selectable) {
        if (schultz_tree_selection_owner(tree) == node) {
            schultz_tree_set_selection_owner(tree, SCHULTZ_HANDLE_NONE);
        }
        schultz_label_stop_hold(tree, node, label);
        label->dragging = 0u;
        schultz_label_clear(tree, node, label);
    }
    return SCHULTZ_OK;
}

int32_t schultz_label_selectable(const schultz_tree *tree,
                                 schultz_handle node)
{
    const schultz_label_data *label;

    if (schultz_node_widget(tree, node) != &schultz_label_widget) {
        return 0;
    }
    label = (const schultz_label_data *)schultz_node_widget_data(tree, node);
    return (label == NULL) ? 0 : (int32_t)label->selectable;
}

int32_t schultz_label_set_selection(schultz_tree *tree, schultz_handle node,
                                    uint32_t start, uint32_t end)
{
    schultz_label_data *label = schultz_label_of(tree, node);

    if (label == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (!label->selectable) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    label->anchor = (start > label->length) ? label->length : start;
    label->caret  = (end > label->length) ? label->length : end;
    if (label->anchor != label->caret) {
        schultz_label_take_selection(tree, node);
    }
    schultz_label_selection_changed(tree, node, label);
    return SCHULTZ_OK;
}

int32_t schultz_label_selection(const schultz_tree *tree, schultz_handle node,
                                uint32_t *out_start, uint32_t *out_end)
{
    const schultz_label_data *label;

    if (out_start == NULL || out_end == NULL ||
        schultz_node_widget(tree, node) != &schultz_label_widget) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    label = (const schultz_label_data *)schultz_node_widget_data(tree, node);
    if (label == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    *out_start = schultz_label_low(label);
    *out_end   = schultz_label_high(label);
    return SCHULTZ_OK;
}

int32_t schultz_label_copy_selection(schultz_tree *tree, schultz_handle node)
{
    schultz_label_data *label = schultz_label_of(tree, node);

    if (label == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (schultz_label_high(label) <= schultz_label_low(label)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_label_copy(tree, label);
    return SCHULTZ_OK;
}

/* ------------------------------------------------------------ Separator */

/** @brief A separator's orientation. */
typedef struct {
    uint32_t orientation; /**< SCHULTZ_ORIENT_HORIZONTAL or _VERTICAL. */
} schultz_separator_data;

static int32_t schultz_separator_measure(schultz_tree *tree,
                                         schultz_handle node, float avail_w,
                                         float avail_h,
                                         schultz_size *out_size)
{
    const schultz_separator_data *data =
        (const schultz_separator_data *)schultz_node_widget_data(tree, node);
    float thickness = schultz_resolved_number(schultz_widget_style(tree, node),
                                              SCHULTZ_PROP_BORDER_WIDTH);

    (void)avail_w;
    (void)avail_h;
    if (thickness <= 0.0f) {
        thickness = 1.0f;
    }
    /* Thin on its own axis, nothing on the other: the parent stretches it. */
    if (data != NULL && data->orientation == SCHULTZ_ORIENT_VERTICAL) {
        *out_size = schultz_size_make(thickness, 0.0f);
    } else {
        *out_size = schultz_size_make(0.0f, thickness);
    }
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_separator_pane = {
    schultz_separator_measure, schultz_leaf_arrange
};

static int32_t schultz_separator_paint(schultz_tree *tree,
                                       schultz_handle node,
                                       schultz_draw_list *list,
                                       schultz_arena *arena)
{
    const schultz_separator_data *data =
        (const schultz_separator_data *)schultz_node_widget_data(tree, node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_stroke stroke = schultz_widget_stroke(style);
    schultz_rect b;
    schultz_point from;
    schultz_point to;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &b) != SCHULTZ_OK ||
        schultz_paint_is_invisible(stroke.paint)) {
        return SCHULTZ_OK;
    }
    if (stroke.width <= 0.0f) {
        stroke.width = 1.0f;
    }

    /* Down the middle of whichever axis is thin. */
    if (data != NULL && data->orientation == SCHULTZ_ORIENT_VERTICAL) {
        from = schultz_point_make(b.x + b.width * 0.5f, b.y);
        to   = schultz_point_make(b.x + b.width * 0.5f, b.y + b.height);
    } else {
        from = schultz_point_make(b.x, b.y + b.height * 0.5f);
        to   = schultz_point_make(b.x + b.width, b.y + b.height * 0.5f);
    }
    return schultz_draw_line(list, from, to, stroke);
}

static const schultz_widget_vtable schultz_separator_widget = {
    .paint = schultz_separator_paint, .destroy = free
};

int32_t schultz_separator_create(schultz_tree *tree, schultz_handle parent,
                                 uint32_t orientation,
                                 schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_separator_data *data;
    int32_t result = schultz_node_create(tree, parent, &node);

    if (result != SCHULTZ_OK) {
        return result;
    }
    data = (schultz_separator_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->orientation = orientation;

    schultz_node_set_pane(tree, node, &schultz_separator_pane);
    schultz_node_set_widget(tree, node, &schultz_separator_widget, data);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_GROUP);
    *out_node = node;
    return SCHULTZ_OK;
}

/* -------------------------------------------------------------- Ellipse */

static int32_t schultz_ellipse_paint(schultz_tree *tree, schultz_handle node,
                                     schultz_draw_list *list,
                                     schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_paint fill = schultz_resolved_paint(style,
                                                SCHULTZ_PROP_BACKGROUND);
    schultz_stroke stroke = schultz_widget_stroke(style);
    schultz_rect bounds;
    int32_t result;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    if (!schultz_paint_is_invisible(fill)) {
        result = schultz_draw_fill_ellipse(list, bounds, fill);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    if (!schultz_paint_is_invisible(stroke.paint) && stroke.width > 0.0f) {
        return schultz_draw_stroke_ellipse(list, bounds, stroke);
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_ellipse_widget = {
    .paint = schultz_ellipse_paint
};

int32_t schultz_ellipse_create(schultz_tree *tree, schultz_handle parent,
                               schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result = schultz_node_create(tree, parent, &node);

    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_set_widget(tree, node, &schultz_ellipse_widget, NULL);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_IMAGE);
    *out_node = node;
    return SCHULTZ_OK;
}

/* ----------------------------------------------------------------- Line */

/** @brief A line's endpoints, in the node's own space. */
typedef struct {
    schultz_point from; /**< One end. */
    schultz_point to;   /**< The other. */
} schultz_line_data;

static int32_t schultz_line_measure(schultz_tree *tree, schultz_handle node,
                                    float avail_w, float avail_h,
                                    schultz_size *out_size)
{
    const schultz_line_data *data =
        (const schultz_line_data *)schultz_node_widget_data(tree, node);

    (void)avail_w;
    (void)avail_h;
    if (data == NULL) {
        *out_size = schultz_size_make(0.0f, 0.0f);
        return SCHULTZ_OK;
    }
    /* Just far enough to hold both endpoints. */
    *out_size = schultz_size_make(
        (data->from.x > data->to.x) ? data->from.x : data->to.x,
        (data->from.y > data->to.y) ? data->from.y : data->to.y);
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_line_pane = {
    schultz_line_measure, schultz_leaf_arrange
};

static int32_t schultz_line_paint(schultz_tree *tree, schultz_handle node,
                                  schultz_draw_list *list,
                                  schultz_arena *arena)
{
    const schultz_line_data *data =
        (const schultz_line_data *)schultz_node_widget_data(tree, node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_stroke stroke = schultz_widget_stroke(style);
    schultz_rect b;

    (void)arena;
    if (data == NULL || schultz_paint_is_invisible(stroke.paint) ||
        schultz_node_absolute_bounds(tree, node, &b) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    if (stroke.width <= 0.0f) {
        stroke.width = 1.0f;
    }
    return schultz_draw_line(list,
        schultz_point_make(b.x + data->from.x, b.y + data->from.y),
        schultz_point_make(b.x + data->to.x, b.y + data->to.y), stroke);
}

static const schultz_widget_vtable schultz_line_widget = {
    .paint = schultz_line_paint, .destroy = free
};

int32_t schultz_line_create(schultz_tree *tree, schultz_handle parent,
                            schultz_point from, schultz_point to,
                            schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_line_data *data;
    int32_t result = schultz_node_create(tree, parent, &node);

    if (result != SCHULTZ_OK) {
        return result;
    }
    data = (schultz_line_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->from = from;
    data->to   = to;

    schultz_node_set_pane(tree, node, &schultz_line_pane);
    schultz_node_set_widget(tree, node, &schultz_line_widget, data);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_IMAGE);
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_line_set_points(schultz_tree *tree, schultz_handle node,
                                schultz_point from, schultz_point to)
{
    schultz_line_data *data;

    if (schultz_node_widget(tree, node) != &schultz_line_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data = (schultz_line_data *)schultz_node_widget_data(tree, node);
    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (data->from.x == from.x && data->from.y == from.y &&
        data->to.x == to.x && data->to.y == to.y) {
        return SCHULTZ_OK;
    }
    data->from = from;
    data->to   = to;
    /* Geometry changed, so it must be measured and painted again. */
    schultz_node_invalidate_layout(tree, node);
    return schultz_node_invalidate(tree, node);
}

/* ----------------------------------------------------------------- Path */

/** @brief A polygon's vertices, in the node's own space. */
typedef struct {
    schultz_point *points; /**< Owned copy of the vertices. */
    uint32_t       count;  /**< How many. */
} schultz_polygon_data;

static void schultz_polygon_destroy(void *data)
{
    schultz_polygon_data *path = (schultz_polygon_data *)data;

    if (path != NULL) {
        free(path->points);
        free(path);
    }
}

static int32_t schultz_polygon_measure(schultz_tree *tree, schultz_handle node,
                                       float avail_w, float avail_h,
                                       schultz_size *out_size)
{
    const schultz_polygon_data *path =
        (const schultz_polygon_data *)schultz_node_widget_data(tree, node);
    float right = 0.0f;
    float bottom = 0.0f;
    uint32_t i;

    (void)avail_w;
    (void)avail_h;
    if (path != NULL) {
        for (i = 0; i < path->count; i++) {
            if (path->points[i].x > right)  { right = path->points[i].x; }
            if (path->points[i].y > bottom) { bottom = path->points[i].y; }
        }
    }
    *out_size = schultz_size_make(right, bottom);
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_polygon_pane = {
    schultz_polygon_measure, schultz_leaf_arrange
};

static int32_t schultz_polygon_paint(schultz_tree *tree, schultz_handle node,
                                     schultz_draw_list *list,
                                     schultz_arena *arena)
{
    const schultz_polygon_data *path =
        (const schultz_polygon_data *)schultz_node_widget_data(tree, node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_paint fill = schultz_resolved_paint(style,
                                                SCHULTZ_PROP_BACKGROUND);
    schultz_rect b;
    schultz_point *placed;
    uint32_t i;

    if (path == NULL || path->count < 2u || schultz_paint_is_invisible(fill) ||
        schultz_node_absolute_bounds(tree, node, &b) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }

    /*
     * The stored points are node relative and the command list is in window
     * space, so they are offset into arena scratch for this frame.
     */
    placed = (schultz_point *)schultz_arena_alloc(
        arena, (size_t)path->count * sizeof(*placed),
        _Alignof(schultz_point));
    if (placed == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    for (i = 0; i < path->count; i++) {
        placed[i] = schultz_point_make(b.x + path->points[i].x,
                                       b.y + path->points[i].y);
    }
    return schultz_draw_fill_polygon(list, placed, path->count, fill,
                                     SCHULTZ_FILL_NONZERO);
}

static const schultz_widget_vtable schultz_polygon_widget = {
    .paint = schultz_polygon_paint, .destroy = schultz_polygon_destroy
};

/* Replaces a path's point array with a copy of the given one. */
static int32_t schultz_polygon_store(schultz_polygon_data *path,
                                     const schultz_point *points,
                                     uint32_t count)
{
    schultz_point *copy = (schultz_point *)malloc((size_t)count *
                                                  sizeof(*copy));

    if (copy == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, points, (size_t)count * sizeof(*copy));
    free(path->points);
    path->points = copy;
    path->count  = count;
    return SCHULTZ_OK;
}

int32_t schultz_polygon_create(schultz_tree *tree, schultz_handle parent,
                               const schultz_point *points, uint32_t count,
                               schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_polygon_data *path;
    int32_t result;

    if (points == NULL || count < 2u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }

    path = (schultz_polygon_data *)calloc(1, sizeof(*path));
    if (path == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_polygon_store(path, points, count);
    if (result != SCHULTZ_OK) {
        free(path);
        schultz_node_destroy(tree, node);
        return result;
    }

    schultz_node_set_pane(tree, node, &schultz_polygon_pane);
    schultz_node_set_widget(tree, node, &schultz_polygon_widget, path);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_IMAGE);
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_polygon_set_points(schultz_tree *tree, schultz_handle node,
                                   const schultz_point *points, uint32_t count)
{
    schultz_polygon_data *path;
    int32_t result;

    if (points == NULL || count < 2u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (schultz_node_widget(tree, node) != &schultz_polygon_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    path = (schultz_polygon_data *)schultz_node_widget_data(tree, node);
    if (path == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* An animated shape is handed a whole new set of vertices every turn,
     * and they are often the set it already has. */
    if (path->count == count && path->points != NULL &&
        memcmp(path->points, points, count * sizeof(*points)) == 0) {
        return SCHULTZ_OK;
    }
    result = schultz_polygon_store(path, points, count);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_invalidate_layout(tree, node);
    return schultz_node_invalidate(tree, node);
}

/* -------------------------------------------------------------- controls */

/*
 * Geometry the style system has no property for. A colour is stylable and a
 * size hint is stylable; these are the proportions that make a control look
 * like itself, and they are the widget's business rather than the theme's.
 */
enum {
    SCHULTZ_TOGGLE_NATURAL = 18 /**< Indicator size when there is no label. */
};

/** How thick the focus ring is drawn. */
#define SCHULTZ_RING_WIDTH  2.0f
/** How far outside the control's border the focus ring sits. */
#define SCHULTZ_RING_OFFSET 3.0f
/** How far past a control's bounds the ring's outer edge reaches. */
#define SCHULTZ_RING_BLEED  (SCHULTZ_RING_OFFSET + SCHULTZ_RING_WIDTH * 0.5f)

/** A switch is wider than it is tall; a checkbox and a radio are square. */
#define SCHULTZ_SWITCH_RATIO 1.75f

/** Natural length of a slider and a progress bar, before any size hint. */
#define SCHULTZ_TRACK_LENGTH   160.0f
/** Natural height of a slider, which is also its thumb's diameter. */
#define SCHULTZ_SLIDER_HEIGHT   20.0f
/** Natural height of a progress bar. */
#define SCHULTZ_PROGRESS_HEIGHT  8.0f

/*
 * Draws the ring that says a control has keyboard focus. Outside the border
 * rather than inside it, so it never eats into the control's content, and
 * skipped entirely when the resolved ring colour is transparent.
 */
static int32_t schultz_widget_draw_focus_ring(
    schultz_draw_list *list, const schultz_resolved_style *style,
    schultz_rect bounds, uint32_t state)
{
    schultz_color color;
    float radius;

    if (!(state & SCHULTZ_STATE_FOCUSED)) {
        return SCHULTZ_OK;
    }
    color = schultz_resolved_color(style, SCHULTZ_PROP_FOCUS_RING_COLOR);
    if (color.a == 0u) {
        return SCHULTZ_OK;
    }
    radius = schultz_resolved_number(style, SCHULTZ_PROP_CORNER_RADIUS);
    return schultz_draw_stroke_round_rect(list,
        schultz_rect_make(bounds.x - SCHULTZ_RING_OFFSET,
                          bounds.y - SCHULTZ_RING_OFFSET,
                          bounds.width + SCHULTZ_RING_OFFSET * 2.0f,
                          bounds.height + SCHULTZ_RING_OFFSET * 2.0f),
        schultz_stroke_solid(color, SCHULTZ_RING_WIDTH),
        radius + SCHULTZ_RING_OFFSET);
}

/* Shorthand for the constructors, which set a great many token references. */
static void schultz_patch_token(schultz_patch *patch, uint32_t property,
                                uint32_t token)
{
    schultz_patch_set(patch, property, schultz_value_token(token));
}

/*
 * Installs a widget's default look as a registered style, added before the
 * application has had a chance to add any of its own. A named style applied
 * afterwards therefore overrides it, and an inline property overrides both,
 * which is the order the style design calls for. Styles are reference
 * counted, so the node dropping it is what releases it.
 */
static int32_t schultz_widget_default_style(schultz_tree *tree,
                                            schultz_handle node,
                                            schultz_patch *patch)
{
    schultz_handle style = SCHULTZ_HANDLE_NONE;
    int32_t result = schultz_style_register(tree, patch, &style);

    schultz_patch_free(patch);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_node_add_style(tree, node, style);
}

/* --------------------------------------------------------------- Button */

static int32_t schultz_button_paint(schultz_tree *tree, schultz_handle node,
                                    schultz_draw_list *list,
                                    schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_rect bounds;
    int32_t result;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    result = schultz_widget_draw_box(list, style, bounds);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_widget_draw_focus_ring(list, style, bounds,
                                          schultz_node_get_state(tree, node));
}

/*
 * No event entry. Space and enter are handled by the router for every node
 * that declares SCHULTZ_ACTION_CLICK, and a button does not consume its
 * click: the host is what wants to hear about it.
 */
static const schultz_widget_vtable schultz_button_widget = {
    .paint = schultz_button_paint
};

int32_t schultz_button_create(schultz_tree *tree, schultz_handle parent,
                              const char *text, schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_label_create(tree, node, text, &label);
    if (result != SCHULTZ_OK) {
        schultz_node_destroy(tree, node);
        return result;
    }
    /* A caption is one line however narrow the button is made, and it is
     * part of the button: a click on the words is a click on the button.
     * Cut short rather than drawn past the edge, because a button squeezed
     * by its layout would otherwise paint its caption over its neighbour. */
    schultz_label_set_wrap(tree, label, 0);
    schultz_label_set_ellipsize(tree, label, 1);
    schultz_node_set_hit_testable(tree, label, 0);
    {
        /* Stack fills the cell by default; a caption sits in the middle. */
        schultz_layout_params params;

        schultz_layout_params_default(&params);
        params.align = SCHULTZ_ALIGN_CENTER;
        schultz_node_set_layout_params(tree, label, &params);
    }

    /* Children share one rectangle, which for one child means centred. */
    schultz_node_set_pane(tree, node, schultz_pane_stack());
    schultz_node_set_widget(tree, node, &schultz_button_widget, NULL);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_BUTTON);
    schultz_node_set_name(tree, node, schultz_label_text(tree, label));
    schultz_node_set_actions(tree, node,
                             SCHULTZ_ACTION_CLICK | SCHULTZ_ACTION_FOCUS);
    /* The focus ring is drawn outside the bounds; say so, or it is never
     * invalidated and lingers as a partly drawn outline. */
    schultz_node_set_paint_margin(tree, node, SCHULTZ_RING_BLEED);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_SURFACE);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                            SCHULTZ_TOKEN_COLOR_BORDER);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_WIDTH,
                            SCHULTZ_TOKEN_BORDER_WIDTH);
        schultz_patch_token(&patch, SCHULTZ_PROP_CORNER_RADIUS,
                            SCHULTZ_TOKEN_RADIUS_CONTROL);
        /*
         * The button's own inset, rather than a step off the spacing scale.
         * That scale doubles, 2 4 8 16 32, so the step below the one this
         * used to take is half as much again as was wanted. Padding is the
         * only lever on a button's height: there is no vertical only
         * padding, so this takes the same amount off its width.
         */
        schultz_patch_set(&patch, SCHULTZ_PROP_PADDING,
                          schultz_value_number(6.0f));
        schultz_widget_default_style(tree, node, &patch);
    }
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_HOVERED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_SURFACE_RAISED));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_PRESSED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_SURFACE_SUNKEN));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_DISABLED,
        SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_DISABLED));

    *out_node = node;
    return SCHULTZ_OK;
}

/* Both are defined further down; a button made into one keeps its caption. */
static const schultz_widget_vtable schultz_toggle_button_widget;
static const schultz_widget_vtable schultz_menu_button_widget;

/*
 * Whether a node was built by schultz_button_create, whatever it has become
 * since.
 *
 * Two widgets in this file are made as buttons and then given a vtable of
 * their own: a toggle button, and a menu button, which is also what a split
 * menu button is. They keep everything the button gave them, the caption
 * child included, but the vtable no longer says so, and a caption accessor
 * that asks about the vtable alone refuses widgets this file built itself.
 *
 * Listed rather than guessed at. A check that only asked whether the first
 * child is a label would answer for a checkbox and a group box too, which is
 * not what the name promises, and a check on the accessibility role would
 * answer for anything a host had marked as a button. A new widget built on
 * schultz_button_create has to be added here, and forgetting is a caption
 * that cannot be reached rather than a wrong node handed back.
 */
static int32_t schultz_widget_is_button(const schultz_widget_vtable *widget)
{
    return (widget == &schultz_button_widget ||
            widget == &schultz_toggle_button_widget ||
            widget == &schultz_menu_button_widget) ? 1 : 0;
}

schultz_handle schultz_button_label(const schultz_tree *tree,
                                    schultz_handle node)
{
    schultz_handle label = SCHULTZ_HANDLE_NONE;

    if (!schultz_widget_is_button(schultz_node_widget(tree, node))) {
        return SCHULTZ_HANDLE_NONE;
    }
    schultz_node_child_at(tree, node, 0, &label);
    return label;
}

/* ------------------------------------------------------------ Hyperlink */

/** What a link remembers that a button does not. */
typedef struct {
    uint32_t visited; /**< Nonzero once the host says the target was used. */
} schultz_hyperlink_data;

static const schultz_widget_vtable schultz_hyperlink_widget;

/*
 * A link is a button that draws as text. No box and no border, an underline
 * while the pointer is over it, and the focus ring a button already gets.
 */
static int32_t schultz_hyperlink_paint(schultz_tree *tree,
                                       schultz_handle node,
                                       schultz_draw_list *list,
                                       schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    uint32_t state = schultz_node_get_state(tree, node);
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    if ((state & SCHULTZ_STATE_HOVERED) &&
        schultz_node_child_at(tree, node, 0, &label) == SCHULTZ_OK) {
        schultz_rect text;

        /*
         * Under the words, not under the box holding them. The caption's node
         * is stretched to the link's width by the stack pane, so its bounds
         * say nothing about where the glyphs are; a rule drawn across them
         * carries on past the end of the text. The lines the caption draws
         * are laid out again here, and each is underlined for exactly as far
         * as it runs. That also puts the rule in the right place when the
         * text is centred or cut short by an ellipsis.
         */
        if (schultz_node_absolute_bounds(tree, label, &text) == SCHULTZ_OK) {
            const schultz_label_data *caption =
                (const schultz_label_data *)schultz_node_widget_data(tree,
                                                                     label);
            schultz_stroke stroke = schultz_widget_stroke(style);
            schultz_label_row *rows = NULL;
            uint32_t count = schultz_label_rows(tree, label, caption, text,
                                                arena, &rows);
            uint32_t i;

            stroke.paint = schultz_paint_solid(
                schultz_resolved_color(style, SCHULTZ_PROP_TEXT_COLOR));
            stroke.width = 1.0f;
            stroke.dash  = SCHULTZ_HANDLE_NONE;
            for (i = 0u; i < count; i++) {
                float y = rows[i].top + rows[i].height - 1.0f;
                int32_t result;

                if (rows[i].count == 0u) {
                    continue;
                }
                result = schultz_draw_line(list,
                    schultz_point_make(rows[i].x, y),
                    schultz_point_make(rows[i].x + rows[i].width, y), stroke);
                if (result != SCHULTZ_OK) {
                    return result;
                }
            }
            return SCHULTZ_OK;
        }
    }
    return schultz_widget_draw_focus_ring(list, style, bounds, state);
}

static const schultz_widget_vtable schultz_hyperlink_widget = {
    .paint = schultz_hyperlink_paint, .destroy = free
};

int32_t schultz_hyperlink_create(schultz_tree *tree, schultz_handle parent,
                                 const char *text, schultz_handle *out_node)
{
    schultz_hyperlink_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_hyperlink_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    result = schultz_label_create(tree, node, text, &label);
    if (result != SCHULTZ_OK) {
        schultz_node_destroy(tree, node);
        free(data);
        return result;
    }
    schultz_label_set_wrap(tree, label, 0);
    schultz_label_set_ellipsize(tree, label, 1);
    schultz_node_set_hit_testable(tree, label, 0);

    schultz_node_set_pane(tree, node, schultz_pane_stack());
    schultz_node_set_widget(tree, node, &schultz_hyperlink_widget, data);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_BUTTON);
    schultz_node_set_name(tree, node, schultz_label_text(tree, label));
    schultz_node_set_actions(tree, node,
                             SCHULTZ_ACTION_CLICK | SCHULTZ_ACTION_FOCUS);
    schultz_node_set_paint_margin(tree, node, SCHULTZ_RING_BLEED);
    schultz_node_set_cursor(tree, node, SCHULTZ_CURSOR_POINTER);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_TEXT_COLOR,
                            SCHULTZ_TOKEN_COLOR_ACCENT);
        schultz_widget_default_style(tree, node, &patch);
    }
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_DISABLED,
        SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_DISABLED));

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_hyperlink_set_text(schultz_tree *tree, schultz_handle node,
                                   const char *text)
{
    schultz_handle label = SCHULTZ_HANDLE_NONE;

    if (schultz_node_widget(tree, node) != &schultz_hyperlink_widget ||
        schultz_node_child_at(tree, node, 0, &label) != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_node_set_name(tree, node, text);
    return schultz_label_set_text(tree, label, text);
}

int32_t schultz_hyperlink_set_visited(schultz_tree *tree, schultz_handle node,
                                      int32_t visited)
{
    schultz_hyperlink_data *data;

    if (schultz_node_widget(tree, node) != &schultz_hyperlink_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data = (schultz_hyperlink_data *)schultz_node_widget_data(tree, node);
    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data->visited = visited ? 1u : 0u;
    /*
     * Visited is a colour rather than a state flag, since the toolkit's
     * states are all things the pointer or the keyboard did. The host owns
     * the meaning; the widget owns the look.
     */
    if (data->visited) {
        schultz_node_set_style_property(tree, node, SCHULTZ_PROP_TEXT_COLOR,
            schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_MUTED));
    } else {
        schultz_node_clear_style_property(tree, node,
                                          SCHULTZ_PROP_TEXT_COLOR);
    }
    return SCHULTZ_OK;
}

int32_t schultz_hyperlink_is_visited(const schultz_tree *tree,
                                     schultz_handle node)
{
    const schultz_hyperlink_data *data;

    if (schultz_node_widget(tree, node) != &schultz_hyperlink_widget) {
        return 0;
    }
    data = (const schultz_hyperlink_data *)schultz_node_widget_data(tree,
                                                                    node);
    return (data == NULL) ? 0 : (int32_t)data->visited;
}

/* --------------------------------------------------------- ToggleButton */

/** A toggle button's group, which works the way a radio's does. */
typedef struct {
    uint32_t group; /**< Zero means ungrouped: it clicks on and off freely. */
} schultz_toggle_button_data;

static const schultz_widget_vtable schultz_toggle_button_widget;

/* Draws exactly as a button does; the selected state supplies the change. */
static int32_t schultz_toggle_button_paint(schultz_tree *tree,
                                           schultz_handle node,
                                           schultz_draw_list *list,
                                           schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_rect bounds;
    int32_t result;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    result = schultz_widget_draw_box(list, style, bounds);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_widget_draw_focus_ring(list, style, bounds,
                                          schultz_node_get_state(tree, node));
}

static int32_t schultz_toggle_button_event(schultz_tree *tree,
                                           schultz_handle node,
                                           const schultz_event *event)
{
    if (event->type != SCHULTZ_EVENT_CLICK) {
        return SCHULTZ_OK;
    }
    /* Grouped behaves as a radio, ungrouped as a checkbox. Not consumed
     * either way: the host still wants to hear the value changed. */
    return schultz_toggle_button_select(tree, node);
}

static const schultz_widget_vtable schultz_toggle_button_widget = {
    .paint = schultz_toggle_button_paint,
    .event = schultz_toggle_button_event,
    .destroy = free
};

int32_t schultz_toggle_button_create(schultz_tree *tree,
                                     schultz_handle parent, const char *text,
                                     uint32_t group, schultz_handle *out_node)
{
    schultz_toggle_button_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_toggle_button_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->group = group;

    /* Built as a button, then given a different widget type and its data,
     * so the caption, padding, states and focus ring all come for free. */
    result = schultz_button_create(tree, parent, text, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    schultz_node_set_widget(tree, node, &schultz_toggle_button_widget, data);
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_SELECTED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_SELECTED,
        SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_ON_ACCENT));

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_toggle_button_select(schultz_tree *tree, schultz_handle node)
{
    const schultz_toggle_button_data *data;
    uint32_t state;
    schultz_handle parent = SCHULTZ_HANDLE_NONE;
    uint32_t count;
    uint32_t i;

    if (schultz_node_widget(tree, node) != &schultz_toggle_button_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data = (const schultz_toggle_button_data *)schultz_node_widget_data(tree,
                                                                       node);
    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    state = schultz_node_get_state(tree, node);

    if (data->group == 0u) {
        /* Ungrouped: on and off, like a checkbox wearing a button. */
        return schultz_node_set_state(tree, node,
                                      state ^ SCHULTZ_STATE_SELECTED);
    }
    if (state & SCHULTZ_STATE_SELECTED) {
        return SCHULTZ_OK;  /* one of a set is always chosen */
    }
    if (schultz_node_parent(tree, node, &parent) != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* A group is the siblings sharing a parent and a group number, the same
     * rule schultz_radio_create documents. */
    count = schultz_node_child_count(tree, parent);
    for (i = 0; i < count; i++) {
        schultz_handle sibling = SCHULTZ_HANDLE_NONE;
        const schultz_toggle_button_data *other;

        if (schultz_node_child_at(tree, parent, i, &sibling) != SCHULTZ_OK ||
            sibling == node ||
            schultz_node_widget(tree, sibling)
                != &schultz_toggle_button_widget) {
            continue;
        }
        other = (const schultz_toggle_button_data *)
            schultz_node_widget_data(tree, sibling);
        if (other != NULL && other->group == data->group) {
            schultz_node_set_state(tree, sibling,
                schultz_node_get_state(tree, sibling)
                    & ~(uint32_t)SCHULTZ_STATE_SELECTED);
        }
    }
    return schultz_node_set_state(tree, node,
                                  state | SCHULTZ_STATE_SELECTED);
}

int32_t schultz_toggle_button_is_selected(const schultz_tree *tree,
                                          schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_toggle_button_widget) {
        return 0;
    }
    return (schultz_node_get_state(tree, node) & SCHULTZ_STATE_SELECTED)
               ? 1 : 0;
}

/* ------------------------------------------------------------ ButtonBar */

/** A button bar's ordering and sizing. */
typedef struct {
    uint32_t order;   /**< One of SCHULTZ_BUTTON_ORDER_*. */
    uint32_t uniform; /**< Nonzero when every button is the widest one. */
} schultz_button_bar_data;

static const schultz_widget_vtable schultz_button_bar_widget;

/*
 * Where each role sits, per platform, reading left to right. The left role is
 * absent from all three because it is pinned to the far left and never takes
 * part in the ordering.
 */
static const uint32_t schultz_button_order_windows[] = {
    SCHULTZ_BUTTON_ROLE_OTHER, SCHULTZ_BUTTON_ROLE_YES,
    SCHULTZ_BUTTON_ROLE_NO, SCHULTZ_BUTTON_ROLE_OK,
    SCHULTZ_BUTTON_ROLE_CANCEL, SCHULTZ_BUTTON_ROLE_APPLY,
    SCHULTZ_BUTTON_ROLE_HELP
};
static const uint32_t schultz_button_order_macos[] = {
    SCHULTZ_BUTTON_ROLE_HELP, SCHULTZ_BUTTON_ROLE_OTHER,
    SCHULTZ_BUTTON_ROLE_APPLY, SCHULTZ_BUTTON_ROLE_NO,
    SCHULTZ_BUTTON_ROLE_CANCEL, SCHULTZ_BUTTON_ROLE_YES,
    SCHULTZ_BUTTON_ROLE_OK
};
static const uint32_t schultz_button_order_linux[] = {
    SCHULTZ_BUTTON_ROLE_HELP, SCHULTZ_BUTTON_ROLE_OTHER,
    SCHULTZ_BUTTON_ROLE_NO, SCHULTZ_BUTTON_ROLE_CANCEL,
    SCHULTZ_BUTTON_ROLE_APPLY, SCHULTZ_BUTTON_ROLE_YES,
    SCHULTZ_BUTTON_ROLE_OK
};

/* Where a role falls in one platform's order. Unknown roles go last. */
static uint32_t schultz_button_rank(uint32_t order, uint32_t role)
{
    const uint32_t *table;
    uint32_t count = 7u;
    uint32_t i;

    if (role == SCHULTZ_BUTTON_ROLE_LEFT) {
        return 0u;  /* pinned, and sorted before everything else */
    }
    switch (order) {
    case SCHULTZ_BUTTON_ORDER_MACOS: table = schultz_button_order_macos; break;
    case SCHULTZ_BUTTON_ORDER_LINUX: table = schultz_button_order_linux; break;
    default:                       table = schultz_button_order_windows; break;
    }
    for (i = 0; i < count; i++) {
        if (table[i] == role) {
            return i + 1u;
        }
    }
    return count + 1u;
}

/*
 * A child's role. Kept in the grid row of its layout params, which a bar
 * never uses for anything else, so the role travels with the child rather
 * than in a table beside it. Public, because a host that built the bar wants
 * to read a role back as much as the bar does.
 */
uint32_t schultz_button_bar_role(const schultz_tree *tree,
                                 schultz_handle button)
{
    schultz_layout_params params;

    if (schultz_node_get_layout_params(tree, button, &params) != SCHULTZ_OK) {
        return SCHULTZ_BUTTON_ROLE_OTHER;
    }
    return params.row;
}

static uint32_t schultz_button_bar_order(const schultz_tree *tree,
                                         schultz_handle node)
{
    const schultz_button_bar_data *data =
        (const schultz_button_bar_data *)schultz_node_widget_data(tree, node);

    return (data == NULL) ? SCHULTZ_BUTTON_ORDER_LINUX : data->order;
}

static int32_t schultz_button_bar_uniform(const schultz_tree *tree,
                                          schultz_handle node)
{
    const schultz_button_bar_data *data =
        (const schultz_button_bar_data *)schultz_node_widget_data(tree, node);

    return (data == NULL) ? 1 : (int32_t)data->uniform;
}

/*
 * Fills `out` with the children in the order they should be drawn. A simple
 * insertion sort: a bar holds a handful of buttons, and it has to be stable
 * so two buttons with the same role keep the order they were added in.
 */
static uint32_t schultz_button_bar_sorted(schultz_tree *tree,
                                          schultz_handle node,
                                          schultz_handle *out, uint32_t max)
{
    uint32_t order = schultz_button_bar_order(tree, node);
    uint32_t count = schultz_node_child_count(tree, node);
    uint32_t written = 0;
    uint32_t i;

    for (i = 0; i < count && written < max; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;
        uint32_t rank;
        uint32_t at;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK) {
            continue;
        }
        rank = schultz_button_rank(order,
                                   schultz_button_bar_role(tree, child));
        at = written;
        while (at > 0u &&
               schultz_button_rank(order,
                   schultz_button_bar_role(tree, out[at - 1u])) > rank) {
            out[at] = out[at - 1u];
            at--;
        }
        out[at] = child;
        written++;
    }
    return written;
}

/* The width every button takes when the bar sizes them alike. */
static float schultz_button_bar_widest(schultz_tree *tree, schultz_handle node,
                                       float avail_h)
{
    uint32_t count = schultz_node_child_count(tree, node);
    float widest = 0.0f;
    uint32_t i;

    for (i = 0; i < count; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;
        schultz_size size;

        if (schultz_node_child_at(tree, node, i, &child) == SCHULTZ_OK &&
            schultz_layout_measure(tree, child, -1.0f, avail_h, &size)
                == SCHULTZ_OK && size.width > widest) {
            widest = size.width;
        }
    }
    return widest;
}

static int32_t schultz_button_bar_measure(schultz_tree *tree,
                                          schultz_handle node, float avail_w,
                                          float avail_h,
                                          schultz_size *out_size)
{
    uint32_t count = schultz_node_child_count(tree, node);
    float padding = 0.0f;
    float gap = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float uniform = 0.0f;
    uint32_t i;

    (void)avail_w;
    schultz_node_get_spacing(tree, node, &padding, &gap);
    if (schultz_button_bar_uniform(tree, node)) {
        uniform = schultz_button_bar_widest(tree, node, avail_h);
    }

    for (i = 0; i < count; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;
        schultz_size size;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            schultz_layout_measure(tree, child, -1.0f, avail_h, &size)
                != SCHULTZ_OK) {
            continue;
        }
        width += (uniform > 0.0f) ? uniform : size.width;
        if (i + 1u < count) {
            width += gap;
        }
        if (size.height > height) {
            height = size.height;
        }
    }
    *out_size = schultz_size_make(width + padding * 2.0f,
                                  height + padding * 2.0f);
    return SCHULTZ_OK;
}

static int32_t schultz_button_bar_arrange(schultz_tree *tree,
                                          schultz_handle node,
                                          schultz_rect rect)
{
    schultz_handle sorted[SCHULTZ_BUTTON_BAR_MAX];
    uint32_t count = schultz_button_bar_sorted(tree, node, sorted,
                                               SCHULTZ_BUTTON_BAR_MAX);
    float padding = 0.0f;
    float gap = 0.0f;
    float uniform = 0.0f;
    float inner_h;
    float x;
    float right;
    uint32_t i;
    uint32_t pinned = 0;

    schultz_node_get_spacing(tree, node, &padding, &gap);
    inner_h = rect.height - padding * 2.0f;
    if (schultz_button_bar_uniform(tree, node)) {
        uniform = schultz_button_bar_widest(tree, node, inner_h);
    }

    /* The left role first, from the left edge. */
    x = padding;
    for (i = 0; i < count; i++) {
        schultz_size size;
        float width;

        if (schultz_button_bar_role(tree, sorted[i])
                != SCHULTZ_BUTTON_ROLE_LEFT) {
            break;
        }
        if (schultz_layout_measure(tree, sorted[i], -1.0f, inner_h, &size)
                != SCHULTZ_OK) {
            continue;
        }
        width = (uniform > 0.0f) ? uniform : size.width;
        schultz_layout_arrange(tree, sorted[i],
                               schultz_rect_make(x, padding, width, inner_h));
        x += width + gap;
        pinned++;
    }

    /*
     * The rest against the right edge, which is where a dialog's buttons sit
     * whichever platform this is. Placed backwards from the far edge so the
     * ordering above reads left to right when it lands.
     */
    right = rect.width - padding;
    for (i = count; i > pinned; i--) {
        schultz_handle child = sorted[i - 1u];
        schultz_size size;
        float width;

        if (schultz_layout_measure(tree, child, -1.0f, inner_h, &size)
                != SCHULTZ_OK) {
            continue;
        }
        width = (uniform > 0.0f) ? uniform : size.width;
        right -= width;
        schultz_layout_arrange(tree, child,
                               schultz_rect_make(right, padding, width,
                                                 inner_h));
        right -= gap;
    }
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_button_bar_pane = {
    schultz_button_bar_measure, schultz_button_bar_arrange
};

static const schultz_widget_vtable schultz_button_bar_widget = {
    .paint = NULL, .destroy = free
};

int32_t schultz_button_bar_create(schultz_tree *tree, schultz_handle parent,
                                  schultz_handle *out_node)
{
    schultz_button_bar_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_button_bar_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->order   = SCHULTZ_BUTTON_ORDER_DEFAULT;
    data->uniform = 1u;

    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    schultz_node_set_pane(tree, node, &schultz_button_bar_pane);
    schultz_node_set_widget(tree, node, &schultz_button_bar_widget, data);
    schultz_node_set_spacing(tree, node, 0.0f, 8.0f);

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_button_bar_add(schultz_tree *tree, schultz_handle node,
                               const char *text, uint32_t role,
                               schultz_handle *out_button)
{
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (schultz_node_widget(tree, node) != &schultz_button_bar_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    result = schultz_button_create(tree, node, text, &button);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_button_bar_set_role(tree, button, role);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (out_button != NULL) {
        *out_button = button;
    }
    return SCHULTZ_OK;
}

int32_t schultz_button_bar_set_role(schultz_tree *tree, schultz_handle button,
                                    uint32_t role)
{
    schultz_layout_params params;
    int32_t result = schultz_node_get_layout_params(tree, button, &params);

    if (result != SCHULTZ_OK) {
        return result;
    }
    if (role >= SCHULTZ_BUTTON_ROLE_COUNT) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    params.row = role;
    return schultz_node_set_layout_params(tree, button, &params);
}

int32_t schultz_button_bar_set_order(schultz_tree *tree, schultz_handle node,
                                     uint32_t order)
{
    schultz_button_bar_data *data;

    if (schultz_node_widget(tree, node) != &schultz_button_bar_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (order > SCHULTZ_BUTTON_ORDER_LINUX) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_button_bar_data *)schultz_node_widget_data(tree, node);
    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (data->order == order) {
        return SCHULTZ_OK;
    }
    data->order = order;
    return schultz_node_invalidate_layout(tree, node);
}

int32_t schultz_button_bar_set_uniform_width(schultz_tree *tree,
                                             schultz_handle node, int32_t on)
{
    schultz_button_bar_data *data;

    if (schultz_node_widget(tree, node) != &schultz_button_bar_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data = (schultz_button_bar_data *)schultz_node_widget_data(tree, node);
    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (data->uniform == (on ? 1u : 0u)) {
        return SCHULTZ_OK;
    }
    data->uniform = on ? 1u : 0u;
    return schultz_node_invalidate_layout(tree, node);
}

/* ------------------------------------------------- Checkbox, Radio, Switch */

/** What a radio needs and the other two do not. */
typedef struct {
    uint32_t group; /**< Radios sharing a parent and a group are exclusive. */
} schultz_toggle_data;

static const schultz_widget_vtable schultz_checkbox_widget;
static const schultz_widget_vtable schultz_radio_widget;
static const schultz_widget_vtable schultz_switch_widget;

/* Nonzero when the node is one of the three toggles. */
static int32_t schultz_is_toggle(const schultz_tree *tree,
                                 schultz_handle node)
{
    const schultz_widget_vtable *widget = schultz_node_widget(tree, node);

    return (widget == &schultz_checkbox_widget ||
            widget == &schultz_radio_widget ||
            widget == &schultz_switch_widget);
}

/* How wide the indicator is for a given height. */
static float schultz_toggle_ratio(const schultz_tree *tree,
                                  schultz_handle node)
{
    return (schultz_node_widget(tree, node) == &schultz_switch_widget)
               ? SCHULTZ_SWITCH_RATIO : 1.0f;
}

/*
 * An indicator on the left, then a gap, then the label. The indicator is a
 * square of the row's height, so it grows with the text rather than needing a
 * size of its own.
 */
static int32_t schultz_toggle_measure(schultz_tree *tree, schultz_handle node,
                                      float avail_w, float avail_h,
                                      schultz_size *out_size)
{
    float gap = 0.0f;
    float height = (float)SCHULTZ_TOGGLE_NATURAL;
    float width;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_size text = { 0.0f, 0.0f };

    schultz_node_get_spacing(tree, node, NULL, &gap);

    if (schultz_node_child_at(tree, node, 0, &label) == SCHULTZ_OK &&
        schultz_layout_measure(tree, label, avail_w, avail_h, &text)
            == SCHULTZ_OK && text.height > 0.0f) {
        height = text.height;
    } else {
        gap = 0.0f;
    }

    width = height * schultz_toggle_ratio(tree, node) + gap + text.width;
    *out_size = schultz_size_make(width, height);
    return SCHULTZ_OK;
}

static int32_t schultz_toggle_arrange(schultz_tree *tree, schultz_handle node,
                                      schultz_rect rect)
{
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    float gap = 0.0f;
    float indicator;

    if (schultz_node_child_at(tree, node, 0, &label) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    schultz_node_get_spacing(tree, node, NULL, &gap);
    indicator = rect.height * schultz_toggle_ratio(tree, node);

    return schultz_layout_arrange(tree, label,
        schultz_rect_make(indicator + gap, 0.0f,
                          rect.width - indicator - gap, rect.height));
}

static const schultz_pane_vtable schultz_toggle_pane = {
    schultz_toggle_measure, schultz_toggle_arrange
};

/* The indicator's rectangle, in window coordinates. */
static schultz_rect schultz_toggle_indicator(schultz_tree *tree,
                                             schultz_handle node,
                                             schultz_rect bounds)
{
    return schultz_rect_make(bounds.x, bounds.y,
                             bounds.height *
                                 schultz_toggle_ratio(tree, node),
                             bounds.height);
}

/*
 * All three toggles do the same thing with a click: flip themselves. They do
 * not consume it, because the host still wants to know the value changed.
 */
static int32_t schultz_toggle_event(schultz_tree *tree, schultz_handle node,
                                    const schultz_event *event)
{
    if (event->type != SCHULTZ_EVENT_CLICK) {
        return SCHULTZ_OK;
    }

    if (schultz_node_widget(tree, node) == &schultz_radio_widget) {
        /*
         * A radio turns on and stays on; the group is what turns it off. One
         * of a set is always chosen, so clicking the chosen one changes
         * nothing.
         */
        schultz_radio_select(tree, node);
        return SCHULTZ_OK;
    }
    return schultz_node_set_state(tree, node,
        schultz_node_get_state(tree, node) ^ SCHULTZ_STATE_CHECKED);
}

static int32_t schultz_checkbox_paint(schultz_tree *tree, schultz_handle node,
                                      schultz_draw_list *list,
                                      schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    uint32_t state = schultz_node_get_state(tree, node);
    schultz_rect bounds;
    schultz_rect box;
    int32_t result;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    box = schultz_toggle_indicator(tree, node, bounds);

    result = schultz_widget_draw_box(list, style, box);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (state & SCHULTZ_STATE_CHECKED) {
        /*
         * A tick, as two strokes. Drawn in the selection colour, which is
         * what marks a thing as chosen everywhere else in the toolkit.
         */
        schultz_color mark = schultz_resolved_color(style,
                                        SCHULTZ_PROP_SELECTION_COLOR);
        float w = box.width;
        float thickness = w * 0.14f;
        schultz_stroke tick;

        if (thickness < 1.0f) {
            thickness = 1.0f;
        }
        /* Rounded, so the two strokes meet as one mark rather than a cross. */
        tick = schultz_stroke_solid(mark, thickness);
        tick.cap = SCHULTZ_CAP_ROUND;
        schultz_draw_line(list,
            schultz_point_make(box.x + w * 0.24f, box.y + w * 0.52f),
            schultz_point_make(box.x + w * 0.44f, box.y + w * 0.72f), tick);
        schultz_draw_line(list,
            schultz_point_make(box.x + w * 0.44f, box.y + w * 0.72f),
            schultz_point_make(box.x + w * 0.76f, box.y + w * 0.30f), tick);
    }
    return schultz_widget_draw_focus_ring(list, style, box, state);
}

static const schultz_widget_vtable schultz_checkbox_widget = {
    .paint = schultz_checkbox_paint, .event = schultz_toggle_event, .destroy = free
};

static int32_t schultz_radio_paint(schultz_tree *tree, schultz_handle node,
                                   schultz_draw_list *list,
                                   schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    uint32_t state = schultz_node_get_state(tree, node);
    schultz_paint fill = schultz_resolved_paint(style,
                                                SCHULTZ_PROP_BACKGROUND);
    schultz_stroke ring = schultz_widget_stroke(style);
    schultz_rect bounds;
    schultz_rect box;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    box = schultz_toggle_indicator(tree, node, bounds);

    if (!schultz_paint_is_invisible(fill)) {
        schultz_draw_fill_ellipse(list, box, fill);
    }
    if (!schultz_paint_is_invisible(ring.paint) && ring.width > 0.0f) {
        schultz_draw_stroke_ellipse(list, box, ring);
    }
    if (state & SCHULTZ_STATE_CHECKED) {
        float inset = box.width * 0.28f;

        schultz_draw_fill_ellipse(list,
            schultz_rect_make(box.x + inset, box.y + inset,
                              box.width - inset * 2.0f,
                              box.height - inset * 2.0f),
            schultz_resolved_paint(style, SCHULTZ_PROP_SELECTION_COLOR));
    }
    return schultz_widget_draw_focus_ring(list, style, box, state);
}

static const schultz_widget_vtable schultz_radio_widget = {
    .paint = schultz_radio_paint, .event = schultz_toggle_event, .destroy = free
};

static int32_t schultz_switch_paint(schultz_tree *tree, schultz_handle node,
                                    schultz_draw_list *list,
                                    schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    uint32_t state = schultz_node_get_state(tree, node);
    schultz_rect bounds;
    schultz_rect track;
    float diameter;
    float x;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    track = schultz_toggle_indicator(tree, node, bounds);

    /* A fully rounded track, whatever the corner radius says. */
    schultz_draw_fill_round_rect(list, track,
        schultz_resolved_paint(style, SCHULTZ_PROP_BACKGROUND),
        track.height * 0.5f);
    {
        schultz_stroke edge = schultz_widget_stroke(style);

        if (edge.width > 0.0f) {
            schultz_draw_stroke_round_rect(list, track, edge,
                                           track.height * 0.5f);
        }
    }

    /* The thumb sits at one end or the other, which is what reads as on. */
    diameter = track.height * 0.72f;
    x = (state & SCHULTZ_STATE_CHECKED)
            ? track.x + track.width - diameter - (track.height - diameter)
                  * 0.5f
            : track.x + (track.height - diameter) * 0.5f;
    schultz_draw_fill_ellipse(list,
        schultz_rect_make(x, track.y + (track.height - diameter) * 0.5f,
                          diameter, diameter),
        schultz_resolved_paint(style, SCHULTZ_PROP_SELECTION_COLOR));

    return schultz_widget_draw_focus_ring(list, style, track, state);
}

static const schultz_widget_vtable schultz_switch_widget = {
    .paint = schultz_switch_paint, .event = schultz_toggle_event, .destroy = free
};

/*
 * The three share everything but their painting: the same pane, the same
 * click handling, the same accessibility actions, the same default look.
 */
static int32_t schultz_toggle_create(schultz_tree *tree, schultz_handle parent,
                                     const char *text,
                                     const schultz_widget_vtable *widget,
                                     uint32_t role, schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_toggle_data *data;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    data = (schultz_toggle_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    if (text != NULL && text[0] != '\0') {
        result = schultz_label_create(tree, node, text, &label);
        if (result != SCHULTZ_OK) {
            free(data);
            schultz_node_destroy(tree, node);
            return result;
        }
        schultz_label_set_wrap(tree, label, 0);
        /* Clicking the caption toggles the control, as it does everywhere. */
        schultz_node_set_hit_testable(tree, label, 0);
    }

    schultz_node_set_pane(tree, node, &schultz_toggle_pane);
    schultz_node_set_widget(tree, node, widget, data);
    schultz_node_set_role(tree, node, role);
    schultz_node_set_name(tree, node, (text == NULL) ? "" : text);
    schultz_node_set_actions(tree, node,
                             SCHULTZ_ACTION_CLICK | SCHULTZ_ACTION_FOCUS);
    /* The focus ring is drawn outside the bounds; say so, or it is never
     * invalidated and lingers as a partly drawn outline. */
    schultz_node_set_paint_margin(tree, node, SCHULTZ_RING_BLEED);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_SURFACE);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                            SCHULTZ_TOKEN_COLOR_BORDER);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_WIDTH,
                            SCHULTZ_TOKEN_BORDER_WIDTH);
        schultz_patch_token(&patch, SCHULTZ_PROP_CORNER_RADIUS,
                            SCHULTZ_TOKEN_RADIUS_CONTROL_SMALL);
        schultz_patch_token(&patch, SCHULTZ_PROP_SELECTION_COLOR,
                            SCHULTZ_TOKEN_COLOR_TEXT_ON_ACCENT);
        schultz_patch_token(&patch, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_SM);
        schultz_widget_default_style(tree, node, &patch);
    }
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_HOVERED,
        SCHULTZ_PROP_BORDER_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_CHECKED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_CHECKED,
        SCHULTZ_PROP_BORDER_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_DISABLED,
        SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_DISABLED));

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_checkbox_create(schultz_tree *tree, schultz_handle parent,
                                const char *text, schultz_handle *out_node)
{
    return schultz_toggle_create(tree, parent, text, &schultz_checkbox_widget,
                                 SCHULTZ_ROLE_CHECKBOX, out_node);
}

int32_t schultz_switch_create(schultz_tree *tree, schultz_handle parent,
                              const char *text, schultz_handle *out_node)
{
    return schultz_toggle_create(tree, parent, text, &schultz_switch_widget,
                                 SCHULTZ_ROLE_CHECKBOX, out_node);
}

int32_t schultz_radio_create(schultz_tree *tree, schultz_handle parent,
                             const char *text, uint32_t group,
                             schultz_handle *out_node)
{
    int32_t result = schultz_toggle_create(tree, parent, text,
                                           &schultz_radio_widget,
                                           SCHULTZ_ROLE_RADIO, out_node);
    schultz_toggle_data *data;

    if (result != SCHULTZ_OK) {
        return result;
    }
    data = (schultz_toggle_data *)schultz_node_widget_data(tree, *out_node);
    data->group = group;
    return SCHULTZ_OK;
}

int32_t schultz_toggle_checked(const schultz_tree *tree, schultz_handle node)
{
    if (!schultz_is_toggle(tree, node)) {
        return 0;
    }
    return (schultz_node_get_state(tree, node) & SCHULTZ_STATE_CHECKED) ? 1
                                                                        : 0;
}

int32_t schultz_toggle_set_checked(schultz_tree *tree, schultz_handle node,
                                   int32_t checked)
{
    uint32_t state;

    if (!schultz_is_toggle(tree, node)) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    state = schultz_node_get_state(tree, node);
    if (checked) {
        state |= SCHULTZ_STATE_CHECKED;
    } else {
        state &= ~(uint32_t)SCHULTZ_STATE_CHECKED;
    }
    return schultz_node_set_state(tree, node, state);
}

int32_t schultz_radio_select(schultz_tree *tree, schultz_handle node)
{
    const schultz_toggle_data *data;
    schultz_handle parent = SCHULTZ_HANDLE_NONE;
    uint32_t count;
    uint32_t i;

    if (schultz_node_widget(tree, node) != &schultz_radio_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data = (const schultz_toggle_data *)schultz_node_widget_data(tree, node);
    if (data == NULL ||
        schultz_node_parent(tree, node, &parent) != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }

    /*
     * A group is the radios that share a parent and a group number. Scoping
     * it to the parent means two groups on one screen need no coordination
     * beyond being in different containers.
     */
    count = schultz_node_child_count(tree, parent);
    for (i = 0; i < count; i++) {
        schultz_handle sibling = SCHULTZ_HANDLE_NONE;
        const schultz_toggle_data *other;

        if (schultz_node_child_at(tree, parent, i, &sibling) != SCHULTZ_OK ||
            sibling == node ||
            schultz_node_widget(tree, sibling) != &schultz_radio_widget) {
            continue;
        }
        other = (const schultz_toggle_data *)schultz_node_widget_data(tree,
                                                                     sibling);
        if (other != NULL && other->group == data->group) {
            schultz_toggle_set_checked(tree, sibling, 0);
        }
    }
    return schultz_toggle_set_checked(tree, node, 1);
}

/* --------------------------------------------------------------- Slider */

/** A slider's value and the range it moves in. */
typedef struct {
    float minimum; /**< Lowest value the slider can take. */
    float maximum; /**< Highest value. Never below the minimum. */
    float value;   /**< Current value, always within the range. */
    float step;    /**< How far one arrow key moves it. */
    uint32_t orientation; /**< SCHULTZ_ORIENT_HORIZONTAL or _VERTICAL. */
} schultz_slider_data;

static const schultz_widget_vtable schultz_slider_widget;

/* Where in the range a value sits, as 0 to 1. */
static float schultz_slider_fraction(const schultz_slider_data *slider)
{
    float span = slider->maximum - slider->minimum;

    return (span > 0.0f) ? (slider->value - slider->minimum) / span : 0.0f;
}

/* Clamps to the range and marks the node when the value actually moved. */
static int32_t schultz_slider_store(schultz_tree *tree, schultz_handle node,
                                    schultz_slider_data *slider, float value)
{
    char text[32];

    if (value < slider->minimum) {
        value = slider->minimum;
    }
    if (value > slider->maximum) {
        value = slider->maximum;
    }
    if (value == slider->value) {
        return SCHULTZ_OK;
    }
    slider->value = value;

    /* What a screen reader reads out, kept in step with what is drawn. */
    snprintf(text, sizeof(text), "%.4g", (double)value);
    schultz_node_set_value(tree, node, text);
    return schultz_node_invalidate(tree, node);
}

static int32_t schultz_slider_measure(schultz_tree *tree, schultz_handle node,
                                      float avail_w, float avail_h,
                                      schultz_size *out_size)
{
    const schultz_slider_data *slider =
        (const schultz_slider_data *)schultz_node_widget_data(tree, node);

    (void)avail_w;
    (void)avail_h;
    /* Long along its own axis and thin across it, whichever axis that is. */
    if (slider != NULL && slider->orientation == SCHULTZ_ORIENT_VERTICAL) {
        *out_size = schultz_size_make(SCHULTZ_SLIDER_HEIGHT,
                                      SCHULTZ_TRACK_LENGTH);
    } else {
        *out_size = schultz_size_make(SCHULTZ_TRACK_LENGTH,
                                      SCHULTZ_SLIDER_HEIGHT);
    }
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_slider_pane = {
    schultz_slider_measure, schultz_leaf_arrange
};

/*
 * The track a slider's thumb runs along, inset by half a thumb at each end so
 * the thumb stays inside the node at both extremes.
 *
 * The thumb is as wide as the slider is thin, whichever way round that is, so
 * the two orientations differ only in which extent is which.
 */
static schultz_rect schultz_slider_track(schultz_rect bounds,
                                         uint32_t orientation,
                                         float *out_thumb)
{
    float across = (orientation == SCHULTZ_ORIENT_VERTICAL) ? bounds.width
                                                            : bounds.height;
    float thumb = across;
    float thickness = across * 0.25f;

    if (thickness < 2.0f) {
        thickness = 2.0f;
    }
    *out_thumb = thumb;
    if (orientation == SCHULTZ_ORIENT_VERTICAL) {
        return schultz_rect_make(bounds.x + (bounds.width - thickness) * 0.5f,
                                 bounds.y + thumb * 0.5f, thickness,
                                 bounds.height - thumb);
    }
    return schultz_rect_make(bounds.x + thumb * 0.5f,
                             bounds.y + (bounds.height - thickness) * 0.5f,
                             bounds.width - thumb, thickness);
}

static int32_t schultz_slider_paint(schultz_tree *tree, schultz_handle node,
                                    schultz_draw_list *list,
                                    schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    const schultz_slider_data *slider =
        (const schultz_slider_data *)schultz_node_widget_data(tree, node);
    schultz_rect bounds;
    schultz_rect track;
    float thumb = 0.0f;
    float fraction;
    float centre;

    (void)arena;
    if (slider == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    track = schultz_slider_track(bounds, slider->orientation, &thumb);
    fraction = schultz_slider_fraction(slider);

    /* The whole track, then the part behind the thumb, then the thumb. */
    if (slider->orientation == SCHULTZ_ORIENT_VERTICAL) {
        /* Upward, so the minimum is at the bottom and the bar grows the way
         * a column of liquid does. */
        centre = track.y + track.height * (1.0f - fraction);
        schultz_draw_fill_round_rect(list, track,
            schultz_resolved_paint(style, SCHULTZ_PROP_BORDER_COLOR),
            track.width * 0.5f);
        schultz_draw_fill_round_rect(list,
            schultz_rect_make(track.x, centre, track.width,
                              track.height * fraction),
            schultz_resolved_paint(style, SCHULTZ_PROP_BACKGROUND),
            track.width * 0.5f);
    } else {
        centre = track.x + track.width * fraction;
        schultz_draw_fill_round_rect(list, track,
            schultz_resolved_paint(style, SCHULTZ_PROP_BORDER_COLOR),
            track.height * 0.5f);
        schultz_draw_fill_round_rect(list,
            schultz_rect_make(track.x, track.y, track.width * fraction,
                              track.height),
            schultz_resolved_paint(style, SCHULTZ_PROP_BACKGROUND),
            track.height * 0.5f);
    }
    /*
     * A filled knob with a ring around it. One colour alone cannot stand out
     * against both halves of the track and the window behind it, so the fill
     * separates it from the track and the ring separates it from the window.
     */
    {
        schultz_rect knob =
            (slider->orientation == SCHULTZ_ORIENT_VERTICAL)
                ? schultz_rect_make(bounds.x, centre - thumb * 0.5f, thumb,
                                    thumb)
                : schultz_rect_make(centre - thumb * 0.5f, bounds.y, thumb,
                                    thumb);
        schultz_stroke ring = schultz_widget_stroke(style);

        schultz_draw_fill_ellipse(list, knob,
            schultz_resolved_paint(style, SCHULTZ_PROP_SELECTION_COLOR));
        ring.paint = schultz_resolved_paint(style, SCHULTZ_PROP_BACKGROUND);
        ring.width = 2.0f;
        ring.dash  = SCHULTZ_HANDLE_NONE;
        schultz_draw_stroke_ellipse(list,
            schultz_rect_make(knob.x + 1.0f, knob.y + 1.0f, knob.width - 2.0f,
                              knob.height - 2.0f), ring);
    }

    return schultz_widget_draw_focus_ring(list, style, bounds,
                                          schultz_node_get_state(tree, node));
}

/*
 * Pointer and keyboard both end up in the same place: a new value. Keys are
 * consumed, because an arrow that moved a slider must not also move focus or
 * scroll whatever contains it.
 */
static int32_t schultz_slider_event(schultz_tree *tree, schultz_handle node,
                                    const schultz_event *event)
{
    schultz_slider_data *slider =
        (schultz_slider_data *)schultz_node_widget_data(tree, node);
    schultz_rect bounds;
    schultz_rect track;
    float thumb = 0.0f;

    if (slider == NULL) {
        return SCHULTZ_OK;
    }

    switch (event->type) {
    case SCHULTZ_EVENT_MOUSE_DOWN:
    case SCHULTZ_EVENT_DRAG:
        if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
            return SCHULTZ_OK;
        }
        track = schultz_slider_track(bounds, slider->orientation, &thumb);
        /*
         * Local coordinates already have the node's origin taken off, so the
         * only correction left is the half thumb the track is inset by. A
         * vertical slider counts from the bottom, which is the one sign that
         * differs between the two.
         */
        if (slider->orientation == SCHULTZ_ORIENT_VERTICAL) {
            if (track.height <= 0.0f) {
                return SCHULTZ_OK;
            }
            schultz_slider_store(tree, node, slider,
                slider->minimum + (slider->maximum - slider->minimum) *
                    (1.0f - (event->local.y - thumb * 0.5f) / track.height));
            return SCHULTZ_EVENT_CONSUMED;
        }
        if (track.width <= 0.0f) {
            return SCHULTZ_OK;
        }
        schultz_slider_store(tree, node, slider,
            slider->minimum + (slider->maximum - slider->minimum) *
                ((event->local.x - thumb * 0.5f) / track.width));
        return SCHULTZ_EVENT_CONSUMED;

    case SCHULTZ_EVENT_KEY_DOWN:
        switch (event->key) {
        case SCHULTZ_KEY_LEFT:
        case SCHULTZ_KEY_DOWN:
            schultz_slider_store(tree, node, slider,
                                 slider->value - slider->step);
            return SCHULTZ_EVENT_CONSUMED;
        case SCHULTZ_KEY_RIGHT:
        case SCHULTZ_KEY_UP:
            schultz_slider_store(tree, node, slider,
                                 slider->value + slider->step);
            return SCHULTZ_EVENT_CONSUMED;
        case SCHULTZ_KEY_HOME:
            schultz_slider_store(tree, node, slider, slider->minimum);
            return SCHULTZ_EVENT_CONSUMED;
        case SCHULTZ_KEY_END:
            schultz_slider_store(tree, node, slider, slider->maximum);
            return SCHULTZ_EVENT_CONSUMED;
        default:
            break;
        }
        break;

    default:
        break;
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_slider_widget = {
    .paint = schultz_slider_paint, .event = schultz_slider_event, .destroy = free
};

int32_t schultz_slider_create(schultz_tree *tree, schultz_handle parent,
                              uint32_t orientation, float minimum,
                              float maximum, float value,
                              schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_slider_data *slider;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL || maximum < minimum ||
        orientation > SCHULTZ_ORIENT_VERTICAL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    slider = (schultz_slider_data *)calloc(1, sizeof(*slider));
    if (slider == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    slider->minimum = minimum;
    slider->maximum = maximum;
    slider->value   = minimum;
    slider->orientation = orientation;
    /* A hundred steps across the range, which is what a volume slider wants. */
    slider->step    = (maximum - minimum) / 100.0f;

    schultz_node_set_pane(tree, node, &schultz_slider_pane);
    schultz_node_set_widget(tree, node, &schultz_slider_widget, slider);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_SLIDER);
    schultz_node_set_actions(tree, node,
        SCHULTZ_ACTION_CLICK | SCHULTZ_ACTION_FOCUS |
        SCHULTZ_ACTION_INCREMENT | SCHULTZ_ACTION_DECREMENT |
        SCHULTZ_ACTION_SET_VALUE);
    schultz_node_set_paint_margin(tree, node, SCHULTZ_RING_BLEED);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_ACCENT);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                            SCHULTZ_TOKEN_COLOR_SURFACE_SUNKEN);
        /* The knob's fill. It sits inside the accent ring set in the paint,
         * so it reads against the filled half of the track. */
        schultz_patch_token(&patch, SCHULTZ_PROP_SELECTION_COLOR,
                            SCHULTZ_TOKEN_COLOR_SURFACE);
        schultz_widget_default_style(tree, node, &patch);
    }
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_DISABLED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_DISABLED));

    schultz_slider_store(tree, node, slider, value);
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_slider_set_value(schultz_tree *tree, schultz_handle node,
                                 float value)
{
    schultz_slider_data *slider;

    if (schultz_node_widget(tree, node) != &schultz_slider_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    slider = (schultz_slider_data *)schultz_node_widget_data(tree, node);
    return schultz_slider_store(tree, node, slider, value);
}

float schultz_slider_value(const schultz_tree *tree, schultz_handle node)
{
    const schultz_slider_data *slider;

    if (schultz_node_widget(tree, node) != &schultz_slider_widget) {
        return 0.0f;
    }
    slider = (const schultz_slider_data *)schultz_node_widget_data(tree,
                                                                   node);
    return (slider == NULL) ? 0.0f : slider->value;
}

int32_t schultz_slider_set_step(schultz_tree *tree, schultz_handle node,
                                float step)
{
    schultz_slider_data *slider;

    if (schultz_node_widget(tree, node) != &schultz_slider_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (step <= 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    slider = (schultz_slider_data *)schultz_node_widget_data(tree, node);
    slider->step = step;
    return SCHULTZ_OK;
}

/* ---------------------------------------------------------- ProgressBar */

/** How far along a task is, or where an unmeasured one's marker sits. */
typedef struct {
    float    value;         /**< 0 to 1. */
    uint32_t indeterminate; /**< Nonzero when the value is a sweep position. */
    uint32_t orientation;   /**< SCHULTZ_ORIENT_HORIZONTAL or _VERTICAL. */
} schultz_progress_data;

static const schultz_widget_vtable schultz_progress_widget;

/** How much of the track an unmeasured progress bar's marker covers. */
#define SCHULTZ_PROGRESS_MARKER 0.33f

static int32_t schultz_progress_measure(schultz_tree *tree,
                                        schultz_handle node, float avail_w,
                                        float avail_h,
                                        schultz_size *out_size)
{
    const schultz_progress_data *bar =
        (const schultz_progress_data *)schultz_node_widget_data(tree, node);

    (void)avail_w;
    (void)avail_h;
    /* Long along its own axis and thin across it, whichever axis that is. */
    if (bar != NULL && bar->orientation == SCHULTZ_ORIENT_VERTICAL) {
        *out_size = schultz_size_make(SCHULTZ_PROGRESS_HEIGHT,
                                      SCHULTZ_TRACK_LENGTH);
    } else {
        *out_size = schultz_size_make(SCHULTZ_TRACK_LENGTH,
                                      SCHULTZ_PROGRESS_HEIGHT);
    }
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_progress_pane = {
    schultz_progress_measure, schultz_leaf_arrange
};

static int32_t schultz_progress_paint(schultz_tree *tree, schultz_handle node,
                                      schultz_draw_list *list,
                                      schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    const schultz_progress_data *bar =
        (const schultz_progress_data *)schultz_node_widget_data(tree, node);
    schultz_paint fill;
    schultz_rect bounds;
    float radius;
    float x;
    float width;

    (void)arena;
    if (bar == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    if (bar->orientation == SCHULTZ_ORIENT_VERTICAL) {
        float height;
        float y;

        radius = bounds.width * 0.5f;
        schultz_draw_fill_round_rect(list, bounds,
            schultz_resolved_paint(style, SCHULTZ_PROP_BORDER_COLOR), radius);
        if (bar->indeterminate) {
            height = bounds.height * SCHULTZ_PROGRESS_MARKER;
            y = bounds.y + (bounds.height - height) * bar->value;
        } else {
            /* Filled from the bottom, because that is the way a column
             * fills and the way a person reads one. */
            height = bounds.height * bar->value;
            y = bounds.y + bounds.height - height;
        }
        if (height <= 0.0f) {
            return SCHULTZ_OK;
        }
        fill = schultz_resolved_paint(style, SCHULTZ_PROP_BACKGROUND);
        return schultz_draw_fill_round_rect(list,
            schultz_rect_make(bounds.x, y, bounds.width, height), fill,
            radius);
    }

    radius = bounds.height * 0.5f;
    schultz_draw_fill_round_rect(list, bounds,
        schultz_resolved_paint(style, SCHULTZ_PROP_BORDER_COLOR), radius);

    if (bar->indeterminate) {
        /*
         * Nothing here knows what time it is, so an unmeasured bar reads its
         * value as where the marker sits rather than how full the bar is.
         * The host moves it, the same way it moves anything else that
         * animates.
         */
        width = bounds.width * SCHULTZ_PROGRESS_MARKER;
        x = bounds.x + (bounds.width - width) * bar->value;
    } else {
        width = bounds.width * bar->value;
        x = bounds.x;
    }
    if (width <= 0.0f) {
        return SCHULTZ_OK;
    }
    fill = schultz_resolved_paint(style, SCHULTZ_PROP_BACKGROUND);
    return schultz_draw_fill_round_rect(list,
        schultz_rect_make(x, bounds.y, width, bounds.height), fill, radius);
}

static const schultz_widget_vtable schultz_progress_widget = {
    .paint = schultz_progress_paint, .destroy = free
};

int32_t schultz_progress_bar_create(schultz_tree *tree, schultz_handle parent,
                                    uint32_t orientation,
                                    schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_progress_data *bar;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL || orientation > SCHULTZ_ORIENT_VERTICAL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    bar = (schultz_progress_data *)calloc(1, sizeof(*bar));
    if (bar == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    bar->orientation = orientation;

    schultz_node_set_pane(tree, node, &schultz_progress_pane);
    schultz_node_set_widget(tree, node, &schultz_progress_widget, bar);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_PROGRESS);
    schultz_node_set_value(tree, node, "0");

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_ACCENT);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                            SCHULTZ_TOKEN_COLOR_SURFACE_SUNKEN);
        schultz_widget_default_style(tree, node, &patch);
    }
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_progress_bar_set_value(schultz_tree *tree,
                                       schultz_handle node, float value)
{
    schultz_progress_data *bar;
    char text[32];

    if (schultz_node_widget(tree, node) != &schultz_progress_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    bar = (schultz_progress_data *)schultz_node_widget_data(tree, node);
    if (value < 0.0f) {
        value = 0.0f;
    }
    if (value > 1.0f) {
        value = 1.0f;
    }
    if (value == bar->value) {
        return SCHULTZ_OK;
    }
    bar->value = value;
    snprintf(text, sizeof(text), "%.4g", (double)value);
    schultz_node_set_value(tree, node, text);
    return schultz_node_invalidate(tree, node);
}

float schultz_progress_bar_value(const schultz_tree *tree,
                                 schultz_handle node)
{
    const schultz_progress_data *bar;

    if (schultz_node_widget(tree, node) != &schultz_progress_widget) {
        return 0.0f;
    }
    bar = (const schultz_progress_data *)schultz_node_widget_data(tree, node);
    return (bar == NULL) ? 0.0f : bar->value;
}

int32_t schultz_progress_bar_set_indeterminate(schultz_tree *tree,
                                               schultz_handle node,
                                               int32_t indeterminate)
{
    schultz_progress_data *bar;

    if (schultz_node_widget(tree, node) != &schultz_progress_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    bar = (schultz_progress_data *)schultz_node_widget_data(tree, node);
    bar->indeterminate = indeterminate ? 1u : 0u;
    return schultz_node_invalidate(tree, node);
}

/* ------------------------------------------------------------ ScrollBar */

/**
 * @brief Natural thickness of a scroll bar, before any size hint.
 *
 * Thin, the way a phone's is: the bar is a position indicator that happens to
 * be draggable, not a control that has to be aimed at.
 */
#define SCHULTZ_BAR_THICKNESS 8.0f
/** Smallest a thumb is drawn, however little of the content is showing. */
#define SCHULTZ_THUMB_MINIMUM 20.0f

/** What a scroll bar knows: how much there is, how much shows, and where. */
typedef struct {
    uint32_t orientation; /**< SCHULTZ_ORIENT_HORIZONTAL or _VERTICAL. */
    float    content;     /**< Total length of what is being scrolled. */
    float    viewport;    /**< Length of the part that shows. */
    float    value;       /**< Offset of the visible part, from the start. */
    float    grab;        /**< Where in the thumb a drag started. */
    schultz_scroll_bar_changed_fn changed; /**< Told when the value moves. */
    void    *context;     /**< Passed to it, unchanged. */
} schultz_scroll_bar_data;

static const schultz_widget_vtable schultz_scroll_bar_widget;

/** Nonzero when the bar runs down rather than across. */
static int32_t schultz_bar_vertical(const schultz_scroll_bar_data *bar)
{
    return bar->orientation == SCHULTZ_ORIENT_VERTICAL;
}

/** The furthest the visible part can be scrolled, never below zero. */
static float schultz_bar_maximum(const schultz_scroll_bar_data *bar)
{
    float span = bar->content - bar->viewport;

    return (span > 0.0f) ? span : 0.0f;
}

/*
 * Clamps and stores, marking the node only when the value really moved, and
 * telling whoever asked to be told. A bar moves for many reasons, so this is
 * the one place that reports it: dragging the thumb, paging the track, a key,
 * or an application setting the value all arrive here.
 */
static int32_t schultz_bar_store(schultz_tree *tree, schultz_handle node,
                                 schultz_scroll_bar_data *bar, float value)
{
    float maximum = schultz_bar_maximum(bar);

    if (value < 0.0f) {
        value = 0.0f;
    }
    if (value > maximum) {
        value = maximum;
    }
    if (value == bar->value) {
        return SCHULTZ_OK;
    }
    bar->value = value;
    if (bar->changed != NULL) {
        bar->changed(bar->context, tree, node, value);
    }
    return schultz_node_invalidate(tree, node);
}

/*
 * The thumb's rectangle. Its length is the fraction of the content that
 * shows, floored so it stays grabbable in a very long document, and its
 * position is the value as a fraction of how far there is to go.
 */
static schultz_rect schultz_bar_thumb(const schultz_scroll_bar_data *bar,
                                      schultz_rect bounds)
{
    int32_t vertical = schultz_bar_vertical(bar);
    float track = vertical ? bounds.height : bounds.width;
    float fraction = (bar->content > 0.0f) ? bar->viewport / bar->content
                                           : 1.0f;
    float length;
    float maximum = schultz_bar_maximum(bar);
    float at;

    if (fraction > 1.0f) {
        fraction = 1.0f;
    }
    length = track * fraction;
    if (length < SCHULTZ_THUMB_MINIMUM) {
        length = SCHULTZ_THUMB_MINIMUM;
    }
    if (length > track) {
        length = track;
    }
    at = (maximum > 0.0f) ? (track - length) * (bar->value / maximum) : 0.0f;

    return vertical
        ? schultz_rect_make(bounds.x, bounds.y + at, bounds.width, length)
        : schultz_rect_make(bounds.x + at, bounds.y, length, bounds.height);
}

static int32_t schultz_bar_measure(schultz_tree *tree, schultz_handle node,
                                   float avail_w, float avail_h,
                                   schultz_size *out_size)
{
    const schultz_scroll_bar_data *bar =
        (const schultz_scroll_bar_data *)schultz_node_widget_data(tree, node);

    (void)avail_w;
    (void)avail_h;
    if (bar == NULL) {
        *out_size = schultz_size_make(0.0f, 0.0f);
        return SCHULTZ_OK;
    }
    /* Thin on its own axis and unopinionated on the other. */
    *out_size = schultz_bar_vertical(bar)
        ? schultz_size_make(SCHULTZ_BAR_THICKNESS, SCHULTZ_TRACK_LENGTH)
        : schultz_size_make(SCHULTZ_TRACK_LENGTH, SCHULTZ_BAR_THICKNESS);
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_scroll_bar_pane = {
    schultz_bar_measure, schultz_leaf_arrange
};

static int32_t schultz_bar_paint(schultz_tree *tree, schultz_handle node,
                                 schultz_draw_list *list,
                                 schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    const schultz_scroll_bar_data *bar =
        (const schultz_scroll_bar_data *)schultz_node_widget_data(tree, node);
    schultz_rect bounds;
    schultz_rect thumb;
    float radius;

    (void)arena;
    if (bar == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        schultz_rect_is_empty(bounds)) {
        return SCHULTZ_OK;
    }
    radius = (schultz_bar_vertical(bar) ? bounds.width : bounds.height) * 0.5f;
    thumb = schultz_bar_thumb(bar, bounds);

    schultz_draw_fill_round_rect(list, bounds,
        schultz_resolved_paint(style, SCHULTZ_PROP_BORDER_COLOR), radius);
    return schultz_draw_fill_round_rect(list, thumb,
        schultz_resolved_paint(style, SCHULTZ_PROP_BACKGROUND), radius);
}

/*
 * Dragging the thumb keeps the grab point under the pointer, which is what
 * makes a scroll bar feel attached rather than snapping to the middle.
 * Pressing the track beyond the thumb pages towards the pointer.
 */
static int32_t schultz_bar_event(schultz_tree *tree, schultz_handle node,
                                 const schultz_event *event)
{
    schultz_scroll_bar_data *bar =
        (schultz_scroll_bar_data *)schultz_node_widget_data(tree, node);
    schultz_rect bounds;
    schultz_rect thumb;
    float track;
    float local;
    float maximum;

    if (bar == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    thumb   = schultz_bar_thumb(bar, bounds);
    track   = schultz_bar_vertical(bar) ? bounds.height : bounds.width;
    local   = schultz_bar_vertical(bar) ? event->local.y : event->local.x;
    maximum = schultz_bar_maximum(bar);

    switch (event->type) {
    case SCHULTZ_EVENT_MOUSE_DOWN: {
        float thumb_at = schultz_bar_vertical(bar) ? thumb.y - bounds.y
                                                   : thumb.x - bounds.x;
        float thumb_len = schultz_bar_vertical(bar) ? thumb.height
                                                    : thumb.width;

        if (local >= thumb_at && local < thumb_at + thumb_len) {
            bar->grab = local - thumb_at;
        } else {
            /* Page towards the press, then grab from the middle. */
            schultz_bar_store(tree, node, bar,
                bar->value + ((local < thumb_at) ? -bar->viewport
                                                 : bar->viewport));
            bar->grab = thumb_len * 0.5f;
        }
        return SCHULTZ_EVENT_CONSUMED;
    }

    case SCHULTZ_EVENT_DRAG: {
        float thumb_len = schultz_bar_vertical(bar) ? thumb.height
                                                    : thumb.width;
        float room = track - thumb_len;

        if (room > 0.0f) {
            schultz_bar_store(tree, node, bar,
                              (local - bar->grab) / room * maximum);
        }
        return SCHULTZ_EVENT_CONSUMED;
    }

    case SCHULTZ_EVENT_KEY_DOWN:
        switch (event->key) {
        case SCHULTZ_KEY_LEFT:
        case SCHULTZ_KEY_UP:
            schultz_bar_store(tree, node, bar, bar->value - bar->viewport
                                                   * 0.1f);
            return SCHULTZ_EVENT_CONSUMED;
        case SCHULTZ_KEY_RIGHT:
        case SCHULTZ_KEY_DOWN:
            schultz_bar_store(tree, node, bar, bar->value + bar->viewport
                                                   * 0.1f);
            return SCHULTZ_EVENT_CONSUMED;
        case SCHULTZ_KEY_PAGE_UP:
            schultz_bar_store(tree, node, bar, bar->value - bar->viewport);
            return SCHULTZ_EVENT_CONSUMED;
        case SCHULTZ_KEY_PAGE_DOWN:
            schultz_bar_store(tree, node, bar, bar->value + bar->viewport);
            return SCHULTZ_EVENT_CONSUMED;
        case SCHULTZ_KEY_HOME:
            schultz_bar_store(tree, node, bar, 0.0f);
            return SCHULTZ_EVENT_CONSUMED;
        case SCHULTZ_KEY_END:
            schultz_bar_store(tree, node, bar, maximum);
            return SCHULTZ_EVENT_CONSUMED;
        default:
            break;
        }
        break;

    default:
        break;
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_scroll_bar_widget = {
    .paint = schultz_bar_paint, .event = schultz_bar_event, .destroy = free
};

int32_t schultz_scroll_bar_create(schultz_tree *tree, schultz_handle parent,
                                  uint32_t orientation,
                                  schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_scroll_bar_data *bar;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL || orientation > SCHULTZ_ORIENT_VERTICAL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    bar = (schultz_scroll_bar_data *)calloc(1, sizeof(*bar));
    if (bar == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    bar->orientation = orientation;
    bar->content     = 1.0f;
    bar->viewport    = 1.0f;

    schultz_node_set_pane(tree, node, &schultz_scroll_bar_pane);
    schultz_node_set_widget(tree, node, &schultz_scroll_bar_widget, bar);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_SLIDER);
    schultz_node_set_actions(tree, node,
        SCHULTZ_ACTION_FOCUS | SCHULTZ_ACTION_INCREMENT |
        SCHULTZ_ACTION_DECREMENT | SCHULTZ_ACTION_SET_VALUE |
        SCHULTZ_ACTION_SCROLL);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        /* Background is the grip and border is the groove: the two things
         * this widget draws, in the two colours named for them. */
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_SCROLL_THUMB);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                            SCHULTZ_TOKEN_COLOR_SCROLL_TRACK);
        schultz_widget_default_style(tree, node, &patch);
    }
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_HOVERED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_scroll_bar_set_range(schultz_tree *tree, schultz_handle node,
                                     float content, float viewport)
{
    schultz_scroll_bar_data *bar;

    if (schultz_node_widget(tree, node) != &schultz_scroll_bar_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (content < 0.0f || viewport < 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    bar = (schultz_scroll_bar_data *)schultz_node_widget_data(tree, node);
    if (bar->content == content && bar->viewport == viewport) {
        return SCHULTZ_OK;
    }
    bar->content  = content;
    bar->viewport = viewport;
    /* A shorter document may leave the value past the end. */
    schultz_bar_store(tree, node, bar, bar->value);
    return schultz_node_invalidate(tree, node);
}

int32_t schultz_scroll_bar_set_value(schultz_tree *tree, schultz_handle node,
                                     float value)
{
    if (schultz_node_widget(tree, node) != &schultz_scroll_bar_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_bar_store(tree, node,
        (schultz_scroll_bar_data *)schultz_node_widget_data(tree, node),
        value);
}

float schultz_scroll_bar_value(const schultz_tree *tree, schultz_handle node)
{
    const schultz_scroll_bar_data *bar;

    if (schultz_node_widget(tree, node) != &schultz_scroll_bar_widget) {
        return 0.0f;
    }
    bar = (const schultz_scroll_bar_data *)schultz_node_widget_data(tree,
                                                                    node);
    return (bar == NULL) ? 0.0f : bar->value;
}

int32_t schultz_scroll_bar_on_change(schultz_tree *tree, schultz_handle node,
                                     schultz_scroll_bar_changed_fn changed,
                                     void *context)
{
    schultz_scroll_bar_data *bar;

    if (schultz_node_widget(tree, node) != &schultz_scroll_bar_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    bar = (schultz_scroll_bar_data *)schultz_node_widget_data(tree, node);
    bar->changed = changed;
    bar->context = context;
    return SCHULTZ_OK;
}

float schultz_scroll_bar_maximum(const schultz_tree *tree,
                                 schultz_handle node)
{
    const schultz_scroll_bar_data *bar;

    if (schultz_node_widget(tree, node) != &schultz_scroll_bar_widget) {
        return 0.0f;
    }
    bar = (const schultz_scroll_bar_data *)schultz_node_widget_data(tree,
                                                                    node);
    return (bar == NULL) ? 0.0f : schultz_bar_maximum(bar);
}

/* ----------------------------------------------------------- ScrollView */

/** How far one notch of the wheel moves the content. */
#define SCHULTZ_WHEEL_STEP 48.0f

/*
 * How far a finger travels before it is scrolling rather than pressing.
 *
 * The same distance the router allows a tap, so the two agree: past it the
 * view starts moving and the router stops calling the release a click. A
 * smaller number here would scroll on presses the router still counted as
 * taps, and the content would jump under a finger that only meant to press.
 */
#define SCHULTZ_DRAG_SLOP 10.0f

/*
 * What a flick loses each millisecond it carries on, and how slow it has to
 * get before it stops.
 *
 * Written per millisecond rather than per frame so the same flick travels the
 * same distance whether the display is running at sixty or at a hundred and
 * twenty.
 */
#define SCHULTZ_GLIDE_DECAY 0.996f
/** Below this, in units per millisecond, a glide has arrived. */
#define SCHULTZ_GLIDE_STOP 0.02f

/** A scroll view's parts, so it never has to guess which child is which. */
typedef struct {
    schultz_handle viewport; /**< Clips and offsets; holds the content. */
    schultz_handle content;  /**< What the application fills. */
    schultz_handle bar_x;    /**< Horizontal bar, along the bottom. */
    schultz_handle bar_y;    /**< Vertical bar, down the right. */
    /*
     * Dragging with a finger. A mouse scrolls with its wheel and never comes
     * through here; a touch screen has no wheel, and before this the only
     * thing on a phone that scrolled a view was its own bar, eight units
     * wide against a documented touch target minimum of forty four.
     */
    uint32_t     touching;   /**< A finger is down on the content. */
    uint32_t     dragging;   /**< It has moved far enough to be scrolling. */
    schultz_point origin;    /**< Where it went down, in window units. */
    float        from_x;     /**< The offsets when it went down. */
    float        from_y;
    /*
     * Speed is measured from how far the view actually moved between two
     * ticks rather than from the events, because an event carries no time
     * and the tick is handed one. It is also the truthful number: what the
     * view did, not what the finger asked for.
     */
    float        last_x;     /**< Where the offsets were at the last tick. */
    float        last_y;
    float        speed_x;    /**< Units per millisecond, signed. */
    float        speed_y;
    uint32_t     gliding;    /**< Carrying on after the finger let go. */
    /**
     * Whether the bars may show at all.
     *
     * A view on a touch panel is dragged rather than aimed at, and a bar
     * there is a strip of height taken from the content for something
     * nobody uses. See schultz_scroll_view_set_bars.
     */
    int32_t      bars;
    /**
     * Whether a pointer that is not a finger may drag the content.
     *
     * Off, because a cursor has a wheel and a bar and dragging with one is
     * how text is selected. On where there is nothing to select and dragging
     * is the whole gesture, which is what the keyboard's field of emoji is.
     * See schultz_scroll_view_set_drag_scrolls.
     */
    int32_t      drag_scrolls;
} schultz_scroll_view_data;

static const schultz_widget_vtable schultz_scroll_view_widget;

/* Shows or hides a bar, which is a state flag rather than a size of zero. */
static void schultz_scroll_show(schultz_tree *tree, schultz_handle bar,
                                int32_t shown)
{
    uint32_t state = schultz_node_get_state(tree, bar);

    schultz_node_set_state(tree, bar,
        shown ? (state | SCHULTZ_STATE_VISIBLE)
              : (state & ~(uint32_t)SCHULTZ_STATE_VISIBLE));
}

/* Pushes the bar values into the viewport's offset. One place does this. */
static void schultz_scroll_apply(schultz_tree *tree,
                                 const schultz_scroll_view_data *view);

/*
 * A bar moved, however it was moved: dragged, paged, keyed, or set by the
 * application. The content follows from here rather than from each of those,
 * so there is no way to move a bar and leave the content behind.
 */
static void schultz_scroll_bar_moved(void *context, schultz_tree *tree,
                                     schultz_handle bar, float value)
{
    (void)bar;
    (void)value;
    schultz_scroll_apply(tree, (const schultz_scroll_view_data *)context);
}

static void schultz_scroll_apply(schultz_tree *tree,
                                 const schultz_scroll_view_data *view)
{
    schultz_node_set_scroll_offset(tree, view->viewport,
        schultz_point_make(schultz_scroll_bar_value(tree, view->bar_x),
                           schultz_scroll_bar_value(tree, view->bar_y)));
}

/*
 * A scroll view asks for whatever its content wants. Under a parent that
 * gives it less, the difference is what there is to scroll; under one that
 * gives it more, the bars simply have nowhere to go.
 */
static int32_t schultz_scroll_measure(schultz_tree *tree, schultz_handle node,
                                      float avail_w, float avail_h,
                                      schultz_size *out_size)
{
    const schultz_scroll_view_data *view =
        (const schultz_scroll_view_data *)schultz_node_widget_data(tree,
                                                                   node);

    if (view == NULL) {
        *out_size = schultz_size_make(0.0f, 0.0f);
        return SCHULTZ_OK;
    }
    return schultz_layout_measure(tree, view->content, avail_w, avail_h,
                                  out_size);
}

/*
 * The content is measured against the width it will actually get and given
 * its full height, because a taller than the viewport content is the whole
 * point. The bars are laid along the edges that need them and hidden
 * otherwise, which is what makes a short document look like a plain panel.
 */
static int32_t schultz_scroll_arrange(schultz_tree *tree, schultz_handle node,
                                      schultz_rect rect)
{
    schultz_scroll_view_data *view =
        (schultz_scroll_view_data *)schultz_node_widget_data(tree, node);
    schultz_size content;
    float inner_w;
    float inner_h;
    int32_t needs_x;
    int32_t needs_y;

    if (view == NULL) {
        return SCHULTZ_OK;
    }

    /*
     * Whether one bar is needed depends on whether the other is, because each
     * takes room from the viewport, and the content has to be measured at the
     * width it will actually get: wrapped text in a narrower column is
     * taller. Two rounds settle it, and a third cannot change what a second
     * did not.
     */
    needs_x = 0;
    needs_y = 0;
    inner_w = rect.width;
    inner_h = rect.height;
    {
        int32_t round;

        for (round = 0; round < 2; round++) {
            schultz_layout_measure(tree, view->content, inner_w, -1.0f,
                                   &content);
            needs_y = view->bars && content.height > inner_h;
            inner_w = rect.width - (needs_y ? SCHULTZ_BAR_THICKNESS : 0.0f);
            needs_x = view->bars && content.width > inner_w;
            inner_h = rect.height - (needs_x ? SCHULTZ_BAR_THICKNESS : 0.0f);
        }
        /* Measure once more at the width that was settled on. */
        schultz_layout_measure(tree, view->content, inner_w, -1.0f, &content);
    }

    schultz_node_set_bounds(tree, view->viewport,
                            schultz_rect_make(0.0f, 0.0f, inner_w, inner_h));
    /* The content keeps its own size; the viewport is the window onto it. */
    schultz_layout_arrange(tree, view->content,
        schultz_rect_make(0.0f, 0.0f,
                          (content.width  > inner_w) ? content.width  : inner_w,
                          (content.height > inner_h) ? content.height : inner_h));

    schultz_scroll_show(tree, view->bar_x, needs_x);
    schultz_scroll_show(tree, view->bar_y, needs_y);
    schultz_node_set_bounds(tree, view->bar_x,
        schultz_rect_make(0.0f, inner_h, inner_w, SCHULTZ_BAR_THICKNESS));
    schultz_node_set_bounds(tree, view->bar_y,
        schultz_rect_make(inner_w, 0.0f, SCHULTZ_BAR_THICKNESS, inner_h));

    schultz_scroll_bar_set_range(tree, view->bar_x, content.width, inner_w);
    schultz_scroll_bar_set_range(tree, view->bar_y, content.height, inner_h);
    schultz_scroll_apply(tree, view);
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_scroll_view_pane = {
    schultz_scroll_measure, schultz_scroll_arrange
};

static int32_t schultz_scroll_paint(schultz_tree *tree, schultz_handle node,
                                    schultz_draw_list *list,
                                    schultz_arena *arena)
{
    schultz_rect bounds;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    return schultz_widget_draw_box(list, schultz_widget_style(tree, node),
                                   bounds);
}

/*
 * The wheel moves the bars, and the bars move the viewport. Nothing here
 * touches the offset directly, so the bars and the content can never
 * disagree about where the content is.
 */
/* Moves both bars to an offset and pushes it into the viewport. The bars
 * clamp, so this cannot scroll past either end. */
static void schultz_scroll_move_to(schultz_tree *tree,
                                   schultz_scroll_view_data *view,
                                   float x, float y)
{
    schultz_scroll_bar_set_value(tree, view->bar_x, x);
    schultz_scroll_bar_set_value(tree, view->bar_y, y);
    schultz_scroll_apply(tree, view);
}

/* Stops a drag or a glide and lets the clock go. */
static void schultz_scroll_settle(schultz_tree *tree, schultz_handle node,
                                  schultz_scroll_view_data *view)
{
    view->touching = 0u;
    view->dragging = 0u;
    view->gliding  = 0u;
    view->speed_x  = 0.0f;
    view->speed_y  = 0.0f;
    schultz_node_set_animating(tree, node, 0);
}

/* Whether one bar has anywhere left to go in the direction a finger moved. */
static int32_t schultz_scroll_bar_can_move(const schultz_tree *tree,
                                           schultz_handle bar, float by)
{
    float value = schultz_scroll_bar_value(tree, bar);

    if (by < 0.0f) {
        return (value > 0.0f);
    }
    if (by > 0.0f) {
        return (value < schultz_scroll_bar_maximum(tree, bar));
    }
    return 0;
}

/*
 * Whether this view can act on the drag at all.
 *
 * A scroll view inside another one -- a list view on a page, which is a
 * scroll view holding a scroll view -- sees a finger first, because events
 * rise from the row that was touched. Taking a drag it has nothing to move
 * for would leave the page still and read as touch scrolling being broken
 * over lists, which is what it did.
 *
 * The question is asked about the direction the finger actually went, so a
 * list that only scrolls sideways does not swallow a drag up the page. It is
 * asked once, when the drag passes the slop: whichever view takes a gesture
 * keeps it until the finger lifts, rather than handing it over part way
 * through and jumping.
 */
static int32_t schultz_scroll_can_move(const schultz_tree *tree,
                                       const schultz_scroll_view_data *view,
                                       float dx, float dy)
{
    float ax = (dx < 0.0f) ? -dx : dx;
    float ay = (dy < 0.0f) ? -dy : dy;

    if (ax > ay) {
        return schultz_scroll_bar_can_move(tree, view->bar_x, dx);
    }
    return schultz_scroll_bar_can_move(tree, view->bar_y, dy);
}

/*
 * Drag to scroll, and the flick that carries on afterwards.
 *
 * The content follows the finger: dragging up moves the content up, which is
 * to say further down the list, so the offset grows as the finger falls. Any
 * other direction feels like pushing a scroll bar rather than moving paper.
 *
 * Only for a finger. A mouse has a wheel and a bar, and a cursor that dragged
 * the content would take the gesture away from selecting text.
 */
static int32_t schultz_scroll_event(schultz_tree *tree, schultz_handle node,
                                    const schultz_event *event)
{
    schultz_scroll_view_data *view =
        (schultz_scroll_view_data *)schultz_node_widget_data(tree, node);

    if (view == NULL) {
        return SCHULTZ_OK;
    }

    if (event->type == SCHULTZ_EVENT_SCROLL) {
        float across = event->scroll_x;
        float down = event->scroll_y;

        /*
         * A wheel over a view that only goes sideways turns it sideways.
         * There is one wheel, a row of emoji has nowhere to go downwards,
         * and a wheel that did nothing at all would read as a view that
         * cannot scroll. Only when down is genuinely stuck, so a view with
         * both directions keeps answering the wheel the ordinary way.
         */
        if (down != 0.0f &&
            !schultz_scroll_bar_can_move(tree, view->bar_y, down) &&
            schultz_scroll_bar_can_move(tree, view->bar_x, down)) {
            across += down;
            down = 0.0f;
        }
        schultz_scroll_move_to(tree, view,
            schultz_scroll_bar_value(tree, view->bar_x) +
                across * SCHULTZ_WHEEL_STEP,
            schultz_scroll_bar_value(tree, view->bar_y) +
                down * SCHULTZ_WHEEL_STEP);
        return SCHULTZ_EVENT_CONSUMED;
    }

    if (event->source != SCHULTZ_POINTER_TOUCH && !view->drag_scrolls) {
        return SCHULTZ_OK;
    }

    switch (event->type) {
    case SCHULTZ_EVENT_MOUSE_DOWN:
        /* A finger down stops a glide where it is, which is what a reader
         * reaching out to halt a moving list expects. */
        view->touching = 1u;
        view->dragging = 0u;
        view->gliding  = 0u;
        view->origin   = event->position;
        view->from_x   = schultz_scroll_bar_value(tree, view->bar_x);
        view->from_y   = schultz_scroll_bar_value(tree, view->bar_y);
        view->last_x   = view->from_x;
        view->last_y   = view->from_y;
        view->speed_x  = 0.0f;
        view->speed_y  = 0.0f;
        /* Ticked from here on, which is what measures the speed. */
        schultz_node_set_animating(tree, node, 1);
        return SCHULTZ_OK;

    case SCHULTZ_EVENT_DRAG: {
        float dx = view->origin.x - event->position.x;
        float dy = view->origin.y - event->position.y;

        if (!view->touching) {
            return SCHULTZ_OK;
        }
        if (!view->dragging) {
            float ax = (dx < 0.0f) ? -dx : dx;
            float ay = (dy < 0.0f) ? -dy : dy;

            if (ax <= SCHULTZ_DRAG_SLOP && ay <= SCHULTZ_DRAG_SLOP) {
                return SCHULTZ_OK; /* still a press, not yet a scroll */
            }
            if (!schultz_scroll_can_move(tree, view, dx, dy)) {
                /* Nothing to move this way. Let go of the gesture so it
                 * rises to whatever encloses this, which is how a short
                 * list on a long page scrolls the page. */
                schultz_scroll_settle(tree, node, view);
                return SCHULTZ_OK;
            }
            view->dragging = 1u;
        }
        schultz_scroll_move_to(tree, view, view->from_x + dx,
                               view->from_y + dy);
        /* Consumed once it is a scroll, so nothing above also acts on it. */
        return SCHULTZ_EVENT_CONSUMED;
    }

    case SCHULTZ_EVENT_MOUSE_UP:
        if (!view->touching) {
            return SCHULTZ_OK;
        }
        view->touching = 0u;
        if (!view->dragging) {
            schultz_scroll_settle(tree, node, view);
            return SCHULTZ_OK;
        }
        view->dragging = 0u;
        /* Let go while still moving and it carries on; let go after holding
         * still and it stays where it was put. */
        if (view->speed_x != 0.0f || view->speed_y != 0.0f) {
            view->gliding = 1u;
            return SCHULTZ_EVENT_CONSUMED;
        }
        schultz_scroll_settle(tree, node, view);
        return SCHULTZ_EVENT_CONSUMED;

    default:
        break;
    }
    return SCHULTZ_OK;
}

/*
 * The clock's half of it: measure the speed while a finger is dragging, and
 * spend it once the finger is gone.
 */
static int32_t schultz_scroll_tick(schultz_tree *tree, schultz_handle node,
                                   uint64_t now_ms, uint32_t elapsed_ms)
{
    schultz_scroll_view_data *view =
        (schultz_scroll_view_data *)schultz_node_widget_data(tree, node);
    float at_x;
    float at_y;

    (void)now_ms;
    if (view == NULL || elapsed_ms == 0u) {
        return 0;
    }
    at_x = schultz_scroll_bar_value(tree, view->bar_x);
    at_y = schultz_scroll_bar_value(tree, view->bar_y);

    if (view->touching) {
        /*
         * How far it actually went, not how far the finger asked it to: a
         * view already at the end of its content does not move, and a flick
         * that ends there must not fly off when the finger lifts.
         */
        view->speed_x = (at_x - view->last_x) / (float)elapsed_ms;
        view->speed_y = (at_y - view->last_y) / (float)elapsed_ms;
        view->last_x  = at_x;
        view->last_y  = at_y;
        return 0;
    }

    if (!view->gliding) {
        schultz_node_set_animating(tree, node, 0);
        return 0;
    }

    schultz_scroll_move_to(tree, view,
                           at_x + view->speed_x * (float)elapsed_ms,
                           at_y + view->speed_y * (float)elapsed_ms);

    /* Per millisecond, so a flick covers the same ground at any frame rate. */
    {
        float keep = powf(SCHULTZ_GLIDE_DECAY, (float)elapsed_ms);

        view->speed_x *= keep;
        view->speed_y *= keep;
    }
    {
        float sx = (view->speed_x < 0.0f) ? -view->speed_x : view->speed_x;
        float sy = (view->speed_y < 0.0f) ? -view->speed_y : view->speed_y;
        float moved_x = schultz_scroll_bar_value(tree, view->bar_x) - at_x;
        float moved_y = schultz_scroll_bar_value(tree, view->bar_y) - at_y;

        /* Slow enough to have arrived, or stopped dead against an end. */
        if ((sx < SCHULTZ_GLIDE_STOP && sy < SCHULTZ_GLIDE_STOP) ||
            (moved_x == 0.0f && moved_y == 0.0f)) {
            schultz_scroll_settle(tree, node, view);
        }
    }
    return 1;
}

static const schultz_widget_vtable schultz_scroll_view_widget = {
    .paint = schultz_scroll_paint, .event = schultz_scroll_event,
    .tick = schultz_scroll_tick, .destroy = free
};

int32_t schultz_scroll_view_create(schultz_tree *tree, schultz_handle parent,
                                   schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_scroll_view_data *view;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    view = (schultz_scroll_view_data *)calloc(1, sizeof(*view));
    if (view == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    /* Bars show when the content needs them, which is what a desktop wants
     * and what every view has always done. */
    view->bars = 1;

    /*
     * The offset goes on the viewport rather than the scroll view, so the
     * bars, which are the scroll view's own children, do not scroll with the
     * thing they are scrolling.
     */
    result = schultz_panel_create(tree, node, &view->viewport);
    if (result == SCHULTZ_OK) {
        result = schultz_panel_create(tree, view->viewport, &view->content);
    }
    if (result == SCHULTZ_OK) {
        result = schultz_scroll_bar_create(tree, node,
                                           SCHULTZ_ORIENT_HORIZONTAL,
                                           &view->bar_x);
    }
    if (result == SCHULTZ_OK) {
        result = schultz_scroll_bar_create(tree, node, SCHULTZ_ORIENT_VERTICAL,
                                           &view->bar_y);
    }
    if (result != SCHULTZ_OK) {
        free(view);
        schultz_node_destroy(tree, node);
        return result;
    }

    schultz_scroll_bar_on_change(tree, view->bar_x, schultz_scroll_bar_moved,
                                 view);
    schultz_scroll_bar_on_change(tree, view->bar_y, schultz_scroll_bar_moved,
                                 view);
    schultz_node_set_pane(tree, view->content, schultz_pane_stack());
    schultz_node_set_clips_children(tree, view->viewport, 1);
    schultz_node_set_hit_testable(tree, view->viewport, 0);
    schultz_node_set_pane(tree, node, &schultz_scroll_view_pane);
    schultz_node_set_widget(tree, node, &schultz_scroll_view_widget, view);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_SCROLL_VIEW);
    schultz_node_set_actions(tree, node, SCHULTZ_ACTION_SCROLL);

    *out_node = node;
    return SCHULTZ_OK;
}

schultz_handle schultz_scroll_view_content(const schultz_tree *tree,
                                           schultz_handle node)
{
    const schultz_scroll_view_data *view;

    if (schultz_node_widget(tree, node) != &schultz_scroll_view_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    view = (const schultz_scroll_view_data *)schultz_node_widget_data(tree,
                                                                      node);
    return (view == NULL) ? SCHULTZ_HANDLE_NONE : view->content;
}

schultz_handle schultz_scroll_view_bar(const schultz_tree *tree,
                                       schultz_handle node,
                                       uint32_t orientation)
{
    const schultz_scroll_view_data *view;

    if (schultz_node_widget(tree, node) != &schultz_scroll_view_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    view = (const schultz_scroll_view_data *)schultz_node_widget_data(tree,
                                                                      node);
    if (view == NULL) {
        return SCHULTZ_HANDLE_NONE;
    }
    return (orientation == SCHULTZ_ORIENT_VERTICAL) ? view->bar_y
                                                    : view->bar_x;
}

/* How far one edge has to move for a span to sit inside a window. */
static float schultz_reveal_shift(float target_start, float target_size,
                                  float window_start, float window_size)
{
    if (target_start < window_start) {
        return target_start - window_start;          /* move back */
    }
    if (target_start + target_size > window_start + window_size) {
        /*
         * Bring the far edge in, but never so far that the near edge leaves:
         * a node taller than the window is shown from its top.
         */
        float shift = (target_start + target_size)
                      - (window_start + window_size);

        if (shift > target_start - window_start) {
            shift = target_start - window_start;
        }
        return shift;
    }
    return 0.0f;                                      /* already inside */
}

int32_t schultz_scroll_view_reveal(schultz_tree *tree, schultz_handle node,
                                   schultz_handle target)
{
    schultz_scroll_view_data *view;
    schultz_rect window;
    schultz_rect bounds;
    schultz_point offset;
    float shift_x;
    float shift_y;

    if (schultz_node_widget(tree, node) != &schultz_scroll_view_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    view = (schultz_scroll_view_data *)schultz_node_widget_data(tree, node);
    if (view == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (schultz_node_absolute_bounds(tree, view->viewport, &window)
            != SCHULTZ_OK ||
        schultz_node_absolute_bounds(tree, target, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }

    /*
     * Absolute bounds already carry the offsets of every scroll view above
     * them, so both rectangles are in the same space and the difference is
     * the distance to travel. Nothing to do when it is already inside, which
     * is the common case while walking a selection with the arrow keys.
     */
    shift_x = schultz_reveal_shift(bounds.x, bounds.width, window.x,
                                   window.width);
    shift_y = schultz_reveal_shift(bounds.y, bounds.height, window.y,
                                   window.height);
    if (shift_x == 0.0f && shift_y == 0.0f) {
        return SCHULTZ_OK;
    }

    offset = schultz_point_make(
        schultz_scroll_bar_value(tree, view->bar_x) + shift_x,
        schultz_scroll_bar_value(tree, view->bar_y) + shift_y);
    /* scroll_to clamps, so an overshoot at either end costs nothing. */
    return schultz_scroll_view_scroll_to(tree, node, offset);
}

int32_t schultz_scroll_view_set_bars(schultz_tree *tree, schultz_handle node,
                                     int32_t shown)
{
    schultz_scroll_view_data *view;

    if (schultz_node_widget(tree, node) != &schultz_scroll_view_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    view = (schultz_scroll_view_data *)schultz_node_widget_data(tree, node);
    if (view == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (view->bars == (shown ? 1 : 0)) {
        return SCHULTZ_OK;
    }
    view->bars = shown ? 1 : 0;
    if (!view->bars) {
        schultz_scroll_show(tree, view->bar_x, 0);
        schultz_scroll_show(tree, view->bar_y, 0);
    }
    schultz_node_invalidate_layout(tree, node);
    return SCHULTZ_OK;
}

int32_t schultz_scroll_view_set_drag_scrolls(schultz_tree *tree,
                                             schultz_handle node,
                                             int32_t drags)
{
    schultz_scroll_view_data *view;

    if (schultz_node_widget(tree, node) != &schultz_scroll_view_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    view = (schultz_scroll_view_data *)schultz_node_widget_data(tree, node);
    if (view == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    view->drag_scrolls = drags ? 1 : 0;
    return SCHULTZ_OK;
}

int32_t schultz_scroll_view_drag_scrolls(const schultz_tree *tree,
                                         schultz_handle node)
{
    const schultz_scroll_view_data *view;

    if (schultz_node_widget(tree, node) != &schultz_scroll_view_widget) {
        return 0;
    }
    view = (const schultz_scroll_view_data *)
        schultz_node_widget_data(tree, node);
    return (view == NULL) ? 0 : view->drag_scrolls;
}

int32_t schultz_scroll_view_shows_bars(const schultz_tree *tree,
                                       schultz_handle node)
{
    const schultz_scroll_view_data *view;

    if (schultz_node_widget(tree, node) != &schultz_scroll_view_widget) {
        return 0;
    }
    view = (const schultz_scroll_view_data *)
        schultz_node_widget_data(tree, node);
    return (view == NULL) ? 0 : view->bars;
}

int32_t schultz_scroll_view_scroll_to(schultz_tree *tree, schultz_handle node,
                                      schultz_point offset)
{
    schultz_scroll_view_data *view;

    if (schultz_node_widget(tree, node) != &schultz_scroll_view_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    view = (schultz_scroll_view_data *)schultz_node_widget_data(tree, node);
    schultz_scroll_bar_set_value(tree, view->bar_x, offset.x);
    schultz_scroll_bar_set_value(tree, view->bar_y, offset.y);
    schultz_scroll_apply(tree, view);
    return SCHULTZ_OK;
}

/* ------------------------------------------------- TextField, TextArea */

/** How many edits can be taken back. */
enum { SCHULTZ_UNDO_DEPTH = 64 };

/** How many lines of room a text area asks for before it is filled. */
#define SCHULTZ_TEXT_AREA_LINES 3u

/** Natural width of a text field, before any size hint. */
#define SCHULTZ_FIELD_WIDTH 180.0f
/** How wide the caret is drawn. */
#define SCHULTZ_CARET_WIDTH 1.5f

/** One point the text can be taken back to. */
typedef struct {
    char    *text;  /**< Owned copy of the whole buffer. */
    uint32_t caret; /**< Where the caret was. */
    /**
     * The spans as they stood, owned. Taking back an edit has to take back
     * how the words looked as well as what they said: undoing the deletion
     * of a bold phrase that gave back plain text would be a different
     * document from the one that was there.
     */
    schultz_span *spans;
    uint32_t      span_count;
} schultz_edit_state;

/** Everything an editable text widget holds. */
typedef struct {
    char    *text;        /**< Owned, NUL terminated. Never NULL. */
    uint32_t length;      /**< Bytes, not counting the terminator. */
    uint32_t capacity;    /**< Bytes allocated, including the terminator. */
    uint32_t caret;       /**< Byte offset the caret sits at. */
    uint32_t anchor;      /**< Other end of the selection. */
    char    *composition; /**< In progress IME text, or NULL. */
    uint32_t multiline;   /**< Nonzero for a text area. */
    uint32_t lines;       /**< How many lines of height an area asks for. */
    uint32_t grows;       /**< Nonzero when its height follows its text. */
    float    scroll_x;    /**< Shift that keeps the caret in view. */
    float    scroll_y;    /**< The same, down the page. */

    schultz_edit_state undo[SCHULTZ_UNDO_DEPTH]; /**< Ring of snapshots. */
    uint32_t undo_count;  /**< How many snapshots are stored. */
    uint32_t undo_at;     /**< Where in them the text currently is. */
    uint32_t undo_typing; /**< Set while a run of typing is being merged. */

    uint32_t mask;        /**< Mask codepoint, or 0 when nothing is masked. */
    char    *masked;      /**< The masked string, rebuilt on demand. */
    uint32_t masked_len;  /**< Its length in bytes. */

    /** What kind of text this holds, which decides what an on-screen
     *  keyboard offers it. One of the SCHULTZ_INPUT_* values. */
    uint32_t input_type;

    /*
     * Rich text. Which stretches of the buffer are not set the way the
     * widget's style says. NULL and zero mean the text is all one face in one
     * colour, which is what nearly every field is.
     */
    schultz_span *spans;
    uint32_t      span_count;
    /**
     * The look the next thing typed should take, and whether one was asked
     * for.
     *
     * Without it, typing takes after the character before the caret, which is
     * what continues a bold word when more is added to the end of it. With
     * it, a program can say that what comes next is bold before there is any
     * of it to mark, which is what a Bold button pressed with nothing
     * selected has to mean.
     *
     * Moving the caret forgets it, because a look asked for in one place was
     * not asked for in another.
     */
    schultz_span  typing;
    uint32_t      typing_set;
} schultz_text_data;

static const schultz_widget_vtable schultz_text_field_widget;
static const schultz_widget_vtable schultz_text_area_widget;

/* Nonzero when the node is a text field or a text area. */
static int32_t schultz_is_text(const schultz_tree *tree, schultz_handle node)
{
    const schultz_widget_vtable *widget = schultz_node_widget(tree, node);

    return (widget == &schultz_text_field_widget ||
            widget == &schultz_text_area_widget);
}

static schultz_text_data *schultz_text_of(schultz_tree *tree,
                                          schultz_handle node)
{
    if (!schultz_is_text(tree, node)) {
        return NULL;
    }
    return (schultz_text_data *)schultz_node_widget_data(tree, node);
}

/* --------------------------------------------------------- UTF-8 walking */

/*
 * A byte in the middle of a character has its top two bits set to 10. Moving
 * the caret means stepping over whole characters, never bytes, or the text
 * would be split into pieces no font can draw.
 */
static int32_t schultz_utf8_tail(char byte)
{
    return ((unsigned char)byte & 0xC0u) == 0x80u;
}

static uint32_t schultz_utf8_next(const char *text, uint32_t length,
                                  uint32_t at)
{
    if (at >= length) {
        return length;
    }
    at++;
    while (at < length && schultz_utf8_tail(text[at])) {
        at++;
    }
    return at;
}

static uint32_t schultz_utf8_prev(const char *text, uint32_t at)
{
    if (at == 0u) {
        return 0u;
    }
    at--;
    while (at > 0u && schultz_utf8_tail(text[at])) {
        at--;
    }
    return at;
}

/* ------------------------------------------------------------- masking */

/*
 * A password field draws one mask character for each character it holds. The
 * two strings do not agree on byte offsets, since a character of any length
 * becomes one mask character, so everything that draws or measures works from
 * a view of the data with the text, the caret and the selection already
 * translated. Doing it once here beats translating at every use, and it means
 * the drawing code never learns that masking exists.
 */

/* Writes one codepoint as UTF-8. Returns how many bytes it took. */
static uint32_t schultz_utf8_encode(uint32_t codepoint, char *out)
{
    if (codepoint < 0x80u) {
        out[0] = (char)codepoint;
        return 1u;
    }
    if (codepoint < 0x800u) {
        out[0] = (char)(0xc0u | (codepoint >> 6));
        out[1] = (char)(0x80u | (codepoint & 0x3fu));
        return 2u;
    }
    if (codepoint < 0x10000u) {
        out[0] = (char)(0xe0u | (codepoint >> 12));
        out[1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        out[2] = (char)(0x80u | (codepoint & 0x3fu));
        return 3u;
    }
    out[0] = (char)(0xf0u | (codepoint >> 18));
    out[1] = (char)(0x80u | ((codepoint >> 12) & 0x3fu));
    out[2] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
    out[3] = (char)(0x80u | (codepoint & 0x3fu));
    return 4u;
}

/* How many characters sit before a byte offset. */
static uint32_t schultz_text_char_index(const schultz_text_data *data,
                                        uint32_t offset)
{
    uint32_t at = 0;
    uint32_t count = 0;

    while (at < offset && at < data->length) {
        at = schultz_utf8_next(data->text, data->length, at);
        count++;
    }
    return count;
}

/* The byte offset that many characters into the real text. */
static uint32_t schultz_text_offset_of_char(const schultz_text_data *data,
                                            uint32_t index)
{
    uint32_t at = 0;
    uint32_t count = 0;

    while (count < index && at < data->length) {
        at = schultz_utf8_next(data->text, data->length, at);
        count++;
    }
    return at;
}

/* Rebuilds the masked string. Cheap: a password is a handful of characters. */
static int32_t schultz_text_mask_rebuild(schultz_text_data *data)
{
    char unit[4];
    uint32_t unit_len = schultz_utf8_encode(data->mask, unit);
    uint32_t chars = schultz_text_char_index(data, data->length);
    uint32_t needed = chars * unit_len + 1u;
    uint32_t i;

    free(data->masked);
    data->masked = (char *)malloc(needed);
    if (data->masked == NULL) {
        data->masked_len = 0;
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    for (i = 0; i < chars; i++) {
        memcpy(data->masked + (size_t)i * unit_len, unit, unit_len);
    }
    data->masked[chars * unit_len] = '\0';
    data->masked_len = chars * unit_len;
    return SCHULTZ_OK;
}

/*
 * The data as it should be drawn. Unmasked, this is the data itself. Masked,
 * it is a copy pointing at the mask string with the offsets translated, and
 * it is only ever read.
 */
static const schultz_text_data *schultz_text_shown(
    const schultz_text_data *data, schultz_text_data *view)
{
    char unit[4];
    uint32_t unit_len;

    if (data->mask == 0u) {
        return data;
    }
    /*
     * The mask buffer is a cache derived from the text rather than part of
     * the value, so filling it in does not change what the data means. That
     * is what lets measuring and hit testing, which are both read only, ask
     * for a masked view.
     */
    if (schultz_text_mask_rebuild((schultz_text_data *)data) != SCHULTZ_OK) {
        return data;
    }
    unit_len = schultz_utf8_encode(data->mask, unit);
    *view = *data;
    view->text   = data->masked;
    view->length = data->masked_len;
    view->caret  = schultz_text_char_index(data, data->caret) * unit_len;
    view->anchor = schultz_text_char_index(data, data->anchor) * unit_len;
    /* A composition is in progress text and is never masked, because it is
     * not committed yet and the platform is already showing it. */
    view->composition = NULL;
    return view;
}

/* Turns an offset in the masked string back into one in the real text. */
static uint32_t schultz_text_unshown_offset(const schultz_text_data *data,
                                            uint32_t shown)
{
    char unit[4];
    uint32_t unit_len;

    if (data->mask == 0u) {
        return shown;
    }
    unit_len = schultz_utf8_encode(data->mask, unit);
    return schultz_text_offset_of_char(data, shown / unit_len);
}

/* Word movement treats anything that is not a space or punctuation as part
 * of a word, so every non-ASCII byte counts as a word character. */
static int32_t schultz_word_byte(char byte)
{
    unsigned char c = (unsigned char)byte;

    if (c >= 0x80u) {
        return 1;
    }
    return (c == '_' || (c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) ? 1 : 0;
}

static uint32_t schultz_word_next(const char *text, uint32_t length,
                                  uint32_t at)
{
    while (at < length && !schultz_word_byte(text[at])) {
        at = schultz_utf8_next(text, length, at);
    }
    while (at < length && schultz_word_byte(text[at])) {
        at = schultz_utf8_next(text, length, at);
    }
    return at;
}

static uint32_t schultz_word_prev(const char *text, uint32_t at)
{
    while (at > 0u && !schultz_word_byte(text[schultz_utf8_prev(text, at)])) {
        at = schultz_utf8_prev(text, at);
    }
    while (at > 0u && schultz_word_byte(text[schultz_utf8_prev(text, at)])) {
        at = schultz_utf8_prev(text, at);
    }
    return at;
}

/* Start of the hard line the offset is on, and one past its last byte. */
static uint32_t schultz_line_start(const char *text, uint32_t at)
{
    while (at > 0u && text[at - 1u] != '\n') {
        at--;
    }
    return at;
}

static uint32_t schultz_line_end(const char *text, uint32_t length,
                                 uint32_t at)
{
    while (at < length && text[at] != '\n') {
        at++;
    }
    return at;
}

/* ------------------------------------------------------- buffer handling */

static uint32_t schultz_sel_low(const schultz_text_data *data)
{
    return (data->caret < data->anchor) ? data->caret : data->anchor;
}

static uint32_t schultz_sel_high(const schultz_text_data *data)
{
    return (data->caret > data->anchor) ? data->caret : data->anchor;
}

static int32_t schultz_text_reserve(schultz_text_data *data, uint32_t needed)
{
    uint32_t capacity = (data->capacity == 0u) ? 32u : data->capacity;
    char *grown;

    if (needed + 1u <= data->capacity) {
        return SCHULTZ_OK;
    }
    while (capacity < needed + 1u) {
        capacity *= 2u;
    }
    grown = (char *)realloc(data->text, capacity);
    if (grown == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->text     = grown;
    data->capacity = capacity;
    return SCHULTZ_OK;
}

/* Frees one snapshot and clears it, so a slot can be reused safely. */
static void schultz_undo_clear(schultz_edit_state *state)
{
    free(state->text);
    state->text  = NULL;
    state->caret = 0u;
    schultz_spans_free(&state->spans, &state->span_count);
}

/*
 * Moves every span over an edit that replaced [start, end) with `added`
 * bytes, and merges what is left.
 *
 * One offset at a time. Everything before the change stays put; everything
 * after it moves by as much as the text grew or shrank; anything inside the
 * part that went collapses onto where it began.
 *
 * The one place that is not obvious is the edge of an insertion, where a
 * span ends exactly where the text was typed. It grows to cover it, which is
 * what continues a bold word when more is added to the end of it. A span that
 * begins there moves along instead, because text typed in front of a phrase
 * is not part of it. Both come out as "an offset at the caret moves with the
 * insertion", which is why they are one line rather than two cases.
 */
static uint32_t schultz_span_moved(uint32_t at, uint32_t start, uint32_t end,
                                   uint32_t added)
{
    uint32_t removed = end - start;

    if (at < start) {
        return at;
    }
    if (at == start) {
        return (removed == 0u) ? (at + added) : at;
    }
    if (at >= end) {
        return at + added - removed;
    }
    return start;
}

/*
 * Gives [from, to) a look of its own, keeping whatever lay outside it.
 *
 * This is what a Bold button does to a selection, and what typing does when a
 * look was asked for before there was any text to put it on. A span the range
 * lands in the middle of is split in two, so the list can grow by one entry
 * per span plus one for the new look.
 *
 * `look` may be NULL, which strips the range back to the widget's own style
 * rather than giving it another.
 */
static int32_t schultz_spans_set_range(schultz_span **spans, uint32_t *count,
                                       uint32_t from, uint32_t to,
                                       const schultz_span *look)
{
    schultz_span *out;
    uint32_t room = (*count * 2u) + 1u;
    uint32_t kept = 0u;
    uint32_t i;

    if (from >= to) {
        return SCHULTZ_OK;
    }
    out = (schultz_span *)calloc(room, sizeof(*out));
    if (out == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    for (i = 0u; i < *count; i++) {
        const schultz_span *old = &(*spans)[i];
        int32_t used = 0;

        /* Whatever of it lies before the range, and whatever lies after. */
        if (old->start < from) {
            out[kept] = *old;
            out[kept].end = (old->end < from) ? old->end : from;
            out[kept].link = NULL;
            if (old->link != NULL) {
                out[kept].link = schultz_span_copy_link(old->link);
            }
            kept++;
            used = 1;
        }
        if (old->end > to) {
            out[kept] = *old;
            out[kept].start = (old->start > to) ? old->start : to;
            out[kept].link = NULL;
            if (old->link != NULL) {
                out[kept].link = schultz_span_copy_link(old->link);
            }
            kept++;
            used = 1;
        }
        (void)used;
    }

    if (look != NULL) {
        out[kept] = *look;
        out[kept].start = from;
        out[kept].end   = to;
        out[kept].link  = (look->link != NULL)
                              ? schultz_span_copy_link(look->link) : NULL;
        kept++;
    }

    /*
     * Back into order. The pieces come out grouped by the span they were cut
     * from and the new look is last, and everything that reads a span list
     * takes the first that covers a byte.
     */
    for (i = 1u; i < kept; i++) {
        schultz_span hold = out[i];
        uint32_t j = i;

        while (j > 0u && out[j - 1u].start > hold.start) {
            out[j] = out[j - 1u];
            j--;
        }
        out[j] = hold;
    }

    schultz_spans_free(spans, count);
    if (kept == 0u) {
        free(out);
        return SCHULTZ_OK;
    }
    *spans = out;
    *count = kept;
    return SCHULTZ_OK;
}

/*
 * Merges neighbours that say the same thing, and drops what covers nothing.
 *
 * Editing splits spans and shifts them about, so without this a paragraph
 * typed one character at a time would end up with one span per character,
 * all of them alike, and the list would grow for as long as the session
 * lasted.
 */
static void schultz_spans_tidy(schultz_span *spans, uint32_t *count)
{
    uint32_t kept = 0u;
    uint32_t i;

    for (i = 0u; i < *count; i++) {
        schultz_span *last;

        if (spans[i].start >= spans[i].end) {
            free((void *)(uintptr_t)spans[i].link);
            continue;
        }
        last = (kept > 0u) ? &spans[kept - 1u] : NULL;
        /*
         * Touching, and alike in every way a reader could tell apart. The
         * tag is part of that: two stretches a program gave different tags
         * are two things to it however alike they look.
         */
        if (last != NULL && last->end == spans[i].start &&
            last->font == spans[i].font && last->size == spans[i].size &&
            last->bold == spans[i].bold && last->italic == spans[i].italic &&
            last->underline == spans[i].underline &&
            last->strikethrough == spans[i].strikethrough &&
            last->clickable == spans[i].clickable &&
            last->tag == spans[i].tag &&
            schultz_color_equals(last->color, spans[i].color) &&
            schultz_color_equals(last->background, spans[i].background) &&
            ((last->link == NULL && spans[i].link == NULL) ||
             (last->link != NULL && spans[i].link != NULL &&
              strcmp(last->link, spans[i].link) == 0))) {
            last->end = spans[i].end;
            free((void *)(uintptr_t)spans[i].link);
            continue;
        }
        if (kept != i) {
            spans[kept] = spans[i];
        }
        kept++;
    }
    *count = kept;
}

/*
 * Takes a snapshot of the text before it changes. Anything that was undone
 * is dropped, because editing after an undo is a new branch of history and
 * the old redo path can no longer be reached.
 */
static void schultz_undo_push(schultz_text_data *data)
{
    uint32_t i;
    char *copy;

    for (i = data->undo_at; i < data->undo_count; i++) {
        schultz_undo_clear(&data->undo[i]);
    }
    data->undo_count = data->undo_at;

    if (data->undo_count == SCHULTZ_UNDO_DEPTH) {
        /* Drop the oldest. A bounded history is what keeps a long session
         * from growing without limit. */
        schultz_undo_clear(&data->undo[0]);
        memmove(&data->undo[0], &data->undo[1],
                sizeof(data->undo[0]) * (SCHULTZ_UNDO_DEPTH - 1u));
        data->undo_count--;
        data->undo[data->undo_count].text = NULL;
    }

    copy = (char *)malloc(data->length + 1u);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, data->text, data->length + 1u);
    data->undo[data->undo_count].text  = copy;
    data->undo[data->undo_count].caret = data->caret;
    data->undo[data->undo_count].spans      = NULL;
    data->undo[data->undo_count].span_count = 0u;
    /*
     * The spans too. A snapshot that gave back the words without the way they
     * looked would restore a different document from the one that was there.
     * Out of memory here costs the look of one step back, not the text.
     */
    (void)schultz_spans_store(&data->undo[data->undo_count].spans,
                              &data->undo[data->undo_count].span_count,
                              data->spans, data->span_count, data->length);
    data->undo_count++;
    data->undo_at = data->undo_count;
}

/* Keeps the accessibility value in step with what is on screen. */
static void schultz_text_publish(schultz_tree *tree, schultz_handle node,
                                 const schultz_text_data *data)
{
    schultz_node_set_value(tree, node, data->text);
}

/*
 * Replaces a byte range with new text. Every edit goes through here, so the
 * caret, the selection, the undo history and the invalidation are handled in
 * one place rather than in each key.
 */
static int32_t schultz_text_replace(schultz_tree *tree, schultz_handle node,
                                    schultz_text_data *data, uint32_t start,
                                    uint32_t end, const char *insert,
                                    uint32_t insert_length, int32_t typing)
{
    uint32_t tail;
    int32_t result;

    if (start > data->length) { start = data->length; }
    if (end > data->length)   { end = data->length; }
    if (end < start)          { end = start; }
    if (start == end && insert_length == 0u) {
        return SCHULTZ_OK;
    }

    /* A run of typing is one undo step; anything else starts a new one. */
    if (!typing || !data->undo_typing) {
        schultz_undo_push(data);
    }
    data->undo_typing = typing ? 1u : 0u;

    result = schultz_text_reserve(data, data->length - (end - start) +
                                        insert_length);
    if (result != SCHULTZ_OK) {
        return result;
    }
    tail = data->length - end;
    memmove(data->text + start + insert_length, data->text + end, tail + 1u);
    if (insert_length > 0u) {
        memcpy(data->text + start, insert, insert_length);
    }
    data->length = start + insert_length + tail;
    data->text[data->length] = '\0';
    data->caret  = start + insert_length;
    data->anchor = data->caret;

    /*
     * Every edit in this widget arrives here, so the spans are moved here and
     * nowhere else. Typing, pasting, deleting and replacing a selection are
     * all the same shape once they reach this line.
     */
    if (data->span_count > 0u) {
        uint32_t i;

        for (i = 0u; i < data->span_count; i++) {
            uint32_t from = schultz_span_moved(data->spans[i].start, start,
                                               end, insert_length);
            uint32_t to   = schultz_span_moved(data->spans[i].end, start, end,
                                               insert_length);

            data->spans[i].start = from;
            data->spans[i].end   = to;
        }
        schultz_spans_tidy(data->spans, &data->span_count);
    }
    /*
     * A look asked for before there was anything to put it on. The shift
     * above will have let the span before the caret grow over what was
     * typed, which is right when nothing was asked for and wrong when
     * something was, so the range is cut back out and given its own.
     */
    if (data->typing_set && insert_length > 0u) {
        (void)schultz_spans_set_range(&data->spans, &data->span_count, start,
                                      start + insert_length, &data->typing);
        schultz_spans_tidy(data->spans, &data->span_count);
    }

    schultz_text_publish(tree, node, data);
    if (data->multiline) {
        /* A line more or fewer changes how tall the content is. */
        schultz_node_invalidate_layout(tree, node);
    }
    return schultz_node_invalidate(tree, node);
}

/* Moves the text to a stored snapshot, which is what undo and redo both do. */
static int32_t schultz_text_restore(schultz_tree *tree, schultz_handle node,
                                    schultz_text_data *data,
                                    const schultz_edit_state *state)
{
    uint32_t length = (uint32_t)strlen(state->text);

    if (schultz_text_reserve(data, length) != SCHULTZ_OK) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    memcpy(data->text, state->text, length + 1u);
    data->length = length;
    data->caret  = (state->caret > length) ? length : state->caret;
    data->anchor = data->caret;
    data->undo_typing = 0u;
    data->typing_set  = 0u;
    (void)schultz_spans_store(&data->spans, &data->span_count, state->spans,
                              state->span_count, length);

    schultz_text_publish(tree, node, data);
    if (data->multiline) {
        schultz_node_invalidate_layout(tree, node);
    }
    return schultz_node_invalidate(tree, node);
}

static int32_t schultz_text_undo(schultz_tree *tree, schultz_handle node,
                                 schultz_text_data *data)
{
    schultz_edit_state current;

    if (data->undo_at == 0u) {
        return SCHULTZ_OK;
    }
    /*
     * Stepping back off the end has to leave the present behind first, or
     * there would be nothing to redo forward into.
     */
    if (data->undo_at == data->undo_count) {
        current.text = (char *)malloc(data->length + 1u);
        if (current.text == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        memcpy(current.text, data->text, data->length + 1u);
        current.caret = data->caret;
        if (data->undo_count == SCHULTZ_UNDO_DEPTH) {
            free(current.text);
            return SCHULTZ_OK;
        }
        data->undo[data->undo_count] = current;
        data->undo_count++;
    }
    data->undo_at--;
    return schultz_text_restore(tree, node, data, &data->undo[data->undo_at]);
}

static int32_t schultz_text_redo(schultz_tree *tree, schultz_handle node,
                                 schultz_text_data *data)
{
    if (data->undo_at + 1u >= data->undo_count) {
        return SCHULTZ_OK;
    }
    data->undo_at++;
    return schultz_text_restore(tree, node, data, &data->undo[data->undo_at]);
}

/* --------------------------------------------------------- measuring text */

/* Where the content sits inside the node, once padding and border are off. */
/*
 * The faces this widget's spans ask for, or none.
 *
 * None when the text is masked: a password field shows one character over and
 * over, and the spans describe the text underneath, which is not what is on
 * the screen. Marking the dots would say something about the secret.
 */
static uint32_t schultz_text_widget_pieces(schultz_font_system *fonts,
                                           const schultz_text_data *data,
                                           const schultz_text_data *shown,
                                           schultz_handle font,
                                           schultz_arena *arena,
                                           schultz_text_piece **out_pieces)
{
    *out_pieces = NULL;
    if (shown != data) {
        return 0u;
    }
    return schultz_spans_pieces(fonts, data->spans, data->span_count, font,
                                arena, out_pieces);
}

static schultz_rect schultz_text_content(const schultz_resolved_style *style,
                                         schultz_rect bounds)
{
    float inset = schultz_resolved_number(style, SCHULTZ_PROP_PADDING) +
                  schultz_resolved_number(style, SCHULTZ_PROP_BORDER_WIDTH);

    return schultz_rect_expand(bounds, -inset);
}

static schultz_handle schultz_text_font(schultz_tree *tree,
                                        schultz_handle node)
{
    return schultz_widget_font(tree, node);
}

/*
 * A field asks for a sensible width and one line; an area asks for the room
 * its text needs at the width it is offered. Either can be overruled by a
 * size hint, which is how an application makes a field fill a row.
 */
static int32_t schultz_editor_measure(schultz_tree *tree,
                                      schultz_handle node, float avail_w,
                                      float avail_h, schultz_size *out_size)
{
    const schultz_text_data *data =
        (const schultz_text_data *)schultz_node_widget_data(tree, node);
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_handle font = schultz_text_font(tree, node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_font_metrics metrics;
    float inset;

    (void)avail_h;
    *out_size = schultz_size_make(0.0f, 0.0f);
    if (data == NULL || fonts == NULL || font == SCHULTZ_HANDLE_NONE ||
        schultz_font_get_metrics(fonts, font, &metrics) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    inset = (schultz_resolved_number(style, SCHULTZ_PROP_PADDING) +
             schultz_resolved_number(style, SCHULTZ_PROP_BORDER_WIDTH)) * 2.0f;

    if (!data->multiline) {
        *out_size = schultz_size_make(SCHULTZ_FIELD_WIDTH,
                                      metrics.line_height + inset);
        return SCHULTZ_OK;
    }

    {
        schultz_arena scratch;
        const schultz_text_line *lines;
        uint32_t count = 0;
        float width = (avail_w < 0.0f) ? -1.0f : avail_w - inset;

        if (schultz_arena_init(&scratch, 0) != SCHULTZ_OK) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        schultz_text_data mask_view;
        const schultz_text_data *shown = schultz_text_shown(data,
                                                            &mask_view);

        schultz_text_piece *pieces = NULL;
        uint32_t piece_count = schultz_text_widget_pieces(fonts, data, shown,
                                                          font, &scratch,
                                                          &pieces);

        if (schultz_text_wrap_pieces(fonts, pieces, piece_count, font,
                                     shown->text, (int32_t)shown->length,
                                     width, &scratch, &lines,
                                     &count) == SCHULTZ_OK) {
            /*
             * A box that got taller with every line typed would push whatever
             * is under it down the screen, so an area is the height it asked
             * for and its text scrolls inside it. An area that was told to
             * grow follows its text instead, and never shrinks below the
             * height it asked for.
             */
            if (!data->grows || count < data->lines) {
                count = data->lines;
            }
            *out_size = schultz_size_make(
                (avail_w < 0.0f) ? SCHULTZ_FIELD_WIDTH : avail_w,
                metrics.line_height * (float)count + inset);
        }
        schultz_arena_free(&scratch);
    }
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_text_pane = {
    schultz_editor_measure, schultz_leaf_arrange
};

/* ---------------------------------------------------------- painting text */

/*
 * Draws one line from a run the caller already shaped: its share of the
 * selection, its glyphs, and the caret when the caret is on it. The run is
 * passed in rather than shaped here because the caller needs it first, to
 * work out how far the text has to be shifted to keep the caret in view.
 */
/*
 * Where a byte range starts and ends along one shaped line, in absolute
 * coordinates. A span's background and the lines under and through it want
 * the same two numbers the selection band works out, so it is written once.
 */
static int32_t schultz_text_span_edges(const schultz_text_run *run, float x,
                                       uint32_t start, uint32_t end,
                                       uint32_t low, uint32_t high,
                                       float *out_left, float *out_right)
{
    float a;
    float b;

    if (high <= start || low >= end || run->count == 0u) {
        return 0;
    }
    a = x + schultz_text_caret_x(run, (low > start) ? (low - start) : 0u);
    b = x + schultz_text_caret_x(run, (high < end) ? (high - start)
                                                   : (end - start));
    *out_left  = (a < b) ? a : b;
    *out_right = (a < b) ? b : a;
    return (*out_right > *out_left) ? 1 : 0;
}

/*
 * A line's glyphs in the colours its spans ask for.
 *
 * A draw command names one face and one paint, so this starts a new one
 * wherever either changes.
 */
static void schultz_text_draw_line_glyphs(schultz_draw_list *list,
                                          const schultz_text_data *data,
                                          const schultz_text_run *run,
                                          const schultz_glyph *placed,
                                          schultz_handle font, uint32_t start,
                                          schultz_color base)
{
    uint32_t from = 0u;
    uint32_t i;

    if (run->count == 0u) {
        return;
    }
    for (i = 1u; i <= run->count; i++) {
        uint32_t at = start + run->clusters[from];
        const schultz_span *span = schultz_spans_at(data->spans,
                                                    data->span_count, at);
        schultz_color here = (span != NULL && span->color.a != 0u)
                                 ? span->color : base;
        int32_t split = (i == run->count);

        if (!split) {
            const schultz_span *next =
                schultz_spans_at(data->spans, data->span_count,
                                 start + run->clusters[i]);
            schultz_color then = (next != NULL && next->color.a != 0u)
                                     ? next->color : base;
            schultz_handle a = (run->fonts != NULL) ? run->fonts[i] : font;
            schultz_handle b = (run->fonts != NULL) ? run->fonts[from] : font;

            split = (a != b) || !schultz_color_equals(here, then);
        }
        if (split) {
            schultz_handle face = (run->fonts != NULL) ? run->fonts[from]
                                                       : font;

            schultz_draw_glyph_run(list, face, placed + from, i - from,
                                   schultz_paint_solid(here));
            from = i;
        }
    }
}

static int32_t schultz_text_paint_line(schultz_tree *tree,
                                       schultz_handle node,
                                       schultz_draw_list *list,
                                       schultz_arena *arena,
                                       const schultz_text_data *data,
                                       const schultz_text_run *run,
                                       schultz_handle font, float x, float y,
                                       float height, uint32_t start,
                                       uint32_t end, uint32_t focused)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_font_metrics metrics;
    uint32_t low = schultz_sel_low(data);
    uint32_t high = schultz_sel_high(data);
    schultz_glyph *placed;
    uint32_t i;

    /*
     * Spans describe the text in the buffer, and a masked field is not
     * showing that text, so the dots are left plain. Marking them would say
     * something about the secret.
     */
    uint32_t styled = (data->mask == 0u && data->span_count > 0u) ? 1u : 0u;

    if (schultz_font_get_metrics(fonts, font, &metrics) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }

    /*
     * A span's own background goes down first, then the selection over it.
     * A selection has to be visible over a highlighted phrase, and both have
     * to be behind the words they are marking.
     */
    if (styled) {
        uint32_t i;

        for (i = 0u; i < data->span_count; i++) {
            float left;
            float right;

            if (data->spans[i].background.a == 0u) {
                continue;
            }
            if (!schultz_text_span_edges(run, x, start, end,
                                         data->spans[i].start,
                                         data->spans[i].end, &left, &right)) {
                continue;
            }
            schultz_draw_fill_rect(list,
                schultz_rect_make(left, y, right - left, height),
                schultz_paint_solid(data->spans[i].background));
        }
    }

    /* The selection, as one band from its start to its end on this line. */
    if (high > low && high > start && low <= end) {
        uint32_t from = (low > start) ? low - start : 0u;
        uint32_t to = (high < end) ? high - start : end - start;
        float a = schultz_text_caret_x(run, from);
        float b = schultz_text_caret_x(run, to);

        if (b > a) {
            schultz_draw_fill_rect(list, schultz_rect_make(x + a, y, b - a,
                                                           height),
                schultz_resolved_paint(style, SCHULTZ_PROP_SELECTION_COLOR));
        }
    }

    if (run->count > 0u) {
        placed = (schultz_glyph *)schultz_arena_alloc(
            arena, (size_t)run->count * sizeof(*placed),
            _Alignof(schultz_glyph));
        if (placed == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        for (i = 0; i < run->count; i++) {
            placed[i].glyph_id = run->glyphs[i].glyph_id;
            placed[i].x = run->glyphs[i].x + x;
            placed[i].y = run->glyphs[i].y + y + metrics.ascent;
        }
        if (styled) {
            schultz_text_draw_line_glyphs(list, data, run, placed, font,
                start, schultz_resolved_color(style,
                                              SCHULTZ_PROP_TEXT_COLOR));
        } else {
            schultz_widget_draw_glyphs(list, font, run->fonts, placed,
                run->count,
                schultz_resolved_paint(style, SCHULTZ_PROP_TEXT_COLOR));
        }
    }

    /* And the lines under and through whatever asked for them. */
    if (styled) {
        schultz_color base = schultz_resolved_color(style,
                                                    SCHULTZ_PROP_TEXT_COLOR);
        uint32_t i;

        for (i = 0u; i < data->span_count; i++) {
            const schultz_span *span = &data->spans[i];
            schultz_color ink = (span->color.a != 0u) ? span->color : base;
            float left;
            float right;
            float thick;

            if (!span->underline && !span->strikethrough) {
                continue;
            }
            if (!schultz_text_span_edges(run, x, start, end, span->start,
                                         span->end, &left, &right)) {
                continue;
            }
            if (span->underline) {
                thick = (metrics.underline_thickness < 1.0f)
                            ? 1.0f : metrics.underline_thickness;
                schultz_draw_fill_rect(list,
                    schultz_rect_make(left,
                                      y + metrics.ascent +
                                          metrics.underline_position,
                                      right - left, thick),
                    schultz_paint_solid(ink));
            }
            if (span->strikethrough) {
                thick = (metrics.strikeout_thickness < 1.0f)
                            ? 1.0f : metrics.strikeout_thickness;
                schultz_draw_fill_rect(list,
                    schultz_rect_make(left,
                                      y + metrics.ascent -
                                          metrics.strikeout_position,
                                      right - left, thick),
                    schultz_paint_solid(ink));
            }
        }
    }

    if (data->caret >= start && data->caret <= end) {
        float at = x + schultz_text_caret_x(run, data->caret - start);

        /*
         * An in progress composition is shown at the caret but is not in the
         * buffer, so it is shaped and drawn separately, on a band that marks
         * it as provisional. Committing it arrives as ordinary text input.
         */
        if (data->composition != NULL) {
            schultz_text_run ime;

            if (schultz_text_shape(fonts, font, data->composition, -1,
                                   SCHULTZ_DIR_AUTO, arena, &ime)
                    == SCHULTZ_OK && ime.count > 0u) {
                schultz_glyph *marks = (schultz_glyph *)schultz_arena_alloc(
                    arena, (size_t)ime.count * sizeof(*marks),
                    _Alignof(schultz_glyph));

                if (marks == NULL) {
                    return SCHULTZ_ERR_OUT_OF_MEMORY;
                }
                schultz_draw_fill_rect(list, schultz_rect_make(at, y + height - 2.0f, ime.width, 2.0f),
                    schultz_resolved_paint(style,
                                           SCHULTZ_PROP_SELECTION_COLOR));
                for (i = 0; i < ime.count; i++) {
                    marks[i].glyph_id = ime.glyphs[i].glyph_id;
                    marks[i].x = ime.glyphs[i].x + at;
                    marks[i].y = ime.glyphs[i].y + y + metrics.ascent;
                }
                schultz_widget_draw_glyphs(list, font, ime.fonts, marks,
                    ime.count,
                    schultz_resolved_paint(style, SCHULTZ_PROP_TEXT_COLOR));
                at += ime.width;
            }
        }

        if (focused) {
            schultz_draw_fill_rect(list, schultz_rect_make(at, y, SCHULTZ_CARET_WIDTH, height),
                schultz_resolved_paint(style, SCHULTZ_PROP_TEXT_COLOR));
        }
    }
    return SCHULTZ_OK;
}

/*
 * How far the text has to be shifted for a position to be inside a window of
 * a given length. Returning the shift rather than applying it keeps the same
 * arithmetic serving both axes.
 */
static float schultz_text_reveal(float shift, float at, float length,
                                 float window)
{
    if (at < shift) {
        shift = at;
    } else if (at + length > shift + window) {
        shift = at + length - window;
    }
    return (shift < 0.0f) ? 0.0f : shift;
}

/* How wide the composition adds to the caret, since it sits after it. */
static float schultz_text_composition_width(schultz_tree *tree,
                                            schultz_handle font,
                                            const schultz_text_data *data,
                                            schultz_arena *arena)
{
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_text_run ime;

    if (data->composition == NULL ||
        schultz_text_shape(fonts, font, data->composition, -1,
                           SCHULTZ_DIR_AUTO, arena, &ime) != SCHULTZ_OK) {
        return 0.0f;
    }
    return ime.width;
}

static int32_t schultz_text_paint(schultz_tree *tree, schultz_handle node,
                                  schultz_draw_list *list,
                                  schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_text_data *data =
        (schultz_text_data *)schultz_node_widget_data(tree, node);
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_handle font = schultz_text_font(tree, node);
    uint32_t state = schultz_node_get_state(tree, node);
    uint32_t focused = state & SCHULTZ_STATE_FOCUSED;
    schultz_font_metrics metrics;
    schultz_rect bounds;
    schultz_rect content;
    int32_t result;

    if (data == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    result = schultz_widget_draw_box(list, style, bounds);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (fonts == NULL || font == SCHULTZ_HANDLE_NONE ||
        schultz_font_get_metrics(fonts, font, &metrics) != SCHULTZ_OK) {
        return schultz_widget_draw_focus_ring(list, style, bounds, state);
    }
    content = schultz_text_content(style, bounds);
    if (schultz_rect_is_empty(content)) {
        return schultz_widget_draw_focus_ring(list, style, bounds, state);
    }

    result = schultz_draw_clip_begin(list, content);
    if (result != SCHULTZ_OK) {
        return result;
    }

    if (!data->multiline) {
        schultz_text_run run;
        schultz_text_data mask_view;
        const schultz_text_data *shown = schultz_text_shown(data,
                                                            &mask_view);

        schultz_text_piece *pieces = NULL;
        uint32_t piece_count = schultz_text_widget_pieces(fonts, data, shown,
                                                          font, arena,
                                                          &pieces);

        if (schultz_text_shape_pieces(fonts, pieces, piece_count, font,
                                      shown->text, (int32_t)shown->length,
                                      SCHULTZ_DIR_AUTO, arena,
                                      &run) == SCHULTZ_OK) {
            /*
             * Where the text sits is settled before it is drawn, not after.
             * Correcting it afterwards would leave the frame that is being
             * built showing the old position, so the character just typed
             * would stay off the end until something else forced a repaint.
             *
             * The caret is given the width of the caret itself plus anything
             * an in progress composition is showing, so the whole of what is
             * being typed stays in view rather than its leading edge.
             */
            data->scroll_x = schultz_text_reveal(data->scroll_x,
                schultz_text_caret_x(&run, shown->caret),
                SCHULTZ_CARET_WIDTH +
                    schultz_text_composition_width(tree, font, data, arena),
                content.width);

            schultz_text_paint_line(tree, node, list, arena, shown, &run,
                                    font, content.x - data->scroll_x,
                                    content.y, content.height, 0u,
                                    shown->length, focused);
        }
    } else {
        const schultz_text_line *lines;
        schultz_text_piece *pieces = NULL;
        schultz_text_piece *line_pieces = NULL;
        uint32_t piece_count;
        uint32_t count = 0;
        uint32_t i;

        piece_count = schultz_spans_pieces(fonts, data->spans,
                                           data->span_count, font, arena,
                                           &pieces);
        if (piece_count > 0u) {
            line_pieces = (schultz_text_piece *)schultz_arena_alloc(
                arena, (size_t)piece_count * sizeof(*line_pieces),
                _Alignof(schultz_text_piece));
            if (line_pieces == NULL) {
                piece_count = 0u;
            }
        }
        if (schultz_text_wrap_pieces(fonts, pieces, piece_count, font,
                                     data->text, (int32_t)data->length,
                                     content.width, arena, &lines, &count)
                == SCHULTZ_OK) {
            /* Settle the shift first, for the same reason as above. */
            for (i = 0; i < count; i++) {
                if (data->caret >= lines[i].start &&
                    data->caret <= lines[i].end) {
                    data->scroll_y = schultz_text_reveal(data->scroll_y,
                        metrics.line_height * (float)i, metrics.line_height,
                        content.height);
                    break;
                }
            }

            for (i = 0; i < count; i++) {
                float y = content.y + metrics.line_height * (float)i
                          - data->scroll_y;
                schultz_text_run run;

                if (y + metrics.line_height < content.y ||
                    y > content.y + content.height) {
                    continue; /* off the top or bottom of the box */
                }
                uint32_t on_line = 0u;

                if (line_pieces != NULL) {
                    on_line = schultz_pieces_on_line(pieces, piece_count,
                                                     lines[i].start,
                                                     lines[i].end,
                                                     line_pieces);
                }
                if (schultz_text_shape_pieces(fonts, line_pieces, on_line,
                                              font,
                                              data->text + lines[i].start,
                                              (int32_t)(lines[i].end -
                                                        lines[i].start),
                                              SCHULTZ_DIR_AUTO, arena, &run)
                        != SCHULTZ_OK) {
                    continue;
                }
                schultz_text_paint_line(tree, node, list, arena, data, &run,
                                        font, content.x, y,
                                        metrics.line_height, lines[i].start,
                                        lines[i].end, focused);
            }
        }
    }

    schultz_draw_clip_end(list);
    return schultz_widget_draw_focus_ring(list, style, bounds, state);
}

/* --------------------------------------------------------- editing input */

/* The byte offset under a point, which is what a click sets the caret to. */
static uint32_t schultz_text_offset_at(schultz_tree *tree,
                                       schultz_handle node,
                                       const schultz_text_data *data,
                                       schultz_point local)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_handle font = schultz_text_font(tree, node);
    schultz_rect bounds;
    schultz_rect content;
    schultz_arena scratch;
    schultz_font_metrics metrics;
    uint32_t offset = 0u;

    if (fonts == NULL || font == SCHULTZ_HANDLE_NONE ||
        schultz_font_get_metrics(fonts, font, &metrics) != SCHULTZ_OK ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return data->caret;
    }
    content = schultz_text_content(style, bounds);
    if (schultz_arena_init(&scratch, 0) != SCHULTZ_OK) {
        return data->caret;
    }

    if (!data->multiline) {
        schultz_text_run run;
        schultz_text_data mask_view;
        const schultz_text_data *shown = schultz_text_shown(data,
                                                            &mask_view);
        float x = local.x - (content.x - bounds.x) + data->scroll_x;
        schultz_text_piece *pieces = NULL;
        uint32_t piece_count = schultz_text_widget_pieces(fonts, data, shown,
                                                          font, &scratch,
                                                          &pieces);

        if (schultz_text_shape_pieces(fonts, pieces, piece_count, font,
                                      shown->text, (int32_t)shown->length,
                                      SCHULTZ_DIR_AUTO, &scratch,
                                      &run) == SCHULTZ_OK) {
            offset = schultz_text_unshown_offset(data,
                schultz_text_caret_offset(&run, x, shown->length));
        }
    } else {
        const schultz_text_line *lines;
        schultz_text_piece *pieces = NULL;
        schultz_text_piece *line_pieces = NULL;
        uint32_t piece_count;
        uint32_t count = 0;
        float y = local.y - (content.y - bounds.y) + data->scroll_y;
        uint32_t index;

        piece_count = schultz_spans_pieces(fonts, data->spans,
                                           data->span_count, font, &scratch,
                                           &pieces);
        if (piece_count > 0u) {
            line_pieces = (schultz_text_piece *)schultz_arena_alloc(
                &scratch, (size_t)piece_count * sizeof(*line_pieces),
                _Alignof(schultz_text_piece));
            if (line_pieces == NULL) {
                piece_count = 0u;
            }
        }
        if (schultz_text_wrap_pieces(fonts, pieces, piece_count, font,
                                     data->text, (int32_t)data->length,
                                     content.width, &scratch, &lines, &count)
                == SCHULTZ_OK && count > 0u) {
            schultz_text_run run;
            uint32_t on_line = 0u;
            float row = y / metrics.line_height;

            index = (row < 0.0f) ? 0u : (uint32_t)row;
            if (index >= count) {
                index = count - 1u;
            }
            offset = lines[index].start;
            if (line_pieces != NULL) {
                on_line = schultz_pieces_on_line(pieces, piece_count,
                                                 lines[index].start,
                                                 lines[index].end,
                                                 line_pieces);
            }
            if (schultz_text_shape_pieces(fonts, line_pieces, on_line, font,
                                          data->text + lines[index].start,
                                          (int32_t)(lines[index].end -
                                                    lines[index].start),
                                          SCHULTZ_DIR_AUTO, &scratch, &run)
                    == SCHULTZ_OK) {
                offset = lines[index].start + schultz_text_caret_offset(&run,
                    local.x - (content.x - bounds.x),
                    lines[index].end - lines[index].start);
            }
        }
    }

    schultz_arena_free(&scratch);
    return offset;
}

/* Moves the caret, extending or collapsing the selection as shift says. */
static int32_t schultz_text_move(schultz_tree *tree, schultz_handle node,
                                 schultz_text_data *data, uint32_t to,
                                 uint32_t extend)
{
    data->caret = (to > data->length) ? data->length : to;
    if (!extend) {
        data->anchor = data->caret;
    }
    data->undo_typing = 0u;
    /*
     * A look asked for at one place was not asked for at another. Pressing
     * Bold and then moving the caret away means the bold was thought better
     * of, which is what every editor does with it.
     */
    if (data->typing_set) {
        free((void *)(uintptr_t)data->typing.link);
        memset(&data->typing, 0, sizeof(data->typing));
        data->typing_set = 0u;
    }
    return schultz_node_invalidate(tree, node);
}

/* Which line of wrapped text an offset is on, and where it starts. */
static int32_t schultz_text_line_at(schultz_tree *tree, schultz_handle node,
                                    const schultz_text_data *data,
                                    uint32_t offset, int32_t step,
                                    uint32_t *out_offset)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_handle font = schultz_text_font(tree, node);
    schultz_rect bounds;
    schultz_rect content;
    schultz_arena scratch;
    const schultz_text_line *lines;
    uint32_t count = 0;
    uint32_t i;
    int32_t moved = 0;

    if (fonts == NULL || font == SCHULTZ_HANDLE_NONE ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        schultz_arena_init(&scratch, 0) != SCHULTZ_OK) {
        return 0;
    }
    content = schultz_text_content(style, bounds);

    if (schultz_text_wrap(fonts, font, data->text, (int32_t)data->length,
                          content.width, &scratch, &lines, &count)
            == SCHULTZ_OK && count > 0u) {
        for (i = 0; i < count; i++) {
            if (offset >= lines[i].start && offset <= lines[i].end) {
                break;
            }
        }
        if (i == count) {
            i = count - 1u;
        }
        /* Keep the column, which is what up and down are expected to do. */
        if (step < 0 && i > 0u) {
            uint32_t column = offset - lines[i].start;
            uint32_t width = lines[i - 1u].end - lines[i - 1u].start;

            *out_offset = lines[i - 1u].start +
                          ((column < width) ? column : width);
            moved = 1;
        } else if (step > 0 && i + 1u < count) {
            uint32_t column = offset - lines[i].start;
            uint32_t width = lines[i + 1u].end - lines[i + 1u].start;

            *out_offset = lines[i + 1u].start +
                          ((column < width) ? column : width);
            moved = 1;
        }
    }
    schultz_arena_free(&scratch);
    return moved;
}

/* Puts the selection on the clipboard, and removes it when cutting. */
static int32_t schultz_text_copy(schultz_tree *tree, schultz_handle node,
                                 schultz_text_data *data, int32_t cut)
{
    uint32_t low = schultz_sel_low(data);
    uint32_t high = schultz_sel_high(data);
    char *piece;

    if (high == low) {
        return SCHULTZ_OK;
    }
    /*
     * A masked field never puts its text on the clipboard. Cut still removes
     * the selection, because refusing to edit would be a different and more
     * surprising rule than refusing to copy.
     */
    if (data->mask != 0u) {
        if (cut) {
            return schultz_text_replace(tree, node, data, low, high, NULL,
                                        0u, 0);
        }
        return SCHULTZ_OK;
    }
    piece = (char *)malloc(high - low + 1u);
    if (piece == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    memcpy(piece, data->text + low, high - low);
    piece[high - low] = '\0';
    schultz_tree_clipboard_write(tree, piece);
    free(piece);

    if (cut) {
        return schultz_text_replace(tree, node, data, low, high, NULL, 0u, 0);
    }
    return SCHULTZ_OK;
}

static int32_t schultz_text_paste(schultz_tree *tree, schultz_handle node,
                                  schultz_text_data *data)
{
    const char *pasted = schultz_tree_clipboard_read(tree);

    if (pasted == NULL) {
        return SCHULTZ_OK;
    }
    return schultz_text_replace(tree, node, data, schultz_sel_low(data),
                                schultz_sel_high(data), pasted,
                                (uint32_t)strlen(pasted), 0);
}

/* Backspace and delete: remove the selection, or one character either way. */
/*
 * How much one press of a delete key takes.
 *
 * A whole character as a person sees it, which is what an editor does and
 * what keeps a family emoji from being broken into people. The exception is
 * a platform that has no keys to report and spells a deletion out one
 * codepoint at a time; there, taking a whole character for each of those
 * would take several. See one_codepoint on schultz_event.
 */
static int32_t schultz_text_erase(schultz_tree *tree, schultz_handle node,
                                  schultz_text_data *data, int32_t forward,
                                  uint32_t by_word, uint32_t by_codepoint)
{
    uint32_t low = schultz_sel_low(data);
    uint32_t high = schultz_sel_high(data);

    if (high == low) {
        if (forward) {
            high = by_word
                ? schultz_word_next(data->text, data->length, low)
                : (by_codepoint
                       ? schultz_utf8_next(data->text, data->length, low)
                       : schultz_text_next_cluster(data->text, data->length,
                                                   low));
        } else {
            low = by_word
                ? schultz_word_prev(data->text, high)
                : (by_codepoint
                       ? schultz_utf8_prev(data->text, high)
                       : schultz_text_prev_cluster(data->text, data->length,
                                                   high));
        }
    }
    return schultz_text_replace(tree, node, data, low, high, NULL, 0u, 0);
}

static int32_t schultz_text_key(schultz_tree *tree, schultz_handle node,
                                schultz_text_data *data,
                                const schultz_event *event)
{
    uint32_t shift = (event->modifiers & SCHULTZ_MOD_SHIFT) ? 1u : 0u;
    uint32_t word = (event->modifiers & SCHULTZ_MOD_CTRL) ? 1u : 0u;
    uint32_t to;

    /* Shortcuts first: they are the same keys with a modifier held. */
    if (event->modifiers & SCHULTZ_MOD_CTRL) {
        switch (event->key) {
        case 'a':
            data->anchor = 0u;
            return schultz_text_move(tree, node, data, data->length, 1u) ==
                       SCHULTZ_OK ? SCHULTZ_EVENT_CONSUMED : SCHULTZ_OK;
        case 'c':
            schultz_text_copy(tree, node, data, 0);
            return SCHULTZ_EVENT_CONSUMED;
        case 'x':
            schultz_text_copy(tree, node, data, 1);
            return SCHULTZ_EVENT_CONSUMED;
        case 'v':
            schultz_text_paste(tree, node, data);
            return SCHULTZ_EVENT_CONSUMED;
        case 'z':
            if (shift) {
                schultz_text_redo(tree, node, data);
            } else {
                schultz_text_undo(tree, node, data);
            }
            return SCHULTZ_EVENT_CONSUMED;
        case 'y':
            schultz_text_redo(tree, node, data);
            return SCHULTZ_EVENT_CONSUMED;
        default:
            break;
        }
    }

    switch (event->key) {
    case SCHULTZ_KEY_LEFT:
        to = word
            ? schultz_word_prev(data->text, data->caret)
            : schultz_text_prev_cluster(data->text, data->length,
                                        data->caret);
        schultz_text_move(tree, node, data, to, shift);
        return SCHULTZ_EVENT_CONSUMED;

    case SCHULTZ_KEY_RIGHT:
        to = word
            ? schultz_word_next(data->text, data->length, data->caret)
            : schultz_text_next_cluster(data->text, data->length,
                                        data->caret);
        schultz_text_move(tree, node, data, to, shift);
        return SCHULTZ_EVENT_CONSUMED;

    case SCHULTZ_KEY_HOME:
        schultz_text_move(tree, node, data,
            data->multiline ? schultz_line_start(data->text, data->caret) : 0u,
            shift);
        return SCHULTZ_EVENT_CONSUMED;

    case SCHULTZ_KEY_END:
        schultz_text_move(tree, node, data,
            data->multiline ? schultz_line_end(data->text, data->length,
                                               data->caret)
                            : data->length, shift);
        return SCHULTZ_EVENT_CONSUMED;

    case SCHULTZ_KEY_UP:
    case SCHULTZ_KEY_DOWN:
        if (!data->multiline) {
            return SCHULTZ_OK; /* one line has nowhere to go */
        }
        if (schultz_text_line_at(tree, node, data, data->caret,
                                 (event->key == SCHULTZ_KEY_UP) ? -1 : 1,
                                 &to)) {
            schultz_text_move(tree, node, data, to, shift);
        }
        return SCHULTZ_EVENT_CONSUMED;

    case SCHULTZ_KEY_BACKSPACE:
        schultz_text_erase(tree, node, data, 0, word,
                           event->one_codepoint);
        return SCHULTZ_EVENT_CONSUMED;

    case SCHULTZ_KEY_DELETE:
        schultz_text_erase(tree, node, data, 1, word,
                           event->one_codepoint);
        return SCHULTZ_EVENT_CONSUMED;

    case SCHULTZ_KEY_RETURN:
        if (!data->multiline) {
            return SCHULTZ_OK; /* the host may want it as an accept */
        }
        schultz_text_replace(tree, node, data, schultz_sel_low(data),
                             schultz_sel_high(data), "\n", 1u, 0);
        return SCHULTZ_EVENT_CONSUMED;

    default:
        break;
    }
    return SCHULTZ_OK;
}

static int32_t schultz_text_event(schultz_tree *tree, schultz_handle node,
                                  const schultz_event *event)
{
    schultz_text_data *data =
        (schultz_text_data *)schultz_node_widget_data(tree, node);

    if (data == NULL) {
        return SCHULTZ_OK;
    }

    switch (event->type) {
    case SCHULTZ_EVENT_MOUSE_DOWN: {
        uint32_t at;

        /*
         * A finger has to lift before it places anything, and the press is
         * not taken. Dragging a finger from inside a field is how the page
         * behind it is scrolled, and a field that placed a caret on the way
         * down would take that gesture away and put a keyboard over what the
         * person was reading. The label beside this does the same, for the
         * same reason.
         *
         * Not consuming matters as much as not placing: an event that is
         * taken goes no further, and the view that scrolls is above this.
         */
        if (event->source == SCHULTZ_POINTER_TOUCH) {
            return 0;
        }
        at = schultz_text_offset_at(tree, node, data, event->local);

        /*
         * One press places the caret, two select the word under it, and
         * three take the whole line, which is a field's entire contents.
         */
        if (event->click_count >= 3u) {
            data->anchor = data->multiline
                ? schultz_line_start(data->text, at) : 0u;
            at = data->multiline
                ? schultz_line_end(data->text, data->length, at)
                : data->length;
        } else if (event->click_count == 2u) {
            /*
             * The word the press landed in, not the next one: stepping
             * forward from the character before it lands on the end of the
             * word the caret is inside.
             */
            data->anchor = schultz_word_prev(data->text,
                schultz_word_next(data->text, data->length, at));
            at = schultz_word_next(data->text, data->length, data->anchor);
        } else {
            data->anchor = at;
        }
        schultz_text_move(tree, node, data, at, 1u);
        return SCHULTZ_EVENT_CONSUMED;
    }

    case SCHULTZ_EVENT_DRAG:
        /* A finger dragging is a page being scrolled, not a selection being
         * made. Left alone, and left for whatever scrolls above. */
        if (event->source == SCHULTZ_POINTER_TOUCH) {
            return 0;
        }
        schultz_text_move(tree, node, data,
            schultz_text_offset_at(tree, node, data, event->local), 1u);
        return SCHULTZ_EVENT_CONSUMED;

    case SCHULTZ_EVENT_CLICK:
        /*
         * Where a finger puts the caret. The router sends this only when the
         * finger stayed still, so a tap places it and a swipe does not.
         */
        if (event->source == SCHULTZ_POINTER_TOUCH) {
            uint32_t at = schultz_text_offset_at(tree, node, data,
                                                 event->local);

            data->anchor = at;
            schultz_text_move(tree, node, data, at, 1u);
            return SCHULTZ_EVENT_CONSUMED;
        }
        return 0;

    case SCHULTZ_EVENT_KEY_DOWN:
        return schultz_text_key(tree, node, data, event);

    case SCHULTZ_EVENT_TEXT_INPUT:
        if (event->text == NULL) {
            return SCHULTZ_OK;
        }
        /* A committed composition replaces whatever was showing. */
        free(data->composition);
        data->composition = NULL;
        schultz_text_replace(tree, node, data, schultz_sel_low(data),
                             schultz_sel_high(data), event->text,
                             (uint32_t)strlen(event->text), 1);
        return SCHULTZ_EVENT_CONSUMED;

    case SCHULTZ_EVENT_TEXT_EDITING: {
        /*
         * A composition is provisional: it is shown but not in the buffer,
         * because the next event replaces it and an undo step per keystroke
         * of a Japanese word would be useless.
         */
        char *copy = NULL;

        if (event->text != NULL && event->text[0] != '\0') {
            size_t n = strlen(event->text);

            copy = (char *)malloc(n + 1u);
            if (copy != NULL) {
                memcpy(copy, event->text, n + 1u);
            }
        }
        free(data->composition);
        data->composition = copy;
        schultz_node_invalidate(tree, node);
        return SCHULTZ_EVENT_CONSUMED;
    }

    case SCHULTZ_EVENT_FOCUS_LOST:
        free(data->composition);
        data->composition = NULL;
        data->undo_typing = 0u;
        return SCHULTZ_OK;

    default:
        break;
    }
    return SCHULTZ_OK;
}

static void schultz_text_destroy(void *pointer)
{
    schultz_text_data *data = (schultz_text_data *)pointer;
    uint32_t i;

    if (data == NULL) {
        return;
    }
    for (i = 0; i < data->undo_count; i++) {
        schultz_undo_clear(&data->undo[i]);
    }
    schultz_spans_free(&data->spans, &data->span_count);
    free((void *)(uintptr_t)data->typing.link);
    free(data->composition);
    free(data->masked);
    free(data->text);
    free(data);
}

/*
 * Taking part in a selection that spans widgets.
 *
 * An editable widget keeps its own press handling, so clicking into it still
 * places a caret and dragging inside it still selects its text to edit. What
 * this adds is the other direction: a drag that began outside and passes over
 * it marks its words too, and a copy of that selection takes them along with
 * whatever they look like.
 */

static uint32_t schultz_text_select_length(schultz_tree *tree,
                                           schultz_handle node)
{
    const schultz_text_data *data = schultz_text_of(tree, node);

    if (data == NULL) {
        return 0u;
    }
    /*
     * A masked field takes no part at all. Its own copy already refuses, and
     * joining a selection that copies would be a way round that refusal.
     */
    return (data->mask != 0u) ? 0u : data->length;
}

static uint32_t schultz_text_select_offset_at(schultz_tree *tree,
                                              schultz_handle node,
                                              schultz_point local)
{
    const schultz_text_data *data = schultz_text_of(tree, node);

    if (data == NULL || data->mask != 0u) {
        return 0u;
    }
    return schultz_text_offset_at(tree, node, data, local);
}

static void schultz_text_select_set_range(schultz_tree *tree,
                                          schultz_handle node, uint32_t low,
                                          uint32_t high)
{
    schultz_text_data *data = schultz_text_of(tree, node);

    if (data == NULL || data->mask != 0u) {
        return;
    }
    if (data->anchor == low && data->caret == high) {
        return;
    }
    data->anchor = (low > data->length) ? data->length : low;
    data->caret  = (high > data->length) ? data->length : high;
    schultz_node_invalidate(tree, node);
}

static void schultz_text_select_range(schultz_tree *tree, schultz_handle node,
                                      uint32_t *out_low, uint32_t *out_high)
{
    const schultz_text_data *data = schultz_text_of(tree, node);

    *out_low  = 0u;
    *out_high = 0u;
    if (data == NULL || data->mask != 0u) {
        return;
    }
    *out_low  = schultz_sel_low(data);
    *out_high = schultz_sel_high(data);
}

static const char *schultz_text_select_text(schultz_tree *tree,
                                            schultz_handle node)
{
    const schultz_text_data *data = schultz_text_of(tree, node);

    if (data == NULL || data->mask != 0u) {
        return NULL;
    }
    return data->text;
}

static const schultz_span *schultz_text_select_spans(schultz_tree *tree,
                                                     schultz_handle node,
                                                     uint32_t *out_count)
{
    return schultz_text_field_spans(tree, node, out_count);
}

static const schultz_selectable_vtable schultz_text_selection_part = {
    schultz_text_select_length,
    schultz_text_select_offset_at,
    schultz_text_select_set_range,
    schultz_text_select_range,
    schultz_text_select_text,
    NULL,                         /* a field is text, not a picture */
    schultz_text_select_spans
};

static const schultz_widget_vtable schultz_text_field_widget = {
    .paint = schultz_text_paint, .event = schultz_text_event,
    .destroy = schultz_text_destroy,
    .selectable = &schultz_text_selection_part
};

static const schultz_widget_vtable schultz_text_area_widget = {
    .paint = schultz_text_paint, .event = schultz_text_event,
    .destroy = schultz_text_destroy,
    .selectable = &schultz_text_selection_part
};

/* Both are the same widget with one flag and one vtable different. */
int32_t schultz_text_field_set_spans(schultz_tree *tree, schultz_handle node,
                                     const schultz_span *spans,
                                     uint32_t count)
{
    schultz_text_data *data = schultz_text_of(tree, node);
    int32_t result;

    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (spans == NULL && count > 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (schultz_spans_same(data->spans, data->span_count, spans, count)) {
        return SCHULTZ_OK;
    }
    result = schultz_spans_store(&data->spans, &data->span_count, spans,
                                 count, data->length);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (data->multiline) {
        schultz_node_invalidate_layout(tree, node);
    }
    return schultz_node_invalidate(tree, node);
}

const schultz_span *schultz_text_field_spans(const schultz_tree *tree,
                                             schultz_handle node,
                                             uint32_t *out_count)
{
    const schultz_text_data *data;

    if (out_count != NULL) {
        *out_count = 0u;
    }
    if (!schultz_is_text(tree, node)) {
        return NULL;
    }
    data = (const schultz_text_data *)schultz_node_widget_data(tree, node);
    if (data == NULL) {
        return NULL;
    }
    if (out_count != NULL) {
        *out_count = data->span_count;
    }
    return data->spans;
}

int32_t schultz_text_field_set_span(schultz_tree *tree, schultz_handle node,
                                    uint32_t from, uint32_t to,
                                    const schultz_span *look)
{
    schultz_text_data *data = schultz_text_of(tree, node);
    int32_t result;

    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (to > data->length) {
        to = data->length;
    }
    if (from >= to) {
        return SCHULTZ_OK;
    }
    result = schultz_spans_set_range(&data->spans, &data->span_count, from,
                                     to, look);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_spans_tidy(data->spans, &data->span_count);
    if (data->multiline) {
        schultz_node_invalidate_layout(tree, node);
    }
    return schultz_node_invalidate(tree, node);
}

int32_t schultz_text_field_set_typing(schultz_tree *tree, schultz_handle node,
                                      const schultz_span *look)
{
    schultz_text_data *data = schultz_text_of(tree, node);

    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    free((void *)(uintptr_t)data->typing.link);
    memset(&data->typing, 0, sizeof(data->typing));
    data->typing_set = 0u;
    if (look == NULL) {
        return SCHULTZ_OK;
    }
    data->typing = *look;
    data->typing.link = schultz_span_copy_link(look->link);
    if (look->link != NULL && data->typing.link == NULL) {
        memset(&data->typing, 0, sizeof(data->typing));
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->typing_set = 1u;
    return SCHULTZ_OK;
}

static int32_t schultz_text_create(schultz_tree *tree, schultz_handle parent,
                                   const char *text, uint32_t multiline,
                                   schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_text_data *data;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    data = (schultz_text_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->multiline = multiline;
    data->lines     = multiline ? SCHULTZ_TEXT_AREA_LINES : 1u;
    if (schultz_text_reserve(data, (text == NULL) ? 0u
                                                  : (uint32_t)strlen(text))
            != SCHULTZ_OK) {
        free(data);
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->length = (text == NULL) ? 0u : (uint32_t)strlen(text);
    if (data->length > 0u) {
        memcpy(data->text, text, data->length);
    }
    data->text[data->length] = '\0';
    data->caret  = data->length;
    data->anchor = data->caret;

    schultz_node_set_pane(tree, node, &schultz_text_pane);
    schultz_node_set_widget(tree, node,
        multiline ? &schultz_text_area_widget : &schultz_text_field_widget,
        data);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_TEXT_INPUT);
    schultz_node_set_actions(tree, node,
        SCHULTZ_ACTION_FOCUS | SCHULTZ_ACTION_SET_VALUE);
    schultz_node_set_cursor(tree, node, SCHULTZ_CURSOR_TEXT);
    schultz_node_set_paint_margin(tree, node, SCHULTZ_RING_BLEED);
    schultz_node_set_value(tree, node, data->text);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_SURFACE_SUNKEN);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                            SCHULTZ_TOKEN_COLOR_BORDER);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_WIDTH,
                            SCHULTZ_TOKEN_BORDER_WIDTH);
        schultz_patch_token(&patch, SCHULTZ_PROP_CORNER_RADIUS,
                            SCHULTZ_TOKEN_RADIUS_CONTROL_SMALL);
        schultz_patch_token(&patch, SCHULTZ_PROP_PADDING,
                            SCHULTZ_TOKEN_SPACE_SM);
        schultz_patch_token(&patch, SCHULTZ_PROP_SELECTION_COLOR,
                            SCHULTZ_TOKEN_COLOR_SELECTION);
        schultz_widget_default_style(tree, node, &patch);
    }
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_HOVERED,
        SCHULTZ_PROP_BORDER_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_DISABLED,
        SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_DISABLED));

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_text_field_create(schultz_tree *tree, schultz_handle parent,
                                  const char *text, schultz_handle *out_node)
{
    return schultz_text_create(tree, parent, text, 0u, out_node);
}

int32_t schultz_text_field_set_input_type(schultz_tree *tree,
                                          schultz_handle node, uint32_t type)
{
    schultz_text_data *data = schultz_text_of(tree, node);

    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (type > SCHULTZ_INPUT_PASSWORD) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data->input_type = type;
    return SCHULTZ_OK;
}

uint32_t schultz_text_field_input_type(const schultz_tree *tree,
                                       schultz_handle node)
{
    const schultz_text_data *data =
        schultz_text_of((schultz_tree *)tree, node);

    if (data == NULL) {
        return SCHULTZ_INPUT_TEXT;
    }
    /*
     * A masked field is a password whether or not anybody said so. The mask
     * is the older way of asking for one and plenty of code uses it, and a
     * keyboard that offered emoji into a masked field would be wrong for the
     * same reason either way.
     */
    if (data->mask != 0u) {
        return SCHULTZ_INPUT_PASSWORD;
    }
    return data->input_type;
}

int32_t schultz_text_field_set_mask(schultz_tree *tree,
                                    schultz_handle node, uint32_t codepoint)
{
    schultz_text_data *data = schultz_text_of(tree, node);

    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (data->multiline && codepoint != 0u) {
        /* Masking a text area would hide wrapped lines of dots, which is
         * not a thing any password field is. */
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (data->mask == codepoint) {
        return SCHULTZ_OK;
    }
    data->mask = codepoint;
    if (codepoint == 0u) {
        free(data->masked);
        data->masked     = NULL;
        data->masked_len = 0u;
    }
    schultz_node_invalidate(tree, node);
    schultz_node_invalidate_layout(tree, node);
    return SCHULTZ_OK;
}

uint32_t schultz_text_field_mask(const schultz_tree *tree,
                                 schultz_handle node)
{
    const schultz_text_data *data;

    data = (const schultz_text_data *)schultz_text_of((schultz_tree *)tree,
                                                      node);
    return (data == NULL) ? 0u : data->mask;
}

int32_t schultz_password_field_create(schultz_tree *tree,
                                      schultz_handle parent,
                                      const char *text,
                                      schultz_handle *out_node)
{
    int32_t result = schultz_text_field_create(tree, parent, text, out_node);

    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_text_field_set_mask(tree, *out_node,
                                       SCHULTZ_PASSWORD_MASK);
}

int32_t schultz_text_area_create(schultz_tree *tree, schultz_handle parent,
                                 const char *text, schultz_handle *out_node)
{
    return schultz_text_create(tree, parent, text, 1u, out_node);
}

const char *schultz_text_get(const schultz_tree *tree, schultz_handle node)
{
    const schultz_text_data *data;

    if (!schultz_is_text(tree, node)) {
        return NULL;
    }
    data = (const schultz_text_data *)schultz_node_widget_data(tree, node);
    return (data == NULL) ? NULL : data->text;
}

int32_t schultz_text_set(schultz_tree *tree, schultz_handle node,
                         const char *text)
{
    schultz_text_data *data = schultz_text_of(tree, node);

    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_text_replace(tree, node, data, 0u, data->length,
                                text, (text == NULL) ? 0u
                                                     : (uint32_t)strlen(text),
                                0);
}

int32_t schultz_text_area_set_visible_lines(schultz_tree *tree,
                                            schultz_handle node,
                                            uint32_t lines)
{
    schultz_text_data *data = schultz_text_of(tree, node);

    if (data == NULL || !data->multiline) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (lines == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (data->lines == lines) {
        return SCHULTZ_OK;
    }
    data->lines = lines;
    schultz_node_invalidate_layout(tree, node);
    return schultz_node_invalidate(tree, node);
}

int32_t schultz_text_area_set_grows(schultz_tree *tree, schultz_handle node,
                                    int32_t grows)
{
    schultz_text_data *data = schultz_text_of(tree, node);

    if (data == NULL || !data->multiline) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (data->grows == (grows ? 1u : 0u)) {
        return SCHULTZ_OK;
    }
    data->grows = grows ? 1u : 0u;
    schultz_node_invalidate_layout(tree, node);
    return schultz_node_invalidate(tree, node);
}

int32_t schultz_text_set_selection(schultz_tree *tree, schultz_handle node,
                                   uint32_t anchor, uint32_t caret)
{
    schultz_text_data *data = schultz_text_of(tree, node);

    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data->anchor = (anchor > data->length) ? data->length : anchor;
    return schultz_text_move(tree, node, data, caret, 1u);
}

int32_t schultz_text_selection(const schultz_tree *tree, schultz_handle node,
                               uint32_t *out_anchor, uint32_t *out_caret)
{
    const schultz_text_data *data;

    if (!schultz_is_text(tree, node)) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data = (const schultz_text_data *)schultz_node_widget_data(tree, node);
    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (out_anchor != NULL) {
        *out_anchor = data->anchor;
    }
    if (out_caret != NULL) {
        *out_caret = data->caret;
    }
    return SCHULTZ_OK;
}

/* --------------------------------------------------------------- Canvas */

/** A canvas holds the drawing it was given, and its own arena to hold it. */
typedef struct {
    schultz_arena     arena;   /**< Owns the commands and what they point at. */
    schultz_draw_list list;    /**< The drawing, in the canvas's own space. */
    uint32_t          ready;   /**< Nonzero once the arena has been set up. */
    uint32_t          open;    /**< Nonzero between begin and end. */
    uint32_t          pixels;  /**< Nonzero to draw in screen pixels. */
    uint32_t          groups;  /**< Groups opened and not yet closed. */
} schultz_canvas_data;

static const schultz_widget_vtable schultz_canvas_widget;

static schultz_canvas_data *schultz_canvas_of(schultz_tree *tree,
                                              schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_canvas_widget) {
        return NULL;
    }
    return (schultz_canvas_data *)schultz_node_widget_data(tree, node);
}

static void schultz_canvas_destroy(void *pointer)
{
    schultz_canvas_data *canvas = (schultz_canvas_data *)pointer;

    if (canvas != NULL) {
        if (canvas->ready) {
            schultz_arena_free(&canvas->arena);
        }
        free(canvas);
    }
}

/*
 * Replays the retained drawing into this frame's list, shifted to wherever
 * the canvas has been laid out. Nothing is shaped or copied again: the
 * commands still point into the canvas's own arena, which outlives the frame.
 */
static int32_t schultz_canvas_paint(schultz_tree *tree, schultz_handle node,
                                    schultz_draw_list *list,
                                    schultz_arena *arena)
{
    const schultz_canvas_data *canvas =
        (const schultz_canvas_data *)schultz_node_widget_data(tree, node);
    schultz_rect bounds;
    uint32_t count;
    uint32_t i;
    int32_t result;

    (void)arena;
    if (canvas == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    result = schultz_widget_draw_box(list, schultz_widget_style(tree, node),
                                     bounds);
    if (result != SCHULTZ_OK) {
        return result;
    }

    count = schultz_draw_list_count(&canvas->list);
    if (count == 0u) {
        return SCHULTZ_OK;
    }
    /* Clipped, so a canvas cannot draw outside the space it was given. */
    result = schultz_draw_clip_begin(list, bounds);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_draw_offset_begin(list, bounds.x, bounds.y);
    /*
     * The canvas is placed in the toolkit's units like any other widget, and
     * only what is drawn inside it switches. So the transform goes on first
     * and the unit second: the drawing lands where the widget is either way.
     */
    if (canvas->pixels) {
        result = schultz_draw_device_pixels_begin(list);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    for (i = 0; i < count; i++) {
        result = schultz_draw_list_replay_one(list,
                     schultz_draw_list_at(&canvas->list, i));
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    if (canvas->pixels) {
        result = schultz_draw_device_pixels_end(list);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    schultz_draw_offset_end(list);
    return schultz_draw_clip_end(list);
}

static const schultz_widget_vtable schultz_canvas_widget = {
    .paint = schultz_canvas_paint, .destroy = schultz_canvas_destroy
};

int32_t schultz_canvas_create(schultz_tree *tree, schultz_handle parent,
                              schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_canvas_data *canvas;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    canvas = (schultz_canvas_data *)calloc(1, sizeof(*canvas));
    if (canvas == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    if (schultz_arena_init(&canvas->arena, 0) != SCHULTZ_OK) {
        free(canvas);
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    canvas->ready = 1u;
    schultz_draw_list_init(&canvas->list, &canvas->arena, 0);

    schultz_node_set_widget(tree, node, &schultz_canvas_widget, canvas);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_IMAGE);
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_canvas_begin(schultz_tree *tree, schultz_handle node)
{
    schultz_canvas_data *canvas = schultz_canvas_of(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /*
     * Reset the arena first, then rebuild the list on top of it. The list's
     * own chunks live in the memory being reclaimed, so it cannot simply be
     * emptied.
     */
    schultz_arena_reset(&canvas->arena);
    canvas->open   = 1u;
    canvas->groups = 0u;
    return schultz_draw_list_init(&canvas->list, &canvas->arena, 0);
}

int32_t schultz_canvas_set_pixel_exact(schultz_tree *tree, schultz_handle node,
                                       int32_t exact)
{
    schultz_canvas_data *canvas = schultz_canvas_of(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (canvas->pixels != (uint32_t)(exact != 0)) {
        canvas->pixels = (uint32_t)(exact != 0);
        schultz_node_invalidate(tree, node);
    }
    return SCHULTZ_OK;
}

int32_t schultz_canvas_pixel_size(const schultz_tree *tree,
                                  schultz_handle node, uint32_t *out_width,
                                  uint32_t *out_height)
{
    schultz_rect bounds;
    float scale;

    if (out_width == NULL || out_height == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_width  = 0u;
    *out_height = 0u;
    if (schultz_canvas_of((schultz_tree *)tree, node) == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (schultz_node_get_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    scale = schultz_tree_pixel_scale(tree);
    *out_width  = (uint32_t)(bounds.width * scale);
    *out_height = (uint32_t)(bounds.height * scale);
    return SCHULTZ_OK;
}

int32_t schultz_canvas_end(schultz_tree *tree, schultz_handle node)
{
    schultz_canvas_data *canvas = schultz_canvas_of(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /*
     * Any group left open is closed here, so a drawing that forgot one still
     * comes out. The alternative is a list the renderer refuses, and losing
     * the whole frame teaches a caller less than a group that ends where the
     * drawing ended: the picture is visibly one group short of what was
     * meant, which is the thing to go and look at.
     *
     * A caller that returned early, on an error or a branch it did not
     * expect, is the ordinary way this happens, and that caller has enough
     * to deal with.
     */
    while (canvas->groups > 0u) {
        (void)schultz_draw_group_end(&canvas->list);
        canvas->groups--;
    }
    canvas->open = 0u;
    if (schultz_draw_list_overflowed(&canvas->list)) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    return schultz_node_invalidate(tree, node);
}

/* Every drawing call checks the same two things before it appends. */
static schultz_canvas_data *schultz_canvas_open(schultz_tree *tree,
                                                schultz_handle node)
{
    schultz_canvas_data *canvas = schultz_canvas_of(tree, node);

    return (canvas != NULL && canvas->open) ? canvas : NULL;
}

int32_t schultz_canvas_fill_rect(schultz_tree *tree, schultz_handle node,
                                 schultz_rect rect, schultz_paint paint,
                                 float radius)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_fill_round_rect(&canvas->list, rect, paint, radius);
}

int32_t schultz_canvas_group_begin(schultz_tree *tree, schultz_handle node,
                                  float opacity, schultz_shadow shadow)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);
    int32_t result;

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    result = schultz_draw_group_begin(&canvas->list, opacity, shadow);
    if (result == SCHULTZ_OK) {
        canvas->groups++;
    }
    return result;
}

int32_t schultz_canvas_group_end(schultz_tree *tree, schultz_handle node)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);
    int32_t result;

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /*
     * Refused rather than passed down. A pop with no push would reach the
     * renderer, which refuses it too, but by then the failure belongs to the
     * frame rather than to the call that caused it, and the caller has
     * nothing to go on.
     */
    if (canvas->groups == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_draw_group_end(&canvas->list);
    if (result == SCHULTZ_OK) {
        canvas->groups--;
    }
    return result;
}

int32_t schultz_canvas_stroke_rect(schultz_tree *tree, schultz_handle node,
                                   schultz_rect rect, schultz_stroke stroke,
                                   float radius)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_stroke_round_rect(&canvas->list, rect, stroke,
                                          radius);
}

int32_t schultz_canvas_stroke_polygon(schultz_tree *tree, schultz_handle node,
                                      const schultz_point *points,
                                      uint32_t count, schultz_stroke stroke,
                                      int32_t closed)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_stroke_polygon(&canvas->list, points, count, stroke,
                                       closed);
}

/*
 * How wide a string will be, so that a caller can lay out around it.
 *
 * Drawing text without being able to measure it means guessing where the next
 * thing goes. The toolkit shapes its own text, so the answer is exact rather
 * than an estimate from a character count.
 */
int32_t schultz_canvas_measure_text(const schultz_tree *tree,
                                    schultz_handle font, const char *utf8,
                                    schultz_size *out_size)
{
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_arena scratch;
    int32_t result;

    if (out_size == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_size = schultz_size_make(0.0f, 0.0f);
    if (fonts == NULL || font == SCHULTZ_HANDLE_NONE || utf8 == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (schultz_arena_init(&scratch, 0) != SCHULTZ_OK) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_text_measure(fonts, font, utf8, -1, &scratch, out_size);
    schultz_arena_free(&scratch);
    return result;
}

int32_t schultz_canvas_stroke_ellipse(schultz_tree *tree,
                                      schultz_handle node,
                                      schultz_rect rect,
                                      schultz_stroke stroke)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_stroke_ellipse(&canvas->list, rect, stroke);
}

int32_t schultz_canvas_image(schultz_tree *tree, schultz_handle node,
                             schultz_handle image, schultz_rect source,
                             schultz_rect dest, uint8_t opacity)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_image(&canvas->list, image, source, dest, opacity);
}

int32_t schultz_canvas_clip_begin(schultz_tree *tree, schultz_handle node,
                                 schultz_rect rect)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_clip_begin(&canvas->list, rect);
}

int32_t schultz_canvas_clip_end(schultz_tree *tree, schultz_handle node)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_clip_end(&canvas->list);
}

int32_t schultz_canvas_offset_begin(schultz_tree *tree, schultz_handle node,
                                   float dx, float dy)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_offset_begin(&canvas->list, dx, dy);
}

int32_t schultz_canvas_offset_end(schultz_tree *tree, schultz_handle node)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_offset_end(&canvas->list);
}

int32_t schultz_canvas_rotation_begin(schultz_tree *tree, schultz_handle node,
                                     float degrees, float cx, float cy)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_rotation_begin(&canvas->list, degrees, cx, cy);
}

int32_t schultz_canvas_rotation_end(schultz_tree *tree, schultz_handle node)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_rotation_end(&canvas->list);
}

int32_t schultz_canvas_fill_ellipse(schultz_tree *tree, schultz_handle node,
                                    schultz_rect rect, schultz_paint paint)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_fill_ellipse(&canvas->list, rect, paint);
}

int32_t schultz_canvas_line(schultz_tree *tree, schultz_handle node,
                            schultz_point from, schultz_point to,
                            schultz_stroke stroke)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_line(&canvas->list, from, to, stroke);
}

int32_t schultz_canvas_fill_polygon(schultz_tree *tree, schultz_handle node,
                                    const schultz_point *points, uint32_t count,
                                    schultz_paint paint, uint32_t rule)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_fill_polygon(&canvas->list, points, count, paint, rule);
}

int32_t schultz_canvas_fill_path(schultz_tree *tree, schultz_handle node,
                                 const uint8_t *steps, uint32_t step_count,
                                 const schultz_point *points,
                                 uint32_t point_count, schultz_paint paint,
                                 uint32_t rule)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_fill_path(&canvas->list, steps, step_count, points,
                                  point_count, paint, rule);
}

int32_t schultz_canvas_stroke_path(schultz_tree *tree, schultz_handle node,
                                   const uint8_t *steps, uint32_t step_count,
                                   const schultz_point *points,
                                   uint32_t point_count,
                                   schultz_stroke stroke)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_draw_stroke_path(&canvas->list, steps, step_count, points,
                                    point_count, stroke);
}

int32_t schultz_canvas_text(schultz_tree *tree, schultz_handle node,
                            schultz_handle font, const char *utf8, float x,
                            float y, schultz_paint paint)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_text_run run;
    schultz_glyph *placed;
    uint32_t i;

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (fonts == NULL || utf8 == NULL || font == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * Shaped once, into the canvas's own arena, and replayed for free every
     * frame thereafter. That is the point of a retained list: a chart redraws
     * when its data changes, not sixty times a second.
     */
    if (schultz_text_shape(fonts, font, utf8, -1, SCHULTZ_DIR_AUTO,
                           &canvas->arena, &run) != SCHULTZ_OK ||
        run.count == 0u) {
        return SCHULTZ_OK;
    }
    placed = (schultz_glyph *)schultz_arena_alloc(
        &canvas->arena, (size_t)run.count * sizeof(*placed),
        _Alignof(schultz_glyph));
    if (placed == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    for (i = 0; i < run.count; i++) {
        placed[i].glyph_id = run.glyphs[i].glyph_id;
        placed[i].x = run.glyphs[i].x + x;
        placed[i].y = run.glyphs[i].y + y;
    }
    schultz_widget_draw_glyphs(&canvas->list, font, run.fonts, placed,
                               run.count, paint);
    return SCHULTZ_OK;
}

int32_t schultz_canvas_stroke_text(schultz_tree *tree, schultz_handle node,
                                   schultz_handle font, const char *utf8,
                                   float x, float y, schultz_stroke stroke)
{
    schultz_canvas_data *canvas = schultz_canvas_open(tree, node);
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_text_run run;
    schultz_glyph *placed;
    uint32_t i;

    if (canvas == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (fonts == NULL || utf8 == NULL || font == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (schultz_text_shape(fonts, font, utf8, -1, SCHULTZ_DIR_AUTO,
                           &canvas->arena, &run) != SCHULTZ_OK ||
        run.count == 0u) {
        return SCHULTZ_OK;
    }
    placed = (schultz_glyph *)schultz_arena_alloc(
        &canvas->arena, (size_t)run.count * sizeof(*placed),
        _Alignof(schultz_glyph));
    if (placed == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    for (i = 0; i < run.count; i++) {
        placed[i].glyph_id = run.glyphs[i].glyph_id;
        placed[i].x = run.glyphs[i].x + x;
        placed[i].y = run.glyphs[i].y + y;
    }
    /*
     * Split by face, the way a filled run is. A glyph index means something
     * only to the face it came from, so stroking a fallback glyph out of the
     * first face would not draw a thinner version of it, it would draw a
     * different letter.
     */
    if (run.fonts == NULL) {
        return schultz_draw_glyph_outline(&canvas->list, font, placed,
                                          run.count, stroke);
    }
    {
        uint32_t start = 0u;

        for (i = 1u; i <= run.count; i++) {
            if (i == run.count || run.fonts[i] != run.fonts[start]) {
                int32_t result = schultz_draw_glyph_outline(&canvas->list,
                                     run.fonts[start], placed + start,
                                     i - start, stroke);

                if (result != SCHULTZ_OK) {
                    return result;
                }
                start = i;
            }
        }
    }
    return SCHULTZ_OK;
}

uint32_t schultz_canvas_count(const schultz_tree *tree, schultz_handle node)
{
    const schultz_canvas_data *canvas;

    if (schultz_node_widget(tree, node) != &schultz_canvas_widget) {
        return 0u;
    }
    canvas = (const schultz_canvas_data *)schultz_node_widget_data(tree,
                                                                   node);
    return (canvas == NULL) ? 0u : schultz_draw_list_count(&canvas->list);
}

/* ------------------------------------------------------------- GroupBox */

/*
 * Takes a fixed amount off an available size, leaving unbounded unbounded.
 * The panes have the same rule; a widget that lays out children needs it too.
 */
static float schultz_widget_inset(float available, float amount)
{
    if (schultz_layout_is_unbounded(available)) {
        return available;
    }
    available -= amount;
    return (available < 0.0f) ? 0.0f : available;
}

/** How far in from the left edge a group box's title sits. */
#define SCHULTZ_GROUP_TITLE_INSET 10.0f

/** A group box remembers its title so it can leave a gap in its border. */
typedef struct {
    schultz_handle title;   /**< The Label set into the top edge. */
    schultz_handle content; /**< What the application fills. */
} schultz_group_data;

static const schultz_widget_vtable schultz_group_widget;

/*
 * The title sits on the top edge and the content fills what is left. Half the
 * title's height is above the border, which is what makes the border look cut
 * rather than crossed.
 */
static int32_t schultz_group_measure(schultz_tree *tree, schultz_handle node,
                                     float avail_w, float avail_h,
                                     schultz_size *out_size)
{
    const schultz_group_data *group =
        (const schultz_group_data *)schultz_node_widget_data(tree, node);
    schultz_size title = { 0.0f, 0.0f };
    schultz_size content = { 0.0f, 0.0f };
    float padding = 0.0f;

    if (group == NULL) {
        *out_size = schultz_size_make(0.0f, 0.0f);
        return SCHULTZ_OK;
    }
    schultz_node_get_spacing(tree, node, &padding, NULL);
    schultz_layout_measure(tree, group->title, avail_w, avail_h, &title);
    schultz_layout_measure(tree, group->content,
                           schultz_widget_inset(avail_w, padding * 2.0f),
                           schultz_widget_inset(avail_h,
                                                padding * 2.0f + title.height),
                           &content);

    *out_size = schultz_size_make(
        (content.width + padding * 2.0f > title.width +
             SCHULTZ_GROUP_TITLE_INSET * 2.0f)
            ? content.width + padding * 2.0f
            : title.width + SCHULTZ_GROUP_TITLE_INSET * 2.0f,
        content.height + padding * 2.0f + title.height);
    return SCHULTZ_OK;
}

static int32_t schultz_group_arrange(schultz_tree *tree, schultz_handle node,
                                     schultz_rect rect)
{
    schultz_group_data *group =
        (schultz_group_data *)schultz_node_widget_data(tree, node);
    schultz_size title = { 0.0f, 0.0f };
    float padding = 0.0f;

    if (group == NULL) {
        return SCHULTZ_OK;
    }
    schultz_node_get_spacing(tree, node, &padding, NULL);
    schultz_layout_measure(tree, group->title, rect.width, -1.0f, &title);

    schultz_layout_arrange(tree, group->title,
        schultz_rect_make(SCHULTZ_GROUP_TITLE_INSET, 0.0f, title.width,
                          title.height));
    return schultz_layout_arrange(tree, group->content,
        schultz_rect_make(padding, title.height + padding,
                          rect.width - padding * 2.0f,
                          rect.height - title.height - padding * 2.0f));
}

static const schultz_pane_vtable schultz_group_pane = {
    schultz_group_measure, schultz_group_arrange
};

/*
 * The border runs around everything below the middle of the title, with a gap
 * where the title sits. Four segments rather than a rectangle, because a
 * rectangle cannot have a hole in one side.
 */
static int32_t schultz_group_paint(schultz_tree *tree, schultz_handle node,
                                   schultz_draw_list *list,
                                   schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    const schultz_group_data *group =
        (const schultz_group_data *)schultz_node_widget_data(tree, node);
    schultz_stroke stroke = schultz_widget_stroke(style);
    schultz_paint fill = schultz_resolved_paint(style,
                                                SCHULTZ_PROP_BACKGROUND);
    schultz_rect bounds;
    schultz_rect title;
    schultz_rect body;
    float top;
    float gap_from;
    float gap_to;

    (void)arena;
    if (group == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        schultz_node_absolute_bounds(tree, group->title, &title)
            != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    top  = title.y + title.height * 0.5f;
    body = schultz_rect_make(bounds.x, top, bounds.width,
                             bounds.height - (top - bounds.y));

    if (!schultz_paint_is_invisible(fill)) {
        schultz_draw_fill_round_rect(list, body, fill,
            schultz_resolved_number(style, SCHULTZ_PROP_CORNER_RADIUS));
    }
    if (schultz_paint_is_invisible(stroke.paint) || stroke.width <= 0.0f) {
        return SCHULTZ_OK;
    }

    gap_from = title.x - 4.0f;
    gap_to   = title.x + title.width + 4.0f;

    /* The top edge, in two pieces with the title between them. */
    schultz_draw_line(list, schultz_point_make(bounds.x, top),
                      schultz_point_make(gap_from, top), stroke);
    schultz_draw_line(list, schultz_point_make(gap_to, top),
                      schultz_point_make(bounds.x + bounds.width, top),
                      stroke);
    /* Then down both sides and along the bottom. */
    schultz_draw_line(list, schultz_point_make(bounds.x, top),
        schultz_point_make(bounds.x, body.y + body.height), stroke);
    schultz_draw_line(list,
        schultz_point_make(bounds.x + bounds.width, top),
        schultz_point_make(bounds.x + bounds.width, body.y + body.height),
        stroke);
    return schultz_draw_line(list,
        schultz_point_make(bounds.x, body.y + body.height),
        schultz_point_make(bounds.x + bounds.width, body.y + body.height),
        stroke);
}

static const schultz_widget_vtable schultz_group_widget = {
    .paint = schultz_group_paint, .destroy = free
};

int32_t schultz_group_box_create(schultz_tree *tree, schultz_handle parent,
                                 const char *title, schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_group_data *group;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    group = (schultz_group_data *)calloc(1, sizeof(*group));
    if (group == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_label_create(tree, node, title, &group->title);
    if (result == SCHULTZ_OK) {
        result = schultz_panel_create(tree, node, &group->content);
    }
    if (result != SCHULTZ_OK) {
        free(group);
        schultz_node_destroy(tree, node);
        return result;
    }
    schultz_label_set_wrap(tree, group->title, 0);
    schultz_node_set_hit_testable(tree, group->title, 0);
    schultz_node_set_pane(tree, group->content, schultz_pane_stack());

    schultz_node_set_pane(tree, node, &schultz_group_pane);
    schultz_node_set_widget(tree, node, &schultz_group_widget, group);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_GROUP);
    schultz_node_set_name(tree, node, (title == NULL) ? "" : title);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                            SCHULTZ_TOKEN_COLOR_BORDER);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_WIDTH,
                            SCHULTZ_TOKEN_BORDER_WIDTH);
        schultz_patch_token(&patch, SCHULTZ_PROP_PADDING,
                            SCHULTZ_TOKEN_SPACE_MD);
        schultz_widget_default_style(tree, node, &patch);
    }
    *out_node = node;
    return SCHULTZ_OK;
}

schultz_handle schultz_group_box_content(const schultz_tree *tree,
                                         schultz_handle node)
{
    const schultz_group_data *group;

    if (schultz_node_widget(tree, node) != &schultz_group_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    group = (const schultz_group_data *)schultz_node_widget_data(tree, node);
    return (group == NULL) ? SCHULTZ_HANDLE_NONE : group->content;
}

/* ------------------------------------------------------------ SplitPane */

/** How wide the divider between a split pane's two halves is. */
#define SCHULTZ_DIVIDER_WIDTH 6.0f

/** Two children and where the line between them sits. */
typedef struct {
    uint32_t orientation; /**< SCHULTZ_ORIENT_HORIZONTAL splits left/right. */
    float    position;    /**< Where the divider is, 0 to 1 across. */
    uint32_t dragging;    /**< Nonzero while the divider is being moved. */
    schultz_handle first;  /**< Left, or top. */
    schultz_handle second; /**< Right, or bottom. */
} schultz_split_data;

static const schultz_widget_vtable schultz_split_widget;

/* Nonzero when the two halves sit side by side rather than stacked. */
static int32_t schultz_split_across(const schultz_split_data *split)
{
    return split->orientation == SCHULTZ_ORIENT_HORIZONTAL;
}

/* The divider's rectangle, in the split pane's own space. */
static schultz_rect schultz_split_divider(const schultz_split_data *split,
                                          schultz_rect rect)
{
    float length = schultz_split_across(split) ? rect.width : rect.height;
    float at = (length - SCHULTZ_DIVIDER_WIDTH) * split->position;

    return schultz_split_across(split)
        ? schultz_rect_make(at, 0.0f, SCHULTZ_DIVIDER_WIDTH, rect.height)
        : schultz_rect_make(0.0f, at, rect.width, SCHULTZ_DIVIDER_WIDTH);
}

static int32_t schultz_split_measure(schultz_tree *tree, schultz_handle node,
                                     float avail_w, float avail_h,
                                     schultz_size *out_size)
{
    const schultz_split_data *split =
        (const schultz_split_data *)schultz_node_widget_data(tree, node);
    schultz_size a = { 0.0f, 0.0f };
    schultz_size b = { 0.0f, 0.0f };

    if (split == NULL) {
        *out_size = schultz_size_make(0.0f, 0.0f);
        return SCHULTZ_OK;
    }
    schultz_layout_measure(tree, split->first, avail_w, avail_h, &a);
    schultz_layout_measure(tree, split->second, avail_w, avail_h, &b);

    /* Both halves plus the divider, along the axis that is split. */
    *out_size = schultz_split_across(split)
        ? schultz_size_make(a.width + b.width + SCHULTZ_DIVIDER_WIDTH,
                            (a.height > b.height) ? a.height : b.height)
        : schultz_size_make((a.width > b.width) ? a.width : b.width,
                            a.height + b.height + SCHULTZ_DIVIDER_WIDTH);
    return SCHULTZ_OK;
}

static int32_t schultz_split_arrange(schultz_tree *tree, schultz_handle node,
                                     schultz_rect rect)
{
    schultz_split_data *split =
        (schultz_split_data *)schultz_node_widget_data(tree, node);
    schultz_rect divider;

    if (split == NULL) {
        return SCHULTZ_OK;
    }
    divider = schultz_split_divider(split, rect);

    if (schultz_split_across(split)) {
        schultz_layout_arrange(tree, split->first,
            schultz_rect_make(0.0f, 0.0f, divider.x, rect.height));
        return schultz_layout_arrange(tree, split->second,
            schultz_rect_make(divider.x + SCHULTZ_DIVIDER_WIDTH, 0.0f,
                              rect.width - divider.x - SCHULTZ_DIVIDER_WIDTH,
                              rect.height));
    }
    schultz_layout_arrange(tree, split->first,
        schultz_rect_make(0.0f, 0.0f, rect.width, divider.y));
    return schultz_layout_arrange(tree, split->second,
        schultz_rect_make(0.0f, divider.y + SCHULTZ_DIVIDER_WIDTH, rect.width,
                          rect.height - divider.y - SCHULTZ_DIVIDER_WIDTH));
}

static const schultz_pane_vtable schultz_split_pane = {
    schultz_split_measure, schultz_split_arrange
};

static int32_t schultz_split_paint(schultz_tree *tree, schultz_handle node,
                                   schultz_draw_list *list,
                                   schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    const schultz_split_data *split =
        (const schultz_split_data *)schultz_node_widget_data(tree, node);
    schultz_rect bounds;
    schultz_rect divider;

    (void)arena;
    if (split == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    divider = schultz_split_divider(split, bounds);
    divider.x += bounds.x;
    divider.y += bounds.y;

    return schultz_draw_fill_rect(list, divider,
        schultz_resolved_paint(style, SCHULTZ_PROP_BORDER_COLOR));
}

/*
 * The divider captures the pointer, which is the behaviour a pane cannot
 * express and the reason a split pane is a widget rather than a layout.
 */
static int32_t schultz_split_event(schultz_tree *tree, schultz_handle node,
                                   const schultz_event *event)
{
    schultz_split_data *split =
        (schultz_split_data *)schultz_node_widget_data(tree, node);
    schultz_rect bounds;
    schultz_rect divider;
    float length;
    float at;

    if (split == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        schultz_rect_is_empty(bounds)) {
        return SCHULTZ_OK;
    }
    divider = schultz_split_divider(split, bounds);
    length  = schultz_split_across(split) ? bounds.width : bounds.height;
    at      = schultz_split_across(split) ? event->local.x : event->local.y;

    switch (event->type) {
    case SCHULTZ_EVENT_MOUSE_DOWN: {
        float edge = schultz_split_across(split) ? divider.x : divider.y;

        if (at < edge || at > edge + SCHULTZ_DIVIDER_WIDTH) {
            return SCHULTZ_OK; /* the press was on a half, not the line */
        }
        split->dragging = 1u;
        return SCHULTZ_EVENT_CONSUMED;
    }

    case SCHULTZ_EVENT_DRAG:
        if (!split->dragging || length <= SCHULTZ_DIVIDER_WIDTH) {
            return SCHULTZ_OK;
        }
        return schultz_split_pane_set_position(tree, node,
            (at - SCHULTZ_DIVIDER_WIDTH * 0.5f) /
                (length - SCHULTZ_DIVIDER_WIDTH)) == SCHULTZ_OK
            ? SCHULTZ_EVENT_CONSUMED : SCHULTZ_OK;

    case SCHULTZ_EVENT_MOUSE_UP:
        split->dragging = 0u;
        break;

    default:
        break;
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_split_widget = {
    .paint = schultz_split_paint, .event = schultz_split_event, .destroy = free
};

int32_t schultz_split_pane_create(schultz_tree *tree, schultz_handle parent,
                                  uint32_t orientation,
                                  schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_split_data *split;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL || orientation > SCHULTZ_ORIENT_VERTICAL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    split = (schultz_split_data *)calloc(1, sizeof(*split));
    if (split == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    split->orientation = orientation;
    split->position    = 0.5f;

    result = schultz_panel_create(tree, node, &split->first);
    if (result == SCHULTZ_OK) {
        result = schultz_panel_create(tree, node, &split->second);
    }
    if (result != SCHULTZ_OK) {
        free(split);
        schultz_node_destroy(tree, node);
        return result;
    }

    schultz_node_set_pane(tree, split->first, schultz_pane_stack());
    schultz_node_set_pane(tree, split->second, schultz_pane_stack());
    schultz_node_set_pane(tree, node, &schultz_split_pane);
    schultz_node_set_widget(tree, node, &schultz_split_widget, split);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_GROUP);
    schultz_node_set_cursor(tree, node,
        (orientation == SCHULTZ_ORIENT_HORIZONTAL) ? SCHULTZ_CURSOR_RESIZE_H
                                                   : SCHULTZ_CURSOR_RESIZE_V);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                            SCHULTZ_TOKEN_COLOR_BORDER);
        schultz_widget_default_style(tree, node, &patch);
    }
    *out_node = node;
    return SCHULTZ_OK;
}

schultz_handle schultz_split_pane_half(const schultz_tree *tree,
                                       schultz_handle node, uint32_t which)
{
    const schultz_split_data *split;

    if (schultz_node_widget(tree, node) != &schultz_split_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    split = (const schultz_split_data *)schultz_node_widget_data(tree, node);
    if (split == NULL) {
        return SCHULTZ_HANDLE_NONE;
    }
    return (which == 0u) ? split->first : split->second;
}

int32_t schultz_split_pane_set_position(schultz_tree *tree,
                                        schultz_handle node, float position)
{
    schultz_split_data *split;

    if (schultz_node_widget(tree, node) != &schultz_split_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    split = (schultz_split_data *)schultz_node_widget_data(tree, node);
    if (position < 0.0f) {
        position = 0.0f;
    }
    if (position > 1.0f) {
        position = 1.0f;
    }
    if (position == split->position) {
        return SCHULTZ_OK;
    }
    split->position = position;
    /* Both halves change size, which is a relayout and not just a repaint. */
    schultz_node_invalidate_layout(tree, node);
    return schultz_node_invalidate(tree, node);
}

float schultz_split_pane_position(const schultz_tree *tree,
                                  schultz_handle node)
{
    const schultz_split_data *split;

    if (schultz_node_widget(tree, node) != &schultz_split_widget) {
        return 0.0f;
    }
    split = (const schultz_split_data *)schultz_node_widget_data(tree, node);
    return (split == NULL) ? 0.0f : split->position;
}

/* -------------------------------------------------------------- TabView */

/** The largest number of tabs one view may hold. */
enum { SCHULTZ_TABS_MAX = 16 };

/** A strip of buttons, and one page showing at a time. */
typedef struct {
    schultz_handle strip;                    /**< Holds the tab buttons. */
    schultz_handle pages[SCHULTZ_TABS_MAX];  /**< One panel per tab. */
    schultz_handle buttons[SCHULTZ_TABS_MAX];/**< The button for each page. */
    uint32_t       count;                    /**< How many tabs there are. */
    uint32_t       selected;                 /**< Which page is showing. */
} schultz_tab_data;

static const schultz_widget_vtable schultz_tab_widget;

/*
 * The strip across the top at its own height, and the selected page filling
 * everything below it. The other pages are hidden rather than laid out, which
 * is the behaviour a pane cannot express.
 */
static int32_t schultz_tab_measure(schultz_tree *tree, schultz_handle node,
                                   float avail_w, float avail_h,
                                   schultz_size *out_size)
{
    const schultz_tab_data *tabs =
        (const schultz_tab_data *)schultz_node_widget_data(tree, node);
    schultz_size strip = { 0.0f, 0.0f };
    schultz_size page = { 0.0f, 0.0f };

    if (tabs == NULL) {
        *out_size = schultz_size_make(0.0f, 0.0f);
        return SCHULTZ_OK;
    }
    schultz_layout_measure(tree, tabs->strip, avail_w, avail_h, &strip);
    if (tabs->selected < tabs->count) {
        schultz_layout_measure(tree, tabs->pages[tabs->selected], avail_w,
                               schultz_widget_inset(avail_h, strip.height),
                               &page);
    }
    *out_size = schultz_size_make(
        (strip.width > page.width) ? strip.width : page.width,
        strip.height + page.height);
    return SCHULTZ_OK;
}

static int32_t schultz_tab_arrange(schultz_tree *tree, schultz_handle node,
                                   schultz_rect rect)
{
    schultz_tab_data *tabs =
        (schultz_tab_data *)schultz_node_widget_data(tree, node);
    schultz_size strip = { 0.0f, 0.0f };
    uint32_t i;

    if (tabs == NULL) {
        return SCHULTZ_OK;
    }
    schultz_layout_measure(tree, tabs->strip, rect.width, -1.0f, &strip);
    schultz_layout_arrange(tree, tabs->strip,
        schultz_rect_make(0.0f, 0.0f, rect.width, strip.height));

    for (i = 0; i < tabs->count; i++) {
        uint32_t state = schultz_node_get_state(tree, tabs->pages[i]);

        schultz_node_set_state(tree, tabs->pages[i],
            (i == tabs->selected) ? (state | SCHULTZ_STATE_VISIBLE)
                                  : (state & ~(uint32_t)SCHULTZ_STATE_VISIBLE));
        if (i == tabs->selected) {
            schultz_layout_arrange(tree, tabs->pages[i],
                schultz_rect_make(0.0f, strip.height, rect.width,
                                  rect.height - strip.height));
        }
    }
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_tab_pane = {
    schultz_tab_measure, schultz_tab_arrange
};

/** @brief How thick the bar under the live tab is. */
#define SCHULTZ_TAB_INDICATOR 2.5f

/*
 * A tab strip is a rule with one segment of it lit, not a row of buttons.
 * The buttons themselves are left unpainted, so what marks the live tab is
 * the indicator under it and the colour of its text.
 */
static int32_t schultz_tab_paint(schultz_tree *tree, schultz_handle node,
                                 schultz_draw_list *list,
                                 schultz_arena *arena)
{
    const schultz_tab_data *tabs =
        (const schultz_tab_data *)schultz_node_widget_data(tree, node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_rect bounds;
    schultz_rect strip;
    schultz_rect live;
    int32_t result;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    result = schultz_widget_draw_box(list, style, bounds);
    if (result != SCHULTZ_OK || tabs == NULL || tabs->count == 0u) {
        return result;
    }
    if (schultz_node_absolute_bounds(tree, tabs->strip, &strip)
            != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }

    /* The rule the whole strip sits on. */
    result = schultz_draw_fill_rect(list,
        schultz_rect_make(bounds.x, strip.y + strip.height - 1.0f,
                          bounds.width, 1.0f),
        schultz_resolved_paint(style, SCHULTZ_PROP_BORDER_COLOR));
    if (result != SCHULTZ_OK) {
        return result;
    }

    /* And the segment of it under whichever tab is showing. */
    if (tabs->selected < tabs->count &&
        schultz_node_absolute_bounds(tree, tabs->buttons[tabs->selected],
                                     &live) == SCHULTZ_OK) {
        result = schultz_draw_fill_rect(list,
            schultz_rect_make(live.x,
                              strip.y + strip.height - SCHULTZ_TAB_INDICATOR,
                              live.width, SCHULTZ_TAB_INDICATOR),
            schultz_resolved_paint(style, SCHULTZ_PROP_SELECTION_COLOR));
        if (result != SCHULTZ_OK) {
            return result;
        }
    }

    /*
     * Focus, in the same shape. Tabs are selected by being activated, not by
     * being reached, so a keyboard can be on a tab that is not the one
     * showing, and something has to say where it is. Marking it the way the
     * live tab is marked keeps that inside the strip rather than putting a
     * box back around the caption.
     *
     * Only when the two differ: when they are the same tab the accent line
     * is already saying where the keyboard is.
     */
    {
        uint32_t i;

        for (i = 0u; i < tabs->count; i++) {
            schultz_rect at;

            if (i == tabs->selected ||
                !(schultz_node_get_state(tree, tabs->buttons[i]) &
                  SCHULTZ_STATE_FOCUSED) ||
                schultz_node_absolute_bounds(tree, tabs->buttons[i], &at)
                    != SCHULTZ_OK) {
                continue;
            }
            return schultz_draw_fill_rect(list,
                schultz_rect_make(at.x,
                                  strip.y + strip.height -
                                      SCHULTZ_TAB_INDICATOR,
                                  at.width, SCHULTZ_TAB_INDICATOR),
                schultz_resolved_paint(style, SCHULTZ_PROP_FOCUS_RING_COLOR));
        }
    }
    return SCHULTZ_OK;
}

/* A click on a tab button selects its page. */
static int32_t schultz_tab_event(schultz_tree *tree, schultz_handle node,
                                 const schultz_event *event)
{
    schultz_tab_data *tabs =
        (schultz_tab_data *)schultz_node_widget_data(tree, node);
    uint32_t i;

    if (tabs == NULL || event->type != SCHULTZ_EVENT_CLICK) {
        return SCHULTZ_OK;
    }
    for (i = 0; i < tabs->count; i++) {
        if (tabs->buttons[i] == event->target) {
            schultz_tab_view_select(tree, node, i);
            return SCHULTZ_OK;
        }
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_tab_widget = {
    .paint = schultz_tab_paint, .event = schultz_tab_event, .destroy = free
};

int32_t schultz_tab_view_create(schultz_tree *tree, schultz_handle parent,
                                schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_tab_data *tabs;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    tabs = (schultz_tab_data *)calloc(1, sizeof(*tabs));
    if (tabs == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_panel_create(tree, node, &tabs->strip);
    if (result != SCHULTZ_OK) {
        free(tabs);
        schultz_node_destroy(tree, node);
        return result;
    }
    schultz_node_set_pane(tree, tabs->strip, schultz_pane_hbox());
    schultz_node_set_style_property(tree, tabs->strip, SCHULTZ_PROP_GAP,
                                    schultz_value_token(
                                        SCHULTZ_TOKEN_SPACE_XS));

    schultz_node_set_pane(tree, node, &schultz_tab_pane);
    schultz_node_set_widget(tree, node, &schultz_tab_widget, tabs);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_GROUP);
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_tab_view_add(schultz_tree *tree, schultz_handle node,
                             const char *title, schultz_handle *out_page)
{
    schultz_tab_data *tabs;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    schultz_handle page = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (schultz_node_widget(tree, node) != &schultz_tab_widget ||
        out_page == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    tabs = (schultz_tab_data *)schultz_node_widget_data(tree, node);
    if (tabs->count == SCHULTZ_TABS_MAX) {
        return SCHULTZ_ERR_EXHAUSTED;
    }

    result = schultz_button_create(tree, tabs->strip, title, &button);
    if (result == SCHULTZ_OK) {
        result = schultz_panel_create(tree, node, &page);
    }
    if (result != SCHULTZ_OK) {
        return result;
    }
    /* Holds one thing and sizes itself around it, as every content node in
     * the toolkit does. A page without this arranges nothing, so everything
     * a host puts in it keeps the empty bounds it was born with and the page
     * comes up blank. */
    schultz_node_set_pane(tree, page, schultz_pane_stack());
    /*
     * A tab is a label with an indicator under it, so the button underneath
     * gives up its box: no fill, no outline, no corner. What is left is the
     * caption, which the strip's own paint marks.
     */
    schultz_node_set_style_property(tree, button, SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TRANSPARENT));
    schultz_node_set_style_property(tree, button, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(0.0f));
    schultz_node_set_style_property(tree, button, SCHULTZ_PROP_CORNER_RADIUS,
                                    schultz_value_number(0.0f));
    /*
     * And the ring with it. A focus ring is a box drawn around a control, and
     * a tab has no box to draw one around: it would put an outline back on
     * the one tab that is supposed to be marked by a line underneath it
     * instead. The strip marks focus in its own shape; see schultz_tab_paint.
     */
    schultz_node_set_style_property(tree, button,
        SCHULTZ_PROP_FOCUS_RING_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TRANSPARENT));
    schultz_node_set_style_property(tree, button, SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_MUTED));
    schultz_node_set_state_property(tree, button,
        SCHULTZ_STYLE_STATE_HOVERED, SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT));
    /* The live tab reads in full strength, the rest are muted. */
    schultz_node_set_state_property(tree, button,
        SCHULTZ_STYLE_STATE_SELECTED, SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));

    tabs->buttons[tabs->count] = button;
    tabs->pages[tabs->count]   = page;
    tabs->count++;
    /* Selecting is what hides the rest, so the first tab has to do it. */
    schultz_tab_view_select(tree, node, tabs->selected);
    schultz_node_invalidate_layout(tree, node);
    *out_page = page;
    return SCHULTZ_OK;
}

int32_t schultz_tab_view_select(schultz_tree *tree, schultz_handle node,
                                uint32_t index)
{
    schultz_tab_data *tabs;
    uint32_t i;
    int32_t  changed;

    if (schultz_node_widget(tree, node) != &schultz_tab_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    tabs = (schultz_tab_data *)schultz_node_widget_data(tree, node);
    if (index >= tabs->count) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * Not an early return. schultz_tab_view_add calls this to hide the pages
     * it did not just add, and the tab it adds is often the one already
     * chosen, so the loop below has to run either way. What is gated is the
     * layout, which is the part that costs.
     */
    changed = (tabs->selected != index);
    tabs->selected = index;

    for (i = 0; i < tabs->count; i++) {
        uint32_t state = schultz_node_get_state(tree, tabs->pages[i]);

        schultz_node_set_state(tree, tabs->pages[i],
            (i == index) ? (state | SCHULTZ_STATE_VISIBLE)
                         : (state & ~(uint32_t)SCHULTZ_STATE_VISIBLE));
        /* The chosen tab's button shows as chosen. */
        schultz_node_set_state(tree, tabs->buttons[i],
            (i == index)
                ? (schultz_node_get_state(tree, tabs->buttons[i]) |
                   SCHULTZ_STATE_SELECTED)
                : (schultz_node_get_state(tree, tabs->buttons[i]) &
                   ~(uint32_t)SCHULTZ_STATE_SELECTED));
    }
    if (!changed) {
        return SCHULTZ_OK;
    }
    schultz_node_invalidate_layout(tree, node);
    return schultz_node_invalidate(tree, node);
}

uint32_t schultz_tab_view_selected(const schultz_tree *tree,
                                   schultz_handle node)
{
    const schultz_tab_data *tabs;

    if (schultz_node_widget(tree, node) != &schultz_tab_widget) {
        return 0u;
    }
    tabs = (const schultz_tab_data *)schultz_node_widget_data(tree, node);
    return (tabs == NULL) ? 0u : tabs->selected;
}

uint32_t schultz_tab_view_count(const schultz_tree *tree, schultz_handle node)
{
    const schultz_tab_data *tabs;

    if (schultz_node_widget(tree, node) != &schultz_tab_widget) {
        return 0u;
    }
    tabs = (const schultz_tab_data *)schultz_node_widget_data(tree, node);
    return (tabs == NULL) ? 0u : tabs->count;
}

/* -------------------------------------------------------------- overlays */

/*
 * Places an overlay next to something.
 *
 * The placing itself belongs to layout, which does it on every pass rather
 * than once here: the window can change size while a menu is open, and on a
 * phone a rotation is a resize and does not dismiss anything. So this hands
 * layout the anchor and lets it keep the overlay where it belongs.
 */
static void schultz_overlay_place(schultz_tree *tree, schultz_handle overlay,
                                  schultz_rect anchor, uint32_t placement,
                                  float gap, int32_t centred, int32_t flips)
{
    schultz_rect at;

    schultz_tree_resolve_styles(tree);
    if (schultz_node_set_anchored(tree, overlay, anchor, placement, gap,
                                  centred, flips) != SCHULTZ_OK) {
        return;
    }
    /* Placed, so arrange what is inside it at the size it just took. */
    if (schultz_node_get_bounds(tree, overlay, &at) == SCHULTZ_OK) {
        schultz_layout_arrange(tree, overlay, at);
    }
}

/* Shows or hides an overlay node and raises or drops its layer with it. */
static int32_t schultz_overlay_show(schultz_tree *tree, schultz_handle node,
                                    int32_t shown, int32_t captures)
{
    uint32_t state = schultz_node_get_state(tree, node);
    uint32_t count = schultz_tree_overlay_count(tree);
    uint32_t i;

    if (shown) {
        schultz_node_set_state(tree, node, state | SCHULTZ_STATE_VISIBLE);
        /* Already raised: leave it where it is rather than stacking twice. */
        for (i = 0; i < count; i++) {
            schultz_handle at = SCHULTZ_HANDLE_NONE;

            if (schultz_tree_overlay_at(tree, i, &at, NULL) == SCHULTZ_OK &&
                at == node) {
                return SCHULTZ_OK;
            }
        }
        return schultz_tree_push_overlay(tree, node, captures);
    }

    schultz_node_set_state(tree, node,
                           state & ~(uint32_t)SCHULTZ_STATE_VISIBLE);
    /*
     * Released with it. A closed overlay has nothing to be beside, and
     * leaving the anchor on would have layout measure it again every time
     * the window changed size, for a node nobody can see.
     */
    schultz_node_clear_anchored(tree, node);
    /*
     * Only the topmost layer can be dropped, which is the right rule: a menu
     * with a submenu open closes the submenu first.
     */
    while (schultz_tree_overlay_count(tree) > 0u) {
        schultz_handle top = SCHULTZ_HANDLE_NONE;

        if (schultz_tree_overlay_at(tree, schultz_tree_overlay_count(tree) - 1u,
                                    &top, NULL) != SCHULTZ_OK) {
            break;
        }
        schultz_tree_pop_overlay(tree, NULL);
        if (top == node) {
            break;
        }
    }
    return schultz_node_invalidate(tree, node);
}

/* ------------------------------------------------------------- MenuItem */

/** One row of a menu: what it says, and what it says about its shortcut. */
typedef struct {
    schultz_handle menu;     /**< The menu it belongs to. */
    schultz_handle label;    /**< What it says, or none for the other kinds. */
    schultz_handle shortcut; /**< The shortcut text, or none. */
    uint32_t       kind;     /**< One of SCHULTZ_MENU_ITEM_*. */
    uint32_t       group;    /**< Radio rows sharing a menu and this are one
                                  set. */
    schultz_handle submenu;  /**< The menu a submenu row opens, or none. */
    uint32_t       hover_ms; /**< How long the pointer has been on this row. */
} schultz_menu_item_data;

static const schultz_widget_vtable schultz_menu_item_widget;
static const schultz_widget_vtable schultz_menu_widget;

static void schultz_menu_tell_owner(schultz_tree *tree, schultz_handle menu,
                                    const schultz_event *event);
int32_t schultz_menu_radio_select(schultz_tree *tree, schultz_handle item);

/*
 * A label on the left and the shortcut text on the right, with whatever room
 * is left between them. Two children in fixed places is less machinery than a
 * pane, and a menu row is never anything else.
 */
/** How wide the mark column is on a row that has one. */
static float schultz_menu_item_mark(const schultz_menu_item_data *item)
{
    return (item->kind == SCHULTZ_MENU_ITEM_CHECK ||
            item->kind == SCHULTZ_MENU_ITEM_RADIO) ? 16.0f : 0.0f;
}

/** How wide the arrow column is on a row that opens a submenu. */
static float schultz_menu_item_arrow(const schultz_menu_item_data *item)
{
    return (item->kind == SCHULTZ_MENU_ITEM_SUBMENU) ? 14.0f : 0.0f;
}

static int32_t schultz_menu_item_measure(schultz_tree *tree,
                                         schultz_handle node, float avail_w,
                                         float avail_h,
                                         schultz_size *out_size)
{
    const schultz_menu_item_data *item =
        (const schultz_menu_item_data *)schultz_node_widget_data(tree, node);
    schultz_size label = { 0.0f, 0.0f };
    schultz_size shortcut = { 0.0f, 0.0f };
    float padding = 0.0f;
    float gap = 0.0f;

    if (item == NULL) {
        *out_size = schultz_size_make(0.0f, 0.0f);
        return SCHULTZ_OK;
    }
    schultz_node_get_spacing(tree, node, &padding, &gap);

    if (item->kind == SCHULTZ_MENU_ITEM_SEPARATOR) {
        /* A rule and the air around it, and nothing else. */
        *out_size = schultz_size_make(0.0f, padding * 2.0f + 1.0f);
        return SCHULTZ_OK;
    }
    if (item->kind == SCHULTZ_MENU_ITEM_CUSTOM) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;
        schultz_size inner = { 0.0f, 0.0f };

        if (schultz_node_child_at(tree, node, 0, &child) == SCHULTZ_OK) {
            schultz_layout_measure(tree, child, avail_w, avail_h, &inner);
        }
        *out_size = schultz_size_make(inner.width + padding * 2.0f,
                                      inner.height + padding * 2.0f);
        return SCHULTZ_OK;
    }

    schultz_layout_measure(tree, item->label, avail_w, avail_h, &label);
    if (item->shortcut != SCHULTZ_HANDLE_NONE) {
        schultz_layout_measure(tree, item->shortcut, avail_w, avail_h,
                               &shortcut);
    }
    *out_size = schultz_size_make(
        schultz_menu_item_mark(item) + label.width + gap + shortcut.width +
            schultz_menu_item_arrow(item) + padding * 2.0f,
        label.height + padding * 2.0f);
    return SCHULTZ_OK;
}

static int32_t schultz_menu_item_arrange(schultz_tree *tree,
                                         schultz_handle node,
                                         schultz_rect rect)
{
    schultz_menu_item_data *item =
        (schultz_menu_item_data *)schultz_node_widget_data(tree, node);
    schultz_size shortcut = { 0.0f, 0.0f };
    float padding = 0.0f;

    if (item == NULL) {
        return SCHULTZ_OK;
    }
    schultz_node_get_spacing(tree, node, &padding, NULL);

    if (item->kind == SCHULTZ_MENU_ITEM_SEPARATOR) {
        return SCHULTZ_OK;
    }
    if (item->kind == SCHULTZ_MENU_ITEM_CUSTOM) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(tree, node, 0, &child) != SCHULTZ_OK) {
            return SCHULTZ_OK;
        }
        return schultz_layout_arrange(tree, child,
            schultz_rect_make(padding, padding,
                              rect.width - padding * 2.0f,
                              rect.height - padding * 2.0f));
    }
    if (item->shortcut != SCHULTZ_HANDLE_NONE) {
        schultz_layout_measure(tree, item->shortcut, rect.width, -1.0f,
                               &shortcut);
        schultz_layout_arrange(tree, item->shortcut,
            schultz_rect_make(rect.width - padding - shortcut.width, padding,
                              shortcut.width, shortcut.height));
    }
    {
        float mark = schultz_menu_item_mark(item);

        return schultz_layout_arrange(tree, item->label,
            schultz_rect_make(padding + mark, padding,
                              rect.width - padding * 2.0f - mark
                                  - shortcut.width
                                  - schultz_menu_item_arrow(item),
                              rect.height - padding * 2.0f));
    }
}

static const schultz_pane_vtable schultz_menu_item_pane = {
    schultz_menu_item_measure, schultz_menu_item_arrange
};

static int32_t schultz_menu_item_paint(schultz_tree *tree,
                                       schultz_handle node,
                                       schultz_draw_list *list,
                                       schultz_arena *arena)
{
    const schultz_menu_item_data *item =
        (const schultz_menu_item_data *)schultz_node_widget_data(tree, node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    uint32_t state = schultz_node_get_state(tree, node);
    schultz_rect bounds;
    float padding = 0.0f;
    int32_t result;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        item == NULL) {
        return SCHULTZ_OK;
    }
    schultz_node_get_spacing(tree, node, &padding, NULL);

    if (item->kind == SCHULTZ_MENU_ITEM_SEPARATOR) {
        schultz_stroke rule = schultz_widget_stroke(style);
        float y = bounds.y + bounds.height * 0.5f;

        rule.paint = schultz_paint_solid(
            schultz_resolved_color(style, SCHULTZ_PROP_BORDER_COLOR));
        rule.width = 1.0f;
        rule.dash  = SCHULTZ_HANDLE_NONE;
        return schultz_draw_line(list,
            schultz_point_make(bounds.x + padding, y),
            schultz_point_make(bounds.x + bounds.width - padding, y), rule);
    }

    result = schultz_widget_draw_box(list, style, bounds);
    if (result != SCHULTZ_OK) {
        return result;
    }

    if ((item->kind == SCHULTZ_MENU_ITEM_CHECK ||
         item->kind == SCHULTZ_MENU_ITEM_RADIO) &&
        (state & SCHULTZ_STATE_CHECKED)) {
        schultz_color ink = schultz_resolved_color(style,
                                                   SCHULTZ_PROP_TEXT_COLOR);
        float cx = bounds.x + padding + 6.0f;
        float cy = bounds.y + bounds.height * 0.5f;

        if (item->kind == SCHULTZ_MENU_ITEM_RADIO) {
            /* A dot, which is what a radio is everywhere else. */
            result = schultz_draw_fill_ellipse(list,
                schultz_rect_make(cx - 3.0f, cy - 3.0f, 6.0f, 6.0f),
                schultz_paint_solid(ink));
        } else {
            /* A tick, as two strokes, matching the checkbox. */
            schultz_stroke mark = schultz_widget_stroke(style);

            mark.paint = schultz_paint_solid(ink);
            mark.width = 2.0f;
            mark.dash  = SCHULTZ_HANDLE_NONE;
            result = schultz_draw_line(list,
                schultz_point_make(cx - 4.0f, cy),
                schultz_point_make(cx - 1.0f, cy + 3.0f), mark);
            if (result == SCHULTZ_OK) {
                result = schultz_draw_line(list,
                    schultz_point_make(cx - 1.0f, cy + 3.0f),
                    schultz_point_make(cx + 4.0f, cy - 4.0f), mark);
            }
        }
        if (result != SCHULTZ_OK) {
            return result;
        }
    }

    if (item->kind == SCHULTZ_MENU_ITEM_SUBMENU) {
        /* A triangle pointing the way the submenu opens. */
        schultz_point tip[3];
        float x = bounds.x + bounds.width - padding - 8.0f;
        float cy = bounds.y + bounds.height * 0.5f;

        tip[0] = schultz_point_make(x, cy - 4.0f);
        tip[1] = schultz_point_make(x + 5.0f, cy);
        tip[2] = schultz_point_make(x, cy + 4.0f);
        return schultz_draw_fill_polygon(list, tip, 3u,
            schultz_paint_solid(schultz_resolved_color(style,
                SCHULTZ_PROP_TEXT_COLOR)), SCHULTZ_FILL_NONZERO);
    }
    return SCHULTZ_OK;
}

/*
 * Choosing an item closes the menu it belongs to. The click is not consumed:
 * closing is the menu's business and acting on the choice is the host's.
 */
static int32_t schultz_menu_item_event(schultz_tree *tree,
                                       schultz_handle node,
                                       const schultz_event *event)
{
    schultz_menu_item_data *item =
        (schultz_menu_item_data *)schultz_node_widget_data(tree, node);

    if (item == NULL) {
        return SCHULTZ_OK;
    }

    /* A row that opens onto another menu, by pointer or by key. */
    if (item->kind == SCHULTZ_MENU_ITEM_SUBMENU) {
        if (event->type == SCHULTZ_EVENT_MOUSE_LEAVE) {
            item->hover_ms = 0u;
            return SCHULTZ_OK;
        }
        if (event->type == SCHULTZ_EVENT_CLICK ||
            (event->type == SCHULTZ_EVENT_KEY_DOWN &&
             event->key == (uint32_t)SCHULTZ_KEY_RIGHT)) {
            schultz_menu_open_for(tree, item->submenu, node,
                                  SCHULTZ_PLACE_RIGHT);
            return SCHULTZ_EVENT_CONSUMED;
        }
        return SCHULTZ_OK;
    }

    if (event->type != SCHULTZ_EVENT_CLICK) {
        return SCHULTZ_OK;
    }

    /*
     * A tick or a dot changes and the menu stays open, because these are the
     * rows a user sets several of at once. Everything else closes.
     */
    if (item->kind == SCHULTZ_MENU_ITEM_CHECK) {
        schultz_node_set_state(tree, node,
            schultz_node_get_state(tree, node) ^ SCHULTZ_STATE_CHECKED);
        return SCHULTZ_OK;
    }
    if (item->kind == SCHULTZ_MENU_ITEM_RADIO) {
        schultz_menu_radio_select(tree, node);
        return SCHULTZ_OK;
    }

    schultz_menu_close(tree, item->menu);
    schultz_menu_tell_owner(tree, item->menu, event);
    return SCHULTZ_OK;
}

/** How long the pointer rests on a submenu row before it opens. */
#define SCHULTZ_SUBMENU_DELAY_MS 300u

/*
 * A submenu opens on its own once the pointer has stayed put, which is what
 * every menu bar does and what stops one flashing open while the pointer is
 * only passing through on its way down the list.
 */
static int32_t schultz_menu_item_tick(schultz_tree *tree, schultz_handle node,
                                      uint64_t now_ms, uint32_t elapsed_ms)
{
    schultz_menu_item_data *item =
        (schultz_menu_item_data *)schultz_node_widget_data(tree, node);

    (void)now_ms;
    if (item == NULL || item->kind != SCHULTZ_MENU_ITEM_SUBMENU) {
        return 0;
    }
    if (!(schultz_node_get_state(tree, node) & SCHULTZ_STATE_HOVERED)) {
        item->hover_ms = 0u;
        return 0;
    }
    if (schultz_menu_is_open(tree, item->submenu)) {
        return 0;
    }
    item->hover_ms += elapsed_ms;
    if (item->hover_ms >= SCHULTZ_SUBMENU_DELAY_MS) {
        schultz_menu_open_for(tree, item->submenu, node,
                              SCHULTZ_PLACE_RIGHT);
        return 1;
    }
    return 0;
}

static const schultz_widget_vtable schultz_menu_item_widget = {
    .paint = schultz_menu_item_paint,
    .event = schultz_menu_item_event,
    .tick = schultz_menu_item_tick,
    .destroy = free
};

/* ----------------------------------------------------------------- Menu */

/** A menu is a panel of items that lives on an overlay layer. */
typedef struct {
    uint32_t       open;  /**< Nonzero while it is showing. */
    float          width; /**< Forced width, or zero to fit its rows. */
    /**
     * The widget the menu belongs to, if any. A menu hangs off the root, so
     * a choice made in it does not reach whatever opened it by rising through
     * the tree the way an ordinary click does. This is how it gets there.
     */
    schultz_handle owner;
} schultz_menu_data;

static int32_t schultz_menu_paint(schultz_tree *tree, schultz_handle node,
                                  schultz_draw_list *list,
                                  schultz_arena *arena)
{
    schultz_rect bounds;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    return schultz_widget_draw_box(list, schultz_widget_style(tree, node),
                                   bounds);
}

/* A press outside a capturing overlay arrives here as a dismissal. */
static int32_t schultz_menu_event(schultz_tree *tree, schultz_handle node,
                                  const schultz_event *event)
{
    if (event->type == SCHULTZ_EVENT_DISMISS ||
        (event->type == SCHULTZ_EVENT_KEY_DOWN &&
         event->key == (uint32_t)SCHULTZ_KEY_ESCAPE)) {
        schultz_menu_close(tree, node);
        return SCHULTZ_EVENT_CONSUMED;
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_menu_widget = {
    .paint = schultz_menu_paint, .event = schultz_menu_event, .destroy = free
};

/*
 * Hands an event to the widget that owns a menu. Only a combo box uses this,
 * and only because a menu deliberately does not live inside whatever opened
 * it.
 */
static void schultz_menu_tell_owner(schultz_tree *tree, schultz_handle menu,
                                    const schultz_event *event)
{
    const schultz_menu_data *data;
    const schultz_widget_vtable *widget;

    if (schultz_node_widget(tree, menu) != &schultz_menu_widget) {
        return;
    }
    data = (const schultz_menu_data *)schultz_node_widget_data(tree, menu);
    if (data == NULL || data->owner == SCHULTZ_HANDLE_NONE) {
        return;
    }
    widget = schultz_node_widget(tree, data->owner);
    /*
     * Not to an owner that has been switched off, for the reason the relay
     * gives: a menu hangs off the root, so disabling the combo box that owns
     * it does not disable its rows, and this path does not go through the
     * router that would otherwise refuse them.
     */
    if (widget != NULL && widget->event != NULL &&
        schultz_node_is_enabled(tree, data->owner)) {
        widget->event(tree, data->owner, event);
    }
}

/*
 * Makes a menu exactly this wide rather than as wide as its widest row. A
 * combo box wants its menu to line up with the button that opened it, which
 * is what every other toolkit does and what makes the two read as one thing.
 */
static void schultz_menu_set_width(schultz_tree *tree, schultz_handle menu,
                                   float width)
{
    schultz_menu_data *data;

    if (schultz_node_widget(tree, menu) != &schultz_menu_widget) {
        return;
    }
    data = (schultz_menu_data *)schultz_node_widget_data(tree, menu);
    if (data != NULL && data->width != width) {
        data->width = width;
        schultz_node_set_style_property(tree, menu, SCHULTZ_PROP_PREF_WIDTH,
                                        schultz_value_number(width));
        schultz_node_invalidate_layout(tree, menu);
    }
}

/* Says which widget a menu belongs to, so choices made in it get back. */
static void schultz_menu_set_owner(schultz_tree *tree, schultz_handle menu,
                                   schultz_handle owner)
{
    schultz_menu_data *data;

    if (schultz_node_widget(tree, menu) != &schultz_menu_widget) {
        return;
    }
    data = (schultz_menu_data *)schultz_node_widget_data(tree, menu);
    if (data != NULL) {
        data->owner = owner;
    }
}

int32_t schultz_menu_create(schultz_tree *tree, schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_menu_data *menu;
    schultz_patch patch;
    uint32_t state;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * A menu hangs off the root rather than off whatever opened it, because
     * an overlay is its own layout root and its coordinates are the window's.
     */
    result = schultz_node_create(tree, schultz_tree_root(tree), &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    menu = (schultz_menu_data *)calloc(1, sizeof(*menu));
    if (menu == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    schultz_node_set_pane(tree, node, schultz_pane_vbox());
    schultz_node_set_widget(tree, node, &schultz_menu_widget, menu);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_MENU);
    /* Hidden until it is opened, so creating one shows nothing. */
    state = schultz_node_get_state(tree, node);
    schultz_node_set_state(tree, node,
                           state & ~(uint32_t)SCHULTZ_STATE_VISIBLE);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_SURFACE_RAISED);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                            SCHULTZ_TOKEN_COLOR_BORDER);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_WIDTH,
                            SCHULTZ_TOKEN_BORDER_WIDTH);
        schultz_patch_token(&patch, SCHULTZ_PROP_CORNER_RADIUS,
                            SCHULTZ_TOKEN_RADIUS_STRUCTURE);
        schultz_patch_token(&patch, SCHULTZ_PROP_PADDING,
                            SCHULTZ_TOKEN_SPACE_XS);
        schultz_widget_default_style(tree, node, &patch);
    }
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_menu_add(schultz_tree *tree, schultz_handle menu,
                         const char *text, const char *shortcut,
                         schultz_handle *out_item)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_menu_item_data *item;
    schultz_patch patch;
    int32_t result;

    if (schultz_node_widget(tree, menu) != &schultz_menu_widget ||
        out_item == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    result = schultz_node_create(tree, menu, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    item = (schultz_menu_item_data *)calloc(1, sizeof(*item));
    if (item == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    item->menu     = menu;
    item->shortcut = SCHULTZ_HANDLE_NONE;

    result = schultz_label_create(tree, node, text, &item->label);
    if (result == SCHULTZ_OK && shortcut != NULL && shortcut[0] != '\0') {
        result = schultz_label_create(tree, node, shortcut, &item->shortcut);
    }
    if (result != SCHULTZ_OK) {
        free(item);
        schultz_node_destroy(tree, node);
        return result;
    }
    schultz_label_set_wrap(tree, item->label, 0);
    schultz_node_set_hit_testable(tree, item->label, 0);
    if (item->shortcut != SCHULTZ_HANDLE_NONE) {
        schultz_label_set_wrap(tree, item->shortcut, 0);
        schultz_node_set_hit_testable(tree, item->shortcut, 0);
        schultz_node_set_style_property(tree, item->shortcut,
            SCHULTZ_PROP_TEXT_COLOR,
            schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_MUTED));
    }

    schultz_node_set_pane(tree, node, &schultz_menu_item_pane);
    schultz_node_set_widget(tree, node, &schultz_menu_item_widget, item);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_MENU_ITEM);
    schultz_node_set_name(tree, node, (text == NULL) ? "" : text);
    schultz_node_set_actions(tree, node,
                             SCHULTZ_ACTION_CLICK | SCHULTZ_ACTION_FOCUS);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_CORNER_RADIUS,
                            SCHULTZ_TOKEN_RADIUS_STRUCTURE);
        schultz_patch_token(&patch, SCHULTZ_PROP_PADDING,
                            SCHULTZ_TOKEN_SPACE_SM);
        schultz_patch_token(&patch, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_LG);
        schultz_widget_default_style(tree, node, &patch);
    }
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_HOVERED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_HOVERED,
        SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_ON_ACCENT));

    schultz_node_invalidate_layout(tree, menu);
    *out_item = node;
    return SCHULTZ_OK;
}

/* The shared tail of every row kind: turn a plain row into another kind. */
static int32_t schultz_menu_item_become(schultz_tree *tree,
                                        schultz_handle item, uint32_t kind,
                                        uint32_t group)
{
    schultz_menu_item_data *data =
        (schultz_menu_item_data *)schultz_node_widget_data(tree, item);

    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data->kind  = kind;
    data->group = group;
    return schultz_node_invalidate_layout(tree, item);
}

int32_t schultz_menu_add_check(schultz_tree *tree, schultz_handle menu,
                               const char *text, const char *shortcut,
                               int32_t checked, schultz_handle *out_item)
{
    int32_t result = schultz_menu_add(tree, menu, text, shortcut, out_item);

    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_menu_item_become(tree, *out_item,
                                      SCHULTZ_MENU_ITEM_CHECK, 0u);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (checked) {
        schultz_node_set_state(tree, *out_item,
            schultz_node_get_state(tree, *out_item) | SCHULTZ_STATE_CHECKED);
    }
    return SCHULTZ_OK;
}

int32_t schultz_menu_add_radio(schultz_tree *tree, schultz_handle menu,
                               const char *text, const char *shortcut,
                               uint32_t group, schultz_handle *out_item)
{
    int32_t result = schultz_menu_add(tree, menu, text, shortcut, out_item);

    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_menu_item_become(tree, *out_item,
                                    SCHULTZ_MENU_ITEM_RADIO, group);
}

int32_t schultz_menu_radio_select(schultz_tree *tree, schultz_handle item)
{
    const schultz_menu_item_data *data =
        (const schultz_menu_item_data *)schultz_node_widget_data(tree, item);
    uint32_t count;
    uint32_t i;

    if (schultz_node_widget(tree, item) != &schultz_menu_item_widget ||
        data == NULL || data->kind != SCHULTZ_MENU_ITEM_RADIO) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* A set is the rows sharing a menu and a group number, which is the same
     * rule schultz_radio_create uses with a parent. */
    count = schultz_node_child_count(tree, data->menu);
    for (i = 0; i < count; i++) {
        schultz_handle row = SCHULTZ_HANDLE_NONE;
        const schultz_menu_item_data *other;

        if (schultz_node_child_at(tree, data->menu, i, &row) != SCHULTZ_OK ||
            row == item ||
            schultz_node_widget(tree, row) != &schultz_menu_item_widget) {
            continue;
        }
        other = (const schultz_menu_item_data *)
            schultz_node_widget_data(tree, row);
        if (other != NULL && other->kind == SCHULTZ_MENU_ITEM_RADIO &&
            other->group == data->group) {
            schultz_node_set_state(tree, row,
                schultz_node_get_state(tree, row)
                    & ~(uint32_t)SCHULTZ_STATE_CHECKED);
        }
    }
    return schultz_node_set_state(tree, item,
        schultz_node_get_state(tree, item) | SCHULTZ_STATE_CHECKED);
}

int32_t schultz_menu_add_separator(schultz_tree *tree, schultz_handle menu,
                                   schultz_handle *out_item)
{
    schultz_handle item = SCHULTZ_HANDLE_NONE;
    int32_t result = schultz_menu_add(tree, menu, "", NULL, &item);

    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_menu_item_become(tree, item,
                                      SCHULTZ_MENU_ITEM_SEPARATOR, 0u);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /*
     * A rule is not a choice: it takes no focus, so keyboard navigation steps
     * over it, and it is invisible to the pointer.
     */
    schultz_node_set_actions(tree, item, 0u);
    schultz_node_set_hit_testable(tree, item, 0);
    if (out_item != NULL) {
        *out_item = item;
    }
    return SCHULTZ_OK;
}

int32_t schultz_menu_add_custom(schultz_tree *tree, schultz_handle menu,
                                schultz_handle *out_content)
{
    schultz_handle item = SCHULTZ_HANDLE_NONE;
    schultz_handle content = SCHULTZ_HANDLE_NONE;
    schultz_menu_item_data *data;
    int32_t result = schultz_menu_add(tree, menu, "", NULL, &item);

    if (result != SCHULTZ_OK) {
        return result;
    }
    /* The caption a plain row was given is not wanted here. */
    data = (schultz_menu_item_data *)schultz_node_widget_data(tree, item);
    if (data != NULL && data->label != SCHULTZ_HANDLE_NONE) {
        schultz_node_destroy(tree, data->label);
        data->label = SCHULTZ_HANDLE_NONE;
    }
    result = schultz_node_create(tree, item, &content);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /* Holds one thing and sizes itself around it, as every content node in
     * the toolkit does. */
    schultz_node_set_pane(tree, content, schultz_pane_stack());
    result = schultz_menu_item_become(tree, item, SCHULTZ_MENU_ITEM_CUSTOM,
                                      0u);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (out_content != NULL) {
        *out_content = content;
    }
    return SCHULTZ_OK;
}

int32_t schultz_menu_add_submenu(schultz_tree *tree, schultz_handle menu,
                                 const char *text,
                                 schultz_handle *out_submenu)
{
    schultz_handle item = SCHULTZ_HANDLE_NONE;
    schultz_handle child = SCHULTZ_HANDLE_NONE;
    schultz_menu_item_data *data;
    int32_t result;

    if (out_submenu == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_menu_create(tree, &child);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_menu_add(tree, menu, text, NULL, &item);
    if (result != SCHULTZ_OK) {
        schultz_node_destroy(tree, child);
        return result;
    }
    data = (schultz_menu_item_data *)schultz_node_widget_data(tree, item);
    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data->submenu = child;
    result = schultz_menu_item_become(tree, item, SCHULTZ_MENU_ITEM_SUBMENU,
                                      0u);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /* It has to be ticked to notice the pointer resting on it. */
    schultz_node_set_animating(tree, item, 1);
    *out_submenu = child;
    return SCHULTZ_OK;
}

int32_t schultz_menu_item_kind(const schultz_tree *tree, schultz_handle item)
{
    const schultz_menu_item_data *data;

    if (schultz_node_widget(tree, item) != &schultz_menu_item_widget) {
        return -1;
    }
    data = (const schultz_menu_item_data *)schultz_node_widget_data(tree,
                                                                    item);
    return (data == NULL) ? -1 : (int32_t)data->kind;
}

schultz_handle schultz_menu_item_label(const schultz_tree *tree,
                                       schultz_handle item)
{
    const schultz_menu_item_data *data;

    if (schultz_node_widget(tree, item) != &schultz_menu_item_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    data = (const schultz_menu_item_data *)schultz_node_widget_data(tree,
                                                                    item);
    return (data == NULL) ? SCHULTZ_HANDLE_NONE : data->label;
}

int32_t schultz_menu_item_checked(const schultz_tree *tree,
                                  schultz_handle item)
{
    if (schultz_node_widget(tree, item) != &schultz_menu_item_widget) {
        return 0;
    }
    return (schultz_node_get_state(tree, item) & SCHULTZ_STATE_CHECKED)
               ? 1 : 0;
}

int32_t schultz_menu_item_set_checked(schultz_tree *tree, schultz_handle item,
                                      int32_t checked)
{
    uint32_t state;

    if (schultz_node_widget(tree, item) != &schultz_menu_item_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    state = schultz_node_get_state(tree, item);
    return schultz_node_set_state(tree, item,
        checked ? (state | SCHULTZ_STATE_CHECKED)
                : (state & ~(uint32_t)SCHULTZ_STATE_CHECKED));
}

int32_t schultz_menu_open(schultz_tree *tree, schultz_handle menu,
                          schultz_rect anchor, uint32_t placement)
{
    schultz_menu_data *data;

    if (schultz_node_widget(tree, menu) != &schultz_menu_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data = (schultz_menu_data *)schultz_node_widget_data(tree, menu);
    data->open = 1u;
    schultz_overlay_show(tree, menu, 1, 1);
    schultz_overlay_place(tree, menu, anchor, placement, 0.0f, 0, 0);
    return schultz_node_invalidate(tree, menu);
}

int32_t schultz_menu_open_at(schultz_tree *tree, schultz_handle menu,
                             schultz_point point)
{
    return schultz_menu_open(tree, menu,
                             schultz_rect_make(point.x, point.y, 0.0f, 0.0f),
                             SCHULTZ_PLACE_OVER);
}

int32_t schultz_menu_open_for(schultz_tree *tree, schultz_handle menu,
                              schultz_handle anchor, uint32_t placement)
{
    schultz_rect bounds;

    if (schultz_node_absolute_bounds(tree, anchor, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_menu_open(tree, menu, bounds, placement);
}

int32_t schultz_menu_close(schultz_tree *tree, schultz_handle menu)
{
    schultz_menu_data *data;

    if (schultz_node_widget(tree, menu) != &schultz_menu_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data = (schultz_menu_data *)schultz_node_widget_data(tree, menu);
    if (!data->open) {
        return SCHULTZ_OK;
    }
    data->open = 0u;
    return schultz_overlay_show(tree, menu, 0, 1);
}

int32_t schultz_menu_is_open(const schultz_tree *tree, schultz_handle menu)
{
    const schultz_menu_data *data;

    if (schultz_node_widget(tree, menu) != &schultz_menu_widget) {
        return 0;
    }
    data = (const schultz_menu_data *)schultz_node_widget_data(tree, menu);
    return (data == NULL) ? 0 : (int32_t)data->open;
}

/* -------------------------------------------------------- MenuButton */

/** A button that opens a menu instead of reporting its click. */
typedef struct {
    schultz_handle menu;  /**< The menu it owns. */
    uint32_t       split; /**< Nonzero when the arrow is a separate part. */
} schultz_menu_button_data;

static const schultz_widget_vtable schultz_menu_button_widget;

/** How wide the arrow part of a split button is. */
#define SCHULTZ_SPLIT_ARROW 22.0f

static int32_t schultz_menu_button_paint(schultz_tree *tree,
                                         schultz_handle node,
                                         schultz_draw_list *list,
                                         schultz_arena *arena)
{
    const schultz_menu_button_data *data =
        (const schultz_menu_button_data *)schultz_node_widget_data(tree,
                                                                   node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_rect bounds;
    schultz_point tip[3];
    float x;
    float cy;
    int32_t result;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        data == NULL) {
        return SCHULTZ_OK;
    }
    result = schultz_widget_draw_box(list, style, bounds);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_widget_draw_focus_ring(list, style, bounds,
                                            schultz_node_get_state(tree,
                                                                   node));
    if (result != SCHULTZ_OK) {
        return result;
    }

    cy = bounds.y + bounds.height * 0.5f;
    if (data->split) {
        /* A rule marks where the button ends and the arrow begins, so it is
         * clear that the two halves do different things. */
        schultz_stroke rule = schultz_widget_stroke(style);

        x = bounds.x + bounds.width - SCHULTZ_SPLIT_ARROW;
        rule.paint = schultz_paint_solid(
            schultz_resolved_color(style, SCHULTZ_PROP_BORDER_COLOR));
        rule.width = 1.0f;
        rule.dash  = SCHULTZ_HANDLE_NONE;
        result = schultz_draw_line(list,
            schultz_point_make(x, bounds.y + 4.0f),
            schultz_point_make(x, bounds.y + bounds.height - 4.0f), rule);
        if (result != SCHULTZ_OK) {
            return result;
        }
        x += SCHULTZ_SPLIT_ARROW * 0.5f;
    } else {
        x = bounds.x + bounds.width - 12.0f;
    }

    /* A triangle pointing down, which is what says "there is more here". */
    tip[0] = schultz_point_make(x - 4.0f, cy - 2.0f);
    tip[1] = schultz_point_make(x + 4.0f, cy - 2.0f);
    tip[2] = schultz_point_make(x, cy + 3.0f);
    return schultz_draw_fill_polygon(list, tip, 3u,
        schultz_paint_solid(schultz_resolved_color(style,
                                                   SCHULTZ_PROP_TEXT_COLOR)),
        SCHULTZ_FILL_NONZERO);
}

/*
 * The caption gets the button less the arrow column, so the two never sit on
 * top of each other. A stack pane would centre the caption across the whole
 * width, which puts the last letter under the arrow.
 */
static float schultz_menu_button_gutter(const schultz_tree *tree,
                                        schultz_handle node)
{
    const schultz_menu_button_data *data =
        (const schultz_menu_button_data *)schultz_node_widget_data(tree,
                                                                   node);

    if (data == NULL) {
        return 0.0f;
    }
    return data->split ? SCHULTZ_SPLIT_ARROW : 18.0f;
}

static int32_t schultz_menu_button_measure(schultz_tree *tree,
                                           schultz_handle node, float avail_w,
                                           float avail_h,
                                           schultz_size *out_size)
{
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_size text = { 0.0f, 0.0f };
    float padding = 0.0f;

    schultz_node_get_spacing(tree, node, &padding, NULL);
    if (schultz_node_child_at(tree, node, 0, &label) == SCHULTZ_OK) {
        schultz_layout_measure(tree, label, avail_w, avail_h, &text);
    }
    *out_size = schultz_size_make(
        text.width + schultz_menu_button_gutter(tree, node) + padding * 2.0f,
        text.height + padding * 2.0f);
    return SCHULTZ_OK;
}

static int32_t schultz_menu_button_arrange(schultz_tree *tree,
                                           schultz_handle node,
                                           schultz_rect rect)
{
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    float padding = 0.0f;

    if (schultz_node_child_at(tree, node, 0, &label) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    schultz_node_get_spacing(tree, node, &padding, NULL);
    return schultz_layout_arrange(tree, label,
        schultz_rect_make(padding, padding,
                          rect.width - padding * 2.0f
                              - schultz_menu_button_gutter(tree, node),
                          rect.height - padding * 2.0f));
}

static const schultz_pane_vtable schultz_menu_button_pane = {
    schultz_menu_button_measure, schultz_menu_button_arrange
};

static int32_t schultz_menu_button_event(schultz_tree *tree,
                                         schultz_handle node,
                                         const schultz_event *event)
{
    const schultz_menu_button_data *data =
        (const schultz_menu_button_data *)schultz_node_widget_data(tree,
                                                                   node);
    schultz_rect bounds;

    if (data == NULL || event->type != SCHULTZ_EVENT_CLICK) {
        return SCHULTZ_OK;
    }
    if (data->split &&
        schultz_node_absolute_bounds(tree, node, &bounds) == SCHULTZ_OK &&
        event->local.x < bounds.width - SCHULTZ_SPLIT_ARROW) {
        /*
         * The wide part of a split button is an ordinary button: the click
         * goes to the host and no menu opens.
         */
        return SCHULTZ_OK;
    }
    schultz_menu_open_for(tree, data->menu, node, SCHULTZ_PLACE_BELOW);
    return SCHULTZ_EVENT_CONSUMED;
}

static const schultz_widget_vtable schultz_menu_button_widget = {
    .paint = schultz_menu_button_paint,
    .event = schultz_menu_button_event,
    .destroy = free
};

/* The shared body of the two menu buttons. */
static int32_t schultz_menu_button_build(schultz_tree *tree,
                                         schultz_handle parent,
                                         const char *text, uint32_t split,
                                         schultz_handle *out_node)
{
    schultz_menu_button_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle menu = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_menu_create(tree, &menu);
    if (result != SCHULTZ_OK) {
        return result;
    }
    data = (schultz_menu_button_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        schultz_node_destroy(tree, menu);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->menu  = menu;
    data->split = split;

    result = schultz_button_create(tree, parent, text, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        schultz_node_destroy(tree, menu);
        return result;
    }
    schultz_node_set_widget(tree, node, &schultz_menu_button_widget, data);
    /* Its own pane, so the caption keeps clear of the arrow. */
    schultz_node_set_pane(tree, node, &schultz_menu_button_pane);

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_menu_button_create(schultz_tree *tree, schultz_handle parent,
                                   const char *text, schultz_handle *out_node)
{
    return schultz_menu_button_build(tree, parent, text, 0u, out_node);
}

int32_t schultz_split_menu_button_create(schultz_tree *tree,
                                         schultz_handle parent,
                                         const char *text,
                                         schultz_handle *out_node)
{
    return schultz_menu_button_build(tree, parent, text, 1u, out_node);
}

schultz_handle schultz_menu_button_menu(const schultz_tree *tree,
                                        schultz_handle node)
{
    const schultz_menu_button_data *data;

    if (schultz_node_widget(tree, node) != &schultz_menu_button_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    data = (const schultz_menu_button_data *)schultz_node_widget_data(tree,
                                                                      node);
    return (data == NULL) ? SCHULTZ_HANDLE_NONE : data->menu;
}

/* ------------------------------------------------------------ MenuBar */

/** A row of menu titles. What it is doing is read off the menus themselves. */
typedef struct {
    uint32_t unused; /**< Nothing yet; the menus carry the state. */
} schultz_menu_bar_data;

static const schultz_widget_vtable schultz_menu_bar_widget;
static const schultz_widget_vtable schultz_menu_title_widget;

/** One title in a bar, and the menu it drops. */
typedef struct {
    schultz_handle bar;  /**< The bar it sits in. */
    schultz_handle menu; /**< The menu it opens. */
} schultz_menu_title_data;

static int32_t schultz_menu_bar_paint(schultz_tree *tree, schultz_handle node,
                                      schultz_draw_list *list,
                                      schultz_arena *arena)
{
    schultz_rect bounds;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    return schultz_widget_draw_box(list, schultz_widget_style(tree, node),
                                   bounds);
}

static const schultz_widget_vtable schultz_menu_bar_widget = {
    .paint = schultz_menu_bar_paint, .destroy = free
};

static int32_t schultz_menu_title_paint(schultz_tree *tree,
                                        schultz_handle node,
                                        schultz_draw_list *list,
                                        schultz_arena *arena)
{
    schultz_rect bounds;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    return schultz_widget_draw_box(list, schultz_widget_style(tree, node),
                                   bounds);
}

/*
 * Once one menu in a bar is showing, moving along the bar shows the next
 * without a click. That is what makes a menu bar feel like one control rather
 * than a row of buttons, and it is the part four hand written bars would each
 * get differently.
 */
/* Nonzero while any of a bar's menus is showing. */
static int32_t schultz_menu_bar_showing(schultz_tree *tree,
                                        schultz_handle bar)
{
    uint32_t count = schultz_node_child_count(tree, bar);
    uint32_t i;

    for (i = 0; i < count; i++) {
        schultz_handle item = SCHULTZ_HANDLE_NONE;
        const schultz_menu_title_data *title;

        if (schultz_node_child_at(tree, bar, i, &item) != SCHULTZ_OK ||
            schultz_node_widget(tree, item) != &schultz_menu_title_widget) {
            continue;
        }
        title = (const schultz_menu_title_data *)
            schultz_node_widget_data(tree, item);
        if (title != NULL && schultz_menu_is_open(tree, title->menu)) {
            return 1;
        }
    }
    return 0;
}

static int32_t schultz_menu_title_event(schultz_tree *tree,
                                        schultz_handle node,
                                        const schultz_event *event)
{
    const schultz_menu_title_data *title =
        (const schultz_menu_title_data *)schultz_node_widget_data(tree, node);

    if (title == NULL) {
        return SCHULTZ_OK;
    }

    if (event->type == SCHULTZ_EVENT_CLICK) {
        if (schultz_menu_is_open(tree, title->menu)) {
            schultz_menu_close(tree, title->menu);
        } else {
            schultz_menu_bar_close(tree, title->bar);
            schultz_menu_open_for(tree, title->menu, node,
                                  SCHULTZ_PLACE_BELOW);
        }
        return SCHULTZ_EVENT_CONSUMED;
    }
    /*
     * Moving along the bar follows only while a menu is already showing.
     * Read off the menus rather than remembered, because a menu can close
     * without the bar hearing about it: choosing a row closes it, and so does
     * a press outside. A remembered flag survives both and leaves the bar
     * opening menus at every passing pointer.
     */
    if (event->type == SCHULTZ_EVENT_MOUSE_ENTER &&
        !schultz_menu_is_open(tree, title->menu) &&
        schultz_menu_bar_showing(tree, title->bar)) {
        schultz_menu_bar_close(tree, title->bar);
        schultz_menu_open_for(tree, title->menu, node, SCHULTZ_PLACE_BELOW);
        return SCHULTZ_EVENT_CONSUMED;
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_menu_title_widget = {
    .paint = schultz_menu_title_paint,
    .event = schultz_menu_title_event,
    .destroy = free
};

int32_t schultz_menu_bar_create(schultz_tree *tree, schultz_handle parent,
                                schultz_handle *out_node)
{
    schultz_menu_bar_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_menu_bar_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    schultz_node_set_pane(tree, node, schultz_pane_hbox());
    schultz_node_set_widget(tree, node, &schultz_menu_bar_widget, data);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_MENU);
    schultz_node_set_spacing(tree, node, 2.0f, 2.0f);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_SURFACE);
        schultz_widget_default_style(tree, node, &patch);
    }

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_menu_bar_add(schultz_tree *tree, schultz_handle node,
                             const char *text, schultz_handle *out_menu)
{
    schultz_menu_title_data *title;
    schultz_handle item = SCHULTZ_HANDLE_NONE;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_handle menu = SCHULTZ_HANDLE_NONE;
    schultz_patch patch;
    int32_t result;

    if (schultz_node_widget(tree, node) != &schultz_menu_bar_widget ||
        out_menu == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    result = schultz_menu_create(tree, &menu);
    if (result != SCHULTZ_OK) {
        return result;
    }
    title = (schultz_menu_title_data *)calloc(1, sizeof(*title));
    if (title == NULL) {
        schultz_node_destroy(tree, menu);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    title->bar  = node;
    title->menu = menu;

    result = schultz_node_create(tree, node, &item);
    if (result != SCHULTZ_OK) {
        free(title);
        schultz_node_destroy(tree, menu);
        return result;
    }
    result = schultz_label_create(tree, item, text, &label);
    if (result != SCHULTZ_OK) {
        free(title);
        schultz_node_destroy(tree, item);
        schultz_node_destroy(tree, menu);
        return result;
    }
    schultz_label_set_wrap(tree, label, 0);
    schultz_node_set_hit_testable(tree, label, 0);

    schultz_node_set_pane(tree, item, schultz_pane_stack());
    schultz_node_set_widget(tree, item, &schultz_menu_title_widget, title);
    schultz_node_set_role(tree, item, SCHULTZ_ROLE_MENU_ITEM);
    schultz_node_set_name(tree, item, (text == NULL) ? "" : text);
    schultz_node_set_actions(tree, item,
                             SCHULTZ_ACTION_CLICK | SCHULTZ_ACTION_FOCUS);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_PADDING,
                            SCHULTZ_TOKEN_SPACE_SM);
        schultz_widget_default_style(tree, item, &patch);
    }
    schultz_node_set_state_property(tree, item, SCHULTZ_STYLE_STATE_HOVERED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_SURFACE_RAISED));

    *out_menu = menu;
    return SCHULTZ_OK;
}

int32_t schultz_menu_bar_close(schultz_tree *tree, schultz_handle node)
{
    schultz_menu_bar_data *data = NULL;
    uint32_t count;
    uint32_t i;

    if (schultz_node_widget(tree, node) != &schultz_menu_bar_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    count = schultz_node_child_count(tree, node);
    for (i = 0; i < count; i++) {
        schultz_handle item = SCHULTZ_HANDLE_NONE;
        const schultz_menu_title_data *title;

        if (schultz_node_child_at(tree, node, i, &item) != SCHULTZ_OK ||
            schultz_node_widget(tree, item) != &schultz_menu_title_widget) {
            continue;
        }
        title = (const schultz_menu_title_data *)
            schultz_node_widget_data(tree, item);
        if (title != NULL) {
            schultz_menu_close(tree, title->menu);
        }
    }
    (void)data;
    return SCHULTZ_OK;
}

/* ------------------------------------------------------------ Accordion */

/** One section of an accordion: a header that opens and closes a body. */
typedef struct {
    schultz_handle accordion; /**< The accordion it sits in. */
    schultz_handle body;      /**< What opens and closes. */
    uint32_t       open;      /**< Nonzero while the body is showing. */
} schultz_section_data;

/** An accordion's rule about how many sections may be open at once. */
typedef struct {
    uint32_t single; /**< Nonzero when opening one closes the rest. */
} schultz_accordion_data;

static const schultz_widget_vtable schultz_accordion_widget;
static const schultz_widget_vtable schultz_section_widget;

/* Draws the header's box and the triangle that says which way it is. */
static int32_t schultz_section_paint(schultz_tree *tree, schultz_handle node,
                                     schultz_draw_list *list,
                                     schultz_arena *arena)
{
    const schultz_section_data *section =
        (const schultz_section_data *)schultz_node_widget_data(tree, node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_rect bounds;
    schultz_point tip[3];
    float x;
    float cy;
    int32_t result;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        section == NULL) {
        return SCHULTZ_OK;
    }
    result = schultz_widget_draw_box(list, style, bounds);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_widget_draw_focus_ring(list, style, bounds,
                                            schultz_node_get_state(tree,
                                                                   node));
    if (result != SCHULTZ_OK) {
        return result;
    }

    x  = bounds.x + 10.0f;
    cy = bounds.y + bounds.height * 0.5f;
    if (section->open) {
        /* Pointing down: what is under this is showing. */
        tip[0] = schultz_point_make(x - 4.0f, cy - 2.0f);
        tip[1] = schultz_point_make(x + 4.0f, cy - 2.0f);
        tip[2] = schultz_point_make(x, cy + 3.0f);
    } else {
        tip[0] = schultz_point_make(x - 2.0f, cy - 4.0f);
        tip[1] = schultz_point_make(x + 3.0f, cy);
        tip[2] = schultz_point_make(x - 2.0f, cy + 4.0f);
    }
    return schultz_draw_fill_polygon(list, tip, 3u,
        schultz_paint_solid(schultz_resolved_color(style,
                                                   SCHULTZ_PROP_TEXT_COLOR)),
        SCHULTZ_FILL_NONZERO);
}

static int32_t schultz_section_event(schultz_tree *tree, schultz_handle node,
                                     const schultz_event *event)
{
    const schultz_section_data *section =
        (const schultz_section_data *)schultz_node_widget_data(tree, node);

    if (section == NULL || event->type != SCHULTZ_EVENT_CLICK) {
        return SCHULTZ_OK;
    }
    schultz_accordion_expand(tree, section->accordion, node, !section->open);
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_section_widget = {
    .paint = schultz_section_paint,
    .event = schultz_section_event,
    .destroy = free
};

static const schultz_widget_vtable schultz_accordion_widget = {
    .destroy = free
};

int32_t schultz_accordion_create(schultz_tree *tree, schultz_handle parent,
                                 schultz_handle *out_node)
{
    schultz_accordion_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_accordion_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    /* A column of headers and bodies, in the order they were added. */
    schultz_node_set_pane(tree, node, schultz_pane_vbox());
    schultz_node_set_widget(tree, node, &schultz_accordion_widget, data);
    schultz_node_set_spacing(tree, node, 0.0f, 2.0f);

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_accordion_add(schultz_tree *tree, schultz_handle node,
                              const char *title, schultz_handle *out_content)
{
    schultz_section_data *section;
    schultz_handle header = SCHULTZ_HANDLE_NONE;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_patch patch;
    int32_t result;

    if (schultz_node_widget(tree, node) != &schultz_accordion_widget ||
        out_content == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    section = (schultz_section_data *)calloc(1, sizeof(*section));
    if (section == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    result = schultz_node_create(tree, node, &header);
    if (result != SCHULTZ_OK) {
        free(section);
        return result;
    }
    result = schultz_label_create(tree, header, title, &label);
    if (result != SCHULTZ_OK) {
        free(section);
        schultz_node_destroy(tree, header);
        return result;
    }
    schultz_label_set_wrap(tree, label, 0);
    schultz_node_set_hit_testable(tree, label, 0);
    {
        /* Room on the left for the triangle. */
        schultz_layout_params params;

        schultz_layout_params_default(&params);
        params.align = SCHULTZ_ALIGN_CENTER;
        schultz_node_set_layout_params(tree, label, &params);
        schultz_node_set_style_property(tree, label, SCHULTZ_PROP_PADDING,
                                        schultz_value_number(0.0f));
    }
    schultz_node_set_pane(tree, header, schultz_pane_stack());
    schultz_node_set_spacing(tree, header, 6.0f, 0.0f);

    result = schultz_node_create(tree, node, &body);
    if (result != SCHULTZ_OK) {
        free(section);
        schultz_node_destroy(tree, header);
        return result;
    }
    /* Holds one thing and sizes itself around it, as content nodes do. */
    schultz_node_set_pane(tree, body, schultz_pane_stack());
    schultz_node_set_state(tree, body,
        schultz_node_get_state(tree, body) & ~(uint32_t)SCHULTZ_STATE_VISIBLE);

    section->accordion = node;
    section->body      = body;
    section->open      = 0u;

    schultz_node_set_widget(tree, header, &schultz_section_widget, section);
    schultz_node_set_role(tree, header, SCHULTZ_ROLE_BUTTON);
    schultz_node_set_name(tree, header, (title == NULL) ? "" : title);
    schultz_node_set_actions(tree, header,
                             SCHULTZ_ACTION_CLICK | SCHULTZ_ACTION_FOCUS);
    schultz_node_set_paint_margin(tree, header, SCHULTZ_RING_BLEED);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_SURFACE);
        schultz_patch_token(&patch, SCHULTZ_PROP_PADDING,
                            SCHULTZ_TOKEN_SPACE_SM);
        schultz_widget_default_style(tree, header, &patch);
    }
    schultz_node_set_state_property(tree, header, SCHULTZ_STYLE_STATE_HOVERED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_SURFACE_RAISED));

    *out_content = body;
    return SCHULTZ_OK;
}

int32_t schultz_accordion_expand(schultz_tree *tree, schultz_handle node,
                                 schultz_handle header, int32_t expanded)
{
    schultz_section_data *section;
    const schultz_accordion_data *data;
    uint32_t count;
    uint32_t i;
    int32_t  changed;

    if (schultz_node_widget(tree, node) != &schultz_accordion_widget ||
        schultz_node_widget(tree, header) != &schultz_section_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    section = (schultz_section_data *)schultz_node_widget_data(tree, header);
    data = (const schultz_accordion_data *)schultz_node_widget_data(tree,
                                                                    node);
    if (section == NULL || data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /*
     * Not an early return, even when this section is already the way it was
     * asked for. schultz_accordion_set_single_expand can turn the one at a
     * time rule on while two are open, and the sweep below is what closes
     * the extras. So the sweep runs either way and the layout is gated on
     * whether anything actually moved.
     */
    changed = (section->open != (expanded ? 1u : 0u));

    if (expanded && data->single) {
        /* One at a time, so opening this one closes whatever was open. */
        count = schultz_node_child_count(tree, node);
        for (i = 0; i < count; i++) {
            schultz_handle other = SCHULTZ_HANDLE_NONE;
            schultz_section_data *sibling;

            if (schultz_node_child_at(tree, node, i, &other) != SCHULTZ_OK ||
                other == header ||
                schultz_node_widget(tree, other) != &schultz_section_widget) {
                continue;
            }
            sibling = (schultz_section_data *)schultz_node_widget_data(tree,
                                                                       other);
            if (sibling != NULL && sibling->open) {
                changed = 1;
                sibling->open = 0u;
                schultz_node_set_state(tree, sibling->body,
                    schultz_node_get_state(tree, sibling->body)
                        & ~(uint32_t)SCHULTZ_STATE_VISIBLE);
                schultz_node_invalidate(tree, other);
            }
        }
    }

    section->open = expanded ? 1u : 0u;
    schultz_node_set_state(tree, section->body,
        expanded ? (schultz_node_get_state(tree, section->body)
                        | SCHULTZ_STATE_VISIBLE)
                 : (schultz_node_get_state(tree, section->body)
                        & ~(uint32_t)SCHULTZ_STATE_VISIBLE));
    if (!changed) {
        return SCHULTZ_OK;
    }
    schultz_node_invalidate(tree, header);
    return schultz_node_invalidate_layout(tree, node);
}

int32_t schultz_accordion_is_expanded(const schultz_tree *tree,
                                      schultz_handle header)
{
    const schultz_section_data *section;

    if (schultz_node_widget(tree, header) != &schultz_section_widget) {
        return 0;
    }
    section = (const schultz_section_data *)schultz_node_widget_data(tree,
                                                                     header);
    return (section == NULL) ? 0 : (int32_t)section->open;
}

int32_t schultz_accordion_set_single_expand(schultz_tree *tree,
                                            schultz_handle node, int32_t on)
{
    schultz_accordion_data *data;

    if (schultz_node_widget(tree, node) != &schultz_accordion_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data = (schultz_accordion_data *)schultz_node_widget_data(tree, node);
    if (data == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    data->single = on ? 1u : 0u;
    return SCHULTZ_OK;
}

/* -------------------------------------------------------------- Toolbar */

/** A toolbar's orientation, and the parts that handle what does not fit. */
typedef struct {
    uint32_t       orientation; /**< SCHULTZ_ORIENT_HORIZONTAL or _VERTICAL. */
    uint32_t       overflow;    /**< Nonzero when items may collapse. */
    uint32_t       hidden;      /**< How many are collapsed right now. */
    schultz_handle button;      /**< The trailing button, or none. */
} schultz_toolbar_data;

static const schultz_widget_vtable schultz_toolbar_widget;

/** How much room the overflow button needs at the end of the row. */
#define SCHULTZ_OVERFLOW_SIZE 28.0f

static schultz_toolbar_data *schultz_toolbar_of(schultz_tree *tree,
                                                schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_toolbar_widget) {
        return NULL;
    }
    return (schultz_toolbar_data *)schultz_node_widget_data(tree, node);
}

/* Nonzero for a child the toolbar arranges, which is everything but its own
 * overflow button. */
static int32_t schultz_toolbar_is_item(const schultz_toolbar_data *bar,
                                       schultz_handle child)
{
    return child != bar->button;
}

static int32_t schultz_toolbar_measure(schultz_tree *tree,
                                       schultz_handle node, float avail_w,
                                       float avail_h, schultz_size *out_size)
{
    schultz_toolbar_data *bar = schultz_toolbar_of(tree, node);
    uint32_t count = schultz_node_child_count(tree, node);
    float padding = 0.0f;
    float gap = 0.0f;
    float along = 0.0f;
    float across = 0.0f;
    uint32_t placed = 0;
    uint32_t i;

    if (bar == NULL) {
        *out_size = schultz_size_make(0.0f, 0.0f);
        return SCHULTZ_OK;
    }
    schultz_node_get_spacing(tree, node, &padding, &gap);

    for (i = 0; i < count; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;
        schultz_size size;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_toolbar_is_item(bar, child) ||
            schultz_layout_measure(tree, child, avail_w, avail_h, &size)
                != SCHULTZ_OK) {
            continue;
        }
        if (bar->orientation == SCHULTZ_ORIENT_VERTICAL) {
            along += size.height;
            if (size.width > across) { across = size.width; }
        } else {
            along += size.width;
            if (size.height > across) { across = size.height; }
        }
        if (placed > 0u) { along += gap; }
        placed++;
    }

    /* What it wants is everything laid out; what it gets may be less, and
     * that is what the overflow button is for. */
    if (bar->orientation == SCHULTZ_ORIENT_VERTICAL) {
        *out_size = schultz_size_make(across + padding * 2.0f,
                                      along + padding * 2.0f);
    } else {
        *out_size = schultz_size_make(along + padding * 2.0f,
                                      across + padding * 2.0f);
    }
    return SCHULTZ_OK;
}

/* Puts a row into the overflow menu for each item that had to be hidden. */
static void schultz_toolbar_refill(schultz_tree *tree, schultz_handle node,
                                   schultz_toolbar_data *bar)
{
    schultz_handle menu = schultz_menu_button_menu(tree, bar->button);
    uint32_t count;
    uint32_t i;

    if (menu == SCHULTZ_HANDLE_NONE) {
        return;
    }
    while (schultz_node_child_count(tree, menu) > 0u) {
        schultz_handle row = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(tree, menu, 0, &row) != SCHULTZ_OK) {
            break;
        }
        schultz_node_destroy(tree, row);
    }

    count = schultz_node_child_count(tree, node);
    for (i = 0; i < count; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;
        schultz_handle row = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_toolbar_is_item(bar, child) ||
            schultz_node_is_visible(tree, child)) {
            continue;
        }
        if (schultz_menu_add(tree, menu, schultz_node_get_name(tree, child),
                             NULL, &row) == SCHULTZ_OK) {
            /*
             * The row carries the hidden item's own token, so a host hears
             * the same thing whether the button was clicked or chosen from
             * the overflow.
             */
            schultz_node_set_token(tree, row,
                                   schultz_node_get_token(tree, child));
        }
    }
}

static int32_t schultz_toolbar_arrange(schultz_tree *tree,
                                       schultz_handle node, schultz_rect rect)
{
    schultz_toolbar_data *bar = schultz_toolbar_of(tree, node);
    uint32_t count = schultz_node_child_count(tree, node);
    float padding = 0.0f;
    float gap = 0.0f;
    float limit;
    float at;
    float across;
    uint32_t hidden = 0;
    uint32_t i;

    if (bar == NULL) {
        return SCHULTZ_OK;
    }
    schultz_node_get_spacing(tree, node, &padding, &gap);
    at = padding;
    if (bar->orientation == SCHULTZ_ORIENT_VERTICAL) {
        limit  = rect.height - padding;
        across = rect.width - padding * 2.0f;
    } else {
        limit  = rect.width - padding;
        across = rect.height - padding * 2.0f;
    }

    for (i = 0; i < count; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;
        schultz_size size;
        float extent;
        float room = limit;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            !schultz_toolbar_is_item(bar, child) ||
            schultz_layout_measure(tree, child, -1.0f, across, &size)
                != SCHULTZ_OK) {
            continue;
        }
        extent = (bar->orientation == SCHULTZ_ORIENT_VERTICAL) ? size.height
                                                               : size.width;
        /*
         * Leave room for the overflow button, but only while there is still
         * an item after this one that might need it. The last item does not
         * have to make room for a button that will have nothing to open.
         */
        if (bar->overflow && i + 1u < count) {
            room -= SCHULTZ_OVERFLOW_SIZE + gap;
        }

        if (bar->overflow && at + extent > room && at > padding) {
            schultz_node_set_state(tree, child,
                schultz_node_get_state(tree, child)
                    & ~(uint32_t)SCHULTZ_STATE_VISIBLE);
            hidden++;
            continue;
        }
        schultz_node_set_state(tree, child,
            schultz_node_get_state(tree, child) | SCHULTZ_STATE_VISIBLE);
        if (bar->orientation == SCHULTZ_ORIENT_VERTICAL) {
            schultz_layout_arrange(tree, child,
                schultz_rect_make(padding, at, across, size.height));
        } else {
            schultz_layout_arrange(tree, child,
                schultz_rect_make(at, padding, size.width, across));
        }
        at += extent + gap;
    }

    if (bar->button != SCHULTZ_HANDLE_NONE) {
        schultz_node_set_state(tree, bar->button,
            hidden > 0u ? (schultz_node_get_state(tree, bar->button)
                               | SCHULTZ_STATE_VISIBLE)
                        : (schultz_node_get_state(tree, bar->button)
                               & ~(uint32_t)SCHULTZ_STATE_VISIBLE));
        if (hidden > 0u) {
            if (bar->orientation == SCHULTZ_ORIENT_VERTICAL) {
                schultz_layout_arrange(tree, bar->button,
                    schultz_rect_make(padding,
                                      rect.height - padding
                                          - SCHULTZ_OVERFLOW_SIZE,
                                      across, SCHULTZ_OVERFLOW_SIZE));
            } else {
                schultz_layout_arrange(tree, bar->button,
                    schultz_rect_make(rect.width - padding
                                          - SCHULTZ_OVERFLOW_SIZE,
                                      padding, SCHULTZ_OVERFLOW_SIZE,
                                      across));
            }
        }
        if (hidden != bar->hidden) {
            bar->hidden = hidden;
            schultz_toolbar_refill(tree, node, bar);
        }
    }
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_toolbar_pane = {
    schultz_toolbar_measure, schultz_toolbar_arrange
};

static int32_t schultz_toolbar_paint(schultz_tree *tree, schultz_handle node,
                                     schultz_draw_list *list,
                                     schultz_arena *arena)
{
    schultz_rect bounds;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    return schultz_widget_draw_box(list, schultz_widget_style(tree, node),
                                   bounds);
}

static const schultz_widget_vtable schultz_toolbar_widget = {
    .paint = schultz_toolbar_paint, .destroy = free
};

int32_t schultz_toolbar_create(schultz_tree *tree, schultz_handle parent,
                               uint32_t orientation, schultz_handle *out_node)
{
    schultz_toolbar_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_toolbar_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->orientation = orientation;
    data->overflow    = 1u;
    data->button      = SCHULTZ_HANDLE_NONE;

    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    schultz_node_set_pane(tree, node, &schultz_toolbar_pane);
    schultz_node_set_widget(tree, node, &schultz_toolbar_widget, data);
    schultz_node_set_spacing(tree, node, 4.0f, 4.0f);
    schultz_node_set_clips_children(tree, node, 1);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_SURFACE);
        schultz_widget_default_style(tree, node, &patch);
    }

    /* The button that holds whatever does not fit. It is created up front
     * and hidden, because creating a node during arrange would mark layout
     * dirty from inside layout. */
    result = schultz_menu_button_create(tree, node, "...", &data->button);
    if (result != SCHULTZ_OK) {
        schultz_node_destroy(tree, node);
        return result;
    }
    schultz_node_set_state(tree, data->button,
        schultz_node_get_state(tree, data->button)
            & ~(uint32_t)SCHULTZ_STATE_VISIBLE);

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_toolbar_add(schultz_tree *tree, schultz_handle node,
                            const char *text, schultz_handle *out_button)
{
    schultz_toolbar_data *bar = schultz_toolbar_of(tree, node);
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (bar == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    result = schultz_button_create(tree, node, text, &button);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /* Added before the overflow button, so it stays at the end. */
    if (bar->button != SCHULTZ_HANDLE_NONE) {
        schultz_node_set_parent(tree, bar->button, node);
    }
    if (out_button != NULL) {
        *out_button = button;
    }
    return SCHULTZ_OK;
}

int32_t schultz_toolbar_add_separator(schultz_tree *tree, schultz_handle node,
                                      schultz_handle *out_node)
{
    schultz_toolbar_data *bar = schultz_toolbar_of(tree, node);
    schultz_handle rule = SCHULTZ_HANDLE_NONE;
    uint32_t across;
    int32_t result;

    if (bar == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    across = (bar->orientation == SCHULTZ_ORIENT_VERTICAL)
                 ? SCHULTZ_ORIENT_HORIZONTAL : SCHULTZ_ORIENT_VERTICAL;
    result = schultz_separator_create(tree, node, across, &rule);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (bar->button != SCHULTZ_HANDLE_NONE) {
        schultz_node_set_parent(tree, bar->button, node);
    }
    if (out_node != NULL) {
        *out_node = rule;
    }
    return SCHULTZ_OK;
}

int32_t schultz_toolbar_set_overflow_enabled(schultz_tree *tree,
                                             schultz_handle node, int32_t on)
{
    schultz_toolbar_data *bar = schultz_toolbar_of(tree, node);

    if (bar == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (bar->overflow == (on ? 1u : 0u)) {
        return SCHULTZ_OK;
    }
    bar->overflow = on ? 1u : 0u;
    return schultz_node_invalidate_layout(tree, node);
}

uint32_t schultz_toolbar_overflow_count(const schultz_tree *tree,
                                        schultz_handle node)
{
    const schultz_toolbar_data *bar;

    if (schultz_node_widget(tree, node) != &schultz_toolbar_widget) {
        return 0u;
    }
    bar = (const schultz_toolbar_data *)schultz_node_widget_data(tree, node);
    return (bar == NULL) ? 0u : bar->hidden;
}

/* ------------------------------------------------------------ StatusBar */

/** A status bar's message area and whatever is showing there now. */
typedef struct {
    schultz_handle label;     /**< The stretching message area. */
    char          *standing;  /**< The message a flash reverts to. */
    uint32_t       flash_ms;  /**< Milliseconds left on a flash, or zero. */
} schultz_status_data;

static const schultz_widget_vtable schultz_status_widget;

static schultz_status_data *schultz_status_of(schultz_tree *tree,
                                              schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_status_widget) {
        return NULL;
    }
    return (schultz_status_data *)schultz_node_widget_data(tree, node);
}

static int32_t schultz_status_paint(schultz_tree *tree, schultz_handle node,
                                    schultz_draw_list *list,
                                    schultz_arena *arena)
{
    schultz_rect bounds;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    return schultz_widget_draw_box(list, schultz_widget_style(tree, node),
                                   bounds);
}

/* A flashed message reverts on its own once its time is up. */
static int32_t schultz_status_tick(schultz_tree *tree, schultz_handle node,
                                   uint64_t now_ms, uint32_t elapsed_ms)
{
    schultz_status_data *bar = schultz_status_of(tree, node);

    (void)now_ms;
    if (bar == NULL || bar->flash_ms == 0u) {
        return 0;
    }
    if (elapsed_ms >= bar->flash_ms) {
        bar->flash_ms = 0u;
        schultz_label_set_text(tree, bar->label,
                               (bar->standing == NULL) ? "" : bar->standing);
        return 1;
    }
    bar->flash_ms -= elapsed_ms;
    return 0;
}

static void schultz_status_destroy(void *pointer)
{
    schultz_status_data *bar = (schultz_status_data *)pointer;

    if (bar != NULL) {
        free(bar->standing);
    }
    free(bar);
}

static const schultz_widget_vtable schultz_status_widget = {
    .paint = schultz_status_paint,
    .tick = schultz_status_tick,
    .destroy = schultz_status_destroy
};

int32_t schultz_status_bar_create(schultz_tree *tree, schultz_handle parent,
                                  schultz_handle *out_node)
{
    schultz_status_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_layout_params params;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_status_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    schultz_node_set_pane(tree, node, schultz_pane_hbox());
    schultz_node_set_spacing(tree, node, 6.0f, 10.0f);

    result = schultz_label_create(tree, node, "", &data->label);
    if (result != SCHULTZ_OK) {
        free(data);
        schultz_node_destroy(tree, node);
        return result;
    }
    schultz_label_set_wrap(tree, data->label, 0);
    /* The message takes what the sections leave, which is what makes the
     * sections sit against the far end. */
    schultz_layout_params_default(&params);
    params.grow  = SCHULTZ_GROW_ALWAYS;
    params.align = SCHULTZ_ALIGN_CENTER;
    schultz_node_set_layout_params(tree, data->label, &params);

    schultz_node_set_widget(tree, node, &schultz_status_widget, data);
    schultz_node_set_animating(tree, node, 1);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_SURFACE);
        schultz_patch_token(&patch, SCHULTZ_PROP_TEXT_COLOR,
                            SCHULTZ_TOKEN_COLOR_TEXT_MUTED);
        schultz_widget_default_style(tree, node, &patch);
    }

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_status_bar_set_message(schultz_tree *tree,
                                       schultz_handle node, const char *text)
{
    schultz_status_data *bar = schultz_status_of(tree, node);
    size_t length;
    char *copy;

    if (bar == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    length = (text == NULL) ? 0u : strlen(text);
    copy = (char *)malloc(length + 1u);
    if (copy == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    if (length > 0u) {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';

    free(bar->standing);
    bar->standing = copy;
    /* Setting the message while one is flashed replaces what the flash will
     * revert to, and leaves the flash showing. */
    if (bar->flash_ms == 0u) {
        return schultz_label_set_text(tree, bar->label, copy);
    }
    return SCHULTZ_OK;
}

int32_t schultz_status_bar_flash(schultz_tree *tree, schultz_handle node,
                                 const char *text, uint32_t ms)
{
    schultz_status_data *bar = schultz_status_of(tree, node);

    if (bar == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (ms == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    bar->flash_ms = ms;
    return schultz_label_set_text(tree, bar->label,
                                  (text == NULL) ? "" : text);
}

const char *schultz_status_bar_message(const schultz_tree *tree,
                                       schultz_handle node)
{
    const schultz_status_data *bar;

    if (schultz_node_widget(tree, node) != &schultz_status_widget) {
        return NULL;
    }
    bar = (const schultz_status_data *)schultz_node_widget_data(tree, node);
    return (bar == NULL) ? NULL : schultz_label_text(tree, bar->label);
}

int32_t schultz_status_bar_add_section(schultz_tree *tree,
                                       schultz_handle node,
                                       schultz_handle child)
{
    schultz_layout_params params;

    if (schultz_status_of(tree, node) == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (schultz_node_set_parent(tree, child, node) != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* Sections take the room they need; only the message stretches. */
    schultz_layout_params_default(&params);
    params.grow  = SCHULTZ_GROW_NEVER;
    params.align = SCHULTZ_ALIGN_CENTER;
    return schultz_node_set_layout_params(tree, child, &params);
}

/* ------------------------------------------------- Popover and Tooltip */

/** Both are a panel on a layer; only whether they capture differs. */
typedef struct {
    schultz_handle content;  /**< What the application fills. */
    uint32_t       captures; /**< Nonzero when a press outside dismisses it. */
    uint32_t       open;     /**< Nonzero while it is showing. */
    /*
     * A tooltip watching a node: which one, how long the pointer has to rest
     * on it, and how long it has so far. Nothing else uses these.
     */
    schultz_handle watching;  /**< The node it explains, or none. */
    uint32_t       delay_ms;  /**< How long the pointer must rest on it. */
    uint32_t       waited_ms; /**< How long it has rested so far. */
    /*
     * The little triangle that points back at what opened it. A popover has
     * one; a tooltip does not, because a tooltip appears at the pointer and
     * the pointer is already the arrow.
     */
    uint32_t       arrow;     /**< Nonzero to draw one. */
    /**
     * Nonzero for a tooltip, which is the one kind that follows the pointer
     * rather than lining up with the widget. A popover with no triangle is
     * not a tooltip: it is a panel dropped beside a control, and it lines up
     * the way a dropdown does.
     */
    uint32_t       tooltip;
    schultz_rect   anchor;    /**< What it was opened against, for aiming. */
} schultz_popup_data;

/** How far a popover or a tooltip sits from what it belongs to. */
static float schultz_popup_gap = 4.0f;

/** How far the triangle sticks out, and half how wide it is at its base. */
#define SCHULTZ_ARROW_REACH 6.0f

static const schultz_widget_vtable schultz_popup_widget;

/* Defined below, and called by the tick that opens a tooltip. */
static int32_t schultz_popup_show(schultz_tree *tree, schultz_handle node,
                                    schultz_handle anchor,
                                    uint32_t placement);

/*
 * How much of a popover's box the triangle takes, and on which side.
 *
 * The strip is inside the node's own bounds rather than hanging outside them,
 * which is how the rounded corners already work: what is drawn need not fill
 * the rectangle it was given. Keeping it inside means the region marked for
 * repainting already covers the tip, with no paint margin for anyone to
 * forget, and a nudge that moves the popover moves the triangle with it.
 */
static float schultz_popup_strip(schultz_tree *tree, schultz_handle node,
                                   uint32_t *out_side)
{
    const schultz_popup_data *pop =
        (const schultz_popup_data *)schultz_node_widget_data(tree, node);
    uint32_t side = schultz_node_anchor_placement(tree, node);

    if (out_side != NULL) {
        *out_side = side;
    }
    if (pop == NULL || !pop->arrow ||
        (side != SCHULTZ_PLACE_ABOVE && side != SCHULTZ_PLACE_BELOW)) {
        return 0.0f;
    }
    return SCHULTZ_ARROW_REACH;
}

/* The box, with the strip the triangle needs taken off whichever side it is
 * on. Everything that draws or places a popover asks this, so the box and the
 * triangle cannot disagree about where the edge between them is. */
static schultz_rect schultz_popup_box(schultz_rect bounds, float strip,
                                        uint32_t side)
{
    if (strip <= 0.0f) {
        return bounds;
    }
    /* Placed above its anchor means the triangle points down, off the bottom;
     * placed below means it points up, off the top. */
    if (side == SCHULTZ_PLACE_ABOVE) {
        return schultz_rect_make(bounds.x, bounds.y, bounds.width,
                                 bounds.height - strip);
    }
    return schultz_rect_make(bounds.x, bounds.y + strip, bounds.width,
                             bounds.height - strip);
}

/*
 * The triangle, as three points, aimed at the middle of what opened the
 * popover.
 *
 * Clamped to stay clear of the rounded corners. A popover nudged sideways to
 * stay on screen can end up well off centre from its anchor, and a triangle
 * that followed the anchor without limit would grow out of a corner or hang
 * off the end entirely.
 */
static void schultz_popup_arrow(schultz_rect bounds, schultz_rect box,
                                  schultz_rect anchor, float radius,
                                  float border, uint32_t side,
                                  schultz_point *out_points)
{
    float aim = anchor.x + anchor.width * 0.5f;
    float least = box.x + radius + SCHULTZ_ARROW_REACH;
    float most = box.x + box.width - radius - SCHULTZ_ARROW_REACH;
    float tip_y;
    float base_y;

    if (most < least) {
        aim = box.x + box.width * 0.5f;
    } else {
        if (aim < least) { aim = least; }
        if (aim > most) { aim = most; }
    }

    /*
     * The base sits a whisker inside the box rather than on its edge, so the
     * triangle's own fill covers the length of border it grows out of. Left
     * on the edge, the box's outline runs straight across the mouth of the
     * arrow and it reads as a pennant stuck to a box rather than as part of
     * one.
     */
    if (side == SCHULTZ_PLACE_ABOVE) {
        base_y = box.y + box.height - border;
        tip_y  = bounds.y + bounds.height;
    } else {
        base_y = box.y + border;
        tip_y  = bounds.y;
    }
    out_points[0] = schultz_point_make(aim - SCHULTZ_ARROW_REACH, base_y);
    out_points[1] = schultz_point_make(aim, tip_y);
    out_points[2] = schultz_point_make(aim + SCHULTZ_ARROW_REACH, base_y);
}

static int32_t schultz_popup_paint(schultz_tree *tree, schultz_handle node,
                                     schultz_draw_list *list,
                                     schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    const schultz_popup_data *pop =
        (const schultz_popup_data *)schultz_node_widget_data(tree, node);
    schultz_rect bounds;
    schultz_rect box;
    uint32_t side = SCHULTZ_PLACE_BELOW;
    float strip;
    int32_t result;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    strip = schultz_popup_strip(tree, node, &side);
    box = schultz_popup_box(bounds, strip, side);

    result = schultz_widget_draw_box(list, style, box);
    if (result != SCHULTZ_OK || strip <= 0.0f || pop == NULL) {
        return result;
    }

    {
        schultz_point points[3];
        schultz_paint fill = schultz_resolved_paint(style,
                                                    SCHULTZ_PROP_BACKGROUND);
        schultz_stroke edge = schultz_widget_stroke(style);

        schultz_popup_arrow(bounds, box, pop->anchor,
            schultz_resolved_number(style, SCHULTZ_PROP_CORNER_RADIUS),
            schultz_resolved_number(style, SCHULTZ_PROP_BORDER_WIDTH), side,
            points);

        result = schultz_draw_fill_polygon(list, points, 3u, fill,
                                           SCHULTZ_FILL_NONZERO);
        if (result != SCHULTZ_OK) {
            return result;
        }
        /*
         * The two outer edges and not the base. Stroking a closed triangle
         * would draw the border straight across the mouth of it, which reads
         * as a line ruled through the shape rather than an arrow growing out
         * of the box.
         */
        edge.paint = schultz_paint_solid(
            schultz_resolved_color(style, SCHULTZ_PROP_BORDER_COLOR));
        edge.width = schultz_resolved_number(style,
                                             SCHULTZ_PROP_BORDER_WIDTH);
        edge.dash  = SCHULTZ_HANDLE_NONE;
        if (edge.width > 0.0f) {
            result = schultz_draw_stroke_polygon(list, points, 3u, edge, 0);
        }
    }
    return result;
}

/*
 * Measure and arrange, so the content keeps clear of the triangle's strip.
 *
 * A pane of its own because padding is one number for all four sides, and the
 * strip is only ever on one of them.
 */
static int32_t schultz_popup_measure(schultz_tree *tree, schultz_handle node,
                                       float avail_w, float avail_h,
                                       schultz_size *out_size)
{
    schultz_handle content = SCHULTZ_HANDLE_NONE;
    float strip = schultz_popup_strip(tree, node, NULL);
    float padding = 0.0f;

    *out_size = schultz_size_make(0.0f, 0.0f);
    schultz_node_get_spacing(tree, node, &padding, NULL);
    if (schultz_node_child_at(tree, node, 0, &content) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    if (schultz_layout_measure(tree, content,
                               schultz_widget_inset(avail_w, padding * 2.0f),
                               schultz_widget_inset(avail_h,
                                                    padding * 2.0f + strip),
                               out_size) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    out_size->width  += padding * 2.0f;
    out_size->height += padding * 2.0f + strip;
    return SCHULTZ_OK;
}

static int32_t schultz_popup_arrange(schultz_tree *tree,
                                       schultz_handle node, schultz_rect rect)
{
    schultz_handle content = SCHULTZ_HANDLE_NONE;
    uint32_t side = SCHULTZ_PLACE_BELOW;
    float strip = schultz_popup_strip(tree, node, &side);
    float padding = 0.0f;
    schultz_rect box;

    if (schultz_node_child_at(tree, node, 0, &content) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    schultz_node_get_spacing(tree, node, &padding, NULL);
    /* In the node's own space, so the box starts at zero rather than at the
     * bounds the paint above works in. */
    box = schultz_popup_box(schultz_rect_make(0.0f, 0.0f, rect.width,
                                                rect.height), strip, side);
    return schultz_layout_arrange(tree, content,
        schultz_rect_make(box.x + padding, box.y + padding,
                          box.width - padding * 2.0f,
                          box.height - padding * 2.0f));
}

static const schultz_pane_vtable schultz_popup_pane = {
    schultz_popup_measure, schultz_popup_arrange
};

static int32_t schultz_popup_event(schultz_tree *tree, schultz_handle node,
                                     const schultz_event *event)
{
    if (event->type == SCHULTZ_EVENT_DISMISS ||
        (event->type == SCHULTZ_EVENT_KEY_DOWN &&
         event->key == (uint32_t)SCHULTZ_KEY_ESCAPE)) {
        schultz_popup_close(tree, node);
        return SCHULTZ_EVENT_CONSUMED;
    }
    return SCHULTZ_OK;
}

/*
 * A tooltip appears when the pointer has rested on what it explains, and goes
 * away when the pointer leaves. Both are decided here, from the hover flag the
 * router already keeps, so watching a node costs no routing changes at all.
 */
static int32_t schultz_popup_tick(schultz_tree *tree, schultz_handle node,
                                    uint64_t now_ms, uint32_t elapsed_ms)
{
    schultz_popup_data *pop =
        (schultz_popup_data *)schultz_node_widget_data(tree, node);
    uint32_t hovered;

    (void)now_ms;
    if (pop == NULL || pop->watching == SCHULTZ_HANDLE_NONE) {
        return 0;
    }
    hovered = schultz_node_get_state(tree, pop->watching) &
              SCHULTZ_STATE_HOVERED;

    if (!hovered) {
        pop->waited_ms = 0u;
        if (pop->open) {
            schultz_popup_close(tree, node);
            return 1;
        }
        return 0;
    }
    if (pop->open) {
        return 0; /* already showing, and still hovered */
    }

    pop->waited_ms += elapsed_ms;
    if (pop->waited_ms < pop->delay_ms) {
        return 0;
    }
    schultz_popup_show(tree, node, pop->watching, SCHULTZ_PLACE_BELOW);
    return 1;
}

static const schultz_widget_vtable schultz_popup_widget = {
    .paint = schultz_popup_paint, .event = schultz_popup_event,
    .tick = schultz_popup_tick, .destroy = free
};

/* Popovers, tooltips and dialogs are one widget with different settings. */
/*
 * How a popup is dressed. Four tokens gathered rather than four arguments in
 * a row, because they are all one kind of number and two of them swapped by
 * mistake would compile and look almost right.
 */
typedef struct {
    uint32_t background; /**< The panel's own colour. */
    uint32_t text;       /**< What is written on it. */
    uint32_t radius;     /**< How much the corners are rounded. */
    uint32_t padding;    /**< Between the edge and what is inside. */
} schultz_popup_look;

static int32_t schultz_popup_build(schultz_tree *tree, uint32_t captures,
                                   const schultz_popup_look *look,
                                   schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_popup_data *pop;
    schultz_patch patch;
    uint32_t state;
    int32_t result = schultz_node_create(tree, schultz_tree_root(tree),
                                         &node);

    if (result != SCHULTZ_OK) {
        return result;
    }
    pop = (schultz_popup_data *)calloc(1, sizeof(*pop));
    if (pop == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_panel_create(tree, node, &pop->content);
    if (result != SCHULTZ_OK) {
        free(pop);
        schultz_node_destroy(tree, node);
        return result;
    }
    /*
     * A content node gets a stack pane, so one child measures and fills it
     * without the application having to say so. Left bare it would measure
     * nothing, and whatever was put inside it would never be asked its size:
     * the container would collapse to its padding and the contents would
     * never be laid out at all. An application wanting rows or a grid sets
     * its own pane over the top.
     */
    schultz_node_set_pane(tree, pop->content, schultz_pane_stack());
    pop->captures = captures;

    schultz_node_set_pane(tree, node, &schultz_popup_pane);
    schultz_node_set_widget(tree, node, &schultz_popup_widget, pop);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_GROUP);
    state = schultz_node_get_state(tree, node);
    schultz_node_set_state(tree, node,
                           state & ~(uint32_t)SCHULTZ_STATE_VISIBLE);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            look->background);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                            SCHULTZ_TOKEN_COLOR_BORDER);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_WIDTH,
                            SCHULTZ_TOKEN_BORDER_WIDTH);
        schultz_patch_token(&patch, SCHULTZ_PROP_CORNER_RADIUS,
                            look->radius);
        schultz_patch_token(&patch, SCHULTZ_PROP_PADDING, look->padding);
        /*
         * Text inherits, so saying it once here settles what a label put
         * inside is set in. Without it a white card on a dark screen would
         * carry the dark screen's near white text.
         */
        schultz_patch_token(&patch, SCHULTZ_PROP_TEXT_COLOR, look->text);
        schultz_widget_default_style(tree, node, &patch);
    }
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_popup_create(schultz_tree *tree, int32_t captures,
                             schultz_handle *out_node)
{
    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * A surface a step above the page, square, with no arrow: a panel that
     * happens to float, which is what a menu or a dropdown is. Anything that
     * wants to look like something else says so afterwards, the way
     * schultz_popover_create and schultz_tooltip_create do.
     */
    {
        static const schultz_popup_look panel = {
            SCHULTZ_TOKEN_COLOR_SURFACE_RAISED,
            SCHULTZ_TOKEN_COLOR_TEXT,
            SCHULTZ_TOKEN_RADIUS_STRUCTURE,
            SCHULTZ_TOKEN_SPACE_SM
        };

        return schultz_popup_build(tree, captures ? 1u : 0u, &panel,
                                   out_node);
    }
}

int32_t schultz_popover_create(schultz_tree *tree, int32_t captures,
                               schultz_handle *out_node)
{
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * A bubble rather than a panel. Opposite to the page, because something
     * pointing at what it belongs to has to read as being in front of the
     * page rather than part of it, and a tone off the surface does not say
     * that on its own. Rounded for the same reason: it is not sitting in the
     * page's grid.
     *
     * Everything after this is a popup, and is driven with the popup calls.
     * This is a constructor, not a second kind of thing.
     */
    {
        /*
         * Roomier and rounder than the panel it is made from. A bubble is
         * read as an object resting on the page rather than a region of it,
         * and the space inside and the corners are what say so.
         */
        static const schultz_popup_look bubble = {
            SCHULTZ_TOKEN_COLOR_SURFACE_INVERSE,
            SCHULTZ_TOKEN_COLOR_TEXT_ON_INVERSE,
            SCHULTZ_TOKEN_RADIUS_CONTROL,
            SCHULTZ_TOKEN_SPACE_MD
        };

        result = schultz_popup_build(tree, captures ? 1u : 0u, &bubble,
                                     out_node);
    }
    if (result == SCHULTZ_OK) {
        schultz_popup_set_arrow(tree, *out_node, 1);
    }
    return result;
}

int32_t schultz_popup_set_arrow(schultz_tree *tree, schultz_handle node,
                                  int32_t arrow)
{
    schultz_popup_data *pop;

    if (schultz_node_widget(tree, node) != &schultz_popup_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    pop = (schultz_popup_data *)schultz_node_widget_data(tree, node);
    if (pop == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (pop->arrow == (arrow ? 1u : 0u)) {
        return SCHULTZ_OK;
    }
    pop->arrow = arrow ? 1u : 0u;
    schultz_node_invalidate_layout(tree, node);
    return schultz_node_invalidate(tree, node);
}

int32_t schultz_popup_set_gap(float gap)
{
    if (gap < 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_popup_gap = gap;
    return SCHULTZ_OK;
}

int32_t schultz_tooltip_create(schultz_tree *tree, const char *text,
                               schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /* A tooltip never captures: it explains, and gets out of the way. */
    {
        static const schultz_popup_look tip = {
            SCHULTZ_TOKEN_COLOR_SURFACE_INVERSE,
            SCHULTZ_TOKEN_COLOR_TEXT_ON_INVERSE,
            SCHULTZ_TOKEN_RADIUS_CONTROL_SMALL,
            SCHULTZ_TOKEN_SPACE_SM
        };

        result = schultz_popup_build(tree, 0u, &tip, &node);
    }
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_label_create(tree, schultz_popup_content(tree, node),
                                  text, &label);
    if (result != SCHULTZ_OK) {
        schultz_node_destroy(tree, node);
        return result;
    }
    schultz_label_set_wrap(tree, label, 0);
    {
        schultz_popup_data *pop =
            (schultz_popup_data *)schultz_node_widget_data(tree, node);

        if (pop != NULL) {
            pop->tooltip = 1u;
        }
    }
    /*
     * The whole thing is invisible to the pointer, not just its outer node: a
     * tooltip sits over what it explains, and one that took a click would
     * make the thing it is explaining unclickable. A panel that can be
     * clicked is a popover.
     */
    schultz_node_set_hit_testable(tree, node, 0);
    schultz_node_set_hit_testable(tree, schultz_popup_content(tree, node),
                                  0);
    schultz_node_set_hit_testable(tree, label, 0);
    *out_node = node;
    return SCHULTZ_OK;
}

schultz_handle schultz_popup_content(const schultz_tree *tree,
                                       schultz_handle node)
{
    const schultz_popup_data *pop;

    if (schultz_node_widget(tree, node) != &schultz_popup_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    pop = (const schultz_popup_data *)schultz_node_widget_data(tree, node);
    return (pop == NULL) ? SCHULTZ_HANDLE_NONE : pop->content;
}

/*
 * Shows a popover or a tooltip beside something.
 *
 * A tooltip appears at the pointer rather than at the corner of what it
 * explains, because that is where the reader is looking and it is what every
 * other toolkit does. Its anchor is a sliver at the pointer's own x spanning
 * the widget's height, so the tooltip's leading edge lands under the pointer
 * and the gap is still measured from the widget's edge.
 *
 * With no pointer to be found, which is a touch screen or a program that has
 * only just started, the anchor is the widget and the tooltip is centred on
 * it. The far left corner is the one answer that is never right.
 */
static int32_t schultz_popup_show(schultz_tree *tree, schultz_handle node,
                                    schultz_handle anchor, uint32_t placement)
{
    schultz_popup_data *pop;
    schultz_rect bounds;
    schultz_rect at;
    schultz_point pointer;
    int32_t centred;

    if (schultz_node_widget(tree, node) != &schultz_popup_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (schultz_node_absolute_bounds(tree, anchor, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    pop = (schultz_popup_data *)schultz_node_widget_data(tree, node);
    at = bounds;

    /*
     * A bubble is centred on what it points at, whichever side it ends up on,
     * because the triangle has to come out of the middle of it. Anything
     * else lines up with the leading edge of the control, which is where a
     * dropdown goes and what a calendar under a date field wants.
     */
    centred = pop->arrow ? 1 : 0;

    if (pop->tooltip) {
        if (schultz_tree_pointer(tree, &pointer) == SCHULTZ_OK &&
            pointer.x >= bounds.x && pointer.x <= bounds.x + bounds.width) {
            at = schultz_rect_make(pointer.x, bounds.y, 0.0f, bounds.height);
            centred = 0;
        } else {
            /* Nothing to line up with, so the middle of the widget. Its far
             * corner is the one answer that is never right. */
            centred = 1;
        }
    }

    pop->open   = 1u;
    pop->anchor = bounds;
    schultz_overlay_show(tree, node, 1, (int32_t)pop->captures);
    schultz_overlay_place(tree, node, at, placement, schultz_popup_gap,
                          centred, 1);
    return schultz_node_invalidate(tree, node);
}

int32_t schultz_popup_open(schultz_tree *tree, schultz_handle node,
                             schultz_handle anchor)
{
    const schultz_popup_data *pop;

    if (schultz_node_widget(tree, node) != &schultz_popup_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    pop = (const schultz_popup_data *)schultz_node_widget_data(tree, node);
    /*
     * A popover goes above what it belongs to, so the thing being talked
     * about stays visible under the reader's eye rather than being covered by
     * the bubble talking about it. A tooltip goes below, out of the way of a
     * pointer that is resting on the widget and would otherwise sit on top of
     * the words. Either flips to the other side sooner than be cut off.
     */
    return schultz_popup_show(tree, node, anchor,
        (pop != NULL && pop->arrow) ? SCHULTZ_PLACE_ABOVE
                                    : SCHULTZ_PLACE_BELOW);
}

int32_t schultz_popup_open_at(schultz_tree *tree, schultz_handle node,
                                schultz_handle anchor, uint32_t placement)
{
    return schultz_popup_show(tree, node, anchor, placement);
}

int32_t schultz_tooltip_watch(schultz_tree *tree, schultz_handle node,
                              schultz_handle anchor, uint32_t delay_ms)
{
    schultz_popup_data *pop;

    if (schultz_node_widget(tree, node) != &schultz_popup_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    pop = (schultz_popup_data *)schultz_node_widget_data(tree, node);
    pop->watching  = anchor;
    pop->delay_ms  = delay_ms;
    pop->waited_ms = 0u;

    if (anchor == SCHULTZ_HANDLE_NONE) {
        schultz_popup_close(tree, node);
        return schultz_node_set_animating(tree, node, 0);
    }
    return schultz_node_set_animating(tree, node, 1);
}

int32_t schultz_popup_close(schultz_tree *tree, schultz_handle node)
{
    schultz_popup_data *pop;

    if (schultz_node_widget(tree, node) != &schultz_popup_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    pop = (schultz_popup_data *)schultz_node_widget_data(tree, node);
    if (!pop->open) {
        return SCHULTZ_OK;
    }
    pop->open = 0u;
    return schultz_overlay_show(tree, node, 0, (int32_t)pop->captures);
}

int32_t schultz_popup_is_open(const schultz_tree *tree, schultz_handle node)
{
    const schultz_popup_data *pop;

    if (schultz_node_widget(tree, node) != &schultz_popup_widget) {
        return 0;
    }
    pop = (const schultz_popup_data *)schultz_node_widget_data(tree, node);
    return (pop == NULL) ? 0 : (int32_t)pop->open;
}

/* --------------------------------------------------------------- Dialog */

/** A dialog is a panel over a scrim that covers the whole window. */
typedef struct {
    schultz_handle panel;   /**< The box in the middle. */
    schultz_handle bar;     /**< The title bar across its top. */
    schultz_handle title;   /**< The label in that bar. */
    schultz_handle rule;    /**< The line under the bar. */
    schultz_handle content; /**< What the application fills. */
    uint32_t       open;    /**< Nonzero while it is showing. */
} schultz_dialog_data;

static const schultz_widget_vtable schultz_dialog_widget;

/*
 * The scrim is the dialog node itself, covering the window, with the panel
 * centred on it by a stack pane. Filling it dims what is behind and takes
 * every press that misses the panel, which is what modal means.
 */
static int32_t schultz_dialog_paint(schultz_tree *tree, schultz_handle node,
                                    schultz_draw_list *list,
                                    schultz_arena *arena)
{
    schultz_rect bounds;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    return schultz_draw_fill_rect(list, bounds,
        schultz_resolved_paint(schultz_widget_style(tree, node),
                               SCHULTZ_PROP_BACKGROUND));
}

static int32_t schultz_dialog_event(schultz_tree *tree, schultz_handle node,
                                    const schultz_event *event)
{
    if (event->type == SCHULTZ_EVENT_KEY_DOWN &&
        event->key == (uint32_t)SCHULTZ_KEY_ESCAPE) {
        schultz_dialog_close(tree, node);
        return SCHULTZ_EVENT_CONSUMED;
    }
    /*
     * A press on the scrim itself is swallowed rather than closing the
     * dialog: a modal dialog is asking a question, and clicking beside it is
     * not an answer. A press on something inside the dialog is that thing's,
     * and passes through untouched.
     */
    if (event->target == node &&
        (event->type == SCHULTZ_EVENT_MOUSE_DOWN ||
         event->type == SCHULTZ_EVENT_DISMISS)) {
        return SCHULTZ_EVENT_CONSUMED;
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_dialog_widget = {
    .paint = schultz_dialog_paint, .event = schultz_dialog_event, .destroy = free
};

int32_t schultz_dialog_create(schultz_tree *tree, const char *title,
                              schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_dialog_data *dialog;
    schultz_patch patch;
    uint32_t state;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, schultz_tree_root(tree), &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    dialog = (schultz_dialog_data *)calloc(1, sizeof(*dialog));
    if (dialog == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    /*
     * A panel with a title bar above its content, rather than a group box.
     * A group box sets its title into its own top edge, half above the
     * border, which is right for a group of fields on a form and wrong for a
     * window: the title ends up sitting outside the thing it names.
     */
    result = schultz_panel_create(tree, node, &dialog->panel);
    if (result == SCHULTZ_OK) {
        result = schultz_panel_create(tree, dialog->panel, &dialog->bar);
    }
    if (result == SCHULTZ_OK) {
        result = schultz_label_create(tree, dialog->bar, title,
                                      &dialog->title);
    }
    if (result == SCHULTZ_OK) {
        /* A rule under the bar. The two surfaces differ by one step, which
         * is enough to see and not enough to say "this is the title". */
        result = schultz_separator_create(tree, dialog->panel,
                                          SCHULTZ_ORIENT_HORIZONTAL,
                                          &dialog->rule);
    }
    if (result == SCHULTZ_OK) {
        result = schultz_panel_create(tree, dialog->panel, &dialog->content);
    }
    if (result != SCHULTZ_OK) {
        free(dialog);
        schultz_node_destroy(tree, node);
        return result;
    }

    schultz_node_set_pane(tree, dialog->panel, schultz_pane_vbox());
    schultz_node_set_spacing(tree, dialog->panel, 0.0f, 0.0f);
    /*
     * A row, not a stack. A stack aligns a child the same way on both axes,
     * and the title wants to be against the left edge and centred down the
     * bar's height at the same time. A row is also where a close button
     * would go.
     */
    schultz_node_set_pane(tree, dialog->bar, schultz_pane_hbox());
    schultz_node_set_pane(tree, dialog->content, schultz_pane_stack());
    schultz_label_set_wrap(tree, dialog->title, 0);
    schultz_node_set_hit_testable(tree, dialog->title, 0);
    {
        /* The title reads from the left of the bar and sits centred down
         * it, and the content fills what is left over below. */
        schultz_layout_params params;

        schultz_layout_params_default(&params);
        params.align = SCHULTZ_ALIGN_CENTER;
        schultz_node_set_layout_params(tree, dialog->title, &params);
        schultz_layout_params_default(&params);
        params.grow  = SCHULTZ_GROW_ALWAYS;
        params.align = SCHULTZ_ALIGN_STRETCH;
        schultz_node_set_layout_params(tree, dialog->content, &params);
    }
    {
        schultz_patch bar_style;

        if (schultz_patch_init(&bar_style) == SCHULTZ_OK) {
            /*
             * A band in its own tone, not a surface a shade off the one
             * below it. One step apart is enough to see and not enough to
             * say "this is the title", and a dialog's title is the one piece
             * of a dialog that has to be read before anything else.
             */
            schultz_patch_token(&bar_style, SCHULTZ_PROP_BACKGROUND,
                                SCHULTZ_TOKEN_COLOR_TITLE_BAR);
            schultz_patch_token(&bar_style, SCHULTZ_PROP_PADDING,
                                SCHULTZ_TOKEN_SPACE_MD);
            /*
             * Tall enough to take a touch. The bar is where a close button
             * belongs, and a strip sized to its text is too thin to put one
             * in.
             */
            schultz_patch_token(&bar_style, SCHULTZ_PROP_MIN_HEIGHT,
                                SCHULTZ_TOKEN_CONTROL_HEIGHT);
            schultz_widget_default_style(tree, dialog->bar, &bar_style);
        }
        schultz_node_set_style_property(tree, dialog->title,
            SCHULTZ_PROP_TEXT_COLOR,
            schultz_value_token(SCHULTZ_TOKEN_COLOR_TITLE_TEXT));
        schultz_node_set_style_property(tree, dialog->title,
            SCHULTZ_PROP_FONT, schultz_value_token(SCHULTZ_TOKEN_FONT_TITLE));
        schultz_node_set_style_property(tree, dialog->title,
            SCHULTZ_PROP_FONT_SIZE,
            schultz_value_token(SCHULTZ_TOKEN_FONT_SIZE_TITLE));
        schultz_node_set_style_property(tree, dialog->content,
            SCHULTZ_PROP_PADDING,
            schultz_value_token(SCHULTZ_TOKEN_SPACE_MD));
    }
    {
        schultz_patch panel_style;

        if (schultz_patch_init(&panel_style) == SCHULTZ_OK) {
            schultz_patch_token(&panel_style, SCHULTZ_PROP_BACKGROUND,
                                SCHULTZ_TOKEN_COLOR_SURFACE);
            schultz_patch_token(&panel_style, SCHULTZ_PROP_BORDER_COLOR,
                                SCHULTZ_TOKEN_COLOR_BORDER);
            schultz_patch_token(&panel_style, SCHULTZ_PROP_BORDER_WIDTH,
                                SCHULTZ_TOKEN_BORDER_WIDTH);
            schultz_patch_token(&panel_style, SCHULTZ_PROP_CORNER_RADIUS,
                                SCHULTZ_TOKEN_RADIUS_STRUCTURE);
            schultz_widget_default_style(tree, dialog->panel, &panel_style);
        }
        schultz_node_set_clips_children(tree, dialog->panel, 1);
    }

    schultz_node_set_pane(tree, node, schultz_pane_stack());
    /* The scrim covers the window, and follows it when it changes size. */
    schultz_node_set_fills_viewport(tree, node, 1);
    schultz_node_set_widget(tree, node, &schultz_dialog_widget, dialog);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_WINDOW);
    schultz_node_set_name(tree, node, (title == NULL) ? "" : title);
    state = schultz_node_get_state(tree, node);
    schultz_node_set_state(tree, node,
                           state & ~(uint32_t)SCHULTZ_STATE_VISIBLE);
    {
        /* The panel sits in the middle rather than filling the scrim. */
        schultz_layout_params params;

        schultz_layout_params_default(&params);
        params.align = SCHULTZ_ALIGN_CENTER;
        schultz_node_set_layout_params(tree, dialog->panel, &params);
    }

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        /* The scrim: dark and part way transparent, so context stays. */
        schultz_patch_set(&patch, SCHULTZ_PROP_BACKGROUND,
                          schultz_value_color(schultz_color_rgba(0u, 0u, 0u,
                                                                 140u)));
        schultz_widget_default_style(tree, node, &patch);
    }
    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_SURFACE_RAISED);
        schultz_widget_default_style(tree, dialog->panel, &patch);
    }
    *out_node = node;
    return SCHULTZ_OK;
}

schultz_handle schultz_dialog_content(const schultz_tree *tree,
                                      schultz_handle node)
{
    const schultz_dialog_data *dialog;

    if (schultz_node_widget(tree, node) != &schultz_dialog_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    dialog = (const schultz_dialog_data *)schultz_node_widget_data(tree,
                                                                   node);
    return (dialog == NULL) ? SCHULTZ_HANDLE_NONE : dialog->content;
}

int32_t schultz_dialog_open(schultz_tree *tree, schultz_handle node)
{
    schultz_dialog_data *dialog;
    schultz_rect viewport;

    if (schultz_node_widget(tree, node) != &schultz_dialog_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    dialog = (schultz_dialog_data *)schultz_node_widget_data(tree, node);
    dialog->open = 1u;
    schultz_overlay_show(tree, node, 1, 1);

    /* The scrim covers everything, so the whole window is its bounds. */
    schultz_tree_get_viewport(tree, &viewport);
    schultz_node_set_bounds(tree, node, viewport);
    schultz_tree_resolve_styles(tree);
    schultz_layout_arrange(tree, node, viewport);
    return schultz_node_invalidate(tree, node);
}

int32_t schultz_dialog_close(schultz_tree *tree, schultz_handle node)
{
    schultz_dialog_data *dialog;

    if (schultz_node_widget(tree, node) != &schultz_dialog_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    dialog = (schultz_dialog_data *)schultz_node_widget_data(tree, node);
    if (!dialog->open) {
        return SCHULTZ_OK;
    }
    dialog->open = 0u;
    return schultz_overlay_show(tree, node, 0, 1);
}

int32_t schultz_dialog_is_open(const schultz_tree *tree, schultz_handle node)
{
    const schultz_dialog_data *dialog;

    if (schultz_node_widget(tree, node) != &schultz_dialog_widget) {
        return 0;
    }
    dialog = (const schultz_dialog_data *)schultz_node_widget_data(tree,
                                                                   node);
    return (dialog == NULL) ? 0 : (int32_t)dialog->open;
}

/* -------------------------------------------------- MessageDialog family */

/** A prebuilt dialog: an icon, a message, and a standard set of buttons. */
typedef struct {
    schultz_handle dialog;  /**< The dialog this listens inside. */
    schultz_handle message; /**< The main line. */
    schultz_handle detail;  /**< The smaller second line, or none. */
    schultz_handle bar;     /**< The button bar along the bottom. */
    schultz_handle input;   /**< A text field or combo box, or none. */
    uint32_t       kind;    /**< Which of the three this is. */
    uint32_t       icon;    /**< One of SCHULTZ_DIALOG_ICON_*. */
    uint32_t       result;  /**< The role chosen, or _ROLE_COUNT for none. */
    uint32_t       allow_empty; /**< Text input: accept an empty answer. */
} schultz_message_data;

static const schultz_widget_vtable schultz_message_widget;

/*
 * The listener sits on the dialog's content node rather than on the dialog
 * itself. Replacing the dialog's own widget would break every
 * schultz_dialog_* call, and the content node is what the buttons hang off,
 * so their clicks rise through it either way.
 */
static schultz_message_data *schultz_message_of(schultz_tree *tree,
                                                schultz_handle node)
{
    schultz_handle content = schultz_dialog_content(tree, node);

    if (content == SCHULTZ_HANDLE_NONE ||
        schultz_node_widget(tree, content) != &schultz_message_widget) {
        return NULL;
    }
    return (schultz_message_data *)schultz_node_widget_data(tree, content);
}

/* Which role a node in the dialog's button bar carries, if it is one. */
static uint32_t schultz_message_role_at(schultz_tree *tree,
                                        schultz_message_data *box,
                                        schultz_handle target)
{
    uint32_t count = schultz_node_child_count(tree, box->bar);
    uint32_t i;

    for (i = 0; i < count; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(tree, box->bar, i, &child) == SCHULTZ_OK &&
            child == target) {
            return schultz_button_bar_role(tree, child);
        }
    }
    return SCHULTZ_BUTTON_ROLE_COUNT;
}

/* The role Enter accepts with, which depends on which set is showing. */
static uint32_t schultz_message_default_role(const schultz_message_data *box)
{
    return (box->kind == 0u) ? SCHULTZ_BUTTON_ROLE_OK
                             : SCHULTZ_BUTTON_ROLE_OK;
}

static int32_t schultz_message_finish(schultz_tree *tree,
                                      schultz_message_data *box,
                                      uint32_t role)
{
    /*
     * A text input dialog with an empty answer cannot be accepted unless it
     * was told empty is fine, so Enter and OK both do nothing there.
     */
    if (role == SCHULTZ_BUTTON_ROLE_OK && box->input != SCHULTZ_HANDLE_NONE &&
        box->kind == 1u && !box->allow_empty) {
        const char *text = schultz_text_get(tree, box->input);

        if (text == NULL || text[0] == '\0') {
            return SCHULTZ_EVENT_CONSUMED;
        }
    }
    box->result = role;
    schultz_dialog_close(tree, box->dialog);
    return SCHULTZ_OK;
}

/*
 * The buttons live inside the dialog, so their clicks rise through it on the
 * way to the host. That is how the dialog learns which one was chosen without
 * the host having to wire each button up.
 */
static int32_t schultz_message_event(schultz_tree *tree, schultz_handle node,
                                     const schultz_event *event)
{
    schultz_message_data *box =
        (schultz_message_data *)schultz_node_widget_data(tree, node);
    uint32_t role;

    if (box == NULL) {
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_CLICK) {
        role = schultz_message_role_at(tree, box, event->target);
        if (role != SCHULTZ_BUTTON_ROLE_COUNT) {
            return schultz_message_finish(tree, box, role);
        }
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_KEY_DOWN) {
        if (event->key == (uint32_t)SCHULTZ_KEY_RETURN) {
            return schultz_message_finish(tree, box,
                                          schultz_message_default_role(box));
        }
        if (event->key == (uint32_t)SCHULTZ_KEY_ESCAPE) {
            /* Escape answers with the dismissing role this set has. */
            box->result = (box->kind == 0u && schultz_node_child_count(tree,
                               box->bar) == 1u)
                              ? SCHULTZ_BUTTON_ROLE_OK
                              : SCHULTZ_BUTTON_ROLE_CANCEL;
            schultz_dialog_close(tree, box->dialog);
            return SCHULTZ_EVENT_CONSUMED;
        }
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_message_widget = {
    .event = schultz_message_event, .destroy = free
};

/* Puts the standard sets on the bar, in the order the platform wants. */
static int32_t schultz_message_buttons(schultz_tree *tree,
                                       schultz_handle bar, uint32_t buttons)
{
    int32_t result = SCHULTZ_OK;

    switch (buttons) {
    case SCHULTZ_DIALOG_OK_CANCEL:
        result = schultz_button_bar_add(tree, bar, "OK",
                                        SCHULTZ_BUTTON_ROLE_OK, NULL);
        if (result == SCHULTZ_OK) {
            result = schultz_button_bar_add(tree, bar, "Cancel",
                                            SCHULTZ_BUTTON_ROLE_CANCEL, NULL);
        }
        break;
    case SCHULTZ_DIALOG_YES_NO:
        result = schultz_button_bar_add(tree, bar, "Yes",
                                        SCHULTZ_BUTTON_ROLE_YES, NULL);
        if (result == SCHULTZ_OK) {
            result = schultz_button_bar_add(tree, bar, "No",
                                            SCHULTZ_BUTTON_ROLE_NO, NULL);
        }
        break;
    case SCHULTZ_DIALOG_YES_NO_CANCEL:
        result = schultz_button_bar_add(tree, bar, "Yes",
                                        SCHULTZ_BUTTON_ROLE_YES, NULL);
        if (result == SCHULTZ_OK) {
            result = schultz_button_bar_add(tree, bar, "No",
                                            SCHULTZ_BUTTON_ROLE_NO, NULL);
        }
        if (result == SCHULTZ_OK) {
            result = schultz_button_bar_add(tree, bar, "Cancel",
                                            SCHULTZ_BUTTON_ROLE_CANCEL, NULL);
        }
        break;
    default:
        result = schultz_button_bar_add(tree, bar, "OK",
                                        SCHULTZ_BUTTON_ROLE_OK, NULL);
        break;
    }
    return result;
}

/* The shared body of the three dialogs. `kind` is 0 message, 1 text, 2 choice. */
static int32_t schultz_message_build(schultz_tree *tree, const char *title,
                                     uint32_t icon, uint32_t buttons,
                                     uint32_t kind, schultz_handle *out_node)
{
    schultz_message_data *box;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle content;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    box = (schultz_message_data *)calloc(1, sizeof(*box));
    if (box == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    box->kind   = kind;
    box->icon   = icon;
    box->result = SCHULTZ_BUTTON_ROLE_COUNT;
    box->input  = SCHULTZ_HANDLE_NONE;
    box->detail = SCHULTZ_HANDLE_NONE;

    result = schultz_dialog_create(tree, title, &node);
    if (result != SCHULTZ_OK) {
        free(box);
        return result;
    }
    content = schultz_dialog_content(tree, node);
    schultz_node_set_pane(tree, content, schultz_pane_vbox());
    /* Padding as well as a gap: setting only the gap would put the message
     * hard against the edge of the dialog it is in. */
    schultz_node_set_spacing(tree, content, 14.0f, 10.0f);

    result = schultz_label_create(tree, content, "", &box->message);
    if (result == SCHULTZ_OK) {
        result = schultz_label_create(tree, content, "", &box->detail);
    }
    if (result != SCHULTZ_OK) {
        free(box);
        schultz_node_destroy(tree, node);
        return result;
    }
    schultz_label_set_wrap(tree, box->message, 1);
    schultz_label_set_wrap(tree, box->detail, 1);
    schultz_node_set_style_property(tree, box->detail,
        SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_MUTED));

    if (kind == 1u) {
        result = schultz_text_field_create(tree, content, "", &box->input);
    } else if (kind == 2u) {
        result = schultz_combo_box_create(tree, content, &box->input);
    }
    if (result != SCHULTZ_OK) {
        free(box);
        schultz_node_destroy(tree, node);
        return result;
    }

    result = schultz_button_bar_create(tree, content, &box->bar);
    if (result == SCHULTZ_OK) {
        result = schultz_message_buttons(tree, box->bar, buttons);
    }
    if (result != SCHULTZ_OK) {
        free(box);
        schultz_node_destroy(tree, node);
        return result;
    }
    /*
     * The bar takes the height of the buttons in it. Pinning it to a number
     * makes its buttons a different size from every other button on screen:
     * a bar is a row, and a row stretches its children to its own height, so
     * a figure typed here wins over what a button asked for.
     */

    box->dialog = node;
    schultz_node_set_widget(tree, content, &schultz_message_widget, box);
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_message_dialog_create(schultz_tree *tree, const char *title,
                                      uint32_t icon, uint32_t buttons,
                                      schultz_handle *out_node)
{
    return schultz_message_build(tree, title, icon, buttons, 0u, out_node);
}

int32_t schultz_message_dialog_set_text(schultz_tree *tree,
                                        schultz_handle node,
                                        const char *message,
                                        const char *detail)
{
    schultz_message_data *box = schultz_message_of(tree, node);
    int32_t result;

    if (box == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    result = schultz_label_set_text(tree, box->message,
                                    (message == NULL) ? "" : message);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /* An empty detail takes no room rather than leaving a blank line. */
    schultz_node_set_state(tree, box->detail,
        (detail != NULL && detail[0] != '\0')
            ? (schultz_node_get_state(tree, box->detail)
                   | SCHULTZ_STATE_VISIBLE)
            : (schultz_node_get_state(tree, box->detail)
                   & ~(uint32_t)SCHULTZ_STATE_VISIBLE));
    return schultz_label_set_text(tree, box->detail,
                                  (detail == NULL) ? "" : detail);
}

int32_t schultz_message_dialog_open(schultz_tree *tree, schultz_handle node)
{
    schultz_message_data *box = schultz_message_of(tree, node);

    if (box == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* A fresh answer each time it is shown. */
    box->result = SCHULTZ_BUTTON_ROLE_COUNT;
    return schultz_dialog_open(tree, node);
}

uint32_t schultz_message_dialog_result(const schultz_tree *tree,
                                       schultz_handle node)
{
    const schultz_message_data *box;

    box = (const schultz_message_data *)schultz_message_of(
        (schultz_tree *)tree, node);
    return (box == NULL) ? SCHULTZ_BUTTON_ROLE_COUNT : box->result;
}

uint32_t schultz_message_dialog_icon(const schultz_tree *tree,
                                     schultz_handle node)
{
    const schultz_message_data *box;

    box = (const schultz_message_data *)schultz_message_of(
        (schultz_tree *)tree, node);
    return (box == NULL) ? SCHULTZ_DIALOG_ICON_NONE : box->icon;
}

int32_t schultz_text_input_dialog_create(schultz_tree *tree,
                                         const char *title,
                                         schultz_handle *out_node)
{
    return schultz_message_build(tree, title, SCHULTZ_DIALOG_ICON_QUESTION,
                                 SCHULTZ_DIALOG_OK_CANCEL, 1u, out_node);
}

int32_t schultz_text_input_dialog_set_value(schultz_tree *tree,
                                            schultz_handle node,
                                            const char *text)
{
    schultz_message_data *box = schultz_message_of(tree, node);

    if (box == NULL || box->kind != 1u) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_text_set(tree, box->input, (text == NULL) ? "" : text);
}

const char *schultz_text_input_dialog_value(const schultz_tree *tree,
                                            schultz_handle node)
{
    const schultz_message_data *box;

    box = (const schultz_message_data *)schultz_message_of(
        (schultz_tree *)tree, node);
    if (box == NULL || box->kind != 1u) {
        return NULL;
    }
    return schultz_text_get(tree, box->input);
}

int32_t schultz_text_input_dialog_set_allow_empty(schultz_tree *tree,
                                                  schultz_handle node,
                                                  int32_t on)
{
    schultz_message_data *box = schultz_message_of(tree, node);

    if (box == NULL || box->kind != 1u) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    box->allow_empty = on ? 1u : 0u;
    return SCHULTZ_OK;
}

int32_t schultz_choice_dialog_create(schultz_tree *tree, const char *title,
                                     schultz_handle *out_node)
{
    return schultz_message_build(tree, title, SCHULTZ_DIALOG_ICON_QUESTION,
                                 SCHULTZ_DIALOG_OK_CANCEL, 2u, out_node);
}

int32_t schultz_choice_dialog_add(schultz_tree *tree, schultz_handle node,
                                  const char *text)
{
    schultz_message_data *box = schultz_message_of(tree, node);

    if (box == NULL || box->kind != 2u) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_combo_box_add(tree, box->input, text);
}

int32_t schultz_choice_dialog_select(schultz_tree *tree, schultz_handle node,
                                     uint32_t index)
{
    schultz_message_data *box = schultz_message_of(tree, node);

    if (box == NULL || box->kind != 2u) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_combo_box_select(tree, box->input, index);
}

uint32_t schultz_choice_dialog_selected(const schultz_tree *tree,
                                        schultz_handle node)
{
    const schultz_message_data *box;

    box = (const schultz_message_data *)schultz_message_of(
        (schultz_tree *)tree, node);
    if (box == NULL || box->kind != 2u) {
        return 0u;
    }
    return schultz_combo_box_selected(tree, box->input);
}

/* -------------------------------------------------------- BusyIndicator */

/** A spinning arc, for work whose length is not known. */
typedef struct {
    float    angle;    /**< Where the arc has turned to, in degrees. */
    float    diameter; /**< How big it is drawn. */
    uint32_t running;  /**< Nonzero while it is turning. */
} schultz_busy_data;

static const schultz_widget_vtable schultz_busy_widget;

/** How fast the arc turns, in degrees a second. */
#define SCHULTZ_BUSY_SPEED 300.0f
/** How many straight pieces the arc is drawn with. */
#define SCHULTZ_BUSY_STEPS 12u

static schultz_busy_data *schultz_busy_of(schultz_tree *tree,
                                          schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_busy_widget) {
        return NULL;
    }
    return (schultz_busy_data *)schultz_node_widget_data(tree, node);
}

static int32_t schultz_busy_measure(schultz_tree *tree, schultz_handle node,
                                    float avail_w, float avail_h,
                                    schultz_size *out_size)
{
    const schultz_busy_data *busy =
        (const schultz_busy_data *)schultz_node_widget_data(tree, node);
    float size = (busy == NULL) ? 20.0f : busy->diameter;

    (void)avail_w;
    (void)avail_h;
    *out_size = schultz_size_make(size, size);
    return SCHULTZ_OK;
}

/* It reports a size and has nothing inside to place, so arrange is absent
 * on purpose rather than forgotten. */
static const schultz_pane_vtable schultz_busy_pane = {
    schultz_busy_measure, NULL
};

/*
 * Three quarters of a circle, drawn as short straight pieces. A real arc
 * would need a curve primitive the draw list does not have, and at this size
 * twelve pieces are indistinguishable from one.
 */
static int32_t schultz_busy_paint(schultz_tree *tree, schultz_handle node,
                                  schultz_draw_list *list,
                                  schultz_arena *arena)
{
    const schultz_busy_data *busy =
        (const schultz_busy_data *)schultz_node_widget_data(tree, node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_stroke stroke;
    schultz_rect bounds;
    float cx;
    float cy;
    float radius;
    uint32_t i;

    (void)arena;
    if (busy == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    cx = bounds.x + bounds.width * 0.5f;
    cy = bounds.y + bounds.height * 0.5f;
    radius = ((bounds.width < bounds.height) ? bounds.width : bounds.height)
                 * 0.5f - 2.0f;
    if (radius <= 0.0f) {
        return SCHULTZ_OK;
    }

    stroke = schultz_widget_stroke(style);
    stroke.paint = schultz_paint_solid(
        schultz_resolved_color(style, SCHULTZ_PROP_TEXT_COLOR));
    stroke.width = 2.0f;
    stroke.dash  = SCHULTZ_HANDLE_NONE;

    for (i = 0; i < SCHULTZ_BUSY_STEPS; i++) {
        float a0 = (busy->angle + (float)i * (270.0f / SCHULTZ_BUSY_STEPS))
                       * 3.14159265f / 180.0f;
        float a1 = (busy->angle + (float)(i + 1u)
                        * (270.0f / SCHULTZ_BUSY_STEPS))
                       * 3.14159265f / 180.0f;
        int32_t result = schultz_draw_line(list,
            schultz_point_make(cx + radius * cosf(a0),
                               cy + radius * sinf(a0)),
            schultz_point_make(cx + radius * cosf(a1),
                               cy + radius * sinf(a1)), stroke);

        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    return SCHULTZ_OK;
}

static int32_t schultz_busy_tick(schultz_tree *tree, schultz_handle node,
                                 uint64_t now_ms, uint32_t elapsed_ms)
{
    schultz_busy_data *busy = schultz_busy_of(tree, node);

    (void)now_ms;
    if (busy == NULL || !busy->running) {
        return 0;
    }
    busy->angle += SCHULTZ_BUSY_SPEED * (float)elapsed_ms / 1000.0f;
    while (busy->angle >= 360.0f) {
        busy->angle -= 360.0f;
    }
    return 1;
}

static const schultz_widget_vtable schultz_busy_widget = {
    .paint = schultz_busy_paint, .tick = schultz_busy_tick, .destroy = free
};

int32_t schultz_busy_indicator_create(schultz_tree *tree,
                                      schultz_handle parent,
                                      schultz_handle *out_node)
{
    schultz_busy_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_busy_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->diameter = 20.0f;

    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    schultz_node_set_pane(tree, node, &schultz_busy_pane);
    schultz_node_set_widget(tree, node, &schultz_busy_widget, data);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_TEXT_COLOR,
                            SCHULTZ_TOKEN_COLOR_ACCENT);
        schultz_widget_default_style(tree, node, &patch);
    }

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_busy_indicator_start(schultz_tree *tree, schultz_handle node)
{
    schultz_busy_data *busy = schultz_busy_of(tree, node);

    if (busy == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    busy->running = 1u;
    /* Only asks to be ticked while it is turning, so a stopped one costs
     * nothing at all. */
    return schultz_node_set_animating(tree, node, 1);
}

int32_t schultz_busy_indicator_stop(schultz_tree *tree, schultz_handle node)
{
    schultz_busy_data *busy = schultz_busy_of(tree, node);

    if (busy == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    busy->running = 0u;
    schultz_node_set_animating(tree, node, 0);
    return schultz_node_invalidate(tree, node);
}

int32_t schultz_busy_indicator_is_running(const schultz_tree *tree,
                                          schultz_handle node)
{
    const schultz_busy_data *busy;

    if (schultz_node_widget(tree, node) != &schultz_busy_widget) {
        return 0;
    }
    busy = (const schultz_busy_data *)schultz_node_widget_data(tree, node);
    return (busy == NULL) ? 0 : (int32_t)busy->running;
}

int32_t schultz_busy_indicator_set_size(schultz_tree *tree,
                                        schultz_handle node, float diameter)
{
    schultz_busy_data *busy = schultz_busy_of(tree, node);

    if (busy == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (diameter <= 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (busy->diameter == diameter) {
        return SCHULTZ_OK;
    }
    busy->diameter = diameter;
    return schultz_node_invalidate_layout(tree, node);
}

/* ---------------------------------------------------------------- Toast */

/** A brief message that shows itself and then gets out of the way. */
typedef struct {
    schultz_handle label;     /**< What it says. */
    uint32_t       remaining; /**< Milliseconds left, or zero when done. */
    /**
     * The window this toast was last placed against. A toast sits against an
     * edge, so a window that changes size moves the edge out from under it,
     * and nothing else would put it back: no pane places a toast, and the
     * two calls that do only run when a toast is shown or taken away.
     */
    schultz_rect   window;
} schultz_toast_data;

static const schultz_widget_vtable schultz_toast_widget;

/** How far a stack of toasts sits from the edge it is anchored to. */
#define SCHULTZ_TOAST_MARGIN 16.0f
/** The gap between stacked toasts. */
#define SCHULTZ_TOAST_GAP 8.0f

/** Where toasts appear. Set for the whole tree, not per toast. */
static uint32_t schultz_toast_place = SCHULTZ_TOAST_BOTTOM;

static schultz_toast_data *schultz_toast_of(schultz_tree *tree,
                                            schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_toast_widget) {
        return NULL;
    }
    return (schultz_toast_data *)schultz_node_widget_data(tree, node);
}

/*
 * Lays the showing toasts out in a stack against whichever edge was chosen.
 * Done for all of them at once, because where one sits depends on how many
 * are above it.
 */
/*
 * The rectangle a toast is placed against, in the root's own coordinates.
 *
 * The safe area rather than the whole window: what is left once a camera
 * notch and a home indicator are taken off. Against the window instead, a
 * toast along the bottom edge of a phone sat under the home indicator or past
 * the end of the screen entirely.
 *
 * And in the root's space rather than the window's, because a toast is a
 * child of the root and that is the space it is placed in.
 *
 * Asked here by both the placing and the check that decides whether to place
 * again, because when each worked it out for itself they drifted: the placing
 * was moved to the safe area and the check was left reading the window, so on
 * a phone the two never matched and every tick took the branch that restacks.
 * On a desktop they are the same rectangle and nothing showed.
 */
static int32_t schultz_toast_area(schultz_tree *tree, schultz_rect *out_area)
{
    schultz_rect box;

    if (schultz_tree_safe_area(tree, out_area) != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (schultz_node_get_bounds(tree, schultz_tree_root(tree), &box)
            == SCHULTZ_OK && !schultz_rect_is_empty(box)) {
        out_area->x -= box.x;
        out_area->y -= box.y;
    }
    return SCHULTZ_OK;
}

static void schultz_toast_restack(schultz_tree *tree)
{
    schultz_rect view;
    schultz_handle root = schultz_tree_root(tree);
    uint32_t count = schultz_node_child_count(tree, root);
    float offset = SCHULTZ_TOAST_MARGIN;
    uint32_t i;

    if (schultz_toast_area(tree, &view) != SCHULTZ_OK) {
        return;
    }
    for (i = 0; i < count; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;
        schultz_size size;
        schultz_rect at;

        if (schultz_node_child_at(tree, root, i, &child) != SCHULTZ_OK ||
            schultz_node_widget(tree, child) != &schultz_toast_widget ||
            !schultz_node_is_visible(tree, child)) {
            continue;
        }
        if (schultz_layout_measure(tree, child, view.width, -1.0f, &size)
                != SCHULTZ_OK) {
            continue;
        }
        at.width  = size.width;
        at.height = size.height;
        at.x      = view.x + (view.width - size.width) * 0.5f;
        if (schultz_toast_place == SCHULTZ_TOAST_TOP) {
            at.y = view.y + offset;
        } else {
            at.y = view.y + view.height - offset - size.height;
        }
        schultz_layout_arrange(tree, child, at);
        offset += size.height + SCHULTZ_TOAST_GAP;

        {
            schultz_toast_data *placed = schultz_toast_of(tree, child);

            if (placed != NULL) {
                placed->window = view;
            }
        }
    }
}

static int32_t schultz_toast_paint(schultz_tree *tree, schultz_handle node,
                                   schultz_draw_list *list,
                                   schultz_arena *arena)
{
    schultz_rect bounds;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    return schultz_widget_draw_box(list, schultz_widget_style(tree, node),
                                   bounds);
}

static int32_t schultz_toast_tick(schultz_tree *tree, schultz_handle node,
                                  uint64_t now_ms, uint32_t elapsed_ms)
{
    schultz_toast_data *toast = schultz_toast_of(tree, node);
    int32_t moved;

    (void)now_ms;
    if (toast == NULL || toast->remaining == 0u) {
        return 0;
    }
    /*
     * A window that changed size moved the edge this is sitting against. The
     * check is here because a showing toast is ticked every turn anyway, and
     * this is the only place that runs often enough to notice. It is the same
     * shape as the one layout makes for an anchored node, and for the same
     * reason: on a phone a rotation is a resize, and a toast half off the
     * bottom of the screen is worse than no toast.
     *
     * The countdown below runs either way. Returning here instead meant that
     * anything which made this branch fire every turn stopped the toast ever
     * expiring, and something did: the two sides of the comparison were
     * worked out in different coordinate spaces and never matched. A toast
     * that will not go away is a worse failure than one placed a few units
     * out, so the two are no longer allowed to depend on each other.
     */
    moved = 0;
    {
        schultz_rect area;

        if (schultz_toast_area(tree, &area) == SCHULTZ_OK &&
            !schultz_rect_equals(area, toast->window)) {
            schultz_toast_restack(tree);
            moved = 1;
        }
    }
    if (elapsed_ms >= toast->remaining) {
        /*
         * Hidden rather than destroyed, because this runs inside the walk
         * over everything that asked to be ticked and destroying a node from
         * in there would cut the walk off at the knees. A hidden toast is
         * picked up and used again by the next one shown.
         */
        toast->remaining = 0u;
        schultz_node_set_state(tree, node,
            schultz_node_get_state(tree, node)
                & ~(uint32_t)SCHULTZ_STATE_VISIBLE);
        schultz_node_set_animating(tree, node, 0);
        schultz_toast_restack(tree);
        return 1;
    }
    toast->remaining -= elapsed_ms;
    return moved;
}

static const schultz_widget_vtable schultz_toast_widget = {
    .paint = schultz_toast_paint, .tick = schultz_toast_tick, .destroy = free
};

/* A toast that has had its turn, so a new one need not build a node. */
static schultz_handle schultz_toast_spare(schultz_tree *tree)
{
    schultz_handle root = schultz_tree_root(tree);
    uint32_t count = schultz_node_child_count(tree, root);
    uint32_t i;

    for (i = 0; i < count; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(tree, root, i, &child) == SCHULTZ_OK &&
            schultz_node_widget(tree, child) == &schultz_toast_widget &&
            !schultz_node_is_visible(tree, child)) {
            return child;
        }
    }
    return SCHULTZ_HANDLE_NONE;
}

int32_t schultz_toast_show(schultz_tree *tree, const char *text, uint32_t ms,
                           schultz_handle *out_node)
{
    schultz_toast_data *toast;
    schultz_handle node = schultz_toast_spare(tree);
    schultz_patch patch;
    int32_t result;

    if (tree == NULL || ms == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    if (node == SCHULTZ_HANDLE_NONE) {
        result = schultz_node_create(tree, schultz_tree_root(tree), &node);
        if (result != SCHULTZ_OK) {
            return result;
        }
        toast = (schultz_toast_data *)calloc(1, sizeof(*toast));
        if (toast == NULL) {
            schultz_node_destroy(tree, node);
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        result = schultz_label_create(tree, node, text, &toast->label);
        if (result != SCHULTZ_OK) {
            free(toast);
            schultz_node_destroy(tree, node);
            return result;
        }
        schultz_label_set_wrap(tree, toast->label, 0);
        schultz_node_set_hit_testable(tree, toast->label, 0);
        schultz_node_set_pane(tree, node, schultz_pane_stack());
        schultz_node_set_widget(tree, node, &schultz_toast_widget, toast);
        /* It explains and gets out of the way, so it never takes the
         * pointer or the keyboard. */
        schultz_node_set_hit_testable(tree, node, 0);
        /*
         * And no pane places it. A toast is stacked against an edge of the
         * window by schultz_toast_restack, wherever it hangs in the tree, so
         * a pane on the root would otherwise sweep it into the row or column
         * with everything else. It is not an overlay, which is the other way
         * a node is left alone: it takes no input, and toasts come and go as
         * their timers run out rather than in the stack order the overlay
         * list keeps.
         */
        schultz_node_set_places_itself(tree, node, 1);

        if (schultz_patch_init(&patch) == SCHULTZ_OK) {
            schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                                SCHULTZ_TOKEN_COLOR_SURFACE_RAISED);
            schultz_patch_token(&patch, SCHULTZ_PROP_CORNER_RADIUS,
                                SCHULTZ_TOKEN_RADIUS_STRUCTURE);
            schultz_patch_token(&patch, SCHULTZ_PROP_PADDING,
                                SCHULTZ_TOKEN_SPACE_MD);
            schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                                SCHULTZ_TOKEN_COLOR_BORDER);
            schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_WIDTH,
                                SCHULTZ_TOKEN_BORDER_WIDTH);
            schultz_widget_default_style(tree, node, &patch);
        }
    } else {
        toast = schultz_toast_of(tree, node);
        if (toast == NULL) {
            return SCHULTZ_ERR_INVALID_HANDLE;
        }
        schultz_label_set_text(tree, toast->label, (text == NULL) ? "" : text);
    }

    toast->remaining = ms;
    schultz_node_set_state(tree, node,
        schultz_node_get_state(tree, node) | SCHULTZ_STATE_VISIBLE);
    schultz_node_set_animating(tree, node, 1);
    schultz_tree_resolve_styles(tree);
    schultz_toast_restack(tree);

    if (out_node != NULL) {
        *out_node = node;
    }
    return SCHULTZ_OK;
}

int32_t schultz_toast_dismiss(schultz_tree *tree, schultz_handle node)
{
    schultz_toast_data *toast = schultz_toast_of(tree, node);

    if (toast == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    toast->remaining = 0u;
    schultz_node_set_state(tree, node,
        schultz_node_get_state(tree, node)
            & ~(uint32_t)SCHULTZ_STATE_VISIBLE);
    schultz_node_set_animating(tree, node, 0);
    schultz_toast_restack(tree);
    return SCHULTZ_OK;
}

int32_t schultz_toast_is_showing(const schultz_tree *tree,
                                 schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_toast_widget) {
        return 0;
    }
    return schultz_node_is_visible(tree, node);
}

int32_t schultz_toast_set_position(uint32_t position)
{
    if (position != SCHULTZ_TOAST_TOP && position != SCHULTZ_TOAST_BOTTOM) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_toast_place = position;
    return SCHULTZ_OK;
}

/* ----------------------------------------------------------- Pagination */

/** Navigation across numbered pages. The pages themselves are the host's. */
typedef struct {
    uint32_t pages;   /**< How many there are. Never zero. */
    uint32_t current; /**< Which one is showing, zero based. */
    uint32_t most;    /**< The most number buttons to show at once. */
} schultz_pagination_data;

static const schultz_widget_vtable schultz_pagination_widget;

/** The four steps, which sit either side of the numbers. */
enum {
    SCHULTZ_PAGE_FIRST = 0,
    SCHULTZ_PAGE_PREVIOUS,
    SCHULTZ_PAGE_NEXT,
    SCHULTZ_PAGE_LAST,
    SCHULTZ_PAGE_STEPS
};

static schultz_pagination_data *schultz_pagination_of(schultz_tree *tree,
                                                      schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_pagination_widget) {
        return NULL;
    }
    return (schultz_pagination_data *)schultz_node_widget_data(tree, node);
}

/*
 * Which page the leftmost number button stands for. The run follows the
 * current page and stops at either end, so the numbers never scroll past
 * the last page or before the first.
 */
static uint32_t schultz_pagination_first(const schultz_pagination_data *page)
{
    uint32_t shown = (page->most < page->pages) ? page->most : page->pages;
    uint32_t half = shown / 2u;

    if (page->pages <= shown || page->current < half) {
        return 0u;
    }
    if (page->current + half >= page->pages) {
        return page->pages - shown;
    }
    return page->current - half;
}

/* Rewrites the number buttons for wherever the current page has moved to. */
static void schultz_pagination_relabel(schultz_tree *tree,
                                       schultz_handle node,
                                       schultz_pagination_data *page)
{
    uint32_t shown = (page->most < page->pages) ? page->most : page->pages;
    uint32_t first = schultz_pagination_first(page);
    uint32_t count = schultz_node_child_count(tree, node);
    uint32_t i;

    for (i = SCHULTZ_PAGE_STEPS; i < count; i++) {
        schultz_handle button = SCHULTZ_HANDLE_NONE;
        uint32_t index = i - SCHULTZ_PAGE_STEPS;
        uint32_t state;
        char text[16];

        if (schultz_node_child_at(tree, node, i, &button) != SCHULTZ_OK) {
            continue;
        }
        state = schultz_node_get_state(tree, button);
        if (index >= shown) {
            schultz_node_set_state(tree, button,
                state & ~(uint32_t)SCHULTZ_STATE_VISIBLE);
            continue;
        }
        state |= SCHULTZ_STATE_VISIBLE;

        /*
         * The far ends always name the first and last page, with an ellipsis
         * where the run was cut, so a reader can always see where the ends
         * are and jump straight to them.
         */
        if (shown < page->pages && index == 0u && first > 0u) {
            snprintf(text, sizeof(text), "1");
        } else if (shown < page->pages && index == shown - 1u &&
                   first + shown < page->pages) {
            snprintf(text, sizeof(text), "%u", page->pages);
        } else if (shown < page->pages && index == 1u && first > 1u) {
            snprintf(text, sizeof(text), "...");
        } else if (shown < page->pages && index == shown - 2u &&
                   first + shown < page->pages - 1u) {
            snprintf(text, sizeof(text), "...");
        } else {
            snprintf(text, sizeof(text), "%u", first + index + 1u);
        }
        schultz_label_set_text(tree, schultz_button_label(tree, button), text);
        schultz_node_set_name(tree, button, text);

        /* The one showing is marked, the way a chosen thing is everywhere. */
        if (first + index == page->current) {
            state |= SCHULTZ_STATE_SELECTED;
        } else {
            state &= ~(uint32_t)SCHULTZ_STATE_SELECTED;
        }
        schultz_node_set_state(tree, button, state);
    }

    /* A step that cannot go anywhere is disabled rather than hidden. */
    for (i = 0; i < SCHULTZ_PAGE_STEPS; i++) {
        schultz_handle button = SCHULTZ_HANDLE_NONE;
        uint32_t state;
        int32_t usable;

        if (schultz_node_child_at(tree, node, i, &button) != SCHULTZ_OK) {
            continue;
        }
        usable = (i == SCHULTZ_PAGE_FIRST || i == SCHULTZ_PAGE_PREVIOUS)
                     ? (page->current > 0u)
                     : (page->current + 1u < page->pages);
        state = schultz_node_get_state(tree, button);
        schultz_node_set_state(tree, button,
            usable ? (state | SCHULTZ_STATE_ENABLED)
                   : (state & ~(uint32_t)SCHULTZ_STATE_ENABLED));
    }
}

/* Clicks on the steps and the numbers both land here. */
static int32_t schultz_pagination_event(schultz_tree *tree,
                                        schultz_handle node,
                                        const schultz_event *event)
{
    schultz_pagination_data *page = schultz_pagination_of(tree, node);
    uint32_t count;
    uint32_t i;

    if (page == NULL || event->type != SCHULTZ_EVENT_CLICK) {
        return SCHULTZ_OK;
    }
    count = schultz_node_child_count(tree, node);
    for (i = 0; i < count; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(tree, node, i, &child) != SCHULTZ_OK ||
            child != event->target) {
            continue;
        }
        if (i < SCHULTZ_PAGE_STEPS) {
            uint32_t to = page->current;

            switch (i) {
            case SCHULTZ_PAGE_FIRST:    to = 0u; break;
            case SCHULTZ_PAGE_PREVIOUS: to = (page->current > 0u)
                                                 ? page->current - 1u : 0u;
                                        break;
            case SCHULTZ_PAGE_NEXT:     to = page->current + 1u; break;
            default:                    to = page->pages - 1u; break;
            }
            schultz_pagination_set_current(tree, node, to);
        } else {
            uint32_t index = i - SCHULTZ_PAGE_STEPS;
            const char *text = schultz_node_get_name(tree, child);

            /* An ellipsis is a gap, not a page. */
            if (text != NULL && text[0] == '.') {
                return SCHULTZ_OK;
            }
            schultz_pagination_set_current(tree, node,
                schultz_pagination_first(page) + index);
        }
        return SCHULTZ_OK;
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_pagination_widget = {
    .event = schultz_pagination_event, .destroy = free
};

int32_t schultz_pagination_create(schultz_tree *tree, schultz_handle parent,
                                  uint32_t pages, schultz_handle *out_node)
{
    static const char *const steps[SCHULTZ_PAGE_STEPS] = {
        "|<", "<", ">", ">|"
    };
    schultz_pagination_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    int32_t result;
    uint32_t i;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_pagination_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->pages   = (pages == 0u) ? 1u : pages;
    data->current = 0u;
    data->most    = 7u;

    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    schultz_node_set_pane(tree, node, schultz_pane_hbox());
    schultz_node_set_widget(tree, node, &schultz_pagination_widget, data);
    schultz_node_set_spacing(tree, node, 0.0f, 4.0f);

    /*
     * The steps first and then the number buttons, always the same count in
     * the same order, so a click can be read straight off the child index.
     * Buttons past the page count are hidden rather than destroyed.
     */
    for (i = 0; i < SCHULTZ_PAGE_STEPS; i++) {
        result = schultz_button_create(tree, node, steps[i], &button);
        if (result != SCHULTZ_OK) {
            schultz_node_destroy(tree, node);
            return result;
        }
    }
    for (i = 0; i < data->most; i++) {
        result = schultz_button_create(tree, node, "1", &button);
        if (result != SCHULTZ_OK) {
            schultz_node_destroy(tree, node);
            return result;
        }
        schultz_node_set_state_property(tree, button,
            SCHULTZ_STYLE_STATE_SELECTED, SCHULTZ_PROP_BACKGROUND,
            schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));
        schultz_node_set_state_property(tree, button,
            SCHULTZ_STYLE_STATE_SELECTED, SCHULTZ_PROP_TEXT_COLOR,
            schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_ON_ACCENT));
    }
    schultz_pagination_relabel(tree, node, data);

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_pagination_set_page_count(schultz_tree *tree,
                                          schultz_handle node, uint32_t pages)
{
    schultz_pagination_data *page = schultz_pagination_of(tree, node);

    if (page == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    page->pages = (pages == 0u) ? 1u : pages;
    if (page->current >= page->pages) {
        page->current = page->pages - 1u;
    }
    schultz_pagination_relabel(tree, node, page);
    return SCHULTZ_OK;
}

int32_t schultz_pagination_set_current(schultz_tree *tree,
                                       schultz_handle node, uint32_t index)
{
    schultz_pagination_data *page = schultz_pagination_of(tree, node);

    if (page == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (index >= page->pages) {
        index = page->pages - 1u;
    }
    page->current = index;
    schultz_pagination_relabel(tree, node, page);
    return SCHULTZ_OK;
}

uint32_t schultz_pagination_current(const schultz_tree *tree,
                                    schultz_handle node)
{
    const schultz_pagination_data *page;

    if (schultz_node_widget(tree, node) != &schultz_pagination_widget) {
        return 0u;
    }
    page = (const schultz_pagination_data *)schultz_node_widget_data(tree,
                                                                     node);
    return (page == NULL) ? 0u : page->current;
}

uint32_t schultz_pagination_page_count(const schultz_tree *tree,
                                       schultz_handle node)
{
    const schultz_pagination_data *page;

    if (schultz_node_widget(tree, node) != &schultz_pagination_widget) {
        return 0u;
    }
    page = (const schultz_pagination_data *)schultz_node_widget_data(tree,
                                                                     node);
    return (page == NULL) ? 0u : page->pages;
}

/* ------------------------------------------------------------- ListView */

/** A scrolling column of rows, with selection and keyboard navigation. */
typedef struct {
    schultz_handle scroll;  /**< The scroll view holding the rows. */
    schultz_handle content; /**< The column the rows are added to. */
    uint32_t       mode;    /**< One of SCHULTZ_SELECT_*. */
    uint32_t       current; /**< The row the keyboard is on. */
    uint32_t       anchor;  /**< Where a shift range measures from. */
} schultz_list_data;

static const schultz_widget_vtable schultz_list_widget;

static schultz_list_data *schultz_list_of(schultz_tree *tree,
                                          schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_list_widget) {
        return NULL;
    }
    return (schultz_list_data *)schultz_node_widget_data(tree, node);
}

/* Marks or unmarks one row. */
static void schultz_list_mark(schultz_tree *tree, schultz_handle row,
                              int32_t on)
{
    uint32_t state = schultz_node_get_state(tree, row);

    schultz_node_set_state(tree, row,
        on ? (state | SCHULTZ_STATE_SELECTED)
           : (state & ~(uint32_t)SCHULTZ_STATE_SELECTED));
}

/* Clears every row's mark. */
static void schultz_list_clear_marks(schultz_tree *tree,
                                     schultz_list_data *list)
{
    uint32_t count = schultz_node_child_count(tree, list->content);
    uint32_t i;

    for (i = 0; i < count; i++) {
        schultz_handle row = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(tree, list->content, i, &row)
                == SCHULTZ_OK) {
            schultz_list_mark(tree, row, 0);
        }
    }
}

/*
 * Which row a node sits in. A row is built out of whatever the host put in
 * it, so a click lands on a label or an icon rather than on the row itself
 * and has to be walked back up.
 */
static int32_t schultz_list_row_index(schultz_tree *tree,
                                      schultz_list_data *list,
                                      schultz_handle target,
                                      uint32_t *out_index)
{
    uint32_t count = schultz_node_child_count(tree, list->content);
    schultz_handle walk = target;

    while (walk != SCHULTZ_HANDLE_NONE) {
        schultz_handle parent = SCHULTZ_HANDLE_NONE;
        uint32_t i;

        for (i = 0; i < count; i++) {
            schultz_handle row = SCHULTZ_HANDLE_NONE;

            if (schultz_node_child_at(tree, list->content, i, &row)
                    == SCHULTZ_OK && row == walk) {
                *out_index = i;
                return 1;
            }
        }
        if (schultz_node_parent(tree, walk, &parent) != SCHULTZ_OK) {
            return 0;
        }
        walk = parent;
    }
    return 0;
}

/* Selects one row, or extends or toggles depending on the modifiers held. */
static void schultz_list_choose(schultz_tree *tree, schultz_handle node,
                                schultz_list_data *list, uint32_t index,
                                uint32_t modifiers)
{
    schultz_handle row = SCHULTZ_HANDLE_NONE;

    if (list->mode == SCHULTZ_SELECT_NONE ||
        schultz_node_child_at(tree, list->content, index, &row)
            != SCHULTZ_OK) {
        return;
    }
    list->current = index;

    if (list->mode == SCHULTZ_SELECT_MULTIPLE &&
        (modifiers & SCHULTZ_MOD_SHIFT)) {
        uint32_t low = (list->anchor < index) ? list->anchor : index;
        uint32_t high = (list->anchor < index) ? index : list->anchor;
        uint32_t i;

        /* A range replaces the selection rather than adding to it, and the
         * anchor stays put so dragging the far end keeps working. */
        schultz_list_clear_marks(tree, list);
        for (i = low; i <= high; i++) {
            schultz_handle each = SCHULTZ_HANDLE_NONE;

            if (schultz_node_child_at(tree, list->content, i, &each)
                    == SCHULTZ_OK) {
                schultz_list_mark(tree, each, 1);
            }
        }
        return;
    }

    if (list->mode == SCHULTZ_SELECT_MULTIPLE &&
        (modifiers & SCHULTZ_MOD_CTRL)) {
        schultz_list_mark(tree, row,
            !(schultz_node_get_state(tree, row) & SCHULTZ_STATE_SELECTED));
        list->anchor = index;
        return;
    }

    schultz_list_clear_marks(tree, list);
    schultz_list_mark(tree, row, 1);
    list->anchor = index;
    schultz_scroll_view_reveal(tree, list->scroll, row);
    (void)node;
}

/* Moves the keyboard by a number of rows, clamped at both ends. */
static void schultz_list_step(schultz_tree *tree, schultz_handle node,
                              schultz_list_data *list, int32_t by,
                              uint32_t modifiers)
{
    uint32_t count = schultz_node_child_count(tree, list->content);
    int32_t to;

    if (count == 0u) {
        return;
    }
    to = (int32_t)list->current + by;
    if (to < 0) {
        to = 0;
    }
    if (to >= (int32_t)count) {
        to = (int32_t)count - 1;
    }
    schultz_list_choose(tree, node, list, (uint32_t)to, modifiers);
    {
        schultz_handle row = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(tree, list->content, (uint32_t)to, &row)
                == SCHULTZ_OK) {
            schultz_scroll_view_reveal(tree, list->scroll, row);
        }
    }
}

/* How many rows fit in the view, for page up and page down. */
static int32_t schultz_list_page(schultz_tree *tree, schultz_list_data *list)
{
    schultz_handle first = SCHULTZ_HANDLE_NONE;
    schultz_rect view;
    schultz_rect row;
    int32_t rows;

    if (schultz_node_get_bounds(tree, list->scroll, &view) != SCHULTZ_OK ||
        schultz_node_child_at(tree, list->content, 0, &first) != SCHULTZ_OK ||
        schultz_node_get_bounds(tree, first, &row) != SCHULTZ_OK ||
        row.height <= 0.0f) {
        return 1;
    }
    rows = (int32_t)(view.height / row.height);
    return (rows < 1) ? 1 : rows;
}

static int32_t schultz_list_event(schultz_tree *tree, schultz_handle node,
                                  const schultz_event *event)
{
    schultz_list_data *list = schultz_list_of(tree, node);
    uint32_t index = 0;

    if (list == NULL || list->mode == SCHULTZ_SELECT_NONE) {
        return SCHULTZ_OK;
    }

    if (event->type == SCHULTZ_EVENT_CLICK) {
        if (schultz_list_row_index(tree, list, event->target, &index)) {
            schultz_list_choose(tree, node, list, index, event->modifiers);
        }
        return SCHULTZ_OK;
    }
    if (event->type != SCHULTZ_EVENT_KEY_DOWN) {
        return SCHULTZ_OK;
    }

    switch (event->key) {
    case SCHULTZ_KEY_UP:
        schultz_list_step(tree, node, list, -1, event->modifiers);
        return SCHULTZ_EVENT_CONSUMED;
    case SCHULTZ_KEY_DOWN:
        schultz_list_step(tree, node, list, 1, event->modifiers);
        return SCHULTZ_EVENT_CONSUMED;
    case SCHULTZ_KEY_HOME:
        schultz_list_step(tree, node, list, -(int32_t)
            schultz_node_child_count(tree, list->content), event->modifiers);
        return SCHULTZ_EVENT_CONSUMED;
    case SCHULTZ_KEY_END:
        schultz_list_step(tree, node, list, (int32_t)
            schultz_node_child_count(tree, list->content), event->modifiers);
        return SCHULTZ_EVENT_CONSUMED;
    case SCHULTZ_KEY_PAGE_UP:
        schultz_list_step(tree, node, list, -schultz_list_page(tree, list),
                          event->modifiers);
        return SCHULTZ_EVENT_CONSUMED;
    case SCHULTZ_KEY_PAGE_DOWN:
        schultz_list_step(tree, node, list, schultz_list_page(tree, list),
                          event->modifiers);
        return SCHULTZ_EVENT_CONSUMED;
    default:
        break;
    }
    /* Space toggles the current row, which is the only way to build a
     * scattered selection without a pointer. */
    if (event->key == (uint32_t)' ' &&
        list->mode == SCHULTZ_SELECT_MULTIPLE) {
        schultz_list_choose(tree, node, list, list->current,
                            SCHULTZ_MOD_CTRL);
        return SCHULTZ_EVENT_CONSUMED;
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_list_widget = {
    .event = schultz_list_event, .destroy = free
};

int32_t schultz_list_view_create(schultz_tree *tree, schultz_handle parent,
                                 schultz_handle *out_node)
{
    schultz_list_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_list_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->mode = SCHULTZ_SELECT_SINGLE;

    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    /*
     * A node of its own holding a scroll view, rather than being one. Taking
     * the scroll view's widget slot would break every schultz_scroll_view_
     * call on it, and the rows still rise through this node on their way to
     * the host, which is all the selection needs.
     */
    schultz_node_set_pane(tree, node, schultz_pane_stack());
    result = schultz_scroll_view_create(tree, node, &data->scroll);
    if (result != SCHULTZ_OK) {
        free(data);
        schultz_node_destroy(tree, node);
        return result;
    }
    data->content = schultz_scroll_view_content(tree, data->scroll);
    schultz_node_set_pane(tree, data->content, schultz_pane_vbox());
    schultz_node_set_spacing(tree, data->content, 0.0f, 1.0f);

    schultz_node_set_widget(tree, node, &schultz_list_widget, data);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_LIST);
    schultz_node_set_actions(tree, node, SCHULTZ_ACTION_FOCUS);

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_list_view_add(schultz_tree *tree, schultz_handle node,
                              schultz_handle *out_row)
{
    schultz_list_data *list = schultz_list_of(tree, node);
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_patch patch;
    int32_t result;

    if (list == NULL || out_row == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* A panel rather than a bare node: a row has to draw its own background
     * or being selected changes nothing but the colour of its text. */
    result = schultz_panel_create(tree, list->content, &row);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_set_pane(tree, row, schultz_pane_stack());
    schultz_node_set_role(tree, row, SCHULTZ_ROLE_LIST_ITEM);
    schultz_node_set_spacing(tree, row, 4.0f, 0.0f);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_widget_default_style(tree, row, &patch);
    }
    schultz_node_set_state_property(tree, row, SCHULTZ_STYLE_STATE_HOVERED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_SURFACE_RAISED));
    schultz_node_set_state_property(tree, row, SCHULTZ_STYLE_STATE_SELECTED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));
    schultz_node_set_state_property(tree, row, SCHULTZ_STYLE_STATE_SELECTED,
        SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_ON_ACCENT));

    *out_row = row;
    return SCHULTZ_OK;
}

int32_t schultz_list_view_remove(schultz_tree *tree, schultz_handle node,
                                 uint32_t index)
{
    schultz_list_data *list = schultz_list_of(tree, node);
    schultz_handle row = SCHULTZ_HANDLE_NONE;

    if (list == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (schultz_node_child_at(tree, list->content, index, &row)
            != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_node_destroy(tree, row);
    /*
     * The removed row takes its own selection with it, and the anchor goes
     * back to the start rather than pointing at whatever slid into the gap.
     */
    list->anchor = 0u;
    if (list->current >= schultz_node_child_count(tree, list->content)) {
        list->current = 0u;
    }
    return SCHULTZ_OK;
}

int32_t schultz_list_view_clear(schultz_tree *tree, schultz_handle node)
{
    schultz_list_data *list = schultz_list_of(tree, node);

    if (list == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    while (schultz_node_child_count(tree, list->content) > 0u) {
        schultz_handle row = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(tree, list->content, 0, &row)
                != SCHULTZ_OK) {
            break;
        }
        schultz_node_destroy(tree, row);
    }
    list->current = 0u;
    list->anchor  = 0u;
    return SCHULTZ_OK;
}

uint32_t schultz_list_view_count(const schultz_tree *tree,
                                 schultz_handle node)
{
    const schultz_list_data *list;

    if (schultz_node_widget(tree, node) != &schultz_list_widget) {
        return 0u;
    }
    list = (const schultz_list_data *)schultz_node_widget_data(tree, node);
    return (list == NULL) ? 0u : schultz_node_child_count(tree,
                                                          list->content);
}

schultz_handle schultz_list_view_row(const schultz_tree *tree,
                                     schultz_handle node, uint32_t index)
{
    const schultz_list_data *list;
    schultz_handle row = SCHULTZ_HANDLE_NONE;

    if (schultz_node_widget(tree, node) != &schultz_list_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    list = (const schultz_list_data *)schultz_node_widget_data(tree, node);
    if (list == NULL ||
        schultz_node_child_at(tree, list->content, index, &row)
            != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    return row;
}

int32_t schultz_list_view_set_selection_mode(schultz_tree *tree,
                                             schultz_handle node,
                                             uint32_t mode)
{
    schultz_list_data *list = schultz_list_of(tree, node);

    if (list == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (mode > SCHULTZ_SELECT_MULTIPLE) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    list->mode = mode;
    if (mode == SCHULTZ_SELECT_NONE) {
        schultz_list_clear_marks(tree, list);
    }
    return SCHULTZ_OK;
}

int32_t schultz_list_view_select(schultz_tree *tree, schultz_handle node,
                                 uint32_t index)
{
    schultz_list_data *list = schultz_list_of(tree, node);

    if (list == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (index >= schultz_node_child_count(tree, list->content)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_list_choose(tree, node, list, index, 0u);
    return SCHULTZ_OK;
}

int32_t schultz_list_view_select_range(schultz_tree *tree,
                                       schultz_handle node, uint32_t first,
                                       uint32_t last)
{
    schultz_list_data *list = schultz_list_of(tree, node);

    if (list == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (list->mode != SCHULTZ_SELECT_MULTIPLE) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (first >= schultz_node_child_count(tree, list->content) ||
        last >= schultz_node_child_count(tree, list->content)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    list->anchor = first;
    schultz_list_choose(tree, node, list, last, SCHULTZ_MOD_SHIFT);
    return SCHULTZ_OK;
}

int32_t schultz_list_view_deselect(schultz_tree *tree, schultz_handle node,
                                   uint32_t index)
{
    schultz_list_data *list = schultz_list_of(tree, node);
    schultz_handle row = SCHULTZ_HANDLE_NONE;

    if (list == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (schultz_node_child_at(tree, list->content, index, &row)
            != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_list_mark(tree, row, 0);
    return SCHULTZ_OK;
}

int32_t schultz_list_view_is_selected(const schultz_tree *tree,
                                      schultz_handle node, uint32_t index)
{
    schultz_handle row = schultz_list_view_row(tree, node, index);

    if (row == SCHULTZ_HANDLE_NONE) {
        return 0;
    }
    return (schultz_node_get_state(tree, row) & SCHULTZ_STATE_SELECTED)
               ? 1 : 0;
}

uint32_t schultz_list_view_selected_count(const schultz_tree *tree,
                                          schultz_handle node)
{
    uint32_t count = schultz_list_view_count(tree, node);
    uint32_t found = 0;
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (schultz_list_view_is_selected(tree, node, i)) {
            found++;
        }
    }
    return found;
}

int32_t schultz_list_view_selected(const schultz_tree *tree,
                                   schultz_handle node)
{
    uint32_t count = schultz_list_view_count(tree, node);
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (schultz_list_view_is_selected(tree, node, i)) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t schultz_list_view_selected_at(const schultz_tree *tree,
                                      schultz_handle node, uint32_t n)
{
    uint32_t count = schultz_list_view_count(tree, node);
    uint32_t found = 0;
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (schultz_list_view_is_selected(tree, node, i)) {
            if (found == n) {
                return (int32_t)i;
            }
            found++;
        }
    }
    return -1;
}

int32_t schultz_list_view_scroll_to(schultz_tree *tree, schultz_handle node,
                                    uint32_t index)
{
    schultz_list_data *list = schultz_list_of(tree, node);
    schultz_handle row = SCHULTZ_HANDLE_NONE;

    if (list == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (schultz_node_child_at(tree, list->content, index, &row)
            != SCHULTZ_OK) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return schultz_scroll_view_reveal(tree, list->scroll, row);
}

/* ------------------------------------------------------------- TreeView */

/*
 * The hierarchy is the node tree rather than a model beside it. A row is a
 * header and a column of child rows; collapsing hides the column. That is
 * what makes a tree cheap in a retained mode toolkit: there is nothing to
 * keep in step with the nodes, because the nodes are the structure.
 */

/** One row of a tree: its header, what hangs off it, and how deep it is. */
typedef struct {
    schultz_handle view;    /**< The tree view it belongs to. */
    schultz_handle content; /**< What the host fills, inside the header. */
    schultz_handle kids;    /**< The column of child rows. */
    uint32_t       depth;   /**< How far in it is indented. */
    uint32_t       open;    /**< Nonzero while its children are showing. */
} schultz_branch_data;

/** A tree view: a list of top level rows, and how far each level indents. */
typedef struct {
    schultz_handle scroll;  /**< The scroll view holding the rows. */
    schultz_handle content; /**< The column the top level rows are in. */
    schultz_handle current; /**< The header the keyboard is on. */
    float          indent;  /**< Pixels added for each level of depth. */
    uint32_t       mode;    /**< One of SCHULTZ_SELECT_*. */
} schultz_treeview_data;

static const schultz_widget_vtable schultz_treeview_widget;
static const schultz_widget_vtable schultz_branch_widget;

static schultz_treeview_data *schultz_treeview_of(schultz_tree *tree,
                                                  schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_treeview_widget) {
        return NULL;
    }
    return (schultz_treeview_data *)schultz_node_widget_data(tree, node);
}

/* The row a host holds is the node it filled, so its header is the parent. */
static schultz_handle schultz_branch_header(schultz_tree *tree,
                                            schultz_handle row)
{
    schultz_handle parent = SCHULTZ_HANDLE_NONE;

    if (schultz_node_widget(tree, row) == &schultz_branch_widget) {
        return row;
    }
    if (schultz_node_parent(tree, row, &parent) != SCHULTZ_OK ||
        schultz_node_widget(tree, parent) != &schultz_branch_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    return parent;
}

static schultz_branch_data *schultz_branch_of(schultz_tree *tree,
                                              schultz_handle row)
{
    schultz_handle header = schultz_branch_header(tree, row);

    if (header == SCHULTZ_HANDLE_NONE) {
        return NULL;
    }
    return (schultz_branch_data *)schultz_node_widget_data(tree, header);
}

/** @brief How wide the disclosure column is, children or not. */
#define SCHULTZ_TREE_ARROW 16.0f

/** @brief How far each level steps in when the view was not told otherwise. */
#define SCHULTZ_TREE_INDENT 16.0f

/* How far in a row sits, which is the view's setting and not the row's. */
static float schultz_branch_lead(schultz_tree *tree,
                                 const schultz_branch_data *branch)
{
    const schultz_treeview_data *view;
    float indent = SCHULTZ_TREE_INDENT;

    if (branch == NULL) {
        return SCHULTZ_TREE_ARROW;
    }
    view = (const schultz_treeview_data *)schultz_node_widget_data(tree,
                                                                   branch->view);
    if (view != NULL) {
        indent = view->indent;
    }
    return SCHULTZ_TREE_ARROW + (float)branch->depth * indent;
}

/*
 * A row is its own header strip with the rows under it stacked below. The
 * strip is as tall as what the host put in it, and the rest of the row is
 * whatever is open underneath, which is nothing at all while it is closed.
 */
static int32_t schultz_branch_measure(schultz_tree *tree, schultz_handle node,
                                      float avail_w, float avail_h,
                                      schultz_size *out_size)
{
    const schultz_branch_data *branch =
        (const schultz_branch_data *)schultz_node_widget_data(tree, node);
    schultz_size inner = { 0.0f, 0.0f };
    schultz_size kids = { 0.0f, 0.0f };
    float lead;

    if (branch == NULL) {
        *out_size = schultz_size_make(0.0f, 0.0f);
        return SCHULTZ_OK;
    }
    lead = schultz_branch_lead(tree, branch);
    schultz_layout_measure(tree, branch->content,
                           schultz_widget_inset(avail_w, lead), avail_h,
                           &inner);
    if (branch->open) {
        schultz_layout_measure(tree, branch->kids, avail_w, avail_h, &kids);
    }
    *out_size = schultz_size_make(
        (lead + inner.width > kids.width) ? lead + inner.width : kids.width,
        inner.height + kids.height);
    return SCHULTZ_OK;
}

static int32_t schultz_branch_arrange(schultz_tree *tree, schultz_handle node,
                                      schultz_rect rect)
{
    const schultz_branch_data *branch =
        (const schultz_branch_data *)schultz_node_widget_data(tree, node);
    schultz_size inner = { 0.0f, 0.0f };
    float lead;

    if (branch == NULL) {
        return SCHULTZ_OK;
    }
    lead = schultz_branch_lead(tree, branch);
    schultz_layout_measure(tree, branch->content, rect.width - lead, -1.0f,
                           &inner);
    schultz_layout_arrange(tree, branch->content,
        schultz_rect_make(lead, 0.0f, rect.width - lead, inner.height));
    /* Everything under this row goes below the strip, at the full width, so
     * a child's own indent is the only thing that steps it in. */
    return schultz_layout_arrange(tree, branch->kids,
        schultz_rect_make(0.0f, inner.height, rect.width,
                          rect.height - inner.height));
}

static const schultz_pane_vtable schultz_branch_pane = {
    schultz_branch_measure, schultz_branch_arrange
};

/* The triangle, drawn only on a row that has something under it. */
static int32_t schultz_branch_paint(schultz_tree *tree, schultz_handle node,
                                    schultz_draw_list *list,
                                    schultz_arena *arena)
{
    const schultz_branch_data *branch =
        (const schultz_branch_data *)schultz_node_widget_data(tree, node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_rect bounds;
    schultz_point tip[3];
    float x;
    float cy;
    int32_t result;

    (void)arena;
    if (branch == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    /* Only the strip is drawn, not the rows opened under it. */
    {
        schultz_rect head;

        if (schultz_node_get_bounds(tree, branch->content, &head)
                == SCHULTZ_OK && head.height > 0.0f) {
            bounds.height = head.height;
        }
    }
    result = schultz_widget_draw_box(list, style, bounds);
    if (result != SCHULTZ_OK ||
        schultz_node_child_count(tree, branch->kids) == 0u) {
        return result;
    }

    x  = bounds.x + schultz_branch_lead(tree, branch) - SCHULTZ_TREE_ARROW
             + 8.0f;
    cy = bounds.y + bounds.height * 0.5f;
    if (branch->open) {
        tip[0] = schultz_point_make(x - 4.0f, cy - 2.0f);
        tip[1] = schultz_point_make(x + 4.0f, cy - 2.0f);
        tip[2] = schultz_point_make(x, cy + 3.0f);
    } else {
        tip[0] = schultz_point_make(x - 2.0f, cy - 4.0f);
        tip[1] = schultz_point_make(x + 3.0f, cy);
        tip[2] = schultz_point_make(x - 2.0f, cy + 4.0f);
    }
    return schultz_draw_fill_polygon(list, tip, 3u,
        schultz_paint_solid(schultz_resolved_color(style,
                                                   SCHULTZ_PROP_TEXT_COLOR)),
        SCHULTZ_FILL_NONZERO);
}

static const schultz_widget_vtable schultz_branch_widget = {
    .paint = schultz_branch_paint, .destroy = free
};

/*
 * Walks the rows a reader can see, in the order they are drawn, skipping
 * whatever is collapsed. This is the order the up and down keys move in, and
 * it is the one thing a flat list gets for free and a tree does not.
 */
static uint32_t schultz_tree_visible(schultz_tree *tree, schultz_handle from,
                                     schultz_handle *out, uint32_t max,
                                     uint32_t at)
{
    uint32_t count = schultz_node_child_count(tree, from);
    uint32_t i;

    for (i = 0; i < count && at < max; i++) {
        schultz_handle header = SCHULTZ_HANDLE_NONE;
        const schultz_branch_data *branch;

        if (schultz_node_child_at(tree, from, i, &header) != SCHULTZ_OK ||
            schultz_node_widget(tree, header) != &schultz_branch_widget) {
            continue;
        }
        out[at++] = header;
        branch = (const schultz_branch_data *)schultz_node_widget_data(tree,
                                                                       header);
        if (branch != NULL && branch->open) {
            at = schultz_tree_visible(tree, branch->kids, out, max, at);
        }
    }
    return at;
}

/** The most rows the keyboard walks in one go. */
#define SCHULTZ_TREE_WALK_MAX 512u

static void schultz_tree_mark(schultz_tree *tree,
                              schultz_treeview_data *view,
                              schultz_handle header)
{
    schultz_handle rows[SCHULTZ_TREE_WALK_MAX];
    uint32_t count = schultz_tree_visible(tree, view->content, rows,
                                          SCHULTZ_TREE_WALK_MAX, 0u);
    uint32_t i;

    if (view->mode == SCHULTZ_SELECT_NONE) {
        return;
    }
    for (i = 0; i < count; i++) {
        uint32_t state = schultz_node_get_state(tree, rows[i]);

        schultz_node_set_state(tree, rows[i],
            (rows[i] == header) ? (state | SCHULTZ_STATE_SELECTED)
                                : (state & ~(uint32_t)SCHULTZ_STATE_SELECTED));
    }
    view->current = header;
    schultz_scroll_view_reveal(tree, view->scroll, header);
}

/* Moves the keyboard one visible row up or down. */
static void schultz_tree_step(schultz_tree *tree,
                              schultz_treeview_data *view, int32_t by)
{
    schultz_handle rows[SCHULTZ_TREE_WALK_MAX];
    uint32_t count = schultz_tree_visible(tree, view->content, rows,
                                          SCHULTZ_TREE_WALK_MAX, 0u);
    uint32_t i;

    if (count == 0u) {
        return;
    }
    for (i = 0; i < count; i++) {
        if (rows[i] == view->current) {
            int32_t to = (int32_t)i + by;

            if (to < 0) { to = 0; }
            if (to >= (int32_t)count) { to = (int32_t)count - 1; }
            schultz_tree_mark(tree, view, rows[to]);
            return;
        }
    }
    schultz_tree_mark(tree, view, rows[0]);
}

static int32_t schultz_treeview_event(schultz_tree *tree, schultz_handle node,
                                      const schultz_event *event)
{
    schultz_treeview_data *view = schultz_treeview_of(tree, node);
    schultz_branch_data *branch;

    if (view == NULL) {
        return SCHULTZ_OK;
    }

    if (event->type == SCHULTZ_EVENT_CLICK) {
        schultz_handle header = schultz_branch_header(tree, event->target);
        schultz_handle walk = event->target;

        /* A click lands on whatever the host put in the row, so walk up. */
        while (header == SCHULTZ_HANDLE_NONE &&
               schultz_node_parent(tree, walk, &walk) == SCHULTZ_OK &&
               walk != SCHULTZ_HANDLE_NONE) {
            header = schultz_branch_header(tree, walk);
        }
        if (header == SCHULTZ_HANDLE_NONE) {
            return SCHULTZ_OK;
        }
        branch = (schultz_branch_data *)schultz_node_widget_data(tree,
                                                                 header);
        /*
         * Inside the disclosure column opens and closes; anywhere else in the
         * row selects it. Measured from the row itself: the press lands on
         * whatever the host put in the row, so the event's local coordinates
         * belong to that node and not to this one.
         */
        if (branch != NULL) {
            schultz_rect strip;
            float from_left = 0.0f;

            if (schultz_node_absolute_bounds(tree, header, &strip)
                    == SCHULTZ_OK) {
                from_left = event->position.x - strip.x;
            }
            if (from_left < schultz_branch_lead(tree, branch)) {
                schultz_tree_view_expand(tree, node, header, !branch->open);
            } else {
                schultz_tree_mark(tree, view, header);
            }
        }
        return SCHULTZ_OK;
    }
    if (event->type != SCHULTZ_EVENT_KEY_DOWN) {
        return SCHULTZ_OK;
    }

    branch = (view->current == SCHULTZ_HANDLE_NONE)
                 ? NULL
                 : (schultz_branch_data *)schultz_node_widget_data(tree,
                                                                   view->current);
    switch (event->key) {
    case SCHULTZ_KEY_UP:
        schultz_tree_step(tree, view, -1);
        return SCHULTZ_EVENT_CONSUMED;
    case SCHULTZ_KEY_DOWN:
        schultz_tree_step(tree, view, 1);
        return SCHULTZ_EVENT_CONSUMED;
    case SCHULTZ_KEY_RIGHT:
        /* Opens a closed row, and steps into an open one. */
        if (branch != NULL &&
            schultz_node_child_count(tree, branch->kids) > 0u) {
            if (!branch->open) {
                schultz_tree_view_expand(tree, node, view->current, 1);
            } else {
                schultz_tree_step(tree, view, 1);
            }
        }
        return SCHULTZ_EVENT_CONSUMED;
    case SCHULTZ_KEY_LEFT:
        /* Closes an open row, and steps out of a closed one. */
        if (branch != NULL && branch->open) {
            schultz_tree_view_expand(tree, node, view->current, 0);
        } else if (branch != NULL) {
            schultz_handle up = SCHULTZ_HANDLE_NONE;

            /* The parent row is two levels up: past the column of children
             * that holds this row, then to the header that owns it. */
            if (schultz_node_parent(tree, view->current, &up) == SCHULTZ_OK &&
                schultz_node_parent(tree, up, &up) == SCHULTZ_OK &&
                schultz_node_widget(tree, up) == &schultz_branch_widget) {
                schultz_tree_mark(tree, view, up);
            }
        }
        return SCHULTZ_EVENT_CONSUMED;
    default:
        break;
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_treeview_widget = {
    .event = schultz_treeview_event, .destroy = free
};

int32_t schultz_tree_view_create(schultz_tree *tree, schultz_handle parent,
                                 schultz_handle *out_node)
{
    schultz_treeview_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_treeview_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->indent  = SCHULTZ_TREE_INDENT;
    data->mode    = SCHULTZ_SELECT_SINGLE;
    data->current = SCHULTZ_HANDLE_NONE;

    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    schultz_node_set_pane(tree, node, schultz_pane_stack());
    result = schultz_scroll_view_create(tree, node, &data->scroll);
    if (result != SCHULTZ_OK) {
        free(data);
        schultz_node_destroy(tree, node);
        return result;
    }
    data->content = schultz_scroll_view_content(tree, data->scroll);
    schultz_node_set_pane(tree, data->content, schultz_pane_vbox());
    schultz_node_set_spacing(tree, data->content, 0.0f, 1.0f);

    schultz_node_set_widget(tree, node, &schultz_treeview_widget, data);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_LIST);
    schultz_node_set_actions(tree, node, SCHULTZ_ACTION_FOCUS);

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_tree_view_add(schultz_tree *tree, schultz_handle node,
                              schultz_handle parent_row,
                              schultz_handle *out_row)
{
    schultz_treeview_data *view = schultz_treeview_of(tree, node);
    schultz_branch_data *branch;
    schultz_branch_data *above = NULL;
    schultz_handle into;
    schultz_handle header = SCHULTZ_HANDLE_NONE;
    schultz_patch patch;
    int32_t result;

    if (view == NULL || out_row == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (parent_row == SCHULTZ_HANDLE_NONE) {
        into = view->content;
    } else {
        above = schultz_branch_of(tree, parent_row);
        if (above == NULL) {
            return SCHULTZ_ERR_INVALID_HANDLE;
        }
        into = above->kids;
    }

    branch = (schultz_branch_data *)calloc(1, sizeof(*branch));
    if (branch == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    branch->view  = node;
    branch->depth = (above == NULL) ? 0u : above->depth + 1u;

    result = schultz_node_create(tree, into, &header);
    if (result != SCHULTZ_OK) {
        free(branch);
        return result;
    }
    result = schultz_node_create(tree, header, &branch->content);
    if (result == SCHULTZ_OK) {
        result = schultz_node_create(tree, header, &branch->kids);
    }
    if (result != SCHULTZ_OK) {
        free(branch);
        schultz_node_destroy(tree, header);
        return result;
    }
    /* The content holds one thing and sizes itself around it; the children
     * are a column that is hidden until the row is opened. */
    schultz_node_set_pane(tree, branch->content, schultz_pane_stack());
    schultz_node_set_pane(tree, branch->kids, schultz_pane_vbox());
    schultz_node_set_spacing(tree, branch->kids, 0.0f, 1.0f);
    /*
     * Text colour inherits, and a selected row sets it to the one that reads
     * against the accent. The rows underneath are not selected, so they say
     * plainly what colour they are rather than taking their parent's.
     */
    schultz_node_set_style_property(tree, branch->kids,
        SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT));
    schultz_node_set_state(tree, branch->kids,
        schultz_node_get_state(tree, branch->kids)
            & ~(uint32_t)SCHULTZ_STATE_VISIBLE);

    schultz_node_set_pane(tree, header, &schultz_branch_pane);
    schultz_node_set_widget(tree, header, &schultz_branch_widget, branch);
    schultz_node_set_role(tree, header, SCHULTZ_ROLE_LIST_ITEM);
    schultz_node_set_spacing(tree, header, 2.0f, 0.0f);

    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_widget_default_style(tree, header, &patch);
    }
    schultz_node_set_state_property(tree, header, SCHULTZ_STYLE_STATE_HOVERED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_SURFACE_RAISED));
    schultz_node_set_state_property(tree, header, SCHULTZ_STYLE_STATE_SELECTED,
        SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));
    schultz_node_set_state_property(tree, header, SCHULTZ_STYLE_STATE_SELECTED,
        SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_ON_ACCENT));

    /* Opening the parent is the host's call, but a row that has just gained
     * its first child would otherwise never show a triangle. */
    if (above != NULL) {
        schultz_node_invalidate(tree, schultz_branch_header(tree, parent_row));
    }

    *out_row = branch->content;
    return SCHULTZ_OK;
}

int32_t schultz_tree_view_remove(schultz_tree *tree, schultz_handle node,
                                 schultz_handle row)
{
    schultz_treeview_data *view = schultz_treeview_of(tree, node);
    schultz_handle header = schultz_branch_header(tree, row);

    if (view == NULL || header == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (view->current == header) {
        view->current = SCHULTZ_HANDLE_NONE;
    }
    return schultz_node_destroy(tree, header);
}

int32_t schultz_tree_view_expand(schultz_tree *tree, schultz_handle node,
                                 schultz_handle row, int32_t expanded)
{
    schultz_treeview_data *view = schultz_treeview_of(tree, node);
    schultz_handle header = schultz_branch_header(tree, row);
    schultz_branch_data *branch;

    if (view == NULL || header == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    branch = (schultz_branch_data *)schultz_node_widget_data(tree, header);
    if (branch == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (branch->open == (expanded ? 1u : 0u)) {
        return SCHULTZ_OK;
    }
    branch->open = expanded ? 1u : 0u;
    schultz_node_set_state(tree, branch->kids,
        expanded ? (schultz_node_get_state(tree, branch->kids)
                        | SCHULTZ_STATE_VISIBLE)
                 : (schultz_node_get_state(tree, branch->kids)
                        & ~(uint32_t)SCHULTZ_STATE_VISIBLE));
    schultz_node_invalidate(tree, header);
    return schultz_node_invalidate_layout(tree, node);
}

int32_t schultz_tree_view_is_expanded(const schultz_tree *tree,
                                      schultz_handle row)
{
    const schultz_branch_data *branch =
        schultz_branch_of((schultz_tree *)tree, row);

    return (branch == NULL) ? 0 : (int32_t)branch->open;
}

uint32_t schultz_tree_view_depth(const schultz_tree *tree, schultz_handle row)
{
    const schultz_branch_data *branch =
        schultz_branch_of((schultz_tree *)tree, row);

    return (branch == NULL) ? 0u : branch->depth;
}

int32_t schultz_tree_view_set_indent(schultz_tree *tree, schultz_handle node,
                                     float pixels)
{
    schultz_treeview_data *view = schultz_treeview_of(tree, node);

    if (view == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (pixels < 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (view->indent == pixels) {
        return SCHULTZ_OK;
    }
    view->indent = pixels;
    return schultz_node_invalidate_layout(tree, node);
}

int32_t schultz_tree_view_set_selection_mode(schultz_tree *tree,
                                             schultz_handle node,
                                             uint32_t mode)
{
    schultz_treeview_data *view = schultz_treeview_of(tree, node);

    if (view == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (mode > SCHULTZ_SELECT_MULTIPLE) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    view->mode = mode;
    return SCHULTZ_OK;
}

int32_t schultz_tree_view_select(schultz_tree *tree, schultz_handle node,
                                 schultz_handle row)
{
    schultz_treeview_data *view = schultz_treeview_of(tree, node);
    schultz_handle header = schultz_branch_header(tree, row);

    if (view == NULL || header == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_tree_mark(tree, view, header);
    return SCHULTZ_OK;
}

schultz_handle schultz_tree_view_selected(const schultz_tree *tree,
                                          schultz_handle node)
{
    const schultz_treeview_data *view;
    const schultz_branch_data *branch;

    if (schultz_node_widget(tree, node) != &schultz_treeview_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    view = (const schultz_treeview_data *)schultz_node_widget_data(tree,
                                                                   node);
    if (view == NULL || view->current == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_HANDLE_NONE;
    }
    branch = (const schultz_branch_data *)schultz_node_widget_data(tree,
                                                                   view->current);
    /* The host holds the node it filled, so that is what it gets back. */
    return (branch == NULL) ? SCHULTZ_HANDLE_NONE : branch->content;
}

int32_t schultz_tree_view_scroll_to(schultz_tree *tree, schultz_handle node,
                                    schultz_handle row)
{
    schultz_treeview_data *view = schultz_treeview_of(tree, node);
    schultz_handle header = schultz_branch_header(tree, row);

    if (view == NULL || header == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_scroll_view_reveal(tree, view->scroll, header);
}

/* ----------------------------------------------------------------- Relay */

/*
 * A popover hangs off the root rather than off whatever opened it, so a click
 * inside one rises to the root and never passes the widget that owns it. That
 * is deliberate: an overlay has to draw over everything, and the paint order
 * follows the tree.
 *
 * The cost is that a picker cannot hear its own calendar. This carries the
 * events back: it sits on the popover's content and hands whatever happens
 * there to the owner's own handler, target and all.
 */
/** @brief Which widget a popover's contents should report to. */
typedef struct {
    schultz_handle owner; /**< The widget the popover belongs to. */
} schultz_relay_data;

static int32_t schultz_relay_event(schultz_tree *tree, schultz_handle node,
                                   const schultz_event *event)
{
    const schultz_relay_data *relay =
        (const schultz_relay_data *)schultz_node_widget_data(tree, node);
    const schultz_widget_vtable *widget;

    if (relay == NULL) {
        return SCHULTZ_OK;
    }
    widget = schultz_node_widget(tree, relay->owner);
    if (widget == NULL || widget->event == NULL) {
        return SCHULTZ_OK;
    }
    /*
     * Not to an owner that has been switched off. This hands the event
     * straight to the owner's handler, so it goes round the router, and the
     * router is where a disabled node is refused. The panel this comes from
     * hangs off the root rather than off the owner, so it is not disabled
     * along with it and the router sees nothing wrong.
     */
    if (!schultz_node_is_enabled(tree, relay->owner)) {
        return SCHULTZ_OK;
    }
    return widget->event(tree, relay->owner, event);
}

static const schultz_widget_vtable schultz_relay_widget = {
    .event = schultz_relay_event, .destroy = free
};

/* Points a popover's content back at the widget that owns it. */
static int32_t schultz_relay_install(schultz_tree *tree, schultz_handle node,
                                     schultz_handle owner)
{
    schultz_relay_data *relay =
        (schultz_relay_data *)calloc(1, sizeof(*relay));

    if (relay == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    relay->owner = owner;
    return schultz_node_set_widget(tree, node, &schultz_relay_widget, relay);
}

/* ----------------------------------------------------------- NumberField */

/** A text field constrained to numbers, with a button at each end. */
typedef struct {
    schultz_handle field;    /**< The text field holding the digits. */
    schultz_handle up;       /**< Steps the value up. */
    schultz_handle down;     /**< Steps it down. */
    double         minimum;  /**< Lowest value it will take. */
    double         maximum;  /**< Highest value. */
    double         value;    /**< Where it is now, always within range. */
    double         step;     /**< How far one press or arrow key moves it. */
    uint32_t       decimals; /**< How many places are shown. */
    uint32_t       wrap;     /**< Nonzero to run off one end onto the other. */
    uint32_t       steps;    /**< SCHULTZ_STEPS_BESIDE or _ABOVE_BELOW. */
    uint32_t       held;     /**< Which button is down, 1 up and 2 down. */
    uint32_t       held_ms;  /**< How long it has been held. */
} schultz_number_data;

static const schultz_widget_vtable schultz_number_widget;
static void schultz_number_step_press(schultz_tree *tree,
                                      schultz_handle step,
                                      int32_t down);

/** How long a button is held before it starts repeating. */
#define SCHULTZ_REPEAT_DELAY_MS 400u
/** How long between repeats after that. */
#define SCHULTZ_REPEAT_RATE_MS 60u
/** How wide the two step buttons are. */
/** The space before each step button: one after the field, one between. */
#define SCHULTZ_STEP_GAP 4.0f

static schultz_number_data *schultz_number_of(schultz_tree *tree,
                                              schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_number_widget) {
        return NULL;
    }
    return (schultz_number_data *)schultz_node_widget_data(tree, node);
}

/*
 * Writes the value into the field. Formatting is done here rather than left
 * to whatever the user typed, so the field always shows a number in the shape
 * this field was told to use.
 */
static void schultz_number_show(schultz_tree *tree,
                                schultz_number_data *number)
{
    char text[48];

    snprintf(text, sizeof(text), "%.*f", (int)number->decimals,
             number->value);
    schultz_text_set(tree, number->field, text);
}

/* Brings a value inside the range, wrapping round instead when asked. */
static double schultz_number_clamp(const schultz_number_data *number,
                                   double value)
{
    if (value < number->minimum) {
        return number->wrap ? number->maximum : number->minimum;
    }
    if (value > number->maximum) {
        return number->wrap ? number->minimum : number->maximum;
    }
    return value;
}

static void schultz_number_move(schultz_tree *tree, schultz_handle node,
                                schultz_number_data *number, double by)
{
    number->value = schultz_number_clamp(number, number->value + by);
    schultz_number_show(tree, number);
    schultz_node_invalidate(tree, node);
}

/*
 * Reads what was typed. Anything that is not a number, or is only partly one,
 * puts the last good value back rather than guessing at what was meant.
 */
static void schultz_number_take(schultz_tree *tree,
                                schultz_number_data *number)
{
    const char *text = schultz_text_get(tree, number->field);
    char *end = NULL;
    double parsed;

    if (text == NULL || text[0] == '\0') {
        schultz_number_show(tree, number);
        return;
    }
    parsed = strtod(text, &end);
    if (end == text || (end != NULL && *end != '\0')) {
        schultz_number_show(tree, number);
        return;
    }
    number->value = schultz_number_clamp(number, parsed);
    schultz_number_show(tree, number);
}

static int32_t schultz_number_measure(schultz_tree *tree, schultz_handle node,
                                      float avail_w, float avail_h,
                                      schultz_size *out_size)
{
    const schultz_number_data *number =
        (const schultz_number_data *)schultz_node_widget_data(tree, node);
    schultz_size field = { 0.0f, 0.0f };
    float steps;

    if (number == NULL) {
        *out_size = schultz_size_make(0.0f, 0.0f);
        return SCHULTZ_OK;
    }
    /*
     * Either way a step is one row high, so the field has to be measured
     * before there is a row height to build from.
     */
    schultz_layout_measure(tree, number->field, avail_w, avail_h, &field);
    if (number->steps == SCHULTZ_STEPS_ABOVE_BELOW) {
        /*
         * A column of three: the button over the field, the field, the
         * button under it. The field keeps its own width, which is what lets
         * several of these stand side by side in a narrow popup.
         */
        *out_size = schultz_size_make(field.width,
            field.height * 3.0f + SCHULTZ_STEP_GAP * 2.0f);
        return SCHULTZ_OK;
    }
    /*
     * Beside, the steps are squares, so their width is that row height, and
     * the field is measured again once it is known: the second measure is
     * the one given the width the steps leave. A single line field's height
     * does not depend on the width it is offered, so the first answer's
     * height stands.
     */
    steps = field.height * 2.0f + SCHULTZ_STEP_GAP * 2.0f;
    if (!schultz_layout_is_unbounded(avail_w)) {
        schultz_layout_measure(tree, number->field,
                               schultz_widget_inset(avail_w, steps), avail_h,
                               &field);
    }
    *out_size = schultz_size_make(field.width + steps, field.height);
    return SCHULTZ_OK;
}

static int32_t schultz_number_arrange(schultz_tree *tree, schultz_handle node,
                                      schultz_rect rect)
{
    const schultz_number_data *number =
        (const schultz_number_data *)schultz_node_widget_data(tree, node);
    float step;
    float text;

    if (number == NULL) {
        return SCHULTZ_OK;
    }
    if (number->steps == SCHULTZ_STEPS_ABOVE_BELOW) {
        /* Three rows of one third each, up over the field and down under. */
        float row = (rect.height - SCHULTZ_STEP_GAP * 2.0f) / 3.0f;

        if (row < 0.0f) {
            row = 0.0f;
        }
        schultz_layout_arrange(tree, number->up,
            schultz_rect_make(0.0f, 0.0f, rect.width, row));
        schultz_layout_arrange(tree, number->field,
            schultz_rect_make(0.0f, row + SCHULTZ_STEP_GAP, rect.width, row));
        schultz_layout_arrange(tree, number->down,
            schultz_rect_make(0.0f, (row + SCHULTZ_STEP_GAP) * 2.0f,
                              rect.width, row));
        return SCHULTZ_OK;
    }
    /* Square, so as wide as the row is tall. One over the other, each step
     * was half a row high and a finger could not tell them apart. */
    step = rect.height;
    text = rect.width - (step * 2.0f + SCHULTZ_STEP_GAP * 2.0f);
    if (text < 0.0f) {
        text = 0.0f;
    }
    schultz_layout_arrange(tree, number->field,
        schultz_rect_make(0.0f, 0.0f, text, rect.height));
    /* Down then up, left to right, the way a minus sits left of a plus. */
    schultz_layout_arrange(tree, number->down,
        schultz_rect_make(rect.width - step * 2.0f - SCHULTZ_STEP_GAP, 0.0f,
                          step, rect.height));
    schultz_layout_arrange(tree, number->up,
        schultz_rect_make(rect.width - step, 0.0f, step, rect.height));
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_number_pane = {
    schultz_number_measure, schultz_number_arrange
};

/* One arrow, pointing up or down, centred in whatever it was given. */
static int32_t schultz_number_arrow(schultz_draw_list *list,
                                    const schultz_resolved_style *style,
                                    schultz_rect box, int32_t up)
{
    schultz_point tip[3];
    float cx = box.x + box.width * 0.5f;
    float cy = box.y + box.height * 0.5f;
    float side = (box.width < box.height) ? box.width : box.height;
    float half;
    float rise;

    if (box.width <= 0.0f || box.height <= 0.0f) {
        return SCHULTZ_OK;
    }
    /* Sized from the square rather than fixed, so the arrow still fills its
     * button on a screen where the row is twice as tall. */
    half = side * 0.24f;
    rise = side * 0.15f;
    if (up) {
        tip[0] = schultz_point_make(cx - half, cy + rise);
        tip[1] = schultz_point_make(cx + half, cy + rise);
        tip[2] = schultz_point_make(cx, cy - rise);
    } else {
        tip[0] = schultz_point_make(cx - half, cy - rise);
        tip[1] = schultz_point_make(cx + half, cy - rise);
        tip[2] = schultz_point_make(cx, cy + rise);
    }
    return schultz_draw_fill_polygon(list, tip, 3u,
        schultz_paint_solid(schultz_resolved_color(style,
                                                   SCHULTZ_PROP_TEXT_COLOR)),
        SCHULTZ_FILL_NONZERO);
}

/* One step: a button sized box with its arrow centred in it. */
static int32_t schultz_number_step_paint(schultz_tree *tree,
                                         schultz_handle step,
                                         schultz_draw_list *list,
                                         schultz_rect box, int32_t up)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, step);
    int32_t result = schultz_widget_draw_box(list, style, box);

    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_number_arrow(list, style, box, up);
}

static int32_t schultz_number_paint(schultz_tree *tree, schultz_handle node,
                                    schultz_draw_list *list,
                                    schultz_arena *arena)
{
    const schultz_number_data *number =
        (const schultz_number_data *)schultz_node_widget_data(tree, node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_rect box;
    int32_t result;

    (void)arena;
    if (number == NULL) {
        return SCHULTZ_OK;
    }
    if (schultz_node_absolute_bounds(tree, number->down, &box) == SCHULTZ_OK) {
        result = schultz_number_step_paint(tree, number->down, list, box, 0);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    if (schultz_node_absolute_bounds(tree, number->up, &box) == SCHULTZ_OK) {
        return schultz_number_step_paint(tree, number->up, list, box, 1);
    }
    (void)style;
    return SCHULTZ_OK;
}

static int32_t schultz_number_event(schultz_tree *tree, schultz_handle node,
                                    const schultz_event *event)
{
    schultz_number_data *number = schultz_number_of(tree, node);

    if (number == NULL) {
        return SCHULTZ_OK;
    }

    if (event->type == SCHULTZ_EVENT_MOUSE_DOWN) {
        if (event->target == number->up || event->target == number->down) {
            number->held = (event->target == number->up) ? 1u : 2u;
            number->held_ms = 0u;
            schultz_number_step_press(tree, event->target, 1);
            schultz_number_move(tree, node, number,
                                (number->held == 1u) ? number->step
                                                     : -number->step);
            schultz_node_set_animating(tree, node, 1);
            return SCHULTZ_EVENT_CONSUMED;
        }
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_MOUSE_UP) {
        if (number->held != 0u) {
            schultz_number_step_press(tree,
                (number->held == 1u) ? number->up : number->down, 0);
        }
        number->held = 0u;
        schultz_node_set_animating(tree, node, 0);
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_FOCUS_LOST &&
        event->target == number->field) {
        /* What was typed is read when the field is left, so a half typed
         * number is not fought with while it is being typed. */
        schultz_number_take(tree, number);
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_KEY_DOWN) {
        if (event->key == (uint32_t)SCHULTZ_KEY_UP) {
            schultz_number_move(tree, node, number, number->step);
            return SCHULTZ_EVENT_CONSUMED;
        }
        if (event->key == (uint32_t)SCHULTZ_KEY_DOWN) {
            schultz_number_move(tree, node, number, -number->step);
            return SCHULTZ_EVENT_CONSUMED;
        }
        if (event->key == (uint32_t)SCHULTZ_KEY_RETURN) {
            schultz_number_take(tree, number);
            return SCHULTZ_EVENT_CONSUMED;
        }
    }
    return SCHULTZ_OK;
}

/* Holding a step button repeats it, after a pause so one press is one step. */
static int32_t schultz_number_tick(schultz_tree *tree, schultz_handle node,
                                   uint64_t now_ms, uint32_t elapsed_ms)
{
    schultz_number_data *number = schultz_number_of(tree, node);
    uint32_t before;

    (void)now_ms;
    if (number == NULL || number->held == 0u) {
        return 0;
    }
    before = number->held_ms;
    number->held_ms += elapsed_ms;
    if (number->held_ms < SCHULTZ_REPEAT_DELAY_MS) {
        return 0;
    }
    /* One step per rate period, however long a frame happened to take. */
    if (before >= SCHULTZ_REPEAT_DELAY_MS &&
        (number->held_ms - SCHULTZ_REPEAT_DELAY_MS) / SCHULTZ_REPEAT_RATE_MS ==
        (before - SCHULTZ_REPEAT_DELAY_MS) / SCHULTZ_REPEAT_RATE_MS) {
        return 0;
    }
    schultz_number_move(tree, node, number,
                        (number->held == 1u) ? number->step : -number->step);
    return 1;
}

static const schultz_widget_vtable schultz_number_widget = {
    .paint = schultz_number_paint,
    .event = schultz_number_event,
    .tick = schultz_number_tick,
    .destroy = free
};

/*
 * A step's look: the same surface, border and corner a button has, so the two
 * read as buttons beside the field. Installed as the node's default style so
 * a host restyling them wins over this.
 */
static void schultz_number_step_style(schultz_tree *tree, schultz_handle step)
{
    schultz_patch patch;

    if (schultz_patch_init(&patch) != SCHULTZ_OK) {
        return;
    }
    schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                        SCHULTZ_TOKEN_COLOR_SURFACE);
    schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                        SCHULTZ_TOKEN_COLOR_BORDER);
    schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_WIDTH,
                        SCHULTZ_TOKEN_BORDER_WIDTH);
    schultz_patch_token(&patch, SCHULTZ_PROP_CORNER_RADIUS,
                        SCHULTZ_TOKEN_RADIUS_CONTROL);
    schultz_widget_default_style(tree, step, &patch);
}

/* Held steps sink, the way a pressed button does. Set here rather than left
 * to state styling, which a bare node does not carry. */
static void schultz_number_step_press(schultz_tree *tree, schultz_handle step,
                                      int32_t down)
{
    schultz_node_set_style_property(tree, step, SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(down ? SCHULTZ_TOKEN_COLOR_SURFACE_SUNKEN
                                 : SCHULTZ_TOKEN_COLOR_SURFACE));
}

int32_t schultz_number_field_create(schultz_tree *tree, schultz_handle parent,
                                    double minimum, double maximum,
                                    double value, schultz_handle *out_node)
{
    schultz_number_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (out_node == NULL || minimum > maximum) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_number_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->minimum = minimum;
    data->maximum = maximum;
    data->step    = 1.0;

    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    result = schultz_text_field_create(tree, node, "", &data->field);
    if (result == SCHULTZ_OK) {
        result = schultz_node_create(tree, node, &data->up);
    }
    if (result == SCHULTZ_OK) {
        result = schultz_node_create(tree, node, &data->down);
    }
    if (result != SCHULTZ_OK) {
        free(data);
        schultz_node_destroy(tree, node);
        return result;
    }
    /*
     * Bare nodes rather than buttons. They look like buttons and are the
     * size of one, but a button takes focus, and three focus stops for one
     * number is two too many: the field is the focus stop and the up and
     * down keys step from there. The number field draws both itself.
     */
    schultz_number_step_style(tree, data->up);
    schultz_number_step_style(tree, data->down);
    /* A field wide enough for a number and no wider. A host that wants more
     * sets a preferred width on the number field itself. */
    schultz_node_set_pref_size(tree, data->field, 56.0f,
                               SCHULTZ_SIZE_UNSET);

    schultz_node_set_pane(tree, node, &schultz_number_pane);
    schultz_node_set_widget(tree, node, &schultz_number_widget, data);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_TEXT_INPUT);

    data->value = schultz_number_clamp(data, value);
    schultz_number_show(tree, data);

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_number_field_set_value(schultz_tree *tree,
                                       schultz_handle node, double value)
{
    schultz_number_data *number = schultz_number_of(tree, node);

    if (number == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    number->value = schultz_number_clamp(number, value);
    schultz_number_show(tree, number);
    return schultz_node_invalidate(tree, node);
}

double schultz_number_field_value(const schultz_tree *tree,
                                  schultz_handle node)
{
    const schultz_number_data *number;

    if (schultz_node_widget(tree, node) != &schultz_number_widget) {
        return 0.0;
    }
    number = (const schultz_number_data *)schultz_node_widget_data(tree,
                                                                   node);
    return (number == NULL) ? 0.0 : number->value;
}

int32_t schultz_number_field_set_range(schultz_tree *tree,
                                       schultz_handle node, double minimum,
                                       double maximum)
{
    schultz_number_data *number = schultz_number_of(tree, node);

    if (number == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (minimum > maximum) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    number->minimum = minimum;
    number->maximum = maximum;
    number->value   = schultz_number_clamp(number, number->value);
    schultz_number_show(tree, number);
    return SCHULTZ_OK;
}

int32_t schultz_number_field_set_step(schultz_tree *tree, schultz_handle node,
                                      double step)
{
    schultz_number_data *number = schultz_number_of(tree, node);

    if (number == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (step <= 0.0) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    number->step = step;
    return SCHULTZ_OK;
}

int32_t schultz_number_field_set_decimals(schultz_tree *tree,
                                          schultz_handle node, uint32_t places)
{
    schultz_number_data *number = schultz_number_of(tree, node);

    if (number == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (places > 9u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    number->decimals = places;
    schultz_number_show(tree, number);
    return SCHULTZ_OK;
}

int32_t schultz_number_field_set_steps(schultz_tree *tree, schultz_handle node,
                                       uint32_t where)
{
    schultz_number_data *number = schultz_number_of(tree, node);

    if (number == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (where > SCHULTZ_STEPS_ABOVE_BELOW) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (number->steps != where) {
        number->steps = where;
        schultz_node_invalidate_layout(tree, node);
    }
    return SCHULTZ_OK;
}

int32_t schultz_number_field_set_wrap(schultz_tree *tree, schultz_handle node,
                                      int32_t on)
{
    schultz_number_data *number = schultz_number_of(tree, node);

    if (number == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    number->wrap = on ? 1u : 0u;
    return SCHULTZ_OK;
}

schultz_handle schultz_number_field_text_field(const schultz_tree *tree,
                                               schultz_handle node)
{
    const schultz_number_data *number;

    if (schultz_node_widget(tree, node) != &schultz_number_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    number = (const schultz_number_data *)schultz_node_widget_data(tree,
                                                                   node);
    return (number == NULL) ? SCHULTZ_HANDLE_NONE : number->field;
}

/* ------------------------------------------------------------- Calendar */

/*
 * A date is three integers and never a time_t. No epoch, no timezone, no
 * daylight saving, and no 2038. The calendar is proleptic Gregorian, which is
 * what every toolkit shows and what every one of these rules assumes.
 */

/** Nonzero for a leap year, by the Gregorian rule. */
static int32_t schultz_year_is_leap(int32_t year)
{
    if ((year % 4) != 0) {
        return 0;
    }
    if ((year % 100) != 0) {
        return 1;
    }
    return ((year % 400) == 0) ? 1 : 0;
}

/** How many days a month has. Months count from one. */
static int32_t schultz_month_days(int32_t year, int32_t month)
{
    static const int32_t days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && schultz_year_is_leap(year)) {
        return 29;
    }
    return days[month - 1];
}

/*
 * Which day of the week a date falls on, zero for Sunday. Sakamoto's method:
 * a table of month offsets plus the leap year correction, which is short
 * enough to check by hand against a real calendar.
 */
static int32_t schultz_day_of_week(int32_t year, int32_t month, int32_t day)
{
    static const int32_t offset[12] = {
        0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4
    };
    int32_t y = year;

    if (month < 3) {
        y -= 1;
    }
    return (int32_t)(((y + y / 4 - y / 100 + y / 400 + offset[month - 1] + day)
                      % 7) + 7) % 7;
}

/** Negative, zero or positive as the first date is before, on, or after. */
static int32_t schultz_date_compare(int32_t ay, int32_t am, int32_t ad,
                                    int32_t by, int32_t bm, int32_t bd)
{
    if (ay != by) { return (ay < by) ? -1 : 1; }
    if (am != bm) { return (am < bm) ? -1 : 1; }
    if (ad != bd) { return (ad < bd) ? -1 : 1; }
    return 0;
}

/* ------------------------------------------- Fields of a known length */

/*
 * The widest digit a font draws.
 *
 * Not every face gives the ten digits one width. A field sized on whichever
 * digits happen to be showing would change width as the value changed,
 * shoving whatever sits beside it along the row, so the worst case is
 * measured instead of the current one.
 */
static char schultz_widest_digit(const schultz_font_system *fonts,
                                 schultz_handle font, schultz_arena *scratch)
{
    char best = '0';
    float widest = -1.0f;
    char one[2];
    uint32_t i;

    one[1] = '\0';
    for (i = 0; i < 10u; i++) {
        const schultz_text_line *lines;
        uint32_t count = 0;

        one[0] = (char)('0' + (int32_t)i);
        if (schultz_text_wrap(fonts, font, one, 1, -1.0f, scratch, &lines,
                             &count) == SCHULTZ_OK && count > 0u &&
            lines[0].width > widest) {
            widest = lines[0].width;
            best   = one[0];
        }
    }
    return best;
}

/*
 * Sizes a field to the longest string it can ever be asked to hold.
 *
 * A one line field measures to the same width whatever is in it, so a field
 * showing something of a known shape has to be told how wide to be. Each
 * pattern is that shape with a zero wherever a digit goes, and every zero is
 * measured as the font's widest digit. Several patterns are measured where
 * the widget can write more than one thing, such as a clock that says either
 * am or pm, and the widest wins.
 *
 * The answer is a preferred width, so a style still has the last word: it is
 * what the field asks for when nobody else has said.
 *
 * `fitted` remembers the width last set. The width follows from the font,
 * the text size and the padding, so it settles after the first measure and
 * is only written again when one of those changes. Writing it every measure
 * would mark the field dirty every measure, and a tree that is always dirty
 * repaints every frame forever.
 */
static void schultz_field_fit(schultz_tree *tree, schultz_handle field,
                              const char *const *patterns, uint32_t count,
                              float *fitted)
{
    const schultz_font_system *fonts =
        (const schultz_font_system *)schultz_tree_font_system(tree);
    schultz_handle font = schultz_text_font(tree, field);
    const schultz_resolved_style *style = schultz_widget_style(tree, field);
    schultz_arena scratch;
    char digit;
    float width = 0.0f;
    uint32_t p;

    if (fonts == NULL || font == SCHULTZ_HANDLE_NONE || patterns == NULL) {
        return;
    }
    if (schultz_arena_init(&scratch, 0) != SCHULTZ_OK) {
        return;
    }
    digit = schultz_widest_digit(fonts, font, &scratch);

    for (p = 0; p < count; p++) {
        char worst[32];
        const schultz_text_line *lines;
        uint32_t lines_count = 0;
        uint32_t i;

        if (patterns[p] == NULL) {
            continue;
        }
        for (i = 0; i + 1u < sizeof(worst) && patterns[p][i] != '\0'; i++) {
            worst[i] = (patterns[p][i] == '0') ? digit : patterns[p][i];
        }
        worst[i] = '\0';
        if (schultz_text_wrap(fonts, font, worst, (int32_t)i, -1.0f, &scratch,
                              &lines, &lines_count) == SCHULTZ_OK &&
            lines_count > 0u && lines[0].width > width) {
            width = lines[0].width;
        }
    }
    schultz_arena_free(&scratch);

    if (width <= 0.0f) {
        return;
    }
    /*
     * The text sits inside the padding and the outline, so both are part of
     * the width the string needs.
     */
    width += (schultz_resolved_number(style, SCHULTZ_PROP_PADDING) +
              schultz_resolved_number(style, SCHULTZ_PROP_BORDER_WIDTH))
             * 2.0f;
    if (width != *fitted) {
        *fitted = width;
        schultz_node_set_pref_size(tree, field, width, SCHULTZ_SIZE_UNSET);
    }
}

/* ----------------------------------------------------------- DatePicker */

/** How many day buttons a month grid holds: six weeks of seven. */
#define SCHULTZ_CALENDAR_CELLS 42u

/** A date picker: a field, a popover, and the month it is showing. */
typedef struct {
    schultz_handle field;    /**< The text field showing the date. */
    schultz_handle open;     /**< The button that drops the calendar. */
    schultz_handle popover;  /**< The calendar. */
    schultz_handle title;    /**< The month and year across the top. */
    schultz_handle previous; /**< Steps back a month. */
    schultz_handle next;     /**< Steps forward a month. */
    schultz_handle heads[7]; /**< The day name row. */
    schultz_handle owner;    /**< The picker itself, for the relay. */
    schultz_handle cells[SCHULTZ_CALENDAR_CELLS]; /**< The day buttons. */

    int32_t year;        /**< The chosen year. */
    int32_t month;       /**< The chosen month, counting from one. */
    int32_t day;         /**< The chosen day, counting from one. */
    /* The month the calendar is showing, which is not always the month the
     * chosen date is in: stepping through months does not choose one. */
    int32_t shown_year;  /**< The year on show. */
    int32_t shown_month; /**< The month on show. */

    int32_t has_min;     /**< Nonzero when an earliest date was set. */
    int32_t min_year;    /**< That date's year. */
    int32_t min_month;   /**< Its month. */
    int32_t min_day;     /**< Its day. */
    int32_t has_max;     /**< Nonzero when a latest date was set. */
    int32_t max_year;    /**< That date's year. */
    int32_t max_month;   /**< Its month. */
    int32_t max_day;     /**< Its day. */

    uint32_t first_day; /**< Which weekday a row starts on. */
    uint32_t format;    /**< One of SCHULTZ_DATE_*. */
    float    fitted;    /**< Field width last worked out from the font. */
    char    *months[12];/**< Month names, owned. */
    char    *days[7];   /**< Day names, owned. */
} schultz_date_data;

static const schultz_widget_vtable schultz_date_widget;

static const char *const schultz_default_months[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char *const schultz_default_days[7] = {
    "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"
};

static schultz_date_data *schultz_date_of(schultz_tree *tree,
                                          schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_date_widget) {
        return NULL;
    }
    return (schultz_date_data *)schultz_node_widget_data(tree, node);
}

/* Replaces one owned string. */
static int32_t schultz_date_keep(char **slot, const char *text)
{
    size_t length = (text == NULL) ? 0u : strlen(text);
    char *copy = (char *)malloc(length + 1u);

    if (copy == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    if (length > 0u) {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';
    free(*slot);
    *slot = copy;
    return SCHULTZ_OK;
}

/* Nonzero when a date sits inside whatever range was set. */
static int32_t schultz_date_allowed(const schultz_date_data *date,
                                    int32_t year, int32_t month, int32_t day)
{
    if (date->has_min &&
        schultz_date_compare(year, month, day, date->min_year,
                             date->min_month, date->min_day) < 0) {
        return 0;
    }
    if (date->has_max &&
        schultz_date_compare(year, month, day, date->max_year,
                             date->max_month, date->max_day) > 0) {
        return 0;
    }
    return 1;
}

/* Writes the date into the field in whichever order was asked for. */
static void schultz_date_show(schultz_tree *tree, schultz_date_data *date)
{
    char text[32];

    switch (date->format) {
    case SCHULTZ_DATE_DMY:
        snprintf(text, sizeof(text), "%02d/%02d/%04d", date->day, date->month,
                 date->year);
        break;
    case SCHULTZ_DATE_MDY:
        snprintf(text, sizeof(text), "%02d/%02d/%04d", date->month, date->day,
                 date->year);
        break;
    default:
        snprintf(text, sizeof(text), "%04d-%02d-%02d", date->year,
                 date->month, date->day);
        break;
    }
    schultz_text_set(tree, date->field, text);
}

/* Reads the field back, putting the date back when it does not parse. */
static void schultz_date_take(schultz_tree *tree, schultz_date_data *date)
{
    const char *text = schultz_text_get(tree, date->field);
    int32_t a = 0;
    int32_t b = 0;
    int32_t c = 0;
    int32_t year;
    int32_t month;
    int32_t day;

    if (text == NULL || sscanf(text, "%d%*[-/]%d%*[-/]%d", &a, &b, &c) != 3) {
        schultz_date_show(tree, date);
        return;
    }
    switch (date->format) {
    case SCHULTZ_DATE_DMY: day = a; month = b; year = c; break;
    case SCHULTZ_DATE_MDY: month = a; day = b; year = c; break;
    default:               year = a; month = b; day = c; break;
    }
    if (month < 1 || month > 12 || day < 1 ||
        day > schultz_month_days(year, month) ||
        !schultz_date_allowed(date, year, month, day)) {
        schultz_date_show(tree, date);
        return;
    }
    date->year  = year;
    date->month = month;
    date->day   = day;
    schultz_date_show(tree, date);
}

/*
 * Fills the month grid. Every cell is written every time, so there is one
 * place where a day number, its month, and whether it can be chosen are
 * decided together.
 */
static void schultz_date_fill(schultz_tree *tree, schultz_date_data *date)
{
    int32_t days = schultz_month_days(date->shown_year, date->shown_month);
    int32_t lead = schultz_day_of_week(date->shown_year, date->shown_month, 1);
    uint32_t i;
    char text[32];

    /* How many blank cells come before the first, once the row is turned to
     * start on whichever weekday was chosen. */
    lead = ((lead - (int32_t)date->first_day) % 7 + 7) % 7;

    snprintf(text, sizeof(text), "%s %d",
             (date->months[date->shown_month - 1] == NULL)
                 ? "" : date->months[date->shown_month - 1],
             date->shown_year);
    schultz_label_set_text(tree, date->title, text);

    for (i = 0; i < 7u; i++) {
        uint32_t which = (date->first_day + i) % 7u;

        schultz_label_set_text(tree, date->heads[i],
            (date->days[which] == NULL) ? "" : date->days[which]);
    }

    for (i = 0; i < SCHULTZ_CALENDAR_CELLS; i++) {
        int32_t day = (int32_t)i - lead + 1;
        uint32_t state = schultz_node_get_state(tree, date->cells[i]);

        if (day < 1 || day > days) {
            /* Outside the month: still there, so the grid keeps its shape,
             * but blank and not choosable. */
            schultz_label_set_text(tree,
                schultz_button_label(tree, date->cells[i]), "");
            state &= ~(uint32_t)SCHULTZ_STATE_ENABLED;
            state &= ~(uint32_t)SCHULTZ_STATE_SELECTED;
        } else {
            snprintf(text, sizeof(text), "%d", day);
            schultz_label_set_text(tree,
                schultz_button_label(tree, date->cells[i]), text);
            if (schultz_date_allowed(date, date->shown_year,
                                     date->shown_month, day)) {
                state |= SCHULTZ_STATE_ENABLED;
            } else {
                state &= ~(uint32_t)SCHULTZ_STATE_ENABLED;
            }
            if (day == date->day && date->shown_month == date->month &&
                date->shown_year == date->year) {
                state |= SCHULTZ_STATE_SELECTED;
            } else {
                state &= ~(uint32_t)SCHULTZ_STATE_SELECTED;
            }
        }
        schultz_node_set_state(tree, date->cells[i], state);
    }
}

/* Steps the calendar a month at a time, rolling the year over. */
static void schultz_date_step_month(schultz_tree *tree,
                                    schultz_date_data *date, int32_t by)
{
    date->shown_month += by;
    while (date->shown_month < 1) {
        date->shown_month += 12;
        date->shown_year -= 1;
    }
    while (date->shown_month > 12) {
        date->shown_month -= 12;
        date->shown_year += 1;
    }
    schultz_date_fill(tree, date);
}

static int32_t schultz_date_event(schultz_tree *tree, schultz_handle node,
                                  const schultz_event *event)
{
    schultz_date_data *date = schultz_date_of(tree, node);
    uint32_t i;

    if (date == NULL) {
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_FOCUS_LOST &&
        event->target == date->field) {
        schultz_date_take(tree, date);
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_KEY_DOWN &&
        event->key == (uint32_t)SCHULTZ_KEY_RETURN) {
        schultz_date_take(tree, date);
        return SCHULTZ_EVENT_CONSUMED;
    }
    if (event->type != SCHULTZ_EVENT_CLICK) {
        return SCHULTZ_OK;
    }

    if (event->target == date->open) {
        date->shown_year  = date->year;
        date->shown_month = date->month;
        schultz_date_fill(tree, date);
        schultz_popup_open_at(tree, date->popover, node,
                                SCHULTZ_PLACE_BELOW);
        return SCHULTZ_EVENT_CONSUMED;
    }
    if (event->target == date->previous) {
        schultz_date_step_month(tree, date, -1);
        return SCHULTZ_EVENT_CONSUMED;
    }
    if (event->target == date->next) {
        schultz_date_step_month(tree, date, 1);
        return SCHULTZ_EVENT_CONSUMED;
    }
    for (i = 0; i < SCHULTZ_CALENDAR_CELLS; i++) {
        const char *text;

        if (event->target != date->cells[i]) {
            continue;
        }
        text = schultz_label_text(tree,
                                  schultz_button_label(tree, date->cells[i]));
        if (text == NULL || text[0] == '\0') {
            return SCHULTZ_EVENT_CONSUMED;
        }
        date->year  = date->shown_year;
        date->month = date->shown_month;
        date->day   = atoi(text);
        schultz_date_show(tree, date);
        schultz_date_fill(tree, date);
        schultz_popup_close(tree, date->popover);
        return SCHULTZ_OK;
    }
    return SCHULTZ_OK;
}

static void schultz_date_destroy(void *pointer)
{
    schultz_date_data *date = (schultz_date_data *)pointer;
    uint32_t i;

    if (date == NULL) {
        return;
    }
    for (i = 0; i < 12u; i++) {
        free(date->months[i]);
    }
    for (i = 0; i < 7u; i++) {
        free(date->days[i]);
    }
    free(date);
}

static const schultz_widget_vtable schultz_date_widget = {
    .event = schultz_date_event, .destroy = schultz_date_destroy
};

/*
 * Every format writes ten characters, but not the same ten: the separator
 * differs, and a hyphen and a slash are not one width.
 */
static void schultz_date_fit(schultz_tree *tree, schultz_date_data *date)
{
    static const char *const iso[]   = { "0000-00-00" };
    static const char *const slash[] = { "00/00/0000" };

    schultz_field_fit(tree, date->field,
                      (date->format == SCHULTZ_DATE_ISO) ? iso : slash, 1u,
                      &date->fitted);
}

/*
 * A row, with the field sized to the date before the row is measured.
 *
 * The fitting happens here rather than when the picker is built because it
 * needs the font, and a node has no resolved style until the tree resolves
 * one. It costs nothing to repeat: the width settles on the first measure
 * and is only written again if the theme changes the font underneath it.
 */
static int32_t schultz_date_measure(schultz_tree *tree, schultz_handle node,
                                    float avail_w, float avail_h,
                                    schultz_size *out_size)
{
    schultz_date_data *date = schultz_date_of(tree, node);

    if (date != NULL) {
        schultz_date_fit(tree, date);
    }
    return schultz_pane_hbox()->measure(tree, node, avail_w, avail_h,
                                        out_size);
}

static int32_t schultz_date_arrange(schultz_tree *tree, schultz_handle node,
                                    schultz_rect rect)
{
    return schultz_pane_hbox()->arrange(tree, node, rect);
}

static const schultz_pane_vtable schultz_date_pane = {
    schultz_date_measure, schultz_date_arrange
};

/* Builds the popover: a header, a row of day names, and the month grid. */
static int32_t schultz_date_build_calendar(schultz_tree *tree,
                                           schultz_date_data *date)
{
    schultz_handle content;
    schultz_handle header = SCHULTZ_HANDLE_NONE;
    schultz_handle grid = SCHULTZ_HANDLE_NONE;
    schultz_grid_track columns[7];
    schultz_grid_track rows[7];
    uint32_t i;
    int32_t result = schultz_popup_create(tree, 1, &date->popover);

    if (result != SCHULTZ_OK) {
        return result;
    }
    /* A panel dropped under the field, not a bubble pointing at it. */
    schultz_popup_set_arrow(tree, date->popover, 0);
    content = schultz_popup_content(tree, date->popover);
    schultz_node_set_pane(tree, content, schultz_pane_vbox());
    schultz_node_set_spacing(tree, content, 0.0f, 6.0f);
    /* The calendar lives on an overlay, so its clicks are carried back. */
    result = schultz_relay_install(tree, content, date->owner);
    if (result != SCHULTZ_OK) {
        return result;
    }

    result = schultz_node_create(tree, content, &header);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_set_pane(tree, header, schultz_pane_hbox());
    schultz_node_set_spacing(tree, header, 0.0f, 6.0f);
    result = schultz_button_create(tree, header, "<", &date->previous);
    if (result == SCHULTZ_OK) {
        result = schultz_label_create(tree, header, "", &date->title);
    }
    if (result == SCHULTZ_OK) {
        result = schultz_button_create(tree, header, ">", &date->next);
    }
    if (result != SCHULTZ_OK) {
        return result;
    }
    {
        schultz_layout_params params;

        schultz_layout_params_default(&params);
        params.grow  = SCHULTZ_GROW_ALWAYS;
        params.align = SCHULTZ_ALIGN_CENTER;
        schultz_node_set_layout_params(tree, date->title, &params);
    }

    result = schultz_node_create(tree, content, &grid);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_set_pane(tree, grid, schultz_pane_grid());
    schultz_node_set_spacing(tree, grid, 0.0f, 0.0f);
    for (i = 0; i < 7u; i++) {
        columns[i].kind  = SCHULTZ_TRACK_FIXED;
        columns[i].value = 30.0f;
    }
    /*
     * Seven rows as well as seven columns: the day names and six weeks. A
     * grid told nothing about its rows derives a single one, which puts the
     * whole month on one line.
     */
    for (i = 0; i < 7u; i++) {
        rows[i].kind  = SCHULTZ_TRACK_CONTENT;
        rows[i].value = 0.0f;
    }
    schultz_node_set_grid_tracks(tree, grid, columns, 7u, rows, 7u);

    for (i = 0; i < 7u; i++) {
        schultz_layout_params params;

        result = schultz_label_create(tree, grid, "", &date->heads[i]);
        if (result != SCHULTZ_OK) {
            return result;
        }
        schultz_layout_params_default(&params);
        params.row         = 0u;
        params.column      = i;
        params.row_span    = 1u;
        params.column_span = 1u;
        params.align       = SCHULTZ_ALIGN_CENTER;
        schultz_node_set_layout_params(tree, date->heads[i], &params);
        schultz_node_set_style_property(tree, date->heads[i],
            SCHULTZ_PROP_TEXT_COLOR,
            schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_MUTED));
    }
    for (i = 0; i < SCHULTZ_CALENDAR_CELLS; i++) {
        schultz_layout_params params;

        result = schultz_button_create(tree, grid, "", &date->cells[i]);
        if (result != SCHULTZ_OK) {
            return result;
        }
        schultz_layout_params_default(&params);
        params.row         = 1u + i / 7u;
        params.column      = i % 7u;
        params.row_span    = 1u;
        params.column_span = 1u;
        schultz_node_set_layout_params(tree, date->cells[i], &params);
        schultz_node_set_style_property(tree, date->cells[i],
            SCHULTZ_PROP_PADDING, schultz_value_number(2.0f));
        /* Square, and touching its neighbours, so the month reads as one
         * grid rather than as forty two separate buttons. */
        schultz_node_set_style_property(tree, date->cells[i],
            SCHULTZ_PROP_CORNER_RADIUS, schultz_value_number(0.0f));
        schultz_node_set_state_property(tree, date->cells[i],
            SCHULTZ_STYLE_STATE_SELECTED, SCHULTZ_PROP_BACKGROUND,
            schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));
        schultz_node_set_state_property(tree, date->cells[i],
            SCHULTZ_STYLE_STATE_SELECTED, SCHULTZ_PROP_TEXT_COLOR,
            schultz_value_token(SCHULTZ_TOKEN_COLOR_TEXT_ON_ACCENT));
    }
    return SCHULTZ_OK;
}

int32_t schultz_date_picker_create(schultz_tree *tree, schultz_handle parent,
                                   schultz_handle *out_node)
{
    schultz_date_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result;
    uint32_t i;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_date_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->year   = 2026;
    data->month  = 1;
    data->day    = 1;
    data->format = SCHULTZ_DATE_ISO;
    for (i = 0; i < 12u; i++) {
        if (schultz_date_keep(&data->months[i], schultz_default_months[i])
                != SCHULTZ_OK) {
            schultz_date_destroy(data);
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
    }
    for (i = 0; i < 7u; i++) {
        if (schultz_date_keep(&data->days[i], schultz_default_days[i])
                != SCHULTZ_OK) {
            schultz_date_destroy(data);
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
    }

    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        schultz_date_destroy(data);
        return result;
    }
    data->owner = node;
    result = schultz_text_field_create(tree, node, "", &data->field);
    if (result == SCHULTZ_OK) {
        result = schultz_button_create(tree, node, "...", &data->open);
    }
    if (result == SCHULTZ_OK) {
        result = schultz_date_build_calendar(tree, data);
    }
    if (result != SCHULTZ_OK) {
        schultz_date_destroy(data);
        schultz_node_destroy(tree, node);
        return result;
    }

    schultz_node_set_pane(tree, node, &schultz_date_pane);
    schultz_node_set_spacing(tree, node, 0.0f, 4.0f);
    schultz_node_set_widget(tree, node, &schultz_date_widget, data);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_TEXT_INPUT);

    data->shown_year  = data->year;
    data->shown_month = data->month;
    schultz_date_show(tree, data);
    schultz_date_fill(tree, data);

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_date_picker_set_date(schultz_tree *tree, schultz_handle node,
                                     int32_t year, int32_t month, int32_t day)
{
    schultz_date_data *date = schultz_date_of(tree, node);

    if (date == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (month < 1 || month > 12 || day < 1 ||
        day > schultz_month_days(year, month)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    date->year        = year;
    date->month       = month;
    date->day         = day;
    date->shown_year  = year;
    date->shown_month = month;
    schultz_date_show(tree, date);
    schultz_date_fill(tree, date);
    return SCHULTZ_OK;
}

int32_t schultz_date_picker_date(const schultz_tree *tree,
                                 schultz_handle node, int32_t *out_year,
                                 int32_t *out_month, int32_t *out_day)
{
    const schultz_date_data *date;

    if (schultz_node_widget(tree, node) != &schultz_date_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    date = (const schultz_date_data *)schultz_node_widget_data(tree, node);
    if (date == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (out_year != NULL)  { *out_year = date->year; }
    if (out_month != NULL) { *out_month = date->month; }
    if (out_day != NULL)   { *out_day = date->day; }
    return SCHULTZ_OK;
}

int32_t schultz_date_picker_set_range(schultz_tree *tree, schultz_handle node,
                                      int32_t min_year, int32_t min_month,
                                      int32_t min_day, int32_t max_year,
                                      int32_t max_month, int32_t max_day)
{
    schultz_date_data *date = schultz_date_of(tree, node);

    if (date == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (schultz_date_compare(min_year, min_month, min_day, max_year,
                             max_month, max_day) > 0) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    date->has_min   = 1;
    date->min_year  = min_year;
    date->min_month = min_month;
    date->min_day   = min_day;
    date->has_max   = 1;
    date->max_year  = max_year;
    date->max_month = max_month;
    date->max_day   = max_day;
    schultz_date_fill(tree, date);
    return SCHULTZ_OK;
}

int32_t schultz_date_picker_set_month_names(schultz_tree *tree,
                                            schultz_handle node,
                                            const char *const *names,
                                            uint32_t count)
{
    schultz_date_data *date = schultz_date_of(tree, node);
    uint32_t i;

    if (date == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (names == NULL || count != 12u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < 12u; i++) {
        int32_t result = schultz_date_keep(&date->months[i], names[i]);

        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    schultz_date_fill(tree, date);
    return SCHULTZ_OK;
}

int32_t schultz_date_picker_set_day_names(schultz_tree *tree,
                                          schultz_handle node,
                                          const char *const *names,
                                          uint32_t count)
{
    schultz_date_data *date = schultz_date_of(tree, node);
    uint32_t i;

    if (date == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (names == NULL || count != 7u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /* Sunday first, whatever the first day of the week is set to; turning
     * the row is the widget's job, not the caller's. */
    for (i = 0; i < 7u; i++) {
        int32_t result = schultz_date_keep(&date->days[i], names[i]);

        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    schultz_date_fill(tree, date);
    return SCHULTZ_OK;
}

int32_t schultz_date_picker_set_first_day(schultz_tree *tree,
                                          schultz_handle node,
                                          uint32_t weekday)
{
    schultz_date_data *date = schultz_date_of(tree, node);

    if (date == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (weekday > 6u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    date->first_day = weekday;
    schultz_date_fill(tree, date);
    return SCHULTZ_OK;
}

int32_t schultz_date_picker_set_format(schultz_tree *tree,
                                       schultz_handle node, uint32_t format)
{
    schultz_date_data *date = schultz_date_of(tree, node);

    if (date == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (format > SCHULTZ_DATE_MDY) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (date->format == format) {
        return SCHULTZ_OK;
    }
    date->format = format;
    schultz_date_show(tree, date);
    /* A different separator is a different width, so measure it again. */
    schultz_node_invalidate_layout(tree, node);
    return SCHULTZ_OK;
}

/* ----------------------------------------------------------- TimePicker */

/** A time picker: hour, minute and optional second, in a popover. */
typedef struct {
    schultz_handle field;   /**< The text field showing the time. */
    schultz_handle open;    /**< The button that drops the fields. */
    schultz_handle popover; /**< Holds the three number fields. */
    schultz_handle hour;    /**< Hours, which wrap. */
    schultz_handle minute;  /**< Minutes, which wrap. */
    schultz_handle second;  /**< Seconds, hidden unless asked for. */

    int32_t  hours;    /**< Always 0 to 23, whatever is displayed. */
    int32_t  minutes;  /**< 0 to 59. */
    int32_t  seconds;  /**< 0 to 59. */
    uint32_t clock24;  /**< Nonzero to show 24 hour time. */
    uint32_t show_sec; /**< Nonzero to show seconds. */
    float    fitted;   /**< Field width last worked out from the font. */
} schultz_time_data;

static const schultz_widget_vtable schultz_time_widget;

static schultz_time_data *schultz_time_of(schultz_tree *tree,
                                          schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_time_widget) {
        return NULL;
    }
    return (schultz_time_data *)schultz_node_widget_data(tree, node);
}

/* Writes the time into the field. The value is always kept as 24 hour. */
static void schultz_time_show(schultz_tree *tree, schultz_time_data *time)
{
    char text[32];
    int32_t shown = time->hours;
    const char *suffix = "";

    if (!time->clock24) {
        suffix = (time->hours < 12) ? " am" : " pm";
        shown = time->hours % 12;
        if (shown == 0) {
            shown = 12;  /* midnight and noon are twelve, not zero */
        }
    }
    if (time->show_sec) {
        snprintf(text, sizeof(text), "%02d:%02d:%02d%s", shown,
                 time->minutes, time->seconds, suffix);
    } else {
        snprintf(text, sizeof(text), "%02d:%02d%s", shown, time->minutes,
                 suffix);
    }
    schultz_text_set(tree, time->field, text);
}

/* Copies the value into the three number fields. */
static void schultz_time_push(schultz_tree *tree, schultz_time_data *time)
{
    schultz_number_field_set_value(tree, time->hour, (double)time->hours);
    schultz_number_field_set_value(tree, time->minute, (double)time->minutes);
    schultz_number_field_set_value(tree, time->second, (double)time->seconds);
}

/* And reads it back out of them. */
static void schultz_time_pull(schultz_tree *tree, schultz_time_data *time)
{
    time->hours   = (int32_t)schultz_number_field_value(tree, time->hour);
    time->minutes = (int32_t)schultz_number_field_value(tree, time->minute);
    time->seconds = (int32_t)schultz_number_field_value(tree, time->second);
    schultz_time_show(tree, time);
}

/* Reads what was typed, putting the time back when it does not parse. */
static void schultz_time_take(schultz_tree *tree, schultz_time_data *time)
{
    const char *text = schultz_text_get(tree, time->field);
    int32_t hours = 0;
    int32_t minutes = 0;
    int32_t seconds = 0;
    int32_t read;

    if (text == NULL) {
        schultz_time_show(tree, time);
        return;
    }
    read = sscanf(text, "%d:%d:%d", &hours, &minutes, &seconds);
    if (read < 2) {
        schultz_time_show(tree, time);
        return;
    }
    if (!time->clock24) {
        /* Twelve hour text carries its half of the day in a suffix. */
        if (strstr(text, "pm") != NULL && hours < 12) {
            hours += 12;
        } else if (strstr(text, "am") != NULL && hours == 12) {
            hours = 0;
        }
    }
    if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59 ||
        seconds < 0 || seconds > 59) {
        schultz_time_show(tree, time);
        return;
    }
    time->hours   = hours;
    time->minutes = minutes;
    time->seconds = (read == 3) ? seconds : time->seconds;
    schultz_time_show(tree, time);
    schultz_time_push(tree, time);
}

static int32_t schultz_time_event(schultz_tree *tree, schultz_handle node,
                                  const schultz_event *event)
{
    schultz_time_data *time = schultz_time_of(tree, node);

    if (time == NULL) {
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_FOCUS_LOST &&
        event->target == time->field) {
        schultz_time_take(tree, time);
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_KEY_DOWN &&
        event->key == (uint32_t)SCHULTZ_KEY_RETURN) {
        schultz_time_take(tree, time);
        return SCHULTZ_EVENT_CONSUMED;
    }
    if (event->type == SCHULTZ_EVENT_CLICK) {
        if (event->target == time->open) {
            schultz_time_push(tree, time);
            schultz_popup_open_at(tree, time->popover, node,
                                 SCHULTZ_PLACE_BELOW);
            return SCHULTZ_EVENT_CONSUMED;
        }
        /* Any click inside the popover may have stepped a number, so the
         * field is written again rather than guessing which one it was. */
        if (schultz_popup_is_open(tree, time->popover)) {
            schultz_time_pull(tree, time);
        }
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_time_widget = {
    .event = schultz_time_event, .destroy = free
};

/*
 * The four strings schultz_time_show can write, and the field is sized for
 * whichever of them the flags allow. A twelve hour clock says either am or
 * pm and has no say in which, so both are measured.
 */
static void schultz_time_fit(schultz_tree *tree, schultz_time_data *time)
{
    static const char *const clock24_sec[] = { "00:00:00" };
    static const char *const clock24_min[] = { "00:00" };
    static const char *const clock12_sec[] = { "00:00:00 am", "00:00:00 pm" };
    static const char *const clock12_min[] = { "00:00 am", "00:00 pm" };

    if (time->clock24) {
        schultz_field_fit(tree, time->field,
                          time->show_sec ? clock24_sec : clock24_min, 1u,
                          &time->fitted);
    } else {
        schultz_field_fit(tree, time->field,
                          time->show_sec ? clock12_sec : clock12_min, 2u,
                          &time->fitted);
    }
}

/*
 * A row, with the field sized to the time before the row is measured. The
 * same reasoning as the date picker's: the width needs the font, and there
 * is no font until the tree has resolved a style.
 */
static int32_t schultz_time_measure(schultz_tree *tree, schultz_handle node,
                                    float avail_w, float avail_h,
                                    schultz_size *out_size)
{
    schultz_time_data *time = schultz_time_of(tree, node);

    if (time != NULL) {
        schultz_time_fit(tree, time);
    }
    return schultz_pane_hbox()->measure(tree, node, avail_w, avail_h,
                                        out_size);
}

static int32_t schultz_time_arrange(schultz_tree *tree, schultz_handle node,
                                    schultz_rect rect)
{
    return schultz_pane_hbox()->arrange(tree, node, rect);
}

static const schultz_pane_vtable schultz_time_pane = {
    schultz_time_measure, schultz_time_arrange
};

int32_t schultz_time_picker_create(schultz_tree *tree, schultz_handle parent,
                                   schultz_handle *out_node)
{
    schultz_time_data *data;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle content;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_time_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->clock24 = 1u;

    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    result = schultz_text_field_create(tree, node, "", &data->field);
    if (result == SCHULTZ_OK) {
        result = schultz_button_create(tree, node, "...", &data->open);
    }
    if (result == SCHULTZ_OK) {
        result = schultz_popup_create(tree, 1, &data->popover);
    }
    if (result != SCHULTZ_OK) {
        free(data);
        schultz_node_destroy(tree, node);
        return result;
    }

    /* A panel dropped under the field, not a bubble pointing at it. */
    schultz_popup_set_arrow(tree, data->popover, 0);
    content = schultz_popup_content(tree, data->popover);
    schultz_node_set_pane(tree, content, schultz_pane_hbox());
    schultz_node_set_spacing(tree, content, 0.0f, 6.0f);
    result = schultz_relay_install(tree, content, node);
    if (result != SCHULTZ_OK) {
        free(data);
        schultz_node_destroy(tree, node);
        return result;
    }
    result = schultz_number_field_create(tree, content, 0.0, 23.0, 0.0,
                                         &data->hour);
    if (result == SCHULTZ_OK) {
        result = schultz_number_field_create(tree, content, 0.0, 59.0, 0.0,
                                             &data->minute);
    }
    if (result == SCHULTZ_OK) {
        result = schultz_number_field_create(tree, content, 0.0, 59.0, 0.0,
                                             &data->second);
    }
    if (result != SCHULTZ_OK) {
        free(data);
        schultz_node_destroy(tree, node);
        return result;
    }
    /* Hours and minutes are circular, so they run off one end onto the
     * other rather than stopping. */
    schultz_number_field_set_wrap(tree, data->hour, 1);
    schultz_number_field_set_wrap(tree, data->minute, 1);
    schultz_number_field_set_wrap(tree, data->second, 1);
    /*
     * Three of these stand side by side in a popup no wider than the field
     * it drops from, so their steps go above and below rather than beside:
     * beside, each one is twice its own width and the digits are squeezed
     * out. Two digits is all any of them ever needs.
     */
    {
        schultz_handle each[3];
        uint32_t i;

        each[0] = data->hour;
        each[1] = data->minute;
        each[2] = data->second;
        for (i = 0; i < 3u; i++) {
            schultz_number_field_set_steps(tree, each[i],
                                           SCHULTZ_STEPS_ABOVE_BELOW);
            schultz_node_set_pref_size(tree, each[i], 58.0f,
                                       SCHULTZ_SIZE_UNSET);
        }
    }
    schultz_node_set_state(tree, data->second,
        schultz_node_get_state(tree, data->second)
            & ~(uint32_t)SCHULTZ_STATE_VISIBLE);

    schultz_node_set_pane(tree, node, &schultz_time_pane);
    schultz_node_set_spacing(tree, node, 0.0f, 4.0f);
    schultz_node_set_widget(tree, node, &schultz_time_widget, data);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_TEXT_INPUT);
    schultz_time_show(tree, data);

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_time_picker_set_time(schultz_tree *tree, schultz_handle node,
                                     int32_t hour, int32_t minute,
                                     int32_t second)
{
    schultz_time_data *time = schultz_time_of(tree, node);

    if (time == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    time->hours   = hour;
    time->minutes = minute;
    time->seconds = second;
    schultz_time_show(tree, time);
    schultz_time_push(tree, time);
    return SCHULTZ_OK;
}

int32_t schultz_time_picker_time(const schultz_tree *tree,
                                 schultz_handle node, int32_t *out_hour,
                                 int32_t *out_minute, int32_t *out_second)
{
    const schultz_time_data *time;

    if (schultz_node_widget(tree, node) != &schultz_time_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    time = (const schultz_time_data *)schultz_node_widget_data(tree, node);
    if (time == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (out_hour != NULL)   { *out_hour = time->hours; }
    if (out_minute != NULL) { *out_minute = time->minutes; }
    if (out_second != NULL) { *out_second = time->seconds; }
    return SCHULTZ_OK;
}

int32_t schultz_time_picker_set_24_hour(schultz_tree *tree,
                                        schultz_handle node, int32_t on)
{
    schultz_time_data *time = schultz_time_of(tree, node);

    if (time == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (time->clock24 == (on ? 1u : 0u)) {
        return SCHULTZ_OK;
    }
    time->clock24 = on ? 1u : 0u;
    schultz_time_show(tree, time);
    /* A different clock is a different longest string, so the field has to
     * be measured again rather than keep the width the old one needed. */
    schultz_node_invalidate_layout(tree, node);
    return SCHULTZ_OK;
}

int32_t schultz_time_picker_set_show_seconds(schultz_tree *tree,
                                             schultz_handle node, int32_t on)
{
    schultz_time_data *time = schultz_time_of(tree, node);

    if (time == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    /* The seconds field is hidden when the picker is made, so this and the
     * flag agree from the start and comparing them is safe. */
    if (time->show_sec == (on ? 1u : 0u)) {
        return SCHULTZ_OK;
    }
    time->show_sec = on ? 1u : 0u;
    schultz_node_set_state(tree, time->second,
        on ? (schultz_node_get_state(tree, time->second)
                  | SCHULTZ_STATE_VISIBLE)
           : (schultz_node_get_state(tree, time->second)
                  & ~(uint32_t)SCHULTZ_STATE_VISIBLE));
    schultz_time_show(tree, time);
    schultz_node_invalidate_layout(tree, node);
    return SCHULTZ_OK;
}

/* ---------------------------------------------------------- ColorPicker */

/*
 * Hue, saturation and value, because that is the space a person picks a
 * colour in: one axis for which colour, two for how strong and how bright.
 * The value the host reads is always RGB.
 */

/** Turns hue, saturation and value into red, green and blue. */
static schultz_color schultz_hsv_to_color(float hue, float saturation,
                                          float value, uint8_t alpha)
{
    float c = value * saturation;
    float h = hue / 60.0f;
    float x = c * (1.0f - (float)fabs(fmod((double)h, 2.0) - 1.0));
    float m = value - c;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    if (h < 1.0f)      { r = c; g = x; }
    else if (h < 2.0f) { r = x; g = c; }
    else if (h < 3.0f) { g = c; b = x; }
    else if (h < 4.0f) { g = x; b = c; }
    else if (h < 5.0f) { r = x; b = c; }
    else               { r = c; b = x; }

    return schultz_color_rgba((uint8_t)((r + m) * 255.0f + 0.5f),
                              (uint8_t)((g + m) * 255.0f + 0.5f),
                              (uint8_t)((b + m) * 255.0f + 0.5f), alpha);
}

/** And back again. */
static void schultz_color_to_hsv(schultz_color color, float *out_hue,
                                 float *out_saturation, float *out_value)
{
    float r = (float)color.r / 255.0f;
    float g = (float)color.g / 255.0f;
    float b = (float)color.b / 255.0f;
    float high = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
    float low  = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
    float span = high - low;
    float hue = 0.0f;

    if (span > 0.0f) {
        if (high == r) {
            hue = 60.0f * (float)fmod((double)((g - b) / span), 6.0);
        } else if (high == g) {
            hue = 60.0f * ((b - r) / span + 2.0f);
        } else {
            hue = 60.0f * ((r - g) / span + 4.0f);
        }
    }
    if (hue < 0.0f) {
        hue += 360.0f;
    }
    *out_hue        = hue;
    *out_saturation = (high <= 0.0f) ? 0.0f : span / high;
    *out_value      = high;
}

/** Where the square is, so it can draw itself and mark the choice. */
typedef struct {
    float h; /**< The hue the square is showing. */
    float s; /**< Where the marker sits across it. */
    float v; /**< And down it. */
} schultz_square_data;

/** @brief How many strips the square is drawn with, across. */
#define SCHULTZ_SQUARE_COLUMNS 32u
/** @brief And down. */
#define SCHULTZ_SQUARE_ROWS 24u

/*
 * The square is drawn rather than filled with a gradient, because a gradient
 * is a resource a widget cannot register and this needs a new one every time
 * the hue moves.
 *
 * It is strips rather than a grid of cells, which is what keeps it cheap.
 * A colour at saturation s and value v is the same colour at full value with
 * black laid over it at one minus v, so the two axes are two passes of strips
 * instead of one pass over every cell: fifty six rectangles rather than seven
 * hundred and sixty eight.
 */
static int32_t schultz_square_paint(schultz_tree *tree, schultz_handle node,
                                    schultz_draw_list *list,
                                    schultz_arena *arena)
{
    const schultz_square_data *square =
        (const schultz_square_data *)schultz_node_widget_data(tree, node);
    schultz_rect bounds;
    schultz_rect strip;
    uint32_t i;
    int32_t result;

    (void)arena;
    if (square == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        bounds.width <= 0.0f || bounds.height <= 0.0f) {
        return SCHULTZ_OK;
    }

    for (i = 0; i < SCHULTZ_SQUARE_COLUMNS; i++) {
        float at = (float)i / (float)(SCHULTZ_SQUARE_COLUMNS - 1u);

        strip.x      = bounds.x + bounds.width * (float)i
                           / (float)SCHULTZ_SQUARE_COLUMNS;
        strip.y      = bounds.y;
        strip.width  = bounds.width / (float)SCHULTZ_SQUARE_COLUMNS + 1.0f;
        strip.height = bounds.height;
        result = schultz_draw_fill_rect(list, strip,
            schultz_paint_solid(schultz_hsv_to_color(square->h, at, 1.0f,
                                                     255u)));
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    for (i = 0; i < SCHULTZ_SQUARE_ROWS; i++) {
        float dark = (float)i / (float)(SCHULTZ_SQUARE_ROWS - 1u);

        strip.x      = bounds.x;
        strip.y      = bounds.y + bounds.height * (float)i
                           / (float)SCHULTZ_SQUARE_ROWS;
        strip.width  = bounds.width;
        strip.height = bounds.height / (float)SCHULTZ_SQUARE_ROWS + 1.0f;
        result = schultz_draw_fill_rect(list, strip,
            schultz_paint_solid(schultz_color_rgba(0, 0, 0,
                (uint8_t)(dark * 255.0f + 0.5f))));
        if (result != SCHULTZ_OK) {
            return result;
        }
    }

    /* A ring where the choice is, drawn in whichever of black or white
     * stands out against what is under it. */
    {
        schultz_stroke ring;
        float cx = bounds.x + bounds.width * square->s;
        float cy = bounds.y + bounds.height * (1.0f - square->v);

        ring = schultz_stroke_solid((square->v > 0.6f)
            ? schultz_color_rgba(0, 0, 0, 255)
            : schultz_color_rgba(255, 255, 255, 255), 2.0f);
        return schultz_draw_stroke_ellipse(list,
            schultz_rect_make(cx - 5.0f, cy - 5.0f, 10.0f, 10.0f), ring);
    }
}

static const schultz_widget_vtable schultz_square_widget = {
    .paint = schultz_square_paint, .destroy = free
};

/** A colour picker: a swatch, and a popover holding the three controls. */
typedef struct {
    schultz_handle swatch;  /**< The button showing the colour. */
    schultz_handle popover; /**< Holds the square and the sliders. */
    schultz_handle square;  /**< Saturation across, value down. */
    schultz_handle hue;     /**< The hue slider. */
    schultz_handle alpha;   /**< The alpha slider, hidden unless asked for. */
    schultz_handle hex;     /**< The hex entry field. */

    float          h;       /**< Hue, in degrees. */
    float          s;       /**< Saturation, zero to one. */
    float          v;       /**< Value, zero to one. */
    uint8_t        a;       /**< Alpha. */
    uint32_t       use_alpha; /**< Nonzero when alpha may be chosen. */
} schultz_colorpicker_data;

static const schultz_widget_vtable schultz_colorpicker_widget;

static schultz_colorpicker_data *schultz_colorpicker_of(schultz_tree *tree,
                                                        schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_colorpicker_widget) {
        return NULL;
    }
    return (schultz_colorpicker_data *)schultz_node_widget_data(tree, node);
}

/* Rebuilds everything that follows from the colour. */
static void schultz_colorpicker_show(schultz_tree *tree,
                                     schultz_handle node,
                                     schultz_colorpicker_data *pick)
{
    schultz_color colour = schultz_hsv_to_color(pick->h, pick->s, pick->v,
                                                pick->a);
    schultz_square_data *square =
        (schultz_square_data *)schultz_node_widget_data(tree, pick->square);
    char text[16];

    schultz_node_set_style_property(tree, pick->swatch,
        SCHULTZ_PROP_BACKGROUND, schultz_value_color(colour));
    if (square != NULL) {
        square->h = pick->h;
        square->s = pick->s;
        square->v = pick->v;
        schultz_node_invalidate(tree, pick->square);
    }

    snprintf(text, sizeof(text), "#%02X%02X%02X", colour.r, colour.g,
             colour.b);
    schultz_text_set(tree, pick->hex, text);
    schultz_node_invalidate(tree, node);
}

/* Reads a hex string back. Anything unreadable puts the colour back. */
static void schultz_colorpicker_take(schultz_tree *tree, schultz_handle node,
                                     schultz_colorpicker_data *pick)
{
    const char *text = schultz_text_get(tree, pick->hex);
    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;

    if (text == NULL) {
        schultz_colorpicker_show(tree, node, pick);
        return;
    }
    if (text[0] == '#') {
        text++;
    }
    if (sscanf(text, "%2x%2x%2x", &r, &g, &b) != 3) {
        schultz_colorpicker_show(tree, node, pick);
        return;
    }
    schultz_color_to_hsv(schultz_color_rgba((uint8_t)r, (uint8_t)g,
                                            (uint8_t)b, pick->a),
                         &pick->h, &pick->s, &pick->v);
    schultz_slider_set_value(tree, pick->hue, pick->h);
    schultz_colorpicker_show(tree, node, pick);
}

static int32_t schultz_colorpicker_event(schultz_tree *tree,
                                         schultz_handle node,
                                         const schultz_event *event)
{
    schultz_colorpicker_data *pick = schultz_colorpicker_of(tree, node);

    if (pick == NULL) {
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_FOCUS_LOST &&
        event->target == pick->hex) {
        schultz_colorpicker_take(tree, node, pick);
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_CLICK && event->target == pick->swatch) {
        schultz_popup_open_at(tree, pick->popover, node,
                                SCHULTZ_PLACE_BELOW);
        return SCHULTZ_EVENT_CONSUMED;
    }
    /*
     * A press or a drag inside the square picks saturation and value straight
     * off where it landed, which is what makes it feel like one control
     * rather than two sliders in disguise.
     */
    if ((event->type == SCHULTZ_EVENT_MOUSE_DOWN ||
         event->type == SCHULTZ_EVENT_DRAG) && event->target == pick->square) {
        schultz_rect bounds;

        if (schultz_node_absolute_bounds(tree, pick->square, &bounds)
                == SCHULTZ_OK && bounds.width > 0.0f &&
            bounds.height > 0.0f) {
            float x = event->local.x / bounds.width;
            float y = event->local.y / bounds.height;

            pick->s = (x < 0.0f) ? 0.0f : ((x > 1.0f) ? 1.0f : x);
            pick->v = 1.0f - ((y < 0.0f) ? 0.0f : ((y > 1.0f) ? 1.0f : y));
            schultz_colorpicker_show(tree, node, pick);
        }
        return SCHULTZ_EVENT_CONSUMED;
    }
    /* Either slider having moved is read off both, since only one of them
     * can have been the one that changed. */
    if (event->type == SCHULTZ_EVENT_DRAG ||
        event->type == SCHULTZ_EVENT_MOUSE_UP ||
        event->type == SCHULTZ_EVENT_KEY_DOWN) {
        float hue = schultz_slider_value(tree, pick->hue);
        float set = schultz_slider_value(tree, pick->alpha);

        if (hue != pick->h ||
            (pick->use_alpha && (uint8_t)set != pick->a)) {
            pick->h = hue;
            if (pick->use_alpha) {
                pick->a = (uint8_t)set;
            }
            schultz_colorpicker_show(tree, node, pick);
        }
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_colorpicker_widget = {
    .event = schultz_colorpicker_event, .destroy = free
};

/*
 * As tall as a field holding one line of text, and no taller.
 *
 * A swatch has nothing written in it, so left to itself it measures to its
 * padding and comes out a sliver. What makes it sit level with the date and
 * time pickers beside it is the height one of their rows has, and that is
 * set by the button on the end of the row rather than by the box: a line of
 * text, plus a button's own inset. The swatch is a button, so reading its
 * padding and outline rather than naming a number keeps the two agreeing
 * when a theme changes either.
 */
static int32_t schultz_colorpicker_measure(schultz_tree *tree,
                                           schultz_handle node,
                                           float avail_w, float avail_h,
                                           schultz_size *out_size)
{
    const schultz_colorpicker_data *pick = schultz_colorpicker_of(tree, node);
    schultz_font_system *fonts =
        (schultz_font_system *)schultz_tree_font_system(tree);
    schultz_font_metrics metrics;
    int32_t result = schultz_pane_stack()->measure(tree, node, avail_w,
                                                   avail_h, out_size);

    if (result != SCHULTZ_OK || pick == NULL) {
        return result;
    }
    {
        const schultz_resolved_style *style =
            schultz_widget_style(tree, pick->swatch);
        schultz_handle font = schultz_widget_font(tree, pick->swatch);

        if (fonts != NULL && font != SCHULTZ_HANDLE_NONE && style != NULL &&
            schultz_font_get_metrics(fonts, font, &metrics) == SCHULTZ_OK) {
            /*
             * Padding only, because that is what a pane puts around a
             * child: an outline is drawn on the edge rather than pushing
             * the edge outward. The same sum a button with a word in it
             * comes to.
             */
            out_size->height =
                schultz_label_line_height(style, &metrics) +
                schultz_resolved_number(style, SCHULTZ_PROP_PADDING) * 2.0f;
        }
    }
    return SCHULTZ_OK;
}

static int32_t schultz_colorpicker_arrange(schultz_tree *tree,
                                           schultz_handle node,
                                           schultz_rect rect)
{
    return schultz_pane_stack()->arrange(tree, node, rect);
}

static const schultz_pane_vtable schultz_colorpicker_pane = {
    schultz_colorpicker_measure, schultz_colorpicker_arrange
};

int32_t schultz_color_picker_create(schultz_tree *tree, schultz_handle parent,
                                    schultz_color color,
                                    schultz_handle *out_node)
{
    schultz_colorpicker_data *data;
    schultz_square_data *square;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle content;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    data = (schultz_colorpicker_data *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->a = color.a;
    schultz_color_to_hsv(color, &data->h, &data->s, &data->v);

    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        free(data);
        return result;
    }
    schultz_node_set_pane(tree, node, &schultz_colorpicker_pane);
    result = schultz_button_create(tree, node, "", &data->swatch);
    if (result == SCHULTZ_OK) {
        result = schultz_popup_create(tree, 1, &data->popover);
    }
    if (result != SCHULTZ_OK) {
        free(data);
        schultz_node_destroy(tree, node);
        return result;
    }

    /* A panel dropped under the swatch, not a bubble pointing at it. */
    schultz_popup_set_arrow(tree, data->popover, 0);
    content = schultz_popup_content(tree, data->popover);
    schultz_node_set_pane(tree, content, schultz_pane_vbox());
    schultz_node_set_spacing(tree, content, 0.0f, 6.0f);
    result = schultz_relay_install(tree, content, node);
    if (result != SCHULTZ_OK) {
        free(data);
        schultz_node_destroy(tree, node);
        return result;
    }

    result = schultz_node_create(tree, content, &data->square);
    if (result == SCHULTZ_OK) {
        result = schultz_slider_create(tree, content,
                              SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 359.0f, data->h,
                                       &data->hue);
    }
    if (result == SCHULTZ_OK) {
        result = schultz_slider_create(tree, content,
                              SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 255.0f,
                                       (float)data->a, &data->alpha);
    }
    if (result == SCHULTZ_OK) {
        result = schultz_text_field_create(tree, content, "", &data->hex);
    }
    if (result != SCHULTZ_OK) {
        free(data);
        schultz_node_destroy(tree, node);
        return result;
    }

    square = (schultz_square_data *)calloc(1, sizeof(*square));
    if (square == NULL) {
        free(data);
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    schultz_node_set_widget(tree, data->square, &schultz_square_widget,
                            square);
    schultz_node_set_pref_size(tree, data->square, 180.0f, 140.0f);
    schultz_node_set_state(tree, data->alpha,
        schultz_node_get_state(tree, data->alpha)
            & ~(uint32_t)SCHULTZ_STATE_VISIBLE);

    schultz_node_set_widget(tree, node, &schultz_colorpicker_widget, data);

    /*
     * A swatch is the one control with nothing in it to measure. Its button
     * has no caption, so left alone the picker measures to the button's
     * padding and nothing else, and comes out a sliver stretched across
     * whatever is holding it.
     *
     * The height is the smallest a control may be, which is the published
     * touch target minimum and moves with the theme. The width is twice
     * that, which is a shape a person reads as a colour rather than as a
     * button waiting for a word. Both are a default style, so a style added
     * afterwards overrides them and an inline size overrides both.
     *
     * The maximums are set as well as the preferred sizes, because a box
     * stretches its children across itself over a preferred size and only a
     * maximum stops it.
     */
    {
        schultz_patch swatch_size;

        if (schultz_patch_init(&swatch_size) == SCHULTZ_OK) {
            schultz_patch_set(&swatch_size, SCHULTZ_PROP_PREF_WIDTH,
                              schultz_value_number(88.0f));
            schultz_patch_set(&swatch_size, SCHULTZ_PROP_MAX_WIDTH,
                              schultz_value_number(88.0f));
            schultz_widget_default_style(tree, node, &swatch_size);
        }
    }

    schultz_colorpicker_show(tree, node, data);

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_color_picker_set_color(schultz_tree *tree,
                                       schultz_handle node,
                                       schultz_color color)
{
    schultz_colorpicker_data *pick = schultz_colorpicker_of(tree, node);

    if (pick == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    pick->a = color.a;
    schultz_color_to_hsv(color, &pick->h, &pick->s, &pick->v);
    schultz_slider_set_value(tree, pick->hue, pick->h);
    schultz_slider_set_value(tree, pick->alpha, (float)pick->a);
    schultz_colorpicker_show(tree, node, pick);
    return SCHULTZ_OK;
}

schultz_color schultz_color_picker_color(const schultz_tree *tree,
                                         schultz_handle node)
{
    const schultz_colorpicker_data *pick;

    if (schultz_node_widget(tree, node) != &schultz_colorpicker_widget) {
        return schultz_color_rgba(0, 0, 0, 255);
    }
    pick = (const schultz_colorpicker_data *)schultz_node_widget_data(tree,
                                                                      node);
    if (pick == NULL) {
        return schultz_color_rgba(0, 0, 0, 255);
    }
    return schultz_hsv_to_color(pick->h, pick->s, pick->v, pick->a);
}

int32_t schultz_color_picker_set_alpha_enabled(schultz_tree *tree,
                                               schultz_handle node,
                                               int32_t on)
{
    schultz_colorpicker_data *pick = schultz_colorpicker_of(tree, node);

    if (pick == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    pick->use_alpha = on ? 1u : 0u;
    schultz_node_set_state(tree, pick->alpha,
        on ? (schultz_node_get_state(tree, pick->alpha)
                  | SCHULTZ_STATE_VISIBLE)
           : (schultz_node_get_state(tree, pick->alpha)
                  & ~(uint32_t)SCHULTZ_STATE_VISIBLE));
    if (!on) {
        pick->a = 255u;
        schultz_colorpicker_show(tree, node, pick);
    }
    return SCHULTZ_OK;
}

/* ------------------------------------------------------------- ComboBox */

/** The largest number of items one combo box may hold. */
enum { SCHULTZ_COMBO_ITEMS_MAX = 64 };

/** A button showing the choice, and the menu the choices come from. */
typedef struct {
    schultz_handle label;   /**< Shows the current selection. */
    schultz_handle menu;    /**< Opened on a click. */
    schultz_handle items[SCHULTZ_COMBO_ITEMS_MAX]; /**< The menu's rows. */
    uint32_t       count;    /**< How many items there are. */
    uint32_t       selected; /**< Which one is chosen. */
} schultz_combo_data;

static const schultz_widget_vtable schultz_combo_widget;

/** How much room the arrow needs on the right of a combo box. */
#define SCHULTZ_COMBO_ARROW 20.0f

/*
 * A stack pane would centre the label on both axes or push it into a corner
 * on both, and a combo box wants neither: its text sits at the left, in the
 * middle vertically, with the arrow's room kept clear on the right.
 */
static int32_t schultz_combo_measure(schultz_tree *tree, schultz_handle node,
                                     float avail_w, float avail_h,
                                     schultz_size *out_size)
{
    const schultz_combo_data *combo =
        (const schultz_combo_data *)schultz_node_widget_data(tree, node);
    schultz_size label = { 0.0f, 0.0f };
    float padding = 0.0f;
    float widest = 0.0f;
    uint32_t i;

    if (combo == NULL) {
        *out_size = schultz_size_make(0.0f, 0.0f);
        return SCHULTZ_OK;
    }
    schultz_node_get_spacing(tree, node, &padding, NULL);
    schultz_layout_measure(tree, combo->label, avail_w, avail_h, &label);

    /*
     * As wide as the longest choice, not as wide as the chosen one. A box
     * that changes size when a choice is made is a box that shifts everything
     * beside it, which is not what a chooser should do.
     */
    for (i = 0; i < combo->count; i++) {
        schultz_size item;

        if (schultz_layout_measure(tree, combo->items[i], -1.0f, -1.0f,
                                   &item) == SCHULTZ_OK &&
            item.width > widest) {
            widest = item.width;
        }
    }
    *out_size = schultz_size_make(widest + SCHULTZ_COMBO_ARROW +
                                      padding * 2.0f,
                                  label.height + padding * 2.0f);
    return SCHULTZ_OK;
}

static int32_t schultz_combo_arrange(schultz_tree *tree, schultz_handle node,
                                     schultz_rect rect)
{
    schultz_combo_data *combo =
        (schultz_combo_data *)schultz_node_widget_data(tree, node);
    schultz_size label = { 0.0f, 0.0f };
    float padding = 0.0f;
    float inner;

    if (combo == NULL) {
        return SCHULTZ_OK;
    }
    schultz_node_get_spacing(tree, node, &padding, NULL);
    inner = rect.width - padding * 2.0f - SCHULTZ_COMBO_ARROW;
    if (inner < 0.0f) {
        inner = 0.0f;
    }
    schultz_layout_measure(tree, combo->label, inner, rect.height, &label);

    return schultz_layout_arrange(tree, combo->label,
        schultz_rect_make(padding, (rect.height - label.height) * 0.5f,
                          inner, label.height));
}

static const schultz_pane_vtable schultz_combo_pane = {
    schultz_combo_measure, schultz_combo_arrange
};

static int32_t schultz_combo_paint(schultz_tree *tree, schultz_handle node,
                                   schultz_draw_list *list,
                                   schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_rect bounds;
    int32_t result;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    result = schultz_widget_draw_box(list, style, bounds);
    if (result != SCHULTZ_OK) {
        return result;
    }

    /* A small triangle on the right, which is what says "there is more". */
    if (bounds.width > SCHULTZ_COMBO_ARROW && bounds.height > 8.0f) {
        schultz_point arrow[3];
        float x = bounds.x + bounds.width - 16.0f;
        float y = bounds.y + bounds.height * 0.5f - 2.0f;

        arrow[0] = schultz_point_make(x, y);
        arrow[1] = schultz_point_make(x + 8.0f, y);
        arrow[2] = schultz_point_make(x + 4.0f, y + 5.0f);
        schultz_draw_fill_polygon(list, arrow, 3,
            schultz_resolved_paint(style, SCHULTZ_PROP_TEXT_COLOR),
            SCHULTZ_FILL_NONZERO);
    }
    return schultz_widget_draw_focus_ring(list, style, bounds,
                                          schultz_node_get_state(tree, node));
}

/*
 * A click opens the menu below the box; a click on one of the menu's rows
 * chooses it. Both arrive here because the rows are the combo box's own, so
 * nothing above has to wire the two together.
 */
static int32_t schultz_combo_event(schultz_tree *tree, schultz_handle node,
                                   const schultz_event *event)
{
    schultz_combo_data *combo =
        (schultz_combo_data *)schultz_node_widget_data(tree, node);
    uint32_t i;

    if (combo == NULL) {
        return SCHULTZ_OK;
    }

    if (event->type == SCHULTZ_EVENT_CLICK && event->target == node) {
        if (schultz_menu_is_open(tree, combo->menu)) {
            schultz_menu_close(tree, combo->menu);
        } else {
            schultz_rect box;

            /* The menu lines up with the button, edge for edge. */
            if (schultz_node_absolute_bounds(tree, node, &box) == SCHULTZ_OK) {
                schultz_menu_set_width(tree, combo->menu, box.width);
            }
            schultz_menu_open_for(tree, combo->menu, node,
                                  SCHULTZ_PLACE_BELOW);
        }
        return SCHULTZ_OK;
    }

    if (event->type == SCHULTZ_EVENT_KEY_DOWN) {
        switch (event->key) {
        case SCHULTZ_KEY_DOWN:
            schultz_combo_box_select(tree, node, combo->selected + 1u);
            return SCHULTZ_EVENT_CONSUMED;
        case SCHULTZ_KEY_UP:
            if (combo->selected > 0u) {
                schultz_combo_box_select(tree, node, combo->selected - 1u);
            }
            return SCHULTZ_EVENT_CONSUMED;
        default:
            break;
        }
    }

    if (event->type != SCHULTZ_EVENT_CLICK) {
        return SCHULTZ_OK;
    }
    for (i = 0; i < combo->count; i++) {
        if (combo->items[i] == event->target) {
            schultz_combo_box_select(tree, node, i);
            return SCHULTZ_OK;
        }
    }
    return SCHULTZ_OK;
}

static const schultz_widget_vtable schultz_combo_widget = {
    .paint = schultz_combo_paint, .event = schultz_combo_event, .destroy = free
};

int32_t schultz_combo_box_create(schultz_tree *tree, schultz_handle parent,
                                 schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_combo_data *combo;
    schultz_patch patch;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    combo = (schultz_combo_data *)calloc(1, sizeof(*combo));
    if (combo == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_label_create(tree, node, "", &combo->label);
    if (result == SCHULTZ_OK) {
        result = schultz_menu_create(tree, &combo->menu);
    }
    if (result != SCHULTZ_OK) {
        free(combo);
        schultz_node_destroy(tree, node);
        return result;
    }
    schultz_label_set_wrap(tree, combo->label, 0);
    schultz_node_set_hit_testable(tree, combo->label, 0);
    schultz_menu_set_owner(tree, combo->menu, node);

    schultz_node_set_pane(tree, node, &schultz_combo_pane);
    schultz_node_set_widget(tree, node, &schultz_combo_widget, combo);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_LIST);
    schultz_node_set_actions(tree, node,
                             SCHULTZ_ACTION_CLICK | SCHULTZ_ACTION_FOCUS);
    schultz_node_set_paint_margin(tree, node, SCHULTZ_RING_BLEED);
    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_SURFACE);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_COLOR,
                            SCHULTZ_TOKEN_COLOR_BORDER);
        schultz_patch_token(&patch, SCHULTZ_PROP_BORDER_WIDTH,
                            SCHULTZ_TOKEN_BORDER_WIDTH);
        schultz_patch_token(&patch, SCHULTZ_PROP_CORNER_RADIUS,
                            SCHULTZ_TOKEN_RADIUS_CONTROL);
        schultz_patch_token(&patch, SCHULTZ_PROP_PADDING,
                            SCHULTZ_TOKEN_SPACE_SM);
        schultz_widget_default_style(tree, node, &patch);
    }
    schultz_node_set_state_property(tree, node, SCHULTZ_STYLE_STATE_HOVERED,
        SCHULTZ_PROP_BORDER_COLOR,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_ACCENT));

    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_combo_box_add(schultz_tree *tree, schultz_handle node,
                              const char *text)
{
    schultz_combo_data *combo;
    schultz_handle item = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (schultz_node_widget(tree, node) != &schultz_combo_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    combo = (schultz_combo_data *)schultz_node_widget_data(tree, node);
    if (combo->count == SCHULTZ_COMBO_ITEMS_MAX) {
        return SCHULTZ_ERR_EXHAUSTED;
    }
    result = schultz_menu_add(tree, combo->menu, text, NULL, &item);
    if (result != SCHULTZ_OK) {
        return result;
    }
    combo->items[combo->count] = item;
    combo->count++;
    /* A longer choice than any before it makes the box wider. */
    schultz_node_invalidate_layout(tree, node);
    /* The first item added becomes the choice, so a box is never blank. */
    if (combo->count == 1u) {
        return schultz_combo_box_select(tree, node, 0u);
    }
    return SCHULTZ_OK;
}

int32_t schultz_combo_box_select(schultz_tree *tree, schultz_handle node,
                                 uint32_t index)
{
    schultz_combo_data *combo;

    if (schultz_node_widget(tree, node) != &schultz_combo_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    combo = (schultz_combo_data *)schultz_node_widget_data(tree, node);
    if (index >= combo->count) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    combo->selected = index;
    schultz_label_set_text(tree, combo->label,
                           schultz_node_get_name(tree, combo->items[index]));
    schultz_node_set_value(tree, node,
                           schultz_node_get_name(tree, combo->items[index]));
    return schultz_node_invalidate(tree, node);
}

uint32_t schultz_combo_box_selected(const schultz_tree *tree,
                                    schultz_handle node)
{
    const schultz_combo_data *combo;

    if (schultz_node_widget(tree, node) != &schultz_combo_widget) {
        return 0u;
    }
    combo = (const schultz_combo_data *)schultz_node_widget_data(tree, node);
    return (combo == NULL) ? 0u : combo->selected;
}

uint32_t schultz_combo_box_count(const schultz_tree *tree,
                                 schultz_handle node)
{
    const schultz_combo_data *combo;

    if (schultz_node_widget(tree, node) != &schultz_combo_widget) {
        return 0u;
    }
    combo = (const schultz_combo_data *)schultz_node_widget_data(tree, node);
    return (combo == NULL) ? 0u : combo->count;
}

schultz_handle schultz_combo_box_menu(const schultz_tree *tree,
                                      schultz_handle node)
{
    const schultz_combo_data *combo;

    if (schultz_node_widget(tree, node) != &schultz_combo_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    combo = (const schultz_combo_data *)schultz_node_widget_data(tree, node);
    return (combo == NULL) ? SCHULTZ_HANDLE_NONE : combo->menu;
}

/* ----------------------------------------------------------------- Icon */

/** How a picture is fitted into the room it is given. */
typedef struct {
    schultz_handle image;   /**< What to draw, or none. */
    uint32_t       fit;     /**< One of the SCHULTZ_FIT_* values. */
    /*
     * Taking part in a selection. A picture is in a range or out of it and
     * has no inside, so one flag is the whole of its state.
     */
    uint32_t       selectable; /**< Nonzero when a drag may take it. */
    uint32_t       selected;   /**< Nonzero while it is in the range. */
} schultz_icon_data;

static const schultz_widget_vtable schultz_icon_widget;

/* The image's natural size, or nothing when there is no image to ask about. */
static int32_t schultz_icon_natural(schultz_tree *tree, schultz_handle node,
                                    schultz_size *out_size)
{
    const schultz_icon_data *icon =
        (const schultz_icon_data *)schultz_node_widget_data(tree, node);
    schultz_image_table *images =
        (schultz_image_table *)schultz_tree_image_table(tree);

    *out_size = schultz_size_make(0.0f, 0.0f);
    if (icon == NULL || images == NULL ||
        icon->image == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_image_size(images, icon->image, out_size);
}

static int32_t schultz_icon_measure(schultz_tree *tree, schultz_handle node,
                                    float avail_w, float avail_h,
                                    schultz_size *out_size)
{
    (void)avail_w;
    (void)avail_h;
    /*
     * Its natural size. A size hint overrules it, which is how an icon is
     * asked for at a particular size, and the fit decides what happens to the
     * picture inside that.
     */
    schultz_icon_natural(tree, node, out_size);
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_icon_pane = {
    schultz_icon_measure, schultz_leaf_arrange
};

/*
 * Where the picture goes inside the room the icon was given. Filling is the
 * odd one out: it is the only fit that changes the picture's proportions.
 */
static schultz_rect schultz_icon_place(uint32_t fit, schultz_size natural,
                                       schultz_rect bounds)
{
    float scale;
    float width;
    float height;

    if (fit == SCHULTZ_FIT_FILL || natural.width <= 0.0f ||
        natural.height <= 0.0f) {
        return bounds;
    }

    scale = bounds.width / natural.width;
    if (fit == SCHULTZ_FIT_CONTAIN) {
        float other = bounds.height / natural.height;

        if (other < scale) {
            scale = other;
        }
    } else if (fit == SCHULTZ_FIT_COVER) {
        float other = bounds.height / natural.height;

        if (other > scale) {
            scale = other;
        }
    } else {
        scale = 1.0f; /* SCHULTZ_FIT_NONE: drawn at its own size */
    }

    width  = natural.width * scale;
    height = natural.height * scale;
    /* Centred in what is left over, whichever way it did not fill. */
    return schultz_rect_make(bounds.x + (bounds.width - width) * 0.5f,
                             bounds.y + (bounds.height - height) * 0.5f,
                             width, height);
}

static int32_t schultz_icon_paint(schultz_tree *tree, schultz_handle node,
                                  schultz_draw_list *list,
                                  schultz_arena *arena)
{
    const schultz_icon_data *icon =
        (const schultz_icon_data *)schultz_node_widget_data(tree, node);
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_size natural;
    schultz_rect bounds;
    schultz_rect at;
    float opacity;
    int32_t result;

    (void)arena;
    if (icon == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    result = schultz_widget_draw_box(list, style, bounds);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (schultz_icon_natural(tree, node, &natural) != SCHULTZ_OK ||
        schultz_rect_is_empty(bounds)) {
        return SCHULTZ_OK;
    }

    at = schultz_icon_place(icon->fit, natural, bounds);
    /*
     * Solid. Opacity is the paint walk's business now: it wraps any node
     * whose opacity is below one in a group, and this picture is inside that
     * group like anything else. Reading the property again here would fade
     * the picture twice.
     */
    opacity = 1.0f;

    /*
     * A cover fit spills past the bounds by design, so it is clipped. The
     * others never do, and pushing a clip for them would cost a pair of
     * commands every frame for nothing.
     */
    if (icon->fit == SCHULTZ_FIT_COVER) {
        result = schultz_draw_clip_begin(list, bounds);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    result = schultz_draw_image(list, icon->image,
                                schultz_rect_make(0.0f, 0.0f, 0.0f, 0.0f), at,
                                (uint8_t)(opacity * 255.0f + 0.5f));
    if (icon->fit == SCHULTZ_FIT_COVER) {
        schultz_draw_clip_end(list);
    }

    /*
     * A selected picture says so.
     *
     * Text shows a band behind it, which a picture cannot do without hiding
     * the thing being selected. So it is marked round the edge instead: a
     * wash of the selection colour over it, which tints without obscuring,
     * and a line round the picture itself rather than round the node, because
     * a picture that does not fill its box would otherwise be marked by a
     * rectangle floating away from it.
     */
    if (result == SCHULTZ_OK && icon->selected) {
        schultz_color tint = schultz_resolved_color(style,
                                                    SCHULTZ_PROP_SELECTION_COLOR);

        result = schultz_draw_fill_rect(list, at, schultz_paint_solid(tint));
        if (result == SCHULTZ_OK) {
            schultz_color edge = tint;

            /* The same colour at full strength, so the edge reads as a line
             * rather than as more of the wash. */
            edge.a = 255u;
            result = schultz_draw_stroke_rect(list, at,
                                              schultz_stroke_solid(edge,
                                                                   2.0f));
        }
    }
    return result;
}

/* The icon's own state, or NULL when the handle is not one. */
static schultz_icon_data *schultz_icon_of(schultz_tree *tree,
                                          schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_icon_widget) {
        return NULL;
    }
    return (schultz_icon_data *)schultz_node_widget_data(tree, node);
}

/* ------------------------------------------ taking part in a selection */

/*
 * A picture counts as one place in a range: a length of one, an offset of
 * nought before it and one after it. There is nothing inside it to land in
 * the middle of, which is what makes a picture simpler here than text.
 */
static uint32_t schultz_icon_select_length(schultz_tree *tree,
                                           schultz_handle node)
{
    const schultz_icon_data *icon = schultz_icon_of(tree, node);

    return (icon == NULL || !icon->selectable) ? 0u : 1u;
}

static uint32_t schultz_icon_select_offset_at(schultz_tree *tree,
                                              schultz_handle node,
                                              schultz_point local)
{
    const schultz_icon_data *icon = schultz_icon_of(tree, node);
    schultz_rect bounds;

    if (icon == NULL || !icon->selectable ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return 0u;
    }
    /* Past the middle is after it, which is how a drag through a picture
     * takes it rather than stopping short of it. */
    return (local.x > bounds.width * 0.5f) ? 1u : 0u;
}

static void schultz_icon_select_set_range(schultz_tree *tree,
                                          schultz_handle node,
                                          uint32_t low, uint32_t high)
{
    schultz_icon_data *icon = schultz_icon_of(tree, node);
    uint32_t now = (high > low) ? 1u : 0u;

    if (icon == NULL || !icon->selectable || icon->selected == now) {
        return;
    }
    icon->selected = now;
    schultz_node_invalidate(tree, node);
}

static void schultz_icon_select_range(schultz_tree *tree, schultz_handle node,
                                      uint32_t *out_low, uint32_t *out_high)
{
    const schultz_icon_data *icon = schultz_icon_of(tree, node);

    *out_low  = 0u;
    *out_high = (icon != NULL && icon->selected) ? 1u : 0u;
}

/* A picture has no text, which is how it adds nothing to a plain text copy. */
static const char *schultz_icon_select_text(schultz_tree *tree,
                                            schultz_handle node)
{
    (void)tree;
    (void)node;
    return NULL;
}

static int32_t schultz_icon_select_is_picture(schultz_tree *tree,
                                              schultz_handle node)
{
    (void)tree;
    (void)node;
    return 1;
}

static const schultz_selectable_vtable schultz_icon_selection_part = {
    schultz_icon_select_length,
    schultz_icon_select_offset_at,
    schultz_icon_select_set_range,
    schultz_icon_select_range,
    schultz_icon_select_text,
    schultz_icon_select_is_picture,
    NULL                          /* a picture has no words to style */
};

static const schultz_widget_vtable schultz_icon_widget = {
    .paint = schultz_icon_paint, .destroy = free,
    .selectable = &schultz_icon_selection_part
};

int32_t schultz_icon_set_selectable(schultz_tree *tree, schultz_handle node,
                                    int32_t selectable)
{
    schultz_icon_data *icon = schultz_icon_of(tree, node);

    if (icon == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (icon->selectable == (selectable ? 1u : 0u)) {
        return SCHULTZ_OK;
    }
    icon->selectable = selectable ? 1u : 0u;
    if (!icon->selectable) {
        icon->selected = 0u;
    }
    return schultz_node_invalidate(tree, node);
}

int32_t schultz_icon_selectable(const schultz_tree *tree, schultz_handle node)
{
    const schultz_icon_data *icon =
        schultz_icon_of((schultz_tree *)tree, node);

    return (icon == NULL) ? 0 : (int32_t)icon->selectable;
}

int32_t schultz_icon_create(schultz_tree *tree, schultz_handle parent,
                            schultz_handle image, schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_icon_data *icon;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    icon = (schultz_icon_data *)calloc(1, sizeof(*icon));
    if (icon == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    icon->image = image;
    icon->fit   = SCHULTZ_FIT_CONTAIN;

    schultz_node_set_pane(tree, node, &schultz_icon_pane);
    schultz_node_set_widget(tree, node, &schultz_icon_widget, icon);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_IMAGE);
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_icon_set_image(schultz_tree *tree, schultz_handle node,
                               schultz_handle image)
{
    schultz_icon_data *icon;
    schultz_image_table *images;
    schultz_size was;
    schultz_size now;
    int32_t resized = 1;

    if (schultz_node_widget(tree, node) != &schultz_icon_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    icon = (schultz_icon_data *)schultz_node_widget_data(tree, node);

    /*
     * A different picture may be a different size, and when it is, the layout
     * has to run again. When it is not, it must not: a host showing a moving
     * picture hands over a new one every frame, and laying the window out
     * sixty times a second for pictures that are all the same size is the
     * most expensive thing it could possibly be doing. Measured on the demo's
     * round trip card, which does this twice a frame: 23 milliseconds a frame
     * of layout, against well under one when the size is checked first.
     */
    images = (schultz_image_table *)schultz_tree_image_table(tree);
    if (images != NULL &&
        schultz_image_size(images, icon->image, &was) == SCHULTZ_OK &&
        schultz_image_size(images, image, &now) == SCHULTZ_OK) {
        resized = (was.width != now.width) || (was.height != now.height);
    }
    icon->image = image;
    if (resized) {
        schultz_node_invalidate_layout(tree, node);
    }
    return schultz_node_invalidate(tree, node);
}

schultz_handle schultz_icon_image(const schultz_tree *tree,
                                  schultz_handle node)
{
    const schultz_icon_data *icon;

    if (schultz_node_widget(tree, node) != &schultz_icon_widget) {
        return SCHULTZ_HANDLE_NONE;
    }
    icon = (const schultz_icon_data *)schultz_node_widget_data(tree, node);
    return (icon == NULL) ? SCHULTZ_HANDLE_NONE : icon->image;
}

int32_t schultz_icon_set_fit(schultz_tree *tree, schultz_handle node,
                             uint32_t fit)
{
    schultz_icon_data *icon;

    if (fit > SCHULTZ_FIT_NONE) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (schultz_node_widget(tree, node) != &schultz_icon_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    icon = (schultz_icon_data *)schultz_node_widget_data(tree, node);
    icon->fit = fit;
    return schultz_node_invalidate(tree, node);
}

/* ----------------------------------------------------------- LottieView */

/** Where an animation is up to, and whether it is going anywhere. */
typedef struct {
    schultz_handle image;    /**< The loaded animation. */
    float          frames;   /**< How many frames it has. */
    float          duration; /**< How long it runs, in seconds. */
    float          at;       /**< Which frame is showing. */
    uint32_t       playing;  /**< Nonzero while it is running. */
    uint32_t       looping;  /**< Nonzero to start again at the end. */
} schultz_lottie_data;

static const schultz_widget_vtable schultz_lottie_widget;

static int32_t schultz_lottie_measure(schultz_tree *tree, schultz_handle node,
                                      float avail_w, float avail_h,
                                      schultz_size *out_size)
{
    const schultz_lottie_data *view =
        (const schultz_lottie_data *)schultz_node_widget_data(tree, node);
    schultz_image_table *images =
        (schultz_image_table *)schultz_tree_image_table(tree);

    (void)avail_w;
    (void)avail_h;
    *out_size = schultz_size_make(0.0f, 0.0f);
    if (view != NULL && images != NULL) {
        schultz_image_size(images, view->image, out_size);
    }
    return SCHULTZ_OK;
}

static const schultz_pane_vtable schultz_lottie_pane = {
    schultz_lottie_measure, schultz_leaf_arrange
};

static int32_t schultz_lottie_paint(schultz_tree *tree, schultz_handle node,
                                    schultz_draw_list *list,
                                    schultz_arena *arena)
{
    const schultz_lottie_data *view =
        (const schultz_lottie_data *)schultz_node_widget_data(tree, node);
    schultz_rect bounds;

    (void)arena;
    if (view == NULL || view->image == SCHULTZ_HANDLE_NONE ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        schultz_rect_is_empty(bounds)) {
        return SCHULTZ_OK;
    }
    /*
     * Whichever frame the animation is on, stretched to the node. Vector
     * artwork, so it is drawn at this size rather than resampled to it.
     */
    return schultz_draw_image(list, view->image,
                              schultz_rect_make(0.0f, 0.0f, 0.0f, 0.0f),
                              bounds, 255u);
}

/*
 * The clock reaches the animation here. Frames are advanced by however long
 * has passed rather than one per tick, so playback runs at the speed it was
 * authored at whatever rate the host is drawing.
 */
static int32_t schultz_lottie_tick(schultz_tree *tree, schultz_handle node,
                                   uint64_t now_ms, uint32_t elapsed_ms)
{
    schultz_lottie_data *view =
        (schultz_lottie_data *)schultz_node_widget_data(tree, node);
    schultz_image_table *images =
        (schultz_image_table *)schultz_tree_image_table(tree);
    float last;
    float advance;

    (void)now_ms;
    if (view == NULL || images == NULL || !view->playing ||
        view->duration <= 0.0f || view->frames <= 1.0f || elapsed_ms == 0u) {
        return 0;
    }

    last = view->frames - 1.0f;
    advance = (float)elapsed_ms / 1000.0f * (last / view->duration);
    view->at += advance;

    if (view->at >= last) {
        if (view->looping) {
            /*
             * Wrapped rather than reset, so a slow frame does not lose the
             * part of the animation it should have covered.
             */
            while (view->at >= last) {
                view->at -= last;
            }
        } else {
            view->at = last;
            view->playing = 0u;
            schultz_node_set_animating(tree, node, 0);
        }
    }

    schultz_image_set_frame(images, view->image, view->at);
    /* Only this node changed, so only this node repaints. */
    schultz_node_invalidate(tree, node);
    return 1;
}

static const schultz_widget_vtable schultz_lottie_widget = {
    .paint = schultz_lottie_paint,
    .tick = schultz_lottie_tick,
    .destroy = free
};

int32_t schultz_lottie_create(schultz_tree *tree, schultz_handle parent,
                              schultz_handle image, schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_image_table *images =
        (schultz_image_table *)schultz_tree_image_table(tree);
    schultz_lottie_data *view;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    view = (schultz_lottie_data *)calloc(1, sizeof(*view));
    if (view == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    view->image   = image;
    view->looping = 1u;
    if (images != NULL) {
        schultz_image_frames(images, image, &view->frames, &view->duration);
    }

    schultz_node_set_pane(tree, node, &schultz_lottie_pane);
    schultz_node_set_widget(tree, node, &schultz_lottie_widget, view);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_IMAGE);
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_lottie_play(schultz_tree *tree, schultz_handle node,
                            int32_t playing)
{
    schultz_lottie_data *view;

    if (schultz_node_widget(tree, node) != &schultz_lottie_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    view = (schultz_lottie_data *)schultz_node_widget_data(tree, node);
    view->playing = playing ? 1u : 0u;
    /*
     * Asking to be ticked is what costs anything, so a paused animation is
     * as cheap as no animation at all.
     */
    return schultz_node_set_animating(tree, node, playing);
}

int32_t schultz_lottie_set_looping(schultz_tree *tree, schultz_handle node,
                                   int32_t looping)
{
    schultz_lottie_data *view;

    if (schultz_node_widget(tree, node) != &schultz_lottie_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    view = (schultz_lottie_data *)schultz_node_widget_data(tree, node);
    view->looping = looping ? 1u : 0u;
    return SCHULTZ_OK;
}

int32_t schultz_lottie_seek(schultz_tree *tree, schultz_handle node,
                            float frame)
{
    schultz_image_table *images =
        (schultz_image_table *)schultz_tree_image_table(tree);
    schultz_lottie_data *view;

    if (schultz_node_widget(tree, node) != &schultz_lottie_widget) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    view = (schultz_lottie_data *)schultz_node_widget_data(tree, node);
    if (frame < 0.0f) {
        frame = 0.0f;
    }
    if (view->frames > 1.0f && frame > view->frames - 1.0f) {
        frame = view->frames - 1.0f;
    }
    view->at = frame;
    if (images != NULL) {
        schultz_image_set_frame(images, view->image, frame);
    }
    return schultz_node_invalidate(tree, node);
}

float schultz_lottie_frame(const schultz_tree *tree, schultz_handle node)
{
    const schultz_lottie_data *view;

    if (schultz_node_widget(tree, node) != &schultz_lottie_widget) {
        return 0.0f;
    }
    view = (const schultz_lottie_data *)schultz_node_widget_data(tree, node);
    return (view == NULL) ? 0.0f : view->at;
}

int32_t schultz_lottie_is_playing(const schultz_tree *tree,
                                  schultz_handle node)
{
    const schultz_lottie_data *view;

    if (schultz_node_widget(tree, node) != &schultz_lottie_widget) {
        return 0;
    }
    view = (const schultz_lottie_data *)schultz_node_widget_data(tree, node);
    return (view == NULL) ? 0 : (int32_t)view->playing;
}

/* ------------------------------------------------------ On-screen keyboard */

/*
 * A keyboard the toolkit draws itself.
 *
 * A touch screen on a Raspberry Pi, started on the framebuffer, has no
 * keyboard and no way to ask for one. SDL implements a screen keyboard for
 * Android, iOS and a few consoles; for KMSDRM it implements nothing at all.
 * So a text field on that machine cannot be typed into unless something draws
 * the keys.
 *
 * A press goes out through schultz_events_text_input and schultz_events_key,
 * which are the calls the platform layer makes for a real keyboard. Nothing
 * below can tell the difference, and no widget needed changing to accept it.
 *
 * All four sets of keys are built once and three are hidden. Switching sets
 * is then a visibility flag rather than building and destroying forty nodes
 * from inside the event handler of a key that would be destroyed doing it.
 *
 * A key does not take focus. The field being edited has to keep it, or the
 * first press would end the edit the keyboard exists to serve.
 */

/** @brief Which set of keys is showing. */
enum {
    SCHULTZ_KEYS_LETTERS = 0, /**< Lower case. */
    SCHULTZ_KEYS_SHIFTED,     /**< Upper case. */
    SCHULTZ_KEYS_SYMBOLS,     /**< Numbers and punctuation. */
    SCHULTZ_KEYS_EMOJI,       /**< A page of common emoji. */
    SCHULTZ_KEYS_SETS         /**< How many there are, not one of them. */
};

/** @brief How long a held key waits before repeating, and then how often. */
#define SCHULTZ_KEY_REPEAT_DELAY_MS  400u
#define SCHULTZ_KEY_REPEAT_EVERY_MS   60u

/** @brief One key, as data rather than as code. */
typedef struct {
    const char *cap;     /**< Drawn on the key. */
    const char *sends;   /**< The text it enters, or NULL. */
    uint32_t    key;     /**< A SCHULTZ_KEY_* it sends instead, or zero. */
    uint32_t    goes_to; /**< A set to switch to, or SCHULTZ_KEYS_SETS. */
    float       width;   /**< Share of its row. One is an ordinary key. */
    const char *name;    /**< What a reader says, when the cap is not words. */
} schultz_key;

/** @brief One row of keys. */
typedef struct {
    const schultz_key *keys;
    uint32_t           count;
} schultz_key_row;

/* The tables read as keyboards this way round, which is the point of them. */
#define SCHULTZ_TYPES(cap)    { cap, cap, 0u, SCHULTZ_KEYS_SETS, 1.0f, NULL }
#define SCHULTZ_SPACE(w)      { " ", " ", 0u, SCHULTZ_KEYS_SETS, w, "space" }
#define SCHULTZ_SENDS(cap, key, w, name) \
    { cap, NULL, key, SCHULTZ_KEYS_SETS, w, name }
#define SCHULTZ_SWITCHES(cap, to, w, name) \
    { cap, NULL, 0u, to, w, name }
#define SCHULTZ_EMOJI(cap, name) \
    { cap, cap, 0u, SCHULTZ_KEYS_SETS, 1.0f, name }

static const schultz_key schultz_letters_row0[] = {
    SCHULTZ_TYPES("q"), SCHULTZ_TYPES("w"), SCHULTZ_TYPES("e"),
    SCHULTZ_TYPES("r"), SCHULTZ_TYPES("t"), SCHULTZ_TYPES("y"),
    SCHULTZ_TYPES("u"), SCHULTZ_TYPES("i"), SCHULTZ_TYPES("o"),
    SCHULTZ_TYPES("p")
};
static const schultz_key schultz_letters_row1[] = {
    SCHULTZ_TYPES("a"), SCHULTZ_TYPES("s"), SCHULTZ_TYPES("d"),
    SCHULTZ_TYPES("f"), SCHULTZ_TYPES("g"), SCHULTZ_TYPES("h"),
    SCHULTZ_TYPES("j"), SCHULTZ_TYPES("k"), SCHULTZ_TYPES("l")
};
static const schultz_key schultz_letters_row2[] = {
    SCHULTZ_SWITCHES("Shift", SCHULTZ_KEYS_SHIFTED, 1.5f, "shift"),
    SCHULTZ_TYPES("z"), SCHULTZ_TYPES("x"), SCHULTZ_TYPES("c"),
    SCHULTZ_TYPES("v"), SCHULTZ_TYPES("b"), SCHULTZ_TYPES("n"),
    SCHULTZ_TYPES("m"),
    SCHULTZ_SENDS("Back", SCHULTZ_KEY_BACKSPACE, 1.5f, "backspace")
};
static const schultz_key schultz_letters_row3[] = {
    SCHULTZ_SWITCHES("?123", SCHULTZ_KEYS_SYMBOLS, 1.5f, "numbers"),
    SCHULTZ_SWITCHES("Emoji", SCHULTZ_KEYS_EMOJI, 1.0f, "emoji"),
    SCHULTZ_TYPES(","), SCHULTZ_SPACE(4.0f), SCHULTZ_TYPES("."),
    SCHULTZ_SENDS("Enter", SCHULTZ_KEY_RETURN, 1.5f, "enter")
};

static const schultz_key schultz_shifted_row0[] = {
    SCHULTZ_TYPES("Q"), SCHULTZ_TYPES("W"), SCHULTZ_TYPES("E"),
    SCHULTZ_TYPES("R"), SCHULTZ_TYPES("T"), SCHULTZ_TYPES("Y"),
    SCHULTZ_TYPES("U"), SCHULTZ_TYPES("I"), SCHULTZ_TYPES("O"),
    SCHULTZ_TYPES("P")
};
static const schultz_key schultz_shifted_row1[] = {
    SCHULTZ_TYPES("A"), SCHULTZ_TYPES("S"), SCHULTZ_TYPES("D"),
    SCHULTZ_TYPES("F"), SCHULTZ_TYPES("G"), SCHULTZ_TYPES("H"),
    SCHULTZ_TYPES("J"), SCHULTZ_TYPES("K"), SCHULTZ_TYPES("L")
};
static const schultz_key schultz_shifted_row2[] = {
    /* Aimed at the shifted set like the other shift key: what a second press
     * means is decided where the press is handled, not here. */
    SCHULTZ_SWITCHES("Shift", SCHULTZ_KEYS_SHIFTED, 1.5f, "shift"),
    SCHULTZ_TYPES("Z"), SCHULTZ_TYPES("X"), SCHULTZ_TYPES("C"),
    SCHULTZ_TYPES("V"), SCHULTZ_TYPES("B"), SCHULTZ_TYPES("N"),
    SCHULTZ_TYPES("M"),
    SCHULTZ_SENDS("Back", SCHULTZ_KEY_BACKSPACE, 1.5f, "backspace")
};

static const schultz_key schultz_symbols_row0[] = {
    SCHULTZ_TYPES("1"), SCHULTZ_TYPES("2"), SCHULTZ_TYPES("3"),
    SCHULTZ_TYPES("4"), SCHULTZ_TYPES("5"), SCHULTZ_TYPES("6"),
    SCHULTZ_TYPES("7"), SCHULTZ_TYPES("8"), SCHULTZ_TYPES("9"),
    SCHULTZ_TYPES("0")
};
static const schultz_key schultz_symbols_row1[] = {
    SCHULTZ_TYPES("-"), SCHULTZ_TYPES("/"), SCHULTZ_TYPES(":"),
    SCHULTZ_TYPES(";"), SCHULTZ_TYPES("("), SCHULTZ_TYPES(")"),
    SCHULTZ_TYPES("$"), SCHULTZ_TYPES("&"), SCHULTZ_TYPES("@"),
    SCHULTZ_TYPES("\"")
};
static const schultz_key schultz_symbols_row2[] = {
    SCHULTZ_TYPES("#"), SCHULTZ_TYPES("%"), SCHULTZ_TYPES("*"),
    SCHULTZ_TYPES("+"), SCHULTZ_TYPES("="), SCHULTZ_TYPES("_"),
    SCHULTZ_TYPES("!"), SCHULTZ_TYPES("?"), SCHULTZ_TYPES("'"),
    SCHULTZ_SENDS("Back", SCHULTZ_KEY_BACKSPACE, 1.0f, "backspace")
};
static const schultz_key schultz_symbols_row3[] = {
    SCHULTZ_SWITCHES("abc", SCHULTZ_KEYS_LETTERS, 1.5f, "letters"),
    SCHULTZ_SWITCHES("Emoji", SCHULTZ_KEYS_EMOJI, 1.0f, "emoji"),
    SCHULTZ_TYPES(","), SCHULTZ_SPACE(4.0f), SCHULTZ_TYPES("."),
    SCHULTZ_SENDS("Enter", SCHULTZ_KEY_RETURN, 1.5f, "enter")
};

/*
 * The emoji, in the order a phone shows them: faces, then gestures, animals,
 * food, places, activities, objects and symbols.
 * Grouped rather than shuffled, because a page of faces followed by a page
 * of animals is how everyone expects to find one, and the font itself has
 * no order worth using.
 *
 * Each is named as well as drawn: the cap is a picture, and a name is what a
 * screen reader has to say. The names are the characters' own, from
 * Unicode.
 */
static const schultz_key schultz_emoji_keys[] = {
    /* Smileys and emotion */
    SCHULTZ_EMOJI("\xF0\x9F\x98\x80", "grinning face"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x83", "smiling face with open mouth"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x84",
        "smiling face with open mouth and smiling eyes"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x81", "grinning face with smiling eyes"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x86",
        "smiling face with open mouth and tightly-closed eyes"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x85",
        "smiling face with open mouth and cold sweat"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x82", "face with tears of joy"),
    SCHULTZ_EMOJI("\xF0\x9F\x99\x82", "slightly smiling face"),
    SCHULTZ_EMOJI("\xF0\x9F\x99\x83", "upside-down face"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x89", "winking face"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x8A", "smiling face with smiling eyes"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x8D", "smiling face with heart-shaped eyes"),
    SCHULTZ_EMOJI("\xF0\x9F\xA5\xB0",
        "smiling face with smiling eyes and three hearts"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x98", "face throwing a kiss"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x8E", "smiling face with sunglasses"),
    SCHULTZ_EMOJI("\xF0\x9F\xA4\xA9", "grinning face with star eyes"),
    SCHULTZ_EMOJI("\xF0\x9F\xA4\x94", "thinking face"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x90", "neutral face"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\xAE", "face with open mouth"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\xB4", "sleeping face"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\xA2", "crying face"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\xAD", "loudly crying face"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\xA1", "pouting face"),
    SCHULTZ_EMOJI("\xF0\x9F\xA5\xB3", "face with party horn and party hat"),
    SCHULTZ_EMOJI("\xF0\x9F\x98\x87", "smiling face with halo"),
    SCHULTZ_EMOJI("\xF0\x9F\xA4\x97", "hugging face"),
    SCHULTZ_EMOJI("\xF0\x9F\xA4\xAF", "shocked face with exploding head"),
    SCHULTZ_EMOJI("\xF0\x9F\xA5\xB1", "yawning face"),
    /* People and gestures */
    SCHULTZ_EMOJI("\xF0\x9F\x91\x8D", "thumbs up sign"),
    SCHULTZ_EMOJI("\xF0\x9F\x91\x8E", "thumbs down sign"),
    SCHULTZ_EMOJI("\xF0\x9F\x91\x8F", "clapping hands sign"),
    SCHULTZ_EMOJI("\xF0\x9F\x99\x8F", "person with folded hands"),
    SCHULTZ_EMOJI("\xF0\x9F\x92\xAA", "flexed biceps"),
    SCHULTZ_EMOJI("\xF0\x9F\x91\x8B", "waving hand sign"),
    SCHULTZ_EMOJI("\xF0\x9F\xA4\x9D", "handshake"),
    SCHULTZ_EMOJI("\xF0\x9F\x91\x8C", "ok hand sign"),
    SCHULTZ_EMOJI("\xF0\x9F\x99\x8C",
        "person raising both hands in celebration"),
    SCHULTZ_EMOJI("\xF0\x9F\xA4\x99", "call me hand"),
    SCHULTZ_EMOJI("\xF0\x9F\x91\x86", "white up pointing backhand index"),
    SCHULTZ_EMOJI("\xF0\x9F\x91\x87", "white down pointing backhand index"),
    /* Animals and nature */
    SCHULTZ_EMOJI("\xF0\x9F\x90\xB6", "dog face"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\xB1", "cat face"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\xAD", "mouse face"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\xB0", "rabbit face"),
    SCHULTZ_EMOJI("\xF0\x9F\xA6\x8A", "fox face"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\xBB", "bear face"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\xBC", "panda face"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\xA8", "koala"),
    SCHULTZ_EMOJI("\xF0\x9F\xA6\x81", "lion face"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\xAE", "cow face"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\xB7", "pig face"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\xB8", "frog face"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\xB5", "monkey face"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\x94", "chicken"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\xA7", "penguin"),
    SCHULTZ_EMOJI("\xF0\x9F\xA6\x89", "owl"),
    SCHULTZ_EMOJI("\xF0\x9F\xA6\x8B", "butterfly"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\x9D", "honeybee"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\x9F", "fish"),
    SCHULTZ_EMOJI("\xF0\x9F\x90\xB3", "spouting whale"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x80", "four leaf clover"),
    SCHULTZ_EMOJI("\xF0\x9F\x8C\xB2", "evergreen tree"),
    SCHULTZ_EMOJI("\xF0\x9F\x8C\xB5", "cactus"),
    SCHULTZ_EMOJI("\xF0\x9F\x8C\xBB", "sunflower"),
    SCHULTZ_EMOJI("\xF0\x9F\x8C\xB9", "rose"),
    SCHULTZ_EMOJI("\xF0\x9F\x8C\x9E", "sun with face"),
    SCHULTZ_EMOJI("\xF0\x9F\x8C\x99", "crescent moon"),
    SCHULTZ_EMOJI("\xF0\x9F\x8C\x9F", "glowing star"),
    SCHULTZ_EMOJI("\xF0\x9F\x8C\x88", "rainbow"),
    SCHULTZ_EMOJI("\xF0\x9F\x94\xA5", "fire"),
    SCHULTZ_EMOJI("\xF0\x9F\x92\xA7", "droplet"),
    SCHULTZ_EMOJI("\xE2\x9D\x84\xEF\xB8\x8F", "snowflake"),
    /* Food and drink */
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x8E", "red apple"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x8C", "banana"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x87", "grapes"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x93", "strawberry"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x92", "cherries"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x91", "peach"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x8D", "pineapple"),
    SCHULTZ_EMOJI("\xF0\x9F\xA5\x91", "avocado"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x85", "tomato"),
    SCHULTZ_EMOJI("\xF0\x9F\xA5\x95", "carrot"),
    SCHULTZ_EMOJI("\xF0\x9F\x8C\xBD", "ear of maize"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x9E", "bread"),
    SCHULTZ_EMOJI("\xF0\x9F\xA7\x80", "cheese wedge"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x94", "hamburger"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x9F", "french fries"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x95", "slice of pizza"),
    SCHULTZ_EMOJI("\xF0\x9F\x8C\xAE", "taco"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\xA3", "sushi"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\x9C", "steaming bowl"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\xB0", "shortcake"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\xAA", "cookie"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\xAB", "chocolate bar"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\xBF", "popcorn"),
    SCHULTZ_EMOJI("\xE2\x98\x95", "hot beverage"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\xB5", "teacup without handle"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\xBA", "beer mug"),
    SCHULTZ_EMOJI("\xF0\x9F\x8D\xB7", "wine glass"),
    /* Travel and places */
    SCHULTZ_EMOJI("\xF0\x9F\x9A\x97", "automobile"),
    SCHULTZ_EMOJI("\xF0\x9F\x9A\x95", "taxi"),
    SCHULTZ_EMOJI("\xF0\x9F\x9A\x8C", "bus"),
    SCHULTZ_EMOJI("\xF0\x9F\x9A\xB2", "bicycle"),
    SCHULTZ_EMOJI("\xF0\x9F\x9B\xB5", "motor scooter"),
    SCHULTZ_EMOJI("\xF0\x9F\x9A\x86", "train"),
    SCHULTZ_EMOJI("\xE2\x9B\xB5", "sailboat"),
    SCHULTZ_EMOJI("\xF0\x9F\x9A\x80", "rocket"),
    SCHULTZ_EMOJI("\xF0\x9F\x8F\xA0", "house building"),
    SCHULTZ_EMOJI("\xF0\x9F\x8F\xA2", "office building"),
    SCHULTZ_EMOJI("\xF0\x9F\x8F\xB0", "european castle"),
    SCHULTZ_EMOJI("\xF0\x9F\x8F\x96\xEF\xB8\x8F", "beach with umbrella"),
    SCHULTZ_EMOJI("\xF0\x9F\x97\xBA\xEF\xB8\x8F", "world map"),
    SCHULTZ_EMOJI("\xF0\x9F\x8C\x8D", "earth globe europe-africa"),
    SCHULTZ_EMOJI("\xF0\x9F\x97\xBC", "tokyo tower"),
    /* Activities */
    SCHULTZ_EMOJI("\xE2\x9A\xBD", "soccer ball"),
    SCHULTZ_EMOJI("\xF0\x9F\x8F\x80", "basketball and hoop"),
    SCHULTZ_EMOJI("\xF0\x9F\x8F\x88", "american football"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xBE", "tennis racquet and ball"),
    SCHULTZ_EMOJI("\xF0\x9F\x8F\x90", "volleyball"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xB1", "billiards"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xAE", "video game"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xB2", "game die"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xB8", "guitar"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xB9", "musical keyboard"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xA4", "microphone"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xA8", "artist palette"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xAC", "clapper board"),
    SCHULTZ_EMOJI("\xF0\x9F\x8F\x86", "trophy"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xAF", "direct hit"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xAA", "circus tent"),
    /* Objects */
    SCHULTZ_EMOJI("\xF0\x9F\x92\xA1", "electric light bulb"),
    SCHULTZ_EMOJI("\xF0\x9F\x94\xA6", "electric torch"),
    SCHULTZ_EMOJI("\xF0\x9F\x93\xB1", "mobile phone"),
    SCHULTZ_EMOJI("\xF0\x9F\x92\xBB", "personal computer"),
    SCHULTZ_EMOJI("\xF0\x9F\x96\xA5\xEF\xB8\x8F", "desktop computer"),
    SCHULTZ_EMOJI("\xF0\x9F\x93\xB7", "camera"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xA7", "headphone"),
    SCHULTZ_EMOJI("\xF0\x9F\x93\x9A", "books"),
    SCHULTZ_EMOJI("\xE2\x9C\x8F\xEF\xB8\x8F", "pencil"),
    SCHULTZ_EMOJI("\xF0\x9F\x93\x9D", "memo"),
    SCHULTZ_EMOJI("\xF0\x9F\x93\x8C", "pushpin"),
    SCHULTZ_EMOJI("\xF0\x9F\x93\x8E", "paperclip"),
    SCHULTZ_EMOJI("\xF0\x9F\x94\x92", "lock"),
    SCHULTZ_EMOJI("\xF0\x9F\x94\x91", "key"),
    SCHULTZ_EMOJI("\xF0\x9F\x94\xA7", "wrench"),
    SCHULTZ_EMOJI("\xF0\x9F\x94\xA8", "hammer"),
    SCHULTZ_EMOJI("\xE2\x8F\xB0", "alarm clock"),
    SCHULTZ_EMOJI("\xF0\x9F\x92\xB0", "money bag"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\x81", "wrapped present"),
    SCHULTZ_EMOJI("\xF0\x9F\x93\xA6", "package"),
    /* Symbols */
    SCHULTZ_EMOJI("\xE2\x9D\xA4\xEF\xB8\x8F", "heavy black heart"),
    SCHULTZ_EMOJI("\xF0\x9F\xA7\xA1", "orange heart"),
    SCHULTZ_EMOJI("\xF0\x9F\x92\x9B", "yellow heart"),
    SCHULTZ_EMOJI("\xF0\x9F\x92\x9A", "green heart"),
    SCHULTZ_EMOJI("\xF0\x9F\x92\x99", "blue heart"),
    SCHULTZ_EMOJI("\xF0\x9F\x92\x9C", "purple heart"),
    SCHULTZ_EMOJI("\xF0\x9F\x96\xA4", "black heart"),
    SCHULTZ_EMOJI("\xE2\x9C\x85", "white heavy check mark"),
    SCHULTZ_EMOJI("\xE2\x9D\x8C", "cross mark"),
    SCHULTZ_EMOJI("\xE2\x9A\xA0\xEF\xB8\x8F", "warning sign"),
    SCHULTZ_EMOJI("\xE2\x9D\x93", "black question mark ornament"),
    SCHULTZ_EMOJI("\xE2\x9D\x97", "heavy exclamation mark symbol"),
    SCHULTZ_EMOJI("\xF0\x9F\x92\xAF", "hundred points symbol"),
    SCHULTZ_EMOJI("\xF0\x9F\x94\x94", "bell"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\xB5", "musical note"),
    SCHULTZ_EMOJI("\xE2\x9E\x95", "heavy plus sign"),
    SCHULTZ_EMOJI("\xE2\x9E\x96", "heavy minus sign"),
    SCHULTZ_EMOJI("\xE2\x9C\xA8", "sparkles"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\x89", "party popper"),
    SCHULTZ_EMOJI("\xF0\x9F\x8E\x8A", "confetti ball")
};

/* The keys that sit under the emoji, which do not scroll with them. */
static const schultz_key schultz_emoji_controls[] = {
    SCHULTZ_SWITCHES("abc", SCHULTZ_KEYS_LETTERS, 2.0f, "letters"),
    SCHULTZ_SPACE(4.0f),
    SCHULTZ_SENDS("Back", SCHULTZ_KEY_BACKSPACE, 1.0f, "backspace"),
    SCHULTZ_SENDS("Enter", SCHULTZ_KEY_RETURN, 1.0f, "enter")
};

#define SCHULTZ_ROW(name) { name, (uint32_t)(sizeof(name) / sizeof(name[0])) }

static const schultz_key_row schultz_letters_rows[] = {
    SCHULTZ_ROW(schultz_letters_row0), SCHULTZ_ROW(schultz_letters_row1),
    SCHULTZ_ROW(schultz_letters_row2), SCHULTZ_ROW(schultz_letters_row3)
};
static const schultz_key_row schultz_shifted_rows[] = {
    SCHULTZ_ROW(schultz_shifted_row0), SCHULTZ_ROW(schultz_shifted_row1),
    SCHULTZ_ROW(schultz_shifted_row2), SCHULTZ_ROW(schultz_letters_row3)
};
static const schultz_key_row schultz_symbols_rows[] = {
    SCHULTZ_ROW(schultz_symbols_row0), SCHULTZ_ROW(schultz_symbols_row1),
    SCHULTZ_ROW(schultz_symbols_row2), SCHULTZ_ROW(schultz_symbols_row3)
};

/** @brief One set of keys, and how many rows it has. */
typedef struct {
    const schultz_key_row *rows;
    uint32_t               count;
} schultz_key_set;

/* The emoji set is not in here: it is rows of keys like the others only in
 * the bottom row, and a scrolling field of faces above it. */
static const schultz_key_set schultz_key_sets[] = {
    SCHULTZ_ROW(schultz_letters_rows), SCHULTZ_ROW(schultz_shifted_rows),
    SCHULTZ_ROW(schultz_symbols_rows)
};

/** @brief What a keyboard node holds. */
typedef struct {
    schultz_events *events;                   /**< Where presses are sent. */
    schultz_handle  sets[SCHULTZ_KEYS_SETS];  /**< One panel per set. */
    schultz_handle  emoji_keys[SCHULTZ_KEYS_SETS]; /**< Keys that open it. */
    uint32_t        emoji_key_count;          /**< How many there are. */
    uint32_t        set;                      /**< Which one is showing. */
    int32_t         caps;                     /**< Shift is held down. */
    int32_t         emoji;                    /**< The window offers them. */
    int32_t         field_emoji;              /**< This field accepts them. */
    uint32_t        input_type;               /**< What it was told it serves. */
} schultz_keyboard_data;

/** @brief What one key node holds. */
typedef struct {
    schultz_handle     keyboard; /**< The keyboard it belongs to. */
    const schultz_key *key;      /**< Its row in the table. */
    /**
     * Whether it acts when the finger lifts rather than when it lands.
     *
     * True of the emoji, which sit in a field that scrolls. A finger that
     * lands on one and then moves is swiping the faces along, and the router
     * already withholds the click when a finger has travelled, so waiting
     * for the click is what tells a tap from a swipe. A letter key does not
     * wait: nothing scrolls under it, and a keyboard that answered on the
     * release would feel half a beat behind every word.
     */
    int32_t            on_release;
    int32_t            acted;    /**< The press was already carried out. */
    int32_t            held;     /**< Whether it is down right now. */
    uint32_t           held_ms;  /**< How long it has been down. */
    uint32_t           next_ms;  /**< How long until it fires again. */
} schultz_key_data;

static const schultz_widget_vtable schultz_keyboard_widget;
static const schultz_widget_vtable schultz_key_widget;

static schultz_keyboard_data *schultz_keyboard_get(const schultz_tree *tree,
                                                   schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_keyboard_widget) {
        return NULL;
    }
    return (schultz_keyboard_data *)schultz_node_widget_data(tree, node);
}

/*
 * Shows one set of keys and hides the others.
 *
 * Hiding is what switching is: every set was built when the keyboard was, so
 * nothing is created or destroyed here, which matters because this runs from
 * inside the event handler of a key that belongs to the set being hidden.
 */
static void schultz_keyboard_show_set(schultz_tree *tree, schultz_handle node,
                                      uint32_t set)
{
    schultz_keyboard_data *board = schultz_keyboard_get(tree, node);
    uint32_t i;
    int32_t  changed;

    if (board == NULL || set >= SCHULTZ_KEYS_SETS) {
        return;
    }
    if (set == SCHULTZ_KEYS_EMOJI && !(board->emoji && board->field_emoji)) {
        return;
    }
    /*
     * Not an early return. A keyboard is made with its set already reading as
     * letters, and it is this call that first makes the letters visible, so
     * the loop below has to run even when the number has not moved.
     */
    changed = (board->set != set);
    board->set = set;
    for (i = 0u; i < SCHULTZ_KEYS_SETS; i++) {
        uint32_t state = schultz_node_get_state(tree, board->sets[i]);

        if (i == set) {
            state |= (uint32_t)SCHULTZ_STATE_VISIBLE;
        } else {
            state &= ~(uint32_t)SCHULTZ_STATE_VISIBLE;
        }
        schultz_node_set_state(tree, board->sets[i], state);
    }
    if (changed) {
        schultz_node_invalidate_layout(tree, node);
    }
}

/*
 * Carries out one key, whether it was pressed or is repeating.
 *
 * Text and key codes go out through the router's own entry points, so a field
 * sees exactly what a real keyboard would produce. A key that switches sets
 * sends nothing at all.
 */
static void schultz_key_act(schultz_tree *tree, schultz_handle node)
{
    schultz_key_data *data =
        (schultz_key_data *)schultz_node_widget_data(tree, node);
    schultz_keyboard_data *board;

    if (data == NULL || data->key == NULL) {
        return;
    }
    board = schultz_keyboard_get(tree, data->keyboard);
    if (board == NULL || board->events == NULL) {
        return;
    }
    if (data->key->goes_to == SCHULTZ_KEYS_SHIFTED) {
        /*
         * Once for one capital, twice to hold the capitals down, and a third
         * time to let them go. Which of the three it is depends on what is
         * already showing, which is the only way to tell one press from two
         * without a clock.
         */
        if (board->set != SCHULTZ_KEYS_SHIFTED) {
            board->caps = 0;
            schultz_keyboard_show_set(tree, data->keyboard,
                                      SCHULTZ_KEYS_SHIFTED);
        } else if (!board->caps) {
            board->caps = 1;
        } else {
            board->caps = 0;
            schultz_keyboard_show_set(tree, data->keyboard,
                                      SCHULTZ_KEYS_LETTERS);
        }
        return;
    }
    if (data->key->goes_to < SCHULTZ_KEYS_SETS) {
        if (data->key->goes_to == SCHULTZ_KEYS_LETTERS) {
            board->caps = 0;
        }
        schultz_keyboard_show_set(tree, data->keyboard, data->key->goes_to);
        return;
    }
    if (data->key->key != 0u) {
        schultz_events_key(board->events, data->key->key, 0u, 1);
        schultz_events_key(board->events, data->key->key, 0u, 0);
        return;
    }
    if (data->key->sends != NULL) {
        schultz_events_text_input(board->events, data->key->sends);
        /*
         * One capital and then back to lower case, which is what shift means
         * everywhere. Held down with a second press, it stays.
         */
        if (board->set == SCHULTZ_KEYS_SHIFTED && !board->caps) {
            schultz_keyboard_show_set(tree, data->keyboard,
                                      SCHULTZ_KEYS_LETTERS);
        }
    }
}

/*
 * A held key repeats, which is what backspace is for.
 *
 * Only keys that ask for it are ticked at all, and a key stops asking the
 * moment it comes up, so a keyboard sitting still costs nothing.
 */
static int32_t schultz_key_tick(schultz_tree *tree, schultz_handle node,
                                uint64_t now_ms, uint32_t elapsed_ms)
{
    schultz_key_data *data =
        (schultz_key_data *)schultz_node_widget_data(tree, node);

    (void)now_ms;
    if (data == NULL || !data->held) {
        return 0;
    }
    /* How long it has been down, rather than what time it is: an event does
     * not carry a clock and this does not need one. */
    data->held_ms += elapsed_ms;
    if (data->held_ms < data->next_ms) {
        return 0;
    }
    data->next_ms = data->held_ms + SCHULTZ_KEY_REPEAT_EVERY_MS;
    schultz_key_act(tree, node);
    return 0;
}

static int32_t schultz_key_event(schultz_tree *tree, schultz_handle node,
                                 const schultz_event *event)
{
    schultz_key_data *data =
        (schultz_key_data *)schultz_node_widget_data(tree, node);

    if (data == NULL) {
        return 0;
    }
    switch (event->type) {
    case SCHULTZ_EVENT_MOUSE_DOWN:
        /*
         * On the way down rather than on the click, except where something
         * scrolls underneath. A keyboard that waited for the release would
         * feel half a beat behind every letter, and a key that repeats has
         * to start somewhere.
         */
        if (data->on_release) {
            /*
             * Not consumed. An event that is taken stops there, and the view
             * that scrolls these keys is an ancestor: it has to see the
             * finger land, or it has no place to measure a drag from and
             * swiping does nothing at all. The press still enters nothing,
             * because this key acts on the click.
             */
            return 0;
        }
        schultz_key_act(tree, node);
        data->acted = 1;
        if (data->key != NULL && data->key->key == SCHULTZ_KEY_BACKSPACE) {
            data->held    = 1;
            data->held_ms = 0u;
            data->next_ms = SCHULTZ_KEY_REPEAT_DELAY_MS;
            schultz_node_set_animating(tree, node, 1);
        }
        return SCHULTZ_EVENT_CONSUMED;
    case SCHULTZ_EVENT_MOUSE_UP:
        data->held = 0;
        schultz_node_set_animating(tree, node, 0);
        /* The same again: letting go is where a flick begins, and the view
         * above has to hear about it. */
        return data->on_release ? 0 : SCHULTZ_EVENT_CONSUMED;
    case SCHULTZ_EVENT_CLICK:
        /*
         * A press was carried out on the way down, so this one is the same
         * press arriving again and is dropped. A click that arrives without
         * a press before it came from somewhere else -- a screen reader
         * activating the key, or the router's own space and enter -- and is
         * the only signal that key will ever get, so it is carried out here.
         */
        if (data->acted) {
            data->acted = 0;
        } else {
            schultz_key_act(tree, node);
        }
        return SCHULTZ_EVENT_CONSUMED;
    default:
        break;
    }
    return 0;
}

static const schultz_widget_vtable schultz_key_widget = {
    .paint = schultz_button_paint, /* a key is a button that acts locally */
    .event = schultz_key_event,
    .tick = schultz_key_tick,
    .destroy = free
};

/*
 * The keyboard's own surface, under the keys.
 *
 * Without it the keyboard is a set of keys with the page showing through the
 * gaps between them: a background is a style property, and something has to
 * paint it. A node with a style and no widget draws nothing at all.
 */
static int32_t schultz_keyboard_paint(schultz_tree *tree, schultz_handle node,
                                      schultz_draw_list *list,
                                      schultz_arena *arena)
{
    const schultz_resolved_style *style = schultz_widget_style(tree, node);
    schultz_rect bounds;

    (void)arena;
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    return schultz_widget_draw_box(list, style, bounds);
}

static const schultz_widget_vtable schultz_keyboard_widget = {
    .paint = schultz_keyboard_paint,
    .destroy = free
};

/* One key: a button, with the button's own look, that acts here instead of
 * telling the host. */
static int32_t schultz_key_create(schultz_tree *tree, schultz_handle row,
                                  schultz_handle keyboard,
                                  const schultz_key *key, uint32_t column,
                                  schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_key_data *data;
    schultz_layout_params params;
    int32_t result = schultz_button_create(tree, row, key->cap, &node);

    if (result != SCHULTZ_OK) {
        return result;
    }
    data = (schultz_key_data *)calloc(1u, sizeof(*data));
    if (data == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    data->keyboard = keyboard;
    data->key      = key;
    schultz_node_set_widget(tree, node, &schultz_key_widget, data);
    /*
     * Pressable but not focusable. Focus belongs to the field being edited,
     * and a key that took it would end the edit it is there to serve.
     */
    schultz_node_set_actions(tree, node, SCHULTZ_ACTION_CLICK);
    /*
     * And a press on it leaves focus where it is. Without this the router
     * clears focus before the key is even told about the press, because a
     * press decides focus and a key is not focusable: the field would lose
     * it, the letter would arrive nowhere, and the keyboard would go away
     * because there was no longer anything being edited.
     */
    schultz_node_set_keeps_focus(tree, node, 1);
    if (key->name != NULL) {
        schultz_node_set_name(tree, node, key->name);
    }
    schultz_layout_params_default(&params);
    params.column = column;
    params.row    = 0u;
    *out_node = node;
    return schultz_node_set_layout_params(tree, node, &params);
}

static int32_t schultz_key_row_create(schultz_tree *tree, schultz_handle set,
                                      schultz_handle keyboard,
                                      const schultz_key_row *row,
                                      schultz_keyboard_data *board)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_grid_track tracks[16];
    schultz_layout_params params;
    uint32_t count = row->count;
    uint32_t i;
    int32_t result;

    if (count > (uint32_t)(sizeof(tracks) / sizeof(tracks[0]))) {
        count = (uint32_t)(sizeof(tracks) / sizeof(tracks[0]));
    }
    result = schultz_panel_create(tree, set, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /*
     * A grid rather than a box, because the keys are not the same width and
     * a weighted track is the only thing that says "this one is four keys
     * wide" and keeps saying it at every window size.
     */
    schultz_node_set_pane(tree, node, schultz_pane_grid());
    for (i = 0u; i < count; i++) {
        tracks[i].kind  = SCHULTZ_TRACK_WEIGHTED;
        tracks[i].value = row->keys[i].width;
    }
    result = schultz_node_set_grid_tracks(tree, node, tracks, count, NULL, 0u);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /* Across rather than down: a finger lands on a key's width and misses
     * between them, and the rows have the keyboard's height to share. */
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_GAP,
        schultz_value_token(SCHULTZ_TOKEN_SPACE_SM));
    schultz_layout_params_default(&params);
    params.grow = SCHULTZ_GROW_ALWAYS;
    schultz_node_set_layout_params(tree, node, &params);

    for (i = 0u; i < count; i++) {
        schultz_handle key = SCHULTZ_HANDLE_NONE;

        result = schultz_key_create(tree, node, keyboard, &row->keys[i], i,
                                    &key);
        if (result != SCHULTZ_OK) {
            return result;
        }
        /* The keys that open the emoji set are noted on the way past, so
         * that switching emoji off can hide them without hunting. */
        if (row->keys[i].goes_to == SCHULTZ_KEYS_EMOJI &&
            board->emoji_key_count <
                (uint32_t)(sizeof(board->emoji_keys) /
                           sizeof(board->emoji_keys[0]))) {
            board->emoji_keys[board->emoji_key_count] = key;
            board->emoji_key_count++;
        }
    }
    return SCHULTZ_OK;
}

/*
 * How wide one emoji key is, and how many rows of them there are.
 *
 * Half the width of a letter key, near enough: a face reads at a glance and
 * twice as many fit in the same band. Three rows leaves the keyboard about
 * the height it is on the letters, so it does not jump when the faces come
 * up.
 */
#define SCHULTZ_EMOJI_KEY_WIDTH 46.0f
#define SCHULTZ_EMOJI_ROWS 3u

static const schultz_key_row schultz_emoji_control_row =
    SCHULTZ_ROW(schultz_emoji_controls);

/*
 * The emoji set: a field of faces that scrolls sideways, over a row of keys
 * that does not.
 *
 * There are far more emoji than fit across a screen, so they are put in a
 * scroll view and a finger drags them along. The view knows the difference
 * between a finger that stays still and one that moves, so a tap still
 * enters the face it landed on and only a drag scrolls.
 *
 * Three to a column, filled top to bottom and then along, so that dragging
 * to the right walks the list in its own order and a group stays together
 * rather than being spread across three distant rows.
 */
static int32_t schultz_emoji_set_create(schultz_tree *tree,
                                        schultz_handle keyboard,
                                        schultz_keyboard_data *board,
                                        schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle content = SCHULTZ_HANDLE_NONE;
    schultz_handle column = SCHULTZ_HANDLE_NONE;
    schultz_layout_params params;
    uint32_t count = (uint32_t)(sizeof(schultz_emoji_keys) /
                                sizeof(schultz_emoji_keys[0]));
    uint32_t i;
    int32_t result = schultz_panel_create(tree, keyboard, &node);

    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_set_pane(tree, node, schultz_pane_vbox());
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_GAP,
        schultz_value_token(SCHULTZ_TOKEN_SPACE_SM));

    result = schultz_scroll_view_create(tree, node, &view);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_layout_params_default(&params);
    params.grow = SCHULTZ_GROW_ALWAYS;
    schultz_node_set_layout_params(tree, view, &params);
    /*
     * No bars. A finger drags the faces along, and a bar is a strip of
     * height taken from three rows that have little to spare for something
     * nobody aims at on a touch panel.
     */
    schultz_scroll_view_set_bars(tree, view, 0);
    /*
     * And a cursor drags it as a finger does. There is nothing to select
     * among the faces, dragging is the whole gesture, and it is the only way
     * to try the thing on a machine with a mouse.
     */
    schultz_scroll_view_set_drag_scrolls(tree, view, 1);
    content = schultz_scroll_view_content(tree, view);
    schultz_node_set_pane(tree, content, schultz_pane_hbox());
    schultz_node_set_style_property(tree, content, SCHULTZ_PROP_GAP,
        schultz_value_token(SCHULTZ_TOKEN_SPACE_SM));

    for (i = 0u; i < count; i++) {
        schultz_handle key = SCHULTZ_HANDLE_NONE;

        if ((i % SCHULTZ_EMOJI_ROWS) == 0u) {
            result = schultz_panel_create(tree, content, &column);
            if (result != SCHULTZ_OK) {
                return result;
            }
            schultz_node_set_pane(tree, column, schultz_pane_vbox());
            schultz_node_set_style_property(tree, column, SCHULTZ_PROP_GAP,
                schultz_value_token(SCHULTZ_TOKEN_SPACE_SM));
        }
        result = schultz_key_create(tree, column, keyboard,
                                    &schultz_emoji_keys[i], 0u, &key);
        if (result != SCHULTZ_OK) {
            return result;
        }
        schultz_node_set_pref_size(tree, key, SCHULTZ_EMOJI_KEY_WIDTH,
                                   SCHULTZ_SIZE_UNSET);
        /*
         * These wait for the finger to lift. Pressing one and moving is how
         * the faces are swiped along, and a face entered on the way down
         * would arrive every time somebody scrolled.
         */
        {
            schultz_key_data *data =
                (schultz_key_data *)schultz_node_widget_data(tree, key);

            if (data != NULL) {
                data->on_release = 1;
            }
        }
    }

    result = schultz_key_row_create(tree, node, keyboard,
                                    &schultz_emoji_control_row, board);
    if (result != SCHULTZ_OK) {
        return result;
    }
    *out_node = node;
    return SCHULTZ_OK;
}

static int32_t schultz_key_set_create(schultz_tree *tree,
                                      schultz_handle keyboard,
                                      const schultz_key_set *set,
                                      schultz_keyboard_data *board,
                                      schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t i;
    int32_t result = schultz_panel_create(tree, keyboard, &node);

    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_set_pane(tree, node, schultz_pane_vbox());
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_GAP,
        schultz_value_token(SCHULTZ_TOKEN_SPACE_SM));
    for (i = 0u; i < set->count; i++) {
        result = schultz_key_row_create(tree, node, keyboard, &set->rows[i],
                                        board);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_keyboard_create(schultz_tree *tree, schultz_handle parent,
                                schultz_events *events,
                                schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_keyboard_data *board;
    schultz_patch patch;
    uint32_t i;
    int32_t result;

    if (out_node == NULL || events == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    board = (schultz_keyboard_data *)calloc(1u, sizeof(*board));
    if (board == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    board->events      = events;
    board->emoji       = 1;
    board->field_emoji = 1;
    /* The sets share one rectangle and one of them is visible, which is what
     * a stack is for. */
    schultz_node_set_pane(tree, node, schultz_pane_stack());
    schultz_node_set_widget(tree, node, &schultz_keyboard_widget, board);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_GROUP);
    schultz_node_set_name(tree, node, "on-screen keyboard");
    /*
     * The whole of it, not only the keys. A finger that lands in the gap
     * between two keys has pressed the keyboard, and that must leave the
     * edit alone exactly as a key does.
     */
    schultz_node_set_keeps_focus(tree, node, 1);
    if (schultz_patch_init(&patch) == SCHULTZ_OK) {
        schultz_patch_token(&patch, SCHULTZ_PROP_BACKGROUND,
                            SCHULTZ_TOKEN_COLOR_SURFACE);
        schultz_patch_token(&patch, SCHULTZ_PROP_PADDING,
                            SCHULTZ_TOKEN_SPACE_SM);
        schultz_widget_default_style(tree, node, &patch);
    }

    for (i = 0u; i < SCHULTZ_KEYS_SETS; i++) {
        result = (i == SCHULTZ_KEYS_EMOJI)
                     ? schultz_emoji_set_create(tree, node, board,
                                                &board->sets[i])
                     : schultz_key_set_create(tree, node,
                                              &schultz_key_sets[i], board,
                                              &board->sets[i]);
        if (result != SCHULTZ_OK) {
            schultz_node_destroy(tree, node);
            return result;
        }
    }
    schultz_keyboard_show_set(tree, node, SCHULTZ_KEYS_LETTERS);
    *out_node = node;
    return SCHULTZ_OK;
}

/*
 * Applies whatever the window and the field between them have decided.
 *
 * Two answers rather than one because they are two different questions: an
 * application says whether this window offers emoji at all, and a field says
 * whether it will take them. Either one saying no is a no, and neither may
 * overwrite the other.
 */
static void schultz_keyboard_apply_emoji(schultz_tree *tree,
                                         schultz_handle node)
{
    schultz_keyboard_data *board = schultz_keyboard_get(tree, node);
    int32_t offered;
    uint32_t i;

    if (board == NULL) {
        return;
    }
    offered = (board->emoji && board->field_emoji) ? 1 : 0;
    /*
     * Showing the emoji set with emoji switched off would leave the only way
     * back sitting inside the set nobody may open, so the letters come back
     * first.
     */
    if (!offered && board->set == SCHULTZ_KEYS_EMOJI) {
        schultz_keyboard_show_set(tree, node, SCHULTZ_KEYS_LETTERS);
    }
    /*
     * The key that opens it goes with it. Layout skips a child that is not
     * visible, so the row closes up rather than leaving a hole.
     */
    for (i = 0u; i < board->emoji_key_count; i++) {
        uint32_t state = schultz_node_get_state(tree, board->emoji_keys[i]);

        if (offered) {
            state |= (uint32_t)SCHULTZ_STATE_VISIBLE;
        } else {
            state &= ~(uint32_t)SCHULTZ_STATE_VISIBLE;
        }
        schultz_node_set_state(tree, board->emoji_keys[i], state);
    }
}

int32_t schultz_keyboard_set_emoji(schultz_tree *tree, schultz_handle node,
                                   int32_t offered)
{
    schultz_keyboard_data *board = schultz_keyboard_get(tree, node);

    if (board == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    board->emoji = offered ? 1 : 0;
    schultz_keyboard_apply_emoji(tree, node);
    return SCHULTZ_OK;
}

int32_t schultz_keyboard_set_input_type(schultz_tree *tree,
                                        schultz_handle node, uint32_t type)
{
    schultz_keyboard_data *board = schultz_keyboard_get(tree, node);

    if (board == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (type > SCHULTZ_INPUT_PASSWORD) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * Neither a number nor a password wants a page of faces: one cannot hold
     * them and the other should not carry them.
     */
    board->input_type  = type;
    board->field_emoji = (type == SCHULTZ_INPUT_NUMBER ||
                          type == SCHULTZ_INPUT_PASSWORD) ? 0 : 1;
    schultz_keyboard_apply_emoji(tree, node);
    /* A field for digits opens on the digits. Anything else starts where a
     * sentence starts. */
    schultz_keyboard_show_set(tree, node, (type == SCHULTZ_INPUT_NUMBER)
                                              ? SCHULTZ_KEYS_SYMBOLS
                                              : SCHULTZ_KEYS_LETTERS);
    return SCHULTZ_OK;
}

uint32_t schultz_keyboard_input_type(const schultz_tree *tree,
                                    schultz_handle node)
{
    const schultz_keyboard_data *board = schultz_keyboard_get(tree, node);

    return (board == NULL) ? (uint32_t)SCHULTZ_INPUT_TEXT : board->input_type;
}

int32_t schultz_keyboard_offers_emoji(const schultz_tree *tree,
                                      schultz_handle node)
{
    const schultz_keyboard_data *board = schultz_keyboard_get(tree, node);

    return (board == NULL) ? 0 : (board->emoji && board->field_emoji);
}
