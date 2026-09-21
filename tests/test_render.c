/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_render.c - rendering a widget, or a window, into memory.
 *
 * These are the only tests that look at pixels. Everything else asserts on the
 * commands a widget emitted, which says what was asked for; this says what
 * came out, which is the question a screenshot or a printed page actually
 * asks.
 */

#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_render.h"
#include "schultz_thorvg.h"
#include "schultz_video.h"
#include "schultz_widgets.h"

#define FONT_PATH "assets/fonts/DejaVuSans.ttf"
#define PNG_PATH  "assets/images/checker.png"

typedef struct {
    schultz_tree           *tree;
    schultz_font_system    *fonts;
    schultz_glyph_cache    *glyphs;
    schultz_image_table    *images;
    schultz_resource_table *resources;
    schultz_handle          font;
    schultz_theme           theme;
    schultz_render_options  options;
} render_fixture;

static int32_t fixture_setup(render_fixture *f)
{
    int32_t result;

    memset(f, 0, sizeof(*f));
    result = schultz_tree_create(&f->tree);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(f->tree, schultz_rect_make(0, 0, 200, 200));

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
    result = schultz_image_table_create(&f->images);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_resource_table_create(&f->resources);
    if (result != SCHULTZ_OK) {
        return result;
    }

    schultz_tree_set_font_system(f->tree, f->fonts);
    schultz_tree_set_image_table(f->tree, f->images);
    schultz_theme_init(&f->theme);
    schultz_theme_set_font(&f->theme, SCHULTZ_TOKEN_FONT_BODY, f->font);
    schultz_tree_set_theme(f->tree, &f->theme);

    f->options.fonts      = f->fonts;
    f->options.glyphs     = f->glyphs;
    f->options.images     = f->images;
    f->options.resources  = f->resources;
    f->options.background = schultz_color_rgba(0, 0, 0, 0);
    return SCHULTZ_OK;
}

static void fixture_teardown(render_fixture *f)
{
    /*
     * The tree first. A node may hold a picture -- a video node holds the
     * frame it is showing -- and it gives that back when it is destroyed, so
     * the table it gives it back to has to still be there. Every other
     * fixture here already does it this way round.
     */
    schultz_resource_table_destroy(f->resources);
    schultz_tree_destroy(f->tree);
    schultz_image_table_destroy(f->images);
    schultz_glyph_cache_destroy(f->glyphs);
    schultz_font_system_destroy(f->fonts);
}

/* The channels of one pixel, unpremultiplied enough to compare against. */
static uint32_t pixel_at(const uint32_t *pixels, uint32_t stride, uint32_t x,
                         uint32_t y)
{
    return pixels[(size_t)y * stride + x];
}

static uint32_t alpha_of(uint32_t pixel)  { return (pixel >> 24) & 0xFFu; }
static uint32_t red_of(uint32_t pixel)    { return (pixel >> 16) & 0xFFu; }
static uint32_t green_of(uint32_t pixel)  { return (pixel >> 8) & 0xFFu; }
static uint32_t blue_of(uint32_t pixel)   { return pixel & 0xFFu; }

/* How many pixels in a buffer are not fully transparent. */
static uint32_t painted_count(const uint32_t *pixels, uint32_t count)
{
    uint32_t i;
    uint32_t n = 0;

    for (i = 0; i < count; i++) {
        if (alpha_of(pixels[i]) != 0u) {
            n++;
        }
    }
    return n;
}

TEST a_node_reports_the_buffer_it_needs(void)
{
    render_fixture f;
    schultz_handle panel;
    uint32_t width = 0;
    uint32_t height = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(5, 7, 40, 30));

    ASSERT_EQ(SCHULTZ_OK, schultz_render_size(f.tree, panel, 1.0f, &width,
                                              &height));
    ASSERT_EQ(40u, width);
    ASSERT_EQ(30u, height);

    /* At three times the resolution, three times the pixels. */
    ASSERT_EQ(SCHULTZ_OK, schultz_render_size(f.tree, panel, 3.0f, &width,
                                              &height));
    ASSERT_EQ(120u, width);
    ASSERT_EQ(90u, height);

    /* A fractional size is rounded outward, so nothing is cut off. */
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 10.5f,
                                                             10.5f));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_size(f.tree, panel, 1.0f, &width,
                                              &height));
    ASSERT_EQ(11u, width);
    ASSERT_EQ(11u, height);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_render_size(f.tree, panel, 0.0f, &width, &height));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_render_size(f.tree, (schultz_handle)999, 1.0f, &width,
                                  &height));

    fixture_teardown(&f);
    PASS();
}

TEST a_panel_comes_out_the_colour_it_was_given(void)
{
    render_fixture f;
    schultz_handle panel;
    uint32_t pixels[32 * 24];

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 32, 24));
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_BACKGROUND,
        schultz_value_color(schultz_color_rgba(200, 100, 50, 255)));
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_CORNER_RADIUS,
                                    schultz_value_number(0.0f));
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, panel, 1.0f,
                                                   &f.options, pixels, 32u,
                                                   24u, 0u));

    /* The middle is the fill, opaque, and the whole buffer is covered. */
    {
        uint32_t middle = pixel_at(pixels, 32u, 16u, 12u);

        ASSERT_EQ(255u, alpha_of(middle));
        ASSERT_EQ(200u, red_of(middle));
        ASSERT_EQ(100u, green_of(middle));
        ASSERT_EQ(50u, blue_of(middle));
    }
    ASSERT_EQ(32u * 24u, painted_count(pixels, 32u * 24u));

    fixture_teardown(&f);
    PASS();
}

/*
 * The node lands at the buffer's top left whatever its position in the tree,
 * so rendering one widget gives that widget and nothing around it.
 */
TEST rendering_one_widget_leaves_out_everything_around_it(void)
{
    render_fixture f;
    schultz_handle behind;
    schultz_handle wanted;
    uint32_t pixels[20 * 20];

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &behind);
    schultz_node_set_bounds(f.tree, behind, schultz_rect_make(0, 0, 200, 200));
    schultz_node_set_style_property(f.tree, behind, SCHULTZ_PROP_BACKGROUND,
        schultz_value_color(schultz_color_rgba(255, 0, 0, 255)));

    schultz_panel_create(f.tree, behind, &wanted);
    schultz_node_set_bounds(f.tree, wanted, schultz_rect_make(60, 80, 20, 20));
    schultz_node_set_style_property(f.tree, wanted, SCHULTZ_PROP_BACKGROUND,
        schultz_value_color(schultz_color_rgba(0, 0, 255, 255)));
    schultz_node_set_style_property(f.tree, wanted, SCHULTZ_PROP_CORNER_RADIUS,
                                    schultz_value_number(0.0f));
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, wanted, 1.0f,
                                                   &f.options, pixels, 20u,
                                                   20u, 0u));

    /* Blue everywhere, and nowhere any of the red it sits on. */
    {
        uint32_t corner = pixel_at(pixels, 20u, 0u, 0u);
        uint32_t middle = pixel_at(pixels, 20u, 10u, 10u);

        ASSERT_EQ(255u, blue_of(corner));
        ASSERT_EQ(0u, red_of(corner));
        ASSERT_EQ(255u, blue_of(middle));
        ASSERT_EQ(0u, red_of(middle));
    }

    fixture_teardown(&f);
    PASS();
}

TEST the_background_shows_where_nothing_was_drawn(void)
{
    render_fixture f;
    schultz_handle panel;
    uint32_t pixels[16 * 16];

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    /* No background of its own, so nothing is painted over the buffer. */
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 16, 16));
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, panel, 1.0f,
                                                   &f.options, pixels, 16u,
                                                   16u, 0u));
    /* Transparent by default, which is what an icon export wants. */
    ASSERT_EQ(0u, painted_count(pixels, 16u * 16u));

    f.options.background = schultz_color_rgba(10, 20, 30, 255);
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, panel, 1.0f,
                                                   &f.options, pixels, 16u,
                                                   16u, 0u));
    ASSERT_EQ(16u * 16u, painted_count(pixels, 16u * 16u));
    ASSERT_EQ(10u, red_of(pixel_at(pixels, 16u, 8u, 8u)));

    fixture_teardown(&f);
    PASS();
}

/*
 * The point of the scale: the same tree at higher resolution without laying it
 * out again, with the picture four times as large rather than four copies of
 * the same pixels.
 */
TEST rendering_at_a_larger_scale_makes_a_larger_picture(void)
{
    render_fixture f;
    schultz_handle panel;
    uint32_t small[16 * 16];
    uint32_t large[64 * 64];

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 16, 16));
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_BACKGROUND,
        schultz_value_color(schultz_color_rgba(0, 200, 0, 255)));
    /* A rounded corner, so scaling has something to be sharp about. */
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_CORNER_RADIUS,
                                    schultz_value_number(6.0f));
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, panel, 1.0f,
                                                   &f.options, small, 16u,
                                                   16u, 0u));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, panel, 4.0f,
                                                   &f.options, large, 64u,
                                                   64u, 0u));

    /* Both fill their buffer in the middle and leave the rounded corner out. */
    ASSERT_EQ(255u, alpha_of(pixel_at(small, 16u, 8u, 8u)));
    ASSERT_EQ(255u, alpha_of(pixel_at(large, 64u, 32u, 32u)));
    ASSERT_EQ(0u, alpha_of(pixel_at(small, 16u, 0u, 0u)));
    ASSERT_EQ(0u, alpha_of(pixel_at(large, 64u, 0u, 0u)));

    /*
     * Scaled up rather than blown up: the large one covers about sixteen
     * times the pixels, since it is four times the size on each side.
     */
    {
        uint32_t few = painted_count(small, 16u * 16u);
        uint32_t many = painted_count(large, 64u * 64u);

        ASSERT(many > few * 14u);
        ASSERT(many < few * 18u);
    }

    fixture_teardown(&f);
    PASS();
}

/*
 * Text is rasterized at the scale asked for. If it were scaled up from screen
 * size the ink would be four blocky copies of the same coverage; rasterized
 * afresh it has its own outline, so the two disagree.
 */
TEST text_is_rasterized_at_the_scale_it_is_rendered_at(void)
{
    render_fixture f;
    schultz_handle label;
    uint32_t small[40 * 24];
    uint32_t large[160 * 96];
    uint32_t few;
    uint32_t many;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_label_create(f.tree,
                    schultz_tree_root(f.tree), "Wo", &label));
    schultz_node_set_bounds(f.tree, label, schultz_rect_make(0, 0, 40, 24));
    schultz_node_set_style_property(f.tree, label, SCHULTZ_PROP_TEXT_COLOR,
        schultz_value_color(schultz_color_rgba(255, 255, 255, 255)));
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, label, 1.0f,
                                                   &f.options, small, 40u,
                                                   24u, 0u));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, label, 4.0f,
                                                   &f.options, large, 160u,
                                                   96u, 0u));

    few  = painted_count(small, 40u * 24u);
    many = painted_count(large, 160u * 96u);
    ASSERT(few > 0u);

    /*
     * Around sixteen times the ink, because the glyphs were drawn four times
     * as large. A blown up bitmap would land near that too, so the check that
     * matters is the next one.
     */
    ASSERT(many > few * 10u);

    /*
     * A glyph rasterized at four times the size has its own antialiasing, so
     * it has partly covered pixels of its own. Four copies of one screen sized
     * pixel would be four identical values, and a run of four identical values
     * across every edge is what a blown up bitmap looks like. Check that the
     * large render has partial coverage that is not simply the small one
     * repeated.
     */
    {
        uint32_t x;
        uint32_t y;
        uint32_t differs = 0;

        for (y = 0; y < 96u; y += 4u) {
            for (x = 0; x < 160u; x += 4u) {
                uint32_t a = alpha_of(pixel_at(large, 160u, x, y));
                uint32_t b = alpha_of(pixel_at(large, 160u, x + 1u, y));

                if (a != b) {
                    differs++;
                }
            }
        }
        ASSERT(differs > 0u);
    }

    fixture_teardown(&f);
    PASS();
}

