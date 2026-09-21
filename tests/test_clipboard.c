/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_clipboard.c - a clipboard that carries more than one format.
 *
 * Nothing here touches a platform. The clipboard is an interface the host
 * fills in, so these fill it in with one that lives in this process and lies
 * about nothing: it records what was offered and produces a format only when
 * something asks for that format, which is the behaviour the real ones have
 * and the behaviour the rest of the toolkit is built on.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "schultz_clipboard.h"
#include "schultz_widget.h"

#define FORMAT_HTML "text/html"
#define FORMAT_PNG  "image/png"

/*
 * More than the interface allows, deliberately.
 *
 * A stand-in that refuses what Schultz accepts turns a test of Schultz into
 * a test of the stand-in, and worse, one that refuses at the same point
 * hides whether Schultz refused at all: the test sees the right error either
 * way. Room to spare here means an offer too large can only have been
 * stopped upstream.
 */
enum { BOARD_FORMATS = SCHULTZ_CLIPBOARD_MAX + 2 };

/*
 * A clipboard that holds a set of formats and does not build any of them
 * until asked, which is what every platform underneath does.
 */
typedef struct {
    const char *formats[BOARD_FORMATS];
    uint32_t    count;
    schultz_clipboard_make_fn make;
    void       *make_context;
    uint32_t    offers;               /**< How many times it was written. */
    uint32_t    made[BOARD_FORMATS];  /**< How often each was produced. */
} board;

static int32_t board_offer(void *context, const char *const *formats,
                           uint32_t count, schultz_clipboard_make_fn make,
                           void *make_context)
{
    board *b = (board *)context;
    uint32_t i;

    if (count > BOARD_FORMATS) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < count; i++) {
        b->formats[i] = formats[i];
        b->made[i]    = 0u;
    }
    b->count        = count;
    b->make         = make;
    b->make_context = make_context;
    b->offers++;
    return SCHULTZ_OK;
}

static const void *board_take(void *context, const char *format,
                              uint64_t *out_length)
{
    board *b = (board *)context;
    uint32_t i;

    *out_length = 0u;
    for (i = 0; i < b->count; i++) {
        if (strcmp(b->formats[i], format) == 0) {
            b->made[i]++;
            return b->make(b->make_context, format, out_length);
        }
    }
    return NULL;
}

static int32_t board_holds(void *context, const char *format)
{
    board *b = (board *)context;
    uint32_t i;

    for (i = 0; i < b->count; i++) {
        if (strcmp(b->formats[i], format) == 0) {
            return 1;
        }
    }
    return 0;
}

/* How often a format was built, by name, for the laziness tests. */
static uint32_t times_made(const board *b, const char *format)
{
    uint32_t i;

    for (i = 0; i < b->count; i++) {
        if (strcmp(b->formats[i], format) == 0) {
            return b->made[i];
        }
    }
    return 0u;
}

/*
 * What a copy would hand over: a different answer for each format.
 *
 * The text case deliberately hands back the first five bytes of a longer
 * string, so what it returns is genuinely not terminated at the length it
 * reports. That is what a platform clipboard looks like, and it is what the
 * termination test below needs in order to mean anything.
 */
static const void *make_three(void *context, const char *format,
                              uint64_t *out_length)
{
    (void)context;
    if (strcmp(format, SCHULTZ_CLIPBOARD_TEXT) == 0) {
        *out_length = 5u;
        return "plain and then some more";
    }
    if (strcmp(format, FORMAT_HTML) == 0 ||
        strcmp(format, SCHULTZ_CLIPBOARD_HTML) == 0) {
        *out_length = 11u;
        return "<b>rich</b>";
    }
    if (strcmp(format, FORMAT_PNG) == 0) {
        *out_length = 4u;
        return "\x89PNG";
    }
    *out_length = 0u;
    return NULL;
}

static int32_t setup(schultz_tree **out_tree, board *b)
{
    int32_t result = schultz_tree_create(out_tree);

    memset(b, 0, sizeof(*b));
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_tree_set_clipboard(*out_tree, board_offer, board_take,
                                      board_holds, b);
}

/* ---------------------------------------------------------------- refusals */

