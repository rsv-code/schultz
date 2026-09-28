/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_camera.h
 * @brief The camera: finding one, opening it, and showing what it sees.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 *
 * A camera is the other direction from schultz_video.h. That plays a film
 * somebody else made; this one takes pictures as they happen.
 *
 * Three things are worth knowing before reading the calls.
 *
 * **A camera is hardware, and may not be there.** Listing on a machine with
 * none succeeds and reports none. That is not an error and a host should not
 * treat it as one: the right answer is to say so and carry on.
 *
 * **Opening one is not the same as being allowed to use it.** On phones, and
 * increasingly on desktops, the operating system asks the person first, and
 * the answer can take seconds or minutes. So opening always succeeds or fails
 * immediately on whether the device exists, and permission is a separate
 * question asked with schultz_camera_permission until it stops saying
 * waiting. Until it says allowed, no pictures arrive.
 *
 * **The pictures come back ready to draw.** A camera's own format is usually
 * Motion JPEG or one of the packed YUV layouts, and none of them is what a
 * screen wants. That conversion happens below this interface, so a frame is
 * always premultiplied ARGB at the size that was asked for.
 *
 * The short way to show one on screen is the preview node at the bottom of
 * this header, which does all of the above on the host's behalf.
 */

#ifndef SCHULTZ_CAMERA_H
#define SCHULTZ_CAMERA_H

#include <stdint.h>

#include "schultz.h"
#include "schultz_node.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif

/** @brief One open camera. */
typedef struct schultz_camera schultz_camera;

/** @brief Whether the person has said yes to a camera being used. */
enum {
    /** Nobody has answered yet. No pictures arrive while this is the answer. */
    SCHULTZ_CAMERA_WAITING = 0,
    /** Allowed. Pictures arrive. */
    SCHULTZ_CAMERA_ALLOWED,
    /** Refused. No pictures will ever arrive from this one. */
    SCHULTZ_CAMERA_REFUSED
};

/** @brief Which way a camera points, for a device that has more than one. */
enum {
    SCHULTZ_CAMERA_FACING_UNKNOWN = 0, /**< It does not say. */
    SCHULTZ_CAMERA_FACING_PERSON,      /**< At whoever is holding it. */
    SCHULTZ_CAMERA_FACING_AWAY         /**< At whatever it is pointed at. */
};

/**
 * @brief Counts the cameras this machine has.
 *
 * Zero is an ordinary answer, not a failure. A desktop with no webcam, a
 * machine whose camera is switched off in firmware, and a phone with the
 * camera disabled by policy all report none.
 *
 * @return How many cameras there are.
 */
uint32_t schultz_camera_count(void);

/**
 * @brief Returns the identifier of the camera at a position in the list.
 *
 * The identifier is what everything else here takes. Positions are not
 * stable across a device being plugged in or unplugged; identifiers are, for
 * as long as the device is present.
 *
 * @param index From zero to schultz_camera_count minus one.
 * @return The identifier, or zero when there is no camera there.
 */
uint64_t schultz_camera_device(uint32_t index);

/**
 * @brief Copies a camera's name, the one a person would recognise.
 *
 * @param device   An identifier from schultz_camera_device.
 * @param out_name Receives the name, NUL terminated and truncated to fit.
 *                 Must not be NULL.
 * @param size     How many bytes out_name holds. Must be at least one.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_UNAVAILABLE when there is no such camera.
 */
int32_t schultz_camera_name(uint64_t device, char *out_name, uint64_t size);

/**
 * @brief Reports which way a camera points.
 *
 * @param device An identifier from schultz_camera_device.
 * @return One of the SCHULTZ_CAMERA_FACING_* values.
 */
uint32_t schultz_camera_facing(uint64_t device);

/**
 * @brief How many sizes a camera offers.
 *
 * @param device An identifier from schultz_camera_device.
 * @return The count, or zero when the camera will not say. A camera is
 *         allowed to answer nothing: on some platforms it tells nobody
 *         anything until it has been opened once.
 */
uint32_t schultz_camera_size_count(uint64_t device);

