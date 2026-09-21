/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_a11y.c - what the toolkit tells a screen reader.
 *
 * Nothing in the suite had ever looked at a published tree before this file.
 * The bridge builds an update and hands it to a platform backend, and every
 * backend that ships talks to a real accessibility service, so there was
 * nowhere to stand and look.
 *
 * This file supplies its own backend. It is the same five functions every
 * platform one has, and because a test binary links no platform backend there
 * is nothing to clash with. The update arrives here, gets copied into plain
 * arrays, and the tests read those.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "schultz_a11y.h"
#include "schultz_a11y_backend.h"
#include "access_tunnel_node_props.h"
#include "schultz_event.h"
#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_selection.h"
#include "schultz_style.h"
#include "schultz_widgets.h"

#define FONT_PATH "assets/fonts/DejaVuSans.ttf"

enum { SEEN_MAX = 256u };

/* One published node, flattened to what a test wants to ask about. */
typedef struct {
    access_tunnel_node_id id;
    access_tunnel_role    role;
    char                  value[128];
    access_tunnel_node_id kids[32];
    size_t                kid_count;
    int32_t               has_selection;
    access_tunnel_text_selection selection;
    /* What a reader would say about how the words look. */
    float                 weight;
    int32_t               italic;
    int32_t               underlined;
    int32_t               struck;
    int32_t               has_ink;
    access_tunnel_color   ink;
    /* What a reader would offer to do with it, and how it stands. */
    int32_t               can_click;
    int32_t               can_focus;
    int32_t               can_step_up;
    int32_t               can_set_value;
    int32_t               disabled;
    int32_t               toggled_on;
    const char           *label;
} seen_node;

/* The last tree the bridge published, kept where the tests can read it. */
static seen_node seen[SEEN_MAX];
static uint32_t  seen_count;
static uint32_t  updates;

struct schultz_a11y_backend {
    int32_t unused;
};

static void remember(const access_tunnel_tree_update *update)
{
    size_t count = access_tunnel_tree_update_node_count(update);
    size_t i;

    seen_count = 0u;
    for (i = 0; i < count && seen_count < SEEN_MAX; i++) {
        access_tunnel_node_id id = 0;
        const access_tunnel_node *node =
            access_tunnel_tree_update_node_at(update, i, &id);
        seen_node *out = &seen[seen_count];
        const char *value;
        const access_tunnel_node_id *kids;
        size_t kid_count = 0u;

        if (node == NULL) {
            continue;
        }
        memset(out, 0, sizeof(*out));
        out->id   = id;
        out->role = access_tunnel_node_role(node);
        value = access_tunnel_node_value(node);
        if (value != NULL) {
            size_t n = strlen(value);

            if (n > sizeof(out->value) - 1u) {
                n = sizeof(out->value) - 1u;
            }
            memcpy(out->value, value, n);
            out->value[n] = '\0';
        }
        kids = access_tunnel_node_children(node, &kid_count);
        if (kids != NULL) {
            if (kid_count > 32u) {
                kid_count = 32u;
            }
            memcpy(out->kids, kids, kid_count * sizeof(*kids));
            out->kid_count = kid_count;
        }
        out->has_selection =
            access_tunnel_node_text_selection(node, &out->selection) ? 1 : 0;
        {
            access_tunnel_text_decoration line;

            access_tunnel_node_font_weight(node, &out->weight);
            out->italic = access_tunnel_node_is_italic(node) ? 1 : 0;
            out->underlined = access_tunnel_node_underline(node, &line) ? 1 : 0;
            out->struck =
                access_tunnel_node_strikethrough(node, &line) ? 1 : 0;
            out->has_ink =
                access_tunnel_node_foreground_color(node, &out->ink) ? 1 : 0;
        }
        out->can_click =
            access_tunnel_node_supports_action(node,
                                               ACCESS_TUNNEL_ACTION_CLICK);
        out->can_focus =
            access_tunnel_node_supports_action(node,
                                               ACCESS_TUNNEL_ACTION_FOCUS);
        out->can_step_up =
            access_tunnel_node_supports_action(node,
                                               ACCESS_TUNNEL_ACTION_INCREMENT);
        out->can_set_value =
            access_tunnel_node_supports_action(node,
                                               ACCESS_TUNNEL_ACTION_SET_VALUE);
        out->disabled   = access_tunnel_node_is_disabled(node) ? 1 : 0;
        {
            access_tunnel_toggled state;

            out->toggled_on =
                (access_tunnel_node_toggled(node, &state) &&
                 state == ACCESS_TUNNEL_TOGGLED_TRUE) ? 1 : 0;
        }
        out->label = access_tunnel_node_label(node);
        seen_count++;
    }
    updates++;
}
/*
 * The way back in. A real backend hands the bridge a callback and calls it
 * when a screen reader asks for something; this keeps the callback so a test
 * can ask for the same things without one.
 */
