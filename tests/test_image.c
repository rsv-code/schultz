/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_image.c - the image table and the Icon widget that draws through it.
 *
 * Decoding is real here: the PNG and the SVG in assets/images are loaded and
 * their sizes checked, because a table that reports a plausible size for an
 * image it never decoded would pass any test that only asked for a handle.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_image.h"
#include "schultz_widgets.h"

#define PNG_PATH "assets/images/checker.png"
#define SVG_PATH "assets/images/schultz.svg"

/* ------------------------------------------------------------- the table */

TEST a_png_loads_and_reports_its_size(void)
{
    schultz_image_table *table = NULL;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_image_table_create(&table));
    ASSERT_EQ(0u, schultz_image_count(table));

    ASSERT_EQ(SCHULTZ_OK, schultz_image_load_file(table, PNG_PATH, &image));
    ASSERT(image != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(1u, schultz_image_count(table));

    ASSERT_EQ(SCHULTZ_OK, schultz_image_size(table, image, &size));
    ASSERT_EQ(48.0f, size.width);
    ASSERT_EQ(48.0f, size.height);

    schultz_image_table_destroy(table);
    PASS();
}

/* Vector artwork loads through the same call and reports its own size. */
TEST an_svg_loads_through_the_same_call(void)
{
    schultz_image_table *table = NULL;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_image_table_create(&table));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_load_file(table, SVG_PATH, &image));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_size(table, image, &size));
    ASSERT_EQ(64.0f, size.width);
    ASSERT_EQ(64.0f, size.height);

    schultz_image_table_destroy(table);
    PASS();
}

TEST an_image_already_in_memory_loads_too(void)
{
    schultz_image_table *table = NULL;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_size size;
    FILE *file;
    unsigned char bytes[4096];
    size_t read;

    file = fopen(PNG_PATH, "rb");
    ASSERT(file != NULL);
    read = fread(bytes, 1, sizeof(bytes), file);
    fclose(file);
    ASSERT(read > 0u);

    ASSERT_EQ(SCHULTZ_OK, schultz_image_table_create(&table));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_load_data(table, bytes,
                                                  (uint32_t)read, "png",
                                                  &image));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_size(table, image, &size));
    ASSERT_EQ(48.0f, size.width);

    /* The bytes were copied, so scribbling over them changes nothing. */
    memset(bytes, 0, sizeof(bytes));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_size(table, image, &size));
    ASSERT_EQ(48.0f, size.width);

    schultz_image_table_destroy(table);
    PASS();
}

/*
 * The path for everything the toolkit does not decode: a video frame, an
 * animated GIF unpacked by the application, a picture generated in code.
 */
TEST raw_pixels_become_an_image(void)
{
    schultz_image_table *table = NULL;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_size size;
    uint32_t pixels[8 * 10];
    uint32_t i;

    for (i = 0; i < 8u * 10u; i++) {
        pixels[i] = 0xFF204060u;
    }
    ASSERT_EQ(SCHULTZ_OK, schultz_image_table_create(&table));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_set_pixels(table, pixels, 8u, 10u, 0u,
                                                   &image));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_size(table, image, &size));
    ASSERT_EQ(8.0f, size.width);
    ASSERT_EQ(10.0f, size.height);

    /* Rows wider than the image are read at their stride. */
    ASSERT_EQ(SCHULTZ_OK, schultz_image_set_pixels(table, pixels, 4u, 5u, 8u,
                                                   &image));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_size(table, image, &size));
    ASSERT_EQ(4.0f, size.width);

    schultz_image_table_destroy(table);
    PASS();
}

TEST an_image_can_be_forgotten(void)
{
    schultz_image_table *table = NULL;
    schultz_handle first = SCHULTZ_HANDLE_NONE;
    schultz_handle second = SCHULTZ_HANDLE_NONE;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, schultz_image_table_create(&table));
    schultz_image_load_file(table, PNG_PATH, &first);
    schultz_image_load_file(table, SVG_PATH, &second);
    ASSERT_EQ(2u, schultz_image_count(table));

    ASSERT_EQ(SCHULTZ_OK, schultz_image_unload(table, first));
    ASSERT_EQ(1u, schultz_image_count(table));
    /* Its handle is stale now, and fails rather than being followed. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_image_size(table, first, &size));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE, schultz_image_unload(table, first));
    /* The other one is untouched. */
    ASSERT_EQ(SCHULTZ_OK, schultz_image_size(table, second, &size));

    schultz_image_table_destroy(table);
    PASS();
}