TEST a_clipboard_refuses_what_it_should(void)
{
    schultz_tree *tree = NULL;
    board b;
    static const char *const formats[] = { SCHULTZ_CLIPBOARD_TEXT };
    uint64_t length = 1u;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_set_clipboard(NULL, board_offer, board_take,
                                         board_holds, &b));
    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_offer(NULL, formats, 1u, make_three,
                                           NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_offer(tree, NULL, 1u, make_three, NULL));
    /* Nothing on offer is not an offer. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_offer(tree, formats, 0u, make_three,
                                           NULL));
    /* And a set of formats with no way to produce them is not one either. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_offer(tree, formats, 1u, NULL, NULL));

    ASSERT_EQ(NULL, schultz_tree_clipboard_take(NULL, SCHULTZ_CLIPBOARD_TEXT,
                                                &length));
    ASSERT_EQ(0u, length);
    ASSERT_EQ(NULL, schultz_tree_clipboard_take(tree, NULL, &length));
    /* No length to put it in means the answer cannot be used. */
    ASSERT_EQ(NULL, schultz_tree_clipboard_take(tree, SCHULTZ_CLIPBOARD_TEXT,
                                                NULL));

    ASSERT_EQ(0, schultz_tree_clipboard_holds(NULL, SCHULTZ_CLIPBOARD_TEXT));
    ASSERT_EQ(0, schultz_tree_clipboard_holds(tree, NULL));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_write(NULL, "x"));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_write(tree, NULL));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_tree_with_no_clipboard_answers_rather_than_crashing(void)
{
    schultz_tree *tree = NULL;
    static const char *const formats[] = { SCHULTZ_CLIPBOARD_TEXT };
    uint64_t length = 1u;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_offer(tree, formats, 1u, make_three,
                                           NULL));
    ASSERT_EQ(NULL, schultz_tree_clipboard_take(tree, SCHULTZ_CLIPBOARD_TEXT,
                                                &length));
    ASSERT_EQ(0u, length);
    ASSERT_EQ(0, schultz_tree_clipboard_holds(tree, SCHULTZ_CLIPBOARD_TEXT));
    ASSERT_EQ(NULL, schultz_tree_clipboard_read(tree));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_write(tree, "x"));

    schultz_tree_destroy(tree);
    PASS();
}

/* ----------------------------------------------------------- more than one */

TEST every_format_offered_can_be_taken_back(void)
{
    schultz_tree *tree = NULL;
    board b;
    static const char *const formats[] = {
        FORMAT_PNG, FORMAT_HTML, SCHULTZ_CLIPBOARD_TEXT
    };
    const void *bytes;
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_tree_clipboard_offer(tree, formats, 3u, make_three,
                                           NULL));
    ASSERT_EQ(1u, b.offers);

    bytes = schultz_tree_clipboard_take(tree, SCHULTZ_CLIPBOARD_TEXT,
                                        &length);
    ASSERT(bytes != NULL);
    ASSERT_EQ(5u, length);
    ASSERT_EQ(0, memcmp(bytes, "plain", 5u));

    bytes = schultz_tree_clipboard_take(tree, FORMAT_PNG, &length);
    ASSERT(bytes != NULL);
    ASSERT_EQ(4u, length);

    bytes = schultz_tree_clipboard_take(tree, FORMAT_HTML, &length);
    ASSERT(bytes != NULL);
    ASSERT_EQ(11u, length);
    ASSERT_EQ(0, memcmp(bytes, "<b>rich</b>", 11u));

    /* And one that was never offered is not there. */
    ASSERT_EQ(NULL, schultz_tree_clipboard_take(tree, "image/tiff", &length));
    ASSERT_EQ(0u, length);

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * The whole reason the interface has a callback rather than a buffer.
 *
 * A picture is expensive to build and most pastes never want one, so offering
 * it has to cost nothing until somebody asks for that format by name.
 */
TEST a_format_nobody_asks_for_is_never_built(void)
{
    schultz_tree *tree = NULL;
    board b;
    static const char *const formats[] = {
        SCHULTZ_CLIPBOARD_TEXT, FORMAT_PNG
    };
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_tree_clipboard_offer(tree, formats, 2u, make_three,
                                           NULL));

    /* Offered, and nothing built yet. */
    ASSERT_EQ(0u, times_made(&b, SCHULTZ_CLIPBOARD_TEXT));
    ASSERT_EQ(0u, times_made(&b, FORMAT_PNG));

    /* Somebody pastes as text. Only the text is built. */
    ASSERT(schultz_tree_clipboard_take(tree, SCHULTZ_CLIPBOARD_TEXT,
                                       &length) != NULL);
    ASSERT_EQ(1u, times_made(&b, SCHULTZ_CLIPBOARD_TEXT));
    ASSERT_EQ(0u, times_made(&b, FORMAT_PNG));

    /* Asking whether the picture is there must not build it either. */
    ASSERT_EQ(1, schultz_tree_clipboard_holds(tree, FORMAT_PNG));
    ASSERT_EQ(0u, times_made(&b, FORMAT_PNG));

    schultz_tree_destroy(tree);
    PASS();
}