static schultz_a11y_backend_action_fn asked;
static void *asked_context;

int32_t schultz_a11y_backend_create(
    const schultz_a11y_backend_config *config,
    const access_tunnel_tree_update *initial,
    schultz_a11y_backend **out_backend)
{
    asked         = config->action;
    asked_context = config->action_userdata;
    *out_backend = (schultz_a11y_backend *)calloc(1, sizeof(**out_backend));
    if (*out_backend == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    remember(initial);
    return SCHULTZ_OK;
}

void schultz_a11y_backend_destroy(schultz_a11y_backend *backend)
{
    free(backend);
}

int32_t schultz_a11y_backend_update(schultz_a11y_backend *backend,
                                    const access_tunnel_tree_update *update)
{
    (void)backend;
    remember(update);
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_pump(schultz_a11y_backend *backend)
{
    (void)backend;
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_set_window(schultz_a11y_backend *backend,
                                        schultz_rect window)
{
    (void)backend;
    (void)window;
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_set_focused(schultz_a11y_backend *backend,
                                         int32_t focused)
{
    (void)backend;
    (void)focused;
    return SCHULTZ_OK;
}

/* ------------------------------------------------------------- the fixture */

typedef struct {
    schultz_tree        *tree;
    schultz_events      *events;
    schultz_a11y        *a11y;
    schultz_font_system *fonts;
    schultz_glyph_cache *glyphs;
    schultz_handle       font;
    schultz_theme        theme;
} board;

static int32_t board_setup(board *b)
{
    schultz_a11y_options options;
    int32_t result;

    memset(b, 0, sizeof(*b));
    seen_count = 0u;
    updates    = 0u;

    result = schultz_tree_create(&b->tree);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(b->tree, schultz_rect_make(0, 0, 400, 300));
    /*
     * The root needs a size. Marking a node for repaint adds its rectangle to
     * the dirty area, and a node with no rectangle adds nothing, so a tree
     * where nothing has bounds never looks changed and never republishes.
     */
    schultz_node_set_bounds(b->tree, schultz_tree_root(b->tree),
                            schultz_rect_make(0, 0, 400, 300));
    result = schultz_font_system_create(&b->fonts);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_glyph_cache_create(b->fonts, &b->glyphs);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_font_load_file(b->fonts, FONT_PATH, 16.0f, &b->font);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_font_system(b->tree, b->fonts);
    schultz_theme_init(&b->theme);
    schultz_theme_set_font(&b->theme, SCHULTZ_TOKEN_FONT_BODY, b->font);
    schultz_tree_set_theme(b->tree, &b->theme);

    result = schultz_events_create(b->tree, &b->events);
    if (result != SCHULTZ_OK) {
        return result;
    }
    memset(&options, 0, sizeof(options));
    options.window = schultz_rect_make(0, 0, 400, 300);
    return schultz_a11y_create(b->tree, b->events, &options, &b->a11y);
}

static void board_teardown(board *b)
{
    schultz_a11y_destroy(b->a11y);
    schultz_events_destroy(b->events);
    schultz_tree_destroy(b->tree);
    schultz_glyph_cache_destroy(b->glyphs);
    schultz_font_system_destroy(b->fonts);
}

/*
 * Publishes the tree as it stands, so the tests can look at it.
 *
 * The bridge only rebuilds when something was marked for repaint, which is
 * the right rule for a running window and the wrong one for a test that has
 * just built a tree and wants to see it. Marking the root is how a test says
 * "publish now" without the bridge needing a door for tests to come in by.
 */
static void publish(board *b)
{
    schultz_tree_resolve_styles(b->tree);
    schultz_node_invalidate(b->tree, schultz_tree_root(b->tree));
    schultz_a11y_update(b->a11y);
}

static const seen_node *node_of(access_tunnel_node_id id)
{
    uint32_t i;

    for (i = 0; i < seen_count; i++) {
        if (seen[i].id == id) {
            return &seen[i];
        }
    }
    return NULL;
}

static uint32_t count_of_role(access_tunnel_role role)
{
    uint32_t i;
    uint32_t found = 0u;

    for (i = 0; i < seen_count; i++) {
        if (seen[i].role == role) {
            found++;
        }
    }
    return found;
}

/* ------------------------------------------------------------ the harness */

/*
 * Before anything else: that this file can see a published tree at all. If
 * this fails, nothing below means anything.
 */
TEST the_bridge_publishes_a_tree_this_test_can_read(void)
{
    board b;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    ASSERT(updates > 0u);
    ASSERT(seen_count > 0u);
    /* The window is always there, and the root maps onto it. */
    ASSERT_EQ(1u, count_of_role(ACCESS_TUNNEL_ROLE_WINDOW));

    board_teardown(&b);
    PASS();
}

/* ------------------------------------------------------------- the labels */

/* A caption is read out, and publishes no runs, because there is nothing to
 * move through inside it. */
TEST a_plain_label_is_published_as_its_text(void)
{
    board b;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    const seen_node *node;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_create(b.tree, schultz_tree_root(b.tree),
                                   "A caption", &label));
    schultz_node_set_bounds(b.tree, label, schultz_rect_make(0, 0, 400, 40));
    publish(&b);

    node = node_of((access_tunnel_node_id)label);
    ASSERT(node != NULL);
    ASSERT_EQ(ACCESS_TUNNEL_ROLE_LABEL, node->role);
    ASSERT_STR_EQ("A caption", node->value);
    ASSERT_EQ(0u, count_of_role(ACCESS_TUNNEL_ROLE_TEXT_RUN));

    board_teardown(&b);
    PASS();
}

/*
 * A selectable label publishes its text as runs, so that a reader can move
 * through it a word at a time rather than only hear the whole thing.
 */
TEST a_selectable_label_publishes_its_text_as_runs(void)
{
    board b;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    const seen_node *node;
    const seen_node *run;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_create(b.tree, schultz_tree_root(b.tree),
                                   "Selectable prose", &label));
    schultz_node_set_bounds(b.tree, label, schultz_rect_make(0, 0, 400, 40));
    schultz_label_set_selectable(b.tree, label, 1);
    publish(&b);

    node = node_of((access_tunnel_node_id)label);
    ASSERT(node != NULL);
    ASSERT_EQ(1u, (uint32_t)node->kid_count);

    run = node_of(node->kids[0]);
    ASSERT(run != NULL);
    ASSERT_EQ(ACCESS_TUNNEL_ROLE_TEXT_RUN, run->role);
    ASSERT_STR_EQ("Selectable prose", run->value);

    board_teardown(&b);
    PASS();
}

/*
 * And it carries its own selection, with both ends naming the run beneath it.
 * That is the shape the schema is built for: a position is a run and an index
 * into it, never an offset into some string the reader cannot see.
 */
TEST a_selected_label_publishes_where_the_selection_is(void)
{
    board b;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    const seen_node *node;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_create(b.tree, schultz_tree_root(b.tree),
                                   "Selectable prose", &label));
    schultz_node_set_bounds(b.tree, label, schultz_rect_make(0, 0, 400, 40));
    schultz_label_set_selectable(b.tree, label, 1);
    schultz_label_set_selection(b.tree, label, 0u, 10u);
    publish(&b);

    node = node_of((access_tunnel_node_id)label);
    ASSERT(node != NULL);
    ASSERT_EQ(1, node->has_selection);
    /* Both ends are in the one run this label published. */
    ASSERT_EQ(node->kids[0], node->selection.anchor.node);
    ASSERT_EQ(node->kids[0], node->selection.focus.node);
    ASSERT_EQ(0u, (uint32_t)node->selection.anchor.character_index);
    ASSERT_EQ(10u, (uint32_t)node->selection.focus.character_index);

    board_teardown(&b);
    PASS();
}

/* ------------------------------------------------- across several widgets */

/* Three paragraphs in an area, the shape the selection tests use. */
typedef struct {
    schultz_handle area;
    schultz_handle one;
    schultz_handle two;
    schultz_handle three;
} spread;

static int32_t spread_build(board *b, spread *s)
{
    int32_t result = schultz_selection_area_create(b->tree,
                                                   schultz_tree_root(b->tree),
                                                   &s->area);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_node_set_bounds(b->tree, s->area,
                            schultz_rect_make(0, 0, 400, 300));
    schultz_label_create(b->tree, s->area, "first", &s->one);
    schultz_label_create(b->tree, s->area, "second", &s->two);
    schultz_label_create(b->tree, s->area, "third", &s->three);
    schultz_node_set_bounds(b->tree, s->one,
                            schultz_rect_make(0, 0, 400, 40));
    schultz_node_set_bounds(b->tree, s->two,
                            schultz_rect_make(0, 40, 400, 40));
    schultz_node_set_bounds(b->tree, s->three,
                            schultz_rect_make(0, 80, 400, 40));
    schultz_label_set_selectable(b->tree, s->one, 1);
    schultz_label_set_selectable(b->tree, s->two, 1);
    schultz_label_set_selectable(b->tree, s->three, 1);
    return SCHULTZ_OK;
}

/*
 * The whole point of this half of the work.
 *
 * A selection that runs from one paragraph into another is carried by the
 * area above them, and its two ends name runs inside two different labels.
 * A reader resolving either end against the area finds it by walking up from
 * the run, which is why the container is the right place for it.
 */
TEST an_area_publishes_a_selection_that_spans_two_labels(void)
{
    board b;
    spread s;
    const seen_node *area;
    const seen_node *first;
    const seen_node *third;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    ASSERT_EQ(SCHULTZ_OK, spread_build(&b, &s));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(b.tree, s.area, s.one, 2u,
                                               s.three, 3u));
    publish(&b);

    area  = node_of((access_tunnel_node_id)s.area);
    first = node_of((access_tunnel_node_id)s.one);
    third = node_of((access_tunnel_node_id)s.three);
    ASSERT(area != NULL);
    ASSERT(first != NULL);
    ASSERT(third != NULL);

    ASSERT_EQ(1, area->has_selection);
    /* The two ends are in different labels, which is the thing that could
     * not be expressed before. */
    ASSERT_EQ(first->kids[0], area->selection.anchor.node);
    ASSERT_EQ(third->kids[0], area->selection.focus.node);
    ASSERT(area->selection.anchor.node != area->selection.focus.node);
    ASSERT_EQ(2u, (uint32_t)area->selection.anchor.character_index);
    ASSERT_EQ(3u, (uint32_t)area->selection.focus.character_index);

    board_teardown(&b);
    PASS();
}

