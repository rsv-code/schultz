/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * schultz_image.c - the table of decoded images.
 *
 * Every entry is a ThorVG picture, decoded once when it is loaded and kept
 * until the table is destroyed. Drawing hands the canvas a copy, because a
 * canvas owns what is added to it and drops everything at the end of a frame.
 */

#include "schultz_image_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* For writing pictures out. It is already here for decoding them. */
#include <png.h>

#include "schultz_handle.h"

/*
 * The largest picture this will take, per side.
 *
 * A width and a height are each a uint32_t, and a caller that passes two
 * large ones asks for a byte count that does not fit a size_t: the widest
 * pair needs sixty six bits once four bytes a pixel are counted in, and
 * 2^31 by 2^31 comes to exactly 2^64, which wraps to nothing at all. The
 * allocation then succeeds at a size nobody asked for and the copy that
 * follows writes the size that was asked for.
 *
 * Checked arithmetic alone would catch that, and this is here as well
 * because a picture 65536 on a side is already four thousand megapixels.
 * Nothing real is bigger, so a number that is says the caller is confused
 * rather than ambitious, and saying so early is kinder than failing to
 * allocate sixteen gigabytes.
 */
#define SCHULTZ_IMAGE_MAX_SIDE 65536u

/*
 * How many bytes a picture of this size takes, or zero when that is more
 * than a size_t holds.
 *
 * Zero is the refusal because a picture of no bytes is already refused
 * everywhere this is called, so it cannot be mistaken for a real answer.
 */
static size_t schultz_image_bytes(uint32_t width, uint32_t height,
                                  size_t each)
{
    size_t pixels;

    if (width == 0u || height == 0u || each == 0u) {
        return 0u;
    }
    if (width > SCHULTZ_IMAGE_MAX_SIDE || height > SCHULTZ_IMAGE_MAX_SIDE) {
        return 0u;
    }
    /* Both sides are now at most 2^16, so the product fits a size_t on any
     * machine with a 32 bit one, and the last step is the only one that can
     * still be too much. */
    pixels = (size_t)width * (size_t)height;
    if (pixels > SIZE_MAX / each) {
        return 0u;
    }
    return pixels * each;
}


/** @brief One loaded image: the decoded picture and the size it came out at. */
typedef struct {
    Tvg_Paint     picture;   /**< Owned by this entry, unless an animation
                              *   owns it instead. */
    Tvg_Animation animation; /**< Owned by this entry, or NULL for a still. */
    schultz_size  size;      /**< Natural size, in pixels. */
    uint32_t     *pixels;    /**< Owned copy, for images given as raw pixels. */
    float         frames;    /**< How many frames, or 0 for a still. */
    float         duration;  /**< How long it runs, in seconds. */
} schultz_image_entry;

/** @brief The table itself: handles pointing at entries it also owns. */
struct schultz_image_table {
    schultz_handle_table  handles;  /**< Handles, pointing into `entries`. */
    schultz_image_entry **entries;  /**< Every entry, so all can be freed. */
    uint32_t              count;    /**< How many entries there are. */
    uint32_t              capacity; /**< How many the array can hold. */
};

