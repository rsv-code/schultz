/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_window.h
 * @brief A window: one call opens it, one call draws it.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 *
 * Everything below this header can be driven by hand, and the toolkit's own
 * demo did exactly that: reset an arena, advance the clock, resolve styles,
 * lay out, read the dirty region, paint, play the draw list, present, then
 * process events. Nine calls in a fixed order, and getting the order wrong
 * is a bug that shows up as a blank or torn window.
 *
 * That order is not a host's decision to make, and for a host bound over a
 * foreign function interface every one of those calls costs a crossing. So
 * it lives here instead. A host opens a window, asks it for the tree and
 * fills that tree in once, and then either calls schultz_window_update for
 * one turn of the loop or hands the whole loop over with schultz_window_run.
 *
 * A window owns everything a picture cannot be drawn without: the platform's
 * own window and its pixel buffer, the rasterizer, the scratch arena, the
 * draw list, the font system, the glyph cache, the gradient and dash table,
 * the image table, the widget tree, and the event router. It hands out the
 * ones a host has to fill in.
 *
 * The theme is not among them, because a tree copies the theme it is given
 * rather than pointing at it. A host keeps its own and installs it with
 * schultz_tree_set_theme whenever it changes.
 *
 * There is one window. Opening a second is not supported.
 */

#ifndef SCHULTZ_WINDOW_H
#define SCHULTZ_WINDOW_H

#include "schultz.h"
#include "schultz_event.h"
#include "schultz_font.h"
#include "schultz_glyphs.h"
#include "schultz_image.h"
#include "schultz_node.h"
#include "schultz_resource.h"
#include "schultz_style.h"

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


/** @brief Opaque window state. Open one with schultz_window_create. */
typedef struct schultz_window schultz_window;

/**
 * @brief Which way up a screen may be used.
 *
 * Not SCHULTZ_ORIENT_HORIZONTAL and _VERTICAL, which say which way a
 * separator or a split runs. This is about which way the device is held.
 */
enum {
    SCHULTZ_SCREEN_ANY = 0,  /**< Whichever way the device is turned. */
    SCHULTZ_SCREEN_PORTRAIT, /**< Taller than wide, either way up. */
    SCHULTZ_SCREEN_LANDSCAPE /**< Wider than tall, either way round. */
};

/**
 * @brief How far the picture is turned before it reaches the panel.
 *
 * For a display fitted into its case the other way round, which is a thing
 * that is decided once at a factory and cannot be argued with afterwards.
 * The interface is drawn in the shape it wants and turned on the way out; a
 * pointer is turned back on the way in, so nothing above this notices.
 *
 * Clockwise, as the panel sees it.
 */
enum {
    /** Work it out from `orientation`, and do not turn if that says any. */
    SCHULTZ_TURN_AUTO = 0,
    SCHULTZ_TURN_NONE,       /**< Straight through. */
    SCHULTZ_TURN_90,         /**< A quarter clockwise. */
    SCHULTZ_TURN_180,        /**< Upside down. */
    SCHULTZ_TURN_270         /**< A quarter anticlockwise. */
};

/**
 * @brief How a window should open.
 *
 * Zero every field first, then set the ones that matter. A zeroed struct asks
 * for an 800x600 window named "Schultz" with vertical sync on and no debug
 * tint, which is a working choice rather than a broken one.
 */
