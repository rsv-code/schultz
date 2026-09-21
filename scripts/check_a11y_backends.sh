#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Checks that every accessibility backend implements the whole interface.
#
# One backend is compiled per target, so on any one machine most of them are
# never built. A missing function in the Linux backend is a link error the
# moment anyone builds; a missing function in the iOS backend is a link error
# only for whoever next builds for iOS, which may be weeks later and will
# look like their problem rather than the change that caused it.
#
# This is not a substitute for compiling them. It catches an omission, not a
# mistake.
#
# Usage:  sh scripts/check_a11y_backends.sh

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# The interface, taken from the header rather than repeated here, so that
# adding a function to schultz_a11y_backend.h makes this ask for it.
WANTED=$(sed -n 's/^\(int32_t\|void\) \(schultz_a11y_backend_[a-z_]*\)(.*/\2/p' \
    "$ROOT/schultz_a11y_backend.h" | sort -u)

[ -n "$WANTED" ] || { echo "no interface found in schultz_a11y_backend.h" >&2
                      exit 1; }

status=0
found_any=0
for file in "$ROOT"/schultz_a11y_*.c "$ROOT"/schultz_a11y_*.m; do
    [ -f "$file" ] || continue
    case $(basename "$file") in
        schultz_a11y_backend.*) continue ;;
    esac
    found_any=1
    missing=""
    for name in $WANTED; do
        # A definition, not a call: the name at the start of a line, which is
        # how every function in this project is written.
        grep -qE "^(int32_t|void) $name\(" "$file" || missing="$missing $name"
    done
    if [ -n "$missing" ]; then
        echo "$(basename "$file") does not define:$missing" >&2
        status=1
    else
        printf "  %-28s complete\n" "$(basename "$file")"
    fi
done

[ "$found_any" = 1 ] || { echo "no backends found" >&2; exit 1; }

# The Makefile has to survive being read for every target, not only this one.
#
# Two of the backends are Objective-C, and a list built with a .c only
# substitution leaves a .m source untouched, which then travels into the
# object list and the dependency list as itself. Make reads a dependency list
# as a makefile, so the first symptom is the source file being reported as a
# broken makefile, on somebody else's machine.
# Every target there is, read from the directory rather than listed here, so
# a new one is covered the day it is added rather than the day somebody
# remembers this file.
for target in $(ls "$ROOT/scripts/targets" | sed 's/\.sh$//'); do
    objs=$(make -s -p -n DEPS_TARGET="$target" -C "$ROOT" 2>/dev/null \
           | grep -m1 '^OBJS :=' || true)
    case "$objs" in
        *.c\ *|*.c|*.m\ *|*.m)
            echo "$target: a source file survives into OBJS: $objs" >&2
            status=1
            ;;
    esac

    # And that make can actually build this target's backend.
    #
    # A static pattern rule fixes the prerequisite's suffix for every target
    # it names, so an object list holding both .c and .m cannot be served by
    # one rule: the .m objects go looking for a .c that was never there.
    # -n as well as -p. On its own, -p prints the database and then goes on
    # to build the default goal, so asking every target in turn which backend
    # it picks was quietly building every target in turn, into this target's
    # directory, with that target's compiler. What it leaves behind is an
    # object for another architecture sitting in build/, and an archive that
    # links nowhere.
    backend=$(make -s -p -n DEPS_TARGET="$target" -C "$ROOT" 2>/dev/null \
              | sed -n 's/^A11Y_BACKEND := *//p' | head -1)
    object=$(printf '%s' "$backend" | sed 's/\.[cm]$/.o/')
    if make -n "build/$object" DEPS_TARGET="$target" -C "$ROOT" 2>&1 \
       | grep -q 'No rule to make target'; then
        echo "$target: make cannot build build/$object from $backend" >&2
        status=1
    fi
done
[ "$status" = 0 ] && echo "every accessibility backend is complete"
exit $status
