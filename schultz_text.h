/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_text.h
 * @brief Shaping, measurement, and line breaking.
 *
 * Internal header.
 *
 * Shaping turns a string plus a font into positioned glyph indices, handling
 * kerning, ligatures, and scripts where a character's shape depends on its
 * neighbours. HarfBuzz does that work; this module owns the buffers, the
 * coordinate conversion, and the arena the results live in.
 *
 * Measurement is the seam layout depends on. Because a pane asks for a
 * preferred size given an available width, and text height depends on the
 * width it is given, every measurement entry point takes an available width.
 *
 * **Not part of the host facing interface.** Shaping and wrapping, which the
 * text widgets do for a host. A host binds to schultz_api.h; this header is
 * the toolkit's own and may change without notice.
 */

#ifndef SCHULTZ_TEXT_H
#define SCHULTZ_TEXT_H

#include "schultz_arena.h"
#include "schultz_font.h"
#include "schultz_paint.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reading direction of a run of text. */
enum {
    SCHULTZ_DIR_AUTO = 0, /**< Infer from the text's own characters. */
    SCHULTZ_DIR_LTR,      /**< Force left to right. */
    SCHULTZ_DIR_RTL       /**< Force right to left. */
};

/**
 * @brief One shaped run: glyphs ready to draw, plus its extent.
 *
 * The glyphs are allocated from the arena passed to the shaping call, so they
 * live until that arena is reset. Positions are relative to the run's origin,
 * which sits on the baseline at the leading edge.
 */
typedef struct {
    const schultz_glyph *glyphs;    /**< Positioned glyphs, in visual order. */
    /**
     * Byte offset into the source string each glyph came from, one per
     * glyph. Several glyphs may share one offset when a character shapes into
     * more than one, and one glyph may cover several bytes when characters
     * combine. This is what a caret is placed from.
     */
    const uint32_t      *clusters;
    /**
     * The face each glyph came from, one per glyph. Usually every entry is
     * the font that was asked for; it differs where the string held
     * something that face could not draw and the emoji face supplied it. A
     * painter draws consecutive glyphs sharing a face as one run.
     */
    const schultz_handle *fonts;
    uint32_t             count;     /**< Number of glyphs. */
    float                width;     /**< Total advance, in pixels. */
    float                height;    /**< Line height of the font used. */
    float                ascent;    /**< Baseline offset from the run's top. */
    int32_t              direction; /**< Resolved SCHULTZ_DIR_LTR or _RTL. */
} schultz_text_run;

/**
 * @brief One stretch of a string set in one face.
 *
 * What the text layer needs to know about rich text, and the whole of it.
 * A span in a widget also carries a colour, an underline and a link; none of
 * those change where a glyph lands, so none of them reach this layer. What
 * does reach it is the face, because the face decides the glyphs and their
 * advances, and therefore the wrapping and the caret.
 *
 * Size travels with the face rather than beside it: a font handle names a
 * face and a pixel size together, which is what FreeType and HarfBuzz both
 * want. So a phrase at another size is another piece, with the handle
 * schultz_font_at_size gave back.
 *
 * Offsets are byte offsets into the string handed to the same call. Pieces
 * are expected in order and not to overlap. A byte no piece covers is drawn
 * in the call's own font, so a caller may describe only the parts that
 * differ, and so does a piece whose font is SCHULTZ_HANDLE_NONE, for a piece
 * that is there to say something other than which face to use.
 */
typedef struct {
    uint32_t       start; /**< First byte. */
    uint32_t       end;   /**< One past the last byte. */
    schultz_handle font;  /**< The face this stretch is set in. */
} schultz_text_piece;

/**
 * @brief Shapes a UTF-8 string into positioned glyphs.
 *
 * The string is treated as a single run: no line breaking and no
 * bidirectional reordering across direction changes. Use
 * schultz_text_wrap for text that must fit a width.
 *
 * @param system    The font system holding the font. Must not be NULL.
 * @param font      The font to shape with.
 * @param utf8      The text, UTF-8 encoded. Must not be NULL.
 * @param length    Bytes to read, or -1 to read to the NUL terminator.
 * @param direction One of SCHULTZ_DIR_AUTO, SCHULTZ_DIR_LTR, SCHULTZ_DIR_RTL.
 * @param arena     Where the glyph array is allocated. Must not be NULL, and
 *                  must outlive any use of the returned run.
 * @param out_run   Receives the shaped run. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad argument,
 *         SCHULTZ_ERR_INVALID_HANDLE when the font is not loaded, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when the arena could not supply space.
 */
int32_t schultz_text_shape(const schultz_font_system *system,
                           schultz_handle font, const char *utf8,
                           int32_t length, int32_t direction,
                           schultz_arena *arena, schultz_text_run *out_run);

