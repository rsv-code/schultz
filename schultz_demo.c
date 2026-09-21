/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_demo.c
 * @brief The showcase: every pane and every widget the toolkit ships.
 *
 * This is the accounting of the library. It is also the end to end check that
 * the whole path works, since every widget here is built, laid out, painted
 * through ThorVG into a CPU buffer, and presented in a real window.
 *
 * The shell is a border pane: actions across the top, a page list down the
 * left, a status line along the bottom, and the pages themselves in a stack
 * pane in the centre. One page is visible at a time.
 *
 * Nothing here draws by hand outside the drawing page's canvas, and nothing
 * here styles anything with a value of its own. Every colour, every corner
 * radius, every border width and every gap comes from a theme token, so what
 * is on screen is the built in theme and nothing else. Changing a token
 * changes the demo; there is no local override to look past.
 *
 * The two exceptions are both structural rather than decorative: the sizes
 * given to sample boxes so the layout pages have something to show, and one
 * deliberate zero gap in the shell.
 *
 * Press Escape or close the window to exit.
 *
 *   --frames N        draw that many frames and exit, for a smoke test
 *   --screenshot FILE render the scene into memory, write it to FILE as a
 *                     PPM, and exit
 *   --shot-scale N    render that screenshot at N times the resolution, which
 *                     is the same call a printed page would use
 *   --page N          open on page N, which is how the tests reach each one
 *   --toast-top       stack toasts along the top instead
 *   --windowed        open at a fixed size rather than maximized
 *   --video-driver N  ask SDL for a video driver, such as offscreen, for a
 *                     machine with no desktop
 *   --debug-dirty     tint the region being repainted each turn
 *   --no-vsync        present as fast as possible, for benchmarking
 *   --exact-pixels    one unit is one screen pixel, with no scaling
 *   --turn DEGREES    turn the picture on its way to the panel, for a
 *                     display fitted the other way round: 0, 90, 180 or 270
 *   --light           start from the built in light theme
 *   --loop-clip       start the silent Video card playing, which it does not
 *                     do on its own: it is the one thing in the demo that
 *                     would decode for as long as the window is open
 *
 * Everything in the demo that moves on its own can be stopped: the spinner in
 * the status bar corner from the toolbar, and the Lottie animation, the
 * sliding block, the turning square, the silent clip and the camera from
 * switches beside them on the Drawing page. That is how to find out what any
 * one of them costs -- stop it and watch the processor. The camera is the
 * other way round: it starts stopped, because opening one turns a light on.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "schultz_event.h"
#include "schultz_font.h"
#include "schultz_camera.h"
#include "schultz_window.h"
#include "schultz_glyphs.h"
#include "schultz_image.h"
#include "schultz_layout.h"
#include "schultz_node.h"
#include "schultz_selection.h"
#include "schultz_render.h"
#include "schultz_video.h"
#include <math.h>

#include "schultz_audio.h"
#include "schultz_resource.h"
#include "schultz_style.h"
#include "schultz_widget.h"
#include "schultz_widgets.h"

enum {
    DEMO_WIDTH  = 1000,
    DEMO_HEIGHT = 700
};

/** @brief Bundled font the demo draws with. */
#define DEMO_FONT_PATH "assets/fonts/DejaVuSans.ttf"
/** @brief A raster image, to show decoding and drawing one. */
#define DEMO_PNG_PATH "assets/images/checker.png"
/** @brief A vector image, which scales rather than being resampled. */
#define DEMO_SVG_PATH "assets/images/schultz.svg"
/** @brief A Lottie animation, which plays off the tree's clock. */
#define DEMO_LOTTIE_PATH "assets/images/pulse.json"
/* A short silent clip, VP9 in WebM, which are the codecs that can be
 * shipped without a patent licence. */
#define DEMO_CLIP_PATH "assets/images/demo-clip.webm"
#define DEMO_FILM_PATH "assets/images/demo-clip-sound.webm"

/*
 * A short chime as a WAV file, in memory.
 *
 * Sixteen bit, one channel, eight thousand a second: the smallest thing every
 * decoder agrees about. Two notes with the second above the first, each faded
 * at both ends so neither starts with a click.
 */
static uint64_t demo_build_wav(unsigned char *into, uint64_t room)
{
    const uint32_t rate = 8000u;
    const uint32_t frames = 3200u;
    const uint32_t data = frames * 2u;
    const uint32_t size = 36u + data;
    unsigned char *at = into;
    uint32_t i;

    if (room < 44u + data) {
        return 0u;
    }
    memcpy(at, "RIFF", 4); at += 4;
    memcpy(at, &size, 4);  at += 4;
    memcpy(at, "WAVEfmt ", 8); at += 8;
    { uint32_t n = 16u;      memcpy(at, &n, 4); at += 4; }
    { uint16_t n = 1u;       memcpy(at, &n, 2); at += 2; }
    { uint16_t n = 1u;       memcpy(at, &n, 2); at += 2; }
    { uint32_t n = rate;     memcpy(at, &n, 4); at += 4; }
    { uint32_t n = rate * 2u; memcpy(at, &n, 4); at += 4; }
    { uint16_t n = 2u;       memcpy(at, &n, 2); at += 2; }
    { uint16_t n = 16u;      memcpy(at, &n, 2); at += 2; }
    memcpy(at, "data", 4); at += 4;
    memcpy(at, &data, 4);  at += 4;

    for (i = 0u; i < frames; i++) {
        double hz = (i < frames / 2u) ? 660.0 : 880.0;
        uint32_t into_note = (i < frames / 2u) ? i : i - frames / 2u;
        uint32_t note_frames = frames / 2u;
        double fade = 1.0;
        int16_t sample;

        if (into_note < 160u) {
            fade = (double)into_note / 160.0;
        } else if (into_note > note_frames - 160u) {
            fade = (double)(note_frames - into_note) / 160.0;
        }
        sample = (int16_t)(sin((double)into_note / (double)rate * hz * 2.0
                               * 3.14159265358979) * 7000.0 * fade);
        memcpy(at, &sample, 2);
        at += 2;
    }
    return (uint64_t)(at - into);
}

/**
 * @brief The pages, in the order they appear down the left.
 *
 * Each is a category rather than a scene, so a widget built later has one
 * obvious place to go.
 */
enum {
    DEMO_PAGE_BASICS = 0,
    DEMO_PAGE_TEXT,
    DEMO_PAGE_PANES,
    DEMO_PAGE_CONTAINERS,
    DEMO_PAGE_LISTS,
    DEMO_PAGE_MENUS,
    DEMO_PAGE_DIALOGS,
    DEMO_PAGE_PICKERS,
    DEMO_PAGE_DRAWING,
    DEMO_PAGE_SELECTION,
    DEMO_PAGE_COUNT
};

/** @brief What the host is told when something is clicked. */
enum {
    DEMO_TOKEN_THEME = 1, /**< Switch between the two built in themes. */
    DEMO_TOKEN_SHOT,      /**< Write an offscreen render. */
    DEMO_TOKEN_QUIT,      /**< Stop the loop and close. */
    DEMO_TOKEN_COUNTER,   /**< The counting button on the basics page. */
    DEMO_TOKEN_MENU,      /**< Open the menu on the menus page. */
    DEMO_TOKEN_DIALOG,    /**< Open the dialog. */
    DEMO_TOKEN_DIALOG_OK, /**< Close it again. */
    DEMO_TOKEN_POPOVER,   /**< Open the popover. */
    DEMO_TOKEN_LINK,      /**< Mark the hyperlink as followed. */
    DEMO_TOKEN_ORDER,     /**< Cycle the button bar's platform order. */
    DEMO_TOKEN_KINDS,     /**< Open the menu showing every row kind. */
    DEMO_TOKEN_TOAST,     /**< Show a toast. */
    DEMO_TAG_LINK,        /**< The pressable link inside a sentence. */
    DEMO_TAG_TERM,        /**< The pressable term inside a sentence. */
    DEMO_TOKEN_BOLD,      /**< Mark the editor's selection bold. */
    DEMO_TOKEN_ITALIC,    /**< Mark it italic. */
    DEMO_TOKEN_PLAIN,     /**< Strip it back to plain. */
    DEMO_TOKEN_ASK,       /**< Open the message dialog. */
    DEMO_TOKEN_RENAME,    /**< Open the text input dialog. */
    DEMO_TOKEN_UNITS,     /**< Open the choice dialog. */
    DEMO_TOKEN_BUSY,      /**< Start or stop the busy indicator. */
    DEMO_TOKEN_OPEN_FILE, /**< Open the platform's file picker. */
    DEMO_TOKEN_SAVE_FILE, /**< And its save picker. */
    DEMO_TOKEN_OPEN_FOLDER, /**< And its folder picker. */
    DEMO_TOKEN_KEYBOARD_SHOW, /**< Put the on-screen keyboard up. */
    DEMO_TOKEN_KEYBOARD_HIDE, /**< Take it away. */
    DEMO_TOKEN_KEYBOARD_EMOJI, /**< Offer emoji on it, or stop. */
    DEMO_TOKEN_TONE,      /**< Play a note through the audio system. */
    DEMO_TOKEN_SOUND,     /**< Play a sound that was decoded from a file. */
    DEMO_TOKEN_FEED,      /**< Push an encoded file in pieces and play it. */
    /*
     * One for each thing in the demo that moves on its own. Everything that
     * animates can be stopped, so what any one of them costs can be seen by
     * stopping it and watching the processor.
     */
    DEMO_TOKEN_CORNER,    /**< The busy indicator in the status bar. */
    DEMO_TOKEN_LOTTIE,    /**< The Lottie animation. */
    DEMO_TOKEN_MOVER,     /**< The block that slides back and forth. */
    DEMO_TOKEN_SPIN,      /**< The square that turns. */
    DEMO_TOKEN_CLIP,      /**< The silent looping clip. */
    DEMO_TOKEN_CAMERA,    /**< Open the camera, or let it go. */
    DEMO_TOKEN_LOOP,      /**< Run the camera through an encoder and back. */
    DEMO_TOKEN_CODEC,     /**< Swap the round trip between VP8 and VP9. */
    /* Page buttons carry this plus their page index, so one case handles
     * every one of them however many pages there are. */
    DEMO_TOKEN_PAGE = 100
};

/** @brief Everything the showcase needs that outlives one turn of the loop. */
typedef struct {
    schultz_window     *window;      /**< Window, rasterizer, loop. */
    schultz_tree           *tree;       /**< The widget tree. */
    schultz_events         *events;     /**< Routing state for input. */
    schultz_font_system    *fonts;      /**< Owns the loaded faces. */
    schultz_glyph_cache    *glyphs;     /**< Rasterized glyphs, reused. */
    schultz_resource_table *resources;  /**< Gradients and dash patterns. */
    schultz_image_table    *images;     /**< Decoded images. */
    schultz_theme           theme;      /**< Design tokens the scene reads. */
    schultz_handle          title_font; /**< Face for the theme's title slot. */
    schultz_handle          body_font;  /**< Small face for body text. */

    /* The shell. */
    schultz_handle shell;                   /**< Border pane holding it all. */
    schultz_handle nav;                     /**< Page buttons, down the left. */
    schultz_handle status;                  /**< The status line's label. */
    schultz_handle pages;                   /**< Stack pane of pages. */
    schultz_handle page[DEMO_PAGE_COUNT];   /**< One scroll view each. */
    schultz_handle tab[DEMO_PAGE_COUNT];    /**< The button that shows it. */
    uint32_t       current;                 /**< Which page is showing. */

    /* Things the demo drives while it runs. */
    schultz_handle slider;    /**< Drives the progress bar. */
    schultz_handle upright;   /**< The same slider, on its side. */
    schultz_handle column;    /**< The same bar, filling upward. */
    schultz_handle progress;  /**< Fed from the slider. */
    schultz_handle counter;   /**< Label showing the click count. */
    schultz_handle menu;      /**< Opened by a button and by right click. */
    schultz_handle dialog;    /**< Opened by a button. */
    schultz_handle popover;   /**< Opened by a button. */
    schultz_handle chart;     /**< Canvas holding a drawn chart. */
    schultz_handle link;      /**< A hyperlink the host marks as visited. */
    schultz_handle editor;    /**< Rich text being edited on the Text page. */
    schultz_handle bar;       /**< A button bar, reordered on demand. */
    schultz_handle kinds;     /**< A menu holding one of every row kind. */
    schultz_handle menu_bar;  /**< The shell's menu bar. */
    schultz_handle toolbar;   /**< The shell's toolbar. */
    schultz_handle ask;       /**< A message dialog. */
    schultz_handle rename;    /**< A text input dialog. */
    schultz_handle units;     /**< A choice dialog. */
    schultz_handle busy;      /**< A busy indicator that can be stopped. */
    schultz_handle corner;    /**< The one in the status bar, likewise. */
    schultz_handle lottie;    /**< The Lottie animation, likewise. */
    int32_t        run_mover; /**< Whether the sliding block is moving. */
    int32_t        run_spin;  /**< Whether the turning square is turning. */
    schultz_handle pager;     /**< Pagination. */
    uint32_t       order;     /**< Which platform order the bar is using. */
    schultz_handle mover;     /**< The one node that animates by itself. */
    schultz_handle sheen;     /**< A gradient a card is filled with. */
    schultz_handle dashes;    /**< A dash pattern for a guide line. */
    schultz_handle strokes;   /**< A canvas: the stroke and fill detail. */
    schultz_handle lettering; /**< A gradient text is filled with. */
    schultz_handle arcs;      /**< A canvas: an arc, a pie and a chord. */
    schultz_handle turned;    /**< A canvas showing drawing at an angle. */
    schultz_handle spinner;   /**< A canvas whose drawing turns every frame. */
    float          spin;      /**< How far the spinner has turned, degrees. */
    schultz_handle clip;      /**< A video node playing a bundled clip. */
    schultz_handle film;      /**< The same thing as a player, with sound. */
    schultz_camera *camera;   /**< The open camera, or NULL. Not a handle:
                               *   a camera is hardware, not a widget. */
    schultz_handle camera_view; /**< The node showing it. */
    schultz_handle camera_note; /**< What it is doing, in words. */
    uint32_t       camera_said; /**< What that label last said. */

    /*
     * The round trip: pictures out of the camera, through an encoder, back
     * through a decoder, and on screen beside where they started. No file and
     * no socket anywhere in it.
     */
    schultz_video_encoder *encoder;
    schultz_video_decoder *decoder;
    schultz_handle loop_before;  /**< What the camera gave. */
    schultz_handle loop_after;   /**< What came back. */
    schultz_handle loop_before_image;
    schultz_handle loop_after_image;
    schultz_handle loop_note;    /**< How big the packets are, in words. */
    int32_t        run_loop;
    /*
     * Whether the round trip is the one that opened the camera. Either card
     * may open it, and stopping has to put things back the way that card
     * found them: the preview showing again if it was already showing, and
     * the camera off if nothing had asked for it before.
     */
    int32_t        camera_from_loop;
    uint64_t       loop_bytes;   /**< Since it was started. */
    uint32_t       loop_count;
    uint64_t       loop_started_ms; /**< So the rate can be worked out. */
    uint32_t       loop_codec;   /**< Which one it is encoding with. */
    schultz_handle loop_codec_label;
    schultz_handle photo;     /**< A raster image, from a PNG. */
    schultz_handle logo;      /**< A vector image, from an SVG. */
    schultz_handle pulse;     /**< A Lottie animation. */

    uint32_t clicks;    /**< How many times the counter was clicked. */
    int32_t  light;     /**< Nonzero when the light theme is showing. */
    int32_t  loop_clip; /**< Nonzero to start the silent clip looping. */
    uint64_t start_ms;  /**< When the first turn ran. */
    uint32_t width;     /**< Window width, which the shell fills. */
    uint32_t height;    /**< Window height. */
    float    shot_scale;/**< Scale for the offscreen render action. */

    /* Sound. One stream, written into when the button is pressed. */
    schultz_audio *audio;
    schultz_handle tone;   /**< Samples the demo writes itself. */
    schultz_handle chime;  /**< A sound decoded from a file in memory. */
    schultz_handle feed;   /**< An encoded file pushed in a piece at a time. */
} demo_app;

/** @brief The name shown on each page's button and in the status line. */
static const char *const demo_page_names[DEMO_PAGE_COUNT] = {
    "Basics", "Text", "Panes", "Containers", "Lists",
    "Menus", "Dialogs", "Pickers", "Drawing", "Selection"
};

/* ------------------------------------------------------------- helpers */

/** Points a style property at a theme token, so the theme stays in charge. */
static void set_token(schultz_tree *tree, schultz_handle node,
                      uint32_t prop, uint32_t token)
{
    schultz_node_set_style_property(tree, node, prop,
                                    schultz_value_token(token));
}

/** Sets a child's layout properties, which only its parent pane reads. */
static void set_params(schultz_tree *tree, schultz_handle node,
                       const schultz_layout_params *params)
{
    schultz_node_set_layout_params(tree, node, params);
}

/** Sets how a child sits across its box pane's cross axis. */
static void set_align(schultz_tree *tree, schultz_handle node, uint32_t align)
{
    schultz_layout_params params;

    schultz_node_get_layout_params(tree, node, &params);
    params.align = align;
    set_params(tree, node, &params);
}

/*
 * The four helpers below each change one layout property and leave the rest
 * alone. Reading the node's own properties first matters: a node may be given
 * a slot and then a size, and starting from the defaults each time would
 * quietly throw the earlier one away.
 */

/** Puts a child in one of a border pane's five slots. */
static void set_slot(schultz_tree *tree, schultz_handle node, uint32_t slot)
{
    schultz_layout_params params;

    schultz_node_get_layout_params(tree, node, &params);
    params.slot = slot;
    set_params(tree, node, &params);
}

/** Places a child of an absolute pane. */
static void set_at(schultz_tree *tree, schultz_handle node, float x, float y)
{
    schultz_layout_params params;

    schultz_node_get_layout_params(tree, node, &params);
    params.x = x;
    params.y = y;
    set_params(tree, node, &params);
}

/** Puts a child in a grid cell. */
static void set_cell(schultz_tree *tree, schultz_handle node,
                     uint32_t row, uint32_t column)
{
    schultz_layout_params params;

    schultz_node_get_layout_params(tree, node, &params);
    params.row         = row;
    params.column      = column;
    params.row_span    = 1u;
    params.column_span = 1u;
    set_params(tree, node, &params);
}

