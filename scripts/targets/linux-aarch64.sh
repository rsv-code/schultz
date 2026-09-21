# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Linux on 64 bit Arm. A Raspberry Pi, or any Arm board or server.
#
# This is the target a Pi detects as, so a native build on the board picks it
# up with no arguments. Cross compiling to it from an x86-64 machine is not
# set up: that needs a toolchain and a cross file, and the machinery here has
# those only for Android and iOS. Building on the board works and is what the
# detection expects.
#
# Nothing about Arm needs saying to the compiler. Every 64 bit Arm chip has
# the vector instructions a rasterizer wants, so there is no baseline to
# raise the way there is on x86.
TARGET_CROSS=0
TARGET_SHLIB_VAR=LD_LIBRARY_PATH
# Debian and its derivatives, which is what a Pi runs, install to a directory
# named after the architecture. Meson follows that; cmake and autotools tend
# to use plain lib. All three are listed so pkg-config finds whichever was
# used.
TARGET_LIBDIRS="lib lib64 lib/aarch64-linux-gnu"

# Which libvpx target to configure. NEON here is C intrinsics, so this needs no assembler
TARGET_VPX=arm64-linux-gcc