/*
 * The runs an area's selection names have to be beneath the area, or nothing
 * resolving them against it will find them. This checks the shape of the
 * tree rather than the numbers: area, then labels, then runs.
 */
TEST the_runs_an_area_names_are_beneath_it(void)
{
    board b;
    spread s;
    const seen_node *area;
    const seen_node *label;
    uint32_t i;
    int32_t found_anchor = 0;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    ASSERT_EQ(SCHULTZ_OK, spread_build(&b, &s));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(b.tree, s.area, s.one, 1u,
                                               s.two, 2u));
    publish(&b);

    area = node_of((access_tunnel_node_id)s.area);
    ASSERT(area != NULL);
    ASSERT_EQ(ACCESS_TUNNEL_ROLE_GROUP, area->role);
    ASSERT_EQ(3u, (uint32_t)area->kid_count);

    /* Every child of the area is a label, and the anchor is a run under one
     * of them. */
    for (i = 0; i < area->kid_count; i++) {
        uint32_t k;

        label = node_of(area->kids[i]);
        ASSERT(label != NULL);
        ASSERT_EQ(ACCESS_TUNNEL_ROLE_LABEL, label->role);
        for (k = 0; k < label->kid_count; k++) {
            if (label->kids[k] == area->selection.anchor.node) {
                found_anchor = 1;
            }
        }
    }
    ASSERT_EQ(1, found_anchor);

    board_teardown(&b);
    PASS();
}