TEST asking_what_is_there_does_not_fetch_it(void)
{
    schultz_tree *tree = NULL;
    board b;
    static const char *const formats[] = { SCHULTZ_CLIPBOARD_TEXT };

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_tree_clipboard_offer(tree, formats, 1u, make_three,
                                           NULL));

    ASSERT_EQ(1, schultz_tree_clipboard_holds(tree, SCHULTZ_CLIPBOARD_TEXT));
    ASSERT_EQ(0, schultz_tree_clipboard_holds(tree, FORMAT_PNG));
    ASSERT_EQ(0u, times_made(&b, SCHULTZ_CLIPBOARD_TEXT));

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * A host may install a take without a holds, because asking is the easy half
 * to leave out. Then the question is answered the expensive way rather than
 * refused, which is the difference between a working paste and a dead one.
 */
TEST a_clipboard_without_an_asker_still_answers(void)
{
    schultz_tree *tree = NULL;
    board b;
    static const char *const formats[] = { SCHULTZ_CLIPBOARD_TEXT };

    memset(&b, 0, sizeof(b));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_tree_set_clipboard(tree, board_offer, board_take, NULL,
                                         &b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_tree_clipboard_offer(tree, formats, 1u, make_three,
                                           NULL));

    ASSERT_EQ(1, schultz_tree_clipboard_holds(tree, SCHULTZ_CLIPBOARD_TEXT));
    ASSERT_EQ(0, schultz_tree_clipboard_holds(tree, FORMAT_PNG));
    /* It had to fetch it to find out, which is the cost of not saying. */
    ASSERT_EQ(1u, times_made(&b, SCHULTZ_CLIPBOARD_TEXT));

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------------------- the text */

TEST writing_text_offers_one_format_and_reads_back(void)
{
    schultz_tree *tree = NULL;
    board b;

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_clipboard_write(tree, "copy me"));

    ASSERT_EQ(1u, b.offers);
    ASSERT_EQ(1u, b.count);
    ASSERT_STR_EQ(SCHULTZ_CLIPBOARD_TEXT, b.formats[0]);
    ASSERT_STR_EQ("copy me", schultz_tree_clipboard_read(tree));

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * The string handed to the writer is nearly always a local, and the bytes are
 * produced later than the call. Writing from a buffer that then changes must
 * still put the original on the clipboard.
 */
TEST text_survives_the_caller_reusing_its_buffer(void)
{
    schultz_tree *tree = NULL;
    board b;
    char scratch[16];

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));
    memcpy(scratch, "first", 6u);
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_clipboard_write(tree, scratch));
    memcpy(scratch, "second", 7u);

    ASSERT_STR_EQ("first", schultz_tree_clipboard_read(tree));

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * Bytes off a clipboard are not promised to end in a zero, because a picture
 * has no reason to. Reading as text has to terminate what it hands back or
 * every caller reads off the end of it.
 */
TEST text_read_back_is_terminated(void)
{
    schultz_tree *tree = NULL;
    board b;
    static const char *const formats[] = { SCHULTZ_CLIPBOARD_TEXT };
    const char *text;

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_tree_clipboard_offer(tree, formats, 1u, make_three,
                                           NULL));

    /* Five bytes of a longer string, so byte five is not a zero. */
    text = schultz_tree_clipboard_read(tree);
    ASSERT(text != NULL);
    ASSERT_EQ(5u, (uint32_t)strlen(text));
    ASSERT_STR_EQ("plain", text);

    schultz_tree_destroy(tree);
    PASS();
}

TEST reading_text_that_is_not_there_gives_nothing(void)
{
    schultz_tree *tree = NULL;
    board b;
    static const char *const formats[] = { FORMAT_PNG };

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_tree_clipboard_offer(tree, formats, 1u, make_three,
                                           NULL));

    /* A picture on the clipboard is not text, and saying so is the point. */
    ASSERT_EQ(NULL, schultz_tree_clipboard_read(tree));
    ASSERT_EQ(0, schultz_tree_clipboard_holds(tree, SCHULTZ_CLIPBOARD_TEXT));

    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------- the wrapper Windows wants */

