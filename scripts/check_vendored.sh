#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Checks that the vendored third party source in third_party/libunibreak is
# exactly what the release archive contains.
#
# Copying someone else's source into a repository is only honest if you can
# show it is unchanged. This re-extracts the archive and diffs it, so an
# accidental edit is caught rather than assumed away.
#
# Usage:  sh scripts/check_vendored.sh

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/third_party/src"
VENDORED="$ROOT/third_party/libunibreak"
. "$ROOT/scripts/versions.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

[ -f "$SRC/$UB_FILE" ] || { echo "missing archive: $UB_FILE" >&2; exit 1; }
tar -xzf "$SRC/$UB_FILE" -C "$TMP"
UP="$TMP/libunibreak-$UB_VERSION"

status=0

# The library's own sources and headers. tests.c and test_skips.h are the
# upstream test program and are deliberately not vendored.
for f in "$UP"/src/*.c "$UP"/src/*.h; do
    name=$(basename "$f")
    case "$name" in tests.c|test_skips.h) continue ;; esac
    if [ ! -f "$VENDORED/$name" ]; then
        echo "MISSING   $name"
        status=1
    elif ! cmp -s "$f" "$VENDORED/$name"; then
        echo "MODIFIED  $name"
        status=1
    fi
done

# Anything vendored that upstream does not have. Schultz's own README.md is
# expected; everything else is a leftover.
for f in "$VENDORED"/*.c "$VENDORED"/*.h; do
    [ -e "$f" ] || continue
    name=$(basename "$f")
    if [ ! -f "$UP/src/$name" ]; then
        echo "EXTRA     $name"
        status=1
    fi
done

if ! cmp -s "$UP/LICENCE" "$VENDORED/LICENCE"; then
    echo "MODIFIED  LICENCE"
    status=1
fi

if [ "$status" = "0" ]; then
    echo "libunibreak $UB_VERSION: vendored copy matches the release archive"
fi

# ------------------------------------------------------------------ nestegg
#
# The same job for the second vendored library. Four files rather than
# twenty five, and the archive is a commit rather than a release, but the
# question is identical: is this copy what upstream published?
NE_VENDORED="$ROOT/third_party/nestegg"
[ -f "$SRC/$NESTEGG_FILE" ] ||
    { echo "missing archive: $NESTEGG_FILE" >&2; exit 1; }
tar -xzf "$SRC/$NESTEGG_FILE" -C "$TMP"
NE_UP="$TMP/nestegg-$NESTEGG_COMMIT"

for pair in "src/nestegg.c" "include/nestegg/nestegg.h" "LICENSE" "AUTHORS"; do
    if [ ! -f "$NE_VENDORED/$pair" ]; then
        echo "MISSING   nestegg/$pair"
        status=1
    elif ! cmp -s "$NE_UP/$pair" "$NE_VENDORED/$pair"; then
        echo "MODIFIED  nestegg/$pair"
        status=1
    fi
done

if [ "$status" = "0" ]; then
    echo "nestegg $NESTEGG_COMMIT: vendored copy matches the archive"
fi

# ----------------------------------------------------------------- speexdsp
#
# The same question again, with one difference worth stating: three files in
# third_party/speexdsp are deliberately not upstream's.
#
#   config.h                             what configure would have written
#   include/speex/speexdsp_config_types.h  likewise, generated from a .in
#   README.md                            ours, explaining the two above
#
# Everything else has to match the release byte for byte. Those three are
# named here rather than skipped silently, so a fourth one appearing is
# something somebody has to justify.
SDSP_VENDORED="$ROOT/third_party/speexdsp"
[ -f "$SRC/$SPEEXDSP_FILE" ] ||
    { echo "missing archive: $SPEEXDSP_FILE" >&2; exit 1; }
tar -xzf "$SRC/$SPEEXDSP_FILE" -C "$TMP"
SDSP_UP="$TMP/speexdsp-$SPEEXDSP_VERSION"

for f in "$SDSP_VENDORED"/libspeexdsp/*.c "$SDSP_VENDORED"/libspeexdsp/*.h; do
    [ -e "$f" ] || continue
    name=$(basename "$f")
    if [ ! -f "$SDSP_UP/libspeexdsp/$name" ]; then
        echo "EXTRA     speexdsp/libspeexdsp/$name"
        status=1
    elif ! cmp -s "$SDSP_UP/libspeexdsp/$name" "$f"; then
        echo "MODIFIED  speexdsp/libspeexdsp/$name"
        status=1
    fi
done

for f in "$SDSP_VENDORED"/include/speex/*.h; do
    [ -e "$f" ] || continue
    name=$(basename "$f")
    case "$name" in speexdsp_config_types.h) continue ;; esac
    if [ ! -f "$SDSP_UP/include/speex/$name" ]; then
        echo "EXTRA     speexdsp/include/speex/$name"
        status=1
    elif ! cmp -s "$SDSP_UP/include/speex/$name" "$f"; then
        echo "MODIFIED  speexdsp/include/speex/$name"
        status=1
    fi
done

for name in COPYING AUTHORS; do
    if [ ! -f "$SDSP_VENDORED/$name" ]; then
        echo "MISSING   speexdsp/$name"
        status=1
    elif ! cmp -s "$SDSP_UP/$name" "$SDSP_VENDORED/$name"; then
        echo "MODIFIED  speexdsp/$name"
        status=1
    fi
done

if [ "$status" = "0" ]; then
    echo "speexdsp $SPEEXDSP_VERSION: vendored copy matches the release archive"
fi
exit $status
