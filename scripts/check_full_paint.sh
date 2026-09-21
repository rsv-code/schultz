#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Checks that every pixel of the window gets painted.
#
# A buffer nobody wrote to holds whatever was in the memory, which reads as a
# black band around the picture. That is not a subtle failure, but it is easy
# to introduce and hard to see from here: the one machine this is developed on
# has nothing to scale, so a length that should have been multiplied and was
# not looks correct until it reaches a phone.
#
# The repaint region was exactly such a length. It is handed to the rasterizer
# to clip against, in the toolkit's units, and the rasterizer wants pixels. On
# a three times screen that named a rectangle a third the size, and two thirds
# of the window was never written to.
#
# Usage:  sh scripts/check_full_paint.sh

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP="$ROOT/build/schultz_demo"
[ -x "$APP" ] || { echo "build the demo first"; exit 1; }
# Drawn with no screen. The demo renders into its own buffer either way, so
# nothing here needs a desktop, and asking for one would put a window on
# whatever screen happens to be attached and skip the check where there is
# none. check_offscreen.sh is the one that proves this driver works.
export SDL_VIDEO_DRIVER=offscreen

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# The pixel check is a C program because it reads pixels, and a shell cannot
# hold a NUL byte in a variable. Compiled here into the same temporary
# directory as the screenshots and thrown away with them: it is a few lines
# of ISO C with no dependency on the toolkit, so there is nothing to keep.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
${CC:-cc} -std=c11 -O1 -o "$OUT/ppm_painted" "$ROOT/scripts/ppm_painted.c" || {
    echo "FAIL: could not compile scripts/ppm_painted.c" >&2
    exit 1
}

status=0
for scale in 1 2 3; do
    SCHULTZ_DUMP_BUFFER="$OUT/buf.ppm" SCHULTZ_PIXEL_SCALE=$scale \
        "$APP" --frames 5 --no-vsync --light >/dev/null 2>&1
    [ -f "$OUT/buf.ppm" ] || { echo "FAIL: no buffer written at ${scale}x" >&2
                               exit 1; }
    "$OUT/ppm_painted" "$OUT/buf.ppm" "$scale" || status=1
    rm -f "$OUT/buf.ppm"
done

[ "$status" = 0 ] && echo "PASS: the whole window is painted at every scale."
exit $status
