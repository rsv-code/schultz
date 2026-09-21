/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * schultz_resource.c - the table of gradients and dash patterns.
 */

#include "schultz_resource.h"

#include <stdlib.h>
#include <string.h>

#include "schultz_handle.h"

/** @brief One entry, which is either a gradient or a dash pattern. */
typedef struct {
    uint32_t is_dash; /**< Nonzero when the dash member is the live one. */
    union {
        schultz_gradient gradient; /**< Valid when is_dash is zero. */
        schultz_dash     dash;     /**< Valid when is_dash is nonzero. */
    } as;             /**< One or the other, never both. */
} schultz_resource;

/** @brief The table itself: handles pointing at entries it also owns. */
struct schultz_resource_table {
    schultz_handle_table handles;  /**< Handles, pointing into `entries`. */
    schultz_resource   **entries;  /**< Every entry, so all can be freed. */
    uint32_t             count;    /**< How many entries there are. */
    uint32_t             capacity; /**< How many the array can hold. */
};

/* Keeps a pointer to every entry, since the handle table holds only handles. */
static int32_t schultz_resource_keep(schultz_resource_table *table,
                                     schultz_resource *entry)
{
    if (table->count == table->capacity) {
        uint32_t capacity = (table->capacity == 0u) ? 8u
                                                    : table->capacity * 2u;
        schultz_resource **grown = (schultz_resource **)realloc(
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

/* Allocates an entry, registers it, and hands back a handle for it. */
static int32_t schultz_resource_add(schultz_resource_table *table,
                                    schultz_resource **out_entry,
                                    schultz_handle *out_handle)
{
    schultz_resource *entry;
    int32_t result;

    if (table == NULL || out_handle == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    entry = (schultz_resource *)calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    result = schultz_resource_keep(table, entry);
    if (result != SCHULTZ_OK) {
        free(entry);
        return result;
    }
    result = schultz_handle_table_insert(&table->handles, entry, out_handle);
    if (result != SCHULTZ_OK) {
        table->count--;
        free(entry);
        return result;
    }
    *out_entry = entry;
    return SCHULTZ_OK;
}

/* Looks an entry up, checking it is the kind the caller asked for. */
static const schultz_resource *schultz_resource_find(
    const schultz_resource_table *table, schultz_handle handle,
    uint32_t is_dash)
{
    void *object = NULL;

    if (table == NULL ||
        schultz_handle_table_lookup(&table->handles, handle, &object)
            != SCHULTZ_OK) {
        return NULL;
    }
    if (((const schultz_resource *)object)->is_dash != is_dash) {
        return NULL;
    }
    return (const schultz_resource *)object;
}

int32_t schultz_resource_table_create(schultz_resource_table **out_table)
{
    schultz_resource_table *table;
    int32_t result;

    if (out_table == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    table = (schultz_resource_table *)calloc(1, sizeof(*table));
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

void schultz_resource_table_destroy(schultz_resource_table *table)
{
    uint32_t i;

    if (table == NULL) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->entries[i]);
    }
    free(table->entries);
    schultz_handle_table_free(&table->handles);
    free(table);
}

/* Copies stops in, after checking there are a sensible number of them. */
static int32_t schultz_gradient_store(schultz_resource_table *table,
                                      const schultz_gradient *definition,
                                      schultz_handle *out_handle)
{
    schultz_resource *entry = NULL;
    int32_t result = schultz_resource_add(table, &entry, out_handle);

    if (result != SCHULTZ_OK) {
        return result;
    }
    entry->is_dash     = 0u;
    entry->as.gradient = *definition;
    return SCHULTZ_OK;
}

/* Both gradient kinds share their stop handling, which is all the checking. */
static int32_t schultz_gradient_fill(schultz_gradient *definition,
                                     const schultz_gradient_stop *stops,
                                     uint32_t count)
{
    uint32_t i;

    if (stops == NULL || count < 2u || count > SCHULTZ_GRADIENT_STOPS_MAX) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < count; i++) {
        definition->stops[i] = stops[i];
    }
    definition->count = count;
    return SCHULTZ_OK;
}

int32_t schultz_gradient_linear(schultz_resource_table *table,
                                schultz_point from, schultz_point to,
                                const schultz_gradient_stop *stops,
                                uint32_t count, schultz_handle *out_handle)
{
    schultz_gradient definition;
    int32_t result;

    memset(&definition, 0, sizeof(definition));
    definition.kind = SCHULTZ_GRADIENT_LINEAR;
    definition.from = from;
    definition.to   = to;
    result = schultz_gradient_fill(&definition, stops, count);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_gradient_store(table, &definition, out_handle);
}

int32_t schultz_gradient_radial(schultz_resource_table *table,
                                schultz_point centre, float radius,
                                const schultz_gradient_stop *stops,
                                uint32_t count, schultz_handle *out_handle)
{
    schultz_gradient definition;
    int32_t result;

    if (radius <= 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    memset(&definition, 0, sizeof(definition));
    definition.kind   = SCHULTZ_GRADIENT_RADIAL;
    definition.from   = centre;
    definition.radius = radius;
    result = schultz_gradient_fill(&definition, stops, count);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_gradient_store(table, &definition, out_handle);
}

int32_t schultz_gradient_get(const schultz_resource_table *table,
                             schultz_handle handle,
                             const schultz_gradient **out_gradient)
{
    const schultz_resource *entry;

    if (out_gradient == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    entry = schultz_resource_find(table, handle, 0u);
    if (entry == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    *out_gradient = &entry->as.gradient;
    return SCHULTZ_OK;
}

int32_t schultz_dash_register(schultz_resource_table *table,
                              const float *lengths, uint32_t count,
                              schultz_handle *out_handle)
{
    schultz_resource *entry = NULL;
    uint32_t i;
    int32_t result;

    if (table == NULL || lengths == NULL || out_handle == NULL ||
        count < 2u || count > SCHULTZ_DASH_MAX) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < count; i++) {
        if (lengths[i] <= 0.0f) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
    }
    result = schultz_resource_add(table, &entry, out_handle);
    if (result != SCHULTZ_OK) {
        return result;
    }
    entry->is_dash = 1u;
    memset(&entry->as.dash, 0, sizeof(entry->as.dash));
    for (i = 0; i < count; i++) {
        entry->as.dash.lengths[i] = lengths[i];
    }
    entry->as.dash.count = count;
    return SCHULTZ_OK;
}

int32_t schultz_dash_pair(schultz_resource_table *table, float on, float off,
                          schultz_handle *out_handle)
{
    float lengths[2];

    lengths[0] = on;
    lengths[1] = off;
    return schultz_dash_register(table, lengths, 2u, out_handle);
}

int32_t schultz_dash_get(const schultz_resource_table *table,
                         schultz_handle handle, const schultz_dash **out_dash)
{
    const schultz_resource *entry;

    if (out_dash == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    entry = schultz_resource_find(table, handle, 1u);
    if (entry == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    *out_dash = &entry->as.dash;
    return SCHULTZ_OK;
}
