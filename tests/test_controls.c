/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_controls.c - the tier 2 controls: button, checkbox, radio, switch,
 * slider and progress bar.
 *
 * Two seams are used together here. The fake host callback says what a host
 * would have been told, and the draw list says what was painted, so a control
 * is asserted on both what it does and what it looks like while doing it.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_widgets.h"

#define FONT_PATH "assets/fonts/DejaVuSans.ttf"

/* ------------------------------------------------------------- fixture */

enum { SEEN_MAX = 64 };

/** Records what the host was told, so behaviour can be asserted. */
typedef struct {
    uint32_t       types[SEEN_MAX];
    schultz_handle targets[SEEN_MAX];
    uint32_t       count;
} seen_events;

typedef struct {
    schultz_tree        *tree;
    schultz_events      *events;
    schultz_arena        arena;
    schultz_draw_list    list;
    schultz_font_system *fonts;
    schultz_glyph_cache *glyphs;
    schultz_handle       font;
    schultz_theme        theme;
    seen_events          seen;
} control_fixture;

static int32_t record_callback(void *context, const schultz_event *event)
{
    seen_events *seen = (seen_events *)context;

    if (seen->count < SEEN_MAX) {
        seen->types[seen->count]   = event->type;
        seen->targets[seen->count] = event->target;
    }
    seen->count++;
    return SCHULTZ_OK;
}

static int32_t seen_count(const seen_events *seen, uint32_t type,
                          schultz_handle target)
{
    uint32_t i;
    int32_t n = 0;

    for (i = 0; i < seen->count && i < SEEN_MAX; i++) {
        if (seen->types[i] == type && seen->targets[i] == target) {
            n++;
        }
    }
    return n;
}

static int32_t fixture_setup(control_fixture *f)
{
    int32_t result;

    memset(f, 0, sizeof(*f));
    result = schultz_tree_create(&f->tree);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(f->tree, schultz_rect_make(0, 0, 800, 600));

    result = schultz_font_system_create(&f->fonts);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_glyph_cache_create(f->fonts, &f->glyphs);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_font_load_file(f->fonts, FONT_PATH, 16.0f, &f->font);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_font_system(f->tree, f->fonts);

    /*
     * The controls draw text through the theme's body font slot, the way an
     * application would set it up, rather than by naming a font per label.
     */
    schultz_theme_init(&f->theme);
    schultz_theme_set_font(&f->theme, SCHULTZ_TOKEN_FONT_BODY, f->font);
    schultz_tree_set_theme(f->tree, &f->theme);

    result = schultz_events_create(f->tree, &f->events);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_events_set_callback(f->events, record_callback, &f->seen);

    result = schultz_arena_init(&f->arena, 0);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_draw_list_init(&f->list, &f->arena, 0);
}

static void fixture_teardown(control_fixture *f)
{
    schultz_arena_free(&f->arena);
    schultz_events_destroy(f->events);
    schultz_tree_destroy(f->tree);
    schultz_glyph_cache_destroy(f->glyphs);
    schultz_font_system_destroy(f->fonts);
}

static uint32_t paint_all(control_fixture *f)
{
    schultz_arena_reset(&f->arena);
    schultz_draw_list_init(&f->list, &f->arena, 0);
    schultz_tree_resolve_styles(f->tree);
    schultz_widget_paint_tree(f->tree, &f->list, &f->arena,
                              schultz_rect_make(0, 0, 0, 0));
    return schultz_draw_list_count(&f->list);
}

static uint32_t count_kind(control_fixture *f, uint32_t kind)
{
    uint32_t i;
    uint32_t n = 0;

    for (i = 0; i < schultz_draw_list_count(&f->list); i++) {
        if (schultz_draw_list_at(&f->list, i)->kind == kind) {
            n++;
        }
    }
    return n;
}

/* The colour of the first command of a kind, or fully transparent. */
static schultz_color first_fill(control_fixture *f, uint32_t kind)
{
    uint32_t i;

    for (i = 0; i < schultz_draw_list_count(&f->list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f->list, i);

        if (cmd->kind != kind) {
            continue;
        }
        switch (kind) {
        case SCHULTZ_DRAW_FILL_RECT:       return cmd->as.fill_rect.paint.as.color;
        case SCHULTZ_DRAW_FILL_ROUND_RECT: return cmd->as.fill_round_rect.paint.as.color;
        case SCHULTZ_DRAW_FILL_ELLIPSE:    return cmd->as.fill_ellipse.paint.as.color;
        default:                           break;
        }
    }
    return schultz_color_rgba(0, 0, 0, 0);
}

/* How many glyphs the whole draw list holds, across every run in it. */
static uint32_t glyphs_drawn(control_fixture *f)
{
    uint32_t i;
    uint32_t n = 0;

    for (i = 0; i < schultz_draw_list_count(&f->list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f->list, i);

        if (cmd->kind == SCHULTZ_DRAW_GLYPH_RUN) {
            n += cmd->as.glyph_run.count;
        }
    }
    return n;
}

/* The x of the rightmost glyph drawn, or zero when none was. */
static float rightmost_glyph(control_fixture *f)
{
    float right = 0.0f;
    uint32_t i;

    for (i = 0; i < schultz_draw_list_count(&f->list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f->list, i);
        uint32_t g;

        if (cmd->kind != SCHULTZ_DRAW_GLYPH_RUN) {
            continue;
        }
        for (g = 0; g < cmd->as.glyph_run.count; g++) {
            if (cmd->as.glyph_run.glyphs[g].x > right) {
                right = cmd->as.glyph_run.glyphs[g].x;
            }
        }
    }
    return right;
}

/* Clicks a node: press and release at the same point inside it. */
static void click_at(control_fixture *f, schultz_point point)
{
    schultz_events_mouse_move(f->events, point, 0);
    schultz_events_mouse_button(f->events, point, SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_button(f->events, point, SCHULTZ_BUTTON_LEFT, 0, 0);
}

static schultz_point centre_of(control_fixture *f, schultz_handle node)
{
    schultz_rect b;

    schultz_node_absolute_bounds(f->tree, node, &b);
    return schultz_point_make(b.x + b.width * 0.5f, b.y + b.height * 0.5f);
}

/* Lays a node out at a position, the way a real pane would. */
static void place(control_fixture *f, schultz_handle node, float x, float y)
{
    schultz_size size;

    schultz_tree_resolve_styles(f->tree);
    schultz_layout_measure(f->tree, node, -1.0f, -1.0f, &size);
    schultz_layout_arrange(f->tree, node,
                           schultz_rect_make(x, y, size.width, size.height));
}

/* --------------------------------------------------------------- Button */

TEST a_button_carries_its_caption_and_schema(void)
{
    control_fixture f;
    schultz_handle button;
    schultz_handle label;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_button_create(f.tree,
                    schultz_tree_root(f.tree), "Save", &button));

    label = schultz_button_label(f.tree, button);
    ASSERT(label != SCHULTZ_HANDLE_NONE);
    ASSERT_STR_EQ("Save", schultz_label_text(f.tree, label));
    ASSERT_STR_EQ("Save", schultz_node_get_name(f.tree, button));
    ASSERT_EQ((uint32_t)SCHULTZ_ROLE_BUTTON,
              schultz_node_get_role(f.tree, button));
    ASSERT_EQ((uint32_t)(SCHULTZ_ACTION_CLICK | SCHULTZ_ACTION_FOCUS),
              schultz_node_get_actions(f.tree, button));

    /* Not a button, so no label. */
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_button_label(f.tree, label));

    fixture_teardown(&f);
    PASS();
}

TEST every_button_hands_back_its_caption(void)
{
    control_fixture f;
    schultz_handle plain = SCHULTZ_HANDLE_NONE;
    schultz_handle toggle = SCHULTZ_HANDLE_NONE;
    schultz_handle menu = SCHULTZ_HANDLE_NONE;
    schultz_handle split = SCHULTZ_HANDLE_NONE;
    schultz_handle panel = SCHULTZ_HANDLE_NONE;
    schultz_handle root;

    /*
     * A toggle button and a menu button are made by schultz_button_create and
     * then given a vtable of their own. They keep the caption the button gave
     * them, so the accessor has to keep answering for them: it is the only
     * way to reach a caption, and the toolkit uses it that way itself.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    root = schultz_tree_root(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_button_create(f.tree, root, "One", &plain));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_toggle_button_create(f.tree, root, "Two", 0u, &toggle));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_menu_button_create(f.tree, root, "Three", &menu));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_split_menu_button_create(f.tree, root, "Four", &split));

    ASSERT_STR_EQ("One",
        schultz_label_text(f.tree, schultz_button_label(f.tree, plain)));
    ASSERT_STR_EQ("Two",
        schultz_label_text(f.tree, schultz_button_label(f.tree, toggle)));
    ASSERT_STR_EQ("Three",
        schultz_label_text(f.tree, schultz_button_label(f.tree, menu)));
    ASSERT_STR_EQ("Four",
        schultz_label_text(f.tree, schultz_button_label(f.tree, split)));

    /* And the caption can be changed through it, which is what it is for. */
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_text(f.tree,
                  schultz_button_label(f.tree, menu), "Renamed"));
    ASSERT_STR_EQ("Renamed",
        schultz_label_text(f.tree, schultz_button_label(f.tree, menu)));

    /* Anything that is not one of them still answers nothing. */
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree, root, &panel));
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_button_label(f.tree, panel));
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_button_label(f.tree, root));

    fixture_teardown(&f);
    PASS();
}

TEST a_button_sizes_itself_around_its_caption(void)
{
    control_fixture f;
    schultz_handle small;
    schultz_handle big;
    schultz_size a;
    schultz_size b;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "OK", &small);
    schultz_button_create(f.tree, schultz_tree_root(f.tree),
                          "Save all the things", &big);
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, small, -1, -1, &a));
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, big, -1, -1, &b));

    ASSERT(b.width > a.width);
    /* Padding on both sides, so the box is taller than the text. */
    ASSERT(a.height > 0.0f);

    fixture_teardown(&f);
    PASS();
}

TEST a_button_paints_a_box_and_its_caption(void)
{
    control_fixture f;
    schultz_handle button;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Go", &button);
    place(&f, button, 20.0f, 30.0f);

    paint_all(&f);
    /* Fill, border, and the caption. */
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_FILL_ROUND_RECT));
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_STROKE_ROUND_RECT));
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_GLYPH_RUN));

    fixture_teardown(&f);
    PASS();
}

TEST a_caption_wider_than_its_button_is_cut_short(void)
{
    control_fixture f;
    schultz_handle button;
    schultz_rect wanted;
    schultz_rect squeezed;
    uint32_t whole;
    uint32_t cut;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree),
                          "Save all the things", &button);
    place(&f, button, 20.0f, 30.0f);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_absolute_bounds(f.tree, button, &wanted));

    paint_all(&f);
    whole = glyphs_drawn(&f);
    ASSERT(whole > 0u);

    /* Half the width the caption asked for, which is what a squeezed row
     * or a fixed size hands a button. */
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_arrange(f.tree, button,
        schultz_rect_make(wanted.x, wanted.y, wanted.width * 0.5f,
                          wanted.height));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_absolute_bounds(f.tree, button, &squeezed));

    paint_all(&f);
    cut = glyphs_drawn(&f);

    /* Fewer letters, and the ellipsis in place of the ones dropped. */
    ASSERT(cut > 0u);
    ASSERT(cut < whole);
    /* Nothing reaches past the button, and the caption is clipped as well,
     * so a glyph the shaper left fractionally wide cannot escape either. */
    ASSERT(rightmost_glyph(&f) < squeezed.x + squeezed.width);
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_CLIP_BEGIN));

    fixture_teardown(&f);
    PASS();
}

