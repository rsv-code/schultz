#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# audit_unused.sh - reports functions that are declared but never called.
#
# Unused code compiles cleanly and passes every test, so nothing else in the
# build catches it. It shows up when one half of a pair is written and the
# other half never is: a function superseded by a rewrite and left behind, or
# a receiver wired up with nothing to send to it. Both have happened here.
#
# A name with no caller is one of three things, and the report cannot tell
# them apart:
#
#   dead code, to delete
#   a missing call site, where the other half was never written
#   public API a host will call, which belongs in scripts/audit_allow.txt
#
# Run from the repository root, or by `make audit`.
#
# awk rather than a scripting language: this reads C source as text and
# counts names in it, which is the work awk exists for.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

ALLOW="scripts/audit_allow.txt"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The names that are allowed to have no caller. One per line, with comments
# and blank lines thrown away.
if [ -f "$ALLOW" ]; then
    sed -e 's/#.*//' -e 's/[ \t]*$//' -e '/^$/d' "$ALLOW" > "$WORK/allow"
else
    : > "$WORK/allow"
fi

# Sources a call may appear in. Headers declare; they do not call, so
# schultz*.h is read for declarations only. The test headers are here because
# a fixture in one of them does call the toolkit.
set -- schultz*.c tests/*.c tests/*.h

LC_ALL=C awk -v allow="$WORK/allow" -v orphans="$WORK/orphans" '
    # ------------------------------------------------------------ pass one
    #
    # What the headers declare. The first schultz_ name on the line that is
    # followed by an opening parenthesis is the function; anything before it
    # is the return type, which may itself be a schultz_ name.
    FNR == 1 { header = (FILENAME ~ /^schultz.*\.h$/) }

    header && /^[A-Za-z_]/ {
        rest = $0
        while (match(rest, /schultz_[a-z0-9_]+/)) {
            name = substr(rest, RSTART, RLENGTH)
            after = substr(rest, RSTART + RLENGTH)
            if (after ~ /^[ \t]*\(/) {
                if (!(name in declared)) { declared[name] = FILENAME }
                break
            }
            rest = after
        }
        next
    }

    # ------------------------------------------------------------ pass two
    #
    # Every mention in a source file, sorted into two kinds.
    #
    # A name followed by a parenthesis is a call. A name without one is the
    # function being handed to something else as a callback, which uses it
    # just as much. The absence of the parenthesis is also what tells a
    # callback apart from every declaration and definition of the same name.
    !header {
        # A definition begins in the first column with a type. It is not a
        # call, so one occurrence in the file that defines it is discounted.
        if (FILENAME ~ /\.c$/ && $0 ~ /^[A-Za-z_]/) {
            rest = $0
            while (match(rest, /schultz_[a-z0-9_]+/)) {
                name = substr(rest, RSTART, RLENGTH)
                after = substr(rest, RSTART + RLENGTH)
                if (after ~ /^[ \t]*\(/) {
                    if (!((name SUBSEP FILENAME) in defined)) {
                        defined[name SUBSEP FILENAME] = 1
                        defs[name]++
                    }
                    break
                }
                rest = after
            }
        }
        rest = $0
        while (match(rest, /schultz_[a-z0-9_]+/)) {
            name = substr(rest, RSTART, RLENGTH)
            after = substr(rest, RSTART + RLENGTH)
            if (after ~ /^[ \t]*\(/) { calls[name]++ } else { uses[name]++ }
            rest = after
        }
    }

    END {
        while ((getline line < allow) > 0) { allowed[line] = 1; nallow++ }

        for (name in declared) {
            ndeclared++
            total = calls[name] - defs[name] + uses[name]
            if (total <= 0 && !(name in allowed)) {
                printf "%s %s\n", name, declared[name] > orphans
                norphan++
            }
        }
        printf "declared functions: %d\n", ndeclared
        printf "allowlisted:        %d\n", nallow
        printf "never called:       %d\n", norphan
        exit (norphan > 0) ? 1 : 0
    }
' "$@" schultz*.h
status=$?

# Sorted by name, which awk does not promise for a loop over an array.
if [ -s "$WORK/orphans" ]; then
    sort "$WORK/orphans" | while read -r name header; do
        printf '  UNREACHABLE  %-46s %s\n' "$name" "$header"
    done
fi

# Markers left in the source. Reported, never a failure: a note saying more
# is wanted here is worth seeing beside the list above, and is not itself a
# reason to stop the build.
grep -n -E 'TODO|FIXME|XXX|not implemented' schultz*.c schultz*.h \
    > "$WORK/stubs" 2>/dev/null

if [ -s "$WORK/stubs" ]; then
    echo
    echo "stub markers (review, not a failure):"
    sed -e 's/^\([^:]*\):\([0-9]*\):[ \t]*/  \1:\2  /' "$WORK/stubs"
fi

if [ "$status" -ne 0 ]; then
    echo
    echo "Each of these is either dead code to delete, a missing call site, or"
    echo "genuine API that belongs in scripts/audit_allow.txt with a reason."
    exit 1
fi

echo
echo "no unreachable functions"
