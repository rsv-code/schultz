# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Windows on x86-64, built under MSYS2 in its MINGW64 shell.
#
# Git Bash is not enough: it ships no compiler and no build tools. Install
# MSYS2 and, in the MINGW64 shell:
#
#   pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
#                      mingw-w64-x86_64-meson mingw-w64-x86_64-ninja \
#                      mingw-w64-x86_64-pkgconf tar
#
# There is no rpath on Windows. A built .dll is found through PATH, which is
# why TARGET_SHLIB_VAR is PATH here.
TARGET_CROSS=0
TARGET_SHLIB_VAR=PATH
TARGET_LIBDIRS="lib bin"
TARGET_CMAKE_EXTRA="-G Ninja"

# Which libvpx target to configure. pure C, and MinGW has no nasm by default
TARGET_VPX=generic-gnu