typedef struct schultz_window_options {
    const char *title;  /**< Window title. NULL means "Schultz". */
    uint32_t    width;  /**< Window width in pixels. 0 means 800. */
    uint32_t    height; /**< Window height in pixels. 0 means 600. */
    /**
     * Nonzero paces the loop to the display, which is what an application
     * wants. Zero presents as fast as the machine allows, which is what
     * measuring wants. A zeroed struct means zero, so
     * schultz_window_options_init exists to turn it back on.
     */
    int32_t     vsync;
    /**
     * Tints the region being repainted each frame, so the dirty rectangles
     * can be seen. An alpha of zero, which is what a zeroed struct holds,
     * turns it off.
     */
    schultz_color debug_tint;
    /**
     * Nonzero opens the window maximized, filling the desktop's work area.
     * The width and height above become the size the window returns to when
     * it is restored. Ask schultz_window_width and schultz_window_height for
     * the size it actually opened at.
     *
     * Maximizing needs a window a desktop is allowed to resize, so this turns
     * resizable on whatever it was set to.
     */
    int32_t     maximized;
    /**
     * Which SDL video driver to ask for, such as "offscreen" for a window
     * nobody sees or "kmsdrm" for a board with no desktop. NULL, which is
     * what a zeroed struct holds, lets SDL choose, which is right on every
     * machine that has a desktop running.
     *
     * Read once, when the first window opens, because that is when SDL
     * settles the choice. Setting it on a later window does nothing.
     *
     * What the environment says still wins: SDL_VIDEO_DRIVER set by whoever
     * is running the program overrides this. That is the right way round.
     * A program says what it would like; a person running it on hardware
     * nobody anticipated gets the last word.
     */
    const char *video_driver;
    /**
     * Nonzero lets the user drag the window's edges. On by default, because a
     * window a desktop cannot resize is the unusual one.
     *
     * Zero fixes the window at the size it opened at. That is what a kiosk or
     * a fixed layout wants, and it is what a phone gets either way, since the
     * window is the screen there.
     */
    int32_t     resizable;
    /**
     * Nonzero lets the screen turn with the device. On by default.
     *
     * Only phones and tablets turn, so this does nothing on a desktop. It is
     * a separate switch from `resizable` because the two are separate
     * questions that one platform flag happens to answer: a kiosk wants a
     * window nobody can drag the edges of and may still want it to turn, and
     * a window that cannot turn is not thereby fixed in size.
     *
     * The application still has the last word. An Android activity that
     * declares `android:screenOrientation`, or an Info.plist naming one
     * orientation, is not overruled from here: this can only narrow what
     * those already allow.
     */
    int32_t     allow_rotate;
    /**
     * Which way up the screen may be used: SCHULTZ_SCREEN_ANY,
     * SCHULTZ_SCREEN_PORTRAIT or SCHULTZ_SCREEN_LANDSCAPE. Any by default.
     *
     * Naming one of the two holds the screen that way whichever way the
     * device is turned, which is what a kiosk display wants. With
     * `allow_rotate` still on it turns only between the two ways up that
     * match: a portrait screen turns end over end and never onto its side.
     *
     * Phones and tablets only, for the same reason as above. A panel that is
     * physically mounted the other way round is a different problem and this
     * does not solve it; see docs/building.md.
     */
    uint32_t    orientation;
    /**
     * How far to turn the picture on its way to the panel:
     * SCHULTZ_TURN_AUTO, _NONE, _90, _180 or _270. Auto by default.
     *
     * This is the low level of the same idea as `orientation`, and it is
     * what a fixed panel needs. Auto asks `orientation` instead: a portrait
     * interface on a landscape panel turns a quarter, and anything already
     * the right shape does not turn.
     *
     * Naming a turn is the only way to say which quarter, and the only way
     * to say upside down. A board whose display is fitted head down needs
     * SCHULTZ_TURN_180, and no description of the shape wanted can imply it.
     *
     * Ignored where the platform turns the screen itself, which is phones
     * and tablets: there the device does the turning and this would turn it
     * twice.
     */
    uint32_t    turn;
    /**
     * How many worker threads the rasterizer may use. Zero rasterizes on the
     * calling thread, which is reproducible; a small number is faster on a
     * large window.
     */
    uint32_t    threads;
    /**
     * Nonzero describes the window to whatever assistive technology is
     * running, so a screen reader can read it. On by default, because a
     * toolkit that has to be asked for accessibility is one that ships
     * without it.
     *
     * Starting it costs nothing when nothing is listening, and a reader may
     * connect at any time afterwards.
     */
    int32_t     accessible;
    /**
     * The application's name, as a user would recognize it, for the
     * accessibility layer. NULL uses the window title.
     */
    const char *app_name;
    /**
     * Nonzero draws everything larger on a screen that packs more pixels
     * into the same space. On by default.
     *
     * A size is a number of pixels either way. What this decides is which
     * pixels. With it on, one unit is a pixel on an ordinary monitor, and a
     * phone that fits three of its own into that space is given three: a two
     * unit border is two pixels on a desktop and six on the phone, and looks
     * the same weight on both. Everything moves together, so a hand written
     * size and a size out of the theme always agree.
     *
     * With it off, one unit is one pixel of the screen in front of you, and
     * a two unit border is two pixels of it. That is the setting for work
     * that has to land on exact pixels, and it means a host does its own
     * arithmetic: the built in theme is written for an ordinary monitor, so
     * a phone gets a control forty four of its own pixels tall, which is
     * about two millimetres. Ask schultz_window_pixel_scale and multiply, or
     * build a theme with the numbers you want.
     *
     * A screen with nothing to scale is unaffected either way.
     */
    int32_t     scale_to_screen;
} schultz_window_options;

