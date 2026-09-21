/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_overlays.c - the tier 4 overlays, the combo box that needs them, and
 * the two routing features menus depend on: keyboard shortcuts and the
 * secondary button.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_widgets.h"

#define FONT_PATH "assets/fonts/DejaVuSans.ttf"

enum { SEEN_MAX = 64 };

typedef struct {
    uint32_t       types[SEEN_MAX];
    schultz_handle targets[SEEN_MAX];
    uint32_t       count;
} seen_events;

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
} overlay_fixture;

static int32_t fixture_setup(overlay_fixture *f)
{
    int32_t result;

    memset(f, 0, sizeof(*f));
    result = schultz_tree_create(&f->tree);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(f->tree, schultz_rect_make(0, 0, 400, 300));
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

static void fixture_teardown(overlay_fixture *f)
{
    schultz_arena_free(&f->arena);
    schultz_events_destroy(f->events);
    schultz_tree_destroy(f->tree);
    schultz_glyph_cache_destroy(f->glyphs);
    schultz_font_system_destroy(f->fonts);
}

static uint32_t paint_all(overlay_fixture *f)
{
    schultz_arena_reset(&f->arena);
    schultz_draw_list_init(&f->list, &f->arena, 0);
    schultz_tree_resolve_styles(f->tree);
    schultz_widget_paint_tree(f->tree, &f->list, &f->arena,
                              schultz_rect_make(0, 0, 0, 0));
    return schultz_draw_list_count(&f->list);
}

static void click_at(overlay_fixture *f, schultz_point at)
{
    schultz_events_mouse_move(f->events, at, 0);
    schultz_events_mouse_button(f->events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
    schultz_events_mouse_button(f->events, at, SCHULTZ_BUTTON_LEFT, 0, 0);
}

static schultz_point centre_of(overlay_fixture *f, schultz_handle node)
{
    schultz_rect b;

    schultz_node_absolute_bounds(f->tree, node, &b);
    return schultz_point_make(b.x + b.width * 0.5f, b.y + b.height * 0.5f);
}

/* ----------------------------------------------------------------- Menu */

TEST a_menu_is_hidden_until_it_is_opened(void)
{
    overlay_fixture f;
    schultz_handle menu;
    schultz_handle item = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_create(f.tree, &menu));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add(f.tree, menu, "Cut", "Ctrl X",
                                           &item));
    ASSERT_STR_EQ("Cut", schultz_node_get_name(f.tree, item));
    ASSERT_EQ((uint32_t)SCHULTZ_ROLE_MENU_ITEM,
              schultz_node_get_role(f.tree, item));
    ASSERT_EQ((uint32_t)SCHULTZ_ROLE_MENU,
              schultz_node_get_role(f.tree, menu));

    ASSERT_FALSE(schultz_menu_is_open(f.tree, menu));
    ASSERT_FALSE(schultz_node_is_visible(f.tree, menu));
    ASSERT_EQ(0u, schultz_tree_overlay_count(f.tree));
    ASSERT_EQ(0u, paint_all(&f));

    ASSERT_EQ(SCHULTZ_OK, schultz_menu_open_at(f.tree, menu,
                                               schultz_point_make(10, 10)));
    ASSERT(schultz_menu_is_open(f.tree, menu));
    ASSERT_EQ(1u, schultz_tree_overlay_count(f.tree));
    ASSERT(paint_all(&f) > 0u);

    fixture_teardown(&f);
    PASS();
}

TEST a_menu_opens_beside_what_it_belongs_to(void)
{
    overlay_fixture f;
    schultz_handle menu;
    schultz_handle button;
    schultz_handle item = SCHULTZ_HANDLE_NONE;
    schultz_rect anchor;
    schultz_rect at;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "File", &button);
    schultz_node_set_bounds(f.tree, button, schultz_rect_make(20, 30, 60,
                                                              24));
    schultz_menu_create(f.tree, &menu);
    schultz_menu_add(f.tree, menu, "Open", NULL, &item);

    ASSERT_EQ(SCHULTZ_OK, schultz_menu_open_for(f.tree, menu, button,
                                                SCHULTZ_PLACE_BELOW));
    schultz_node_absolute_bounds(f.tree, button, &anchor);
    schultz_node_absolute_bounds(f.tree, menu, &at);
    ASSERT_EQ(anchor.x, at.x);
    ASSERT_EQ(anchor.y + anchor.height, at.y);

    fixture_teardown(&f);
    PASS();
}

/* An overlay that would hang off the window is pulled back inside it. */
TEST a_menu_stays_inside_the_window(void)
{
    overlay_fixture f;
    schultz_handle menu;
    schultz_handle item = SCHULTZ_HANDLE_NONE;
    schultz_rect at;
    schultz_rect viewport;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_menu_create(f.tree, &menu);
    schultz_menu_add(f.tree, menu, "A rather long menu row", NULL, &item);
    schultz_menu_add(f.tree, menu, "Another one", NULL, &item);

    /* Opened right at the bottom corner, where it cannot possibly fit. */
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_open_at(f.tree, menu,
                                               schultz_point_make(395, 295)));
    schultz_tree_get_viewport(f.tree, &viewport);
    schultz_node_absolute_bounds(f.tree, menu, &at);

    ASSERT(at.x >= viewport.x);
    ASSERT(at.y >= viewport.y);
    ASSERT(at.x + at.width <= viewport.x + viewport.width);
    ASSERT(at.y + at.height <= viewport.y + viewport.height);

    fixture_teardown(&f);
    PASS();
}

TEST choosing_a_row_closes_the_menu_and_tells_the_host(void)
{
    overlay_fixture f;
    schultz_handle menu;
    schultz_handle item = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_menu_create(f.tree, &menu);
    schultz_menu_add(f.tree, menu, "Paste", NULL, &item);
    schultz_menu_open_at(f.tree, menu, schultz_point_make(10, 10));

    click_at(&f, centre_of(&f, item));

    ASSERT_FALSE(schultz_menu_is_open(f.tree, menu));
    ASSERT_EQ(0u, schultz_tree_overlay_count(f.tree));
    /* Closing is the menu's business; acting on the choice is the host's. */
    ASSERT_EQ(1, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, item));

    fixture_teardown(&f);
    PASS();
}

TEST a_press_outside_a_menu_closes_it(void)
{
    overlay_fixture f;
    schultz_handle menu;
    schultz_handle item = SCHULTZ_HANDLE_NONE;
    schultz_handle behind;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Behind",
                          &behind);
    schultz_node_set_bounds(f.tree, behind,
                            schultz_rect_make(200, 200, 80, 30));
    schultz_menu_create(f.tree, &menu);
    schultz_menu_add(f.tree, menu, "Row", NULL, &item);
    schultz_menu_open_at(f.tree, menu, schultz_point_make(10, 10));

    click_at(&f, schultz_point_make(240, 215));

    ASSERT_FALSE(schultz_menu_is_open(f.tree, menu));
    /* The press that closed it did not also press what was underneath. */
    ASSERT_EQ(0, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, behind));

    fixture_teardown(&f);
    PASS();
}

TEST escape_closes_a_menu(void)
{
    overlay_fixture f;
    schultz_handle menu;
    schultz_handle item = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_menu_create(f.tree, &menu);
    schultz_menu_add(f.tree, menu, "Row", NULL, &item);
    schultz_menu_open_at(f.tree, menu, schultz_point_make(10, 10));
    schultz_events_set_focus(f.events, item);

    schultz_events_key(f.events, SCHULTZ_KEY_ESCAPE, 0, 1);
    ASSERT_FALSE(schultz_menu_is_open(f.tree, menu));

    fixture_teardown(&f);
    PASS();
}

/*
 * A menu when the window changes size underneath it.
 *
 * An overlay is placed against its anchor and then nudged back inside the
 * window if that would hang it off an edge. That nudge is only right for the
 * window it was worked out against, and the window can change while the menu
 * is open: on a phone a rotation is a resize, and it dismisses nothing.
 */
TEST a_menu_stays_inside_a_window_that_shrinks(void)
{
    overlay_fixture f;
    schultz_handle menu;
    schultz_handle item = SCHULTZ_HANDLE_NONE;
    schultz_rect at;
    schultz_rect window;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_menu_create(f.tree, &menu);
    schultz_menu_add(f.tree, menu, "A row with some width", NULL, &item);

    /* Opened near the right hand edge of a wide window. */
    schultz_menu_open_at(f.tree, menu, schultz_point_make(300, 20));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, menu, &at));
    schultz_tree_get_viewport(f.tree, &window);
    ASSERT(at.x + at.width <= window.width);

    /* The window is now narrower, and nobody told the menu. Still wide
     * enough to hold it, so it has somewhere to be nudged to. */
    window = schultz_rect_make(0, 0, 300, 300);
    schultz_tree_set_viewport(f.tree, window);
    schultz_node_set_bounds(f.tree, schultz_tree_root(f.tree), window);
    schultz_node_invalidate_layout(f.tree, schultz_tree_root(f.tree));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);

    ASSERT(schultz_menu_is_open(f.tree, menu));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, menu, &at));
    /* Nudged back inside rather than left hanging off the new edge. */
    ASSERT(at.x >= 0.0f);
    ASSERT(at.x + at.width <= window.width);

    fixture_teardown(&f);
    PASS();
}

/*
 * A menu under a root that has a pane.
 *
 * A menu is a child of the root, and a pane places its children in a row or
 * a column. A menu is placed against whatever opened it instead, so a pane
 * must leave it alone: otherwise opening a menu shuffles the interface and
 * the menu appears wherever the pane felt like putting it.
 */
TEST a_menu_is_not_arranged_into_the_flow(void)
{
    overlay_fixture f;
    schultz_handle menu;
    schultz_handle sibling;
    schultz_handle item = SCHULTZ_HANDLE_NONE;
    schultz_rect at;
    schultz_rect beside;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_node_set_pane(f.tree, schultz_tree_root(f.tree),
                          schultz_pane_vbox());
    schultz_node_set_bounds(f.tree, schultz_tree_root(f.tree),
                            schultz_rect_make(0, 0, 400, 300));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Beside",
                          &sibling);

    schultz_menu_create(f.tree, &menu);
    schultz_menu_add(f.tree, menu, "Row", NULL, &item);
    schultz_menu_open_at(f.tree, menu, schultz_point_make(120, 90));

    schultz_node_invalidate_layout(f.tree, schultz_tree_root(f.tree));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, menu, &at));
    /* Where it was opened, not stacked under the button by the column. */
    ASSERT_EQ(120.0f, at.x);
    ASSERT_EQ(90.0f, at.y);

    /* And the button is still where a column would put it, so the pane did
     * its job for everything that is not an overlay. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, sibling,
                                                       &beside));
    ASSERT_EQ(0.0f, beside.y);

    fixture_teardown(&f);
    PASS();
}

TEST menu_accessors_reject_other_widgets(void)
{
    overlay_fixture f;
    schultz_handle panel;
    schultz_handle item = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_menu_add(f.tree, panel, "x", NULL, &item));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_menu_open_at(f.tree, panel, schultz_point_make(0, 0)));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_menu_close(f.tree, panel));
    ASSERT_EQ(0, schultz_menu_is_open(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT, schultz_menu_create(f.tree, NULL));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------- Popover and Tooltip */

