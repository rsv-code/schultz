/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_selection.c - one selection over many widgets.
 *
 * These build a tree by hand and set the two ends directly, which is what a
 * drag will do later. Nothing here needs input, a window or a font: an area
 * hands each participant its share and the participant records it, so the
 * whole of the range logic can be checked by asking each label what it thinks
 * is selected.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "schultz_event.h"
#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_image.h"
#include "schultz_render.h"
#include "schultz_resource.h"
#include "schultz_selection.h"
#include "schultz_style.h"
#include "schultz_thorvg.h"
#include "schultz_widgets.h"

#define FONT_PATH "assets/fonts/DejaVuSans.ttf"

/* What a label believes is selected, as a pair. */
static void selected(schultz_tree *tree, schultz_handle label,
                     uint32_t *low, uint32_t *high)
{
    *low = 0u;
    *high = 0u;
    schultz_label_selection(tree, label, low, high);
}

/* A label that takes part, with the text given. */
static schultz_handle prose(schultz_tree *tree, schultz_handle parent,
                            const char *text)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    if (schultz_label_create(tree, parent, text, &node) != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    schultz_label_set_selectable(tree, node, 1);
    return node;
}

/*
 * A page of three paragraphs inside one area, which is the shape every test
 * below works on:
 *
 *   area
 *     one    "first"   5 bytes
 *     two    "second"  6 bytes
 *     three  "third"   5 bytes
 */
typedef struct {
    schultz_tree  *tree;
    schultz_handle area;
    schultz_handle one;
    schultz_handle two;
    schultz_handle three;
} page;

static int32_t page_setup(page *p)
{
    int32_t result = schultz_tree_create(&p->tree);

    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_selection_area_create(p->tree,
                                           schultz_tree_root(p->tree),
                                           &p->area);
    if (result != SCHULTZ_OK) {
        return result;
    }
    p->one   = prose(p->tree, p->area, "first");
    p->two   = prose(p->tree, p->area, "second");
    p->three = prose(p->tree, p->area, "third");
    if (p->one == SCHULTZ_HANDLE_NONE || p->two == SCHULTZ_HANDLE_NONE ||
        p->three == SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    return SCHULTZ_OK;
}

static void page_teardown(page *p)
{
    schultz_tree_destroy(p->tree);
}

/* ---------------------------------------------------------------- refusals */

TEST an_area_refuses_what_it_should(void)
{
    page p;
    schultz_handle stray = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_selection_area_create(NULL, SCHULTZ_HANDLE_NONE,
                                            &node));
    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_selection_area_create(p.tree, p.area, NULL));

    /* A label is not an area. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_selection_area_set_range(p.tree, p.one, p.one, 0u,
                                               p.one, 1u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_selection_area_clear(p.tree, p.one));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_selection_area_ends(p.tree, p.one, NULL, NULL, NULL,
                                          NULL));

    /* An end outside the area names a range the area cannot walk. */
    stray = prose(p.tree, schultz_tree_root(p.tree), "elsewhere");
    ASSERT(stray != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_selection_area_set_range(p.tree, p.area, p.one, 0u,
                                               stray, 2u));

    page_teardown(&p);
    PASS();
}

TEST an_area_with_nothing_selected_says_so(void)
{
    page p;

    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_selection_area_ends(p.tree, p.area, NULL, NULL, NULL,
                                          NULL));
    page_teardown(&p);
    PASS();
}

/* ------------------------------------------------------------ one widget */

TEST a_range_inside_one_paragraph_selects_only_that_much(void)
{
    page p;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.two, 1u,
                                               p.two, 4u));

    selected(p.tree, p.one, &low, &high);
    ASSERT_EQ(low, high);
    selected(p.tree, p.two, &low, &high);
    ASSERT_EQ(1u, low);
    ASSERT_EQ(4u, high);
    selected(p.tree, p.three, &low, &high);
    ASSERT_EQ(low, high);

    page_teardown(&p);
    PASS();
}

/* --------------------------------------------------------- across widgets */

/*
 * The whole point. A range that starts in the first paragraph and ends in the
 * third selects the tail of the first, all of the second, and the head of the
 * third.
 */
TEST a_range_across_three_selects_part_all_and_part(void)
{
    page p;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.one, 2u,
                                               p.three, 3u));

    selected(p.tree, p.one, &low, &high);
    ASSERT_EQ(2u, low);
    ASSERT_EQ(5u, high);   /* to the end of "first" */

    selected(p.tree, p.two, &low, &high);
    ASSERT_EQ(0u, low);
    ASSERT_EQ(6u, high);   /* all of "second" */

    selected(p.tree, p.three, &low, &high);
    ASSERT_EQ(0u, low);
    ASSERT_EQ(3u, high);   /* the head of "third" */

    page_teardown(&p);
    PASS();
}

/*
 * Dragging backwards is an ordinary thing to do, so the two ends arrive in
 * the wrong order and the area has to sort them by reading order rather than
 * by which was given first.
 */
TEST a_range_dragged_backwards_selects_the_same_text(void)
{
    page p;
    uint32_t forward_low[3];
    uint32_t forward_high[3];
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.one, 2u,
                                               p.three, 3u));
    selected(p.tree, p.one, &forward_low[0], &forward_high[0]);
    selected(p.tree, p.two, &forward_low[1], &forward_high[1]);
    selected(p.tree, p.three, &forward_low[2], &forward_high[2]);

    /* The same range, named from the far end. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.three, 3u,
                                               p.one, 2u));
    selected(p.tree, p.one, &low, &high);
    ASSERT_EQ(forward_low[0], low);
    ASSERT_EQ(forward_high[0], high);
    selected(p.tree, p.two, &low, &high);
    ASSERT_EQ(forward_low[1], low);
    ASSERT_EQ(forward_high[1], high);
    selected(p.tree, p.three, &low, &high);
    ASSERT_EQ(forward_low[2], low);
    ASSERT_EQ(forward_high[2], high);

    page_teardown(&p);
    PASS();
}

TEST a_backwards_range_inside_one_paragraph_is_sorted_too(void)
{
    page p;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.two, 5u,
                                               p.two, 1u));
    selected(p.tree, p.two, &low, &high);
    ASSERT_EQ(1u, low);
    ASSERT_EQ(5u, high);

    page_teardown(&p);
    PASS();
}

TEST the_ends_come_back_in_reading_order(void)
{
    page p;
    schultz_handle from = SCHULTZ_HANDLE_NONE;
    schultz_handle to = SCHULTZ_HANDLE_NONE;
    uint32_t from_at = 0u;
    uint32_t to_at = 0u;

    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    /* Given backwards. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.three, 3u,
                                               p.one, 2u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_ends(p.tree, p.area, &from, &from_at,
                                          &to, &to_at));
    ASSERT_EQ(p.one, from);
    ASSERT_EQ(2u, from_at);
    ASSERT_EQ(p.three, to);
    ASSERT_EQ(3u, to_at);

    page_teardown(&p);
    PASS();
}

/* ------------------------------------------------------------- clearing */

TEST clearing_takes_the_selection_off_every_widget(void)
{
    page p;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.one, 0u,
                                               p.three, 5u));
    ASSERT_EQ(SCHULTZ_OK, schultz_selection_area_clear(p.tree, p.area));

    selected(p.tree, p.one, &low, &high);
    ASSERT_EQ(low, high);
    selected(p.tree, p.two, &low, &high);
    ASSERT_EQ(low, high);
    selected(p.tree, p.three, &low, &high);
    ASSERT_EQ(low, high);
    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_selection_area_ends(p.tree, p.area, NULL, NULL, NULL,
                                          NULL));

    page_teardown(&p);
    PASS();
}

/*
 * Moving the range has to unselect whatever fell out of it. A widget that was
 * inside and now is not keeps its highlight otherwise, and the screen shows
 * two selections at once.
 */
