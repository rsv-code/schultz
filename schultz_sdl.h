/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_sdl.h
 * @brief SDL3 window and presentation.
 *
 * Internal header. This layer owns the window, the CPU pixel buffer that
 * ThorVG rasterizes into, and the texture that buffer is uploaded to. It does
 * not draw anything itself.
 *
 * Presentation is deliberately dumb: upload the changed rectangle, draw one
 * textured quad, present. The same path serves every target, so there is no
 * GPU renderer to keep in sync with a software one.
 *
 * Platform input is turned into toolkit events here and handed to the router,
 * which is what decides where each one lands. Nothing in this layer knows
 * what a widget is.
 *
 * **Not part of the host facing interface.** The platform layer. A host
 * binds to schultz_api.h; this header is the toolkit's own and may change
 * without notice.
 */

#ifndef SCHULTZ_SDL_H
#define SCHULTZ_SDL_H

#include "schultz_event.h"
#include "schultz_geom.h"
/* For schultz_clipboard_make_fn, which the clipboard functions below take. */
#include "schultz_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque window state. Create with schultz_sdl_window_create. */
typedef struct schultz_sdl_window schultz_sdl_window;

/*
 * SDL's own window type, declared the way SDL declares it rather than by
 * including SDL here. This header is included by files that have no business
 * seeing the whole platform library, and a repeated typedef of the same type
 * is allowed.
 */
/** @brief SDL's own window type, forward declared as SDL declares it. */
typedef struct SDL_Window SDL_Window;

/**
 * @brief Returns the platform's own window, for the few calls that need it.
 *
 * A file dialog is told which window it belongs to, and nothing above the
 * platform layer can name one. This is the seam.
 *
 * @param window The window to unwrap. NULL yields NULL.
 * @return The platform window.
 */
SDL_Window *schultz_sdl_window_handle(schultz_sdl_window *window);

/**
 * @brief Starts SDL's video subsystem.
 *
 * Call once before creating a window, and pair with schultz_sdl_quit.
 *
 * @param video_driver Which SDL video driver to ask for, such as "offscreen"
 *                     or "kmsdrm", or NULL to let SDL choose. Only read here,
 *                     because SDL settles the choice while it starts and
 *                     never revisits it. What the environment says still
 *                     wins.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when SDL refused to
 *         start. Call schultz_sdl_error for the reason.
 */
int32_t schultz_sdl_init(const char *video_driver);

/** @brief Shuts SDL down. Safe to call without a matching successful init. */
void schultz_sdl_quit(void);

/**
 * @brief Returns SDL's description of the most recent failure.
 *
 * @return A NUL terminated string owned by SDL. Never NULL, but it is empty
 *         when nothing has failed.
 */
const char *schultz_sdl_error(void);

/**
 * @brief Creates a window, its pixel buffer, and its texture.
 *
 * @param title      Window title, NUL terminated. Must not be NULL.
 * @param width      Window width in pixels. Must be greater than zero. When
 *                   maximized, this is the size the window returns to.
 * @param height     Window height in pixels. Must be greater than zero.
 * @param maximized  Nonzero to open maximized, filling the desktop's work
 *                   area. The buffer is then sized to whatever the window
 *                   manager actually gave, which may not be either the
 *                   requested size or the whole display. Maximizing implies
 *                   resizable, because a window manager will not maximize a
 *                   window that says it cannot be resized.
 * @param resizable  Nonzero to let the user drag the window's edges. Zero
 *                   fixes the window at the size it opened at.
 * @param allow_rotate Nonzero to let a screen that turns follow the device.
 *                   Zero holds it the way it opened. Ignored where the
 *                   screen does not turn.
 * @param out_window Receives the new window. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad argument or an
 *         SDL failure, or SCHULTZ_ERR_OUT_OF_MEMORY when the pixel buffer
 *         could not be allocated.
 */
int32_t schultz_sdl_window_create(const char *title, uint32_t width,
                                  uint32_t height, int32_t maximized,
                                  int32_t resizable, int32_t allow_rotate,
                                  schultz_sdl_window **out_window);

