# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Android on x86-64, for the emulator, cross compiled from Linux or macOS with the NDK.
#
# Set ANDROID_NDK_HOME to the NDK root before running. API 24 is the oldest
# level SDL3 supports.
TARGET_CROSS=1
TARGET_SHLIB_VAR=LD_LIBRARY_PATH
TARGET_LIBDIRS="lib lib64"

: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the Android NDK root}"
TARGET_ANDROID_ABI=x86_64
TARGET_ANDROID_API=${TARGET_ANDROID_API:-24}
TARGET_TRIPLE=x86_64-linux-android
TARGET_MESON_CPU_FAMILY=x86_64
TARGET_MESON_CPU=x86_64

TARGET_CMAKE_EXTRA="-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
 -DANDROID_ABI=$TARGET_ANDROID_ABI -DANDROID_PLATFORM=android-$TARGET_ANDROID_API"

# The toolkit's own compile. The NDK ships one wrapper per ABI and API level,
# and the wrapper already carries the target, the sysroot and the runtime, so
# no extra flags are needed. llvm-ar rather than the host's, because the host
# ar cannot index an archive for another architecture.
TARGET_NDK_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64/bin"
TARGET_TOOLKIT_CC="$TARGET_NDK_BIN/${TARGET_TRIPLE}${TARGET_ANDROID_API}-clang"
TARGET_TOOLKIT_AR="$TARGET_NDK_BIN/llvm-ar"
TARGET_TOOLKIT_RANLIB="$TARGET_NDK_BIN/llvm-ranlib"

# Which libvpx target to configure. x86 again: pure C rather than an assembler
TARGET_VPX=generic-gnu
