#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Builds the vendored third party libraries for one target.
#
# The sources live in third_party/src as the archives upstream published, and
# they are in git. Nothing here reaches the network. Everything this produces
# goes under build-deps/<target>/, which is not in git, so third_party stays
# source only and one checkout can hold builds for several targets at once.
#
# Every run builds the target from nothing. There is no partial build and
# nothing to keep in step, which is the point: see the note above the rm below.
#
# Usage:
#   sh scripts/build_deps.sh [--target NAME] [--fuzz] [--list]
#   sh scripts/build_deps.sh --print-target     what the host detects as
#
# With no --target the host is detected. See docs/building.md.
#
# --fuzz builds the same libraries again, with AFL's instrumentation and
# AddressSanitizer, into build-deps/<target>-fuzz. It is a second prefix
# rather than a replacement: the ordinary one is what the toolkit ships
# against, and this one is only useful under a fuzzer. See the fuzzing
# section of the Makefile for why it is worth having.

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/third_party/src"
TARGETS_DIR="$ROOT/scripts/targets"

. "$ROOT/scripts/versions.sh"

# ------------------------------------------------------------ host tooling
#
# The one place the host operating system leaks in. macOS has no sha256sum and
# Windows under MSYS2 has no nproc, so both are asked for rather than assumed.
# Shell functions share one variable scope, so every local here is prefixed
# to keep it from overwriting a caller's. Getting this wrong silently
# corrupted the path the archive was then extracted from.
sha256_check() {
    _ck_sha=$1; _ck_file=$2
    if command -v sha256sum >/dev/null 2>&1; then
        echo "$_ck_sha  $_ck_file" | sha256sum -c - >/dev/null
    elif command -v shasum >/dev/null 2>&1; then
        echo "$_ck_sha  $_ck_file" | shasum -a 256 -c - >/dev/null
    elif command -v openssl >/dev/null 2>&1; then
        _ck_got=$(openssl dgst -sha256 "$_ck_file" | awk '{print $NF}')
        [ "$_ck_got" = "$_ck_sha" ] || {
            echo "checksum failed: $_ck_file" >&2; exit 1; }
    else
        echo "no sha256 tool found (sha256sum, shasum or openssl)" >&2
        exit 1
    fi
}

jobs_count() {
    if command -v nproc >/dev/null 2>&1; then nproc
    elif command -v sysctl >/dev/null 2>&1; then sysctl -n hw.ncpu
    else echo 4
    fi
}

detect_host_target() {
    case "$(uname -s)" in
        Linux)   echo "linux-$(uname -m)" ;;
        Darwin)  case "$(uname -m)" in
                     arm64) echo "macos-arm64" ;;
                     *)     echo "macos-x86_64" ;;
                 esac ;;
        MINGW*|MSYS*|CYGWIN*) echo "windows-x86_64" ;;
        *) echo "unknown" ;;
    esac
}

usage() {
    echo "usage: sh scripts/build_deps.sh [--target NAME] [--fuzz] [--list]"
    echo
    echo "  Builds one target from nothing, every time. Run make clean"
    echo "  afterwards: the toolkit's Makefile does not watch this prefix."
    echo
    echo "  --fuzz builds a second prefix, <target>-fuzz, under AFL"
    echo "  instrumentation and AddressSanitizer. Native targets only."
    echo
    echo "targets:"
    for _t in "$TARGETS_DIR"/*.sh; do
        echo "  $(basename "$_t" .sh)"
    done
}

# --------------------------------------------------------------- arguments
TARGET=""
FUZZ=0
while [ $# -gt 0 ]; do
    case "$1" in
        --target) TARGET=${2:?--target needs a name}; shift 2 ;;
        --fuzz)   FUZZ=1; shift ;;
        --list)   usage; exit 0 ;;
        --print-target) detect_host_target; exit 0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[ -n "$TARGET" ] || TARGET=$(detect_host_target)

# A fuzz prefix is named for the target it was built from, with -fuzz on the
# end, and that name is what the toolkit's Makefile passes as DEPS_TARGET. So
# the name has to be accepted here too, or the one command the Makefile tells
# a reader to run is a command that fails.
case "$TARGET" in
    *-fuzz) TARGET=${TARGET%-fuzz}; FUZZ=1 ;;
esac
TARGET_FILE="$TARGETS_DIR/$TARGET.sh"
if [ ! -f "$TARGET_FILE" ]; then
    echo "no such target: $TARGET" >&2
    usage >&2
    exit 2
fi

# What this prefix is called on disk. A fuzz build is a separate one, so that
# an afternoon of fuzzing cannot leave the prefix the toolkit ships against
# full of sanitizer instrumented libraries.
DEPS_NAME="$TARGET"
[ "$FUZZ" = "0" ] || DEPS_NAME="$TARGET-fuzz"

OUT="$ROOT/build-deps/$DEPS_NAME"
WORK="$OUT/work"
PREFIX="$OUT/prefix"

# Defaults a target file may override.
TARGET_CROSS=0
TARGET_SHLIB_VAR=LD_LIBRARY_PATH
TARGET_LIBDIRS="lib lib64"
TARGET_CMAKE_EXTRA=""
TARGET_CFLAGS_EXTRA=""
# What the toolkit's own Makefile needs to build for this target. Empty means
# the host's own compiler, which is right for a native target and wrong for
# every other one.
TARGET_TOOLKIT_CC=""
TARGET_TOOLKIT_CFLAGS=""
TARGET_TOOLKIT_AR=""
TARGET_TOOLKIT_RANLIB=""
# What the toolkit links against, when it is not the default -lstdc++ -lm.
TARGET_TOOLKIT_LDLIBS=""
TARGET_TRIPLE=""
TARGET_MESON_CPU_FAMILY=""
TARGET_MESON_CPU=""
# Which libvpx target to configure. Every target file sets this; the default
# is the one that needs no assembler, because a wrong guess that needs one
# fails the build rather than quietly producing a slower library.
TARGET_VPX=generic-gnu
# Set only by a fuzz build, read unconditionally further down, so it is
# declared here: this script runs under set -u.
FUZZ_CXX_GCC_DIR=""
# shellcheck disable=SC1090
. "$TARGET_FILE"

# ------------------------------------------------------ the fuzzing prefix
#
# Everything here exists to answer one complaint: a fuzzer that cannot see
# inside libpng is barely fuzzing at all.
#
# A fuzzer chooses its next input by watching which branches the last one
# reached, and it can only watch branches in code an instrumenting compiler
# touched. Build the toolkit under AFL and link it against an ordinary libpng
# and every well formed picture looks identical to the fuzzer: it reaches the
# same handful of edges in our own code and learns nothing about the decoding
# underneath. Measured before this existed: seven million inputs, one new path.
#
# AddressSanitizer has the same blind spot for the same reason. It checks
# accesses in code it compiled, so an overread inside an ordinary libpng is
# invisible to it. What it still catches anywhere is a double free, a
# mismatched free, and an access far enough out to hit an unmapped page.
#
# So both halves of the problem are the same fact, and the fix is the same
# fix: build the libraries the same way the fuzz target is built.
#
# AFL_USE_ASAN rather than adding -fsanitize=address to the flags by hand.
# Half of these builds overwrite the flags they are given, and a sanitizer
# that a build system quietly dropped is worse than none, because the run
# still looks clean. The compiler driver adds it either way.
#
# ASan without UndefinedBehaviorSanitizer, which is deliberate. Codecs shift
# and convert in ways UBSan objects to and their authors intended, so turning
# it on here buys a stream of stops in working code. Our own sources are
# built with both; those we can fix.
if [ "$FUZZ" = "1" ]; then
    if [ "$TARGET_CROSS" = "1" ]; then
        echo "--fuzz is for a target built on this machine, not $TARGET" >&2
        exit 2
    fi
    command -v afl-clang-fast >/dev/null 2>&1 || {
        echo "--fuzz needs afl-clang-fast, which comes with AFL++" >&2
        echo >&2
        echo "  sudo apt install afl++" >&2
        exit 1
    }
    # afl-clang-fast rather than afl-gcc-fast: the GCC one is a compiler
    # plugin built against one exact GCC, and a distribution that has since
    # moved its GCC by a patch release leaves it unloadable. The clang driver
    # is a wrapper with no such tie.
    CC=afl-clang-fast
    CXX=afl-clang-fast++

    # Can that C++ driver find a C++ standard library at all?
    #
    # Asked rather than assumed, because on this machine it could not. clang
    # does not carry libstdc++ headers: it looks for a GCC installation and
    # borrows theirs, and it takes the newest one it finds. Ubuntu 24.04 has
    # a bare /usr/lib/gcc/x86_64-linux-gnu/14 from the runtime packages and
    # the headers only under /usr/include/c++/13, so clang chooses 14, finds
    # no headers beside it, and stops on
    #
    #   fatal error: 'cstdint' file not found
    #
    # which sounds like a broken installation and is a version mismatch.
    # --gcc-install-dir names the one to borrow from. Which one is right is
    # decided by compiling with it rather than by comparing version numbers,
    # because the question is whether the headers are there.
    _probe=$(mktemp -d)
    printf '#include <cstdint>\nint main(void){return 0;}\n' > "$_probe/p.cc"
    if ! $CXX -c "$_probe/p.cc" -o "$_probe/p.o" 2>/dev/null; then
        for _d in $(ls -d /usr/lib/gcc/*/*/ 2>/dev/null | sort -Vr); do
            if $CXX --gcc-install-dir="${_d%/}" -c "$_probe/p.cc" \
                    -o "$_probe/p.o" 2>/dev/null; then
                FUZZ_CXX_GCC_DIR=${_d%/}
                break
            fi
        done
        if [ -z "$FUZZ_CXX_GCC_DIR" ]; then
            rm -rf "$_probe"
            echo "afl-clang-fast++ cannot find a C++ standard library" >&2
            echo >&2
            echo "  sudo apt install libstdc++-13-dev" >&2
            exit 1
        fi
        # Passed through a wrapper rather than added to CXX, because a
        # compiler with a flag in it is a string some of these build systems
        # accept and others split down the middle. A program on disk is one
        # word to every one of them.
        CXX="$OUT/bin/fuzz-c++"
    fi
    rm -rf "$_probe"

    AFL_USE_ASAN=1
    # Quiet, because otherwise every one of several thousand compiles prints
    # its own instrumentation summary and the build log is unreadable.
    AFL_QUIET=1
    export CC CXX AFL_USE_ASAN AFL_QUIET
    # cmake is told in its own language. It does read CC, but only when
    # nothing else has already decided, and several of these projects set a
    # compiler themselves.
    #
    # --no-warn-unused-cli because a C only project among these is handed a
    # C++ compiler it has no use for, and says so, once per library. The
    # warning is correct and says nothing worth reading eleven times.
    TARGET_CMAKE_EXTRA="$TARGET_CMAKE_EXTRA --no-warn-unused-cli -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX"

    # And what the toolkit itself must be built with to link against this
    # prefix, written into config.mk below and read by the Makefile. Sources
    # we wrote get UndefinedBehaviorSanitizer as well, because a stop in our
    # own code is a bug report rather than noise.
    TARGET_TOOLKIT_CC="afl-clang-fast"
    TARGET_TOOLKIT_CFLAGS="$TARGET_TOOLKIT_CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -g"
    # The C++ runtime, which ThorVG needs, is named as a file rather than as
    # -lstdc++. The bare name resolves through a symlink that ships with g++
    # inside GCC's own library directory, and clang does not search there.
    # Only Linux: this spelling is a GNU linker feature, and the Apple
    # targets set their own link line already.
    case "$TARGET" in
        linux-*) TARGET_TOOLKIT_LDLIBS="-l:libstdc++.so.6 -lm" ;;
    esac
