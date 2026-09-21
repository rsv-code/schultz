/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_video.h
 * @brief Playing video: a node that shows frames fed to it.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 *
 * There are two ways to give a node something to play.
 *
 *   - **schultz_video_open_file** names a file and Schultz reads it. This is
 *     the one most hosts want, and it keeps a long film out of memory:
 *     nothing is held but the part being decoded.
 *   - **schultz_video_write** hands over bytes the host already has. It never
 *     opens a socket, so an HTTP response, a pipe and a file the host read
 *     itself are all the same thing here. That is the arrangement
 *     schultz_audio_stream_write uses.
 *
 * A file can be seeked in, because Schultz still has it. A stream cannot,
 * because the bytes are gone once they have been handed over.
 * schultz_video_can_seek says which kind a node holds.
 *
 * **Shutting down.** A node playing sound holds a stream that belongs to the
 * sound system, and gives it back when the node is destroyed. So a host tears
 * down in this order: the video nodes, then the sound system, then the
 * window. The window is last because destroying it ends SDL, and the sound
 * system cannot be taken down after that.
 *
 * WebM only, carrying VP8 or VP9. Those are the codecs that can be shipped
 * without a patent licence, which is why every other one was rejected.
 *
 * Sound comes with the picture when the file carries an Opus track and the
 * tree has been given a sound system with schultz_tree_set_audio. **The order
 * does not matter**: a node built before the sound system exists picks it up
 * when it starts, so a host may set up sound whenever it suits and need not
 * do it before building its widgets. The sound
 * is also the clock: pictures are shown against where the sound has actually
 * got to, so a decode that falls behind drops pictures rather than drifting
 * out of step. A film with no sound runs on the tree's clock instead.
 *
 * A node shows no controls at all until it is asked for some. See
 * schultz_video_set_controls.
 *
 * Encoding is here as well as playing: schultz_video_encoder_create takes
 * pictures and gives back packets, and schultz_camera.h is where those
 * pictures come from when they come from a camera.
 */

#ifndef SCHULTZ_VIDEO_H
#define SCHULTZ_VIDEO_H

#include <stdint.h>

#include "schultz.h"
#include "schultz_node.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif

/** @brief Which codec a video track carries. */
enum {
    SCHULTZ_VIDEO_CODEC_NONE = 0, /**< Nothing opened yet, or not video. */
    SCHULTZ_VIDEO_CODEC_VP8,      /**< VP8. */
    SCHULTZ_VIDEO_CODEC_VP9       /**< VP9. */
};

/**
 * @brief Which controls a video node shows.
 *
 * Any set of these. They appear as ordinary widgets in a row across the
 * bottom of the picture, built from the button, slider and label that already
 * exist, so a theme that restyles buttons restyles these.
 */
enum {
    /** No controls, which is the silent clip on a landing screen. */
    SCHULTZ_VIDEO_CONTROLS_NONE  = 0u,
    /** One button that plays, and pauses once it is playing. */
    SCHULTZ_VIDEO_CONTROL_PLAY     = 1u << 0,
    /** Stop, which returns to the beginning. */
    SCHULTZ_VIDEO_CONTROL_STOP     = 1u << 1,
    /** How far through, draggable when the source can be seeked in. */
    SCHULTZ_VIDEO_CONTROL_POSITION = 1u << 2,
    /** Elapsed and total, as text. */
    SCHULTZ_VIDEO_CONTROL_TIME     = 1u << 3,
    /** A button that silences the sound and gives it back. */
    SCHULTZ_VIDEO_CONTROL_MUTE     = 1u << 4,
    /** How loud, as a slider. */
    SCHULTZ_VIDEO_CONTROL_VOLUME   = 1u << 5,
    /** Every one of them, which is a player. */
    SCHULTZ_VIDEO_CONTROLS_ALL   = 0x3Fu
};

