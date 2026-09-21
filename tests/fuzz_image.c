/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * fuzz_image.c - hands a picture decoder whatever bytes it is given.
 *
 * Not a test. A test says what a known input produces; this says nothing at
 * all about the answer and only cares whether getting one wrecks the process.
 * The fuzzer supplies the bytes, and AddressSanitizer turns a read or write
 * past the end of something into a crash at the moment it happens rather
 * than a wrong picture somewhere later.
 *
 * This reaches libpng, libwebp, libjpeg-turbo and ThorVG's own loaders, which
 * is most of where bytes chosen by somebody else enter this library.
 *
 * Build with `make fuzz`, then:
 *
 *   afl-fuzz -i tests/corpus/image -o build-fuzz/out/image \
 *            -- build-fuzz/fuzz/fuzz_image
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>   /* AFL persistent mode reads the next case itself */

#include "schultz_image.h"

/*
 * Persistent mode. Without it AFL starts a process per input, and starting
 * one costs more than decoding a small picture does. The macro is a no-op
 * when the binary was not built by an AFL compiler, so this still builds and
 * runs as an ordinary program.
 */
#ifdef __AFL_HAVE_MANUAL_CONTROL
__AFL_FUZZ_INIT();
#endif

/* Decodes one picture and lets go of it. */
static void feed(const unsigned char *bytes, size_t length)
{
    schultz_image_table *table = NULL;
    schultz_handle image = SCHULTZ_HANDLE_NONE;

    if (length == 0u || length > 0xFFFFFFFFu) {
        return;
    }
    if (schultz_image_table_create(&table) != SCHULTZ_OK) {
        return;
    }
    /*
     * No format hint, which is the interesting case: the loader has to work
     * out what it is from the bytes, so one input can reach every decoder
     * rather than only the one it was announced as.
     */
    if (schultz_image_load_data(table, bytes, (uint32_t)length, NULL,
                                &image) == SCHULTZ_OK) {
        schultz_size size = {0};

        /* Ask it something, so a picture that decoded into a bad size is
         * used rather than only made. */
        schultz_image_size(table, image, &size);
    }
    schultz_image_table_destroy(table);
}

/*
 * Two ways in, chosen by whether a file was named.
 *
 * No argument means the fuzzer is driving: AFL hands each input through
 * shared memory and the loop below runs thousands of them in one process,
 * which is worth doing because starting a process costs more than decoding a
 * small picture does. So afl-fuzz is given no @@ for these targets.
 *
 * A file argument means somebody is replaying one input, which is what a
 * saved crash needs. That has to work in the instrumented binary too, and
 * not only in a build without AFL: the crash is only guaranteed to happen
 * again in the binary that produced it.
 *
 * Getting this wrong is quiet rather than loud. With the choice made at
 * compile time instead, the instrumented binary ignored the filename and sat
 * waiting on standard input, which reads as a hang with no cause.
 */
int main(int argc, char **argv)
{
    FILE *in;
    unsigned char *bytes;
    long size;

    if (argc < 2) {
#ifdef __AFL_HAVE_MANUAL_CONTROL
        unsigned char *buffer;

        __AFL_INIT();
        buffer = __AFL_FUZZ_TESTCASE_BUF;
        while (__AFL_LOOP(10000)) {
            feed(buffer, (size_t)__AFL_FUZZ_TESTCASE_LEN);
        }
        return 0;
#else
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 2;
#endif
    }
    in = fopen(argv[1], "rb");
    if (in == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (size < 0) {
        fclose(in);
        return 2;
    }
    bytes = (unsigned char *)malloc((size_t)size + 1u);
    if (bytes == NULL) {
        fclose(in);
        return 2;
    }
    if (fread(bytes, 1u, (size_t)size, in) != (size_t)size) {
        free(bytes);
        fclose(in);
        return 2;
    }
    fclose(in);
    feed(bytes, (size_t)size);
    free(bytes);
    return 0;
}
