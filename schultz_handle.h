/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_handle.h
 * @brief Handle table: opaque 64 bit identity with stale handle detection.
 *
 * Internal header. Not part of the public ABI, so the struct is visible here
 * and callers may place a table on the stack or embed it in another struct.
 *
 * The table maps opaque 64 bit handles to void pointers. Each slot carries a
 * generation counter that is bumped when the slot is released, so a handle to
 * a released object is rejected rather than resolving to whatever now
 * occupies the slot. That check is what makes use after free a returned error
 * instead of memory corruption, which matters when two language runtimes are
 * calling in.
 *
 * **Not part of the host facing interface.** How the toolkit identifies
 * objects. A host binds to schultz_api.h; this header is the toolkit's own
 * and may change without notice.
 */

#ifndef SCHULTZ_HANDLE_H
#define SCHULTZ_HANDLE_H

#include "schultz.h"

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


/** @brief One entry in a handle table. */
typedef struct {
    void    *object;     /**< User pointer, valid only while live is nonzero. */
    uint32_t generation; /**< Bumped on release; never zero. */
    uint32_t next_free;  /**< Next free slot index, or UINT32_MAX. */
    uint32_t live;       /**< Nonzero while the slot holds an object. */
} schultz_handle_slot;

/**
 * @brief A table of handle slots with a free list.
 *
 * Zero initialize or call schultz_handle_table_init before use.
 */
typedef struct {
    schultz_handle_slot *slots;     /**< Slot array, capacity entries long. */
    uint32_t             capacity;  /**< Slots currently allocated. */
    uint32_t             count;     /**< Slots currently live. */
    uint32_t             free_head; /**< First free slot, or UINT32_MAX. */
} schultz_handle_table;

/**
 * @brief Prepares a table for use.
 *
 * @param table            The table to initialize. Must not be NULL. Its
 *                         previous contents are overwritten, so call
 *                         schultz_handle_table_free first if it was in use.
 * @param initial_capacity Slots to allocate up front. Pass 0 to accept the
 *                         default. The table grows on demand regardless.
 * @return SCHULTZ_OK on success, SCHULTZ_ERR_INVALID_ARGUMENT when table is
 *         NULL or initial_capacity exceeds the maximum, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY when the slot array could not be
 *         allocated.
 */
int32_t schultz_handle_table_init(schultz_handle_table *table,
                                  uint32_t initial_capacity);

/**
 * @brief Releases a table's storage.
 *
 * The objects the table pointed at are not touched; ownership of those
 * belongs to the caller. The table is left safe to call again and safe to
 * pass to schultz_handle_table_init.
 *
 * @param table The table to release. NULL and an already freed table are both
 *              accepted and do nothing.
 */
void schultz_handle_table_free(schultz_handle_table *table);

/**
 * @brief Stores an object and issues a handle for it.
 *
 * Reuses a free slot when one exists, otherwise grows the table.
 *
 * @param table      The table to insert into. Must not be NULL.
 * @param object     The pointer to store. Must not be NULL, because NULL is
 *                   how a released slot is recognized. The table does not
 *                   take ownership and never dereferences it.
 * @param out_handle Receives the new handle on success. Must not be NULL.
 *                   Left untouched on failure.
 * @return SCHULTZ_OK on success, SCHULTZ_ERR_INVALID_ARGUMENT when any
 *         pointer is NULL, SCHULTZ_ERR_OUT_OF_MEMORY when growth failed, or
 *         SCHULTZ_ERR_EXHAUSTED when the index space is used up.
 */
int32_t schultz_handle_table_insert(schultz_handle_table *table, void *object,
                                    schultz_handle *out_handle);

/**
 * @brief Resolves a handle to the object it names.
 *
 * @param table      The table to look in. NULL yields
 *                   SCHULTZ_ERR_INVALID_HANDLE rather than a crash.
 * @param handle     The handle to resolve.
 * @param out_object Receives the stored pointer on success. Must not be NULL.
 *                   Left untouched on failure.
 * @return SCHULTZ_OK on success, SCHULTZ_ERR_INVALID_ARGUMENT when out_object
 *         is NULL, or SCHULTZ_ERR_INVALID_HANDLE when the handle is zero, out
 *         of range, names a free slot, or carries a stale generation.
 */
int32_t schultz_handle_table_lookup(const schultz_handle_table *table,
                                    schultz_handle handle, void **out_object);

/**
 * @brief Releases the slot a handle names.
 *
 * Bumps the slot's generation, which invalidates this handle and every copy
 * of it. The slot is returned to the free list for reuse. The object itself
 * is not freed; that is the caller's business.
 *
 * @param table  The table to remove from. NULL yields
 *               SCHULTZ_ERR_INVALID_HANDLE rather than a crash.
 * @param handle The handle to release.
 * @return SCHULTZ_OK on success, or SCHULTZ_ERR_INVALID_HANDLE when the
 *         handle is not currently valid, which includes releasing twice.
 */
int32_t schultz_handle_table_remove(schultz_handle_table *table,
                                    schultz_handle handle);

/**
 * @brief Counts live handles.
 *
 * @param table The table to query. NULL yields 0.
 * @return The number of handles currently outstanding.
 */
uint32_t schultz_handle_table_count(const schultz_handle_table *table);

/**
 * @brief Reports allocated slot count.
 *
 * @param table The table to query. NULL yields 0.
 * @return The number of slots allocated, live and free together.
 */
uint32_t schultz_handle_table_capacity(const schultz_handle_table *table);

/**
 * @brief Packs an index and generation into a handle.
 *
 * Exposed for tests and debug tooling. Ordinary code receives handles from
 * schultz_handle_table_insert instead.
 *
 * @param index      Slot index, in the low 32 bits of the result.
 * @param generation Generation counter, in the high 32 bits of the result.
 * @return The packed handle. Note that a generation of zero produces a handle
 *         that can never be valid.
 */
schultz_handle schultz_handle_make(uint32_t index, uint32_t generation);

/**
 * @brief Extracts the slot index from a handle.
 *
 * @param handle The handle to unpack.
 * @return The low 32 bits, which is the slot index. Not validated against any
 *         table.
 */
uint32_t schultz_handle_index(schultz_handle handle);

/**
 * @brief Extracts the generation counter from a handle.
 *
 * @param handle The handle to unpack.
 * @return The high 32 bits, which is the generation. Not validated against
 *         any table.
 */
uint32_t schultz_handle_generation(schultz_handle handle);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_HANDLE_H */