/**
 * @brief Creates a video node.
 *
 * It starts empty and shows nothing. Give it bytes with schultz_video_write
 * and start it with schultz_video_play.
 *
 * The node takes the size the layout gives it and draws the picture to fit,
 * keeping the picture's own proportions. It is an ordinary node: it is laid
 * out, clipped and hit tested like any other, and a caption can be drawn over
 * it.
 *
 * **`threads` says where decoding happens**, and is the same choice
 * `schultz_window_options.threads` offers for the rasterizer.
 *
 *   - **Zero** decodes on whichever thread advances the tree. Reproducible,
 *     and enough for a small picture: a frame of 480p costs a few
 *     milliseconds and a frame budget is sixteen.
 *   - **One or more** decodes ahead on that many threads of its own, created
 *     here and owned here. The first reads the file and drives the decoder;
 *     any beyond it are given to the decoder to split a picture between. A
 *     frame of 1080p costs more than a budget, so anything that size needs
 *     this.
 *
 * A thread only ever produces pictures. It does not touch the tree, the draw
 * list or any handle, so nothing else in the toolkit becomes concurrent.
 *
 * @param tree     The tree to create in. Must not be NULL.
 * @param parent   The parent node. Must name a live node.
 * @param threads  How many decode threads, or zero for none.
 * @param out_node Receives the new node. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_video_create(schultz_tree *tree, schultz_handle parent,
                             uint32_t threads, schultz_handle *out_node);

/**
 * @brief Names a file for the node to read and play.
 *
 * Schultz opens the file and reads from it as it decodes, so a long film
 * costs a buffer rather than its whole size in memory. The node takes over
 * the reading; whatever was written to it before is dropped.
 *
 * Opening does not start playing. Call schultz_video_play for that.
 *
 * A node given a file can seek. See schultz_video_seek.
 *
 * @param tree The tree the node is in. Must not be NULL.
 * @param node The video node.
 * @param path The file to play. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when path is NULL,
 *         SCHULTZ_ERR_INVALID_HANDLE when the node is not a video node, or
 *         SCHULTZ_ERR_UNREADABLE when the file cannot be opened.
 */
int32_t schultz_video_open_file(schultz_tree *tree, schultz_handle node,
                                const char *path);

/**
 * @brief Hands the node more of the file.
 *
 * Copied, so the memory is the host's again the moment this returns. Bytes
 * may arrive in any size at any rate; nothing is decoded here.
 *
 * Writing continues to work while the node is playing, which is what a stream
 * needs. A host with the whole file may write it in one call.
 *
 * **Everything written is kept** while the node lives, because opening a WebM
 * file means reading backwards through it. That is fine for a clip and wrong
 * for an hour of television, and it is one of the things phase two fixes.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   A node from schultz_video_create.
 * @param bytes  The bytes. Must not be NULL when length is not zero.
 * @param length How many.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_INVALID_HANDLE
 *         or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_video_write(schultz_tree *tree, schultz_handle node,
                            const void *bytes, uint64_t length);

/**
 * @brief Returns how many bytes have been written and not yet read.
 *
 * What it is for is deciding when to write more, so a host streaming from a
 * socket can keep a little ahead without reading the whole thing into memory.
 *
 * @param tree The tree holding the node. NULL yields zero.
 * @param node A node from schultz_video_create.
 * @return Bytes waiting.
 */
uint64_t schultz_video_queued(const schultz_tree *tree, schultz_handle node);

/**
 * @brief How much memory a pushed stream is holding.
 *
 * The companion to schultz_video_queued, which counts what has arrived and
 * not been read. This counts what is kept, which is a different and usually
 * much smaller number: the header, and everything from the oldest picture
 * still waiting to be decoded. A stream that plays for a week holds about a
 * second of video, not a week of it.
 *
 * A node reading a file holds nothing and answers zero.
 *
 * @param tree The tree holding the node. NULL yields zero.
 * @param node A node from schultz_video_create.
 * @return Bytes held.
 */