TEST a_popover_opens_and_closes_beside_a_node(void)
{
    overlay_fixture f;
    schultz_handle pop;
    schultz_handle anchor;
    schultz_handle content;
    schultz_rect anchored;
    schultz_rect at;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &anchor);
    schultz_node_set_bounds(f.tree, anchor, schultz_rect_make(50, 60, 40,
                                                              20));
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_create(f.tree, 1, &pop));

    content = schultz_popup_content(f.tree, pop);
    ASSERT(content != SCHULTZ_HANDLE_NONE);
    schultz_node_set_style_property(f.tree, content, SCHULTZ_PROP_PREF_WIDTH,
                                    schultz_value_number(60.0f));
    schultz_node_set_style_property(f.tree, content, SCHULTZ_PROP_PREF_HEIGHT,
                                    schultz_value_number(40.0f));

    ASSERT_FALSE(schultz_popup_is_open(f.tree, pop));
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_open_at(f.tree, pop, anchor,
                                                  SCHULTZ_PLACE_RIGHT));
    ASSERT(schultz_popup_is_open(f.tree, pop));

    schultz_node_absolute_bounds(f.tree, anchor, &anchored);
    schultz_node_absolute_bounds(f.tree, pop, &at);
    /* Beside it, with the clearance a popover keeps from what it belongs to. */
    ASSERT_EQ(anchored.x + anchored.width + 4.0f, at.x);

    ASSERT_EQ(SCHULTZ_OK, schultz_popup_close(f.tree, pop));
    ASSERT_FALSE(schultz_popup_is_open(f.tree, pop));
    ASSERT_EQ(0u, schultz_tree_overlay_count(f.tree));

    /* Closing one that is already closed changes nothing. */
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_close(f.tree, pop));

    fixture_teardown(&f);
    PASS();
}

TEST a_tooltip_never_takes_the_pointer(void)
{
    overlay_fixture f;
    schultz_handle tip;
    schultz_handle anchor;
    schultz_handle hit = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Hover", &anchor);
    schultz_node_set_bounds(f.tree, anchor, schultz_rect_make(0, 0, 80, 30));
    ASSERT_EQ(SCHULTZ_OK, schultz_tooltip_create(f.tree, "Explains itself",
                                                 &tip));

    ASSERT_EQ(SCHULTZ_OK, schultz_popup_open_at(f.tree, tip, anchor,
                                                  SCHULTZ_PLACE_OVER));
    ASSERT(schultz_popup_is_open(f.tree, tip));

    /* Sitting right on top of the button, and the button is still reachable. */
    schultz_events_hit_test(f.events, schultz_point_make(40, 15), &hit);
    ASSERT_EQ(anchor, hit);

    fixture_teardown(&f);
    PASS();
}

/*
 * A container that hands back a node to fill gives it a pane, so one child
 * measures and fills it without the application saying so. Left bare the
 * container measures its padding and nothing inside it is ever asked its
 * size, which is a tooltip the size of a full stop.
 */
TEST a_popover_measures_what_was_put_inside_it(void)
{
    overlay_fixture f;
    schultz_handle tip;
    schultz_handle pop;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_size tip_size;
    schultz_size text;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_tooltip_create(f.tree, "Explains itself",
                                                 &tip));
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, tip, -1, -1,
                                                 &tip_size));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree,
                    schultz_popup_content(f.tree, tip), 0, &label));
    schultz_layout_measure(f.tree, label, -1, -1, &text);
    ASSERT(text.width > 0.0f);
    /* Around the text, with padding, rather than the padding alone. */
    ASSERT(tip_size.width > text.width);
    ASSERT(tip_size.height > text.height);

    /* And the same for a popover the application fills itself. */
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_create(f.tree, 1, &pop));
    schultz_button_create(f.tree, schultz_popup_content(f.tree, pop), "OK",
                          &label);
    schultz_tree_resolve_styles(f.tree);
    {
        schultz_size pop_size;
        schultz_size button;

        schultz_layout_measure(f.tree, pop, -1, -1, &pop_size);
        schultz_layout_measure(f.tree, label, -1, -1, &button);
        ASSERT(pop_size.width > button.width);
    }

    fixture_teardown(&f);
    PASS();
}

TEST popover_accessors_reject_other_widgets(void)
{
    overlay_fixture f;
    schultz_handle panel;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_popup_content(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_popup_open(f.tree, panel, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_popup_open_at(f.tree, panel, panel, 0));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_popup_close(f.tree, panel));
    ASSERT_EQ(0, schultz_popup_is_open(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_popup_create(f.tree, 0, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_tooltip_create(f.tree, "x", NULL));

    fixture_teardown(&f);
    PASS();
}

/* --------------------------------------------------------------- Dialog */

TEST a_dialog_covers_the_window_and_swallows_presses_beside_it(void)
{
    overlay_fixture f;
    schultz_handle dialog;
    schultz_handle behind;
    schultz_rect at;
    schultz_rect viewport;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Behind",
                          &behind);
    schultz_node_set_bounds(f.tree, behind, schultz_rect_make(0, 0, 60, 20));
    ASSERT_EQ(SCHULTZ_OK, schultz_dialog_create(f.tree, "Are you sure",
                                                &dialog));
    ASSERT(schultz_dialog_content(f.tree, dialog) != SCHULTZ_HANDLE_NONE);

    ASSERT_FALSE(schultz_dialog_is_open(f.tree, dialog));
    ASSERT_EQ(SCHULTZ_OK, schultz_dialog_open(f.tree, dialog));
    ASSERT(schultz_dialog_is_open(f.tree, dialog));

    schultz_tree_get_viewport(f.tree, &viewport);
    schultz_node_absolute_bounds(f.tree, dialog, &at);
    ASSERT_EQ(viewport.width, at.width);
    ASSERT_EQ(viewport.height, at.height);

    /* A press beside the panel neither closes it nor reaches what is under. */
    click_at(&f, schultz_point_make(20, 10));
    ASSERT(schultz_dialog_is_open(f.tree, dialog));
    ASSERT_EQ(0, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, behind));

    /* Escape does close it. */
    schultz_events_set_focus(f.events, dialog);
    schultz_events_key(f.events, SCHULTZ_KEY_ESCAPE, 0, 1);
    ASSERT_FALSE(schultz_dialog_is_open(f.tree, dialog));

    fixture_teardown(&f);
    PASS();
}

/*
 * A window that changes size while a dialog is open.
 *
 * Something outside the application can do this at any moment: a phone being
 * turned, a keyboard appearing, the system putting a band across the top. A
 * dialog is the awkward case because it sizes itself from the viewport rather
 * than from a parent, so it is the one thing on screen that has to be told
 * twice.
 */
TEST a_dialog_survives_the_window_changing_size(void)
{
    overlay_fixture f;
    schultz_handle dialog;
    schultz_rect at;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_dialog_create(f.tree, "Are you sure",
                                                &dialog));
    ASSERT_EQ(SCHULTZ_OK, schultz_dialog_open(f.tree, dialog));
    /* It says so itself; nothing outside has to know to refit it. */
    ASSERT(schultz_node_fills_viewport(f.tree, dialog));

    /* Several times, and back again, the way a rotation and a keyboard do. */
    for (i = 0; i < 4; i++) {
        schultz_rect window = (i % 2)
            ? schultz_rect_make(0, 0, 400, 300)
            : schultz_rect_make(0, 0, 812, 375);

        schultz_tree_set_viewport(f.tree, window);
        schultz_node_set_bounds(f.tree, schultz_tree_root(f.tree), window);
        schultz_node_invalidate_layout(f.tree, schultz_tree_root(f.tree));
        schultz_node_invalidate(f.tree, schultz_tree_root(f.tree));

        schultz_tree_resolve_styles(f.tree);
        schultz_layout_run(f.tree);

        ASSERT(schultz_dialog_is_open(f.tree, dialog));
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_node_absolute_bounds(f.tree, dialog, &at));
        /* It covers the window it is in, not the one it was opened in. */
        ASSERT_EQ(window.width, at.width);
        ASSERT_EQ(window.height, at.height);
    }

    /* And it still closes. */
    schultz_events_set_focus(f.events, dialog);
    schultz_events_key(f.events, SCHULTZ_KEY_ESCAPE, 0, 1);
    ASSERT_FALSE(schultz_dialog_is_open(f.tree, dialog));

    fixture_teardown(&f);
    PASS();
}

/*
 * A dialog under a root that has a pane.
 *
 * A dialog is a child of the root, so a pane on the root arranges it along
 * with everything else and it stops covering the window. Until this worked
 * the rule was that the root must not have a pane, which is not a rule
 * anything enforced and not one a host would guess.
 */
TEST a_dialog_is_not_arranged_into_the_flow(void)
{
    overlay_fixture f;
    schultz_handle dialog;
    schultz_handle sibling;
    schultz_rect viewport;
    schultz_rect at;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    /* The thing that used to break it. */
    schultz_node_set_pane(f.tree, schultz_tree_root(f.tree),
                          schultz_pane_vbox());
    schultz_tree_get_viewport(f.tree, &viewport);
    schultz_node_set_bounds(f.tree, schultz_tree_root(f.tree), viewport);

    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Beside",
                          &sibling);
    ASSERT_EQ(SCHULTZ_OK, schultz_dialog_create(f.tree, "Are you sure",
                                                &dialog));
    ASSERT_EQ(SCHULTZ_OK, schultz_dialog_open(f.tree, dialog));

    schultz_node_invalidate_layout(f.tree, schultz_tree_root(f.tree));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);

    /* Still covering the window, not stacked under the button. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, dialog, &at));
    ASSERT_EQ(viewport.width, at.width);
    ASSERT_EQ(viewport.height, at.height);
    ASSERT_EQ(0.0f, at.y);

    /* And still swallowing a press meant for what is behind it. */
    click_at(&f, schultz_point_make(20, 10));
    ASSERT(schultz_dialog_is_open(f.tree, dialog));
    ASSERT_EQ(0, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, sibling));

    fixture_teardown(&f);
    PASS();
}

TEST a_click_inside_a_dialog_reaches_the_host(void)
{
    overlay_fixture f;
    schultz_handle dialog;
    schultz_handle ok;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_dialog_create(f.tree, "Title", &dialog);
    schultz_button_create(f.tree, schultz_dialog_content(f.tree, dialog),
                          "OK", &ok);
    schultz_dialog_open(f.tree, dialog);
    schultz_node_set_bounds(f.tree, ok, schultz_rect_make(0, 0, 60, 24));

    click_at(&f, centre_of(&f, ok));
    ASSERT_EQ(1, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, ok));

    fixture_teardown(&f);
    PASS();
}

