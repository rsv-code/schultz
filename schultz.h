/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz.h
 * @brief Public interface for the Schultz UI toolkit.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header.
 * This header defines the types that cross the host boundary. It follows
 * these ABI rules:
 *
 *   - fixed width integer types only, never "long"
 *   - no varargs, no function like macros, no static inline
 *   - structs cross by value only when they are small and hold nothing but
 *     fixed width scalars: a point, a rectangle, a colour, a paint. Anything
 *     holding a pointer crosses behind a pointer of its own.
 *
 * That last rule started out as "no struct by value at all". It was relaxed
 * deliberately, because forbidding it entirely would mean every rectangle
 * arrived as four arguments or behind a pointer, which makes the C interface
 * worse to read and to use for no gain: every foreign function interface
 * worth targeting passes small scalar structs by value correctly, and each
 * one that does not can wrap them on its own side.
 *
 * Result codes are declared as enum constants for readability but every
 * function returns int32_t, because an enum's underlying type is
 * implementation defined and would not have a stable size across an FFI
 * boundary.
 */

#ifndef SCHULTZ_H
#define SCHULTZ_H

#include <stdint.h>

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


/** @brief Library version, compiled in. */
enum {
    SCHULTZ_VERSION_MAJOR = 0,
    SCHULTZ_VERSION_MINOR = 1,
    SCHULTZ_VERSION_PATCH = 0
};

/**
 * @brief Opaque object identity.
 *
 * A handle packs a table index in the low 32 bits and a generation counter in
 * the high 32 bits. Because the generation of a live slot is never zero, a
 * valid handle is never zero, and a handle naming a destroyed object fails its
 * generation check instead of being dereferenced.
 *
 * Hosts see a plain 64 bit integer. Pointers never cross the boundary.
 */
typedef uint64_t schultz_handle;

enum {
    /** @brief The null handle. Never names a live object. */
    SCHULTZ_HANDLE_NONE = 0
};

/** @brief Result codes. Zero is success; every failure is negative. */
enum {
    /** Call succeeded. */
    SCHULTZ_OK                   =  0,
    /** Handle was zero, out of range, freed, or carried a stale generation. */
    SCHULTZ_ERR_INVALID_HANDLE   = -1,
    /** A pointer argument was NULL, or a value was outside its valid range. */
    SCHULTZ_ERR_INVALID_ARGUMENT = -2,
    /** An allocation failed. */
    SCHULTZ_ERR_OUT_OF_MEMORY    = -3,
    /** A fixed capacity was used up and cannot grow further. */
    SCHULTZ_ERR_EXHAUSTED        = -4,
    /**
     * A file or a block of bytes could not be read, or held nothing this
     * toolkit knows how to decode. The arguments were fine and nothing is
     * wrong with the program: the picture is missing, or truncated, or is not
     * a picture. Told apart from SCHULTZ_ERR_INVALID_HANDLE on purpose, so a
     * host can tell a bad image from a dead handle and say something useful
     * about the first.
     */
    SCHULTZ_ERR_UNREADABLE       = -5,
    /**
     * The machine cannot do this, and no argument would have changed that.
     * There is no sound card, no microphone, or the platform refused
     * permission to use one. Told apart from the codes above because there is
     * nothing for a host to fix and nothing to retry: the honest answer to a
     * person is that this machine will not do it, which is different from
     * saying a call was wrong.
     */
    SCHULTZ_ERR_UNAVAILABLE      = -6
};

/**
 * @brief Returns a stable, human readable name for a result code.
 *
 * Intended for logging and test output, not for display to end users.
 *
 * @param result A result code returned by any Schultz function.
 * @return A NUL terminated string owned by the library, which must not be
 *         freed. An unrecognized code yields "SCHULTZ_ERR_UNKNOWN" rather
 *         than NULL, so the return value is always safe to print.
 */
const char *schultz_result_string(int32_t result);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_H */