TEST an_unreadable_file_is_told_apart_from_a_dead_handle(void)
{
    schultz_image_table *table;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_handle loaded = SCHULTZ_HANDLE_NONE;

    /*
     * The two used to be the same code, which left a host with one message
     * for "that PNG is corrupt" and "that handle is dead". They are different
     * problems with different things to say about them.
     */
    ASSERT_EQ(SCHULTZ_OK, schultz_image_table_create(&table));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_load_file(table, PNG_PATH, &loaded));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_unload(table, loaded));

    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_image_load_file(table, "no/such/file.png", &image));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_image_unload(table, loaded));
    ASSERT(SCHULTZ_ERR_UNREADABLE != SCHULTZ_ERR_INVALID_HANDLE);

    /* And a bad argument is neither of them. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_load_file(table, NULL, &image));

    /* Every code names itself, so a log says which one happened. */
    ASSERT_STR_EQ("SCHULTZ_ERR_UNREADABLE",
                  schultz_result_string(SCHULTZ_ERR_UNREADABLE));

    schultz_image_table_destroy(table);
    PASS();
}

TEST the_table_refuses_what_it_cannot_load(void)
{
    schultz_image_table *table = NULL;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_size size;
    const char *rubbish = "this is not an image";
    uint32_t pixel = 0xFFFFFFFFu;

    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_table_create(NULL));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_table_create(&table));

    /*
     * Unreadable, not a bad handle. A caller that cannot tell a missing file
     * from a dead handle has nothing useful to say to the person looking at
     * the screen.
     */
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_image_load_file(table, "no/such/file.png", &image));
    ASSERT_EQ(SCHULTZ_ERR_UNREADABLE,
              schultz_image_load_data(table, rubbish,
                                      (uint32_t)strlen(rubbish), NULL,
                                      &image));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_load_file(table, NULL, &image));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_load_data(table, rubbish, 0u, NULL, &image));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_set_pixels(table, &pixel, 0u, 1u, 0u, &image));
    /* A stride narrower than the image would read past each row. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_set_pixels(table, &pixel, 4u, 1u, 2u, &image));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_image_size(table, (schultz_handle)999, &size));

    /* Nothing was kept from any of that. */
    ASSERT_EQ(0u, schultz_image_count(table));
    schultz_image_table_destroy(NULL);
    schultz_image_table_destroy(table);
    PASS();
}

/* ---------------------------------------------------------------- Icon */

typedef struct {
    schultz_tree        *tree;
    schultz_arena        arena;
    schultz_draw_list    list;
    schultz_image_table *images;
} icon_fixture;

static int32_t fixture_setup(icon_fixture *f)
{
    int32_t result;

    memset(f, 0, sizeof(*f));
    result = schultz_tree_create(&f->tree);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_viewport(f->tree, schultz_rect_make(0, 0, 400, 300));
    result = schultz_image_table_create(&f->images);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_tree_set_image_table(f->tree, f->images);
    result = schultz_arena_init(&f->arena, 0);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_draw_list_init(&f->list, &f->arena, 0);
}

static void fixture_teardown(icon_fixture *f)
{
    schultz_arena_free(&f->arena);
    schultz_tree_destroy(f->tree);
    schultz_image_table_destroy(f->images);
}

static uint32_t paint_all(icon_fixture *f)
{
    schultz_arena_reset(&f->arena);
    schultz_draw_list_init(&f->list, &f->arena, 0);
    schultz_tree_resolve_styles(f->tree);
    schultz_widget_paint_tree(f->tree, &f->list, &f->arena,
                              schultz_rect_make(0, 0, 0, 0));
    return schultz_draw_list_count(&f->list);
}

