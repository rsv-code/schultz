/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * schultz_arena.c - bump allocator for per frame scratch memory.
 */

#include "schultz_arena.h"

#include <stdint.h>
#include <stdlib.h>

enum {
    SCHULTZ_ARENA_DEFAULT_BLOCK_SIZE = 16 * 1024
};

static size_t schultz_arena_default_alignment(void)
{
    return _Alignof(max_align_t);
}

static int schultz_arena_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static schultz_arena_block *schultz_arena_new_block(size_t capacity)
{
    schultz_arena_block *block;

    if (capacity > SIZE_MAX - sizeof(*block)) {
        return NULL;
    }

    block = (schultz_arena_block *)malloc(sizeof(*block) + capacity);
    if (block == NULL) {
        return NULL;
    }

    block->next     = NULL;
    block->capacity = capacity;
    block->used     = 0;
    return block;
}

/*
 * Attempts to carve size bytes out of block at the requested alignment.
 * Returns NULL without modifying the block if it does not fit.
 */
static void *schultz_arena_carve(schultz_arena_block *block, size_t size,
                            size_t alignment)
{
    uintptr_t base    = (uintptr_t)block->data;
    uintptr_t cursor  = base + (uintptr_t)block->used;
    uintptr_t aligned = (cursor + (uintptr_t)alignment - 1u) &
                        ~((uintptr_t)alignment - 1u);
    size_t offset;

    if (aligned < cursor) {
        return NULL; /* alignment rounding overflowed */
    }

    offset = (size_t)(aligned - base);
    if (offset > block->capacity || size > block->capacity - offset) {
        return NULL;
    }

    block->used = offset + size;
    return (void *)aligned;
}

int32_t schultz_arena_init(schultz_arena *arena, size_t block_size)
{
    if (arena == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    arena->first      = NULL;
    arena->current    = NULL;
    arena->block_size = (block_size == 0) ? SCHULTZ_ARENA_DEFAULT_BLOCK_SIZE
                                          : block_size;
    return SCHULTZ_OK;
}

void schultz_arena_free(schultz_arena *arena)
{
    schultz_arena_block *block;

    if (arena == NULL) {
        return;
    }

    block = arena->first;
    while (block != NULL) {
        schultz_arena_block *next = block->next;
        free(block);
        block = next;
    }

    arena->first   = NULL;
    arena->current = NULL;
}

void *schultz_arena_alloc(schultz_arena *arena, size_t size, size_t alignment)
{
    schultz_arena_block *block;
    size_t capacity;

    if (arena == NULL || size == 0) {
        return NULL;
    }

    if (alignment == 0) {
        alignment = schultz_arena_default_alignment();
    } else if (!schultz_arena_is_power_of_two(alignment)) {
        return NULL;
    }

    /*
     * Try the current block, then any blocks after it. Blocks after the
     * current one exist when the arena has been reset and is filling up
     * again, which is the steady state of a frame loop.
     */
    for (block = arena->current; block != NULL; block = block->next) {
        void *memory = schultz_arena_carve(block, size, alignment);
        if (memory != NULL) {
            arena->current = block;
            return memory;
        }
    }

    /*
     * Nothing fits. Take a block large enough for this request even when the
     * request is bigger than the arena's block size, so a single large
     * allocation cannot fail while smaller ones succeed.
     */
    if (size > SIZE_MAX - alignment) {
        return NULL;
    }
    capacity = arena->block_size;
    if (size + alignment > capacity) {
        capacity = size + alignment;
    }

    block = schultz_arena_new_block(capacity);
    if (block == NULL) {
        return NULL;
    }

    if (arena->first == NULL) {
        arena->first = block;
    } else {
        schultz_arena_block *tail = arena->first;
        while (tail->next != NULL) {
            tail = tail->next;
        }
        tail->next = block;
    }
    arena->current = block;

    return schultz_arena_carve(block, size, alignment);
}

void schultz_arena_reset(schultz_arena *arena)
{
    schultz_arena_block *block;

    if (arena == NULL) {
        return;
    }

    for (block = arena->first; block != NULL; block = block->next) {
        block->used = 0;
    }
    arena->current = arena->first;
}

size_t schultz_arena_used(const schultz_arena *arena)
{
    const schultz_arena_block *block;
    size_t used = 0;

    if (arena == NULL) {
        return 0;
    }

    for (block = arena->first; block != NULL; block = block->next) {
        used += block->used;
    }
    return used;
}

size_t schultz_arena_capacity(const schultz_arena *arena)
{
    const schultz_arena_block *block;
    size_t capacity = 0;

    if (arena == NULL) {
        return 0;
    }

    for (block = arena->first; block != NULL; block = block->next) {
        capacity += block->capacity;
    }
    return capacity;
}

uint32_t schultz_arena_block_count(const schultz_arena *arena)
{
    const schultz_arena_block *block;
    uint32_t count = 0;

    if (arena == NULL) {
        return 0;
    }

    for (block = arena->first; block != NULL; block = block->next) {
        count++;
    }
    return count;
}