TEST a_button_wide_enough_for_its_caption_cuts_nothing(void)
{
    control_fixture f;
    schultz_handle plain;
    schultz_handle spacious;
    schultz_rect bounds;
    uint32_t whole;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Save", &plain);
    place(&f, plain, 20.0f, 30.0f);
    paint_all(&f);
    whole = glyphs_drawn(&f);
    fixture_teardown(&f);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Save",
                          &spacious);
    place(&f, spacious, 20.0f, 30.0f);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_absolute_bounds(f.tree, spacious, &bounds));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_arrange(f.tree, spacious,
        schultz_rect_make(bounds.x, bounds.y, bounds.width * 4.0f,
                          bounds.height));

    paint_all(&f);
    ASSERT_EQ(whole, glyphs_drawn(&f));

    fixture_teardown(&f);
    PASS();
}

TEST a_button_changes_colour_when_hovered_and_pressed(void)
{
    control_fixture f;
    schultz_handle button;
    schultz_color plain;
    schultz_color hovered;
    schultz_color pressed;
    schultz_point at;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Go", &button);
    place(&f, button, 20.0f, 30.0f);
    at = centre_of(&f, button);

    paint_all(&f);
    plain = first_fill(&f, SCHULTZ_DRAW_FILL_ROUND_RECT);

    schultz_events_mouse_move(f.events, at, 0);
    paint_all(&f);
    hovered = first_fill(&f, SCHULTZ_DRAW_FILL_ROUND_RECT);

    schultz_events_mouse_button(f.events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
    paint_all(&f);
    pressed = first_fill(&f, SCHULTZ_DRAW_FILL_ROUND_RECT);

    /* Three different looks, none of them named by this test. */
    ASSERT(memcmp(&plain, &hovered, sizeof(plain)) != 0);
    ASSERT(memcmp(&hovered, &pressed, sizeof(plain)) != 0);

    fixture_teardown(&f);
    PASS();
}

TEST a_focused_button_draws_a_ring(void)
{
    control_fixture f;
    schultz_handle button;
    uint32_t without;
    uint32_t with;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Go", &button);
    place(&f, button, 20.0f, 30.0f);

    paint_all(&f);
    without = count_kind(&f, SCHULTZ_DRAW_STROKE_ROUND_RECT);

    schultz_events_set_focus(f.events, button);
    paint_all(&f);
    with = count_kind(&f, SCHULTZ_DRAW_STROKE_ROUND_RECT);

    ASSERT_EQ(without + 1u, with);

    fixture_teardown(&f);
    PASS();
}

TEST a_button_click_reaches_the_host(void)
{
    control_fixture f;
    schultz_handle button;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Go", &button);
    place(&f, button, 20.0f, 30.0f);

    click_at(&f, centre_of(&f, button));

    /* The button does not consume its own click. */
    ASSERT_EQ(1, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, button));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------- keyboard */

TEST space_and_enter_activate_the_focused_control(void)
{
    control_fixture f;
    schultz_handle button;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Go", &button);
    place(&f, button, 20.0f, 30.0f);
    schultz_events_set_focus(f.events, button);

    schultz_events_key(f.events, SCHULTZ_KEY_SPACE, 0, 1);
    /* Pressed on the way down, which is what makes the styling work. */
    ASSERT(schultz_node_get_state(f.tree, button) & SCHULTZ_STATE_PRESSED);
    schultz_events_key(f.events, SCHULTZ_KEY_SPACE, 0, 0);
    ASSERT_FALSE(schultz_node_get_state(f.tree, button) &
                 SCHULTZ_STATE_PRESSED);

    schultz_events_key(f.events, SCHULTZ_KEY_RETURN, 0, 1);
    schultz_events_key(f.events, SCHULTZ_KEY_RETURN, 0, 0);

    ASSERT_EQ(2, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, button));

    fixture_teardown(&f);
    PASS();
}

TEST an_ordinary_key_does_not_activate_anything(void)
{
    control_fixture f;
    schultz_handle button;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Go", &button);
    place(&f, button, 20.0f, 30.0f);
    schultz_events_set_focus(f.events, button);

    schultz_events_key(f.events, (uint32_t)'a', 0, 1);
    schultz_events_key(f.events, (uint32_t)'a', 0, 0);

    ASSERT_EQ(0, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, button));
    ASSERT_EQ(1, seen_count(&f.seen, SCHULTZ_EVENT_KEY_DOWN, button));

    fixture_teardown(&f);
    PASS();
}

TEST a_node_that_cannot_be_clicked_is_not_activated_by_a_key(void)
{
    control_fixture f;
    schultz_handle panel;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_node_set_actions(f.tree, panel, SCHULTZ_ACTION_FOCUS);
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 40, 40));
    schultz_events_set_focus(f.events, panel);

    schultz_events_key(f.events, SCHULTZ_KEY_SPACE, 0, 1);
    schultz_events_key(f.events, SCHULTZ_KEY_SPACE, 0, 0);

    ASSERT_EQ(0, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, panel));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------- Checkbox */

TEST a_checkbox_toggles_itself_on_a_click(void)
{
    control_fixture f;
    schultz_handle box;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_checkbox_create(f.tree,
                    schultz_tree_root(f.tree), "Remember me", &box));
    place(&f, box, 10.0f, 10.0f);

    ASSERT_EQ(0, schultz_toggle_checked(f.tree, box));
    click_at(&f, centre_of(&f, box));
    ASSERT_EQ(1, schultz_toggle_checked(f.tree, box));
    click_at(&f, centre_of(&f, box));
    ASSERT_EQ(0, schultz_toggle_checked(f.tree, box));

    /* Toggling itself does not swallow the click. */
    ASSERT_EQ(2, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, box));

    fixture_teardown(&f);
    PASS();
}

TEST a_disabled_checkbox_ignores_a_click(void)
{
    control_fixture f;
    schultz_handle box;
    uint32_t state;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_checkbox_create(f.tree, schultz_tree_root(f.tree), "No", &box);
    place(&f, box, 10.0f, 10.0f);

    state = schultz_node_get_state(f.tree, box);
    schultz_node_set_state(f.tree, box, state & ~(uint32_t)SCHULTZ_STATE_ENABLED);

    click_at(&f, centre_of(&f, box));
    ASSERT_EQ(0, schultz_toggle_checked(f.tree, box));

    fixture_teardown(&f);
    PASS();
}

TEST a_checked_checkbox_draws_a_tick(void)
{
    control_fixture f;
    schultz_handle box;
    uint32_t unchecked;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_checkbox_create(f.tree, schultz_tree_root(f.tree), "On", &box);
    place(&f, box, 10.0f, 10.0f);

    paint_all(&f);
    unchecked = count_kind(&f, SCHULTZ_DRAW_LINE);
    ASSERT_EQ(0u, unchecked);

    ASSERT_EQ(SCHULTZ_OK, schultz_toggle_set_checked(f.tree, box, 1));
    paint_all(&f);
    /* A tick is two strokes. */
    ASSERT_EQ(2u, count_kind(&f, SCHULTZ_DRAW_LINE));

    fixture_teardown(&f);
    PASS();
}

TEST a_checkbox_indicator_is_a_square_beside_its_caption(void)
{
    control_fixture f;
    schultz_handle box;
    schultz_handle bare;
    schultz_size labelled;
    schultz_size alone;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_checkbox_create(f.tree, schultz_tree_root(f.tree), "Wide caption",
                            &box);
    schultz_checkbox_create(f.tree, schultz_tree_root(f.tree), NULL, &bare);
    schultz_tree_resolve_styles(f.tree);

    schultz_layout_measure(f.tree, box, -1, -1, &labelled);
    schultz_layout_measure(f.tree, bare, -1, -1, &alone);

    /* Square when there is nothing beside it. */
    ASSERT_EQ(alone.width, alone.height);
    ASSERT(labelled.width > alone.width);

    fixture_teardown(&f);
    PASS();
}

TEST toggle_accessors_reject_other_widgets(void)
{
    control_fixture f;
    schultz_handle panel;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ(0, schultz_toggle_checked(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_toggle_set_checked(f.tree, panel, 1));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_radio_select(f.tree, panel));

    fixture_teardown(&f);
    PASS();
}

/* ---------------------------------------------------------------- Radio */

TEST selecting_a_radio_clears_the_rest_of_its_group(void)
{
    control_fixture f;
    schultz_handle row;
    schultz_handle a;
    schultz_handle b;
    schultz_handle c;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &row);
    ASSERT_EQ(SCHULTZ_OK, schultz_radio_create(f.tree, row, "A", 1, &a));
    ASSERT_EQ(SCHULTZ_OK, schultz_radio_create(f.tree, row, "B", 1, &b));
    /* A second group in the same container, to prove groups are honoured. */
    ASSERT_EQ(SCHULTZ_OK, schultz_radio_create(f.tree, row, "C", 2, &c));

    ASSERT_EQ(SCHULTZ_OK, schultz_radio_select(f.tree, c));
    ASSERT_EQ(SCHULTZ_OK, schultz_radio_select(f.tree, a));
    ASSERT_EQ(1, schultz_toggle_checked(f.tree, a));
    ASSERT_EQ(0, schultz_toggle_checked(f.tree, b));
    ASSERT_EQ(1, schultz_toggle_checked(f.tree, c));

    schultz_radio_select(f.tree, b);
    ASSERT_EQ(0, schultz_toggle_checked(f.tree, a));
    ASSERT_EQ(1, schultz_toggle_checked(f.tree, b));
    ASSERT_EQ(1, schultz_toggle_checked(f.tree, c));

    fixture_teardown(&f);
    PASS();
}

TEST clicking_a_selected_radio_leaves_it_selected(void)
{
    control_fixture f;
    schultz_handle row;
    schultz_handle a;
    schultz_handle b;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &row);
    schultz_node_set_bounds(f.tree, row, schultz_rect_make(0, 0, 200, 100));
    schultz_radio_create(f.tree, row, "A", 1, &a);
    schultz_radio_create(f.tree, row, "B", 1, &b);
    place(&f, a, 0.0f, 0.0f);
    place(&f, b, 0.0f, 40.0f);

    click_at(&f, centre_of(&f, a));
    ASSERT_EQ(1, schultz_toggle_checked(f.tree, a));
    click_at(&f, centre_of(&f, a));
    ASSERT_EQ(1, schultz_toggle_checked(f.tree, a));

    click_at(&f, centre_of(&f, b));
    ASSERT_EQ(0, schultz_toggle_checked(f.tree, a));
    ASSERT_EQ(1, schultz_toggle_checked(f.tree, b));

    fixture_teardown(&f);
    PASS();
}