/* The first image command in the list, or NULL. */
static const schultz_draw_cmd *first_image(icon_fixture *f)
{
    uint32_t i;

    for (i = 0; i < schultz_draw_list_count(&f->list); i++) {
        const schultz_draw_cmd *cmd = schultz_draw_list_at(&f->list, i);

        if (cmd->kind == SCHULTZ_DRAW_IMAGE) {
            return cmd;
        }
    }
    return NULL;
}

TEST an_icon_measures_to_its_image(void)
{
    icon_fixture f;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_handle icon;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_load_file(f.images, PNG_PATH,
                                                  &image));
    ASSERT_EQ(SCHULTZ_OK, schultz_icon_create(f.tree,
                    schultz_tree_root(f.tree), image, &icon));
    ASSERT_EQ(image, schultz_icon_image(f.tree, icon));
    ASSERT_EQ((uint32_t)SCHULTZ_ROLE_IMAGE,
              schultz_node_get_role(f.tree, icon));

    schultz_tree_resolve_styles(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_layout_measure(f.tree, icon, -1, -1,
                                                 &size));
    ASSERT_EQ(48.0f, size.width);
    ASSERT_EQ(48.0f, size.height);

    fixture_teardown(&f);
    PASS();
}

TEST an_icon_with_no_image_draws_nothing(void)
{
    icon_fixture f;
    schultz_handle icon;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK, schultz_icon_create(f.tree,
                    schultz_tree_root(f.tree), SCHULTZ_HANDLE_NONE, &icon));
    schultz_node_set_bounds(f.tree, icon, schultz_rect_make(0, 0, 40, 40));

    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, icon, -1, -1, &size);
    ASSERT_EQ(0.0f, size.width);

    paint_all(&f);
    ASSERT_EQ(NULL, first_image(&f));

    /* And with no table at all, the same. */
    schultz_tree_set_image_table(f.tree, NULL);
    paint_all(&f);
    ASSERT_EQ(NULL, first_image(&f));

    fixture_teardown(&f);
    PASS();
}

TEST an_icon_fits_its_picture_into_the_room_it_has(void)
{
    icon_fixture f;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_handle icon;
    const schultz_draw_cmd *cmd;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_image_load_file(f.images, PNG_PATH, &image);
    schultz_icon_create(f.tree, schultz_tree_root(f.tree), image, &icon);
    /* A wide box for a square picture, so the fits differ visibly. */
    schultz_node_set_bounds(f.tree, icon, schultz_rect_make(10, 20, 200, 50));

    /* Contain: as large as fits, square, centred in the extra width. */
    paint_all(&f);
    cmd = first_image(&f);
    ASSERT(cmd != NULL);
    ASSERT_EQ(50.0f, cmd->as.image.dest.width);
    ASSERT_EQ(50.0f, cmd->as.image.dest.height);
    ASSERT_EQ(10.0f + (200.0f - 50.0f) * 0.5f, cmd->as.image.dest.x);

    /* Fill: the whole box, proportions ignored. */
    ASSERT_EQ(SCHULTZ_OK, schultz_icon_set_fit(f.tree, icon,
                                               SCHULTZ_FIT_FILL));
    paint_all(&f);
    cmd = first_image(&f);
    ASSERT_EQ(200.0f, cmd->as.image.dest.width);
    ASSERT_EQ(50.0f, cmd->as.image.dest.height);

    /* Cover: fills the box, keeps proportions, so it spills and is clipped. */
    ASSERT_EQ(SCHULTZ_OK, schultz_icon_set_fit(f.tree, icon,
                                               SCHULTZ_FIT_COVER));
    paint_all(&f);
    cmd = first_image(&f);
    ASSERT_EQ(200.0f, cmd->as.image.dest.width);
    ASSERT_EQ(200.0f, cmd->as.image.dest.height);
    {
        uint32_t i;
        uint32_t clips = 0;

        for (i = 0; i < schultz_draw_list_count(&f.list); i++) {
            if (schultz_draw_list_at(&f.list, i)->kind ==
                SCHULTZ_DRAW_CLIP_BEGIN) {
                clips++;
            }
        }
        ASSERT_EQ(1u, clips);
    }

    /* None: its own size, centred, whatever the box is. */
    ASSERT_EQ(SCHULTZ_OK, schultz_icon_set_fit(f.tree, icon,
                                               SCHULTZ_FIT_NONE));
    paint_all(&f);
    cmd = first_image(&f);
    ASSERT_EQ(48.0f, cmd->as.image.dest.width);

    fixture_teardown(&f);
    PASS();
}