uint64_t schultz_video_held(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Starts playing, from where it stopped.
 *
 * A node with nothing written yet starts as soon as there is enough to open.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node from schultz_video_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_video_play(schultz_tree *tree, schultz_handle node);

/**
 * @brief Stops playing and holds the frame that is showing.
 *
 * @param tree The tree holding the node. Must not be NULL.
 * @param node A node from schultz_video_create.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_video_pause(schultz_tree *tree, schultz_handle node);

/**
 * @brief Stops playing and returns to the beginning.
 *
 * The difference from schultz_video_pause is where it leaves the film.
 * Pausing holds the picture where it is; stopping winds back, so the next
 * schultz_video_play starts from the top.
 *
 * A node that cannot seek stops where it is and plays on from there, because
 * a stream has no beginning to return to.
 *
 * @param tree The tree the node is in. Must not be NULL.
 * @param node The video node.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_video_stop(schultz_tree *tree, schultz_handle node);

/**
 * @brief Reports whether it is playing.
 *
 * @param tree The tree holding the node. NULL yields zero.
 * @param node A node from schultz_video_create.
 * @return Nonzero while playing.
 */
int32_t schultz_video_is_playing(const schultz_tree *tree,
                                 schultz_handle node);

/**
 * @brief Starts again from the beginning when the end is reached.
 *
 * Off by default, so a clip plays once and holds its last frame.
 *
 * @param tree    The tree holding the node. Must not be NULL.
 * @param node    A node from schultz_video_create.
 * @param looping Nonzero to loop.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_video_set_looping(schultz_tree *tree, schultz_handle node,
                                  int32_t looping);

/**
 * @brief Reports whether this node can be moved to another point in the film.
 *
 * True for a file, false for a stream. A host building its own controls asks
 * this to decide whether its position bar should accept a drag.
 *
 * @param tree The tree the node is in.
 * @param node The video node.
 * @return Nonzero when schultz_video_seek will work.
 */
int32_t schultz_video_can_seek(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Moves to a point in the film.
 *
 * **It moves to the keyframe at or before the point asked for**, not to the
 * point itself, because a keyframe is the last place a decoder can start.
 * How far back that is depends on how the film was encoded: a second is
 * common and several seconds happens. schultz_video_position afterwards says
 * where it actually went, and is the number to show somebody rather than the
 * one that was asked for.
 *
 * @param tree The tree the node is in. Must not be NULL.
 * @param node The video node.
 * @param at_ms How far into the film to move, in milliseconds.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_UNAVAILABLE when the node holds a stream rather than a
 *         file, or nothing has been opened yet.
 */
int32_t schultz_video_seek(schultz_tree *tree, schultz_handle node,
                           uint64_t at_ms);

/**
 * @brief Returns how far into the film the node has played, in milliseconds.
 *
 * Zero before anything is shown.
 *
 * @param tree The tree the node is in.
 * @param node The video node.
 * @return The position in milliseconds.
 */
uint64_t schultz_video_position(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Returns how long the film is, in milliseconds.
 *
 * Known once the file has been opened. A stream that has not said how long it
 * is answers zero, and so does one that is still arriving.
 *
 * @param tree The tree the node is in.
 * @param node The video node.
 * @return The length in milliseconds, or zero when it is not known.
 */
uint64_t schultz_video_duration(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Chooses which controls the node shows.
 *
 * The controls are child nodes, made here and destroyed here. Asking for a
 * different set rebuilds the row; asking for none removes it, and the node
 * goes back to being a picture and nothing else.
 *
 * The position bar shows progress whatever the source is, and accepts a drag
 * only when schultz_video_can_seek says the film can be moved about in.
 *
 * @param tree     The tree the node is in. Must not be NULL.
 * @param node     The video node.
 * @param controls Any set of the SCHULTZ_VIDEO_CONTROL_* flags, or
 *                 SCHULTZ_VIDEO_CONTROLS_NONE.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or
 *         SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_video_set_controls(schultz_tree *tree, schultz_handle node,
                                   uint32_t controls);

/**
 * @brief Returns which controls the node shows.
 *
 * @param tree The tree the node is in.
 * @param node The video node.
 * @return The flags last set, or SCHULTZ_VIDEO_CONTROLS_NONE.
 */
uint32_t schultz_video_controls(const schultz_tree *tree,
                                schultz_handle node);

/**
 * @brief Reports whether the film has a sound track this can play.
 *
 * True only once the file has been opened, it carries Opus, and the tree has
 * a sound system. Everything else answers no, which is the same thing as
 * saying the film will play silently.
 *
 * @param tree The tree the node is in.
 * @param node The video node.
 * @return Nonzero when there is sound.
 */
int32_t schultz_video_has_sound(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Sets how loud the film plays.
 *
 * @param tree   The tree the node is in. Must not be NULL.
 * @param node   The video node.
 * @param volume Zero for silence, one for as recorded. Clamped.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_video_set_volume(schultz_tree *tree, schultz_handle node,
                                 float volume);

/**
 * @brief Returns how loud the film plays.
 *
 * @param tree The tree the node is in.
 * @param node The video node.
 * @return The volume last set, or one.
 */
float schultz_video_volume(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Silences the film without forgetting how loud it was.
 *
 * Unmuting returns to the volume that was set, which is what a mute button
 * has to do and what setting the volume to zero cannot.
 *
 * @param tree  The tree the node is in. Must not be NULL.
 * @param node  The video node.
 * @param muted Nonzero to silence it.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_video_set_muted(schultz_tree *tree, schultz_handle node,
                                int32_t muted);

/**
 * @brief Reports whether the film is muted.
 *
 * @param tree The tree the node is in.
 * @param node The video node.
 * @return Nonzero while muted.
 */
int32_t schultz_video_is_muted(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Returns the size of the picture, once one is known.
 *
 * Both are zero until enough has been written to open the file, which is why
 * this is asked rather than assumed.
 *
 * @param tree       The tree holding the node. NULL yields an error.
 * @param node       A node from schultz_video_create.
 * @param out_width  Receives the width in pixels. Must not be NULL.
 * @param out_height Receives the height. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_video_size(const schultz_tree *tree, schultz_handle node,
                           uint32_t *out_width, uint32_t *out_height);

/**
 * @brief Returns which codec the opened track carries.
 *
 * @param tree The tree holding the node. NULL yields SCHULTZ_VIDEO_CODEC_NONE.
 * @param node A node from schultz_video_create.
 * @return One of the SCHULTZ_VIDEO_CODEC_* values.
 */
uint32_t schultz_video_codec(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Counts the frames shown since the node was created.
 *
 * Intended for tests and for a host that wants to know whether anything is
 * actually arriving, which a still picture cannot answer on its own.
 *
 * @param tree The tree holding the node. NULL yields zero.
 * @param node A node from schultz_video_create.
 * @return How many frames have been decoded and shown.
 */
uint64_t schultz_video_frames_shown(const schultz_tree *tree,
                                    schultz_handle node);

/* ---------------------------------------------------- the other direction */

/**
 * @brief An encoder: pictures in, compressed packets out.
 *
 * VP8 or VP9, the two codecs that can be shipped without a patent licence,
 * which is why every other one was rejected.
 *
 * **Which to ask for.** VP9 makes smaller packets for the same picture --
 * call it a third smaller at the same quality -- and costs considerably more
 * processor to do it. VP8 is the one to fall back to when that cost does not
 * fit, and it is what real-time video has spoken for years.
 */
typedef struct schultz_video_encoder schultz_video_encoder;

/**
 * @brief Creates an encoder.
 *
 * Nothing here sends anything anywhere. The packets come back to the caller,
 * which decides what they are for: a socket, a file, a recording. That is
 * the whole of the arrangement, and it is deliberate -- a transport is a
 * different problem from a codec and this toolkit solves only the second.
 *
 * The bitrate is a target, not a limit. The encoder spends more on a picture
 * that changed a lot and less on one that did not, and averages out to what
 * was asked for over a few seconds.
 *
 * Both are set up for real time: nothing is held back to look at the picture
 * after it, and the encoder is asked for its fastest useful setting. A call
 * cannot wait for the future, and lag is what people notice first.
 *
 * @param codec      SCHULTZ_VIDEO_CODEC_VP8 or SCHULTZ_VIDEO_CODEC_VP9.
 * @param width      The picture width. Must not be zero.
 * @param height     The picture height. Must not be zero.
 * @param per_second How many pictures a second are meant to arrive. Used for
 *                   rate control, not enforced. Must not be zero.
 * @param bitrate    The target, in bits a second. Must not be zero.
 * @param out_encoder Receives the encoder. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_OUT_OF_MEMORY
 *         or SCHULTZ_ERR_UNAVAILABLE when the encoder will not start.
 */
int32_t schultz_video_encoder_create(uint32_t codec, uint32_t width,
                                     uint32_t height, uint32_t per_second,
                                     uint32_t bitrate,
                                     schultz_video_encoder **out_encoder);

/**
 * @brief Returns which codec an encoder is producing.
 *
 * @param encoder The encoder. NULL yields SCHULTZ_VIDEO_CODEC_NONE.
 * @return One of the SCHULTZ_VIDEO_CODEC_* values.
 */
uint32_t schultz_video_encoder_codec(const schultz_video_encoder *encoder);

/**
 * @brief Closes an encoder. Safe on NULL.
 *
 * @param encoder The encoder to close.
 */
void schultz_video_encoder_destroy(schultz_video_encoder *encoder);

/**
 * @brief Hands the encoder one picture.
 *
 * Premultiplied ARGB, one word a pixel, width by height with no padding
 * between rows -- the same shape schultz_camera_frame hands back, so a camera
 * can be wired to an encoder without anything in between.
 *
 * Times must go forward. A picture whose time is not later than the one
 * before it is refused rather than quietly reordered, because a decoder given
 * such a stream has no way to play it.
 *
 * Encoding happens here, on the calling thread. What it produces is collected
 * with schultz_video_encoder_read_packet, which should be called until it
 * says there is nothing more: one picture can produce more than one packet.
 *
 * @param encoder The encoder. Must not be NULL.
 * @param argb    The picture. Must not be NULL.
 * @param width   Its width. Must match what the encoder was created with.
 * @param height  Its height. Must match what the encoder was created with.
 * @param when_ns When the picture was taken, in nanoseconds.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT when an argument is wrong
 *         or the time did not go forward, SCHULTZ_ERR_OUT_OF_MEMORY, or
 *         SCHULTZ_ERR_UNREADABLE when the encoder refused the picture.
 */
int32_t schultz_video_encoder_write_frame(schultz_video_encoder *encoder,
                                          const uint32_t *argb,
                                          uint32_t width, uint32_t height,
                                          uint64_t when_ns);

/**
 * @brief Takes the next packet the encoder has ready.
 *
 * Call it until it answers SCHULTZ_ERR_EXHAUSTED, which means there is
 * nothing more from the last picture.
 *
 * The bytes belong to the encoder and stay valid until the next call to this
 * or to schultz_video_encoder_write_frame. A caller that keeps one copies it.
 *
 * The keyframe flag is what a transport needs to know: a keyframe can be
 * decoded on its own, and everything else depends on the packets before it.
 * A receiver that lost something has to be sent one before it can show
 * anything again.
 *
 * @param encoder     The encoder. Must not be NULL.
 * @param out_bytes   Receives the packet. Must not be NULL.
 * @param out_length  Receives how many bytes. Must not be NULL.
 * @param out_when_ns Receives the time of the picture it came from. May be
 *                    NULL.
 * @param out_keyframe Receives nonzero when this packet is a keyframe. May be
 *                     NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or SCHULTZ_ERR_EXHAUSTED
 *         when there is nothing more.
 */
int32_t schultz_video_encoder_read_packet(schultz_video_encoder *encoder,
                                          const void **out_bytes,
                                          uint64_t *out_length,
                                          uint64_t *out_when_ns,
                                          int32_t *out_keyframe);

/**
 * @brief Changes the target bitrate, from the next picture onward.
 *
 * What congestion control is built on. A transport that sees the network
 * getting worse turns this down and the pictures get softer; it turns it up
 * again when there is room. Nothing here decides that: this is the handle,
 * and the decision belongs to whoever is watching the network.
 *
 * @param encoder The encoder. Must not be NULL.
 * @param bitrate The new target, in bits a second. Must not be zero.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_UNAVAILABLE when the encoder refused it.
 */
int32_t schultz_video_encoder_set_bitrate(schultz_video_encoder *encoder,
                                          uint32_t bitrate);

/**
 * @brief Makes the next picture a keyframe.
 *
 * What loss recovery is built on. A receiver that has lost packets cannot
 * show anything until a keyframe reaches it, and asking for one is the only
 * way to shorten that wait.
 *
 * It applies to the next picture written, once. Asking twice before writing
 * one is the same as asking once.
 *
 * @param encoder The encoder. Must not be NULL.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_video_encoder_force_keyframe(schultz_video_encoder *encoder);

/**
 * @brief Returns the target bitrate the encoder is working to.
 *
 * @param encoder The encoder. NULL yields zero.
 * @return The target, in bits a second.
 */
uint32_t schultz_video_encoder_bitrate(const schultz_video_encoder *encoder);

/** @brief The quickest an encoder can be asked to be. */
enum {
    SCHULTZ_VIDEO_SPEED_MOST = 10u
};

/**
 * @brief Sets how quickly the encoder works, against how good it looks.
 *
 * Zero is the slowest and the best picture; SCHULTZ_VIDEO_SPEED_MOST is the
 * quickest and the roughest. The default is eight, near the quick end,
 * because this is built for calls and lag is what people notice first.
 *
 * **It matters far more to VP9 than to VP8.** Measured on a build with no
 * SIMD, at 320x240: VP8 takes about six milliseconds a picture whatever it is
 * asked for, and VP9 ranges from 58 milliseconds at zero to under four at
 * eight and above. Turning this down is how a host trades the processor it
 * has for a better picture, and on VP9 there is a great deal to trade.
 *
 * Can be changed at any time; it applies from the next picture.
 *
 * @param encoder The encoder. Must not be NULL.
 * @param speed   Zero to SCHULTZ_VIDEO_SPEED_MOST. Above that is taken as
 *                the most.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_UNAVAILABLE when the encoder refused it.
 */
int32_t schultz_video_encoder_set_speed(schultz_video_encoder *encoder,
                                        uint32_t speed);

/**
 * @brief Returns how quickly the encoder has been asked to work.
 *
 * @param encoder The encoder. NULL yields zero.
 * @return Zero to SCHULTZ_VIDEO_SPEED_MOST.
 */
uint32_t schultz_video_encoder_speed(const schultz_video_encoder *encoder);

/**
 * @brief A decoder for the packets an encoder produces.
 *
 * The other half of the pair, and the one that makes the whole thing
 * testable: what came out of an encoder can be turned back into pictures
 * without a file, a container or a network in between.
 *
 * This is not how a film is played. A film arrives as WebM and is played by a
 * video node, which knows about tracks, timing and sound. This takes bare
 * packets, in the order they were produced, and gives back pictures -- which
 * is what a receiver on the far end of a transport has and nothing more.
 */
typedef struct schultz_video_decoder schultz_video_decoder;

/**
 * @brief Creates a decoder for bare packets.
 *
 * The codec has to be said, because a bare packet does not carry it. A file
 * would: that is part of what a container is for, and why the video node
 * never has to be told. A receiver learns it from whatever agreed the call.
 *
 * @param codec       SCHULTZ_VIDEO_CODEC_VP8 or SCHULTZ_VIDEO_CODEC_VP9.
 * @param out_decoder Receives the decoder. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_OUT_OF_MEMORY
 *         or SCHULTZ_ERR_UNAVAILABLE.
 */
int32_t schultz_video_decoder_create(uint32_t codec,
                                     schultz_video_decoder **out_decoder);

/**
 * @brief Closes a decoder. Safe on NULL.
 *
 * @param decoder The decoder to close.
 */
void schultz_video_decoder_destroy(schultz_video_decoder *decoder);

/**
 * @brief Hands the decoder one packet.
 *
 * Packets must arrive in the order they were produced. A decoder given the
 * first packet after a loss shows nothing until a keyframe reaches it, which
 * is what schultz_video_encoder_force_keyframe is for.
 *
 * @param decoder The decoder. Must not be NULL.
 * @param bytes   The packet. Must not be NULL.
 * @param length  How many bytes. Must not be zero.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_UNREADABLE when the packet could not be decoded.
 */
int32_t schultz_video_decoder_write_packet(schultz_video_decoder *decoder,
                                           const void *bytes,
                                           uint64_t length);

/**
 * @brief Takes the next picture the decoder has ready.
 *
 * Premultiplied ARGB, one word a pixel, width by height with no padding
 * between rows -- the same shape everything else here hands over.
 *
 * The pixels belong to the decoder and stay valid until the next call to this
 * or to schultz_video_decoder_write_packet.
 *
 * @param decoder    The decoder. Must not be NULL.
 * @param out_pixels Receives the picture. Must not be NULL.
 * @param out_width  Receives the width. May be NULL.
 * @param out_height Receives the height. May be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_EXHAUSTED
 *         when there is nothing more, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_video_decoder_read_frame(schultz_video_decoder *decoder,
                                         const uint32_t **out_pixels,
                                         uint32_t *out_width,
                                         uint32_t *out_height);

/* ------------------------------------------------------------ into a file */

/**
 * @brief Writes packets into a WebM file.
 *
 * The other end of schultz_video_open_file. That one reads a film; this one
 * makes one, out of the packets an encoder produces.
 *
 * It is the only piece that turns encoding into something a person can keep.
 * An encoder hands back bare packets, which are what a transport wants and
 * nothing a media player will open: a file needs a container, saying which
 * codecs are inside, how big the picture is, when each packet belongs, and
 * where to jump to when somebody drags a slider. That is what this writes.
 *
 * WebM, carrying VP8 or VP9 and Opus, which is the same shape this toolkit
 * already reads. What comes out opens in a browser.
 */
typedef struct schultz_video_writer schultz_video_writer;

/**
 * @brief Starts a WebM file.
 *
 * Nothing is written until the first packet, so a sound track can still be
 * added after this and before then.
 *
 * @param path       The file to write. Overwritten if it exists. Must not be
 *                   NULL.
 * @param codec      SCHULTZ_VIDEO_CODEC_VP8 or SCHULTZ_VIDEO_CODEC_VP9. It
 *                   must match what the encoder is producing: a container
 *                   that names the wrong codec plays as nothing.
 * @param width      The picture width. Must not be zero.
 * @param height     The picture height. Must not be zero.
 * @param out_writer Receives the writer. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_OUT_OF_MEMORY
 *         or SCHULTZ_ERR_UNREADABLE when the file will not open.
 */
int32_t schultz_video_writer_create(const char *path, uint32_t codec,
                                    uint32_t width, uint32_t height,
                                    schultz_video_writer **out_writer);

/**
 * @brief Adds an Opus sound track.
 *
 * Only before the first packet is written, because the tracks are described
 * at the top of the file and nothing can be added to that list afterwards.
 *
 * Forty eight thousand samples a second, which is what Opus works in and what
 * schultz_audio_encoder_create produces.
 *
 * @param writer   The writer. Must not be NULL.
 * @param channels One for mono, two for stereo.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, or
 *         SCHULTZ_ERR_UNAVAILABLE when something has already been written.
 */
int32_t schultz_video_writer_add_sound(schultz_video_writer *writer,
                                       uint32_t channels);

/**
 * @brief Writes one picture.
 *
 * Straight from schultz_video_encoder_read_packet: the bytes, the time it
 * carries, and whether it is a keyframe.
 *
 * The keyframe flag is not decoration. It is what a player jumps to when
 * somebody drags a slider, and this records where each one landed so that
 * seeking the finished file works. A file written with every packet marked
 * ordinary plays from the start and cannot be moved about in.
 *
 * Times must go forward.
 *
 * @param writer   The writer. Must not be NULL.
 * @param bytes    The packet. Must not be NULL.
 * @param length   How many bytes. Must not be zero.
 * @param when_ns  When it belongs, in nanoseconds from the start.
 * @param keyframe Nonzero when the packet can be decoded on its own.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_OUT_OF_MEMORY
 *         or SCHULTZ_ERR_UNREADABLE when the file would not take it.
 */
int32_t schultz_video_writer_write_picture(schultz_video_writer *writer,
                                           const void *bytes, uint64_t length,
                                           uint64_t when_ns,
                                           int32_t keyframe);

/**
 * @brief Writes one packet of sound.
 *
 * Straight from schultz_audio_encoder_read_packet. Needs a sound track, added
 * with schultz_video_writer_add_sound before anything was written.
 *
 * Every Opus packet stands on its own, so there is no keyframe to say.
 *
 * @param writer  The writer. Must not be NULL.
 * @param bytes   The packet. Must not be NULL.
 * @param length  How many bytes. Must not be zero.
 * @param when_ns When it belongs, in nanoseconds from the start.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT, SCHULTZ_ERR_OUT_OF_MEMORY
 *         or SCHULTZ_ERR_UNAVAILABLE when there is no sound track.
 */
int32_t schultz_video_writer_write_sound(schultz_video_writer *writer,
                                         const void *bytes, uint64_t length,
                                         uint64_t when_ns);

/**
 * @brief Finishes the file and closes it.
 *
 * This is where the parts that could only be known at the end are written:
 * how long the film is, and the index of where each keyframe landed. A writer
 * that is dropped without this leaves a file that plays but cannot be seeked
 * in and does not know its own length.
 *
 * The writer is released either way, so there is nothing to free afterwards
 * and calling it twice is not possible. Safe on NULL.
 *
 * @param writer The writer to finish.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_UNREADABLE when the file would not take
 *         the last of it.
 */
int32_t schultz_video_writer_close(schultz_video_writer *writer);

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_VIDEO_H */