/**
 * Gives a node a size and holds it there. A value of zero or less on either
 * axis leaves that axis to be measured, which is what a wrapped label wants:
 * a width to wrap inside and whatever height that turns out to need.
 *
 * The maximum as well as the preference, because a preference is only what a
 * node would like: a box pane stretches its children across the cross axis
 * by default and would fill the width just asked for. A maximum is the rule
 * that says otherwise, and unlike reaching for the node's alignment it
 * changes nothing but the size.
 */
static void set_size(schultz_tree *tree, schultz_handle node,
                     float width, float height)
{
    float wanted_width  = (width > 0.0f) ? width : SCHULTZ_SIZE_UNSET;
    float wanted_height = (height > 0.0f) ? height : SCHULTZ_SIZE_UNSET;

    schultz_node_set_pref_size(tree, node, wanted_width, wanted_height);
    schultz_node_set_max_size(tree, node, wanted_width, wanted_height);
}

/**
 * A titled box holding a column, which is what every page is made of. Returns
 * the column to fill.
 */
static int32_t card(schultz_tree *tree, schultz_handle parent,
                    const char *title, schultz_handle *out_body)
{
    schultz_handle box = SCHULTZ_HANDLE_NONE;
    schultz_handle body;
    int32_t result = schultz_group_box_create(tree, parent, title, &box);

    if (result != SCHULTZ_OK) {
        return result;
    }
    body = schultz_group_box_content(tree, box);
    schultz_node_set_pane(tree, body, schultz_pane_vbox());
    set_token(tree, body, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_SM);
    *out_body = body;
    return SCHULTZ_OK;
}

/** A small labelled block, used to show what a pane does with its children. */
static int32_t block(schultz_tree *tree, schultz_handle parent,
                     const char *text, schultz_handle *out_node)
{
    schultz_handle panel = SCHULTZ_HANDLE_NONE;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    int32_t result = schultz_panel_create(tree, parent, &panel);

    if (result != SCHULTZ_OK) {
        return result;
    }
    set_token(tree, panel, SCHULTZ_PROP_BACKGROUND,
              SCHULTZ_TOKEN_COLOR_SURFACE_RAISED);
    set_token(tree, panel, SCHULTZ_PROP_CORNER_RADIUS,
              SCHULTZ_TOKEN_RADIUS_STRUCTURE);
    set_token(tree, panel, SCHULTZ_PROP_PADDING, SCHULTZ_TOKEN_SPACE_SM);
    schultz_node_set_pane(tree, panel, schultz_pane_stack());
    result = schultz_label_create(tree, panel, text, &label);
    if (result != SCHULTZ_OK) {
        return result;
    }
    if (out_node != NULL) {
        *out_node = panel;
    }
    return SCHULTZ_OK;
}

/** A line of explanation, in the muted colour. */
static int32_t note(schultz_tree *tree, schultz_handle parent,
                    const char *text)
{
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    int32_t result = schultz_label_create(tree, parent, text, &label);

    if (result != SCHULTZ_OK) {
        return result;
    }
    set_token(tree, label, SCHULTZ_PROP_TEXT_COLOR,
              SCHULTZ_TOKEN_COLOR_TEXT_MUTED);
    schultz_label_set_wrap(tree, label, 1);
    return SCHULTZ_OK;
}

/* --------------------------------------------------------- the pages */

