/*
 * What speexdsp's configure would have written, written by hand.
 *
 * This copy is compiled into the toolkit rather than built as a library, the
 * same way libunibreak and nestegg are, so there is no configure step to
 * produce this. Everything here is a choice rather than something detected:
 *
 *   FLOATING_POINT  Schultz has a floating point unit on every target it
 *                   builds for. The fixed point build exists for chips that
 *                   do not, and is slower and less accurate on chips that do.
 *   USE_KISS_FFT    The FFT that ships with speexdsp. The alternatives are
 *                   FFTW and Intel's MKL, which are separate dependencies
 *                   with separate licences, and neither is worth adding for
 *                   what this does.
 *   EXPORT          Nothing. These symbols do not leave the library: Schultz
 *                   exports schultz_* and nothing else.
 *
 * The three headers speexdsp asks about are all C99 and present everywhere.
 */
#ifndef SCHULTZ_SPEEXDSP_CONFIG_H
#define SCHULTZ_SPEEXDSP_CONFIG_H

#define FLOATING_POINT
#define USE_KISS_FFT
#define EXPORT

#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1

#endif /* SCHULTZ_SPEEXDSP_CONFIG_H */
