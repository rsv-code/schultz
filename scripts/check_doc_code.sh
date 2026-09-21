#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Compiles every C snippet in the documentation.
#
# Documentation that does not compile is worse than none: a reader trusts it,
# and a call that was renamed six months ago reads exactly like one that
# works. This pulls every fenced C block out of docs/ and puts it through the
# compiler, so a renamed function breaks the docs the same day it breaks the
# code.
#
# Only the syntax and the names are checked, not the behaviour. The snippets
# use variables the harness declares for them.
#
# Usage:  sh scripts/check_doc_code.sh

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

PREFIX="$ROOT/build-deps/$(sh "$ROOT/scripts/build_deps.sh" --print-target)/prefix"
[ -d "$PREFIX" ] || { echo "build the dependencies first"; exit 1; }
# The dependency build records where it put its .pc files, which is the only
# thing that knows: the directory is named after the architecture on Debian
# and its derivatives, and plain lib elsewhere.
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig"
if [ -f "$(dirname "$PREFIX")/env.sh" ]; then
    . "$(dirname "$PREFIX")/env.sh"
fi
export PKG_CONFIG_PATH

# The declarations every snippet is compiled against.
#
# Documentation shows fragments, not programs: a snippet says
# schultz_label_create(tree, parent, ...) without ever saying what tree is.
# These stand in for what the reader is assumed to have, so the fragment
# compiles as written rather than being rewritten to suit the checker.
cat > "$OUT/blocks.c" <<'DECLS'
#include "schultz_api.h"
static schultz_tree *tree;
static schultz_events *events;
static schultz_image_table *images;
static schultz_resource_table *resources;
static schultz_handle parent, child, caption, field, button, some_child;
static schultz_handle header, some_label, form, column, stack, board;
static schultz_handle piece, tags, menu_bar_node, shell, another, font;
static schultz_handle gradient_handle, dash_handle, image, canvas;
static schultz_handle node, menu, path, glow, sprites;
static schultz_color black, accent, border, text_color;
static schultz_paint fill;
static const char *const months[12];
static void on_scrolled(void *c, schultz_tree *t, schultz_handle b,
                        float v) { (void)c; (void)t; (void)b; (void)v; }
static void *context;
static schultz_window *window;
static schultz_font_system *fonts;
static schultz_theme theme;
static schultz_handle face, shell;
static schultz_window_options options;
static uint32_t width, height;
static float scale;
static schultz_audio *audio;
static schultz_handle sound, click, radio, mic, stream;
static unsigned char *samples, *chunk, *first_chunk, *into;
static uint64_t read;
static uint32_t i, n;
static uint64_t length, want;
static const schultz_event *event;
static schultz_handle area;
static const void *png_bytes;
static uint64_t png_length;
static schultz_render_options render_options;
static const void *bytes;
static uint32_t *pixels;
static schultz_glyph_cache *glyphs;
static uint64_t milliseconds;
DECLS

# Every fenced C block in the documentation, each wrapped in a function of
# its own so that one snippet's names cannot satisfy another's.
#
# awk rather than a scripting language: this is finding fenced blocks in text
# and writing them out again, which is the work awk was written for.
count=$(
    LC_ALL=C awk '
        /^```c$/ { inside = 1; body = ""; whole = 0; next }
        inside && /^```/ {
            # A complete program is checked by building it, not by this.
            if (!whole) {
                printf "static void block_%d(void)\n{\n%s\n}\n", n++, body
            }
            inside = 0
            next
        }
        inside {
            if (index($0, "int main(") > 0) { whole = 1 }
            line = $0
            gsub(/menu_bar,/, "menu_bar_node,", line)
            body = body line "\n"
        }
        END { print n > "/dev/stderr" }
    ' "$ROOT"/docs/*.md "$ROOT/README.md" 2>"$OUT/count" >> "$OUT/blocks.c"
    cat "$OUT/count"
)
echo "$count snippets"

# The docs show the include the way an installed copy is included,
# <schultz/schultz_api.h>. Stand that layout up so those snippets compile
# against the working tree rather than against an install nobody has made.
mkdir -p "$OUT/inc"
ln -sf "$ROOT" "$OUT/inc/schultz"

gcc -std=c11 -I"$ROOT" -I"$OUT/inc" -isystem "$ROOT/third_party/libunibreak" \
    -isystem "$ROOT/../access-tunnel" \
    $(pkg-config --cflags sdl3 freetype2 harfbuzz sheenbidi thorvg-1) \
    -fsyntax-only "$OUT/blocks.c"
echo "every documented snippet compiles"
