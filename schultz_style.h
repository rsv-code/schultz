/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_style.h
 * @brief Tokens, properties, style patches and themes.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 *
 * Four concepts, from the bottom up:
 *
 *   - A **token** is a named value such as `color.accent` or `space.md`.
 *     Every token has a value compiled into the toolkit, so a lookup can
 *     never find a hole.
 *   - A **theme** is a sparse override table over those defaults. An empty
 *     theme is valid and means "use the defaults throughout".
 *   - A **property** is something a node can set: background, padding, text
 *     colour. Each one declares whether changing it costs a repaint or a
 *     relayout.
 *   - A **patch** is a sparse set of property values. Styles, inline node
 *     style and per state style are all patches.
 *
 * A property value is either a literal or a reference to a token. The token
 * form is what lets a style follow a theme change; a literal will not.
 */

#ifndef SCHULTZ_STYLE_H
#define SCHULTZ_STYLE_H

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


/** @brief Named colours in a theme. */
enum {
    SCHULTZ_TOKEN_COLOR_WINDOW = 0,     /**< Behind everything. */
    SCHULTZ_TOKEN_COLOR_SURFACE,        /**< A panel or control background. */
    SCHULTZ_TOKEN_COLOR_SURFACE_RAISED, /**< A surface above another surface. */
    SCHULTZ_TOKEN_COLOR_SURFACE_SUNKEN, /**< A surface pressed into another. */
    SCHULTZ_TOKEN_COLOR_ACCENT,         /**< The primary action colour. */
    SCHULTZ_TOKEN_COLOR_BORDER,         /**< Outline of a control. */
    SCHULTZ_TOKEN_COLOR_TEXT,           /**< Body text. */
    SCHULTZ_TOKEN_COLOR_TEXT_MUTED,     /**< Secondary text. */
    SCHULTZ_TOKEN_COLOR_TEXT_ON_ACCENT, /**< Text drawn on the accent colour. */
    SCHULTZ_TOKEN_COLOR_TEXT_DISABLED,  /**< Text that cannot be used. */
    SCHULTZ_TOKEN_COLOR_FOCUS_RING,     /**< Ring around the focused control. */
    SCHULTZ_TOKEN_COLOR_SELECTION,      /**< Selected text or list row. */
    SCHULTZ_TOKEN_COLOR_DANGER,         /**< Destructive action. */
    /**
     * A surface deliberately opposite to the rest of the page, and the text
     * that goes on it. Dark where the page is light and light where it is
     * dark, so that something laid over the page reads as being in front of
     * it rather than part of it. Tooltips and popovers use it.
     */
    SCHULTZ_TOKEN_COLOR_SURFACE_INVERSE,
    SCHULTZ_TOKEN_COLOR_TEXT_ON_INVERSE,
    /** The band across the top of a dialog, and the title written on it. */
    SCHULTZ_TOKEN_COLOR_TITLE_BAR,
    SCHULTZ_TOKEN_COLOR_TITLE_TEXT,
    /**
     * A scroll bar's groove, and the part of it that moves. These are their
     * own colours rather than borrowed ones because the two have to stay
     * apart from each other, and a token that means something else is free
     * to drift until they meet.
     */
    SCHULTZ_TOKEN_COLOR_SCROLL_TRACK,
    SCHULTZ_TOKEN_COLOR_SCROLL_THUMB,
    /**
     * The colour a drop shadow is drawn in. Black at an alpha, rather than a
     * grey, so that it darkens whatever it falls on instead of tinting it.
     *
     * It is a theme's colour because how heavy a shadow is says as much about
     * a design as a surface tone does, and because a shadow needs a different
     * strength on a dark page than on a light one to read as the same depth.
     *
     * Nothing carries a shadow unless it is asked to: SCHULTZ_PROP_SHADOW_COLOR
     * still defaults to transparent, and this is the colour to reach for when
     * a host wants one.
     */
    SCHULTZ_TOKEN_COLOR_SHADOW,
    SCHULTZ_TOKEN_COLOR_TRANSPARENT,    /**< Fully transparent. */