fi

# ------------------------------------------------------ the tools it needs
#
# Checked here, together, before anything is downloaded, deleted or compiled.
#
# Without this the first missing one arrives as whatever error the tool that
# wanted it happens to produce, which is rarely the name of the package to
# install. A missing ninja on MSYS2 reads:
#
#   CMake Error: CMake was unable to find a build program corresponding to
#   "Ninja".  CMAKE_MAKE_PROGRAM is not set.
#   CMake Error: CMAKE_C_COMPILER not set, after EnableLanguage
#
# which names neither ninja nor the fact that a compiler was never the
# problem. cmake on MSYS2 defaults to the Ninja generator where cmake
# elsewhere defaults to makefiles, so that one only happens on Windows, and it
# happens on the first library of fifteen.
#
# ninja is not optional anywhere, whatever cmake chooses: ThorVG, FreeType,
# HarfBuzz and SheenBidi are built with meson, and meson builds with ninja.
#
# python3 is on the list because meson is a Python program: its own shebang is
# /usr/bin/python3 and the Debian package declares the dependency. ThorVG
# builds with meson and ships no other build system at all, so a machine that
# can build Schultz has Python whether anybody wanted it to or not. Named here
# so that the missing one is reported by name rather than surfacing as
# whatever meson says when it cannot start.
missing=""
for _t in cmake meson ninja pkg-config tar python3; do
    command -v "$_t" >/dev/null 2>&1 || missing="$missing $_t"
done
# A compiler, for a target that uses this machine's. A cross target brings its
# own from the NDK or from Xcode, so asking about this one would be asking the
# wrong question.
if [ "$TARGET_CROSS" = "0" ]; then
    command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 ||
        command -v clang >/dev/null 2>&1 || missing="$missing a C compiler"
fi
if [ -n "$missing" ]; then
    echo "missing build tools:$missing" >&2
    echo >&2
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            echo "  pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-meson mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-python make tar" >&2
            echo >&2
            echo "  Install those in the MINGW64 shell, not the MSYS one." >&2
            ;;
        Darwin)
            echo "  brew install cmake meson ninja pkg-config python3" >&2
            ;;
        *)
            echo "  sudo apt install build-essential cmake meson ninja-build pkg-config python3" >&2
            ;;
    esac
    echo >&2
    echo "See docs/building.md." >&2
    exit 1
fi

# Everything above succeeded, so this target is thrown away and built again.
#
# There is no partial build kept, on purpose. Skipping a library already
# installed in the prefix made a second run quick and made it wrong the moment
# anything changed: a library built from the old pinned version, or with the
# old set of options, was already installed, so it was skipped, and the build
# succeeded while producing something nobody asked for.
#
# It bit hardest where one library is configured against another. Turning a
# decoder on in SDL_mixer only takes effect when SDL_mixer is configured, so a
# prefix that already had it kept the decoders it was built with no matter
# what the options here said. Nothing failed. The formats just quietly did not
# work, and on Android and iOS no tests run to notice.
#
# A dependency build happens when somebody bumps a version, adds a library or
# sets up a new machine, which is rare, and one slow build is cheaper than a
# silent one.
echo "removing $OUT"
rm -rf "$OUT"

# Where a cross build is allowed to look for a library.
#
# A cross toolchain narrows find_library, find_path and find_package to the
# paths under CMAKE_FIND_ROOT_PATH, so that a build for a phone cannot pick up
# the host's copy of something. The Android NDK sets all three to ONLY and
# puts the NDK itself in that list. Nothing else is in it, including this
# prefix, so a library built one step earlier here is invisible to the next
# one that looks for it, and the build stops:
#
#   Could NOT find Ogg (missing: OGG_LIBRARY OGG_INCLUDE_DIR)
#
# pkg-config still answers, which is what makes it confusing: cmake reports
# the version it could not find.
#
# Worse than stopping is not stopping. Where the sysroot has its own copy, the
# search succeeds against that instead. libpng on Android was linking the
# NDK's shared libz.so rather than the static zlib pinned here, quietly
# undoing both the vendoring and the static build for that one library.
#
# So the prefix goes in front of the toolchain's own entry. The NDK appends to
# this list rather than replacing it, and first match wins, which is what
# makes ours the one that is found.
if [ "$TARGET_CROSS" = "1" ]; then
    TARGET_CMAKE_EXTRA="$TARGET_CMAKE_EXTRA -DCMAKE_FIND_ROOT_PATH=$PREFIX"
