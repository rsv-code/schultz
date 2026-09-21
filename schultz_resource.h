/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_resource.h
 * @brief Gradients and dash patterns, registered once and used by handle.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 * A style value is a small union: a colour, a number, a font. A gradient's
 * colour stops and a dash pattern's lengths are both too much to fit in one,
 * and both are usually shared between many nodes, so they are registered here
 * and referred to by handle, exactly as fonts and styles are.
 *
 * The table sits beside the font system rather than inside the tree because
 * the renderer has to read it, and a renderer that knows about the widget
 * tree would be a renderer coupled to it.
 */

#ifndef SCHULTZ_RESOURCE_H
#define SCHULTZ_RESOURCE_H

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


/** @brief The table of registered gradients and dash patterns. */
typedef struct schultz_resource_table schultz_resource_table;

/** @brief The largest number of colour stops one gradient may have. */
enum {
    SCHULTZ_GRADIENT_STOPS_MAX = 8
};

/** @brief The largest number of lengths one dash pattern may have. */
enum {
    SCHULTZ_DASH_MAX = 8
};

/** @brief What a gradient's colour is at one point along it. */
typedef struct {
    float         offset; /**< 0 at the start, 1 at the end. */
    schultz_color color;  /**< The colour there. */
} schultz_gradient_stop;

/** @brief Which way a gradient runs. */
enum {
    SCHULTZ_GRADIENT_LINEAR = 0, /**< Along a line, from one point to another. */
    SCHULTZ_GRADIENT_RADIAL      /**< Out from a centre. */
};

/**
 * @brief A registered gradient, as the renderer reads it.
 *
 * Geometry is in the 0 to 1 range of whatever shape is being painted, so one
 * gradient scales to a button and to a window alike.
 */
typedef struct {
    uint32_t              kind;   /**< SCHULTZ_GRADIENT_LINEAR or _RADIAL. */
    schultz_point         from;   /**< Linear: the start. Radial: the centre. */
    schultz_point         to;     /**< Linear: the end. Unused for radial. */
    float                 radius; /**< Radial: how far out, as a fraction. */
    schultz_gradient_stop stops[SCHULTZ_GRADIENT_STOPS_MAX]; /**< In order. */
    uint32_t              count;  /**< How many stops are used. */
} schultz_gradient;

/** @brief A registered dash pattern, as the renderer reads it. */
typedef struct {
    float    lengths[SCHULTZ_DASH_MAX]; /**< On, off, on, off, and so on. */
    uint32_t count;                     /**< How many lengths are used. */
} schultz_dash;

/**
 * @brief Creates an empty table.
 *
 * @param out_table Receives the table. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_resource_table_create(schultz_resource_table **out_table);

/**
 * @brief Releases a table and everything registered in it.
 *
 * @param table The table to release. NULL is ignored.
 */
void schultz_resource_table_destroy(schultz_resource_table *table);

/**
 * @brief Registers a gradient that runs along a line.
 *
 * @param table     The table to register with. Must not be NULL.
 * @param from      Where it starts, in the 0 to 1 range of the shape.
 * @param to        Where it ends, in the same range.
 * @param stops     The colour stops, in rising order of offset.
 * @param count     How many, from 2 to SCHULTZ_GRADIENT_STOPS_MAX.
 * @param out_handle Receives the handle. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad stop count, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_gradient_linear(schultz_resource_table *table,
                                schultz_point from, schultz_point to,
                                const schultz_gradient_stop *stops,
                                uint32_t count, schultz_handle *out_handle);

/**
 * @brief Registers a gradient that runs out from a centre.
 *
 * @param table      The table to register with. Must not be NULL.
 * @param centre     The middle, in the 0 to 1 range of the shape.
 * @param radius     How far out, as a fraction of the shape's larger side.
 * @param stops      The colour stops, in rising order of offset.
 * @param count      How many, from 2 to SCHULTZ_GRADIENT_STOPS_MAX.
 * @param out_handle Receives the handle. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_gradient_radial(schultz_resource_table *table,
                                schultz_point centre, float radius,
                                const schultz_gradient_stop *stops,
                                uint32_t count, schultz_handle *out_handle);

/**
 * @brief Looks up a registered gradient.
 *
 * @param table     The table to look in. NULL yields an error.
 * @param handle    A handle from schultz_gradient_linear or _radial.
 * @param out_gradient Receives a pointer into the table, valid until the
 *                  table is destroyed. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_gradient_get(const schultz_resource_table *table,
                             schultz_handle handle,
                             const schultz_gradient **out_gradient);

/**
 * @brief Registers a dash pattern.
 *
 * The lengths alternate between drawn and skipped, starting with drawn, and
 * repeat for as long as the line does.
 *
 * @param table      The table to register with. Must not be NULL.
 * @param lengths    The lengths, in pixels. All must be positive.
 * @param count      How many, from 2 to SCHULTZ_DASH_MAX.
 * @param out_handle Receives the handle. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_dash_register(schultz_resource_table *table,
                              const float *lengths, uint32_t count,
                              schultz_handle *out_handle);

/**
 * @brief Registers the common dash pattern: one mark and one gap.
 *
 * The same thing as schultz_dash_register with a two length array, without
 * the array. A pattern of more than two lengths, such as a dash dot rule,
 * still goes through schultz_dash_register.
 *
 * How far along the pattern a line starts is not set here. That is
 * `dash_offset` on the stroke, so one registered pattern serves every offset.
 *
 * @param table      The table to register with. Must not be NULL.
 * @param on         How long each mark is, in pixels. Must be positive.
 * @param off        How long each gap is, in pixels. Must be positive.
 * @param out_handle Receives the handle. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when either length is not
 *         positive, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_dash_pair(schultz_resource_table *table, float on, float off,
                          schultz_handle *out_handle);

/**
 * @brief Looks up a registered dash pattern.
 *
 * @param table    The table to look in. NULL yields an error.
 * @param handle   A handle from schultz_dash_register.
 * @param out_dash Receives a pointer into the table, valid until the table is
 *                 destroyed. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_dash_get(const schultz_resource_table *table,
                         schultz_handle handle, const schultz_dash **out_dash);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_RESOURCE_H */
