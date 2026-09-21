/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_style.c
 * @brief Tokens, properties, style patches and themes.
 */

#include "schultz_style.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------ built in tokens */

static schultz_color schultz_rgb(uint32_t value)
{
    return schultz_color_rgba((uint8_t)((value >> 16) & 0xFFu),
                              (uint8_t)((value >> 8) & 0xFFu),
                              (uint8_t)(value & 0xFFu), 255u);
}

/*
 * The compiled in defaults, which are a dark look. Every token has a value
 * here, which is what guarantees resolution can never find a hole. A theme
 * overrides whichever of these it cares about and leaves the rest alone.
 *
 * The colours are Aussom's, taken from the palette its own site is built on:
 * teal #1AABAB and its darker #0E7272, navy #1A1E2E, slate #252839, red
 * #D94438, and the greys around them. What follows is that palette put into
 * the roles this toolkit has, which is not a job the stylesheet had to do:
 * a page has sections and cards, an interface has four levels of surface, a
 * focus ring and a disabled state.
 */
static schultz_color schultz_token_color_default(uint32_t token)
{
    switch (token) {
    /*
     * A tonal ladder rather than a set of picked colours. Elevation is told
     * by tone: the further a surface is from the page, the further its
     * lightness is, which is how a design system says "above" without a
     * shadow. The steps are even in L*, so they look evenly spaced rather
     * than merely being evenly numbered.
     *
     *   sunken  L*6.5   a well, below the page
     *   window  L*11.6  the page itself, the site's navy
     *   surface L*16.5  a panel on it, the site's slate
     *   raised  L*21.3  a menu or a tooltip over that
     *
     * The middle two are the site's own pair, which it uses for exactly this
     * relationship: a dark section, and the code block sitting on it. That
     * step is 4.9 in L*, so the two outer rungs are the same step again in
     * each direction rather than a fresh guess.
     */
    case SCHULTZ_TOKEN_COLOR_SURFACE_SUNKEN: return schultz_rgb(0x0f1423);
    case SCHULTZ_TOKEN_COLOR_WINDOW:         return schultz_rgb(0x1a1e2e);
    case SCHULTZ_TOKEN_COLOR_SURFACE:        return schultz_rgb(0x252839);
    case SCHULTZ_TOKEN_COLOR_SURFACE_RAISED: return schultz_rgb(0x303244);
    case SCHULTZ_TOKEN_COLOR_ACCENT:         return schultz_rgb(0x1aabab);
    /*
     * The site draws its lines on dark as white at a low opacity. These are
     * those overlays worked out against the navy and written down opaque,
     * because a token is one colour and not a colour plus a background:
     * border is white at 0.14, muted text at 0.65, disabled at 0.40.
     */
    case SCHULTZ_TOKEN_COLOR_BORDER:         return schultz_rgb(0x3a3e4b);
    case SCHULTZ_TOKEN_COLOR_TEXT:           return schultz_rgb(0xffffff);
    case SCHULTZ_TOKEN_COLOR_TEXT_MUTED:     return schultz_rgb(0xafb0b6);
    /*
     * Navy on the teal, not white. The site puts white on teal for its
     * buttons, which measures 2.82 to 1 and is under the 4.5 body text
     * wants; the navy it already owns gives 5.88 on the same fill.
     */
    case SCHULTZ_TOKEN_COLOR_TEXT_ON_ACCENT: return schultz_rgb(0x1a1e2e);
    case SCHULTZ_TOKEN_COLOR_TEXT_DISABLED:  return schultz_rgb(0x767882);
    case SCHULTZ_TOKEN_COLOR_FOCUS_RING:     return schultz_rgb(0x1aabab);
    case SCHULTZ_TOKEN_COLOR_SELECTION:
        return schultz_color_rgba(0x1a, 0xab, 0xab, 0x59);
    /*
     * The site declares this red and never puts it on screen, so the role is
     * this toolkit's choice rather than a copy of one. It measures 3.81 on
     * the window, which is a fill and an icon colour rather than a body text
     * one; a theme that sets destructive text in it should darken it first.
     */
    case SCHULTZ_TOKEN_COLOR_DANGER:         return schultz_rgb(0xd94438);
    /*
     * The other theme's page and text, which is what "opposite" means: a
     * tooltip on a dark screen is a white card, and the same tooltip on a
     * light screen is a dark one.
     */
    case SCHULTZ_TOKEN_COLOR_SURFACE_INVERSE: return schultz_rgb(0xffffff);
    case SCHULTZ_TOKEN_COLOR_TEXT_ON_INVERSE: return schultz_rgb(0x1c1c2a);
    /*
     * The accent across the top of a dialog. A band a shade off the surface
     * below it is a band nobody sees on a dark screen; the brand's own
     * colour says "this is the title" at a glance, and the navy that goes on
     * it is the pairing used everywhere else the accent is a fill.
     */
    case SCHULTZ_TOKEN_COLOR_TITLE_BAR:      return schultz_rgb(0x1aabab);
    case SCHULTZ_TOKEN_COLOR_TITLE_TEXT:     return schultz_rgb(0x1a1e2e);
    /*
     * A groove below the page and a grip above it. On a dark screen the
     * moving part is the lighter of the two, which is the way tone says
     * "nearer" everywhere else here.
     */
    case SCHULTZ_TOKEN_COLOR_SCROLL_TRACK:   return schultz_rgb(0x0f1423);
    case SCHULTZ_TOKEN_COLOR_SCROLL_THUMB:   return schultz_rgb(0x3a3e4b);
    /*
     * Black at four tenths. The page under it is already dark, so there is
     * less room between it and black than there would be on a light one, and
     * a weaker shadow than this reads as nothing. This theme carries depth by
     * tone anyway; a shadow here is for the things tone cannot lift, such as
     * a menu standing off a surface of the same colour.
     */
    case SCHULTZ_TOKEN_COLOR_SHADOW:
        return schultz_color_rgba(0u, 0u, 0u, 0x66u);
    case SCHULTZ_TOKEN_COLOR_TRANSPARENT:
        return schultz_color_rgba(0u, 0u, 0u, 0u);
    default:
        /* Deliberately hideous: a bad token must be obvious on screen. */
        return schultz_color_rgba(255u, 0u, 255u, 255u);
    }
}