TEST shrinking_a_range_unselects_what_it_left_behind(void)
{
    page p;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.one, 0u,
                                               p.three, 5u));
    selected(p.tree, p.three, &low, &high);
    ASSERT(high > low);

    /* Back to the first paragraph only. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.one, 0u,
                                               p.one, 3u));
    selected(p.tree, p.two, &low, &high);
    ASSERT_EQ(low, high);
    selected(p.tree, p.three, &low, &high);
    ASSERT_EQ(low, high);

    page_teardown(&p);
    PASS();
}

/* ------------------------------------------------------- who takes part */

/*
 * A label that was never made selectable is passed over. The range still
 * covers the ones on either side of it, because taking no part is not the
 * same as ending the selection.
 */
TEST a_label_that_is_not_selectable_takes_no_part(void)
{
    page p;
    schultz_handle quiet = SCHULTZ_HANDLE_NONE;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_create(p.tree, p.area, "quiet", &quiet));
    /* Created after the three, so it sits last in reading order. */

    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.one, 0u,
                                               p.three, 5u));
    selected(p.tree, quiet, &low, &high);
    ASSERT_EQ(low, high);
    selected(p.tree, p.three, &low, &high);
    ASSERT_EQ(5u, high);

    page_teardown(&p);
    PASS();
}

/*
 * A button inside the area is not dragged over into the selection.
 *
 * The mechanism is that a button builds its caption without making it
 * selectable, so the caption reports no length and the area passes over it.
 * That is the "controls are out by default" rule working: nothing had to know
 * that this particular label belongs to a button.
 */
TEST a_control_inside_an_area_is_not_selected(void)
{
    page p;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_button_create(p.tree, p.area, "Press", &button));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.one, 0u,
                                               p.three, 5u));

    /* The button's own caption label, if it has one, stays unselected. */
    {
        uint32_t count = schultz_node_child_count(p.tree, button);
        uint32_t i;

        for (i = 0; i < count; i++) {
            schultz_handle kid = SCHULTZ_HANDLE_NONE;

            if (schultz_node_child_at(p.tree, button, i, &kid) == SCHULTZ_OK) {
                selected(p.tree, kid, &low, &high);
                ASSERT_EQ(low, high);
            }
        }
    }

    page_teardown(&p);
    PASS();
}

/* ------------------------------------------------------------- nesting */

/*
 * An area inside an area keeps its own selection. The outer one must not
 * reach into it, or a drag across a page would select the contents of a text
 * field it passed over.
 */
TEST an_area_inside_an_area_keeps_its_own_selection(void)
{
    page p;
    schultz_handle inner = SCHULTZ_HANDLE_NONE;
    schultz_handle guarded = SCHULTZ_HANDLE_NONE;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_create(p.tree, p.area, &inner));
    guarded = prose(p.tree, inner, "guarded");
    ASSERT(guarded != SCHULTZ_HANDLE_NONE);

    /* The outer area selects everything it can reach. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.one, 0u,
                                               p.three, 5u));
    selected(p.tree, guarded, &low, &high);
    ASSERT_EQ(low, high);

    /* And the outer one cannot even name an end inside the inner one. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_selection_area_set_range(p.tree, p.area, p.one, 0u,
                                               guarded, 3u));

    /* The inner one selects its own, and the outer's stays where it was. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, inner, guarded, 1u,
                                               guarded, 4u));
    selected(p.tree, guarded, &low, &high);
    ASSERT_EQ(1u, low);
    ASSERT_EQ(4u, high);

    page_teardown(&p);
    PASS();
}

/* ------------------------------------------------------------- clamping */

/*
 * The offsets came from a caller and the text may have moved on since. A
 * range past the end of a paragraph is clamped rather than trusted, because
 * the alternative is a widget painting a highlight over bytes it does not
 * have.
 */
TEST offsets_past_the_end_are_clamped(void)
{
    page p;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, page_setup(&p));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(p.tree, p.area, p.one, 99u,
                                               p.three, 99u));
    selected(p.tree, p.one, &low, &high);
    ASSERT(high <= 5u);
    ASSERT(low <= high);
    selected(p.tree, p.three, &low, &high);
    ASSERT(high <= 5u);
    ASSERT(low <= high);

    page_teardown(&p);
    PASS();
}

/* ------------------------------------------------------- driven by a drag */

/*
 * The same page, laid out and wired for input, so a press and a drag can be
 * routed at it the way a window would. Text needs a font to measure, and
 * measuring is what turns a point into a byte offset.
 */
typedef struct {
    page                 body;
    schultz_font_system *fonts;
    schultz_glyph_cache *glyphs;
    schultz_handle       font;
    schultz_theme        theme;
    schultz_events      *events;
    char                 board[256];
    uint32_t             copies;
    schultz_image_table    *images;
    schultz_resource_table *resources;
    schultz_render_options  options;
    const char             *offered[4];
    uint32_t                formats;
    schultz_clipboard_make_fn make;
    void                     *make_context;
} live;

static const void *live_take(void *context, const char *format,
                             uint64_t *out_length)
{
    live *l = (live *)context;

    *out_length = 0u;
    if (strcmp(format, SCHULTZ_CLIPBOARD_TEXT) != 0 || l->board[0] == '\0') {
        return NULL;
    }
    *out_length = (uint64_t)strlen(l->board);
    return l->board;
}

static int32_t live_holds(void *context, const char *format)
{
    live *l = (live *)context;

    return (strcmp(format, SCHULTZ_CLIPBOARD_TEXT) == 0 &&
            l->board[0] != '\0') ? 1 : 0;
}

