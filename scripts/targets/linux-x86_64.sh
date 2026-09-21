# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# Linux on x86-64. The host build on an ordinary desktop.
TARGET_CROSS=0
TARGET_SHLIB_VAR=LD_LIBRARY_PATH
TARGET_LIBDIRS="lib lib64 lib/x86_64-linux-gnu"

# Which libvpx target to configure. x86 needs nasm or yasm for its assembly; generic-gnu is pure C and needs neither
TARGET_VPX=generic-gnu