static float schultz_token_number_default(uint32_t token)
{
    switch (token) {
    case SCHULTZ_TOKEN_SPACE_XS:         return 2.0f;
    case SCHULTZ_TOKEN_SPACE_SM:         return 4.0f;
    case SCHULTZ_TOKEN_SPACE_MD:         return 8.0f;
    case SCHULTZ_TOKEN_SPACE_LG:         return 16.0f;
    case SCHULTZ_TOKEN_SPACE_XL:         return 32.0f;
    /*
     * The shape scale, split by how close a thing is to the finger rather
     * than by how big it is.
     *
     *   STRUCTURE      square: panels, cards, group boxes, dialogs, menus,
     *                  popovers, toolbars, status bars, and the full width
     *                  rows in lists, trees, menus and accordions
     *   CONTROL_SMALL  a checkbox's box, and a text field
     *   CONTROL        a button, and everything built out of one
     *
     * Rounding is what marks the things a finger presses, so spending it
     * everywhere spends it on nothing. A row in a list answers a tap but is
     * still structure: it is a full width band in a grid, not an object
     * sitting on a surface, and the things that read as data stay square.
     */
    case SCHULTZ_TOKEN_RADIUS_STRUCTURE:     return 0.0f;
    case SCHULTZ_TOKEN_RADIUS_CONTROL_SMALL: return 4.0f;
    case SCHULTZ_TOKEN_RADIUS_CONTROL:       return 8.0f;
    /* One pixel. A hairline is the whole of the border on a phone, and a
     * heavier one turns a screen into a set of boxes. */
    case SCHULTZ_TOKEN_BORDER_WIDTH:     return 1.0f;
    case SCHULTZ_TOKEN_FOCUS_RING_WIDTH: return 2.0f;
    case SCHULTZ_TOKEN_CONTROL_HEIGHT:   return 44.0f;
    case SCHULTZ_TOKEN_FONT_SIZE_SM:     return 12.0f;
    case SCHULTZ_TOKEN_FONT_SIZE_BODY:   return 16.0f;
    /*
     * The same size as body text, and set apart by weight instead. The only
     * title the toolkit draws is a dialog's, which sits in a strip in its own
     * tone: the band is already saying that a heading is a heading, so the
     * size does not have to say it a second time. Larger text in a strip that
     * short reads as content rather than as the chrome around it.
     *
     * A theme that wants a bigger title raises this without touching body
     * text, which is what a separate token is for.
     */
    case SCHULTZ_TOKEN_FONT_SIZE_TITLE:  return 16.0f;
    /*
     * The font's own line height, unchanged. Every label in a form is laid
     * out against this, so a theme that wants roomier prose raises it knowing
     * that it moves captions and list rows with it.
     */
    case SCHULTZ_TOKEN_LINE_SPACING:     return 1.0f;
    default:                             return 0.0f;
    }
}

