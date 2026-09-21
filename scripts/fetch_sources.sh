#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Re-downloads the vendored source archives into third_party/src.
#
# You do not need this to build. The archives are in git, and
# scripts/build_deps.sh never reaches the network. This exists for one job:
# bumping a dependency. Edit the version and the SHA-256 in
# scripts/versions.sh, run this, and commit what changes in third_party/src.
#
# Usage:  sh scripts/fetch_sources.sh [--verify]
#
# With --verify nothing is downloaded; the vendored archives are checked
# against the recorded hashes, which is what a release check wants.

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/third_party/src"
. "$ROOT/scripts/versions.sh"

VERIFY_ONLY=0
[ "${1:-}" = "--verify" ] && VERIFY_ONLY=1

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
    else openssl dgst -sha256 "$1" | awk '{print $NF}'
    fi
}

mkdir -p "$SRC"
status=0

# get <url> <file> <sha256>
get() {
    _url=$1; _file=$2; _sha=$3
    if [ "$VERIFY_ONLY" = "0" ] && [ ! -f "$SRC/$_file" ]; then
        echo "fetching $_file"
        command -v curl >/dev/null 2>&1 || { echo "curl not found" >&2; exit 1; }
        curl -sSL --max-time 600 -o "$SRC/$_file" "$_url"
    fi
    if [ ! -f "$SRC/$_file" ]; then
        echo "MISSING  $_file"
        status=1
        return
    fi
    _got=$(sha256_of "$SRC/$_file")
    if [ "$_got" = "$_sha" ]; then
        echo "ok       $_file"
    else
        echo "MISMATCH $_file"
        echo "         expected $_sha"
        echo "         got      $_got"
        status=1
    fi
}

get "$SDL_URL"  "$SDL_FILE"  "$SDL_SHA256"
get "$MIX_URL"  "$MIX_FILE"  "$MIX_SHA256"
get "$TVG_URL"  "$TVG_FILE"  "$TVG_SHA256"
get "$FT_URL"   "$FT_FILE"   "$FT_SHA256"
get "$HB_URL"   "$HB_FILE"   "$HB_SHA256"
get "$SB_URL"   "$SB_FILE"   "$SB_SHA256"
get "$UB_URL"   "$UB_FILE"   "$UB_SHA256"
get "$ZLIB_URL" "$ZLIB_FILE" "$ZLIB_SHA256"
get "$PNG_URL"  "$PNG_FILE"  "$PNG_SHA256"
get "$WEBP_URL" "$WEBP_FILE" "$WEBP_SHA256"
get "$JPEG_URL" "$JPEG_FILE" "$JPEG_SHA256"
get "$OGG_URL"      "$OGG_FILE"      "$OGG_SHA256"
get "$VORBIS_URL"   "$VORBIS_FILE"   "$VORBIS_SHA256"
get "$FLAC_URL"     "$FLAC_FILE"     "$FLAC_SHA256"
get "$OPUS_URL"     "$OPUS_FILE"     "$OPUS_SHA256"
get "$OPUSFILE_URL" "$OPUSFILE_FILE" "$OPUSFILE_SHA256"
get "$WAVPACK_URL"  "$WAVPACK_FILE"  "$WAVPACK_SHA256"
get "$FONT_URL" "$FONT_FILE" "$FONT_SHA256"
get "$LIBVPX_URL"  "$LIBVPX_FILE"  "$LIBVPX_SHA256"
get "$NESTEGG_URL" "$NESTEGG_FILE" "$NESTEGG_SHA256"
get "$SPEEXDSP_URL" "$SPEEXDSP_FILE" "$SPEEXDSP_SHA256"

exit $status