TEST dialog_accessors_reject_other_widgets(void)
{
    overlay_fixture f;
    schultz_handle panel;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_dialog_content(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE, schultz_dialog_open(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE, schultz_dialog_close(f.tree, panel));
    ASSERT_EQ(0, schultz_dialog_is_open(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_dialog_create(f.tree, "x", NULL));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------- ComboBox */

TEST a_combo_box_shows_its_first_item(void)
{
    overlay_fixture f;
    schultz_handle combo;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_combo_box_create(f.tree,
                    schultz_tree_root(f.tree), &combo));
    ASSERT_EQ(0u, schultz_combo_box_count(f.tree, combo));

    ASSERT_EQ(SCHULTZ_OK, schultz_combo_box_add(f.tree, combo, "Small"));
    ASSERT_EQ(SCHULTZ_OK, schultz_combo_box_add(f.tree, combo, "Medium"));
    ASSERT_EQ(SCHULTZ_OK, schultz_combo_box_add(f.tree, combo, "Large"));
    ASSERT_EQ(3u, schultz_combo_box_count(f.tree, combo));

    /* Never blank: the first one added is the choice. */
    ASSERT_EQ(0u, schultz_combo_box_selected(f.tree, combo));
    ASSERT_STR_EQ("Small", schultz_node_get_value(f.tree, combo));

    ASSERT_EQ(SCHULTZ_OK, schultz_combo_box_select(f.tree, combo, 2));
    ASSERT_STR_EQ("Large", schultz_node_get_value(f.tree, combo));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_combo_box_select(f.tree, combo, 9));

    fixture_teardown(&f);
    PASS();
}

TEST clicking_a_combo_box_opens_its_menu_and_choosing_closes_it(void)
{
    overlay_fixture f;
    schultz_handle combo;
    schultz_handle menu;
    schultz_handle row = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_combo_box_create(f.tree, schultz_tree_root(f.tree), &combo);
    schultz_combo_box_add(f.tree, combo, "One");
    schultz_combo_box_add(f.tree, combo, "Two");
    schultz_node_set_bounds(f.tree, combo, schultz_rect_make(10, 10, 120,
                                                             28));
    menu = schultz_combo_box_menu(f.tree, combo);
    ASSERT(menu != SCHULTZ_HANDLE_NONE);

    click_at(&f, centre_of(&f, combo));
    ASSERT(schultz_menu_is_open(f.tree, menu));

    /* The second row is the menu's second child. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, menu, 1, &row));
    click_at(&f, centre_of(&f, row));

    ASSERT_FALSE(schultz_menu_is_open(f.tree, menu));
    ASSERT_EQ(1u, schultz_combo_box_selected(f.tree, combo));
    ASSERT_STR_EQ("Two", schultz_node_get_value(f.tree, combo));

    fixture_teardown(&f);
    PASS();
}

TEST arrow_keys_move_a_combo_box_without_opening_it(void)
{
    overlay_fixture f;
    schultz_handle combo;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_combo_box_create(f.tree, schultz_tree_root(f.tree), &combo);
    schultz_combo_box_add(f.tree, combo, "One");
    schultz_combo_box_add(f.tree, combo, "Two");
    schultz_node_set_bounds(f.tree, combo, schultz_rect_make(0, 0, 100, 28));
    schultz_events_set_focus(f.events, combo);

    schultz_events_key(f.events, SCHULTZ_KEY_DOWN, 0, 1);
    ASSERT_EQ(1u, schultz_combo_box_selected(f.tree, combo));
    ASSERT_FALSE(schultz_menu_is_open(f.tree,
                                      schultz_combo_box_menu(f.tree, combo)));

    /* Past the end stays put rather than wrapping. */
    schultz_events_key(f.events, SCHULTZ_KEY_DOWN, 0, 1);
    ASSERT_EQ(1u, schultz_combo_box_selected(f.tree, combo));

    schultz_events_key(f.events, SCHULTZ_KEY_UP, 0, 1);
    ASSERT_EQ(0u, schultz_combo_box_selected(f.tree, combo));
    schultz_events_key(f.events, SCHULTZ_KEY_UP, 0, 1);
    ASSERT_EQ(0u, schultz_combo_box_selected(f.tree, combo));

    fixture_teardown(&f);
    PASS();
}

/*
 * A stack pane would centre the text on both axes or push it into a corner on
 * both. A combo box wants its text at the left and in the middle, with the
 * arrow's room kept clear on the right.
 */
TEST a_combo_box_centres_its_text_and_keeps_room_for_the_arrow(void)
{
    overlay_fixture f;
    schultz_handle combo;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_size natural;
    schultz_size text;
    schultz_rect box;
    schultz_rect at;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_combo_box_create(f.tree, schultz_tree_root(f.tree), &combo);
    schultz_combo_box_add(f.tree, combo, "Comfortable");
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, combo, 0, &label));

    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, combo, -1, -1, &natural);
    schultz_layout_measure(f.tree, label, -1, -1, &text);
    /* Wider than its text, by the room the arrow needs. */
    ASSERT(natural.width > text.width + 10.0f);

    schultz_layout_arrange(f.tree, combo,
                           schultz_rect_make(10, 10, natural.width, 40.0f));
    schultz_node_absolute_bounds(f.tree, combo, &box);
    schultz_node_absolute_bounds(f.tree, label, &at);

    /* Left aligned, with the same gap above and below. */
    ASSERT(at.x - box.x < 12.0f);
    {
        float above = at.y - box.y;
        float below = (box.y + box.height) - (at.y + at.height);
        float slack = (above > below) ? above - below : below - above;

        ASSERT(slack < 1.0f);
    }
    /* And it stops before the arrow. */
    ASSERT(at.x + at.width <= box.x + box.width - 10.0f);

    fixture_teardown(&f);
    PASS();
}

/*
 * A box that changes size when a choice is made shifts everything beside it,
 * so its width comes from the longest choice rather than the chosen one.
 */
TEST a_combo_box_keeps_its_width_whatever_is_chosen(void)
{
    overlay_fixture f;
    schultz_handle combo;
    schultz_size first;
    schultz_size after;
    schultz_size grown;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_combo_box_create(f.tree, schultz_tree_root(f.tree), &combo);
    schultz_combo_box_add(f.tree, combo, "Comfortable");
    schultz_combo_box_add(f.tree, combo, "Cosy");
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, combo, -1, -1, &first);

    /* Choosing the short one must not shrink the box. */
    ASSERT_EQ(SCHULTZ_OK, schultz_combo_box_select(f.tree, combo, 1));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, combo, -1, -1, &after);
    ASSERT_EQ(first.width, after.width);

    /* A longer choice than any before it does make it wider. */
    schultz_combo_box_add(f.tree, combo, "Considerably roomier than that");
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, combo, -1, -1, &grown);
    ASSERT(grown.width > first.width);

    fixture_teardown(&f);
    PASS();
}

TEST a_combo_box_menu_is_as_wide_as_its_button(void)
{
    overlay_fixture f;
    schultz_handle combo;
    schultz_handle menu;
    schultz_rect box;
    schultz_rect opened;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_combo_box_create(f.tree, schultz_tree_root(f.tree), &combo);
    schultz_combo_box_add(f.tree, combo, "One");
    schultz_combo_box_add(f.tree, combo, "Two");
    menu = schultz_combo_box_menu(f.tree, combo);
    schultz_node_set_bounds(f.tree, combo, schultz_rect_make(10, 10, 160,
                                                             28));

    click_at(&f, centre_of(&f, combo));
    ASSERT(schultz_menu_is_open(f.tree, menu));

    schultz_node_absolute_bounds(f.tree, combo, &box);
    schultz_node_absolute_bounds(f.tree, menu, &opened);
    ASSERT_EQ(box.width, opened.width);
    ASSERT_EQ(box.x, opened.x);
    ASSERT_EQ(box.y + box.height, opened.y);

    fixture_teardown(&f);
    PASS();
}

TEST combo_box_accessors_reject_other_widgets(void)
{
    overlay_fixture f;
    schultz_handle panel;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    ASSERT_EQ(0u, schultz_combo_box_count(f.tree, panel));
    ASSERT_EQ(0u, schultz_combo_box_selected(f.tree, panel));
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_combo_box_menu(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_combo_box_add(f.tree, panel, "x"));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_combo_box_select(f.tree, panel, 0));

    fixture_teardown(&f);
    PASS();
}

/* ---------------------------------------------- shortcuts and the menu key */

TEST a_shortcut_activates_a_node_wherever_focus_is(void)
{
    overlay_fixture f;
    schultz_handle button;
    schultz_handle other;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Save", &button);
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Other", &other);
    schultz_node_set_bounds(f.tree, other, schultz_rect_make(0, 0, 60, 24));
    schultz_events_set_focus(f.events, other);

    ASSERT_EQ(SCHULTZ_OK, schultz_events_add_accelerator(f.events, 's',
                                                         SCHULTZ_MOD_CTRL,
                                                         button));
    schultz_events_key(f.events, 's', SCHULTZ_MOD_CTRL, 1);
    ASSERT_EQ(1, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, button));
    /* The key did not also press whatever had focus. */
    ASSERT_EQ(0, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, other));

    /* Without the modifier it is just a key. */
    schultz_events_key(f.events, 's', 0, 1);
    ASSERT_EQ(1, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, button));

    /* Registering the same keys again replaces what was there. */
    ASSERT_EQ(SCHULTZ_OK, schultz_events_add_accelerator(f.events, 's',
                                                         SCHULTZ_MOD_CTRL,
                                                         other));
    schultz_events_key(f.events, 's', SCHULTZ_MOD_CTRL, 1);
    ASSERT_EQ(1, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, button));
    ASSERT_EQ(1, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, other));

    fixture_teardown(&f);
    PASS();
}

/*
 * A text field typing control with V is not also a paste command aimed at a
 * menu, which is why the shortcut is offered after the key.
 */
TEST a_shortcut_does_not_fire_when_the_key_was_wanted(void)
{
    overlay_fixture f;
    schultz_handle field;
    schultz_handle button;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_text_field_create(f.tree, schultz_tree_root(f.tree), "abc",
                              &field);
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Paste",
                          &button);
    schultz_node_set_bounds(f.tree, field, schultz_rect_make(0, 0, 100, 24));
    schultz_events_set_focus(f.events, field);
    schultz_events_add_accelerator(f.events, 'v', SCHULTZ_MOD_CTRL, button);

    schultz_events_key(f.events, 'v', SCHULTZ_MOD_CTRL, 1);
    ASSERT_EQ(0, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, button));

    fixture_teardown(&f);
    PASS();
}

