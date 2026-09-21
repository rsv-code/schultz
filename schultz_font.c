/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_font.c
 * @brief Font loading and metrics.
 */

#include "schultz_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb.h>
#include <hb-ft.h>

#include "schultz_colr.h"
#include "schultz_font_builtin.h"
#include "schultz_font_internal.h"
#include "schultz_handle.h"

/** @brief The FreeType library plus the table naming every loaded font. */
struct schultz_font_system {
    FT_Library           library; /**< Owns rasterization for every face. */
    schultz_handle_table fonts;   /**< Maps a handle to a schultz_font. */
    /*
     * Every font ever loaded, so the same face at a second size can be found
     * rather than read from disk again. A face bound to a pixel size is what
     * FreeType and HarfBuzz both want, so one file at three sizes is three
     * entries here and one file read.
     */
    schultz_handle      *loaded;   /**< Every font ever loaded. */
    uint32_t             count;    /**< How many of them there are. */
    uint32_t             capacity; /**< How many the array can hold. */
    /**
     * The faces compiled into the library, one per theme font slot, loaded
     * when the system is created. SCHULTZ_HANDLE_NONE for one that would not
     * load, which nothing but a broken build produces.
     */
    schultz_handle       builtin[SCHULTZ_TOKEN_FONT_COUNT];
    /**
     * The face that draws what a text face cannot. One face rather than a
     * list of them, because emoji are the whole of the case: any other gap
     * is a script, and the answer to a script is to set the body font to a
     * face that has it.
     */
    schultz_handle       emoji;
};

/**
 * The size the built in faces are first loaded at. Any other size comes from
 * schultz_font_at_size, which shares their bytes rather than copying them, so
 * this only decides which size costs nothing to ask for.
 */
#define SCHULTZ_BUILTIN_FONT_SIZE 16.0f

/*
 * Builds a font over bytes whose ownership the caller has settled, and
 * registers it. Defined below, and named here because creating the font
 * system loads the faces compiled into the library through it.
 */
static int32_t schultz_font_add(schultz_font_system *system,
                                const unsigned char *data, size_t size,
                                uint32_t owns_data, char *path,
                                float size_px, schultz_handle *out_font);

