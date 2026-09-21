# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# macOS on Apple Silicon. Built on the Mac itself with the Xcode command line tools.
#
# Not a cross build: clang targets the machine it runs on. The deployment
# target is set low enough to run on anything still receiving updates.
TARGET_CROSS=0
TARGET_SHLIB_VAR=DYLD_LIBRARY_PATH
TARGET_LIBDIRS="lib lib64"
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
TARGET_MESON_CPU_FAMILY=aarch64
TARGET_MESON_CPU=aarch64
TARGET_CMAKE_EXTRA="-DCMAKE_OSX_ARCHITECTURES=arm64"
# The same thing said in the form meson takes. A Mac builds for
# itself unless told otherwise, and cmake and meson have to be told
# separately: without this the four meson libraries come out for the
# machine while the eleven cmake ones come out for the target, and
# the prefix holds both architectures without saying so.
TARGET_CFLAGS_EXTRA="-arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"

# The toolkit's own compile. A Mac can build either architecture whichever it
# runs on, so the architecture has to be said out loud here as well, or the
# toolkit comes out matching the machine instead of the target.
TARGET_TOOLKIT_CC=clang
TARGET_TOOLKIT_CFLAGS="$TARGET_CFLAGS_EXTRA"

# Apple dropped libstdc++ with Xcode 10; clang's C++ runtime is libc++, and
# ThorVG is C++. Foundation and AppKit are for the accessibility backend,
# which answers VoiceOver's questions on an NSView.
TARGET_TOOLKIT_LDLIBS="-lc++ -lm -framework Foundation -framework AppKit"

# Which libvpx target to configure. NEON through intrinsics, no assembler
TARGET_VPX=arm64-darwin-gcc