TEST an_image_renders_into_the_buffer(void)
{
    render_fixture f;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_handle icon;
    uint32_t pixels[48 * 48];

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_load_file(f.images, PNG_PATH,
                                                  &image));
    ASSERT_EQ(SCHULTZ_OK, schultz_icon_create(f.tree,
                    schultz_tree_root(f.tree), image, &icon));
    schultz_node_set_bounds(f.tree, icon, schultz_rect_make(0, 0, 48, 48));
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, icon, 1.0f,
                                                   &f.options, pixels, 48u,
                                                   48u, 0u));

    /* The picture covers the whole icon, and it is not one flat colour. */
    ASSERT_EQ(48u * 48u, painted_count(pixels, 48u * 48u));
    {
        uint32_t first = pixel_at(pixels, 48u, 4u, 4u);
        uint32_t other = pixel_at(pixels, 48u, 12u, 4u);

        ASSERT(first != other);
    }
    /* The warm diagonal the test image draws is on the diagonal. */
    ASSERT(red_of(pixel_at(pixels, 48u, 20u, 20u)) >
           blue_of(pixel_at(pixels, 48u, 20u, 20u)));

    fixture_teardown(&f);
    PASS();
}

/* A render can be handed straight back as an image, since the layout matches. */
/*
 * A still picture is resized to its box, so a fill fit really stretches. An
 * animation is scaled instead, because resizing the scene its animation is
 * driving corrupts the frame, so this is the case that says the two are
 * handled differently on purpose.
 */
TEST a_still_picture_stretches_when_it_is_told_to(void)
{
    render_fixture f;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_handle icon;
    uint32_t pixels[96 * 24];
    uint32_t x;
    uint32_t painted = 0;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_image_load_file(f.images, PNG_PATH, &image);
    schultz_icon_create(f.tree, schultz_tree_root(f.tree), image, &icon);
    ASSERT_EQ(SCHULTZ_OK, schultz_icon_set_fit(f.tree, icon,
                                               SCHULTZ_FIT_FILL));
    /* A box four times as wide as it is tall, for a square picture. */
    schultz_node_set_bounds(f.tree, icon, schultz_rect_make(0, 0, 96, 24));
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, icon, 1.0f,
                                                   &f.options, pixels, 96u,
                                                   24u, 0u));
    /* Every column has ink in it, which only happens if it really stretched. */
    for (x = 0; x < 96u; x++) {
        if (alpha_of(pixel_at(pixels, 96u, x, 12u)) != 0u) {
            painted++;
        }
    }
    ASSERT_EQ(96u, painted);

    fixture_teardown(&f);
    PASS();
}

TEST a_render_can_become_an_image(void)
{
    render_fixture f;
    schultz_handle panel;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_size size;
    uint32_t pixels[24 * 16];

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 24, 16));
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_BACKGROUND,
        schultz_value_color(schultz_color_rgba(1, 2, 3, 255)));
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, panel, 1.0f,
                                                   &f.options, pixels, 24u,
                                                   16u, 0u));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_set_pixels(f.images, pixels, 24u, 16u,
                                                   0u, &image));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_size(f.images, image, &size));
    ASSERT_EQ(24.0f, size.width);
    ASSERT_EQ(16.0f, size.height);

    fixture_teardown(&f);
    PASS();
}

TEST rendering_refuses_what_it_cannot_do(void)
{
    render_fixture f;
    schultz_handle panel;
    uint32_t pixels[8 * 8];

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 8, 8));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_render_to_buffer(NULL, panel, 1.0f, &f.options, pixels,
                                       8u, 8u, 0u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_render_to_buffer(f.tree, panel, 1.0f, NULL, pixels, 8u,
                                       8u, 0u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_render_to_buffer(f.tree, panel, 1.0f, &f.options, NULL,
                                       8u, 8u, 0u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_render_to_buffer(f.tree, panel, 0.0f, &f.options,
                                       pixels, 8u, 8u, 0u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_render_to_buffer(f.tree, panel, 1.0f, &f.options,
                                       pixels, 0u, 8u, 0u));
    /* A stride narrower than the buffer would write past each row. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_render_to_buffer(f.tree, panel, 1.0f, &f.options,
                                       pixels, 8u, 8u, 4u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_render_to_buffer(f.tree, (schultz_handle)999, 1.0f,
                                       &f.options, pixels, 8u, 8u, 0u));

    fixture_teardown(&f);
    PASS();
}

/* A window that changed size hands the rasterizer a new buffer. */
TEST the_rasterizer_can_be_pointed_at_a_new_buffer(void)
{
    schultz_thorvg *backend = NULL;
    schultz_painter painter;
    uint32_t small[16 * 16];
    uint32_t large[32 * 32];
    schultz_arena arena;
    schultz_draw_list list;

    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_engine_init(0));
    memset(small, 0, sizeof(small));
    memset(large, 0, sizeof(large));
    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_create(small, 16u, 16u, 16u,
                                                &backend));
    schultz_thorvg_painter(backend, &painter);

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_fill_rect(&list,
        schultz_rect_make(0, 0, 32, 32),
        schultz_paint_solid(schultz_color_rgba(255, 0, 0, 255))));

    /* Pointed at the larger buffer, it paints the whole of it, which the
     * small one could never have held. */
    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_set_target(backend, large, 32u, 32u,
                                                    32u));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&list, &painter,
        schultz_rect_make(0, 0, 32, 32)));
    ASSERT((large[31 * 32 + 31] >> 16 & 0xffu) > 200u);

    /* And the buffer it left behind was not touched. */
    ASSERT_EQ(0u, small[16 * 16 - 1]);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_thorvg_set_target(backend, NULL, 32u, 32u, 32u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_thorvg_set_target(backend, large, 0u, 32u, 32u));

    schultz_arena_free(&arena);
    schultz_thorvg_destroy(backend);
    schultz_thorvg_engine_term();
    PASS();
}

/* A canvas with nothing of its own showing, so only what is drawn into it
 * lands in the buffer. */
static schultz_handle bare_canvas(render_fixture *f, float w, float h)
{
    schultz_handle canvas = SCHULTZ_HANDLE_NONE;

    if (schultz_canvas_create(f->tree, schultz_tree_root(f->tree), &canvas)
            != SCHULTZ_OK) {
        return SCHULTZ_HANDLE_NONE;
    }
    schultz_node_set_bounds(f->tree, canvas, schultz_rect_make(0, 0, w, h));
    schultz_node_set_style_property(f->tree, canvas, SCHULTZ_PROP_BACKGROUND,
        schultz_value_color(schultz_color_rgba(0, 0, 0, 0)));
    schultz_node_set_style_property(f->tree, canvas, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(0.0f));
    schultz_node_set_style_property(f->tree, canvas, SCHULTZ_PROP_CORNER_RADIUS,
                                    schultz_value_number(0.0f));
    schultz_tree_resolve_styles(f->tree);
    return canvas;
}

/*
 * The two fill rules only disagree where an outline crosses itself, which is
 * why the shape here is a five pointed star drawn as one unbroken line. Under
 * the nonzero rule the middle is inside the shape; under even odd it is a
 * hole. Anything that ignored the rule would give the same picture twice.
 */
TEST a_self_crossing_path_leaves_a_hole_under_the_even_odd_rule(void)
{
    static const schultz_point star[5] = {
        { 50.0f, 10.0f }, { 73.51f, 82.36f }, { 11.96f, 37.64f },
        { 88.04f, 37.64f }, { 26.49f, 82.36f }
    };
    render_fixture f;
    schultz_handle canvas;
    uint32_t solid[100 * 100];
    uint32_t holed[100 * 100];
    uint32_t solid_painted;
    uint32_t holed_painted;
    schultz_paint ink = schultz_paint_solid(schultz_color_rgba(200, 30, 30,
                                                               255));

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 100.0f, 100.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_polygon(f.tree, canvas, star, 5,
                                                      ink, SCHULTZ_FILL_NONZERO));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, solid, 100u, 100u, 0u));

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_polygon(f.tree, canvas, star, 5,
                              ink, SCHULTZ_FILL_EVEN_ODD));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, holed, 100u, 100u, 0u));

    /* The centre is the whole difference between the two rules. */
    ASSERT_EQ(255u, alpha_of(pixel_at(solid, 100u, 50u, 50u)));
    ASSERT_EQ(0u, alpha_of(pixel_at(holed, 100u, 50u, 50u)));

    /* And the hole is a hole, not one stray pixel. */
    solid_painted = painted_count(solid, 100u * 100u);
    holed_painted = painted_count(holed, 100u * 100u);
    ASSERT(holed_painted < solid_painted);

    fixture_teardown(&f);
    PASS();
}

/*
 * A dash offset says how far along the pattern a line starts. With a ten on
 * ten off pattern, moving the offset by ten swaps every mark for a gap, so
 * the two renders are photographic negatives of each other where the line is.
 */
TEST a_dash_offset_moves_where_the_first_gap_falls(void)
{
    render_fixture f;
    schultz_handle canvas;
    schultz_handle dashes = SCHULTZ_HANDLE_NONE;
    uint32_t start[60 * 20];
    uint32_t moved[60 * 20];
    schultz_stroke stroke;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 60.0f, 20.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_OK, schultz_dash_pair(f.resources, 10.0f, 10.0f,
                                            &dashes));

    stroke = schultz_stroke_solid(schultz_color_rgba(0, 0, 0, 255), 4.0f);
    stroke.dash = dashes;

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_line(f.tree, canvas,
                              schultz_point_make(0.0f, 10.0f),
                              schultz_point_make(60.0f, 10.0f), stroke));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, start, 60u, 20u, 0u));

    stroke.dash_offset = 10.0f;
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_line(f.tree, canvas,
                              schultz_point_make(0.0f, 10.0f),
                              schultz_point_make(60.0f, 10.0f), stroke));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, moved, 60u, 20u, 0u));

    /* At no offset the line opens with a mark and the second ten is a gap. */
    ASSERT(alpha_of(pixel_at(start, 60u, 3u, 10u)) > 0u);
    ASSERT_EQ(0u, alpha_of(pixel_at(start, 60u, 15u, 10u)));

    /* Move the offset one whole mark and the two trade places. */
    ASSERT_EQ(0u, alpha_of(pixel_at(moved, 60u, 3u, 10u)));
    ASSERT(alpha_of(pixel_at(moved, 60u, 15u, 10u)) > 0u);

    fixture_teardown(&f);
    PASS();
}

/*
 * Text takes a paint now, so a gradient reaches the letters. A run is
 * composited into one buffer and uploaded as a picture, which a gradient
 * cannot fill, so this is really asking whether the mask route ran at all.
 */
TEST text_filled_with_a_gradient_changes_colour_across_the_run(void)
{
    static const schultz_gradient_stop stops[2] = {
        { 0.0f, { 255u, 0u, 0u, 255u } },
        { 1.0f, { 0u, 0u, 255u, 255u } }
    };
    render_fixture f;
    schultz_handle canvas;
    schultz_handle sheen = SCHULTZ_HANDLE_NONE;
    uint32_t flat[120 * 40];
    uint32_t graded[120 * 40];
    uint32_t i;
    uint32_t flat_blue = 0u;
    uint32_t graded_blue = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 120.0f, 40.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(SCHULTZ_OK, schultz_gradient_linear(f.resources,
                  schultz_point_make(0.0f, 0.0f),
                  schultz_point_make(1.0f, 0.0f), stops, 2u, &sheen));

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_text(f.tree, canvas, f.font,
                  "Gradient", 2.0f, 26.0f,
                  schultz_paint_solid(schultz_color_rgba(255, 0, 0, 255))));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, flat, 120u, 40u, 0u));

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_text(f.tree, canvas, f.font,
                  "Gradient", 2.0f, 26.0f, schultz_paint_gradient(sheen)));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, graded, 120u, 40u, 0u));

    /* Both drew letters. */
    ASSERT(painted_count(flat, 120u * 40u) > 0u);
    ASSERT(painted_count(graded, 120u * 40u) > 0u);

    /* Red text can never have a pixel bluer than it is red. Graded text must,
     * because the far end of the run is the blue stop. */
    for (i = 0; i < 120u * 40u; i++) {
        if (alpha_of(flat[i]) > 0u && blue_of(flat[i]) > red_of(flat[i])) {
            flat_blue++;
        }
        if (alpha_of(graded[i]) > 0u &&
            blue_of(graded[i]) > red_of(graded[i])) {
            graded_blue++;
        }
    }
    ASSERT_EQ(0u, flat_blue);
    ASSERT(graded_blue > 0u);

    fixture_teardown(&f);
    PASS();
}