fi

# One per core, unless told otherwise. A small board has more cores than it
# has memory to compile with, and a killed compiler is the usual way that
# shows, so JOBS is left overridable.
JOBS=${JOBS:-$(jobs_count)}

# Position independent code, in every dependency and in the toolkit.
#
# A static archive of ordinary code cannot go into a shared library: the
# linker refuses it, because the addresses are baked in. Building everything
# this way is what lets one set of archives serve both the static product and
# a .so, .dylib or .dll, and it costs a register on the architectures that
# still care and nothing at all on the ones that do not.
CMAKE_PIC="-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
# Meson builds static libraries this way already; said out loud so that a
# change of default cannot quietly take it away.
MESON_PIC="-Db_staticpic=true"
mkdir -p "$WORK" "$PREFIX"

# The C++ wrapper the fuzz block asked for, now that there is somewhere to
# put it. See the comment there for why it exists.
if [ "$FUZZ" = "1" ] && [ -n "$FUZZ_CXX_GCC_DIR" ]; then
    mkdir -p "$OUT/bin"
    cat > "$OUT/bin/fuzz-c++" <<WRAPEOF
#!/bin/sh
# Generated by scripts/build_deps.sh. afl-clang-fast++ with the one flag that
# tells clang which GCC installation to borrow C++ headers from.
exec afl-clang-fast++ --gcc-install-dir=$FUZZ_CXX_GCC_DIR "\$@"
WRAPEOF
    chmod +x "$OUT/bin/fuzz-c++"
fi


# pkg-config has to look in our prefix and nowhere else that matters, so that
# a system copy of FreeType cannot satisfy a check and silently skip the
# pinned build. The libdir varies by platform, so every candidate is listed.
PC_PATH=""
for _d in $TARGET_LIBDIRS; do
    PC_PATH="$PC_PATH$PREFIX/$_d/pkgconfig:"
done
PKG_CONFIG_PATH="$PC_PATH${PKG_CONFIG_PATH:-}"
export PKG_CONFIG_PATH
# A cross build must not read the host's .pc files at all.
if [ "$TARGET_CROSS" = "1" ]; then
    PKG_CONFIG_LIBDIR="$PC_PATH"
    export PKG_CONFIG_LIBDIR
fi

# extract <archive> <expected sha256> <directory it unpacks to>
extract() {
    _ex_file=$1; _ex_sha=$2; _ex_dir=$3
    [ -f "$SRC/$_ex_file" ] || {
        echo "missing source archive: $_ex_file" >&2; exit 1; }
    sha256_check "$_ex_sha" "$SRC/$_ex_file"
    if [ ! -d "$WORK/$_ex_dir" ]; then
        case "$_ex_file" in
            *.tar.gz)  tar -xzf "$SRC/$_ex_file" -C "$WORK" ;;
            *.tar.xz)  tar -xJf "$SRC/$_ex_file" -C "$WORK" ;;
            *.tar.bz2) tar -xjf "$SRC/$_ex_file" -C "$WORK" ;;
            *) echo "unknown archive kind: $_ex_file" >&2; exit 1 ;;
        esac
    fi
}

# A meson cross file, written from what the target file declared. Meson needs
# one for any build whose host machine is not the machine doing the building.
CROSS_FILE="$OUT/meson-cross.txt"
NATIVE_FILE="$OUT/meson-native.txt"
write_cross_file() {
    case "$TARGET" in
    android-*)
        _hosttag=$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64
        _bin="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$_hosttag/bin"
        cat > "$CROSS_FILE" <<CROSSEOF
[binaries]
c = '$_bin/${TARGET_TRIPLE}${TARGET_ANDROID_API}-clang'
cpp = '$_bin/${TARGET_TRIPLE}${TARGET_ANDROID_API}-clang++'
ar = '$_bin/llvm-ar'
strip = '$_bin/llvm-strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'android'
cpu_family = '$TARGET_MESON_CPU_FAMILY'
cpu = '$TARGET_MESON_CPU'
endian = 'little'
CROSSEOF
        ;;
    ios-*)
        cat > "$CROSS_FILE" <<CROSSEOF
[binaries]
c = 'clang'
cpp = 'clang++'
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['$(echo "$TARGET_CFLAGS_EXTRA" | sed "s/ /', '/g")']
cpp_args = ['$(echo "$TARGET_CFLAGS_EXTRA" | sed "s/ /', '/g")']
c_link_args = ['$(echo "$TARGET_CFLAGS_EXTRA" | sed "s/ /', '/g")']
cpp_link_args = ['$(echo "$TARGET_CFLAGS_EXTRA" | sed "s/ /', '/g")']

[host_machine]
system = 'darwin'
cpu_family = '$TARGET_MESON_CPU_FAMILY'
cpu = '$TARGET_MESON_CPU'
endian = 'little'
CROSSEOF
        ;;
    macos-*)
        #
        # Only when the architecture is not this machine's, and then it is a
        # cross file rather than a native one for a reason that costs an
        # afternoon to find.
        #
        # A native build ends its compiler check by running what it just
        # compiled. That is the difference between the two files: a cross file
        # carries a [host_machine] section, which tells meson the result is not
        # for this machine and to stop at compiling. Handed an arm64 -arch flag
        # and no [host_machine], meson on an Intel Mac builds an arm64 test
        # program, tries to run it, and stops before a line of ThorVG is
        # compiled. Rosetta translates the other direction only.
        #
        # So building for the other architecture is a cross build as far as
        # meson is concerned, whatever the rest of this script calls it.
        if [ "$(uname -m)" != "${TARGET#macos-}" ]; then
            cat > "$CROSS_FILE" <<CROSSEOF
[binaries]
c = 'clang'
cpp = 'clang++'
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['$(echo "$TARGET_CFLAGS_EXTRA" | sed "s/ /', '/g")']
cpp_args = ['$(echo "$TARGET_CFLAGS_EXTRA" | sed "s/ /', '/g")']
c_link_args = ['$(echo "$TARGET_CFLAGS_EXTRA" | sed "s/ /', '/g")']
cpp_link_args = ['$(echo "$TARGET_CFLAGS_EXTRA" | sed "s/ /', '/g")']

[host_machine]
system = 'darwin'
cpu_family = '$TARGET_MESON_CPU_FAMILY'
cpu = '$TARGET_MESON_CPU'
endian = 'little'
CROSSEOF
        fi
        ;;
    esac
}
write_cross_file

# And a native machine file, for a target on this machine's own operating
# system but not its architecture. Written as a file rather than passed as
# -Dc_args so that the flags survive as one argument each: they contain
# spaces, and the command lines below expand unquoted.
write_native_file() {
    [ -f "$CROSS_FILE" ] && return 0
    [ "$TARGET_CROSS" = "0" ] || return 0
    [ -n "$TARGET_CFLAGS_EXTRA" ] || [ "$FUZZ" = "1" ] || return 0
    : > "$NATIVE_FILE"
    # A fuzz build names its compiler here as well as in the environment.
    # meson does read CC, and it also caches what it found the first time it
    # configured a directory, so saying it in the file is what makes the
    # answer the same however the build was reached.
    if [ "$FUZZ" = "1" ]; then
        cat >> "$NATIVE_FILE" <<BINEOF
[binaries]
c = '$CC'
cpp = '$CXX'
BINEOF
    fi
    [ -n "$TARGET_CFLAGS_EXTRA" ] || return 0
    _args=$(echo "$TARGET_CFLAGS_EXTRA" | sed "s/ /', '/g")
    cat >> "$NATIVE_FILE" <<NATIVEEOF
[built-in options]
c_args = ['$_args']
cpp_args = ['$_args']
c_link_args = ['$_args']
cpp_link_args = ['$_args']
NATIVEEOF
}
write_native_file

