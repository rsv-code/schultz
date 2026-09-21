#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Checks that the toolkit uses the screen it is on.
#
# A dense screen packs several of its pixels into the space an ordinary
# monitor gives one. The buffer is that screen's own resolution either way;
# what changes is the size of a unit. This is the one machine this project is
# developed on and it has nothing to scale, so SCHULTZ_PIXEL_SCALE makes it
# claim otherwise, through the same code a phone takes.
#
# What has to be true, with the buffer the same size in every case:
#
#   1. Scaling to the screen, a unit covers as many pixels as the screen
#      packs, so there are proportionally fewer of them to build against.
#   2. Not scaling, a unit is a pixel and the count is the buffer's own.
#   3. A screen with nothing to scale is unaffected either way.
#
# Usage:  sh scripts/check_pixel_scale.sh

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

# The demo builds its shell to the size it is told, and writes a screenshot
# of it, so the screenshot's size is what the toolkit thought it had.
#
# Rounded outward, because that is what schultz_render_size does: a node
# 333 and a third units wide needs 334 pixels or the last third is cut off.
# The size a host builds against is the other way, truncated, since a
# fraction of a unit is not one to place anything in.
shot() {
    env $1 "$APP" --frames 20 --no-vsync --light $2 \
        --screenshot "$OUT/$3.ppm" >/dev/null 2>&1
    sed -n '2p' "$OUT/$3.ppm"
}

BASE=$(shot "SCHULTZ_PIXEL_SCALE=1" "" base)
AUTO=$(shot "SCHULTZ_PIXEL_SCALE=3" "" auto)
EXACT=$(shot "SCHULTZ_PIXEL_SCALE=3" "--exact-pixels" exact)

echo "  no scaling needed:     $BASE"
echo "  three times, scaled:   $AUTO"
echo "  three times, exact:    $EXACT"

set -- $BASE
base_w=$1 base_h=$2
set -- $AUTO
auto_w=$1 auto_h=$2
set -- $EXACT
exact_w=$1 exact_h=$2

# Rounded up, so a buffer whose size does not divide by three is not cut off.
# (n + 2) / 3 is the ceiling in integer arithmetic, which is all sh has.
want_w=$(( (base_w + 2) / 3 ))
want_h=$(( (base_h + 2) / 3 ))

fail=0
if [ "$auto_w" -ne "$want_w" ] || [ "$auto_h" -ne "$want_h" ]; then
    echo "FAIL: scaling to a three times screen reported $auto_w $auto_h,"
    echo "      wanted $want_w $want_h. A unit has to cover three pixels"
    echo "      there, so there are a third as many of them, rounded up so"
    echo "      none is cut off."
    fail=1
fi
if [ "$exact_w" -ne "$base_w" ] || [ "$exact_h" -ne "$base_h" ]; then
    echo "FAIL: not scaling reported $exact_w $exact_h, wanted the buffer's"
    echo "      own $base_w $base_h. Without scaling a unit is a pixel and"
    echo "      nothing is divided."
    fail=1
fi
[ "$fail" -eq 0 ] || exit 1
echo "PASS: a unit is the screen's pixel, or as many as it packs."