TEST the_shortcut_table_has_a_limit_and_can_be_cleared(void)
{
    overlay_fixture f;
    schultz_handle button;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "x", &button);

    for (i = 0; i < SCHULTZ_ACCELERATORS_MAX; i++) {
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_events_add_accelerator(f.events, 'a' + i,
                                                 SCHULTZ_MOD_CTRL, button));
    }
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED,
              schultz_events_add_accelerator(f.events, '0', SCHULTZ_MOD_CTRL,
                                             button));

    /* Removing one is setting it to nothing. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_events_add_accelerator(f.events, 'a', SCHULTZ_MOD_CTRL,
                                             SCHULTZ_HANDLE_NONE));
    schultz_events_key(f.events, 'a', SCHULTZ_MOD_CTRL, 1);
    ASSERT_EQ(0, seen_count(&f.seen, SCHULTZ_EVENT_CLICK, button));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_events_add_accelerator(f.events, 0u, 0u, button));

    fixture_teardown(&f);
    PASS();
}

TEST the_secondary_button_asks_for_a_context_menu(void)
{
    overlay_fixture f;
    schultz_handle panel;
    schultz_point at = schultz_point_make(30, 30);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 100, 100));

    schultz_events_mouse_move(f.events, at, 0);
    schultz_events_mouse_button(f.events, at, SCHULTZ_BUTTON_RIGHT, 1, 0);

    ASSERT_EQ(1, seen_count(&f.seen, SCHULTZ_EVENT_CONTEXT_MENU, panel));
    /* It asked for a menu rather than pressing what was under it. */
    ASSERT_EQ(0, seen_count(&f.seen, SCHULTZ_EVENT_MOUSE_DOWN, panel));
    ASSERT_FALSE(schultz_node_get_state(f.tree, panel) &
                 SCHULTZ_STATE_PRESSED);

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------- menu item kinds */

/* Lays the tree out, then clicks the middle of a node. */
static void click_node(schultz_events *events, schultz_tree *tree,
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

TEST a_check_row_toggles_and_leaves_the_menu_open(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle menu = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_create(tree, &menu));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add_check(tree, menu, "Word wrap",
                                                 NULL, 0, &row));
    ASSERT_EQ(SCHULTZ_MENU_ITEM_CHECK, schultz_menu_item_kind(tree, row));
    schultz_menu_open_at(tree, menu, schultz_point_make(10.0f, 10.0f));

    ASSERT_EQ(0, schultz_menu_item_checked(tree, row));
    click_node(events, tree, row);
    ASSERT_EQ(1, schultz_menu_item_checked(tree, row));
    /* Still open, which is the point of a tick. */
    ASSERT_EQ(1, schultz_menu_is_open(tree, menu));

    click_node(events, tree, row);
    ASSERT_EQ(0, schultz_menu_item_checked(tree, row));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

TEST a_radio_row_clears_the_rest_of_its_set(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle menu = SCHULTZ_HANDLE_NONE;
    schultz_handle small = SCHULTZ_HANDLE_NONE;
    schultz_handle large = SCHULTZ_HANDLE_NONE;
    schultz_handle other = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_create(tree, &menu));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add_radio(tree, menu, "Small", NULL,
                                                 1u, &small));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add_radio(tree, menu, "Large", NULL,
                                                 1u, &large));
    /* A second set in the same menu is left alone. */
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add_radio(tree, menu, "Metric", NULL,
                                                 2u, &other));
    schultz_menu_radio_select(tree, other);
    schultz_menu_open_at(tree, menu, schultz_point_make(10.0f, 10.0f));

    click_node(events, tree, small);
    ASSERT_EQ(1, schultz_menu_item_checked(tree, small));

    click_node(events, tree, large);
    ASSERT_EQ(0, schultz_menu_item_checked(tree, small));
    ASSERT_EQ(1, schultz_menu_item_checked(tree, large));
    ASSERT_EQ(1, schultz_menu_item_checked(tree, other));
    ASSERT_EQ(1, schultz_menu_is_open(tree, menu));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

/* A rule is not a choice, so the keyboard steps over it. */
TEST a_separator_row_takes_no_focus(void)
{
    schultz_tree *tree;
    schultz_handle menu = SCHULTZ_HANDLE_NONE;
    schultz_handle rule = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_create(tree, &menu));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add_separator(tree, menu, &rule));

    ASSERT_EQ(SCHULTZ_MENU_ITEM_SEPARATOR, schultz_menu_item_kind(tree, rule));
    ASSERT_EQ(0u, schultz_node_get_actions(tree, rule));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_custom_row_holds_whatever_it_is_given(void)
{
    schultz_tree *tree;
    schultz_handle menu = SCHULTZ_HANDLE_NONE;
    schultz_handle content = SCHULTZ_HANDLE_NONE;
    schultz_handle slider = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_create(tree, &menu));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add_custom(tree, menu, &content));
    ASSERT(content != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(SCHULTZ_OK, schultz_slider_create(tree, content,
                                                SCHULTZ_ORIENT_HORIZONTAL,
                                                0.0f, 1.0f,
                                                0.5f, &slider));
    ASSERT_EQ(1u, schultz_node_child_count(tree, content));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_submenu_row_opens_another_menu(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle menu = SCHULTZ_HANDLE_NONE;
    schultz_handle sub = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_handle deep = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_create(tree, &menu));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add_submenu(tree, menu, "Recent",
                                                   &sub));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add(tree, sub, "One", NULL, &deep));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, menu, 0, &row));
    ASSERT_EQ(SCHULTZ_MENU_ITEM_SUBMENU, schultz_menu_item_kind(tree, row));

    schultz_menu_open_at(tree, menu, schultz_point_make(10.0f, 10.0f));
    ASSERT_EQ(0, schultz_menu_is_open(tree, sub));

    /* The pointer resting on the row is what opens it. */
    schultz_node_set_state(tree, row,
        schultz_node_get_state(tree, row) | SCHULTZ_STATE_HOVERED);
    schultz_tree_advance(tree, 0u);
    schultz_tree_advance(tree, 1000u);
    ASSERT_EQ(1, schultz_menu_is_open(tree, sub));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

/* --------------------------------------------- menu buttons and the bar */

TEST a_menu_button_opens_its_menu_instead_of_reporting(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    schultz_handle menu;
    schultz_handle row = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_button_create(tree,
        schultz_tree_root(tree), "Actions", &button));
    schultz_node_set_bounds(tree, button, schultz_rect_make(0, 0, 120, 30));

    menu = schultz_menu_button_menu(tree, button);
    ASSERT(menu != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add(tree, menu, "Rename", NULL, &row));

    ASSERT_EQ(0, schultz_menu_is_open(tree, menu));
    click_node(events, tree, button);
    ASSERT_EQ(1, schultz_menu_is_open(tree, menu));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

/* The two halves of a split button do different things. */
TEST a_split_menu_button_separates_the_click_from_the_arrow(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    schultz_handle menu;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_rect b;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_split_menu_button_create(tree,
        schultz_tree_root(tree), "Save", &button));
    schultz_node_set_bounds(tree, button, schultz_rect_make(0, 0, 160, 30));
    menu = schultz_menu_button_menu(tree, button);
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add(tree, menu, "Save as", NULL, &row));
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);
    schultz_node_absolute_bounds(tree, button, &b);

    /* The wide part: no menu. */
    {
        schultz_point at = schultz_point_make(b.x + 20.0f,
                                              b.y + b.height * 0.5f);

        schultz_events_mouse_move(events, at, 0);
        schultz_events_mouse_button(events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
        schultz_events_mouse_button(events, at, SCHULTZ_BUTTON_LEFT, 0, 0);
    }
    ASSERT_EQ(0, schultz_menu_is_open(tree, menu));

    /* The arrow: menu. */
    {
        schultz_point at = schultz_point_make(b.x + b.width - 8.0f,
                                              b.y + b.height * 0.5f);

        schultz_events_mouse_move(events, at, 0);
        schultz_events_mouse_button(events, at, SCHULTZ_BUTTON_LEFT, 1, 0);
        schultz_events_mouse_button(events, at, SCHULTZ_BUTTON_LEFT, 0, 0);
    }
    ASSERT_EQ(1, schultz_menu_is_open(tree, menu));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

TEST a_menu_bar_swaps_menus_as_the_pointer_moves_along_it(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle bar = SCHULTZ_HANDLE_NONE;
    schultz_handle file = SCHULTZ_HANDLE_NONE;
    schultz_handle edit = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_handle title = SCHULTZ_HANDLE_NONE;
    schultz_rect b;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_bar_create(tree,
        schultz_tree_root(tree), &bar));
    schultz_node_set_bounds(tree, bar, schultz_rect_make(0, 0, 300, 28));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_bar_add(tree, bar, "File", &file));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_bar_add(tree, bar, "Edit", &edit));
    schultz_menu_add(tree, file, "New", NULL, &row);
    schultz_menu_add(tree, edit, "Undo", NULL, &row);

    /* Clicking the first title opens it. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, bar, 0, &title));
    click_node(events, tree, title);
    ASSERT_EQ(1, schultz_menu_is_open(tree, file));
    ASSERT_EQ(0, schultz_menu_is_open(tree, edit));

    /* Moving onto the second swaps them, with no click at all. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, bar, 1, &title));
    schultz_node_absolute_bounds(tree, title, &b);
    schultz_events_mouse_move(events,
        schultz_point_make(b.x + b.width * 0.5f, b.y + b.height * 0.5f), 0);
    ASSERT_EQ(0, schultz_menu_is_open(tree, file));
    ASSERT_EQ(1, schultz_menu_is_open(tree, edit));

    /* And closing the bar closes whichever is showing. */
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_bar_close(tree, bar));
    ASSERT_EQ(0, schultz_menu_is_open(tree, edit));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------------------------ dialogs */

/* Finds the button carrying a role inside a message dialog's button bar. */
static schultz_handle dialog_button(schultz_tree *tree, schultz_handle box,
                                    uint32_t role)
{
    schultz_handle content = schultz_dialog_content(tree, box);
    uint32_t i;

    for (i = 0; i < schultz_node_child_count(tree, content); i++) {
        schultz_handle bar = SCHULTZ_HANDLE_NONE;
        uint32_t j;

        if (schultz_node_child_at(tree, content, i, &bar) != SCHULTZ_OK) {
            continue;
        }
        for (j = 0; j < schultz_node_child_count(tree, bar); j++) {
            schultz_handle child = SCHULTZ_HANDLE_NONE;

            if (schultz_node_child_at(tree, bar, j, &child) == SCHULTZ_OK &&
                schultz_button_bar_role(tree, child) == role) {
                return child;
            }
        }
    }
    return SCHULTZ_HANDLE_NONE;
}