/* With nothing selected the area carries no selection at all, rather than an
 * empty one pointing at nothing. */
TEST an_area_with_no_selection_publishes_none(void)
{
    board b;
    spread s;
    const seen_node *area;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    ASSERT_EQ(SCHULTZ_OK, spread_build(&b, &s));
    publish(&b);

    area = node_of((access_tunnel_node_id)s.area);
    ASSERT(area != NULL);
    ASSERT_EQ(0, area->has_selection);

    board_teardown(&b);
    PASS();
}

/* And clearing takes it away again. */
TEST clearing_a_selection_takes_it_off_the_area(void)
{
    board b;
    spread s;
    const seen_node *area;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    ASSERT_EQ(SCHULTZ_OK, spread_build(&b, &s));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_selection_area_set_range(b.tree, s.area, s.one, 0u,
                                               s.three, 5u));
    publish(&b);
    area = node_of((access_tunnel_node_id)s.area);
    ASSERT(area != NULL);
    ASSERT_EQ(1, area->has_selection);

    ASSERT_EQ(SCHULTZ_OK, schultz_selection_area_clear(b.tree, s.area));
    publish(&b);
    area = node_of((access_tunnel_node_id)s.area);
    ASSERT(area != NULL);
    ASSERT_EQ(0, area->has_selection);

    board_teardown(&b);
    PASS();
}

/* A span over [start, end) with nothing else asked for. */
static schultz_span a11y_span(uint32_t start, uint32_t end)
{
    schultz_span span;

    memset(&span, 0, sizeof(span));
    span.start = start;
    span.end   = end;
    return span;
}

/*
 * A run is a stretch that reads the same throughout. A phrase in another
 * style is a different stretch, so it becomes a run of its own, and the run
 * is what carries "this is bold" to a reader.
 */