/* -------------------------------------------------------- property table */

/** @brief One property's fixed characteristics. */
typedef struct {
    const char *name;    /**< For debugging and error messages. */
    uint8_t     kind;    /**< SCHULTZ_VALUE_*. */
    uint8_t     layout;  /**< Nonzero when a change forces a relayout. */
    uint8_t     inherit; /**< Nonzero when the property is inherited. */
    uint16_t    token;   /**< Default token, or 0xFFFF for a literal default. */
    float       number;  /**< Literal default, for number properties. */
} schultz_property_info;

enum { SCHULTZ_NO_TOKEN = 0xFFFFu };

/*
 * The single table that says what every property is. Adding a property means
 * adding one row here; nothing else needs to know about it.
 */
static const schultz_property_info schultz_properties[SCHULTZ_PROP_COUNT] = {
    /* name                 kind                  lay inh token                             default */
    { "background",         SCHULTZ_VALUE_COLOR,  0, 0, SCHULTZ_TOKEN_COLOR_TRANSPARENT,   0.0f },
    { "border.color",       SCHULTZ_VALUE_COLOR,  0, 0, SCHULTZ_TOKEN_COLOR_BORDER,        0.0f },
    { "corner.radius",      SCHULTZ_VALUE_NUMBER, 0, 0, SCHULTZ_TOKEN_RADIUS_STRUCTURE,    0.0f },
    { "text.color",         SCHULTZ_VALUE_COLOR,  0, 1, SCHULTZ_TOKEN_COLOR_TEXT,          0.0f },
    { "focus.ring.color",   SCHULTZ_VALUE_COLOR,  0, 0, SCHULTZ_TOKEN_COLOR_FOCUS_RING,    0.0f },
    { "selection.color",    SCHULTZ_VALUE_COLOR,  0, 0, SCHULTZ_TOKEN_COLOR_SELECTION,     0.0f },
    { "opacity",            SCHULTZ_VALUE_NUMBER, 0, 0, SCHULTZ_NO_TOKEN,                  1.0f },
    { "shadow.color",       SCHULTZ_VALUE_COLOR,  0, 0, SCHULTZ_TOKEN_COLOR_TRANSPARENT,   0.0f },
    { "shadow.angle",       SCHULTZ_VALUE_NUMBER, 0, 0, SCHULTZ_NO_TOKEN,                180.0f },
    { "shadow.distance",    SCHULTZ_VALUE_NUMBER, 0, 0, SCHULTZ_NO_TOKEN,                  0.0f },
    { "shadow.blur",        SCHULTZ_VALUE_NUMBER, 0, 0, SCHULTZ_NO_TOKEN,                  0.0f },

    { "padding",            SCHULTZ_VALUE_NUMBER, 1, 0, SCHULTZ_NO_TOKEN,                  0.0f },
    { "gap",                SCHULTZ_VALUE_NUMBER, 1, 0, SCHULTZ_NO_TOKEN,                  0.0f },
    { "border.width",       SCHULTZ_VALUE_NUMBER, 1, 0, SCHULTZ_NO_TOKEN,                  0.0f },
    { "font",               SCHULTZ_VALUE_FONT,   1, 1, SCHULTZ_TOKEN_FONT_BODY,           0.0f },
    { "font.size",          SCHULTZ_VALUE_NUMBER, 1, 1, SCHULTZ_TOKEN_FONT_SIZE_BODY,      0.0f },
    { "min.width",          SCHULTZ_VALUE_NUMBER, 1, 0, SCHULTZ_NO_TOKEN,                  0.0f },
    { "min.height",         SCHULTZ_VALUE_NUMBER, 1, 0, SCHULTZ_NO_TOKEN,                  0.0f },
    { "pref.width",         SCHULTZ_VALUE_NUMBER, 1, 0, SCHULTZ_NO_TOKEN,                 -1.0f },
    { "pref.height",        SCHULTZ_VALUE_NUMBER, 1, 0, SCHULTZ_NO_TOKEN,                 -1.0f },
    { "max.width",          SCHULTZ_VALUE_NUMBER, 1, 0, SCHULTZ_NO_TOKEN,                 -1.0f },
    { "max.height",         SCHULTZ_VALUE_NUMBER, 1, 0, SCHULTZ_NO_TOKEN,                 -1.0f },
    { "border.dash",        SCHULTZ_VALUE_DASH,   0, 0, SCHULTZ_NO_TOKEN,                  0.0f },
    { "border.cap",         SCHULTZ_VALUE_NUMBER, 0, 0, SCHULTZ_NO_TOKEN,                  0.0f },
    { "border.join",        SCHULTZ_VALUE_NUMBER, 0, 0, SCHULTZ_NO_TOKEN,                  0.0f },
    { "border.dash.offset", SCHULTZ_VALUE_NUMBER, 0, 0, SCHULTZ_NO_TOKEN,                  0.0f },
    /* Four, which is where SVG and the rasterizer both start. */
    { "border.miter.limit", SCHULTZ_VALUE_NUMBER, 0, 0, SCHULTZ_NO_TOKEN,                  4.0f },

    /*
     * Both inherit, so a page can set its prose style once on the container
     * holding it rather than on every paragraph, which is what text.color and
     * font already do and what a reader of CSS would expect.
     */
    { "text.align",         SCHULTZ_VALUE_NUMBER, 0, 1, SCHULTZ_NO_TOKEN,                  0.0f },
    { "line.spacing",      SCHULTZ_VALUE_NUMBER, 1, 1, SCHULTZ_TOKEN_LINE_SPACING,      0.0f }
};