/**
 * @brief Called once per turn of the loop, before anything is drawn.
 *
 * This is where a host moves things: the tree may be changed freely here, and
 * what changes is what gets repainted.
 *
 * @param context The pointer handed to schultz_window_run.
 * @return SCHULTZ_OK to draw. Anything else stops the loop and is
 *         returned from schultz_window_run.
 */
typedef int32_t (*schultz_window_update_fn)(void *context);

/**
 * @brief Fills in the options a zeroed struct cannot express.
 *
 * @param options The struct to fill. NULL does nothing.
 */
void schultz_window_options_init(schultz_window_options *options);

/**
 * @brief Opens a window and builds everything drawing it needs.
 *
 * On success the tree is empty but ready: its viewport and root bounds match
 * the window, its theme is the built in dark one, its font system, image
 * table and clipboard are wired up, and an event router is attached.
 *
 * @param options   How to start. NULL accepts every default.
 * @param out_window Receives the window. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when out_window is NULL or
 *         the platform refused the window, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_window_create(const schultz_window_options *options,
                             schultz_window **out_window);

/**
 * @brief Returns the platform's description of the most recent failure.
 *
 * A result code says what went wrong in the toolkit's terms; when the window
 * itself is what failed, this says why in the platform's. It is the only
 * reason a host ever needs the platform layer, so it is passed through here
 * rather than leaving that header to be included.
 *
 * @return A NUL terminated string owned by the platform. Never NULL, but it
 *         is empty when nothing has failed.
 */
const char *schultz_window_error(void);

/**
 * @brief Closes the window and releases everything it owns.
 *
 * That includes the tree, so every handle a host holds into it is dead
 * afterwards.
 *
 * @param window The window to close. NULL is accepted and does nothing.
 */
void schultz_window_destroy(schultz_window *window);

/**
 * @brief Returns the widget tree to build in. Never outlives the window.
 *
 * @param window The window. NULL yields NULL.
 * @return The tree.
 */
schultz_tree *schultz_window_tree(schultz_window *window);

/**
 * @brief Returns the event router, for accelerators and the host callback.
 *
 * @param window The window. NULL yields NULL.
 * @return The router.
 */
schultz_events *schultz_window_events(schultz_window *window);

/**
 * @brief Returns the font system to load faces into.
 *
 * @param window The window. NULL yields NULL.
 * @return The font system.
 */
schultz_font_system *schultz_window_fonts(schultz_window *window);

/**
 * @brief Returns the glyph cache, which a host needs only to render offscreen.
 *
 * @param window The window. NULL yields NULL.
 * @return The glyph cache.
 */
schultz_glyph_cache *schultz_window_glyphs(schultz_window *window);

/**
 * @brief Returns the gradient and dash table to register into.
 *
 * @param window The window. NULL yields NULL.
 * @return The resource table.
 */
schultz_resource_table *schultz_window_resources(schultz_window *window);

/**
 * @brief Returns the image table to load pictures and animations into.
 *
 * @param window The window. NULL yields NULL.
 * @return The image table.
 */
