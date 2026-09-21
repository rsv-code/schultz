/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_glyphs.c
 * @brief Rasterized glyph cache.
 *
 * An open addressed hash table keyed by font handle and glyph index. Linear
 * probing, power of two capacity, grown when it passes three quarters full.
 * Entries are never individually removed, only purged per font, which keeps
 * probing simple: there are no tombstones to skip.
 */

#include "schultz_glyphs.h"

#include "schultz_colr.h"

#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "schultz_font_internal.h"

enum {
    SCHULTZ_GLYPH_INITIAL_CAPACITY = 256
};

/** @brief One hash table slot. */
typedef struct {
    schultz_handle       font;     /**< Owning font, or SCHULTZ_HANDLE_NONE. */
    uint32_t             glyph_id; /**< Glyph index within that font. */
    uint32_t             live;     /**< Nonzero when the slot is occupied. */
    schultz_glyph_bitmap glyph;    /**< The cached bitmap and its bearings. */
    unsigned char       *pixels;   /**< Owned coverage bytes, or NULL. */
} schultz_glyph_entry;

/** @brief Open addressed table of rasterized glyphs. */
struct schultz_glyph_cache {
    schultz_font_system *system;   /**< Where fonts are resolved. Not owned. */
    schultz_glyph_entry *entries;  /**< Slot array, capacity entries long. */
    uint32_t             capacity; /**< Always a power of two. */
    uint32_t             count;    /**< Occupied slots. */
};

/*
 * Mixes the two key parts into a well spread hash. This is the 64 bit
 * finalizer from MurmurHash3, which is cheap and avoids the clustering that
 * plain addition produces for sequential glyph indices.
 */