uint32_t schultz_property_kind(uint32_t property)
{
    if (property >= SCHULTZ_PROP_COUNT) {
        return SCHULTZ_VALUE_NUMBER;
    }
    return schultz_properties[property].kind;
}

int32_t schultz_property_affects_layout(uint32_t property)
{
    if (property >= SCHULTZ_PROP_COUNT) {
        return 0;
    }
    return schultz_properties[property].layout ? 1 : 0;
}

int32_t schultz_property_inherits(uint32_t property)
{
    if (property >= SCHULTZ_PROP_COUNT) {
        return 0;
    }
    return schultz_properties[property].inherit ? 1 : 0;
}

const char *schultz_property_name(uint32_t property)
{
    if (property >= SCHULTZ_PROP_COUNT) {
        return NULL;
    }
    return schultz_properties[property].name;
}

/* ---------------------------------------------------------------- theme */

int32_t schultz_theme_init(schultz_theme *theme)
{
    if (theme == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    memset(theme, 0, sizeof(*theme));
    return SCHULTZ_OK;
}

int32_t schultz_theme_preset_light(schultz_theme *theme)
{
    int32_t result = schultz_theme_init(theme);

    if (result != SCHULTZ_OK) {
        return result;
    }
    /*
     * The same ladder as the dark theme, walked the other way: a surface
     * that is further from the page is whiter, not darker. Without shadows
     * that is the only thing left to say "above" with.
     *
     *   sunken  L*89.7   window L*96.1   surface L*97.9   raised L*100
     *
     * All four are greys the site already uses, in the order it uses them:
     * the line under a card, the tone a section is laid on, the near white
     * of a panel, and the white of the page.
     */
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_SURFACE_SUNKEN,
                            schultz_rgb(0xdee2e6));
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_WINDOW,
                            schultz_rgb(0xf2f4f7));
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_SURFACE,
                            schultz_rgb(0xf8f9fa));
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_SURFACE_RAISED,
                            schultz_rgb(0xffffff));
    /*
     * Lighter than the dark theme's, at a fifth rather than four tenths.
     * There is the whole range between white and black to fall through here,
     * so a soft shadow is enough, and a heavy one on a near white page looks
     * like dirt rather than like height.
     */
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_SHADOW,
                            schultz_color_rgba(0u, 0u, 0u, 0x33u));
    /*
     * The same teal as the dark theme, because it is the brand's colour and
     * a selected row in one theme should not be a different green from the
     * other. What changes with the theme is the ladder it sits on, not the
     * accent standing on it.
     *
     * White will not go on it: that measures 2.82 to 1. The navy does, at
     * 5.88, which is the pairing the dark theme already uses, so both themes
     * put the same two colours together for the same job.
     */
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_ACCENT,
                            schultz_rgb(0x1aabab));
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_BORDER,
                            schultz_rgb(0xe0e3ea));
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_TEXT,
                            schultz_rgb(0x1c1c2a));
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_TEXT_MUTED,
                            schultz_rgb(0x6b6e82));
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_TEXT_ON_ACCENT,
                            schultz_rgb(0x1a1e2e));
    /* The muted tone taken most of the way to the window it sits on. */
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_TEXT_DISABLED,
                            schultz_rgb(0x9a9dab));
    /*
     * The same teal again, so that everything the theme puts forward is one
     * colour. It measures 2.56 against the window, under the 3 a mark that
     * is not text is asked for, and it is kept anyway: the ring is two units
     * of a strongly coloured stroke against a near neutral grey, which the
     * eye separates by hue rather than by lightness, and a ring in a second
     * green would be read as a different kind of mark rather than as the
     * same interface paying attention.
     */
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_FOCUS_RING,
                            schultz_rgb(0x1aabab));
    /* The bright teal for the wash behind selected text: it is a tint over
     * what it covers rather than something read on its own. */
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_SELECTION,
                            schultz_color_rgba(0x1a, 0xab, 0xab, 0x40));
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_DANGER,
                            schultz_rgb(0xd94438));
    /* The opposite of this page, which is the other theme's page. */
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_SURFACE_INVERSE,
                            schultz_rgb(0x1a1e2e));
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_TEXT_ON_INVERSE,
                            schultz_rgb(0xffffff));
    /*
     * A tone off the surface, not the accent. On a light screen the step is
     * plainly visible, and a band of brand colour across every dialog would
     * be shouting where a quiet strip already says what it needs to.
     */
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_TITLE_BAR,
                            schultz_rgb(0xdee2e6));
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_TITLE_TEXT,
                            schultz_rgb(0x1c1c2a));
    /*
     * The other way round from the dark theme: on a light screen the grip is
     * the darker of the two, because tone says "nearer" by moving away from
     * the page and the page is at the top of the scale here.
     *
     * The grip is the grey a dialog's title bar is set in, so the two greys
     * a light screen shows on top of the page are the same grey. The groove
     * steps back to the page's own tone rather than competing with it: with
     * the grip carrying the colour, a second band beside it is one line too
     * many.
     */
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_SCROLL_TRACK,
                            schultz_rgb(0xf2f4f7));
    schultz_theme_set_color(theme, SCHULTZ_TOKEN_COLOR_SCROLL_THUMB,
                            schultz_rgb(0xdee2e6));
    return SCHULTZ_OK;
}