int32_t schultz_image_table_create(schultz_image_table **out_table)
{
    schultz_image_table *table;
    int32_t result;

    if (out_table == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    table = (schultz_image_table *)calloc(1, sizeof(*table));
    if (table == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_handle_table_init(&table->handles, 0);
    if (result != SCHULTZ_OK) {
        free(table);
        return result;
    }
    *out_table = table;
    return SCHULTZ_OK;
}

/* Releases what one entry owns. The entry itself is freed by the caller. */
static void schultz_image_entry_free(schultz_image_entry *entry)
{
    if (entry->animation != NULL) {
        /* The animation owns its picture, so releasing it releases both. */
        tvg_animation_del(entry->animation);
        entry->animation = NULL;
        entry->picture   = NULL;
    }
    if (entry->picture != NULL) {
        /*
         * The picture was never added to a canvas, so nothing else holds a
         * reference and this is what releases it.
         */
        tvg_paint_unref(entry->picture, true);
        entry->picture = NULL;
    }
    free(entry->pixels);
    entry->pixels = NULL;
}

void schultz_image_table_destroy(schultz_image_table *table)
{
    uint32_t i;

    if (table == NULL) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        schultz_image_entry_free(table->entries[i]);
        free(table->entries[i]);
    }
    free(table->entries);
    schultz_handle_table_free(&table->handles);
    free(table);
}

/* Keeps a pointer to every entry, since the handle table holds only handles. */
static int32_t schultz_image_keep(schultz_image_table *table,
                                  schultz_image_entry *entry)
{
    if (table->count == table->capacity) {
        uint32_t capacity = (table->capacity == 0u) ? 8u
                                                    : table->capacity * 2u;
        schultz_image_entry **grown = (schultz_image_entry **)realloc(
            table->entries, (size_t)capacity * sizeof(*grown));

        if (grown == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        table->entries  = grown;
        table->capacity = capacity;
    }
    table->entries[table->count++] = entry;
    return SCHULTZ_OK;
}

/* Makes an empty entry and gives it a handle, or fails having kept nothing. */
static int32_t schultz_image_add(schultz_image_table *table,
                                 schultz_image_entry **out_entry,
                                 schultz_handle *out_image)
{
    schultz_image_entry *entry = (schultz_image_entry *)calloc(1,
                                                               sizeof(*entry));
    int32_t result;

    if (entry == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_image_keep(table, entry);
    if (result == SCHULTZ_OK) {
        result = schultz_handle_table_insert(&table->handles, entry,
                                             out_image);
        if (result != SCHULTZ_OK) {
            table->count--;
        }
    }
    if (result != SCHULTZ_OK) {
        free(entry);
        return result;
    }
    *out_entry = entry;
    return SCHULTZ_OK;
}

/*
 * Takes a decoded picture and gives it a handle. On any failure the picture is
 * released here, so every caller can simply return the result.
 */
static int32_t schultz_image_adopt(schultz_image_table *table,
                                   Tvg_Paint picture, uint32_t *pixels,
                                   schultz_handle *out_image)
{
    schultz_image_entry *entry = NULL;
    float width = 0.0f;
    float height = 0.0f;
    int32_t result = schultz_image_add(table, &entry, out_image);

    if (result != SCHULTZ_OK) {
        tvg_paint_unref(picture, true);
        free(pixels);
        return result;
    }
    tvg_picture_get_size(picture, &width, &height);
    entry->picture = picture;
    entry->pixels  = pixels;
    entry->size    = schultz_size_make(width, height);
    return SCHULTZ_OK;
}

static schultz_image_entry *schultz_image_find(
    const schultz_image_table *table, schultz_handle image)
{
    void *object = NULL;

    if (table == NULL ||
        schultz_handle_table_lookup(&table->handles, image, &object)
            != SCHULTZ_OK) {
        return NULL;
    }
    return (schultz_image_entry *)object;
}

int32_t schultz_image_load_file(schultz_image_table *table, const char *path,
                                schultz_handle *out_image)
{
    Tvg_Paint picture;

    if (table == NULL || path == NULL || out_image == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    picture = tvg_picture_new();
    if (picture == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    if (tvg_picture_load(picture, path) != TVG_RESULT_SUCCESS) {
        /* Missing, unreadable, or a format no loader recognised. */
        tvg_paint_unref(picture, true);
        return SCHULTZ_ERR_UNREADABLE;
    }
    return schultz_image_adopt(table, picture, NULL, out_image);
}

int32_t schultz_image_load_data(schultz_image_table *table, const void *bytes,
                                uint32_t size, const char *kind,
                                schultz_handle *out_image)
{
    Tvg_Paint picture;

    if (table == NULL || bytes == NULL || out_image == NULL || size == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    picture = tvg_picture_new();
    if (picture == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    /*
     * Copied, so the caller's buffer need not outlive the call. The decoder
     * keeps what it needs for as long as the picture lives.
     */
    if (tvg_picture_load_data(picture, (const char *)bytes, size, kind, NULL,
                              true) != TVG_RESULT_SUCCESS) {
        /* Truncated, or not the format the caller said it was. */
        tvg_paint_unref(picture, true);
        return SCHULTZ_ERR_UNREADABLE;
    }
    return schultz_image_adopt(table, picture, NULL, out_image);
}

int32_t schultz_image_set_pixels(schultz_image_table *table,
                                 const uint32_t *pixels, uint32_t width,
                                 uint32_t height, uint32_t stride,
                                 schultz_handle *out_image)
{
    Tvg_Paint picture;
    uint32_t *packed;
    size_t bytes;
    uint32_t y;

    if (table == NULL || pixels == NULL || out_image == NULL ||
        width == 0u || height == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (stride == 0u) {
        stride = width;
    }
    if (stride < width) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    /* Packed into rows of exactly the width, which is what the loader wants. */
    bytes = schultz_image_bytes(width, height, sizeof(*packed));
    if (bytes == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    packed = (uint32_t *)malloc(bytes);
    if (packed == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    for (y = 0; y < height; y++) {
        memcpy(packed + (size_t)y * width, pixels + (size_t)y * stride,
               (size_t)width * sizeof(*packed));
    }

    picture = tvg_picture_new();
    if (picture == NULL) {
        free(packed);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    /*
     * Not copied by the loader, so the packed rows are kept alongside the
     * picture and freed with it. The layout is the window buffer's own, which
     * is what makes handing over a rendered frame free.
     */
    if (tvg_picture_load_raw(picture, packed, width, height,
                             TVG_COLORSPACE_ARGB8888, false)
            != TVG_RESULT_SUCCESS) {
        tvg_paint_unref(picture, true);
        free(packed);
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    return schultz_image_adopt(table, picture, packed, out_image);
}

int32_t schultz_image_load_animation(schultz_image_table *table,
                                     const char *path,
                                     schultz_handle *out_image)
{
    Tvg_Animation animation;
    Tvg_Paint picture;
    schultz_image_entry *entry = NULL;
    float width = 0.0f;
    float height = 0.0f;
    int32_t result;

    if (table == NULL || path == NULL || out_image == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    animation = tvg_animation_new();
    if (animation == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    /*
     * The animation makes its own picture and keeps it; loading through that
     * picture is what tells the animation how many frames there are.
     */
    picture = tvg_animation_get_picture(animation);
    if (picture == NULL ||
        tvg_picture_load(picture, path) != TVG_RESULT_SUCCESS) {
        /* Missing, unreadable, or not an animation this build can decode. */
        tvg_animation_del(animation);
        return SCHULTZ_ERR_UNREADABLE;
    }

    result = schultz_image_add(table, &entry, out_image);
    if (result != SCHULTZ_OK) {
        tvg_animation_del(animation);
        return result;
    }
    tvg_picture_get_size(picture, &width, &height);
    tvg_animation_get_total_frame(animation, &entry->frames);
    tvg_animation_get_duration(animation, &entry->duration);
    entry->animation = animation;
    entry->picture   = picture;
    entry->size      = schultz_size_make(width, height);
    return SCHULTZ_OK;
}

int32_t schultz_image_frames(const schultz_image_table *table,
                             schultz_handle image, float *out_frames,
                             float *out_duration)
{
    const schultz_image_entry *entry = schultz_image_find(table, image);

    if (entry == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (out_frames != NULL) {
        *out_frames = entry->frames;
    }
    if (out_duration != NULL) {
        *out_duration = entry->duration;
    }
    return SCHULTZ_OK;
}

int32_t schultz_image_set_frame(schultz_image_table *table,
                                schultz_handle image, float frame)
{
    schultz_image_entry *entry = schultz_image_find(table, image);

    if (entry == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    if (entry->animation == NULL) {
        /* A still picture has one frame and it is already showing. */
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (frame < 0.0f) {
        frame = 0.0f;
    }
    if (entry->frames > 0.0f && frame > entry->frames - 1.0f) {
        frame = entry->frames - 1.0f;
    }
    return (tvg_animation_set_frame(entry->animation, frame)
                == TVG_RESULT_SUCCESS) ? SCHULTZ_OK
                                       : SCHULTZ_ERR_INVALID_ARGUMENT;
}

int32_t schultz_image_size(const schultz_image_table *table,
                           schultz_handle image, schultz_size *out_size)
{
    const schultz_image_entry *entry;

    if (out_size == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    entry = schultz_image_find(table, image);
    if (entry == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    *out_size = entry->size;
    return SCHULTZ_OK;
}

int32_t schultz_image_unload(schultz_image_table *table, schultz_handle image)
{
    schultz_image_entry *entry = schultz_image_find(table, image);
    uint32_t i;

    if (entry == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_handle_table_remove(&table->handles, image);
    for (i = 0; i < table->count; i++) {
        if (table->entries[i] == entry) {
            table->entries[i] = table->entries[table->count - 1u];
            table->count--;
            break;
        }
    }
    schultz_image_entry_free(entry);
    free(entry);
    return SCHULTZ_OK;
}

uint32_t schultz_image_count(const schultz_image_table *table)
{
    return (table == NULL) ? 0u : table->count;
}

Tvg_Paint schultz_image_picture(const schultz_image_table *table,
                                schultz_handle image)
{
    const schultz_image_entry *entry = schultz_image_find(table, image);

    if (entry == NULL || entry->picture == NULL) {
        return NULL;
    }
    return tvg_paint_duplicate(entry->picture);
}

/* --------------------------------------------------------------- writing out */

/*
 * One buffer, handed out and reused.
 *
 * A clipboard asks for one format at a time and reads it before asking for
 * the next, so holding the last answer is enough and saves every caller an
 * allocation to free. The same rule the clipboard itself uses for the bytes
 * it hands back.
 */
static unsigned char *schultz_image_out = NULL;
static size_t         schultz_image_out_room = 0u;

static unsigned char *schultz_image_room(size_t want)
{
    if (want > schultz_image_out_room) {
        unsigned char *bigger = (unsigned char *)realloc(schultz_image_out,
                                                         want);

        if (bigger == NULL) {
            return NULL;
        }
        schultz_image_out      = bigger;
        schultz_image_out_room = want;
    }
    return schultz_image_out;
}

/*
 * Straight alpha from premultiplied.
 *
 * Everything in the toolkit holds colour already multiplied by its alpha,
 * because that is what compositing wants. A file format means the other
 * thing by an alpha channel, so the multiplication has to be undone or every
 * half transparent picture comes out dark.
 */
static void schultz_image_straighten(uint32_t pixel, unsigned char *out)
{
    uint32_t a = (pixel >> 24) & 0xFFu;
    uint32_t r = (pixel >> 16) & 0xFFu;
    uint32_t g = (pixel >> 8) & 0xFFu;
    uint32_t b = pixel & 0xFFu;

    if (a != 0u && a != 255u) {
        r = (r * 255u + a / 2u) / a;
        g = (g * 255u + a / 2u) / a;
        b = (b * 255u + a / 2u) / a;
        if (r > 255u) { r = 255u; }
        if (g > 255u) { g = 255u; }
        if (b > 255u) { b = 255u; }
    }
    out[0] = (unsigned char)r;
    out[1] = (unsigned char)g;
    out[2] = (unsigned char)b;
    out[3] = (unsigned char)a;
}

/* Little endian, which is what both formats are written in. */
static void schultz_image_put32(unsigned char *at, uint32_t value)
{
    at[0] = (unsigned char)(value & 0xFFu);
    at[1] = (unsigned char)((value >> 8) & 0xFFu);
    at[2] = (unsigned char)((value >> 16) & 0xFFu);
    at[3] = (unsigned char)((value >> 24) & 0xFFu);
}

static void schultz_image_put16(unsigned char *at, uint32_t value)
{
    at[0] = (unsigned char)(value & 0xFFu);
    at[1] = (unsigned char)((value >> 8) & 0xFFu);
}

/*
 * A BMP, by hand, because it is a fixed header and the rows upside down.
 *
 * Twenty four bits a pixel: no alpha, rows padded to a multiple of four
 * bytes, bottom row first. That is the shape every reader agrees on, which is
 * the only reason to produce one at all.
 */
static int32_t schultz_image_write_bmp(const uint32_t *pixels, uint32_t width,
                                       uint32_t height, uint32_t stride,
                                       const void **out_bytes,
                                       uint64_t *out_length)
{
    size_t row;
    size_t body;
    size_t total;
    unsigned char *out;
    uint32_t y;

    /*
     * Three bytes a pixel, padded out to a multiple of four, then one such
     * row per line and a header in front. Every step is checked because the
     * padding makes a row wider than the pixels it holds, so a width that
     * looked safe on its own can still overflow once it is rounded up.
     */
    row = schultz_image_bytes(width, 1u, 3u);
    if (row == 0u || row > SIZE_MAX - 3u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    row = (row + 3u) & ~(size_t)3u;
    if (row > SIZE_MAX / height) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    body = row * height;
    if (body > SIZE_MAX - (14u + 40u)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    total = 14u + 40u + body;
    out = schultz_image_room(total);

    if (out == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    memset(out, 0, total);
    out[0] = 'B';
    out[1] = 'M';
    schultz_image_put32(out + 2, (uint32_t)total);
    schultz_image_put32(out + 10, 14u + 40u);      /* where the pixels start */

    schultz_image_put32(out + 14, 40u);            /* header size */
    schultz_image_put32(out + 18, width);
    schultz_image_put32(out + 22, height);
    schultz_image_put16(out + 26, 1u);             /* one plane */
    schultz_image_put16(out + 28, 24u);            /* bits a pixel */
    schultz_image_put32(out + 34, (uint32_t)body);

    for (y = 0; y < height; y++) {
        const uint32_t *line = pixels + (size_t)(height - 1u - y) * stride;
        unsigned char *at = out + 14u + 40u + row * y;
        uint32_t x;

        for (x = 0; x < width; x++) {
            unsigned char rgba[4];

            schultz_image_straighten(line[x], rgba);
            at[x * 3u]      = rgba[2];   /* blue first */
            at[x * 3u + 1u] = rgba[1];
            at[x * 3u + 2u] = rgba[0];
        }
    }
    *out_bytes  = out;
    *out_length = (uint64_t)total;
    return SCHULTZ_OK;
}

/* Where libpng puts the bytes it produces: into the buffer above. */
typedef struct {
    size_t used;   /**< How much of the buffer is written. */
    int32_t full;  /**< Nonzero once growing it failed. */
} schultz_image_sink;

static void schultz_image_png_write(png_structp png, png_bytep data,
                                    png_size_t length)
{
    schultz_image_sink *sink = (schultz_image_sink *)png_get_io_ptr(png);
    unsigned char *out;

    if (sink->full) {
        return;
    }
    out = schultz_image_room(sink->used + length);
    if (out == NULL) {
        sink->full = 1;
        return;
    }
    memcpy(out + sink->used, data, length);
    sink->used += length;
}

static void schultz_image_png_flush(png_structp png)
{
    (void)png;
}

/*
 * A PNG, through the library that is already here to read them.
 *
 * Eight bits a channel with alpha, which is what the toolkit holds and what
 * every clipboard that takes a picture expects.
 */
static int32_t schultz_image_write_png(const uint32_t *pixels, uint32_t width,
                                       uint32_t height, uint32_t stride,
                                       const void **out_bytes,
                                       uint64_t *out_length)
{
    png_structp png;
    png_infop info;
    schultz_image_sink sink;
    unsigned char *row;
    uint32_t y;

    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    info = png_create_info_struct(png);
    if (info == NULL) {
        png_destroy_write_struct(&png, NULL);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    row = (unsigned char *)malloc(schultz_image_bytes(width, 1u, 4u));
    if (row == NULL) {
        png_destroy_write_struct(&png, &info);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    /*
     * libpng reports a failure by jumping back here rather than by returning,
     * so this is the only path an error takes out of the calls below.
     */
    if (setjmp(png_jmpbuf(png))) {
        free(row);
        png_destroy_write_struct(&png, &info);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    sink.used = 0u;
    sink.full = 0;
    png_set_write_fn(png, &sink, schultz_image_png_write,
                     schultz_image_png_flush);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    for (y = 0; y < height; y++) {
        const uint32_t *line = pixels + (size_t)y * stride;
        uint32_t x;

        for (x = 0; x < width; x++) {
            schultz_image_straighten(line[x], row + x * 4u);
        }
        png_write_row(png, row);
    }
    png_write_end(png, NULL);
    free(row);
    png_destroy_write_struct(&png, &info);

    if (sink.full) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    *out_bytes  = schultz_image_out;
    *out_length = (uint64_t)sink.used;
    return SCHULTZ_OK;
}

int32_t schultz_image_encode(const uint32_t *pixels, uint32_t width,
                             uint32_t height, uint32_t stride,
                             uint32_t format, const void **out_bytes,
                             uint64_t *out_length)
{
    if (out_bytes == NULL || out_length == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_bytes  = NULL;
    *out_length = 0u;
    if (pixels == NULL || width == 0u || height == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (stride == 0u) {
        stride = width;
    }
    if (stride < width) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * Refused here rather than left to each writer, so that a picture too
     * large to describe is the same answer whichever format was asked for.
     * libpng would refuse its own way and the BMP writer another, and a
     * caller should not have to know which it asked for to read the result.
     */
    if (schultz_image_bytes(width, height, 4u) == 0u ||
        schultz_image_bytes(stride, height, 4u) == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (format == SCHULTZ_IMAGE_PNG) {
        return schultz_image_write_png(pixels, width, height, stride,
                                       out_bytes, out_length);
    }
    if (format == SCHULTZ_IMAGE_BMP) {
        return schultz_image_write_bmp(pixels, width, height, stride,
                                       out_bytes, out_length);
    }
    return SCHULTZ_ERR_INVALID_ARGUMENT;
}