TEST a_span_breaks_the_text_into_runs_a_reader_can_tell_apart(void)
{
    board b;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_span span;
    const seen_node *node;
    const seen_node *plain;
    const seen_node *bold;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_create(b.tree, schultz_tree_root(b.tree),
                                   "one two", &label));
    schultz_node_set_bounds(b.tree, label, schultz_rect_make(0, 0, 400, 40));
    schultz_label_set_selectable(b.tree, label, 1);

    /* Without spans the whole line is one run. */
    publish(&b);
    node = node_of((access_tunnel_node_id)label);
    ASSERT(node != NULL);
    ASSERT_EQ(1u, (uint32_t)node->kid_count);

    span = a11y_span(0u, 3u);
    span.bold = 1u;
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(b.tree, label, &span, 1u));
    publish(&b);

    node = node_of((access_tunnel_node_id)label);
    ASSERT(node != NULL);
    ASSERT_EQ(2u, (uint32_t)node->kid_count);

    bold  = node_of(node->kids[0]);
    plain = node_of(node->kids[1]);
    ASSERT(bold != NULL);
    ASSERT(plain != NULL);
    ASSERT_STR_EQ("one", bold->value);
    ASSERT_STR_EQ(" two", plain->value);

    /* Seven hundred is bold on the scale the property is defined on, and
     * the rest of the line says nothing about its weight. */
    ASSERT_EQ(700.0f, bold->weight);
    ASSERT_EQ(0.0f, plain->weight);

    board_teardown(&b);
    PASS();
}

TEST a_run_says_how_its_words_look(void)
{
    board b;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_span span;
    const seen_node *node;
    const seen_node *run;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_label_create(b.tree, schultz_tree_root(b.tree),
                                   "one two", &label));
    schultz_node_set_bounds(b.tree, label, schultz_rect_make(0, 0, 400, 40));
    schultz_label_set_selectable(b.tree, label, 1);

    span = a11y_span(0u, 3u);
    span.italic        = 1u;
    span.underline     = 1u;
    span.strikethrough = 1u;
    span.color = schultz_color_rgba(0x12u, 0x34u, 0x56u, 255u);
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_spans(b.tree, label, &span, 1u));
    publish(&b);

    node = node_of((access_tunnel_node_id)label);
    ASSERT(node != NULL);
    ASSERT_EQ(2u, (uint32_t)node->kid_count);
    run = node_of(node->kids[0]);
    ASSERT(run != NULL);

    ASSERT_EQ(1, run->italic);
    ASSERT_EQ(1, run->underlined);
    ASSERT_EQ(1, run->struck);
    ASSERT_EQ(1, run->has_ink);
    ASSERT_EQ(0x12u, (uint32_t)run->ink.red);
    ASSERT_EQ(0x34u, (uint32_t)run->ink.green);
    ASSERT_EQ(0x56u, (uint32_t)run->ink.blue);

    /* And the stretch that was not marked says none of it. */
    run = node_of(node->kids[1]);
    ASSERT(run != NULL);
    ASSERT_EQ(0, run->italic);
    ASSERT_EQ(0, run->underlined);
    ASSERT_EQ(0, run->struck);
    ASSERT_EQ(0, run->has_ink);

    board_teardown(&b);
    PASS();
}

/* ------------------------------------------------ what a reader is told */

/*
 * The mapping from a Schultz widget to the role a reader announces. The
 * tunnel's own tests cover what a role means; what only this project can
 * check is that its widgets pick the right ones. A button announced as a
 * generic container is a button nobody can find.
 */
TEST every_widget_publishes_the_role_it_is(void)
{
    board b;
    schultz_handle root;
    struct { schultz_handle node; access_tunnel_role want; const char *what; }
        cases[10];
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    root = schultz_tree_root(b.tree);

    memset(cases, 0, sizeof(cases));
    schultz_button_create(b.tree, root, "press", &cases[0].node);
    cases[0].want = ACCESS_TUNNEL_ROLE_BUTTON;
    cases[0].what = "button";
    schultz_checkbox_create(b.tree, root, "tick", &cases[1].node);
    cases[1].want = ACCESS_TUNNEL_ROLE_CHECK_BOX;
    cases[1].what = "check box";
    schultz_radio_create(b.tree, root, "one", 1u, &cases[2].node);
    cases[2].want = ACCESS_TUNNEL_ROLE_RADIO_BUTTON;
    cases[2].what = "radio button";
    schultz_text_field_create(b.tree, root, "type", &cases[3].node);
    cases[3].want = ACCESS_TUNNEL_ROLE_TEXT_INPUT;
    cases[3].what = "text field";
    schultz_slider_create(b.tree, root, SCHULTZ_ORIENT_HORIZONTAL,
                          0.0f, 10.0f, 5.0f, &cases[4].node);
    cases[4].want = ACCESS_TUNNEL_ROLE_SLIDER;
    cases[4].what = "slider";
    schultz_progress_bar_create(b.tree, root,
                                SCHULTZ_ORIENT_HORIZONTAL,
                                &cases[5].node);
    cases[5].want = ACCESS_TUNNEL_ROLE_PROGRESS_INDICATOR;
    cases[5].what = "progress bar";
    schultz_label_create(b.tree, root, "words", &cases[6].node);
    cases[6].want = ACCESS_TUNNEL_ROLE_LABEL;
    cases[6].what = "label";
    schultz_list_view_create(b.tree, root, &cases[7].node);
    cases[7].want = ACCESS_TUNNEL_ROLE_LIST;
    cases[7].what = "list view";
    schultz_scroll_view_create(b.tree, root, &cases[8].node);
    cases[8].want = ACCESS_TUNNEL_ROLE_SCROLL_VIEW;
    cases[8].what = "scroll view";
    schultz_password_field_create(b.tree, root, "secret", &cases[9].node);
    cases[9].want = ACCESS_TUNNEL_ROLE_PASSWORD_INPUT;
    cases[9].what = "password field";

    for (i = 0u; i < 10u; i++) {
        ASSERT(cases[i].node != SCHULTZ_HANDLE_NONE);
        schultz_node_set_bounds(b.tree, cases[i].node,
                                schultz_rect_make(0, (float)(i * 20u), 200,
                                                  18));
    }
    publish(&b);

    for (i = 0u; i < 10u; i++) {
        const seen_node *out = node_of((access_tunnel_node_id)cases[i].node);

        ASSERT(out != NULL);
        if (out->role != cases[i].want) {
            FAILm(cases[i].what);
        }
    }

    board_teardown(&b);
    PASS();
}