    SCHULTZ_TOKEN_COLOR_COUNT           /**< How many colour tokens exist. */
};

/** @brief Named lengths in a theme, in pixels. */
enum {
    SCHULTZ_TOKEN_SPACE_XS = 0,        /**< Tightest spacing step. */
    SCHULTZ_TOKEN_SPACE_SM,            /**< Small spacing step. */
    SCHULTZ_TOKEN_SPACE_MD,            /**< Default spacing step. */
    SCHULTZ_TOKEN_SPACE_LG,            /**< Large spacing step. */
    SCHULTZ_TOKEN_SPACE_XL,            /**< Largest spacing step. */
    /*
     * The shape scale is split by how close a thing is to the finger, not by
     * how big it is. When every surface shares one radius, a button and the
     * card behind it read as the same kind of object and the thing meant to
     * be pressed stops standing out. So structure is square and the objects
     * you press are round, and the contrast between them is what says which
     * is which.
     *
     * Depth is carried by surface tone, so the radius does not have to carry
     * it as well.
     */
    /** Square. Surfaces, containers, and rows: what you look at. */
    SCHULTZ_TOKEN_RADIUS_STRUCTURE,
    /** Rounding for a small pressed object, such as a checkbox's box. */
    SCHULTZ_TOKEN_RADIUS_CONTROL_SMALL,
    /** Rounding for an ordinary pressed object, such as a button. */
    SCHULTZ_TOKEN_RADIUS_CONTROL,
    SCHULTZ_TOKEN_BORDER_WIDTH,        /**< Default control outline width. */
    SCHULTZ_TOKEN_FOCUS_RING_WIDTH,    /**< Focus ring width. */
    /**
     * Smallest height a control may have. This is the published touch target
     * minimum on both mobile platforms, so it is an accessibility floor
     * rather than a style preference.
     */
    SCHULTZ_TOKEN_CONTROL_HEIGHT,
    SCHULTZ_TOKEN_FONT_SIZE_SM,        /**< Caption text size. */
    SCHULTZ_TOKEN_FONT_SIZE_BODY,      /**< Body text size. */
    SCHULTZ_TOKEN_FONT_SIZE_TITLE,     /**< Heading text size. */
    /**
     * How far apart lines of wrapped text sit, as a multiple of the font's
     * own line height. Prose needs more air between lines than a caption
     * does, and the font's own figure is the caption's.
     */
    SCHULTZ_TOKEN_LINE_SPACING,

    SCHULTZ_TOKEN_NUMBER_COUNT         /**< How many number tokens exist. */
};

/**
 * @brief Named font slots in a theme.
 *
 * A slot the application does not fill falls back to the matching face
 * compiled into the library, so text draws whether or not a host has said
 * anything about fonts. See schultz_font_builtin.
 */
enum {
    SCHULTZ_TOKEN_FONT_BODY = 0, /**< Default text. */
    SCHULTZ_TOKEN_FONT_TITLE,    /**< Headings. */
    SCHULTZ_TOKEN_FONT_MONO,     /**< Fixed pitch text. */
    /**
     * Emoji, which no text face has. This slot is never chosen by a style
     * the way the three above are: it is the face that draws what the chosen
     * one has no glyph for. See schultz_font_set_emoji.
     */
    SCHULTZ_TOKEN_FONT_EMOJI,

    SCHULTZ_TOKEN_FONT_COUNT     /**< How many font slots exist. */
};

/**
 * @brief Where the lines of a wrapped block sit across its width.
 *
 * Start and end rather than left and right, because a line of Arabic starts
 * on the right. For left to right text start is the left edge, and for right
 * to left text it is the right one.
 */
enum {
    SCHULTZ_TEXT_ALIGN_START = 0, /**< Against the leading edge. */
    SCHULTZ_TEXT_ALIGN_CENTER,    /**< Centred in the width. */
    SCHULTZ_TEXT_ALIGN_END,       /**< Against the trailing edge. */
    SCHULTZ_TEXT_ALIGN_JUSTIFY    /**< Spread to both edges, last line not. */
};