/*
 * The rasterizer has no arc, so one is built out of cubics split at the
 * quarter. A full turn is the case that checks the splitting: four segments
 * joined end to end have to close back onto the first one, and if the
 * approximation or the joins were wrong the result would not match an
 * ellipse drawn the ordinary way.
 */
TEST an_arc_of_a_full_turn_covers_the_same_pixels_as_a_circle(void)
{
    render_fixture f;
    schultz_handle canvas;
    uint32_t built[80 * 80];
    uint32_t plain[80 * 80];
    uint8_t steps[SCHULTZ_ARC_STEPS_MAX];
    schultz_point points[SCHULTZ_ARC_POINTS_MAX];
    uint32_t step_count = 0u;
    uint32_t point_count = 0u;
    schultz_rect box = { 8.0f, 8.0f, 64.0f, 64.0f };
    schultz_paint ink = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));
    uint32_t i;
    uint32_t differ = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 80.0f, 80.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(SCHULTZ_OK, schultz_arc_path(box, 0.0f, 360.0f,
                                           SCHULTZ_ARC_PIE, steps,
                                           &step_count, points,
                                           &point_count));
    /* A move, four quarter curves, a line home and a close. */
    ASSERT_EQ(7u, step_count);
    ASSERT_EQ(14u, point_count);

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_path(f.tree, canvas, steps,
                              step_count, points, point_count, ink,
                              SCHULTZ_FILL_NONZERO));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, built, 80u, 80u, 0u));

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_ellipse(f.tree, canvas, box,
                                                      ink));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, plain, 80u, 80u, 0u));

    /*
     * Not pixel for pixel: the two take different routes through the
     * rasterizer and the edge is antialiased. Whether a pixel was painted at
     * all is the question, and a handful of edge pixels either way is the
     * approximation showing, not a broken arc.
     */
    for (i = 0; i < 80u * 80u; i++) {
        if ((alpha_of(built[i]) > 128u) != (alpha_of(plain[i]) > 128u)) {
            differ++;
        }
    }
    ASSERT(painted_count(plain, 80u * 80u) > 2000u);
    ASSERT(differ < 40u);

    fixture_teardown(&f);
    PASS();
}

/*
 * A pie closes by way of the centre and a chord closes straight across, so
 * the triangle between the chord and the centre belongs to one and not the
 * other. That wedge is the whole difference and it is what this looks at.
 */
TEST a_pie_closes_through_its_centre_and_a_chord_does_not(void)
{
    render_fixture f;
    schultz_handle canvas;
    uint32_t pie[80 * 80];
    uint32_t chord[80 * 80];
    uint8_t steps[SCHULTZ_ARC_STEPS_MAX];
    schultz_point points[SCHULTZ_ARC_POINTS_MAX];
    uint32_t step_count = 0u;
    uint32_t point_count = 0u;
    schultz_rect box = { 8.0f, 8.0f, 64.0f, 64.0f };
    schultz_paint ink = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 80.0f, 80.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);

    /* A quarter, from three o'clock round to six. */
    ASSERT_EQ(SCHULTZ_OK, schultz_arc_path(box, 0.0f, 90.0f, SCHULTZ_ARC_PIE,
                              steps, &step_count, points, &point_count));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_path(f.tree, canvas, steps,
                              step_count, points, point_count, ink,
                              SCHULTZ_FILL_NONZERO));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, pie, 80u, 80u, 0u));

    ASSERT_EQ(SCHULTZ_OK, schultz_arc_path(box, 0.0f, 90.0f,
                              SCHULTZ_ARC_CHORD, steps, &step_count, points,
                              &point_count));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_path(f.tree, canvas, steps,
                              step_count, points, point_count, ink,
                              SCHULTZ_FILL_NONZERO));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, chord, 80u, 80u, 0u));

    /*
     * The centre of the box is a corner of the pie and is outside the chord.
     * The chord's own piece is the thin lens between the straight line and
     * the curve, so a point out there is in both. The line runs from (72,40)
     * to (40,72), which is x + y = 112, and (59,59) sums to 118.
     */
    ASSERT(alpha_of(pixel_at(pie, 80u, 41u, 41u)) > 128u);
    ASSERT_EQ(0u, alpha_of(pixel_at(chord, 80u, 40u, 40u)));
    ASSERT(alpha_of(pixel_at(pie, 80u, 59u, 59u)) > 128u);
    ASSERT(alpha_of(pixel_at(chord, 80u, 59u, 59u)) > 128u);

    /* So the pie is the larger of the two by about that triangle. */
    ASSERT(painted_count(pie, 80u * 80u) >
           painted_count(chord, 80u * 80u) + 400u);

    fixture_teardown(&f);
    PASS();
}

/* An open arc has no closing step at all, so a stroke follows the curve and
 * stops rather than cutting back across. */
TEST an_open_arc_is_a_line_rather_than_a_shape(void)
{
    render_fixture f;
    schultz_handle canvas;
    uint32_t pixels[80 * 80];
    uint8_t steps[SCHULTZ_ARC_STEPS_MAX];
    schultz_point points[SCHULTZ_ARC_POINTS_MAX];
    uint32_t step_count = 0u;
    uint32_t point_count = 0u;
    schultz_rect box = { 8.0f, 8.0f, 64.0f, 64.0f };
    schultz_stroke pen = schultz_stroke_solid(schultz_color_rgba(0, 0, 0, 255),
                                              3.0f);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 80.0f, 80.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(SCHULTZ_OK, schultz_arc_path(box, 0.0f, 90.0f, SCHULTZ_ARC_OPEN,
                              steps, &step_count, points, &point_count));
    /* One move and one quarter curve, and nothing joining the ends. */
    ASSERT_EQ(2u, step_count);
    ASSERT_EQ(4u, point_count);
    ASSERT_EQ((uint8_t)SCHULTZ_PATH_MOVE, steps[0]);
    ASSERT_EQ((uint8_t)SCHULTZ_PATH_CURVE, steps[1]);

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_stroke_path(f.tree, canvas, steps,
                              step_count, points, point_count, pen));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, pixels, 80u, 80u, 0u));

    /* The curve is there. */
    ASSERT(painted_count(pixels, 80u * 80u) > 100u);
    /* The middle is not, because nothing closed the shape and it is a line. */
    ASSERT_EQ(0u, alpha_of(pixel_at(pixels, 80u, 45u, 45u)));

    fixture_teardown(&f);
    PASS();
}

/* A sweep of nothing draws nothing, rather than a stray move. */
TEST an_arc_refuses_what_it_cannot_draw(void)
{
    uint8_t steps[SCHULTZ_ARC_STEPS_MAX];
    schultz_point points[SCHULTZ_ARC_POINTS_MAX];
    uint32_t step_count = 99u;
    uint32_t point_count = 99u;
    schultz_rect box = { 0.0f, 0.0f, 10.0f, 10.0f };
    schultz_rect empty = { 0.0f, 0.0f, 0.0f, 10.0f };

    ASSERT_EQ(SCHULTZ_OK, schultz_arc_path(box, 0.0f, 0.0f, SCHULTZ_ARC_OPEN,
                              steps, &step_count, points, &point_count));
    ASSERT_EQ(0u, step_count);
    ASSERT_EQ(0u, point_count);

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_arc_path(empty, 0.0f, 90.0f, SCHULTZ_ARC_OPEN, steps,
                               &step_count, points, &point_count));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_arc_path(box, 0.0f, 90.0f, 99u, steps, &step_count,
                               points, &point_count));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_arc_path(box, 0.0f, 90.0f, SCHULTZ_ARC_OPEN, NULL,
                               &step_count, points, &point_count));
    PASS();
}

/*
 * A square turned a quarter about its own middle lands exactly where it
 * started, which is the one rotation whose answer is known without measuring
 * anything. If the pivot, the direction or the composition were wrong, this
 * is the test that could not pass by accident.
 */
TEST a_square_turned_a_quarter_covers_the_same_pixels(void)
{
    render_fixture f;
    schultz_handle canvas;
    uint32_t still[80 * 80];
    uint32_t turned[80 * 80];
    schultz_rect box = { 20.0f, 20.0f, 40.0f, 40.0f };
    schultz_paint ink = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));
    uint32_t i;
    uint32_t differ = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 80.0f, 80.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas, box, ink,
                                                   0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, still, 80u, 80u, 0u));

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_rotation_begin(f.tree, canvas, 90.0f,
                                                       40.0f, 40.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas, box, ink,
                                                   0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_rotation_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, turned, 80u, 80u, 0u));

    for (i = 0; i < 80u * 80u; i++) {
        if ((alpha_of(still[i]) > 128u) != (alpha_of(turned[i]) > 128u)) {
            differ++;
        }
    }
    ASSERT(painted_count(still, 80u * 80u) > 1400u);
    ASSERT(differ < 20u);

    /* And a different angle really does move it, so the test above is not
     * passing because nothing happened. */
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_rotation_begin(f.tree, canvas, 45.0f,
                                                       40.0f, 40.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas, box, ink,
                                                   0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_rotation_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, turned, 80u, 80u, 0u));
    ASSERT_EQ(0u, alpha_of(pixel_at(turned, 80u, 22u, 22u)));
    ASSERT(alpha_of(pixel_at(still, 80u, 22u, 22u)) > 128u);

    fixture_teardown(&f);
    PASS();
}

/*
 * Text at a quarter turn is the case this was built for: a label running up
 * the side of something. A run that was wide and short has to come out tall
 * and narrow, and it has to be drawn from outlines rather than from a turned
 * picture of itself.
 */
TEST text_turned_a_quarter_reads_down_the_side(void)
{
    render_fixture f;
    schultz_handle canvas;
    uint32_t flat[120 * 120];
    uint32_t side[120 * 120];
    schultz_paint ink = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));
    uint32_t x;
    uint32_t y;
    uint32_t flat_w = 0u, flat_h = 0u, side_w = 0u, side_h = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 120.0f, 120.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_text(f.tree, canvas, f.font,
                              "sideways", 10.0f, 60.0f, ink));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, flat, 120u, 120u, 0u));

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_rotation_begin(f.tree, canvas, -90.0f,
                                                       10.0f, 60.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_text(f.tree, canvas, f.font,
                              "sideways", 10.0f, 60.0f, ink));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_rotation_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, side, 120u, 120u, 0u));

    /* The extent of the ink, each way, for both renders. */
    for (x = 0; x < 120u; x++) {
        for (y = 0; y < 120u; y++) {
            if (alpha_of(pixel_at(flat, 120u, x, y)) > 0u) {
                if (x + 1u > flat_w) { flat_w = x + 1u; }
                if (y + 1u > flat_h) { flat_h = y + 1u; }
            }
            if (alpha_of(pixel_at(side, 120u, x, y)) > 0u) {
                if (x + 1u > side_w) { side_w = x + 1u; }
                if (y + 1u > side_h) { side_h = y + 1u; }
            }
        }
    }
    ASSERT(painted_count(flat, 120u * 120u) > 100u);
    ASSERT(painted_count(side, 120u * 120u) > 100u);
    /* Wide and short becomes tall and narrow. */
    ASSERT(flat_w > flat_h);
    ASSERT(side_h > side_w);

    /*
     * Roughly as much ink either way. A turned picture of the text would lose
     * coverage to resampling; outlines rasterized at the angle do not.
     */
    {
        uint32_t a = painted_count(flat, 120u * 120u);
        uint32_t b = painted_count(side, 120u * 120u);
        uint32_t big = (a > b) ? a : b;
        uint32_t small = (a > b) ? b : a;

        ASSERT(small * 5u > big * 4u);
    }

    fixture_teardown(&f);
    PASS();
}

/*
 * The whole design rests on a canvas keeping its own upright bounds, so
 * nothing turned inside one can reach the rest of the window. The canvas here
 * sits inside a larger panel and the text is turned so it would run well past
 * the canvas if nothing stopped it.
 */