int32_t schultz_theme_set_color(schultz_theme *theme, uint32_t token,
                                schultz_color color)
{
    if (theme == NULL || token >= SCHULTZ_TOKEN_COLOR_COUNT) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    theme->colors[token] = color;
    theme->color_set |= (1u << token);
    return SCHULTZ_OK;
}

int32_t schultz_theme_set_number(schultz_theme *theme, uint32_t token,
                                 float value)
{
    if (theme == NULL || token >= SCHULTZ_TOKEN_NUMBER_COUNT) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    theme->numbers[token] = value;
    theme->number_set |= (1u << token);
    return SCHULTZ_OK;
}

int32_t schultz_theme_set_font(schultz_theme *theme, uint32_t token,
                               schultz_handle font)
{
    if (theme == NULL || token >= SCHULTZ_TOKEN_FONT_COUNT) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    theme->fonts[token] = font;
    theme->font_set |= (1u << token);
    return SCHULTZ_OK;
}

schultz_color schultz_theme_color(const schultz_theme *theme, uint32_t token)
{
    if (token >= SCHULTZ_TOKEN_COLOR_COUNT) {
        return schultz_color_rgba(255u, 0u, 255u, 255u);
    }
    if (theme != NULL && (theme->color_set & (1u << token))) {
        return theme->colors[token];
    }
    return schultz_token_color_default(token);
}

float schultz_theme_number(const schultz_theme *theme, uint32_t token)
{
    if (token >= SCHULTZ_TOKEN_NUMBER_COUNT) {
        return 0.0f;
    }
    if (theme != NULL && (theme->number_set & (1u << token))) {
        return theme->numbers[token];
    }
    return schultz_token_number_default(token);
}

