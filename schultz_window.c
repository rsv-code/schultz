/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_window.c
 * @brief The window driver.
 */

#include "schultz_window.h"

#include <SDL3/SDL.h>

#ifdef SCHULTZ_CLIPBOARD_WINDOWS
#include "schultz_clipboard_windows.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "schultz_arena.h"
#include "schultz_debug.h"
#include "schultz_layout.h"
#include "schultz_paint.h"
#include "schultz_a11y.h"
/* For the keyboard the window puts up when there is no other way to type. */
#include "schultz_widgets.h"
#include "schultz_files_backend.h"
#include "schultz_sdl.h"
#include "schultz_thorvg.h"
#include "schultz_widget.h"

enum {
    SCHULTZ_WINDOW_DEFAULT_WIDTH  = 800,
    SCHULTZ_WINDOW_DEFAULT_HEIGHT = 600
};

/** How many file dialogs may be waiting for an answer at once. */
enum { SCHULTZ_FRAME_REQUESTS = 4 };
/** How many paths one answer may carry. */
enum { SCHULTZ_FRAME_PATHS = 64 };

/**
 * @brief One file dialog waiting for, or holding, an answer.
 *
 * The platform may answer on another thread, so everything the callback
 * writes is read only under the driver's lock, and nothing it writes touches
 * the tree.
 */
typedef struct {
    schultz_window *window;     /**< The driver, for the callback. */
    uint32_t       request;   /**< The number the host was given, 0 if free. */
    uint32_t       done;      /**< Set by the callback when it answers. */
    uint32_t       delivered; /**< Set once the host has been told. */
    uint32_t       cancelled; /**< The user dismissed it. */
    uint32_t       count;     /**< How many paths came back. */
    int32_t        filter;    /**< Which filter was showing, or -1. */
    char          *paths[SCHULTZ_FRAME_PATHS]; /**< Owned copies. */

    /* The filters the backend was handed. They must outlive the call, so
     * the driver owns them until the answer arrives. */
    schultz_file_filter *filters;     /**< The array the backend got. */
    char               **filter_text; /**< Its strings, owned here. */
    uint32_t             filter_count;/**< How many filters there are. */
} schultz_window_request;

/**
 * @brief Everything one window needs to draw itself.
 *
 * The order the fields are torn down in matters more than the order they are
 * listed in, so schultz_window_destroy walks them backwards from here.
 */
struct schultz_window {
    schultz_sdl_window     *platform;  /**< The platform's own window. */
    schultz_thorvg         *backend;   /**< Rasterizes into that buffer. */
    schultz_painter         painter;   /**< The backend as a painter. */
    schultz_arena           arena;     /**< Scratch, reset every window. */
    schultz_draw_list       list;      /**< Rebuilt on the arena every window. */
    schultz_font_system    *fonts;     /**< Faces, at the sizes in use. */
    schultz_glyph_cache    *glyphs;    /**< Rasterized glyphs, kept between
                                            frames. */
    schultz_resource_table *resources; /**< Gradients and dash patterns. */
    schultz_image_table    *images;    /**< Decoded pictures and animations. */
    schultz_tree           *tree;      /**< The widget tree. */
    schultz_events         *events;    /**< Routes input into that tree. */
    schultz_rect            whole;     /**< The window, for a full repaint. */
    schultz_rect            safe;      /**< The part content may occupy. */
    /**
     * Screen pixels per toolkit unit. One unless scaling to the screen, and
     * one on a screen with nothing to scale.
     */
    float                   scale;
    int32_t                 scale_to_screen; /**< The option, kept. */
    /** The two orientation options, kept, because auto resolves one against
     *  the other and either can be changed after the window is open. */
    uint32_t                orientation;
    uint32_t                turn;
    /**
     * The keyboard this window draws, and what it may do.
     *
     * Built the first time one is wanted rather than with the window, since
     * most windows never want one. `slide` is how far the content has been
     * moved up to keep the edited field clear of it.
     */
    schultz_keyboard_options keyboard_options;
    schultz_handle          keyboard;
    schultz_handle          keyboard_field; /**< The field it was told about. */
    int32_t                 keyboard_up;    /**< Whether it is showing. */
    int32_t                 keyboard_asked; /**< A host asked for it. */
    float                   slide;          /**< Units the content moved up. */
    int32_t                 quit_requested;  /**< A host asked to stop. */
    schultz_a11y           *a11y;      /**< Describes the tree to a reader. */
    int32_t                 a11y_focus;/**< Focus last told to the reader. */
    schultz_color           tint;      /**< Debug tint, or alpha zero. */
    uint64_t                now_ms;    /**< When the last window ran. */
    uint64_t                frames;    /**< How many have been presented. */
    /*
     * What changed this frame, copied out of the tree before the drawing
     * clears it, because the upload afterwards still needs it.
     */
    schultz_rect            dirty[SCHULTZ_TREE_DIRTY_PARTS];
    uint32_t                dirty_count;
    int32_t                 engine;    /**< 1 once the rasterizer started. */

    /** File dialogs waiting for, or holding, an answer. */
    schultz_window_request   requests[SCHULTZ_FRAME_REQUESTS];
    uint32_t                next_request; /**< The number to hand out next. */
    SDL_Mutex              *lock;         /**< Guards the requests. */
};

static void schultz_window_request_clear(schultz_window_request *request);

/*
 * How far to turn, from what the caller asked for and what the panel is.
 *
 * A named turn is taken as given: somebody who says the display is upside
 * down has looked at it. Auto asks the wanted shape instead, and turns a
 * quarter when the panel is the other shape, which is the ordinary case of a
 * portrait interface on a display that was only ever made in landscape.
 */
static uint32_t schultz_window_quarters(schultz_sdl_window *platform,
                                        uint32_t orientation, uint32_t turn)
{
    uint32_t panel_w;
    uint32_t panel_h;
    int32_t panel_is_wide;

    /* The platform turns its own screen, so turning it here as well would
     * turn it twice. */
    if (schultz_sdl_window_turns(platform)) {
        return 0u;
    }
    switch (turn) {
    case SCHULTZ_TURN_NONE: return 0u;
    case SCHULTZ_TURN_90:   return 1u;
    case SCHULTZ_TURN_180:  return 2u;
    case SCHULTZ_TURN_270:  return 3u;
    default:                break;
    }

    /*
     * The panel, not the buffer. They are the same until something turns,
     * and after that the buffer is the panel on its side; deciding again
     * from a buffer that is already turned decides from the wrong shape and
     * the turn never comes back off.
     */
    panel_w = schultz_sdl_window_panel_width(platform);
    panel_h = schultz_sdl_window_panel_height(platform);
    if (panel_w == 0u || panel_h == 0u) {
        return 0u;
    }
    panel_is_wide = (panel_w >= panel_h) ? 1 : 0;

    if (orientation == SCHULTZ_SCREEN_PORTRAIT && panel_is_wide) {
        return 1u;
    }
    if (orientation == SCHULTZ_SCREEN_LANDSCAPE && !panel_is_wide) {
        return 1u;
    }
    return 0u;
}

