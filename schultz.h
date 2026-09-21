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


/**
 * @brief The version this header describes.
 *
 * Macros rather than an enum, because an enum constant is invisible to the
 * preprocessor: a caller writing
 *
 *     #if SCHULTZ_VERSION_MAJOR >= 1
 *
 * against an enum gets the name treated as zero and no warning about it,
 * which is the one thing a version number is for.
 *
 * These say what was **compiled** against. schultz_version says what is
 * **running**, and on a shared library the two need not agree.
 *
 * While the major number is zero nothing here is promised: the interface may
 * change in any release, and a caller should pin an exact version. What the
 * numbers will mean from 1.0 onward is in the project's README.
 */
#define SCHULTZ_VERSION_MAJOR  0
/**< Changes when something that existed is changed or taken away. */
#define SCHULTZ_VERSION_MINOR  1
/**< Changes when something is added and nothing existing moves. */
#define SCHULTZ_VERSION_PATCH  0
/**< Changes when only the implementation did. */

/** @brief What follows the numbers, such as "-alpha". Empty for a release. */
#define SCHULTZ_VERSION_LABEL "-alpha"

/**
 * @cond
 *
 * Turning a macro's value into a string takes two steps: the inner one
 * expands the argument, the outer one quotes what came out. One step alone
 * quotes the name. Not part of the interface; they exist for the line below.
 */
#define SCHULTZ_QUOTE_(x) #x
#define SCHULTZ_QUOTE(x)  SCHULTZ_QUOTE_(x)
/** @endcond */

/**
 * @brief The same version as text, with any pre-release label on the end.
 *
 * The numbers above are for comparing; this is for showing and for logging.
 * Built from those same three numbers rather than written out again, so a
 * release is one edit and the two cannot disagree. The label is the only
 * part written by hand, because "alpha" is not an integer.
 */
#define SCHULTZ_VERSION_STRING \
    SCHULTZ_QUOTE(SCHULTZ_VERSION_MAJOR) "." \
    SCHULTZ_QUOTE(SCHULTZ_VERSION_MINOR) "." \
    SCHULTZ_QUOTE(SCHULTZ_VERSION_PATCH) SCHULTZ_VERSION_LABEL

/**
 * @brief The version of the library that is actually loaded.
 *
 * Worth asking because it need not match SCHULTZ_VERSION_STRING. A program
 * compiles against one set of headers and, where Schultz is a shared library,
 * may be run against another build entirely. The header answers the first
 * question and this answers the second.
 *
 * @return A NUL terminated string owned by the library. Never NULL.
 */
const char *schultz_version(void);

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
