/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_api.h
 * @brief The whole host facing interface, in one include.
 *
 * A host binds to this and to nothing else. Everything reachable from here
 * follows the ABI rules in schultz.h and is safe to generate bindings from.
 * Everything not reachable from here is how the toolkit is built rather than
 * what it offers, and is not meant to be bound to.
 *
 * The split matters because Schultz has about twenty headers and only some of
 * them are aimed at anyone outside: the arena, the handle table, the draw
 * command list and the two rasterizer backends are internal machinery. A
 * binding generator pointed at the whole directory would export all of it.
 *
 * What is here:
 *
 *   - **schultz.h** handles, result codes, version
 *   - **schultz_audio.h** sound out and microphone in
 *   - **schultz_geom.h** points, rectangles, colours, paints and strokes
 *   - **schultz_node.h** the tree: nodes, bounds, state, accessibility
 *   - **schultz_style.h** tokens, values, patches, themes, resolved styles
 *   - **schultz_layout.h** the panes and per child layout properties
 *   - **schultz_event.h** routing, keys, and what a host is told
 *   - **schultz_widget.h** the widget interface, and what the tree is given
 *   - **schultz_widgets.h** every widget that ships
 *   - **schultz_font.h** loading faces and reading their metrics
 *   - **schultz_image.h** loading pictures and animations
 *   - **schultz_resource.h** gradients and dash patterns
 *   - **schultz_render.h** rendering a widget, or a window, into memory
 *   - **schultz_video.h** playing video into a node
 *   - **schultz_camera.h** the camera, and a node showing what it sees
 *   - **schultz_voice.h** cleaning up microphone sound before it is sent
 *   - **schultz_window.h** the frame driver: a window and its frame loop
 *
 * What is not, and why:
 *
 *   - **schultz_arena.h**, **schultz_handle.h** how the toolkit allocates and
 *     identifies things. A host holds handles; it does not make them.
 *   - **schultz_paint.h** the draw command list. A widget written in C emits
 *     into it; a host written above the boundary does not.
 *   - **schultz_text.h**, **schultz_glyphs.h** shaping and glyph rasterizing,
 *     which the widgets do on the host's behalf.
 *   - **schultz_thorvg.h**, **schultz_sdl.h** the rasterizer and the platform.
 *     A host reaches these through the frame driver rather than directly.
 *   - **schultz_debug.h** a development aid.
 *   - anything ending in **_internal.h**, which exists to keep a backend type
 *     out of a public header.
 *
 * schultz_widget.h does include the arena and the draw list, because the
 * widget vtable is written against them. That is for widgets written in C
 * inside the toolkit. A host does not implement that vtable; see below.
 *
 * ## Upcalls
 *
 * An upcall is the toolkit calling the host, and across a foreign function
 * interface it is the expensive direction: it needs a stub the host language
 * has to keep alive, it runs on the toolkit's thread, and it cannot be
 * batched. So there are five of them in the whole interface, every one is
 * optional, and none happens per widget or per frame by necessity.
 *
 *   - **schultz_event_callback_fn** what a widget wants the host to know: a click,
 *     a changed value, a menu choice. A host that would rather not be called
 *     at all turns on schultz_events_set_queue and reads the same events with
 *     schultz_events_drain, one downcall per frame.
 *   - **schultz_clipboard_offer_fn**, **schultz_clipboard_take_fn** and
 *     **schultz_clipboard_holds_fn** cut, copy and paste, which the toolkit
 *     cannot reach on its own. A clipboard carries a set of formats and
 *     builds one only when something asks for it, which is how every platform
 *     underneath works. The frame driver installs the platform's set, so a
 *     host normally supplies none of them.
 *   - **schultz_scroll_bar_changed_fn** where a bar was dragged to. Needed
 *     only for a bar used on its own; a scroll view wires its own.
 *   - **schultz_window_update_fn** what to change before each frame, when the
 *     loop itself has been handed to schultz_window_run. A host that calls
 *     schultz_window_update once a frame passes nothing.
 *
 * Two cases that usually force an upcall do not here.
 *
 * **Text measurement.** The toolkit shapes and measures text itself, with
 * HarfBuzz and FreeType, so it never has to ask the host how wide a string
 * is. That is the single upcall most toolkits cannot avoid.
 *
 * **Custom widget paint.** A host does not implement the widget vtable; it
 * uses a canvas. Drawing recorded between schultz_canvas_begin and
 * schultz_canvas_end is kept and replayed every frame, so appearance costs
 * downcalls once rather than an upcall per frame, and a node token plus the
 * event queue gives the same node its behaviour. A widget written in C is a
 * different thing, and stays inside the toolkit.
 */

#ifndef SCHULTZ_API_H
#define SCHULTZ_API_H

#include "schultz.h"
#include "schultz_audio.h"
#include "schultz_camera.h"
#include "schultz_event.h"
#include "schultz_font.h"
#include "schultz_window.h"
#include "schultz_geom.h"
#include "schultz_image.h"
#include "schultz_layout.h"
#include "schultz_node.h"
#include "schultz_render.h"
#include "schultz_selection.h"
#include "schultz_resource.h"
#include "schultz_style.h"
#include "schultz_video.h"
#include "schultz_voice.h"
#include "schultz_widget.h"
#include "schultz_widgets.h"

#endif /* SCHULTZ_API_H */