TEST a_rotated_run_stays_inside_the_canvas_that_clips_it(void)
{
    render_fixture f;
    schultz_handle panel = SCHULTZ_HANDLE_NONE;
    schultz_handle canvas = SCHULTZ_HANDLE_NONE;
    uint32_t pixels[120 * 120];
    schultz_paint ink = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));
    uint32_t x;
    uint32_t y;
    uint32_t outside = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree,
                              schultz_tree_root(f.tree), &panel));
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 120, 120));
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_BACKGROUND,
        schultz_value_color(schultz_color_rgba(0, 0, 0, 0)));
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_create(f.tree, panel, &canvas));
    /* A small canvas in the middle of a large panel. */
    schultz_node_set_bounds(f.tree, canvas,
                            schultz_rect_make(40, 40, 40, 40));
    schultz_node_set_style_property(f.tree, canvas, SCHULTZ_PROP_BACKGROUND,
        schultz_value_color(schultz_color_rgba(0, 0, 0, 0)));
    schultz_node_set_style_property(f.tree, canvas, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(0.0f));
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_rotation_begin(f.tree, canvas, 35.0f,
                                                       20.0f, 20.0f));
    /* Long enough to run off in both directions if it were not clipped. */
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_text(f.tree, canvas, f.font,
                              "far too long to fit in here", -60.0f, 20.0f,
                              ink));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas,
                              schultz_rect_make(-40, -40, 120, 120), ink,
                              0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_rotation_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, panel, 1.0f,
                              &f.options, pixels, 120u, 120u, 0u));

    /* Something was drawn, and every bit of it is inside the canvas. */
    ASSERT(painted_count(pixels, 120u * 120u) > 100u);
    for (x = 0; x < 120u; x++) {
        for (y = 0; y < 120u; y++) {
            if (alpha_of(pixel_at(pixels, 120u, x, y)) == 0u) {
                continue;
            }
            if (x < 40u || x >= 80u || y < 40u || y >= 80u) {
                outside++;
            }
        }
    }
    ASSERT_EQ(0u, outside);

    fixture_teardown(&f);
    PASS();
}

/*
 * Stroked text is a ring round each letter rather than a fill inside it, so
 * the middle of a thick stroke is empty where a filled letter is solid. Both
 * come from the same outlines; only what is done with them differs.
 */
TEST text_can_be_stroked_instead_of_filled(void)
{
    render_fixture f;
    schultz_handle canvas;
    /* Rendered large, because at body size a one pixel ring is as wide as
     * the stem it surrounds and the two cover the same ground. */
    uint32_t filled[480 * 180];
    uint32_t outlined[480 * 180];
    schultz_paint ink = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));
    schultz_stroke pen = schultz_stroke_solid(schultz_color_rgba(0, 0, 0, 255),
                                              1.0f);

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 160.0f, 60.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_text(f.tree, canvas, f.font,
                              "HHHH", 10.0f, 40.0f, ink));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 3.0f,
                              &f.options, filled, 480u, 180u, 0u));

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_stroke_text(f.tree, canvas, f.font,
                              "HHHH", 10.0f, 40.0f, pen));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 3.0f,
                              &f.options, outlined, 480u, 180u, 0u));

    /* Both drew letters, in about the same place. */
    ASSERT(painted_count(filled, 480u * 180u) > 500u);
    ASSERT(painted_count(outlined, 480u * 180u) > 500u);

    /*
     * The test is not which covers more ground: a ring straddles the outline
     * and spills outside it, so the stroked version paints more pixels than
     * the fill, not fewer. What only a ring can do is leave the middle of a
     * stem empty. So: pixels that are solid in the fill and untouched in the
     * outline. Drawing the filled version twice would find none of those.
     */
    {
        uint32_t x;
        uint32_t y;
        uint32_t hollow = 0u;

        for (x = 0; x < 480u; x++) {
            for (y = 0; y < 180u; y++) {
                if (alpha_of(pixel_at(filled, 480u, x, y)) > 200u &&
                    alpha_of(pixel_at(outlined, 480u, x, y)) == 0u) {
                    hollow++;
                }
            }
        }
        ASSERT(hollow > 50u);
    }

    fixture_teardown(&f);
    PASS();
}

/*
 * The dash offset reaches a widget's own border, not only a stroke a canvas
 * built by hand. Two renders of the same panel, half a period apart, have to
 * differ where the pattern falls: the marks of one land in the gaps of the
 * other.
 */
TEST a_dashed_border_honours_an_offset_from_style(void)
{
    render_fixture f;
    schultz_handle panel = SCHULTZ_HANDLE_NONE;
    schultz_handle dashes = SCHULTZ_HANDLE_NONE;
    uint32_t start[80 * 40];
    uint32_t moved[80 * 40];
    uint32_t i;
    uint32_t differ = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_dash_pair(f.resources, 6.0f, 6.0f,
                                            &dashes));
    ASSERT_EQ(SCHULTZ_OK, schultz_panel_create(f.tree,
                              schultz_tree_root(f.tree), &panel));
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 80, 40));
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_BACKGROUND,
        schultz_value_color(schultz_color_rgba(0, 0, 0, 0)));
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_CORNER_RADIUS,
                                    schultz_value_number(0.0f));
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_BORDER_COLOR,
        schultz_value_color(schultz_color_rgba(0, 0, 0, 255)));
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_BORDER_WIDTH,
                                    schultz_value_number(2.0f));
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_BORDER_DASH,
                                    schultz_value_dash(dashes));
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, panel, 1.0f,
                              &f.options, start, 80u, 40u, 0u));

    /* Half a period along, so every mark trades places with a gap. */
    schultz_node_set_style_property(f.tree, panel,
                                    SCHULTZ_PROP_BORDER_DASH_OFFSET,
                                    schultz_value_number(6.0f));
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, panel, 1.0f,
                              &f.options, moved, 80u, 40u, 0u));

    ASSERT(painted_count(start, 80u * 40u) > 50u);
    for (i = 0; i < 80u * 40u; i++) {
        if ((alpha_of(start[i]) > 128u) != (alpha_of(moved[i]) > 128u)) {
            differ++;
        }
    }
    /* Most of the border moved, rather than a stray pixel at one corner. */
    ASSERT(differ > 50u);

    fixture_teardown(&f);
    PASS();
}

/*
 * A decoded frame reaching the buffer, which is the last step of the video
 * pipeline and the only one that can be checked by looking.
 *
 * The clip is one flat red. Getting the conversion coefficients or the
 * sixteen-to-235 range wrong shows up as a colour that is close but not
 * right, and swapping two channels shows up as blue. Neither would be caught
 * by any test that only counts frames.
 */
TEST a_decoded_frame_reaches_the_buffer_the_right_colour(void)
{
    render_fixture f;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    uint32_t pixels[64 * 64];
    unsigned char *bytes = NULL;
    long size = 0;
    FILE *file;
    uint32_t middle;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_video_create(f.tree,
                              schultz_tree_root(f.tree), 0u, &node));
    schultz_node_set_bounds(f.tree, node, schultz_rect_make(0, 0, 64, 64));
    schultz_tree_resolve_styles(f.tree);

    file = fopen("tests/assets/clip-red.webm", "rb");
    ASSERT(file != NULL);
    ASSERT_EQ(0, fseek(file, 0, SEEK_END));
    size = ftell(file);
    ASSERT(size > 0);
    rewind(file);
    bytes = (unsigned char *)malloc((size_t)size);
    ASSERT(bytes != NULL);
    ASSERT_EQ((size_t)size, fread(bytes, 1, (size_t)size, file));
    fclose(file);
    ASSERT_EQ(SCHULTZ_OK, schultz_video_write(f.tree, node, bytes,
                                              (uint64_t)size));
    free(bytes);

    ASSERT_EQ(SCHULTZ_OK, schultz_video_play(f.tree, node));
    for (i = 1; i <= 10u; i++) {
        schultz_tree_advance(f.tree, (uint64_t)i * 33u);
    }
    ASSERT(schultz_video_frames_shown(f.tree, node) > 0u);

    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, node, 1.0f,
                              &f.options, pixels, 64u, 64u, 0u));

    middle = pixel_at(pixels, 64u, 32u, 32u);
    ASSERT_EQ(255u, alpha_of(middle));
    /*
     * Not exact: the colour went through a lossy encoder and back, so a few
     * counts either way is the encoder rather than the conversion. Red at
     * 180, or red and blue swapped, is what this is looking for.
     */
    ASSERT(red_of(middle) > 220u);
    ASSERT(green_of(middle) < 40u);
    ASSERT(blue_of(middle) < 40u);

    /* And the picture fills the node rather than sitting in a corner. */
    ASSERT_EQ(64u * 64u, painted_count(pixels, 64u * 64u));

    fixture_teardown(&f);
    PASS();
}


/* ------------------------------------------------- repainting in parts */

/* A panel of one flat colour, with square corners so the pixels are exact. */
static void paint_it(schultz_tree *tree, schultz_handle node,
                     schultz_color colour)
{
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BACKGROUND,
                                    schultz_value_color(colour));
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_CORNER_RADIUS,
                                    schultz_value_number(0.0f));
}


/*
 * Two panels far apart, and a third between them that must not be touched.
 *
 * 400 wide so that covering both ends with one rectangle wastes far more
 * than SCHULTZ_TREE_DIRTY_WASTE allows -- about 11,800 pixels against a limit
 * of 4,096. That margin is deliberate: at 160 wide the waste is exactly the
 * limit, the two merge, and the test passes or fails on which way the
 * comparison rounds.
 */
static void build_two_ends(schultz_tree *tree, schultz_handle *left,
                           schultz_handle *middle, schultz_handle *right)
{
    schultz_handle root = schultz_tree_root(tree);

    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 32));
    schultz_node_set_bounds(tree, root, schultz_rect_make(0, 0, 400, 32));

    schultz_panel_create(tree, root, left);
    schultz_node_set_bounds(tree, *left, schultz_rect_make(0, 0, 16, 32));
    paint_it(tree, *left, schultz_color_rgba(255, 0, 0, 255));

    schultz_panel_create(tree, root, middle);
    schultz_node_set_bounds(tree, *middle, schultz_rect_make(192, 0, 16, 32));
    paint_it(tree, *middle, schultz_color_rgba(0, 255, 0, 255));

    schultz_panel_create(tree, root, right);
    schultz_node_set_bounds(tree, *right, schultz_rect_make(384, 0, 16, 32));
    paint_it(tree, *right, schultz_color_rgba(0, 0, 255, 255));
}

/*
 * Two changes at opposite ends stay two areas rather than becoming one.
 *
 * This is the whole reason the tree keeps a few rectangles instead of one.
 * With one, these two sixteen pixel panels would name the whole 160 pixel
 * strip between them, and everything in it would be repainted every frame.
 */
TEST two_changes_far_apart_stay_two_areas(void)
{
    schultz_tree *tree = NULL;
    schultz_handle left, middle, right;
    schultz_rect one;
    schultz_rect two;
    schultz_rect whole;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    build_two_ends(tree, &left, &middle, &right);

    schultz_tree_clear_dirty(tree);
    ASSERT_EQ(0u, schultz_tree_dirty_count(tree));

    schultz_node_invalidate(tree, left);
    schultz_node_invalidate(tree, right);
    ASSERT_EQ(2u, schultz_tree_dirty_count(tree));

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_at(tree, 0u, &one));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_at(tree, 1u, &two));
    ASSERT_EQ(SCHULTZ_ERR_EXHAUSTED, schultz_tree_dirty_at(tree, 2u, &one));

    /* Neither reaches the middle panel, which is what a single rectangle
     * covering both could not manage. */
    ASSERT(schultz_rect_right(one) < 192.0f || one.x > 208.0f);
    ASSERT(schultz_rect_right(two) < 192.0f || two.x > 208.0f);

    /* And the union is still available, for whoever wants one number. */
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_region(tree, &whole));
    ASSERT(whole.width > 380.0f);

    schultz_tree_destroy(tree);
    PASS();
}

/* Two changes close together are cheaper as one, and become one. */
TEST two_changes_close_together_become_one_area(void)
{
    schultz_tree *tree = NULL;
    schultz_handle root;
    schultz_handle a = SCHULTZ_HANDLE_NONE;
    schultz_handle b = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    root = schultz_tree_root(tree);
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 160, 32));
    schultz_node_set_bounds(tree, root, schultz_rect_make(0, 0, 160, 32));

    schultz_panel_create(tree, root, &a);
    schultz_node_set_bounds(tree, a, schultz_rect_make(0, 0, 16, 16));
    schultz_panel_create(tree, root, &b);
    schultz_node_set_bounds(tree, b, schultz_rect_make(18, 0, 16, 16));

    schultz_tree_clear_dirty(tree);
    schultz_node_invalidate(tree, a);
    schultz_node_invalidate(tree, b);

    /* Side by side: covering both wastes 32 pixels, far under the line. */
    ASSERT_EQ(1u, schultz_tree_dirty_count(tree));

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * However scattered a frame gets, it never keeps more areas than it said it
 * would, and what it keeps still covers everything that changed. That second
 * half is what stops a merge losing a pixel.
 */