TEST a_radio_draws_circles_and_a_dot_when_selected(void)
{
    control_fixture f;
    schultz_handle radio;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_radio_create(f.tree, schultz_tree_root(f.tree), "A", 1, &radio);
    place(&f, radio, 10.0f, 10.0f);

    paint_all(&f);
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_FILL_ELLIPSE));
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_STROKE_ELLIPSE));

    schultz_radio_select(f.tree, radio);
    paint_all(&f);
    /* The box and the dot. */
    ASSERT_EQ(2u, count_kind(&f, SCHULTZ_DRAW_FILL_ELLIPSE));

    fixture_teardown(&f);
    PASS();
}

/* --------------------------------------------------------------- Switch */

TEST a_switch_moves_its_thumb_when_turned_on(void)
{
    control_fixture f;
    schultz_handle toggle;
    float off_x;
    float on_x;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_switch_create(f.tree,
                    schultz_tree_root(f.tree), "Sound", &toggle));
    place(&f, toggle, 10.0f, 10.0f);

    paint_all(&f);
    off_x = 0.0f;
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_FILL_ELLIPSE) {
            off_x = cmd->as.fill_ellipse.rect.x;
        }
    }

    schultz_toggle_set_checked(f.tree, toggle, 1);
    paint_all(&f);
    on_x = 0.0f;
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_FILL_ELLIPSE) {
            on_x = cmd->as.fill_ellipse.rect.x;
        }
    }

    ASSERT(on_x > off_x);

    fixture_teardown(&f);
    PASS();
}

TEST a_switch_is_wider_than_a_checkbox(void)
{
    control_fixture f;
    schultz_handle toggle;
    schultz_handle box;
    schultz_size a;
    schultz_size b;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_switch_create(f.tree, schultz_tree_root(f.tree), NULL, &toggle);
    schultz_checkbox_create(f.tree, schultz_tree_root(f.tree), NULL, &box);
    schultz_tree_resolve_styles(f.tree);

    schultz_layout_measure(f.tree, toggle, -1, -1, &a);
    schultz_layout_measure(f.tree, box, -1, -1, &b);

    ASSERT(a.width > b.width);
    ASSERT_EQ(a.height, b.height);

    fixture_teardown(&f);
    PASS();
}

/* --------------------------------------------------------------- Slider */

TEST a_slider_starts_inside_its_range(void)
{
    control_fixture f;
    schultz_handle slider;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                                    SCHULTZ_ORIENT_HORIZONTAL,
                                    0.0f, 10.0f, 4.0f, &slider));
    ASSERT_EQ(4.0f, schultz_slider_value(f.tree, slider));
    ASSERT_EQ((uint32_t)SCHULTZ_ROLE_SLIDER,
              schultz_node_get_role(f.tree, slider));
    ASSERT_STR_EQ("4", schultz_node_get_value(f.tree, slider));

    /* Out of range on either side is clamped, not rejected. */
    schultz_slider_set_value(f.tree, slider, 99.0f);
    ASSERT_EQ(10.0f, schultz_slider_value(f.tree, slider));
    schultz_slider_set_value(f.tree, slider, -5.0f);
    ASSERT_EQ(0.0f, schultz_slider_value(f.tree, slider));

    /* A backwards range is refused. */
    {
        schultz_handle bad = SCHULTZ_HANDLE_NONE;
        ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
                  schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                                        SCHULTZ_ORIENT_HORIZONTAL,
                                        10.0f, 0.0f, 1.0f, &bad));
    }

    fixture_teardown(&f);
    PASS();
}

TEST arrow_keys_step_a_slider_and_are_consumed(void)
{
    control_fixture f;
    schultz_handle slider;
    int32_t result;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                          SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 100.0f,
                          50.0f, &slider);
    place(&f, slider, 10.0f, 10.0f);
    schultz_events_set_focus(f.events, slider);
    ASSERT_EQ(SCHULTZ_OK, schultz_slider_set_step(f.tree, slider, 5.0f));

    result = schultz_events_key(f.events, SCHULTZ_KEY_RIGHT, 0, 1);
    ASSERT_EQ(SCHULTZ_EVENT_CONSUMED, result);
    ASSERT_EQ(55.0f, schultz_slider_value(f.tree, slider));

    schultz_events_key(f.events, SCHULTZ_KEY_LEFT, 0, 1);
    schultz_events_key(f.events, SCHULTZ_KEY_DOWN, 0, 1);
    ASSERT_EQ(45.0f, schultz_slider_value(f.tree, slider));

    schultz_events_key(f.events, SCHULTZ_KEY_HOME, 0, 1);
    ASSERT_EQ(0.0f, schultz_slider_value(f.tree, slider));
    schultz_events_key(f.events, SCHULTZ_KEY_END, 0, 1);
    ASSERT_EQ(100.0f, schultz_slider_value(f.tree, slider));

    /* A consumed key never reaches the host. */
    ASSERT_EQ(0, seen_count(&f.seen, SCHULTZ_EVENT_KEY_DOWN, slider));

    fixture_teardown(&f);
    PASS();
}

TEST a_bad_slider_step_is_refused(void)
{
    control_fixture f;
    schultz_handle slider;
    schultz_handle panel;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                          SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 1.0f, 0.0f,
                          &slider);
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_slider_set_step(f.tree, slider, 0.0f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_slider_set_step(f.tree, panel, 1.0f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_slider_set_value(f.tree, panel, 1.0f));
    ASSERT_EQ(0.0f, schultz_slider_value(f.tree, panel));

    fixture_teardown(&f);
    PASS();
}

TEST dragging_a_slider_moves_its_value(void)
{
    control_fixture f;
    schultz_handle slider;
    schultz_rect bounds;
    float middle;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                          SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 100.0f,
                          0.0f, &slider);
    place(&f, slider, 10.0f, 10.0f);
    schultz_node_absolute_bounds(f.tree, slider, &bounds);

    /* Press in the middle of the track: half way along the range. */
    schultz_events_mouse_move(f.events,
        schultz_point_make(bounds.x + bounds.width * 0.5f,
                           bounds.y + bounds.height * 0.5f), 0);
    schultz_events_mouse_button(f.events,
        schultz_point_make(bounds.x + bounds.width * 0.5f,
                           bounds.y + bounds.height * 0.5f),
        SCHULTZ_BUTTON_LEFT, 1, 0);
    middle = schultz_slider_value(f.tree, slider);
    ASSERT(middle > 45.0f && middle < 55.0f);

    /* Dragging past the end clamps rather than overshooting. */
    schultz_events_mouse_move(f.events,
        schultz_point_make(bounds.x + bounds.width + 200.0f, bounds.y), 0);
    ASSERT_EQ(100.0f, schultz_slider_value(f.tree, slider));

    schultz_events_mouse_button(f.events,
        schultz_point_make(bounds.x, bounds.y), SCHULTZ_BUTTON_LEFT, 0, 0);

    fixture_teardown(&f);
    PASS();
}

TEST a_slider_paints_a_track_a_fill_and_a_thumb(void)
{
    control_fixture f;
    schultz_handle slider;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                          SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 1.0f, 0.5f,
                          &slider);
    place(&f, slider, 10.0f, 10.0f);

    paint_all(&f);
    ASSERT_EQ(2u, count_kind(&f, SCHULTZ_DRAW_FILL_ROUND_RECT));
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_FILL_ELLIPSE));

    fixture_teardown(&f);
    PASS();
}

/* ---------------------------------------------------------- ProgressBar */

TEST a_progress_bar_fills_in_proportion_to_its_value(void)
{
    control_fixture f;
    schultz_handle bar;
    schultz_rect bounds;
    float quarter = 0.0f;
    float full = 0.0f;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_progress_bar_create(f.tree, schultz_tree_root(f.tree),
                                                      SCHULTZ_ORIENT_HORIZONTAL,
                                                      &bar));
    place(&f, bar, 0.0f, 0.0f);
    schultz_node_absolute_bounds(f.tree, bar, &bounds);

    ASSERT_EQ((uint32_t)SCHULTZ_ROLE_PROGRESS,
              schultz_node_get_role(f.tree, bar));

    schultz_progress_bar_set_value(f.tree, bar, 0.25f);
    ASSERT_EQ(0.25f, schultz_progress_bar_value(f.tree, bar));
    paint_all(&f);
    ASSERT_EQ(2u, count_kind(&f, SCHULTZ_DRAW_FILL_ROUND_RECT));
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_FILL_ROUND_RECT) {
            quarter = cmd->as.fill_round_rect.rect.width;
        }
    }

    schultz_progress_bar_set_value(f.tree, bar, 1.0f);
    paint_all(&f);
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_FILL_ROUND_RECT) {
            full = cmd->as.fill_round_rect.rect.width;
        }
    }

    ASSERT(full > quarter);
    ASSERT_EQ(bounds.width, full);

    fixture_teardown(&f);
    PASS();
}

TEST an_empty_progress_bar_draws_only_its_track(void)
{
    control_fixture f;
    schultz_handle bar;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_progress_bar_create(f.tree, schultz_tree_root(f.tree),
                                SCHULTZ_ORIENT_HORIZONTAL, &bar);
    place(&f, bar, 0.0f, 0.0f);

    paint_all(&f);
    ASSERT_EQ(1u, count_kind(&f, SCHULTZ_DRAW_FILL_ROUND_RECT));

    fixture_teardown(&f);
    PASS();
}

TEST an_unmeasured_progress_bar_moves_a_marker(void)
{
    control_fixture f;
    schultz_handle bar;
    float left = -1.0f;
    float right = -1.0f;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_progress_bar_create(f.tree, schultz_tree_root(f.tree),
                                SCHULTZ_ORIENT_HORIZONTAL, &bar);
    place(&f, bar, 0.0f, 0.0f);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_progress_bar_set_indeterminate(f.tree, bar, 1));

    schultz_progress_bar_set_value(f.tree, bar, 0.0f);
    paint_all(&f);
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_FILL_ROUND_RECT) {
            left = cmd->as.fill_round_rect.rect.x;
        }
    }

    schultz_progress_bar_set_value(f.tree, bar, 1.0f);
    paint_all(&f);
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);
        if (cmd->kind == SCHULTZ_DRAW_FILL_ROUND_RECT) {
            right = cmd->as.fill_round_rect.rect.x;
        }
    }

    /* The marker slid across rather than the bar filling up. */
    ASSERT(right > left);

    fixture_teardown(&f);
    PASS();
}

