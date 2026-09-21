#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Checks that a program can open a window with no desktop running.
#
# SDL picks a video driver while it starts and never revisits the choice, so
# a program that wants the offscreen driver has to say so before the first
# window. It used to have only one way to say it, an environment variable,
# which meant a host embedding Schultz had to reach around the toolkit and
# set one. schultz_window_options.video_driver is that request made properly.
#
# Run with the display deliberately hidden, because that is the case being
# tested: a build machine, a container, a board with no desktop.
#
# Usage:  sh scripts/check_offscreen.sh

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP="$ROOT/build/schultz_demo"
[ -x "$APP" ] || { echo "build the demo first"; exit 1; }

cd "$ROOT"
status=0

# Without a driver and without a desktop there is nothing to open, and the
# reason has to survive the tidying up that follows the failure.
out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$APP" --frames 1 --windowed 2>&1 \
      || true)
case "$out" in
    *"No available video device"*)
        echo "  no desktop, no driver: refused, and said why" ;;
    *)
        echo "FAIL: expected a reason for the failure, got: $out"
        status=1 ;;
esac

# With the driver asked for in code, the same program runs.
if out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$APP" --frames 2 --windowed \
         --video-driver offscreen 2>&1); then
    case "$out" in
        *"presented 2 frames"*)
            echo "  no desktop, offscreen asked for in code: ran" ;;
        *)
            echo "FAIL: unexpected output: $out"
            status=1 ;;
    esac
else
    echo "FAIL: could not run offscreen: $out"
    status=1
fi

# And what the environment says still wins, so a person running the program
# on hardware nobody anticipated gets the last word.
if env -u DISPLAY -u WAYLAND_DISPLAY SDL_VIDEO_DRIVER=offscreen \
       "$APP" --frames 1 --windowed --video-driver nosuchdriver \
       >/dev/null 2>&1; then
    echo "  the environment overrides what the program asked for"
else
    echo "FAIL: the environment did not override the program's choice"
    status=1
fi

[ "$status" -eq 0 ] && echo "PASS: a window opens with no desktop."
exit "$status"
