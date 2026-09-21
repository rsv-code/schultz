/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_text.c
 * @brief Shaping, measurement, and line breaking.
 */

#include "schultz_text.h"

#include <stdlib.h>
#include <string.h>

#include <hb.h>

#include <emojidef.h>
#include <graphemebreak.h>
#include <linebreak.h>

#include <SheenBidi/SBAlgorithm.h>
#include <SheenBidi/SBCodepointSequence.h>
#include <SheenBidi/SBParagraph.h>

#include "schultz_font_internal.h"

/*
 * libunibreak keeps global tables that must be built once. C11 has no
 * portable one-time initializer that works without threads support, and the
 * threading rule puts all of this on the UI thread anyway, so a plain flag is
 * both correct here and honest about the assumption.
 */
static int schultz_text_linebreak_ready;

static void schultz_text_init_linebreak(void)
{
    if (!schultz_text_linebreak_ready) {
        init_linebreak();
        schultz_text_linebreak_ready = 1;
    }
}

/* Resolves length -1 to strlen, and rejects a negative length otherwise. */
static int32_t schultz_text_length(const char *utf8, int32_t length,
                                   size_t *out_length)
{
    if (utf8 == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (length < 0) {
        if (length != -1) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
        *out_length = strlen(utf8);
    } else {
        *out_length = (size_t)length;
    }
    return SCHULTZ_OK;
}

int32_t schultz_text_base_direction(const char *utf8, int32_t length)
{
    SBCodepointSequence sequence;
    SBAlgorithmRef algorithm;
    SBParagraphRef paragraph;
    SBLevel level;
    size_t bytes;

    if (schultz_text_length(utf8, length, &bytes) != SCHULTZ_OK ||
        bytes == 0) {
        return SCHULTZ_DIR_LTR;
    }

    sequence.stringEncoding = SBStringEncodingUTF8;
    sequence.stringBuffer   = (void *)utf8;
    sequence.stringLength   = bytes;

    algorithm = SBAlgorithmCreate(&sequence);
    if (algorithm == NULL) {
        return SCHULTZ_DIR_LTR;
    }

    /*
     * SBLevelDefaultLTR asks the algorithm to derive the paragraph direction
     * from the first strong character, falling back to left to right when
     * there is none. That is rule P2/P3 of the bidirectional algorithm.
     */
    paragraph = SBAlgorithmCreateParagraph(algorithm, 0, bytes,
                                           SBLevelDefaultLTR);
    if (paragraph == NULL) {
        SBAlgorithmRelease(algorithm);
        return SCHULTZ_DIR_LTR;
    }

    level = SBParagraphGetBaseLevel(paragraph);
    SBParagraphRelease(paragraph);
    SBAlgorithmRelease(algorithm);

    /* Odd embedding levels are right to left. */
    return (level & 1) ? SCHULTZ_DIR_RTL : SCHULTZ_DIR_LTR;
}

/* ------------------------------------------------- grapheme clusters */

/*
 * Where the caret may sit.
 *
 * A caret moves over whole characters as a person sees them, not over the
 * bytes or even the codepoints that spell them. A family emoji is five
 * codepoints joined by zero width joiners and one picture on screen: stepping
 * by codepoint would take five presses to cross it and a backspace would
 * leave four people behind. The Unicode term for one of these is a grapheme
 * cluster, and libunibreak marks them.
 *
 * The marks are worked out over the whole string each time rather than kept,
 * because they change with every edit and a field is short. A string too long
 * to mark falls back to stepping by codepoint, which is wrong only for text
 * that is both enormous and full of emoji.
 */
static char *schultz_text_cluster_marks(const char *utf8, uint32_t length)
{
    char *marks;

    if (utf8 == NULL || length == 0u) {
        return NULL;
    }
    marks = (char *)malloc(length);
    if (marks == NULL) {
        return NULL;
    }
    schultz_text_init_linebreak();
    set_graphemebreaks_utf8((const utf8_t *)utf8, (size_t)length, NULL,
                            marks);
    return marks;
}

/* One codepoint forward, for when the marks are not available. */
static uint32_t schultz_text_step_forward(const char *utf8, uint32_t length,
                                          uint32_t at)
{
    if (at >= length) {
        return length;
    }
    at++;
    while (at < length && ((unsigned char)utf8[at] & 0xC0u) == 0x80u) {
        at++;
    }
    return at;
}

/* And one back. */
static uint32_t schultz_text_step_back(const char *utf8, uint32_t at)
{
    if (at == 0u) {
        return 0u;
    }
    at--;
    while (at > 0u && ((unsigned char)utf8[at] & 0xC0u) == 0x80u) {
        at--;
    }
    return at;
}

uint32_t schultz_text_next_cluster(const char *utf8, uint32_t length,
                                   uint32_t at)
{
    char *marks;
    uint32_t i;

    if (utf8 == NULL || at >= length) {
        return length;
    }
    marks = schultz_text_cluster_marks(utf8, length);
    if (marks == NULL) {
        return schultz_text_step_forward(utf8, length, at);
    }
    /* A mark says a cluster ends after that byte, so the next boundary is
     * one past the first marked byte at or after here. */
    for (i = at; i < length; i++) {
        if (marks[i] == GRAPHEMEBREAK_BREAK) {
            free(marks);
            return i + 1u;
        }
    }
    free(marks);
    return length;
}

uint32_t schultz_text_prev_cluster(const char *utf8, uint32_t length,
                                   uint32_t at)
{
    char *marks;
    uint32_t i;

    if (utf8 == NULL || at == 0u) {
        return 0u;
    }
    if (at > length) {
        at = length;
    }
    marks = schultz_text_cluster_marks(utf8, length);
    if (marks == NULL) {
        return schultz_text_step_back(utf8, at);
    }
    /* Back to the first byte of the cluster the byte before here is in. */
    for (i = at - 1u; i > 0u; i--) {
        if (marks[i - 1u] == GRAPHEMEBREAK_BREAK) {
            free(marks);
            return i;
        }
    }
    free(marks);
    return 0u;
}

/* --------------------------------------------------- covering the text */

/*
 * Reads one UTF-8 character. Returns the codepoint and moves `at` past it.
 * A malformed byte is reported as itself and skipped, which keeps the walk
 * moving rather than looping on bad input.
 */
static uint32_t schultz_text_codepoint(const char *utf8, size_t bytes,
                                       size_t *at)
{
    unsigned char lead = (unsigned char)utf8[*at];
    uint32_t value;
    uint32_t extra;
    uint32_t i;

    if (lead < 0x80u)      { extra = 0u; value = lead; }
    else if (lead < 0xC0u) { extra = 0u; value = lead; } /* stray tail byte */
    else if (lead < 0xE0u) { extra = 1u; value = lead & 0x1Fu; }
    else if (lead < 0xF0u) { extra = 2u; value = lead & 0x0Fu; }
    else                   { extra = 3u; value = lead & 0x07u; }

    if (*at + (size_t)extra >= bytes) {
        /* Truncated at the end of the string: report the byte and move on. */
        (*at)++;
        return lead;
    }
    for (i = 1u; i <= extra; i++) {
        value = (value << 6) | ((unsigned char)utf8[*at + i] & 0x3Fu);
    }
    *at += (size_t)extra + 1u;
    return value;
}

/*
 * One stretch of the string and the face that draws it.
 *
 * The split is made at grapheme cluster boundaries rather than at character
 * boundaries, because a cluster is one thing on screen and has to be shaped
 * as one. A family emoji is five characters joined by zero width joiners: the
 * joiners are in the text face and the people are not, so splitting per
 * character would hand the pieces to different faces and draw five people
 * where there should be a family.
 */
/* schultz_text_piece, from the header, is what one of these is. */

/* The two characters that say outright how what precedes them is to be
 * drawn: as a picture, or as a letter. */
enum {
    SCHULTZ_TEXT_AS_TEXT  = 0xFE0Eu,
    SCHULTZ_TEXT_AS_EMOJI = 0xFE0Fu
};

/*
 * Which of the two faces draws a cluster.
 *
 * The first character decides what the cluster is, because a cluster is a
 * base plus things that modify it: a letter with an accent is a letter
 * however the accent is encoded, and an emoji with a skin tone is an emoji.
 *
 * Asking which face merely has the character is not enough, and that is the
 * whole of what makes this awkward. A text face covers a great many emoji as
 * plain outlines -- DejaVu draws the smileys, the hearts, the aeroplane and
 * the tick -- so a face chosen on coverage draws a smiling face as thin
 * black line art next to emoji that came out in colour. The question that
 * sorts them is which face has the colours.
 *
 * That leaves the characters both faces draw properly, such as an arrowhead
 * or a tick, which are a picture in one place and punctuation in another.
 * Unicode settles those with a character written after them saying which was
 * meant, and both are honoured here.
 */
static schultz_handle schultz_text_face_for(const schultz_font_system *system,
                                            schultz_handle font,
                                            schultz_handle other,
                                            const char *utf8, size_t start,
                                            size_t end, size_t bytes)
{
    size_t at = start;
    uint32_t base = schultz_text_codepoint(utf8, bytes, &at);

    /* Anything in the cluster that says which was meant wins outright. */
    while (at < end) {
        uint32_t next = schultz_text_codepoint(utf8, bytes, &at);

        if (next == SCHULTZ_TEXT_AS_TEXT) {
            return schultz_font_has_glyph(system, font, base) ? font : other;
        }
        if (next == SCHULTZ_TEXT_AS_EMOJI) {
            return other;
        }
    }

    /*
     * A picture the emoji face can draw goes to the emoji face.
     *
     * Both halves are needed. Asking only what the emoji face has would hand
     * it the digits, the hash and the asterisk, which it carries so it can
     * draw them in a keycap and which it draws in colour even on their own:
     * a year would come out as coloured tiles. Asking only whether the
     * character is a picture would send it emoji the face has never heard
     * of. Unicode calls the pictures extended pictographic, and libunibreak
     * knows which are which.
     */
    if (ub_is_extended_pictographic((utf32_t)base) &&
        schultz_font_has_color_glyph(system, other, base)) {
        return other;
    }
    if (schultz_font_has_glyph(system, font, base)) {
        return font;
    }
    /*
     * The emoji face may still have it without colour, which is how the
     * pieces of a sequence are carried, and a real glyph beats no glyph.
     */
    if (schultz_font_has_glyph(system, other, base)) {
        return other;
    }
    /*
     * Neither has it, so the text face draws it: what a face shows for a
     * character it does not have is a box, and an emoji face's is usually
     * blank. A character nobody can draw should look missing rather than
     * look like nothing was typed.
     */
    return font;
}

/*
 * The emoji face, at the same size as the face being shaped with.
 *
 * A face is bound to one pixel size, so the emoji at the end of a line of
 * sixteen pixel text has to be the emoji face at sixteen pixels. Asking for
 * it here rather than storing a sized handle means one call covers every
 * size a screen is set in.
 */
static schultz_handle schultz_text_emoji_face(
    const schultz_font_system *system, schultz_handle font)
{
    schultz_handle other = schultz_font_emoji(system);
    schultz_handle sized = SCHULTZ_HANDLE_NONE;
    float size;

    if (other == SCHULTZ_HANDLE_NONE || other == font) {
        return SCHULTZ_HANDLE_NONE;
    }
    size = schultz_font_size(system, font);
    if (size <= 0.0f) {
        return other;
    }
    /*
     * Const is cast off because asking for a size may load one, which is a
     * change to the system's own bookkeeping and not to anything the caller
     * can see. Every other reader of a font system does the same.
     */
    if (schultz_font_at_size((schultz_font_system *)system, other, size,
                             &sized) != SCHULTZ_OK) {
        return other;
    }
    return sized;
}

/*
 * Splits the string into stretches, each drawn by one face.
 *
 * With no emoji face, or when the chosen one covers everything, this is one
 * stretch and shaping takes the same path it always did.
 */
/*
 * The face a byte is set in before emoji fallback has its say: the caller's
 * piece covering it, or the call's own font where no piece does. Pieces are
 * few and in order, so a walk is cheaper than anything cleverer.
 */
static schultz_handle schultz_text_base_face(const schultz_text_piece *pieces,
                                             uint32_t piece_count,
                                             schultz_handle font, size_t at)
{
    uint32_t i;

    for (i = 0u; i < piece_count; i++) {
        if ((size_t)pieces[i].start <= at && at < (size_t)pieces[i].end) {
            /*
             * A piece that names no face is saying something about the text
             * other than which face it is in, so it keeps the call's. The
             * alternative is a stretch nothing can draw, which comes out as
             * a hole in the middle of a sentence.
             */
            return (pieces[i].font == SCHULTZ_HANDLE_NONE) ? font
                                                           : pieces[i].font;
        }
    }
    return font;
}

static int32_t schultz_text_split(const schultz_font_system *system,
                                  schultz_handle font,
                                  const schultz_text_piece *given,
                                  uint32_t given_count,
                                  schultz_handle other,
                                  const char *utf8, size_t bytes,
                                  schultz_arena *arena,
                                  schultz_text_piece **out_pieces,
                                  uint32_t *out_count)
{
    schultz_text_piece *pieces;
    char *breaks;
    size_t at;
    uint32_t count = 0u;
    /*
     * The emoji face for the last base face asked about. Finding one walks
     * every loaded font, and this runs once per cluster, so without this a
     * bold paragraph would pay that walk for every character of it, on every
     * candidate wrapping measures. Neighbouring clusters nearly always share
     * a face, so one entry catches nearly all of it.
     */
    schultz_handle last_base  = font;
    schultz_handle last_other = other;

    *out_pieces = NULL;
    *out_count  = 0u;

    /*
     * Nothing to fall back to and nothing to change face for, so there is
     * nothing to decide: one stretch in the chosen face, and no walk of the
     * string at all. This is ordinary prose, which is most of it.
     */
    if (other == SCHULTZ_HANDLE_NONE && given_count == 0u) {
        pieces = (schultz_text_piece *)schultz_arena_alloc(
            arena, sizeof(*pieces), _Alignof(schultz_text_piece));
        if (pieces == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        pieces[0].start = 0u;
        pieces[0].end   = (uint32_t)bytes;
        pieces[0].font  = font;
        *out_pieces = pieces;
        *out_count  = 1u;
        return SCHULTZ_OK;
    }

    /*
     * At most one stretch per byte, which is the worst case of alternating
     * one byte clusters. The arena is scratch for this call in every caller,
     * so the slack costs a bump of a pointer.
     */
    pieces = (schultz_text_piece *)schultz_arena_alloc(
        arena, bytes * sizeof(*pieces), _Alignof(schultz_text_piece));
    breaks = (char *)schultz_arena_alloc(arena, bytes, 1u);
    if (pieces == NULL || breaks == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    schultz_text_init_linebreak();
    set_graphemebreaks_utf8((const utf8_t *)utf8, bytes, NULL, breaks);

    at = 0u;
    while (at < bytes) {
        size_t end = at;
        schultz_handle face;

        /* To the end of this cluster: the byte after the next break. */
        while (end < bytes && breaks[end] != GRAPHEMEBREAK_BREAK) {
            end++;
        }
        end++;
        if (end > bytes) {
            end = bytes;
        }

        /*
         * The caller's face for this cluster first, then emoji fallback
         * asked about that face rather than about the call's own. A bold
         * phrase with an emoji in it falls back from the bold face, and gets
         * the same picture either way.
         */
        schultz_handle base = schultz_text_base_face(given, given_count, font,
                                                     at);

        if (base != last_base) {
            last_other = schultz_text_emoji_face(system, base);
            last_base  = base;
        }

        face = schultz_text_face_for(system, base, last_other, utf8, at, end,
                                     bytes);

        if (count > 0u && pieces[count - 1u].font == face) {
            pieces[count - 1u].end = (uint32_t)end;
        } else {
            pieces[count].start = (uint32_t)at;
            pieces[count].end   = (uint32_t)end;
            pieces[count].font  = face;
            count++;
        }
        at = end;
    }

    *out_pieces = pieces;
    *out_count  = count;
    return SCHULTZ_OK;
}

/*
 * How tall a line of these pieces has to be, and where its baseline sits.
 *
 * The tallest face wins both. A line carrying one word at twice the size is
 * as tall as that word, and every piece on it sits on the one baseline, which
 * is what makes mixed sizes read as a line rather than as a ransom note.
 */
static int32_t schultz_text_piece_metrics(const schultz_font_system *system,
                                          const schultz_text_piece *pieces,
                                          uint32_t piece_count,
                                          schultz_handle font, size_t bytes,
                                          schultz_font_metrics *out_metrics)
{
    schultz_font_metrics one;
    uint32_t i;
    int32_t found = 0;

    memset(out_metrics, 0, sizeof(*out_metrics));
    for (i = 0u; i < piece_count; i++) {
        /* A piece describing text that is not here says nothing about how
         * tall this line is. */
        if ((size_t)pieces[i].start >= bytes ||
            pieces[i].end <= pieces[i].start) {
            continue;
        }
        if (schultz_font_get_metrics(system, pieces[i].font, &one)
                != SCHULTZ_OK) {
            continue;
        }
        if (one.line_height > out_metrics->line_height) {
            out_metrics->line_height = one.line_height;
        }
        if (one.ascent > out_metrics->ascent) {
            out_metrics->ascent = one.ascent;
        }
        if (one.descent > out_metrics->descent) {
            out_metrics->descent = one.descent;
        }
        found = 1;
    }
    /*
     * The call's own font counts too, because any byte no piece covers is
     * drawn in it. When the pieces covered everything and there is no such
     * font, what they gave is the whole answer.
     */
    if (schultz_font_get_metrics(system, font, &one) == SCHULTZ_OK) {
        if (one.line_height > out_metrics->line_height) {
            out_metrics->line_height = one.line_height;
        }
        if (one.ascent > out_metrics->ascent) {
            out_metrics->ascent = one.ascent;
        }
        if (one.descent > out_metrics->descent) {
            out_metrics->descent = one.descent;
        }
        found = 1;
    }
    return found ? SCHULTZ_OK : SCHULTZ_ERR_INVALID_HANDLE;
}

int32_t schultz_text_shape(const schultz_font_system *system,
                           schultz_handle font, const char *utf8,
                           int32_t length, int32_t direction,
                           schultz_arena *arena, schultz_text_run *out_run)
{
    return schultz_text_shape_pieces(system, NULL, 0u, font, utf8, length,
                                     direction, arena, out_run);
}

int32_t schultz_text_shape_pieces(const schultz_font_system *system,
                                  const schultz_text_piece *given,
                                  uint32_t given_count,
                                  schultz_handle font_handle, const char *utf8,
                                  int32_t length, int32_t direction,
                                  schultz_arena *arena,
                                  schultz_text_run *out_run)
{
    hb_buffer_t *buffer;
    hb_glyph_info_t *infos;
    hb_glyph_position_t *positions;
    schultz_glyph *glyphs;
    uint32_t *clusters;
    schultz_handle *fonts;
    schultz_text_piece *pieces = NULL;
    schultz_font_metrics metrics;
    unsigned int count;
    unsigned int i;
    uint32_t piece;
    uint32_t piece_count = 0u;
    uint32_t total = 0u;
    size_t bytes;
    float pen_x = 0.0f;
    float pen_y = 0.0f;
    int32_t result;

    if (arena == NULL || out_run == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_text_length(utf8, length, &bytes);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (given == NULL) {
        given_count = 0u;
    }
    result = schultz_text_piece_metrics(system, given, given_count,
                                        font_handle, bytes, &metrics);
    if (result != SCHULTZ_OK) {
        return result;
    }

    memset(out_run, 0, sizeof(*out_run));
    out_run->height = metrics.line_height;
    out_run->ascent = metrics.ascent;

    if (direction == SCHULTZ_DIR_AUTO) {
        direction = schultz_text_base_direction(utf8, (int32_t)bytes);
    }
    out_run->direction = direction;

    if (bytes == 0) {
        out_run->glyphs   = NULL;
        out_run->clusters = NULL;
        out_run->fonts    = NULL;
        out_run->count    = 0;
        out_run->width    = 0.0f;
        return SCHULTZ_OK;
    }

    /*
     * Which face draws what. Nearly always one stretch in the chosen face,
     * in which case this costs one walk of the string and the shaping below
     * is the single buffer it always was.
     */
    {
        schultz_handle other = schultz_text_emoji_face(system, font_handle);

        result = schultz_text_split(system, font_handle, given, given_count,
                                    other, utf8, bytes, arena, &pieces,
                                    &piece_count);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }

    /*
     * An upper bound on the glyphs, so one array holds all the stretches.
     * Shaping never produces more glyphs than the string has bytes: the
     * shortest character is one byte, and a character that becomes several
     * glyphs is spelled with several bytes.
     */
    glyphs = (schultz_glyph *)schultz_arena_alloc(
        arena, bytes * sizeof(*glyphs), _Alignof(schultz_glyph));
    clusters = (uint32_t *)schultz_arena_alloc(
        arena, bytes * sizeof(*clusters), _Alignof(uint32_t));
    fonts = (schultz_handle *)schultz_arena_alloc(
        arena, bytes * sizeof(*fonts), _Alignof(schultz_handle));
    if (glyphs == NULL || clusters == NULL || fonts == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    for (piece = 0u; piece < piece_count; piece++) {
        /*
         * Right to left text runs the other way across the screen, so the
         * last stretch is the leftmost and the stretches are laid down in
         * reverse. Within a stretch HarfBuzz has already put the glyphs in
         * visual order.
         */
        const schultz_text_piece *part =
            &pieces[(direction == SCHULTZ_DIR_RTL)
                        ? (piece_count - 1u - piece)
                        : piece];
        schultz_font *face;
        size_t part_bytes = part->end - part->start;

        if (schultz_font_resolve(system, part->font, &face) != SCHULTZ_OK) {
            continue;
        }

        buffer = hb_buffer_create();
        if (buffer == NULL || !hb_buffer_allocation_successful(buffer)) {
            if (buffer != NULL) {
                hb_buffer_destroy(buffer);
            }
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }

        /*
         * The whole string is handed over with only part of it marked for
         * shaping, so HarfBuzz can see the characters on either side. That
         * is what lets a letter next to a stretch boundary still join and
         * kern as it should.
         */
        hb_buffer_add_utf8(buffer, utf8, (int)bytes, (int)part->start,
                           (int)part_bytes);
        hb_buffer_set_direction(buffer, (direction == SCHULTZ_DIR_RTL)
                                            ? HB_DIRECTION_RTL
                                            : HB_DIRECTION_LTR);
        /*
         * Fill in script and language from the text itself. Direction was set
         * above and is left alone by this call.
         */
        hb_buffer_guess_segment_properties(buffer);

        hb_shape(face->hb_font, buffer, NULL, 0);

        infos     = hb_buffer_get_glyph_infos(buffer, &count);
        positions = hb_buffer_get_glyph_positions(buffer, &count);

        /*
         * HarfBuzz reports positions in 26.6 fixed point because the face
         * was created through hb-ft at a fixed pixel size, so every offset
         * and advance is in 64ths of a pixel.
         */
        for (i = 0; i < count && total < bytes; i++) {
            glyphs[total].glyph_id = infos[i].codepoint;
            /* Where in the source string this glyph came from, for the
             * caret. Offsets are into the whole string already, because the
             * whole string is what was added to the buffer. */
            clusters[total] = (uint32_t)infos[i].cluster;
            fonts[total]    = part->font;
            glyphs[total].x = pen_x + (float)positions[i].x_offset / 64.0f;
            glyphs[total].y = pen_y - (float)positions[i].y_offset / 64.0f;
            pen_x += (float)positions[i].x_advance / 64.0f;
            pen_y -= (float)positions[i].y_advance / 64.0f;
            total++;
        }

        hb_buffer_destroy(buffer);
    }

    if (total == 0u) {
        out_run->glyphs   = NULL;
        out_run->clusters = NULL;
        out_run->fonts    = NULL;
        out_run->count    = 0;
        out_run->width    = 0.0f;
        return SCHULTZ_OK;
    }

    out_run->glyphs   = glyphs;
    out_run->clusters = clusters;
    out_run->fonts    = fonts;
    out_run->count    = total;
    out_run->width    = pen_x;
    return SCHULTZ_OK;
}

float schultz_text_caret_x(const schultz_text_run *run, uint32_t offset)
{
    uint32_t i;

    if (run == NULL || run->count == 0u || run->clusters == NULL) {
        return 0.0f;
    }
    /*
     * Glyphs are in visual order, so for right to left text the first glyph
     * is the last character. Scanning for the glyph that starts at or after
     * the offset works either way, because the answer wanted is a position,
     * not an index.
     */
    for (i = 0; i < run->count; i++) {
        if (run->clusters[i] >= offset) {
            return run->glyphs[i].x;
        }
    }
    return run->width;
}

uint32_t schultz_text_caret_offset(const schultz_text_run *run, float x,
                                   uint32_t length)
{
    uint32_t best = length;
    float best_distance;
    uint32_t i;

    if (run == NULL || run->count == 0u || run->clusters == NULL) {
        return 0u;
    }

    /* Start from the end of the run, then look for anything closer. */
    best_distance = (x > run->width) ? (x - run->width) : (run->width - x);
    for (i = 0; i < run->count; i++) {
        float at = run->glyphs[i].x;
        float distance = (x > at) ? (x - at) : (at - x);

        if (distance < best_distance) {
            best_distance = distance;
            best = run->clusters[i];
        }
    }
    return best;
}

int32_t schultz_text_measure(const schultz_font_system *system,
                             schultz_handle font_handle, const char *utf8,
                             int32_t length, schultz_arena *arena,
                             schultz_size *out_size)
{
    return schultz_text_measure_pieces(system, NULL, 0u, font_handle, utf8,
                                       length, arena, out_size);
}

int32_t schultz_text_measure_pieces(const schultz_font_system *system,
                                    const schultz_text_piece *given,
                                    uint32_t given_count,
                                    schultz_handle font_handle,
                                    const char *utf8, int32_t length,
                                    schultz_arena *arena,
                                    schultz_size *out_size)
{
    schultz_text_run run;
    int32_t result;

    if (out_size == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    result = schultz_text_shape_pieces(system, given, given_count, font_handle,
                                       utf8, length, SCHULTZ_DIR_AUTO, arena,
                                       &run);
    if (result != SCHULTZ_OK) {
        return result;
    }

    out_size->width  = run.width;
    out_size->height = run.height;
    return SCHULTZ_OK;
}

/*
 * Measures [from, to) of a string whose face changes partway through.
 *
 * Wrapping asks about one prefix after another, and the pieces it was given
 * describe the whole string, so each question needs them clipped to the part
 * being asked about and moved to the front of it. `scratch` holds the result
 * and is reused for every question rather than allocated per candidate;
 * wrapping asks a great many of them.
 */
static int32_t schultz_text_measure_part(const schultz_font_system *system,
                                         const schultz_text_piece *given,
                                         uint32_t given_count,
                                         schultz_handle font_handle,
                                         const char *utf8, size_t from,
                                         size_t to, schultz_arena *arena,
                                         schultz_text_piece *scratch,
                                         schultz_size *out_size)
{
    uint32_t count = 0u;
    uint32_t i;

    /*
     * Pieces with nowhere to clip them to would be dropped, and dropping
     * them measures the text in one face and reports a width that is not
     * what anything will draw. Both callers allocate the scratch whenever
     * there are pieces, so this refuses a mistake rather than a case.
     */
    if (given_count > 0u && scratch == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    for (i = 0u; i < given_count; i++) {
        size_t start = (size_t)given[i].start;
        size_t end   = (size_t)given[i].end;

        if (start < from) {
            start = from;
        }
        if (end > to) {
            end = to;
        }
        if (start >= end) {
            continue;
        }
        scratch[count].start = (uint32_t)(start - from);
        scratch[count].end   = (uint32_t)(end - from);
        scratch[count].font  = given[i].font;
        count++;
    }
    return schultz_text_measure_pieces(system, scratch, count, font_handle,
                                       utf8 + from, (int32_t)(to - from),
                                       arena, out_size);
}

/*
 * Moves an end offset back over the line break that caused it, so a line's
 * reported text stops before the break. Handles the ASCII breaks and the two
 * Unicode separators, which are the only mandatory breaks that carry bytes.
 */
static size_t schultz_text_trim_break(const char *utf8, size_t start,
                                      size_t end)
{
    while (end > start) {
        unsigned char c = (unsigned char)utf8[end - 1];

        if (c == '\n' || c == '\r' || c == 0x0Bu || c == 0x0Cu ||
            c == 0x85u) {
            end--;
            continue;
        }
        /* U+2028 line separator and U+2029 paragraph separator. */
        if (end - start >= 3u && (unsigned char)utf8[end - 3] == 0xE2u &&
            (unsigned char)utf8[end - 2] == 0x80u && (c == 0xA8u ||
                                                      c == 0xA9u)) {
            end -= 3u;
            continue;
        }
        break;
    }
    return end;
}

/*
 * The largest prefix of a run that fits a width, snapped to a character
 * boundary and never empty. This is the last resort: a run with no break
 * opportunity in it still has to be broken somewhere, or it runs out of the
 * box it was given and cannot be read at all.
 *
 * Binary search over the character starts, because measuring means shaping
 * and shaping every prefix in turn would be quadratic.
 */
static size_t schultz_text_fit(const schultz_font_system *system,
                               const schultz_text_piece *given,
                               uint32_t given_count,
                               schultz_handle font_handle, const char *utf8,
                               size_t start, size_t end, float width,
                               schultz_arena *arena,
                               schultz_text_piece *scratch)
{
    size_t low = start;
    size_t high = end;
    size_t best = start;

    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        schultz_size measured;

        /* Land on the start of a character, never inside one. */
        while (middle > low &&
               ((unsigned char)utf8[middle] & 0xC0u) == 0x80u) {
            middle--;
        }
        if (middle <= low) {
            break;
        }
        if (schultz_text_measure_part(system, given, given_count, font_handle,
                                      utf8, start, middle, arena, scratch,
                                      &measured) != SCHULTZ_OK) {
            break;
        }
        if (measured.width <= width) {
            best = middle;
            low  = middle + 1u;
        } else {
            high = middle;
        }
    }

    if (best > start) {
        return best;
    }
    /* Not even one character fits. Emit one anyway, or nothing ever ends. */
    best = start + 1u;
    while (best < end && ((unsigned char)utf8[best] & 0xC0u) == 0x80u) {
        best++;
    }
    return best;
}

int32_t schultz_text_wrap(const schultz_font_system *system,
                          schultz_handle font, const char *utf8,
                          int32_t length, float available_width,
                          schultz_arena *arena,
                          const schultz_text_line **out_lines,
                          uint32_t *out_count)
{
    return schultz_text_wrap_pieces(system, NULL, 0u, font, utf8, length,
                                    available_width, arena, out_lines,
                                    out_count);
}

int32_t schultz_text_wrap_pieces(const schultz_font_system *system,
                                 const schultz_text_piece *given,
                                 uint32_t given_count,
                                 schultz_handle font_handle, const char *utf8,
                                 int32_t length, float available_width,
                                 schultz_arena *arena,
                                 const schultz_text_line **out_lines,
                                 uint32_t *out_count)
{
    schultz_font_metrics metrics;
    schultz_text_piece *scratch = NULL;
    schultz_text_line *lines;
    char *breaks;
    size_t bytes;
    size_t i;
    size_t line_start = 0;
    size_t last_break = 0;      /* end of the last fitting break candidate */
    int32_t have_candidate = 0;
    uint32_t used = 0;
    uint32_t capacity;
    int32_t result;

    if (arena == NULL || out_lines == NULL || out_count == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_text_length(utf8, length, &bytes);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (given == NULL) {
        given_count = 0u;
    }
    /*
     * Not for the height, which a line's own pieces decide, but to refuse a
     * face that is not loaded here rather than measure zeroes all the way
     * through and report lines nothing can draw.
     */
    result = schultz_text_piece_metrics(system, given, given_count,
                                        font_handle, bytes, &metrics);
    if (result != SCHULTZ_OK) {
        return result;
    }

    *out_lines = NULL;
    *out_count = 0;
    if (bytes == 0) {
        return SCHULTZ_OK;
    }

    if (given_count > 0u) {
        scratch = (schultz_text_piece *)schultz_arena_alloc(
            arena, (size_t)given_count * sizeof(*scratch),
            _Alignof(schultz_text_piece));
        if (scratch == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
    }

    schultz_text_init_linebreak();

    breaks = (char *)schultz_arena_alloc(arena, bytes, 1);
    if (breaks == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    /*
     * NULL language asks libunibreak for the language neutral rules of
     * UAX #14, which is the right default until a widget carries a locale.
     */
    set_linebreaks_utf8((const utf8_t *)utf8, bytes, NULL, breaks);

    /*
     * One line per byte is the worst case and cannot be exceeded. Allocating
     * it up front keeps this a single pass with no reallocation, which the
     * arena could not do anyway.
     */
    capacity = (uint32_t)bytes + 1u;
    lines = (schultz_text_line *)schultz_arena_alloc(
        arena, (size_t)capacity * sizeof(*lines), _Alignof(schultz_text_line));
    if (lines == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    for (i = 0; i < bytes; i++) {
        int is_mandatory = (breaks[i] == LINEBREAK_MUSTBREAK);
        int is_allowed   = (breaks[i] == LINEBREAK_ALLOWBREAK);
        size_t candidate_end;
        schultz_size measured;

        if (!is_mandatory && !is_allowed) {
            continue;
        }

        /* libunibreak marks the last byte of the character before a break. */
        candidate_end = i + 1;

        if (is_mandatory) {
            /*
             * A mandatory break belongs to neither line. Leaving it on the
             * first one would put a character no font draws into the shaped
             * run and would let a caret sit inside the break, so the reported
             * end stops before it while scanning carries on past it.
             */
            size_t text_end = schultz_text_trim_break(utf8, line_start,
                                                      candidate_end);

            result = schultz_text_measure_part(system, given, given_count,
                                               font_handle, utf8, line_start,
                                               text_end, arena, scratch,
                                               &measured);
            if (result != SCHULTZ_OK) {
                return result;
            }
            lines[used].start  = (uint32_t)line_start;
            lines[used].end    = (uint32_t)text_end;
            lines[used].width  = measured.width;
            lines[used].height = measured.height;
            used++;
            line_start     = candidate_end;
            have_candidate = 0;
            continue;
        }

        result = schultz_text_measure_part(system, given, given_count,
                                           font_handle, utf8,
                                           line_start, candidate_end,
                                           arena, scratch,
                                           &measured);
        if (result != SCHULTZ_OK) {
            return result;
        }

        if (available_width < 0.0f || measured.width <= available_width) {
            /* Still fits: remember this break and keep going. */
            last_break     = candidate_end;
            have_candidate = 1;
            continue;
        }

        /*
         * Overflowed. Break at the last opportunity that fit. If none did,
         * this run is wider than the line all by itself and is broken where
         * it stops fitting: a run that is left to overflow leaves the box it
         * was given and cannot be read.
         */
        if (have_candidate) {
            candidate_end = last_break;
        } else {
            candidate_end = schultz_text_fit(system, given, given_count,
                                             font_handle, utf8,
                                             line_start, candidate_end,
                                             available_width, arena,
                                             scratch);
        }

        result = schultz_text_measure_part(system, given, given_count,
                                           font_handle, utf8,
                                           line_start, candidate_end,
                                           arena, scratch,
                                           &measured);
        if (result != SCHULTZ_OK) {
            return result;
        }
        lines[used].start  = (uint32_t)line_start;
        lines[used].end    = (uint32_t)candidate_end;
        lines[used].width  = measured.width;
        lines[used].height = measured.height;
        used++;
        line_start = candidate_end;

        /*
         * The opportunity that overflowed is still an opportunity for the
         * line that just began, so it carries over rather than being thrown
         * away. Forgetting it leaves the new line with no place to break
         * until the one after next, which is a break in the wrong place or,
         * once a run with no opportunity in it is broken as a last resort, a
         * break in the middle of a word that had a space in it all along.
         */
        have_candidate = (i + 1u > line_start) ? 1 : 0;
        if (have_candidate) {
            last_break = i + 1u;
        }
    }

    /*
     * Whatever is left after the last break is the final line. It has to be
     * checked for overflow like any other: the loop only looks at break
     * opportunities, and the text being typed at the end of a paragraph has
     * none after it yet. Without this check a word being typed sits past the
     * edge until a space is pressed, and only then jumps to the next line.
     */
    while (line_start < bytes) {
        schultz_size measured;
        size_t end = bytes;

        result = schultz_text_measure_part(system, given, given_count,
                                           font_handle, utf8,
                                           line_start, end,
                                           arena, scratch,
                                           &measured);
        if (result != SCHULTZ_OK) {
            return result;
        }
        if (available_width >= 0.0f && measured.width > available_width) {
            /*
             * The tail overflows. Take the last break opportunity in it if
             * there was one, and otherwise break it where it stops fitting.
             * Either way the loop measures what is left over again, which may
             * itself overflow.
             */
            end = (have_candidate && last_break > line_start)
                ? last_break
                : schultz_text_fit(system, given, given_count, font_handle,
                                   utf8, line_start, end, available_width,
                                   arena, scratch);
            result = schultz_text_measure_part(system, given, given_count,
                                           font_handle, utf8,
                                           line_start, end,
                                           arena, scratch,
                                           &measured);
            if (result != SCHULTZ_OK) {
                return result;
            }
            have_candidate = 0;
        }
        lines[used].start  = (uint32_t)line_start;
        lines[used].end    = (uint32_t)end;
        lines[used].width  = measured.width;
        lines[used].height = measured.height;
        used++;
        line_start = end;
    }

    *out_lines = lines;
    *out_count = used;
    return SCHULTZ_OK;
}