schultz_image_table *schultz_window_images(schultz_window *window);

/**
 * @brief Returns the width content may fill, in pixels.
 *
 * This is the area content may fill, which on a phone is the window minus
 * the camera notch and the home indicator. A tree meant to fill the screen is
 * built against this and is then correct on both, since on a desktop it is
 * the whole window.
 *
 * Worth asking rather than assuming, since a maximized window is whatever
 * size the window manager gave it.
 *
 * This is a size and not a rectangle. The safe area's position is carried by
 * the root's bounds, which the window sets; a host that sets those itself
 * from this and an origin of (0, 0) throws the position away and draws under
 * the notch. Use schultz_window_safe_area for the whole rectangle.
 *
 * @param window The window. NULL yields 0.
 * @return The width.
 */
uint32_t schultz_window_width(const schultz_window *window);

/**
 * @brief Returns the height content may fill, in pixels.
 *
 * @param window The window. NULL yields 0.
 * @return The height.
 */
uint32_t schultz_window_height(const schultz_window *window);

/**
 * @brief Returns the part of the window it is safe to put content in.
 *
 * A phone puts a camera notch over the top of its screen and a home indicator
 * across the bottom, and a rounded corner cuts the edges. This is what is
 * left, in the same pixels schultz_window_width counts.
 *
 * **Most hosts do not need this.** The root's bounds are already the safe
 * area, so a tree built as its children is placed correctly with nothing
 * asked for, and schultz_window_width reports the size to build against. This
 * is here for a host that wants the rectangle's position within the window.
 *
 * Node bounds are relative to the parent, and the root already sits at the
 * safe origin, so a child of the root starts at (0, 0) rather than here.
 *
 * On a screen with nothing in the way, which is every desktop, this is the
 * whole window.
 *
 * @param window    The window. Must not be NULL.
 * @param out_area Receives the safe rectangle. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer.
 */
/* ------------------------------------------------------ On-screen keyboard */

/** @brief When a window puts up a keyboard of its own. */
enum {
    /**
     * Not at all, which is what a window that never asks gets. A zeroed
     * options struct means this.
     */
    SCHULTZ_KEYBOARD_NEVER = 0,
    /**
     * Only when the machine has no keys of its own. A touch screen on a
     * framebuffer has no other way to type; a desktop needs nothing drawn.
     * This is what schultz_keyboard_options_init asks for.
     */
    SCHULTZ_KEYBOARD_WHEN_NEEDED,
    SCHULTZ_KEYBOARD_ALWAYS /**< Whenever a text widget takes focus. */
};

/** @brief What the window's own keyboard may do. */
typedef struct {
    /** One of the SCHULTZ_KEYBOARD_* values above. */
    uint32_t policy;
    /**
     * Nonzero to offer the page of emoji.
     *
     * The face that draws them is compiled in whether or not anything uses
     * it, so this costs nothing but the keys. A field may still refuse them:
     * see schultz_text_field_set_input_type.
     */
    int32_t  emoji;
} schultz_keyboard_options;

/**
 * @brief Fills in the defaults: a keyboard where the machine has no keys,
 *        and emoji offered.
 *
 * @param out_options The struct to fill. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_keyboard_options_init(schultz_keyboard_options *out_options);

/**
 * @brief Says whether this window draws a keyboard, and what it may offer.
 *
 * The window builds one the first time it is needed, puts it across the
 * bottom of the safe area when a text widget takes focus, and takes it away
 * when focus leaves. While it is up, the content slides so that the field
 * being edited stays visible.
 *
 * A platform with a keyboard of its own, which is every phone, is left to
 * use it: this is for a machine where there is nothing else.
 *
 * @param window  The window. Must not be NULL.
 * @param options What it may do. NULL restores the defaults.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or an error from
 *         building the keyboard.
 */
int32_t schultz_window_set_keyboard(schultz_window *window,
                                    const schultz_keyboard_options *options);

/**
 * @brief Returns what this window's keyboard has been told it may do.
 *
 * A window that has never been asked answers the defaults, which are no
 * keyboard at all.
 *
 * @param window      The window. Must not be NULL.
 * @param out_options Receives the settings. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer.
 */