/**
 * @brief Says which ways up a screen may be used, before one is opened.
 *
 * Read when a window is created and not afterwards, so this belongs beside
 * schultz_sdl_init rather than anywhere later. Does nothing on a platform
 * whose screens do not turn.
 *
 * @param orientation SCHULTZ_SCREEN_ANY, _PORTRAIT or _LANDSCAPE.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT for anything else.
 */
int32_t schultz_sdl_set_orientation(uint32_t orientation);

/**
 * @brief Sets how far the picture is turned before it reaches the panel.
 *
 * For a display fitted into its case the other way round. The interface is
 * drawn in the shape it wants and turned on the way out, and a pointer is
 * turned back on the way in, so nothing above this knows the panel is
 * sideways.
 *
 * Set before the first frame. It resizes the buffer, so calling it while
 * drawing throws away the frame in hand.
 *
 * @param window  The window. Must not be NULL.
 * @param quarters Quarter turns clockwise, 0 to 3.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_sdl_window_set_turn(schultz_sdl_window *window,
                                    uint32_t quarters);

/**
 * @brief Whether the platform turns the screen itself.
 *
 * True on a phone or a tablet, where the device turns and the toolkit only
 * says which ways up it may. False everywhere else, where a screen that has
 * to be the other way round has to be turned on the way out.
 *
 * @param window The window. Unused, and accepted so this reads as a question
 *               about the window rather than about the program.
 * @return Nonzero where the screen turns on its own.
 */
int32_t schultz_sdl_window_turns(const schultz_sdl_window *window);

/**
 * @brief Returns the panel's own width in pixels, before any turn.
 *
 * schultz_sdl_window_width is the buffer, which on a quarter turn is the
 * panel on its side. This is the panel itself, which is what deciding how
 * far to turn has to be measured against.
 *
 * @param window The window. NULL yields zero.
 * @return Width in pixels.
 */
uint32_t schultz_sdl_window_panel_width(const schultz_sdl_window *window);

/**
 * @brief Returns the panel's own height in pixels, before any turn.
 *
 * @param window The window. NULL yields zero.
 * @return Height in pixels.
 */
uint32_t schultz_sdl_window_panel_height(const schultz_sdl_window *window);

/**
 * @brief Destroys a window, its texture, and its pixel buffer.
 *
 * @param window The window to destroy. NULL is accepted and does nothing.
 */
void schultz_sdl_window_destroy(schultz_sdl_window *window);

/**
 * @brief Returns how many buffer pixels the screen packs into one of its own
 *        coordinate units.
 *
 * Three on a dense phone, two on a Retina laptop, one on an ordinary monitor.
 * It is what the frame multiplies by when it is scaling to the screen, and
 * what a host divides by when it wants to name a real pixel.
 *
 * @param window The window to query. NULL yields 1.
 * @return The scale. Never zero or negative.
 */
float schultz_sdl_window_unit_scale(const schultz_sdl_window *window);

/**
 * @brief How many pixels sit behind one of the platform's window
 *        coordinates.
 *
 * One where a window is measured in pixels, which is Android and an ordinary
 * monitor; two or three where it is measured in points and the buffer behind
 * it is denser, which is Apple. This is what a reported pointer position is
 * in, not what the interface is drawn at.
 *
 * @param window The window. NULL yields 1.
 * @return The density. Never zero or negative.
 */
float schultz_sdl_window_pixel_density(const schultz_sdl_window *window);

/**
 * @brief Sets how a platform pointer position becomes a toolkit one.
 *
 * The platform reports a pointer in its own window coordinates whatever the
 * buffer's resolution, so the two agree only when the toolkit is working in
 * those same units. Tell this layer which it is, or every press lands
 * somewhere other than where it happened.
 *
 * @param window The window to configure. Must not be NULL.
 * @param scale  Toolkit units per window coordinate. One when the frame is
 *               scaling to the screen, the pixel scale when it is not.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_sdl_window_set_input_scale(schultz_sdl_window *window,
                                           float scale);

/**
 * @brief Returns the part of the window it is safe to put content in.
 *
 * A phone puts a camera notch over the top of its screen and a home indicator
 * across the bottom. This is what is left.
 *
 * In buffer pixels, not window coordinates: the platform reports it in the
 * latter, and on a three times display those differ by three.
 *
 * A window with nothing in the way of it, which is every desktop, reports the
 * whole buffer. So does one the platform will not answer for, because a
 * window that cannot say what is safe is better treated as entirely safe than
 * as entirely unsafe.
 *
 * @param window   The window to query. Must not be NULL.
 * @param out_area Receives the safe rectangle. Must not be NULL. Never empty.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer.
 */