TEST more_changes_than_slots_still_cover_everything(void)
{
    schultz_tree *tree = NULL;
    schultz_handle root;
    schultz_handle boxes[20];
    uint32_t i;
    uint32_t parts;

    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    root = schultz_tree_root(tree);
    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 400, 400));
    schultz_node_set_bounds(tree, root, schultz_rect_make(0, 0, 400, 400));

    for (i = 0; i < 20u; i++) {
        boxes[i] = SCHULTZ_HANDLE_NONE;
        schultz_panel_create(tree, root, &boxes[i]);
        /* Scattered on a diagonal, so no two are close. */
        schultz_node_set_bounds(tree, boxes[i],
            schultz_rect_make((float)(i * 19u), (float)(i * 19u), 8, 8));
    }
    schultz_tree_clear_dirty(tree);
    for (i = 0; i < 20u; i++) {
        schultz_node_invalidate(tree, boxes[i]);
    }

    parts = schultz_tree_dirty_count(tree);
    ASSERT(parts > 1u);
    ASSERT(parts <= (uint32_t)SCHULTZ_TREE_DIRTY_PARTS);

    /* Every box is inside one of the areas kept. Nothing was dropped. */
    for (i = 0; i < 20u; i++) {
        schultz_rect box;
        uint32_t part;
        int32_t covered = 0;

        ASSERT_EQ(SCHULTZ_OK,
                  schultz_node_absolute_bounds(tree, boxes[i], &box));
        for (part = 0; part < parts; part++) {
            schultz_rect one;

            schultz_tree_dirty_at(tree, part, &one);
            if (box.x >= one.x - 0.5f && box.y >= one.y - 0.5f &&
                schultz_rect_right(box) <= schultz_rect_right(one) + 0.5f &&
                schultz_rect_bottom(box) <=
                    schultz_rect_bottom(one) + 0.5f) {
                covered = 1;
                break;
            }
        }
        ASSERT(covered);
    }

    schultz_tree_destroy(tree);
    PASS();
}

/*
 * The one that matters: what a partial repaint leaves on screen is exactly
 * what a full repaint would have.
 *
 * A partial repaint is only correct if the pixels it does not touch were
 * already right and the ones it does touch end up right. Nothing about the
 * rectangles being small proves that, and a merge that lost a pixel would
 * show up here and nowhere else. So this paints a scene, changes two things
 * at opposite ends, repaints only what the tree says changed, and compares
 * against painting the whole thing from scratch.
 */
TEST a_partial_repaint_matches_a_full_one(void)
{
    schultz_tree *tree = NULL;
    schultz_thorvg *backend = NULL;
    schultz_painter painter;
    schultz_arena arena;
    schultz_draw_list list;
    schultz_handle left, middle, right;
    static uint32_t part_way[400 * 32];
    static uint32_t all_at_once[400 * 32];
    schultz_rect parts[SCHULTZ_TREE_DIRTY_PARTS];
    uint32_t count;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_engine_init(0));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    build_two_ends(tree, &left, &middle, &right);
    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 0));

    /* Everything, into the buffer the partial repaint will build on. */
    memset(part_way, 0, sizeof(part_way));
    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_create(part_way, 400u, 32u, 400u,
                                                &backend));
    schultz_thorvg_painter(backend, &painter);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_widget_paint_tree(tree, &list, &arena,
                                        schultz_rect_make(0, 0, 0, 0)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&list, &painter,
                              schultz_rect_make(0, 0, 400, 32)));

    /* Two changes, at opposite ends. */
    schultz_tree_clear_dirty(tree);
    paint_it(tree, left, schultz_color_rgba(255, 255, 0, 255));
    paint_it(tree, right, schultz_color_rgba(0, 255, 255, 255));
    schultz_tree_resolve_styles(tree);

    count = schultz_tree_dirty_count(tree);
    /* Two ends, two areas: this is the case the whole thing exists for. */
    ASSERT_EQ(2u, count);
    for (i = 0; i < count; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_at(tree, i, &parts[i]));
    }

    /* Only what changed, on top of what was already there. */
    schultz_arena_reset(&arena);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_widget_paint_tree_parts(tree, &list, &arena, parts,
                                              count));
    for (i = 0; i < count; i++) {
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_draw_list_play(&list, &painter, parts[i]));
    }
    schultz_thorvg_destroy(backend);

    /* The same scene painted from nothing, which is the answer to match. */
    memset(all_at_once, 0, sizeof(all_at_once));
    backend = NULL;
    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_create(all_at_once, 400u, 32u, 400u,
                                                &backend));
    schultz_thorvg_painter(backend, &painter);
    schultz_arena_reset(&arena);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_widget_paint_tree(tree, &list, &arena,
                                        schultz_rect_make(0, 0, 0, 0)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&list, &painter,
                              schultz_rect_make(0, 0, 400, 32)));
    schultz_thorvg_destroy(backend);

    /* Every pixel. A stale one anywhere is what this is looking for. */
    for (i = 0; i < 400u * 32u; i++) {
        if (part_way[i] != all_at_once[i]) {
            printf("      pixel %u,%u: partial %08X, full %08X\n",
                   i % 400u, i / 400u, part_way[i], all_at_once[i]);
        }
        ASSERT_EQ(all_at_once[i], part_way[i]);
    }

    schultz_arena_free(&arena);
    schultz_tree_destroy(tree);
    PASS();
}

/* ------------------------------------------------------------------ groups */

/*
 * Builds a faded card: an opaque page, and on it a parent carrying an opacity
 * with two opaque children that overlap. The overlap is the whole point.
 */
static void build_faded_card(schultz_tree *tree, schultz_handle *card,
                             schultz_handle *first, schultz_handle *second)
{
    schultz_handle root = schultz_tree_root(tree);
    schultz_handle page = SCHULTZ_HANDLE_NONE;

    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 120, 60));
    schultz_node_set_bounds(tree, root, schultz_rect_make(0, 0, 120, 60));

    /* Something opaque underneath, so the fade has a known thing to fade
     * against and the buffer is never composited onto itself. */
    schultz_panel_create(tree, root, &page);
    schultz_node_set_bounds(tree, page, schultz_rect_make(0, 0, 120, 60));
    paint_it(tree, page, schultz_color_rgba(0, 0, 0, 255));

    schultz_panel_create(tree, root, card);
    schultz_node_set_bounds(tree, *card, schultz_rect_make(0, 0, 120, 60));
    paint_it(tree, *card, schultz_color_rgba(0, 0, 0, 0));
    schultz_node_set_style_property(tree, *card, SCHULTZ_PROP_OPACITY,
                                    schultz_value_number(0.5f));

    schultz_panel_create(tree, *card, first);
    schultz_node_set_bounds(tree, *first, schultz_rect_make(10, 10, 60, 40));
    paint_it(tree, *first, schultz_color_rgba(255, 0, 0, 255));

    /* Overlapping the first, and painted after it, so it is on top. */
    schultz_panel_create(tree, *card, second);
    schultz_node_set_bounds(tree, *second, schultz_rect_make(40, 10, 60, 40));
    paint_it(tree, *second, schultz_color_rgba(0, 0, 255, 255));
}

/* A speck inside the overlap, for dirtying a very small area of a group. */
static schultz_handle add_speck(schultz_tree *tree, schultz_handle card)
{
    schultz_handle speck = SCHULTZ_HANDLE_NONE;

    schultz_panel_create(tree, card, &speck);
    schultz_node_set_bounds(tree, speck, schultz_rect_make(50, 25, 6, 6));
    paint_it(tree, speck, schultz_color_rgba(255, 255, 0, 160));
    return speck;
}

/*
 * The difference between fading a group and fading its pieces.
 *
 * Two opaque panels overlap inside a parent at half opacity. Faded as one
 * picture, the overlap is the top panel at half strength over the page, the
 * same as anywhere else the top panel shows. Faded piece by piece it would
 * be the top panel over an already faded bottom panel, which is a different
 * colour, and the red underneath would show through where it should not.
 */
TEST a_faded_subtree_is_composed_before_it_is_faded(void)
{
    render_fixture f;
    schultz_handle card, first, second;
    static uint32_t pixels[120 * 60];

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    build_faded_card(f.tree, &card, &first, &second);
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_render_to_buffer(f.tree, schultz_tree_root(f.tree),
                                       1.0f, &f.options, pixels, 120u, 60u,
                                       0u));

    {
        /* Blue only, over black: half of 255 blue. */
        uint32_t alone = pixel_at(pixels, 120u, 85u, 30u);
        /* Blue over red, over black. The blue is on top either way, so if
         * the group was composed first these two are the same colour. */
        uint32_t over = pixel_at(pixels, 120u, 55u, 30u);

        ASSERT_EQ(255u, alpha_of(alone));
        ASSERT_EQ(255u, alpha_of(over));

        /* Half of blue, give or take rounding. */
        ASSERT(blue_of(alone) >= 126u && blue_of(alone) <= 129u);
        ASSERT_EQ(0u, red_of(alone));

        /* And the overlap matches it. Red showing through here is the bug
         * this test exists for. */
        ASSERT_EQ(blue_of(alone), blue_of(over));
        ASSERT_EQ(0u, red_of(over));
    }

    fixture_teardown(&f);
    PASS();
}

/*
 * The two ways of writing "half" and why they are not the same.
 *
 * One opacity on the parent fades the pair once. The same opacity on each
 * child fades them one at a time, and the overlap is then a blend of both,
 * so what is underneath shows through where it should be hidden. This is the
 * comparison the demo puts side by side, so the demo's claim is checked here.
 */
TEST fading_a_parent_is_not_the_same_as_fading_each_child(void)
{
    render_fixture f;
    schultz_handle root;
    schultz_handle page = SCHULTZ_HANDLE_NONE;
    schultz_handle pair = SCHULTZ_HANDLE_NONE;
    schultz_handle one = SCHULTZ_HANDLE_NONE;
    schultz_handle two = SCHULTZ_HANDLE_NONE;
    static uint32_t together[60 * 60];
    static uint32_t apart[60 * 60];
    uint32_t i;

    for (i = 0; i < 2u; i++) {
        ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
        root = schultz_tree_root(f.tree);
        schultz_tree_set_viewport(f.tree, schultz_rect_make(0, 0, 60, 60));
        schultz_node_set_bounds(f.tree, root,
                                schultz_rect_make(0, 0, 60, 60));

        schultz_panel_create(f.tree, root, &page);
        schultz_node_set_bounds(f.tree, page,
                                schultz_rect_make(0, 0, 60, 60));
        paint_it(f.tree, page, schultz_color_rgba(255, 255, 255, 255));

        schultz_panel_create(f.tree, root, &pair);
        schultz_node_set_bounds(f.tree, pair,
                                schultz_rect_make(0, 0, 60, 60));
        paint_it(f.tree, pair, schultz_color_rgba(0, 0, 0, 0));

        schultz_panel_create(f.tree, pair, &one);
        schultz_node_set_bounds(f.tree, one,
                                schultz_rect_make(5, 20, 30, 20));
        paint_it(f.tree, one, schultz_color_rgba(0, 255, 0, 255));
        schultz_panel_create(f.tree, pair, &two);
        schultz_node_set_bounds(f.tree, two,
                                schultz_rect_make(25, 20, 30, 20));
        paint_it(f.tree, two, schultz_color_rgba(255, 0, 0, 255));

        if (i == 0u) {
            schultz_node_set_style_property(f.tree, pair,
                SCHULTZ_PROP_OPACITY, schultz_value_number(0.5f));
        } else {
            schultz_node_set_style_property(f.tree, one,
                SCHULTZ_PROP_OPACITY, schultz_value_number(0.5f));
            schultz_node_set_style_property(f.tree, two,
                SCHULTZ_PROP_OPACITY, schultz_value_number(0.5f));
        }
        schultz_tree_resolve_styles(f.tree);
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_render_to_buffer(f.tree, root, 1.0f, &f.options,
                                           (i == 0u) ? together : apart,
                                           60u, 60u, 0u));
        fixture_teardown(&f);
    }

    {
        /* Inside the overlap, where both panels are. */
        uint32_t as_one  = pixel_at(together, 60u, 30u, 30u);
        uint32_t one_by_one = pixel_at(apart, 60u, 30u, 30u);

        if (as_one == one_by_one) {
            printf("      overlap: together %08X, apart %08X\n", as_one,
                   one_by_one);
        }
        /*
         * Faded together, the overlap is the red panel at half strength over
         * the white page: full red, and none of the green panel, because the
         * red covers it before anything fades.
         */
        ASSERT_EQ(255u, red_of(as_one));
        /*
         * Faded one at a time, the red is laid over a green panel that has
         * already been faded onto the page, so the red comes out darker and
         * the green underneath has pulled the blue down with it.
         */
        ASSERT(red_of(one_by_one) < 210u);
        ASSERT(blue_of(one_by_one) < blue_of(as_one));
        ASSERT(as_one != one_by_one);
    }
    PASS();
}

