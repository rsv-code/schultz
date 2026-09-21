/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * ppm_painted.c - checks that a dumped window buffer was wholly painted, and
 * wholly handed to the screen.
 *
 * Used by scripts/check_full_paint.sh, which compiles it into its own
 * temporary directory and throws it away afterwards. It is C rather than
 * shell because it reads pixels: a shell cannot hold a NUL byte in a
 * variable, so it cannot look at the inside of a PPM at all.
 *
 *   ppm_painted <file.ppm> <scale>
 *
 * Exits 0 when every sampled pixel was written and the whole buffer was
 * uploaded, 1 otherwise, and 2 when the file could not be read.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One line of a PPM header.
 *
 * The header is text and the pixels are not, so this reads a byte at a time
 * and stops at the newline rather than reading ahead into the picture.
 */
static int header_line(FILE *in, char *out, size_t room)
{
    size_t used = 0u;
    int c;

    while ((c = fgetc(in)) != EOF) {
        if (c == '\n') {
            out[used] = '\0';
            return 1;
        }
        if (used + 1u < room) {
            out[used++] = (char)c;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    char line[256];
    const char *path;
    const char *scale;
    FILE *in;
    unsigned char *pixels;
    long width = 0;
    long height = 0;
    long count;
    long x;
    long y;
    long blank = 0;
    long total = 0;
    /* What the window handed to the screen, out of the dump's own comment. */
    long up[4] = { -1, -1, -1, -1 };
    int have_upload = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.ppm> <scale>\n", argv[0]);
        return 2;
    }
    path  = argv[1];
    scale = argv[2];

    in = fopen(path, "rb");
    if (in == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    if (!header_line(in, line, sizeof(line)) || strncmp(line, "P6", 2) != 0) {
        fprintf(stderr, "%s is not a P6 PPM\n", path);
        fclose(in);
        return 2;
    }
    /* Comment lines, one of which carries what was uploaded. */
    for (;;) {
        if (!header_line(in, line, sizeof(line))) {
            fprintf(stderr, "%s ended inside its header\n", path);
            fclose(in);
            return 2;
        }
        if (line[0] != '#') {
            break;
        }
        if (sscanf(line, "# upload %ld %ld %ld %ld",
                   &up[0], &up[1], &up[2], &up[3]) == 4) {
            have_upload = 1;
        }
    }
    if (sscanf(line, "%ld %ld", &width, &height) != 2 ||
        width <= 0 || height <= 0) {
        fprintf(stderr, "%s has no size\n", path);
        fclose(in);
        return 2;
    }
    if (!header_line(in, line, sizeof(line))) {   /* the maximum value */
        fclose(in);
        return 2;
    }

    /*
     * What reached the screen, which is not the same question as what was
     * drawn. A buffer painted correctly and an upload covering a third of it
     * look identical in the pixels; only the second is what anybody sees.
     */
    if (!have_upload) {
        printf("FAIL: the dump did not record what was uploaded.\n");
        fclose(in);
        return 1;
    }
    if (up[0] != 0 || up[1] != 0 || up[2] != width || up[3] != height) {
        printf("  %sx screen: buffer %ldx%ld, uploaded [%ld, %ld, %ld, %ld]\n",
               scale, width, height, up[0], up[1], up[2], up[3]);
        printf("FAIL: only part of the buffer was handed to the screen.\n");
        printf("      A length that should have been scaled to pixels was"
               " not,\n");
        printf("      so a correctly drawn window arrives mostly covered"
               " up.\n");
        fclose(in);
        return 1;
    }

    count = width * height * 3;
    pixels = (unsigned char *)malloc((size_t)count);
    if (pixels == NULL || fread(pixels, 1u, (size_t)count, in)
                              != (size_t)count) {
        fprintf(stderr, "%s is shorter than its header says\n", path);
        free(pixels);
        fclose(in);
        return 2;
    }
    fclose(in);

    /* Every fourth column, every row: enough to find a band, cheap enough
     * to run. */
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x += 4) {
            long at = (y * width + x) * 3;

            total++;
            if (pixels[at] == 0u && pixels[at + 1] == 0u &&
                pixels[at + 2] == 0u) {
                blank++;
            }
        }
    }
    free(pixels);

    printf("  %sx screen: buffer %ldx%ld, %ld.%ld%% never painted\n",
           scale, width, height,
           (total > 0) ? (blank * 100 / total) : 0L,
           (total > 0) ? ((blank * 1000 / total) % 10) : 0L);
    if (blank > 0) {
        printf("FAIL: part of the window was left as it was found.\n");
        printf("      A length that should have been scaled to pixels was"
               " not.\n");
        return 1;
    }
    return 0;
}