int32_t schultz_window_keyboard_options(const schultz_window *window,
                                        schultz_keyboard_options *out_options);

/**
 * @brief Puts the keyboard up now, whatever the focus is.
 *
 * For an application that offers a button rather than waiting for a field.
 * Does nothing when the policy is SCHULTZ_KEYBOARD_NEVER.
 *
 * @param window The window. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_window_show_keyboard(schultz_window *window);

/**
 * @brief Takes the keyboard away.
 *
 * It comes back on the next focus into a text widget, unless the policy
 * says otherwise.
 *
 * @param window The window. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_window_hide_keyboard(schultz_window *window);

/**
 * @brief Returns the keyboard node, or SCHULTZ_HANDLE_NONE.
 *
 * For a host that wants to style it or read what it offers. The window owns
 * it; destroying it is the window's business.
 *
 * @param window The window. NULL yields SCHULTZ_HANDLE_NONE.
 * @return The keyboard, or SCHULTZ_HANDLE_NONE when there is none yet.
 */
schultz_handle schultz_window_keyboard(const schultz_window *window);

/**
 * @brief The part of the window content may occupy.
 *
 * The whole window everywhere that nothing is in the way of it, and less than
 * that on a screen with a camera notch, a rounded corner or a home indicator.
 * A host that puts its own shell together rather than using the tree's root
 * measures against this rather than against the window.
 *
 * @param window   The window. Must not be NULL.
 * @param out_area Receives the area, in toolkit units. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer.
 */
int32_t schultz_window_safe_area(const schultz_window *window,
                                schultz_rect *out_area);

/**
 * @brief Returns how many screen pixels fit in one toolkit unit.
 *
 * Three on a dense phone, two on a Retina laptop, one on an ordinary monitor.
 *
 * With scale_to_screen on this is what the toolkit is multiplying by, so a
 * one unit line covers this many pixels and a host that wants exactly one
 * writes 1 divided by this. With it off it is one, because a unit is already
 * a pixel, and this reports what the screen would have scaled by.
 *
 * @param window The window. NULL yields 1.
 * @return The scale. Never zero or negative.
 */
float schultz_window_pixel_scale(const schultz_window *window);

/**
 * @brief Changes which ways up the screen may be used, after it is open.
 *
 * The same values as the `orientation` option: SCHULTZ_SCREEN_ANY,
 * SCHULTZ_SCREEN_PORTRAIT or SCHULTZ_SCREEN_LANDSCAPE. A window whose turn
 * is SCHULTZ_TURN_AUTO works its turn out from this, so changing it here can
 * turn the picture straight away.
 *
 * **What this reaches on a phone is limited, and the limit is SDL's.** The
 * orientations an application permits are one hint, and SDL reads that hint
 * when it makes the window. iOS asks again whenever the system re-examines
 * the view controller, so a change here is picked up at the next rotation.
 * Android reads it again only when the window's resizability changes, so a
 * change here may not be noticed at all until something else does that.
 * Say it in the options instead where a phone is the target.
 *
 * On a fixed panel, where SCHULTZ_TURN_AUTO does the work, this is exact and
 * immediate.
 *
 * @param window      The window. Must not be NULL.
 * @param orientation The wanted orientation.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT for a NULL window or an
 *         orientation that is not one of the three.
 */
int32_t schultz_window_set_orientation(schultz_window *window,
                                       uint32_t orientation);

/**
 * @brief Changes how far the picture is turned on its way to the panel.
 *
 * The same values as the `turn` option: SCHULTZ_TURN_NONE, _90, _180, _270,
 * or SCHULTZ_TURN_AUTO to work it out from the orientation again.
 *
 * The buffer is rebuilt in the shape the new turn calls for, so a quarter
 * turn swaps the width and the height the interface is built against, and
 * everything lays out again on the next redraw. A pointer keeps arriving in
 * the coordinates the interface was built in.
 *
 * Ignored on phones and tablets, which turn their own screens.
 *
 * @param window The window. Must not be NULL.
 * @param turn   How far to turn.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT for a NULL window or a
 *         turn that is not one of the five.
 */