static int32_t live_offer(void *context, const char *const *formats,
                          uint32_t count, schultz_clipboard_make_fn make,
                          void *make_context)
{
    live *l = (live *)context;
    uint64_t length = 0u;
    const void *bytes;
    size_t n;
    uint32_t f;

    if (count == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    l->formats = (count > 4u) ? 4u : count;
    for (f = 0; f < l->formats; f++) {
        l->offered[f] = formats[f];
    }
    /* Kept so a test can ask for a format later, as a paste target does. */
    l->make         = make;
    l->make_context = make_context;
    /*
     * The plain text by name, not whichever format came first. A text box
     * asks for the text, and the richest format now leads the list.
     */
    bytes = make(make_context, SCHULTZ_CLIPBOARD_TEXT, &length);
    /*
     * No text is not a failure. A selection may be a picture and nothing
     * else, and a clipboard that refused it would be refusing something the
     * platforms are happy to carry.
     */
    if (bytes == NULL) {
        l->board[0] = '\0';
        l->copies++;
        return SCHULTZ_OK;
    }
    n = (size_t)length;
    if (n > sizeof(l->board) - 1u) {
        n = sizeof(l->board) - 1u;
    }
    memcpy(l->board, bytes, n);
    l->board[n] = '\0';
    l->copies++;
    return SCHULTZ_OK;
}

/* Three paragraphs stacked down the page, each its own band to aim at. */
static int32_t live_setup(live *l)
{
    int32_t result;

    memset(l, 0, sizeof(*l));
    result = page_setup(&l->body);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(l->body.tree,
                              schultz_rect_make(0, 0, 400, 300));
    result = schultz_font_system_create(&l->fonts);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_glyph_cache_create(l->fonts, &l->glyphs);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_font_load_file(l->fonts, FONT_PATH, 16.0f, &l->font);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_font_system(l->body.tree, l->fonts);
    schultz_theme_init(&l->theme);
    schultz_theme_set_font(&l->theme, SCHULTZ_TOKEN_FONT_BODY, l->font);
    schultz_tree_set_theme(l->body.tree, &l->theme);
    schultz_tree_set_clipboard(l->body.tree, live_offer, live_take,
                               live_holds, l);

    schultz_node_set_bounds(l->body.tree, l->body.area,
                            schultz_rect_make(0, 0, 400, 300));
    schultz_node_set_bounds(l->body.tree, l->body.one,
                            schultz_rect_make(0, 0, 400, 40));
    schultz_node_set_bounds(l->body.tree, l->body.two,
                            schultz_rect_make(0, 40, 400, 40));
    schultz_node_set_bounds(l->body.tree, l->body.three,
                            schultz_rect_make(0, 80, 400, 40));
    schultz_tree_resolve_styles(l->body.tree);
    return schultz_events_create(l->body.tree, &l->events);
}

static void live_teardown(live *l)
{
    schultz_events_destroy(l->events);
    page_teardown(&l->body);
    if (l->images != NULL) {
        schultz_image_table_destroy(l->images);
    }
    if (l->resources != NULL) {
        schultz_resource_table_destroy(l->resources);
    }
    schultz_glyph_cache_destroy(l->glyphs);
    schultz_font_system_destroy(l->fonts);
}

static void press(live *l, float x, float y)
{
    schultz_events_mouse_button(l->events, schultz_point_make(x, y),
                                SCHULTZ_BUTTON_LEFT, 1, 0u);
}

static void drag_to(live *l, float x, float y)
{
    schultz_events_mouse_move(l->events, schultz_point_make(x, y), 0u);
}

static void release(live *l, float x, float y)
{
    schultz_events_mouse_button(l->events, schultz_point_make(x, y),
                                SCHULTZ_BUTTON_LEFT, 0, 0u);
}

/*
 * A drag that starts in the first paragraph and ends in the third has to
 * select across all three. This is the feature.
 */
TEST a_drag_across_three_paragraphs_selects_all_three(void)
{
    live l;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    press(&l, 2.0f, 20.0f);
    drag_to(&l, 200.0f, 60.0f);
    drag_to(&l, 380.0f, 100.0f);
    release(&l, 380.0f, 100.0f);

    selected(l.body.tree, l.body.one, &low, &high);
    ASSERT(high > low);
    selected(l.body.tree, l.body.two, &low, &high);
    ASSERT_EQ(0u, low);
    ASSERT_EQ(6u, high);          /* all of "second" */
    selected(l.body.tree, l.body.three, &low, &high);
    ASSERT(high > low);

    live_teardown(&l);
    PASS();
}

/* A press with no drag puts a caret down and selects nothing. */
TEST a_press_on_its_own_selects_nothing(void)
{
    live l;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    press(&l, 20.0f, 20.0f);
    release(&l, 20.0f, 20.0f);

    selected(l.body.tree, l.body.one, &low, &high);
    ASSERT_EQ(low, high);
    selected(l.body.tree, l.body.two, &low, &high);
    ASSERT_EQ(low, high);

    live_teardown(&l);
    PASS();
}

/* Moving with no button down is a hover, and must not select. */
TEST moving_without_pressing_selects_nothing(void)
{
    live l;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    drag_to(&l, 20.0f, 20.0f);
    drag_to(&l, 200.0f, 100.0f);

    selected(l.body.tree, l.body.two, &low, &high);
    ASSERT_EQ(low, high);

    live_teardown(&l);
    PASS();
}

/* And once the button is up, more movement does not extend it. */
TEST movement_after_the_release_does_not_extend_it(void)
{
    live l;
    uint32_t before_low, before_high, low, high;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    press(&l, 2.0f, 20.0f);
    drag_to(&l, 200.0f, 60.0f);
    release(&l, 200.0f, 60.0f);
    selected(l.body.tree, l.body.two, &before_low, &before_high);

    drag_to(&l, 380.0f, 100.0f);
    selected(l.body.tree, l.body.two, &low, &high);
    ASSERT_EQ(before_low, low);
    ASSERT_EQ(before_high, high);
    selected(l.body.tree, l.body.three, &low, &high);
    ASSERT_EQ(low, high);

    live_teardown(&l);
    PASS();
}

/* --------------------------------------------------------------- copying */

TEST copying_gives_the_pieces_in_reading_order(void)
{
    live l;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.one, 2u,
                                               l.body.three, 3u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_copy(l.body.tree, l.body.area, &l.options));

    /* "rst" from first, all of second, "thi" from third, a break between. */
    ASSERT_STR_EQ("rst\nsecond\nthi", l.board);
    ASSERT_EQ(1u, l.copies);

    live_teardown(&l);
    PASS();
}

TEST copying_one_paragraph_has_no_break_in_it(void)
{
    live l;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.two, 1u,
                                               l.body.two, 4u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_copy(l.body.tree, l.body.area, &l.options));
    ASSERT_STR_EQ("eco", l.board);

    live_teardown(&l);
    PASS();
}

TEST copying_nothing_is_refused_rather_than_copying_an_empty_string(void)
{
    live l;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_selection_area_copy(l.body.tree, l.body.area, &l.options));
    ASSERT_EQ(0u, l.copies);
    ASSERT_EQ(NULL, schultz_selection_area_text(l.body.tree, l.body.area));

    live_teardown(&l);
    PASS();
}

/* Control with C reaches the area, which is what the selection owner is for. */
TEST control_with_c_copies_the_selection(void)
{
    live l;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    press(&l, 2.0f, 20.0f);
    drag_to(&l, 380.0f, 60.0f);
    release(&l, 380.0f, 60.0f);

    schultz_events_key(l.events, 'c', SCHULTZ_MOD_CTRL, 1);
    ASSERT_EQ(1u, l.copies);
    ASSERT(strchr(l.board, '\n') != NULL);  /* it crossed a paragraph */

    live_teardown(&l);
    PASS();
}

/* ---------------------------------------------------- the other presses */

/*
 * A right click inside the selection is asking for a menu about the
 * selection, not asking to reselect one word. Losing the range here is the
 * mistake that is easy to make once a selection spans widgets: with a single
 * owner, a press plainly belonged to somebody else and clearing was right.
 */
TEST a_right_click_keeps_the_selection(void)
{
    live l;
    uint32_t before_low, before_high, low, high;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    press(&l, 2.0f, 20.0f);
    drag_to(&l, 380.0f, 100.0f);
    release(&l, 380.0f, 100.0f);
    selected(l.body.tree, l.body.two, &before_low, &before_high);
    ASSERT(before_high > before_low);

    schultz_events_mouse_button(l.events, schultz_point_make(200.0f, 60.0f),
                                SCHULTZ_BUTTON_RIGHT, 1, 0u);
    schultz_events_mouse_button(l.events, schultz_point_make(200.0f, 60.0f),
                                SCHULTZ_BUTTON_RIGHT, 0, 0u);

    selected(l.body.tree, l.body.two, &low, &high);
    ASSERT_EQ(before_low, low);
    ASSERT_EQ(before_high, high);

    live_teardown(&l);
    PASS();
}

/* Scrolling moves the view, and says nothing about what is selected. */
TEST scrolling_keeps_the_selection(void)
{
    live l;
    uint32_t before_low, before_high, low, high;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    press(&l, 2.0f, 20.0f);
    drag_to(&l, 380.0f, 100.0f);
    release(&l, 380.0f, 100.0f);
    selected(l.body.tree, l.body.two, &before_low, &before_high);

    schultz_events_scroll(l.events, schultz_point_make(200.0f, 60.0f),
                          0.0f, -3.0f);

    selected(l.body.tree, l.body.two, &low, &high);
    ASSERT_EQ(before_low, low);
    ASSERT_EQ(before_high, high);

    live_teardown(&l);
    PASS();
}

/*
 * And a plain press on empty ground inside the area does clear it, because
 * that is a person putting the caret somewhere else.
 */
TEST a_press_on_nothing_clears_the_selection(void)
{
    live l;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    press(&l, 2.0f, 20.0f);
    drag_to(&l, 380.0f, 100.0f);
    release(&l, 380.0f, 100.0f);
    selected(l.body.tree, l.body.two, &low, &high);
    ASSERT(high > low);

    /* Below the last paragraph, still inside the area. */
    press(&l, 200.0f, 200.0f);
    release(&l, 200.0f, 200.0f);

    selected(l.body.tree, l.body.one, &low, &high);
    ASSERT_EQ(low, high);
    selected(l.body.tree, l.body.two, &low, &high);
    ASSERT_EQ(low, high);
    selected(l.body.tree, l.body.three, &low, &high);
    ASSERT_EQ(low, high);

    live_teardown(&l);
    PASS();
}