TEST a_message_dialog_reports_which_button_answered_it(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle box = SCHULTZ_HANDLE_NONE;
    schultz_handle button = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 600, 400));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 600, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_message_dialog_create(tree, "Quit",
        SCHULTZ_DIALOG_ICON_QUESTION, SCHULTZ_DIALOG_YES_NO, &box));
    ASSERT_EQ(SCHULTZ_OK, schultz_message_dialog_set_text(tree, box,
        "Close without saving?", "Your changes will be lost."));

    /* Unanswered until it is answered. */
    ASSERT_EQ(SCHULTZ_BUTTON_ROLE_COUNT,
              schultz_message_dialog_result(tree, box));
    ASSERT_EQ(SCHULTZ_DIALOG_ICON_QUESTION,
              schultz_message_dialog_icon(tree, box));
    ASSERT_EQ(SCHULTZ_OK, schultz_message_dialog_open(tree, box));
    ASSERT_EQ(1, schultz_dialog_is_open(tree, box));

    button = dialog_button(tree, box, SCHULTZ_BUTTON_ROLE_NO);
    ASSERT(button != SCHULTZ_HANDLE_NONE);

    click_node(events, tree, button);
    ASSERT_EQ(SCHULTZ_BUTTON_ROLE_NO,
              schultz_message_dialog_result(tree, box));
    /* Answering closes it. */
    ASSERT_EQ(0, schultz_dialog_is_open(tree, box));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

TEST escape_answers_a_message_dialog_with_cancel(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle box = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 600, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_message_dialog_create(tree, "Save",
        SCHULTZ_DIALOG_ICON_WARNING, SCHULTZ_DIALOG_OK_CANCEL, &box));
    schultz_message_dialog_open(tree, box);
    /* Focus goes inside the dialog, which is what makes Enter and Escape
     * reach it: keys rise from the focused node through the dialog. */
    schultz_events_set_focus(events,
        dialog_button(tree, box, SCHULTZ_BUTTON_ROLE_OK));

    schultz_events_key(events, SCHULTZ_KEY_ESCAPE, 0u, 1);
    ASSERT_EQ(SCHULTZ_BUTTON_ROLE_CANCEL,
              schultz_message_dialog_result(tree, box));
    ASSERT_EQ(0, schultz_dialog_is_open(tree, box));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

/* An empty answer is not an answer, unless the host says it is. */
TEST a_text_input_dialog_refuses_an_empty_answer(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle box = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 600, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_input_dialog_create(tree, "Rename",
                                                           &box));
    schultz_message_dialog_open(tree, box);
    /* Focus goes inside the dialog, which is what makes Enter and Escape
     * reach it: keys rise from the focused node through the dialog. */
    schultz_events_set_focus(events,
        dialog_button(tree, box, SCHULTZ_BUTTON_ROLE_OK));

    schultz_events_key(events, SCHULTZ_KEY_RETURN, 0u, 1);
    ASSERT_EQ(SCHULTZ_BUTTON_ROLE_COUNT,
              schultz_message_dialog_result(tree, box));
    ASSERT_EQ(1, schultz_dialog_is_open(tree, box));

    ASSERT_EQ(SCHULTZ_OK, schultz_text_input_dialog_set_value(tree, box,
                                                              "notes.txt"));
    schultz_events_key(events, SCHULTZ_KEY_RETURN, 0u, 1);
    ASSERT_EQ(SCHULTZ_BUTTON_ROLE_OK,
              schultz_message_dialog_result(tree, box));
    ASSERT_STR_EQ("notes.txt", schultz_text_input_dialog_value(tree, box));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

TEST a_text_input_dialog_can_be_told_empty_is_fine(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle box = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 600, 400));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_text_input_dialog_create(tree, "Note",
                                                           &box));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_text_input_dialog_set_allow_empty(tree, box, 1));
    schultz_message_dialog_open(tree, box);
    /* Focus goes inside the dialog, which is what makes Enter and Escape
     * reach it: keys rise from the focused node through the dialog. */
    schultz_events_set_focus(events,
        dialog_button(tree, box, SCHULTZ_BUTTON_ROLE_OK));

    schultz_events_key(events, SCHULTZ_KEY_RETURN, 0u, 1);
    ASSERT_EQ(SCHULTZ_BUTTON_ROLE_OK,
              schultz_message_dialog_result(tree, box));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}

TEST a_choice_dialog_carries_a_list_of_choices(void)
{
    schultz_tree *tree;
    schultz_handle box = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_choice_dialog_create(tree, "Units", &box));
    ASSERT_EQ(SCHULTZ_OK, schultz_choice_dialog_add(tree, box, "Metric"));
    ASSERT_EQ(SCHULTZ_OK, schultz_choice_dialog_add(tree, box, "Imperial"));
    ASSERT_EQ(SCHULTZ_OK, schultz_choice_dialog_select(tree, box, 1u));
    ASSERT_EQ(1u, schultz_choice_dialog_selected(tree, box));

    /* The choice calls refuse a dialog that holds no choices. */
    {
        schultz_handle plain = SCHULTZ_HANDLE_NONE;

        ASSERT_EQ(SCHULTZ_OK, schultz_message_dialog_create(tree, "Note",
            SCHULTZ_DIALOG_ICON_INFO, SCHULTZ_DIALOG_OK, &plain));
        ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
                  schultz_choice_dialog_add(tree, plain, "No"));
    }

    schultz_tree_destroy(tree);
    PASS();
}

/* --------------------------------------------------- busy and toast */

TEST a_busy_indicator_only_asks_to_be_ticked_while_it_runs(void)
{
    schultz_tree *tree;
    schultz_handle busy = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 200, 200));
    ASSERT_EQ(SCHULTZ_OK, schultz_busy_indicator_create(tree,
        schultz_tree_root(tree), &busy));

    ASSERT_EQ(0, schultz_busy_indicator_is_running(tree, busy));
    schultz_tree_advance(tree, 0u);
    /* Nothing moves while it is stopped. */
    ASSERT_EQ(0u, schultz_tree_advance(tree, 100u));

    ASSERT_EQ(SCHULTZ_OK, schultz_busy_indicator_start(tree, busy));
    ASSERT_EQ(1, schultz_busy_indicator_is_running(tree, busy));
    ASSERT(schultz_tree_advance(tree, 200u) > 0u);

    ASSERT_EQ(SCHULTZ_OK, schultz_busy_indicator_stop(tree, busy));
    ASSERT_EQ(0u, schultz_tree_advance(tree, 300u));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_toast_takes_itself_away_and_is_used_again(void)
{
    schultz_tree *tree;
    schultz_handle first = SCHULTZ_HANDLE_NONE;
    schultz_handle second = SCHULTZ_HANDLE_NONE;
    uint32_t before;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 300));

    ASSERT_EQ(SCHULTZ_OK, schultz_toast_show(tree, "Saved", 500u, &first));
    ASSERT_EQ(1, schultz_toast_is_showing(tree, first));
    before = schultz_tree_node_count(tree);

    schultz_tree_advance(tree, 0u);
    schultz_tree_advance(tree, 200u);
    ASSERT_EQ(1, schultz_toast_is_showing(tree, first));

    schultz_tree_advance(tree, 900u);
    ASSERT_EQ(0, schultz_toast_is_showing(tree, first));

    /* The next one reuses the node rather than building another. */
    ASSERT_EQ(SCHULTZ_OK, schultz_toast_show(tree, "Copied", 500u, &second));
    ASSERT_EQ(first, second);
    ASSERT_EQ(before, schultz_tree_node_count(tree));

    schultz_tree_destroy(tree);
    PASS();
}

TEST toasts_showing_at_once_stack_rather_than_overlap(void)
{
    schultz_tree *tree;
    schultz_handle one = SCHULTZ_HANDLE_NONE;
    schultz_handle two = SCHULTZ_HANDLE_NONE;
    schultz_rect a;
    schultz_rect b;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 300));

    ASSERT_EQ(SCHULTZ_OK, schultz_toast_show(tree, "One", 500u, &one));
    ASSERT_EQ(SCHULTZ_OK, schultz_toast_show(tree, "Two", 500u, &two));
    ASSERT(one != two);

    schultz_node_get_bounds(tree, one, &a);
    schultz_node_get_bounds(tree, two, &b);
    /* Along the bottom by default, so the second sits above the first. */
    ASSERT(b.y < a.y);

    ASSERT_EQ(SCHULTZ_OK, schultz_toast_dismiss(tree, one));
    ASSERT_EQ(0, schultz_toast_is_showing(tree, one));
    ASSERT_EQ(1, schultz_toast_is_showing(tree, two));

    schultz_tree_destroy(tree);
    PASS();
}

TEST a_pane_on_the_root_leaves_a_toast_where_it_is(void)
{
    overlay_fixture f;
    schultz_handle root;
    schultz_handle panel = SCHULTZ_HANDLE_NONE;
    schultz_handle toast = SCHULTZ_HANDLE_NONE;
    schultz_rect placed;
    schultz_rect after;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    root = schultz_tree_root(f.tree);
    schultz_node_set_bounds(f.tree, root, schultz_rect_make(0, 0, 400, 300));

    /*
     * A host is allowed to give its root a pane. Nothing says otherwise, and
     * a binding that maps a window's children onto the root has to.
     */
    schultz_node_set_pane(f.tree, root, schultz_pane_vbox());
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree, root, &panel));

    ASSERT_EQ(SCHULTZ_OK, schultz_toast_show(f.tree, "Saved", 500u, &toast));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(f.tree, toast, &placed));
    ASSERT(placed.height > 0.0f);

    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);

    /* The column arranged the panel and left the toast alone. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(f.tree, toast, &after));
    ASSERT_EQ(1, schultz_rect_equals(placed, after));
    /* Still against the bottom edge rather than at the top of a column. */
    ASSERT(after.y > 200.0f);
    ASSERT(after.width < 400.0f);

    fixture_teardown(&f);
    PASS();
}

TEST a_toast_follows_a_window_that_changes_size(void)
{
    overlay_fixture f;
    schultz_handle toast = SCHULTZ_HANDLE_NONE;
    schultz_rect before;
    schultz_rect after;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_toast_show(f.tree, "Saved", 5000u, &toast));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(f.tree, toast, &before));
    /* Along the bottom of a 300 tall window. */
    ASSERT(before.y > 200.0f);

    /* A rotation on a phone is a resize, and it dismisses nothing. */
    schultz_tree_advance(f.tree, 1000u);
    schultz_tree_set_viewport(f.tree, schultz_rect_make(0, 0, 400, 200));
    schultz_tree_advance(f.tree, 1016u);

    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(f.tree, toast, &after));
    /* Moved up with the edge, and still inside the window. */
    ASSERT(after.y < before.y);
    ASSERT(after.y + after.height <= 200.0f);
    ASSERT_EQ(1, schultz_toast_is_showing(f.tree, toast));

    fixture_teardown(&f);
    PASS();
}