/**
 * @brief Everything a node can be styled with.
 *
 * Each property has a fixed type and a fixed invalidation class; see
 * schultz_property_kind and schultz_property_affects_layout.
 */
enum {
    /* Paint properties: changing one repaints, and never relayouts. */
    SCHULTZ_PROP_BACKGROUND = 0,   /**< Fill behind the node. */
    SCHULTZ_PROP_BORDER_COLOR,     /**< Outline colour. */
    SCHULTZ_PROP_CORNER_RADIUS,    /**< Rounded corner radius. */
    SCHULTZ_PROP_TEXT_COLOR,       /**< Text colour. Inherits. */
    SCHULTZ_PROP_FOCUS_RING_COLOR, /**< Focus ring colour. */
    SCHULTZ_PROP_SELECTION_COLOR,  /**< Selection highlight colour. */
    SCHULTZ_PROP_OPACITY,          /**< 0 transparent to 1 opaque. */
    SCHULTZ_PROP_SHADOW_COLOR,     /**< Shadow colour. Clear means none. */
    /**
     * Which way the shadow falls, in degrees clockwise, with 0 above the
     * node. So 90 is to its right, 180 directly below it, 270 to its left.
     * Clockwise to agree with every other angle in the toolkit.
     */
    SCHULTZ_PROP_SHADOW_ANGLE,
    SCHULTZ_PROP_SHADOW_DISTANCE,  /**< How far that way, in units. */
    SCHULTZ_PROP_SHADOW_BLUR,      /**< How soft its edge is. 0 is hard. */

    /* Layout properties: changing one relayouts, and therefore repaints. */
    SCHULTZ_PROP_PADDING,          /**< Inset from this node's own edges. */
    SCHULTZ_PROP_GAP,              /**< Space between adjacent children. */
    SCHULTZ_PROP_BORDER_WIDTH,     /**< Outline width, which insets content. */
    SCHULTZ_PROP_FONT,             /**< Font to draw text with. Inherits. */
    SCHULTZ_PROP_FONT_SIZE,        /**< Text size. Inherits. */
    SCHULTZ_PROP_MIN_WIDTH,        /**< Never narrower than this. */
    SCHULTZ_PROP_MIN_HEIGHT,       /**< Never shorter than this. */
    SCHULTZ_PROP_PREF_WIDTH,       /**< Wanted width, negative to compute. */
    SCHULTZ_PROP_PREF_HEIGHT,      /**< Wanted height, negative to compute. */
    SCHULTZ_PROP_MAX_WIDTH,        /**< Never wider, negative for no limit. */
    SCHULTZ_PROP_MAX_HEIGHT,       /**< Never taller, negative for no limit. */
    SCHULTZ_PROP_BORDER_DASH,      /**< Dash pattern for the outline. */
    SCHULTZ_PROP_BORDER_CAP,       /**< One of the SCHULTZ_CAP_* values. */
    SCHULTZ_PROP_BORDER_JOIN,      /**< One of the SCHULTZ_JOIN_* values. */
    /**
     * How far along the dash pattern the outline starts, in pixels. Drawing
     * the same border at two offsets is how a dashed outline is made to look
     * like it is moving. Ignored when there is no dash pattern.
     */
    SCHULTZ_PROP_BORDER_DASH_OFFSET,
    /**
     * How far a mitred corner may reach past the corner itself, as a
     * multiple of the outline's width. Past this the corner is cut flat
     * instead, which is what stops a very sharp angle running away. Four by
     * default; ignored unless `border.join` is SCHULTZ_JOIN_MITER.
     */
    SCHULTZ_PROP_BORDER_MITER_LIMIT,
    SCHULTZ_PROP_TEXT_ALIGN,       /**< SCHULTZ_TEXT_ALIGN_*. Inherits. */
    SCHULTZ_PROP_LINE_SPACING,     /**< Line height multiplier. Inherits. */