/* Starting a new drag replaces the old range rather than adding to it. */
TEST a_second_drag_replaces_the_first(void)
{
    live l;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    press(&l, 2.0f, 20.0f);
    drag_to(&l, 380.0f, 100.0f);
    release(&l, 380.0f, 100.0f);

    /* A fresh drag inside the last paragraph only. */
    press(&l, 2.0f, 100.0f);
    drag_to(&l, 60.0f, 100.0f);
    release(&l, 60.0f, 100.0f);

    selected(l.body.tree, l.body.one, &low, &high);
    ASSERT_EQ(low, high);
    selected(l.body.tree, l.body.two, &low, &high);
    ASSERT_EQ(low, high);
    selected(l.body.tree, l.body.three, &low, &high);
    ASSERT(high > low);

    live_teardown(&l);
    PASS();
}

/* ------------------------------------------------------------- pictures */

/* A small selectable picture added to the page, after the three paragraphs. */
static schultz_handle add_picture(live *l)
{
    static uint32_t pixels[8 * 8];
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_handle icon = SCHULTZ_HANDLE_NONE;
    schultz_image_table *images = NULL;
    uint32_t i;

    for (i = 0; i < 8u * 8u; i++) {
        pixels[i] = 0xFF3366CCu;
    }
    /* Copying a picture renders it, and rendering needs the engine up. */
    if (schultz_thorvg_engine_init(0) != SCHULTZ_OK ||
        schultz_image_table_create(&images) != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    if (schultz_resource_table_create(&l->resources) != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    l->options.fonts      = l->fonts;
    l->options.glyphs     = l->glyphs;
    l->options.images     = images;
    l->options.resources  = l->resources;
    l->options.background = schultz_color_rgba(0, 0, 0, 0);
    l->images = images;
    schultz_tree_set_image_table(l->body.tree, images);
    if (schultz_image_set_pixels(images, pixels, 8u, 8u, 0u, &image)
            != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    if (schultz_icon_create(l->body.tree, l->body.area, image, &icon)
            != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    schultz_node_set_bounds(l->body.tree, icon,
                            schultz_rect_make(0, 120, 8, 8));
    schultz_icon_set_selectable(l->body.tree, icon, 1);
    schultz_tree_resolve_styles(l->body.tree);
    return icon;
}

TEST a_picture_is_not_selectable_until_it_is_asked_to_be(void)
{
    live l;
    schultz_handle icon;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    icon = add_picture(&l);
    ASSERT(icon != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(1, schultz_icon_selectable(l.body.tree, icon));

    schultz_icon_set_selectable(l.body.tree, icon, 0);
    ASSERT_EQ(0, schultz_icon_selectable(l.body.tree, icon));
    /* And then a range across everything leaves it out. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.one, 0u, icon, 1u));
    {
        const void *bytes = NULL;
        uint64_t length = 0u;

        ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
                  schultz_selection_area_picture(l.body.tree, l.body.area,
                                                 SCHULTZ_IMAGE_PNG,
                                                 &l.options, &bytes,
                                                 &length));
        ASSERT_EQ(NULL, bytes);
        /*
         * And without render options it answers the same way, because it
         * assembles them from the tree rather than refusing. That is what
         * lets a copy from the keyboard carry a picture.
         */
        ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
                  schultz_selection_area_picture(l.body.tree, l.body.area,
                                                 SCHULTZ_IMAGE_PNG, NULL,
                                                 &bytes, &length));
    }

    live_teardown(&l);
    PASS();
}

/*
 * A picture inside the range is carried by a format that can hold one, and
 * adds nothing to the text. That split is the whole design: the person did
 * not choose, the paste target did.
 */
TEST a_selected_picture_comes_out_as_a_png_and_not_as_text(void)
{
    live l;
    schultz_handle icon;
    const void *bytes = NULL;
    uint64_t length = 0u;
    const char *text;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    icon = add_picture(&l);
    ASSERT(icon != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.three, 0u, icon, 1u));

    /* The text has the paragraph and nothing standing in for the picture. */
    text = schultz_selection_area_text(l.body.tree, l.body.area);
    ASSERT(text != NULL);
    ASSERT_STR_EQ("third", text);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_picture(l.body.tree, l.body.area,
                                             SCHULTZ_IMAGE_PNG, &l.options,
                                             &bytes, &length));
    ASSERT(bytes != NULL);
    ASSERT(length > 0u);
    ASSERT_EQ(0x89u, ((const unsigned char *)bytes)[0]);
    ASSERT_EQ('P', ((const unsigned char *)bytes)[1]);

    live_teardown(&l);
    PASS();
}

/* With no picture in the range there is nothing to hand over. */
TEST a_selection_with_no_picture_has_no_picture_to_give(void)
{
    live l;
    const void *bytes = NULL;
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.one, 0u,
                                               l.body.three, 5u));
    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_selection_area_picture(l.body.tree, l.body.area,
                                             SCHULTZ_IMAGE_PNG, &l.options,
                                             &bytes, &length));
    ASSERT_EQ(NULL, bytes);

    live_teardown(&l);
    PASS();
}

/*
 * Copying a selection that holds a picture offers three formats rather than
 * one, and the clipboard takes whichever it is asked for.
 */
TEST copying_a_picture_offers_more_than_one_format(void)
{
    live l;
    schultz_handle icon;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    icon = add_picture(&l);
    ASSERT(icon != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.three, 0u, icon, 1u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_copy(l.body.tree, l.body.area, &l.options));

    /*
     * Words and a picture together: two formats, and no picture on its own.
     * Offering one out of a document would let a program paste a stray
     * picture where the person had selected a page.
     */
    ASSERT_EQ(2u, l.formats);
    /* The markup leads, because a program takes the first it recognises. */
    ASSERT_STR_EQ(SCHULTZ_CLIPBOARD_HTML, l.offered[0]);
    ASSERT_STR_EQ(SCHULTZ_CLIPBOARD_TEXT, l.offered[1]);

    /* Text alone offers the same two. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.one, 0u,
                                               l.body.two, 2u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_copy(l.body.tree, l.body.area, &l.options));
    ASSERT_EQ(2u, l.formats);
    ASSERT_STR_EQ(SCHULTZ_CLIPBOARD_HTML, l.offered[0]);

    live_teardown(&l);
    PASS();
}

/* ---------------------------------------------------------- the budget */

TEST the_budget_starts_at_twenty_megabytes_and_can_be_set(void)
{
    live l;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    ASSERT_EQ(20u * 1024u * 1024u,
              (uint32_t)schultz_selection_area_limit(l.body.tree,
                                                     l.body.area));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_limit(l.body.tree, l.body.area,
                                               64u));
    ASSERT_EQ(64u, (uint32_t)schultz_selection_area_limit(l.body.tree,
                                                          l.body.area));
    ASSERT_EQ(0, schultz_selection_area_was_cut(l.body.tree, l.body.area));

    live_teardown(&l);
    PASS();
}

/*
 * A copy that would pass the limit stops on a whole block and says it was
 * cut, rather than ending mid sentence or quietly handing back less.
 */
