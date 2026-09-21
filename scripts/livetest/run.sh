#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Starts the demo, reads its tree back with a real AT-SPI client, and stops.
#
# Needs a running accessibility bus, so it is not part of `make test`. On a
# machine without one it says so and exits 77, the skip convention.
#
# Usage:  sh scripts/livetest/run.sh

set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
APP="$ROOT/build/schultz_demo"

[ -x "$APP" ] || { echo "build the demo first: make"; exit 1; }

if ! dbus-send --session --dest=org.a11y.Bus --print-reply /org/a11y/bus \
        org.a11y.Bus.GetAddress >/dev/null 2>&1; then
    echo "SKIP: no accessibility bus on this machine"
    exit 77
fi

# The demo has to be alive AND publishing while the client reads it, and
# those are not the same thing.
#
# With vsync on, a frame is presented only when the desktop asks for one, and
# a compositor stops asking for a window nobody is looking at. The demo then
# blocks in present, having published its tree exactly once at startup, and
# the client reads three nodes where it should read sixty. A locked screen
# was enough to cause it. So this presents as fast as it can, which is what
# --no-vsync is for, and no longer depends on the desktop at all.
#
# That makes the frame count a duration again, but a much shorter one: the
# same 1800 frames take under a second uncapped. The count is therefore large
# enough to outlast the read on a slow machine, and the timeout is what
# actually ends it.
#
# The demo is killed once the tree has been read rather than waited for.
# A killed process does not deregister, so the bus can be left holding an
# entry with the right name and nothing behind it; read_tree.py picks the
# instance that has the most children, which is what makes that harmless.
PAGE="${1:-1}"
timeout 90 "$APP" --frames 400000 --windowed --no-vsync --page "$PAGE" \
    >/dev/null 2>&1 &
APP_PID=$!
sleep 3

python3 "$ROOT/scripts/livetest/read_tree.py"
STATUS=$?

kill "$APP_PID" 2>/dev/null
wait "$APP_PID" 2>/dev/null
exit $STATUS