void schultz_window_options_init(schultz_window_options *options)
{
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->width  = SCHULTZ_WINDOW_DEFAULT_WIDTH;
    options->height = SCHULTZ_WINDOW_DEFAULT_HEIGHT;
    options->vsync  = 1;
    /*
     * Also on by default, and for the same reason: a toolkit that has to be
     * asked to use the screen it is on ships blurry.
     */
    options->scale_to_screen = 1;
    /*
     * A zeroed struct means off, and off is the wrong default for this:
     * accessibility that has to be asked for is accessibility that ships
     * turned off. This is the same reason vsync is turned back on here.
     */
    options->accessible = 1;
    /* Turned back on for the same reason: a window nobody can resize is the
     * unusual one, and a zeroed struct cannot say so. */
    options->resizable = 1;
    /*
     * And the same again. A phone that will not turn when it is turned reads
     * as broken, so following the device is the default and refusing to is
     * the thing that has to be asked for.
     *
     * SCHULTZ_SCREEN_ANY is zero, so the orientation needs no line here.
     */
    options->allow_rotate = 1;
    /*
     * SCHULTZ_SCREEN_ANY and SCHULTZ_TURN_AUTO are both zero, so neither
     * needs a line here: asking for nothing in particular means the picture
     * goes to the panel the way it was drawn.
     */
}

/*
 * Points the tree at the part of the window content may occupy.
 *
 * A phone puts a camera notch over the top of its screen and a home indicator
 * across the bottom. What is left is the safe area, and that is what the root
 * node is given as its bounds. A host builds its shell as a child of the
 * root, so the shell lands inside the safe area without asking, and so does
 * everything under it.
 *
 * The window is still covered. The window paints the window colour across all
 * of it before the tree draws, so the notch and the indicator sit on the same
 * background as everything else rather than on stale pixels. That fill is the
 * reason the root can be made smaller than the window at all.
 *
 * On a screen with nothing in the way, which is every desktop, the safe area
 * is the whole window and none of this shows.
 */
/*
 * Reads the window's geometry and settles the toolkit's unit.
 *
 * The platform layer talks in buffer pixels throughout: the buffer's size,
 * and the part of it a notch leaves alone. This is the one place that turns
 * those into whatever the toolkit is working in, by dividing by the scale.
 *
 * The scale is also handed to the painter, which multiplies everything back
 * on the way out and rasterizes glyphs at the size they will actually be
 * drawn. So the numbers stay small and readable up here, and the pixels are
 * all used down there.
 */
static void schultz_window_take_geometry(schultz_window *window)
{
    schultz_rect safe;
    float width  = (float)schultz_sdl_window_width(window->platform);
    float height = (float)schultz_sdl_window_height(window->platform);

    window->scale = window->scale_to_screen
                       ? schultz_sdl_window_unit_scale(window->platform)
                       : 1.0f;
    if (window->scale <= 0.0f) {
        window->scale = 1.0f;
    }
    window->whole = schultz_rect_make(0.0f, 0.0f, width / window->scale,
                                     height / window->scale);
    if (schultz_sdl_window_safe_area(window->platform, &safe) != SCHULTZ_OK) {
        safe = schultz_rect_make(0.0f, 0.0f, width, height);
    }
    window->safe = schultz_rect_make(safe.x / window->scale,
                                    safe.y / window->scale,
                                    safe.width / window->scale,
                                    safe.height / window->scale);

    schultz_thorvg_set_scale(window->backend, window->scale);
    /*
     * The platform reports a pointer in its own window coordinates, and what
     * one of those is worth is the density rather than the scale above.
     *
     * A window coordinate covers `density` pixels and a unit covers `scale`
     * of them, so one is worth density over scale of the other. On Apple the
     * two are the same number and a coordinate is already a unit; on Android
     * the density is one and the coordinate is a pixel, so it has to come
     * down by the whole of the scale to land where it was pressed.
     */
    schultz_sdl_window_set_input_scale(
        window->platform,
        schultz_sdl_window_pixel_density(window->platform) / window->scale);

    /*
     * Everything about the window's geometry in one line, when asked.
     *
     * These numbers are the whole of what the toolkit decided, and every
     * question about a screen behaving oddly is answered by them: what the
     * buffer is, what a unit turned out to be, what a host is being told to
     * build against. Printing them beats guessing from a photograph, and on a
     * phone a photograph is usually all there is.
     */
    if (getenv("SCHULTZ_TRACE_GEOMETRY") != NULL) {
        fprintf(stderr,
                "schultz: buffer %.0fx%.0f  screen scale %.2f  scaling %s\n"
                "schultz: a unit is %.2f pixels, so the window is %.0fx%.0f\n"
                "schultz: safe area %.0f,%.0f %.0fx%.0f\n"
                "schultz: build against %ux%u\n",
                (double)schultz_sdl_window_width(window->platform),
                (double)schultz_sdl_window_height(window->platform),
                (double)schultz_sdl_window_unit_scale(window->platform),
                window->scale_to_screen ? "on" : "off",
                (double)window->scale, (double)window->whole.width,
                (double)window->whole.height,
                (double)window->safe.x, (double)window->safe.y,
                (double)window->safe.width, (double)window->safe.height,
                schultz_window_width(window), schultz_window_height(window));
    }
    schultz_tree_set_pixel_scale(window->tree, window->scale);
    schultz_tree_set_viewport(window->tree, window->whole);
    schultz_node_set_bounds(window->tree, schultz_tree_root(window->tree),
                            window->safe);
}

/*
 * Why the last attempt to open a window failed, kept here because the cleanup
 * that runs on the way out clears SDL's own copy. Empty when nothing has
 * failed since the last attempt.
 */
static char schultz_window_failure[256];

/* Takes a copy of the platform's reason before anything can clear it. */
static void schultz_window_keep_failure(void)
{
    const char *reason = schultz_sdl_error();
    size_t length;

    if (reason == NULL) {
        return;
    }
    length = strlen(reason);
    if (length >= sizeof(schultz_window_failure)) {
        length = sizeof(schultz_window_failure) - 1u;
    }
    memcpy(schultz_window_failure, reason, length);
    schultz_window_failure[length] = '\0';
}