int32_t schultz_window_set_turn(schultz_window *window, uint32_t turn);

/**
 * @brief Returns the millisecond clock the last redraw ran at.
 *
 * The same reading the tree's animations were advanced with, so host motion
 * driven from it stays in step with theirs.
 *
 * @param window The window. NULL yields 0.
 * @return Milliseconds since the window was opened.
 */
uint64_t schultz_window_time_ms(const schultz_window *window);

/**
 * @brief Returns how many frames have been presented.
 *
 * @param window The window. NULL yields 0.
 * @return The frame count.
 */
uint64_t schultz_window_frame_count(const schultz_window *window);

/** @brief One entry in a file dialog's list of what it will show. */
typedef struct {
    const char *name;    /**< What the entry says, such as "Text files". */
    /**
     * The extensions it covers, separated by semicolons and without dots,
     * such as `"txt;md"`. A single asterisk means everything.
     */
    const char *pattern;
} schultz_file_filter;

/**
 * @brief How a file dialog should open.
 *
 * Zero every field first, then set the ones that matter. Not every platform
 * honours every field; the ones it does not are ignored rather than refused.
 */
typedef struct {
    const char *title;    /**< The dialog's title, or NULL for the usual. */
    const char *location; /**< Where to start, or NULL for the usual. */
    int32_t     allow_many; /**< Nonzero to let several files be chosen. */
    const schultz_file_filter *filters; /**< What to show, or NULL for all. */
    uint32_t    filter_count;           /**< How many filters there are. */
} schultz_file_options;

/**
 * @brief Opens the platform's file picker, without waiting for an answer.
 *
 * The call returns straight away with a request number. The answer arrives
 * later as a `SCHULTZ_EVENT_FILES_CHOSEN` or `SCHULTZ_EVENT_FILES_CANCELLED`
 * event carrying that number in its `token`, which is the same way every
 * other answer reaches a host.
 *
 * **Keep drawing redraws while it is open.** On Linux the dialog runs through
 * XDG portals over DBus, which needs the event loop turning; a host that
 * blocks waiting for the answer will wait forever.
 *
 * **On iOS the path is a copy.** iOS does not let an application open a file
 * the person chose; it hands back something outside the sandbox that an
 * ordinary open would be refused. So the file is copied into this
 * application's own temporary directory and that copy's path is what comes
 * back. Reading it works the way it does everywhere else. Writing to it
 * changes the copy and not the original, and the temporary directory is the
 * system's to empty, so anything worth keeping should be read and stored
 * rather than pointed at later. Nothing about this is visible on the other
 * five platforms, where the path is the file.
 *
 * The options are copied, so nothing in them has to outlive the call.
 *
 * @param window      The window. Must not be NULL.
 * @param options    How to open it, or NULL for every default.
 * @param out_request Receives the request number. May be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_OUT_OF_MEMORY,
 *         or SCHULTZ_ERR_INVALID_HANDLE when too many are already open.
 */
int32_t schultz_window_open_file(schultz_window *window,
                                const schultz_file_options *options,
                                uint32_t *out_request);

/**
 * @brief Opens the platform's save picker. Otherwise as open file.
 *
 * **On iOS this asks a different question.** The platform has no "choose
 * somewhere to write": it can only export a file that already exists. So an
 * empty one is made first and the picker moves it where the person chose,
 * which means they are choosing a folder for a file that is already named
 * rather than typing a name. The path that comes back is where it landed and
 * can be written to. The name is taken from the first filter's extension,
 * because the options carry no name to suggest.
 *
 * @param window      The window. Must not be NULL.
 * @param options    How to open it, or NULL for every default.
 * @param out_request Receives the request number. May be NULL.
 * @return As schultz_window_open_file.
 */
int32_t schultz_window_save_file(schultz_window *window,
                                const schultz_file_options *options,
                                uint32_t *out_request);

