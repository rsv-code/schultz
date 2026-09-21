/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_sdl.c
 * @brief SDL3 window and presentation.
 */

#include "schultz_sdl.h"

/* For SCHULTZ_SCREEN_*, which is what an orientation is named in. */
#include "schultz_window.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

/** @brief A window, its presentation texture, and the buffer behind it. */
struct schultz_sdl_window {
    SDL_Window   *window;   /**< The platform window. */
    SDL_Renderer *renderer; /**< Used only to present one textured quad. */
    SDL_Texture  *texture;  /**< Streaming texture the buffer uploads into. */
    uint32_t     *pixels;   /**< Premultiplied ARGB8888, owned here. */
    SDL_Cursor   *cursors[SCHULTZ_CURSOR_COUNT]; /**< Made once, on demand. */
    uint32_t      cursor_on; /**< Shape currently shown. */
    uint32_t      width;    /**< Buffer width in pixels. */
    uint32_t      height;   /**< Buffer height in pixels. */
    uint32_t      stride;   /**< Pixels per row, currently always width. */
    int32_t       text_input_on; /**< Whether text input is currently started. */
    int32_t       resized;  /**< Set when the size changed, cleared on ask. */
    /**
     * Window coordinates per toolkit unit, for input.
     *
     * The platform reports a pointer in window coordinates, and the toolkit
     * works in whatever unit the frame chose. Those agree only when the frame
     * scales to the screen. One otherwise, and one on every screen that has
     * nothing to scale.
     */
    float         input_scale;
    int32_t       focused;  /**< Whether the window currently has focus. */
    /**
     * Quarter turns clockwise between what is drawn and what the panel
     * shows, 0 to 3.
     *
     * For a screen fitted into a case the other way round. The buffer is the
     * shape the interface wants and the panel is the shape it is; presenting
     * turns one into the other, and a pointer comes back the other way.
     */
    uint32_t      turn;
    uint32_t      panel_width;  /**< The panel itself, in pixels. */
    uint32_t      panel_height;
    float         panel_across; /**< And in the coordinates a pointer uses. */
    float         panel_down;
};

/*
 * Whether this platform reads SDL_WINDOW_RESIZABLE as "the screen may turn"
 * rather than "the edges may be dragged".
 *
 * Two drivers do, and they are the two with no window edges to drag: on iOS
 * the flag is cleared again the moment it has been read, and on Android it
 * is forwarded to the activity as the set of orientations it may adopt. So
 * one flag means two different things depending on where it lands, and this
 * is the only place that has to know which.
 */
static int32_t schultz_sdl_turns(void);

int32_t schultz_sdl_window_turns(const schultz_sdl_window *window)
{
    (void)window;
    return schultz_sdl_turns();
}

static int32_t schultz_sdl_turns(void)
{
    const char *driver = SDL_GetCurrentVideoDriver();

    if (driver == NULL) {
        return 0;
    }
    return (SDL_strcmp(driver, "uikit") == 0 ||
            SDL_strcmp(driver, "android") == 0) ? 1 : 0;
}

