#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Checks what a shared library built from the toolkit would export.
#
# A Java binding loads this as a shared library, into a process that already
# has its own FreeType, its own libpng and its own zlib. Two copies of a
# symbol in one process is one copy too many: whichever the loader picks, the
# other library's calls land in the wrong implementation, and the failure is
# a crash a long way from the cause.
#
# So the toolkit is compiled with -fvisibility=hidden and the public headers
# put the visibility back for what they declare. That is a pair of flags and
# a pragma, all of which are easy to lose, and losing them shows up only in
# somebody else's process. This is the check that they are all still there.
#
# Two ways to fail, and the second is the quiet one:
#
#   1. Something other than schultz_* is exported. A dependency is leaking.
#   2. Almost nothing is exported. The visibility pragma is missing from a
#      header, or -fvisibility=hidden reached a file it should not have, and
#      the library is now useless to a host rather than dangerous to one.
#
# Usage:  sh scripts/check_exports.sh

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIB="$ROOT/build/libschultz.a"
[ -f "$LIB" ] || { echo "build the library first: make lib"; exit 1; }
command -v nm >/dev/null 2>&1 || { echo "SKIP: no nm"; exit 77; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# Undefined symbols are expected and fine: the real product links the
# dependencies in beside this. What matters is what comes out with a name.
if ! cc -shared -o "$OUT/lib.so" \
        -Wl,--whole-archive "$LIB" -Wl,--no-whole-archive -lm 2>"$OUT/err"; then
    echo "FAIL: the archive cannot go into a shared library." >&2
    echo "      Almost always -fPIC missing from something it contains." >&2
    sed 's/^/      /' "$OUT/err" >&2
    exit 1
fi

nm -D --defined-only "$OUT/lib.so" | awk '$2 == "T" || $2 == "D" || $2 == "B" { print $3 }' \
    | sort > "$OUT/exported"

total=$(wc -l < "$OUT/exported")
foreign=$(grep -cv '^schultz_' "$OUT/exported" || true)

echo "  exported: $total symbols, $foreign of them not schultz_*"

if [ "$foreign" -gt 0 ]; then
    echo "FAIL: something other than the toolkit's own interface is exported:" >&2
    grep -v '^schultz_' "$OUT/exported" | head -20 | sed 's/^/      /' >&2
    echo "      A shared library exporting these meets the host's copies." >&2
    exit 1
fi

# The other direction, and it needs to be exact rather than a count. Losing
# the pragma from one header hides everything that header declares, which is
# tens of functions out of hundreds: a floor low enough not to fail on a
# growing interface is too low to notice. So every function the public
# headers declare is looked for by name.
PUB_HDRS="schultz_api.h schultz.h schultz_geom.h schultz_node.h
          schultz_style.h schultz_layout.h schultz_event.h schultz_widget.h
          schultz_widgets.h schultz_font.h schultz_image.h
          schultz_resource.h schultz_render.h schultz_window.h
          schultz_arena.h schultz_paint.h schultz_handle.h schultz_glyphs.h"

for h in $PUB_HDRS; do
    sed -n 's/^[A-Za-z_][A-Za-z0-9_ *]* \**\(schultz_[a-z0-9_]*\)(.*/\1/p' \
        "$ROOT/$h"
done | sort -u > "$OUT/declared"

comm -23 "$OUT/declared" "$OUT/exported" > "$OUT/missing"
count=$(wc -l < "$OUT/missing")
echo "  declared in the public headers: $(wc -l < "$OUT/declared"), missing: $count"

if [ "$count" -gt 0 ]; then
    echo "FAIL: the public headers declare these and the library does not" >&2
    echo "      export them, so a host cannot reach them:" >&2
    head -20 "$OUT/missing" | sed "s/^/      /" >&2
    echo "      A header is missing its visibility pragma." >&2
    exit 1
fi

echo "PASS: the interface is exported and nothing else is."