int32_t schultz_sdl_window_safe_area(schultz_sdl_window *window,
                                     schultz_rect *out_area);

/**
 * @brief Returns the pixel buffer a rasterizer should target.
 *
 * The buffer is premultiplied ARGB8888 and stays at one address for the life
 * of the window, so a backend may hold the pointer.
 *
 * @param window The window to query. Must not be NULL.
 * @return The pixel buffer, or NULL when window is NULL.
 */
uint32_t *schultz_sdl_window_pixels(schultz_sdl_window *window);

/**
 * @brief Returns the buffer width in pixels.
 *
 * @param window The window to query. NULL yields 0.
 * @return Width in pixels.
 */
uint32_t schultz_sdl_window_width(const schultz_sdl_window *window);

/**
 * @brief Returns the buffer height in pixels.
 *
 * @param window The window to query. NULL yields 0.
 * @return Height in pixels.
 */
uint32_t schultz_sdl_window_height(const schultz_sdl_window *window);

/**
 * @brief Returns the distance between buffer rows, in pixels.
 *
 * @param window The window to query. NULL yields 0.
 * @return Stride in pixels, not bytes.
 */
uint32_t schultz_sdl_window_stride(const schultz_sdl_window *window);

/**
 * @brief Reports whether this window is the one the user is looking at.
 *
 * A window opens without focus until the platform says otherwise, so this
 * starts at zero and becomes true when the first focus event arrives.
 *
 * @param window The window to ask. NULL yields 0.
 * @return 1 when the window has focus, 0 otherwise.
 */
int32_t schultz_sdl_window_focused(const schultz_sdl_window *window);

/**
 * @brief Reports whether the window changed size, and forgets that it did.
 *
 * The pixel buffer and the texture are rebuilt as the change arrives, so by
 * the time this says yes the window's width, height, stride and pixels are
 * already the new ones. Anything above the platform layer that remembers a
 * size has to ask this once a frame.
 *
 * @param window The window to ask. NULL yields 0.
 * @return 1 when the size changed since the last call, 0 otherwise.
 */
int32_t schultz_sdl_window_take_resized(schultz_sdl_window *window);

/**
 * @brief Uploads one region of the pixel buffer to the texture.
 *
 * Only the given rectangle is uploaded, which is the point of tracking what
 * changed at all. An empty rectangle uploads nothing.
 *
 * Call it once for each area that changed, then schultz_sdl_window_present
 * once. The two are apart because a frame has several changed areas and only
 * one of it: presenting per area would show the window several times a frame.
 *
 * @param window The window to upload into. Must not be NULL.
 * @param dirty  A region changed since the last present, in pixels. It is
 *               clamped to the buffer, so a caller may pass a region that
 *               overhangs an edge.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when window is NULL or
 *         SDL rejected the upload.
 */
int32_t schultz_sdl_window_upload(schultz_sdl_window *window,
                                  schultz_rect dirty);

/**
 * @brief Puts the texture on the screen.
 *
 * Whatever schultz_sdl_window_upload has put into the texture, plus whatever
 * was already there from previous frames, which is what makes a partial
 * repaint work at all.
 *
 * @param window The window to present. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_sdl_window_present(schultz_sdl_window *window);

/**
 * @brief Turns vertical sync on or off for a window.
 *
 * With vsync on, presenting blocks until the display is ready, which paces
 * the frame loop to the refresh rate and stops it spinning a core. It is off
 * by default in SDL, so a loop that never enables it runs as fast as the CPU
 * allows.
 *
 * @param window  The window to configure. Must not be NULL.
 * @param enabled Nonzero to present in step with the display, zero to present
 *                as fast as possible, which is what benchmarking wants.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when window is NULL or
 *         the driver refused the setting.
 */