TEST a_copy_past_the_limit_stops_on_a_block_and_says_so(void)
{
    live l;
    const char *text;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    /* Room for "first" and the break, but not for "second" as well. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_limit(l.body.tree, l.body.area, 8u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.one, 0u,
                                               l.body.three, 5u));
    text = schultz_selection_area_text(l.body.tree, l.body.area);
    ASSERT(text != NULL);
    ASSERT_STR_EQ("first", text);
    ASSERT_EQ(1, schultz_selection_area_was_cut(l.body.tree, l.body.area));

    live_teardown(&l);
    PASS();
}

/* A picture too big for the budget is refused before it is rendered. */
TEST a_picture_past_the_limit_is_refused(void)
{
    live l;
    schultz_handle icon;
    const void *bytes = NULL;
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    icon = add_picture(&l);
    ASSERT(icon != SCHULTZ_HANDLE_NONE);
    /* Eight by eight at four bytes a pixel is 256, so this is too small. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_limit(l.body.tree, l.body.area,
                                               100u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.three, 0u, icon, 1u));
    ASSERT_EQ(SCHULTZ_ERR_UNAVAILABLE,
              schultz_selection_area_picture(l.body.tree, l.body.area,
                                             SCHULTZ_IMAGE_PNG, &l.options,
                                             &bytes, &length));
    ASSERT_EQ(1, schultz_selection_area_was_cut(l.body.tree, l.body.area));

    live_teardown(&l);
    PASS();
}

/* ------------------------------------------- the shape the demo page has */

/*
 * The demo's page, in miniature: labels stacked down, then a row holding two
 * pictures, then another label under it. The pictures being inside a row
 * rather than directly under the area is the part that matters, because the
 * walk has to go down into the row and come back out again.
 */
typedef struct {
    live           base;
    schultz_handle top;
    schultz_handle middle;
    schultz_handle row;
    schultz_handle left;
    schultz_handle right;
    schultz_handle bottom;
    schultz_handle note;   /**< Not selectable, like the demo's note. */
} stack;

static int32_t stack_setup(stack *st)
{
    static uint32_t dots[8 * 8];
    schultz_image_table *images = NULL;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_handle area;
    int32_t result = live_setup(&st->base);
    uint32_t i;

    if (result != SCHULTZ_OK) {
        return result;
    }
    area = st->base.body.area;
    /* The three the fixture already made are in the way; hide them. */
    schultz_label_set_selectable(st->base.body.tree, st->base.body.one, 0);
    schultz_label_set_selectable(st->base.body.tree, st->base.body.two, 0);
    schultz_label_set_selectable(st->base.body.tree, st->base.body.three, 0);
    schultz_node_set_bounds(st->base.body.tree, st->base.body.one,
                            schultz_rect_make(0, 0, 0, 0));
    schultz_node_set_bounds(st->base.body.tree, st->base.body.two,
                            schultz_rect_make(0, 0, 0, 0));
    schultz_node_set_bounds(st->base.body.tree, st->base.body.three,
                            schultz_rect_make(0, 0, 0, 0));

    for (i = 0; i < 8u * 8u; i++) {
        dots[i] = 0xFF3366CCu;
    }
    if (schultz_thorvg_engine_init(0) != SCHULTZ_OK ||
        schultz_image_table_create(&images) != SCHULTZ_OK) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    st->base.images = images;
    schultz_tree_set_image_table(st->base.body.tree, images);
    schultz_image_set_pixels(images, dots, 8u, 8u, 0u, &image);

    schultz_label_create(st->base.body.tree, area, "top", &st->top);
    schultz_label_set_selectable(st->base.body.tree, st->top, 1);
    schultz_node_set_bounds(st->base.body.tree, st->top,
                            schultz_rect_make(0, 0, 300, 20));

    schultz_label_create(st->base.body.tree, area, "middle", &st->middle);
    schultz_label_set_selectable(st->base.body.tree, st->middle, 1);
    schultz_node_set_bounds(st->base.body.tree, st->middle,
                            schultz_rect_make(0, 20, 300, 20));

    schultz_panel_create(st->base.body.tree, area, &st->row);
    schultz_node_set_bounds(st->base.body.tree, st->row,
                            schultz_rect_make(0, 40, 300, 40));
    schultz_icon_create(st->base.body.tree, st->row, image, &st->left);
    /* Inside the row, so these are relative to it and not to the area. */
    schultz_node_set_bounds(st->base.body.tree, st->left,
                            schultz_rect_make(0, 0, 40, 40));
    schultz_icon_set_selectable(st->base.body.tree, st->left, 1);
    schultz_icon_create(st->base.body.tree, st->row, image, &st->right);
    schultz_node_set_bounds(st->base.body.tree, st->right,
                            schultz_rect_make(40, 0, 40, 40));
    schultz_icon_set_selectable(st->base.body.tree, st->right, 1);

    schultz_label_create(st->base.body.tree, area, "bottom", &st->bottom);
    schultz_label_set_selectable(st->base.body.tree, st->bottom, 1);
    schultz_node_set_bounds(st->base.body.tree, st->bottom,
                            schultz_rect_make(0, 80, 300, 20));

    /* A note under it all, which takes no part: the demo has one. */
    schultz_label_create(st->base.body.tree, area, "a note", &st->note);
    schultz_node_set_bounds(st->base.body.tree, st->note,
                            schultz_rect_make(0, 100, 300, 40));

    schultz_tree_resolve_styles(st->base.body.tree);
    return SCHULTZ_OK;
}

/*
 * Reported from the demo: a drag downward from the top stopped at the
 * pictures and never reached what was below them, while the same drag upward
 * worked.
 */
TEST a_drag_down_past_pictures_reaches_what_is_below(void)
{
    stack st;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, stack_setup(&st));
    press(&st.base, 2.0f, 10.0f);          /* in "top" */
    drag_to(&st.base, 20.0f, 50.0f);       /* over the left picture */
    drag_to(&st.base, 60.0f, 50.0f);       /* over the right picture */
    drag_to(&st.base, 200.0f, 90.0f);      /* into "bottom" */
    release(&st.base, 200.0f, 90.0f);

    selected(st.base.body.tree, st.top, &low, &high);
    ASSERT(high > low);
    selected(st.base.body.tree, st.middle, &low, &high);
    ASSERT_EQ(0u, low);
    ASSERT_EQ(6u, high);
    selected(st.base.body.tree, st.bottom, &low, &high);
    if (high <= low) {
        printf("      bottom not selected: %u..%u\n", low, high);
    }
    ASSERT(high > low);

    live_teardown(&st.base);
    PASS();
}

/* The same drag the other way round, which was reported as working. */
TEST a_drag_up_past_pictures_reaches_what_is_above(void)
{
    stack st;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, stack_setup(&st));
    press(&st.base, 200.0f, 90.0f);
    drag_to(&st.base, 60.0f, 50.0f);
    drag_to(&st.base, 2.0f, 10.0f);
    release(&st.base, 2.0f, 10.0f);

    selected(st.base.body.tree, st.top, &low, &high);
    ASSERT(high > low);
    selected(st.base.body.tree, st.middle, &low, &high);
    ASSERT_EQ(0u, low);
    ASSERT_EQ(6u, high);
    selected(st.base.body.tree, st.bottom, &low, &high);
    ASSERT(high > low);

    live_teardown(&st.base);
    PASS();
}

/*
 * Reported from the demo: a drag that ends over something that takes no part
 * left everything past the last picture unselected.
 *
 * Dragging below the last paragraph should select to the end of it, which is
 * what every other text selection does. Keeping the end wherever the pointer
 * last met a participant means a drag that overshoots loses whatever it
 * crossed on the way.
 */
TEST a_drag_that_ends_over_nothing_still_reaches_the_last_widget(void)
{
    stack st;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, stack_setup(&st));
    press(&st.base, 2.0f, 10.0f);          /* in "top" */
    drag_to(&st.base, 20.0f, 50.0f);       /* over a picture */
    drag_to(&st.base, 150.0f, 250.0f);     /* past everything, over nothing */
    release(&st.base, 150.0f, 250.0f);

    selected(st.base.body.tree, st.bottom, &low, &high);
    if (high <= low) {
        printf("      bottom left out: %u..%u\n", low, high);
    }
    ASSERT(high > low);

    live_teardown(&st.base);
    PASS();
}