TEST progress_bar_accessors_reject_other_widgets(void)
{
    control_fixture f;
    schultz_handle panel;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ(0.0f, schultz_progress_bar_value(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_progress_bar_set_value(f.tree, panel, 0.5f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_progress_bar_set_indeterminate(f.tree, panel, 1));

    fixture_teardown(&f);
    PASS();
}

/* --------------------------------------------------------------- shared */

TEST a_control_can_be_restyled_by_a_named_style(void)
{
    control_fixture f;
    schultz_handle button;
    schultz_handle style = SCHULTZ_HANDLE_NONE;
    schultz_patch patch;
    schultz_color painted;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Go", &button);
    place(&f, button, 0.0f, 0.0f);

    /*
     * The look a constructor installs is a style added before the
     * application's, so a style the application adds afterwards wins.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_patch_init(&patch));
    schultz_patch_set(&patch, SCHULTZ_PROP_BACKGROUND,
                      schultz_value_color(schultz_color_rgba(1, 2, 3, 255)));
    ASSERT_EQ(SCHULTZ_OK, schultz_style_register(f.tree, &patch, &style));
    schultz_patch_free(&patch);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_add_style(f.tree, button, style));

    paint_all(&f);
    painted = first_fill(&f, SCHULTZ_DRAW_FILL_ROUND_RECT);
    ASSERT_EQ(1u, painted.r);
    ASSERT_EQ(2u, painted.g);
    ASSERT_EQ(3u, painted.b);

    fixture_teardown(&f);
    PASS();
}

TEST controls_reject_a_null_out_handle(void)
{
    control_fixture f;
    schultz_handle root;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    root = schultz_tree_root(f.tree);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_button_create(f.tree, root, "x", NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_checkbox_create(f.tree, root, "x", NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_radio_create(f.tree, root, "x", 0, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_switch_create(f.tree, root, "x", NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_slider_create(f.tree, root,
                                    SCHULTZ_ORIENT_HORIZONTAL, 0, 1, 0, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_progress_bar_create(f.tree, root,
                                          SCHULTZ_ORIENT_HORIZONTAL, NULL));

    fixture_teardown(&f);
    PASS();
}

TEST controls_reject_a_stale_parent(void)
{
    control_fixture f;
    schultz_handle parent;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &parent);
    schultz_node_destroy(f.tree, parent);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_button_create(f.tree, parent, "x", &node));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_checkbox_create(f.tree, parent, "x", &node));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_slider_create(f.tree, parent,
                                    SCHULTZ_ORIENT_HORIZONTAL, 0, 1, 0, &node));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_progress_bar_create(f.tree, parent,
                                          SCHULTZ_ORIENT_HORIZONTAL, &node));

    fixture_teardown(&f);
    PASS();
}

TEST a_control_with_no_size_paints_without_trouble(void)
{
    control_fixture f;
    schultz_handle button;
    schultz_handle slider;
    schultz_handle bar;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "", &button);
    schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                          SCHULTZ_ORIENT_HORIZONTAL, 0, 1, 0,
                          &slider);
    schultz_progress_bar_create(f.tree, schultz_tree_root(f.tree),
                                SCHULTZ_ORIENT_HORIZONTAL, &bar);

    schultz_node_set_bounds(f.tree, button, schultz_rect_make(0, 0, 0, 0));
    schultz_node_set_bounds(f.tree, slider, schultz_rect_make(0, 0, 0, 0));
    schultz_node_set_bounds(f.tree, bar, schultz_rect_make(0, 0, 0, 0));

    /* Nothing here should crash or divide by a zero width. */
    paint_all(&f);

    fixture_teardown(&f);
    PASS();
}

/* --------------------------------------------------- focus ring bleeding */

/*
 * The ring is drawn outside the control, so invalidating only the control's
 * bounds leaves part of the ring unrepainted. This is what the paint margin
 * is for, and this test is the bug that found it: three sides of the ring
 * appeared and the fourth did not.
 */
TEST focusing_a_control_invalidates_the_ring_too(void)
{
    control_fixture f;
    schultz_handle slider;
    schultz_rect bounds;
    schultz_rect dirty;
    float margin;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                          SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 1.0f, 0.5f,
                          &slider);
    place(&f, slider, 100.0f, 100.0f);
    schultz_node_absolute_bounds(f.tree, slider, &bounds);

    margin = schultz_node_paint_margin(f.tree, slider);
    ASSERT(margin > 0.0f);

    schultz_tree_clear_dirty(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_events_set_focus(f.events, slider));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(f.tree, &dirty));

    /* Every edge of the ring is inside what will be repainted. */
    ASSERT(dirty.x <= bounds.x - margin);
    ASSERT(dirty.y <= bounds.y - margin);
    ASSERT(dirty.x + dirty.width >= bounds.x + bounds.width + margin);
    ASSERT(dirty.y + dirty.height >= bounds.y + bounds.height + margin);

    fixture_teardown(&f);
    PASS();
}

TEST a_control_repaints_when_only_its_ring_is_dirty(void)
{
    control_fixture f;
    schultz_handle slider;
    schultz_rect bounds;
    schultz_rect sliver;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                          SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 1.0f, 0.5f,
                          &slider);
    place(&f, slider, 100.0f, 100.0f);
    schultz_events_set_focus(f.events, slider);
    schultz_node_absolute_bounds(f.tree, slider, &bounds);

    /* A strip just past the right edge: ring only, no part of the control. */
    sliver = schultz_rect_make(bounds.x + bounds.width + 1.0f, bounds.y,
                               2.0f, bounds.height);

    schultz_arena_reset(&f.arena);
    schultz_draw_list_init(&f.list, &f.arena, 0);
    schultz_tree_resolve_styles(f.tree);
    schultz_widget_paint_tree(f.tree, &f.list, &f.arena, sliver);

    ASSERT(schultz_draw_list_count(&f.list) > 0u);

    fixture_teardown(&f);
    PASS();
}

TEST the_paint_margin_refuses_a_negative_value(void)
{
    control_fixture f;
    schultz_handle panel;
    schultz_handle gone;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    ASSERT_EQ(0.0f, schultz_node_paint_margin(f.tree, panel));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_node_set_paint_margin(f.tree, panel, -1.0f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_set_paint_margin(f.tree, panel, 6.0f));
    ASSERT_EQ(6.0f, schultz_node_paint_margin(f.tree, panel));

    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &gone);
    schultz_node_destroy(f.tree, gone);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_node_set_paint_margin(f.tree, gone, 1.0f));
    ASSERT_EQ(0.0f, schultz_node_paint_margin(f.tree, gone));

    fixture_teardown(&f);
    PASS();
}

/* --------------------------------------------------------- hyperlink */

TEST a_hyperlink_is_a_button_that_reports_visits(void)
{
    schultz_tree *tree;
    schultz_handle link = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_hyperlink_create(tree,
        schultz_tree_root(tree), "Open the docs", &link));

    /* Focusable and clickable, which is what makes it a button. */
    ASSERT(schultz_node_get_actions(tree, link) & SCHULTZ_ACTION_CLICK);
    ASSERT(schultz_node_get_actions(tree, link) & SCHULTZ_ACTION_FOCUS);

    ASSERT_EQ(0, schultz_hyperlink_is_visited(tree, link));
    ASSERT_EQ(SCHULTZ_OK, schultz_hyperlink_set_visited(tree, link, 1));
    ASSERT_EQ(1, schultz_hyperlink_is_visited(tree, link));
    ASSERT_EQ(SCHULTZ_OK, schultz_hyperlink_set_visited(tree, link, 0));
    ASSERT_EQ(0, schultz_hyperlink_is_visited(tree, link));

    ASSERT_EQ(SCHULTZ_OK, schultz_hyperlink_set_text(tree, link, "Elsewhere"));

    /* The calls refuse a node that is not one. */
    ASSERT_EQ(0, schultz_hyperlink_is_visited(tree, schultz_tree_root(tree)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_hyperlink_set_visited(tree, schultz_tree_root(tree), 1));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_hyperlinks_underline_stops_where_the_words_do(void)
{
    control_fixture f;
    schultz_handle link;
    schultz_rect natural;
    const schultz_draw_cmd *rule = NULL;
    float length = 0.0f;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_hyperlink_create(f.tree,
        schultz_tree_root(f.tree), "Open the handbook", &link));
    place(&f, link, 20.0f, 30.0f);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_node_absolute_bounds(f.tree, link, &natural));

    /* Four times the room the words need, which is what a column that
     * stretches its children hands a link. */
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_arrange(f.tree, link,
        schultz_rect_make(natural.x, natural.y, natural.width * 4.0f,
                          natural.height));

    /* The rule is only drawn under the pointer. */
    schultz_events_mouse_move(f.events, centre_of(&f, link), 0);
    paint_all(&f);

    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);

        if (cmd->kind == SCHULTZ_DRAW_LINE) {
            rule = cmd;
        }
    }
    ASSERT(rule != NULL);

    length = rule->as.line.to.x - rule->as.line.from.x;
    ASSERT(length > 0.0f);
    /* Under the words, not across the whole link. */
    ASSERT(length <= natural.width);
    ASSERT(length < natural.width * 4.0f * 0.5f);
    ASSERT_EQ(natural.x, rule->as.line.from.x);

    fixture_teardown(&f);
    PASS();
}

/* ----------------------------------------------------- toggle button */

TEST an_ungrouped_toggle_button_turns_on_and_off(void)
{
    schultz_tree *tree;
    schultz_handle button = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_toggle_button_create(tree,
        schultz_tree_root(tree), "Bold", 0u, &button));

    ASSERT_EQ(0, schultz_toggle_button_is_selected(tree, button));
    schultz_toggle_button_select(tree, button);
    ASSERT_EQ(1, schultz_toggle_button_is_selected(tree, button));
    /* Ungrouped, so selecting again turns it back off. */
    schultz_toggle_button_select(tree, button);
    ASSERT_EQ(0, schultz_toggle_button_is_selected(tree, button));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_grouped_toggle_button_clears_the_rest_of_its_group(void)
{
    schultz_tree *tree;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_handle left = SCHULTZ_HANDLE_NONE;
    schultz_handle centre = SCHULTZ_HANDLE_NONE;
    schultz_handle right = SCHULTZ_HANDLE_NONE;
    schultz_handle other = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(tree, schultz_tree_root(tree),
                                              &row));
    ASSERT_EQ(SCHULTZ_OK, schultz_toggle_button_create(tree, row, "Left", 1u,
                                                       &left));
    ASSERT_EQ(SCHULTZ_OK, schultz_toggle_button_create(tree, row, "Centre", 1u,
                                                       &centre));
    ASSERT_EQ(SCHULTZ_OK, schultz_toggle_button_create(tree, row, "Right", 1u,
                                                       &right));
    /* A different group in the same row is not disturbed. */
    ASSERT_EQ(SCHULTZ_OK, schultz_toggle_button_create(tree, row, "Wrap", 2u,
                                                       &other));
    schultz_toggle_button_select(tree, other);

    schultz_toggle_button_select(tree, left);
    ASSERT_EQ(1, schultz_toggle_button_is_selected(tree, left));

    schultz_toggle_button_select(tree, right);
    ASSERT_EQ(0, schultz_toggle_button_is_selected(tree, left));
    ASSERT_EQ(1, schultz_toggle_button_is_selected(tree, right));
    ASSERT_EQ(1, schultz_toggle_button_is_selected(tree, other));

    /* One of a set stays chosen, so selecting it again is not a toggle. */
    schultz_toggle_button_select(tree, right);
    ASSERT_EQ(1, schultz_toggle_button_is_selected(tree, right));

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------------- button bar */

