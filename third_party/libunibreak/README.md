# libunibreak (vendored)

**This is not Schultz source code.** It is a copy of someone else's library,
kept here unmodified and compiled into Schultz.

| | |
|---|---|
| **Upstream** | https://github.com/adah1972/libunibreak |
| **Version** | 7.0 |
| **Licence** | zlib. See `LICENCE` in this directory |
| **Authors** | Wu Yongwei, Tom Hacohen, Petr Filipsky, Andreas Roever |
| **Came from** | `third_party/src/libunibreak-7.0.tar.gz`, the release archive |

## What it does

It answers one question: where is a run of text allowed to break across lines?
Schultz calls `set_linebreaks_utf8` in `schultz_text.c` and gets one flag per
byte, saying whether a break there is required, allowed, or forbidden.

Breaking at spaces is not good enough. Thai and Japanese have no spaces and
still wrap. A line may not break before a closing bracket or after an opening
quote. A hyphen allows a break and a non-breaking hyphen does not. Those rules
are Unicode Annex #14, and this library implements them.

## Why it is compiled in rather than built

Every other dependency ships cmake or meson, both of which cross compile
cleanly. This one ships autotools only, which made it the one library needing
a hand written cross build for Android and iOS, and the most likely thing to
break first on a new target.

It did not need to be. The source is plain ISO C: `stddef.h`, `string.h`,
`assert.h`, `stdint.h` and `stdbool.h`, no platform conditionals anywhere, and
no dependency on a generated `config.h`. So the build system was doing nothing
that mattered, and the files are simply compiled alongside Schultz's own. Every
target now builds it the same way, and there is one less thing to cross
compile.

## Keeping track of it

The files here are byte for byte what the 7.0 release contains. Nothing is
patched. `sh scripts/check_vendored.sh` re-extracts the archive in
`third_party/src` and diffs it against this directory, so a local edit or a
bad copy is caught rather than assumed away.

The zlib licence allows modification, on the condition that altered versions
are marked as altered. If a change ever becomes necessary, say so here and in
the file itself, and expect `check_vendored.sh` to start failing until this
note is updated to explain why.

## Upgrading

1. Update the version, URL and SHA-256 in `scripts/versions.sh`.
2. `sh scripts/fetch_sources.sh` to bring down the new archive.
3. Replace the `.c` and `.h` files here from the new archive's `src/`,
   dropping `tests.c` and `test_skips.h`, and refresh `LICENCE`.
4. `sh scripts/check_vendored.sh` to confirm the copy matches.
5. Update the version in this file and in `THIRD_PARTY_NOTICES.md`.
