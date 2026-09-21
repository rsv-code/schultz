/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_image.h
 * @brief Decoded images, registered once and drawn by handle.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 * The table sits beside the font system for the same reason: the renderer has
 * to read it, and a renderer that knows about the widget tree would be a
 * renderer coupled to it. An application loads an image once, gets a handle,
 * and hands that handle to a widget or to a draw command.
 *
 * Decoding is not Schultz's own work. ThorVG is built with its PNG, JPEG,
 * WebP and SVG loaders, so all four load through one call and an SVG scales to
 * whatever size it is drawn at rather than being resampled.
 *
 * There is no GIF and no video. ThorVG can write a GIF and cannot read one,
 * and every practical video decoder is either copyleft or tied to one
 * platform. Both follow the same path instead: the application decodes a
 * frame however it likes and hands over raw pixels with
 * schultz_image_set_pixels.
 */

#ifndef SCHULTZ_IMAGE_H
#define SCHULTZ_IMAGE_H

#include "schultz.h"
#include "schultz_geom.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Everything declared below is part of the host facing interface, so it is
 * marked visible. The toolkit is compiled with -fvisibility=hidden, which
 * hides everything by default: that is what stops a shared library built
 * from it exporting the whole of FreeType, libpng and zlib alongside, where
 * they would meet the copies already loaded by whatever is hosting it.
 *
 * A pragma rather than an attribute on each declaration, because there are
 * some hundreds of them and one pair of lines per header says the same thing.
 */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif


/** @brief The table of decoded images. */
typedef struct schultz_image_table schultz_image_table;

/**
 * @brief Creates an empty table.
 *
 * @param out_table Receives the table. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_image_table_create(schultz_image_table **out_table);

/**
 * @brief Releases a table and every image in it.
 *
 * Handles into it are stale afterwards and fail their generation check rather
 * than being followed.
 *
 * @param table The table to release. NULL is ignored.
 */
void schultz_image_table_destroy(schultz_image_table *table);

/**
 * @brief Loads an image from a file, deciding the format from its contents.
 *
 * PNG, JPEG, WebP and SVG all load through here.
 *
 * @param table     The table to load into. Must not be NULL.
 * @param path      Path to the file. Must not be NULL.
 * @param out_image Receives the handle. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer,
 *         SCHULTZ_ERR_UNREADABLE when the file is missing, unreadable, or
 *         holds nothing this build can decode, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_image_load_file(schultz_image_table *table, const char *path,
                                schultz_handle *out_image);

/**
 * @brief Loads an image already in memory.
 *
 * The bytes are copied, so the caller's buffer need not outlive the call.
 *
 * @param table     The table to load into. Must not be NULL.
 * @param bytes     The encoded image. Must not be NULL.
 * @param size      How many bytes. Must be greater than zero.
 * @param kind      A hint at the format, such as "png" or "svg", or NULL to
 *                  let the decoder work it out from the bytes.
 * @param out_image Receives the handle. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT,
 *         SCHULTZ_ERR_UNREADABLE when the bytes could not be decoded, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_image_load_data(schultz_image_table *table, const void *bytes,
                                uint32_t size, const char *kind,
                                schultz_handle *out_image);

/**
 * @brief Takes raw pixels, already decoded by the application.
 *
 * This is the path for everything Schultz does not decode: a video frame, an
 * animated GIF the application unpacked itself, a picture generated in code.
 * Pixels are premultiplied ARGB8888, the same layout the window buffer uses,
 * and are copied.
 *
 * @param table     The table to load into. Must not be NULL.
 * @param pixels    The pixels, row major. Must not be NULL.
 * @param width     Width in pixels. Must be greater than zero.
 * @param height    Height in pixels. Must be greater than zero.
 * @param stride    Pixels per row, which may exceed the width. Zero means the
 *                  rows are packed.
 * @param out_image Receives the handle. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_image_set_pixels(schultz_image_table *table,
                                 const uint32_t *pixels, uint32_t width,
                                 uint32_t height, uint32_t stride,
                                 schultz_handle *out_image);

/** @brief The file formats pixels can be written out as. */
enum {
    /**
     * PNG. Lossless, keeps alpha, and the one format every desktop
     * clipboard takes. This is the one to use unless something needs
     * otherwise.
     */
    SCHULTZ_IMAGE_PNG = 0,
    /**
     * BMP, twenty four bits a pixel and no alpha at all.
     *
     * Only worth producing for an older application that takes nothing else.
     * Alpha is left out rather than written, because a BMP's alpha is the
     * thing readers disagree about and a wrong one looks worse than none.
     */
    SCHULTZ_IMAGE_BMP
};