/* Reads one of the header's numbers back out, by keyword. */
static unsigned header_number(const char *blob, const char *keyword)
{
    const char *at = strstr(blob, keyword);

    if (at == NULL) {
        return 0u;
    }
    return (unsigned)strtoul(at + strlen(keyword), NULL, 10);
}

TEST wrapping_markup_refuses_what_it_should(void)
{
    const char *bytes = (const char *)1;
    uint64_t length = 1u;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_clipboard_windows_html(NULL, &bytes, &length));
    ASSERT_EQ(NULL, bytes);
    ASSERT_EQ(0u, length);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_clipboard_windows_html("<html></html>", NULL, &length));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_clipboard_windows_html("<html></html>", &bytes, NULL));
    PASS();
}

/*
 * The offsets count from the first byte of the whole thing, header included.
 * That is the part worth testing: every number has to land on the byte it
 * names, and the header's own length is part of the sum.
 */
TEST wrapped_markup_offsets_point_where_they_say(void)
{
    static const char *const html =
        "<html><body><!--StartFragment--><p>hello</p>"
        "<!--EndFragment--></body></html>";
    const char *blob = NULL;
    uint64_t length = 0u;
    unsigned start_html;
    unsigned end_html;
    unsigned start_fragment;
    unsigned end_fragment;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_clipboard_windows_html(html, &blob, &length));
    ASSERT(blob != NULL);

    start_html     = header_number(blob, "StartHTML:");
    end_html       = header_number(blob, "EndHTML:");
    start_fragment = header_number(blob, "StartFragment:");
    end_fragment   = header_number(blob, "EndFragment:");

    /* The document begins where the header ends, and ends at the end. */
    ASSERT_EQ(0, memcmp(blob + start_html, "<html>", 6u));
    ASSERT_EQ((unsigned)length, end_html);

    /* And the fragment is what sits between the two comments. */
    ASSERT(end_fragment > start_fragment);
    ASSERT_EQ(0, memcmp(blob + start_fragment, "<p>hello</p>",
                        end_fragment - start_fragment));

    PASS();
}

/*
 * Every number is written at the same width, padded with zeros. That is what
 * makes the arithmetic possible at all: the header's length has to be known
 * before the numbers that go in it are.
 */
TEST wrapped_markup_pads_its_numbers_to_a_fixed_width(void)
{
    const char *blob = NULL;
    uint64_t length = 0u;
    const char *at;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_clipboard_windows_html("<html><body>x</body></html>",
                                             &blob, &length));
    ASSERT_EQ(0, memcmp(blob, "Version:0.9\r\n", 13u));
    at = strstr(blob, "StartHTML:");
    ASSERT(at != NULL);
    at += strlen("StartHTML:");
    /*
     * Exactly ten digits and then the end of the line. The width is the
     * point rather than the digits: a fixed width is what lets the header be
     * measured before the numbers that go in it are known.
     */
    {
        uint32_t digits = 0u;

        while (at[digits] >= '0' && at[digits] <= '9') {
            digits++;
        }
        if (digits != 10u) {
            printf("      the offset field is %u wide\n", digits);
        }
        ASSERT_EQ(10u, digits);
        ASSERT_EQ('\r', at[digits]);
    }
    /* And a short number is padded rather than written short. */
    ASSERT_EQ('0', at[0]);
    PASS();
}

/* Markup with no markers in it is wrapped whole, rather than refused. */
TEST markup_without_markers_is_all_fragment(void)
{
    const char *blob = NULL;
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_clipboard_windows_html("<p>bare</p>", &blob, &length));
    ASSERT_EQ(header_number(blob, "StartHTML:"),
              header_number(blob, "StartFragment:"));
    ASSERT_EQ((unsigned)length, header_number(blob, "EndFragment:"));
    PASS();
}

/* The markup comes through unchanged, byte for byte, after the header. */
TEST wrapped_markup_keeps_the_markup(void)
{
    static const char *const html = "<html><body><p>&amp; &lt;</p></body></html>";
    const char *blob = NULL;
    uint64_t length = 0u;
    unsigned start;

    ASSERT_EQ(SCHULTZ_OK,
              schultz_clipboard_windows_html(html, &blob, &length));
    start = header_number(blob, "StartHTML:");
    ASSERT_STR_EQ(html, blob + start);
    ASSERT_EQ((uint32_t)(start + strlen(html)), (uint32_t)length);
    PASS();
}

