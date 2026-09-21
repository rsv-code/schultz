/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_arena.h
 * @brief Bump allocator for per frame scratch memory.
 *
 * Internal header. The layout and paint passes must not call malloc, both
 * because allocation in the middle of a frame is a latency hazard and because
 * on the C host it can trigger a collection at a bad moment. They allocate
 * from an arena instead.
 *
 * An arena hands out memory by advancing a pointer, and reclaims all of it at
 * once. Reset keeps the blocks it has already obtained, so a steady state
 * frame loop stops calling malloc entirely after the first few frames.
 *
 * Memory returned by the arena is invalidated by schultz_arena_reset. Nothing
 * that outlives a frame belongs here.
 *
 * **Not part of the host facing interface.** How the toolkit allocates per
 * frame scratch. A host binds to schultz_api.h; this header is the toolkit's
 * own and may change without notice.
 */

#ifndef SCHULTZ_ARENA_H
#define SCHULTZ_ARENA_H

#include <stddef.h>

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


/** @brief Forward declaration so a block can chain to the next one. */
typedef struct schultz_arena_block schultz_arena_block;

/** @brief One contiguous run of arena memory. */
struct schultz_arena_block {
    schultz_arena_block *next;     /**< Next block in the chain, or NULL. */
    size_t               capacity; /**< Usable bytes in data. */
    size_t               used;     /**< Bytes handed out from this block. */
    unsigned char        data[];   /**< The memory itself. */
};

/**
 * @brief A chain of blocks that allocates by bumping a cursor.
 *
 * Call schultz_arena_init before use.
 */
typedef struct {
    schultz_arena_block *first;      /**< Oldest block, head of the chain. */
    schultz_arena_block *current;    /**< Block being filled. */
    size_t               block_size; /**< Size of blocks this arena requests. */
} schultz_arena;

/**
 * @brief Prepares an arena for use.
 *
 * No memory is obtained from the system until the first allocation.
 *
 * @param arena      The arena to initialize. Must not be NULL. Its previous
 *                   contents are overwritten, so call schultz_arena_free
 *                   first if it was in use.
 * @param block_size Bytes to request per block. Pass 0 to accept the default.
 *                   A single allocation larger than this still succeeds; the
 *                   arena takes an oversized block for it.
 * @return SCHULTZ_OK on success, or SCHULTZ_ERR_INVALID_ARGUMENT when arena
 *         is NULL.
 */
int32_t schultz_arena_init(schultz_arena *arena, size_t block_size);

/**
 * @brief Releases every block back to the system.
 *
 * Every pointer the arena has returned becomes invalid.
 *
 * @param arena The arena to release. NULL and an already freed arena are both
 *              accepted and do nothing.
 */
void schultz_arena_free(schultz_arena *arena);

/**
 * @brief Allocates aligned, uninitialized memory from the arena.
 *
 * The returned memory stays valid until schultz_arena_reset or
 * schultz_arena_free is called on this arena. It must never be passed to
 * free().
 *
 * @param arena     The arena to allocate from. NULL yields NULL.
 * @param size      Bytes to allocate. Must be greater than zero.
 * @param alignment Required address alignment in bytes. Must be a power of
 *                  two. Pass 0 to accept the platform's maximum fundamental
 *                  alignment, which is correct for any ordinary object.
 * @return A pointer to at least size bytes aligned as requested, or NULL when
 *         arena is NULL, size is zero, alignment is not a power of two, or
 *         the underlying allocation failed.
 */
void *schultz_arena_alloc(schultz_arena *arena, size_t size, size_t alignment);

/**
 * @brief Makes every prior allocation available again.
 *
 * Blocks are kept rather than returned to the system, which is what lets a
 * frame loop settle and stop calling malloc. Every pointer the arena has
 * returned becomes invalid.
 *
 * @param arena The arena to reset. NULL is accepted and does nothing.
 */
void schultz_arena_reset(schultz_arena *arena);

/**
 * @brief Reports bytes handed out since the last reset.
 *
 * Includes bytes lost to alignment padding, so it may exceed the sum of the
 * sizes requested.
 *
 * @param arena The arena to query. NULL yields 0.
 * @return Bytes currently handed out across all blocks.
 */
size_t schultz_arena_used(const schultz_arena *arena);

/**
 * @brief Reports bytes currently held from the system.
 *
 * @param arena The arena to query. NULL yields 0.
 * @return The sum of every block's capacity, whether in use or not.
 */
size_t schultz_arena_capacity(const schultz_arena *arena);

/**
 * @brief Counts blocks held.
 *
 * Useful for asserting that a frame loop has settled: once this stops
 * growing, the arena has stopped calling malloc.
 *
 * @param arena The arena to query. NULL yields 0.
 * @return The number of blocks in the chain.
 */
uint32_t schultz_arena_block_count(const schultz_arena *arena);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_ARENA_H */