/**
 * @brief Writes pixels out as a file format, in memory.
 *
 * The other direction from loading. What comes back is the bytes of a whole
 * file, ready to be put on a clipboard, sent somewhere or written to disk.
 *
 * Pixels go in premultiplied, the way everything else in the toolkit holds
 * them, and come out straight, because that is what a file format means by
 * an alpha channel.
 *
 * @param pixels     The pixels, row major, premultiplied ARGB8888. Must not
 *                   be NULL.
 * @param width      Width in pixels. Must be greater than zero.
 * @param height     Height in pixels. Must be greater than zero.
 * @param stride     Pixels per row, which may exceed the width. Zero means
 *                   the rows are packed.
 * @param format     SCHULTZ_IMAGE_PNG or SCHULTZ_IMAGE_BMP.
 * @param out_bytes  Receives the file's bytes. Must not be NULL. Owned by
 *                   the toolkit and valid until the next call to this
 *                   function, so a caller wanting two formats at once copies
 *                   the first.
 * @param out_length Receives how many bytes. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_image_encode(const uint32_t *pixels, uint32_t width,
                             uint32_t height, uint32_t stride,
                             uint32_t format, const void **out_bytes,
                             uint64_t *out_length);

/**
 * @brief Loads a Lottie animation from a file.
 *
 * Lottie is the animation format the toolkit reads, and it is a good one:
 * vector, small, and with the frame handling already compiled in. There is no
 * GIF and no video; both take the raw pixel path instead.
 *
 * The animation loads showing its first frame and stays there. Something has
 * to move it, which is what schultz_lottie_create does with the tree's clock,
 * or an application can step it by hand with schultz_image_set_frame.
 *
 * @param table     The table to load into. Must not be NULL.
 * @param path      Path to the Lottie JSON. Must not be NULL.
 * @param out_image Receives the handle. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer,
 *         SCHULTZ_ERR_UNREADABLE when it could not be read or decoded, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_image_load_animation(schultz_image_table *table,
                                     const char *path,
                                     schultz_handle *out_image);

/**
 * @brief Reads how long an animation runs.
 *
 * A still picture reports zero for both, which is how one is told apart from
 * an animation.
 *
 * @param table        The table holding it. Must not be NULL.
 * @param image        A handle from one of the load calls.
 * @param out_frames   Receives the frame count, or NULL if not wanted.
 * @param out_duration Receives the length in seconds, or NULL if not wanted.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_image_frames(const schultz_image_table *table,
                             schultz_handle image, float *out_frames,
                             float *out_duration);

/**
 * @brief Moves an animation to a frame.
 *
 * Frames are counted from zero and clamped to what there is. Fractions are
 * allowed and are what makes playback smooth rather than stepped.
 *
 * @param table The table holding it. Must not be NULL.
 * @param image A handle from schultz_image_load_animation.
 * @param frame Which frame to show.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when the image is a still
 *         picture, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_image_set_frame(schultz_image_table *table,
                                schultz_handle image, float frame);

/**
 * @brief Reads an image's natural size.
 *
 * This is what an Icon measures to when it is not given a size of its own. A
 * vector image reports the size its author drew it at, and scales cleanly from
 * there.
 *
 * @param table    The table holding it. Must not be NULL.
 * @param image    A handle from one of the load calls.
 * @param out_size Receives the size in pixels. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_image_size(const schultz_image_table *table,
                           schultz_handle image, schultz_size *out_size);

/**
 * @brief Forgets one image and releases what it decoded to.
 *
 * @param table The table holding it. Must not be NULL.
 * @param image A handle from one of the load calls.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_image_unload(schultz_image_table *table, schultz_handle image);

/**
 * @brief Counts the images a table holds.
 *
 * @param table The table to query. NULL yields 0.
 * @return How many images are loaded.
 */
uint32_t schultz_image_count(const schultz_image_table *table);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_IMAGE_H */