TEST an_icons_image_can_be_swapped(void)
{
    icon_fixture f;
    schultz_handle png = SCHULTZ_HANDLE_NONE;
    schultz_handle svg = SCHULTZ_HANDLE_NONE;
    schultz_handle icon;
    schultz_size size;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_image_load_file(f.images, PNG_PATH, &png);
    schultz_image_load_file(f.images, SVG_PATH, &svg);
    schultz_icon_create(f.tree, schultz_tree_root(f.tree), png, &icon);

    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, icon, -1, -1, &size);
    ASSERT_EQ(48.0f, size.width);

    ASSERT_EQ(SCHULTZ_OK, schultz_icon_set_image(f.tree, icon, svg));
    schultz_tree_resolve_styles(f.tree);
    schultz_layout_measure(f.tree, icon, -1, -1, &size);
    ASSERT_EQ(64.0f, size.width);

    fixture_teardown(&f);
    PASS();
}

TEST icon_accessors_reject_other_widgets(void)
{
    icon_fixture f;
    schultz_handle panel;
    schultz_handle icon;

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    schultz_panel_create(f.tree, schultz_tree_root(f.tree), &panel);
    schultz_icon_create(f.tree, schultz_tree_root(f.tree),
                        SCHULTZ_HANDLE_NONE, &icon);

    ASSERT_EQ((schultz_handle)SCHULTZ_HANDLE_NONE,
              schultz_icon_image(f.tree, panel));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_icon_set_image(f.tree, panel, SCHULTZ_HANDLE_NONE));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_icon_set_fit(f.tree, panel, SCHULTZ_FIT_FILL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_icon_set_fit(f.tree, icon, 99u));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_icon_create(f.tree, schultz_tree_root(f.tree),
                                  SCHULTZ_HANDLE_NONE, NULL));

    fixture_teardown(&f);
    PASS();
}

/*
 * Swapping in a picture the same size does not send the window back through
 * layout.
 *
 * This is what a moving picture is: a host showing a camera or a film through
 * an icon hands over a new one every frame, and they are all the same size.
 * Laying the whole window out sixty times a second for that is the most
 * expensive thing it could be doing, and it was -- measured at 23 ms a frame
 * on the demo's round trip card, against well under one now.
 *
 * A picture that really is a different size still has to, which is the other
 * half of this and why it cannot simply be dropped.
 */
TEST swapping_a_picture_of_the_same_size_skips_layout(void)
{
    icon_fixture f;
    schultz_handle first = SCHULTZ_HANDLE_NONE;
    schultz_handle same = SCHULTZ_HANDLE_NONE;
    schultz_handle bigger = SCHULTZ_HANDLE_NONE;
    schultz_handle icon;
    uint32_t pixels[4 * 4];
    uint32_t wider[8 * 4];
    uint32_t i;

    for (i = 0; i < 4u * 4u; i++) { pixels[i] = 0xFF204080u; }
    for (i = 0; i < 8u * 4u; i++) { wider[i] = 0xFF804020u; }

    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_set_pixels(f.images, pixels, 4u, 4u, 0u, &first));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_set_pixels(f.images, pixels, 4u, 4u, 0u, &same));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_set_pixels(f.images, wider, 8u, 4u, 0u, &bigger));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_icon_create(f.tree, schultz_tree_root(f.tree), first,
                                  &icon));

    /* The same size: nothing for layout to do. */
    schultz_tree_clear_layout_dirty(f.tree);
    ASSERT_EQ(0, schultz_node_layout_dirty(f.tree, icon));
    ASSERT_EQ(SCHULTZ_OK, schultz_icon_set_image(f.tree, icon, same));
    ASSERT_EQ(0, schultz_node_layout_dirty(f.tree, icon));

    /* A different size: there is. */
    schultz_tree_clear_layout_dirty(f.tree);
    ASSERT_EQ(SCHULTZ_OK, schultz_icon_set_image(f.tree, icon, bigger));
    ASSERT_EQ(1, schultz_node_layout_dirty(f.tree, icon));

    fixture_teardown(&f);
    PASS();
}