/* Reads back the captions in the order the bar actually placed them. */
static void bar_order(schultz_tree *tree, schultz_handle bar, char *out,
                      size_t max)
{
    uint32_t count = schultz_node_child_count(tree, bar);
    schultz_handle placed[8];
    uint32_t n = 0;
    uint32_t i;
    uint32_t j;

    out[0] = '\0';
    for (i = 0; i < count && n < 8u; i++) {
        schultz_handle child = SCHULTZ_HANDLE_NONE;
        schultz_rect b;
        uint32_t at;

        if (schultz_node_child_at(tree, bar, i, &child) != SCHULTZ_OK ||
            schultz_node_get_bounds(tree, child, &b) != SCHULTZ_OK) {
            continue;
        }
        /* Insertion sort by x, so the answer is what the eye would read. */
        at = n;
        while (at > 0u) {
            schultz_rect prev;

            schultz_node_get_bounds(tree, placed[at - 1u], &prev);
            if (prev.x <= b.x) {
                break;
            }
            placed[at] = placed[at - 1u];
            at--;
        }
        placed[at] = child;
        n++;
    }
    for (j = 0; j < n; j++) {
        schultz_handle label = schultz_button_label(tree, placed[j]);
        const char *text = schultz_label_text(tree, label);

        if (out[0] != '\0') {
            strncat(out, ",", max - strlen(out) - 1u);
        }
        strncat(out, (text == NULL) ? "?" : text, max - strlen(out) - 1u);
    }
}

/* Builds the same bar every time, added in a deliberately unhelpful order. */
static schultz_handle build_bar(schultz_tree *tree, uint32_t order)
{
    schultz_handle bar = SCHULTZ_HANDLE_NONE;

    if (schultz_button_bar_create(tree, schultz_tree_root(tree), &bar)
            != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    schultz_button_bar_set_order(tree, bar, order);
    schultz_button_bar_add(tree, bar, "OK", SCHULTZ_BUTTON_ROLE_OK, NULL);
    schultz_button_bar_add(tree, bar, "Cancel", SCHULTZ_BUTTON_ROLE_CANCEL,
                           NULL);
    schultz_button_bar_add(tree, bar, "Help", SCHULTZ_BUTTON_ROLE_LEFT, NULL);
    schultz_node_set_bounds(tree, bar, schultz_rect_make(0, 0, 400, 40));
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);
    return bar;
}

TEST a_button_bar_orders_by_platform_not_by_insertion(void)
{
    schultz_tree *tree;
    schultz_handle bar;
    char order[64];

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 800, 600));

    bar = build_bar(tree, SCHULTZ_BUTTON_ORDER_WINDOWS);
    ASSERT(bar != SCHULTZ_HANDLE_NONE);
    bar_order(tree, bar, order, sizeof(order));
    ASSERT_STR_EQ("Help,OK,Cancel", order);

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_button_bar_puts_accept_last_on_macos(void)
{
    schultz_tree *tree;
    schultz_handle bar;
    char order[64];

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 800, 600));

    bar = build_bar(tree, SCHULTZ_BUTTON_ORDER_MACOS);
    ASSERT(bar != SCHULTZ_HANDLE_NONE);
    bar_order(tree, bar, order, sizeof(order));
    /* Accept on the right, and the left role stays left either way. */
    ASSERT_STR_EQ("Help,Cancel,OK", order);

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_button_bar_sizes_every_button_alike(void)
{
    schultz_tree *tree;
    schultz_handle bar = SCHULTZ_HANDLE_NONE;
    schultz_handle narrow = SCHULTZ_HANDLE_NONE;
    schultz_handle wide = SCHULTZ_HANDLE_NONE;
    schultz_rect a;
    schultz_rect b;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 800, 600));
    ASSERT_EQ(SCHULTZ_OK, schultz_button_bar_create(tree,
        schultz_tree_root(tree), &bar));
    schultz_button_bar_add(tree, bar, "No", SCHULTZ_BUTTON_ROLE_NO, &narrow);
    schultz_button_bar_add(tree, bar, "Save all changes",
                           SCHULTZ_BUTTON_ROLE_OK, &wide);
    /*
     * Widths by hint rather than by caption: this suite has no font system,
     * so every caption measures zero and the two would otherwise be alike
     * whether the bar matched them or not.
     */
    schultz_node_set_pref_size(tree, narrow, 40.0f, SCHULTZ_SIZE_UNSET);
    schultz_node_set_pref_size(tree, wide, 120.0f, SCHULTZ_SIZE_UNSET);
    schultz_node_set_bounds(tree, bar, schultz_rect_make(0, 0, 400, 40));
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);

    schultz_node_get_bounds(tree, narrow, &a);
    schultz_node_get_bounds(tree, wide, &b);
    ASSERT_EQ(120.0f, a.width);
    ASSERT_EQ(120.0f, b.width);

    /* Turned off, each takes the width its caption needs. */
    schultz_button_bar_set_uniform_width(tree, bar, 0);
    schultz_layout_run(tree);
    schultz_node_get_bounds(tree, narrow, &a);
    schultz_node_get_bounds(tree, wide, &b);
    ASSERT_EQ(40.0f, a.width);
    ASSERT_EQ(120.0f, b.width);

    schultz_tree_destroy(tree);
    PASS();
}

/* -------------------------------------------------------- number field */

TEST a_number_field_clamps_and_steps(void)
{
    schultz_tree *tree;
    schultz_handle number = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_number_field_create(tree,
        schultz_tree_root(tree), 0.0, 10.0, 5.0, &number));

    ASSERT_EQ(5.0, schultz_number_field_value(tree, number));

    schultz_number_field_set_value(tree, number, 99.0);
    ASSERT_EQ(10.0, schultz_number_field_value(tree, number));
    schultz_number_field_set_value(tree, number, -5.0);
    ASSERT_EQ(0.0, schultz_number_field_value(tree, number));

    /* A range that no longer holds the value pulls it inside. */
    schultz_number_field_set_value(tree, number, 8.0);
    ASSERT_EQ(SCHULTZ_OK, schultz_number_field_set_range(tree, number, 0.0,
                                                         3.0));
    ASSERT_EQ(3.0, schultz_number_field_value(tree, number));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_number_field_set_range(tree, number, 5.0, 1.0));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_number_field_set_step(tree, number, 0.0));

    schultz_tree_destroy(tree);
    PASS();
}

/* Text that is not a number puts the last good value back. */
TEST a_number_field_refuses_what_is_not_a_number(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle number = SCHULTZ_HANDLE_NONE;
    schultz_handle field;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 200));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_number_field_create(tree,
        schultz_tree_root(tree), 0.0, 100.0, 7.0, &number));
    field = schultz_number_field_text_field(tree, number);
    ASSERT(field != SCHULTZ_HANDLE_NONE);

    schultz_text_set(tree, field, "42");
    schultz_events_set_focus(events, number);
    schultz_events_key(events, SCHULTZ_KEY_RETURN, 0u, 1);
    ASSERT_EQ(42.0, schultz_number_field_value(tree, number));

    schultz_text_set(tree, field, "nonsense");
    schultz_events_key(events, SCHULTZ_KEY_RETURN, 0u, 1);
    ASSERT_EQ(42.0, schultz_number_field_value(tree, number));
    ASSERT_STR_EQ("42", schultz_text_get(tree, field));

    /* Half a number is not a number either. */
    schultz_text_set(tree, field, "12abc");
    schultz_events_key(events, SCHULTZ_KEY_RETURN, 0u, 1);
    ASSERT_EQ(42.0, schultz_number_field_value(tree, number));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

TEST a_wrapping_number_field_runs_off_one_end_onto_the_other(void)
{
    schultz_tree *tree;
    schultz_handle number = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_number_field_create(tree,
        schultz_tree_root(tree), 0.0, 23.0, 23.0, &number));
    ASSERT_EQ(SCHULTZ_OK, schultz_number_field_set_wrap(tree, number, 1));

    schultz_number_field_set_value(tree, number, 24.0);
    ASSERT_EQ(0.0, schultz_number_field_value(tree, number));
    schultz_number_field_set_value(tree, number, -1.0);
    ASSERT_EQ(23.0, schultz_number_field_value(tree, number));

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------------- date picker */

TEST the_calendar_knows_how_long_a_month_is(void)
{
    schultz_tree *tree;
    schultz_handle date = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_create(tree,
        schultz_tree_root(tree), &date));

    /* 2024 is a leap year, 2023 is not, 2000 is, 1900 is not. */
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_date(tree, date, 2024, 2,
                                                       29));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_date_picker_set_date(tree, date, 2023, 2, 29));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_date(tree, date, 2000, 2,
                                                       29));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_date_picker_set_date(tree, date, 1900, 2, 29));

    /* And the ordinary month lengths. */
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_date(tree, date, 2026, 1,
                                                       31));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_date_picker_set_date(tree, date, 2026, 4, 31));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_date_picker_set_date(tree, date, 2026, 13, 1));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_date_picker_set_date(tree, date, 2026, 1, 0));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_date_picker_reads_back_what_it_was_given(void)
{
    schultz_tree *tree;
    schultz_handle date = SCHULTZ_HANDLE_NONE;
    int32_t year = 0;
    int32_t month = 0;
    int32_t day = 0;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_create(tree,
        schultz_tree_root(tree), &date));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_date(tree, date, 2026, 8,
                                                       31));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_date(tree, date, &year, &month,
                                                   &day));
    ASSERT_EQ(2026, year);
    ASSERT_EQ(8, month);
    ASSERT_EQ(31, day);

    /* Any of the three may be left out. */
    year = 0;
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_date(tree, date, &year, NULL,
                                                   NULL));
    ASSERT_EQ(2026, year);

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_date_picker_refuses_a_backwards_range(void)
{
    schultz_tree *tree;
    schultz_handle date = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_create(tree,
        schultz_tree_root(tree), &date));

    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_range(tree, date,
        2026, 1, 1, 2026, 12, 31));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_date_picker_set_range(tree, date,
                  2026, 12, 31, 2026, 1, 1));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_date_picker_set_first_day(tree, date, 7u));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_first_day(tree, date, 1u));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_format(tree, date,
                                                         SCHULTZ_DATE_DMY));

    schultz_tree_destroy(tree);
    PASS();
}

/* Month and day names are the host's to supply, and must come complete. */
TEST a_date_picker_takes_a_full_set_of_names_or_none(void)
{
    schultz_tree *tree;
    schultz_handle date = SCHULTZ_HANDLE_NONE;
    static const char *const months[12] = {
        "ene", "feb", "mar", "abr", "may", "jun",
        "jul", "ago", "sep", "oct", "nov", "dic"
    };
    static const char *const days[7] = {
        "do", "lu", "ma", "mi", "ju", "vi", "sa"
    };

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_create(tree,
        schultz_tree_root(tree), &date));

    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_month_names(tree, date,
                                                              months, 12u));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_day_names(tree, date, days,
                                                            7u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_date_picker_set_month_names(tree, date, months, 11u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_date_picker_set_day_names(tree, date, days, 6u));

    schultz_tree_destroy(tree);
    PASS();
}

/* ---------------------------------------------------------- time picker */