/*
 * Groups nest, and the fades multiply.
 *
 * A half faded thing inside a half faded thing is a quarter. Anything else
 * would mean the inner group escaped its parent's fade, or replaced it.
 */
TEST a_group_inside_a_group_fades_twice(void)
{
    render_fixture f;
    schultz_handle root;
    schultz_handle page = SCHULTZ_HANDLE_NONE;
    schultz_handle card = SCHULTZ_HANDLE_NONE;
    schultz_handle inner = SCHULTZ_HANDLE_NONE;
    static uint32_t pixels[60 * 40];

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    root = schultz_tree_root(f.tree);
    schultz_tree_set_viewport(f.tree, schultz_rect_make(0, 0, 60, 40));
    schultz_node_set_bounds(f.tree, root, schultz_rect_make(0, 0, 60, 40));

    schultz_panel_create(f.tree, root, &page);
    schultz_node_set_bounds(f.tree, page, schultz_rect_make(0, 0, 60, 40));
    paint_it(f.tree, page, schultz_color_rgba(0, 0, 0, 255));

    schultz_panel_create(f.tree, root, &card);
    schultz_node_set_bounds(f.tree, card, schultz_rect_make(0, 0, 60, 40));
    paint_it(f.tree, card, schultz_color_rgba(0, 0, 0, 0));
    schultz_node_set_style_property(f.tree, card, SCHULTZ_PROP_OPACITY,
                                    schultz_value_number(0.5f));

    schultz_panel_create(f.tree, card, &inner);
    schultz_node_set_bounds(f.tree, inner, schultz_rect_make(10, 10, 40, 20));
    paint_it(f.tree, inner, schultz_color_rgba(255, 0, 0, 255));
    schultz_node_set_style_property(f.tree, inner, SCHULTZ_PROP_OPACITY,
                                    schultz_value_number(0.5f));

    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_render_to_buffer(f.tree, root, 1.0f, &f.options,
                                       pixels, 60u, 40u, 0u));

    {
        uint32_t middle = pixel_at(pixels, 60u, 30u, 20u);

        ASSERT_EQ(255u, alpha_of(middle));
        /* A quarter of 255, give or take rounding at each step. */
        ASSERT(red_of(middle) >= 62u && red_of(middle) <= 66u);
    }

    fixture_teardown(&f);
    PASS();
}

/*
 * Builds a white page with one small panel on it that casts a shadow.
 * The shadow is hard edged and straight down, so where it lands is exact
 * arithmetic rather than a judgement about a soft edge.
 */
static schultz_handle build_shadowed_panel(schultz_tree *tree, float angle,
                                           float distance, float blur)
{
    schultz_handle root = schultz_tree_root(tree);
    schultz_handle page = SCHULTZ_HANDLE_NONE;
    schultz_handle card = SCHULTZ_HANDLE_NONE;

    schultz_tree_set_viewport(tree, schultz_rect_make(0, 0, 60, 60));
    schultz_node_set_bounds(tree, root, schultz_rect_make(0, 0, 60, 60));

    schultz_panel_create(tree, root, &page);
    schultz_node_set_bounds(tree, page, schultz_rect_make(0, 0, 60, 60));
    paint_it(tree, page, schultz_color_rgba(255, 255, 255, 255));

    schultz_panel_create(tree, root, &card);
    schultz_node_set_bounds(tree, card, schultz_rect_make(20, 20, 20, 20));
    paint_it(tree, card, schultz_color_rgba(255, 0, 0, 255));
    schultz_node_set_style_property(tree, card, SCHULTZ_PROP_SHADOW_COLOR,
        schultz_value_color(schultz_color_rgba(0, 0, 0, 255)));
    schultz_node_set_style_property(tree, card, SCHULTZ_PROP_SHADOW_ANGLE,
                                    schultz_value_number(angle));
    schultz_node_set_style_property(tree, card, SCHULTZ_PROP_SHADOW_DISTANCE,
                                    schultz_value_number(distance));
    schultz_node_set_style_property(tree, card, SCHULTZ_PROP_SHADOW_BLUR,
                                    schultz_value_number(blur));
    return card;
}

/*
 * Where a shadow lands, which is the thing about this that is easy to get
 * backwards. Zero is above the node and it goes round clockwise, so 180 is
 * below, 90 is to the right and 270 is to the left.
 */
TEST a_shadow_falls_the_way_the_angle_says(void)
{
    static const struct {
        float    angle;
        uint32_t x;      /* Somewhere the shadow should be. */
        uint32_t y;
        uint32_t clear_x;/* The opposite side, which should stay white. */
        uint32_t clear_y;
    } cases[] = {
        { 180.0f, 30u, 44u, 30u, 15u },   /* below, not above */
        {   0.0f, 30u, 15u, 30u, 44u },   /* above, not below */
        {  90.0f, 44u, 30u, 15u, 30u },   /* right, not left */
        { 270.0f, 15u, 30u, 44u, 30u }    /* left, not right */
    };
    uint32_t c;

    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        render_fixture f;
        static uint32_t pixels[60 * 60];
        uint32_t on, off;

        ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
        build_shadowed_panel(f.tree, cases[c].angle, 6.0f, 0.0f);
        schultz_tree_resolve_styles(f.tree);
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_render_to_buffer(f.tree, schultz_tree_root(f.tree),
                                           1.0f, &f.options, pixels, 60u,
                                           60u, 0u));

        on  = pixel_at(pixels, 60u, cases[c].x, cases[c].y);
        off = pixel_at(pixels, 60u, cases[c].clear_x, cases[c].clear_y);
        if (red_of(on) > 64u || red_of(off) < 200u) {
            printf("      angle %g: shadow side %08X, clear side %08X\n",
                   (double)cases[c].angle, on, off);
        }
        /* Dark where the shadow is. */
        ASSERT(red_of(on) <= 64u);
        /* And the page still white on the far side. */
        ASSERT(red_of(off) >= 200u);

        fixture_teardown(&f);
    }
    PASS();
}

/*
 * Distance and blur are lengths, so they scale with the screen like every
 * other length. The angle does not, being an angle.
 *
 * Rendered at twice the size, a shadow six units below has to land twelve
 * pixels below, or a shadow on a dense screen sits in the wrong place.
 */
TEST a_shadow_scales_with_the_screen(void)
{
    render_fixture f;
    static uint32_t small[60 * 60];
    static uint32_t large[120 * 120];
    uint32_t i;
    uint32_t small_edge = 0u;
    uint32_t large_edge = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    build_shadowed_panel(f.tree, 180.0f, 6.0f, 0.0f);
    schultz_tree_resolve_styles(f.tree);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_render_to_buffer(f.tree, schultz_tree_root(f.tree),
                                       1.0f, &f.options, small, 60u, 60u,
                                       0u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_render_to_buffer(f.tree, schultz_tree_root(f.tree),
                                       2.0f, &f.options, large, 120u, 120u,
                                       0u));

    /* How far down the shadow reaches, in each buffer, along its middle. */
    for (i = 0; i < 60u; i++) {
        if (red_of(pixel_at(small, 60u, 30u, i)) <= 64u) {
            small_edge = i;
        }
    }
    for (i = 0; i < 120u; i++) {
        if (red_of(pixel_at(large, 120u, 60u, i)) <= 64u) {
            large_edge = i;
        }
    }
    ASSERT(small_edge > 40u);   /* Below the panel, which ends at 40. */
    if (large_edge != small_edge * 2u) {
        printf("      shadow ends at %u of 60, and %u of 120\n",
               small_edge, large_edge);
    }
    /* Twice the scale, twice as far down, give or take a pixel of rounding. */
    ASSERT(large_edge + 1u >= small_edge * 2u);
    ASSERT(large_edge <= small_edge * 2u + 1u);

    fixture_teardown(&f);
    PASS();
}

/*
 * A node can carry both, and they do not interfere: the shadow is cast by the
 * picture and then the picture is faded, so the shadow fades with it.
 */
TEST a_node_can_be_faded_and_cast_a_shadow_at_once(void)
{
    render_fixture f;
    schultz_handle card;
    static uint32_t solid[60 * 60];
    static uint32_t faded[60 * 60];

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    card = build_shadowed_panel(f.tree, 180.0f, 6.0f, 0.0f);
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_render_to_buffer(f.tree, schultz_tree_root(f.tree),
                                       1.0f, &f.options, solid, 60u, 60u,
                                       0u));

    schultz_node_set_style_property(f.tree, card, SCHULTZ_PROP_OPACITY,
                                    schultz_value_number(0.5f));
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_render_to_buffer(f.tree, schultz_tree_root(f.tree),
                                       1.0f, &f.options, faded, 60u, 60u,
                                       0u));

    {
        uint32_t shadow_solid = pixel_at(solid, 60u, 30u, 44u);
        uint32_t shadow_faded = pixel_at(faded, 60u, 30u, 44u);

        /* Black against white, then half of it. */
        ASSERT(red_of(shadow_solid) <= 8u);
        ASSERT(red_of(shadow_faded) >= 118u && red_of(shadow_faded) <= 138u);
    }

    fixture_teardown(&f);
    PASS();
}

/*
 * An incremental frame that touches a group has to come out the same as
 * painting the whole thing from nothing.
 *
 * The dirty area here is a six pixel speck buried inside the overlap of two
 * panels, all three of them inside the faded parent. That is the smallest
 * useful case: the group is recomposed from whatever the culling left in it,
 * faded, and blended over a region an opaque page has just repainted.
 */
TEST a_partial_repaint_of_a_group_matches_a_full_one(void)
{
    schultz_tree *tree = NULL;
    schultz_thorvg *backend = NULL;
    schultz_painter painter;
    schultz_arena arena;
    schultz_draw_list list;
    schultz_handle card, first, second;
    schultz_handle speck;
    static uint32_t part_way[120 * 60];
    static uint32_t all_at_once[120 * 60];
    schultz_rect parts[SCHULTZ_TREE_DIRTY_PARTS];
    uint32_t count;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_engine_init(0));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    build_faded_card(tree, &card, &first, &second);
    speck = add_speck(tree, card);
    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 0));

    memset(part_way, 0, sizeof(part_way));
    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_create(part_way, 120u, 60u, 120u,
                                                &backend));
    schultz_thorvg_painter(backend, &painter);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_widget_paint_tree(tree, &list, &arena,
                                        schultz_rect_make(0, 0, 0, 0)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&list, &painter,
                              schultz_rect_make(0, 0, 120, 60)));

    /* A speck changes, deep inside the faded parent and inside the overlap. */
    schultz_tree_clear_dirty(tree);
    paint_it(tree, speck, schultz_color_rgba(0, 255, 255, 160));
    schultz_tree_resolve_styles(tree);

    count = schultz_tree_dirty_count(tree);
    ASSERT(count >= 1u);
    for (i = 0; i < count; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_at(tree, i, &parts[i]));
    }

    schultz_arena_reset(&arena);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_widget_paint_tree_parts(tree, &list, &arena, parts,
                                              count));
    for (i = 0; i < count; i++) {
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_draw_list_play(&list, &painter, parts[i]));
    }
    schultz_thorvg_destroy(backend);

    memset(all_at_once, 0, sizeof(all_at_once));
    backend = NULL;
    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_create(all_at_once, 120u, 60u, 120u,
                                                &backend));
    schultz_thorvg_painter(backend, &painter);
    schultz_arena_reset(&arena);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_widget_paint_tree(tree, &list, &arena,
                                        schultz_rect_make(0, 0, 0, 0)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&list, &painter,
                              schultz_rect_make(0, 0, 120, 60)));
    schultz_thorvg_destroy(backend);

    for (i = 0; i < 120u * 60u; i++) {
        if (part_way[i] != all_at_once[i]) {
            printf("      pixel %u,%u: partial %08X, full %08X\n",
                   i % 120u, i / 120u, part_way[i], all_at_once[i]);
        }
        ASSERT_EQ(all_at_once[i], part_way[i]);
    }

    schultz_arena_free(&arena);
    schultz_tree_destroy(tree);
    PASS();
}