TEST a_node_that_places_itself_is_left_alone_by_its_pane(void)
{
    overlay_fixture f;
    schultz_handle root;
    schultz_handle column = SCHULTZ_HANDLE_NONE;
    schultz_handle ordinary = SCHULTZ_HANDLE_NONE;
    schultz_handle loose = SCHULTZ_HANDLE_NONE;
    schultz_rect put;
    schultz_rect after;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    root = schultz_tree_root(f.tree);
    schultz_node_set_bounds(f.tree, root, schultz_rect_make(0, 0, 400, 300));
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree, root, &column));
    schultz_node_set_pane(f.tree, column, schultz_pane_vbox());
    schultz_node_set_bounds(f.tree, column, schultz_rect_make(0, 0, 400, 300));
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree, column, &ordinary));
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree, column, &loose));

    /* Put by hand where no column would put it. */
    put = schultz_rect_make(310, 250, 80, 40);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_places_itself(f.tree, loose, 1));
    ASSERT_EQ(1, schultz_node_places_itself(f.tree, loose));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_bounds(f.tree, loose, put));

    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(f.tree, loose, &after));
    ASSERT_EQ(1, schultz_rect_equals(put, after));

    /* Handing it back to the column moves it into the flow again. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_set_places_itself(f.tree, loose, 0));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_get_bounds(f.tree, loose, &after));
    ASSERT_EQ(0, schultz_rect_equals(put, after));

    fixture_teardown(&f);
    PASS();
}

/*
 * A phone: the window is one rectangle and the root is given a smaller one,
 * because a camera notch has taken the top of it. On a desktop the two are
 * the same and none of the tests below can fail.
 */
static void inset_root(overlay_fixture *f, float inset)
{
    schultz_tree_set_viewport(f->tree, schultz_rect_make(0, 0, 400, 800));
    schultz_node_set_bounds(f->tree, schultz_tree_root(f->tree),
                            schultz_rect_make(0, inset, 400, 800 - inset));
}

TEST an_overlay_lands_where_it_belongs_under_a_notch(void)
{
    overlay_fixture f;
    schultz_handle shell = SCHULTZ_HANDLE_NONE;
    schultz_handle anchor = SCHULTZ_HANDLE_NONE;
    schultz_handle menu = SCHULTZ_HANDLE_NONE;
    schultz_rect on;
    schultz_rect at;

    /*
     * Placement is worked out in window coordinates and handed to a setter
     * that takes the parent's. Those agree only while the root sits at the
     * origin, which is every desktop and no phone: with the root inset for a
     * notch, every overlay was placed that much too low.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    inset_root(&f, 60.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree,
                              schultz_tree_root(f.tree), &shell));
    schultz_node_set_bounds(f.tree, shell, schultz_rect_make(0, 0, 400, 740));
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree, shell, &anchor));
    schultz_node_set_bounds(f.tree, anchor,
                            schultz_rect_make(40, 200, 120, 40));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, anchor, &on));
    ASSERT_EQ(260.0f, on.y);   /* 200 inside the root, 60 down the window */

    ASSERT_EQ(SCHULTZ_OK, schultz_menu_create(f.tree, &menu));
    {
        schultz_handle row = SCHULTZ_HANDLE_NONE;

        ASSERT_EQ(SCHULTZ_OK, schultz_menu_add(f.tree, menu, "One", NULL,
                                               &row));
    }
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_open(f.tree, menu, on,
                                            SCHULTZ_PLACE_BELOW));

    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, menu, &at));
    ASSERT_EQ(on.y + on.height, at.y);
    ASSERT_EQ(on.x, at.x);

    fixture_teardown(&f);
    PASS();
}

TEST a_toast_stays_inside_the_safe_area(void)
{
    overlay_fixture f;
    schultz_handle toast = SCHULTZ_HANDLE_NONE;
    schultz_rect safe;
    schultz_rect at;

    /*
     * The worst of the four. A toast anchors near the bottom of the window,
     * and the shift then carried it past the end of the root's box, which is
     * to say off the bottom of the screen.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    inset_root(&f, 60.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_toast_show(f.tree, "Saved", 3000u, &toast));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_safe_area(f.tree, &safe));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, toast, &at));

    ASSERT(at.y >= safe.y);
    ASSERT(at.y + at.height <= safe.y + safe.height);
    /* Along the bottom of the safe area, not the top of it. */
    ASSERT(at.y > safe.y + safe.height * 0.5f);

    fixture_teardown(&f);
    PASS();
}

TEST a_toast_still_counts_down_under_a_notch(void)
{
    overlay_fixture f;
    schultz_handle toast = SCHULTZ_HANDLE_NONE;
    uint64_t now = 1000u;
    int32_t i;

    /*
     * Placing a toast and deciding whether to place it again used to work the
     * rectangle out separately, and one of the two was moved to the safe area
     * while the other went on reading the whole window. On a phone they never
     * matched, so every tick took the branch that re-places and returned
     * before the countdown, and a toast asked for half a second stayed up for
     * good. On a desktop the two rectangles are identical and it counted down
     * perfectly, which is why every test here passed.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    inset_root(&f, 60.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_toast_show(f.tree, "Saved", 500u, &toast));
    ASSERT(schultz_toast_is_showing(f.tree, toast));

    /* The first call only sets a starting point, then three seconds of
     * frames against a toast that asked for half of one. */
    schultz_tree_advance(f.tree, now);
    for (i = 0; i < 180; i++) {
        now += 16u;
        schultz_tree_advance(f.tree, now);
    }
    ASSERT_FALSE(schultz_toast_is_showing(f.tree, toast));

    fixture_teardown(&f);
    PASS();
}

TEST a_dialog_covers_the_notch_as_well(void)
{
    overlay_fixture f;
    schultz_handle dialog = SCHULTZ_HANDLE_NONE;
    schultz_rect window;
    schultz_rect at;

    /*
     * The one that wants the whole window rather than the safe area: a scrim
     * that stopped at the notch would leave a strip of the application
     * showing above a modal dialog.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    inset_root(&f, 60.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_dialog_create(f.tree, "Title", &dialog));
    ASSERT_EQ(SCHULTZ_OK, schultz_dialog_open(f.tree, dialog));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_get_viewport(f.tree, &window));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, dialog, &at));
    ASSERT_EQ(1, schultz_rect_equals(window, at));

    fixture_teardown(&f);
    PASS();
}

TEST an_overlay_is_kept_clear_of_the_notch(void)
{
    overlay_fixture f;
    schultz_handle shell = SCHULTZ_HANDLE_NONE;
    schultz_handle anchor = SCHULTZ_HANDLE_NONE;
    schultz_handle menu = SCHULTZ_HANDLE_NONE;
    schultz_rect on;
    schultz_rect safe;
    schultz_rect at;

    /*
     * The quieter half: an overlay nudged back inside the window can still
     * come to rest under a notch, where it is on screen and cannot be read.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    inset_root(&f, 60.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree,
                              schultz_tree_root(f.tree), &shell));
    schultz_node_set_bounds(f.tree, shell, schultz_rect_make(0, 0, 400, 740));
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree, shell, &anchor));
    /* Right at the top of the safe area, so above it is the notch. */
    schultz_node_set_bounds(f.tree, anchor, schultz_rect_make(40, 0, 120, 20));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, anchor, &on));

    ASSERT_EQ(SCHULTZ_OK, schultz_menu_create(f.tree, &menu));
    {
        schultz_handle row = SCHULTZ_HANDLE_NONE;

        ASSERT_EQ(SCHULTZ_OK, schultz_menu_add(f.tree, menu, "One", NULL,
                                               &row));
    }
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_open(f.tree, menu, on,
                                            SCHULTZ_PLACE_ABOVE));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_safe_area(f.tree, &safe));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, menu, &at));
    ASSERT(at.y >= safe.y);

    fixture_teardown(&f);
    PASS();
}

TEST a_popover_sits_above_what_it_belongs_to(void)
{
    overlay_fixture f;
    schultz_handle anchor = SCHULTZ_HANDLE_NONE;
    schultz_handle pop = SCHULTZ_HANDLE_NONE;
    schultz_handle content = SCHULTZ_HANDLE_NONE;
    schultz_rect at;
    schultz_rect on;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &anchor);
    schultz_node_set_bounds(f.tree, anchor, schultz_rect_make(150, 150, 100,
                                                             30));
    ASSERT_EQ(SCHULTZ_OK, schultz_popover_create(f.tree, 1, &pop));
    content = schultz_popup_content(f.tree, pop);
    schultz_node_set_pref_size(f.tree, content, 60.0f, 40.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_popup_open(f.tree, pop, anchor));
    ASSERT_EQ((uint32_t)SCHULTZ_PLACE_ABOVE,
              schultz_node_anchor_placement(f.tree, pop));

    schultz_node_absolute_bounds(f.tree, anchor, &on);
    schultz_node_absolute_bounds(f.tree, pop, &at);

    /* Clear of it by the gap, and centred on it. */
    ASSERT_EQ(on.y - 4.0f, at.y + at.height);
    ASSERT_EQ(on.x + on.width * 0.5f, at.x + at.width * 0.5f);

    fixture_teardown(&f);
    PASS();
}

TEST a_popover_with_no_room_above_goes_below(void)
{
    overlay_fixture f;
    schultz_handle anchor = SCHULTZ_HANDLE_NONE;
    schultz_handle pop = SCHULTZ_HANDLE_NONE;
    schultz_rect at;
    schultz_rect on;

    /*
     * Flipped rather than nudged. Nudged, it would come to rest on top of the
     * thing it is pointing at, which is the one place it must not be.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &anchor);
    schultz_node_set_bounds(f.tree, anchor, schultz_rect_make(150, 2, 100,
                                                             30));
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_create(f.tree, 1, &pop));
    schultz_node_set_pref_size(f.tree,
        schultz_popup_content(f.tree, pop), 60.0f, 40.0f);

    ASSERT_EQ(SCHULTZ_OK, schultz_popup_open(f.tree, pop, anchor));
    ASSERT_EQ((uint32_t)SCHULTZ_PLACE_BELOW,
              schultz_node_anchor_placement(f.tree, pop));

    schultz_node_absolute_bounds(f.tree, anchor, &on);
    schultz_node_absolute_bounds(f.tree, pop, &at);
    ASSERT_EQ(on.y + on.height + 4.0f, at.y);

    fixture_teardown(&f);
    PASS();
}

TEST a_tooltip_lines_up_with_the_pointer(void)
{
    overlay_fixture f;
    schultz_handle anchor = SCHULTZ_HANDLE_NONE;
    schultz_handle tip = SCHULTZ_HANDLE_NONE;
    schultz_rect at;
    schultz_rect on;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Hover", &anchor);
    schultz_node_set_bounds(f.tree, anchor, schultz_rect_make(100, 100, 120,
                                                              30));
    ASSERT_EQ(SCHULTZ_OK, schultz_tooltip_create(f.tree, "Explains", &tip));

    /* The pointer well to the right of the widget's left corner. */
    schultz_events_mouse_move(f.events, schultz_point_make(190, 110), 0);
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_open(f.tree, tip, anchor));

    schultz_node_absolute_bounds(f.tree, anchor, &on);
    schultz_node_absolute_bounds(f.tree, tip, &at);
    ASSERT_EQ(190.0f, at.x);
    ASSERT(at.x > on.x);
    /* Below the widget, clear of the pointer resting on it. */
    ASSERT_EQ((uint32_t)SCHULTZ_PLACE_BELOW,
              schultz_node_anchor_placement(f.tree, tip));
    ASSERT_EQ(on.y + on.height + 4.0f, at.y);

    fixture_teardown(&f);
    PASS();
}

