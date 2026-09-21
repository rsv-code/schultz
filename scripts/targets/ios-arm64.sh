# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# iOS on arm64, for a real device. Cross compiled on a Mac with Xcode.
#
# An Intel Mac builds this fine: the compiler targets arm64 whatever it runs
# on. For the simulator on an Intel Mac use ios-sim-x86_64 instead.
TARGET_CROSS=1
TARGET_SHLIB_VAR=DYLD_LIBRARY_PATH
TARGET_LIBDIRS="lib lib64"

TARGET_SDK=iphoneos
TARGET_TRIPLE=arm64-apple-ios
TARGET_MESON_CPU_FAMILY=aarch64
TARGET_MESON_CPU=aarch64
# 14.0 rather than 13.0 because of the file picker.
#
# The modern document picker, and the UTType it takes its content types from,
# both arrived in iOS 14. The older calls still exist and are deprecated, and
# carrying both paths would mean writing and never testing one of them for a
# release that came out in 2019. Overridable, for anyone who has a reason.
TARGET_MIN_VERSION=${TARGET_MIN_VERSION:-14.0}
TARGET_SYSROOT=$(xcrun --sdk "$TARGET_SDK" --show-sdk-path)
TARGET_CFLAGS_EXTRA="-arch arm64 -isysroot $TARGET_SYSROOT -miphoneos-version-min=$TARGET_MIN_VERSION"

TARGET_CMAKE_EXTRA="-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
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

# Which libvpx target to configure. libvpx has no iOS target; the darwin one plus the SDK flags is what it wants
TARGET_VPX=arm64-darwin-gcc