/**
 * @brief Reads one of the sizes a camera offers.
 *
 * **Ask before opening.** A camera converts anything it is given, so opening
 * at a size it does not have works and is the wrong thing to do twice over:
 * the conversion is done in software on every frame, and a phone sensor's own
 * size is enormous. A twelve megapixel frame is forty eight megabytes of
 * pixels, which is measured in seconds per picture rather than pictures per
 * second. Every phone offers something small natively; this is how to find
 * it.
 *
 * @param device     An identifier from schultz_camera_device.
 * @param index      Which one, from zero to schultz_camera_size_count - 1.
 * @param out_width  Receives the width. May be NULL.
 * @param out_height Receives the height. May be NULL.
 * @param out_rate   Receives pictures per second, rounded, or zero where the
 *                   camera did not say. May be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when there is no such
 *         camera or no such index.
 */
int32_t schultz_camera_size_at(uint64_t device, uint32_t index,
                               uint32_t *out_width, uint32_t *out_height,
                               uint32_t *out_rate);

/**
 * @brief Opens a camera.
 *
 * The size is what the pictures are wanted at. **Choose one the camera
 * offers**, using schultz_camera_size_count and schultz_camera_size_at. A
 * camera that cannot produce the size asked for converts every frame in
 * software instead, which costs more than the drawing does and has been seen
 * to produce a torn picture. Passing zero for both takes the camera's own
 * size, which on a phone is the whole sensor and far larger than anything
 * worth previewing.
 *
 * Opening does not mean the camera is usable yet. See
 * schultz_camera_permission.
 *
 * **Name a rate, or let this pick the fastest.** Passing zero here does not
 * mean "no preference" to the layer underneath: it reads an unset rate as
 * zero and then chooses the rate closest to it, which is the *slowest* the
 * camera offers. A phone lists the bottom of every range as well as the top,
 * so that is often one picture a second, and the session is then pinned to
 * it. This call therefore never leaves the rate unset: zero here means the
 * fastest offered at the size chosen, and any other number is matched to the
 * nearest the camera has.
 *
 * @param device     An identifier from schultz_camera_device.
 * @param width      The width wanted, or zero for the camera's own.
 * @param height     The height wanted, or zero for the camera's own.
 * @param rate       Pictures per second wanted, or zero for the fastest the
 *                   camera offers at that size.
 * @param out_camera Receives the open camera. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT,
 *         SCHULTZ_ERR_OUT_OF_MEMORY, or SCHULTZ_ERR_UNAVAILABLE when there is
 *         no such camera or the machine will not open it.
 */
int32_t schultz_camera_open(uint64_t device, uint32_t width, uint32_t height,
                            uint32_t rate, schultz_camera **out_camera);

/**
 * @brief How many pictures a second the camera settled on.
 *
 * What was actually negotiated, which is not always what was asked for. Zero
 * until the camera has been allowed and has chosen, so a program that wants
 * to show it asks again rather than once.
 *
 * @param camera An open camera. NULL yields 0.
 * @return Pictures per second, rounded, or zero when it is not settled yet.
 */
uint32_t schultz_camera_rate(const schultz_camera *camera);

/**
 * @brief Closes a camera and releases it.
 *
 * Safe on NULL. The pixels from the last schultz_camera_frame belong to the
 * camera and are gone after this.
 *
 * **On the thread that runs the tree**, this is safe to call while a preview
 * is still showing the camera, and in any order with destroying that preview.
 * A camera knows which previews are pointed at it and tells them when it
 * goes, so a stop button needs no particular sequence. The preview keeps
 * whatever it last drew on the screen and simply stops asking for more; point
 * it somewhere else, or hide it, to clear that.
 *
 * **From any other thread it is not safe, and the failure is quiet.** A
 * preview reads its camera on every turn, and there is no lock between the
 * two: closing from a worker while the tree is being advanced can free the
 * camera between the turn's check and its use. What happens then is the
 * platform's business -- on a desktop it faults, and on a phone it can block
 * instead, inside a lock belonging to memory that has been handed back, which
 * shows as an interface that never draws again rather than as a crash.
 *
 * Close it from the thread that runs the window, like everything else that
 * touches the tree. If the decision to stop is made elsewhere, carry it over
 * as a message and act on it there.
 *
 * @param camera The camera to close.
 */
void schultz_camera_close(schultz_camera *camera);

/**
 * @brief Asks whether the person has allowed this camera to be used.
 *
 * Ask each turn of the loop until it stops saying waiting. There is nothing
 * to do in the meantime and nothing a host can do to hurry it: the answer is
 * the operating system's and the person's.
 *
 * @param camera An open camera. NULL yields SCHULTZ_CAMERA_REFUSED.
 * @return One of the SCHULTZ_CAMERA_* permission values.
 */
uint32_t schultz_camera_permission(const schultz_camera *camera);