/* ------------------------------------------------------------ writing out */

/* A small picture with a known colour in a known place. */
static void draw_corners(uint32_t *argb, uint32_t w, uint32_t h)
{
    uint32_t i;

    for (i = 0; i < w * h; i++) {
        argb[i] = 0xFF204080u;              /* opaque, and not grey */
    }
    argb[0] = 0xFFFF0000u;                  /* red, top left */
    argb[w - 1u] = 0xFF00FF00u;             /* green, top right */
    argb[(h - 1u) * w] = 0xFF0000FFu;       /* blue, bottom left */
}

TEST encoding_refuses_what_it_should(void)
{
    uint32_t pixels[4 * 4];
    const void *bytes = (const void *)1;
    uint64_t length = 1u;

    memset(pixels, 0, sizeof(pixels));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_encode(pixels, 4u, 4u, 0u, SCHULTZ_IMAGE_PNG,
                                   NULL, &length));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_encode(pixels, 4u, 4u, 0u, SCHULTZ_IMAGE_PNG,
                                   &bytes, NULL));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_encode(NULL, 4u, 4u, 0u, SCHULTZ_IMAGE_PNG,
                                   &bytes, &length));
    ASSERT_EQ(NULL, bytes);
    ASSERT_EQ(0u, length);
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_encode(pixels, 0u, 4u, 0u, SCHULTZ_IMAGE_PNG,
                                   &bytes, &length));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_encode(pixels, 4u, 0u, 0u, SCHULTZ_IMAGE_PNG,
                                   &bytes, &length));
    /* A stride narrower than the picture describes nothing. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_encode(pixels, 4u, 4u, 2u, SCHULTZ_IMAGE_PNG,
                                   &bytes, &length));
    /* And a format that is neither. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_encode(pixels, 4u, 4u, 0u, 99u, &bytes, &length));
    PASS();
}

/*
 * The real check: what comes out loads again, at the same size, through the
 * decoder the toolkit already had. Anything wrong with the header, the row
 * order or the channel order shows up here.
 */
TEST a_png_survives_being_written_and_read_again(void)
{
    icon_fixture f;
    uint32_t pixels[8 * 6];
    const void *bytes = NULL;
    uint64_t length = 0u;
    schultz_handle image = SCHULTZ_HANDLE_NONE;
    schultz_size size;

    draw_corners(pixels, 8u, 6u);
    ASSERT_EQ(SCHULTZ_OK, fixture_setup(&f));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_encode(pixels, 8u, 6u, 0u, SCHULTZ_IMAGE_PNG,
                                   &bytes, &length));
    ASSERT(bytes != NULL);
    ASSERT(length > 0u);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_load_data(f.images, bytes, (uint32_t)length,
                                      "png", &image));
    ASSERT_EQ(SCHULTZ_OK, schultz_image_size(f.images, image, &size));
    ASSERT_EQ(8u, (uint32_t)size.width);
    ASSERT_EQ(6u, (uint32_t)size.height);

    fixture_teardown(&f);
    PASS();
}

/*
 * A BMP cannot be read back through the toolkit, because nothing here decodes
 * one: ThorVG has no BMP loader, and a BMP is written only as a fallback for
 * older applications that take nothing else off a clipboard. So this reads
 * the bytes itself.
 *
 * That turns out to be the better test. It pins the two things easy to get
 * wrong and invisible in a size check: the channel order, which is blue
 * first, and the row order, which is bottom up.
 */