TEST a_node_with_no_role_is_walked_through_silently(void)
{
    board b;
    schultz_handle bare = SCHULTZ_HANDLE_NONE;
    schultz_handle panel = SCHULTZ_HANDLE_NONE;
    const seen_node *out;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_create(b.tree, schultz_tree_root(b.tree), &bare));
    schultz_node_set_bounds(b.tree, bare, schultz_rect_make(0, 0, 100, 40));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_panel_create(b.tree, schultz_tree_root(b.tree),
                                   &panel));
    schultz_node_set_bounds(b.tree, panel, schultz_rect_make(0, 50, 100, 40));
    publish(&b);

    /*
     * A node nobody classified is a box put there for layout. Unknown is a
     * role a reader announces; a generic container is one it walks through
     * without saying anything, which is what a layout box deserves.
     */
    out = node_of((access_tunnel_node_id)bare);
    ASSERT(out != NULL);
    ASSERT_EQ(ACCESS_TUNNEL_ROLE_GENERIC_CONTAINER, out->role);

    /* A Panel is not that: it asks to be a group, and is announced as one. */
    out = node_of((access_tunnel_node_id)panel);
    ASSERT(out != NULL);
    ASSERT_EQ(ACCESS_TUNNEL_ROLE_GROUP, out->role);

    board_teardown(&b);
    PASS();
}

TEST a_widget_says_what_can_be_done_with_it(void)
{
    board b;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    schultz_handle slider = SCHULTZ_HANDLE_NONE;
    schultz_handle words = SCHULTZ_HANDLE_NONE;
    const seen_node *out;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    schultz_button_create(b.tree, schultz_tree_root(b.tree), "press", &button);
    schultz_slider_create(b.tree, schultz_tree_root(b.tree),
                          SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 10.0f,
                          5.0f, &slider);
    schultz_label_create(b.tree, schultz_tree_root(b.tree), "words", &words);
    schultz_node_set_bounds(b.tree, button, schultz_rect_make(0, 0, 100, 30));
    schultz_node_set_bounds(b.tree, slider, schultz_rect_make(0, 40, 100, 30));
    schultz_node_set_bounds(b.tree, words, schultz_rect_make(0, 80, 100, 30));
    publish(&b);

    /* A button can be pressed and focused. */
    out = node_of((access_tunnel_node_id)button);
    ASSERT(out != NULL);
    ASSERT_EQ(1, out->can_click);
    ASSERT_EQ(1, out->can_focus);
    ASSERT_EQ(0, out->can_step_up);

    /* A slider can be stepped and set. */
    out = node_of((access_tunnel_node_id)slider);
    ASSERT(out != NULL);
    ASSERT_EQ(1, out->can_step_up);
    ASSERT_EQ(1, out->can_set_value);

    /* Static text can have none of it done to it. */
    out = node_of((access_tunnel_node_id)words);
    ASSERT(out != NULL);
    ASSERT_EQ(0, out->can_click);
    ASSERT_EQ(0, out->can_step_up);

    board_teardown(&b);
    PASS();
}

