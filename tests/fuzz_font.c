/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * fuzz_font.c - hands the font machinery whatever bytes it is given.
 *
 * A font arrives with a document as readily as a picture does, and parsing
 * one reaches FreeType and, once anything is shaped with it, HarfBuzz. Both
 * have a long history of being handed fonts nobody meant them to read.
 *
 * Build with `make fuzz`, then:
 *
 *   afl-fuzz -i tests/corpus/font -o build-fuzz/out/font \
 *            -- build-fuzz/fuzz/fuzz_font
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>   /* AFL persistent mode reads the next case itself */

#include "schultz_arena.h"
#include "schultz_font.h"
#include "schultz_text.h"

#ifdef __AFL_HAVE_MANUAL_CONTROL
__AFL_FUZZ_INIT();
#endif

static void feed(const unsigned char *bytes, size_t length)
{
    schultz_font_system *system = NULL;
    schultz_handle font = SCHULTZ_HANDLE_NONE;

    if (length == 0u) {
        return;
    }
    if (schultz_font_system_create(&system) != SCHULTZ_OK) {
        return;
    }
    if (schultz_font_load_memory(system, bytes, length, 16.0f, &font)
            == SCHULTZ_OK) {
        schultz_font_metrics metrics;

        /*
         * Used, not merely loaded. A face that opened and then reports
         * nonsense is only interesting once something asks it questions.
         */
        schultz_arena arena;

        schultz_font_get_metrics(system, font, &metrics);
        schultz_font_is_fixed_pitch(system, font);
        schultz_font_has_glyph(system, font, 0x41u);
        schultz_font_is_bold(system, font);
        schultz_font_is_italic(system, font);

        /*
         * Shaped as well as opened, because that is where HarfBuzz reads the
         * tables the loader only skimmed: the character map, the kerning and
         * substitution tables, and the glyph outlines themselves. A face can
         * open cleanly and come apart the first time something asks it to
         * turn letters into glyphs. The string mixes scripts and a combining
         * mark so more than one path through shaping is taken.
         */
        if (schultz_arena_init(&arena, 64u * 1024u) == SCHULTZ_OK) {
            schultz_text_run run;

            if (schultz_text_shape(system, font, "Af1 \xd7\x90\xd7\x91 e\xcc\x81",
                                   -1, SCHULTZ_DIR_AUTO, &arena,
                                   &run) == SCHULTZ_OK) {
                (void)run;
            }
            schultz_arena_free(&arena);
        }
    }
    schultz_font_system_destroy(system);
}

/*
 * Two ways in, chosen by whether a file was named. See tests/fuzz_image.c for
 * why the choice is made here rather than at compile time.
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