schultz_handle schultz_theme_font(const schultz_theme *theme, uint32_t token)
{
    if (token >= SCHULTZ_TOKEN_FONT_COUNT) {
        return SCHULTZ_HANDLE_NONE;
    }
    if (theme != NULL && (theme->font_set & (1u << token))) {
        return theme->fonts[token];
    }
    /*
     * Empty. A theme on its own knows nothing about a font system, so it
     * cannot reach the faces compiled into the library; a tree fills its own
     * copy of the theme from them when it is given both, which is what makes
     * an unfilled slot draw text anyway. See schultz_tree_set_font_system.
     */
    return SCHULTZ_HANDLE_NONE;
}

/* ---------------------------------------------------------------- patch */

int32_t schultz_patch_init(schultz_patch *patch)
{
    if (patch == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    patch->entries  = NULL;
    patch->count    = 0;
    patch->capacity = 0;
    return SCHULTZ_OK;
}

void schultz_patch_free(schultz_patch *patch)
{
    if (patch == NULL) {
        return;
    }
    free(patch->entries);
    patch->entries  = NULL;
    patch->count    = 0;
    patch->capacity = 0;
}

void schultz_patch_clear(schultz_patch *patch)
{
    if (patch != NULL) {
        patch->count = 0;
    }
}

int32_t schultz_patch_set(schultz_patch *patch, uint32_t property,
                          schultz_value value)
{
    uint32_t i;

    if (patch == NULL || property >= SCHULTZ_PROP_COUNT) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    /* Replace in place when the property is already set, so a patch never
     * holds two settings for one property. */
    for (i = 0; i < patch->count; i++) {
        if (patch->entries[i].property == (uint16_t)property) {
            patch->entries[i].value = value;
            return SCHULTZ_OK;
        }
    }

    if (patch->count == patch->capacity) {
        uint32_t capacity = (patch->capacity == 0) ? 4u : patch->capacity * 2u;
        schultz_patch_entry *grown = (schultz_patch_entry *)realloc(
            patch->entries, (size_t)capacity * sizeof(*grown));
        if (grown == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        patch->entries  = grown;
        patch->capacity = capacity;
    }

    patch->entries[patch->count].property = (uint16_t)property;
    patch->entries[patch->count].value    = value;
    patch->count++;
    return SCHULTZ_OK;
}

int32_t schultz_patch_unset(schultz_patch *patch, uint32_t property)
{
    uint32_t i;

    if (patch == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < patch->count; i++) {
        if (patch->entries[i].property == (uint16_t)property) {
            memmove(&patch->entries[i], &patch->entries[i + 1],
                    (size_t)(patch->count - i - 1) * sizeof(*patch->entries));
            patch->count--;
            return SCHULTZ_OK;
        }
    }
    return SCHULTZ_OK;
}

int32_t schultz_patch_get(const schultz_patch *patch, uint32_t property,
                          schultz_value *out_value)
{
    uint32_t i;

    if (patch == NULL || out_value == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < patch->count; i++) {
        if (patch->entries[i].property == (uint16_t)property) {
            *out_value = patch->entries[i].value;
            return SCHULTZ_OK;
        }
    }
    return SCHULTZ_ERR_INVALID_HANDLE;
}

uint32_t schultz_patch_count(const schultz_patch *patch)
{
    return (patch == NULL) ? 0 : patch->count;
}

int32_t schultz_patch_merge(schultz_patch *dest, const schultz_patch *source)
{
    uint32_t i;

    if (dest == NULL || source == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < source->count; i++) {
        int32_t result = schultz_patch_set(dest,
                                           source->entries[i].property,
                                           source->entries[i].value);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    return SCHULTZ_OK;
}

/* --------------------------------------------------------- value makers */

schultz_value schultz_value_color(schultz_color color)
{
    schultz_value value;
    memset(&value, 0, sizeof(value));
    value.source        = SCHULTZ_SOURCE_LITERAL;
    value.literal.color = color;
    return value;
}

schultz_value schultz_value_number(float number)
{
    schultz_value value;
    memset(&value, 0, sizeof(value));
    value.source         = SCHULTZ_SOURCE_LITERAL;
    value.literal.number = number;
    return value;
}

schultz_value schultz_value_font(schultz_handle font)
{
    schultz_value value;
    memset(&value, 0, sizeof(value));
    value.source       = SCHULTZ_SOURCE_LITERAL;
    value.literal.font = font;
    return value;
}

schultz_value schultz_value_gradient(schultz_handle gradient)
{
    schultz_value value;
    memset(&value, 0, sizeof(value));
    value.source           = SCHULTZ_SOURCE_LITERAL;
    value.kind             = SCHULTZ_VALUE_GRADIENT;
    value.literal.gradient = gradient;
    return value;
}

schultz_value schultz_value_dash(schultz_handle dash)
{
    schultz_value value;
    memset(&value, 0, sizeof(value));
    value.source       = SCHULTZ_SOURCE_LITERAL;
    value.kind         = SCHULTZ_VALUE_DASH;
    value.literal.dash = dash;
    return value;
}

schultz_value schultz_value_token(uint32_t token)
{
    schultz_value value;
    memset(&value, 0, sizeof(value));
    value.source = SCHULTZ_SOURCE_TOKEN;
    value.token  = (uint16_t)token;
    return value;
}

/* ------------------------------------------------------------- resolved */

int32_t schultz_resolved_defaults(schultz_resolved_style *resolved,
                                  const schultz_theme *theme)
{
    uint32_t i;

    if (resolved == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    memset(resolved, 0, sizeof(*resolved));

    for (i = 0; i < SCHULTZ_PROP_COUNT; i++) {
        const schultz_property_info *info = &schultz_properties[i];

        switch (info->kind) {
        case SCHULTZ_VALUE_COLOR:
            resolved->values[i].paint = schultz_paint_solid(
                (info->token == SCHULTZ_NO_TOKEN)
                    ? schultz_color_rgba(0u, 0u, 0u, 0u)
                    : schultz_theme_color(theme, info->token));
            break;
        case SCHULTZ_VALUE_FONT:
            resolved->values[i].font =
                (info->token == SCHULTZ_NO_TOKEN)
                    ? SCHULTZ_HANDLE_NONE
                    : schultz_theme_font(theme, info->token);
            break;
        case SCHULTZ_VALUE_DASH:
            resolved->values[i].dash = SCHULTZ_HANDLE_NONE;
            break;
        case SCHULTZ_VALUE_NUMBER:
        default:
            resolved->values[i].number =
                (info->token == SCHULTZ_NO_TOKEN)
                    ? info->number
                    : schultz_theme_number(theme, info->token);
            break;
        }
    }
    return SCHULTZ_OK;
}

int32_t schultz_resolved_apply(schultz_resolved_style *resolved,
                               const schultz_patch *patch,
                               const schultz_theme *theme)
{
    uint32_t i;

    if (resolved == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (patch == NULL) {
        return SCHULTZ_OK;
    }

    for (i = 0; i < patch->count; i++) {
        uint32_t property = patch->entries[i].property;
        const schultz_value *value = &patch->entries[i].value;
        uint32_t kind;

        if (property >= SCHULTZ_PROP_COUNT) {
            continue;
        }
        kind = schultz_properties[property].kind;

        /*
         * A token reference is looked up here, as it is applied, so the
         * resolved struct holds no indirection for the renderer to chase.
         */
        if (value->source == SCHULTZ_SOURCE_TOKEN) {
            switch (kind) {
            case SCHULTZ_VALUE_COLOR:
                resolved->values[property].paint = schultz_paint_solid(
                    schultz_theme_color(theme, value->token));
                break;
            case SCHULTZ_VALUE_FONT:
                resolved->values[property].font =
                    schultz_theme_font(theme, value->token);
                break;
            case SCHULTZ_VALUE_DASH:
                /* There are no dash tokens, so a token means no dashes. */
                resolved->values[property].dash = SCHULTZ_HANDLE_NONE;
                break;
            case SCHULTZ_VALUE_NUMBER:
            default:
                resolved->values[property].number =
                    schultz_theme_number(theme, value->token);
                break;
            }
        } else {
            switch (kind) {
            case SCHULTZ_VALUE_COLOR:
                /*
                 * A colour property takes either kind of value: a flat
                 * colour, or a gradient standing in its place.
                 */
                resolved->values[property].paint =
                    (value->kind == SCHULTZ_VALUE_GRADIENT)
                        ? schultz_paint_gradient(value->literal.gradient)
                        : schultz_paint_solid(value->literal.color);
                break;
            case SCHULTZ_VALUE_FONT:
                resolved->values[property].font = value->literal.font;
                break;
            case SCHULTZ_VALUE_DASH:
                resolved->values[property].dash = value->literal.dash;
                break;
            case SCHULTZ_VALUE_NUMBER:
            default:
                resolved->values[property].number = value->literal.number;
                break;
            }
        }
    }
    return SCHULTZ_OK;
}

schultz_paint schultz_resolved_paint(const schultz_resolved_style *resolved,
                                     uint32_t property)
{
    if (resolved == NULL || property >= SCHULTZ_PROP_COUNT ||
        schultz_properties[property].kind != SCHULTZ_VALUE_COLOR) {
        return schultz_paint_solid(schultz_color_rgba(255u, 0u, 255u, 255u));
    }
    return resolved->values[property].paint;
}

schultz_color schultz_resolved_color(const schultz_resolved_style *resolved,
                                     uint32_t property)
{
    schultz_paint paint = schultz_resolved_paint(resolved, property);

    /*
     * A gradient has no one colour. Callers that can only use a colour, text
     * being the main one, get the first stop's worth of nothing rather than a
     * guess: transparent, so nothing is drawn in a colour nobody chose.
     */
    return (paint.kind == SCHULTZ_PAINT_SOLID)
               ? paint.as.color : schultz_color_rgba(0u, 0u, 0u, 0u);
}

schultz_handle schultz_resolved_dash(const schultz_resolved_style *resolved,
                                     uint32_t property)
{
    if (resolved == NULL || property >= SCHULTZ_PROP_COUNT ||
        schultz_properties[property].kind != SCHULTZ_VALUE_DASH) {
        return SCHULTZ_HANDLE_NONE;
    }
    return resolved->values[property].dash;
}

float schultz_resolved_number(const schultz_resolved_style *resolved,
                              uint32_t property)
{
    if (resolved == NULL || property >= SCHULTZ_PROP_COUNT ||
        schultz_properties[property].kind != SCHULTZ_VALUE_NUMBER) {
        return 0.0f;
    }
    return resolved->values[property].number;
}

schultz_handle schultz_resolved_font(const schultz_resolved_style *resolved,
                                     uint32_t property)
{
    if (resolved == NULL || property >= SCHULTZ_PROP_COUNT ||
        schultz_properties[property].kind != SCHULTZ_VALUE_FONT) {
        return SCHULTZ_HANDLE_NONE;
    }
    return resolved->values[property].font;
}

/* True when one property differs between two resolved styles. */
static int32_t schultz_resolved_property_differs(
    const schultz_resolved_style *a, const schultz_resolved_style *b,
    uint32_t property)
{
    switch (schultz_properties[property].kind) {
    case SCHULTZ_VALUE_COLOR: {
        schultz_paint one = a->values[property].paint;
        schultz_paint two = b->values[property].paint;

        if (one.kind != two.kind) {
            return 1;
        }
        return (one.kind == SCHULTZ_PAINT_GRADIENT)
                   ? ((one.as.gradient == two.as.gradient) ? 0 : 1)
                   : (schultz_color_equals(one.as.color, two.as.color) ? 0
                                                                       : 1);
    }
    case SCHULTZ_VALUE_FONT:
        return (a->values[property].font == b->values[property].font) ? 0 : 1;
    case SCHULTZ_VALUE_DASH:
        return (a->values[property].dash == b->values[property].dash) ? 0 : 1;
    case SCHULTZ_VALUE_NUMBER:
    default:
        return (a->values[property].number == b->values[property].number)
                   ? 0 : 1;
    }
}

int32_t schultz_resolved_layout_differs(const schultz_resolved_style *a,
                                        const schultz_resolved_style *b)
{
    uint32_t i;

    if (a == NULL || b == NULL) {
        return 1;
    }
    for (i = 0; i < SCHULTZ_PROP_COUNT; i++) {
        if (schultz_properties[i].layout &&
            schultz_resolved_property_differs(a, b, i)) {
            return 1;
        }
    }
    return 0;
}

int32_t schultz_resolved_differs(const schultz_resolved_style *a,
                                 const schultz_resolved_style *b)
{
    uint32_t i;

    if (a == NULL || b == NULL) {
        return 1;
    }
    for (i = 0; i < SCHULTZ_PROP_COUNT; i++) {
        if (schultz_resolved_property_differs(a, b, i)) {
            return 1;
        }
    }
    return 0;
}