/*
 * A clipboard backend may take a short cut for plain text alone, to keep the
 * other names a platform has for it. The test is that the short cut is for
 * plain text *alone*: markup is a text media type too, and an offer of words
 * and markup that took the short cut would throw the markup away.
 */
TEST offering_words_and_markup_keeps_both(void)
{
    schultz_tree *tree = NULL;
    board b;
    static const char *const both[] = {
        SCHULTZ_CLIPBOARD_HTML, SCHULTZ_CLIPBOARD_TEXT
    };
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_tree_clipboard_offer(tree, both, 2u, make_three, NULL));

    ASSERT_EQ(2u, b.count);
    ASSERT_EQ(1, schultz_tree_clipboard_holds(tree, SCHULTZ_CLIPBOARD_HTML));
    ASSERT_EQ(1, schultz_tree_clipboard_holds(tree, SCHULTZ_CLIPBOARD_TEXT));
    ASSERT(schultz_tree_clipboard_take(tree, SCHULTZ_CLIPBOARD_HTML,
                                       &length) != NULL);
    ASSERT(length > 0u);

    schultz_tree_destroy(tree);
    PASS();
}

/* -------------------------------------------------------- bytes the caller has */

TEST bytes_handed_over_come_back_the_same(void)
{
    schultz_tree *tree = NULL;
    board b;
    schultz_clipboard_entry two[2];
    const void *back;
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));

    two[0].format = FORMAT_PNG;
    two[0].bytes  = "\x89PNG\x0d\x0a";
    two[0].length = 6u;
    two[1].format = SCHULTZ_CLIPBOARD_TEXT;
    two[1].bytes  = "a picture";
    two[1].length = 9u;
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_clipboard_write_bytes(tree, two, 2u));

    /* Both formats are up, in the order they were given. */
    ASSERT_EQ(2u, b.count);
    ASSERT_EQ(0, strcmp(b.formats[0], FORMAT_PNG));
    ASSERT_EQ(0, strcmp(b.formats[1], SCHULTZ_CLIPBOARD_TEXT));

    back = board_take(&b, FORMAT_PNG, &length);
    ASSERT(back != NULL);
    ASSERT_EQ(6u, (unsigned)length);
    ASSERT_EQ(0, memcmp(back, "\x89PNG\x0d\x0a", 6u));

    back = board_take(&b, SCHULTZ_CLIPBOARD_TEXT, &length);
    ASSERT(back != NULL);
    ASSERT_EQ(9u, (unsigned)length);
    ASSERT_EQ(0, memcmp(back, "a picture", 9u));

    /* A format nobody put up is not invented. */
    back = board_take(&b, SCHULTZ_CLIPBOARD_HTML, &length);
    ASSERT(back == NULL);

    schultz_tree_destroy(tree);
    PASS();
}

TEST bytes_survive_the_caller_letting_go_of_them(void)
{
    schultz_tree *tree = NULL;
    board b;
    schultz_clipboard_entry one;
    char *mine;
    const void *back;
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));

    /*
     * Freed before anything asks for it, which is the whole point of the
     * call copying. A platform asks for the bytes when somebody pastes, and
     * that is long after the caller has moved on.
     */
    mine = (char *)malloc(5u);
    ASSERT(mine != NULL);
    memcpy(mine, "first", 5u);
    one.format = FORMAT_PNG;
    one.bytes  = mine;
    one.length = 5u;
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_clipboard_write_bytes(tree, &one, 1u));
    memcpy(mine, "WRONG", 5u);
    free(mine);

    back = board_take(&b, FORMAT_PNG, &length);
    ASSERT(back != NULL);
    ASSERT_EQ(5u, (unsigned)length);
    ASSERT_EQ(0, memcmp(back, "first", 5u));

    schultz_tree_destroy(tree);
    PASS();
}

TEST writing_bytes_again_replaces_rather_than_adds(void)
{
    schultz_tree *tree = NULL;
    board b;
    schultz_clipboard_entry one;
    const void *back;
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));

    one.format = FORMAT_PNG;
    one.bytes  = "old";
    one.length = 3u;
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_clipboard_write_bytes(tree, &one, 1u));

    one.format = SCHULTZ_CLIPBOARD_TEXT;
    one.bytes  = "new";
    one.length = 3u;
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_clipboard_write_bytes(tree, &one, 1u));

    /* One format up, not two: a clipboard holds one thing at a time. */
    ASSERT_EQ(1u, b.count);
    back = board_take(&b, SCHULTZ_CLIPBOARD_TEXT, &length);
    ASSERT(back != NULL);
    ASSERT_EQ(0, memcmp(back, "new", 3u));
    ASSERT(board_take(&b, FORMAT_PNG, &length) == NULL);

    schultz_tree_destroy(tree);
    PASS();
}