TEST a_time_picker_keeps_24_hour_whatever_it_shows(void)
{
    schultz_tree *tree;
    schultz_handle time = SCHULTZ_HANDLE_NONE;
    int32_t hour = 0;
    int32_t minute = 0;
    int32_t second = 0;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_create(tree,
        schultz_tree_root(tree), &time));

    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_set_time(tree, time, 14, 30,
                                                       15));
    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_time(tree, time, &hour, &minute,
                                                   &second));
    ASSERT_EQ(14, hour);
    ASSERT_EQ(30, minute);
    ASSERT_EQ(15, second);

    /* Showing it as twelve hour does not change what it holds. */
    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_set_24_hour(tree, time, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_time(tree, time, &hour, NULL,
                                                   NULL));
    ASSERT_EQ(14, hour);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_time_picker_set_time(tree, time, 24, 0, 0));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_time_picker_set_time(tree, time, 0, 60, 0));

    schultz_tree_destroy(tree);
    PASS();
}

/* --------------------------------------------------------- colour picker */

TEST a_colour_picker_survives_a_round_trip_through_hsv(void)
{
    schultz_tree *tree;
    schultz_handle pick = SCHULTZ_HANDLE_NONE;
    schultz_color back;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_color_picker_create(tree,
        schultz_tree_root(tree), schultz_color_rgba(216, 139, 74, 255),
        &pick));

    back = schultz_color_picker_color(tree, pick);
    /* Within a step, since the value is held as hue, saturation and value. */
    ASSERT(back.r >= 215 && back.r <= 217);
    ASSERT(back.g >= 138 && back.g <= 140);
    ASSERT(back.b >= 73 && back.b <= 75);

    ASSERT_EQ(SCHULTZ_OK, schultz_color_picker_set_color(tree, pick,
        schultz_color_rgba(0, 128, 255, 255)));
    back = schultz_color_picker_color(tree, pick);
    ASSERT(back.r <= 1);
    ASSERT(back.g >= 127 && back.g <= 129);
    ASSERT(back.b >= 254);

    /* Grey has no hue, and must come back as the grey it went in as. */
    ASSERT_EQ(SCHULTZ_OK, schultz_color_picker_set_color(tree, pick,
        schultz_color_rgba(128, 128, 128, 255)));
    back = schultz_color_picker_color(tree, pick);
    ASSERT(back.r >= 127 && back.r <= 129);
    ASSERT_EQ(back.r, back.g);
    ASSERT_EQ(back.g, back.b);

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_colour_picker_holds_alpha_only_when_asked(void)
{
    schultz_tree *tree;
    schultz_handle pick = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_color_picker_create(tree,
        schultz_tree_root(tree), schultz_color_rgba(10, 20, 30, 128),
        &pick));

    /* It was handed a translucent colour and keeps it. */
    ASSERT_EQ(128, schultz_color_picker_color(tree, pick).a);

    /* Turning alpha off makes it opaque, which is what off means. */
    ASSERT_EQ(SCHULTZ_OK, schultz_color_picker_set_alpha_enabled(tree, pick,
                                                                 0));
    ASSERT_EQ(255, schultz_color_picker_color(tree, pick).a);

    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------------ pickers and popovers */

/*
 * A popover hangs off the root, not off the widget that opened it, so what
 * happens inside one does not rise past the picker that owns it. These cover
 * the relay that carries those events back, because without it a picker
 * looks right and does nothing.
 */

/* The overlay most recently added to the root, which is the one just opened. */
static schultz_handle newest_overlay(schultz_tree *tree)
{
    schultz_handle root = schultz_tree_root(tree);
    uint32_t count = schultz_node_child_count(tree, root);
    schultz_handle out = SCHULTZ_HANDLE_NONE;

    if (count == 0u) {
        return SCHULTZ_HANDLE_NONE;
    }
    schultz_node_child_at(tree, root, count - 1u, &out);
    return out;
}

static schultz_handle child_of(schultz_tree *tree, schultz_handle parent,
                               uint32_t index)
{
    schultz_handle out = SCHULTZ_HANDLE_NONE;

    schultz_node_child_at(tree, parent, index, &out);
    return out;
}

/* Lays the tree out, then clicks the middle of a node. */
static void press_node(schultz_events *events, schultz_tree *tree,
                       schultz_handle node)
{
    schultz_rect b;

    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);
    if (schultz_node_absolute_bounds(tree, node, &b) != SCHULTZ_OK) {
        return;
    }
    {
        schultz_point at = schultz_point_make(b.x + b.width * 0.5f,
                                              b.y + b.height * 0.5f);

        schultz_events_mouse_move(events, at, 0);
        schultz_events_mouse_button(events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
        schultz_events_mouse_button(events, at, SCHULTZ_BUTTON_LEFT, 0, 0);
    }
}

TEST a_calendars_month_buttons_reach_the_picker(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle date = SCHULTZ_HANDLE_NONE;
    schultz_handle popover;
    schultz_handle header;
    const char *first;
    char kept[64];
    int32_t year = 0;
    int32_t month = 0;
    int32_t day = 0;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 600, 500));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 600, 500));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_create(tree,
        schultz_tree_root(tree), &date));
    schultz_node_set_bounds(tree, date, schultz_rect_make(10, 10, 220, 30));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_date(tree, date, 2026, 8,
                                                       15));

    /* Child one is the button that drops the calendar. */
    press_node(events, tree, child_of(tree, date, 1u));
    popover = newest_overlay(tree);
    ASSERT(popover != SCHULTZ_HANDLE_NONE);
    header = child_of(tree, child_of(tree, popover, 0u), 0u);

    first = schultz_label_text(tree, child_of(tree, header, 1u));
    ASSERT(first != NULL);
    snprintf(kept, sizeof(kept), "%s", first);

    /* Stepping back moves what the calendar shows. */
    press_node(events, tree, child_of(tree, header, 0u));
    ASSERT(strcmp(kept, schultz_label_text(tree,
                                           child_of(tree, header, 1u))) != 0);

    /* And forward again puts it back. */
    press_node(events, tree, child_of(tree, header, 2u));
    ASSERT_STR_EQ(kept, schultz_label_text(tree, child_of(tree, header, 1u)));

    /* Walking the months chooses nothing; only a day does that. */
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_date(tree, date, &year, &month,
                                                   &day));
    ASSERT_EQ(2026, year);
    ASSERT_EQ(8, month);
    ASSERT_EQ(15, day);

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

/* The calendar is a month, not a line: seven columns and seven rows. */
TEST a_disabled_widget_hears_nothing_from_its_own_overlay(void)
{
    control_fixture f;
    schultz_handle combo = SCHULTZ_HANDLE_NONE;
    schultz_handle menu = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    uint32_t state;

    /*
     * A combo box's menu hangs off the root, not off the combo, so disabling
     * the combo does not disable its rows and the router sees nothing wrong
     * with pressing one. The menu then hands the choice straight to the
     * owner's handler, going round the router entirely, which is why the
     * refusal has to sit on that path too.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_combo_box_create(f.tree,
                              schultz_tree_root(f.tree), &combo));
    ASSERT_EQ(SCHULTZ_OK, schultz_combo_box_add(f.tree, combo, "One"));
    ASSERT_EQ(SCHULTZ_OK, schultz_combo_box_add(f.tree, combo, "Two"));
    place(&f, combo, 10.0f, 10.0f);
    ASSERT_EQ(0u, schultz_combo_box_selected(f.tree, combo));

    /* Open it, then switch the combo off while the menu is still up. */
    click_at(&f, centre_of(&f, combo));
    menu = schultz_combo_box_menu(f.tree, combo);
    ASSERT(schultz_menu_is_open(f.tree, menu));

    state = schultz_node_get_state(f.tree, combo);
    schultz_node_set_state(f.tree, combo,
        state & ~(uint32_t)SCHULTZ_STATE_ENABLED);

    /* The row is still enabled, because it is not inside the combo. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, menu, 1u, &row));
    ASSERT(schultz_node_is_enabled(f.tree, row));

    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);
    click_at(&f, centre_of(&f, row));
    ASSERT_EQ(0u, schultz_combo_box_selected(f.tree, combo));

    /*
     * Enabled again, the same press is heard. The menu has to be opened
     * again first: choosing a row closes the menu whether or not the owner
     * was told, because closing is the menu's own doing.
     */
    schultz_node_set_state(f.tree, combo,
        schultz_node_get_state(f.tree, combo) | SCHULTZ_STATE_ENABLED);
    click_at(&f, centre_of(&f, combo));
    ASSERT(schultz_menu_is_open(f.tree, menu));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, menu, 1u, &row));
    click_at(&f, centre_of(&f, row));
    ASSERT_EQ(1u, schultz_combo_box_selected(f.tree, combo));

    fixture_teardown(&f);
    PASS();
}

TEST a_date_outside_the_range_cannot_be_chosen(void)
{
    control_fixture f;
    schultz_handle picker = SCHULTZ_HANDLE_NONE;
    schultz_handle cell = SCHULTZ_HANDLE_NONE;
    int32_t year = 0;
    int32_t month = 0;
    int32_t day = 0;
    uint32_t i;

    /*
     * The calendar marks a day outside its range by disabling the cell, and a
     * cell is a plain button whose whole job is to report its click. Before
     * the router stopped disabled nodes, that report went through and the
     * range could be walked straight past.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_create(f.tree,
                              schultz_tree_root(f.tree), &picker));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_date(f.tree, picker, 2026,
                                                       9, 10));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_range(f.tree, picker, 2026,
                                                        9, 5, 2026, 9, 15));

    /* The cells live in the calendar the picker opens; find a disabled one
     * that still says a day, which is one inside the month and out of range. */
    for (i = 0; i < schultz_tree_node_count(f.tree) + 64u; i++) {
        schultz_handle node = (schultz_handle)(((uint64_t)1 << 32) | i);
        schultz_handle label = schultz_button_label(f.tree, node);
        const char *text;

        if (label == SCHULTZ_HANDLE_NONE) {
            continue;
        }
        text = schultz_label_text(f.tree, label);
        if (text == NULL || text[0] == '\0' || text[0] < '0' ||
            text[0] > '9') {
            continue;
        }
        if (!(schultz_node_get_state(f.tree, node) &
              SCHULTZ_STATE_ENABLED)) {
            cell = node;
            break;
        }
    }
    ASSERT(cell != SCHULTZ_HANDLE_NONE);

    /* Asking the router to press it is refused, so no click reaches the
     * calendar and the date it was given still stands. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_events_perform(f.events, cell, SCHULTZ_ACTION_CLICK));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_date(f.tree, picker, &year,
                                                   &month, &day));
    ASSERT_EQ(10, day);

    fixture_teardown(&f);
    PASS();
}

TEST a_calendar_lays_out_a_whole_month(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle date = SCHULTZ_HANDLE_NONE;
    schultz_handle grid;
    schultz_rect cell;
    schultz_rect last;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 600, 500));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 600, 500));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_create(tree,
        schultz_tree_root(tree), &date));
    schultz_node_set_bounds(tree, date, schultz_rect_make(10, 10, 220, 30));

    press_node(events, tree, child_of(tree, date, 1u));
    grid = child_of(tree, child_of(tree, newest_overlay(tree), 0u), 1u);
    /* Seven day names and six weeks of seven. */
    ASSERT_EQ(49u, schultz_node_child_count(tree, grid));

    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);
    /* The first cell and the last must not share a row. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(tree,
        child_of(tree, grid, 7u), &cell));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(tree,
        child_of(tree, grid, 48u), &last));
    ASSERT(last.y > cell.y);

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

TEST a_colour_squares_press_reaches_the_picker(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle pick = SCHULTZ_HANDLE_NONE;
    schultz_handle square;
    schultz_color before;
    schultz_color after;
    schultz_rect box;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 600, 500));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 600, 500));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_color_picker_create(tree,
        schultz_tree_root(tree), schultz_color_rgba(216, 139, 74, 255),
        &pick));
    schultz_node_set_bounds(tree, pick, schultz_rect_make(10, 10, 60, 30));
    before = schultz_color_picker_color(tree, pick);

    press_node(events, tree, child_of(tree, pick, 0u));   /* the swatch */
    square = child_of(tree, child_of(tree, newest_overlay(tree), 0u), 0u);
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(tree, square, &box));
    ASSERT(box.width > 0.0f);

    /* The top left of the square is white, whatever the hue is. */
    {
        schultz_point at = schultz_point_make(box.x + 1.0f, box.y + 1.0f);

        schultz_events_mouse_move(events, at, 0);
        schultz_events_mouse_button(events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
        schultz_events_mouse_button(events, at, SCHULTZ_BUTTON_LEFT, 0, 0);
    }
    after = schultz_color_picker_color(tree, pick);
    ASSERT(after.r != before.r || after.g != before.g || after.b != before.b);
    ASSERT(after.r > 240 && after.g > 240 && after.b > 240);

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

/* A number field's steps are drawn, so they take no focus and no highlight. */
TEST a_number_fields_steps_are_not_buttons(void)
{
    schultz_tree *tree;
    schultz_handle number = SCHULTZ_HANDLE_NONE;
    schultz_handle up;
    schultz_handle down;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_number_field_create(tree,
        schultz_tree_root(tree), 0.0, 10.0, 5.0, &number));

    up   = child_of(tree, number, 1u);
    down = child_of(tree, number, 2u);
    ASSERT(up != SCHULTZ_HANDLE_NONE);
    ASSERT(down != SCHULTZ_HANDLE_NONE);

    /* Nothing to focus and nothing to activate: they are drawn, not built. */
    ASSERT_EQ(0u, schultz_node_get_actions(tree, up));
    ASSERT_EQ(0u, schultz_node_get_actions(tree, down));
    /* But still pressable, or they would do nothing at all. */
    ASSERT_EQ(1, schultz_node_hit_testable(tree, up));

    schultz_tree_destroy(tree);
    PASS();
}


