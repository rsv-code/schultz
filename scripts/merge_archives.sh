#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Merges several static archives into one.
#
# There is no portable command for this. GNU binutils ar reads a script in
# what it calls MRI mode, where "addlib" copies in every member of another
# archive. Apple's ar has no such mode and prints its usage instead, which is
# what an iOS build hits. macOS ships libtool, whose -static mode does exactly
# this job and is what Xcode itself uses, so that is the one to reach for
# there.
#
# Usage:  sh scripts/merge_archives.sh OUT.a IN.a [IN.a ...]

set -eu

# A cross build must use its own toolchain's archiver: the host's cannot
# index an archive for another architecture. The Makefile passes them in.
AR=${AR:-ar}
RANLIB=${RANLIB:-ranlib}

if [ $# -lt 2 ]; then
    echo "usage: merge_archives.sh OUT.a IN.a [IN.a ...]" >&2
    exit 2
fi

out=$1
shift
rm -f "$out"

# Apple's libtool. Named by full path on purpose: a Mac with Homebrew often
# has GNU libtool earlier in PATH, and that is an unrelated program.
if [ -x /usr/bin/libtool ] && [ "$(uname -s)" = "Darwin" ]; then
    /usr/bin/libtool -static -o "$out" "$@"
    exit 0
fi

# GNU ar. The probe is the mode itself: binutils accepts an empty script and
# says nothing, Apple's ar rejects the flag.
if "$AR" -M </dev/null >/dev/null 2>&1; then
    {
        echo "create $out"
        for archive in "$@"; do
            echo "addlib $archive"
        done
        echo save
        echo end
    } | "$AR" -M
    "$RANLIB" "$out"
    exit 0
fi

echo "merge_archives.sh: no archive merger found." >&2
echo "  Needs GNU binutils ar, or Apple libtool on macOS." >&2
exit 1
