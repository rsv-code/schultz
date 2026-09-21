#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Runs the camera tests that need a real camera.
#
# These are kept out of `make test` for two reasons. A machine with no camera
# cannot run them, which would make the suite's result depend on the hardware
# it happened to run on. And opening a camera turns a light on and, on some
# platforms, asks the person for permission -- neither of which a test suite
# should do to somebody who only typed `make test`.
#
# The tests live in tests/test_camera.c, in a second suite that runs only when
# SCHULTZ_LIVE_CAMERA is set. Each one skips rather than fails when there is
# no camera, so this is safe to run anywhere.
#
# Usage:  sh scripts/livetest/camera.sh

set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
APP="$ROOT/build/tests/test_camera"

[ -x "$APP" ] || { echo "build the tests first: make test"; exit 1; }

echo "Opening this machine's camera. The indicator light may come on."
SCHULTZ_LIVE_CAMERA=1 "$APP" -v
STATUS=$?

if [ "$STATUS" -ne 0 ]; then
    exit "$STATUS"
fi
exit 0