int32_t schultz_sdl_window_set_vsync(schultz_sdl_window *window,
                                     int32_t enabled);

/**
 * @brief Returns milliseconds since SDL started.
 *
 * Animation and timing should be driven from this rather than from a frame
 * counter, so behaviour does not change with the frame rate of the machine.
 *
 * @return Milliseconds elapsed since schultz_sdl_init.
 */
uint64_t schultz_sdl_ticks_ms(void);

/**
 * @brief Processes every pending platform event into the toolkit's router.
 *
 * This is the seam between the platform and the toolkit: SDL's event union is
 * translated into calls on schultz_events, and nothing above this function
 * sees an SDL type.
 *
 * @param window          The window to poll. Must not be NULL.
 * @param events          The router to feed, or NULL to only watch for a
 *                        close request.
 * @param out_should_quit Set to 1 when the user asked to close, 0 otherwise.
 *                        Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when window or
 *         out_should_quit is NULL.
 */
int32_t schultz_sdl_window_process_events(schultz_sdl_window *window,
                                schultz_events *events,
                                int32_t *out_should_quit);

/**
 * @brief Turns text input on or off for a window.
 *
 * While it is on, the platform delivers committed text and in progress IME
 * composition. It should be enabled only while a text field holds focus,
 * because on mobile it is what raises the on screen keyboard.
 *
 * @param window  The window to configure. Must not be NULL.
 * @param enabled Nonzero to start text input, zero to stop it.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_sdl_window_set_text_input(schultz_sdl_window *window,
                                          int32_t enabled);

/**
 * @brief Whether a physical keyboard is attached.
 *
 * What decides whether a window puts up a keyboard of its own: a machine with
 * keys does not need one drawn, and a touch screen with no keys has no other
 * way to type.
 *
 * @return Nonzero when the platform reports at least one keyboard.
 */
int32_t schultz_sdl_has_keyboard(void);

/**
 * @brief Tells the platform where the text being edited is on screen.
 *
 * An IME places its candidate window relative to this, so a CJK user sees the
 * candidate list beside the text rather than in a corner. Call it whenever
 * the caret moves.
 *
 * @param window The window to configure. Must not be NULL.
 * @param area   The caret area in window pixels.
 * @param cursor Cursor offset within that area, in pixels.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_sdl_window_set_text_area(schultz_sdl_window *window,
                                         schultz_rect area, int32_t cursor);

/**
 * @brief Offers formats to the system clipboard, as the toolkit's writer.
 *
 * Matches schultz_clipboard_offer_fn, so it installs straight into a tree
 * with schultz_tree_set_clipboard. The context is ignored.
 *
 * An offer of text and nothing else goes through SDL's own text call, because
 * that is the one that knows every platform's other names for plain text.
 * Anything else is offered as given and produced only when something asks.
 *
 * @param context      Ignored.
 * @param formats      The media types, most preferred first.
 * @param count        How many.
 * @param make         Produces the bytes of whichever is asked for.
 * @param make_context Passed to make, unchanged.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when the platform
 *         refused.
 */
int32_t schultz_sdl_clipboard_offer(void *context,
                                    const char *const *formats,
                                    uint32_t count,
                                    schultz_clipboard_make_fn make,
                                    void *make_context);

/**
 * @brief Takes one format off the system clipboard, as the toolkit's reader.
 *
 * Matches schultz_clipboard_take_fn. The context is ignored.
 *
 * @param context    Ignored.
 * @param format     The media type wanted.
 * @param out_length Receives how many bytes.
 * @return The bytes, or NULL when the clipboard holds none in that format.
 *         Owned here and valid until the next call to this function.
 */
const void *schultz_sdl_clipboard_take(void *context, const char *format,
                                       uint64_t *out_length);

/**
 * @brief Says whether the system clipboard holds a format.
 *
 * Matches schultz_clipboard_holds_fn. The context is ignored.
 *
 * @param context Ignored.
 * @param format  The media type to ask about.
 * @return Nonzero when it is there.
 */
int32_t schultz_sdl_clipboard_holds(void *context, const char *format);

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_SDL_H */