/**
 * @brief Shapes a string whose face changes partway through.
 *
 * What schultz_text_shape does, for text that is not all one face: a bold
 * phrase inside a sentence, a term at another size, a word set in the code
 * face. Each piece is shaped in its own face and the results are laid down
 * end to end on one line, so the run that comes back is one run and every
 * caller of a run works on it unchanged.
 *
 * Shaping stops at a piece boundary, which is what a change of face means:
 * two faces have no kerning pair in common and no ligature that spans them.
 * Within a piece nothing changes, so a sentence with one bold word in it
 * kerns exactly as it did on either side of that word.
 *
 * The run's height and ascent are the tallest of the faces on it, so a line
 * carrying one large word is as tall as that word and the rest of it sits on
 * the same baseline.
 *
 * @param pieces      Which face each stretch is set in. May be NULL, which
 *                    means the whole string in `font`.
 * @param piece_count How many pieces. Zero means the same as NULL.
 * @param font        The face for any byte no piece covers. May be
 *                    SCHULTZ_HANDLE_NONE only when the pieces cover
 *                    everything.
 * @param system      The font system holding the faces. Must not be NULL.
 * @param utf8        The text, UTF-8 encoded. Must not be NULL.
 * @param length      Bytes to read, or -1 to read to the NUL terminator.
 * @param direction   One of SCHULTZ_DIR_AUTO, SCHULTZ_DIR_LTR, _RTL.
 * @param arena       Where the glyph array is allocated. Must not be NULL.
 * @param out_run     Receives the shaped run. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad argument,
 *         SCHULTZ_ERR_INVALID_HANDLE when a face is not loaded, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_text_shape_pieces(const schultz_font_system *system,
                                  const schultz_text_piece *pieces,
                                  uint32_t piece_count, schultz_handle font,
                                  const char *utf8, int32_t length,
                                  int32_t direction, schultz_arena *arena,
                                  schultz_text_run *out_run);

/**
 * @brief Measures a string without keeping its glyphs.
 *
 * Shapes into a scratch arena and reports only the extent, which is what a
 * layout pass needs. Cheaper to call than schultz_text_shape when the glyphs
 * are going to be thrown away.
 *
 * @param system   The font system holding the font. Must not be NULL.
 * @param font     The font to measure with.
 * @param utf8     The text, UTF-8 encoded. Must not be NULL.
 * @param length   Bytes to read, or -1 to read to the NUL terminator.
 * @param arena    Scratch space. Must not be NULL. Nothing in it is retained.
 * @param out_size Receives the width and line height. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad argument,
 *         SCHULTZ_ERR_INVALID_HANDLE when the font is not loaded, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_text_measure(const schultz_font_system *system,
                             schultz_handle font, const char *utf8,
                             int32_t length, schultz_arena *arena,
                             schultz_size *out_size);

/**
 * @brief Measures a string whose face changes partway through.
 *
 * What schultz_text_measure does, for pieces. The width is the pieces laid
 * end to end; the height is the tallest face among them, which is what the
 * line has to be to hold all of it.
 *
 * @param pieces      Which face each stretch is set in, or NULL for one face.
 * @param piece_count How many pieces. Zero means the same as NULL.
 * @param font        The face for any byte no piece covers.
 * @param system      The font system holding the faces. Must not be NULL.
 * @param utf8        The text, UTF-8 encoded. Must not be NULL.
 * @param length      Bytes to read, or -1 to read to the NUL terminator.
 * @param arena       Scratch space. Must not be NULL. Nothing is retained.
 * @param out_size    Receives the width and line height. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad argument,
 *         SCHULTZ_ERR_INVALID_HANDLE when a face is not loaded, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_text_measure_pieces(const schultz_font_system *system,
                                    const schultz_text_piece *pieces,
                                    uint32_t piece_count, schultz_handle font,
                                    const char *utf8, int32_t length,
                                    schultz_arena *arena,
                                    schultz_size *out_size);

/**
 * @brief The next place the caret may sit, moving forward.
 *
 * A caret steps over whole characters as a person sees them, which is not the
 * same as one codepoint: an emoji with a skin tone is two, and a family is
 * five joined together. Unicode calls one of these a grapheme cluster, and
 * this is where the next one starts.
 *
 * @param utf8   The text. NULL yields `length`.
 * @param length Bytes of text.
 * @param at     A byte offset in the text.
 * @return The next boundary at or after `at`, or `length` at the end.
 */
uint32_t schultz_text_next_cluster(const char *utf8, uint32_t length,
                                   uint32_t at);

/**
 * @brief The previous place the caret may sit.
 *
 * @param utf8   The text. NULL yields zero.
 * @param length Bytes of text.
 * @param at     A byte offset in the text.
 * @return The boundary before `at`, or zero at the start.
 */
uint32_t schultz_text_prev_cluster(const char *utf8, uint32_t length,
                                   uint32_t at);

/**
 * @brief One line produced by wrapping.
 *
 * Offsets are byte offsets into the original string, so a caller can shape or
 * highlight the line without copying it.
 */