int32_t schultz_sdl_set_orientation(uint32_t orientation)
{
    /*
     * Read when a window is made, so this has to be set before one is.
     *
     * Both ways up for each, because holding a phone the other way round is
     * still holding it that way, and a screen that refuses to turn end over
     * end reads as broken rather than as deliberate.
     */
    switch (orientation) {
    case SCHULTZ_SCREEN_PORTRAIT:
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait PortraitUpsideDown");
        break;
    case SCHULTZ_SCREEN_LANDSCAPE:
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        break;
    case SCHULTZ_SCREEN_ANY:
        break;
    default:
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return SCHULTZ_OK;
}

int32_t schultz_sdl_init(const char *video_driver)
{
    /*
     * Before SDL_Init, which is the only time it is read: SDL settles on a
     * video driver while it is starting up and never revisits the choice.
     *
     * At normal priority on purpose, so SDL_VIDEO_DRIVER in the environment
     * still wins. Someone running the program decides how it reaches the
     * screen; the program only says what it would like when nobody has said
     * otherwise.
     */
    if (video_driver != NULL && video_driver[0] != '\0') {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, video_driver);
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return SCHULTZ_OK;
}

void schultz_sdl_quit(void)
{
    SDL_Quit();
}

const char *schultz_sdl_error(void)
{
    return SDL_GetError();
}

int32_t schultz_sdl_window_create(const char *title, uint32_t width,
                                  uint32_t height, int32_t maximized,
                                  int32_t resizable, int32_t allow_rotate,
                                  schultz_sdl_window **out_window)
{
    schultz_sdl_window *w;
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;

    if (title == NULL || out_window == NULL || width == 0 || height == 0) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    w = (schultz_sdl_window *)calloc(1, sizeof(*w));
    if (w == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    /*
     * RESIZABLE has to go on alongside MAXIMIZED even when the caller did not
     * ask for it, because a window manager will not maximize a window that
     * has declared it cannot be resized, and the request is dropped without
     * an error.
     */
    if (maximized) {
        flags |= SDL_WINDOW_MAXIMIZED | SDL_WINDOW_RESIZABLE;
    } else if (schultz_sdl_turns() ? allow_rotate : resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    /*
     * HIGH_PIXEL_DENSITY: without it a phone hands back a buffer the size of
     * the window in its own coordinates, which the system then stretches to
     * the panel. Everything drawn is upscaled and soft, and the extra pixels
     * a dense screen has cannot be reached at all. With it the buffer is the
     * panel's own resolution and every pixel is ours.
     */
    if (!SDL_CreateWindowAndRenderer(title, (int)width, (int)height, flags,
                                     &w->window, &w->renderer)) {
        free(w);
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    /*
     * The buffer has to match the size the window actually opened at. A
     * later resize is caught by SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED and
     * rebuilds it, but nothing reports the opening size, and that is never
     * reliably the size that was asked for. The request is not even in the
     * same units as the answer:
     *
     *   - A size is requested in the window system's own coordinates, and the
     *     buffer is in pixels. On a screen that packs two or three pixels
     *     into one of those, asking for 800 by 600 gets a buffer of 1600 by
     *     1200 or larger.
     *   - A phone gives every window the whole screen whatever it was asked
     *     for, so the request is ignored outright.
     *   - Maximizing is the window manager's decision, and the work area it
     *     leaves is rarely the whole display.
     *
     * So the size is always read back, never assumed. Getting this wrong is
     * quiet and expensive: the buffer is then a fraction of the window, a
     * host is told to build against that fraction, and the mistake only shows
     * on hardware nobody develops on. Syncing first because the manager may
     * not have applied the request yet.
     */
    {
        int actual_w = 0;
        int actual_h = 0;

        SDL_SyncWindow(w->window);
        if (SDL_GetWindowSizeInPixels(w->window, &actual_w, &actual_h) &&
            actual_w > 0 && actual_h > 0) {
            width  = (uint32_t)actual_w;
            height = (uint32_t)actual_h;
        }
    }
    w->width  = width;
    w->height = height;
    w->stride = width;
    w->input_scale = 1.0f;
    /*
     * Nothing is turned yet, so the panel and the buffer are the same thing.
     * Recorded even so, because turning later needs to know what the panel
     * was, and this is the only place that has it before the first resize.
     */
    w->panel_width  = width;
    w->panel_height = height;
    {
        int across = 0;
        int down = 0;

        if (SDL_GetWindowSize(w->window, &across, &down) && across > 0 &&
            down > 0) {
            w->panel_across = (float)across;
            w->panel_down   = (float)down;
        } else {
            w->panel_across = (float)width;
            w->panel_down   = (float)height;
        }
    }

    /*
     * STREAMING because the whole point is that the CPU writes these pixels
     * every frame. ARGB8888 matches what ThorVG is told to produce, so the
     * upload is a straight copy with no conversion.
     */
    w->texture = SDL_CreateTexture(w->renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   (int)w->width, (int)w->height);
    if (w->texture == NULL) {
        SDL_DestroyRenderer(w->renderer);
        SDL_DestroyWindow(w->window);
        free(w);
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    w->pixels = (uint32_t *)calloc((size_t)w->stride * w->height,
                                   sizeof(*w->pixels));
    if (w->pixels == NULL) {
        SDL_DestroyTexture(w->texture);
        SDL_DestroyRenderer(w->renderer);
        SDL_DestroyWindow(w->window);
        free(w);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    *out_window = w;
    return SCHULTZ_OK;
}

void schultz_sdl_window_destroy(schultz_sdl_window *window)
{
    if (window == NULL) {
        return;
    }
    {
        uint32_t i;
        for (i = 0; i < SCHULTZ_CURSOR_COUNT; i++) {
            if (window->cursors[i] != NULL) {
                SDL_DestroyCursor(window->cursors[i]);
            }
        }
    }
    free(window->pixels);
    if (window->texture != NULL) {
        SDL_DestroyTexture(window->texture);
    }
    if (window->renderer != NULL) {
        SDL_DestroyRenderer(window->renderer);
    }
    if (window->window != NULL) {
        SDL_DestroyWindow(window->window);
    }
    free(window);
}

SDL_Window *schultz_sdl_window_handle(schultz_sdl_window *window)
{
    return (window == NULL) ? NULL : window->window;
}

/*
 * A testing hook: SCHULTZ_SAFE_INSET="top,bottom,left,right", in pixels.
 *
 * Only a phone has a camera notch, and a phone is the one machine this cannot
 * be developed on. This makes a desktop pretend, so the inset can be seen in
 * a screenshot and asserted on, using the same path the real one takes.
 * Unset, which it is everywhere but a test, it does nothing.
 */
static int32_t schultz_sdl_fake_inset(schultz_rect full, schultz_rect *out)
{
    const char *spec = getenv("SCHULTZ_SAFE_INSET");
    double top = 0.0, bottom = 0.0, left = 0.0, right = 0.0;

    if (spec == NULL || *spec == '\0') {
        return 0;
    }
    if (sscanf(spec, "%lf,%lf,%lf,%lf", &top, &bottom, &left, &right) != 4) {
        return 0;
    }
    *out = schultz_rect_make(full.x + (float)left, full.y + (float)top,
                             full.width - (float)(left + right),
                             full.height - (float)(top + bottom));
    return 1;
}

float schultz_sdl_window_unit_scale(const schultz_sdl_window *window)
{
    /*
     * A testing hook, like SCHULTZ_SAFE_INSET beside it: SCHULTZ_PIXEL_SCALE
     * makes an ordinary monitor claim to pack pixels the way a phone does, so
     * the scaling can be exercised on a machine that has none. Unset, which
     * it is everywhere but a test, it does nothing.
     */
    const char *spec = getenv("SCHULTZ_PIXEL_SCALE");
    float scale;

    if (spec != NULL && *spec != '\0') {
        double forced = atof(spec);

        if (forced > 0.0) {
            return (float)forced;
        }
    }
    if (window == NULL) {
        return 1.0f;
    }
    /*
     * How much bigger the platform wants an interface drawn, which is not the
     * same question as how many pixels sit behind a window coordinate, and
     * the two are answered by different platforms in different halves.
     *
     * Apple hands out a window measured in points and a buffer measured in
     * pixels, so the whole of it lands in the density and nothing is left for
     * the content scale. Android hands out a window already measured in
     * pixels, so the density is one and the whole of it lands in the content
     * scale. Asking for the density alone is right on one and gives one on
     * the other, which is an interface drawn at a third of its size on a
     * phone.
     *
     * This asks for both multiplied together, which is what the platform
     * means by "draw an interface this big" wherever the number happens to
     * be kept.
     */
    scale = SDL_GetWindowDisplayScale(window->window);
    return (scale > 0.0f) ? scale : 1.0f;
}

float schultz_sdl_window_pixel_density(const schultz_sdl_window *window)
{
    /*
     * How many pixels sit behind one window coordinate. Not the scale above:
     * this one says what the platform's own numbers mean, and is what turns a
     * reported pointer position into a place on the screen.
     *
     * No testing hook here. Forcing it would be claiming the platform reports
     * its coordinates differently than it does, and every pointer would land
     * somewhere other than where it was pressed.
     */
    float density;

    if (window == NULL) {
        return 1.0f;
    }
    density = SDL_GetWindowPixelDensity(window->window);
    return (density > 0.0f) ? density : 1.0f;
}

int32_t schultz_sdl_window_set_input_scale(schultz_sdl_window *window,
                                           float scale)
{
    if (window == NULL || scale <= 0.0f) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    window->input_scale = scale;
    return SCHULTZ_OK;
}

int32_t schultz_sdl_window_safe_area(schultz_sdl_window *window,
                                     schultz_rect *out_area)
{
    SDL_Rect area;
    float density;

    if (window == NULL || out_area == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * The whole buffer, until proved otherwise. Every failure below leaves
     * this, because a window that cannot say what is safe is better treated
     * as entirely safe than as entirely unsafe.
     */
    *out_area = schultz_rect_make(0.0f, 0.0f, (float)window->width,
                                  (float)window->height);
    if (schultz_sdl_fake_inset(*out_area, out_area)) {
        return SCHULTZ_OK;
    }
    if (!SDL_GetWindowSafeArea(window->window, &area)) {
        return SCHULTZ_OK;
    }
    if (area.w <= 0 || area.h <= 0) {
        return SCHULTZ_OK;
    }

    /*
     * The safe area comes back in window coordinates and the buffer is in
     * pixels, and on a phone those are not the same thing: a three times
     * display reports a 393 point wide window backed by 1179 pixels. Using
     * the numbers as they arrive puts the content in the top left ninth of
     * the screen, so they are scaled by the density first.
     */
    density = SDL_GetWindowPixelDensity(window->window);
    if (density <= 0.0f) {
        density = 1.0f;
    }
    *out_area = schultz_rect_make((float)area.x * density,
                                  (float)area.y * density,
                                  (float)area.w * density,
                                  (float)area.h * density);

    /*
     * SDL describes the panel, and on a turned panel the buffer is not the
     * panel: a notch along the panel's top edge runs down one side of what is
     * drawn. So the rect is turned back the same way a pointer is, which on a
     * quarter turn swaps its width and its height as well as moving it.
     */
    if (window->turn != 0u) {
        float pw = (float)window->panel_width;
        float ph = (float)window->panel_height;
        schultz_rect a = *out_area;

        switch (window->turn) {
        case 1u:
            *out_area = schultz_rect_make(a.y, pw - a.x - a.width, a.height,
                                          a.width);
            break;
        case 2u:
            *out_area = schultz_rect_make(pw - a.x - a.width,
                                          ph - a.y - a.height, a.width,
                                          a.height);
            break;
        default:
            *out_area = schultz_rect_make(ph - a.y - a.height, a.x, a.height,
                                          a.width);
            break;
        }
    }

    /*
     * Rounding a scaled edge can put it a pixel past the buffer, and a safe
     * area larger than the window is not safe at all.
     */
    *out_area = schultz_rect_intersect(
        *out_area,
        schultz_rect_make(0.0f, 0.0f, (float)window->width,
                          (float)window->height));
    if (schultz_rect_is_empty(*out_area)) {
        *out_area = schultz_rect_make(0.0f, 0.0f, (float)window->width,
                                      (float)window->height);
    }
    return SCHULTZ_OK;
}

uint32_t schultz_sdl_window_panel_width(const schultz_sdl_window *window)
{
    return (window == NULL) ? 0u : window->panel_width;
}

uint32_t schultz_sdl_window_panel_height(const schultz_sdl_window *window)
{
    return (window == NULL) ? 0u : window->panel_height;
}

uint32_t *schultz_sdl_window_pixels(schultz_sdl_window *window)
{
    return (window == NULL) ? NULL : window->pixels;
}

uint32_t schultz_sdl_window_width(const schultz_sdl_window *window)
{
    return (window == NULL) ? 0 : window->width;
}

uint32_t schultz_sdl_window_height(const schultz_sdl_window *window)
{
    return (window == NULL) ? 0 : window->height;
}

uint32_t schultz_sdl_window_stride(const schultz_sdl_window *window)
{
    return (window == NULL) ? 0 : window->stride;
}

int32_t schultz_sdl_window_set_vsync(schultz_sdl_window *window,
                                     int32_t enabled)
{
    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (!SDL_SetRenderVSync(window->renderer,
                            enabled ? 1 : SDL_RENDERER_VSYNC_DISABLED)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return SCHULTZ_OK;
}

int32_t schultz_sdl_has_keyboard(void)
{
    return SDL_HasKeyboard() ? 1 : 0;
}

uint64_t schultz_sdl_ticks_ms(void)
{
    return SDL_GetTicks();
}

/*
 * Rebuilds everything that was sized for the old window. The buffer is a
 * plain allocation and the texture is fixed size, so a window that changed
 * size needs both again; without this the renderer stretches the old picture
 * across the new window and nothing is laid out for the space it now has.
 */
static int32_t schultz_sdl_window_resize(schultz_sdl_window *window,
                                         uint32_t panel_w, uint32_t panel_h)
{
    SDL_Texture *texture;
    uint32_t *pixels;
    uint32_t width  = panel_w;
    uint32_t height = panel_h;
    int across = 0;
    int down = 0;

    /*
     * A quarter turn either way means the buffer is the panel on its side:
     * the interface is drawn in the shape it wants and put on the panel
     * turned. A half turn is the same shape upside down.
     */
    if ((window->turn & 1u) != 0u) {
        width  = panel_h;
        height = panel_w;
    }
    window->panel_width  = panel_w;
    window->panel_height = panel_h;
    /*
     * The panel again in whatever coordinates a pointer is reported in,
     * which are pixels everywhere but Apple. Kept because turning a pointer
     * back needs the size of the thing it was reported against.
     */
    if (SDL_GetWindowSize(window->window, &across, &down) && across > 0 &&
        down > 0) {
        window->panel_across = (float)across;
        window->panel_down   = (float)down;
    } else {
        window->panel_across = (float)panel_w;
        window->panel_down   = (float)panel_h;
    }

    if (width == 0u || height == 0u ||
        (width == window->width && height == window->height)) {
        return SCHULTZ_OK;
    }
    texture = SDL_CreateTexture(window->renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, (int)width,
                                (int)height);
    if (texture == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    pixels = (uint32_t *)calloc((size_t)width * height, sizeof(*pixels));
    if (pixels == NULL) {
        SDL_DestroyTexture(texture);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    SDL_DestroyTexture(window->texture);
    free(window->pixels);
    window->texture = texture;
    window->pixels  = pixels;
    window->width   = width;
    window->height  = height;
    window->stride  = width;
    window->resized = 1;
    return SCHULTZ_OK;
}

int32_t schultz_sdl_window_set_turn(schultz_sdl_window *window,
                                    uint32_t quarters)
{
    uint32_t panel_w;
    uint32_t panel_h;

    if (window == NULL || quarters > 3u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (window->turn == quarters) {
        return SCHULTZ_OK;
    }
    panel_w = window->panel_width;
    panel_h = window->panel_height;
    if (panel_w == 0u || panel_h == 0u) {
        /* No panel to turn against yet. Remember the wish; the first resize
         * will honour it. */
        window->turn = quarters;
        return SCHULTZ_OK;
    }
    window->turn = quarters;
    /*
     * The buffer is the other way round now, and resizing does nothing when
     * the size it works out has not changed, so the old one is forgotten
     * first. Only once there is a panel to rebuild it from.
     */
    window->width  = 0u;
    window->height = 0u;
    return schultz_sdl_window_resize(window, panel_w, panel_h);
}

int32_t schultz_sdl_window_focused(const schultz_sdl_window *window)
{
    return (window == NULL) ? 0 : window->focused;
}

int32_t schultz_sdl_window_take_resized(schultz_sdl_window *window)
{
    int32_t was;

    if (window == NULL) {
        return 0;
    }
    was = window->resized;
    window->resized = 0;
    return was;
}

/*
 * Writes the pixel buffer to a file, once, when SCHULTZ_DUMP_BUFFER names one.
 *
 * This is the only way to see what was actually rasterized. Photographing a
 * phone is imprecise, and asking the window system for a picture of a window
 * answers with the frame around it and whatever the desktop had behind it,
 * which is how a fully painted window can be made to look two thirds empty.
 * The buffer is the truth, and scripts/check_full_paint.sh reads it.
 */
/* The last rectangle handed to the texture, for schultz_sdl_dump. */
static schultz_rect schultz_sdl_last_upload;

static void schultz_sdl_dump(const schultz_sdl_window *w)
{
    const char *path = getenv("SCHULTZ_DUMP_BUFFER");
    static int done = 0;
    FILE *f;
    uint32_t x, y;

    if (path == NULL || done) { return; }
    done = 1;
    f = fopen(path, "wb");
    if (f == NULL) { return; }
    /*
     * The uploaded rectangle, as a comment the format allows. A buffer that
     * is painted correctly and an upload that covers a third of it produce
     * the same picture here, and only the second reaches the screen.
     */
    fprintf(f, "P6\n# upload %d %d %d %d\n",
            (int)schultz_sdl_last_upload.x, (int)schultz_sdl_last_upload.y,
            (int)schultz_sdl_last_upload.width,
            (int)schultz_sdl_last_upload.height);
    fprintf(f, "%u %u\n255\n", w->width, w->height);
    for (y = 0; y < w->height; y++) {
        for (x = 0; x < w->width; x++) {
            uint32_t p = w->pixels[y * w->stride + x];
            fputc((int)((p >> 16) & 0xFFu), f);
            fputc((int)((p >> 8) & 0xFFu), f);
            fputc((int)(p & 0xFFu), f);
        }
    }
    fclose(f);
}

int32_t schultz_sdl_window_upload(schultz_sdl_window *window,
                                  schultz_rect dirty)
{
    SDL_Rect rect;
    const uint32_t *source;
    schultz_rect surface;
    schultz_rect clamped;

    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    surface = schultz_rect_make(0.0f, 0.0f, (float)window->width,
                                (float)window->height);
    /*
     * Align outward before clamping, so the uploaded rectangle always fully
     * contains the region that was repainted. Truncating instead loses the
     * far edge and leaves a column of the previous frame on screen.
     */
    clamped = schultz_rect_intersect(surface,
                                     schultz_rect_pixel_bounds(dirty, schultz_paint_slack()));

    if (!schultz_rect_is_empty(clamped)) {
        rect.x = (int)clamped.x;
        rect.y = (int)clamped.y;
        rect.w = (int)clamped.width;
        rect.h = (int)clamped.height;

        schultz_sdl_last_upload = clamped;
        source = window->pixels + (size_t)rect.y * window->stride + rect.x;
        if (!SDL_UpdateTexture(window->texture, &rect, source,
                               (int)(window->stride * sizeof(uint32_t)))) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
    }
    return SCHULTZ_OK;
}

int32_t schultz_sdl_window_present(schultz_sdl_window *window)
{
    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    if (!SDL_RenderClear(window->renderer)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (window->turn == 0u) {
        if (!SDL_RenderTexture(window->renderer, window->texture, NULL,
                               NULL)) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
    } else {
        /*
         * The buffer laid on the panel unturned and then turned about its
         * own middle, which is what this call does. For a quarter turn the
         * two are different shapes, so the rectangle hangs off the panel on
         * two sides before the turn and lands square on it after.
         *
         * A NULL centre means the middle of the rectangle, which is the
         * only point that works: turning about anything else moves the
         * picture off the panel.
         */
        SDL_FRect dst;

        dst.w = (float)window->width;
        dst.h = (float)window->height;
        dst.x = ((float)window->panel_width  - dst.w) * 0.5f;
        dst.y = ((float)window->panel_height - dst.h) * 0.5f;
        if (!SDL_RenderTextureRotated(window->renderer, window->texture,
                                      NULL, &dst, 90.0 * (double)window->turn,
                                      NULL, SDL_FLIP_NONE)) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
    }
    if (!SDL_RenderPresent(window->renderer)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    schultz_sdl_dump(window);
    return SCHULTZ_OK;
}

/* Translates SDL's modifier mask into the toolkit's. */
static uint32_t schultz_sdl_modifiers(SDL_Keymod mod)
{
    uint32_t result = 0;

    if (mod & SDL_KMOD_SHIFT) { result |= SCHULTZ_MOD_SHIFT; }
    if (mod & SDL_KMOD_CTRL)  { result |= SCHULTZ_MOD_CTRL; }
    if (mod & SDL_KMOD_ALT)   { result |= SCHULTZ_MOD_ALT; }
    if (mod & SDL_KMOD_GUI)   { result |= SCHULTZ_MOD_SUPER; }
    return result;
}

/*
 * Translates an SDL key code into the toolkit's own. SDL names a key that
 * types a character by that character's codepoint, and so does Schultz, so
 * those pass straight through. The rest are named one by one: SDL gives them
 * either an ASCII control code or a masked scancode, and neither carries any
 * meaning worth exposing above the platform layer.
 */
/*
 * Shows the shape the node under the pointer asks for. Cursors are created
 * once each and kept, since SDL makes a system cursor every time it is asked.
 */
static void schultz_sdl_sync_cursor(schultz_sdl_window *window,
                                    schultz_events *events)
{
    static const SDL_SystemCursor kinds[SCHULTZ_CURSOR_COUNT] = {
        SDL_SYSTEM_CURSOR_DEFAULT,
        SDL_SYSTEM_CURSOR_TEXT,
        SDL_SYSTEM_CURSOR_POINTER,
        SDL_SYSTEM_CURSOR_EW_RESIZE,
        SDL_SYSTEM_CURSOR_NS_RESIZE
    };
    schultz_tree *tree;
    schultz_handle hovered;
    uint32_t wanted = SCHULTZ_CURSOR_DEFAULT;

    if (events == NULL) {
        return;
    }
    tree    = schultz_events_tree(events);
    hovered = schultz_events_hovered(events);
    if (tree != NULL && hovered != SCHULTZ_HANDLE_NONE) {
        wanted = schultz_node_cursor(tree, hovered);
    }
    if (wanted == window->cursor_on || wanted >= SCHULTZ_CURSOR_COUNT) {
        return;
    }

    if (window->cursors[wanted] == NULL) {
        window->cursors[wanted] = SDL_CreateSystemCursor(kinds[wanted]);
        if (window->cursors[wanted] == NULL) {
            return;
        }
    }
    SDL_SetCursor(window->cursors[wanted]);
    window->cursor_on = wanted;
}

/*
 * The bytes a Schultz caller produced, wrapped for SDL.
 *
 * SDL asks for one format at the moment somebody pastes, which may be long
 * after the copy. Its callback signature differs from ours only in the types,
 * so this is a thin shim and the pair of pointers it needs is kept beside it.
 */
static schultz_clipboard_make_fn schultz_sdl_clip_make = NULL;
static void                     *schultz_sdl_clip_context = NULL;

static const void *SDLCALL schultz_sdl_clipboard_produce(void *userdata,
                                                         const char *mime_type,
                                                         size_t *size)
{
    uint64_t length = 0u;
    const void *bytes;

    (void)userdata;
    *size = 0u;
    if (schultz_sdl_clip_make == NULL) {
        return NULL;
    }
    /*
     * The bare name for plain text is offered as well as the full one, so a
     * request for it is answered with the same bytes. The producer never has
     * to know there are two names for the one thing.
     */
    if (strcmp(mime_type, "text/plain") == 0) {
        mime_type = SCHULTZ_CLIPBOARD_TEXT;
    }
    bytes = schultz_sdl_clip_make(schultz_sdl_clip_context, mime_type,
                                  &length);
    if (bytes == NULL) {
        return NULL;
    }
    *size = (size_t)length;
    return bytes;
}

const void *schultz_sdl_clipboard_take(void *context, const char *format,
                                       uint64_t *out_length)
{
    /*
     * SDL hands back memory it expects to be freed. Keeping the last one and
     * freeing it on the next call is what lets this return a plain pointer
     * with no ownership crossing the boundary.
     */
    static void *held = NULL;
    size_t size = 0u;

    (void)context;
    *out_length = 0u;
    SDL_free(held);
    held = SDL_GetClipboardData(format, &size);
    if (held != NULL && size == 0u) {
        SDL_free(held);
        held = NULL;
    }
    *out_length = (uint64_t)size;
    return held;
}

int32_t schultz_sdl_clipboard_holds(void *context, const char *format)
{
    (void)context;
    return SDL_HasClipboardData(format) ? 1 : 0;
}

int32_t schultz_sdl_clipboard_offer(void *context,
                                    const char *const *formats,
                                    uint32_t count,
                                    schultz_clipboard_make_fn make,
                                    void *make_context)
{
    uint32_t i;
    int32_t only_text = 0;
    const char *widened[8];
    uint32_t count_out = 0u;

    (void)context;
    if (formats == NULL || count == 0u || make == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < count; i++) {
        if (formats[i] == NULL) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
    }
    /*
     * One format, and it is plain text. Not "every format looks like text":
     * markup is a text media type too, and treating an offer of words and
     * markup as plain alone threw the markup away.
     */
    only_text = (count == 1u &&
                 strcmp(formats[0], SCHULTZ_CLIPBOARD_TEXT) == 0);

    /*
     * Plain text alone goes through SDL's own text call rather than the
     * general one, and the reason is worth keeping.
     *
     * Every platform names plain text more than one way. X11 wants
     * UTF8_STRING and STRING as well as the media type, and the list differs
     * per platform. SDL_SetClipboardText knows each platform's list and
     * offers all of them; SDL_SetClipboardData offers exactly what it is
     * given. Sending text through the general call would quietly stop older
     * applications from pasting, so the simple case keeps the path that
     * already works.
     *
     * The cost is that the bytes are produced now rather than when somebody
     * pastes. Text is cheap, so that is a fair trade.
     */
    if (only_text) {
        uint64_t length = 0u;
        const void *bytes = make(make_context, SCHULTZ_CLIPBOARD_TEXT,
                                 &length);
        char *copy;
        bool ok;

        if (bytes == NULL) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
        /*
         * The length is whatever the host's producer reported. Refused
         * before a terminator is added to it, because at the top of the
         * range that sum wraps to nothing and the copy below still writes
         * the length that was reported.
         */
        if (length > (uint64_t)SIZE_MAX - 1u) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
        copy = (char *)malloc((size_t)length + 1u);
        if (copy == NULL) {
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        memcpy(copy, bytes, (size_t)length);
        copy[length] = '\0';
        ok = SDL_SetClipboardText(copy);
        free(copy);
        return ok ? SCHULTZ_OK : SCHULTZ_ERR_INVALID_ARGUMENT;
    }

    /*
     * Anything else is offered as given, with one addition: plain text under
     * its bare name as well as its full one. A program asking for
     * "text/plain" rather than "text/plain;charset=utf-8" is common, and
     * without this it finds nothing. The bare name is a media type like any
     * other, so it is safe on every platform; the names X11 alone uses, such
     * as UTF8_STRING, are not and are left to the call above.
     */
    for (i = 0; i < count && count_out < 7u; i++) {
        widened[count_out++] = formats[i];
        if (strcmp(formats[i], SCHULTZ_CLIPBOARD_TEXT) == 0) {
            widened[count_out++] = "text/plain";
        }
    }

    schultz_sdl_clip_make    = make;
    schultz_sdl_clip_context = make_context;
    return SDL_SetClipboardData(schultz_sdl_clipboard_produce, NULL, NULL,
                                widened, (size_t)count_out)
               ? SCHULTZ_OK : SCHULTZ_ERR_INVALID_ARGUMENT;
}

static uint32_t schultz_sdl_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return SCHULTZ_KEY_RETURN;
    case SDLK_ESCAPE:   return SCHULTZ_KEY_ESCAPE;
    case SDLK_TAB:      return SCHULTZ_KEY_TAB;
    case SDLK_BACKSPACE:return SCHULTZ_KEY_BACKSPACE;
    case SDLK_DELETE:   return SCHULTZ_KEY_DELETE;
    case SDLK_LEFT:     return SCHULTZ_KEY_LEFT;
    case SDLK_RIGHT:    return SCHULTZ_KEY_RIGHT;
    case SDLK_UP:       return SCHULTZ_KEY_UP;
    case SDLK_DOWN:     return SCHULTZ_KEY_DOWN;
    case SDLK_HOME:     return SCHULTZ_KEY_HOME;
    case SDLK_END:      return SCHULTZ_KEY_END;
    case SDLK_PAGEUP:   return SCHULTZ_KEY_PAGE_UP;
    case SDLK_PAGEDOWN: return SCHULTZ_KEY_PAGE_DOWN;
    default:            break;
    }

    /*
     * Anything left that fits in the Unicode range is a key that types that
     * character, which is exactly what Schultz names it. Everything else is
     * a key the toolkit has no name for.
     */
    return (key <= 0x10FFFFu) ? (uint32_t)key : (uint32_t)SCHULTZ_KEY_UNKNOWN;
}

/*
 * Which device produced a mouse event. SDL synthesizes mouse events from
 * touch and marks them with a reserved id, which is the only way to tell a
 * finger from a cursor once the translation has happened.
 */
static uint32_t schultz_sdl_source(SDL_MouseID which)
{
    return (which == SDL_TOUCH_MOUSEID) ? SCHULTZ_POINTER_TOUCH
                                        : SCHULTZ_POINTER_MOUSE;
}

static uint32_t schultz_sdl_button(Uint8 button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:   return SCHULTZ_BUTTON_LEFT;
    case SDL_BUTTON_MIDDLE: return SCHULTZ_BUTTON_MIDDLE;
    case SDL_BUTTON_RIGHT:  return SCHULTZ_BUTTON_RIGHT;
    default:                return SCHULTZ_BUTTON_LEFT;
    }
}

/*
 * Starts or stops text input to match what holds focus, and tells the
 * platform where the caret is.
 *
 * This has to happen automatically rather than being left to the host,
 * because the platform delivers no text events at all until text input is
 * started. It is also what raises and dismisses the on screen keyboard on
 * mobile, so tying it to focus is the behaviour a user expects.
 */
/*
 * Whether a delete key here means one codepoint or one whole character.
 *
 * A platform with a keyboard of its own reports no keys at all. It keeps a
 * text field, watches it change, and afterwards sends one backspace for
 * every codepoint that went, so one tap over an emoji spelled with a
 * modifier arrives as two. Told that, the toolkit takes one codepoint per
 * key and the run of them removes exactly what the platform removed.
 *
 * Only while that is where the keys come from. Plug a keyboard into the same
 * phone and the presses are real again, so this is asked every turn rather
 * than settled once.
 */
static void schultz_sdl_sync_delete(schultz_events *events)
{
    if (events == NULL) {
        return;
    }
    schultz_events_set_delete_by_codepoint(events,
        (SDL_HasScreenKeyboardSupport() && !SDL_HasKeyboard()) ? 1 : 0);
}

static void schultz_sdl_sync_text_input(schultz_sdl_window *window,
                                        schultz_events *events)
{
    schultz_tree *tree;
    schultz_handle focused;
    int32_t wanted = 0;

    if (events == NULL) {
        return;
    }
    tree    = schultz_events_tree(events);
    focused = schultz_events_focus(events);

    if (tree != NULL && focused != SCHULTZ_HANDLE_NONE) {
        wanted = (schultz_node_get_role(tree, focused) ==
                  SCHULTZ_ROLE_TEXT_INPUT) ? 1 : 0;
    }

    if (wanted != window->text_input_on) {
        schultz_sdl_window_set_text_input(window, wanted);
        window->text_input_on = wanted;
    }

    /*
     * Where the field is. Two platforms want it for two different reasons.
     *
     * On a desktop it places an input method's list of candidate characters
     * beside the word being typed. On a phone it is how the platform knows
     * which part of the window has to stay visible: it slides the whole
     * window up by however much the keyboard would have covered the field,
     * and slides it back when the keyboard goes.
     *
     * Sliding is the right answer here rather than making the window
     * shorter, which is the other way this is usually done. A shorter window
     * has to be laid out again, and a screen of controls laid out again in
     * half the height is a screen of controls squeezed into half the height.
     * Sliding moves everything and changes nothing, at the cost of the top
     * going off screen while the keyboard is up.
     *
     * Making the window shorter is not available in any case: it needs to
     * know how tall the keyboard is, and nothing reports that.
     */
    if (wanted) {
        schultz_rect bounds;
        if (schultz_node_absolute_bounds(tree, focused, &bounds)
                == SCHULTZ_OK) {
            schultz_sdl_window_set_text_area(window, bounds, 0);
        }
    }
}

/* A pointer position, from window coordinates into the toolkit's units. */
static schultz_point schultz_sdl_pointer(const schultz_sdl_window *window,
                                         float x, float y)
{
    /*
     * Back the way the picture went. The panel was given the buffer turned
     * clockwise, so a place on the panel is that many quarter turns
     * anticlockwise on the buffer.
     */
    float across = window->panel_across;
    float down   = window->panel_down;
    float bx = x;
    float by = y;

    switch (window->turn) {
    case 1u: bx = y;          by = across - x; break;
    case 2u: bx = across - x; by = down - y;   break;
    case 3u: bx = down - y;   by = x;          break;
    default: break;
    }
    return schultz_point_make(bx * window->input_scale,
                              by * window->input_scale);
}

int32_t schultz_sdl_window_process_events(schultz_sdl_window *window,
                                schultz_events *events,
                                int32_t *out_should_quit)
{
    SDL_Event event;

    if (window == NULL || out_should_quit == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_should_quit = 0;

    while (SDL_PollEvent(&event)) {
        /*
         * SDL stamps every event in nanoseconds. The router needs the time
         * only to tell one press from two, so milliseconds are plenty, and
         * using the event's own stamp rather than the clock now keeps a
         * backlog of queued events from reading as one very fast burst.
         */
        if (events != NULL) {
            schultz_events_set_time(events, event.common.timestamp
                                                / 1000000u);
        }
        uint32_t modifiers = schultz_sdl_modifiers(SDL_GetModState());

        switch (event.type) {
        /* The platform asking the application to stop: Cmd-Q, a session
         * ending, a phone terminating the app. Not a window event, and it
         * carries no window in it. */
        case SDL_EVENT_QUIT:
            *out_should_quit = 1;
            break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            /* The size in pixels, not in whatever units the desktop scales
             * by, because that is what the buffer is measured in. */
            schultz_sdl_window_resize(window, (uint32_t)event.window.data1,
                                      (uint32_t)event.window.data2);
            break;

        /*
         * Whether this window is the one the user is looking at. A screen
         * reader needs it: what it reads out should be the focused window,
         * and a background one still claiming focus is how a reader ends up
         * describing something nobody is looking at.
         */
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            window->focused = 1;
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            window->focused = 0;
            break;

        /*
         * What is safe to draw in changed without the window changing size,
         * which is a keyboard coming up or a phone being turned. Flagged as a
         * resize because everything that has to happen next is the same, and
         * the buffer is already the right size so nothing is reallocated.
         */
        case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
            /*
             * Not while a keyboard is up. The only thing moving the safe
             * area then is the platform sliding the window to keep the
             * field clear of the keyboard, and laying out to that would be
             * laying out to where the window has been slid rather than to
             * the screen, which amounts to undoing the slide.
             *
             * The platform is asked rather than remembered. It already
             * tracks this, and a flag kept here would be one more thing that
             * can fall out of step with what is actually on screen.
             *
             * A phone being turned changes the window's size as well, and
             * that arrives as its own event, so nothing real waits on this.
             */
            if (!SDL_ScreenKeyboardShown(window->window)) {
                window->resized = 1;
            }
            break;

        /*
         * The keyboard has gone and the platform has slid the window back
         * down. Everything held still while it was up is read again here,
         * so that nothing the safe area did in the meantime is left behind.
         */
        case SDL_EVENT_SCREEN_KEYBOARD_HIDDEN:
            window->resized = 1;
            break;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            *out_should_quit = 1;
            break;

        /*
         * SDL turns a finger into a mouse for us, which is what makes a touch
         * screen work at all without a second set of handlers. The one thing
         * it keeps is which device it came from, and a widget that wants a
         * different gesture for a finger needs to be told, so it is passed on
         * before the event is.
         */
        case SDL_EVENT_MOUSE_MOTION:
            if (events != NULL) {
                schultz_events_set_pointer_source(events,
                    schultz_sdl_source(event.motion.which));
                schultz_events_mouse_move(events,
                    schultz_sdl_pointer(window, event.motion.x,
                                        event.motion.y),
                    modifiers);
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (events != NULL) {
                schultz_events_set_pointer_source(events,
                    schultz_sdl_source(event.button.which));
                schultz_events_mouse_button(events,
                    schultz_sdl_pointer(window, event.button.x,
                                        event.button.y),
                    schultz_sdl_button(event.button.button),
                    event.type == SDL_EVENT_MOUSE_BUTTON_DOWN, modifiers);
            }
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            if (events != NULL) {
                float x = 0.0f;
                float y = 0.0f;
                SDL_GetMouseState(&x, &y);
                schultz_events_scroll(events,
                                      schultz_sdl_pointer(window, x, y),
                                      event.wheel.x, -event.wheel.y);
            }
            break;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (events != NULL) {
                uint32_t key = schultz_sdl_key(event.key.key);

                /* Tab moves focus before the key reaches a listener, which is
                 * what every desktop toolkit does. */
                if (event.type == SDL_EVENT_KEY_DOWN &&
                    key == (uint32_t)SCHULTZ_KEY_TAB) {
                    schultz_events_focus_move(events,
                        (modifiers & SCHULTZ_MOD_SHIFT) ? 0 : 1);
                } else {
                    schultz_events_key(events, key, modifiers,
                                       event.type == SDL_EVENT_KEY_DOWN);
                }
            }
            break;

        case SDL_EVENT_TEXT_INPUT:
            if (events != NULL && event.text.text != NULL) {
                schultz_events_text_input(events, event.text.text);
            }
            break;

        case SDL_EVENT_TEXT_EDITING:
            /* An in progress IME composition, replaced as the user types. */
            if (events != NULL && event.edit.text != NULL) {
                schultz_events_text_editing(events, event.edit.text,
                                            event.edit.start);
            }
            break;

        default:
            break;
        }
    }

    schultz_sdl_sync_delete(events);
    schultz_sdl_sync_text_input(window, events);
    schultz_sdl_sync_cursor(window, events);
    return SCHULTZ_OK;
}

int32_t schultz_sdl_window_set_text_input(schultz_sdl_window *window,
                                          int32_t enabled)
{
    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (enabled) {
        if (!SDL_StartTextInput(window->window)) {
            return SCHULTZ_ERR_INVALID_ARGUMENT;
        }
    } else {
        SDL_StopTextInput(window->window);
    }
    return SCHULTZ_OK;
}

int32_t schultz_sdl_window_set_text_area(schultz_sdl_window *window,
                                         schultz_rect area, int32_t cursor)
{
    SDL_Rect rect;

    if (window == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * Back the other way: the platform places an input method's candidate
     * window in its own coordinates, and this rectangle is in the toolkit's.
     */
    {
        float x = area.x / window->input_scale;
        float y = area.y / window->input_scale;
        float w = area.width / window->input_scale;
        float h = area.height / window->input_scale;

        /*
         * And the turn, which everywhere else goes panel to buffer and here
         * goes buffer to panel: this is the one rectangle the toolkit hands
         * outward rather than takes in. A quarter turn swaps its sides.
         */
        switch (window->turn) {
        case 1u:
            rect.x = (int)(window->panel_across - y - h);
            rect.y = (int)x;
            rect.w = (int)h;
            rect.h = (int)w;
            break;
        case 2u:
            rect.x = (int)(window->panel_across - x - w);
            rect.y = (int)(window->panel_down - y - h);
            rect.w = (int)w;
            rect.h = (int)h;
            break;
        case 3u:
            rect.x = (int)y;
            rect.y = (int)(window->panel_down - x - w);
            rect.w = (int)h;
            rect.h = (int)w;
            break;
        default:
            rect.x = (int)x;
            rect.y = (int)y;
            rect.w = (int)w;
            rect.h = (int)h;
            break;
        }
    }
    if (!SDL_SetTextInputArea(window->window, &rect, cursor)) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    return SCHULTZ_OK;
}