    SCHULTZ_PROP_COUNT             /**< How many properties exist. */
};

/** @brief What type a property holds. */
enum {
    SCHULTZ_VALUE_COLOR = 0, /**< A colour, or a gradient in its place. */
    SCHULTZ_VALUE_NUMBER,    /**< A float in pixels. */
    SCHULTZ_VALUE_FONT,      /**< A loaded font handle. */
    SCHULTZ_VALUE_DASH,      /**< A registered dash pattern handle. */

    /**
     * Not a property kind: the kind of a value handed to a colour property
     * when a gradient is wanted instead of a flat colour. A property is
     * either a colour property or it is not; what fills it is the value's
     * business.
     */
    SCHULTZ_VALUE_GRADIENT
};

/** @brief Where a property value comes from. */
enum {
    SCHULTZ_SOURCE_LITERAL = 0, /**< The value is written in the patch. */
    SCHULTZ_SOURCE_TOKEN        /**< The value is looked up in the theme. */
};

/**
 * @brief One property value: a literal, or a reference to a token.
 *
 * A style that references tokens follows a theme change. One that writes
 * literals does not, which is occasionally what is wanted and usually not.
 */
typedef struct {
    uint8_t  source; /**< SCHULTZ_SOURCE_LITERAL or SCHULTZ_SOURCE_TOKEN. */
    uint16_t token;  /**< Which token, when source is SCHULTZ_SOURCE_TOKEN. */
    uint8_t  kind;   /**< SCHULTZ_VALUE_GRADIENT marks a gradient literal. */
    union {
        schultz_color  color;    /**< When the property holds a colour. */
        float          number;   /**< When the property holds a number. */
        schultz_handle font;     /**< When the property holds a font. */
        schultz_handle gradient; /**< When a colour property takes one. */
        schultz_handle dash;     /**< When the property holds a dash. */
    } literal;       /**< The value, when source is SCHULTZ_SOURCE_LITERAL. */
} schultz_value;

/** @brief One property setting inside a patch. */
typedef struct {
    uint16_t      property; /**< One of the SCHULTZ_PROP_* values. */
    schultz_value value;    /**< What it is set to. */
} schultz_patch_entry;

/**
 * @brief A sparse set of property values.
 *
 * Only the properties actually set appear. A property that is absent does not
 * mean transparent or zero: it means whatever layer is beneath decides.
 */
typedef struct {
    schultz_patch_entry *entries;  /**< Settings, in the order they were set. */
    uint32_t             count;    /**< How many are set. */
    uint32_t             capacity; /**< Slots allocated. */
} schultz_patch;

/**
 * @brief A theme: a sparse override table over the compiled in token values.
 *
 * Plain data with no allocation, so it can live on the stack and be copied. An
 * all zero theme is valid and resolves entirely to the built in defaults.
 */
typedef struct {
    schultz_color  colors[SCHULTZ_TOKEN_COLOR_COUNT];   /**< Overrides. */
    uint32_t       color_set;                           /**< Which are set. */
    float          numbers[SCHULTZ_TOKEN_NUMBER_COUNT]; /**< Overrides. */
    uint32_t       number_set;                          /**< Which are set. */
    schultz_handle fonts[SCHULTZ_TOKEN_FONT_COUNT];     /**< Overrides. */
    uint32_t       font_set;                            /**< Which are set. */
} schultz_theme;

/**
 * @brief A node's fully resolved style: every property, no gaps.
 *
 * This is what drawing code reads, and the only thing it should read. Access
 * it through schultz_resolved_color and friends rather than by indexing.
 */
typedef struct {
    union {
        schultz_paint  paint;  /**< For colour properties. */
        float          number; /**< For number properties. */
        schultz_handle font;   /**< For font properties. */
        schultz_handle dash;   /**< For dash properties. */
    } values[SCHULTZ_PROP_COUNT]; /**< Indexed by property. */
} schultz_resolved_style;

/* ------------------------------------------------------- property table */