/*
 * A shadow falls outside the node's bounds, so a frame that repaints only
 * what changed has to repaint the shadow too. Marking just the bounds leaves
 * the old shadow on screen where the new one is not.
 */
TEST a_partial_repaint_redraws_the_shadow_as_well(void)
{
    schultz_tree *tree = NULL;
    schultz_thorvg *backend = NULL;
    schultz_painter painter;
    schultz_arena arena;
    schultz_draw_list list;
    schultz_handle card;
    static uint32_t part_way[60 * 60];
    static uint32_t all_at_once[60 * 60];
    schultz_rect parts[SCHULTZ_TREE_DIRTY_PARTS];
    uint32_t count;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_engine_init(0));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    card = build_shadowed_panel(tree, 180.0f, 6.0f, 0.0f);
    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 0));

    memset(part_way, 0, sizeof(part_way));
    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_create(part_way, 60u, 60u, 60u,
                                                &backend));
    schultz_thorvg_painter(backend, &painter);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_widget_paint_tree(tree, &list, &arena,
                                        schultz_rect_make(0, 0, 0, 0)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&list, &painter,
                              schultz_rect_make(0, 0, 60, 60)));

    /* The shadow goes away. Everywhere it was has to go back to white. */
    schultz_tree_clear_dirty(tree);
    schultz_node_set_style_property(tree, card, SCHULTZ_PROP_SHADOW_COLOR,
        schultz_value_color(schultz_color_rgba(0, 0, 0, 0)));
    schultz_tree_resolve_styles(tree);

    count = schultz_tree_dirty_count(tree);
    ASSERT(count >= 1u);
    for (i = 0; i < count; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_at(tree, i, &parts[i]));
    }

    schultz_arena_reset(&arena);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_widget_paint_tree_parts(tree, &list, &arena, parts,
                                              count));
    for (i = 0; i < count; i++) {
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_draw_list_play(&list, &painter, parts[i]));
    }
    schultz_thorvg_destroy(backend);

    memset(all_at_once, 0, sizeof(all_at_once));
    backend = NULL;
    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_create(all_at_once, 60u, 60u, 60u,
                                                &backend));
    schultz_thorvg_painter(backend, &painter);
    schultz_arena_reset(&arena);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_widget_paint_tree(tree, &list, &arena,
                                        schultz_rect_make(0, 0, 0, 0)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&list, &painter,
                              schultz_rect_make(0, 0, 60, 60)));
    schultz_thorvg_destroy(backend);

    for (i = 0; i < 60u * 60u; i++) {
        if (part_way[i] != all_at_once[i]) {
            printf("      pixel %u,%u: partial %08X, full %08X\n",
                   i % 60u, i / 60u, part_way[i], all_at_once[i]);
            break;
        }
    }
    for (i = 0; i < 60u * 60u; i++) {
        ASSERT_EQ(all_at_once[i], part_way[i]);
    }

    schultz_arena_free(&arena);
    schultz_tree_destroy(tree);
    PASS();
}

/*
 * The shadow is cast by the whole subtree, so a change to a child changes the
 * shadow's shape. The frame has to repaint the shadow even though nothing
 * about the child reaches where the shadow falls.
 */
TEST changing_a_child_repaints_the_shadow_its_parent_casts(void)
{
    schultz_tree *tree = NULL;
    schultz_thorvg *backend = NULL;
    schultz_painter painter;
    schultz_arena arena;
    schultz_draw_list list;
    schultz_handle card;
    schultz_handle inner = SCHULTZ_HANDLE_NONE;
    static uint32_t part_way[60 * 60];
    static uint32_t all_at_once[60 * 60];
    schultz_rect parts[SCHULTZ_TREE_DIRTY_PARTS];
    uint32_t count;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_engine_init(0));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    /* A soft shadow, so this also checks that the reach allowed for the
     * blur is enough: a blur spreads further than the distance alone. */
    card = build_shadowed_panel(tree, 180.0f, 6.0f, 3.0f);
    /* A child that hangs below the card, so the picture the shadow is cast
     * from is taller than the card and changing the child changes it. */
    schultz_panel_create(tree, card, &inner);
    schultz_node_set_bounds(tree, inner, schultz_rect_make(4, 16, 12, 12));
    paint_it(tree, inner, schultz_color_rgba(0, 0, 255, 255));
    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 0));

    memset(part_way, 0, sizeof(part_way));
    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_create(part_way, 60u, 60u, 60u,
                                                &backend));
    schultz_thorvg_painter(backend, &painter);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    schultz_tree_resolve_styles(tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_widget_paint_tree(tree, &list, &arena,
                                        schultz_rect_make(0, 0, 0, 0)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&list, &painter,
                              schultz_rect_make(0, 0, 60, 60)));

    /* The child goes away, so the shadow it was casting must go with it. */
    schultz_tree_clear_dirty(tree);
    schultz_node_set_state(tree, inner,
        schultz_node_get_state(tree, inner)
            & ~(uint32_t)SCHULTZ_STATE_VISIBLE);
    schultz_tree_resolve_styles(tree);

    count = schultz_tree_dirty_count(tree);
    ASSERT(count >= 1u);
    for (i = 0; i < count; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_tree_dirty_at(tree, i, &parts[i]));
    }

    schultz_arena_reset(&arena);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_widget_paint_tree_parts(tree, &list, &arena, parts,
                                              count));
    for (i = 0; i < count; i++) {
        ASSERT_EQ(SCHULTZ_OK,
                  schultz_draw_list_play(&list, &painter, parts[i]));
    }
    schultz_thorvg_destroy(backend);

    memset(all_at_once, 0, sizeof(all_at_once));
    backend = NULL;
    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_create(all_at_once, 60u, 60u, 60u,
                                                &backend));
    schultz_thorvg_painter(backend, &painter);
    schultz_arena_reset(&arena);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_widget_paint_tree(tree, &list, &arena,
                                        schultz_rect_make(0, 0, 0, 0)));
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_play(&list, &painter,
                              schultz_rect_make(0, 0, 60, 60)));
    schultz_thorvg_destroy(backend);

    for (i = 0; i < 60u * 60u; i++) {
        if (part_way[i] != all_at_once[i]) {
            printf("      pixel %u,%u: partial %08X, full %08X\n",
                   i % 60u, i / 60u, part_way[i], all_at_once[i]);
            break;
        }
    }
    for (i = 0; i < 60u * 60u; i++) {
        ASSERT_EQ(all_at_once[i], part_way[i]);
    }
    (void)card;

    schultz_arena_free(&arena);
    schultz_tree_destroy(tree);
    PASS();
}

/*
 * A pop with no push. A draw list built by the toolkit never does this, but a
 * list is a public thing a host can build, so the backend answers rather than
 * walking off the bottom of its stack.
 */
TEST closing_a_group_that_was_never_opened_is_refused(void)
{
    schultz_tree *tree = NULL;
    schultz_thorvg *backend = NULL;
    schultz_painter painter;
    schultz_arena arena;
    schultz_draw_list list;
    static uint32_t pixels[16 * 16];

    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_engine_init(0));
    ASSERT_EQ(SCHULTZ_OK, schultz_tree_create(&tree));
    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 0));
    memset(pixels, 0, sizeof(pixels));
    ASSERT_EQ(SCHULTZ_OK, schultz_thorvg_create(pixels, 16u, 16u, 16u,
                                                &backend));
    schultz_thorvg_painter(backend, &painter);
    ASSERT_EQ(SCHULTZ_OK, schultz_draw_list_init(&list, &arena, 0));

    ASSERT_EQ(SCHULTZ_OK, schultz_draw_group_end(&list));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_draw_list_play(&list, &painter,
                                     schultz_rect_make(0, 0, 16, 16)));

    schultz_thorvg_destroy(backend);
    schultz_arena_free(&arena);
    schultz_tree_destroy(tree);
    PASS();
}

/*
 * A picture written out and loaded again is the same picture.
 *
 * The size checks in test_image say the header is right. This says the pixels
 * are: encode, decode through the loader the toolkit already has, render, and
 * compare against rendering the original. A wrong channel order or a flipped
 * row passes every size check and fails here.
 */
TEST one_call_gives_the_same_picture_as_the_three_it_replaces(void)
{
    render_fixture f;
    schultz_handle panel;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t *pixels;
    const void *bytes = NULL;
    uint64_t length = 0u;
    void *apart;
    size_t apart_length;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 20, 14));
    schultz_node_set_style_property(f.tree, panel, SCHULTZ_PROP_BACKGROUND,
        schultz_value_color(schultz_color_rgba(30, 140, 210, 255)));
    schultz_tree_resolve_styles(f.tree);

    /* The long way, which is what this call is a shorthand for. */
    ASSERT_EQ(SCHULTZ_OK, schultz_render_size(f.tree, panel, 2.0f, &width,
                                              &height));
    pixels = (uint32_t *)malloc((size_t)width * height * sizeof(*pixels));
    ASSERT(pixels != NULL);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_render_to_buffer(f.tree, panel, 2.0f, &f.options,
                                       pixels, width, height, 0u));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_encode(pixels, width, height, 0u,
                                   SCHULTZ_IMAGE_PNG, &bytes, &length));
    /*
     * Kept, because the encoder hands back one buffer and reuses it. Without
     * this copy the comparison below is a buffer against itself, which
     * passes whatever the one call did.
     */
    apart_length = (size_t)length;
    apart = malloc(apart_length);
    ASSERT(apart != NULL);
    memcpy(apart, bytes, apart_length);
    free(pixels);

    /* The short way. */
    bytes = NULL;
    length = 0u;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_render_encode(f.tree, panel, 2.0f, &f.options,
                                    SCHULTZ_IMAGE_PNG, &bytes, &length));
    ASSERT_EQ(apart_length, (size_t)length);
    ASSERT_EQ(0, memcmp(apart, bytes, apart_length));

    /* BMP as well, so the format argument is not being ignored. */
    bytes = NULL;
    length = 0u;
    ASSERT_EQ(SCHULTZ_OK,
              schultz_render_encode(f.tree, panel, 2.0f, &f.options,
                                    SCHULTZ_IMAGE_BMP, &bytes, &length));
    ASSERT(length > 0u);
    ASSERT(apart_length != (size_t)length);

    free(apart);
    fixture_teardown(&f);
    PASS();
}

TEST rendering_to_a_picture_refuses_a_size_before_it_draws_it(void)
{
    render_fixture f;
    schultz_handle panel;
    const void *bytes = NULL;
    uint64_t length = 0u;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_node_set_bounds(f.tree, panel, schultz_rect_make(0, 0, 20, 14));
    schultz_tree_resolve_styles(f.tree);

    /*
     * A scale the caller chose that asks for more than the writers can
     * describe. Refused, and refused without allocating it: a machine that
     * tried would be swapping rather than failing this test.
     */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_render_encode(f.tree, panel, 100000.0f, &f.options,
                                    SCHULTZ_IMAGE_PNG, &bytes, &length));
    ASSERT(bytes == NULL);
    ASSERT_EQ(0u, (unsigned)length);

    /* And the ordinary refusals. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_render_encode(f.tree, panel, 0.0f, &f.options,
                                    SCHULTZ_IMAGE_PNG, &bytes, &length));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_render_encode(f.tree, panel, 1.0f, NULL,
                                    SCHULTZ_IMAGE_PNG, &bytes, &length));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_render_encode(f.tree, panel, 1.0f, &f.options,
                                    999u, &bytes, &length));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_render_encode(f.tree, (schultz_handle)999, 1.0f,
                                    &f.options, SCHULTZ_IMAGE_PNG, &bytes,
                                    &length));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_render_encode(f.tree, panel, 1.0f, &f.options,
                                    SCHULTZ_IMAGE_PNG, NULL, &length));

    fixture_teardown(&f);
    PASS();
}

/*
 * The two things a group buys, and one rule about leaving one open.
 *
 * Both are about several drawn pieces being one thing to the eye: they cast
 * one shadow between them, and they fade as one picture rather than each
 * fading separately and darkening wherever they overlap.
 */