# What meson has to be told about the target, which is not the same question
# cmake was asked.
#
# A cross target hands it a machine file, written above. A target on this
# machine's own operating system but for another of its architectures has no
# machine file and still needs the architecture named: macOS builds for
# whatever it is running on unless a flag says otherwise, and the flag that
# tells cmake is not the flag that tells meson.
#
# Without this the prefix ends up holding two architectures. The eleven cmake
# libraries come out for the target and the four meson ones for the machine,
# nothing complains, and it surfaces much later as a link that cannot find
# symbols it can plainly see.
meson_args() {
    if [ -f "$CROSS_FILE" ]; then
        echo "--cross-file $CROSS_FILE"
    elif [ -f "$NATIVE_FILE" ]; then
        echo "--native-file $NATIVE_FILE"
    fi
}

echo "target:  $TARGET"
[ "$FUZZ" = "0" ] || echo "build:   AFL instrumentation, AddressSanitizer"
[ -z "$FUZZ_CXX_GCC_DIR" ] || echo "c++:     borrowing headers from $FUZZ_CXX_GCC_DIR"
echo "prefix:  $PREFIX"
echo

# Every dependency is built static, so what ships is one binary with nothing
# beside it. A shared build would mean carrying five libraries and an rpath
# to find them, on five platforms, which is a packaging problem on every one
# of them and an installation problem for whoever runs it.
#
# The cost is that a static link needs each library's own dependencies named
# too, which is what pkg-config --static is for; see the Makefile.

# ------------------------------------------------------------------- zlib
#
# First, because libpng needs it and FreeType can use it. Nothing else in the
# set depends on the system having one.
echo "building zlib $ZLIB_VERSION"
extract "$ZLIB_FILE" "$ZLIB_SHA256" "zlib-$ZLIB_VERSION"
# shellcheck disable=SC2086
cmake -S "$WORK/zlib-$ZLIB_VERSION" -B "$WORK/zlib-$ZLIB_VERSION/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      $CMAKE_PIC \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON \
      -DZLIB_BUILD_TESTING=OFF \
      $TARGET_CMAKE_EXTRA >/dev/null
cmake --build "$WORK/zlib-$ZLIB_VERSION/build" --parallel "$JOBS" >/dev/null
cmake --install "$WORK/zlib-$ZLIB_VERSION/build" >/dev/null
#
# On Windows the static library is installed as libzs.a rather than libz.a.
# zlib's CMakeLists adds an "s" to the name on WIN32 without asking which
# Windows toolchain it is, which suits MSVC, where the import library for the
# DLL would otherwise take the same name, and does not suit MinGW, where the
# import library is libz.dll.a and there is no clash to avoid.
#
# The zlib.pc it installs says -lz on every platform regardless, so on Windows
# that names a library this prefix does not contain. Everything then finds the
# system one instead: the demo and the shared library bind to MSYS2's
# zlib1.dll, and the merged archive, which looks for libz.a by name, leaves
# zlib out altogether. All three fail only on a machine without MSYS2.
#
# A copy under the name pkg-config asks for is the whole fix. libpng has the
# same decision to make and gets it right, testing for a Unix-like toolchain
# rather than for Windows, so zlib is the only one this applies to.
for _d in $TARGET_LIBDIRS; do
    if [ -f "$PREFIX/$_d/libzs.a" ] && [ ! -f "$PREFIX/$_d/libz.a" ]; then
        cp "$PREFIX/$_d/libzs.a" "$PREFIX/$_d/libz.a"
        echo "  zlib: copied libzs.a to libz.a, which is what -lz asks for"
    fi
done
echo "  zlib installed"

# ----------------------------------------------------------------- libpng
#
# ThorVG decodes PNG with this, and FreeType reads PNG for colour bitmap
# fonts. PNG_BUILD_ZLIB stays off so it uses the one just built rather than
# fetching its own.
echo "building libpng $PNG_VERSION"
extract "$PNG_FILE" "$PNG_SHA256" "libpng-$PNG_VERSION"
# shellcheck disable=SC2086
cmake -S "$WORK/libpng-$PNG_VERSION" -B "$WORK/libpng-$PNG_VERSION/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      $CMAKE_PIC \
      -DCMAKE_PREFIX_PATH="$PREFIX" \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_BUILD_ZLIB=OFF \
      -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_EXECUTABLES=OFF \
      $TARGET_CMAKE_EXTRA >/dev/null
cmake --build "$WORK/libpng-$PNG_VERSION/build" --parallel "$JOBS" >/dev/null
cmake --install "$WORK/libpng-$PNG_VERSION/build" >/dev/null
echo "  libpng installed"

# ---------------------------------------------------------------- libwebp
#
# Only the decoding library is wanted. The command line tools, the muxer and
# the GIF and JPEG helpers are all off, which keeps this to what ThorVG calls.
# sharpyuv is part of this build rather than a separate download.
echo "building libwebp $WEBP_VERSION"
extract "$WEBP_FILE" "$WEBP_SHA256" "libwebp-$WEBP_VERSION"
# shellcheck disable=SC2086
cmake -S "$WORK/libwebp-$WEBP_VERSION" \
      -B "$WORK/libwebp-$WEBP_VERSION/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      $CMAKE_PIC \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
      -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
      -DWEBP_BUILD_GIF=OFF -DWEBP_BUILD_IMG=OFF \
      -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
      -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_LIBWEBPMUX=OFF \
      -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_EXTRAS=OFF \
      $TARGET_CMAKE_EXTRA >/dev/null
cmake --build "$WORK/libwebp-$WEBP_VERSION/build" --parallel "$JOBS" >/dev/null
cmake --install "$WORK/libwebp-$WEBP_VERSION/build" >/dev/null
echo "  libwebp installed"

# --------------------------------------------------------- libjpeg-turbo
#
# ThorVG decodes JPEG with this, and it is vendored so that it decodes JPEG
# with this everywhere.
#
# ThorVG's loader asks for libturbojpeg and falls back to a decoder in its own
# tree when it finds none, so before this was pinned, which decoder shipped
# depended on what the build machine happened to have installed. A Mac with
# Homebrew's copy produced a ThorVG that needed a library nothing here
# vendored, was not named in its .pc file, and on that machine had been built
# for the other architecture.
#
# Built before ThorVG, which looks for it.
#
# The tools are off, and tests, and JPEG7 and JPEG8, which change the libjpeg
# API's ABI for compatibility with releases nothing here asks for.
#
# WITH_SYSTEM_ZLIB is the one that matters. libjpeg-turbo carries a copy of
# zlib and compiles it into libturbojpeg.a, which is one zlib too many: the
# merged archive would then hold two adler32.c.o members and the shared
# library would not link.
#
#   multiple definition of `adler32_combine'
#
# On means the zlib built here, found through CMAKE_PREFIX_PATH, which is also
# the answer the rest of this prefix gives. zlib is built before this for that
# reason.
#
# Its copy of libspng stays: it is used by the TurboJPEG API's image loading,
# nothing else here provides those symbols, and it is BSD 2-Clause. See
# THIRD_PARTY_NOTICES.md.
echo "building libjpeg-turbo $JPEG_VERSION"
extract "$JPEG_FILE" "$JPEG_SHA256" "libjpeg-turbo-$JPEG_VERSION"
# shellcheck disable=SC2086
cmake -S "$WORK/libjpeg-turbo-$JPEG_VERSION" \
      -B "$WORK/libjpeg-turbo-$JPEG_VERSION/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      $CMAKE_PIC \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_PREFIX_PATH="$PREFIX" \
      -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
      -DWITH_TURBOJPEG=ON -DWITH_TOOLS=OFF -DWITH_TESTS=OFF \
      -DWITH_JPEG7=OFF -DWITH_JPEG8=OFF \
      -DWITH_SYSTEM_ZLIB=ON \
      $TARGET_CMAKE_EXTRA >/dev/null