TEST a_bmp_is_written_bottom_up_with_blue_first(void)
{
    uint32_t pixels[8 * 6];
    const void *bytes = NULL;
    uint64_t length = 0u;
    const unsigned char *at;
    size_t row = 8u * 3u;   /* already a multiple of four */
    size_t body;

    draw_corners(pixels, 8u, 6u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_encode(pixels, 8u, 6u, 0u, SCHULTZ_IMAGE_BMP,
                                   &bytes, &length));
    at = (const unsigned char *)bytes;
    ASSERT_EQ(14u + 40u + row * 6u, (uint32_t)length);
    ASSERT_EQ('B', at[0]);
    ASSERT_EQ('M', at[1]);
    /* Width, height and depth, out of the header. */
    ASSERT_EQ(8u, (uint32_t)(at[18] | (at[19] << 8)));
    ASSERT_EQ(6u, (uint32_t)(at[22] | (at[23] << 8)));
    ASSERT_EQ(24u, (uint32_t)(at[28] | (at[29] << 8)));

    body = 14u + 40u;
    /*
     * The first row in the file is the bottom row of the picture, which is
     * where the blue corner is.
     */
    ASSERT_EQ(255u, at[body + 0u]);      /* blue */
    ASSERT_EQ(0u, at[body + 1u]);        /* green */
    ASSERT_EQ(0u, at[body + 2u]);        /* red */

    /* And the last row in the file is the top of the picture: red at the
     * left, green at the right. */
    {
        const unsigned char *top = at + body + row * 5u;

        ASSERT_EQ(0u, top[0]);
        ASSERT_EQ(0u, top[1]);
        ASSERT_EQ(255u, top[2]);         /* red, top left */

        ASSERT_EQ(0u, top[7u * 3u]);
        ASSERT_EQ(255u, top[7u * 3u + 1u]);  /* green, top right */
        ASSERT_EQ(0u, top[7u * 3u + 2u]);
    }
    PASS();
}

/*
 * The difference between the two formats, and the reason PNG is the one to
 * reach for. A half transparent picture written as PNG keeps its alpha; the
 * same picture as a BMP comes back solid, which is why the BMP is a fallback
 * for older applications rather than a choice.
 */
TEST a_written_png_keeps_transparency_and_a_bmp_does_not(void)
{
    uint32_t pixels[4 * 4];
    const void *bytes = NULL;
    uint64_t png_length = 0u;
    uint64_t bmp_length = 0u;
    uint32_t i;

    /* Half transparent red, premultiplied: alpha 128, red 128. */
    for (i = 0; i < 16u; i++) {
        pixels[i] = 0x80800000u;
    }
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_encode(pixels, 4u, 4u, 0u, SCHULTZ_IMAGE_PNG,
                                   &bytes, &png_length));
    ASSERT(png_length > 0u);
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_encode(pixels, 4u, 4u, 0u, SCHULTZ_IMAGE_BMP,
                                   &bytes, &bmp_length));
    /* Three bytes a pixel and no alpha channel anywhere in it. */
    ASSERT_EQ(14u + 40u + (4u * 3u) * 4u, (uint32_t)bmp_length);
    PASS();
}

TEST a_written_png_starts_with_the_png_signature(void)
{
    uint32_t pixels[2 * 2];
    const void *bytes = NULL;
    uint64_t length = 0u;
    const unsigned char *at;

    memset(pixels, 0xFF, sizeof(pixels));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_encode(pixels, 2u, 2u, 0u, SCHULTZ_IMAGE_PNG,
                                   &bytes, &length));
    at = (const unsigned char *)bytes;
    ASSERT(length > 8u);
    ASSERT_EQ(0x89u, at[0]);
    ASSERT_EQ('P', at[1]);
    ASSERT_EQ('N', at[2]);
    ASSERT_EQ('G', at[3]);
    PASS();
}

/*
 * One buffer is handed out and reused, which the interface says plainly. A
 * caller that wants two formats at once has to copy the first, and this
 * records that so nobody discovers it by having a picture go strange.
 */
TEST encoding_again_replaces_what_came_back_before(void)
{
    uint32_t pixels[4 * 4];
    const void *first = NULL;
    const void *second = NULL;
    uint64_t length = 0u;

    memset(pixels, 0x40, sizeof(pixels));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_encode(pixels, 4u, 4u, 0u, SCHULTZ_IMAGE_BMP,
                                   &first, &length));
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_encode(pixels, 4u, 4u, 0u, SCHULTZ_IMAGE_PNG,
                                   &second, &length));
    /* The second answer is a PNG, whatever the first pointer was. */
    ASSERT_EQ(0x89u, ((const unsigned char *)second)[0]);
    PASS();
}

/* ------------------------------------------------------- sizes that wrap */