/**
 * @brief Returns the size the pictures actually come at.
 *
 * Known once permission has been given, and not always the size that was
 * asked for. Both are zero before then.
 *
 * @param camera     An open camera.
 * @param out_width  Receives the width. Must not be NULL.
 * @param out_height Receives the height. Must not be NULL.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_camera_size(const schultz_camera *camera, uint32_t *out_width,
                            uint32_t *out_height);

/**
 * @brief Takes the newest picture, if one has arrived.
 *
 * Premultiplied ARGB, one word a pixel, width by height with no padding
 * between rows. Whatever the camera's own format was, and whatever had to be
 * done to it, is already done.
 *
 * The pixels belong to the camera and stay valid until the next call to this
 * for the same camera, or until it is closed. A host that needs to keep one
 * copies it.
 *
 * Returning SCHULTZ_ERR_EXHAUSTED means no new picture since the last call,
 * which is the ordinary answer most times it is asked: a camera at thirty a
 * second has nothing new on most turns of a loop running at sixty.
 *
 * @param camera     An open camera. Must not be NULL.
 * @param out_pixels Receives the pixels. Must not be NULL.
 * @param out_width  Receives the width. May be NULL.
 * @param out_height Receives the height. May be NULL.
 * @param out_when_ns Receives when the picture was taken, in nanoseconds on
 *                    the platform's own clock. May be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_EXHAUSTED
 *         when there is nothing new, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_camera_frame(schultz_camera *camera,
                             const uint32_t **out_pixels, uint32_t *out_width,
                             uint32_t *out_height, uint64_t *out_when_ns);

/**
 * @brief Counts the pictures taken since the camera was opened.
 *
 * @param camera An open camera. NULL yields zero.
 * @return How many pictures have been handed over.
 */
uint64_t schultz_camera_frames_taken(const schultz_camera *camera);

/**
 * @brief Counts the pictures passed over to get to a newer one.
 *
 * A camera goes on producing whether or not anything is looking, and what
 * matters to a preview is the newest picture rather than every picture. So
 * asking for one takes whatever has piled up and answers with the last of
 * them; the ones in front are counted here.
 *
 * Which makes this the answer to "is this program keeping up?". Zero, or
 * rising slowly, means it is. Rising as fast as the camera's own rate means
 * every second picture is being passed over, and so on. It is the number to
 * look at when a preview feels late rather than slow.
 *
 * @param camera An open camera. NULL yields zero.
 * @return How many pictures have been passed over.
 */
uint64_t schultz_camera_frames_skipped(const schultz_camera *camera);

/**
 * @brief Creates a node that shows what a camera sees.
 *
 * The node takes the size the layout gives it and draws the picture to fit,
 * keeping the picture's own proportions, exactly as a video node does. It is
 * an ordinary node: laid out, clipped and hit tested like any other.
 *
 * It does the waiting for permission on the host's behalf. Before the answer
 * comes it draws nothing; if the answer is no it goes on drawing nothing, and
 * schultz_camera_permission on the camera says which.
 *
 * A node with no camera asks for no clock at all, so a preview that is not
 * showing anything costs nothing.
 *
 * The camera is not owned by the node. It must outlive it, and closing it
 * while a node still points at it leaves the node showing its last picture.
 *
 * **The preview asks to be the shape of the pictures.** A camera's shape is
 * not something a host can know in advance: a phone turns its pictures
 * upright, so a stream that is sixteen by nine on the wire arrives nine by
 * sixteen, and which it is depends on how the phone is being held. So when
 * the shape changes the preview sets its own preferred height, from whatever
 * width the host asked for and the picture's proportions. The width is left
 * alone, because how wide to be is the host's decision.
 *
 * A host that also fixes the height, or caps it with a maximum size, still
 * wins: this is a preference and a maximum outranks it. That is also the
 * reason a preview still letterboxed after all this is a host holding its
 * height down.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param camera   The camera to show, or NULL for none yet.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_camera_preview_create(schultz_tree *tree,
                                      schultz_handle parent,
                                      schultz_camera *camera,
                                      schultz_handle *out_node);

/**
 * @brief Points a preview node at a different camera, or at none.
 *
 * @param tree   The tree the node is in. Must not be NULL.
 * @param node   A node from schultz_camera_preview_create.
 * @param camera The camera to show, or NULL to show nothing.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_camera_preview_set_camera(schultz_tree *tree,
                                          schultz_handle node,
                                          schultz_camera *camera);

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_CAMERA_H */