cmake --build "$WORK/libjpeg-turbo-$JPEG_VERSION/build" --parallel "$JOBS" \
    >/dev/null
cmake --install "$WORK/libjpeg-turbo-$JPEG_VERSION/build" >/dev/null
#
# And the zlib it now depends on, named in its .pc file, which it does not do
# for itself. Its bundled libspng calls compress2 and compressBound, so
# libturbojpeg.a needs zlib after it on a static link, and pkg-config is what
# decides that order. Without this, -lz lands before -lturbojpeg and the link
# stops on two symbols from a library that is right there.
for _d in $TARGET_LIBDIRS; do
    if [ -f "$PREFIX/$_d/pkgconfig/libturbojpeg.pc" ] &&
       ! grep -q "^Requires.private:" "$PREFIX/$_d/pkgconfig/libturbojpeg.pc"
    then
        printf 'Requires.private: zlib\n' \
            >> "$PREFIX/$_d/pkgconfig/libturbojpeg.pc"
    fi
done
echo "  libjpeg-turbo installed"

# ----------------------------------------------------------------- libvpx
#
# VP8 and VP9, the two video codecs this project can ship.
#
# Both codecs, both directions.
#
# Both are clean enough to ship. VP9 is the default here and
# VP8 the fallback -- and patent exposure attaches to implementing a codec
# rather than to which direction it is run, so there was never a reason to
# ship one direction of VP9 and not the other.
#
# VP9 encoding is expensive, and more so here because the paragraph below
# builds libvpx with no SIMD on x86. Whether it is too expensive to be worth
# having is a question to answer by measuring rather than by leaving it out:
# a host that finds it too slow asks for VP8 and pays nothing for the VP9
# encoder being present but unused.
#
# libvpx has its own configure, not autotools and not cmake, and it wants a
# target name rather than a host triple. Each target file names one in
# TARGET_VPX.
#
# **No assembler.** libvpx will not configure for x86 without nasm or yasm,
# and this build has never needed either. Every x86 target therefore asks for
# generic-gnu, which is pure C. ARM loses nothing by this: libvpx's NEON is
# written as C intrinsics rather than assembly, so an ARM target gets it from
# the C compiler alone.
#
# **What it costs on x86.** Measured on this machine, decoding only, with
# nothing else running: 560x416 VP9 at 24 frames a second takes 32% of one
# core, and 1080p at 24 takes a whole one. It keeps up, and it is a lot for
# what it is -- roughly four fifths of everything a playing video node
# spends. Adding nasm to the x86 targets is the way to get it back, at the
# price of a build dependency every x86 contributor would need. Not taken
# yet.
echo "building libvpx $LIBVPX_VERSION for $TARGET_VPX"
extract "$LIBVPX_FILE" "$LIBVPX_SHA256" "libvpx-$LIBVPX_VERSION"
mkdir -p "$WORK/libvpx-$LIBVPX_VERSION/build"
(
    cd "$WORK/libvpx-$LIBVPX_VERSION/build"
    # libvpx chooses its linker separately from its compiler, and defaults it
    # to gcc rather than to whatever CC says. That is harmless in an ordinary
    # build, where both are gcc anyway, and fatal in a fuzz build: the objects
    # come out with AFL's instrumentation and AddressSanitizer in them, gcc is
    # asked to link them, and the first test program configure tries stops on
    #
    #   undefined reference to `__afl_area_ptr'
    #   undefined reference to `__asan_init'
    #
    # which reads as a broken compiler and is nothing of the kind. Set inside
    # this subshell so no other library sees it.
    if [ "$FUZZ" = "1" ]; then
        LD="$CC"
        export LD
    fi
    # shellcheck disable=SC2086
    ../configure \
        --target="$TARGET_VPX" \
        --prefix="$PREFIX" \
        --disable-examples --disable-tools --disable-docs \
        --disable-unit-tests --disable-webm-io \
        --enable-static --disable-shared --enable-pic \
        --enable-vp8-encoder --enable-vp9-encoder \
        --enable-vp8-decoder --enable-vp9-decoder >/dev/null
    make -j"$JOBS" >/dev/null
    make install >/dev/null
)
echo "  libvpx installed"

# ------------------------------------------------------------------- SDL3
#
# SDL_ANDROID_JAR off, because what is being built here is the native side.
# On Android SDL also compiles its own Java activity classes into a jar, and
# those belong in the application's own project alongside its manifest rather
# than in a prefix full of static libraries. Left on, the build stops: the jar
# is compiled against whichever android.jar cmake finds, and SDL's Java is
# newer than the platform an NDK-only install has lying about.
#
# The option is ignored everywhere else, so it is passed unconditionally.
echo "building SDL $SDL_VERSION"
extract "$SDL_FILE" "$SDL_SHA256" "SDL3-$SDL_VERSION"
# shellcheck disable=SC2086
cmake -S "$WORK/SDL3-$SDL_VERSION" -B "$WORK/SDL3-$SDL_VERSION/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      $CMAKE_PIC \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON \
      -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF \
      -DSDL_ANDROID_JAR=OFF \
      $TARGET_CMAKE_EXTRA >/dev/null
cmake --build "$WORK/SDL3-$SDL_VERSION/build" --parallel "$JOBS" >/dev/null
cmake --install "$WORK/SDL3-$SDL_VERSION/build" >/dev/null
echo "  SDL installed"
#
# Which screens SDL can actually drive, decided when it was configured
# from what was installed at the time. Reported because the answer is
# invisible otherwise: a board with no desktop needs KMSDRM, that needs
# libdrm and gbm to have been present just now, and finding out later
# means finding out on the board with nothing on the screen and no clue
# why. Adding the packages afterwards is not enough either, since SDL is
# not rebuilt once it is installed.
_cfg="$WORK/SDL3-$SDL_VERSION/build/include-config-release/build_config/SDL_build_config.h"
if [ -f "$_cfg" ]; then
    printf "  SDL video drivers: "
    grep -E "^#define SDL_VIDEO_DRIVER_[A-Z0-9]+ 1" "$_cfg" \
        | sed "s/#define SDL_VIDEO_DRIVER_//; s/ 1//" \
        | tr "A-Z\\n" "a-z " 
    echo
    if ! grep -q "^#define SDL_VIDEO_DRIVER_KMSDRM 1" "$_cfg"; then
        echo "  note: no KMSDRM. A machine with no desktop cannot show a"
        echo "        window. Install libdrm-dev and libgbm-dev, then"
        echo "        run this script again."
    fi
    #
    # And whether it can read a keyboard. A desktop hands input to its
    # clients, so this only matters where there is no desktop, which is
    # where it is also hardest to notice: the application runs, draws
    # perfectly, and answers nothing. It looks like a hang.
    if grep -q "^#define SDL_VIDEO_DRIVER_KMSDRM 1" "$_cfg" &&
       ! grep -q "^#define HAVE_LIBUDEV_H 1" "$_cfg"; then
        echo "  note: no libudev. SDL can drive the screen but cannot find"
        echo "        a keyboard or a mouse without a desktop to hand it"
        echo "        them. Install libudev-dev, then run this script"
        echo "        again."
    fi
fi

# ------------------------------------------------------- the audio codecs
#
# Six libraries, all built before SDL_mixer, which links them. They are the
# format authors' own decoders, used instead of the small single file ones
# bundled inside SDL_mixer: they read more of each format, they are what every
# other player uses, and an advisory against one of them gets published.
#
# Opus and WavPack had no bundled decoder to replace. Those two are formats
# the toolkit could not play at all before.
#
# Every one is BSD style, and THIRD_PARTY_NOTICES.md records each. Nothing
# here is copyleft; see the note above SDL_mixer for what was left out.

