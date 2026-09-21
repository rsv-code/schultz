#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Checks that a screen with something in the way of it is handled.
#
# Only a phone has a camera notch and a home indicator, and a phone is the one
# machine this project cannot be developed on. SCHULTZ_SAFE_INSET makes a
# desktop pretend it has them, through the same code the real ones take, so
# the behaviour can be asserted here rather than discovered on a device.
#
# What has to be true:
#
#   1. The reported size is what content may fill, not what the window is.
#   2. The root sits inside the safe area, so a shell built as its child does
#      too, without the host asking for anything.
#   3. Nothing is left unpainted. The bands are the window's own colour, so
#      the notch borders the same background as the rest of the screen rather
#      than a black bar.
#
# Usage:  sh scripts/check_safe_area.sh

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

TOP=140
BOTTOM=100

# The demo builds its shell to the reported size, so the screenshot it writes
# is that size. That is the assertion: with a notch it must be shorter.
"$APP" --frames 30 --no-vsync --light \
    --screenshot "$OUT/plain.ppm" >/dev/null 2>&1
SCHULTZ_SAFE_INSET="$TOP,$BOTTOM,0,0" "$APP" --frames 30 --no-vsync --light \
    --screenshot "$OUT/inset.ppm" >/dev/null 2>&1

# The width and height out of a PPM header.
#
# Only the header is read, never the pixels, so this stays in the shell: the
# first line is the magic, any number of comment lines may follow, and the
# line after those carries the two numbers. awk stops at that line, so the
# binary that follows it is never read at all.
ppm_size() {
    head -n 4 "$1" | awk '
        /^#/ { next }
        { if (!seen) { seen = 1; next }
          print $1, $2; exit }
    '
}

set -- $(ppm_size "$OUT/plain.ppm")
pw=$1 ph=$2
set -- $(ppm_size "$OUT/inset.ppm")
iw=$1 ih=$2

want_h=$(( ph - TOP - BOTTOM ))

echo "  no notch: ${pw}x${ph}"
echo "  notch:    ${iw}x${ih}, expected ${pw}x${want_h}"

if [ "$iw" -ne "$pw" ] || [ "$ih" -ne "$want_h" ]; then
    echo "FAIL: content was not kept out of the notch and the indicator."
    echo "      The reported size still describes the whole window, so a"
    echo "      host builds its shell over the parts it cannot use."
    exit 1
fi

echo "PASS: content is sized to the safe area."