static int32_t page_basics(demo_app *app, schultz_handle page)
{
    schultz_tree *tree = app->tree;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result;

    result = card(tree, page, "Button", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &row);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, row, schultz_pane_hbox());
    set_token(tree, row, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);
    result = schultz_button_create(tree, row, "Click me", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_COUNTER);
    result = schultz_label_create(tree, row, "clicked 0", &app->counter);
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Toggles", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_checkbox_create(tree, body, "Remember me", &node);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_switch_create(tree, body, "Play sounds", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_toggle_set_checked(tree, node, 1);
    result = schultz_radio_create(tree, body, "First", 1u, &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_radio_select(tree, node);
    result = schultz_radio_create(tree, body, "Second", 1u, &node);
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Values", &body);
    if (result != SCHULTZ_OK) { return result; }
    {
        /* Sound, which needs no window and is not part of one. */
        schultz_handle sound_body = SCHULTZ_HANDLE_NONE;
        schultz_handle node = SCHULTZ_HANDLE_NONE;

        result = card(tree, page, "Sound", &sound_body);
        if (result != SCHULTZ_OK) { return result; }
        result = schultz_button_create(tree, sound_body, "Play a note", &node);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_token(tree, node, DEMO_TOKEN_TONE);
        result = note(tree, sound_body,
                      "Samples the demo works out and writes into an audio "
                      "stream, which is the same path internet radio would "
                      "take. Nothing is decoded and no file is read.");
        if (result != SCHULTZ_OK) { return result; }

        result = schultz_button_create(tree, sound_body, "Play a chime",
                                       &node);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_token(tree, node, DEMO_TOKEN_SOUND);
        result = note(tree, sound_body,
                      "The other road: a whole sound file, decoded once and "
                      "kept. This one is built in memory rather than read "
                      "from disk, so the demo needs no sound files, but an "
                      "MP3 or an Ogg would take exactly the same call.");
        if (result != SCHULTZ_OK) { return result; }

        result = schultz_button_create(tree, sound_body, "Play it in pieces",
                                       &node);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_token(tree, node, DEMO_TOKEN_FEED);
        result = note(tree, sound_body,
                      "The third road, and the one internet radio takes. The "
                      "same file is handed over two hundred bytes at a time "
                      "and decoded as it plays, which is what a socket gives "
                      "you. Nothing calls the program back for more: it "
                      "writes, and asks how much is left.");
        if (result != SCHULTZ_OK) { return result; }
    }

    result = schultz_slider_create(tree, body,
                                   SCHULTZ_ORIENT_HORIZONTAL, 0.0f, 1.0f, 0.35f,
                                   &app->slider);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_progress_bar_create(tree, body,
                                         SCHULTZ_ORIENT_HORIZONTAL,
                                         &app->progress);
    if (result != SCHULTZ_OK) { return result; }
    result = note(tree, body, "The slider drives the bar.");
    if (result != SCHULTZ_OK) { return result; }

    /*
     * The same pair the other way up, side by side so the two read against
     * each other. A vertical slider counts from the bottom and a vertical
     * bar fills from it, which is the way a column is read.
     */
    {
        schultz_handle row = SCHULTZ_HANDLE_NONE;

        result = schultz_panel_create(tree, body, &row);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_pane(tree, row, schultz_pane_hbox());
        set_token(tree, row, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_LG);
        set_align(tree, row, SCHULTZ_ALIGN_START);

        result = schultz_slider_create(tree, row, SCHULTZ_ORIENT_VERTICAL,
                                       0.0f, 1.0f, 0.35f, &app->upright);
        if (result != SCHULTZ_OK) { return result; }
        result = schultz_progress_bar_create(tree, row,
                                             SCHULTZ_ORIENT_VERTICAL,
                                             &app->column);
        if (result != SCHULTZ_OK) { return result; }
    }
    result = note(tree, body, "And the same pair on their side.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Hyperlink", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_hyperlink_create(tree, body, "Open the handbook", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_node_set_token(tree, node, DEMO_TOKEN_LINK);
    app->link = node;
    result = note(tree, body,
                  "The toolkit opens nothing; the click reaches the host.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Toggle buttons", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_button_bar_create(tree, body, &row);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, row, schultz_pane_hbox());
    set_token(tree, row, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_SM);
    set_align(tree, row, SCHULTZ_ALIGN_START);
    {
        static const char *const names[] = { "Left", "Centre", "Right" };
        uint32_t i;

        for (i = 0; i < 3u; i++) {
            result = schultz_toggle_button_create(tree, row, names[i], 1u,
                                                  &node);
            if (result != SCHULTZ_OK) { return result; }
            if (i == 0u) {
                schultz_toggle_button_select(tree, node);
            }
        }
    }
    result = note(tree, body, "One group, so choosing one clears the rest.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Button bar", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_button_bar_create(tree, body, &app->bar);
    if (result != SCHULTZ_OK) { return result; }
    /* Width only: the bar's height is its buttons', and naming one here
     * would stretch them to it. */
    set_size(tree, app->bar, 420.0f, 0.0f);
    schultz_button_bar_add(tree, app->bar, "Help", SCHULTZ_BUTTON_ROLE_LEFT,
                           NULL);
    schultz_button_bar_add(tree, app->bar, "OK", SCHULTZ_BUTTON_ROLE_OK,
                           NULL);
    schultz_button_bar_add(tree, app->bar, "Cancel",
                           SCHULTZ_BUTTON_ROLE_CANCEL, NULL);
    result = schultz_button_create(tree, body, "Reorder for another platform",
                                   &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_node_set_token(tree, node, DEMO_TOKEN_ORDER);
    result = note(tree, body,
                  "Added Help, OK, Cancel. The bar puts them where the "
                  "platform expects.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Static", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_label_create(tree, body, "A label", &node);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_separator_create(tree, body, SCHULTZ_ORIENT_HORIZONTAL,
                                      &node);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_icon_create(tree, body, app->logo, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 48.0f, 48.0f);
    return note(tree, body, "Label, separator, icon.");
}

/*
 * A span over the first place `what` appears in `text`.
 *
 * Offsets counted by hand drift the moment anyone edits the sentence, so they
 * are found instead. Everything else about the span is left at zero, which
 * means "as the label is", and the caller fills in what it wants changed.
 */
static schultz_span mark(const char *text, const char *what)
{
    schultz_span span;
    const char *found = strstr(text, what);

    memset(&span, 0, sizeof(span));
    if (found != NULL) {
        span.start = (uint32_t)(found - text);
        span.end   = span.start + (uint32_t)strlen(what);
    }
    return span;
}

/* A wrapped, selectable block of prose carrying spans. */
static int32_t styled(demo_app *app, schultz_handle parent, const char *text,
                      const schultz_span *spans, uint32_t count,
                      schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result = schultz_label_create(app->tree, parent, text, &node);

    if (result != SCHULTZ_OK) {
        return result;
    }
    schultz_label_set_wrap(app->tree, node, 1);
    schultz_label_set_selectable(app->tree, node, 1);
    result = schultz_label_set_spans(app->tree, node, spans, count);
    set_size(app->tree, node, 420.0f, 0.0f);
    if (out_node != NULL) {
        *out_node = node;
    }
    return result;
}

static int32_t page_text(demo_app *app, schultz_handle page)
{
    schultz_tree *tree = app->tree;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle tip = SCHULTZ_HANDLE_NONE;
    int32_t result;

    /*
     * A block of prose, which is what a page of writing is made of rather
     * than a form. Nothing here is styled: the paragraph takes the theme's
     * text colour, size and line spacing, and the only thing asked of it is
     * that its text can be taken.
     */
    result = card(tree, page, "Prose", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_label_create(tree, body,
        "This is a block of ordinary text rather than a caption on a "
        "control. Drag across it with the mouse to select part of it, "
        "double click for a word, or triple click for the whole block, then "
        "press control with C to copy it. On a touch screen, hold a finger "
        "still on a word until it is selected and then move the two grips.",
        &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_label_set_wrap(tree, node, 1);
    schultz_label_set_selectable(tree, node, 1);
    set_size(tree, node, 420.0f, 0.0f);
    result = note(tree, body,
                  "A Label that has been asked to be selectable. It is not "
                  "focusable, so it stays out of the tab order.");
    if (result != SCHULTZ_OK) { return result; }

    /*
     * Rich text. One label, one string, and spans saying which stretches of
     * it are not set the way the label is. Every colour comes from a theme
     * token, the same as everything else on this page.
     */
    {
        static const char *const weights =
            "Bold, italic and bold italic are not settings on a face. They "
            "are members of its family, and asking for one is how a Bold "
            "button works without knowing which face the words are in.";
        schultz_span spans[3];

        result = card(tree, page, "Weight and slant", &body);
        if (result != SCHULTZ_OK) { return result; }
        spans[0] = mark(weights, "Bold");
        spans[0].bold = 1u;
        spans[1] = mark(weights, "italic");
        spans[1].italic = 1u;
        spans[2] = mark(weights, "bold italic");
        spans[2].bold   = 1u;
        spans[2].italic = 1u;
        result = styled(app, body, weights, spans, 3u, NULL);
        if (result != SCHULTZ_OK) { return result; }
        result = note(tree, body,
                      "Three spans on one Label. The bold italic phrase is "
                      "drawn from a fourth face, not from the upright one "
                      "leaned over.");
        if (result != SCHULTZ_OK) { return result; }
    }

    {
        static const char *const mixed =
            "A phrase can be larger than the line it sits on, and a term "
            "like schultz_label_set_spans can be set in the code face while "
            "the sentence round it is not.";
        schultz_span spans[2];

        result = card(tree, page, "Size and face", &body);
        if (result != SCHULTZ_OK) { return result; }
        spans[0] = mark(mixed, "larger");
        spans[0].size = 24.0f;
        spans[1] = mark(mixed, "schultz_label_set_spans");
        spans[1].font = schultz_theme_font(&app->theme,
                                           SCHULTZ_TOKEN_FONT_MONO);
        result = styled(app, body, mixed, spans, 2u, NULL);
        if (result != SCHULTZ_OK) { return result; }
        result = note(tree, body,
                      "The line is as tall as the tallest face on it, and "
                      "everything on it sits on one baseline.");
        if (result != SCHULTZ_OK) { return result; }
    }

    {
        static const char *const marked =
            "A span can colour its words, wash a background behind them, "
            "underline them, or strike them through.";
        schultz_span spans[4];

        result = card(tree, page, "Colour and rules", &body);
        if (result != SCHULTZ_OK) { return result; }
        spans[0] = mark(marked, "colour its words");
        spans[0].color = schultz_theme_color(&app->theme,
                                             SCHULTZ_TOKEN_COLOR_ACCENT);
        spans[1] = mark(marked, "wash a background behind them");
        /*
         * The inverse pair, because a background behind words has to be
         * chosen together with the words. Setting only the background leaves
         * the text at the page's own colour, and on a surface near that
         * colour the phrase comes out looking struck out rather than marked.
         */
        spans[1].background = schultz_theme_color(
            &app->theme, SCHULTZ_TOKEN_COLOR_SURFACE_INVERSE);
        spans[1].color = schultz_theme_color(
            &app->theme, SCHULTZ_TOKEN_COLOR_TEXT_ON_INVERSE);
        spans[2] = mark(marked, "underline them");
        spans[2].underline = 1u;
        spans[3] = mark(marked, "strike them through");
        spans[3].strikethrough = 1u;
        result = styled(app, body, marked, spans, 4u, NULL);
        if (result != SCHULTZ_OK) { return result; }
        result = note(tree, body,
                      "Where the two rules go comes from the face itself, so "
                      "a larger phrase is underlined further down and more "
                      "thickly.");
        if (result != SCHULTZ_OK) { return result; }
    }

    {
        static const char *const linked =
            "A stretch of a sentence can point somewhere, the way a link "
            "inside a paragraph does, and it travels with the words when "
            "they are copied.";
        schultz_span span;

        result = card(tree, page, "Links", &body);
        if (result != SCHULTZ_OK) { return result; }
        span = mark(linked, "point somewhere");
        span.link      = "https://example.org/";
        span.underline = 1u;
        span.clickable = 1u;
        span.tag       = DEMO_TAG_LINK;
        span.color     = schultz_theme_color(&app->theme,
                                             SCHULTZ_TOKEN_COLOR_ACCENT);
        result = styled(app, body, linked, &span, 1u, NULL);
        if (result != SCHULTZ_OK) { return result; }
        result = note(tree, body,
                      "The pointer turns to a hand over it, and pressing it "
                      "reports which stretch was pressed. The toolkit opens "
                      "nothing; what a press means is the program's "
                      "business.");
        if (result != SCHULTZ_OK) { return result; }
    }

    {
        static const char *const everything =
            "Everything at once: a bold opening, a term in the code face, a "
            "larger word, something marked, something struck out, and a "
            "closing phrase in another colour.";
        schultz_span spans[6];

        result = card(tree, page, "All of it together", &body);
        if (result != SCHULTZ_OK) { return result; }
        spans[0] = mark(everything, "a bold opening");
        spans[0].bold = 1u;
        spans[1] = mark(everything, "the code face");
        spans[1].font = schultz_theme_font(&app->theme,
                                           SCHULTZ_TOKEN_FONT_MONO);
        spans[2] = mark(everything, "larger word");
        spans[2].size   = 22.0f;
        spans[2].italic = 1u;
        spans[3] = mark(everything, "something marked");
        spans[3].background = schultz_theme_color(
            &app->theme, SCHULTZ_TOKEN_COLOR_SURFACE_INVERSE);
        spans[3].color = schultz_theme_color(
            &app->theme, SCHULTZ_TOKEN_COLOR_TEXT_ON_INVERSE);
        spans[3].underline = 1u;
        spans[4] = mark(everything, "something struck out");
        spans[4].strikethrough = 1u;
        spans[5] = mark(everything, "closing phrase in another colour");
        spans[5].color = schultz_theme_color(&app->theme,
                                             SCHULTZ_TOKEN_COLOR_ACCENT);
        /*
         * Pressable without being a link. This is what a keyword in a code
         * editor would be: the program says which of its own things the
         * stretch is, and hears that back when it is pressed.
         */
        spans[1].clickable = 1u;
        spans[1].tag       = DEMO_TAG_TERM;
        result = styled(app, body, everything, spans, 6u, NULL);
        if (result != SCHULTZ_OK) { return result; }
        result = note(tree, body,
                      "Six spans, given in no particular order. Drag across "
                      "the lot and copy it: the markup keeps the bold, the "
                      "colours and the rules.");
        if (result != SCHULTZ_OK) { return result; }
    }

    /*
     * The same spans, on text that can be typed into. The three buttons are
     * the two halves of a formatting button: something selected is marked
     * where it stands, and nothing selected sets what comes next.
     */
    {
        static const char *const editable =
            "Edit this. Select a phrase and press a button, or press one "
            "with nothing selected and type.";
        schultz_handle row = SCHULTZ_HANDLE_NONE;
        schultz_handle node = SCHULTZ_HANDLE_NONE;
        schultz_span span;

        result = card(tree, page, "Editing rich text", &body);
        if (result != SCHULTZ_OK) { return result; }

        result = schultz_text_area_create(tree, body, editable,
                                          &app->editor);
        if (result != SCHULTZ_OK) { return result; }
        schultz_text_area_set_visible_lines(tree, app->editor, 3u);
        set_size(tree, app->editor, 420.0f, 0.0f);

        /* Something marked to begin with, so there is a span to edit round. */
        span = mark(editable, "Edit this.");
        span.bold = 1u;
        result = schultz_text_field_set_spans(tree, app->editor, &span, 1u);
        if (result != SCHULTZ_OK) { return result; }

        result = schultz_panel_create(tree, body, &row);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_pane(tree, row, schultz_pane_hbox());
        set_token(tree, row, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);

        result = schultz_button_create(tree, row, "Bold", &node);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_token(tree, node, DEMO_TOKEN_BOLD);
        result = schultz_button_create(tree, row, "Italic", &node);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_token(tree, node, DEMO_TOKEN_ITALIC);
        result = schultz_button_create(tree, row, "Plain", &node);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_token(tree, node, DEMO_TOKEN_PLAIN);

        result = note(tree, body,
                      "Marks stay on the characters they were put on. Type "
                      "in front of the bold words and the new text stays "
                      "plain; type inside them and it joins them. Undo takes "
                      "back the look along with the words.");
        if (result != SCHULTZ_OK) { return result; }
    }

    result = card(tree, page, "On-screen keyboard", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_text_field_create(tree, body, "type with the keys", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 260.0f, 0.0f);
    result = schultz_text_field_create(tree, body, "digits only", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 260.0f, 0.0f);
    /* A number field opens on the digits and is offered no emoji, whatever
     * the window says. */
    schultz_text_field_set_input_type(tree, node, SCHULTZ_INPUT_NUMBER);
    result = schultz_button_create(tree, body, "Show keyboard", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_KEYBOARD_SHOW);
    result = schultz_button_create(tree, body, "Hide keyboard", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_KEYBOARD_HIDE);
    result = schultz_button_create(tree, body, "Emoji on or off", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_KEYBOARD_EMOJI);
    result = note(tree, body,
                  "A keyboard the toolkit draws, for a machine with a touch "
                  "screen and no keys. Focus either field to bring it up. It "
                  "is wired to always show here so that it can be seen on a "
                  "desktop; a real application asks for it only where the "
                  "platform has none.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Text field", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_text_field_create(tree, body, "type here", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 260.0f, 0.0f);
    result = schultz_tooltip_create(tree, "Hover me for a second", &tip);
    if (result != SCHULTZ_OK) { return result; }
    schultz_tooltip_watch(tree, tip, node, 600u);
    result = note(tree, body, "With a tooltip on a hover delay.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Text area", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_text_area_create(tree, body,
        "Wrapped, scrolling, editable text. Keep typing past the bottom and "
        "this scrolls rather than growing.", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 320.0f, 0.0f);
    schultz_text_area_set_visible_lines(tree, node, 4u);

    result = card(tree, page, "Password field", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_password_field_create(tree, body, "hunter2", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 260.0f, 0.0f);
    result = note(tree, body,
                  "A text field with a mask. Cut and copy are refused while "
                  "it is masked.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Combo box", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_combo_box_create(tree, body, &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_combo_box_add(tree, node, "Comfortable");
    schultz_combo_box_add(tree, node, "Cosy");
    schultz_combo_box_add(tree, node, "Compact");
    schultz_combo_box_select(tree, node, 0u);
    set_size(tree, node, 180.0f, 0.0f);
    return SCHULTZ_OK;
}

static int32_t page_panes(demo_app *app, schultz_handle page)
{
    schultz_tree *tree = app->tree;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_handle host = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_grid_track tracks[2];
    int32_t result;
    uint32_t i;

    result = card(tree, page, "VBox: a column", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &host);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, host, schultz_pane_vbox());
    set_token(tree, host, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_XS);
    for (i = 0; i < 3u; i++) {
        char text[8];
        snprintf(text, sizeof(text), "%u", i + 1u);
        result = block(tree, host, text, NULL);
        if (result != SCHULTZ_OK) { return result; }
    }

    result = card(tree, page, "HBox: a row", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &host);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, host, schultz_pane_hbox());
    set_token(tree, host, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_XS);
    for (i = 0; i < 3u; i++) {
        char text[8];
        snprintf(text, sizeof(text), "%u", i + 1u);
        result = block(tree, host, text, NULL);
        if (result != SCHULTZ_OK) { return result; }
    }

    result = card(tree, page, "Grid: rows and columns line up", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &host);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, host, schultz_pane_grid());
    set_token(tree, host, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_SM);
    tracks[0].kind = SCHULTZ_TRACK_CONTENT;
    tracks[0].value = 0.0f;
    tracks[1].kind = SCHULTZ_TRACK_CONTENT;
    tracks[1].value = 0.0f;
    schultz_node_set_grid_tracks(tree, host, tracks, 2u, tracks, 2u);
    for (i = 0; i < 4u; i++) {
        char text[16];
        snprintf(text, sizeof(text), "r%u c%u", i / 2u, i % 2u);
        result = block(tree, host, text, &node);
        if (result != SCHULTZ_OK) { return result; }
        set_cell(tree, node, i / 2u, i % 2u);
    }

    result = card(tree, page, "Stack: one on top of another", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &host);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, host, schultz_pane_stack());
    set_size(tree, host, 160.0f, 60.0f);
    result = schultz_panel_create(tree, host, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_token(tree, node, SCHULTZ_PROP_BACKGROUND,
              SCHULTZ_TOKEN_COLOR_SURFACE_SUNKEN);
    result = block(tree, host, "on top", NULL);
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Absolute: placed by coordinate", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &host);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, host, schultz_pane_absolute());
    set_token(tree, host, SCHULTZ_PROP_BACKGROUND,
              SCHULTZ_TOKEN_COLOR_SURFACE_SUNKEN);
    set_size(tree, host, 240.0f, 90.0f);
    result = block(tree, host, "10, 10", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_at(tree, node, 10.0f, 10.0f);
    result = block(tree, host, "120, 45", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_at(tree, node, 120.0f, 45.0f);

    result = card(tree, page, "Border: four edges and a centre", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &host);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, host, schultz_pane_border());
    set_token(tree, host, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_SM);
    set_size(tree, host, 280.0f, 140.0f);
    {
        static const struct { const char *text; uint32_t slot; } slots[] = {
            { "top",    SCHULTZ_SLOT_TOP },
            { "left",   SCHULTZ_SLOT_LEFT },
            { "centre", SCHULTZ_SLOT_CENTER },
            { "right",  SCHULTZ_SLOT_RIGHT },
            { "bottom", SCHULTZ_SLOT_BOTTOM }
        };

        for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
            result = block(tree, host, slots[i].text, &node);
            if (result != SCHULTZ_OK) { return result; }
            set_slot(tree, node, slots[i].slot);
        }
    }

    result = card(tree, page, "Flow: wraps when it runs out of room", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &host);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, host, schultz_pane_flow());
    set_token(tree, host, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_SM);
    set_size(tree, host, 300.0f, 0.0f);
    {
        static const char *const tags[] = {
            "layout", "text", "images", "overlays", "scrolling", "gradients",
            "animation", "printing"
        };

        for (i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
            result = block(tree, host, tags[i], NULL);
            if (result != SCHULTZ_OK) { return result; }
        }
    }
    return SCHULTZ_OK;
}

static int32_t page_containers(demo_app *app, schultz_handle page)
{
    schultz_tree *tree = app->tree;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle split = SCHULTZ_HANDLE_NONE;
    schultz_handle inner = SCHULTZ_HANDLE_NONE;
    schultz_handle tabs = SCHULTZ_HANDLE_NONE;
    schultz_handle tab_page = SCHULTZ_HANDLE_NONE;
    int32_t result;

    result = card(tree, page, "Group box", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = note(tree, body,
                  "Every card on every page is one of these.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Split pane, nested for three ways", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_split_pane_create(tree, body, SCHULTZ_ORIENT_HORIZONTAL,
                                       &split);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, split, 320.0f, 140.0f);
    result = block(tree, schultz_split_pane_half(tree, split, 0u), "left",
                   NULL);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_split_pane_create(tree,
                                       schultz_split_pane_half(tree, split,
                                                               1u),
                                       SCHULTZ_ORIENT_VERTICAL, &inner);
    if (result != SCHULTZ_OK) { return result; }
    result = block(tree, schultz_split_pane_half(tree, inner, 0u), "top right",
                   NULL);
    if (result != SCHULTZ_OK) { return result; }
    result = block(tree, schultz_split_pane_half(tree, inner, 1u),
                   "bottom right", NULL);
    if (result != SCHULTZ_OK) { return result; }
    result = note(tree, body, "Drag either divider.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Tab view", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_tab_view_create(tree, body, &tabs);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, tabs, 320.0f, 120.0f);
    result = schultz_tab_view_add(tree, tabs, "One", &tab_page);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, tab_page, schultz_pane_stack());
    result = block(tree, tab_page, "first page", NULL);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_tab_view_add(tree, tabs, "Two", &tab_page);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, tab_page, schultz_pane_stack());
    result = block(tree, tab_page, "second page", NULL);
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Scroll view", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_scroll_view_create(tree, body, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 320.0f, 100.0f);
    {
        schultz_handle content = schultz_scroll_view_content(tree, node);
        schultz_handle text = SCHULTZ_HANDLE_NONE;

        schultz_node_set_pane(tree, content, schultz_pane_vbox());
        set_token(tree, content, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_SM);
        result = schultz_label_create(tree, content,
            "Schultz paints this paragraph from a node in its tree. HarfBuzz "
            "shapes it, FreeType rasterizes it, libunibreak decides where the "
            "lines may end, and ThorVG composites the result.", &text);
        if (result != SCHULTZ_OK) { return result; }
        schultz_label_set_wrap(tree, text, 1);
        set_size(tree, text, 290.0f, 0.0f);
        result = schultz_label_create(tree, content,
            "Scrolling shifts where these are drawn without laying them out "
            "again.", &text);
        if (result != SCHULTZ_OK) { return result; }
        schultz_label_set_wrap(tree, text, 1);
        set_size(tree, text, 290.0f, 0.0f);
    }

    result = card(tree, page, "Accordion", &body);
    if (result != SCHULTZ_OK) { return result; }
    {
        schultz_handle acc = SCHULTZ_HANDLE_NONE;
        schultz_handle section = SCHULTZ_HANDLE_NONE;
        schultz_handle header = SCHULTZ_HANDLE_NONE;

        result = schultz_accordion_create(tree, body, &acc);
        if (result != SCHULTZ_OK) { return result; }
        schultz_accordion_set_single_expand(tree, acc, 1);
        result = schultz_accordion_add(tree, acc, "General", &section);
        if (result != SCHULTZ_OK) { return result; }
        result = block(tree, section, "one section's content", NULL);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_child_at(tree, acc, 0, &header);
        schultz_accordion_expand(tree, acc, header, 1);

        result = schultz_accordion_add(tree, acc, "Advanced", &section);
        if (result != SCHULTZ_OK) { return result; }
        result = block(tree, section, "another section's content", NULL);
        if (result != SCHULTZ_OK) { return result; }
    }
    result = note(tree, body, "One open at a time on this one.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Scroll bar on its own", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_scroll_bar_create(tree, body, SCHULTZ_ORIENT_HORIZONTAL,
                                       &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 300.0f, 14.0f);
    schultz_scroll_bar_set_range(tree, node, 1000.0f, 300.0f);
    return note(tree, body, "A bar with no view behind it.");
}

static int32_t page_lists(demo_app *app, schultz_handle page)
{
    schultz_tree *tree = app->tree;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    int32_t result;

    result = card(tree, page, "Pagination", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_pagination_create(tree, body, 24u, &app->pager);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, app->pager, SCHULTZ_ALIGN_START);
    result = note(tree, body,
                  "Twenty four pages. The run follows the current page and "
                  "the ends stay reachable.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "List view", &body);
    if (result != SCHULTZ_OK) { return result; }
    {
        static const char *const items[] = {
            "Anchovies", "Basil", "Capers", "Dill", "Endive", "Fennel",
            "Garlic", "Horseradish", "Juniper", "Kale"
        };
        schultz_handle list = SCHULTZ_HANDLE_NONE;
        uint32_t i;

        result = schultz_list_view_create(tree, body, &list);
        if (result != SCHULTZ_OK) { return result; }
        set_size(tree, list, 260.0f, 130.0f);
        schultz_list_view_set_selection_mode(tree, list,
                                             SCHULTZ_SELECT_MULTIPLE);
        for (i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
            schultz_handle row = SCHULTZ_HANDLE_NONE;
            schultz_handle text = SCHULTZ_HANDLE_NONE;

            result = schultz_list_view_add(tree, list, &row);
            if (result != SCHULTZ_OK) { return result; }
            result = schultz_label_create(tree, row, items[i], &text);
            if (result != SCHULTZ_OK) { return result; }
            schultz_node_set_hit_testable(tree, text, 0);
        }
        schultz_list_view_select(tree, list, 0u);
    }
    result = note(tree, body,
                  "Control and shift click to choose several; the arrow keys "
                  "walk it.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Tree view", &body);
    if (result != SCHULTZ_OK) { return result; }
    {
        schultz_handle view = SCHULTZ_HANDLE_NONE;
        schultz_handle branch = SCHULTZ_HANDLE_NONE;
        schultz_handle leaf = SCHULTZ_HANDLE_NONE;
        schultz_handle text = SCHULTZ_HANDLE_NONE;

        result = schultz_tree_view_create(tree, body, &view);
        if (result != SCHULTZ_OK) { return result; }
        set_size(tree, view, 260.0f, 150.0f);

        result = schultz_tree_view_add(tree, view, SCHULTZ_HANDLE_NONE,
                                       &branch);
        if (result != SCHULTZ_OK) { return result; }
        result = schultz_label_create(tree, branch, "Documents", &text);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_hit_testable(tree, text, 0);

        result = schultz_tree_view_add(tree, view, branch, &leaf);
        if (result != SCHULTZ_OK) { return result; }
        result = schultz_label_create(tree, leaf, "notes.txt", &text);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_hit_testable(tree, text, 0);

        result = schultz_tree_view_add(tree, view, branch, &leaf);
        if (result != SCHULTZ_OK) { return result; }
        result = schultz_label_create(tree, leaf, "budget.csv", &text);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_hit_testable(tree, text, 0);

        result = schultz_tree_view_add(tree, view, leaf, &leaf);
        if (result != SCHULTZ_OK) { return result; }
        result = schultz_label_create(tree, leaf, "a nested row", &text);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_hit_testable(tree, text, 0);

        schultz_tree_view_expand(tree, view, branch, 1);
        schultz_tree_view_select(tree, view, branch);

        result = schultz_tree_view_add(tree, view, SCHULTZ_HANDLE_NONE,
                                       &branch);
        if (result != SCHULTZ_OK) { return result; }
        result = schultz_label_create(tree, branch, "Pictures", &text);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_hit_testable(tree, text, 0);
    }
    return note(tree, body,
                "The hierarchy is the node tree. Right opens, left closes.");
}

static int32_t page_menus(demo_app *app, schultz_handle page)
{
    schultz_tree *tree = app->tree;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    int32_t result;

    result = schultz_menu_create(tree, &app->menu);
    if (result != SCHULTZ_OK) { return result; }
    schultz_menu_add(tree, app->menu, "Cut", "Ctrl+X", &node);
    schultz_menu_add(tree, app->menu, "Copy", "Ctrl+C", &node);
    schultz_menu_add(tree, app->menu, "Paste", "Ctrl+V", &node);

    result = card(tree, page, "Menu", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_button_create(tree, body, "Open a menu", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_node_set_token(tree, node, DEMO_TOKEN_MENU);
    result = note(tree, body,
                  "The same menu opens on a right click anywhere on this "
                  "page.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Every kind of row", &body);
    if (result != SCHULTZ_OK) { return result; }
    {
        schultz_handle rows = SCHULTZ_HANDLE_NONE;
        schultz_handle sub = SCHULTZ_HANDLE_NONE;
        schultz_handle content = SCHULTZ_HANDLE_NONE;
        schultz_handle inner = SCHULTZ_HANDLE_NONE;

        result = schultz_menu_create(tree, &app->kinds);
        if (result != SCHULTZ_OK) { return result; }
        schultz_menu_add(tree, app->kinds, "Plain row", "Ctrl+P", &rows);
        schultz_menu_add_separator(tree, app->kinds, NULL);
        schultz_menu_add_check(tree, app->kinds, "Word wrap", NULL, 0, &rows);
        schultz_menu_item_set_checked(tree, rows, 1);
        schultz_menu_add_check(tree, app->kinds, "Line numbers", NULL, 0,
                               &rows);
        schultz_menu_add_separator(tree, app->kinds, NULL);
        schultz_menu_add_radio(tree, app->kinds, "Small", NULL, 1u, &rows);
        schultz_menu_radio_select(tree, rows);
        schultz_menu_add_radio(tree, app->kinds, "Large", NULL, 1u, &rows);
        schultz_menu_add_separator(tree, app->kinds, NULL);
        result = schultz_menu_add_submenu(tree, app->kinds, "Recent files",
                                          &sub);
        if (result != SCHULTZ_OK) { return result; }
        schultz_menu_add(tree, sub, "notes.txt", NULL, &rows);
        schultz_menu_add(tree, sub, "budget.csv", NULL, &rows);
        result = schultz_menu_add_custom(tree, app->kinds, &content);
        if (result != SCHULTZ_OK) { return result; }
        result = schultz_slider_create(tree, content,
                                       SCHULTZ_ORIENT_HORIZONTAL,
                                       0.0f, 1.0f, 0.6f,
                                       &inner);
        if (result != SCHULTZ_OK) { return result; }
        set_size(tree, inner, 140.0f, 0.0f);
    }
    result = schultz_button_create(tree, body, "Open every kind", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_node_set_token(tree, node, DEMO_TOKEN_KINDS);
    result = note(tree, body,
                  "Plain, separator, check, radio, submenu and a custom row "
                  "holding a slider.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Menu button and split menu button", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &row);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, row, schultz_pane_hbox());
    set_token(tree, row, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);
    set_align(tree, row, SCHULTZ_ALIGN_START);
    result = schultz_menu_button_create(tree, row, "Actions", &node);
    if (result != SCHULTZ_OK) { return result; }
    {
        schultz_handle menu = schultz_menu_button_menu(tree, node);
        schultz_handle item = SCHULTZ_HANDLE_NONE;

        schultz_menu_add(tree, menu, "Rename", NULL, &item);
        schultz_menu_add(tree, menu, "Duplicate", NULL, &item);
    }
    result = schultz_split_menu_button_create(tree, row, "Save", &node);
    if (result != SCHULTZ_OK) { return result; }
    {
        schultz_handle menu = schultz_menu_button_menu(tree, node);
        schultz_handle item = SCHULTZ_HANDLE_NONE;

        schultz_menu_add(tree, menu, "Save as", NULL, &item);
        schultz_menu_add(tree, menu, "Save a copy", NULL, &item);
    }
    return note(tree, body,
                "The wide part of the split button clicks; the arrow opens "
                "the menu.");
}

static int32_t page_dialogs(demo_app *app, schultz_handle page)
{
    schultz_tree *tree = app->tree;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    int32_t result;

    result = schultz_dialog_create(tree, "A dialog", &app->dialog);
    if (result != SCHULTZ_OK) { return result; }
    {
        schultz_handle content = schultz_dialog_content(tree, app->dialog);

        schultz_node_set_pane(tree, content, schultz_pane_vbox());
        set_token(tree, content, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);
        result = schultz_label_create(tree, content,
            "Modal, and it dims what is behind it.", &node);
        if (result != SCHULTZ_OK) { return result; }
        result = schultz_button_create(tree, content, "Close", &node);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_token(tree, node, DEMO_TOKEN_DIALOG_OK);
    }

    result = schultz_popover_create(tree, 1, &app->popover);
    if (result != SCHULTZ_OK) { return result; }
    {
        schultz_handle content = schultz_popup_content(tree, app->popover);

        result = schultz_label_create(tree, content,
            "A popover, which closes when you click away.", &node);
        if (result != SCHULTZ_OK) { return result; }
    }

    result = card(tree, page, "Dialog", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_button_create(tree, body, "Open the dialog", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_node_set_token(tree, node, DEMO_TOKEN_DIALOG);

    result = card(tree, page, "Popover", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_button_create(tree, body, "Open a popover", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_node_set_token(tree, node, DEMO_TOKEN_POPOVER);

    result = schultz_message_dialog_create(tree, "Quit",
        SCHULTZ_DIALOG_ICON_QUESTION, SCHULTZ_DIALOG_YES_NO_CANCEL,
        &app->ask);
    if (result != SCHULTZ_OK) { return result; }
    schultz_message_dialog_set_text(tree, app->ask,
        "Close without saving?", "Your changes will be lost.");

    result = schultz_text_input_dialog_create(tree, "Rename", &app->rename);
    if (result != SCHULTZ_OK) { return result; }
    schultz_message_dialog_set_text(tree, app->rename, "What should it be?",
                                    NULL);
    schultz_text_input_dialog_set_value(tree, app->rename, "notes.txt");

    result = schultz_choice_dialog_create(tree, "Units", &app->units);
    if (result != SCHULTZ_OK) { return result; }
    schultz_message_dialog_set_text(tree, app->units, "Measure in what?",
                                    NULL);
    schultz_choice_dialog_add(tree, app->units, "Metric");
    schultz_choice_dialog_add(tree, app->units, "Imperial");

    result = card(tree, page, "Message dialogs", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &row);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, row, schultz_pane_hbox());
    set_token(tree, row, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);
    set_align(tree, row, SCHULTZ_ALIGN_START);
    result = schultz_button_create(tree, row, "Ask", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_ASK);
    result = schultz_button_create(tree, row, "Rename", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_RENAME);
    result = schultz_button_create(tree, row, "Units", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_UNITS);
    result = note(tree, body,
                  "The answer arrives through the event queue; the status "
                  "line shows it.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Busy indicator", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &row);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, row, schultz_pane_hbox());
    set_token(tree, row, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);
    set_align(tree, row, SCHULTZ_ALIGN_START);
    result = schultz_busy_indicator_create(tree, row, &app->busy);
    if (result != SCHULTZ_OK) { return result; }
    schultz_busy_indicator_set_size(tree, app->busy, 28.0f);
    schultz_busy_indicator_start(tree, app->busy);
    result = schultz_button_create(tree, row, "Start or stop", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_BUSY);
    result = note(tree, body,
                  "For work of unknown length, beside the progress bar for "
                  "work of known length.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Toast", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_button_create(tree, body, "Show a toast", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_node_set_token(tree, node, DEMO_TOKEN_TOAST);
    result = note(tree, body,
                  "Several at once stack along the bottom and take "
                  "themselves away.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "File dialogs", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &row);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, row, schultz_pane_hbox());
    set_token(tree, row, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);
    set_align(tree, row, SCHULTZ_ALIGN_START);
    result = schultz_button_create(tree, row, "Open a file", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_OPEN_FILE);
    result = schultz_button_create(tree, row, "Save a file", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_SAVE_FILE);
    result = schultz_button_create(tree, row, "Open a folder", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_OPEN_FOLDER);
    return note(tree, body,
                "The platform's own picker. The answer arrives through the "
                "event queue, and the status line shows it.");
}

static int32_t page_pickers(demo_app *app, schultz_handle page)
{
    schultz_tree *tree = app->tree;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result;

    result = card(tree, page, "Number field", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_number_field_create(tree, body, 0.0, 100.0, 25.0, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 0.0f, 30.0f);
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_number_field_set_step(tree, node, 5.0);
    result = note(tree, body,
                  "Holding a step repeats it. Text that is not a number is "
                  "put back.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Number field with decimals", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_number_field_create(tree, body, 0.0, 1.0, 0.25, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 0.0f, 30.0f);
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_number_field_set_step(tree, node, 0.05);
    schultz_number_field_set_decimals(tree, node, 2u);

    result = card(tree, page, "Date picker", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_date_picker_create(tree, body, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 0.0f, 30.0f);
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_date_picker_set_date(tree, node, 2026, 8, 31);
    schultz_date_picker_set_first_day(tree, node, 1u);
    result = note(tree, body,
                  "Three integers, never a time_t. Weeks start on Monday "
                  "here.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Time picker", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_time_picker_create(tree, body, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 0.0f, 30.0f);
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_time_picker_set_time(tree, node, 14, 30, 0);
    schultz_time_picker_set_show_seconds(tree, node, 1);

    result = card(tree, page, "Colour picker", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_color_picker_create(tree, body,
        schultz_color_rgba(216, 139, 74, 255), &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 60.0f, 30.0f);
    schultz_color_picker_set_alpha_enabled(tree, node, 1);
    return note(tree, body,
                "The square is strips rather than a gradient, so the hue can "
                "move without registering a new one.");
}

/* Draws a bar chart into the canvas, which is the one place the demo draws. */
static void draw_chart(demo_app *app)
{
    static const float values[7] = {
        0.35f, 0.62f, 0.48f, 0.81f, 0.70f, 0.95f, 0.55f
    };
    schultz_tree *tree = app->tree;
    schultz_handle font = schultz_theme_font(&app->theme,
                                             SCHULTZ_TOKEN_FONT_BODY);
    schultz_color line = schultz_theme_color(&app->theme,
                                             SCHULTZ_TOKEN_COLOR_BORDER);
    schultz_color text = schultz_theme_color(&app->theme,
                                             SCHULTZ_TOKEN_COLOR_TEXT);
    schultz_stroke stroke;
    const float width = 300.0f;
    const float height = 140.0f;
    const float gap = 8.0f;
    float bar = (width - gap * 6.0f) / 7.0f;
    uint32_t i;

    if (schultz_canvas_begin(tree, app->chart) != SCHULTZ_OK) {
        return;
    }
    for (i = 0; i < 7u; i++) {
        schultz_rect rect;

        rect.x      = (float)i * (bar + gap);
        rect.width  = bar;
        rect.height = values[i] * (height - 24.0f);
        rect.y      = height - 20.0f - rect.height;
        schultz_canvas_fill_rect(tree, app->chart, rect,
                                 schultz_paint_gradient(app->sheen), 3.0f);
    }
    /*
     * A smooth line through the tops of the bars. Each bar top is a control
     * point and the midpoint between two of them is where the curve passes
     * through, which is the usual way to get one curve through a series of
     * points without solving anything. These are quadratics, and the toolkit
     * raises each one to the cubic that draws the same curve.
     */
    {
        uint8_t steps[7];
        schultz_point points[13];
        schultz_point top[7];
        uint32_t steps_used = 0u;
        uint32_t points_used = 0u;
        schultz_stroke trend = schultz_stroke_solid(
            schultz_theme_color(&app->theme, SCHULTZ_TOKEN_COLOR_ACCENT),
            2.0f);

        for (i = 0; i < 7u; i++) {
            top[i] = schultz_point_make(
                (float)i * (bar + gap) + bar * 0.5f,
                height - 20.0f - values[i] * (height - 24.0f));
        }
        steps[steps_used++] = SCHULTZ_PATH_MOVE;
        points[points_used++] = top[0];
        for (i = 1u; i < 6u; i++) {
            steps[steps_used++] = SCHULTZ_PATH_QUAD;
            points[points_used++] = top[i];
            points[points_used++] = schultz_point_make(
                (top[i].x + top[i + 1u].x) * 0.5f,
                (top[i].y + top[i + 1u].y) * 0.5f);
        }
        steps[steps_used++] = SCHULTZ_PATH_QUAD;
        points[points_used++] = top[6];
        points[points_used++] = top[6];
        schultz_canvas_stroke_path(tree, app->chart, steps, steps_used,
                                   points, points_used, trend);
    }

    stroke = schultz_stroke_solid(line, 1.0f);
    stroke.dash = app->dashes;
    schultz_canvas_line(tree, app->chart,
                        schultz_point_make(0.0f, height - 20.0f),
                        schultz_point_make(width, height - 20.0f), stroke);
    schultz_canvas_text(tree, app->chart, font, "seven readings", 0.0f,
                        height - 6.0f, schultz_paint_solid(text));
    schultz_canvas_end(tree, app->chart);
}

/*
 * Draws the four things a stroke and a fill can be told that they could not
 * be told before: where a dash pattern starts, which rule decides what a self
 * crossing outline encloses, how far a mitred corner may reach, and that text
 * takes a paint rather than a colour.
 */
static void draw_strokes(demo_app *app)
{
    /* A five pointed star, drawn as one unbroken line so it crosses itself. */
    static const schultz_point star[5] = {
        { 28.0f, 25.0f }, { 40.93f, 64.8f }, { 7.07f, 40.2f },
        { 48.93f, 40.2f }, { 15.07f, 64.8f }
    };
    schultz_tree *tree = app->tree;
    schultz_handle font = schultz_theme_font(&app->theme,
                                             SCHULTZ_TOKEN_FONT_BODY);
    schultz_color ink = schultz_theme_color(&app->theme,
                                            SCHULTZ_TOKEN_COLOR_TEXT);
    schultz_paint accent = schultz_paint_solid(
        schultz_theme_color(&app->theme, SCHULTZ_TOKEN_COLOR_ACCENT));
    schultz_point moved[5];
    schultz_stroke stroke;
    schultz_point corner[3];
    uint32_t i;

    if (schultz_canvas_begin(tree, app->strokes) != SCHULTZ_OK) {
        return;
    }

    /* The same pattern twice, half a period apart, so the marks interleave. */
    stroke = schultz_stroke_solid(ink, 3.0f);
    stroke.dash = app->dashes;
    schultz_canvas_line(tree, app->strokes, schultz_point_make(0.0f, 8.0f),
                        schultz_point_make(300.0f, 8.0f), stroke);
    stroke.dash_offset = 6.0f;
    schultz_canvas_line(tree, app->strokes, schultz_point_make(0.0f, 16.0f),
                        schultz_point_make(300.0f, 16.0f), stroke);

    /* The same star under each rule: solid, then with the middle cut out. */
    schultz_canvas_fill_polygon(tree, app->strokes, star, 5u, accent,
                                SCHULTZ_FILL_NONZERO);
    for (i = 0; i < 5u; i++) {
        moved[i] = schultz_point_make(star[i].x + 70.0f, star[i].y);
    }
    schultz_canvas_fill_polygon(tree, app->strokes, moved, 5u, accent,
                                SCHULTZ_FILL_EVEN_ODD);

    /*
     * One sharp corner drawn twice. A generous limit lets the mitre run out
     * to its point; a limit of one cuts it flat, which is what stops a very
     * sharp angle reaching off the screen.
     */
    corner[0] = schultz_point_make(150.0f, 65.0f);
    corner[1] = schultz_point_make(175.0f, 32.0f);
    corner[2] = schultz_point_make(200.0f, 65.0f);
    stroke = schultz_stroke_solid(ink, 8.0f);
    schultz_canvas_stroke_polygon(tree, app->strokes, corner, 3u, stroke, 0);
    for (i = 0; i < 3u; i++) {
        corner[i].x += 70.0f;
    }
    stroke.miter_limit = 1.0f;
    schultz_canvas_stroke_polygon(tree, app->strokes, corner, 3u, stroke, 0);

    /* Text takes a paint, so the gradient reaches the letters themselves. */
    schultz_canvas_text(tree, app->strokes, font, "filled with a gradient",
                        0.0f, 92.0f, schultz_paint_gradient(app->lettering));
    schultz_canvas_end(tree, app->strokes);
}

/*
 * An arc, a pie wedge and a chord, which are the same curve finished three
 * ways. No rasterizer here has an arc of its own, so schultz_arc_path builds
 * one out of quarter turn cubics and hands back a path to fill or stroke.
 */
static void draw_arcs(demo_app *app)
{
    schultz_tree *tree = app->tree;
    schultz_paint accent = schultz_paint_solid(
        schultz_theme_color(&app->theme, SCHULTZ_TOKEN_COLOR_ACCENT));
    schultz_stroke pen = schultz_stroke_solid(
        schultz_theme_color(&app->theme, SCHULTZ_TOKEN_COLOR_TEXT), 3.0f);
    uint8_t steps[SCHULTZ_ARC_STEPS_MAX];
    schultz_point points[SCHULTZ_ARC_POINTS_MAX];
    uint32_t step_count = 0u;
    uint32_t point_count = 0u;

    if (schultz_canvas_begin(tree, app->arcs) != SCHULTZ_OK) {
        return;
    }
    /* Open, and stroked: a line that happens to bend. */
    if (schultz_arc_path(schultz_rect_make(2.0f, 2.0f, 56.0f, 56.0f),
                         200.0f, 250.0f, SCHULTZ_ARC_OPEN, steps,
                         &step_count, points, &point_count) == SCHULTZ_OK) {
        schultz_canvas_stroke_path(tree, app->arcs, steps, step_count, points,
                                   point_count, pen);
    }
    /* Closed through the centre: a wedge. */
    if (schultz_arc_path(schultz_rect_make(72.0f, 2.0f, 56.0f, 56.0f),
                         200.0f, 250.0f, SCHULTZ_ARC_PIE, steps, &step_count,
                         points, &point_count) == SCHULTZ_OK) {
        schultz_canvas_fill_path(tree, app->arcs, steps, step_count, points,
                                 point_count, accent, SCHULTZ_FILL_NONZERO);
    }
    /* Closed straight across: the piece a chord cuts off. */
    if (schultz_arc_path(schultz_rect_make(142.0f, 2.0f, 56.0f, 56.0f),
                         200.0f, 250.0f, SCHULTZ_ARC_CHORD, steps,
                         &step_count, points, &point_count) == SCHULTZ_OK) {
        schultz_canvas_fill_path(tree, app->arcs, steps, step_count, points,
                                 point_count, accent, SCHULTZ_FILL_NONZERO);
    }
    schultz_canvas_end(tree, app->arcs);
}

/*
 * Drawing at an angle. The canvas itself does not turn: it is laid out, sized
 * and clipped exactly as any other, and only what is drawn inside it is
 * turned. Text at an angle is drawn from the font's outlines rather than from
 * the cached upright pictures of each letter, which is what keeps it sharp.
 */
static void draw_turned(demo_app *app)
{
    schultz_tree *tree = app->tree;
    schultz_handle font = schultz_theme_font(&app->theme,
                                             SCHULTZ_TOKEN_FONT_BODY);
    schultz_paint ink = schultz_paint_solid(
        schultz_theme_color(&app->theme, SCHULTZ_TOKEN_COLOR_TEXT));
    schultz_paint accent = schultz_paint_solid(
        schultz_theme_color(&app->theme, SCHULTZ_TOKEN_COLOR_ACCENT));
    schultz_stroke pen = schultz_stroke_solid(
        schultz_theme_color(&app->theme, SCHULTZ_TOKEN_COLOR_ACCENT), 1.0f);
    uint32_t i;

    if (schultz_canvas_begin(tree, app->turned) != SCHULTZ_OK) {
        return;
    }
    /* A picture, with its caption running up the left edge beside it. */
    schultz_canvas_fill_rect(tree, app->turned,
                             schultz_rect_make(28.0f, 6.0f, 80.0f, 80.0f),
                             accent, 4.0f);
    schultz_canvas_rotation_begin(tree, app->turned, -90.0f, 20.0f, 86.0f);
    schultz_canvas_text(tree, app->turned, font, "up the side", 20.0f, 86.0f,
                        ink);
    schultz_canvas_rotation_end(tree, app->turned);

    /*
     * A word at forty five degrees, which is the angle that shows what the
     * outline route is for: no stroke of a letter lines up with the pixel
     * grid, so anything that turned a picture of the text would show it.
     */
    schultz_canvas_rotation_begin(tree, app->turned, -45.0f, 112.0f, 100.0f);
    schultz_canvas_text(tree, app->turned, font, "Schultz", 112.0f, 100.0f,
                        ink);
    schultz_canvas_rotation_end(tree, app->turned);

    /* Stroked text, which is the other thing outlines make possible. */
    {
        schultz_stroke outline = schultz_stroke_solid(
            schultz_theme_color(&app->theme, SCHULTZ_TOKEN_COLOR_TEXT), 1.0f);

        schultz_canvas_stroke_text(tree, app->turned, font, "outlined",
                                   20.0f, 112.0f, outline);
    }

    /*
     * A gauge, whose labels lean to follow the rim. Each is turned by how far
     * round it sits, less the quarter that puts the top of the arc upright,
     * so the numbers lean away from the middle rather than standing on their
     * heads at the ends.
     */
    for (i = 0; i < 7u; i++) {
        float degrees = 180.0f + (float)i * 30.0f;
        char mark[4];

        snprintf(mark, sizeof(mark), "%u", i);
        schultz_canvas_rotation_begin(tree, app->turned, degrees, 215.0f,
                                     62.0f);
        schultz_canvas_line(tree, app->turned,
                            schultz_point_make(249.0f, 62.0f),
                            schultz_point_make(257.0f, 62.0f), pen);
        schultz_canvas_rotation_end(tree, app->turned);

        schultz_canvas_rotation_begin(tree, app->turned, degrees - 270.0f,
                                     215.0f, 62.0f);
        schultz_canvas_text(tree, app->turned, font, mark, 211.0f, 18.0f,
                            ink);
        schultz_canvas_rotation_end(tree, app->turned);
    }
    schultz_canvas_end(tree, app->turned);
}

/* One shape turning a little further every frame, off the same clock the
 * moving panel beside it uses. */
static void draw_spinner(demo_app *app)
{
    schultz_tree *tree = app->tree;
    schultz_paint accent = schultz_paint_solid(
        schultz_theme_color(&app->theme, SCHULTZ_TOKEN_COLOR_ACCENT));

    if (schultz_canvas_begin(tree, app->spinner) != SCHULTZ_OK) {
        return;
    }
    schultz_canvas_rotation_begin(tree, app->spinner, app->spin, 25.0f, 25.0f);
    schultz_canvas_fill_rect(tree, app->spinner,
                             schultz_rect_make(8.0f, 8.0f, 34.0f, 34.0f),
                             accent, 3.0f);
    schultz_canvas_rotation_end(tree, app->spinner);
    schultz_canvas_end(tree, app->spinner);
}

/*
 * A page for trying out selection across widgets.
 *
 * Everything on it sits inside one selection area, so a drag that starts in
 * one paragraph and ends in another selects both, and a copy takes the lot in
 * reading order. The point of the page is to have something worth dragging
 * across: text at three sizes, text in a fixed pitch face, and pictures
 * between them, so that what a selection does to each can be seen rather than
 * taken on trust.
 */
static int32_t page_selection(demo_app *app, schultz_handle page)
{
    schultz_tree *tree = app->tree;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_handle area = SCHULTZ_HANDLE_NONE;
    schultz_handle row = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result;

    result = card(tree, page, "Selection across widgets", &body);
    if (result != SCHULTZ_OK) { return result; }

    result = schultz_selection_area_create(tree, body, &area);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, area, schultz_pane_vbox());
    set_token(tree, area, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_SM);

    /*
     * A heading, in the title face and larger than anything the theme names.
     *
     * The size is a number rather than a token on purpose: the theme's title
     * size is the same sixteen as its body size, because the only heading the
     * toolkit draws sits in a coloured strip that is already saying it is a
     * heading. This page is about seeing a selection cross text of different
     * sizes, so it asks for one.
     */
    result = schultz_label_create(tree, area, "A heading, larger and bolder",
                                 &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_label_set_selectable(tree, node, 1);
    set_token(tree, node, SCHULTZ_PROP_FONT, SCHULTZ_TOKEN_FONT_TITLE);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_FONT_SIZE,
                                    schultz_value_number(28.0f));

    result = schultz_label_create(tree, area,
        "A line between the two, at the title size the theme names, which "
        "is the same as the body size.", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_label_set_selectable(tree, node, 1);
    schultz_label_set_wrap(tree, node, 1);
    set_token(tree, node, SCHULTZ_PROP_FONT, SCHULTZ_TOKEN_FONT_TITLE);
    set_token(tree, node, SCHULTZ_PROP_FONT_SIZE,
              SCHULTZ_TOKEN_FONT_SIZE_TITLE);

    result = schultz_label_create(tree, area,
        "Body text, at the size everything else on this page is. Drag from "
        "here into the paragraph below it and both are selected, which is "
        "the whole point of the page. Copy and the two come across with a "
        "line break between them.", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_label_set_selectable(tree, node, 1);
    schultz_label_set_wrap(tree, node, 1);

    /* A fixed pitch face, which is what code and identifiers want. */
    result = schultz_label_create(tree, area,
        "schultz_selection_area_create(tree, parent, &area);", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_label_set_selectable(tree, node, 1);
    set_token(tree, node, SCHULTZ_PROP_FONT, SCHULTZ_TOKEN_FONT_MONO);

    /* Two pictures, selectable, so a drag can pass over them. */
    result = schultz_panel_create(tree, area, &row);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, row, schultz_pane_hbox());
    set_token(tree, row, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);
    result = schultz_icon_create(tree, row, app->photo, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 64.0f, 64.0f);
    schultz_icon_set_selectable(tree, node, 1);
    result = schultz_icon_create(tree, row, app->logo, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 64.0f, 64.0f);
    schultz_icon_set_selectable(tree, node, 1);

    result = schultz_label_create(tree, area,
        "A caption under the pictures, at the small size.", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_label_set_selectable(tree, node, 1);
    set_token(tree, node, SCHULTZ_PROP_FONT_SIZE, SCHULTZ_TOKEN_FONT_SIZE_SM);

    /*
     * A button in the middle of the prose, still inside the area. A drag that
     * passes over it leaves it alone and carries on to the line beyond, which
     * is the rule about controls, shown rather than described.
     */
    result = schultz_button_create(tree, area, "Not selectable", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);

    result = schultz_label_create(tree, area,
        "A last line, on the far side of the button. A drag that started at "
        "the heading reaches here, and the button's caption does not come "
        "with it.", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_label_set_selectable(tree, node, 1);
    schultz_label_set_wrap(tree, node, 1);

    return note(tree, body,
                "Everything above is one selection area, the button "
                "included. A drag runs from where it started to where it is "
                "now, across however many widgets it crosses, and control "
                "with C copies the text in reading order with a break "
                "between each. The pictures take part too: they mark "
                "themselves when selected, add nothing to the text, and go "
                "to the clipboard as a picture for anything that can take "
                "one. The button takes no part, because its caption is a "
                "label on a machine part rather than something written to "
                "be read. On a phone the text still copies and the picture "
                "does not, because the clipboard underneath carries text "
                "alone.");
}

static int32_t page_drawing(demo_app *app, schultz_handle page)
{
    schultz_tree *tree = app->tree;
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    schultz_handle host = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    int32_t result;

    result = card(tree, page, "Shapes", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &host);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, host, schultz_pane_hbox());
    set_token(tree, host, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);

    result = schultz_ellipse_create(tree, host, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 90.0f, 60.0f);
    set_token(tree, node, SCHULTZ_PROP_BACKGROUND,
              SCHULTZ_TOKEN_COLOR_SURFACE_RAISED);
    set_token(tree, node, SCHULTZ_PROP_BORDER_COLOR,
              SCHULTZ_TOKEN_COLOR_ACCENT);
    set_token(tree, node, SCHULTZ_PROP_BORDER_WIDTH,
              SCHULTZ_TOKEN_BORDER_WIDTH);

    result = schultz_line_create(tree, host, schultz_point_make(0.0f, 0.0f),
                                 schultz_point_make(70.0f, 60.0f), &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 70.0f, 60.0f);
    set_token(tree, node, SCHULTZ_PROP_BORDER_COLOR,
              SCHULTZ_TOKEN_COLOR_ACCENT);
    set_token(tree, node, SCHULTZ_PROP_BORDER_WIDTH,
              SCHULTZ_TOKEN_BORDER_WIDTH);

    {
        schultz_point points[3];

        points[0] = schultz_point_make(0.0f, 0.0f);
        points[1] = schultz_point_make(70.0f, 0.0f);
        points[2] = schultz_point_make(35.0f, 60.0f);
        result = schultz_polygon_create(tree, host, points, 3u, &node);
        if (result != SCHULTZ_OK) { return result; }
        set_size(tree, node, 70.0f, 60.0f);
        set_token(tree, node, SCHULTZ_PROP_BACKGROUND,
                  SCHULTZ_TOKEN_COLOR_ACCENT);
    }

    result = schultz_canvas_create(tree, body, &app->arcs);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, app->arcs, 200.0f, 60.0f);
    draw_arcs(app);
    result = note(tree, body,
                  "An arc open, closed through the centre as a pie, and "
                  "closed straight across as a chord.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Canvas", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_canvas_create(tree, body, &app->chart);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, app->chart, 300.0f, 140.0f);
    draw_chart(app);

    result = card(tree, page, "Images", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &host);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, host, schultz_pane_hbox());
    set_token(tree, host, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);
    result = schultz_icon_create(tree, host, app->photo, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 64.0f, 64.0f);
    result = schultz_icon_create(tree, host, app->logo, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 64.0f, 64.0f);
    result = note(tree, body, "A PNG and an SVG, through the same call.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Animation", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_lottie_create(tree, body, app->pulse, &app->lottie);
    if (result != SCHULTZ_OK) { return result; }
    node = app->lottie;
    set_size(tree, node, 80.0f, 80.0f);
    /* An animation fills the node it is given, so the node has to be its own
     * size rather than stretched across the card. */
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_lottie_set_looping(tree, node, 1);
    schultz_lottie_play(tree, node, 1);
    result = schultz_button_create(tree, body, "Start or stop", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_node_set_token(tree, node, DEMO_TOKEN_LOTTIE);

    result = card(tree, page, "Video", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_video_create(tree, body, 0u, &app->clip);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, app->clip, 240.0f, 135.0f);
    set_align(tree, app->clip, SCHULTZ_ALIGN_START);
    /*
     * The whole file in one write. A host streaming from a socket would write
     * it in pieces instead, and the node cannot tell the difference: it sees
     * bytes either way and never learns where they came from.
     */
    {
        unsigned char *bytes = NULL;
        long size = 0;
        FILE *file = fopen(DEMO_CLIP_PATH, "rb");

        if (file != NULL) {
            if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 0) {
                rewind(file);
                bytes = (unsigned char *)malloc((size_t)size);
                if (bytes != NULL &&
                    fread(bytes, 1, (size_t)size, file) == (size_t)size) {
                    schultz_video_write(tree, app->clip, bytes, (uint64_t)size);
                }
            }
            fclose(file);
            free(bytes);
        }
    }
    schultz_video_set_looping(tree, app->clip, 1);
    /*
     * Stopped unless it was asked for. A node with no controls and nothing
     * to stop it decodes for as long as the window is open, which is the
     * right thing for the landing screen this card is showing and the wrong
     * thing to leave running behind every other page of a demo. --loop-clip
     * turns it on, which is also how the two can be measured against each
     * other.
     */
    if (app->loop_clip) {
        schultz_video_play(tree, app->clip);
    }
    result = note(tree, body,
                  "VP9 in WebM, decoded here and drawn like any other node. "
                  "The host hands over bytes; a file and a socket look the "
                  "same from inside. No controls were asked for, so there "
                  "are none. It starts stopped, because a looping clip with "
                  "nothing to stop it decodes for as long as the window is "
                  "open.");
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_button_create(tree, body, "Start or stop", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_node_set_token(tree, node, DEMO_TOKEN_CLIP);

    /*
     * The same widget as a player: a file it reads itself, its sound track
     * played through the tree's sound system, and every control shown. Set
     * beside the silent clip above, the two say what the control flags do.
     */
    result = card(tree, page, "Video player", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_video_create(tree, body, 1u, &app->film);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, app->film, 400.0f, 297.0f);
    set_align(tree, app->film, SCHULTZ_ALIGN_START);
    schultz_video_open_file(tree, app->film, DEMO_FILM_PATH);
    schultz_video_set_controls(tree, app->film, SCHULTZ_VIDEO_CONTROLS_ALL);
    schultz_video_set_volume(tree, app->film, 0.6f);
    result = note(tree, body,
                  "The same node, given a file to read and asked for every "
                  "control. It decodes on a thread of its own and takes its "
                  "timing from the sound, so a picture that is late is "
                  "dropped rather than played slowly. Drag the bar to move "
                  "about in it.");
    if (result != SCHULTZ_OK) { return result; }

    /*
     * The camera. Closed until somebody asks for it: opening one turns a
     * light on, and a demo that did that the moment it started would be
     * doing it on every page nobody was looking at.
     */
    result = card(tree, page, "Camera", &body);
    if (result != SCHULTZ_OK) { return result; }
    {
        uint32_t found = schultz_camera_count();
        char text[200];

        if (found == 0u) {
            snprintf(text, sizeof(text),
                     "No camera on this machine. Everything below still "
                     "works; there is just nothing to show.");
        } else {
            char name[128];

            if (schultz_camera_name(schultz_camera_device(0u), name,
                                    sizeof(name)) != SCHULTZ_OK) {
                name[0] = '\0';
            }
            snprintf(text, sizeof(text), "%u found. The first is %s",
                     (unsigned)found, name);
        }
        result = note(tree, body, text);
        if (result != SCHULTZ_OK) { return result; }
    }
    result = schultz_camera_preview_create(tree, body, NULL,
                                           &app->camera_view);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, app->camera_view, 320.0f, 240.0f);
    set_align(tree, app->camera_view, SCHULTZ_ALIGN_START);
    result = schultz_button_create(tree, body, "Start or stop", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_node_set_token(tree, node, DEMO_TOKEN_CAMERA);
    result = schultz_label_create(tree, body, "Stopped.",
                                  &app->camera_note);
    if (result != SCHULTZ_OK) { return result; }
    app->camera_said = (uint32_t)-1;

    /*
     * The whole outbound half, with nothing in the middle. Pictures come off
     * the camera, go through an encoder, come back through a decoder and are
     * drawn beside where they started. What a transport would do with the
     * packets is the only piece missing, and that is deliberately not here.
     */
    result = card(tree, page, "Round trip", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &host);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, host, schultz_pane_hbox());
    set_token(tree, host, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);
    set_align(tree, host, SCHULTZ_ALIGN_START);
    result = schultz_icon_create(tree, host, SCHULTZ_HANDLE_NONE,
                                 &app->loop_before);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, app->loop_before, 240.0f, 180.0f);
    result = schultz_icon_create(tree, host, SCHULTZ_HANDLE_NONE,
                                 &app->loop_after);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, app->loop_after, 240.0f, 180.0f);
    {
        schultz_handle row = SCHULTZ_HANDLE_NONE;

        result = schultz_panel_create(tree, body, &row);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_pane(tree, row, schultz_pane_hbox());
        set_token(tree, row, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);
        set_align(tree, row, SCHULTZ_ALIGN_START);

        result = schultz_button_create(tree, row, "Start or stop", &node);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_token(tree, node, DEMO_TOKEN_LOOP);
        result = schultz_button_create(tree, row, "VP8 or VP9", &node);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_token(tree, node, DEMO_TOKEN_CODEC);
        result = schultz_label_create(tree, row, "VP9",
                                      &app->loop_codec_label);
        if (result != SCHULTZ_OK) { return result; }
    }
    result = schultz_label_create(tree, body, "Stopped.", &app->loop_note);
    if (result != SCHULTZ_OK) { return result; }
    result = note(tree, body,
                  "The camera on the left, and on the right the same "
                  "pictures after encoding and decoding again. VP9 makes "
                  "smaller packets and costs more processor to do it; the "
                  "second button swaps between the two so the difference can "
                  "be seen rather than argued about. This starts the camera "
                  "itself if the card above has not. Nothing is sent "
                  "anywhere: the packets go straight back into a decoder, "
                  "which is what a receiver on the far end of a transport "
                  "would do with them.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Motion", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &host);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, host, schultz_pane_absolute());
    set_token(tree, host, SCHULTZ_PROP_BACKGROUND,
              SCHULTZ_TOKEN_COLOR_SURFACE_SUNKEN);
    set_size(tree, host, 300.0f, 50.0f);
    result = schultz_panel_create(tree, host, &app->mover);
    if (result != SCHULTZ_OK) { return result; }
    set_token(tree, app->mover, SCHULTZ_PROP_BACKGROUND,
              SCHULTZ_TOKEN_COLOR_ACCENT);
    set_token(tree, app->mover, SCHULTZ_PROP_CORNER_RADIUS,
              SCHULTZ_TOKEN_RADIUS_CONTROL_SMALL);
    set_size(tree, app->mover, 60.0f, 26.0f);
    set_at(tree, app->mover, 0.0f, 12.0f);
    result = schultz_button_create(tree, body, "Start or stop", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_node_set_token(tree, node, DEMO_TOKEN_MOVER);

    result = schultz_canvas_create(tree, body, &app->spinner);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, app->spinner, 50.0f, 50.0f);
    set_align(tree, app->spinner, SCHULTZ_ALIGN_START);
    draw_spinner(app);
    result = schultz_button_create(tree, body, "Start or stop", &node);
    if (result != SCHULTZ_OK) { return result; }
    set_align(tree, node, SCHULTZ_ALIGN_START);
    schultz_node_set_token(tree, node, DEMO_TOKEN_SPIN);
    result = note(tree, body,
                  "Moved, and turned, every turn off the tree's clock. Each "
                  "has its own switch, because each costs something "
                  "different.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Rotation", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_canvas_create(tree, body, &app->turned);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, app->turned, 300.0f, 120.0f);
    draw_turned(app);
    result = note(tree, body,
                  "A caption up the side of a picture, a word at forty five "
                  "degrees, a dial whose labels follow the rim, and text "
                  "stroked from its outlines. The canvas stays upright; only "
                  "the drawing turns.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Gradient and dashes", &body);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_canvas_create(tree, body, &app->strokes);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, app->strokes, 300.0f, 100.0f);
    draw_strokes(app);
    result = note(tree, body,
                  "Two dashed rules half a period apart, one star under each "
                  "fill rule, a mitred corner with and without a limit, and "
                  "text filled with the gradient.");
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_panel_create(tree, body, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 300.0f, 60.0f);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BACKGROUND,
                                    schultz_value_gradient(app->sheen));
    set_token(tree, node, SCHULTZ_PROP_CORNER_RADIUS,
              SCHULTZ_TOKEN_RADIUS_CONTROL);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BORDER_DASH,
                                    schultz_value_dash(app->dashes));
    set_token(tree, node, SCHULTZ_PROP_BORDER_COLOR,
              SCHULTZ_TOKEN_COLOR_BORDER);
    set_token(tree, node, SCHULTZ_PROP_BORDER_WIDTH,
              SCHULTZ_TOKEN_BORDER_WIDTH);

    /*
     * The same border again, started half a period along the pattern. Every
     * setting a stroke has is a style property, so a widget reaches the
     * offset without anyone building a stroke by hand.
     */
    result = schultz_panel_create(tree, body, &node);
    if (result != SCHULTZ_OK) { return result; }
    set_size(tree, node, 300.0f, 24.0f);
    set_token(tree, node, SCHULTZ_PROP_CORNER_RADIUS,
              SCHULTZ_TOKEN_RADIUS_CONTROL);
    schultz_node_set_style_property(tree, node, SCHULTZ_PROP_BORDER_DASH,
                                    schultz_value_dash(app->dashes));
    schultz_node_set_style_property(tree, node,
                                    SCHULTZ_PROP_BORDER_DASH_OFFSET,
                                    schultz_value_number(5.0f));
    set_token(tree, node, SCHULTZ_PROP_BORDER_COLOR,
              SCHULTZ_TOKEN_COLOR_ACCENT);
    set_token(tree, node, SCHULTZ_PROP_BORDER_WIDTH,
              SCHULTZ_TOKEN_BORDER_WIDTH);
    result = note(tree, body,
                  "The second border is the same pattern started half a "
                  "period along, set from style like every other part of a "
                  "stroke.");
    if (result != SCHULTZ_OK) { return result; }

    result = card(tree, page, "Fading and shadows", &body);
    if (result != SCHULTZ_OK) { return result; }
    {
        schultz_handle row = SCHULTZ_HANDLE_NONE;
        schultz_handle pair = SCHULTZ_HANDLE_NONE;
        schultz_handle one = SCHULTZ_HANDLE_NONE;
        schultz_handle two = SCHULTZ_HANDLE_NONE;
        uint32_t i;
        static const float angles[4] = { 0.0f, 90.0f, 180.0f, 270.0f };

        /*
         * Left: one opacity on the parent, so the two panels under it fade
         * together. Right: the same opacity on each panel instead. Where
         * they overlap the right hand pair is darker, because that pixel is
         * blended twice. Same number, different meaning.
         */
        result = schultz_panel_create(tree, body, &row);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_pane(tree, row, schultz_pane_hbox());
        set_token(tree, row, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);

        for (i = 0; i < 2u; i++) {
            result = schultz_panel_create(tree, row, &pair);
            if (result != SCHULTZ_OK) { return result; }
            schultz_node_set_pane(tree, pair, schultz_pane_absolute());
            set_token(tree, pair, SCHULTZ_PROP_BACKGROUND,
                      SCHULTZ_TOKEN_COLOR_SURFACE_SUNKEN);
            set_size(tree, pair, 140.0f, 80.0f);
            if (i == 0u) {
                schultz_node_set_style_property(tree, pair,
                    SCHULTZ_PROP_OPACITY, schultz_value_number(0.5f));
            }
            result = schultz_panel_create(tree, pair, &one);
            if (result != SCHULTZ_OK) { return result; }
            set_token(tree, one, SCHULTZ_PROP_BACKGROUND,
                      SCHULTZ_TOKEN_COLOR_ACCENT);
            set_size(tree, one, 70.0f, 50.0f);
            set_at(tree, one, 10.0f, 15.0f);
            result = schultz_panel_create(tree, pair, &two);
            if (result != SCHULTZ_OK) { return result; }
            set_token(tree, two, SCHULTZ_PROP_BACKGROUND,
                      SCHULTZ_TOKEN_COLOR_DANGER);
            set_size(tree, two, 70.0f, 50.0f);
            set_at(tree, two, 55.0f, 15.0f);
            if (i == 1u) {
                schultz_node_set_style_property(tree, one,
                    SCHULTZ_PROP_OPACITY, schultz_value_number(0.5f));
                schultz_node_set_style_property(tree, two,
                    SCHULTZ_PROP_OPACITY, schultz_value_number(0.5f));
            }
        }
        result = note(tree, body,
                      "One opacity on the parent, then the same opacity on "
                      "each panel instead. On the left the pair is drawn "
                      "first and faded once, so the overlap is the colour "
                      "on top at half strength. On the right each is faded "
                      "on its own and the overlap is blended twice, which "
                      "is darker and lets the panel underneath show "
                      "through.");
        if (result != SCHULTZ_OK) { return result; }

        /* Four shadows, one for each quarter of the compass. */
        result = schultz_panel_create(tree, body, &row);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_pane(tree, row, schultz_pane_hbox());
        set_token(tree, row, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_LG);
        set_token(tree, row, SCHULTZ_PROP_PADDING, SCHULTZ_TOKEN_SPACE_MD);

        for (i = 0; i < 4u; i++) {
            result = schultz_panel_create(tree, row, &one);
            if (result != SCHULTZ_OK) { return result; }
            set_token(tree, one, SCHULTZ_PROP_BACKGROUND,
                      SCHULTZ_TOKEN_COLOR_SURFACE_RAISED);
            set_token(tree, one, SCHULTZ_PROP_CORNER_RADIUS,
                      SCHULTZ_TOKEN_RADIUS_CONTROL);
            set_size(tree, one, 50.0f, 50.0f);
            set_token(tree, one, SCHULTZ_PROP_SHADOW_COLOR,
                      SCHULTZ_TOKEN_COLOR_SHADOW);
            schultz_node_set_style_property(tree, one,
                SCHULTZ_PROP_SHADOW_ANGLE, schultz_value_number(angles[i]));
            schultz_node_set_style_property(tree, one,
                SCHULTZ_PROP_SHADOW_DISTANCE, schultz_value_number(6.0f));
            schultz_node_set_style_property(tree, one,
                SCHULTZ_PROP_SHADOW_BLUR, schultz_value_number(4.0f));
        }
        result = note(tree, body,
                      "The same shadow at nought, ninety, a hundred and "
                      "eighty and two hundred and seventy degrees. Nought "
                      "is above and it goes round clockwise, so a hundred "
                      "and eighty is below, which is where most designs "
                      "want it. The colour comes from the theme like every "
                      "other colour here.");
        if (result != SCHULTZ_OK) { return result; }
    }
    return SCHULTZ_OK;
}

/* --------------------------------------------------------- the shell */

/* Shows one page and marks its button, which is the whole of navigation. */
static void show_page(demo_app *app, uint32_t index)
{
    schultz_tree *tree = app->tree;
    char line[96];
    uint32_t i;

    if (index >= (uint32_t)DEMO_PAGE_COUNT) {
        return;
    }
    for (i = 0; i < (uint32_t)DEMO_PAGE_COUNT; i++) {
        uint32_t state = schultz_node_get_state(tree, app->page[i]);

        schultz_node_set_state(tree, app->page[i],
            (i == index) ? (state | SCHULTZ_STATE_VISIBLE)
                         : (state & ~(uint32_t)SCHULTZ_STATE_VISIBLE));

    }
    app->current = index;
    schultz_list_view_select(tree, app->nav, index);

    snprintf(line, sizeof(line), "%s   |   %d of %d pages",
             demo_page_names[index], (int)index + 1, (int)DEMO_PAGE_COUNT);
    schultz_status_bar_set_message(tree, app->status, line);
}

/* Rebuilds the theme and hands it to the tree, which keeps its own copy. */
static void apply_theme(demo_app *app)
{
    if (app->light) {
        schultz_theme_preset_light(&app->theme);
    } else {
        schultz_theme_init(&app->theme);
    }
    /* A preset resets every token, so the faces go back in afterwards. */
    schultz_theme_set_font(&app->theme, SCHULTZ_TOKEN_FONT_TITLE,
                           app->title_font);
    schultz_theme_set_font(&app->theme, SCHULTZ_TOKEN_FONT_BODY,
                           app->body_font);
    schultz_tree_set_theme(app->tree, &app->theme);
    schultz_node_invalidate(app->tree, schultz_tree_root(app->tree));
}

static int32_t write_screenshot(demo_app *app, const char *path, float scale);

/*
 * Puts the keyboard inside a dialog once it is showing. Enter and Escape
 * reach a dialog by rising from whatever is focused, so a dialog with
 * nothing inside it focused answers to neither.
 */
/* Defined further down, beside the round trip it stops. */
static void demo_stop_round_trip(demo_app *app);

static void demo_focus_dialog(demo_app *app, schultz_handle box)
{
    schultz_handle content = schultz_dialog_content(app->tree, box);
    uint32_t i;

    for (i = 0; i < schultz_node_child_count(app->tree, content); i++) {
        schultz_handle bar = SCHULTZ_HANDLE_NONE;
        schultz_handle button = SCHULTZ_HANDLE_NONE;

        if (schultz_node_child_at(app->tree, content, i, &bar) == SCHULTZ_OK &&
            schultz_node_child_at(app->tree, bar, 0, &button) == SCHULTZ_OK &&
            schultz_button_bar_role(app->tree, button)
                != SCHULTZ_BUTTON_ROLE_OTHER) {
            schultz_events_set_focus(app->events, button);
            return;
        }
    }
}

/*
 * The host side of the callback contract. The toolkit hands back the token it
 * was given; the host decides what that token means.
 */
static int32_t demo_on_event(void *context, const schultz_event *event)
{
    demo_app *app = (demo_app *)context;

    /*
     * Ctrl+Q, because a machine showing one application on a bare screen has
     * no window furniture to close it with and no desktop to switch to. The
     * toolkit binds no key to this: Escape closes a dialog, and a toolkit
     * that ended the process on it would take the application with the
     * dialog.
     */
    if (event->type == SCHULTZ_EVENT_KEY_DOWN &&
        event->key == (uint32_t)'q' &&
        (event->modifiers & SCHULTZ_MOD_CTRL) != 0u) {
        schultz_window_request_close(app->window);
        return SCHULTZ_EVENT_CONSUMED;
    }
    if (event->type == SCHULTZ_EVENT_WINDOW_RESIZED) {
        /*
         * The shell was given the window's size when it was built, and the
         * driver does not know that. Everything inside it follows from the
         * border pane, so this one line is the whole of the demo's resize
         * handling.
         */
        app->width  = (uint32_t)event->position.x;
        app->height = (uint32_t)event->position.y;
        schultz_node_set_bounds(app->tree, app->shell,
            schultz_rect_make(0, 0, event->position.x, event->position.y));
        schultz_node_invalidate_layout(app->tree, app->shell);
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_FILES_CHOSEN) {
        char line[256];
        uint32_t count = schultz_window_file_count(app->window, event->token);
        const char *first = schultz_window_file_path(app->window, event->token,
                                                    0u);

        snprintf(line, sizeof(line), "%u file%s, filter %d: %s", count,
                 (count == 1u) ? "" : "s",
                 (int)schultz_window_file_filter(app->window, event->token),
                 (first == NULL) ? "" : first);
        schultz_status_bar_flash(app->tree, app->status, line, 6000u);
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_FILES_CANCELLED) {
        schultz_status_bar_flash(app->tree, app->status, "Nothing chosen",
                                 3000u);
        return SCHULTZ_OK;
    }
    if (event->type == SCHULTZ_EVENT_CONTEXT_MENU) {
        schultz_menu_open_at(app->tree, app->menu,
                             event->position);
        return SCHULTZ_EVENT_CONSUMED;
    }
    if (event->type != SCHULTZ_EVENT_CLICK) {
        return SCHULTZ_OK;
    }

    /*
     * A press inside a block of prose, on one of the stretches marked
     * pressable. The event names which one, so the page does not have to
     * work out where the press landed.
     */
    if (event->span != SCHULTZ_SPAN_NONE) {
        if (event->span_tag == DEMO_TAG_LINK) {
            schultz_status_bar_flash(app->tree, app->status,
                                     "The link was pressed", 3000u);
            return SCHULTZ_OK;
        }
        if (event->span_tag == DEMO_TAG_TERM) {
            schultz_status_bar_flash(app->tree, app->status,
                                     "The term was pressed", 3000u);
            return SCHULTZ_OK;
        }
    }

    /*
     * The navigation list says which row was chosen rather than which button
     * was pressed, so the page follows the list's own selection.
     */
    {
        int32_t chosen = schultz_list_view_selected(app->tree, app->nav);

        if (chosen >= 0 && (uint32_t)chosen != app->current) {
            show_page(app, (uint32_t)chosen);
            return SCHULTZ_OK;
        }
    }

    /*
     * The two halves of a formatting button. With something selected the
     * range is marked where it stands; with nothing selected the look is
     * remembered and given to whatever is typed next.
     */
    if (event->token == DEMO_TOKEN_BOLD ||
        event->token == DEMO_TOKEN_ITALIC ||
        event->token == DEMO_TOKEN_PLAIN) {
        schultz_span look;
        const schultz_span *want = NULL;
        uint32_t anchor = 0u;
        uint32_t caret = 0u;

        memset(&look, 0, sizeof(look));
        if (event->token == DEMO_TOKEN_BOLD) {
            look.bold = 1u;
            want = &look;
        } else if (event->token == DEMO_TOKEN_ITALIC) {
            look.italic = 1u;
            want = &look;
        }
        schultz_text_selection(app->tree, app->editor, &anchor, &caret);
        if (anchor != caret) {
            uint32_t low  = (anchor < caret) ? anchor : caret;
            uint32_t high = (anchor < caret) ? caret : anchor;

            schultz_text_field_set_span(app->tree, app->editor, low, high,
                                        want);
        } else {
            schultz_text_field_set_typing(app->tree, app->editor, want);
            schultz_status_bar_flash(app->tree, app->status,
                                     "Now type something", 3000u);
        }
        return SCHULTZ_OK;
    }

    switch (event->token) {
    case DEMO_TOKEN_THEME:
        app->light = !app->light;
        apply_theme(app);
        /* Both canvases hold colours they read from the theme, so redraw. */
        draw_chart(app);
        draw_strokes(app);
        draw_arcs(app);
        draw_turned(app);
        break;
    case DEMO_TOKEN_SHOT:
        write_screenshot(app, "schultz_demo.ppm", app->shot_scale);
        break;
    case DEMO_TOKEN_QUIT:
        schultz_window_request_close(app->window);
        break;
    case DEMO_TOKEN_COUNTER:
        app->clicks++;
        break;
    case DEMO_TOKEN_MENU:
        schultz_menu_open_for(app->tree, app->menu,
                              schultz_events_focus(app->events),
                              SCHULTZ_PLACE_BELOW);
        break;
    case DEMO_TOKEN_DIALOG:
        schultz_dialog_open(app->tree, app->dialog);
        break;
    case DEMO_TOKEN_DIALOG_OK:
        schultz_dialog_close(app->tree, app->dialog);
        break;
    case DEMO_TOKEN_OPEN_FILE:
    case DEMO_TOKEN_SAVE_FILE:
    case DEMO_TOKEN_OPEN_FOLDER: {
        static const schultz_file_filter filters[2] = {
            { "Text files", "txt;md" },
            { "Every file", "*" }
        };
        schultz_file_options options;

        memset(&options, 0, sizeof(options));
        options.title        = "Choose";
        options.filters      = filters;
        options.filter_count = 2u;
        options.allow_many   = (event->token == DEMO_TOKEN_OPEN_FILE) ? 1 : 0;

        if (event->token == DEMO_TOKEN_OPEN_FILE) {
            schultz_window_open_file(app->window, &options, NULL);
        } else if (event->token == DEMO_TOKEN_SAVE_FILE) {
            schultz_window_save_file(app->window, &options, NULL);
        } else {
            schultz_window_open_folder(app->window, &options, NULL);
        }
        break;
    }
    case DEMO_TOKEN_TONE: {
        /*
         * A quarter second of A, worked out here and handed over as bytes.
         * The ends are faded so it starts and stops without a click, which
         * is the one thing a square edge in a buffer always produces.
         */
        static const uint32_t rate = 48000u;
        static const uint32_t samples = 12000u;
        int16_t note[12000];
        uint32_t i;

        if (app->audio == NULL || app->tone == SCHULTZ_HANDLE_NONE) {
            break;
        }
        for (i = 0u; i < samples; i++) {
            double t = (double)i / (double)rate;
            double fade = 1.0;

            if (i < 480u) {
                fade = (double)i / 480.0;
            } else if (i > samples - 480u) {
                fade = (double)(samples - i) / 480.0;
            }
            note[i] = (int16_t)(sin(t * 440.0 * 2.0 * 3.14159265358979)
                                * 8000.0 * fade);
        }
        schultz_audio_stream_write(app->audio, app->tone, note, sizeof(note));
        schultz_audio_stream_play(app->audio, app->tone);
        break;
    }
    case DEMO_TOKEN_SOUND:
        if (app->audio != NULL && app->chime != SCHULTZ_HANDLE_NONE) {
            schultz_sound_play(app->audio, app->chime);
        }
        break;
    case DEMO_TOKEN_FEED: {
        static unsigned char wav[8192];
        uint64_t length;
        uint64_t at;

        if (app->audio == NULL) {
            break;
        }
        /* A decoder plays its file once, so each press gets a new one. */
        if (app->feed != SCHULTZ_HANDLE_NONE) {
            schultz_audio_decoder_destroy(app->audio, app->feed);
            app->feed = SCHULTZ_HANDLE_NONE;
        }
        length = demo_build_wav(wav, sizeof(wav));
        if (length == 0u ||
            schultz_audio_decoder_create(app->audio, &app->feed)
                != SCHULTZ_OK) {
            break;
        }
        for (at = 0u; at < length; at += 200u) {
            uint64_t piece = (length - at < 200u) ? length - at : 200u;

            schultz_audio_decoder_write(app->audio, app->feed, wav + at,
                                        piece);
        }
        /* The file has ended. Without this the decoder would wait for more,
         * which is exactly what a station wants and a file does not. */
        schultz_audio_decoder_finish(app->audio, app->feed);
        schultz_audio_decoder_play(app->audio, app->feed);
        break;
    }
    case DEMO_TOKEN_KEYBOARD_SHOW:
        schultz_window_show_keyboard(app->window);
        break;
    case DEMO_TOKEN_KEYBOARD_HIDE:
        schultz_window_hide_keyboard(app->window);
        break;
    case DEMO_TOKEN_KEYBOARD_EMOJI: {
        schultz_handle keys = schultz_window_keyboard(app->window);

        if (keys != SCHULTZ_HANDLE_NONE) {
            schultz_keyboard_set_emoji(app->tree, keys,
                !schultz_keyboard_offers_emoji(app->tree, keys));
        }
        break;
    }
    case DEMO_TOKEN_TOAST:
        schultz_toast_show(app->tree, "Hello from a toast", 2500u, NULL);
        break;
    case DEMO_TOKEN_ASK:
        schultz_message_dialog_open(app->tree, app->ask);
        demo_focus_dialog(app, app->ask);
        break;
    case DEMO_TOKEN_RENAME:
        schultz_message_dialog_open(app->tree, app->rename);
        demo_focus_dialog(app, app->rename);
        break;
    case DEMO_TOKEN_UNITS:
        schultz_message_dialog_open(app->tree, app->units);
        demo_focus_dialog(app, app->units);
        break;
    case DEMO_TOKEN_BUSY:
        if (schultz_busy_indicator_is_running(app->tree, app->busy)) {
            schultz_busy_indicator_stop(app->tree, app->busy);
        } else {
            schultz_busy_indicator_start(app->tree, app->busy);
        }
        break;
    case DEMO_TOKEN_CORNER:
        if (schultz_busy_indicator_is_running(app->tree, app->corner)) {
            schultz_busy_indicator_stop(app->tree, app->corner);
        } else {
            schultz_busy_indicator_start(app->tree, app->corner);
        }
        break;
    case DEMO_TOKEN_LOTTIE:
        schultz_lottie_play(app->tree, app->lottie,
                            !schultz_lottie_is_playing(app->tree,
                                                       app->lottie));
        break;
    case DEMO_TOKEN_MOVER:
        app->run_mover = !app->run_mover;
        break;
    case DEMO_TOKEN_SPIN:
        app->run_spin = !app->run_spin;
        break;
    case DEMO_TOKEN_CAMERA:
        /*
         * Whatever else was using the camera stops first, and putting things
         * back the way that card found them may itself leave the camera
         * closed. So what this button does is decided after that rather than
         * before: a camera that is on goes off, and one that is off comes on.
         */
        demo_stop_round_trip(app);
        if (app->camera != NULL) {
            /* The node lets go first, so nothing is left pointing at a
             * camera that has been closed. */
            schultz_camera_preview_set_camera(app->tree, app->camera_view,
                                              NULL);
            schultz_camera_close(app->camera);
            app->camera = NULL;
        } else {
            uint64_t device = schultz_camera_device(0u);

            /*
             * 320 by 240, which is the size of the node it is drawn in and
             * the size the round trip below encodes. Asking for more would
             * cost more to encode and show no better.
             */
            if (device != 0u &&
                schultz_camera_open(device, 320u, 240u, &app->camera)
                    == SCHULTZ_OK) {
                schultz_camera_preview_set_camera(app->tree,
                                                  app->camera_view,
                                                  app->camera);
            }
        }
        app->camera_from_loop = 0;
        app->camera_said = (uint32_t)-1;   /* say so on the next turn */
        break;
    case DEMO_TOKEN_LOOP:
        if (app->run_loop) {
            demo_stop_round_trip(app);
        } else {
            /*
             * Open the camera if the card above has not. Asking somebody to
             * press two buttons in the right order, and doing nothing at all
             * when they press the wrong one first, is not a demo.
             */
            if (app->camera == NULL) {
                uint64_t device = schultz_camera_device(0u);

                if (device != 0u &&
                    schultz_camera_open(device, 320u, 240u, &app->camera)
                        == SCHULTZ_OK) {
                    app->camera_from_loop = 1;
                }
            }
            if (app->camera == NULL) {
                schultz_label_set_text(app->tree, app->loop_note,
                                       "No camera on this machine.");
            } else {
                /*
                 * One reader at a time. Each call for a picture takes the
                 * newest there is, so leaving the preview pointed at the
                 * camera as well would have the two of them taking every
                 * other picture.
                 */
                schultz_camera_preview_set_camera(app->tree,
                                                  app->camera_view, NULL);
                app->run_loop   = 1;
                app->loop_bytes = 0u;
                app->loop_count = 0u;
                schultz_label_set_text(app->tree, app->loop_note,
                                       "Running.");
            }
            app->camera_said = (uint32_t)-1;
        }
        break;
    case DEMO_TOKEN_CODEC:
        app->loop_codec = (app->loop_codec == SCHULTZ_VIDEO_CODEC_VP9)
                        ? (uint32_t)SCHULTZ_VIDEO_CODEC_VP8
                        : (uint32_t)SCHULTZ_VIDEO_CODEC_VP9;
        schultz_label_set_text(app->tree, app->loop_codec_label,
                               app->loop_codec == SCHULTZ_VIDEO_CODEC_VP9
                                   ? "VP9" : "VP8");
        /*
         * Letting go of both is enough: the next picture builds them again
         * with whichever codec is now asked for, so this works whether the
         * round trip is running or not.
         */
        schultz_video_encoder_destroy(app->encoder);
        schultz_video_decoder_destroy(app->decoder);
        app->encoder    = NULL;
        app->decoder    = NULL;
        app->loop_bytes = 0u;
        app->loop_count = 0u;
        break;
    case DEMO_TOKEN_CLIP:
        if (schultz_video_is_playing(app->tree, app->clip)) {
            schultz_video_pause(app->tree, app->clip);
        } else {
            schultz_video_play(app->tree, app->clip);
        }
        break;
    case DEMO_TOKEN_KINDS:
        schultz_menu_open_for(app->tree, app->kinds,
                              schultz_events_focus(app->events),
                              SCHULTZ_PLACE_BELOW);
        break;
    case DEMO_TOKEN_LINK:
        /* Only the host knows whether the target was reached. */
        schultz_hyperlink_set_visited(app->tree, app->link, 1);
        break;
    case DEMO_TOKEN_ORDER:
        app->order = (app->order + 1u) % 3u;
        schultz_button_bar_set_order(app->tree, app->bar, app->order);
        break;
    case DEMO_TOKEN_POPOVER:
        schultz_popup_open(app->tree, app->popover,
                             schultz_events_focus(app->events));
        break;
    default:
        break;
    }
    return SCHULTZ_OK;
}

/* Builds one page: a scroll view whose content is a column of cards. */
static int32_t build_page(demo_app *app, uint32_t index,
                          schultz_handle *out_body)
{
    schultz_tree *tree = app->tree;
    schultz_handle view = SCHULTZ_HANDLE_NONE;
    schultz_handle content;
    int32_t result = schultz_scroll_view_create(tree, app->pages, &view);

    if (result != SCHULTZ_OK) {
        return result;
    }
    app->page[index] = view;
    content = schultz_scroll_view_content(tree, view);
    schultz_node_set_pane(tree, content, schultz_pane_vbox());
    set_token(tree, content, SCHULTZ_PROP_PADDING, SCHULTZ_TOKEN_SPACE_MD);
    set_token(tree, content, SCHULTZ_PROP_GAP, SCHULTZ_TOKEN_SPACE_MD);
    *out_body = content;
    return SCHULTZ_OK;
}

static int32_t build_shell(demo_app *app)
{
    schultz_tree *tree = app->tree;
    schultz_handle root = schultz_tree_root(tree);
    schultz_handle actions = SCHULTZ_HANDLE_NONE;
    schultz_handle status_bar = SCHULTZ_HANDLE_NONE;
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_layout_params params;
    int32_t result;
    uint32_t i;

    result = schultz_panel_create(tree, root, &app->shell);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_pane(tree, app->shell, schultz_pane_border());
    schultz_node_set_bounds(tree, app->shell,
                            schultz_rect_make(0, 0, (float)app->width,
                                              (float)app->height));
    set_token(tree, app->shell, SCHULTZ_PROP_BACKGROUND,
              SCHULTZ_TOKEN_COLOR_WINDOW);
    /* Text properties inherit, so the whole showcase picks these up. */
    set_token(tree, app->shell, SCHULTZ_PROP_FONT, SCHULTZ_TOKEN_FONT_BODY);
    set_token(tree, app->shell, SCHULTZ_PROP_TEXT_COLOR,
              SCHULTZ_TOKEN_COLOR_TEXT);

    /*
     * Top: the actions. This becomes a menu bar over a toolbar once those
     * are built, in phases 2 and 3.
     */
    result = schultz_panel_create(tree, app->shell, &actions);
    if (result != SCHULTZ_OK) { return result; }
    set_slot(tree, actions, SCHULTZ_SLOT_TOP);
    schultz_node_set_pane(tree, actions, schultz_pane_vbox());
    /* No gap: the menu bar and the toolbar are meant to sit together as one
     * band. Zero is the absence of spacing rather than a choice about it, so
     * it is the one measurement here that is not a token. */
    schultz_node_set_spacing(tree, actions, 0.0f, 0.0f);
    {
        /* The real menu bar, which replaces phase 1's stand in. */
        schultz_handle menu = SCHULTZ_HANDLE_NONE;
        schultz_handle item = SCHULTZ_HANDLE_NONE;

        result = schultz_menu_bar_create(tree, actions, &app->menu_bar);
        if (result != SCHULTZ_OK) { return result; }
        result = schultz_menu_bar_add(tree, app->menu_bar, "File", &menu);
        if (result != SCHULTZ_OK) { return result; }
        schultz_menu_add(tree, menu, "Save a render", NULL, &item);
        schultz_node_set_token(tree, item, DEMO_TOKEN_SHOT);
        schultz_menu_add_separator(tree, menu, NULL);
        schultz_menu_add(tree, menu, "Quit", "Ctrl+Q", &item);
        schultz_node_set_token(tree, item, DEMO_TOKEN_QUIT);

        result = schultz_menu_bar_add(tree, app->menu_bar, "View", &menu);
        if (result != SCHULTZ_OK) { return result; }
        schultz_menu_add(tree, menu, "Switch theme", NULL, &item);
        schultz_node_set_token(tree, item, DEMO_TOKEN_THEME);
    }
    /* The real toolbar, which replaces phase 1's row of buttons. */
    result = schultz_toolbar_create(tree, actions, SCHULTZ_ORIENT_HORIZONTAL,
                                    &app->toolbar);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_toolbar_add(tree, app->toolbar, "Theme", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_THEME);
    result = schultz_toolbar_add(tree, app->toolbar, "Save a render", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_SHOT);
    result = schultz_toolbar_add_separator(tree, app->toolbar, &node);
    if (result != SCHULTZ_OK) { return result; }
    result = schultz_toolbar_add(tree, app->toolbar, "Say hello", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_TOAST);
    result = schultz_toolbar_add(tree, app->toolbar, "Ask a question", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_ASK);
    result = schultz_toolbar_add_separator(tree, app->toolbar, &node);
    if (result != SCHULTZ_OK) { return result; }
    /*
     * The one animation that runs on every page, so its switch belongs here
     * rather than on a page. Stopping it is what lets the window go
     * completely still, which is the only way to see what the window costs
     * when nothing is moving.
     */
    result = schultz_toolbar_add(tree, app->toolbar, "Corner spinner", &node);
    if (result != SCHULTZ_OK) { return result; }
    schultz_node_set_token(tree, node, DEMO_TOKEN_CORNER);

    /*
     * Left: the pages, as a real list view. This was a column of buttons
     * until phase 4 built the list.
     */
    result = schultz_list_view_create(tree, app->shell, &app->nav);
    if (result != SCHULTZ_OK) { return result; }
    set_slot(tree, app->nav, SCHULTZ_SLOT_LEFT);
    set_size(tree, app->nav, 140.0f, 0.0f);
    for (i = 0; i < (uint32_t)DEMO_PAGE_COUNT; i++) {
        schultz_handle text = SCHULTZ_HANDLE_NONE;

        result = schultz_list_view_add(tree, app->nav, &app->tab[i]);
        if (result != SCHULTZ_OK) { return result; }
        result = schultz_label_create(tree, app->tab[i], demo_page_names[i],
                                      &text);
        if (result != SCHULTZ_OK) { return result; }
        schultz_node_set_hit_testable(tree, text, 0);
        set_token(tree, app->tab[i], SCHULTZ_PROP_PADDING,
                  SCHULTZ_TOKEN_SPACE_MD);
    }
    (void)params;

    /*
     * Bottom: the status line. This becomes a status bar in phase 3.
     */
    /* The real status bar, which replaces phase 1's panel and label. */
    result = schultz_status_bar_create(tree, app->shell, &app->status);
    if (result != SCHULTZ_OK) { return result; }
    set_slot(tree, app->status, SCHULTZ_SLOT_BOTTOM);
    /* A busy indicator in the corner, as a fixed size section. */
    result = schultz_busy_indicator_create(tree, app->shell, &app->corner);
    if (result != SCHULTZ_OK) { return result; }
    node = app->corner;
    schultz_busy_indicator_set_size(tree, node, 16.0f);
    schultz_busy_indicator_start(tree, node);
    result = schultz_status_bar_add_section(tree, app->status, node);
    if (result != SCHULTZ_OK) { return result; }
    status_bar = app->status;
    (void)status_bar;

    /* Centre: the pages, one visible at a time. */
    result = schultz_panel_create(tree, app->shell, &app->pages);
    if (result != SCHULTZ_OK) { return result; }
    set_slot(tree, app->pages, SCHULTZ_SLOT_CENTER);
    schultz_node_set_pane(tree, app->pages, schultz_pane_stack());
    return SCHULTZ_OK;
}

static int32_t build_tree(demo_app *app)
{
    schultz_handle body = SCHULTZ_HANDLE_NONE;
    int32_t result = build_shell(app);
    uint32_t i;

    if (result != SCHULTZ_OK) { return result; }

    for (i = 0; i < (uint32_t)DEMO_PAGE_COUNT; i++) {
        result = build_page(app, i, &body);
        if (result != SCHULTZ_OK) { return result; }

        switch (i) {
        case DEMO_PAGE_BASICS:     result = page_basics(app, body);     break;
        case DEMO_PAGE_TEXT:       result = page_text(app, body);       break;
        case DEMO_PAGE_PANES:      result = page_panes(app, body);      break;
        case DEMO_PAGE_CONTAINERS: result = page_containers(app, body); break;
        case DEMO_PAGE_LISTS:      result = page_lists(app, body);      break;
        case DEMO_PAGE_MENUS:      result = page_menus(app, body);      break;
        case DEMO_PAGE_DIALOGS:    result = page_dialogs(app, body);    break;
        case DEMO_PAGE_PICKERS:    result = page_pickers(app, body);    break;
        case DEMO_PAGE_DRAWING:    result = page_drawing(app, body);    break;
        case DEMO_PAGE_SELECTION:  result = page_selection(app, body);  break;
        default:                   result = SCHULTZ_OK;                 break;
        }
        if (result != SCHULTZ_OK) { return result; }
    }
    return SCHULTZ_OK;
}

/* -------------------------------------------------- the window and main */

/*
 * Puts a picture the host is holding into an icon.
 *
 * The same two calls a video node makes: hand the pixels to the image table,
 * point the node at what comes back, and let go of the one before. Without
 * that last part the table grows a picture a frame for as long as it runs.
 */
static void show_pixels(demo_app *app, schultz_handle icon,
                        schultz_handle *held, const uint32_t *pixels,
                        uint32_t width, uint32_t height)
{
    schultz_handle fresh = SCHULTZ_HANDLE_NONE;

    if (schultz_image_set_pixels(app->images, pixels, width, height, 0u,
                                 &fresh) != SCHULTZ_OK) {
        return;
    }
    schultz_icon_set_image(app->tree, icon, fresh);
    if (*held != SCHULTZ_HANDLE_NONE) {
        schultz_image_unload(app->images, *held);
    }
    *held = fresh;
}

/* Stops the round trip and puts the camera back the way it was found. Safe
 * to call when it was not running. */
static void demo_stop_round_trip(demo_app *app)
{
    if (!app->run_loop) {
        return;
    }
    app->run_loop = 0;
    schultz_video_encoder_destroy(app->encoder);
    schultz_video_decoder_destroy(app->decoder);
    app->encoder = NULL;
    app->decoder = NULL;

    if (app->camera_from_loop) {
        /*
         * Nothing had asked for the camera before this did, so stopping puts
         * it away rather than handing it to a preview nobody started.
         */
        schultz_camera_close(app->camera);
        app->camera           = NULL;
        app->camera_from_loop = 0;
        app->camera_said      = (uint32_t)-1;
    } else {
        /* The card above had it showing, so it gets it back. */
        schultz_camera_preview_set_camera(app->tree, app->camera_view,
                                          app->camera);
    }
    schultz_label_set_text(app->tree, app->loop_note, "Stopped.");
}

/*
 * One turn of the round trip: a picture off the camera, through the encoder,
 * back through the decoder, and both ends on screen.
 *
 * The encoder is built on the first picture rather than when the button was
 * pressed, because until one arrives nobody knows what size the camera
 * settled on.
 */
static void run_round_trip(demo_app *app)
{
    const uint32_t *pixels = NULL;
    const uint32_t *back = NULL;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint64_t when = 0u;
    const void *bytes = NULL;
    uint64_t length = 0u;

    if (app->camera == NULL ||
        schultz_camera_frame(app->camera, &pixels, &width, &height, &when)
            != SCHULTZ_OK) {
        return;                 /* nothing new this turn, which is usual */
    }
    if (app->encoder == NULL) {
        if (schultz_video_encoder_create(app->loop_codec, width, height, 30u,
                                         600000u, &app->encoder)
                != SCHULTZ_OK ||
            schultz_video_decoder_create(app->loop_codec, &app->decoder)
                != SCHULTZ_OK) {
            app->run_loop = 0;
            return;
        }
        app->loop_started_ms = schultz_window_time_ms(app->window);
    }
    show_pixels(app, app->loop_before, &app->loop_before_image, pixels,
                width, height);

    if (schultz_video_encoder_write_frame(app->encoder, pixels, width, height,
                                          when) != SCHULTZ_OK) {
        return;
    }
    while (schultz_video_encoder_read_packet(app->encoder, &bytes, &length,
                                             NULL, NULL) == SCHULTZ_OK) {
        app->loop_bytes += length;
        app->loop_count++;
        if (schultz_video_decoder_write_packet(app->decoder, bytes, length)
                != SCHULTZ_OK) {
            continue;
        }
        while (schultz_video_decoder_read_frame(app->decoder, &back, &width,
                                                &height) == SCHULTZ_OK) {
            show_pixels(app, app->loop_after, &app->loop_after_image, back,
                        width, height);
        }
    }
}

/*
 * Called once per turn of the loop, before anything is drawn. This is the
 * whole of the host's per turn work: move what moves, and let the toolkit
 * work out what that means for the screen.
 */
static int32_t demo_before_draw(void *context)
{
    demo_app *app = (demo_app *)context;
    const float sweep_seconds = 4.0f;
    uint64_t now = schultz_window_time_ms(app->window);
    schultz_rect bounds;
    float phase;
    char label[64];

    if (app->start_ms == 0u) {
        app->start_ms = now;
    }
    phase = (float)(now - app->start_ms) / 1000.0f / sweep_seconds;
    phase -= (float)(long)phase;      /* keep the fraction only */
    if (phase > 0.5f) {
        phase = 1.0f - phase;         /* and bounce back */
    }
    /*
     * Both of these belong to the Drawing page, so neither is touched while
     * another page is showing. Moving a node on a hidden page changes nothing
     * anybody can see and still costs the work of changing it.
     */
    if (app->run_mover && app->current == DEMO_PAGE_DRAWING &&
        schultz_node_get_bounds(app->tree, app->mover, &bounds)
            == SCHULTZ_OK) {
        bounds.x = 240.0f * (phase * 2.0f);
        schultz_node_set_bounds(app->tree, app->mover, bounds);
    }

    /*
     * The square beside it turns rather than moves. Its drawing is recorded
     * again each frame because the angle is part of what was recorded, which
     * is the one case where a retained canvas is rebuilt as often as an
     * immediate one would be.
     */
    if (app->run_spin && app->current == DEMO_PAGE_DRAWING) {
        app->spin = (float)(now - app->start_ms) * 0.06f;
        app->spin -= 360.0f * (float)(long)(app->spin / 360.0f);
        draw_spinner(app);
    }

    /*
     * What the camera is doing, in words, and only when it changes. Waiting
     * for permission is a state a person needs told about: on a phone it can
     * last as long as it takes them to answer a prompt.
     */
    {
        uint32_t state = (app->camera == NULL)
                       ? (uint32_t)-2
                       : schultz_camera_permission(app->camera);

        if (state != app->camera_said) {
            const char *says = "Stopped.";

            if (state == SCHULTZ_CAMERA_WAITING) {
                says = "Waiting for permission.";
            } else if (state == SCHULTZ_CAMERA_ALLOWED) {
                says = "Running.";
            } else if (state == SCHULTZ_CAMERA_REFUSED) {
                says = "Permission refused.";
            }
            schultz_label_set_text(app->tree, app->camera_note, says);
            app->camera_said = state;
        }
    }

    if (app->run_loop && app->current == DEMO_PAGE_DRAWING) {
        run_round_trip(app);
        /*
         * The average packet, in words, once a second or so. A number that
         * moved every turn would be unreadable and would reshape the text
         * sixty times a second to say so.
         */
        if (app->loop_count > 0u && (app->loop_count % 15u) == 0u) {
            char says[120];
            uint64_t ran = schultz_window_time_ms(app->window) -
                           app->loop_started_ms;
            unsigned rate = (ran > 0u)
                          ? (unsigned)((uint64_t)app->loop_count * 1000u / ran)
                          : 0u;

            /*
             * Pictures a second is the number that answers whether this is
             * fast enough, so it goes first. A camera offers thirty; what
             * comes out here is what the encoder could keep up with.
             */
            snprintf(says, sizeof(says),
                     "%u pictures a second, %llu bytes a packet, %u in all.",
                     rate,
                     (unsigned long long)(app->loop_bytes / app->loop_count),
                     (unsigned)app->loop_count);
            schultz_label_set_text(app->tree, app->loop_note, says);
        }
    }

    snprintf(label, sizeof(label), "clicked %u", app->clicks);
    if (strcmp(label, schultz_label_text(app->tree, app->counter)) != 0) {
        schultz_label_set_text(app->tree, app->counter, label);
    }

    schultz_progress_bar_set_value(app->tree, app->column,
                                   schultz_slider_value(app->tree,
                                                        app->upright));
    schultz_progress_bar_set_value(app->tree, app->progress,
                                   schultz_slider_value(app->tree,
                                                        app->slider));
    return SCHULTZ_OK;
}

/*
 * Writes a PPM: a two line header and the pixels, which needs no library and
 * every image tool reads.
 */
static int32_t write_ppm(const char *path, const uint32_t *pixels,
                         uint32_t width, uint32_t height, uint32_t stride)
{
    FILE *file = fopen(path, "wb");
    uint32_t y;
    uint32_t x;

    if (file == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint32_t argb = pixels[(size_t)y * stride + x];
            unsigned char rgb[3];

            rgb[0] = (unsigned char)((argb >> 16) & 0xffu);
            rgb[1] = (unsigned char)((argb >> 8) & 0xffu);
            rgb[2] = (unsigned char)(argb & 0xffu);
            if (fwrite(rgb, 1, 3, file) != 3) {
                fclose(file);
                return SCHULTZ_ERR_INVALID_ARGUMENT;
            }
        }
    }
    fclose(file);
    return SCHULTZ_OK;
}

/*
 * Renders the scene again into memory rather than reading it back from the
 * window, which is what makes --shot-scale possible: the same tree at any
 * resolution, laid out once.
 */
static int32_t write_screenshot(demo_app *app, const char *path, float scale)
{
    schultz_render_options options;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t *pixels = NULL;
    int32_t result;

    memset(&options, 0, sizeof(options));
    options.fonts      = app->fonts;
    options.glyphs     = app->glyphs;
    options.resources  = app->resources;
    options.images     = app->images;
    options.background = schultz_theme_color(&app->theme,
                                             SCHULTZ_TOKEN_COLOR_WINDOW);

    result = schultz_render_size(app->tree, SCHULTZ_HANDLE_NONE, scale,
                                 &width, &height);
    if (result == SCHULTZ_OK) {
        pixels = (uint32_t *)malloc((size_t)width * height * sizeof(*pixels));
        if (pixels == NULL) {
            result = SCHULTZ_ERR_OUT_OF_MEMORY;
        }
    }
    if (result == SCHULTZ_OK) {
        result = schultz_render_to_buffer(app->tree, SCHULTZ_HANDLE_NONE,
                                          scale, &options, pixels, width,
                                          height, 0u);
    }
    if (result == SCHULTZ_OK) {
        result = write_ppm(path, pixels, width, height, width);
    }
    free(pixels);
    if (result == SCHULTZ_OK) {
        printf("wrote %s at %ux%u\n", path, width, height);
    }
    return result;
}

/**
 * @brief Runs the showcase.
 *
 * @param argc Argument count.
 * @param argv Arguments. See the file comment for what each one does.
 * @return EXIT_SUCCESS when every frame was presented, EXIT_FAILURE on a bad
 *         argument or any setup, playback, or present failure.
 */
int main(int argc, char **argv)
{
    schultz_window_options options;
    demo_app              app;
    int32_t               result;
    const char           *screenshot = NULL;
    int32_t               exact_pixels = 0;
    long                  frame_limit = -1;
    long                  first_page = 0;
    int                   status = EXIT_FAILURE;
    int                   i;

    memset(&app, 0, sizeof(app));
    app.shot_scale = 1.0f;
    /* Both start running, and both have a switch on the Drawing page. */
    app.run_mover  = 1;
    app.run_spin   = 1;
    /* VP9, the default of the two: smaller packets for the same picture. */
    app.loop_codec = (uint32_t)SCHULTZ_VIDEO_CODEC_VP9;

    schultz_window_options_init(&options);
    options.title      = "Schultz";
    options.width      = DEMO_WIDTH;
    options.height     = DEMO_HEIGHT;
    options.maximized  = 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frame_limit = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--page") == 0 && i + 1 < argc) {
            first_page = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--debug-dirty") == 0) {
            options.debug_tint = schultz_color_rgba(255, 64, 64, 40);
        } else if (strcmp(argv[i], "--video-driver") == 0 && i + 1 < argc) {
            options.video_driver = argv[++i];
        } else if (strcmp(argv[i], "--windowed") == 0) {
            options.maximized = 0;
        } else if (strcmp(argv[i], "--turn") == 0 && i + 1 < argc) {
            /* How far the picture is turned on its way to the panel, for a
             * display fitted the other way round. Given in degrees, because
             * that is how anyone describes a panel; anything else leaves the
             * default, which works the turn out from the orientation. */
            int degrees = atoi(argv[++i]);
            if (degrees == 0) {
                options.turn = SCHULTZ_TURN_NONE;
            } else if (degrees == 90) {
                options.turn = SCHULTZ_TURN_90;
            } else if (degrees == 180) {
                options.turn = SCHULTZ_TURN_180;
            } else if (degrees == 270) {
                options.turn = SCHULTZ_TURN_270;
            }
        } else if (strcmp(argv[i], "--exact-pixels") == 0) {
            exact_pixels = 1;
        } else if (strcmp(argv[i], "--no-vsync") == 0) {
            options.vsync = 0;
        } else if (strcmp(argv[i], "--toast-top") == 0) {
            schultz_toast_set_position(SCHULTZ_TOAST_TOP);
        } else if (strcmp(argv[i], "--light") == 0) {
            app.light = 1;
        } else if (strcmp(argv[i], "--loop-clip") == 0) {
            app.loop_clip = 1;
        } else if (strcmp(argv[i], "--shot-scale") == 0 && i + 1 < argc) {
            app.shot_scale = (float)atof(argv[++i]);
            if (app.shot_scale <= 0.0f) {
                app.shot_scale = 1.0f;
            }
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot = argv[++i];
            if (frame_limit < 0) {
                frame_limit = 1;
            }
        } else {
            fprintf(stderr,
                    "usage: %s [--frames N] [--screenshot FILE] "
                    "[--shot-scale N] [--page N] [--windowed] "
                    "[--toast-top] [--debug-dirty] [--no-vsync] "
                    "[--exact-pixels] [--video-driver NAME] "
                    "[--turn DEGREES] [--light]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    options.scale_to_screen = !exact_pixels;

    /*
     * One call opens the window, starts the rasterizer, and builds the font
     * system, image table, tree and event router that drawing needs.
     */
    result = schultz_window_create(&options, &app.window);
    if (result != SCHULTZ_OK) {
        fprintf(stderr, "startup failed: %s: %s\n",
                schultz_result_string(result), schultz_window_error());
        return EXIT_FAILURE;
    }
    app.tree      = schultz_window_tree(app.window);
    app.events    = schultz_window_events(app.window);
    /*
     * Sound. Not part of the window on purpose, and it fails quietly: a
     * machine with no sound card still runs the rest of the demo.
     */
    if (schultz_audio_create(&app.audio) == SCHULTZ_OK) {
        /*
         * The tree is told about it, because a video node plays its sound
         * through whatever the tree was given rather than opening a device of
         * its own.
         */
        schultz_tree_set_audio(app.tree, app.audio);
        if (schultz_audio_stream_create(app.audio, SCHULTZ_AUDIO_S16, 1u,
                                        48000u, &app.tone) != SCHULTZ_OK) {
            app.tone = SCHULTZ_HANDLE_NONE;
        }
        /*
         * A WAV built here rather than read from disk, so the demo ships no
         * sound files. What the toolkit does with it is what it would do with
         * an MP3 or an Ogg: decode it once and keep the samples.
         */
        {
            static unsigned char chime[8192];
            uint64_t length = demo_build_wav(chime, sizeof(chime));

            if (length == 0u ||
                schultz_sound_load_memory(app.audio, chime, length,
                                          &app.chime) != SCHULTZ_OK) {
                app.chime = SCHULTZ_HANDLE_NONE;
            }
        }
    }
    {
        /*
         * Temporary, for trying the keyboard out on a desktop. ALWAYS rather
         * than WHEN_NEEDED because this machine has keys and would otherwise
         * never show one; an application on a touch panel asks for
         * SCHULTZ_KEYBOARD_WHEN_NEEDED and gets it only where it is the only
         * way to type.
         */
        schultz_keyboard_options keys;

        schultz_keyboard_options_init(&keys);
        keys.policy = SCHULTZ_KEYBOARD_ALWAYS;
        schultz_window_set_keyboard(app.window, &keys);
        /* Read back, so the line below says what the window actually has
         * rather than what it was handed. */
        schultz_window_keyboard_options(app.window, &keys);
        if (getenv("SCHULTZ_TRACE_GEOMETRY") != NULL) {
            fprintf(stderr, "demo: keyboard policy %u, emoji %s\n",
                    keys.policy, keys.emoji ? "on" : "off");
        }
    }
    app.fonts     = schultz_window_fonts(app.window);
    app.glyphs    = schultz_window_glyphs(app.window);
    app.resources = schultz_window_resources(app.window);
    app.images    = schultz_window_images(app.window);
    app.width     = schultz_window_width(app.window);
    app.height    = schultz_window_height(app.window);

    result = schultz_font_load_file(app.fonts, DEMO_FONT_PATH, 16.0f,
                                    &app.title_font);
    if (result == SCHULTZ_OK) {
        result = schultz_font_load_file(app.fonts, DEMO_FONT_PATH, 16.0f,
                                        &app.body_font);
    }
    if (result != SCHULTZ_OK) {
        fprintf(stderr, "could not load %s: %s\n", DEMO_FONT_PATH,
                schultz_result_string(result));
        goto cleanup;
    }
    apply_theme(&app);

    /*
     * Images decode once into the driver's table. The PNG and the SVG go
     * through the same call; ThorVG works the format out from the bytes.
     */
    if (schultz_image_load_file(app.images, DEMO_PNG_PATH, &app.photo)
            != SCHULTZ_OK ||
        schultz_image_load_file(app.images, DEMO_SVG_PATH, &app.logo)
            != SCHULTZ_OK ||
        schultz_image_load_animation(app.images, DEMO_LOTTIE_PATH,
                                     &app.pulse) != SCHULTZ_OK) {
        fprintf(stderr, "could not load the demo images\n");
        result = SCHULTZ_ERR_INVALID_ARGUMENT;
        goto cleanup;
    }

    /*
     * Gradients and dash patterns are registered once and referred to by
     * handle, the same way fonts and styles are.
     */
    {
        schultz_gradient_stop stops[2];

        stops[0].offset = 0.0f;
        stops[0].color  = schultz_theme_color(&app.theme,
                                              SCHULTZ_TOKEN_COLOR_SURFACE);
        stops[1].offset = 1.0f;
        stops[1].color  = schultz_theme_color(&app.theme,
                                              SCHULTZ_TOKEN_COLOR_ACCENT);
        schultz_gradient_linear(app.resources, schultz_point_make(0.0f, 0.0f),
                                schultz_point_make(1.0f, 1.0f), stops, 2,
                                &app.sheen);
        /*
         * A second one for text. The first runs from the surface colour, so
         * letters filled with it would disappear into the card they sit on.
         */
        stops[0].color = schultz_theme_color(&app.theme,
                                             SCHULTZ_TOKEN_COLOR_TEXT);
        schultz_gradient_linear(app.resources, schultz_point_make(0.0f, 0.0f),
                                schultz_point_make(1.0f, 0.0f), stops, 2,
                                &app.lettering);
        schultz_dash_pair(app.resources, 6.0f, 4.0f, &app.dashes);
    }

    result = build_tree(&app);
    if (result != SCHULTZ_OK) {
        fprintf(stderr, "scene build failed: %s\n",
                schultz_result_string(result));
        goto cleanup;
    }

    schultz_events_set_callback(app.events, demo_on_event, &app);
    if (first_page < 0 || first_page >= DEMO_PAGE_COUNT) {
        first_page = 0;
    }
    show_page(&app, (uint32_t)first_page);

    /*
     * The loop itself lives in the toolkit. All the host supplies is what to
     * change each time round.
     */
    result = schultz_window_run(app.window, demo_before_draw, &app,
                                frame_limit < 0 ? 0u : (uint64_t)frame_limit);
    if (result != SCHULTZ_OK) {
        fprintf(stderr, "loop failed: %s\n",
                schultz_result_string(result));
        goto cleanup;
    }

    {
        uint64_t frames = schultz_window_frame_count(app.window);

        printf("presented %llu frame%s\n", (unsigned long long)frames,
               frames == 1u ? "" : "s");
    }
    status = EXIT_SUCCESS;

    if (screenshot != NULL) {
        result = write_screenshot(&app, screenshot, app.shot_scale);
        if (result != SCHULTZ_OK) {
            fprintf(stderr, "could not write %s: %s\n", screenshot,
                    schultz_result_string(result));
            status = EXIT_FAILURE;
        }
    }

cleanup:
    /*
     * Order matters here, and in one direction only.
     *
     * A video node playing sound holds a stream that belongs to the sound
     * system, and hands it back when the node is destroyed. So the player
     * goes first, then the sound system, then the window -- which ends in
     * SDL_Quit and therefore has to be last of all.
     */
    if (app.film != SCHULTZ_HANDLE_NONE) {
        schultz_node_destroy(app.tree, app.film);
    }
    /* Before the window, for the same reason as the sound system: closing a
     * camera is an SDL call, and destroying the window ends SDL. */
    schultz_video_encoder_destroy(app.encoder);
    schultz_video_decoder_destroy(app.decoder);
    schultz_camera_close(app.camera);
    schultz_audio_destroy(app.audio);
    schultz_window_destroy(app.window);
    return status;
}
