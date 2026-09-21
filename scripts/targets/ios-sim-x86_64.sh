# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# The iOS simulator on an Intel Mac.
#
# This is the pair for ios-arm64 when developing on Intel hardware: the
# simulator runs the host architecture, so an Intel Mac needs an x86-64
# simulator build. On Apple Silicon the simulator is arm64 instead.
TARGET_CROSS=1
TARGET_SHLIB_VAR=DYLD_LIBRARY_PATH
TARGET_LIBDIRS="lib lib64"

TARGET_SDK=iphonesimulator
TARGET_TRIPLE=x86_64-apple-ios-simulator
TARGET_MESON_CPU_FAMILY=x86_64
TARGET_MESON_CPU=x86_64
# 14.0 rather than 13.0 because of the file picker.
#
# The modern document picker, and the UTType it takes its content types from,
# both arrived in iOS 14. The older calls still exist and are deprecated, and
# carrying both paths would mean writing and never testing one of them for a
# release that came out in 2019. Overridable, for anyone who has a reason.
TARGET_MIN_VERSION=${TARGET_MIN_VERSION:-14.0}
TARGET_SYSROOT=$(xcrun --sdk "$TARGET_SDK" --show-sdk-path)
TARGET_CFLAGS_EXTRA="-arch x86_64 -isysroot $TARGET_SYSROOT -mios-simulator-version-min=$TARGET_MIN_VERSION"

TARGET_CMAKE_EXTRA="-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=x86_64 \
 -DCMAKE_OSX_DEPLOYMENT_TARGET=$TARGET_MIN_VERSION \
 -DCMAKE_OSX_SYSROOT=$TARGET_SDK"

# The toolkit's own compile. The dependency builds get these through meson's
# cross file; Schultz itself is built by its Makefile, which reads them from
# the config.mk that build_deps.sh writes.
TARGET_TOOLKIT_CC=clang
TARGET_TOOLKIT_CFLAGS="$TARGET_CFLAGS_EXTRA"

# As macOS, but the accessibility backend answers UIKit rather than AppKit.
# UniformTypeIdentifiers is what the file picker names its content types
# with. See schultz_files_ios.m.
TARGET_TOOLKIT_LDLIBS="-lc++ -lm -framework Foundation -framework UIKit -framework UniformTypeIdentifiers"

# Which libvpx target to configure. x86 simulator: pure C
TARGET_VPX=generic-gnu
