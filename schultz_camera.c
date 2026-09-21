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

int32_t schultz_camera_open(uint64_t device, uint32_t width, uint32_t height,
                            schultz_camera **out_camera)
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

/* ------------------------------------------------------------- a picture */

int32_t schultz_camera_frame(schultz_camera *camera,
                             const uint32_t **out_pixels, uint32_t *out_width,
                             uint32_t *out_height, uint64_t *out_when_ns)
{
    SDL_Surface *picture;
    uint64_t when = 0u;
    size_t words;
    uint32_t row;

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
    if (picture->w <= 0 || picture->h <= 0 ||
        picture->format != SDL_PIXELFORMAT_ARGB8888) {
        SDL_ReleaseCameraFrame(camera->device, picture);
        return SCHULTZ_ERR_EXHAUSTED;   /* not a picture this can use */
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
     * interface promises pixels with nothing between the rows.
     */
    for (row = 0; row < (uint32_t)picture->h; row++) {
        const unsigned char *from = (const unsigned char *)picture->pixels +
                                    (size_t)row * (size_t)picture->pitch;

        memcpy(camera->argb + (size_t)row * (size_t)picture->w, from,
               (size_t)picture->w * sizeof(uint32_t));
    }
    camera->width  = (uint32_t)picture->w;
    camera->height = (uint32_t)picture->h;
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
typedef struct {
    schultz_camera      *camera;  /**< Not owned; the host's. */
    schultz_image_table *images;  /**< Not owned; the tree's. */
    schultz_handle       frame;   /**< The picture showing, or none. */
    uint32_t             width;
    uint32_t             height;
    uint64_t             shown;
} schultz_camera_preview_data;

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
 * Takes whatever the camera has, once a turn.
 *
 * Nothing is asked of the camera before permission is given, which is the
 * whole of the waiting a host would otherwise have to write.
 */
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
    view->width  = width;
    view->height = height;
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
    view->camera = camera;
    view->images = (schultz_image_table *)schultz_tree_image_table(tree);

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
    view->camera = camera;
    return schultz_node_set_animating(tree, node, camera != NULL);
}