TEST a_group_casts_one_shadow_for_everything_inside_it(void)
{
    render_fixture f;
    schultz_handle canvas;
    static uint32_t plain[60 * 60];
    static uint32_t shaded[60 * 60];
    schultz_paint fill;
    schultz_shadow shadow;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 60.0f, 60.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);
    fill = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));

    /* Two squares, touching, well away from the edges. */
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas,
                  schultz_rect_make(10, 10, 20, 20), fill, 0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas,
                  schultz_rect_make(30, 10, 20, 20), fill, 0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, plain, 60u, 60u, 0u));

    /* Nothing below them, because a canvas had no shadow of any kind. */
    ASSERT_EQ(0u, alpha_of(pixel_at(plain, 60u, 30u, 42u)));

    /* The same two squares, in a group that casts a shadow straight down. */
    shadow.color    = schultz_color_rgba(0, 0, 0, 255);
    shadow.angle    = 180.0f;
    shadow.distance = 10.0f;
    shadow.blur     = 0.0f;
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_group_begin(f.tree, canvas, 1.0f,
                                                    shadow));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas,
                  schultz_rect_make(10, 10, 20, 20), fill, 0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas,
                  schultz_rect_make(30, 10, 20, 20), fill, 0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_group_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, shaded, 60u, 60u, 0u));

    /*
     * Ten below the squares there is now something, and it runs across both
     * of them, which is what one shadow of the pair looks like. The seam
     * between them at x=30 is shadowed too, because the picture that cast it
     * had no seam.
     */
    ASSERT(alpha_of(pixel_at(shaded, 60u, 15u, 35u)) > 0u);
    ASSERT(alpha_of(pixel_at(shaded, 60u, 30u, 35u)) > 0u);
    ASSERT(alpha_of(pixel_at(shaded, 60u, 45u, 35u)) > 0u);

    fixture_teardown(&f);
    PASS();
}

TEST a_group_fades_as_one_picture_rather_than_piece_by_piece(void)
{
    render_fixture f;
    schultz_handle canvas;
    static uint32_t grouped[40 * 40];
    static uint32_t apart[40 * 40];
    schultz_paint solid;
    schultz_paint half;
    uint32_t alone;
    uint32_t overlap;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 40.0f, 40.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);
    solid = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));
    half  = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 128));

    /* Two overlapping squares, opaque, faded as one group. */
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_group_begin(f.tree, canvas, 0.5f,
                                                    schultz_shadow_none()));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas,
                  schultz_rect_make(5, 5, 20, 20), solid, 0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas,
                  schultz_rect_make(15, 5, 20, 20), solid, 0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_group_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, grouped, 40u, 40u, 0u));

    /*
     * The overlap is no darker than either square on its own. That is the
     * whole point: one picture, blended once.
     */
    alone   = alpha_of(pixel_at(grouped, 40u, 8u, 15u));
    overlap = alpha_of(pixel_at(grouped, 40u, 20u, 15u));
    /*
     * Faded at all, first. Without this the comparison below passes when the
     * opacity is ignored entirely: two solid squares are equally solid where
     * they overlap, so equal alone proves nothing on its own.
     */
    ASSERT(alone > 0u);
    ASSERT(alone < 255u);
    ASSERT_EQ(alone, overlap);

    /* The same two squares faded one at a time, which is the old way. */
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas,
                  schultz_rect_make(5, 5, 20, 20), half, 0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas,
                  schultz_rect_make(15, 5, 20, 20), half, 0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, apart, 40u, 40u, 0u));

    /* And there the overlap is darker, which is the defect the group fixes. */
    ASSERT(alpha_of(pixel_at(apart, 40u, 20u, 15u)) >
           alpha_of(pixel_at(apart, 40u, 8u, 15u)));

    fixture_teardown(&f);
    PASS();
}

TEST a_group_left_open_is_closed_when_the_drawing_ends(void)
{
    render_fixture f;
    schultz_handle canvas;
    static uint32_t pixels[60 * 60];
    schultz_paint fill;
    schultz_shadow shadow;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 60.0f, 60.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);
    fill = schultz_paint_solid(schultz_color_rgba(0, 0, 0, 255));
    shadow.color    = schultz_color_rgba(0, 0, 0, 255);
    shadow.angle    = 180.0f;
    shadow.distance = 10.0f;
    shadow.blur     = 0.0f;

    /* Pushed and never popped, the way a caller that returned early leaves it. */
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_group_begin(f.tree, canvas, 1.0f,
                                                    shadow));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_fill_rect(f.tree, canvas,
                  schultz_rect_make(10, 10, 20, 20), fill, 0.0f));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_render_to_buffer(f.tree, canvas, 1.0f,
                              &f.options, pixels, 60u, 60u, 0u));

    /* The square is there, and so is its shadow: the group was closed for it. */
    ASSERT(alpha_of(pixel_at(pixels, 60u, 15u, 15u)) > 0u);
    ASSERT(alpha_of(pixel_at(pixels, 60u, 15u, 35u)) > 0u);

    /* And the next drawing starts level rather than one group deep. */
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_canvas_group_end(f.tree, canvas));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));

    fixture_teardown(&f);
    PASS();
}

TEST grouping_refuses_what_it_should(void)
{
    render_fixture f;
    schultz_handle canvas;
    schultz_handle panel = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    canvas = bare_canvas(&f, 40.0f, 40.0f);
    ASSERT(canvas != SCHULTZ_HANDLE_NONE);
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);

    /* Not recording: begin has not been called. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_group_begin(f.tree, canvas, 1.0f,
                                        schultz_shadow_none()));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_group_end(f.tree, canvas));

    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_begin(f.tree, canvas));
    /* A pop with no push, said here rather than left to the renderer. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_canvas_group_end(f.tree, canvas));
    /* Not a canvas at all. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_canvas_group_begin(f.tree, panel, 1.0f,
                                        schultz_shadow_none()));
    ASSERT_EQ(SCHULTZ_OK, schultz_canvas_end(f.tree, canvas));

    fixture_teardown(&f);
    PASS();
}

TEST a_png_written_out_and_loaded_again_draws_the_same(void)
{
    render_fixture f;
    uint32_t source[16 * 12];
    static uint32_t first[16 * 12];
    static uint32_t again[16 * 12];
    const void *bytes = NULL;
    uint64_t length = 0u;
    schultz_handle original = SCHULTZ_HANDLE_NONE;
    schultz_handle reloaded = SCHULTZ_HANDLE_NONE;
    schultz_handle icon = SCHULTZ_HANDLE_NONE;
    uint32_t i;

    /* Something with a different colour in every corner, so a flip or a
     * channel swap cannot come out looking the same. */
    for (i = 0; i < 16u * 12u; i++) {
        uint32_t x = i % 16u;
        uint32_t y = i / 16u;

        source[i] = 0xFF000000u | (x * 16u) << 16 | (y * 20u) << 8 | 0x40u;
    }

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_set_pixels(f.images, source, 16u, 12u, 0u,
                                       &original));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_encode(source, 16u, 12u, 0u, SCHULTZ_IMAGE_PNG,
                                   &bytes, &length));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_load_data(f.images, bytes, (uint32_t)length,
                                      "png", &reloaded));

    ASSERT_EQ(SCHULTZ_OK,
              schultz_icon_create(f.tree, schultz_tree_root(f.tree), original,
                                  &icon));
    schultz_node_set_bounds(f.tree, icon, schultz_rect_make(0, 0, 16, 12));
    schultz_icon_set_fit(f.tree, icon, SCHULTZ_FIT_FILL);
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_render_to_buffer(f.tree, icon, 1.0f, &f.options, first,
                                       16u, 12u, 0u));

    ASSERT_EQ(SCHULTZ_OK, schultz_icon_set_image(f.tree, icon, reloaded));
    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_render_to_buffer(f.tree, icon, 1.0f, &f.options, again,
                                       16u, 12u, 0u));

    for (i = 0; i < 16u * 12u; i++) {
        if (first[i] != again[i]) {
            printf("      pixel %u,%u: original %08X, reloaded %08X\n",
                   i % 16u, i / 16u, first[i], again[i]);
            break;
        }
    }
    for (i = 0; i < 16u * 12u; i++) {
        ASSERT_EQ(first[i], again[i]);
    }

    fixture_teardown(&f);
    PASS();
}

SUITE(render)
{
    RUN_TEST(the_rasterizer_can_be_pointed_at_a_new_buffer);
    RUN_TEST(two_changes_far_apart_stay_two_areas);
    RUN_TEST(two_changes_close_together_become_one_area);
    RUN_TEST(more_changes_than_slots_still_cover_everything);
    RUN_TEST(a_partial_repaint_matches_a_full_one);
    RUN_TEST(a_faded_subtree_is_composed_before_it_is_faded);
    RUN_TEST(fading_a_parent_is_not_the_same_as_fading_each_child);
    RUN_TEST(a_group_inside_a_group_fades_twice);
    RUN_TEST(a_shadow_falls_the_way_the_angle_says);
    RUN_TEST(a_shadow_scales_with_the_screen);
    RUN_TEST(closing_a_group_that_was_never_opened_is_refused);
    RUN_TEST(a_node_can_be_faded_and_cast_a_shadow_at_once);
    RUN_TEST(a_partial_repaint_redraws_the_shadow_as_well);
    RUN_TEST(changing_a_child_repaints_the_shadow_its_parent_casts);
    RUN_TEST(a_partial_repaint_of_a_group_matches_a_full_one);
    RUN_TEST(a_node_reports_the_buffer_it_needs);
    RUN_TEST(a_panel_comes_out_the_colour_it_was_given);
    RUN_TEST(rendering_one_widget_leaves_out_everything_around_it);
    RUN_TEST(the_background_shows_where_nothing_was_drawn);
    RUN_TEST(rendering_at_a_larger_scale_makes_a_larger_picture);
    RUN_TEST(text_is_rasterized_at_the_scale_it_is_rendered_at);
    RUN_TEST(an_image_renders_into_the_buffer);
    RUN_TEST(a_still_picture_stretches_when_it_is_told_to);
    RUN_TEST(a_render_can_become_an_image);
    RUN_TEST(a_group_casts_one_shadow_for_everything_inside_it);
    RUN_TEST(a_group_fades_as_one_picture_rather_than_piece_by_piece);
    RUN_TEST(a_group_left_open_is_closed_when_the_drawing_ends);
    RUN_TEST(grouping_refuses_what_it_should);
    RUN_TEST(a_png_written_out_and_loaded_again_draws_the_same);
    RUN_TEST(one_call_gives_the_same_picture_as_the_three_it_replaces);
    RUN_TEST(rendering_to_a_picture_refuses_a_size_before_it_draws_it);
    RUN_TEST(rendering_refuses_what_it_cannot_do);
    RUN_TEST(a_self_crossing_path_leaves_a_hole_under_the_even_odd_rule);
    RUN_TEST(a_dash_offset_moves_where_the_first_gap_falls);
    RUN_TEST(text_filled_with_a_gradient_changes_colour_across_the_run);
    RUN_TEST(an_arc_of_a_full_turn_covers_the_same_pixels_as_a_circle);
    RUN_TEST(a_pie_closes_through_its_centre_and_a_chord_does_not);
    RUN_TEST(an_open_arc_is_a_line_rather_than_a_shape);
    RUN_TEST(an_arc_refuses_what_it_cannot_draw);
    RUN_TEST(a_square_turned_a_quarter_covers_the_same_pixels);
    RUN_TEST(text_turned_a_quarter_reads_down_the_side);
    RUN_TEST(a_rotated_run_stays_inside_the_canvas_that_clips_it);
    RUN_TEST(text_can_be_stroked_instead_of_filled);
    RUN_TEST(a_dashed_border_honours_an_offset_from_style);
    RUN_TEST(a_decoded_frame_reaches_the_buffer_the_right_colour);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(render);
    GREATEST_MAIN_END();
}