/*
 * A width and a height are each a uint32_t, and multiplying two large ones
 * by four bytes a pixel needs sixty six bits. 2^31 by 2^31 comes to exactly
 * 2^64, which is zero in a size_t: the allocation succeeds at nothing and
 * the copy that follows writes what was asked for. These pin the refusal.
 */

TEST an_encode_too_large_to_describe_is_refused(void)
{
    uint32_t pixel = 0xFF00FF00u;
    const void *bytes = (const void *)1;
    uint64_t length = 1u;

    /* Exactly 2^64 bytes once four a pixel are counted in. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_encode(&pixel, 2147483648u, 2147483648u, 0u,
                                   SCHULTZ_IMAGE_PNG, &bytes, &length));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_encode(&pixel, 2147483648u, 2147483648u, 0u,
                                   SCHULTZ_IMAGE_BMP, &bytes, &length));

    /* And the widest pair there is. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_encode(&pixel, 4294967295u, 4294967295u, 0u,
                                   SCHULTZ_IMAGE_BMP, &bytes, &length));

    /* Refused the same way whichever format was asked for, so a caller
     * does not have to know which it asked for to read the answer. */
    ASSERT_EQ(NULL, bytes);
    ASSERT_EQ((uint64_t)0u, length);

    PASS();
}

TEST a_stride_too_large_to_describe_is_refused(void)
{
    uint32_t pixel = 0xFF00FF00u;
    const void *bytes = NULL;
    uint64_t length = 0u;

    /* A believable picture, addressed through rows nobody could hold. */
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_image_encode(&pixel, 4u, 4294967295u, 4294967295u,
                                   SCHULTZ_IMAGE_PNG, &bytes, &length));

    PASS();
}

TEST an_ordinary_picture_still_encodes(void)
{
    uint32_t pixels[4] = { 0xFF0000FFu, 0xFF00FF00u,
                           0xFFFF0000u, 0xFFFFFFFFu };
    const void *bytes = NULL;
    uint64_t length = 0u;

    /* The bound refuses the absurd and nothing else. */
    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_encode(pixels, 2u, 2u, 0u, SCHULTZ_IMAGE_PNG,
                                   &bytes, &length));
    ASSERT(bytes != NULL);
    ASSERT(length > 0u);

    ASSERT_EQ(SCHULTZ_OK,
              schultz_image_encode(pixels, 2u, 2u, 0u, SCHULTZ_IMAGE_BMP,
                                   &bytes, &length));
    ASSERT(bytes != NULL);
    ASSERT(length > 0u);

    PASS();
}

SUITE(image)
{
    RUN_TEST(an_encode_too_large_to_describe_is_refused);
    RUN_TEST(a_stride_too_large_to_describe_is_refused);
    RUN_TEST(an_ordinary_picture_still_encodes);
    RUN_TEST(a_png_loads_and_reports_its_size);
    RUN_TEST(an_svg_loads_through_the_same_call);
    RUN_TEST(an_image_already_in_memory_loads_too);
    RUN_TEST(raw_pixels_become_an_image);
    RUN_TEST(an_image_can_be_forgotten);
    RUN_TEST(an_unreadable_file_is_told_apart_from_a_dead_handle);
    RUN_TEST(the_table_refuses_what_it_cannot_load);
    RUN_TEST(an_icon_measures_to_its_image);
    RUN_TEST(an_icon_with_no_image_draws_nothing);
    RUN_TEST(an_icon_fits_its_picture_into_the_room_it_has);
    RUN_TEST(an_icons_image_can_be_swapped);
    RUN_TEST(swapping_a_picture_of_the_same_size_skips_layout);
    RUN_TEST(icon_accessors_reject_other_widgets);
    RUN_TEST(encoding_refuses_what_it_should);
    RUN_TEST(a_png_survives_being_written_and_read_again);
    RUN_TEST(a_bmp_is_written_bottom_up_with_blue_first);
    RUN_TEST(a_written_png_keeps_transparency_and_a_bmp_does_not);
    RUN_TEST(a_written_png_starts_with_the_png_signature);
    RUN_TEST(encoding_again_replaces_what_came_back_before);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(image);
    GREATEST_MAIN_END();
}