TEST a_tooltip_with_no_pointer_is_centred_on_its_widget(void)
{
    overlay_fixture f;
    schultz_handle anchor = SCHULTZ_HANDLE_NONE;
    schultz_handle tip = SCHULTZ_HANDLE_NONE;
    schultz_rect at;
    schultz_rect on;

    /*
     * A touch screen never moves a pointer, so there is none to line up with.
     * The far left corner of the widget is the one answer that is never
     * right, so it is centred instead.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_button_create(f.tree, schultz_tree_root(f.tree), "Hold", &anchor);
    schultz_node_set_bounds(f.tree, anchor, schultz_rect_make(100, 100, 120,
                                                              30));
    ASSERT_EQ(SCHULTZ_OK, schultz_tooltip_create(f.tree, "Explains", &tip));
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_open(f.tree, tip, anchor));

    schultz_node_absolute_bounds(f.tree, anchor, &on);
    schultz_node_absolute_bounds(f.tree, tip, &at);
    ASSERT_EQ(on.x + on.width * 0.5f, at.x + at.width * 0.5f);

    fixture_teardown(&f);
    PASS();
}

TEST a_panel_with_no_triangle_lines_up_like_a_dropdown(void)
{
    overlay_fixture f;
    schultz_handle anchor = SCHULTZ_HANDLE_NONE;
    schultz_handle panel = SCHULTZ_HANDLE_NONE;
    schultz_handle bubble = SCHULTZ_HANDLE_NONE;
    schultz_rect at;
    schultz_rect on;

    /*
     * Centring belongs to the bubble, not to every popover. A triangle has to
     * come out of the middle of the thing it points at; a panel dropped under
     * a control is a dropdown, and a dropdown lines up with the control's
     * leading edge. The date, time and colour pickers all open one of these.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &anchor);
    schultz_node_set_bounds(f.tree, anchor, schultz_rect_make(120, 120, 80,
                                                             30));

    /* A popup, which is the plain floating panel the pickers open. It has
     * no triangle to begin with; a popover is the one that does. */
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_create(f.tree, 1, &panel));
    schultz_node_set_pref_size(f.tree,
        schultz_popup_content(f.tree, panel), 160.0f, 40.0f);
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_open_at(f.tree, panel, anchor,
                                                  SCHULTZ_PLACE_BELOW));

    schultz_node_absolute_bounds(f.tree, anchor, &on);
    schultz_node_absolute_bounds(f.tree, panel, &at);
    /* Wider than what opened it, and still starting where it starts. */
    ASSERT(at.width > on.width);
    ASSERT_EQ(on.x, at.x);

    /* The same popover with its triangle back on centres again. */
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_set_arrow(f.tree, panel, 1));
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_open_at(f.tree, panel, anchor,
                                                  SCHULTZ_PLACE_BELOW));
    schultz_node_absolute_bounds(f.tree, panel, &at);
    ASSERT_EQ(on.x + on.width * 0.5f, at.x + at.width * 0.5f);

    /* And a bubble is centred whichever side it lands on. */
    ASSERT_EQ(SCHULTZ_OK, schultz_popover_create(f.tree, 1, &bubble));
    schultz_node_set_pref_size(f.tree,
        schultz_popup_content(f.tree, bubble), 40.0f, 20.0f);
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_open(f.tree, bubble, anchor));
    schultz_node_absolute_bounds(f.tree, bubble, &at);
    ASSERT_EQ(on.x + on.width * 0.5f, at.x + at.width * 0.5f);

    fixture_teardown(&f);
    PASS();
}

TEST a_popovers_triangle_points_at_what_opened_it(void)
{
    overlay_fixture f;
    schultz_handle anchor = SCHULTZ_HANDLE_NONE;
    schultz_handle pop = SCHULTZ_HANDLE_NONE;
    schultz_rect at;
    schultz_rect on;
    const schultz_draw_cmd *arrow = NULL;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &anchor);
    schultz_node_set_bounds(f.tree, anchor, schultz_rect_make(150, 150, 100,
                                                             30));
    ASSERT_EQ(SCHULTZ_OK, schultz_popover_create(f.tree, 1, &pop));
    schultz_node_set_pref_size(f.tree,
        schultz_popup_content(f.tree, pop), 60.0f, 40.0f);
    ASSERT_EQ(SCHULTZ_OK, schultz_popup_open(f.tree, pop, anchor));

    paint_all(&f);
    for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f.list, i);

        if (cmd->kind == SCHULTZ_DRAW_FILL_POLYGON) {
            arrow = cmd;
        }
    }
    ASSERT(arrow != NULL);
    ASSERT_EQ(3u, arrow->as.fill_polygon.count);

    schultz_node_absolute_bounds(f.tree, anchor, &on);
    schultz_node_absolute_bounds(f.tree, pop, &at);

    /* The middle point is the tip: aimed at the anchor and reaching the
     * bottom of the popover's own bounds, where the gap begins. */
    ASSERT_EQ(on.x + on.width * 0.5f, arrow->as.fill_polygon.points[1].x);
    ASSERT_EQ(at.y + at.height, arrow->as.fill_polygon.points[1].y);
    /* The two corners sit either side of it, inside the box. */
    ASSERT(arrow->as.fill_polygon.points[0].x <
           arrow->as.fill_polygon.points[1].x);
    ASSERT(arrow->as.fill_polygon.points[2].x >
           arrow->as.fill_polygon.points[1].x);
    ASSERT(arrow->as.fill_polygon.points[0].y <
           arrow->as.fill_polygon.points[1].y);

    /* A tooltip has no triangle: it appears at the pointer, which is the
     * arrow already. */
    fixture_teardown(&f);
    PASS();
}

TEST a_menu_row_hands_back_its_caption(void)
{
    overlay_fixture f;
    schultz_handle menu = SCHULTZ_HANDLE_NONE;
    schultz_handle plain = SCHULTZ_HANDLE_NONE;
    schultz_handle check = SCHULTZ_HANDLE_NONE;
    schultz_handle rule = SCHULTZ_HANDLE_NONE;
    schultz_handle panel = SCHULTZ_HANDLE_NONE;

    /*
     * The last of the family. A button, a toggle button and a menu button
     * answer schultz_button_label; a menu row is not a button, so it has its
     * own way to be asked the same thing.
     */
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_create(f.tree, &menu));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add(f.tree, menu, "Open", NULL,
                                           &plain));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add_check(f.tree, menu, "Wrap", NULL,
                                                 0, &check));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_add_separator(f.tree, menu, &rule));

    ASSERT_STR_EQ("Open", schultz_label_text(f.tree,
                      schultz_menu_item_label(f.tree, plain)));
    ASSERT_STR_EQ("Wrap", schultz_label_text(f.tree,
                      schultz_menu_item_label(f.tree, check)));

    /* And it is what a caption is changed through, which is the point. */
    ASSERT_EQ(SCHULTZ_OK, schultz_label_set_text(f.tree,
                  schultz_menu_item_label(f.tree, plain), "Open recent"));
    ASSERT_STR_EQ("Open recent", schultz_label_text(f.tree,
                      schultz_menu_item_label(f.tree, plain)));

    /* A separator has one too; it just says nothing. */
    ASSERT(schultz_menu_item_label(f.tree, rule) != SCHULTZ_HANDLE_NONE);

    /* Anything that is not a row answers nothing. */
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree,
                  schultz_tree_root(f.tree), &panel));
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_menu_item_label(f.tree, panel));
    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_menu_item_label(f.tree, menu));

    fixture_teardown(&f);
    PASS();
}

/* ---------------------------------------------------------- pagination */

TEST pagination_steps_and_clamps(void)
{
    schultz_tree *tree;
    schultz_handle pages = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 600, 200));
    ASSERT_EQ(SCHULTZ_OK, schultz_pagination_create(tree,
        schultz_tree_root(tree), 20u, &pages));

    ASSERT_EQ(20u, schultz_pagination_page_count(tree, pages));
    ASSERT_EQ(0u, schultz_pagination_current(tree, pages));

    schultz_pagination_set_current(tree, pages, 5u);
    ASSERT_EQ(5u, schultz_pagination_current(tree, pages));

    /* Past the end lands on the last page. */
    schultz_pagination_set_current(tree, pages, 99u);
    ASSERT_EQ(19u, schultz_pagination_current(tree, pages));

    /* Fewer pages than the current one pulls it back. */
    schultz_pagination_set_page_count(tree, pages, 3u);
    ASSERT_EQ(2u, schultz_pagination_current(tree, pages));

    /* Zero pages is one page. */
    schultz_pagination_set_page_count(tree, pages, 0u);
    ASSERT_EQ(1u, schultz_pagination_page_count(tree, pages));
    ASSERT_EQ(0u, schultz_pagination_current(tree, pages));

    schultz_tree_destroy(tree);
    PASS();
}

/* The far ends always name the first and last page, however long the run. */
TEST pagination_keeps_the_ends_reachable(void)
{
    schultz_tree *tree;
    schultz_handle pages = SCHULTZ_HANDLE_NONE;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    uint32_t last;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 600, 200));
    ASSERT_EQ(SCHULTZ_OK, schultz_pagination_create(tree,
        schultz_tree_root(tree), 50u, &pages));
    schultz_pagination_set_current(tree, pages, 25u);

    /* Child 4 is the first number button; the last is the one before the
     * end of the child list. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, pages, 4u, &button));
    ASSERT_STR_EQ("1", schultz_node_get_name(tree, button));

    last = schultz_node_child_count(tree, pages) - 1u;
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, pages, last, &button));
    ASSERT_STR_EQ("50", schultz_node_get_name(tree, button));

    schultz_tree_destroy(tree);
    PASS();
}

/* A widget that reports a change from its tick must be repainted for it. */
TEST a_tick_that_reports_a_change_marks_the_node(void)
{
    schultz_tree *tree;
    schultz_handle busy = SCHULTZ_HANDLE_NONE;
    schultz_rect dirty;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 200, 200));
    ASSERT_EQ(SCHULTZ_OK, schultz_busy_indicator_create(tree,
        schultz_tree_root(tree), &busy));
    schultz_node_set_bounds(tree, busy, schultz_rect_make(10, 10, 24, 24));
    schultz_busy_indicator_start(tree, busy);

    schultz_tree_advance(tree, 0u);
    schultz_tree_clear_dirty(tree);

    /* One frame's worth of turning, and the region it covers must be stale. */
    ASSERT(schultz_tree_advance(tree, 16u) > 0u);
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &dirty));
    ASSERT(!schultz_rect_is_empty(dirty));

    /* Stopped, it costs nothing and marks nothing. */
    schultz_busy_indicator_stop(tree, busy);
    schultz_tree_clear_dirty(tree);
    ASSERT_EQ(0u, schultz_tree_advance(tree, 32u));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &dirty));
    ASSERT(schultz_rect_is_empty(dirty));

    schultz_tree_destroy(tree);
    PASS();
}