TEST a_button_is_the_same_height_wherever_it_is_put(void)
{
    control_fixture f;
    schultz_handle root;
    schultz_handle plain = SCHULTZ_HANDLE_NONE;
    schultz_handle bar = SCHULTZ_HANDLE_NONE;
    schultz_handle in_bar = SCHULTZ_HANDLE_NONE;
    schultz_handle box = SCHULTZ_HANDLE_NONE;
    schultz_handle content;
    schultz_handle in_message = SCHULTZ_HANDLE_NONE;
    schultz_handle message_bar = SCHULTZ_HANDLE_NONE;
    schultz_rect a;
    schultz_rect b;
    uint32_t n;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    root = schultz_tree_root(f.tree);
    schultz_node_set_bounds(f.tree, root, schultz_rect_make(0, 0, 600, 400));
    schultz_node_set_pane(f.tree, root, schultz_pane_vbox());

    ASSERT_EQ(SCHULTZ_OK, schultz_button_create(f.tree, root, "Plain",
                                                &plain));
    ASSERT_EQ(SCHULTZ_OK, schultz_button_bar_create(f.tree, root, &bar));
    ASSERT_EQ(SCHULTZ_OK, schultz_button_bar_add(f.tree, bar, "OK",
                    SCHULTZ_BUTTON_ROLE_OK, &in_bar));

    ASSERT_EQ(SCHULTZ_OK, schultz_message_dialog_create(f.tree, "Sure?", 0u,
                    SCHULTZ_DIALOG_OK_CANCEL, &box));
    ASSERT_EQ(SCHULTZ_OK, schultz_message_dialog_open(f.tree, box));

    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);

    /*
     * A bar is a row, and a row stretches its children to its own height, so
     * a height named on the bar wins over what its buttons asked for and
     * they end up a different size from every other button on screen.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, plain, &a));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, in_bar, &b));
    ASSERT(a.height > 0.0f);
    ASSERT_EQ(a.height, b.height);

    content = schultz_dialog_content(f.tree, box);
    n = schultz_node_child_count(f.tree, content);
    ASSERT(n > 0u);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, content, n - 1u,
                                                &message_bar));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, message_bar, 0u,
                                                &in_message));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, in_message,
                                                       &b));
    ASSERT_EQ(a.height, b.height);

    fixture_teardown(&f);
    PASS();
}


/*
 * A picker's field holds a string of a known shape, so it is sized to the
 * longest one it can ever be asked to show rather than to a number typed in
 * when the widget was written. The check is the same in every case: measure
 * the string in the same font the field draws it in, and see that the room
 * inside the field's padding is at least that wide.
 */

/* How much of a field is left for text once the padding and outline are
 * taken off both sides. */
static float usable_width(control_fixture *f, schultz_handle picker)
{
    schultz_handle field = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;
    float padding = 0.0f;

    schultz_tree_resolve_styles(f->tree);
    schultz_layout_arrange(f->tree, picker,
                           schultz_rect_make(0, 0, 400, 60));
    if (schultz_node_child_at(f->tree, picker, 0, &field) != SCHULTZ_OK ||
        schultz_node_get_bounds(f->tree, field, &bounds) != SCHULTZ_OK) {
        return -1.0f;
    }
    schultz_node_get_spacing(f->tree, field, &padding, NULL);
    return bounds.width -
           (padding +
            schultz_theme_number(&f->theme, SCHULTZ_TOKEN_BORDER_WIDTH))
           * 2.0f;
}

/* The width that string takes in the body font the pickers draw with. */
static float shown_width(control_fixture *f, const char *text)
{
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_size size;

    if (schultz_label_create(f->tree, schultz_tree_root(f->tree), text,
                             &label) != SCHULTZ_OK) {
        return -1.0f;
    }
    schultz_tree_resolve_styles(f->tree);
    if (schultz_layout_measure(f->tree, label, -1.0f, -1.0f, &size)
            != SCHULTZ_OK) {
        return -1.0f;
    }
    schultz_node_destroy(f->tree, label);
    return size.width;
}

TEST a_time_picker_fits_every_clock_it_can_show(void)
{
    control_fixture f;
    schultz_handle time = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_create(f.tree,
        schultz_tree_root(f.tree), &time));
    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_set_time(f.tree, time, 9, 30,
                                                       0));

    /* Twenty four hour, no seconds: the shortest it ever is. */
    ASSERT(usable_width(&f, time) >= shown_width(&f, "09:30"));

    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_set_show_seconds(f.tree, time,
                                                               1));
    ASSERT(usable_width(&f, time) >= shown_width(&f, "09:30:00"));

    /*
     * Twelve hour with seconds is the longest, and it is what the demo asks
     * for. This is the case the field used to lose the leading digits of.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_set_24_hour(f.tree, time, 0));
    ASSERT(usable_width(&f, time) >= shown_width(&f, "09:30:00 am"));

    /* And the same field has to hold the afternoon, which it does not
     * choose: pm is not am in every font. */
    ASSERT_EQ(SCHULTZ_OK, schultz_time_picker_set_time(f.tree, time, 21, 30,
                                                       0));
    ASSERT(usable_width(&f, time) >= shown_width(&f, "09:30:00 pm"));

    fixture_teardown(&f);
    PASS();
}

TEST a_date_picker_fits_every_format_it_can_show(void)
{
    control_fixture f;
    schultz_handle date = SCHULTZ_HANDLE_NONE;
    schultz_handle bold = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_create(f.tree,
        schultz_tree_root(f.tree), &date));
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_date(f.tree, date, 2026, 12,
                                                       31));

    ASSERT(usable_width(&f, date) >= shown_width(&f, "2026-12-31"));

    /*
     * In a heavier face the same ten characters are wider. A field sized by
     * a number someone typed once fits the face it was typed against and no
     * other, which is the whole reason the width is measured.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_font_load_file(f.fonts,
        "assets/fonts/DejaVuSans-Bold.ttf", 16.0f, &bold));
    schultz_theme_set_font(&f.theme, SCHULTZ_TOKEN_FONT_BODY, bold);
    schultz_tree_set_theme(f.tree, &f.theme);
    schultz_node_invalidate_layout(f.tree, date);
    schultz_tree_resolve_styles(f.tree);
    ASSERT(usable_width(&f, date) >= shown_width(&f, "2026-12-31"));

    /* A slash is not a hyphen, so each format is measured on its own. */
    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_format(f.tree, date,
                                                         SCHULTZ_DATE_DMY));
    ASSERT(usable_width(&f, date) >= shown_width(&f, "31/12/2026"));

    ASSERT_EQ(SCHULTZ_OK, schultz_date_picker_set_format(f.tree, date,
                                                         SCHULTZ_DATE_MDY));
    ASSERT(usable_width(&f, date) >= shown_width(&f, "12/31/2026"));

    fixture_teardown(&f);
    PASS();
}

TEST a_colour_picker_is_a_swatch_and_not_a_sliver(void)
{
    control_fixture f;
    schultz_handle box = SCHULTZ_HANDLE_NONE;
    schultz_handle picker = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_create(f.tree,
        schultz_tree_root(f.tree), &box));
    schultz_node_set_pane(f.tree, box, schultz_pane_vbox());
    ASSERT_EQ(SCHULTZ_OK, schultz_color_picker_create(f.tree, box,
        schultz_color_rgba(255, 0, 0, 255), &picker));

    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_arrange(f.tree, box,
        schultz_rect_make(0, 0, 400, 600)));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(f.tree, picker, &bounds));

    /*
     * The swatch has no caption to measure, so before it was given a size of
     * its own it came out as its padding stretched across the whole box. The
     * maximum width is what stops the stretching.
     */
    ASSERT_IN_RANGE(88.0f, bounds.width, 0.01f);

    /*
     * And it stands exactly as tall as the controls it sits among, which is
     * a line of text plus a button's inset rather than a number of its own.
     * A plain button beside it is the thing it has to match.
     */
    {
        schultz_handle button = SCHULTZ_HANDLE_NONE;
        schultz_size size;

        ASSERT_EQ(SCHULTZ_OK, schultz_button_create(f.tree, box, "Ok",
                                                    &button));
        schultz_tree_resolve_styles(f.tree);
        ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, button, -1.0f,
                                                     -1.0f, &size));
        ASSERT_IN_RANGE(size.height, bounds.height, 0.01f);
    }

    fixture_teardown(&f);
    PASS();
}

/* Long down and thin across, which is what an axis being vertical means for
 * the size a widget asks for. */