# ------------------------------------------------------------------ libogg
#
# First of the six. Vorbis, FLAC and Opus all store their packets in Ogg
# containers, and all three look for this.
echo "building libogg $OGG_VERSION"
extract "$OGG_FILE" "$OGG_SHA256" "libogg-$OGG_VERSION"
# shellcheck disable=SC2086
cmake -S "$WORK/libogg-$OGG_VERSION" -B "$WORK/libogg-$OGG_VERSION/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      $CMAKE_PIC \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DBUILD_SHARED_LIBS=OFF -DINSTALL_DOCS=OFF \
      $TARGET_CMAKE_EXTRA >/dev/null
cmake --build "$WORK/libogg-$OGG_VERSION/build" --parallel "$JOBS" >/dev/null
cmake --install "$WORK/libogg-$OGG_VERSION/build" >/dev/null
echo "  libogg installed"

# --------------------------------------------------------------- libvorbis
#
# Builds three libraries: vorbis decodes packets, vorbisenc encodes, and
# vorbisfile reads a whole Ogg Vorbis file. SDL_mixer links vorbisfile, which
# pulls the other two with it. This replaces the bundled stb_vorbis.
echo "building libvorbis $VORBIS_VERSION"
extract "$VORBIS_FILE" "$VORBIS_SHA256" "libvorbis-$VORBIS_VERSION"
# CMAKE_POLICY_VERSION_MINIMUM is what lets a new cmake configure an old
# project. libvorbis 1.3.7 is from 2020 and opens with
# cmake_minimum_required(VERSION 2.8.12); cmake 4 removed compatibility with
# anything below 3.5 and stops:
#
#   Compatibility with CMake < 3.5 has been removed from CMake.
#
# There is no newer libvorbis to move to. It is the only vendored library this
# affects: every other one declares 3.6 or later, or uses the <min>...<max>
# form, which is how wavpack gets away with a minimum of 3.2.
#
# Set as an environment variable rather than passed with -D on purpose. The
# variable did not exist before cmake 3.31, and an older cmake reports a -D it
# does not know as an unused variable, which would put a warning in front of
# everyone on an older toolchain to fix a problem only a newer one has. An
# unknown environment variable is ignored in silence.
#
# A future cmake is likely to do the same for 3.10, and libogg 1.3.6 already
# warns about it. This is the pattern to repeat when that day comes.
# shellcheck disable=SC2086
CMAKE_POLICY_VERSION_MINIMUM=3.5 \
cmake -S "$WORK/libvorbis-$VORBIS_VERSION" \
      -B "$WORK/libvorbis-$VORBIS_VERSION/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_PREFIX_PATH="$PREFIX" \
      $CMAKE_PIC \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DBUILD_SHARED_LIBS=OFF \
      $TARGET_CMAKE_EXTRA >/dev/null
cmake --build "$WORK/libvorbis-$VORBIS_VERSION/build" --parallel "$JOBS" \
    >/dev/null
cmake --install "$WORK/libvorbis-$VORBIS_VERSION/build" >/dev/null
echo "  libvorbis installed"

# ------------------------------------------------------------------ libFLAC
#
# BUILD_PROGRAMS off is a licence switch, not a size one. This archive holds
# the library and the flac and metaflac command line tools, and they are not
# under the same licence: libFLAC is BSD and the two tools are GPL. Only the
# library is built. BUILD_CXXLIBS off drops libFLAC++, which is BSD as well
# and which nothing here calls.
#
# Replaces the bundled dr_flac.
echo "building FLAC $FLAC_VERSION"
extract "$FLAC_FILE" "$FLAC_SHA256" "flac-$FLAC_VERSION"
# shellcheck disable=SC2086
cmake -S "$WORK/flac-$FLAC_VERSION" -B "$WORK/flac-$FLAC_VERSION/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_PREFIX_PATH="$PREFIX" \
      $CMAKE_PIC \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_PROGRAMS=OFF -DBUILD_CXXLIBS=OFF \
      -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF \
      -DINSTALL_MANPAGES=OFF \
      -DWITH_OGG=ON \
      $TARGET_CMAKE_EXTRA >/dev/null
cmake --build "$WORK/flac-$FLAC_VERSION/build" --parallel "$JOBS" >/dev/null
cmake --install "$WORK/flac-$FLAC_VERSION/build" >/dev/null
echo "  FLAC installed"

# -------------------------------------------------------------------- Opus
#
# A new format rather than a replacement: nothing bundled decodes Opus.
#
# DRED and OSCE stay off. Both are neural network extensions added in Opus
# 1.5 for patching up badly damaged speech, and each carries model weights
# that would be compiled into every binary for something a local file never
# needs.
echo "building Opus $OPUS_VERSION"
extract "$OPUS_FILE" "$OPUS_SHA256" "opus-$OPUS_VERSION"
# shellcheck disable=SC2086
cmake -S "$WORK/opus-$OPUS_VERSION" -B "$WORK/opus-$OPUS_VERSION/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      $CMAKE_PIC \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_SHARED_LIBRARY=OFF \
      -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF \
      -DOPUS_DRED=OFF -DOPUS_OSCE=OFF \
      $TARGET_CMAKE_EXTRA >/dev/null
cmake --build "$WORK/opus-$OPUS_VERSION/build" --parallel "$JOBS" >/dev/null
cmake --install "$WORK/opus-$OPUS_VERSION/build" >/dev/null
echo "  Opus installed"

# ---------------------------------------------------------------- opusfile
#
# libopus decodes Opus packets; this reads an .opus file. SDL_mixer calls
# this one, not libopus directly.
#
# HTTP off, which is what keeps OpenSSL out: opusfile can fetch a stream over
# the network by itself, and the toolkit already has its own way of being fed
# bytes from a host.
#
# It is the only library here that compiles with warnings, all of them
# -Wmaybe-uninitialized in src/opusfile.c. They are upstream's and they are
# not caused by pinning a commit: building the 0.12 release with the same
# flags produces the same warnings on the same lines. Two invariants the
# compiler cannot follow are behind them. Most are OP_ALWAYS_TRUE, which is
# the author asserting that a call cannot fail and which compiles to
# ((void)(_cond)) with assertions off, erasing the claim before gcc sees it.
# One pairs two variables that are always assigned together across a loop.
# Left alone rather than silenced with -Wno-maybe-uninitialized, which would
# hide a real one in some later version.
#
# The package_version file is written here because this source is a commit
# rather than a release tarball, and a release tarball is where that file
# normally comes from. Without it CMake warns and calls the version 0.0. The
# string is what "git describe" reports for this commit; see
# scripts/versions.sh for why a commit is used at all.
echo "building opusfile $OPUSFILE_VERSION"
extract "$OPUSFILE_FILE" "$OPUSFILE_SHA256" "opusfile-$OPUSFILE_COMMIT"
echo "PACKAGE_VERSION=\"$OPUSFILE_VERSION\"" \
    > "$WORK/opusfile-$OPUSFILE_COMMIT/package_version"
# shellcheck disable=SC2086
cmake -S "$WORK/opusfile-$OPUSFILE_COMMIT" \
      -B "$WORK/opusfile-$OPUSFILE_COMMIT/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_PREFIX_PATH="$PREFIX" \
      $CMAKE_PIC \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DBUILD_SHARED_LIBS=OFF \
      -DOP_DISABLE_HTTP=ON -DOP_DISABLE_EXAMPLES=ON -DOP_DISABLE_DOCS=ON \
      $TARGET_CMAKE_EXTRA >/dev/null
cmake --build "$WORK/opusfile-$OPUSFILE_COMMIT/build" --parallel "$JOBS" \
    >/dev/null