/**
 * @brief Returns what type a property holds.
 *
 * @param property One of the SCHULTZ_PROP_* values.
 * @return SCHULTZ_VALUE_COLOR, SCHULTZ_VALUE_NUMBER or SCHULTZ_VALUE_FONT.
 *         An out of range property reports SCHULTZ_VALUE_NUMBER.
 */
uint32_t schultz_property_kind(uint32_t property);

/**
 * @brief Reports whether changing a property forces a relayout.
 *
 * This is the table that keeps a colour change from causing a relayout, which
 * is the whole advantage of retained mode.
 *
 * @param property One of the SCHULTZ_PROP_* values.
 * @return 1 when the property affects layout, 0 when it only affects paint.
 */
int32_t schultz_property_affects_layout(uint32_t property);

/**
 * @brief Reports whether a property is inherited from ancestors.
 *
 * The inheriting set is fixed and short on purpose: text colour, font and
 * font size. A short list keeps resolution cheap and keeps "why is this the
 * wrong colour" answerable by looking at one ancestor chain.
 *
 * @param property One of the SCHULTZ_PROP_* values.
 * @return 1 when the property inherits, 0 otherwise.
 */
int32_t schultz_property_inherits(uint32_t property);

/**
 * @brief Returns a property's name, for debugging and error messages.
 *
 * @param property One of the SCHULTZ_PROP_* values.
 * @return The name, such as "background", or NULL when out of range.
 */
const char *schultz_property_name(uint32_t property);

/* ---------------------------------------------------------------- theme */

/**
 * @brief Clears a theme so every token falls back to its built in value.
 *
 * @param theme The theme to clear. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when theme is NULL.
 */
int32_t schultz_theme_init(schultz_theme *theme);

/**
 * @brief Fills a theme with a light set of colours.
 *
 * The compiled in defaults are a dark look, so this is the one preset worth
 * shipping. It populates a struct and installs nothing; use
 * schultz_tree_set_theme to apply it.
 *
 * @param theme The theme to fill. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when theme is NULL.
 */
int32_t schultz_theme_preset_light(schultz_theme *theme);

/**
 * @brief Overrides one colour token.
 *
 * @param theme The theme to update. Must not be NULL.
 * @param token One of the SCHULTZ_TOKEN_COLOR_* values.
 * @param color The value to use.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT for a NULL theme or an
 *         out of range token.
 */
int32_t schultz_theme_set_color(schultz_theme *theme, uint32_t token,
                                schultz_color color);

/**
 * @brief Overrides one number token.
 *
 * @param theme The theme to update. Must not be NULL.
 * @param token One of the SCHULTZ_TOKEN_SPACE_* and related values.
 * @param value The value to use.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_theme_set_number(schultz_theme *theme, uint32_t token,
                                 float value);

/**
 * @brief Overrides one font slot.
 *
 * @param theme The theme to update. Must not be NULL.
 * @param token One of the SCHULTZ_TOKEN_FONT_* values.
 * @param font  The loaded font to use.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_theme_set_font(schultz_theme *theme, uint32_t token,
                               schultz_handle font);

/**
 * @brief Reads a colour token, falling back to the built in default.
 *
 * @param theme The theme to read, or NULL to read the built in default.
 * @param token One of the SCHULTZ_TOKEN_COLOR_* values.
 * @return The colour. An out of range token yields opaque magenta, so a bad
 *         lookup is obvious on screen rather than passing for a design choice.
 */
schultz_color schultz_theme_color(const schultz_theme *theme, uint32_t token);

/**
 * @brief Reads a number token, falling back to the built in default.
 *
 * @param theme The theme to read, or NULL to read the built in default.
 * @param token One of the number token values.
 * @return The value, or 0 when the token is out of range.
 */
float schultz_theme_number(const schultz_theme *theme, uint32_t token);

/**
 * @brief Reads a font slot, falling back to the built in default.
 *
 * @param theme The theme to read, or NULL to read the built in default.
 * @param token One of the SCHULTZ_TOKEN_FONT_* values.
 * @return The font handle, or SCHULTZ_HANDLE_NONE when unset or out of range.
 */