/*
 * Two areas on one page must not both hold a selection. There is one in a
 * window, the same as there is one on a desktop, and starting a new one ends
 * whatever came before it.
 */
TEST starting_a_selection_ends_the_one_in_another_area(void)
{
    live l;
    schultz_handle other = SCHULTZ_HANDLE_NONE;
    schultz_handle far = SCHULTZ_HANDLE_NONE;
    uint32_t low, high;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_create(l.body.tree,
                                            schultz_tree_root(l.body.tree),
                                            &other));
    schultz_node_set_bounds(l.body.tree, other,
                            schultz_rect_make(0, 150, 400, 60));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_create(l.body.tree, other, "elsewhere", &far));
    schultz_label_set_selectable(l.body.tree, far, 1);
    schultz_node_set_bounds(l.body.tree, far,
                            schultz_rect_make(0, 0, 400, 40));
    schultz_tree_resolve_styles(l.body.tree);

    /* A selection in the first area. */
    press(&l, 2.0f, 20.0f);
    drag_to(&l, 380.0f, 60.0f);
    release(&l, 380.0f, 60.0f);
    selected(l.body.tree, l.body.one, &low, &high);
    ASSERT(high > low);

    /* Then one in the second, which must end the first. */
    press(&l, 2.0f, 160.0f);
    drag_to(&l, 200.0f, 170.0f);
    release(&l, 200.0f, 170.0f);

    selected(l.body.tree, far, &low, &high);
    ASSERT(high > low);
    selected(l.body.tree, l.body.one, &low, &high);
    ASSERT_EQ(low, high);
    selected(l.body.tree, l.body.two, &low, &high);
    ASSERT_EQ(low, high);
    /* And the copy now comes from the area that has it. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_copy(l.body.tree, other, NULL));
    ASSERT_STR_EQ("elsewhere", l.board);

    live_teardown(&l);
    PASS();
}

/*
 * What is highlighted and what is copied are the same bytes.
 *
 * They come from the same pair of offsets, and this says so out loud after a
 * real drag rather than after a range set by hand: a mismatch here would show
 * on screen as a copy that starts a character away from the highlight.
 */
TEST what_is_highlighted_is_what_is_copied(void)
{
    live l;
    uint32_t low, high;
    const char *text;
    char expect[32];

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    press(&l, 14.0f, 20.0f);      /* part way into "first" */
    drag_to(&l, 30.0f, 20.0f);
    release(&l, 30.0f, 20.0f);

    selected(l.body.tree, l.body.one, &low, &high);
    ASSERT(high > low);
    ASSERT(high <= 5u);

    text = schultz_selection_area_text(l.body.tree, l.body.area);
    ASSERT(text != NULL);
    memcpy(expect, "first" + low, (size_t)(high - low));
    expect[high - low] = '\0';
    if (strcmp(expect, text) != 0) {
        printf("      highlighted %u..%u is \"%s\", copied \"%s\"\n",
               low, high, expect, text);
    }
    ASSERT_STR_EQ(expect, text);

    live_teardown(&l);
    PASS();
}

/* ---------------------------------------------- through the clipboard */

/* What a paste would fetch: asks the clipboard for one format by name. */
static const void *paste(live *l, const char *format, uint64_t *out_length)
{
    uint32_t i;

    *out_length = 0u;
    if (l->make == NULL) {
        return NULL;
    }
    for (i = 0; i < l->formats; i++) {
        if (strcmp(l->offered[i], format) == 0) {
            return l->make(l->make_context, format, out_length);
        }
    }
    return NULL;
}

/*
 * Copy, then paste, for each format the clipboard was offered.
 *
 * This is the end to end shape a person goes through: select, press the key,
 * and have another application ask for what it can take. Asking through the
 * callback the clipboard was handed is exactly what a paste does, so a
 * format that is offered but cannot be produced shows up here and nowhere
 * else.
 */
TEST a_copy_can_be_pasted_back_as_text_and_as_a_picture(void)
{
    live l;
    schultz_handle icon;
    const void *bytes;
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    icon = add_picture(&l);
    ASSERT(icon != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.three, 0u, icon, 1u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_copy(l.body.tree, l.body.area,
                                          &l.options));

    /* Plain text, which every target can take. */
    bytes = paste(&l, SCHULTZ_CLIPBOARD_TEXT, &length);
    ASSERT(bytes != NULL);
    ASSERT_EQ(5u, (uint32_t)length);
    ASSERT_EQ(0, memcmp(bytes, "third", 5u));

    /* And the markup, which is where the picture is. */
    bytes = paste(&l, SCHULTZ_CLIPBOARD_HTML, &length);
    ASSERT(bytes != NULL);
    ASSERT(strstr((const char *)bytes, "data:image/png;base64,") != NULL);
    ASSERT(strstr((const char *)bytes, ">third</p>") != NULL);

    /*
     * A picture on its own is not offered, because this selection is a
     * document rather than a picture. Handing one out would let a program
     * paste a stray picture where a page was selected.
     */
    ASSERT_EQ(NULL, paste(&l, "image/png", &length));
    ASSERT_EQ(NULL, paste(&l, "image/bmp", &length));

    live_teardown(&l);
    PASS();
}

/*
 * The key a person actually presses has to offer the picture too.
 *
 * This was wrong once: the key handler passed no render options, so a copy
 * from the keyboard could only ever produce text, and a picture in the
 * selection was silently dropped on the way to the clipboard.
 */
TEST control_with_c_offers_the_picture_as_well_as_the_text(void)
{
    live l;
    schultz_handle icon;
    const void *bytes;
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    icon = add_picture(&l);
    ASSERT(icon != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.three, 0u, icon, 1u));
    schultz_tree_set_selection_owner(l.body.tree, l.body.area);

    schultz_events_key(l.events, 'c', SCHULTZ_MOD_CTRL, 1);

    if (l.formats < 2u) {
        printf("      only %u format(s) offered from the key\n", l.formats);
    }
    ASSERT_EQ(2u, l.formats);
    /* The markup is what carries the picture, so that is what to ask for. */
    bytes = paste(&l, SCHULTZ_CLIPBOARD_HTML, &length);
    ASSERT(bytes != NULL);
    ASSERT(strstr((const char *)bytes, "data:image/png;base64,") != NULL);

    live_teardown(&l);
    PASS();
}

/* ------------------------------------------------------------- as markup */

/*
 * The format a word processor takes. Pasting into one was reported as giving
 * only unformatted text, and the reason was that plain text plus a picture is
 * not what such a program wants: it wants a document.
 */
TEST copying_offers_markup_with_the_styles_in_it(void)
{
    live l;
    const char *html;
    const void *bytes;
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.one, 0u,
                                               l.body.three, 5u));
    html = schultz_selection_area_html(l.body.tree, l.body.area);
    ASSERT(html != NULL);

    ASSERT(strstr(html, "<html>") != NULL);
    ASSERT(strstr(html, "charset=") != NULL);
    ASSERT(strstr(html, ">first</p>") != NULL);
    ASSERT(strstr(html, ">second</p>") != NULL);
    ASSERT(strstr(html, ">third</p>") != NULL);
    /* Carrying the style each one resolved to. */
    ASSERT(strstr(html, "font-size:") != NULL);
    ASSERT(strstr(html, "color:#") != NULL);

    /* And it reaches the clipboard, which is where it has to end up. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_copy(l.body.tree, l.body.area, NULL));
    bytes = paste(&l, SCHULTZ_CLIPBOARD_HTML, &length);
    ASSERT(bytes != NULL);
    ASSERT(length > 0u);
    ASSERT_EQ(0, memcmp(bytes, "<html>", 6u));

    live_teardown(&l);
    PASS();
}

/*
 * Characters that mean something to a parser are taken out, or a line of code
 * arrives as broken markup or as nothing at all.
 */