TEST a_vertical_slider_is_taller_than_it_is_wide(void)
{
    control_fixture f;
    schultz_handle across = SCHULTZ_HANDLE_NONE;
    schultz_handle down = SCHULTZ_HANDLE_NONE;
    schultz_size wide;
    schultz_size tall;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                          SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 1.0f, 0.5f,
                          &across);
    schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                          SCHULTZ_ORIENT_VERTICAL, 0.0f, 1.0f, 0.5f, &down);
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, across, -1, -1,
                                                 &wide));
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, down, -1, -1, &tall));

    ASSERT(wide.width > wide.height);
    ASSERT(tall.height > tall.width);
    /* The same two numbers, swapped, rather than two different sliders. */
    ASSERT_EQ(wide.width, tall.height);
    ASSERT_EQ(wide.height, tall.width);

    fixture_teardown(&f);
    PASS();
}

/*
 * A vertical slider counts upward: pressing near the bottom is the minimum
 * and near the top is the maximum. Getting the sign wrong here is the whole
 * of what could go wrong, and it would not show in any other test.
 */
TEST a_vertical_sliders_value_grows_upward(void)
{
    control_fixture f;
    schultz_handle slider = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;
    float middle;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                          SCHULTZ_ORIENT_VERTICAL, 0.0f, 100.0f, 50.0f,
                          &slider);
    place(&f, slider, 10.0f, 10.0f);
    schultz_node_absolute_bounds(f.tree, slider, &bounds);

    /* The middle of the track is the middle of the range, either way up. */
    schultz_events_mouse_move(f.events,
        schultz_point_make(bounds.x + bounds.width * 0.5f,
                           bounds.y + bounds.height * 0.5f), 0);
    schultz_events_mouse_button(f.events,
        schultz_point_make(bounds.x + bounds.width * 0.5f,
                           bounds.y + bounds.height * 0.5f),
        SCHULTZ_BUTTON_LEFT, 1, 0);
    middle = schultz_slider_value(f.tree, slider);
    ASSERT(middle > 45.0f && middle < 55.0f);

    /* Dragging to the top is the maximum, and to the bottom the minimum. */
    schultz_events_mouse_move(f.events,
        schultz_point_make(bounds.x + bounds.width * 0.5f,
                           bounds.y - 200.0f), 0);
    ASSERT_EQ(100.0f, schultz_slider_value(f.tree, slider));
    schultz_events_mouse_move(f.events,
        schultz_point_make(bounds.x + bounds.width * 0.5f,
                           bounds.y + bounds.height + 200.0f), 0);
    ASSERT_EQ(0.0f, schultz_slider_value(f.tree, slider));

    schultz_events_mouse_button(f.events,
        schultz_point_make(bounds.x, bounds.y), SCHULTZ_BUTTON_LEFT, 0, 0);
    fixture_teardown(&f);
    PASS();
}

/* Up steps forward and down steps back, which reads the same way round on a
 * vertical slider as the pointer does. */
TEST an_arrow_key_moves_a_vertical_slider_the_way_it_points(void)
{
    control_fixture f;
    schultz_handle slider = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_slider_create(f.tree, schultz_tree_root(f.tree),
                          SCHULTZ_ORIENT_VERTICAL, 0.0f, 100.0f, 50.0f,
                          &slider);
    schultz_slider_set_value(f.tree, slider, 50.0f);
    schultz_events_set_focus(f.events, slider);

    schultz_events_key(f.events, SCHULTZ_KEY_UP, 0, 1);
    ASSERT(schultz_slider_value(f.tree, slider) > 50.0f);
    schultz_events_key(f.events, SCHULTZ_KEY_DOWN, 0, 1);
    schultz_events_key(f.events, SCHULTZ_KEY_DOWN, 0, 1);
    ASSERT(schultz_slider_value(f.tree, slider) < 50.0f);

    schultz_events_key(f.events, SCHULTZ_KEY_END, 0, 1);
    ASSERT_EQ(100.0f, schultz_slider_value(f.tree, slider));
    schultz_events_key(f.events, SCHULTZ_KEY_HOME, 0, 1);
    ASSERT_EQ(0.0f, schultz_slider_value(f.tree, slider));

    fixture_teardown(&f);
    PASS();
}

/*
 * A vertical bar fills from the bottom, so the filled part touches the bottom
 * edge and stops short of the top. Painting it the other way up would give a
 * rectangle of exactly the same size in exactly the wrong place, which is why
 * this asks where it is rather than how big it is.
 */
TEST a_vertical_progress_bar_fills_from_the_bottom(void)
{
    control_fixture f;
    schultz_handle bar = SCHULTZ_HANDLE_NONE;
    schultz_rect bounds;
    schultz_rect fill;
    uint32_t i;
    uint32_t found = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_progress_bar_create(f.tree, schultz_tree_root(f.tree),
                                          SCHULTZ_ORIENT_VERTICAL, &bar));
    schultz_progress_bar_set_value(f.tree, bar, 0.25f);
    place(&f, bar, 10.0f, 10.0f);
    schultz_node_absolute_bounds(f.tree, bar, &bounds);
    ASSERT(bounds.height > bounds.width);

    ASSERT(paint_all(&f) > 0u);
    /* The second round rectangle is the filled part; the first is the track. */
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);

        if (cmd->kind != (uint32_t)SCHULTZ_DRAW_FILL_ROUND_RECT) {
            continue;
        }
        found++;
        if (found == 2u) {
            fill = cmd->as.fill_round_rect.rect;
        }
    }
    ASSERT_EQ(2u, found);

    /* A quarter full, sitting on the bottom edge. */
    ASSERT(fill.height > bounds.height * 0.2f);
    ASSERT(fill.height < bounds.height * 0.3f);
    ASSERT(fill.y + fill.height > bounds.y + bounds.height - 1.0f);
    ASSERT(fill.y > bounds.y + bounds.height * 0.5f);

    fixture_teardown(&f);
    PASS();
}

/* Neither widget guesses at an orientation it does not recognise. */
TEST a_bad_orientation_is_refused(void)
{
    control_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_slider_create(f.tree, schultz_tree_root(f.tree), 99u,
                                    0.0f, 1.0f, 0.5f, &node));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_progress_bar_create(f.tree, schultz_tree_root(f.tree),
                                          99u, &node));
    fixture_teardown(&f);
    PASS();
}

SUITE(controls)
{
    RUN_TEST(a_vertical_slider_is_taller_than_it_is_wide);
    RUN_TEST(a_vertical_sliders_value_grows_upward);
    RUN_TEST(an_arrow_key_moves_a_vertical_slider_the_way_it_points);
    RUN_TEST(a_vertical_progress_bar_fills_from_the_bottom);
    RUN_TEST(a_bad_orientation_is_refused);
    RUN_TEST(a_number_field_clamps_and_steps);
    RUN_TEST(a_calendars_month_buttons_reach_the_picker);
    RUN_TEST(a_button_is_the_same_height_wherever_it_is_put);
    RUN_TEST(a_disabled_widget_hears_nothing_from_its_own_overlay);
    RUN_TEST(a_date_outside_the_range_cannot_be_chosen);
    RUN_TEST(a_calendar_lays_out_a_whole_month);
    RUN_TEST(a_colour_squares_press_reaches_the_picker);
    RUN_TEST(a_number_fields_steps_are_not_buttons);
    RUN_TEST(a_number_field_refuses_what_is_not_a_number);
    RUN_TEST(a_wrapping_number_field_runs_off_one_end_onto_the_other);
    RUN_TEST(the_calendar_knows_how_long_a_month_is);
    RUN_TEST(a_date_picker_reads_back_what_it_was_given);
    RUN_TEST(a_date_picker_refuses_a_backwards_range);
    RUN_TEST(a_date_picker_takes_a_full_set_of_names_or_none);
    RUN_TEST(a_time_picker_keeps_24_hour_whatever_it_shows);
    RUN_TEST(a_time_picker_fits_every_clock_it_can_show);
    RUN_TEST(a_date_picker_fits_every_format_it_can_show);
    RUN_TEST(a_colour_picker_is_a_swatch_and_not_a_sliver);
    RUN_TEST(a_colour_picker_survives_a_round_trip_through_hsv);
    RUN_TEST(a_colour_picker_holds_alpha_only_when_asked);
    RUN_TEST(a_hyperlink_is_a_button_that_reports_visits);
    RUN_TEST(a_hyperlinks_underline_stops_where_the_words_do);
    RUN_TEST(a_button_bar_orders_by_platform_not_by_insertion);
    RUN_TEST(a_button_bar_puts_accept_last_on_macos);
    RUN_TEST(a_button_bar_sizes_every_button_alike);
    RUN_TEST(an_ungrouped_toggle_button_turns_on_and_off);
    RUN_TEST(a_grouped_toggle_button_clears_the_rest_of_its_group);
    RUN_TEST(focusing_a_control_invalidates_the_ring_too);
    RUN_TEST(a_control_repaints_when_only_its_ring_is_dirty);
    RUN_TEST(the_paint_margin_refuses_a_negative_value);
    RUN_TEST(a_button_carries_its_caption_and_schema);
    RUN_TEST(every_button_hands_back_its_caption);
    RUN_TEST(a_button_sizes_itself_around_its_caption);
    RUN_TEST(a_button_paints_a_box_and_its_caption);
    RUN_TEST(a_caption_wider_than_its_button_is_cut_short);
    RUN_TEST(a_button_wide_enough_for_its_caption_cuts_nothing);
    RUN_TEST(a_button_changes_colour_when_hovered_and_pressed);
    RUN_TEST(a_focused_button_draws_a_ring);
    RUN_TEST(a_button_click_reaches_the_host);
    RUN_TEST(space_and_enter_activate_the_focused_control);
    RUN_TEST(an_ordinary_key_does_not_activate_anything);
    RUN_TEST(a_node_that_cannot_be_clicked_is_not_activated_by_a_key);
    RUN_TEST(a_checkbox_toggles_itself_on_a_click);
    RUN_TEST(a_disabled_checkbox_ignores_a_click);
    RUN_TEST(a_checked_checkbox_draws_a_tick);
    RUN_TEST(a_checkbox_indicator_is_a_square_beside_its_caption);
    RUN_TEST(toggle_accessors_reject_other_widgets);
    RUN_TEST(selecting_a_radio_clears_the_rest_of_its_group);
    RUN_TEST(clicking_a_selected_radio_leaves_it_selected);
    RUN_TEST(a_radio_draws_circles_and_a_dot_when_selected);
    RUN_TEST(a_switch_moves_its_thumb_when_turned_on);
    RUN_TEST(a_switch_is_wider_than_a_checkbox);
    RUN_TEST(a_slider_starts_inside_its_range);
    RUN_TEST(arrow_keys_step_a_slider_and_are_consumed);
    RUN_TEST(a_bad_slider_step_is_refused);
    RUN_TEST(dragging_a_slider_moves_its_value);
    RUN_TEST(a_slider_paints_a_track_a_fill_and_a_thumb);
    RUN_TEST(a_progress_bar_fills_in_proportion_to_its_value);
    RUN_TEST(an_empty_progress_bar_draws_only_its_track);
    RUN_TEST(an_unmeasured_progress_bar_moves_a_marker);
    RUN_TEST(progress_bar_accessors_reject_other_widgets);
    RUN_TEST(a_control_can_be_restyled_by_a_named_style);
    RUN_TEST(controls_reject_a_null_out_handle);
    RUN_TEST(controls_reject_a_stale_parent);
    RUN_TEST(a_control_with_no_size_paints_without_trouble);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(controls);
    GREATEST_MAIN_END();
}
