/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * schultz_handle.c - handle table.
 */

#include "schultz_handle.h"

#include <stdlib.h>
#include <string.h>

enum {
    SCHULTZ_HANDLE_DEFAULT_CAPACITY = 16,
    /*
     * One index is reserved as the free list terminator, so the table can
     * hold at most UINT32_MAX - 1 slots.
     */
    SCHULTZ_HANDLE_MAX_CAPACITY = 0xFFFFFFFEu
};

schultz_handle schultz_handle_make(uint32_t index, uint32_t generation)
{
    return ((uint64_t)generation << 32) | (uint64_t)index;
}

uint32_t schultz_handle_index(schultz_handle handle)
{
    return (uint32_t)(handle & 0xFFFFFFFFu);
}

uint32_t schultz_handle_generation(schultz_handle handle)
{
    return (uint32_t)(handle >> 32);
}

/*
 * Chains slots [from, table->capacity) onto the front of the free list, in
 * ascending order so that handles are handed out low index first.
 */
static void schultz_handle_chain_free(schultz_handle_table *table, uint32_t from)
{
    uint32_t i = table->capacity;

    while (i > from) {
        i--;
        table->slots[i].object     = NULL;
        table->slots[i].generation = 1;
        table->slots[i].live       = 0;
        table->slots[i].next_free  = table->free_head;
        table->free_head           = i;
    }
}

static int32_t schultz_handle_grow(schultz_handle_table *table)
{
    uint32_t old_capacity = table->capacity;
    uint32_t new_capacity;
    schultz_handle_slot *slots;

    if (old_capacity >= SCHULTZ_HANDLE_MAX_CAPACITY) {
        return SCHULTZ_ERR_EXHAUSTED;
    }

    if (old_capacity == 0) {
        new_capacity = SCHULTZ_HANDLE_DEFAULT_CAPACITY;
    } else if (old_capacity > SCHULTZ_HANDLE_MAX_CAPACITY / 2) {
        new_capacity = SCHULTZ_HANDLE_MAX_CAPACITY;
    } else {
        new_capacity = old_capacity * 2;
    }

    slots = (schultz_handle_slot *)realloc(table->slots,
                                      (size_t)new_capacity * sizeof(*slots));
    if (slots == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    table->slots    = slots;
    table->capacity = new_capacity;
    schultz_handle_chain_free(table, old_capacity);
    return SCHULTZ_OK;
}

int32_t schultz_handle_table_init(schultz_handle_table *table, uint32_t initial_capacity)
{
    uint32_t capacity;

    if (table == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (initial_capacity > SCHULTZ_HANDLE_MAX_CAPACITY) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    capacity = (initial_capacity == 0) ? SCHULTZ_HANDLE_DEFAULT_CAPACITY
                                       : initial_capacity;

    table->slots = (schultz_handle_slot *)malloc((size_t)capacity *
                                            sizeof(*table->slots));
    if (table->slots == NULL) {
        table->capacity  = 0;
        table->count     = 0;
        table->free_head = UINT32_MAX;
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    table->capacity  = capacity;
    table->count     = 0;
    table->free_head = UINT32_MAX;
    schultz_handle_chain_free(table, 0);
    return SCHULTZ_OK;
}

void schultz_handle_table_free(schultz_handle_table *table)
{
    if (table == NULL) {
        return;
    }
    free(table->slots);
    table->slots     = NULL;
    table->capacity  = 0;
    table->count     = 0;
    table->free_head = UINT32_MAX;
}

int32_t schultz_handle_table_insert(schultz_handle_table *table, void *object,
                               schultz_handle *out_handle)
{
    schultz_handle_slot *slot;
    uint32_t index;

    if (table == NULL || object == NULL || out_handle == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    if (table->free_head == UINT32_MAX) {
        int32_t result = schultz_handle_grow(table);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }

    index            = table->free_head;
    slot             = &table->slots[index];
    table->free_head = slot->next_free;

    slot->object    = object;
    slot->live      = 1;
    slot->next_free = UINT32_MAX;
    table->count++;

    *out_handle = schultz_handle_make(index, slot->generation);
    return SCHULTZ_OK;
}

/*
 * Resolves a handle to its slot, or NULL if the handle is not currently
 * valid. Const correctness is handled by the two callers.
 */
static schultz_handle_slot *schultz_handle_resolve(const schultz_handle_table *table,
                                         schultz_handle handle)
{
    uint32_t index;
    uint32_t generation;
    schultz_handle_slot *slot;

    if (table == NULL || table->slots == NULL || handle == SCHULTZ_HANDLE_NONE) {
        return NULL;
    }

    index      = schultz_handle_index(handle);
    generation = schultz_handle_generation(handle);

    if (index >= table->capacity) {
        return NULL;
    }

    slot = &table->slots[index];
    if (slot->live == 0 || slot->generation != generation) {
        return NULL;
    }
    return slot;
}

int32_t schultz_handle_table_lookup(const schultz_handle_table *table, schultz_handle handle,
                               void **out_object)
{
    schultz_handle_slot *slot;

    if (out_object == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    slot = schultz_handle_resolve(table, handle);
    if (slot == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }

    *out_object = slot->object;
    return SCHULTZ_OK;
}

int32_t schultz_handle_table_remove(schultz_handle_table *table, schultz_handle handle)
{
    schultz_handle_slot *slot = schultz_handle_resolve(table, handle);

    if (slot == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }

    /*
     * Bumping the generation is what invalidates this handle and every copy
     * of it. Zero is skipped on wrap so that a live slot's generation is
     * never zero, which keeps SCHULTZ_HANDLE_NONE from ever naming a live object.
     */
    slot->generation++;
    if (slot->generation == 0) {
        slot->generation = 1;
    }

    slot->object     = NULL;
    slot->live       = 0;
    slot->next_free  = table->free_head;
    table->free_head = (uint32_t)(slot - table->slots);
    table->count--;
    return SCHULTZ_OK;
}

uint32_t schultz_handle_table_count(const schultz_handle_table *table)
{
    return (table == NULL) ? 0 : table->count;
}

uint32_t schultz_handle_table_capacity(const schultz_handle_table *table)
{
    return (table == NULL) ? 0 : table->capacity;
}