TEST markup_escapes_what_would_be_read_as_tags(void)
{
    live l;
    schultz_handle code = SCHULTZ_HANDLE_NONE;
    const char *html;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_create(l.body.tree, l.body.area,
                                   "a < b && c", &code));
    schultz_label_set_selectable(l.body.tree, code, 1);
    schultz_node_set_bounds(l.body.tree, code,
                            schultz_rect_make(0, 120, 400, 40));
    schultz_tree_resolve_styles(l.body.tree);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               code, 0u, code, 10u));
    html = schultz_selection_area_html(l.body.tree, l.body.area);
    ASSERT(html != NULL);
    ASSERT(strstr(html, "a &lt; b &amp;&amp; c") != NULL);
    /* And no raw angle bracket survived inside the text. */
    ASSERT(strstr(html, "a < b") == NULL);

    live_teardown(&l);
    PASS();
}

/*
 * Every picture goes inside the document rather than beside it, which is what
 * lets a selection holding two pictures arrive as two pictures. The separate
 * picture format can only ever carry one.
 */
TEST markup_carries_every_picture_inside_it(void)
{
    live l;
    schultz_handle first;
    schultz_handle second = SCHULTZ_HANDLE_NONE;
    const char *html;
    const char *at;
    uint32_t found = 0u;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    first = add_picture(&l);
    ASSERT(first != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_icon_create(l.body.tree, l.body.area,
                                  schultz_icon_image(l.body.tree, first),
                                  &second));
    schultz_node_set_bounds(l.body.tree, second,
                            schultz_rect_make(0, 140, 8, 8));
    schultz_icon_set_selectable(l.body.tree, second, 1);
    schultz_tree_resolve_styles(l.body.tree);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.three, 0u, second, 1u));
    html = schultz_selection_area_html(l.body.tree, l.body.area);
    ASSERT(html != NULL);

    for (at = html; (at = strstr(at, "data:image/png;base64,")) != NULL;
         at++) {
        found++;
    }
    if (found != 2u) {
        printf("      %u picture(s) in the markup\n", found);
    }
    ASSERT_EQ(2u, found);
    ASSERT(strstr(html, ">third</p>") != NULL);

    live_teardown(&l);
    PASS();
}

/*
 * A selection that is a picture and nothing else does offer the picture on
 * its own, because then the picture is what was selected. That is the one
 * case where a program pasting it cannot get it wrong.
 */
TEST a_picture_selected_alone_is_offered_as_a_picture(void)
{
    live l;
    schultz_handle icon;
    const void *bytes;
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    icon = add_picture(&l);
    ASSERT(icon != SCHULTZ_HANDLE_NONE);
    /* The picture and no words at all. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               icon, 0u, icon, 1u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_copy(l.body.tree, l.body.area, NULL));

    ASSERT_EQ(3u, l.formats);
    ASSERT_STR_EQ("image/png", l.offered[2]);
    bytes = paste(&l, "image/png", &length);
    ASSERT(bytes != NULL);
    ASSERT_EQ(0x89u, ((const unsigned char *)bytes)[0]);

    live_teardown(&l);
    PASS();
}

/* A span over [start, end) of a label, with nothing else asked for. */
static schultz_span plain_span(uint32_t start, uint32_t end)
{
    schultz_span span;

    memset(&span, 0, sizeof(span));
    span.start = start;
    span.end   = end;
    return span;
}

/* The markup for a selection of the whole of the first label. */
static const char *markup_of_first(live *l)
{
    if (schultz_selection_area_set_range(l->body.tree, l->body.area,
                                         l->body.one, 0u,
                                         l->body.one, 5u) != SCHULTZ_OK) {
        return NULL;
    }
    return schultz_selection_area_html(l->body.tree, l->body.area);
}

TEST markup_carries_each_thing_a_span_says(void)
{
    live l;
    schultz_span span;
    const char *html;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));

    /* "first", with "fir" marked. One style at a time, so a failure names
     * which one stopped being written. */
    span = plain_span(0u, 3u);
    span.bold = 1u;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_set_spans(l.body.tree, l.body.one, &span, 1u));
    html = markup_of_first(&l);
    ASSERT(html != NULL);
    ASSERT(strstr(html, "<strong>fir</strong>st") != NULL);

    span = plain_span(0u, 3u);
    span.italic = 1u;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_set_spans(l.body.tree, l.body.one, &span, 1u));
    ASSERT(strstr(markup_of_first(&l), "<em>fir</em>st") != NULL);

    span = plain_span(0u, 3u);
    span.underline = 1u;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_set_spans(l.body.tree, l.body.one, &span, 1u));
    ASSERT(strstr(markup_of_first(&l), "<u>fir</u>st") != NULL);

    span = plain_span(0u, 3u);
    span.strikethrough = 1u;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_set_spans(l.body.tree, l.body.one, &span, 1u));
    ASSERT(strstr(markup_of_first(&l), "<s>fir</s>st") != NULL);

    span = plain_span(0u, 3u);
    span.color = schultz_color_rgba(0x11u, 0x22u, 0x33u, 255u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_set_spans(l.body.tree, l.body.one, &span, 1u));
    ASSERT(strstr(markup_of_first(&l), "color:#112233;") != NULL);

    span = plain_span(0u, 3u);
    span.background = schultz_color_rgba(0x44u, 0x55u, 0x66u, 255u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_set_spans(l.body.tree, l.body.one, &span, 1u));
    ASSERT(strstr(markup_of_first(&l), "background-color:#445566;") != NULL);

    span = plain_span(0u, 3u);
    span.size = 27.0f;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_set_spans(l.body.tree, l.body.one, &span, 1u));
    ASSERT(strstr(markup_of_first(&l), "font-size:27px;") != NULL);

    live_teardown(&l);
    PASS();
}

TEST a_link_span_becomes_an_anchor(void)
{
    live l;
    schultz_span span;
    const char *html;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    span = plain_span(0u, 5u);
    span.link = "https://example.org/a?b=1&c=2";
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_set_spans(l.body.tree, l.body.one, &span, 1u));

    html = markup_of_first(&l);
    ASSERT(html != NULL);
    /*
     * The ampersand in the address has to be escaped like any other, or the
     * document it lands in is malformed and the link is wrong besides.
     */
    ASSERT(strstr(html, "href=\"https://example.org/a?b=1&amp;c=2\"") != NULL);
    ASSERT(strstr(html, ">first</a>") != NULL);

    live_teardown(&l);
    PASS();
}

TEST markup_wraps_only_what_a_span_covers(void)
{
    live l;
    schultz_span span;
    const char *html;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    /* The middle of "first", so there are plain bytes on both sides. */
    span = plain_span(1u, 3u);
    span.bold = 1u;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_set_spans(l.body.tree, l.body.one, &span, 1u));

    html = markup_of_first(&l);
    ASSERT(html != NULL);
    ASSERT(strstr(html, ">f<strong>ir</strong>st</p>") != NULL);

    live_teardown(&l);
    PASS();
}

TEST a_label_with_no_spans_still_copies_as_it_did(void)
{
    live l;
    const char *html;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    html = markup_of_first(&l);
    ASSERT(html != NULL);
    /* No extra markup round the words when nothing was said about them. */
    ASSERT(strstr(html, ">first</p>") != NULL);
    ASSERT(strstr(html, "<strong>") == NULL);
    ASSERT(strstr(html, "<span style=") == NULL);

    live_teardown(&l);
    PASS();
}

TEST spans_given_in_any_order_produce_the_same_markup(void)
{
    live l;
    schultz_span forward[2];
    schultz_span backward[2];
    char first[512];
    const char *html;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    forward[0] = plain_span(0u, 2u);
    forward[0].bold = 1u;
    forward[1] = plain_span(3u, 5u);
    forward[1].italic = 1u;
    backward[0] = forward[1];
    backward[1] = forward[0];

    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_set_spans(l.body.tree, l.body.one, forward, 2u));
    html = markup_of_first(&l);
    ASSERT(html != NULL);
    ASSERT(strlen(html) < sizeof(first));
    memcpy(first, html, strlen(html) + 1u);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_set_spans(l.body.tree, l.body.one, backward, 2u));
    html = markup_of_first(&l);
    ASSERT(html != NULL);
    /* The document describes the text, so the order the spans arrived in
     * must not be visible in it. */
    ASSERT_EQ(0, strcmp(first, html));

    live_teardown(&l);
    PASS();
}