schultz_handle schultz_theme_font(const schultz_theme *theme, uint32_t token);

/* ---------------------------------------------------------------- patch */

/**
 * @brief Prepares an empty patch.
 *
 * @param patch The patch to initialize. Must not be NULL. Previous contents
 *              are overwritten, so call schultz_patch_free first if it was in
 *              use.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when patch is NULL.
 */
int32_t schultz_patch_init(schultz_patch *patch);

/**
 * @brief Releases a patch's storage.
 *
 * @param patch The patch to release. NULL is accepted and does nothing.
 */
void schultz_patch_free(schultz_patch *patch);

/**
 * @brief Removes every setting from a patch, keeping its storage.
 *
 * @param patch The patch to clear. NULL is accepted and does nothing.
 */
void schultz_patch_clear(schultz_patch *patch);

/**
 * @brief Sets a property to a value, replacing any previous setting.
 *
 * @param patch    The patch to update. Must not be NULL.
 * @param property One of the SCHULTZ_PROP_* values.
 * @param value    What to set it to.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL patch or an out
 *         of range property, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_patch_set(schultz_patch *patch, uint32_t property,
                          schultz_value value);

/**
 * @brief Removes one property from a patch.
 *
 * @param patch    The patch to update. Must not be NULL.
 * @param property The property to unset.
 * @return SCHULTZ_OK whether or not the property was set, or
 *         SCHULTZ_ERR_INVALID_ARGUMENT when patch is NULL.
 */
int32_t schultz_patch_unset(schultz_patch *patch, uint32_t property);

/**
 * @brief Reads one property from a patch.
 *
 * @param patch     The patch to read. Must not be NULL.
 * @param property  The property to look for.
 * @param out_value Receives the value when it is set. Must not be NULL.
 * @return SCHULTZ_OK when the property is set, or SCHULTZ_ERR_INVALID_HANDLE
 *         when it is not.
 */
int32_t schultz_patch_get(const schultz_patch *patch, uint32_t property,
                          schultz_value *out_value);

/**
 * @brief Counts the properties a patch sets.
 *
 * @param patch The patch to query. NULL yields 0.
 * @return How many properties are set.
 */
uint32_t schultz_patch_count(const schultz_patch *patch);

/**
 * @brief Copies every setting from one patch onto another.
 *
 * Settings already in the destination that the source does not mention are
 * left alone; the rest are replaced. This is how the layers stack.
 *
 * @param dest   The patch to update. Must not be NULL.
 * @param source The patch to copy from. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_patch_merge(schultz_patch *dest, const schultz_patch *source);

/* --------------------------------------------------------- value makers */

/**
 * @brief Builds a literal colour value.
 *
 * @param color The colour.
 * @return A value holding it directly, which will not follow a theme change.
 */
schultz_value schultz_value_color(schultz_color color);

/**
 * @brief Builds a literal number value.
 *
 * @param number The value in pixels.
 * @return A value holding it directly.
 */
schultz_value schultz_value_number(float number);

/**
 * @brief Builds a literal font value.
 *
 * @param font The loaded font.
 * @return A value holding it directly.
 */
schultz_value schultz_value_font(schultz_handle font);

/**
 * @brief Builds a value that refers to a theme token.
 *
 * Which token namespace applies follows from the property it is set on: a
 * colour property reads a colour token, a number property a number token.
 *
 * @param token The token index within its namespace.
 * @return A value that resolves through whatever theme is in effect.
 */
schultz_value schultz_value_token(uint32_t token);

/**
 * @brief Makes a value that fills a colour property with a gradient.
 *
 * @param gradient A handle from schultz_gradient_linear or _radial.
 * @return The value.
 */
schultz_value schultz_value_gradient(schultz_handle gradient);

/**
 * @brief Makes a value naming a registered dash pattern.
 *
 * @param dash A handle from schultz_dash_register, or SCHULTZ_HANDLE_NONE
 *             for a solid line.
 * @return The value.
 */