TEST a_disabled_control_is_published_as_disabled(void)
{
    board b;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    const seen_node *out;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    schultz_button_create(b.tree, schultz_tree_root(b.tree), "press", &button);
    schultz_node_set_bounds(b.tree, button, schultz_rect_make(0, 0, 100, 30));
    publish(&b);
    out = node_of((access_tunnel_node_id)button);
    ASSERT(out != NULL);
    ASSERT_EQ(0, out->disabled);

    /* Greyed out on the screen has to be greyed out to a reader as well, or
     * it is announced as something that can be pressed and cannot. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_state(b.tree, button, SCHULTZ_STATE_VISIBLE));
    publish(&b);
    out = node_of((access_tunnel_node_id)button);
    ASSERT(out != NULL);
    ASSERT_EQ(1, out->disabled);

    board_teardown(&b);
    PASS();
}

TEST a_ticked_box_is_published_as_ticked(void)
{
    board b;
    schultz_handle box = SCHULTZ_HANDLE_NONE;
    const seen_node *out;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    schultz_checkbox_create(b.tree, schultz_tree_root(b.tree), "tick",
                            &box);
    schultz_node_set_bounds(b.tree, box, schultz_rect_make(0, 0, 100, 30));
    publish(&b);
    out = node_of((access_tunnel_node_id)box);
    ASSERT(out != NULL);
    ASSERT_EQ(0, out->toggled_on);

    ASSERT_EQ(SCHULTZ_OK, schultz_toggle_set_checked(b.tree, box, 1));
    publish(&b);
    out = node_of((access_tunnel_node_id)box);
    ASSERT(out != NULL);
    ASSERT_EQ(1, out->toggled_on);

    board_teardown(&b);
    PASS();
}

/* ------------------------------------------------- what a reader asks for */

/* Asks the bridge for something, the way a screen reader would. */
static uint32_t ask(board *b, schultz_handle node,
                    access_tunnel_action action)
{
    access_tunnel_action_request request;

    memset(&request, 0, sizeof(request));
    request.target_node = (access_tunnel_node_id)node;
    request.action      = action;
    asked(&request, asked_context);
    return schultz_a11y_drain(b->a11y);
}

TEST pressing_a_button_through_the_bridge_presses_it(void)
{
    board b;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    const schultz_event *queued = NULL;
    uint32_t count = 0u;
    uint32_t i;
    int32_t pressed = 0;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_queue(b.events, 1));
    schultz_button_create(b.tree, schultz_tree_root(b.tree), "press", &button);
    schultz_node_set_bounds(b.tree, button, schultz_rect_make(0, 0, 100, 30));
    publish(&b);

    /*
     * The way in, not the way out. Everything else here checks what a reader
     * is told; this checks that what a reader asks for actually happens.
     */
    ASSERT_EQ(1u, ask(&b, button, ACCESS_TUNNEL_ACTION_CLICK));

    ASSERT_EQ(SCHULTZ_OK, schultz_events_drain(b.events, &queued, &count));
    for (i = 0u; i < count; i++) {
        if (queued[i].type == SCHULTZ_EVENT_CLICK &&
            queued[i].target == button) {
            pressed = 1;
        }
    }
    ASSERT_EQ(1, pressed);

    board_teardown(&b);
    PASS();
}

TEST stepping_a_slider_through_the_bridge_moves_it(void)
{
    board b;
    schultz_handle slider = SCHULTZ_HANDLE_NONE;
    float before;
    float after;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    schultz_slider_create(b.tree, schultz_tree_root(b.tree),
                          SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 10.0f,
                          5.0f, &slider);
    schultz_slider_set_value(b.tree, slider, 5.0f);
    schultz_node_set_bounds(b.tree, slider, schultz_rect_make(0, 0, 100, 30));
    publish(&b);

    before = schultz_slider_value(b.tree, slider);
    ASSERT_EQ(1u, ask(&b, slider, ACCESS_TUNNEL_ACTION_INCREMENT));
    after = schultz_slider_value(b.tree, slider);
    ASSERT(after > before);

    ASSERT_EQ(1u, ask(&b, slider, ACCESS_TUNNEL_ACTION_DECREMENT));
    ASSERT_IN_RANGE(before, schultz_slider_value(b.tree, slider), 0.01f);

    board_teardown(&b);
    PASS();
}

TEST setting_a_number_through_the_bridge_sets_the_slider(void)
{
    board b;
    schultz_handle slider = SCHULTZ_HANDLE_NONE;
    access_tunnel_action_request request;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    schultz_slider_create(b.tree, schultz_tree_root(b.tree),
                          SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 10.0f,
                          5.0f, &slider);
    schultz_slider_set_value(b.tree, slider, 1.0f);
    schultz_node_set_bounds(b.tree, slider, schultz_rect_make(0, 0, 100, 30));
    publish(&b);

    memset(&request, 0, sizeof(request));
    request.target_node        = (access_tunnel_node_id)slider;
    request.action             = ACCESS_TUNNEL_ACTION_SET_VALUE;
    request.has_data           = true;
    request.data.kind          = ACCESS_TUNNEL_ACTION_DATA_NUMERIC_VALUE;
    request.data.as.numeric_value = 7.0;
    asked(&request, asked_context);
    ASSERT_EQ(1u, schultz_a11y_drain(b.a11y));

    ASSERT_IN_RANGE(7.0f, schultz_slider_value(b.tree, slider), 0.01f);

    board_teardown(&b);
    PASS();
}