/**
 * @brief Opens the platform's folder picker. Otherwise as open file.
 *
 * Filters are ignored, since a folder has no extension.
 *
 * @param window      The window. Must not be NULL.
 * @param options    How to open it, or NULL for every default.
 * @param out_request Receives the request number. May be NULL.
 * @return As schultz_window_open_file.
 */
int32_t schultz_window_open_folder(schultz_window *window,
                                  const schultz_file_options *options,
                                  uint32_t *out_request);

/**
 * @brief Returns how many paths a finished request came back with.
 *
 * @param window   The window. NULL yields 0.
 * @param request The number the request was given.
 * @return The count, which is zero for a cancelled dialog.
 */
uint32_t schultz_window_file_count(const schultz_window *window,
                                  uint32_t request);

/**
 * @brief Returns one of a finished request's paths.
 *
 * The paths stay valid until the next redraw that delivers a result, so a
 * host reads what it wants out of them when it is told, not later.
 *
 * @param window   The window. NULL yields NULL.
 * @param request The number the request was given.
 * @param index   Which path, counting from zero.
 * @return The path, or NULL when there is none there.
 */
const char *schultz_window_file_path(const schultz_window *window,
                                    uint32_t request, uint32_t index);

/**
 * @brief Returns which filter was showing when the choice was made.
 *
 * @param window   The window. NULL yields -1.
 * @param request The number the request was given.
 * @return The index into the filters given, or -1 when the platform did not
 *         say.
 */
int32_t schultz_window_file_filter(const schultz_window *window,
                                  uint32_t request);

/**
 * @brief Advances the clock, draws one redraw, presents it, and reads input.
 *
 * The whole redraw in one call, in the only order that works:
 *
 *   1. reclaim last redraw's scratch memory
 *   2. advance animations to the current time
 *   3. resolve styles, because painting reads only what that produces
 *   4. lay out whatever asked to be laid out
 *   5. read the dirty region, which bounds the next two steps
 *   6. paint the tree into a draw list, skipping what is outside it
 *   7. play that list through the rasterizer
 *   8. upload the dirty region and present
 *   9. turn platform input into events and route it
 *
 * Input is read last so that what it changes is drawn by the next redraw,
 * rather than half of it landing in this one.
 *
 * @param window    The window. Must not be NULL.
 * @param out_quit Set to 1 when the user asked to close the window, 0
 *                 otherwise. May be NULL if the caller does not care.
 * @return SCHULTZ_OK, or the first failure the redraw hit.
 */
int32_t schultz_window_update(schultz_window *window, int32_t *out_quit);

/**
 * @brief Asks the redraw loop to stop after this redraw.
 *
 * What a Quit menu item calls. Without it a host has no way to end
 * schultz_window_run of its own accord: the loop stops when the window is
 * closed, and a machine showing one application on a bare screen has no
 * window furniture to close it with.
 *
 * Takes effect at the end of the current redraw, so a handler may call it and
 * carry on doing whatever else it was doing.
 *
 * @param window The window. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_window_request_close(schultz_window *window);

/**
 * @brief Runs the loop until the user closes the window.
 *
 * The loop a host would otherwise write, kept on this side so that a bound
 * language pays one crossing a frame rather than nine.
 *
 * **A host that cannot take an upcall drives the loop itself.** The update
 * function is called once per turn, which is an upcall per frame; a host
 * that would rather not be called at all keeps its own loop and calls
 * schultz_window_update, which is one turn of this one. The nine crossings
 * become one either way.
 *
 * @param window     The window. Must not be NULL.
 * @param update     Called before each frame is drawn, or NULL for a still
 *                   scene.
 * @param context    Passed to update untouched.
 * @param max_frames Stop after drawing this many frames. Zero means run until
 *                   the window is closed.
 * @return SCHULTZ_OK when the loop ended because the window closed or the
 *         frame count ran out, or the failure that stopped it.
 */
int32_t schultz_window_run(schultz_window *window, schultz_window_update_fn update,
                           void *context, uint64_t max_frames);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_WINDOW_H */