cmake --install "$WORK/opusfile-$OPUSFILE_COMMIT/build" >/dev/null
#
# And its pkg-config file, which that install does not write. opusfile's
# CMake build publishes a CMake package only, while SDL_mixer records
# "Requires.private: opusfile" in its own .pc no matter which it found.
# Without this, pkg-config --static --libs sdl3-mixer fails and nothing
# links. This is upstream's own template with the paths filled in; the
# autotools build it replaced produced exactly this file.
sed -e "s|@prefix@|$PREFIX|" \
    -e "s|@exec_prefix@|\${prefix}|" \
    -e "s|@libdir@|\${prefix}/lib|" \
    -e "s|@includedir@|\${prefix}/include|" \
    -e "s|@PACKAGE_VERSION@|$OPUSFILE_VERSION|" \
    -e "s|@lrintf_lib@|-lm|" \
    "$WORK/opusfile-$OPUSFILE_COMMIT/opusfile.pc.in" \
    > "$PREFIX/lib/pkgconfig/opusfile.pc"
echo "  opusfile installed"

# ---------------------------------------------------------------- WavPack
#
# The other new format. Lossless like FLAC, with a hybrid mode nothing else
# here has: a small lossy file beside a correction file that restores the
# original exactly.
#
# Only the library is wanted. The programs and the two Windows audio editor
# plugins are off, and so is libiconv, which only the programs use for tag
# text.
echo "building WavPack $WAVPACK_VERSION"
extract "$WAVPACK_FILE" "$WAVPACK_SHA256" "wavpack-$WAVPACK_VERSION"
# shellcheck disable=SC2086
cmake -S "$WORK/wavpack-$WAVPACK_VERSION" \
      -B "$WORK/wavpack-$WAVPACK_VERSION/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      $CMAKE_PIC \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
      -DWAVPACK_BUILD_PROGRAMS=OFF \
      -DWAVPACK_BUILD_COOLEDIT_PLUGIN=OFF \
      -DWAVPACK_BUILD_WINAMP_PLUGIN=OFF \
      -DWAVPACK_ENABLE_LIBICONV=OFF \
      -DWAVPACK_INSTALL_DOCS=OFF \
      $TARGET_CMAKE_EXTRA >/dev/null
cmake --build "$WORK/wavpack-$WAVPACK_VERSION/build" --parallel "$JOBS" \
    >/dev/null
cmake --install "$WORK/wavpack-$WAVPACK_VERSION/build" >/dev/null
echo "  WavPack installed"

# ------------------------------------------------------------- SDL_mixer
#
# Decoding. SDL itself reads WAV and nothing else, and this adds MP3, FLAC,
# Ogg Vorbis, Opus and WavPack.
#
# Four of those five come from the libraries built just above rather than from
# the small decoders bundled inside SDL_mixer. Only MP3 still uses a bundled
# one, dr_mp3, because the alternative is libmpg123 and that is copyleft.
#
# The two lines that turn dr_flac and stb_vorbis off matter: leaving them on
# would compile both decoders for those formats into the binary and use only
# one. Their FLAC and Vorbis switches are the choice between the two.
#
# Everything else switched off below is switched off for a licence rather than
# a preference. libxmp, libmpg123, FluidSynth and game-music-emu are copyleft,
# and everything here is linked statically into one archive, so they suit the
# GPL build and not the commercial one. MIDI goes with FluidSynth: the only
# other synthesiser is the bundled Timidity, whose own licence could not be
# established, and a MIDI file needs a patch set on disk to make a sound
# anyway. See THIRD_PARTY_NOTICES.md.
echo "building SDL_mixer $MIX_VERSION"
extract "$MIX_FILE" "$MIX_SHA256" "SDL3_mixer-$MIX_VERSION"
cmake -S "$WORK/SDL3_mixer-$MIX_VERSION" \
      -B "$WORK/SDL3_mixer-$MIX_VERSION/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_PREFIX_PATH="$PREFIX" \
      $CMAKE_PIC \
      -DBUILD_SHARED_LIBS=OFF \
      -DSDLMIXER_VENDORED=OFF -DSDLMIXER_DEPS_SHARED=OFF \
      -DSDLMIXER_TESTS=OFF -DSDLMIXER_EXAMPLES=OFF \
      -DSDLMIXER_SAMPLES=OFF \
      -DSDLMIXER_MOD=OFF \
      -DSDLMIXER_GME=OFF \
      -DSDLMIXER_MIDI=OFF \
      -DSDLMIXER_MP3_MPG123=OFF \
      -DSDLMIXER_FLAC_LIBFLAC=ON -DSDLMIXER_FLAC_DRFLAC=OFF \
      -DSDLMIXER_VORBIS_VORBISFILE=ON -DSDLMIXER_VORBIS_STB=OFF \
      -DSDLMIXER_OPUS=ON \
      -DSDLMIXER_WAVPACK=ON \
      $TARGET_CMAKE_EXTRA >/dev/null
cmake --build "$WORK/SDL3_mixer-$MIX_VERSION/build" --parallel "$JOBS" \
    >/dev/null
cmake --install "$WORK/SDL3_mixer-$MIX_VERSION/build" >/dev/null
echo "  SDL_mixer installed"

# ----------------------------------------------------------------- ThorVG
#
# engines=cpu     the software rasterizer, the one path used on every target
# bindings=capi   the C API; off by default, and Schultz is C
# loaders         png, jpg and webp added for the Image widget
# threads=false   deterministic; the frame budget work turns this on
# partial=true    ThorVG's own dirty region rendering
# extra           openmp is dropped, since threads are off anyway and it drags
#                 libgomp in for nothing. Lottie expressions stay.
#
# Not set: static=true. It switches ThorVG to its own bundled PNG and WebP
# decoders instead of the system ones, which would make the binary
# self-contained, and it stops images rendering: with it the toolkit's own
# image tests draw nothing at all, every pixel left untouched. Until that is
# understood, the system decoders are used and the binary needs libpng and
# libwebp beside it. A cross build finds neither and does without them, so
# this costs image decoding on a phone rather than on a desktop.
echo "building ThorVG $TVG_VERSION"
extract "$TVG_FILE" "$TVG_SHA256" "thorvg-$TVG_VERSION"
# shellcheck disable=SC2046
#
# pkg-config is pointed at this prefix and nowhere else for this one build,
# and every library it may ask for is in it.
#
# ThorVG's three image loaders each look for an outside library and fall back
# to one it carries:
#
#   if jpg_loader
#       if get_option('static')  subdir('jpg')           <- the one it carries
#       else                     subdir('external_jpg')  <- libturbojpeg
#            if not jpg_dep.found()  subdir('jpg')
#
# so which decoder ships was decided by what happened to be installed on the
# machine doing the building. libpng and libwebp were always here and always
# found; libturbojpeg was not vendored, so a machine with Homebrew's copy
# built against it and a machine without used the one ThorVG carries. Now it
# is vendored too and all three are answered from this prefix on every
# machine.
#
# -Dstatic=true is the other way to settle it, forcing all three of ThorVG's
# own decoders, and it is not used: it was tried, and rendering an image
# through the ones it carries fails two of the render tests.
#
# Both variables are set. PKG_CONFIG_LIBDIR replaces the built in system
# directories and PKG_CONFIG_PATH is searched as well as them, so narrowing
# one and leaving the other still finds whatever the environment offered.
PKG_CONFIG_PATH="$PC_PATH" PKG_CONFIG_LIBDIR="$PC_PATH" \
meson setup "$WORK/thorvg-$TVG_VERSION/build" "$WORK/thorvg-$TVG_VERSION" \
    $(meson_args) --prefix="$PREFIX" --buildtype=release \
    --default-library=static $MESON_PIC \
    -Dengines=cpu -Dbindings=capi \
    -Dloaders=svg,lottie,ttf,png,jpg,webp \
    -Dthreads=false -Dpartial=true -Dtools='' -Dtests=false \
    -Dextra=lottie_exp >/dev/null
ninja -C "$WORK/thorvg-$TVG_VERSION/build" >/dev/null
ninja -C "$WORK/thorvg-$TVG_VERSION/build" install >/dev/null
echo "  ThorVG installed"