/* ------------------------------------------------- editable text joining */

/*
 * A text area inside a selection area takes part like anything else. A drag
 * that began outside and passed over it marks its words, and a copy takes
 * them along with whatever they look like.
 */
TEST a_text_area_takes_part_in_a_selection(void)
{
    live l;
    schultz_handle area = SCHULTZ_HANDLE_NONE;
    const char *html;
    uint32_t low = 9u;
    uint32_t high = 9u;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_area_create(l.body.tree, l.body.area, "typed here",
                                       &area));
    schultz_node_set_bounds(l.body.tree, area,
                            schultz_rect_make(0, 120, 300, 40));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.one, 0u, area, 5u));

    /* It was told its share, the same as a label would be. */
    schultz_text_selection(l.body.tree, area, &low, &high);
    ASSERT_EQ(0u, low);
    ASSERT_EQ(5u, high);

    html = schultz_selection_area_html(l.body.tree, l.body.area);
    ASSERT(html != NULL);
    ASSERT(strstr(html, "typed") != NULL);

    live_teardown(&l);
    PASS();
}

TEST a_text_area_copies_with_the_look_of_its_words(void)
{
    live l;
    schultz_handle area = SCHULTZ_HANDLE_NONE;
    schultz_span span;
    const char *html;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_area_create(l.body.tree, l.body.area, "bold plain",
                                       &area));
    schultz_node_set_bounds(l.body.tree, area,
                            schultz_rect_make(0, 120, 300, 40));

    memset(&span, 0, sizeof(span));
    span.start = 0u;
    span.end   = 4u;
    span.bold  = 1u;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_field_set_spans(l.body.tree, area, &span, 1u));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area, area,
                                               0u, area, 10u));
    html = schultz_selection_area_html(l.body.tree, l.body.area);
    ASSERT(html != NULL);
    /* The whole point of joining: the words arrive marked up, not flat. */
    ASSERT(strstr(html, "<strong>bold</strong> plain") != NULL);

    live_teardown(&l);
    PASS();
}

TEST a_masked_field_takes_no_part_in_a_selection(void)
{
    live l;
    schultz_handle field = SCHULTZ_HANDLE_NONE;
    schultz_handle after = SCHULTZ_HANDLE_NONE;
    const char *text;

    ASSERT_EQ(SCHULTZ_OK, live_setup(&l));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_field_create(l.body.tree, l.body.area, "secret",
                                        &field));
    schultz_node_set_bounds(l.body.tree, field,
                            schultz_rect_make(0, 120, 300, 30));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_field_set_mask(l.body.tree, field, 0x2022u));

    /*
     * A block after it, so the range runs right across the masked field
     * rather than stopping in front of it. Without something on the far side
     * this would pass whether the field declined or not.
     */
    after = prose(l.body.tree, l.body.area, "last");
    ASSERT(after != SCHULTZ_HANDLE_NONE);
    schultz_node_set_bounds(l.body.tree, after,
                            schultz_rect_make(0, 160, 300, 30));

    /*
     * Its own copy already refuses to hand over a masked secret, and joining
     * a selection that copies would be a way round that refusal.
     */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(l.body.tree, l.body.area,
                                               l.body.one, 0u, after, 4u));
    text = schultz_selection_area_text(l.body.tree, l.body.area);
    ASSERT(text != NULL);
    /* The far side is in, so the walk really did pass over the field. */
    ASSERT(strstr(text, "last") != NULL);
    ASSERT(strstr(text, "secret") == NULL);

    live_teardown(&l);
    PASS();
}

SUITE(selection)
{
    RUN_TEST(a_text_area_takes_part_in_a_selection);
    RUN_TEST(a_text_area_copies_with_the_look_of_its_words);
    RUN_TEST(a_masked_field_takes_no_part_in_a_selection);
    RUN_TEST(markup_carries_each_thing_a_span_says);
    RUN_TEST(a_link_span_becomes_an_anchor);
    RUN_TEST(markup_wraps_only_what_a_span_covers);
    RUN_TEST(a_label_with_no_spans_still_copies_as_it_did);
    RUN_TEST(spans_given_in_any_order_produce_the_same_markup);
    RUN_TEST(an_area_refuses_what_it_should);
    RUN_TEST(an_area_with_nothing_selected_says_so);
    RUN_TEST(a_range_inside_one_paragraph_selects_only_that_much);
    RUN_TEST(a_range_across_three_selects_part_all_and_part);
    RUN_TEST(a_range_dragged_backwards_selects_the_same_text);
    RUN_TEST(a_backwards_range_inside_one_paragraph_is_sorted_too);
    RUN_TEST(the_ends_come_back_in_reading_order);
    RUN_TEST(clearing_takes_the_selection_off_every_widget);
    RUN_TEST(shrinking_a_range_unselects_what_it_left_behind);
    RUN_TEST(a_label_that_is_not_selectable_takes_no_part);
    RUN_TEST(a_control_inside_an_area_is_not_selected);
    RUN_TEST(an_area_inside_an_area_keeps_its_own_selection);
    RUN_TEST(offsets_past_the_end_are_clamped);
    RUN_TEST(a_drag_across_three_paragraphs_selects_all_three);
    RUN_TEST(a_press_on_its_own_selects_nothing);
    RUN_TEST(moving_without_pressing_selects_nothing);
    RUN_TEST(movement_after_the_release_does_not_extend_it);
    RUN_TEST(copying_gives_the_pieces_in_reading_order);
    RUN_TEST(copying_one_paragraph_has_no_break_in_it);
    RUN_TEST(copying_nothing_is_refused_rather_than_copying_an_empty_string);
    RUN_TEST(control_with_c_copies_the_selection);
    RUN_TEST(a_right_click_keeps_the_selection);
    RUN_TEST(scrolling_keeps_the_selection);
    RUN_TEST(a_press_on_nothing_clears_the_selection);
    RUN_TEST(a_second_drag_replaces_the_first);
    RUN_TEST(a_picture_is_not_selectable_until_it_is_asked_to_be);
    RUN_TEST(a_selected_picture_comes_out_as_a_png_and_not_as_text);
    RUN_TEST(a_selection_with_no_picture_has_no_picture_to_give);
    RUN_TEST(copying_a_picture_offers_more_than_one_format);
    RUN_TEST(the_budget_starts_at_twenty_megabytes_and_can_be_set);
    RUN_TEST(a_copy_past_the_limit_stops_on_a_block_and_says_so);
    RUN_TEST(a_picture_past_the_limit_is_refused);
    RUN_TEST(a_drag_down_past_pictures_reaches_what_is_below);
    RUN_TEST(a_drag_up_past_pictures_reaches_what_is_above);
    RUN_TEST(a_drag_that_ends_over_nothing_still_reaches_the_last_widget);
    RUN_TEST(starting_a_selection_ends_the_one_in_another_area);
    RUN_TEST(what_is_highlighted_is_what_is_copied);
    RUN_TEST(a_copy_can_be_pasted_back_as_text_and_as_a_picture);
    RUN_TEST(control_with_c_offers_the_picture_as_well_as_the_text);
    RUN_TEST(copying_offers_markup_with_the_styles_in_it);
    RUN_TEST(markup_escapes_what_would_be_read_as_tags);
    RUN_TEST(markup_carries_every_picture_inside_it);
    RUN_TEST(a_picture_selected_alone_is_offered_as_a_picture);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(selection);
    GREATEST_MAIN_END();
}