schultz_value schultz_value_dash(schultz_handle dash);

/* ------------------------------------------------------------- resolved */

/**
 * @brief Fills a resolved style with the built in defaults for every property.
 *
 * @param resolved The struct to fill. Must not be NULL.
 * @param theme    The theme to resolve tokens through, or NULL for the built
 *                 in values.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when resolved is NULL.
 */
int32_t schultz_resolved_defaults(schultz_resolved_style *resolved,
                                  const schultz_theme *theme);

/**
 * @brief Applies a patch over a resolved style.
 *
 * Token references are looked up in the theme as they are applied, so the
 * result holds no indirection.
 *
 * @param resolved The struct to update. Must not be NULL.
 * @param patch    The patch to apply. NULL is accepted and does nothing.
 * @param theme    The theme token references resolve through, or NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when resolved is NULL.
 */
int32_t schultz_resolved_apply(schultz_resolved_style *resolved,
                               const schultz_patch *patch,
                               const schultz_theme *theme);

/**
 * @brief Reads a colour property from a resolved style.
 *
 * @param resolved The resolved style. Must not be NULL.
 * @param property A property whose kind is SCHULTZ_VALUE_COLOR.
 * @return The colour, or opaque magenta for a NULL struct or a property that
 *         does not hold a colour.
 */
schultz_color schultz_resolved_color(const schultz_resolved_style *resolved,
                                     uint32_t property);

/**
 * @brief Reads a number property from a resolved style.
 *
 * @param resolved The resolved style. Must not be NULL.
 * @param property A property whose kind is SCHULTZ_VALUE_NUMBER.
 * @return The value, or 0 for a NULL struct or a mismatched property.
 */
float schultz_resolved_number(const schultz_resolved_style *resolved,
                              uint32_t property);

/**
 * @brief Reads a resolved colour property as a paint.
 *
 * This is what drawing code should read: it carries a gradient as readily as
 * a colour. schultz_resolved_color remains for the many places that only ever
 * want a colour, such as text.
 *
 * @param resolved The resolved style. NULL yields a solid magenta, which is
 *                 loud on purpose.
 * @param property A property whose kind is SCHULTZ_VALUE_COLOR.
 * @return The paint.
 */
schultz_paint schultz_resolved_paint(const schultz_resolved_style *resolved,
                                     uint32_t property);

/**
 * @brief Reads a resolved dash property.
 *
 * @param resolved The resolved style. NULL yields SCHULTZ_HANDLE_NONE.
 * @param property A property whose kind is SCHULTZ_VALUE_DASH.
 * @return The dash handle, or SCHULTZ_HANDLE_NONE for a solid line.
 */
schultz_handle schultz_resolved_dash(const schultz_resolved_style *resolved,
                                     uint32_t property);

/**
 * @brief Reads a font property from a resolved style.
 *
 * @param resolved The resolved style. Must not be NULL.
 * @param property A property whose kind is SCHULTZ_VALUE_FONT.
 * @return The font handle, or SCHULTZ_HANDLE_NONE for a NULL struct or a
 *         mismatched property.
 */
schultz_handle schultz_resolved_font(const schultz_resolved_style *resolved,
                                     uint32_t property);

/**
 * @brief Reports whether two resolved styles differ in any layout property.
 *
 * This is what lets resolution decide whether a change costs a relayout or
 * only a repaint.
 *
 * @param a First resolved style. Must not be NULL.
 * @param b Second resolved style. Must not be NULL.
 * @return 1 when any layout property differs, 0 when they agree.
 */
int32_t schultz_resolved_layout_differs(const schultz_resolved_style *a,
                                        const schultz_resolved_style *b);

/**
 * @brief Reports whether two resolved styles differ in any property at all.
 *
 * @param a First resolved style. Must not be NULL.
 * @param b Second resolved style. Must not be NULL.
 * @return 1 when any property differs, 0 when they are identical.
 */
int32_t schultz_resolved_differs(const schultz_resolved_style *a,
                                 const schultz_resolved_style *b);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_STYLE_H */