TEST writing_bytes_refuses_what_it_should(void)
{
    schultz_tree *tree = NULL;
    board b;
    schultz_clipboard_entry many[SCHULTZ_CLIPBOARD_MAX + 1];
    schultz_clipboard_entry one;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));
    one.format = FORMAT_PNG;
    one.bytes  = "png";
    one.length = 3u;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_write_bytes(NULL, &one, 1u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_write_bytes(tree, NULL, 1u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_write_bytes(tree, &one, 0u));

    for (i = 0; i < (uint32_t)SCHULTZ_CLIPBOARD_MAX + 1u; i++) {
        many[i] = one;
    }
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_write_bytes(
                  tree, many, (uint32_t)SCHULTZ_CLIPBOARD_MAX + 1u));

    one.format = NULL;
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_write_bytes(tree, &one, 1u));
    one.format = FORMAT_PNG;
    one.bytes = NULL;
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_write_bytes(tree, &one, 1u));
    one.bytes = "png";
    one.length = 0u;
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_write_bytes(tree, &one, 1u));

    /* Nothing was offered by any of those. */
    ASSERT_EQ(0u, b.offers);

    schultz_tree_destroy(tree);
    PASS();
}

TEST offering_more_formats_than_fit_is_refused(void)
{
    schultz_tree *tree = NULL;
    board b;
    const char *many[SCHULTZ_CLIPBOARD_MAX + 1];
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, setup(&tree, &b));
    for (i = 0; i < (uint32_t)SCHULTZ_CLIPBOARD_MAX + 1u; i++) {
        many[i] = FORMAT_PNG;
    }

    /*
     * Refused, not trimmed. The layer underneath carries a fixed number and
     * used to drop the rest in silence, which left a caller believing it had
     * offered something it had not.
     */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tree_clipboard_offer(tree, many,
                                           (uint32_t)SCHULTZ_CLIPBOARD_MAX + 1u,
                                           make_three, NULL));
    ASSERT_EQ(0u, b.offers);

    /* One fewer is fine, so the limit is the limit and not an off by one. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_tree_clipboard_offer(tree, many,
                                           (uint32_t)SCHULTZ_CLIPBOARD_MAX,
                                           make_three, NULL));
    ASSERT_EQ(1u, b.offers);

    schultz_tree_destroy(tree);
    PASS();
}

SUITE(clipboard)
{
    RUN_TEST(a_clipboard_refuses_what_it_should);
    RUN_TEST(a_tree_with_no_clipboard_answers_rather_than_crashing);
    RUN_TEST(every_format_offered_can_be_taken_back);
    RUN_TEST(a_format_nobody_asks_for_is_never_built);
    RUN_TEST(asking_what_is_there_does_not_fetch_it);
    RUN_TEST(a_clipboard_without_an_asker_still_answers);
    RUN_TEST(writing_text_offers_one_format_and_reads_back);
    RUN_TEST(text_survives_the_caller_reusing_its_buffer);
    RUN_TEST(text_read_back_is_terminated);
    RUN_TEST(reading_text_that_is_not_there_gives_nothing);
    RUN_TEST(wrapping_markup_refuses_what_it_should);
    RUN_TEST(wrapped_markup_offsets_point_where_they_say);
    RUN_TEST(wrapped_markup_pads_its_numbers_to_a_fixed_width);
    RUN_TEST(markup_without_markers_is_all_fragment);
    RUN_TEST(wrapped_markup_keeps_the_markup);
    RUN_TEST(offering_words_and_markup_keeps_both);
    RUN_TEST(bytes_handed_over_come_back_the_same);
    RUN_TEST(bytes_survive_the_caller_letting_go_of_them);
    RUN_TEST(writing_bytes_again_replaces_rather_than_adds);
    RUN_TEST(writing_bytes_refuses_what_it_should);
    RUN_TEST(offering_more_formats_than_fit_is_refused);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(clipboard);
    GREATEST_MAIN_END();
}
