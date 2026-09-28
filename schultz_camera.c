/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_camera.c
 * @brief Finding a camera, opening it, and showing what it sees.
 *
 * Thin. SDL does more here than anywhere else in the toolkit: it enumerates
 * the devices, asks the operating system for permission, and -- the part that
 * matters most -- converts whatever the camera actually produces into the
 * format asked for. A webcam's own formats are Motion JPEG and packed YUV,
 * and SDL carries a JPEG decoder and the colour conversions to turn either
 * into ARGB. So there is no decoding in this file at all, and the camera cost
 * no new dependency.
 *
 * What is left for this file is the shape of the interface: plain
 * identifiers rather than SDL's, a frame that stays valid until the next one
 * is asked for rather than one that has to be handed back, and a node that
 * does the permission waiting on the host's behalf.
 */

#include "schultz_camera.h"
#include "schultz_camera_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_camera.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>

#include "schultz_image.h"
#include "schultz_paint.h"
#include "schultz_widget.h"

/*
 * SDL's camera subsystem, started the first time anything here needs it and
 * then left running.
 *
 * It has to be left running, because the identifiers it hands out only mean
 * anything while it is. Stopping it between a call that lists the cameras and
 * a call that opens one renumbers them, and the identifier the host is
 * holding then names nothing. That was tried first and is why this is
 * written the way it is.
 *
 * Leaving it running costs nothing anyone can see. Starting the subsystem
 * enumerates the devices; it opens none of them, so no indicator light comes
 * on and nobody is asked for permission. schultz_camera_open is what does
 * that, and it is the only thing that does.
 *
 * SDL_WasInit rather than a count of our own, because a host that also has a
 * window will have SDL_Quit called underneath this when the window goes, and
 * a count would then say the subsystem was up when it was not.
 */
static int32_t schultz_camera_ready(void)
{
    if (SDL_WasInit(SDL_INIT_CAMERA) != 0u) {
        return 1;
    }
    return SDL_InitSubSystem(SDL_INIT_CAMERA) ? 1 : 0;
}

/** What one open camera carries. */
/*
 * Named here because a camera holds a list of the previews showing it. What
 * one is lives further down, beside the widget that uses it.
 */
typedef struct schultz_camera_preview_data schultz_camera_preview_data;

/*
 * Tells every preview showing this camera to stop. Defined further down, with
 * the previews, and declared here because closing a camera is the one thing
 * that has to reach them.
 */
static void schultz_camera_drop_viewers(schultz_camera *camera);

struct schultz_camera {
    SDL_Camera *device;
    uint32_t    width;      /**< Of the pictures, once they arrive. */
    uint32_t    height;
    /*
     * The last picture, copied. SDL hands out a surface that has to be given
     * back before the next one can arrive, so keeping one means copying it.
     * The copy is what lets the interface promise pixels that stay put until
     * the next call rather than pixels that must be released.
     */
    uint32_t   *argb;
    size_t      words;
    uint64_t    taken;
    /**
     * Pictures that arrived while this one was being dealt with, and were
     * passed over to get to the newest. A count of how far behind the camera
     * this program is running: zero means it is keeping up.
     */
    uint64_t    skipped;
    /**
     * The previews showing this camera, threaded through their own data.
     *
     * A preview holds the camera by pointer, and closing one used to leave
     * every preview holding a pointer to freed memory -- read on the very
     * next turn, which is the turn after a stop button. So each says it is
     * watching, and closing tells them all to stop.
     */
    schultz_camera_preview_data *viewers;
};

/* ------------------------------------------------------------- the list */

uint32_t schultz_camera_count(void)
{
    SDL_CameraID *ids;
    int count = 0;

    if (!schultz_camera_ready()) {
        return 0u;
    }
    ids = SDL_GetCameras(&count);
    SDL_free(ids);
    return (count > 0) ? (uint32_t)count : 0u;
}