int32_t schultz_window_create(const schultz_window_options *options,
                             schultz_window **out_window)
{
    schultz_window_options used;
    schultz_window *window;
    int32_t result;

    if (out_window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_window = NULL;
    /* This attempt's reason, not the last one's. */
    schultz_window_failure[0] = '\0';

    schultz_window_options_init(&used);
    if (options != NULL) {
        used = *options;
        if (used.width == 0u) {
            used.width = SCHULTZ_WINDOW_DEFAULT_WIDTH;
        }
        if (used.height == 0u) {
            used.height = SCHULTZ_WINDOW_DEFAULT_HEIGHT;
        }
    }
    if (used.title == NULL) {
        used.title = "Schultz";
    }

    window = (schultz_window *)calloc(1, sizeof(*window));
    if (window == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    window->tint = used.debug_tint;

    /*
     * From here on every failure leaves through the one exit, because half a
     * driver still owns a window and a rasterizer.
     */
    result = schultz_arena_init(&window->arena, 0);
    if (result != SCHULTZ_OK) {
        goto failed;
    }
    result = schultz_draw_list_init(&window->list, &window->arena, 0);
    if (result != SCHULTZ_OK) {
        goto failed;
    }

    result = schultz_sdl_init(used.video_driver);
    if (result != SCHULTZ_OK) {
        goto failed;
    }
    /*
     * Read when the window is made, so it is said before making one. This is
     * the half a phone acts on; the half a fixed panel acts on comes after,
     * once there is a window to measure.
     */
    result = schultz_sdl_set_orientation(used.orientation);
    if (result != SCHULTZ_OK) {
        goto failed;
    }
    result = schultz_sdl_window_create(used.title, used.width, used.height,
                                       used.maximized, used.resizable,
                                       used.allow_rotate, &window->platform);
    if (result != SCHULTZ_OK) {
        goto failed;
    }
    /*
     * And the other half: how far to turn what is drawn before it reaches
     * the panel. Nothing on a phone, which turns its own screen; on a fixed
     * panel this is the only thing that can.
     */
    window->orientation = used.orientation;
    window->turn        = used.turn;
    result = schultz_sdl_window_set_turn(window->platform,
        schultz_window_quarters(window->platform, used.orientation,
                                used.turn));
    if (result != SCHULTZ_OK) {
        goto failed;
    }

    /*
     * From the window rather than from what was asked for: maximizing is the
     * window manager's call, so this is the only size that is certain.
     */
    window->scale_to_screen = used.scale_to_screen;
    window->whole = schultz_rect_make(
        0.0f, 0.0f, (float)schultz_sdl_window_width(window->platform),
        (float)schultz_sdl_window_height(window->platform));
    schultz_sdl_window_set_vsync(window->platform, used.vsync);

    result = schultz_thorvg_engine_init(used.threads);
    if (result != SCHULTZ_OK) {
        goto failed;
    }
    window->engine = 1;
    result = schultz_thorvg_create(schultz_sdl_window_pixels(window->platform),
                                   schultz_sdl_window_width(window->platform),
                                   schultz_sdl_window_height(window->platform),
                                   schultz_sdl_window_stride(window->platform),
                                   &window->backend);
    if (result != SCHULTZ_OK) {
        goto failed;
    }
    schultz_thorvg_painter(window->backend, &window->painter);

    result = schultz_font_system_create(&window->fonts);
    if (result != SCHULTZ_OK) {
        goto failed;
    }
    result = schultz_glyph_cache_create(window->fonts, &window->glyphs);
    if (result != SCHULTZ_OK) {
        goto failed;
    }
    result = schultz_resource_table_create(&window->resources);
    if (result != SCHULTZ_OK) {
        goto failed;
    }
    result = schultz_image_table_create(&window->images);
    if (result != SCHULTZ_OK) {
        goto failed;
    }
    schultz_thorvg_set_fonts(window->backend, window->fonts, window->glyphs);
    schultz_thorvg_set_resources(window->backend, window->resources);
    schultz_thorvg_set_images(window->backend, window->images);

    result = schultz_tree_create(&window->tree);
    if (result != SCHULTZ_OK) {
        goto failed;
    }
    /* A tree copies the theme it is given, so the built in one can be a
     * temporary and a host may install its own at any time. */
    schultz_tree_set_theme(window->tree, NULL);
    schultz_tree_set_font_system(window->tree, window->fonts);
    schultz_tree_set_image_table(window->tree, window->images);
    /*
     * The other two a render needs. Anything that has to draw the tree on its
     * own, such as copying a picture out of a selection, assembles what it
     * wants from the tree rather than asking the host for it again.
     */
    schultz_tree_set_glyph_cache(window->tree, window->glyphs);
    schultz_tree_set_resources(window->tree, window->resources);
    /* Cut, copy and paste, which the toolkit cannot reach on its own. */
    /*
     * The clipboard is SDL's everywhere but Windows. There, rich text lives
     * in a format registered by name rather than one of the numbered ones
     * SDL knows, and SDL would take the markup for the clipboard's plain
     * text, so that platform talks to the clipboard itself.
     */
#ifdef SCHULTZ_CLIPBOARD_WINDOWS
    schultz_tree_set_clipboard(window->tree, schultz_windows_clipboard_offer,
                               schultz_windows_clipboard_take,
                               schultz_windows_clipboard_holds,
                               schultz_windows_clipboard_owner(
                                   schultz_sdl_window_handle(window->platform)));
#else
    schultz_tree_set_clipboard(window->tree, schultz_sdl_clipboard_offer,
                               schultz_sdl_clipboard_take,
                               schultz_sdl_clipboard_holds, NULL);
#endif
    schultz_window_take_geometry(window);
    /*
     * The window's own colour, so that the band a notch sits in matches the
     * rest of the screen. A host that wants another one sets it on the root.
     */
    schultz_node_set_style_property(window->tree,
                                    schultz_tree_root(window->tree),
                                    SCHULTZ_PROP_BACKGROUND,
                                    schultz_value_token(
                                        SCHULTZ_TOKEN_COLOR_WINDOW));
    /* Nothing on screen is valid yet, so the first window is a whole one. */
    schultz_node_invalidate(window->tree, schultz_tree_root(window->tree));

    result = schultz_events_create(window->tree, &window->events);
    if (result != SCHULTZ_OK) {
        goto failed;
    }
    /* A file dialog may answer on another thread, so what it writes is
     * guarded. Everything else here runs on the window loop's thread. */
    window->lock = SDL_CreateMutex();
    if (window->lock == NULL) {
        result = SCHULTZ_ERR_OUT_OF_MEMORY;
        goto failed;
    }

    /*
     * Accessibility last, because it publishes the tree as it starts and so
     * needs everything above to exist. It succeeds whether or not anything is
     * listening: a reader may connect at any point afterwards, and failing
     * startup because none is running would be absurd.
     */
    if (options == NULL || options->accessible) {
        schultz_a11y_options a11y;

        memset(&a11y, 0, sizeof(a11y));
        a11y.app_name = (options != NULL && options->app_name != NULL)
                            ? options->app_name
                            : ((options != NULL) ? options->title : NULL);
        a11y.window   = window->whole;
        a11y.focused  = 1;
        /*
         * Every platform but Linux hangs accessibility off a native window
         * or view, and this is the only place that has one to give.
         */
        a11y.native_window = schultz_sdl_window_handle(window->platform);
        schultz_a11y_create(window->tree, window->events, &a11y, &window->a11y);
    }

    *out_window = window;
    return SCHULTZ_OK;

failed:
    /* Before the cleanup below, which ends in SDL_Quit and clears it. */
    schultz_window_keep_failure();
    schultz_window_destroy(window);
    return result;
}

const char *schultz_window_error(void)
{
    /*
     * The saved one first. Opening a window has one cleanup path, and it ends
     * in SDL_Quit, which throws SDL's error string away: by the time a host
     * asks why the window did not open, the reason has been cleared by the
     * tidying up after it. Nowhere is that worse than the case a host most
     * needs told about, a machine with no display, where the answer was an
     * empty string.
     */
    if (schultz_window_failure[0] != '\0') {
        return schultz_window_failure;
    }
    return schultz_sdl_error();
}

void schultz_window_destroy(schultz_window *window)
{
    if (window == NULL) {
        return;
    }
    schultz_a11y_destroy(window->a11y);
    {
        uint32_t i;

        for (i = 0; i < SCHULTZ_FRAME_REQUESTS; i++) {
            schultz_window_request_clear(&window->requests[i]);
        }
    }
    if (window->lock != NULL) {
        SDL_DestroyMutex(window->lock);
    }
    schultz_events_destroy(window->events);
    schultz_tree_destroy(window->tree);
    schultz_image_table_destroy(window->images);
    schultz_resource_table_destroy(window->resources);
    schultz_glyph_cache_destroy(window->glyphs);
    schultz_font_system_destroy(window->fonts);
    schultz_thorvg_destroy(window->backend);
    if (window->engine) {
        schultz_thorvg_engine_term();
    }
    schultz_sdl_window_destroy(window->platform);
    schultz_sdl_quit();
    schultz_arena_free(&window->arena);
    free(window);
}

schultz_tree *schultz_window_tree(schultz_window *window)
{
    return (window == NULL) ? NULL : window->tree;
}

schultz_events *schultz_window_events(schultz_window *window)
{
    return (window == NULL) ? NULL : window->events;
}

schultz_font_system *schultz_window_fonts(schultz_window *window)
{
    return (window == NULL) ? NULL : window->fonts;
}

schultz_glyph_cache *schultz_window_glyphs(schultz_window *window)
{
    return (window == NULL) ? NULL : window->glyphs;
}

schultz_resource_table *schultz_window_resources(schultz_window *window)
{
    return (window == NULL) ? NULL : window->resources;
}

schultz_image_table *schultz_window_images(schultz_window *window)
{
    return (window == NULL) ? NULL : window->images;
}

uint32_t schultz_window_width(const schultz_window *window)
{
    return (window == NULL) ? 0u : (uint32_t)window->safe.width;
}

uint32_t schultz_window_height(const schultz_window *window)
{
    return (window == NULL) ? 0u : (uint32_t)window->safe.height;
}

float schultz_window_pixel_scale(const schultz_window *window)
{
    float scale;

    if (window == NULL) {
        return 1.0f;
    }
    scale = schultz_sdl_window_unit_scale(window->platform);
    return (scale > 0.0f) ? scale : 1.0f;
}

/*
 * Both setters end the same way: work the turn out again from whatever the
 * two values now are, and hand it to the platform, which rebuilds the buffer
 * and marks the window resized so the next redraw lays everything out for
 * the shape it is now.
 */
static int32_t schultz_window_retake_turn(schultz_window *window)
{
    return schultz_sdl_window_set_turn(window->platform,
        schultz_window_quarters(window->platform, window->orientation,
                                window->turn));
}

int32_t schultz_window_set_orientation(schultz_window *window,
                                       uint32_t orientation)
{
    int32_t result;

    if (window == NULL || orientation > SCHULTZ_SCREEN_LANDSCAPE) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    window->orientation = orientation;
    /*
     * Said to the platform as well, even though a phone may not look at it
     * again until it makes a window or is asked something else. Saying it is
     * still right: the value is there when it does look.
     */
    result = schultz_sdl_set_orientation(orientation);
    if (result != SCHULTZ_OK) {
        return result;
    }
    return schultz_window_retake_turn(window);
}

int32_t schultz_window_set_turn(schultz_window *window, uint32_t turn)
{
    if (window == NULL || turn > SCHULTZ_TURN_270) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    window->turn = turn;
    return schultz_window_retake_turn(window);
}

/* ------------------------------------------------------ On-screen keyboard */

/*
 * How much of the window the keyboard may take, and how much room is left
 * above the field being edited when the content slides.
 */
#define SCHULTZ_KEYBOARD_MOST 0.6f
#define SCHULTZ_KEYBOARD_CLEARANCE 8.0f

int32_t schultz_keyboard_options_init(schultz_keyboard_options *out_options)
{
    if (out_options == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    memset(out_options, 0, sizeof(*out_options));
    out_options->policy = SCHULTZ_KEYBOARD_WHEN_NEEDED;
    out_options->emoji  = 1;
    return SCHULTZ_OK;
}

/*
 * Builds the keyboard, once, the first time one is wanted.
 *
 * It is an overlay so that it paints over the content and is offered input
 * before it, which is what every other thing that covers the screen does
 * here. Its place is set each turn by the function below rather than by a
 * pane, because where it goes depends on the safe area and on how far the
 * content has slid.
 */
static int32_t schultz_window_build_keyboard(schultz_window *window)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result;

    if (window->keyboard != SCHULTZ_HANDLE_NONE) {
        return SCHULTZ_OK;
    }
    result = schultz_keyboard_create(window->tree,
                                     schultz_tree_root(window->tree),
                                     window->events, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    result = schultz_tree_push_overlay(window->tree, node, 0);
    if (result != SCHULTZ_OK) {
        schultz_node_destroy(window->tree, node);
        return result;
    }
    schultz_keyboard_set_emoji(window->tree, node,
                               window->keyboard_options.emoji);
    window->keyboard = node;
    window->keyboard_up = 1; /* cleared below unless something wants it */
    return SCHULTZ_OK;
}

/* Shows or hides the keyboard node itself. */
static void schultz_window_keyboard_visible(schultz_window *window,
                                            int32_t visible)
{
    uint32_t state;

    if (window->keyboard == SCHULTZ_HANDLE_NONE) {
        return;
    }
    state = schultz_node_get_state(window->tree, window->keyboard);
    if (visible) {
        state |= (uint32_t)SCHULTZ_STATE_VISIBLE;
    } else {
        state &= ~(uint32_t)SCHULTZ_STATE_VISIBLE;
    }
    schultz_node_set_state(window->tree, window->keyboard, state);
}

/*
 * Puts the content back where the safe area says it goes, having slid.
 */
static void schultz_window_place_root(schultz_window *window)
{
    schultz_rect at = window->safe;

    at.y -= window->slide;
    schultz_node_set_bounds(window->tree, schultz_tree_root(window->tree), at);
}

/*
 * Decides whether the keyboard belongs on screen, puts it where it goes, and
 * slides the content so the field being edited stays visible.
 *
 * Run every turn, before anything is laid out. Focus is read rather than
 * remembered: it is the one thing that says whether a person is typing, and
 * the router already keeps it.
 */
static void schultz_window_follow_keyboard(schultz_window *window)
{
    schultz_handle focused;
    schultz_size size;
    schultz_rect field;
    int32_t editing = 0;
    int32_t wanted;
    float height;
    float top;
    float slide = 0.0f;

    if (window->keyboard_options.policy == SCHULTZ_KEYBOARD_NEVER) {
        return;
    }
    if (window->keyboard_options.policy == SCHULTZ_KEYBOARD_WHEN_NEEDED &&
        schultz_sdl_has_keyboard()) {
        return;
    }
    focused = schultz_events_focus(window->events);
    if (focused != SCHULTZ_HANDLE_NONE) {
        editing = (schultz_node_get_role(window->tree, focused) ==
                   SCHULTZ_ROLE_TEXT_INPUT) ? 1 : 0;
    }
    wanted = (editing || window->keyboard_asked) ? 1 : 0;

    if (!wanted) {
        if (window->keyboard_up) {
            schultz_window_keyboard_visible(window, 0);
            window->keyboard_up = 0;
        }
        window->keyboard_field = SCHULTZ_HANDLE_NONE;
        if (window->slide != 0.0f) {
            window->slide = 0.0f;
            schultz_window_place_root(window);
        }
        return;
    }
    if (window->keyboard == SCHULTZ_HANDLE_NONE &&
        schultz_window_build_keyboard(window) != SCHULTZ_OK) {
        return;
    }
    /*
     * What the field will take, asked once when focus arrives rather than
     * every turn: telling the keyboard again while it is up would put it
     * back on the letters under the hand of someone who had just asked for
     * the digits.
     */
    if (focused != window->keyboard_field) {
        window->keyboard_field = focused;
        if (editing) {
            schultz_keyboard_set_input_type(window->tree, window->keyboard,
                schultz_text_field_input_type(window->tree, focused));
        }
    }

    /*
     * As tall as its keys need at this width, and never more than part of
     * the window: a keyboard that took the whole screen would leave nothing
     * to type into.
     */
    if (schultz_layout_measure(window->tree, window->keyboard,
                               window->safe.width, -1.0f, &size) != SCHULTZ_OK) {
        return;
    }
    height = size.height;
    /*
     * Nothing yet. A keyboard measured before anything has been laid out
     * reports no size at all, and placing it at that size would show a
     * sliver of a keyboard for one turn. It is asked again next turn, by
     * which time its keys know how big they are.
     */
    if (height <= 0.0f) {
        return;
    }
    if (height > window->safe.height * SCHULTZ_KEYBOARD_MOST) {
        height = window->safe.height * SCHULTZ_KEYBOARD_MOST;
    }
    top = window->safe.y + window->safe.height - height;

    /*
     * Sliding rather than shortening. A screen of controls laid out again in
     * the height that is left is a screen of controls squeezed into it;
     * moving everything up changes nothing but where it is, which is what
     * the platform does with its own keyboard on a phone.
     */
    if (editing &&
        schultz_node_absolute_bounds(window->tree, focused, &field)
            == SCHULTZ_OK) {
        /*
         * Where the field would be if nothing had slid, which is what the
         * answer has to be worked out from. The field's own position already
         * includes whatever slide is in force, so measuring against that
         * asks "how far must I move from where I have already moved to",
         * and each turn's answer undoes part of the last one: the interface
         * rocks up and down for as long as the field is focused. Adding the
         * current slide back takes the feedback out, and the same field in
         * the same window then gives the same answer every turn.
         */
        float rest = field.y + window->slide;
        float below = (rest + field.height + SCHULTZ_KEYBOARD_CLEARANCE)
                          - top;

        if (below > 0.0f) {
            slide = below;
            /* Never so far that the top of the field itself goes off. */
            if (slide > rest - window->safe.y) {
                slide = rest - window->safe.y;
            }
            if (slide < 0.0f) {
                slide = 0.0f;
            }
        }
    }
    if (slide != window->slide) {
        window->slide = slide;
        schultz_window_place_root(window);
    }

    /*
     * Pinned to the bottom of the screen rather than to the content, so it
     * stays put while what is above it slides. Its bounds are relative to
     * the root, which has itself moved up by the slide.
     */
    {
        schultz_rect at = schultz_rect_make(
            0.0f, window->safe.height - height + window->slide,
            window->safe.width, height);
        schultz_rect now;

        if (schultz_node_get_bounds(window->tree, window->keyboard, &now)
                != SCHULTZ_OK || !schultz_rect_equals(now, at)) {
            schultz_node_set_bounds(window->tree, window->keyboard, at);
            /*
             * Bounds are not a layout. Setting them marks what has to be
             * painted again and nothing else, so the keys would keep the
             * places they were given when the keyboard was built, which was
             * before it had a size at all. Saying the layout is stale is
             * what runs the pane over the rectangle it now has.
             */
            schultz_node_invalidate_layout(window->tree, window->keyboard);
        }
    }
    if (!window->keyboard_up) {
        schultz_window_keyboard_visible(window, 1);
        window->keyboard_up = 1;
    }
}

int32_t schultz_window_set_keyboard(schultz_window *window,
                                    const schultz_keyboard_options *options)
{
    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (options == NULL) {
        memset(&window->keyboard_options, 0, sizeof(window->keyboard_options));
    } else {
        if (options->policy > SCHULTZ_KEYBOARD_ALWAYS) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
        window->keyboard_options = *options;
    }
    if (window->keyboard_options.policy == SCHULTZ_KEYBOARD_NEVER) {
        window->keyboard_asked = 0;
        if (window->keyboard != SCHULTZ_HANDLE_NONE) {
            schultz_window_keyboard_visible(window, 0);
            window->keyboard_up = 0;
        }
        if (window->slide != 0.0f) {
            window->slide = 0.0f;
            schultz_window_place_root(window);
        }
        return SCHULTZ_OK;
    }
    if (window->keyboard != SCHULTZ_HANDLE_NONE) {
        schultz_keyboard_set_emoji(window->tree, window->keyboard,
                                   window->keyboard_options.emoji);
    }
    return SCHULTZ_OK;
}

int32_t schultz_window_keyboard_options(const schultz_window *window,
                                        schultz_keyboard_options *out_options)
{
    if (window == NULL || out_options == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_options = window->keyboard_options;
    return SCHULTZ_OK;
}

int32_t schultz_window_show_keyboard(schultz_window *window)
{
    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (window->keyboard_options.policy != SCHULTZ_KEYBOARD_NEVER) {
        window->keyboard_asked = 1;
    }
    return SCHULTZ_OK;
}

int32_t schultz_window_hide_keyboard(schultz_window *window)
{
    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    window->keyboard_asked = 0;
    return SCHULTZ_OK;
}

schultz_handle schultz_window_keyboard(const schultz_window *window)
{
    return (window == NULL) ? SCHULTZ_HANDLE_NONE : window->keyboard;
}

int32_t schultz_window_safe_area(const schultz_window *window,
                                schultz_rect *out_area)
{
    if (window == NULL || out_area == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_area = window->safe;
    return SCHULTZ_OK;
}

uint64_t schultz_window_time_ms(const schultz_window *window)
{
    return (window == NULL) ? 0u : window->now_ms;
}

uint64_t schultz_window_frame_count(const schultz_window *window)
{
    return (window == NULL) ? 0u : window->frames;
}


/* ------------------------------------------------------- file dialogs */

/* Releases everything one request owns and frees its slot. */
static void schultz_window_request_clear(schultz_window_request *request)
{
    uint32_t i;

    for (i = 0; i < request->count; i++) {
        free(request->paths[i]);
        request->paths[i] = NULL;
    }
    for (i = 0; i < request->filter_count; i++) {
        free(request->filter_text[i * 2u]);
        free(request->filter_text[i * 2u + 1u]);
    }
    free(request->filter_text);
    free(request->filters);
    memset(request, 0, sizeof(*request));
}

/*
 * The platform's answer. This may arrive on a thread that is not the one the
 * window loop runs on, so it copies what it was given and says so, and never
 * looks at the tree.
 */
static void schultz_window_file_answer(void *userdata,
                                       const char *const *filelist,
                                       int32_t filter)
{
    schultz_window_request *request = (schultz_window_request *)userdata;
    schultz_window *window;

    if (request == NULL) {
        return;
    }
    window = request->window;
    SDL_LockMutex(window->lock);

    request->filter = (int32_t)filter;
    if (filelist == NULL) {
        /* NULL is a failure and an empty list is a dismissal. Both leave the
         * host with nothing, and both have to be reported. */
        request->cancelled = 1u;
    } else {
        uint32_t i = 0;

        while (filelist[i] != NULL && i < SCHULTZ_FRAME_PATHS) {
            size_t length = strlen(filelist[i]);
            char *copy = (char *)malloc(length + 1u);

            if (copy == NULL) {
                break;
            }
            memcpy(copy, filelist[i], length + 1u);
            request->paths[i] = copy;
            i++;
        }
        request->count = i;
        request->cancelled = (i == 0u) ? 1u : 0u;
    }
    request->done = 1u;
    SDL_UnlockMutex(window->lock);
}

/* Finds a free slot and fills in the parts every kind of dialog needs. */
static schultz_window_request *schultz_window_request_open(
    schultz_window *window, const schultz_file_options *options,
    uint32_t *out_request)
{
    schultz_window_request *request = NULL;
    uint32_t i;

    for (i = 0; i < SCHULTZ_FRAME_REQUESTS; i++) {
        if (window->requests[i].request == 0u) {
            request = &window->requests[i];
            break;
        }
    }
    if (request == NULL) {
        return NULL;
    }
    memset(request, 0, sizeof(*request));
    request->window   = window;
    request->filter  = -1;
    request->request = ++window->next_request;

    /*
     * A backend keeps the filter array until it answers, so the driver owns a
     * copy of it and of every string in it rather than trusting the caller's
     * to still be there by then.
     */
    if (options != NULL && options->filters != NULL &&
        options->filter_count > 0u) {
        request->filters = (schultz_file_filter *)calloc(
            options->filter_count, sizeof(*request->filters));
        request->filter_text = (char **)calloc(options->filter_count * 2u,
                                               sizeof(char *));
        if (request->filters == NULL || request->filter_text == NULL) {
            schultz_window_request_clear(request);
            return NULL;
        }
        for (i = 0; i < options->filter_count; i++) {
            const char *name = (options->filters[i].name == NULL)
                                   ? "" : options->filters[i].name;
            const char *pattern = (options->filters[i].pattern == NULL)
                                      ? "*" : options->filters[i].pattern;

            request->filter_text[i * 2u] = SDL_strdup(name);
            request->filter_text[i * 2u + 1u] = SDL_strdup(pattern);
            if (request->filter_text[i * 2u] == NULL ||
                request->filter_text[i * 2u + 1u] == NULL) {
                schultz_window_request_clear(request);
                return NULL;
            }
            request->filters[i].name = request->filter_text[i * 2u];
            request->filters[i].pattern = request->filter_text[i * 2u + 1u];
        }
        request->filter_count = options->filter_count;
    }

    if (out_request != NULL) {
        *out_request = request->request;
    }
    return request;
}

int32_t schultz_window_open_file(schultz_window *window,
                                const schultz_file_options *options,
                                uint32_t *out_request)
{
    schultz_window_request *request;

    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    request = schultz_window_request_open(window, options, out_request);
    if (request == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_files_show(SCHULTZ_FILES_OPEN,
                       schultz_sdl_window_handle(window->platform), options,
                       request->filters, request->filter_count,
                       schultz_window_file_answer, request);
    return SCHULTZ_OK;
}

int32_t schultz_window_save_file(schultz_window *window,
                                const schultz_file_options *options,
                                uint32_t *out_request)
{
    schultz_window_request *request;

    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    request = schultz_window_request_open(window, options, out_request);
    if (request == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_files_show(SCHULTZ_FILES_SAVE,
                       schultz_sdl_window_handle(window->platform), options,
                       request->filters, request->filter_count,
                       schultz_window_file_answer, request);
    return SCHULTZ_OK;
}

int32_t schultz_window_open_folder(schultz_window *window,
                                  const schultz_file_options *options,
                                  uint32_t *out_request)
{
    schultz_window_request *request;

    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    request = schultz_window_request_open(window, options, out_request);
    if (request == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_files_show(SCHULTZ_FILES_FOLDER,
                       schultz_sdl_window_handle(window->platform), options,
                       NULL, 0u, schultz_window_file_answer, request);
    return SCHULTZ_OK;
}

/* Finds a request by the number the host was given. */
static schultz_window_request *schultz_window_request_at(
    const schultz_window *window, uint32_t number)
{
    uint32_t i;

    if (window == NULL || number == 0u) {
        return NULL;
    }
    for (i = 0; i < SCHULTZ_FRAME_REQUESTS; i++) {
        if (window->requests[i].request == number) {
            return (schultz_window_request *)&window->requests[i];
        }
    }
    return NULL;
}

uint32_t schultz_window_file_count(const schultz_window *window,
                                  uint32_t request)
{
    const schultz_window_request *found = schultz_window_request_at(window,
                                                                  request);

    return (found == NULL) ? 0u : found->count;
}

const char *schultz_window_file_path(const schultz_window *window,
                                    uint32_t request, uint32_t index)
{
    const schultz_window_request *found = schultz_window_request_at(window,
                                                                  request);

    if (found == NULL || index >= found->count) {
        return NULL;
    }
    return found->paths[index];
}

int32_t schultz_window_file_filter(const schultz_window *window,
                                  uint32_t request)
{
    const schultz_window_request *found = schultz_window_request_at(window,
                                                                  request);

    return (found == NULL) ? -1 : found->filter;
}

/*
 * Hands over whatever answered since the last window. Run at the top of a
 * window, on the window loop's own thread, which is what makes it safe for the
 * host to touch the tree from the event it gets.
 */
static void schultz_window_deliver_files(schultz_window *window)
{
    uint32_t i;

    if (window->lock == NULL) {
        return;
    }
    SDL_LockMutex(window->lock);
    for (i = 0; i < SCHULTZ_FRAME_REQUESTS; i++) {
        schultz_window_request *request = &window->requests[i];
        schultz_event event;

        /* Last window's answer has been read by now, so its paths go. */
        if (request->delivered) {
            schultz_window_request_clear(request);
            continue;
        }
        if (request->request == 0u || !request->done) {
            continue;
        }
        memset(&event, 0, sizeof(event));
        event.type = request->cancelled ? SCHULTZ_EVENT_FILES_CANCELLED
                                        : SCHULTZ_EVENT_FILES_CHOSEN;
        event.target = SCHULTZ_HANDLE_NONE;
        /* The request number rides in the token, which is where a host
         * already looks to find out what an event is about. */
        event.token = request->request;
        request->delivered = 1u;

        /*
         * Posted with the lock held, because the paths the host is about to
         * read live under it. Nothing in the callback path takes this lock
         * again, so there is nothing here to deadlock against.
         */
        schultz_events_post(window->events, &event);
    }
    SDL_UnlockMutex(window->lock);
}

/*
 * Follows the window when it changes size. The platform layer has already
 * rebuilt the buffer and the texture by the time this runs, so what is left
 * is everything above it that remembered the old size: the rasterizer's
 * target, the tree's viewport and root, and the host.
 */
static void schultz_window_take_resize(schultz_window *window)
{
    schultz_event event;
    uint32_t width;
    uint32_t height;

    if (!schultz_sdl_window_take_resized(window->platform)) {
        return;
    }
    /* The target is the buffer, so it is told the buffer's own size. What
     * the toolkit works in is settled by take_geometry, below. */
    width  = schultz_sdl_window_width(window->platform);
    height = schultz_sdl_window_height(window->platform);
    schultz_thorvg_set_target(window->backend,
                              schultz_sdl_window_pixels(window->platform),
                              width, height,
                              schultz_sdl_window_stride(window->platform));
    schultz_window_take_geometry(window);
    /* Bounds mark a repaint but not a layout, and the new buffer holds
     * nothing at all, so both are asked for here. */
    schultz_node_invalidate_layout(window->tree,
                                   schultz_tree_root(window->tree));
    schultz_node_invalidate(window->tree, schultz_tree_root(window->tree));

    /* A screen reader places what it reads on screen, so it needs the window
     * to still be where it was told it is. */
    schultz_a11y_set_window(window->a11y, window->whole);

    memset(&event, 0, sizeof(event));
    event.type     = SCHULTZ_EVENT_WINDOW_RESIZED;
    event.target   = schultz_tree_root(window->tree);
    event.position = schultz_point_make(window->safe.width,
                                        window->safe.height);
    schultz_events_post(window->events, &event);
}

int32_t schultz_window_request_close(schultz_window *window)
{
    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    window->quit_requested = 1;
    return SCHULTZ_OK;
}

int32_t schultz_window_update(schultz_window *window, int32_t *out_quit)
{
    schultz_rect dirty;
    schultz_rect region;
    int32_t quit = 0;
    int32_t result;

    if (out_quit != NULL) {
        *out_quit = 0;
    }
    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    /*
     * Reset the arena first, then rebuild the list on top of it. Shaping
     * allocates from the same arena, so the list cannot simply be reset: its
     * chunks live in the memory being reclaimed.
     */
    schultz_arena_reset(&window->arena);
    result = schultz_draw_list_init(&window->list, &window->arena, 0);
    if (result != SCHULTZ_OK) {
        return result;
    }

    /* Anything a file dialog answered since the last window, handed over on
     * this thread before the tree is touched. */
    schultz_window_deliver_files(window);
    schultz_window_take_resize(window);
    /* Before styles and layout, because it decides the root's own place. */
    schultz_window_follow_keyboard(window);

    /*
     * The one clock the toolkit has. Anything that changes on its own rather
     * than in answer to input gets its chance here, before styles are
     * resolved and the dirty region is read.
     */
    /*
     * What an assistive technology asked for, before anything else this
     * window. It was queued on whatever thread the platform called in on, and
     * this is the thread that may touch widgets, so it is carried out here
     * and whatever it changes is drawn by this window rather than the next.
     */
    schultz_a11y_drain(window->a11y);

    window->now_ms = schultz_sdl_ticks_ms();
    schultz_tree_advance(window->tree, window->now_ms);

    /* Resolve first: paint reads only what this produces. */
    schultz_tree_resolve_styles(window->tree);
    schultz_layout_run(window->tree);


    /*
     * The dirty region drives both stages: the rasterizer works only inside
     * it, and only it is uploaded. Everything outside keeps the pixels from
     * the previous window.
     */
    result = schultz_tree_dirty_region(window->tree, &dirty);
    if (result != SCHULTZ_OK) {
        return result;
    }
    /*
     * Taken now, because the drawing below clears them and the upload after
     * it still needs to know which areas to send.
     */
    window->dirty_count = schultz_tree_dirty_count(window->tree);
    {
        uint32_t part;

        for (part = 0u; part < window->dirty_count; part++) {
            schultz_tree_dirty_at(window->tree, part, &window->dirty[part]);
        }
    }
    /*
     * An empty region means nothing changed, and the drawing below is then
     * skipped entirely.
     *
     * It has to be said out loud, because everything down there reads an
     * empty rectangle as "no limit" rather than as "nothing". Painting the
     * tree with an empty dirty region emits every command in the window, the
     * rasterizer draws all of them, and the whole buffer is uploaded -- so a
     * window sitting still cost several times what a window with a small
     * animation in it cost. Measured on the demo with its one spinner
     * stopped: 40% of a core, against 9% with it running.
     *
     * The frame is still presented, with an empty region. The buffer already
     * holds the right pixels, so nothing is uploaded; what present still does
     * is put the texture on the screen and then wait for the display, and
     * that wait is what stops the loop spinning. Input is handled after it,
     * as always, which is why this is a skipped frame rather than an early
     * return: a window that stopped reading input because nothing had changed
     * would never change again.
     */
    region = dirty;

    /*
     * The window's own colour, under everything, every window.
     *
     * Nothing else paints it. The root carries a background but has no widget
     * behind it, and a node with no widget draws nothing at all, so whatever
     * the root does not have a child sitting on holds the last thing that was
     * in the buffer. On a phone that reads as a black bar: beside the camera
     * notch, and anywhere a host's own tree happens not to reach.
     *
     * This used to run only when the safe area differed from the window,
     * which covered the notch and left every other gap black. It is one
     * rectangle, clipped to the region being repainted like everything else,
     * so making it unconditional costs almost nothing and there is no longer
     * a case where part of the window belongs to nobody.
     */
    if (!schultz_rect_is_empty(region)) {
        const schultz_resolved_style *style =
            schultz_node_resolved(window->tree,
                                  schultz_tree_root(window->tree));
        uint32_t parts = schultz_tree_dirty_count(window->tree);
        uint32_t part;

        if (style != NULL) {
            schultz_draw_fill_rect(&window->list, window->whole,
                schultz_paint_solid(
                    schultz_resolved_color(style, SCHULTZ_PROP_BACKGROUND)));
        }

        /*
         * One walk of the tree, culled against every area at once, and then
         * the rasterizer run once for each. Walking once matters: the walk
         * costs the same whether it is testing one rectangle or eight, and
         * doing it per area would throw away most of what this is for.
         */
        result = schultz_widget_paint_tree_parts(window->tree, &window->list,
                                                 &window->arena,
                                                 window->dirty, parts);
        if (result != SCHULTZ_OK) {
            return result;
        }
        if (schultz_draw_list_overflowed(&window->list)) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        if (window->tint.a != 0u) {
            schultz_debug_tint_dirty(&window->list, window->tree,
                                     window->tint);
        }

        for (part = 0u; part < parts; part++) {
            result = schultz_draw_list_play(&window->list, &window->painter,
                                            window->dirty[part]);
            if (result != SCHULTZ_OK) {
                return result;
            }
        }
        /*
         * Republish before the dirty region is cleared, because that region
         * is what says whether anything a reader could notice changed. Doing
         * it afterwards looks at an empty region every time and quietly stops
         * republishing at all, which is the exact failure that leaves a
         * screen reader describing a window several clicks out of date.
         */
        schultz_a11y_update(window->a11y);

        schultz_tree_clear_dirty(window->tree);
    }

    /*
     * The platform layer deals in buffer pixels, and the region is in the
     * toolkit's units, so it is scaled here like every other length that
     * crosses over. Left unscaled it names a rectangle a third the size on a
     * screen that packs three pixels to the unit, only that much of the
     * buffer is uploaded, and the rest of the window shows whatever the
     * texture held: a correctly drawn picture with most of it covered up.
     */
    {
        uint32_t part;

        for (part = 0u; part < window->dirty_count; part++) {
            schultz_rect one = window->dirty[part];

            result = schultz_sdl_window_upload(
                window->platform,
                schultz_rect_make(one.x * window->scale,
                                  one.y * window->scale,
                                  one.width * window->scale,
                                  one.height * window->scale));
            if (result != SCHULTZ_OK) {
                return result;
            }
        }
    }
    result = schultz_sdl_window_present(window->platform);
    if (result != SCHULTZ_OK) {
        return result;
    }
    window->frames++;

    /*
     * Tell the reader when the window gains or loses focus, so that what it
     * reads is the window the user is actually looking at.
     */
    {
        int32_t focused = schultz_sdl_window_focused(window->platform);

        if (focused != window->a11y_focus) {
            window->a11y_focus = focused;
            schultz_a11y_set_focused(window->a11y, focused);
        }
    }

    /* Answer whatever the platform is asking. Cheap when nothing is. */
    schultz_a11y_pump(window->a11y);

    /*
     * Input last, so what it changes is drawn by the next window rather than
     * half of it landing in this one.
     */
    result = schultz_sdl_window_process_events(window->platform, window->events,
                                               &quit);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (out_quit != NULL) {
        /* Either the platform asked, or the host did. */
        *out_quit = quit || window->quit_requested;
    }
    return SCHULTZ_OK;
}

int32_t schultz_window_run(schultz_window *window,
                           schultz_window_update_fn update,
                           void *context, uint64_t max_frames)
{
    int32_t quit = 0;
    int32_t result = SCHULTZ_OK;

    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    while (!quit && (max_frames == 0u || window->frames < max_frames)) {
        if (update != NULL) {
            result = update(context);
            if (result != SCHULTZ_OK) {
                return result;
            }
        }
        result = schultz_window_update(window, &quit);
        if (result != SCHULTZ_OK) {
            return result;
        }
    }
    return SCHULTZ_OK;
}