# --------------------------------------------------------------- FreeType
#
# harfbuzz=disabled breaks a circular dependency: FreeType can use HarfBuzz
# for autohinting and HarfBuzz can use FreeType for font access.
#
# zlib=internal uses FreeType's own copy rather than the system one, so a
# compressed font still loads without anything installed beside the binary.
# PNG is left on automatic: where libpng exists it is used, for colour bitmap
# fonts, and where it does not FreeType does without it.
echo "building FreeType $FT_VERSION"
extract "$FT_FILE" "$FT_SHA256" "freetype-$FT_VERSION"
# shellcheck disable=SC2046
meson setup "$WORK/freetype-$FT_VERSION/build" "$WORK/freetype-$FT_VERSION" \
    $(meson_args) --prefix="$PREFIX" --buildtype=release \
    --default-library=static $MESON_PIC \
    -Dharfbuzz=disabled -Dbrotli=disabled -Dbzip2=disabled \
    -Dzlib=internal >/dev/null
ninja -C "$WORK/freetype-$FT_VERSION/build" >/dev/null
ninja -C "$WORK/freetype-$FT_VERSION/build" install >/dev/null
echo "  FreeType installed"

# --------------------------------------------------------------- HarfBuzz
#
# Only the shaping core. glib, gobject, cairo, icu and the utilities are all
# off, which keeps the dependency surface down to FreeType.
echo "building HarfBuzz $HB_VERSION"
extract "$HB_FILE" "$HB_SHA256" "harfbuzz-$HB_VERSION"
# shellcheck disable=SC2046
meson setup "$WORK/harfbuzz-$HB_VERSION/build" "$WORK/harfbuzz-$HB_VERSION" \
    $(meson_args) --prefix="$PREFIX" --buildtype=release \
    --default-library=static $MESON_PIC \
    -Dfreetype=enabled -Dglib=disabled -Dgobject=disabled \
    -Dcairo=disabled -Dicu=disabled -Dchafa=disabled \
    -Dtests=disabled -Ddocs=disabled -Dintrospection=disabled \
    -Dutilities=disabled -Dbenchmark=disabled >/dev/null
ninja -C "$WORK/harfbuzz-$HB_VERSION/build" >/dev/null
ninja -C "$WORK/harfbuzz-$HB_VERSION/build" install >/dev/null
echo "  HarfBuzz installed"

# -------------------------------------------------------------- SheenBidi
echo "building SheenBidi $SB_VERSION"
extract "$SB_FILE" "$SB_SHA256" "SheenBidi-$SB_VERSION"
# shellcheck disable=SC2046
meson setup "$WORK/SheenBidi-$SB_VERSION/build" "$WORK/SheenBidi-$SB_VERSION" \
    $(meson_args) --prefix="$PREFIX" --buildtype=release \
    --default-library=static $MESON_PIC >/dev/null
ninja -C "$WORK/SheenBidi-$SB_VERSION/build" >/dev/null
ninja -C "$WORK/SheenBidi-$SB_VERSION/build" install >/dev/null
echo "  SheenBidi installed"

# libunibreak is not built here. It is vendored as source in
# third_party/libunibreak and compiled straight into Schultz, because it is
# plain ISO C with no configuration step and shipping autotools made it the
# only dependency needing a hand written cross build.
# See third_party/libunibreak/README.md.

# ----------------------------------------------------------- bundled font
#
# Extracted once into assets/, not per target: a font file is the same bytes
# everywhere, and the toolkit ships it rather than discovering one.
FONT_DIR="$ROOT/assets/fonts"
if [ ! -f "$FONT_DIR/DejaVuSans.ttf" ]; then
    echo "extracting DejaVu $FONT_VERSION"
    mkdir -p "$FONT_DIR"
    extract "$FONT_FILE" "$FONT_SHA256" "dejavu-fonts-ttf-$FONT_VERSION"
    _f="$WORK/dejavu-fonts-ttf-$FONT_VERSION"
    cp "$_f/ttf/DejaVuSans.ttf" "$_f/ttf/DejaVuSans-Bold.ttf" \
       "$_f/ttf/DejaVuSansMono.ttf" "$FONT_DIR/"
    cp "$_f/LICENSE" "$FONT_DIR/LICENSE-DejaVu.txt"
    echo "  fonts installed into assets/fonts"
fi

# An env file per target, so a shell and the Makefile find this target's
# libraries without hardcoding anyone's layout.
_libpath=""
for _d in $TARGET_LIBDIRS; do
    _libpath="$_libpath$PREFIX/$_d:"
done
# The same facts in make's syntax, for the toolkit's own build. Without this
# a cross target builds its dependencies for the target and the toolkit for
# the machine doing the building, and the two only fail to fit at link time.
cat > "$OUT/config.mk" <<MKEOF
# Generated by scripts/build_deps.sh for $DEPS_NAME. Do not edit.
TARGET_TOOLKIT_CC     := $TARGET_TOOLKIT_CC
TARGET_TOOLKIT_CFLAGS := $TARGET_TOOLKIT_CFLAGS
TARGET_TOOLKIT_AR     := $TARGET_TOOLKIT_AR
TARGET_TOOLKIT_RANLIB := $TARGET_TOOLKIT_RANLIB
TARGET_PC_PATH        := $PC_PATH
TARGET_TOOLKIT_LDLIBS := $TARGET_TOOLKIT_LDLIBS
TARGET_CROSS          := $TARGET_CROSS
MKEOF

cat > "$OUT/env.sh" <<ENVEOF
# Generated by scripts/build_deps.sh for $DEPS_NAME. Do not edit.
SCHULTZ_TARGET="$DEPS_NAME"
SCHULTZ_PREFIX="$PREFIX"
export SCHULTZ_TARGET SCHULTZ_PREFIX
export PKG_CONFIG_PATH="$PC_PATH\${PKG_CONFIG_PATH:-}"
export $TARGET_SHLIB_VAR="$_libpath\${$TARGET_SHLIB_VAR:-}"
ENVEOF

echo
for _p in sdl3 sdl3-mixer thorvg-1 freetype2 harfbuzz sheenbidi zlib \
         libpng16 libwebp libturbojpeg ogg vorbisfile flac opus opusfile \
         wavpack vpx; do
    printf "  %-12s %s\n" "$_p" \
        "$(PKG_CONFIG_PATH="$PC_PATH" pkg-config --modversion "$_p" 2>/dev/null || echo '?')"
done

# ---------------------------------------------- does this prefix stand alone
#
# Everything above is built so that a program can be linked against this
# prefix and nothing else. That is an assumption until it is asked, and the
# way it fails is quiet: a library that finds a system package during its own
# configure records a dependency on it, the build succeeds, and the prefix
# only turns out to be incomplete later, somewhere else, in a message about
# whatever was looking.
#
# So pkg-config is asked to resolve the whole set with the system directories
# taken away. Anything named here that is not in this prefix is a library that
# picked something up from the machine.
if ! PKG_CONFIG_PATH="$PC_PATH" PKG_CONFIG_LIBDIR="$PC_PATH" \
     pkg-config --exists sdl3 sdl3-mixer thorvg-1 freetype2 harfbuzz sheenbidi \
     2>/dev/null; then
    echo >&2
    echo "this prefix depends on something outside itself:" >&2
    PKG_CONFIG_PATH="$PC_PATH" PKG_CONFIG_LIBDIR="$PC_PATH" \
        pkg-config --print-errors --exists \
        sdl3 sdl3-mixer thorvg-1 freetype2 harfbuzz sheenbidi 2>&1 \
        | sed 's/^/  /' >&2
    echo >&2
    echo "A library found that package on this machine during its own" >&2
    echo "configure and recorded it. Whatever is built here would then need" >&2
    echo "it too, and it is not vendored, so the result is not portable and" >&2
    echo "on a second architecture would not even be the right one." >&2
    exit 1
fi

echo
echo "env:     build-deps/$DEPS_NAME/env.sh"
echo "make:    build-deps/$DEPS_NAME/config.mk"