uint64_t schultz_camera_device(uint32_t index)
{
    SDL_CameraID *ids;
    int count = 0;
    uint64_t found = 0u;

    if (!schultz_camera_ready()) {
        return 0u;
    }
    ids = SDL_GetCameras(&count);
    if (ids != NULL && index < (uint32_t)count) {
        found = (uint64_t)ids[index];
    }
    SDL_free(ids);
    return found;
}

int32_t schultz_camera_name(uint64_t device, char *out_name, uint64_t size)
{
    const char *name;

    if (out_name == NULL || size == 0u) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    out_name[0] = '\0';
    if (!schultz_camera_ready()) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    name = SDL_GetCameraName((SDL_CameraID)device);
    if (name == NULL) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    strncpy(out_name, name, (size_t)size - 1u);
    out_name[size - 1u] = '\0';
    return SCHULTZ_OK;
}

uint32_t schultz_camera_facing(uint64_t device)
{
    SDL_CameraPosition where;
    uint32_t facing = SCHULTZ_CAMERA_FACING_UNKNOWN;

    if (!schultz_camera_ready()) {
        return facing;
    }
    where = SDL_GetCameraPosition((SDL_CameraID)device);
    if (where == SDL_CAMERA_POSITION_FRONT_FACING) {
        facing = SCHULTZ_CAMERA_FACING_PERSON;
    } else if (where == SDL_CAMERA_POSITION_BACK_FACING) {
        facing = SCHULTZ_CAMERA_FACING_AWAY;
    }
    return facing;
}

/* ------------------------------------------------------ opening and closing */

/*
 * The sizes a camera offers.
 *
 * SDL hands back one allocation holding the whole list, so both calls below
 * ask for it, read what they need and give it back. That is two allocations
 * to read one entry, and it is the right trade: a preview asks this once
 * while it is starting, and the alternative is a cache with a lifetime and a
 * camera that may be unplugged.
 */
uint32_t schultz_camera_size_count(uint64_t device)
{
    SDL_CameraSpec **specs;
    int count = 0;

    if (!schultz_camera_ready()) {
        return 0u;
    }
    specs = SDL_GetCameraSupportedFormats((SDL_CameraID)device, &count);
    if (specs == NULL) {
        return 0u;              /* a camera is allowed to say nothing */
    }
    SDL_free(specs);
    return (count < 0) ? 0u : (uint32_t)count;
}

int32_t schultz_camera_size_at(uint64_t device, uint32_t index,
                               uint32_t *out_width, uint32_t *out_height,
                               uint32_t *out_rate)
{
    SDL_CameraSpec **specs;
    int count = 0;
    int32_t result = SCHULTZ_ERR_INVALID_ARGUMENT;

    if (!schultz_camera_ready()) {
        return result;
    }
    specs = SDL_GetCameraSupportedFormats((SDL_CameraID)device, &count);
    if (specs == NULL) {
        return result;
    }
    if (count > 0 && index < (uint32_t)count && specs[index] != NULL) {
        const SDL_CameraSpec *one = specs[index];

        if (out_width != NULL) {
            *out_width = (one->width > 0) ? (uint32_t)one->width : 0u;
        }
        if (out_height != NULL) {
            *out_height = (one->height > 0) ? (uint32_t)one->height : 0u;
        }
        if (out_rate != NULL) {
            /*
             * A rate arrives as a fraction, so that 30000/1001 can say what
             * 29.97 cannot. Rounded to whole pictures a second, because that
             * is what a caller choosing a size is deciding between.
             */
            *out_rate = (one->framerate_denominator > 0)
                ? (uint32_t)((one->framerate_numerator +
                              one->framerate_denominator / 2) /
                             one->framerate_denominator)
                : 0u;
        }
        result = SCHULTZ_OK;
    }
    SDL_free(specs);
    return result;
}
/*
 * The rate to ask for, as the fraction the camera itself named.
 *
 * Asking for nothing is not neutral. SDL reads an unset rate as zero and then
 * picks the rate nearest to it, which is the *slowest* the camera offers at
 * that size; and a phone lists the bottom of every range as well as the top,
 * so the bottom is often one picture a second. The Apple backend then pins
 * the session to exactly that, both ends of the range, and the camera obeys.
 * A preview that draws once a second looks broken and is not.
 *
 * So a rate is always named. The fraction is carried across whole rather than
 * reduced to a number, because 30000/1001 is a real rate and 29 is not.
 */
