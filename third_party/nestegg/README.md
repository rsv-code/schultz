# nestegg (vendored)

**This is not Schultz source code.** It is a copy of someone else's library,
kept here unmodified and compiled into Schultz.

| | |
|---|---|
| **Upstream** | https://github.com/mozilla/nestegg |
| **Version** | commit `d6ec55624d8187a2699ac890b83460b973331400`, 26 August 2026 |
| **Licence** | ISC. See `LICENSE` in this directory |
| **Authors** | Matthew Gregan and contributors. See `AUTHORS` |
| **Came from** | `third_party/src/nestegg-d6ec55624d8187a2699ac890b83460b973331400.tar.gz` |

## What it does

It reads WebM and Matroska: opens the file, reports what tracks are in it and
what codec each carries, and hands back packets with the timestamps they were
stored with. It decodes nothing. The pictures inside go to libvpx and the
sound to Opus.

WebM is the container VP8, VP9 and Opus travel in, which makes it the only one
Schultz needs. See `design/video.md` for why those are the codecs.

## Why it is compiled in rather than linked

One C file and one header, plain C with no configuration step, so every target
builds it the same way and there is nothing to cross compile. libunibreak is
here for the same reason and in the same shape.

The header includes itself as `<nestegg/nestegg.h>`, so the layout upstream
uses is kept: `include/nestegg/nestegg.h` and `src/nestegg.c`. The Makefile
puts `include` on the search path as `NE_CFLAGS`.

It is compiled without `-Wall -Wextra`, like every vendored source here. It is
not our code to fix, and upstream has one `-Wmaybe-uninitialized` that we
would never act on.

## Pinned to a commit, not a release

nestegg has never made one: the repository has no tags at all. opusfile is the
only other dependency in this position, and `THIRD_PARTY_NOTICES.md` explains
both.

A commit is as reproducible as a release here, because the archive itself is
vendored in `third_party/src` with its SHA-256 recorded in
`scripts/versions.sh`, and `sh scripts/fetch_sources.sh --verify` checks it.