/* Moving along a menu bar follows an open menu; it does not start one. */
TEST a_menu_bar_opens_on_a_click_not_on_a_hover(void)
{
    schultz_tree *tree;
    schultz_events *events = NULL;
    schultz_handle bar = SCHULTZ_HANDLE_NONE;
    schultz_handle file = SCHULTZ_HANDLE_NONE;
    schultz_handle edit = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_handle first;
    schultz_handle second;
    schultz_rect b;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 300));
    schultz_node_set_bounds(tree, schultz_tree_root(tree),
                            schultz_rect_make(0, 0, 400, 300));
    ASSERT_EQ(SCHULTZ_OK, schultz_events_create(tree, &events));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_bar_create(tree,
        schultz_tree_root(tree), &bar));
    schultz_node_set_bounds(tree, bar, schultz_rect_make(0, 0, 300, 28));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_bar_add(tree, bar, "File", &file));
    ASSERT_EQ(SCHULTZ_OK, schultz_menu_bar_add(tree, bar, "Edit", &edit));
    schultz_menu_add(tree, file, "New", NULL, &row);
    schultz_menu_add(tree, edit, "Undo", NULL, &row);
    schultz_tree_resolve_styles(tree);
    schultz_layout_run(tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, bar, 0, &first));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(tree, bar, 1, &second));

    /* Passing over a quiet bar opens nothing. */
    schultz_node_absolute_bounds(tree, first, &b);
    schultz_events_mouse_move(events,
        schultz_point_make(b.x + b.width * 0.5f, b.y + b.height * 0.5f), 0);
    ASSERT_EQ(0, schultz_menu_is_open(tree, file));

    /* A click opens it, and then moving along follows. */
    click_node(events, tree, first);
    ASSERT_EQ(1, schultz_menu_is_open(tree, file));
    schultz_node_absolute_bounds(tree, second, &b);
    schultz_events_mouse_move(events,
        schultz_point_make(b.x + b.width * 0.5f, b.y + b.height * 0.5f), 0);
    ASSERT_EQ(1, schultz_menu_is_open(tree, edit));

    /* Once everything is closed, passing over it opens nothing again. This
     * is what a remembered flag got wrong: a menu closes without the bar
     * being told. */
    schultz_menu_bar_close(tree, bar);
    schultz_node_absolute_bounds(tree, first, &b);
    schultz_events_mouse_move(events, schultz_point_make(0.0f, 200.0f), 0);
    schultz_events_mouse_move(events,
        schultz_point_make(b.x + b.width * 0.5f, b.y + b.height * 0.5f), 0);
    ASSERT_EQ(0, schultz_menu_is_open(tree, file));

    schultz_events_destroy(events);
    schultz_tree_destroy(tree);
    PASS();
}


TEST a_dialog_title_bar_is_tall_enough_to_take_a_touch(void)
{
    overlay_fixture f;
    schultz_handle dialog = SCHULTZ_HANDLE_NONE;
    schultz_handle panel = SCHULTZ_HANDLE_NONE;
    schultz_handle bar = SCHULTZ_HANDLE_NONE;
    schultz_handle title = SCHULTZ_HANDLE_NONE;
    schultz_rect bar_at;
    schultz_rect title_at;
    float centre;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    /*
     * The title draws with the theme's title face, and an unfilled font slot
     * resolves to nothing at all, so the title would measure to zero and the
     * centring below would pass without meaning anything. A host fills this
     * slot; the fixture only fills the body one.
     *
     * The tree copies the theme it is given, so it has to be handed the
     * changed one again.
     */
    schultz_theme_set_font(&f.theme, SCHULTZ_TOKEN_FONT_TITLE, f.font);
    schultz_tree_set_theme(f.tree, &f.theme);

    ASSERT_EQ(SCHULTZ_OK, schultz_dialog_create(f.tree, "Unsaved changes",
                                                &dialog));
    ASSERT_EQ(SCHULTZ_OK, schultz_dialog_open(f.tree, dialog));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_run(f.tree);

    /* The dialog is a panel holding a title bar above its content. */
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, dialog, 0u, &panel));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, panel, 0u, &bar));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_child_at(f.tree, bar, 0u, &title));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, bar, &bar_at));
    ASSERT_EQ(SCHULTZ_OK, schultz_node_absolute_bounds(f.tree, title,
                                                       &title_at));

    /* A strip sized to its own text is too thin to put a close button in. */
    ASSERT(bar_at.height >=
           schultz_theme_number(NULL, SCHULTZ_TOKEN_CONTROL_HEIGHT));

    /*
     * The title reads from the left and sits centred down the bar. A stack
     * pane aligns a child the same way on both axes, so it cannot do both at
     * once and the title ends up against the top of a bar this tall.
     */
    centre = bar_at.y + bar_at.height * 0.5f;
    ASSERT(title_at.height > 0.0f);
    ASSERT(title_at.y < centre);
    ASSERT(title_at.y + title_at.height > centre);
    ASSERT(title_at.y > bar_at.y);

    fixture_teardown(&f);
    PASS();
}

SUITE(overlays)
{
    RUN_TEST(a_check_row_toggles_and_leaves_the_menu_open);
    RUN_TEST(a_tick_that_reports_a_change_marks_the_node);
    RUN_TEST(a_menu_bar_opens_on_a_click_not_on_a_hover);
    RUN_TEST(a_message_dialog_reports_which_button_answered_it);
    RUN_TEST(escape_answers_a_message_dialog_with_cancel);
    RUN_TEST(a_text_input_dialog_refuses_an_empty_answer);
    RUN_TEST(a_text_input_dialog_can_be_told_empty_is_fine);
    RUN_TEST(a_choice_dialog_carries_a_list_of_choices);
    RUN_TEST(a_busy_indicator_only_asks_to_be_ticked_while_it_runs);
    RUN_TEST(a_toast_takes_itself_away_and_is_used_again);
    RUN_TEST(toasts_showing_at_once_stack_rather_than_overlap);
    RUN_TEST(a_pane_on_the_root_leaves_a_toast_where_it_is);
    RUN_TEST(a_toast_follows_a_window_that_changes_size);
    RUN_TEST(a_node_that_places_itself_is_left_alone_by_its_pane);
    RUN_TEST(an_overlay_lands_where_it_belongs_under_a_notch);
    RUN_TEST(a_toast_stays_inside_the_safe_area);
    RUN_TEST(a_toast_still_counts_down_under_a_notch);
    RUN_TEST(a_dialog_covers_the_notch_as_well);
    RUN_TEST(an_overlay_is_kept_clear_of_the_notch);
    RUN_TEST(a_popover_sits_above_what_it_belongs_to);
    RUN_TEST(a_popover_with_no_room_above_goes_below);
    RUN_TEST(a_tooltip_lines_up_with_the_pointer);
    RUN_TEST(a_tooltip_with_no_pointer_is_centred_on_its_widget);
    RUN_TEST(a_panel_with_no_triangle_lines_up_like_a_dropdown);
    RUN_TEST(a_popovers_triangle_points_at_what_opened_it);
    RUN_TEST(a_menu_row_hands_back_its_caption);
    RUN_TEST(pagination_steps_and_clamps);
    RUN_TEST(pagination_keeps_the_ends_reachable);
    RUN_TEST(a_menu_button_opens_its_menu_instead_of_reporting);
    RUN_TEST(a_split_menu_button_separates_the_click_from_the_arrow);
    RUN_TEST(a_menu_bar_swaps_menus_as_the_pointer_moves_along_it);
    RUN_TEST(a_radio_row_clears_the_rest_of_its_set);
    RUN_TEST(a_separator_row_takes_no_focus);
    RUN_TEST(a_custom_row_holds_whatever_it_is_given);
    RUN_TEST(a_submenu_row_opens_another_menu);
    RUN_TEST(a_menu_is_hidden_until_it_is_opened);
    RUN_TEST(a_menu_opens_beside_what_it_belongs_to);
    RUN_TEST(a_menu_stays_inside_the_window);
    RUN_TEST(choosing_a_row_closes_the_menu_and_tells_the_host);
    RUN_TEST(a_press_outside_a_menu_closes_it);
    RUN_TEST(escape_closes_a_menu);
    RUN_TEST(a_menu_stays_inside_a_window_that_shrinks);
    RUN_TEST(a_menu_is_not_arranged_into_the_flow);
    RUN_TEST(menu_accessors_reject_other_widgets);
    RUN_TEST(a_popover_opens_and_closes_beside_a_node);
    RUN_TEST(a_tooltip_never_takes_the_pointer);
    RUN_TEST(a_popover_measures_what_was_put_inside_it);
    RUN_TEST(popover_accessors_reject_other_widgets);
    RUN_TEST(a_dialog_covers_the_window_and_swallows_presses_beside_it);
    RUN_TEST(a_dialog_survives_the_window_changing_size);
    RUN_TEST(a_dialog_is_not_arranged_into_the_flow);
    RUN_TEST(a_dialog_title_bar_is_tall_enough_to_take_a_touch);
    RUN_TEST(a_click_inside_a_dialog_reaches_the_host);
    RUN_TEST(dialog_accessors_reject_other_widgets);
    RUN_TEST(a_combo_box_shows_its_first_item);
    RUN_TEST(clicking_a_combo_box_opens_its_menu_and_choosing_closes_it);
    RUN_TEST(arrow_keys_move_a_combo_box_without_opening_it);
    RUN_TEST(a_combo_box_centres_its_text_and_keeps_room_for_the_arrow);
    RUN_TEST(a_combo_box_keeps_its_width_whatever_is_chosen);
    RUN_TEST(a_combo_box_menu_is_as_wide_as_its_button);
    RUN_TEST(combo_box_accessors_reject_other_widgets);
    RUN_TEST(a_shortcut_activates_a_node_wherever_focus_is);
    RUN_TEST(a_shortcut_does_not_fire_when_the_key_was_wanted);
    RUN_TEST(the_shortcut_table_has_a_limit_and_can_be_cleared);
    RUN_TEST(the_secondary_button_asks_for_a_context_menu);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(overlays);
    GREATEST_MAIN_END();
}