TEST setting_text_through_the_bridge_sets_the_field(void)
{
    board b;
    schultz_handle field = SCHULTZ_HANDLE_NONE;
    access_tunnel_action_request request;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    schultz_text_field_create(b.tree, schultz_tree_root(b.tree), "before",
                              &field);
    schultz_node_set_bounds(b.tree, field, schultz_rect_make(0, 0, 100, 30));
    publish(&b);

    memset(&request, 0, sizeof(request));
    request.target_node  = (access_tunnel_node_id)field;
    request.action       = ACCESS_TUNNEL_ACTION_SET_VALUE;
    request.has_data     = true;
    request.data.kind    = ACCESS_TUNNEL_ACTION_DATA_VALUE;
    request.data.as.value = "after";
    asked(&request, asked_context);
    ASSERT_EQ(1u, schultz_a11y_drain(b.a11y));

    ASSERT_STR_EQ("after", schultz_text_get(b.tree, field));

    board_teardown(&b);
    PASS();
}

TEST a_value_aimed_at_something_that_takes_none_is_ignored(void)
{
    board b;
    schultz_handle words = SCHULTZ_HANDLE_NONE;
    access_tunnel_action_request request;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    schultz_label_create(b.tree, schultz_tree_root(b.tree), "words", &words);
    schultz_node_set_bounds(b.tree, words, schultz_rect_make(0, 0, 100, 30));
    publish(&b);

    /*
     * Static text advertises no set value action, so a request naming it is
     * refused rather than acted on. Otherwise anything on the screen could be
     * rewritten from outside.
     */
    memset(&request, 0, sizeof(request));
    request.target_node  = (access_tunnel_node_id)words;
    request.action       = ACCESS_TUNNEL_ACTION_SET_VALUE;
    request.has_data     = true;
    request.data.kind    = ACCESS_TUNNEL_ACTION_DATA_VALUE;
    request.data.as.value = "rewritten";
    asked(&request, asked_context);
    ASSERT_EQ(1u, schultz_a11y_drain(b.a11y));

    ASSERT_STR_EQ("words", schultz_node_get_name(b.tree, words));

    board_teardown(&b);
    PASS();
}

TEST an_action_this_toolkit_does_not_offer_is_dropped(void)
{
    board b;
    schultz_handle button = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, board_setup(&b));
    schultz_button_create(b.tree, schultz_tree_root(b.tree), "press", &button);
    schultz_node_set_bounds(b.tree, button, schultz_rect_make(0, 0, 100, 30));
    publish(&b);

    /* Nothing is queued, so nothing is drained, and nothing happens. */
    ASSERT_EQ(0u, ask(&b, button, ACCESS_TUNNEL_ACTION_EXPAND));

    board_teardown(&b);
    PASS();
}

SUITE(a11y)
{
    RUN_TEST(every_widget_publishes_the_role_it_is);
    RUN_TEST(a_node_with_no_role_is_walked_through_silently);
    RUN_TEST(a_widget_says_what_can_be_done_with_it);
    RUN_TEST(a_disabled_control_is_published_as_disabled);
    RUN_TEST(a_ticked_box_is_published_as_ticked);
    RUN_TEST(pressing_a_button_through_the_bridge_presses_it);
    RUN_TEST(stepping_a_slider_through_the_bridge_moves_it);
    RUN_TEST(setting_a_number_through_the_bridge_sets_the_slider);
    RUN_TEST(setting_text_through_the_bridge_sets_the_field);
    RUN_TEST(a_value_aimed_at_something_that_takes_none_is_ignored);
    RUN_TEST(an_action_this_toolkit_does_not_offer_is_dropped);
    RUN_TEST(the_bridge_publishes_a_tree_this_test_can_read);
    RUN_TEST(a_plain_label_is_published_as_its_text);
    RUN_TEST(a_selectable_label_publishes_its_text_as_runs);
    RUN_TEST(a_span_breaks_the_text_into_runs_a_reader_can_tell_apart);
    RUN_TEST(a_run_says_how_its_words_look);
    RUN_TEST(a_selected_label_publishes_where_the_selection_is);
    RUN_TEST(an_area_publishes_a_selection_that_spans_two_labels);
    RUN_TEST(the_runs_an_area_names_are_beneath_it);
    RUN_TEST(an_area_with_no_selection_publishes_none);
    RUN_TEST(clearing_a_selection_takes_it_off_the_area);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(a11y);
    GREATEST_MAIN_END();
}