static void schultz_camera_pick_rate(uint64_t device, uint32_t width,
                                     uint32_t height, uint32_t wanted,
                                     int *out_num, int *out_den)
{
    SDL_CameraSpec **specs;
    int count = 0;
    int i;
    float best = -1.0f;

    *out_num = 0;
    *out_den = 0;
    specs = SDL_GetCameraSupportedFormats((SDL_CameraID)device, &count);
    if (specs == NULL) {
        return;                 /* it will not say; let SDL decide */
    }
    for (i = 0; i < count; i++) {
        const SDL_CameraSpec *one = specs[i];
        float rate;
        float score;

        if (one == NULL || one->framerate_denominator <= 0) {
            continue;
        }
        /* A size of zero means the camera's own, so every entry is a
         * candidate and the rate is all that is being chosen. */
        if (width > 0u && height > 0u &&
            ((uint32_t)one->width != width ||
             (uint32_t)one->height != height)) {
            continue;
        }
        rate = (float)one->framerate_numerator /
               (float)one->framerate_denominator;
        /* Fastest when nothing was asked for, nearest when something was. */
        score = (wanted == 0u) ? rate : -fabsf(rate - (float)wanted);
        if (score > best) {
            best = score;
            *out_num = one->framerate_numerator;
            *out_den = one->framerate_denominator;
        }
    }
    SDL_free(specs);
}