typedef struct {
    uint32_t start; /**< Byte offset of the first byte on the line. */
    uint32_t end;   /**< Byte offset one past the last byte, excluding the
                     *   break character itself. */
    float    width; /**< Measured width of the line, in pixels. */
    /**
     * Line height of the tallest face on this line. Measuring already had to
     * shape the line to learn its width, so this costs nothing and saves a
     * caller shaping it again only to find out how tall it is. With one face
     * throughout it is that face's line height, every line the same.
     */
    float    height;
} schultz_text_line;

/**
 * @brief Breaks text into lines that fit a width.
 *
 * Break opportunities come from libunibreak, which implements the Unicode line
 * breaking algorithm, so wrapping respects the rules of the script rather than
 * splitting on spaces.
 *
 * A run with no break opportunity in it that is still wider than the line is
 * broken where it stops fitting, at a character boundary. That is a last
 * resort and never happens to ordinary prose, but a run left to overflow
 * leaves the box it was given and cannot be read at all, which is worse than
 * an awkward break.
 *
 * @param system         The font system holding the font. Must not be NULL.
 * @param font           The font to measure with.
 * @param utf8           The text, UTF-8 encoded. Must not be NULL.
 * @param length         Bytes to read, or -1 to read to the NUL terminator.
 * @param available_width Width to fit, in pixels. Pass a negative value
 *                       for unbounded, which produces one line per hard break.
 * @param arena          Where the line array is allocated. Must not be NULL.
 * @param out_lines      Receives the line array. Must not be NULL.
 * @param out_count      Receives the number of lines. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad argument,
 *         SCHULTZ_ERR_INVALID_HANDLE when the font is not loaded, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_text_wrap(const schultz_font_system *system,
                          schultz_handle font, const char *utf8,
                          int32_t length, float available_width,
                          schultz_arena *arena,
                          const schultz_text_line **out_lines,
                          uint32_t *out_count);

/**
 * @brief Breaks text whose face changes partway through into lines.
 *
 * What schultz_text_wrap does, for pieces. Break opportunities come from the
 * same place and do not depend on the faces: where a line may be broken is a
 * property of the writing, not of how it is set. What the pieces change is
 * how wide the text before each opportunity turns out to be, so a phrase
 * turning bold can push the break earlier.
 *
 * A piece boundary is not itself a break opportunity. A word half of which
 * is bold is still one word and still breaks only where the word does.
 *
 * @param pieces          Which face each stretch is set in, or NULL.
 * @param piece_count     How many pieces. Zero means the same as NULL.
 * @param font            The face for any byte no piece covers.
 * @param system          The font system holding the faces. Must not be NULL.
 * @param utf8            The text, UTF-8 encoded. Must not be NULL.
 * @param length          Bytes to read, or -1 to read to the terminator.
 * @param available_width Width to fit, in pixels, or negative for unbounded.
 * @param arena           Where the line array is allocated. Must not be NULL.
 * @param out_lines       Receives the line array. Must not be NULL.
 * @param out_count       Receives the number of lines. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad argument,
 *         SCHULTZ_ERR_INVALID_HANDLE when a face is not loaded, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_text_wrap_pieces(const schultz_font_system *system,
                                 const schultz_text_piece *pieces,
                                 uint32_t piece_count, schultz_handle font,
                                 const char *utf8, int32_t length,
                                 float available_width, schultz_arena *arena,
                                 const schultz_text_line **out_lines,
                                 uint32_t *out_count);

/**
 * @brief Reports the dominant reading direction of a string.
 *
 * Runs the Unicode bidirectional algorithm over the text and returns the
 * paragraph level's direction. Text with no strongly directional characters
 * reports left to right.
 *
 * @param utf8   The text, UTF-8 encoded. Must not be NULL.
 * @param length Bytes to read, or -1 to read to the NUL terminator.
 * @return SCHULTZ_DIR_LTR or SCHULTZ_DIR_RTL. A NULL or empty string reports
 *         SCHULTZ_DIR_LTR.
 */
int32_t schultz_text_base_direction(const char *utf8, int32_t length);

/**
 * @brief Where a caret sits, in pixels from the run's leading edge.
 *
 * A caret is placed between characters, so an offset naming the start of a
 * glyph puts the caret at that glyph's leading edge, and an offset past every
 * glyph puts it at the end of the run. An offset inside a glyph's bytes, such
 * as the middle of a combining sequence, is treated as that glyph's start.
 *
 * @param run    A run shaped from the same string. Must not be NULL.
 * @param offset A byte offset into the string the run was shaped from.
 * @return The x position of the caret.
 */
float schultz_text_caret_x(const schultz_text_run *run, uint32_t offset);

/**
 * @brief The byte offset a caret lands on for a position along a run.
 *
 * The inverse of schultz_text_caret_x, rounded to the nearest boundary
 * between characters rather than truncated, which is what clicking in the
 * middle of a letter should do.
 *
 * @param run    A run shaped from the string. Must not be NULL.
 * @param x      A position from the run's leading edge, in pixels.
 * @param length Byte length of the string the run was shaped from.
 * @return The byte offset, between 0 and length.
 */
uint32_t schultz_text_caret_offset(const schultz_text_run *run, float x,
                                   uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_TEXT_H */