static uint64_t schultz_glyph_hash(schultz_handle font, uint32_t glyph_id)
{
    uint64_t h = font ^ ((uint64_t)glyph_id << 32) ^ (uint64_t)glyph_id;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

/*
 * Finds the slot for a key: either the occupied slot holding it, or the first
 * free slot where it belongs. Never fails, because the table is grown before
 * it can fill.
 */
static schultz_glyph_entry *schultz_glyph_slot(schultz_glyph_entry *entries,
                                               uint32_t capacity,
                                               schultz_handle font,
                                               uint32_t glyph_id)
{
    uint32_t mask = capacity - 1u;
    uint32_t i = (uint32_t)(schultz_glyph_hash(font, glyph_id)) & mask;

    for (;;) {
        schultz_glyph_entry *entry = &entries[i];
        if (!entry->live ||
            (entry->font == font && entry->glyph_id == glyph_id)) {
            return entry;
        }
        i = (i + 1u) & mask;
    }
}

static int32_t schultz_glyph_grow(schultz_glyph_cache *cache)
{
    uint32_t new_capacity = (cache->capacity == 0)
                                ? SCHULTZ_GLYPH_INITIAL_CAPACITY
                                : cache->capacity * 2u;
    schultz_glyph_entry *entries;
    uint32_t i;

    entries = (schultz_glyph_entry *)calloc(new_capacity, sizeof(*entries));
    if (entries == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    for (i = 0; i < cache->capacity; i++) {
        schultz_glyph_entry *old = &cache->entries[i];
        if (old->live) {
            *schultz_glyph_slot(entries, new_capacity, old->font,
                                old->glyph_id) = *old;
        }
    }

    free(cache->entries);
    cache->entries  = entries;
    cache->capacity = new_capacity;
    return SCHULTZ_OK;
}

int32_t schultz_glyph_cache_create(schultz_font_system *system,
                                   schultz_glyph_cache **out_cache)
{
    schultz_glyph_cache *cache;
    int32_t result;

    if (system == NULL || out_cache == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    cache = (schultz_glyph_cache *)calloc(1, sizeof(*cache));
    if (cache == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    cache->system = system;

    result = schultz_glyph_grow(cache);
    if (result != SCHULTZ_OK) {
        free(cache);
        return result;
    }

    *out_cache = cache;
    return SCHULTZ_OK;
}

void schultz_glyph_cache_destroy(schultz_glyph_cache *cache)
{
    uint32_t i;

    if (cache == NULL) {
        return;
    }
    for (i = 0; i < cache->capacity; i++) {
        free(cache->entries[i].pixels);
    }
    free(cache->entries);
    free(cache);
}

/*
 * Counts a newly filled entry and hands it back.
 *
 * Growing has to happen after the entry is filled rather than before,
 * because growing moves every entry, so the pointer to hand back is only
 * settled once the table has stopped changing size.
 */
static int32_t schultz_glyph_keep(schultz_glyph_cache *cache,
                                  schultz_glyph_entry *entry,
                                  schultz_handle font_handle,
                                  uint32_t glyph_id,
                                  const schultz_glyph_bitmap **out_glyph)
{
    cache->count++;

    /* Grow before the table can fill, so probing always terminates. */
    if (cache->count * 4u >= cache->capacity * 3u) {
        int32_t result = schultz_glyph_grow(cache);

        if (result != SCHULTZ_OK) {
            return result;
        }
        entry = schultz_glyph_slot(cache->entries, cache->capacity,
                                   font_handle, glyph_id);
    }

    *out_glyph = &entry->glyph;
    return SCHULTZ_OK;
}

int32_t schultz_glyph_cache_get(schultz_glyph_cache *cache,
                                schultz_handle font_handle, uint32_t glyph_id,
                                const schultz_glyph_bitmap **out_glyph)
{
    schultz_glyph_entry *entry;
    schultz_font *font;
    FT_GlyphSlot slot;
    size_t bytes;
    int32_t result;

    if (cache == NULL || out_glyph == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    entry = schultz_glyph_slot(cache->entries, cache->capacity, font_handle,
                               glyph_id);
    if (entry->live) {
        *out_glyph = &entry->glyph;
        return SCHULTZ_OK;
    }

    result = schultz_font_resolve(cache->system, font_handle, &font);
    if (result != SCHULTZ_OK) {
        return result;
    }

    /*
     * A glyph the font describes as a drawing rather than an outline is
     * drawn here rather than by FreeType, which reads those and hands them
     * over unrasterized. Everything else takes the path below.
     */
    if (schultz_colr_has_drawing(font->face, (FT_UInt)glyph_id)) {
        unsigned char *drawn = NULL;
        uint32_t drawn_w = 0u;
        uint32_t drawn_h = 0u;
        int32_t drawn_left = 0;
        int32_t drawn_top = 0;

        if (schultz_colr_render(font->face, (FT_UInt)glyph_id, &drawn,
                                &drawn_w, &drawn_h, &drawn_left, &drawn_top)
                == SCHULTZ_OK) {
            memset(entry, 0, sizeof(*entry));
            entry->font      = font_handle;
            entry->glyph_id  = glyph_id;
            entry->live      = 1;
            entry->pixels    = drawn;
            entry->glyph.bitmap    = drawn;
            entry->glyph.width     = drawn_w;
            entry->glyph.height    = drawn_h;
            entry->glyph.pitch     = drawn_w * 4u;
            entry->glyph.bearing_x = drawn_left;
            entry->glyph.bearing_y = drawn_top;
            entry->glyph.format    = (uint32_t)SCHULTZ_GLYPH_COLOR;
            return schultz_glyph_keep(cache, entry, font_handle, glyph_id,
                                      out_glyph);
        }
        /* It would not draw, so fall through and let FreeType try: a face
         * that carries both kinds still has the ordinary one. */
    }

    /*
     * FT_LOAD_RENDER rasterizes in the same call. The default target is 8 bit
     * antialiased coverage, which is what the painter blends with.
     *
     * FT_LOAD_COLOR additionally lets a face that carries colour answer with
     * it. A text face has none and is unaffected, so this is asked for every
     * time rather than only for the emoji face: which faces have colour is
     * the font's business, not the cache's.
     */
    if (FT_Load_Glyph(font->face, (FT_UInt)glyph_id,
                      FT_LOAD_RENDER | FT_LOAD_COLOR) != 0) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    slot = font->face->glyph;

    memset(entry, 0, sizeof(*entry));
    entry->font      = font_handle;
    entry->glyph_id  = glyph_id;
    entry->live      = 1;
    entry->glyph.width     = slot->bitmap.width;
    entry->glyph.height    = slot->bitmap.rows;
    entry->glyph.pitch     = (uint32_t)(slot->bitmap.pitch < 0
                                            ? -slot->bitmap.pitch
                                            : slot->bitmap.pitch);
    entry->glyph.bearing_x = slot->bitmap_left;
    entry->glyph.bearing_y = slot->bitmap_top;
    /*
     * Anything that is not four channel colour is treated as coverage. The
     * other modes FreeType can produce are monochrome and the subpixel ones,
     * none of which are asked for here, so in practice this is the two.
     */
    entry->glyph.format = (slot->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA)
                              ? (uint32_t)SCHULTZ_GLYPH_COLOR
                              : (uint32_t)SCHULTZ_GLYPH_COVERAGE;

    bytes = (size_t)entry->glyph.pitch * entry->glyph.height;
    if (bytes > 0) {
        entry->pixels = (unsigned char *)malloc(bytes);
        if (entry->pixels == NULL) {
            memset(entry, 0, sizeof(*entry));
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        memcpy(entry->pixels, slot->bitmap.buffer, bytes);
    }
    entry->glyph.bitmap = entry->pixels;

    return schultz_glyph_keep(cache, entry, font_handle, glyph_id,
                              out_glyph);
}

void *schultz_glyph_outline(schultz_glyph_cache *cache, schultz_handle font,
                            uint32_t glyph_id, float *out_scale)
{
    schultz_font *loaded = NULL;

    if (cache == NULL || out_scale == NULL) {
        return NULL;
    }
    *out_scale = 1.0f;
    if (schultz_font_resolve(cache->system, font, &loaded) != SCHULTZ_OK ||
        loaded->face == NULL || loaded->face->units_per_EM == 0) {
        return NULL;
    }
    *out_scale = (float)loaded->face->size->metrics.x_ppem
                     / (float)loaded->face->units_per_EM;
    return schultz_colr_glyph_outline(loaded->face, (FT_UInt)glyph_id);
}

void schultz_glyph_cache_purge_font(schultz_glyph_cache *cache,
                                    schultz_handle font_handle)
{
    schultz_glyph_entry *survivors;
    uint32_t capacity;
    uint32_t kept = 0;
    uint32_t i;

    if (cache == NULL) {
        return;
    }

    /*
     * Rebuilt rather than cleared in place: removing from an open addressed
     * table without tombstones would break the probe chains of entries that
     * hashed past the hole.
     */
    capacity  = cache->capacity;
    survivors = (schultz_glyph_entry *)calloc(capacity, sizeof(*survivors));
    if (survivors == NULL) {
        return;
    }

    for (i = 0; i < capacity; i++) {
        schultz_glyph_entry *entry = &cache->entries[i];
        if (!entry->live) {
            continue;
        }
        if (entry->font == font_handle) {
            free(entry->pixels);
            continue;
        }
        *schultz_glyph_slot(survivors, capacity, entry->font,
                            entry->glyph_id) = *entry;
        kept++;
    }

    free(cache->entries);
    cache->entries = survivors;
    cache->count   = kept;
}

uint32_t schultz_glyph_cache_count(const schultz_glyph_cache *cache)
{
    return (cache == NULL) ? 0 : cache->count;
}