/* Remembers a handle so the same file at another size can find its siblings. */
static int32_t schultz_font_remember(schultz_font_system *system,
                                     schultz_handle font)
{
    if (system->count == system->capacity) {
        uint32_t capacity = (system->capacity == 0u) ? 8u
                                                     : system->capacity * 2u;
        schultz_handle *grown = (schultz_handle *)realloc(
            system->loaded, (size_t)capacity * sizeof(*grown));

        if (grown == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        system->loaded   = grown;
        system->capacity = capacity;
    }
    system->loaded[system->count++] = font;
    return SCHULTZ_OK;
}

/*
 * Reads a whole file. Fonts are small enough that streaming buys nothing, and
 * owning the bytes means the caller may delete the file afterwards.
 */
static int32_t schultz_font_read_file(const char *path, unsigned char **out_data,
                                      size_t *out_size)
{
    FILE *file;
    long length;
    unsigned char *data;

    file = fopen(path, "rb");
    if (file == NULL) {
        return SCHULTZ_ERR_UNREADABLE;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return SCHULTZ_ERR_UNREADABLE;
    }
    length = ftell(file);
    if (length <= 0) {
        fclose(file);
        return SCHULTZ_ERR_UNREADABLE;
    }
    rewind(file);

    data = (unsigned char *)malloc((size_t)length);
    if (data == NULL) {
        fclose(file);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return SCHULTZ_ERR_UNREADABLE;
    }
    fclose(file);

    *out_data = data;
    *out_size = (size_t)length;
    return SCHULTZ_OK;
}

int32_t schultz_font_system_create(schultz_font_system **out_system)
{
    schultz_font_system *system;
    int32_t result;

    if (out_system == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    system = (schultz_font_system *)calloc(1, sizeof(*system));
    if (system == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    if (FT_Init_FreeType(&system->library) != 0) {
        free(system);
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    result = schultz_handle_table_init(&system->fonts, 8);
    if (result != SCHULTZ_OK) {
        FT_Done_FreeType(system->library);
        free(system);
        return result;
    }

    /*
     * The faces compiled into the library, loaded once here so a program that
     * says nothing about fonts still draws text. A theme with an empty font
     * slot falls back to one of these; a host that loads its own face
     * overrides it and these cost nothing but the handles.
     *
     * At the body text size, which is the one most of a screen is set in.
     * Every other size comes from schultz_font_at_size on demand, the same
     * way it does for a face a host loaded.
     */
    {
        uint32_t faces = schultz_font_builtin_count();
        uint32_t token;
        uint32_t index;

        for (token = 0u; token < SCHULTZ_TOKEN_FONT_COUNT; token++) {
            system->builtin[token] = SCHULTZ_HANDLE_NONE;
        }
        /*
         * Every compiled in face, not only the ones that fill a slot. The two
         * oblique Sans faces fill none: they are here so that the Sans family
         * is complete, and a family is only complete if every member of it is
         * loaded for schultz_font_at_style to find.
         */
        for (index = 0u; index < faces; index++) {
            const unsigned char *bytes;
            size_t length = 0u;
            schultz_handle font = SCHULTZ_HANDLE_NONE;

            token = SCHULTZ_TOKEN_FONT_COUNT;
            bytes = schultz_font_builtin_face(index, &length, &token);
            if (bytes == NULL) {
                continue;
            }
            /*
             * A face that will not load is not worth refusing a font system
             * over: everything else still works, and a host that loads its
             * own faces never notices. The slot stays empty and there is
             * nothing to draw the missing character with, which is where
             * this started.
             */
            if (schultz_font_add(system, bytes, length, 0u, NULL,
                                 SCHULTZ_BUILTIN_FONT_SIZE,
                                 &font) == SCHULTZ_OK &&
                token < SCHULTZ_TOKEN_FONT_COUNT) {
                system->builtin[token] = font;
            }
        }
    }

    /*
     * Emoji out of the box. A host that wants its own artwork replaces this
     * with schultz_font_set_emoji, and the bundled face then costs it
     * nothing but the handle.
     */
    system->emoji = system->builtin[SCHULTZ_TOKEN_FONT_EMOJI];

    *out_system = system;
    return SCHULTZ_OK;
}

int32_t schultz_font_set_emoji(schultz_font_system *system,
                                  schultz_handle font)
{
    if (system == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (font != SCHULTZ_HANDLE_NONE) {
        schultz_font *unused;

        if (schultz_font_resolve(system, font, &unused) != SCHULTZ_OK) {
            return SCHULTZ_ERR_INVALID_HANDLE;
        }
    }
    system->emoji = font;
    return SCHULTZ_OK;
}

schultz_handle schultz_font_emoji(const schultz_font_system *system)
{
    return (system == NULL) ? SCHULTZ_HANDLE_NONE : system->emoji;
}

int32_t schultz_font_is_fixed_pitch(const schultz_font_system *system,
                                    schultz_handle font)
{
    schultz_font *loaded;

    if (schultz_font_resolve(system, font, &loaded) != SCHULTZ_OK) {
        return 0;
    }
    /*
     * The face says so itself. FreeType reads it out of the font's own
     * tables, so this is what the designer declared rather than a guess from
     * measuring two glyphs and hoping.
     */
    return FT_IS_FIXED_WIDTH(loaded->face) ? 1 : 0;
}

int32_t schultz_font_is_bold(const schultz_font_system *system,
                             schultz_handle font)
{
    schultz_font *loaded;

    if (schultz_font_resolve(system, font, &loaded) != SCHULTZ_OK) {
        return 0;
    }
    return (int32_t)loaded->bold;
}

int32_t schultz_font_is_italic(const schultz_font_system *system,
                               schultz_handle font)
{
    schultz_font *loaded;

    if (schultz_font_resolve(system, font, &loaded) != SCHULTZ_OK) {
        return 0;
    }
    return (int32_t)loaded->italic;
}

int32_t schultz_font_has_glyph(const schultz_font_system *system,
                               schultz_handle font, uint32_t codepoint)
{
    schultz_font *loaded;

    if (schultz_font_resolve(system, font, &loaded) != SCHULTZ_OK) {
        return 0;
    }
    return FT_Get_Char_Index(loaded->face, (FT_ULong)codepoint) != 0;
}

int32_t schultz_font_has_color_glyph(const schultz_font_system *system,
                                     schultz_handle font, uint32_t codepoint)
{
    schultz_font *loaded;
    FT_LayerIterator layers;
    FT_UInt glyph;
    FT_UInt layer_glyph = 0;
    FT_UInt layer_color = 0;

    if (schultz_font_resolve(system, font, &loaded) != SCHULTZ_OK) {
        return 0;
    }
    if (!FT_HAS_COLOR(loaded->face)) {
        return 0;
    }
    glyph = FT_Get_Char_Index(loaded->face, (FT_ULong)codepoint);
    if (glyph == 0u) {
        return 0;
    }
    /*
     * A face built out of layers says so per glyph, which is the answer
     * wanted: an emoji face carries the digits and the joiners as well, for
     * the sequences they take part in, and those are drawn in one colour
     * like any letter.
     */
    layers.p = NULL;
    if (FT_Get_Color_Glyph_Layer(loaded->face, glyph, &layer_glyph,
                                 &layer_color, &layers)) {
        return 1;
    }
    /*
     * A face that describes its colour glyphs as drawings says so per glyph
     * as well, and those are drawn by schultz_colr rather than by FreeType.
     * Asking here rather than only at drawing time is what keeps the two
     * agreeing about which face owns a character.
     */
    if (schultz_colr_has_drawing(loaded->face, glyph)) {
        return 1;
    }
    /*
     * A face built out of pictures has no layers to report and no way to say
     * this per glyph, so the face answers for all of them: every glyph in it
     * is a picture. Strikes are what mark one, since a face drawn at fixed
     * sizes is a face made of images.
     */
    return loaded->face->num_fixed_sizes > 0;
}

schultz_handle schultz_font_builtin(const schultz_font_system *system,
                                    uint32_t token)
{
    if (system == NULL || token >= SCHULTZ_TOKEN_FONT_COUNT) {
        return SCHULTZ_HANDLE_NONE;
    }
    return system->builtin[token];
}

/* Releases one font's FreeType and HarfBuzz state and its file bytes. */
static void schultz_font_free(schultz_font *font)
{
    if (font == NULL) {
        return;
    }
    if (font->hb_font != NULL) {
        hb_font_destroy(font->hb_font);
    }
    if (font->face != NULL) {
        FT_Done_Face(font->face);
    }
    /*
     * Only when they were allocated for this font. The faces compiled into
     * the library are static and shared by every size loaded from them, so
     * freeing those would take out every other size and the next program run
     * besides.
     */
    if (font->owns_data) {
        free((void *)(uintptr_t)font->data);
    }
    free(font->path);
    free(font);
}

void schultz_font_system_destroy(schultz_font_system *system)
{
    uint32_t i;

    if (system == NULL) {
        return;
    }

    /*
     * Walk the slot array directly rather than the handle API: this is
     * teardown, and every live slot must be released whether or not the caller
     * still holds its handle.
     */
    for (i = 0; i < system->fonts.capacity; i++) {
        if (system->fonts.slots[i].live) {
            schultz_font_free((schultz_font *)system->fonts.slots[i].object);
        }
    }
    schultz_handle_table_free(&system->fonts);
    free(system->loaded);

    if (system->library != NULL) {
        FT_Done_FreeType(system->library);
    }
    free(system);
}

/*
 * Builds a font over bytes the caller has already settled ownership of, and
 * registers it.
 *
 * Everything that loads a face ends up here, because a file load has always
 * been a memory load with a read in front of it: the bytes are read, and
 * FreeType is given the buffer rather than the path. What differs between the
 * ways in is only who owns the bytes and whether there is a path to load the
 * face again from at another size.
 *
 * On failure the font is freed, which takes the bytes with it when it owns
 * them, so a caller hands over ownership by calling this at all.
 */
static int32_t schultz_font_add(schultz_font_system *system,
                                const unsigned char *data, size_t size,
                                uint32_t owns_data, char *path,
                                float size_px, schultz_handle *out_font)
{
    schultz_font *font = (schultz_font *)calloc(1, sizeof(*font));
    int32_t result;

    if (font == NULL) {
        if (owns_data) {
            free((void *)(uintptr_t)data);
        }
        free(path);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    font->size_px   = size_px;
    font->data      = data;
    font->size      = size;
    font->owns_data = owns_data;
    font->path      = path;

    /*
     * Unreadable rather than a bad argument: the path and the size were fine,
     * and what went wrong is that the bytes are not a face this build of
     * FreeType can read. Setting the size fails the same way, on a face that
     * only comes in sizes of its own.
     */
    if (FT_New_Memory_Face(system->library, font->data, (FT_Long)font->size, 0,
                           &font->face) != 0) {
        schultz_font_free(font);
        return SCHULTZ_ERR_UNREADABLE;
    }

    if (FT_Set_Pixel_Sizes(font->face, 0, (FT_UInt)(size_px + 0.5f)) != 0) {
        schultz_font_free(font);
        return SCHULTZ_ERR_UNREADABLE;
    }

    /*
     * Which family this is and which member of it. Both come from the face
     * itself, so nothing has to be told and nothing can disagree with the
     * bytes. A face with no family name keeps an empty one and joins no
     * family; see the field.
     */
    if (font->face->family_name != NULL) {
        size_t room = sizeof(font->family) - 1u;
        size_t i;

        for (i = 0u; i < room && font->face->family_name[i] != '\0'; i++) {
            font->family[i] = font->face->family_name[i];
        }
        font->family[i] = '\0';
    }
    font->bold   = (font->face->style_flags & FT_STYLE_FLAG_BOLD) ? 1u : 0u;
    font->italic = (font->face->style_flags & FT_STYLE_FLAG_ITALIC) ? 1u : 0u;

    /*
     * hb_ft_font_create_referenced takes its own reference on the face, so
     * HarfBuzz and FreeType lifetimes are independent from here on.
     */
    font->hb_font = hb_ft_font_create_referenced(font->face);
    if (font->hb_font == NULL) {
        schultz_font_free(font);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    result = schultz_handle_table_insert(&system->fonts, font, out_font);
    if (result != SCHULTZ_OK) {
        schultz_font_free(font);
        return result;
    }
    result = schultz_font_remember(system, *out_font);
    if (result != SCHULTZ_OK) {
        schultz_handle_table_remove(&system->fonts, *out_font);
        schultz_font_free(font);
        return result;
    }
    return SCHULTZ_OK;
}

int32_t schultz_font_load_memory(schultz_font_system *system,
                                 const void *bytes, size_t length,
                                 float size_px, schultz_handle *out_font)
{
    unsigned char *copy;

    if (system == NULL || bytes == NULL || length == 0u || out_font == NULL ||
        size_px <= 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * Copied, because FreeType reads from the buffer for as long as the face
     * lives and a caller cannot be asked to keep a buffer alive for a handle
     * it may pass on. The file path does the same thing: it reads the file
     * into a buffer the font owns.
     */
    copy = (unsigned char *)malloc(length);
    if (copy == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, bytes, length);
    return schultz_font_add(system, copy, length, 1u, NULL, size_px,
                            out_font);
}

int32_t schultz_font_load_file(schultz_font_system *system, const char *path,
                               float size_px, schultz_handle *out_font)
{
    unsigned char *data = NULL;
    size_t size = 0u;
    char *kept;
    int32_t result;

    if (system == NULL || path == NULL || out_font == NULL ||
        size_px <= 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    /* Kept so the same face can be loaded again at another size. */
    kept = (char *)malloc(strlen(path) + 1u);
    if (kept == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    memcpy(kept, path, strlen(path) + 1u);

    result = schultz_font_read_file(path, &data, &size);
    if (result != SCHULTZ_OK) {
        free(kept);
        return result;
    }
    return schultz_font_add(system, data, size, 1u, kept, size_px, out_font);
}

int32_t schultz_font_at_size(schultz_font_system *system,
                             schultz_handle handle, float size_px,
                             schultz_handle *out_font)
{
    schultz_font *font;
    uint32_t i;
    int32_t result;

    if (system == NULL || out_font == NULL || size_px <= 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_font_resolve(system, handle, &font);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /*
     * Sizes are compared after rounding to whole pixels, because that is what
     * FreeType is given and two sizes that round together are the same face.
     */
    if ((uint32_t)(font->size_px + 0.5f) == (uint32_t)(size_px + 0.5f)) {
        *out_font = handle;
        return SCHULTZ_OK;
    }

    for (i = 0; i < system->count; i++) {
        schultz_font *other;

        if (schultz_font_resolve(system, system->loaded[i], &other)
                != SCHULTZ_OK ||
            (uint32_t)(other->size_px + 0.5f) != (uint32_t)(size_px + 0.5f)) {
            continue;
        }
        /*
         * The same face means the same source. A face loaded from a file is
         * named by its path; a face handed over as bytes has no name, so the
         * bytes themselves say whether two fonts came from the same place.
         * The two are never mixed up, because a font has one or the other.
         */
        if (font->path != NULL) {
            if (other->path != NULL &&
                strcmp(other->path, font->path) == 0) {
                *out_font = system->loaded[i];
                return SCHULTZ_OK;
            }
        } else if (other->path == NULL && other->data == font->data) {
            *out_font = system->loaded[i];
            return SCHULTZ_OK;
        }
    }

    /* Not loaded at that size yet, so load it once and keep it. */
    if (font->path != NULL) {
        return schultz_font_load_file(system, font->path, size_px, out_font);
    }
    /*
     * From the bytes instead. A face compiled into the library does not own
     * them, so every size shares the one static copy and the comparison above
     * can match on the pointer. A face the host handed over owns its copy, so
     * this makes another one rather than pointing a second font at bytes the
     * first would free.
     */
    if (font->owns_data) {
        return schultz_font_load_memory(system, font->data, font->size,
                                        size_px, out_font);
    }
    return schultz_font_add(system, font->data, font->size, 0u, NULL, size_px,
                            out_font);
}

/*
 * The member of a family carrying these styles, or SCHULTZ_HANDLE_NONE.
 *
 * A member at the size already wanted is the best answer because it needs no
 * further work, but a member at any size will do: the caller asks it for the
 * size it wants afterwards, which is one lookup rather than a second face.
 */
static schultz_handle schultz_font_member(schultz_font_system *system,
                                          const schultz_font *want,
                                          uint32_t bold, uint32_t italic)
{
    schultz_handle any_size = SCHULTZ_HANDLE_NONE;
    uint32_t i;

    for (i = 0; i < system->count; i++) {
        schultz_font *other;

        if (schultz_font_resolve(system, system->loaded[i], &other)
                != SCHULTZ_OK ||
            other->bold != bold || other->italic != italic ||
            strcmp(other->family, want->family) != 0) {
            continue;
        }
        if ((uint32_t)(other->size_px + 0.5f) ==
            (uint32_t)(want->size_px + 0.5f)) {
            return system->loaded[i];
        }
        if (any_size == SCHULTZ_HANDLE_NONE) {
            any_size = system->loaded[i];
        }
    }
    return any_size;
}

int32_t schultz_font_at_style(schultz_font_system *system,
                              schultz_handle handle, int32_t bold,
                              int32_t italic, schultz_handle *out_font)
{
    uint32_t want_bold   = (bold != 0) ? 1u : 0u;
    uint32_t want_italic = (italic != 0) ? 1u : 0u;
    schultz_handle found;
    schultz_font *font;
    int32_t result;

    if (system == NULL || out_font == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_font_resolve(system, handle, &font);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /*
     * Already the face asked for, and a face that names no family has no
     * siblings to look through. Both are their own answer.
     */
    if ((font->bold == want_bold && font->italic == want_italic) ||
        font->family[0] == '\0') {
        *out_font = handle;
        return SCHULTZ_OK;
    }

    found = schultz_font_member(system, font, want_bold, want_italic);
    /*
     * Nothing exact. Half of what was asked for reads better than none of it,
     * so bold italic falls back to bold, and then to italic, before it gives
     * up. Bold first because weight carries emphasis further than slant does
     * at the sizes most text is set in.
     */
    if (found == SCHULTZ_HANDLE_NONE && want_bold && want_italic) {
        found = schultz_font_member(system, font, 1u, 0u);
        if (found == SCHULTZ_HANDLE_NONE) {
            found = schultz_font_member(system, font, 0u, 1u);
        }
    }
    /*
     * A family with nothing closer answers with the face it was handed, so a
     * caller never has to check whether it got what it asked for. Nothing is
     * synthesized: a sheared upright is spaced for the face it was sheared
     * from, and reads worse than the upright it came from.
     */
    if (found == SCHULTZ_HANDLE_NONE) {
        *out_font = handle;
        return SCHULTZ_OK;
    }
    return schultz_font_at_size(system, found, font->size_px, out_font);
}

int32_t schultz_font_unload(schultz_font_system *system, schultz_handle handle)
{
    schultz_font *font;
    int32_t result;

    if (system == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }

    result = schultz_font_resolve(system, handle, &font);
    if (result != SCHULTZ_OK) {
        return result;
    }

    result = schultz_handle_table_remove(&system->fonts, handle);
    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_font_free(font);
    return SCHULTZ_OK;
}

int32_t schultz_font_resolve(const schultz_font_system *system,
                             schultz_handle handle, schultz_font **out_font)
{
    void *object = NULL;
    int32_t result;

    if (system == NULL || out_font == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_handle_table_lookup(&system->fonts, handle, &object);
    if (result != SCHULTZ_OK) {
        return result;
    }
    *out_font = (schultz_font *)object;
    return SCHULTZ_OK;
}

int32_t schultz_font_get_metrics(const schultz_font_system *system,
                                 schultz_handle handle,
                                 schultz_font_metrics *out_metrics)
{
    schultz_font *font;
    int32_t result;
    FT_Size_Metrics m;

    if (out_metrics == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_font_resolve(system, handle, &font);
    if (result != SCHULTZ_OK) {
        return result;
    }

    /* FreeType reports scaled metrics in 26.6 fixed point: 64ths of a pixel. */
    m = font->face->size->metrics;
    out_metrics->ascent      = (float)m.ascender / 64.0f;
    out_metrics->descent     = (float)(-m.descender) / 64.0f;
    out_metrics->line_height = (float)m.height / 64.0f;

    /*
     * Underline metrics live on the face in font units and must be scaled by
     * hand, unlike the size metrics above.
     */
    if (font->face->units_per_EM != 0) {
        float scale = font->size_px / (float)font->face->units_per_EM;
        out_metrics->underline_position =
            (float)(-font->face->underline_position) * scale;
        out_metrics->underline_thickness =
            (float)font->face->underline_thickness * scale;
    } else {
        out_metrics->underline_position  = out_metrics->descent * 0.5f;
        out_metrics->underline_thickness = 1.0f;
    }

    /*
     * Where a line through the text goes. This is in the OS/2 table rather
     * than on the face itself, and a face may not carry one, so the fallback
     * is halfway up the ascent, which is where a strikethrough sits when
     * nobody has said otherwise.
     */
    {
        TT_OS2 *os2 = (TT_OS2 *)FT_Get_Sfnt_Table(font->face, FT_SFNT_OS2);
        float scale = (font->face->units_per_EM != 0)
                          ? font->size_px / (float)font->face->units_per_EM
                          : 0.0f;

        if (os2 != NULL && scale > 0.0f && os2->yStrikeoutSize > 0) {
            out_metrics->strikeout_position =
                (float)os2->yStrikeoutPosition * scale;
            out_metrics->strikeout_thickness =
                (float)os2->yStrikeoutSize * scale;
        } else {
            out_metrics->strikeout_position  = out_metrics->ascent * 0.5f;
            out_metrics->strikeout_thickness =
                out_metrics->underline_thickness;
        }
    }
    return SCHULTZ_OK;
}

float schultz_font_size(const schultz_font_system *system,
                        schultz_handle handle)
{
    schultz_font *font;

    if (schultz_font_resolve(system, handle, &font) != SCHULTZ_OK) {
        return 0.0f;
    }
    return font->size_px;
}

uint32_t schultz_font_count(const schultz_font_system *system)
{
    return (system == NULL) ? 0 : schultz_handle_table_count(&system->fonts);
}