int32_t schultz_camera_open(uint64_t device, uint32_t width, uint32_t height,
                            uint32_t rate, schultz_camera **out_camera)
{
    schultz_camera *camera;
    SDL_CameraSpec want;
    const SDL_CameraSpec *asked = NULL;

    if (out_camera == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_camera = NULL;
    if (device == 0u) {
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    camera = (schultz_camera *)calloc(1, sizeof(*camera));
    if (camera == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    if (!schultz_camera_ready()) {
        free(camera);
        return SCHULTZ_ERR_UNAVAILABLE;
    }

    /*
     * Asking for ARGB is what makes the rest of this file short. A camera
     * that cannot produce it -- which is nearly all of them -- has its output
     * converted, Motion JPEG decoded and all, before a frame is handed over.
     *
     * Asking for nothing at all is also allowed, and then the camera's own
     * size is used. The format is still asked for either way: a picture this
     * toolkit cannot draw is no use whatever size it is.
     */
    SDL_zero(want);
    want.format = SDL_PIXELFORMAT_ARGB8888;
    {
        int num = 0;
        int den = 0;

        schultz_camera_pick_rate(device, width, height, rate, &num, &den);
        want.framerate_numerator   = num;
        want.framerate_denominator = den;
    }
    if (width > 0u && height > 0u) {
        want.width  = (int)width;
        want.height = (int)height;
    }
    asked = &want;

    camera->device = SDL_OpenCamera((SDL_CameraID)device, asked);
    if (camera->device == NULL) {
        free(camera);
        return SCHULTZ_ERR_UNAVAILABLE;
    }
    *out_camera = camera;
    return SCHULTZ_OK;
}

void schultz_camera_close(schultz_camera *camera)
{
    if (camera == NULL) {
        return;
    }
    /*
     * Before anything is freed. A preview reads its camera on every turn, so
     * one still pointing here would read this memory on the turn after this
     * call returns -- which is the turn after a stop button is pressed. What
     * it was last showing stays on screen; it simply stops asking for more.
     */
    schultz_camera_drop_viewers(camera);
    if (camera->device != NULL) {
        SDL_CloseCamera(camera->device);
    }
    free(camera->argb);
    free(camera);
}

uint32_t schultz_camera_permission(const schultz_camera *camera)
{
    int state;

    if (camera == NULL || camera->device == NULL) {
        return (uint32_t)SCHULTZ_CAMERA_REFUSED;
    }
    state = SDL_GetCameraPermissionState(camera->device);
    if (state > 0) {
        return (uint32_t)SCHULTZ_CAMERA_ALLOWED;
    }
    if (state < 0) {
        return (uint32_t)SCHULTZ_CAMERA_REFUSED;
    }
    return (uint32_t)SCHULTZ_CAMERA_WAITING;
}

int32_t schultz_camera_size(const schultz_camera *camera, uint32_t *out_width,
                            uint32_t *out_height)
{
    if (out_width == NULL || out_height == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    *out_width  = (camera == NULL) ? 0u : camera->width;
    *out_height = (camera == NULL) ? 0u : camera->height;
    return SCHULTZ_OK;
}

uint64_t schultz_camera_frames_taken(const schultz_camera *camera)
{
    return (camera == NULL) ? 0u : camera->taken;
}

uint64_t schultz_camera_frames_skipped(const schultz_camera *camera)
{
    return (camera == NULL) ? 0u : camera->skipped;
}

uint32_t schultz_camera_quarters(float degrees)
{
    int32_t whole;

    /*
     * To the nearest degree first, then into 0 to 359, so that -90 and 270
     * are the same turn and a full turn is none.
     */
    whole = (int32_t)(degrees + ((degrees < 0.0f) ? -0.5f : 0.5f));
    whole %= 360;
    if (whole < 0) {
        whole += 360;
    }
    if (whole == 90)  { return 1u; }
    if (whole == 180) { return 2u; }
    if (whole == 270) { return 3u; }
    return 0u;
}

void schultz_camera_upright(const void *from, uint32_t width, uint32_t height,
                            uint32_t pitch, uint32_t quarters, uint32_t *into)
{
    uint32_t row;

    if (from == NULL || into == NULL) {
        return;
    }
    for (row = 0; row < height; row++) {
        const uint32_t *line = (const uint32_t *)
            ((const unsigned char *)from + (size_t)row * (size_t)pitch);
        uint32_t col;

        if (quarters == 0u) {
            /* Straight across, which is the case on a desktop and on a
             * phone held the way its camera faces. */
            memcpy(into + (size_t)row * (size_t)width, line,
                   (size_t)width * sizeof(*into));
            continue;
        }
        for (col = 0; col < width; col++) {
            uint32_t x;
            uint32_t y;
            uint32_t stride;

            switch (quarters) {
            case 1u:   /* a quarter clockwise: the top edge becomes the right */
                x = height - 1u - row;
                y = col;
                stride = height;
                break;
            case 2u:   /* upside down */
                x = width - 1u - col;
                y = height - 1u - row;
                stride = width;
                break;
            default:   /* three quarters: the top edge becomes the left */
                x = row;
                y = width - 1u - col;
                stride = height;
                break;
            }
            into[(size_t)y * (size_t)stride + (size_t)x] = line[col];
        }
    }
}

/* ------------------------------------------------------------- a picture */

uint32_t schultz_camera_rate(const schultz_camera *camera)
{
    SDL_CameraSpec got;

    if (camera == NULL || camera->device == NULL) {
        return 0u;
    }
    /*
     * Asked every time rather than kept from the open. A camera settles its
     * format when the person allows it, which is later than the open and may
     * be much later, so a value stored at the open would be a guess.
     */
    SDL_zero(got);
    if (!SDL_GetCameraFormat(camera->device, &got) ||
        got.framerate_denominator <= 0) {
        return 0u;
    }
    return (uint32_t)((got.framerate_numerator +
                       got.framerate_denominator / 2) /
                      got.framerate_denominator);
}

int32_t schultz_camera_frame(schultz_camera *camera,
                             const uint32_t **out_pixels, uint32_t *out_width,
                             uint32_t *out_height, uint64_t *out_when_ns)
{
    SDL_Surface *picture;
    uint64_t when = 0u;
    size_t words;

    uint32_t quarters;
    uint32_t out_w;
    uint32_t out_h;

    if (camera == NULL || out_pixels == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (camera->device == NULL) {
        return SCHULTZ_ERR_EXHAUSTED;
    }
    picture = SDL_AcquireCameraFrame(camera->device, &when);
    if (picture == NULL) {
        return SCHULTZ_ERR_EXHAUSTED;   /* nothing new, which is usual */
    }
    /*
     * The newest picture, not the oldest, which is what one call gives.
     *
     * SDL holds eight of these and hands them over in the order they arrived.
     * That is right for something recording every frame and wrong for
     * something showing one: a program that cannot keep up with the camera
     * fills the eight, and from then on SDL says so itself --
     *
     *     // uhoh, no output frames available! ... Drop this new frame.
     *
     * -- so every new picture is thrown away while the eight stale ones are
     * still queued. Taking one of those per turn leaves the preview a
     * quarter of a second behind the room and keeps it there, because the
     * only pictures ever taken are the old ones. It reads as lag rather than
     * as a low count, which is what makes it hard to place.
     *
     * So the queue is emptied instead of sipped, and all but the last go
     * back. A program that can only manage half the camera's rate then shows
     * the newest half rather than the oldest, and is never behind by more
     * than the turn it is on.
     */
    for (;;) {
        uint64_t newer_when = 0u;
        SDL_Surface *newer = SDL_AcquireCameraFrame(camera->device,
                                                    &newer_when);

        if (newer == NULL) {
            break;
        }
        SDL_ReleaseCameraFrame(camera->device, picture);
        picture = newer;
        when    = newer_when;
        camera->skipped++;
    }
    if (picture->w <= 0 || picture->h <= 0 ||
        picture->format != SDL_PIXELFORMAT_ARGB8888) {
        SDL_ReleaseCameraFrame(camera->device, picture);
        return SCHULTZ_ERR_EXHAUSTED;   /* not a picture this can use */
    }

    /*
     * How far the frame is out, which the camera measured and did not act on.
     *
     * A phone camera is fitted one way round and the phone is held another,
     * so the sensor's idea of up is rarely the person's. SDL reports the
     * difference on every frame and leaves the turn to whoever draws it; read
     * nothing and the preview is sideways, which is what it was.
     */
    quarters = schultz_camera_quarters(
        SDL_GetFloatProperty(SDL_GetSurfaceProperties(picture),
                             SDL_PROP_SURFACE_ROTATION_FLOAT, 0.0f));
    /* A quarter turn either way trades the sides for the ends. */
    if ((quarters & 1u) != 0u) {
        out_w = (uint32_t)picture->h;
        out_h = (uint32_t)picture->w;
    } else {
        out_w = (uint32_t)picture->w;
        out_h = (uint32_t)picture->h;
    }

    words = (size_t)picture->w * (size_t)picture->h;
    if (words > camera->words) {
        uint32_t *grown = (uint32_t *)realloc(camera->argb,
                                              words * sizeof(*grown));

        if (grown == NULL) {
            SDL_ReleaseCameraFrame(camera->device, picture);
            return SCHULTZ_ERR_OUT_OF_MEMORY;
        }
        camera->argb  = grown;
        camera->words = words;
    }
    /*
     * Row by row, because a surface's rows are padded to its pitch and the
     * interface promises pixels with nothing between the rows. The turn rides
     * along with that copy rather than costing a second pass.
     */
    schultz_camera_upright(picture->pixels, (uint32_t)picture->w,
                           (uint32_t)picture->h, (uint32_t)picture->pitch,
                           quarters, camera->argb);
    camera->width  = out_w;
    camera->height = out_h;
    camera->taken++;
    SDL_ReleaseCameraFrame(camera->device, picture);

    *out_pixels = camera->argb;
    if (out_width != NULL)   { *out_width = camera->width; }
    if (out_height != NULL)  { *out_height = camera->height; }
    if (out_when_ns != NULL) { *out_when_ns = when; }
    return SCHULTZ_OK;
}

/* --------------------------------------------------------- the preview node */

/*
 * The node is the video node's smaller cousin: it holds one image handle,
 * replaces it when a new picture arrives, and draws it fitted inside its
 * bounds. What it does not need is any of the decoding, timing or buffering
 * that a film needs, because a camera has no timeline. There is the picture
 * it is showing now and the one that arrives next.
 */
struct schultz_camera_preview_data {
    schultz_camera      *camera;  /**< Not owned; the host's. */
    schultz_image_table *images;  /**< Not owned; the tree's. */
    schultz_handle       frame;   /**< The picture showing, or none. */
    uint32_t             width;
    uint32_t             height;
    /* The height this node last asked to be, so that a height the host set
     * can be told apart from one of ours and left alone. */
    float                asked_height;
    uint64_t             shown;
    /** The next preview showing the same camera; see schultz_camera.viewers. */
    schultz_camera_preview_data *next_viewer;
};

static const schultz_widget_vtable schultz_camera_preview_widget;

static schultz_camera_preview_data *schultz_camera_preview_of(
    const schultz_tree *tree, schultz_handle node)
{
    if (schultz_node_widget(tree, node) != &schultz_camera_preview_widget) {
        return NULL;
    }
    return (schultz_camera_preview_data *)
        schultz_node_widget_data(tree, node);
}

/*
 * Keeps the node's shape in step with the pictures arriving.
 *
 * A widget in this toolkit cannot tell layout what size it wants: the vtable
 * has no measure, and a leaf takes its preferred size hints and nothing else.
 * So the preview has no way to say "I am sixteen by nine" except by setting
 * that hint itself, which is what this does.
 *
 * It matters because a camera's shape is not the host's to know. A phone
 * turns its pictures upright, so a stream that is sixteen by nine on the wire
 * arrives nine by sixteen, and a host that guessed a landscape box gets a
 * tall picture fitted inside a wide one: correct, centred, and a third of the
 * width it could have been. The host cannot fix that in advance because the
 * shape depends on which way the phone is held.
 *
 * The width the host asked for is kept, because that is a layout decision and
 * this is not entitled to it. Only the height is worked out, from that width
 * and the picture's proportions. A host that asked for no width at all gets
 * the picture's own size, which is the only shape there is to offer.
 *
 * A host that pins the height as well, or caps it with a maximum size, still
 * wins: this sets a preference and a maximum outranks it. That is the right
 * way round, but it is also the reason a preview that is still letterboxed
 * after this is a host holding its height down.
 */
static void schultz_camera_preview_shape(schultz_tree *tree,
                                         schultz_handle node,
                                         schultz_camera_preview_data *view)
{
    schultz_rect bounds;
    float want_w = SCHULTZ_SIZE_UNSET;
    float want_h = SCHULTZ_SIZE_UNSET;
    float height = SCHULTZ_SIZE_UNSET;

    if (view->width == 0u || view->height == 0u) {
        return;
    }
    /*
     * The width the node was actually given, not the width it asked for.
     *
     * Those are different, and the difference is the whole of this. A column
     * stretches a child across itself and pays no attention to the width it
     * requested, so a preview that worked its height out from what it asked
     * for is computing against a number the node will never have: ask for the
     * picture's own 3024 and a column hands back its own 354 and a height of
     * four thousand, which is a node you scroll to the end of to find.
     *
     * Reading it back closes the loop instead. The first turn may find
     * nothing, because nothing has been laid out; the turn after has the real
     * width and the shape settles there.
     */
    if (schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK ||
        bounds.width <= 0.0f) {
        return;
    }
    if (schultz_node_get_pref_size(tree, node, &want_w, &want_h)
            != SCHULTZ_OK) {
        return;
    }
    /*
     * A height the host chose is the host's. Ours is recognised by being the
     * one we last wrote, which is how this tells "nobody has said" from "the
     * answer is 240 and I meant it".
     */
    if (want_h > 0.0f && want_h != view->asked_height) {
        return;
    }
    schultz_camera_preview_size(bounds.width, view->width, view->height,
                                &bounds.width, &height);
    view->asked_height = height;
    schultz_node_set_pref_size(tree, node, want_w, height);
}

void schultz_camera_preview_size(float want_width, uint32_t frame_w,
                                 uint32_t frame_h, float *out_width,
                                 float *out_height)
{
    if (out_width == NULL || out_height == NULL ||
        frame_w == 0u || frame_h == 0u) {
        return;
    }
    /* No width asked for leaves only the picture's own to go on. */
    if (want_width <= 0.0f) {
        want_width = (float)frame_w;
    }
    *out_width  = want_width;
    *out_height = want_width * (float)frame_h / (float)frame_w;
}

/*
 * Takes whatever the camera has, once a turn.
 *
 * Nothing is asked of the camera before permission is given, which is the
 * whole of the waiting a host would otherwise have to write.
 */
/*
 * Says that this preview is showing that camera, and that it no longer shows
 * whichever it showed before.
 *
 * Both directions matter. A camera closing has to find the previews holding
 * it, and a preview going away has to be taken off the camera's list before
 * its own memory is freed, or closing the camera later would walk into it.
 * One function does both, and every place a preview's camera changes goes
 * through here.
 */
static void schultz_camera_drop_viewers(schultz_camera *camera)
{
    while (camera->viewers != NULL) {
        schultz_camera_preview_data *view = camera->viewers;

        camera->viewers   = view->next_viewer;
        view->camera      = NULL;
        view->next_viewer = NULL;
    }
}

static void schultz_camera_watch(schultz_camera_preview_data *view,
                                 schultz_camera *camera)
{
    if (view->camera == camera) {
        return;
    }
    if (view->camera != NULL) {
        schultz_camera_preview_data **link = &view->camera->viewers;

        while (*link != NULL) {
            if (*link == view) {
                *link = view->next_viewer;
                break;
            }
            link = &(*link)->next_viewer;
        }
    }
    view->next_viewer = NULL;
    view->camera      = camera;
    if (camera != NULL) {
        view->next_viewer = camera->viewers;
        camera->viewers   = view;
    }
}

static int32_t schultz_camera_preview_tick(schultz_tree *tree,
                                           schultz_handle node,
                                           uint64_t now_ms,
                                           uint32_t elapsed_ms)
{
    schultz_camera_preview_data *view = schultz_camera_preview_of(tree, node);
    const uint32_t *pixels = NULL;
    uint32_t width = 0u;
    uint32_t height = 0u;
    schultz_handle fresh = SCHULTZ_HANDLE_NONE;

    (void)now_ms;
    (void)elapsed_ms;
    if (view == NULL || view->camera == NULL || view->images == NULL) {
        return 0;
    }
    if (schultz_camera_permission(view->camera) != SCHULTZ_CAMERA_ALLOWED) {
        return 0;
    }
    if (schultz_camera_frame(view->camera, &pixels, &width, &height, NULL)
            != SCHULTZ_OK) {
        return 0;               /* nothing new this turn, which is usual */
    }
    if (schultz_image_set_pixels(view->images, pixels, width, height, 0u,
                                 &fresh) != SCHULTZ_OK) {
        return 0;
    }
    /*
     * The one before goes now. A camera is thirty of these a second and the
     * table would otherwise grow a handle a frame for as long as it is open.
     */
    if (view->frame != SCHULTZ_HANDLE_NONE) {
        schultz_image_unload(view->images, view->frame);
    }
    view->frame  = fresh;
    if (width != view->width || height != view->height) {
        view->width  = width;
        view->height = height;
        /* Only when the shape changes, because setting a size hint costs a
         * layout pass and a camera hands over thirty pictures a second. */
        schultz_camera_preview_shape(tree, node, view);
    }
    view->shown++;
    return schultz_node_invalidate(tree, node);
}

/*
 * The picture, fitted inside the node and centred, keeping its proportions.
 * The same rule the video node uses, for the same reason: a stretched picture
 * looks like a bug and bars look like a choice.
 */
static int32_t schultz_camera_preview_paint(schultz_tree *tree,
                                            schultz_handle node,
                                            schultz_draw_list *list,
                                            schultz_arena *arena)
{
    const schultz_camera_preview_data *view =
        schultz_camera_preview_of(tree, node);
    schultz_rect bounds;
    schultz_rect into;
    float scale;

    (void)arena;
    if (view == NULL ||
        schultz_node_absolute_bounds(tree, node, &bounds) != SCHULTZ_OK) {
        return SCHULTZ_OK;
    }
    if (view->frame == SCHULTZ_HANDLE_NONE || view->width == 0u ||
        view->height == 0u) {
        return SCHULTZ_OK;      /* nothing yet: draw nothing, not a hole */
    }

    scale = bounds.width / (float)view->width;
    if ((float)view->height * scale > bounds.height) {
        scale = bounds.height / (float)view->height;
    }
    into = schultz_rect_make(
        bounds.x + (bounds.width - (float)view->width * scale) * 0.5f,
        bounds.y + (bounds.height - (float)view->height * scale) * 0.5f,
        (float)view->width * scale, (float)view->height * scale);

    return schultz_draw_image(list, view->frame,
                              schultz_rect_make(0.0f, 0.0f, 0.0f, 0.0f), into,
                              255u);
}

static void schultz_camera_preview_destroy(void *data)
{
    schultz_camera_preview_data *view = (schultz_camera_preview_data *)data;

    if (view == NULL) {
        return;
    }
    if (view->images != NULL && view->frame != SCHULTZ_HANDLE_NONE) {
        schultz_image_unload(view->images, view->frame);
    }
    /* Off the camera's list before this memory goes, or closing it later
     * would walk through a freed link. */
    schultz_camera_watch(view, NULL);
    free(view);
}

static const schultz_widget_vtable schultz_camera_preview_widget = {
    .paint = schultz_camera_preview_paint,
    .tick = schultz_camera_preview_tick,
    .destroy = schultz_camera_preview_destroy
};

int32_t schultz_camera_preview_create(schultz_tree *tree,
                                      schultz_handle parent,
                                      schultz_camera *camera,
                                      schultz_handle *out_node)
{
    schultz_handle node = SCHULTZ_HANDLE_NONE;
    schultz_camera_preview_data *view;
    int32_t result;

    if (out_node == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    result = schultz_node_create(tree, parent, &node);
    if (result != SCHULTZ_OK) {
        return result;
    }
    view = (schultz_camera_preview_data *)calloc(1, sizeof(*view));
    if (view == NULL) {
        schultz_node_destroy(tree, node);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    view->frame  = SCHULTZ_HANDLE_NONE;
    view->images = (schultz_image_table *)schultz_tree_image_table(tree);
    schultz_camera_watch(view, camera);

    schultz_node_set_widget(tree, node, &schultz_camera_preview_widget, view);
    schultz_node_set_role(tree, node, SCHULTZ_ROLE_IMAGE);
    /*
     * Ticked only while it has a camera. There is no play to press -- a
     * camera that is open is running -- so pointing the node at one is what
     * starts it and pointing it at nothing is what stops it.
     */
    schultz_node_set_animating(tree, node, camera != NULL);
    *out_node = node;
    return SCHULTZ_OK;
}

int32_t schultz_camera_preview_set_camera(schultz_tree *tree,
                                          schultz_handle node,
                                          schultz_camera *camera)
{
    schultz_camera_preview_data *view = schultz_camera_preview_of(tree, node);

    if (view == NULL) {
        return SCHULTZ_ERR_INVALID_HANDLE;
    }
    schultz_camera_watch(view, camera);
    return schultz_node_set_animating(tree, node, camera != NULL);
}
