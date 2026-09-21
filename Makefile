# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman

# Makefile for the schultz project.

CC       := gcc
AR       := ar
RANLIB   := ranlib
# -fPIC: the archives have to be usable inside a shared library, which is how
# a Java binding loads this. Ordinary code cannot go in one.
#
# -fvisibility=hidden: nothing is exported from a shared library unless it
# says so. The public headers say so, with a visibility pragma around their
# declarations, so what comes out is schultz_* and not the whole of FreeType,
# libpng and zlib as well. A shared library that exports those meets the
# JVM's copies of them and one of the two wins.
# Hardening. These cost nothing at run time worth measuring and turn a class
# of memory error into a clean abort instead of a foothold:
#
#   _FORTIFY_SOURCE  checks the size of buffers the compiler can see through,
#                    at the calls that overrun them most often. Needs an
#                    optimizing build, which is why it is not in the debug
#                    flags below.
#   stack-protector  puts a canary in front of the return address in any
#                    function with a local array or an address taken.
#   -Wformat=2       warns about format strings that are not literals and
#                    about arguments that do not match them.
CFLAGS   := -std=c11 -Wall -Wextra -Wformat=2 -O2 -fPIC -fvisibility=hidden \
            -D_FORTIFY_SOURCE=3 -fstack-protector-strong
LDFLAGS  :=
# The maths library: the widgets draw an arc with trigonometry, and glibc
# keeps those out of libc.
# ThorVG is C++, and a static archive does not carry its own runtime the way
# a shared library does: libthorvg.so recorded a dependency on libstdc++,
# libthorvg.a cannot. ThorVG's pkg-config file does not name it either, so it
# is named here.
LDLIBS   := -lstdc++ -lm

TARGET   := schultz_demo
BUILD    ?= build

# Vendored dependencies, built per target by scripts/build_deps.sh.
# Which target's dependencies to link against. The host by default; set it on
# the command line to use another, for example
#   make DEPS_TARGET=android-arm64
# Not to be confused with TARGET above, which is the name of the binary.
# Named separately so that "is this a cross build" can be asked without
# running the detection a second time.
HOST_TARGET  := $(shell sh scripts/build_deps.sh --print-target)
DEPS_TARGET  ?= $(HOST_TARGET)
# What to tell somebody to run when those dependencies are not there. The
# script detects the machine it is on, so --target is worth naming only for a
# target that is not this machine. Telling a Windows user to pass
# --target windows-x86_64 invites the reasonable question of why it cannot
# work that out, when it already has.
# A fuzzing target is this machine with instrumented dependencies, not another
# machine. Anything below that asks "is this the host" has to see through the
# suffix, or a fuzz build starts looking for cross compiled pieces that were
# never going to exist.
DEPS_BASE_TARGET := $(patsubst %-fuzz,%,$(DEPS_TARGET))
ifeq ($(DEPS_TARGET),$(HOST_TARGET))
DEPS_COMMAND := sh scripts/build_deps.sh
else ifeq ($(DEPS_TARGET),$(HOST_TARGET)-fuzz)
DEPS_COMMAND := sh scripts/build_deps.sh --fuzz
else
DEPS_COMMAND := sh scripts/build_deps.sh --target $(DEPS_TARGET)
endif
PREFIX_DIR   := build-deps/$(DEPS_TARGET)/prefix

# The toolchain that target was built with, written by build_deps.sh. A native
# target leaves every field empty and the host's own tools are kept. A cross
# target names a compiler, and without it the dependencies would be built for
# the target while the toolkit was built for this machine.
-include build-deps/$(DEPS_TARGET)/config.mk
# And every object is made to depend on that file, so that a change to it
# rebuilds them.
#
# It names the compiler and the architecture, and neither of those is visible
# to make otherwise: an object compiled before this file existed is a perfectly
# good object, newer than its source, and make has no reason to touch it again.
# What comes out is an archive holding two architectures at once, which ar
# reports as a member whose cputype does not match the members before it.
#
# Through wildcard because it is allowed to be absent. A missing file as a
# prerequisite is an error; a missing file through wildcard is nothing at all,
# and it appears as a prerequisite the moment the target is built.
TARGET_CONFIG := $(wildcard build-deps/$(DEPS_TARGET)/config.mk)
ifneq ($(TARGET_TOOLKIT_CC),)
CC := $(TARGET_TOOLKIT_CC)
endif
ifneq ($(TARGET_TOOLKIT_AR),)
AR := $(TARGET_TOOLKIT_AR)
endif
ifneq ($(TARGET_TOOLKIT_RANLIB),)
RANLIB := $(TARGET_TOOLKIT_RANLIB)
endif
# Apple has no libstdc++, and its accessibility backend needs frameworks, so
# a target may replace the link line rather than add to it.
ifneq ($(TARGET_TOOLKIT_LDLIBS),)
LDLIBS := $(TARGET_TOOLKIT_LDLIBS)
endif
CFLAGS  += $(TARGET_TOOLKIT_CFLAGS)
LDFLAGS += $(TARGET_TOOLKIT_CFLAGS)

# Three source groups.
#
#   MAIN_SRC     holds main(), so it is kept out of what the tests link.
#   BACKEND_SRCS need SDL3, and so need a display. Keeping them separate is
#                what lets every test run headless on any machine, which is
#                worth protecting.
#   LIB_SRCS     the dependency free core: everything else.
MAIN_SRC     := schultz_demo.c

# Accessibility is one shared file and one platform file. Describing a tree
# is the same everywhere; handing it to the platform is not, so exactly one
# of these is compiled, chosen by the target's name. See
# schultz_a11y_backend.h.
#
# A target with no backend of its own gets the do nothing one, so it builds
# and runs while its accessibility is still being written.
A11Y_SRCS    := $(wildcard schultz_a11y_*.c) $(wildcard schultz_a11y_*.m)
ifneq ($(filter linux-%,$(DEPS_TARGET)),)
A11Y_BACKEND := schultz_a11y_atspi.c
else ifneq ($(filter windows-%,$(DEPS_TARGET)),)
A11Y_BACKEND := schultz_a11y_uia.c
else ifneq ($(filter macos-%,$(DEPS_TARGET)),)
A11Y_BACKEND := schultz_a11y_macos.m
else ifneq ($(filter ios-%,$(DEPS_TARGET)),)
A11Y_BACKEND := schultz_a11y_ios.m
else ifneq ($(filter android-%,$(DEPS_TARGET)),)
A11Y_BACKEND := schultz_a11y_android.c
else
A11Y_BACKEND := schultz_a11y_none.c
endif

# The clipboard is SDL's everywhere but Windows, which carries rich text in a
# registered format SDL has no code for. See schultz_clipboard_windows.c.
ifneq ($(filter windows-%,$(DEPS_TARGET)),)
CLIPBOARD_BACKEND := schultz_clipboard_windows.c
CFLAGS += -DSCHULTZ_CLIPBOARD_WINDOWS
else
CLIPBOARD_BACKEND :=
endif

# File dialogs are SDL's everywhere but iOS, which SDL has no backend for at
# all: its build selects the cocoa one with elseif(MACOS), iOS matches no
# branch, and what gets compiled is a dummy that reports failure without
# showing anything. That is why a picker on iOS looked dismissed the instant
# it opened. See schultz_files_backend.h.
FILES_SRCS   := $(wildcard schultz_files_*.c) $(wildcard schultz_files_*.m)
ifneq ($(filter ios-%,$(DEPS_TARGET)),)
FILES_BACKEND := schultz_files_ios.m
else
FILES_BACKEND := schultz_files_sdl.c
endif

BACKEND_SRCS := schultz_sdl.c schultz_window.c schultz_a11y.c schultz_audio.c \
                $(A11Y_BACKEND) $(CLIPBOARD_BACKEND) $(FILES_BACKEND)
# The text pipeline needs FreeType, HarfBuzz, SheenBidi and libunibreak, but
# not SDL or ThorVG, so its tests run without a display.
TEXT_SRCS    := schultz_font.c schultz_text.c schultz_glyphs.c \
                schultz_widgets.c
# These need ThorVG, which decodes and rasterizes, but not SDL, so their tests
# run without a display just as the text ones do. Rendering into a buffer is
# exactly what the tests want: a rendered picture can be inspected pixel by
# pixel with no window anywhere.
# schultz_colr.c is here rather than with the text sources because it
# draws with the vector engine; it needs FreeType's headers as well,
# which is why the rule below carries both sets of flags.
TVG_SRCS     := schultz_thorvg.c schultz_image.c schultz_render.c \
                  schultz_colr.c
# Video needs the nestegg headers as well as the toolkit's own, so it is
# compiled with NE_CFLAGS on top of everything else. libvpx arrives through
# pkg-config like the other linked libraries.
VIDEO_SRCS   := schultz_video.c schultz_camera.c schultz_webm.c
# libunibreak, vendored as source in third_party/libunibreak and compiled in
# rather than linked. It is plain ISO C with no configuration step, so every
# target builds it the same way and there is nothing to cross compile. This
# list is upstream's own, from its Makefile.am: the remaining *data.c files
# are #included by these and must not be compiled separately.
# See third_party/libunibreak/README.md.
# nestegg reads WebM, which is the container video arrives in. One C file and
# one header, plain C with no configuration step, so it is compiled in rather
# than linked for the same reason libunibreak is. Its header includes itself
# as <nestegg/nestegg.h>, so the include path is the directory above that.
# See third_party/nestegg/README.md.
NE_DIR       := third_party/nestegg
NE_SRCS      := $(NE_DIR)/src/nestegg.c
NE_CFLAGS    := -isystem $(NE_DIR)/include

# speexdsp cleans up microphone sound before it is sent: echo cancellation,
# noise suppression, gain and voice detection. Compiled in rather than built
# as a library for the same reason libunibreak is, and one more: it is the
# only dependency here that is autotools only, and every other library builds
# with cmake. third_party/speexdsp/config.h is what its configure would have
# written. See third_party/speexdsp/README.md.
SDSP_DIR     := third_party/speexdsp
SDSP_SRCS    := $(addprefix $(SDSP_DIR)/libspeexdsp/, \
                  buffer.c fftwrap.c filterbank.c jitter.c kiss_fft.c \
                  kiss_fftr.c mdf.c preprocess.c resample.c scal.c \
                  smallft.c)
SDSP_CFLAGS  := -DHAVE_CONFIG_H -I$(SDSP_DIR) -isystem $(SDSP_DIR)/include
# schultz_voice.c includes <speex/...>, so it needs the include path too, but
# not the config header: that is speexdsp's own business.
VOICE_CFLAGS := -isystem $(SDSP_DIR)/include

UB_DIR       := third_party/libunibreak
UB_SRCS      := $(addprefix $(UB_DIR)/, \
                  unibreakbase.c unibreakdef.c linebreak.c linebreakdata.c \
                  linebreakdef.c eastasianwidthdef.c emojidef.c \
                  graphemebreak.c wordbreak.c)
# A11Y_SRCS and FILES_SRCS rather than the _BACKEND of each: every backend has
# to be kept out of the core, not only the one this target builds. Miss one
# and an iOS build compiles schultz_files_sdl.c into the core beside the
# Objective-C backend, and the link stops on a function defined twice.
CORE_SRCS    := $(filter-out $(MAIN_SRC) $(BACKEND_SRCS) $(TEXT_SRCS) \
                  $(TVG_SRCS) $(VIDEO_SRCS) $(A11Y_SRCS) $(FILES_SRCS) \
                  schultz_clipboard_windows.c,$(wildcard *.c))
LIB_SRCS     := $(CORE_SRCS) $(TEXT_SRCS) $(TVG_SRCS) $(VIDEO_SRCS) \
                  $(UB_SRCS) $(NE_SRCS) $(SDSP_SRCS)
SRCS         := $(MAIN_SRC) $(BACKEND_SRCS) $(LIB_SRCS)
# .m as well as .c. A plain $(SRCS:%.c=...) leaves an Objective-C source
# untouched, and it then travels as itself into the object list and the
# dependency list, where make tries to read the source as a makefile.
OBJS         := $(patsubst %.m,$(BUILD)/%.o,\
                  $(patsubst %.c,$(BUILD)/%.o,$(SRCS)))
LIB_OBJS     := $(LIB_SRCS:%.c=$(BUILD)/%.o)
TEXT_OBJS    := $(TEXT_SRCS:%.c=$(BUILD)/%.o)
UB_OBJS      := $(UB_SRCS:%.c=$(BUILD)/%.o)
NE_OBJS      := $(NE_SRCS:%.c=$(BUILD)/%.o)
SDSP_OBJS    := $(SDSP_SRCS:%.c=$(BUILD)/%.o)
VIDEO_OBJS   := $(VIDEO_SRCS:%.c=$(BUILD)/%.o)
TVG_OBJS     := $(TVG_SRCS:%.c=$(BUILD)/%.o)
# .m as well as .c, because two of the backends are Objective-C. They are
# also kept apart, because a static pattern rule fixes the suffix of the
# prerequisite for every target it names: one rule cannot serve both.
# The bundled faces, turned into a C source file by scripts/embed_fonts.sh so
# a program that links Schultz draws text without finding a file at runtime.
#
# Generated rather than checked in. The bytes already live in assets/fonts,
# and as C escapes the same 1.7 MB comes to about 7.5 MB of source: the same
# data twice, in a form nobody reads and git cannot pack.
FONT_SRC     := $(BUILD)/schultz_font_builtin.c
FONT_OBJ     := $(BUILD)/schultz_font_builtin.o
FONT_FILES   := $(wildcard assets/fonts/*.ttf)
LIB_OBJS     += $(FONT_OBJ)
OBJS         += $(FONT_OBJ)
BACKEND_OBJC_OBJS := $(patsubst %.m,$(BUILD)/%.o,$(filter %.m,$(BACKEND_SRCS)))
BACKEND_C_OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(filter %.c,$(BACKEND_SRCS)))
BACKEND_OBJS      := $(BACKEND_C_OBJS) $(BACKEND_OBJC_OBJS)
BIN          := $(BUILD)/$(TARGET)
# The toolkit as one archive, for a project that consumes Schultz rather than
# runs the demo. Everything but the demo's own main goes in, including the
# platform layer and the vendored libunibreak.
LIB          := $(BUILD)/libschultz.a
# The same toolkit with every vendored dependency merged in, so a consumer can
# link one file instead of ten. That is what a mobile build wants.
LIB_ALL      := $(BUILD)/libschultz_all.a
# And the same thing again as a shared library, for a consumer that would
# rather load one file than link one: a language binding, above all, which is
# how the Java side takes it.
#
# Self contained like the archive it is made from. There is no arrangement in
# which somebody supplies their own SDL or their own FreeType to this, so it
# carries all of them.
ifneq ($(filter windows-%,$(DEPS_TARGET)),)
SHARED       := $(BUILD)/schultz.dll
SHARED_IMP   := $(BUILD)/libschultz.dll.a
SHARED_LIST  := $(BUILD)/schultz.def
else ifneq ($(filter macos-% ios-%,$(DEPS_TARGET)),)
SHARED       := $(BUILD)/libschultz.dylib
SHARED_IMP   :=
SHARED_LIST  := $(BUILD)/schultz.syms
else
SHARED       := $(BUILD)/libschultz.so
SHARED_IMP   :=
SHARED_LIST  := $(BUILD)/schultz.map
endif
# nm reads the archive to write the Windows export list. The other two
# platforms take a pattern and need no such list built.
NM           ?= nm

# macOS ships one library that runs on both architectures, and asking for it
# is the ordinary "make shared".
#
# Apple Silicon and Intel are separate targets here, each with its own prefix
# and its own dependency build, so a universal library is two builds and a
# lipo. Doing that by hand is a step somebody forgets once and then ships an
# Intel only library, which loads on nothing newer and is not caught by any
# build that succeeded.
#
# It cannot be one pass. ThorVG, FreeType, HarfBuzz and SheenBidi are built
# with meson and meson has no fat binary support: one build directory, one
# architecture. The eleven cmake ones would take -DCMAKE_OSX_ARCHITECTURES
# with both and it would not help while those four cannot.
#
# Naming a target explicitly still builds that one on its own, which is what
# stops this recursing: the two builds below pass DEPS_TARGET on the command
# line, so they take the single architecture branch. That is what the origin
# test asks. "file" means DEPS_TARGET came from the ?= further up rather than
# from whoever ran make.
APPLE_ARCH :=
ifneq ($(filter macos-% ios-%,$(DEPS_TARGET)),)
APPLE_ARCH := $(lastword $(subst -, ,$(DEPS_TARGET)))
endif

MAC_UNIVERSAL :=
ifeq ($(origin DEPS_TARGET),file)
ifneq ($(filter macos-%,$(DEPS_TARGET)),)
MAC_UNIVERSAL := 1
MAC_SLICES    := macos-arm64 macos-x86_64
MAC_SLICE_LIBS = $(foreach t,$(MAC_SLICES),$(BUILD)/$(t)/libschultz.dylib)
endif
endif

# What the shared library is linked from.
#
# The merged archive everywhere but Windows, where the resource objects come
# out of it first. Several of the vendored projects compile a .rc file
# describing the DLL they ship -- FreeType, SDL and SDL_mixer among them --
# and each lands in the archive as an object of its own. Linking them into one
# DLL merges their .rsrc sections, and two resources of the same type and name
# cannot both be there:
#
#   .rsrc merge failure: duplicate leaf: type: 10 (VERSION) name: 1 lang: 409
#
# The members are removed rather than emptied. Stripping the sections out with
# objcopy leaves a file with nothing in it, which is not an object any more
# and stops the link a step later:
#
#   member ...(src_base_ftver.rc_ftver.o) in archive is not an object
#
# Nothing is lost either way. Those resources describe the DLLs those projects
# ship and say nothing about this one, which carries no version resource of
# its own.
#
# What was removed is printed, because the members are chosen by name and a
# name is a guess. Anything in that list that is not a compiled .rc file is a
# bug in the pattern.
ifneq ($(filter windows-%,$(DEPS_TARGET)),)
SHARED_INPUT := $(BUILD)/libschultz_all_nores.a
else
SHARED_INPUT := $(LIB_ALL)
endif

# Which C++ runtime the shared library carries. ThorVG is C++, so there is
# always one.
#
# Linux, macOS and iOS link the system copy: it is part of those platforms and
# every program there already has it. Android and Windows do not work that
# way. The NDK's libc++ is not on a phone, it is shipped in the apk, and MinGW
# runtime DLLs are on a build machine and nowhere else. On both, leaving it
# shared would mean this file plus two more, and a "self contained" library
# that is not, in the one place people notice: the app runs on the machine
# that built it and nowhere else.
#
# Statically is safe here because of what this library exports. Two copies of
# a C++ runtime in one process is a real problem when two libraries pass C++
# objects across a boundary, and nothing crosses this one: the whole of the
# API is C functions and opaque handles, and the export list below is
# schultz_* and nothing else.
#
# -lstdc++ has to come off the line with it. -static-libstdc++ tells the
# driver which runtime to add for itself; an explicit -lstdc++ still names the
# shared one, and naming both puts the shared one back.
#
# The shared library is linked with the C++ driver, not the C one. ThorVG is
# C++, so the runtime has to come from somewhere, and only the C++ driver adds
# it: gcc does not, and neither does gcc with -static-libstdc++, which says
# which copy to use rather than that there should be one.
#
# Naming -lstdc++ by hand is what the archive link does instead, and it is not
# enough here. It picks the shared runtime whatever -static-libstdc++ asks
# for, so it comes off this line and the driver is left to supply it.
#
# Make predefines CXX as g++, which is the host compiler and wrong for every
# cross target, so it is only kept if somebody set it themselves.
ifeq ($(origin CXX),default)
CXX := $(if $(filter %gcc,$(CC)),$(patsubst %gcc,%g++,$(CC)),\
         $(if $(filter %cc,$(CC)),$(patsubst %cc,%c++,$(CC)),$(CC)++))
endif
# Deferred assignment, not immediate. DEPS_LIBS is worked out further down the
# file, and := here would capture it while it is still empty, which drops
# every library the link needs and takes a while to notice: the shared library
# builds, and its undefined symbols simply have nothing recorded to resolve
# them.
ifneq ($(filter windows-%,$(DEPS_TARGET)),)
SHARED_LDLIBS     = $(LDLIBS)
SHARED_DEPS_LIBS  = $(DEPS_LIBS)
else
SHARED_LDLIBS     = $(filter-out -lstdc++,$(LDLIBS))
SHARED_DEPS_LIBS  = $(filter-out -lstdc++,$(DEPS_LIBS))
endif
#
# Windows asks for -Wl,-Bstatic instead, and asks for -static-libstdc++ to be
# left off, which looks backwards and is the whole point.
#
# -Bstatic is a switch the linker carries forward: everything named after it
# prefers an archive, including the libraries the driver appends for itself at
# the end. -static-libstdc++ is not a linker switch at all. g++ implements it
# by emitting "-Bstatic -lstdc++ -Bdynamic" at its own position, and that
# trailing -Bdynamic turns ours back off for everything after it. Asking for
# both gives a static C++ runtime and a dynamic everything else:
#
#   232:-Bstatic     <- ours
#   234:-lstdc++
#   235:-Bdynamic    <- g++ putting it back
#
# On a MinGW built for posix threads the driver then appends libwinpthread
# below that line, so the DLL imports libwinpthread-1.dll and will not start
# on a machine without MSYS2. Naming -lstdc++ ourselves under -Bstatic gets
# the same static runtime with nothing switching back.
#
# -lwinpthread is named too rather than left to the driver, so that the
# pthread calls in the toolkit and in ThorVG's thread pool resolve from the
# archive whatever the driver decides to add.
#
# The Windows API libraries are unaffected either way: MinGW ships those as .a
# import libraries, so kernel32 and the rest still bind as they always did.
ifneq ($(filter windows-%,$(DEPS_TARGET)),)
SHARED_RUNTIME    = -static-libgcc -Wl,-Bstatic
SHARED_EXTRA      = -lwinpthread
else ifneq ($(filter android-%,$(DEPS_TARGET)),)
SHARED_RUNTIME    = -static-libstdc++ -static-libgcc
SHARED_EXTRA      =
else
SHARED_RUNTIME    =
SHARED_EXTRA      =
endif

# AccessTunnel: accessibility on all five platforms. A separate repository,
# expected beside this one, and a required dependency. Build it there first;
# there is no cross project dependency tracking.
#
# One archive per target there as well, laid out the way this one is: the
# machine you are on in build/, and everything else in build/<target>. Naming
# only the first would link the Linux AT-SPI backend into an Android build,
# which is an archive that builds, installs, and then cannot load, because
# the function the Java side calls on startup is not in it.
ACCESS_TUNNEL_DIR ?= ../access-tunnel
#
# Where that project leaves its archive, which is not one rule for every
# target:
#
#   macOS      build/libaccess_tunnel.a, and it is universal. Its default
#              build makes both architectures and joins them with lipo, so
#              one archive answers for arm64 and x86_64 alike and both
#              slices of a universal build here point at the same file.
#   iOS        build-ios/, which holds one build at a time, chosen there by
#              IOS_SDK and IOS_ARCH. Building for a device and then for the
#              simulator replaces it rather than adding to it.
#   Android    build/<target>/, named the same way the targets here are.
#   otherwise  build/, the ordinary build for the machine it ran on.
#
# A fuzz target takes the ordinary host archive. Accessibility is not a place
# bytes chosen by an attacker arrive, so there is nothing to gain from a
# second instrumented copy of it, and the archive links into a sanitized
# binary either way.
#
# Naming ACCESS_TUNNEL_LIB on the command line overrides all of it, and
# reaches the slice builds of a universal macOS build too.
ifneq ($(filter macos-%,$(DEPS_TARGET)),)
ACCESS_TUNNEL_LIB := $(ACCESS_TUNNEL_DIR)/build/libaccess_tunnel.a
else ifneq ($(filter ios-%,$(DEPS_TARGET)),)
ACCESS_TUNNEL_LIB := $(ACCESS_TUNNEL_DIR)/build-ios/libaccess_tunnel.a
else ifeq ($(DEPS_BASE_TARGET),$(HOST_TARGET))
ACCESS_TUNNEL_LIB := $(ACCESS_TUNNEL_DIR)/build/libaccess_tunnel.a
else
ACCESS_TUNNEL_LIB := $(ACCESS_TUNNEL_DIR)/build/$(DEPS_TARGET)/libaccess_tunnel.a
endif
ACCESS_TUNNEL_CFLAGS := -isystem $(ACCESS_TUNNEL_DIR)

# Where the target's own .pc files went. build_deps.sh knows, because it
# knows which directories that target installs to, so it writes the path into
# config.mk rather than leaving this to guess at an architecture. The list
# after it is the fallback for a prefix built before that was recorded.
PC_DIRS := $(CURDIR)/$(PREFIX_DIR)/lib/pkgconfig:$(CURDIR)/$(PREFIX_DIR)/lib64/pkgconfig:$(CURDIR)/$(PREFIX_DIR)/lib/x86_64-linux-gnu/pkgconfig:$(CURDIR)/$(PREFIX_DIR)/lib/aarch64-linux-gnu/pkgconfig:$(CURDIR)/$(PREFIX_DIR)/bin/pkgconfig
# This prefix and nothing else, for the self contained check in "ready". The
# PKG_CONFIG_PATH below can have more than this on it for a native target,
# which is how a prefix that quietly needs a system package still resolves.
PC_SELF := $(TARGET_PC_PATH)$(PC_DIRS)
# A cross build must not read the host's .pc files at all, the way
# scripts/build_deps.sh does not either.
#
# Whatever PKG_CONFIG_PATH was already set to is kept for this machine, where
# it is a reasonable thing for somebody to have set. For another machine it is
# a way to answer a question about iOS with an answer about this computer: the
# packages are all there, under the wrong architecture, so the check passes and
# the compile uses the host's headers. PKG_CONFIG_LIBDIR goes with it, because
# clearing the search path still leaves pkg-config its built in system
# directories.
ifeq ($(DEPS_TARGET),$(HOST_TARGET))
PKG_CONFIG_PATH := $(TARGET_PC_PATH)$(PC_DIRS):$(PKG_CONFIG_PATH)
PKG_CONFIG_ENV  := PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)"
else
PKG_CONFIG_PATH := $(TARGET_PC_PATH)$(PC_DIRS)
PKG_CONFIG_ENV  := PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" PKG_CONFIG_LIBDIR="$(PKG_CONFIG_PATH)"
endif
export PKG_CONFIG_PATH

DEPS_PKGS    := sdl3 sdl3-mixer thorvg-1 freetype2 harfbuzz sheenbidi vpx opus
# ThorVG is linked as an archive, and its header has to be told so.
#
# thorvg_capi.h decides how to declare its 180 functions from whether
# TVG_STATIC is defined. Without it, on Windows, every one is declared
# __declspec(dllimport), the compiler emits a reference to __imp_tvg_whatever,
# and nothing in a static libthorvg-1.a answers to that name:
#
#   undefined reference to `__imp_tvg_canvas_set_viewport'
#
# Elsewhere the same branch is a visibility attribute, which a static link
# ignores, so this only ever showed on Windows. thorvg-1.pc does not say which
# kind of library it is, so it cannot come from pkg-config; the Makefile is
# what knows, because the Makefile is what links the archive.
TVG_STATIC_DEF := -DTVG_STATIC
TEXT_PKGS    := freetype2 harfbuzz sheenbidi
TVG_PKGS     := thorvg-1
# PKG_CONFIG_PATH is passed on each command line rather than relied on through
# export: GNU make's export reaches recipes, but not $(shell) expansions during
# parsing, which is where these run.
PKG_CONFIG   := $(PKG_CONFIG_ENV) pkg-config
DEPS_FOUND   := $(shell $(PKG_CONFIG) --exists $(DEPS_PKGS) 2>/dev/null && echo yes)
# -isystem rather than -I: ThorVG's own header trips -Wextra, and third party
# warnings are not ours to fix.
DEPS_CFLAGS  := $(TVG_STATIC_DEF) $(subst -I,-isystem ,$(shell $(PKG_CONFIG) --cflags $(DEPS_PKGS) 2>/dev/null))
# --static: the dependencies are built static, so their own dependencies have
# to be named on the link line too. A shared library carries that list itself;
# an archive does not.
DEPS_LIBS    := $(shell $(PKG_CONFIG) --libs --static $(DEPS_PKGS) 2>/dev/null)
TEXT_CFLAGS  := $(subst -I,-isystem ,$(shell $(PKG_CONFIG) --cflags $(TEXT_PKGS) 2>/dev/null))
TEXT_LIBS    := $(shell $(PKG_CONFIG) --libs --static $(TEXT_PKGS) 2>/dev/null)
TVG_CFLAGS   := $(TVG_STATIC_DEF) $(subst -I,-isystem ,$(shell $(PKG_CONFIG) --cflags $(TVG_PKGS) 2>/dev/null))
TVG_LIBS     := $(shell $(PKG_CONFIG) --libs --static $(TVG_PKGS) 2>/dev/null)
# Video, kept as its own group for the same reason text and the rasterizer
# are: a test binary links the library objects rather than the library, so
# every group a source needs has to be nameable on its own.
# SDL is in here for the decode thread and the lock around the slots, not for
# anything on screen: a video node runs in a test binary that has no window.
VIDEO_PKGS   := vpx opus sdl3
VIDEO_CFLAGS := $(subst -I,-isystem ,$(shell $(PKG_CONFIG) --cflags $(VIDEO_PKGS) 2>/dev/null))
VIDEO_LIBS   := $(shell $(PKG_CONFIG) --libs --static $(VIDEO_PKGS) 2>/dev/null)
# Nothing to find at run time: every dependency is linked in, so there is no
# rpath and no library path to set. Kept as an empty variable rather than
# removed, so the link rules below read the same on a target that needs one.
DEPS_RPATH   :=

# Tests: one binary per tests/test_*.c, so a crash loses one file's results
# rather than the whole run.
GREATEST_DIR := third_party/greatest
TEST_SRCS    := $(wildcard tests/test_*.c)
TEST_BINS    := $(TEST_SRCS:tests/%.c=$(BUILD)/tests/%)
TEST_CFLAGS  := $(CFLAGS) -I$(GREATEST_DIR) -I.
SAN_FLAGS    := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1

DEPS     := $(OBJS:.o=.d) $(TEST_BINS:%=%.d)

PREFIX   ?= /usr/local
BINDIR   := $(PREFIX)/bin
LIBDIR   := $(PREFIX)/lib
INCDIR   := $(PREFIX)/include/schultz
PCDIR    := $(LIBDIR)/pkgconfig
# Every header a consuming project needs: the umbrella and everything it
# includes. The internal ones are deliberately left out.
PUB_HDRS := schultz_api.h schultz.h schultz_geom.h schultz_node.h \
            schultz_style.h schultz_layout.h schultz_event.h \
            schultz_widget.h schultz_widgets.h schultz_font.h \
            schultz_glyphs.h \
            schultz_image.h schultz_resource.h schultz_render.h \
            schultz_window.h schultz_arena.h schultz_paint.h schultz_handle.h \
            schultz_selection.h

.PHONY: all lib shared debug run test test-asan fuzz docs audit clean install uninstall

# A cross target builds the archives only. The demo is a command line program
# with a main, and there is nothing to run it on: an iOS or Android build wants
# the toolkit to link into an app, not an executable.
ifeq ($(TARGET_CROSS),1)
all: $(LIB) $(LIB_ALL)
else
all: $(BIN) $(LIB) $(LIB_ALL)
endif

# The archives on their own, for a consumer that does not want the demo built.
lib: $(LIB) $(LIB_ALL)

# ---------------------------------------------------------- other machines
#
# One command per target, named after the target. Each builds that target's
# dependencies if they are not there yet, then builds the toolkit against
# them with the compiler that target needs.
#
# The list comes from scripts/targets, so a new file there is a new command
# here with nothing else to write.
#
# Output goes to build/<target>, because objects for one machine are not
# objects for another and one directory holding both is an archive that links
# nowhere. The plain build stays in build/, where it has always been.
#
# Archives only. The demo is a command line program with a main, and on a
# phone the toolkit links into an app rather than an executable.
CROSS_TARGETS := $(patsubst scripts/targets/%.sh,%,$(wildcard scripts/targets/*.sh))
CLEAN_TARGETS := $(addprefix clean-,$(CROSS_TARGETS))

# The dependencies are deliberately not built here. build_deps.sh builds a
# target from nothing every time it runs, so calling it from this rule would
# rebuild all fifteen libraries on every "make android-arm64", which is most
# of an hour to recompile something nobody changed. They are a separate step
# because they change rarely; when they are missing the build says so and
# names the command.
.PHONY: $(CROSS_TARGETS)
$(CROSS_TARGETS):
	@$(MAKE) --no-print-directory DEPS_TARGET=$@ BUILD=$(BUILD)/$@ lib

# ar rcs: replace, create if missing, and write the index. One archive holding
# the toolkit and nothing else; the dependencies stay separate archives, and
# schultz.pc names them so a consumer can ask pkg-config for the whole line.
$(LIB): $(LIB_OBJS) $(BACKEND_OBJS) | $(BUILD)
	@rm -f $@
	$(AR) rcs $@ $(LIB_OBJS) $(BACKEND_OBJS)
	@echo "archive: $@"

# The member list is derived from the same pkg-config line the demo links
# with, so it cannot drift from what the toolkit actually needs. Names repeat
# on that line, so each one is taken once. Merging the archives is left to a
# script because the command for it differs between GNU and Apple toolchains.
$(LIB_ALL): $(LIB) $(ACCESS_TUNNEL_LIB)
	@dirs=""; libs=""; \
	for tok in $(DEPS_LIBS); do \
	  case $$tok in \
	    -L*) dirs="$$dirs $${tok#-L}" ;; \
	    -l*) libs="$$libs $${tok#-l}" ;; \
	  esac; \
	done; \
	seen=""; members=""; \
	for l in $$libs; do \
	  case " $$seen " in *" $$l "*) continue ;; esac; \
	  seen="$$seen $$l"; \
	  for d in $$dirs; do \
	    if [ -f "$$d/lib$$l.a" ]; then members="$$members $$d/lib$$l.a"; break; fi; \
	  done; \
	done; \
	AR="$(AR)" RANLIB="$(RANLIB)" \
	sh scripts/merge_archives.sh $@ $(LIB) $(ACCESS_TUNNEL_LIB) $$members
	@echo "archive: $@ (self contained)"

# What the shared library is allowed to export.
#
# Not the same question as for the archive, and the reason is --whole-archive
# below. An archive hands the linker a menu; a shared library has everything
# in it, dependencies included, and publishes a list. Without a list it
# publishes what those dependencies made visible: SDL_*, FT_*, png_*, z_* and
# the rest. A JVM that already has its own zlib then has two, and whichever
# the dynamic linker binds first wins for both.
#
# -fvisibility=hidden does not cover this. It applies to the toolkit's own
# objects, and the dependencies were compiled by cmake and meson with their
# own settings. So the list is stated here, in the one form each platform
# takes: everything named schultz_ and nothing else.
ifneq ($(filter windows-%,$(DEPS_TARGET)),)
$(SHARED_LIST): $(LIB) | $(BUILD)
	@printf 'EXPORTS\n' > $@
	@$(NM) -g --defined-only $(LIB) \
	  | awk '$$2 == "T" && $$3 ~ /^schultz_/ { print "  " $$3 }' \
	  | sort -u >> $@
	@echo "exports: $@ ($$(( $$(wc -l < $@) - 1 )) names)"
else ifneq ($(filter macos-% ios-%,$(DEPS_TARGET)),)
$(SHARED_LIST): | $(BUILD)
	@echo '_schultz_*' > $@
else
$(SHARED_LIST): | $(BUILD)
	@printf '%s\n' '{' '  global: schultz_*;' '  local: *;' '};' > $@
endif

# The shared library itself.
#
# $(LDFLAGS) first, because it is what carries the architecture. On macOS the
# compiler builds for the machine it runs on unless a flag says otherwise, and
# the flag lives there: without it the link asks for x86_64, every input is
# arm64, and ld reports symbols missing from libraries it has just listed.
#
# --whole-archive is what makes it self contained. Without it the linker takes
# only the members something already references, and nothing inside the
# archive references the public API, so the result would export almost
# nothing. Apple spells the same idea -force_load.
#
# $(DEPS_LIBS) is named again after the archive for the system libraries in
# it, which are not merged in and cannot be: -lkernel32 on Windows, the
# frameworks on Apple. The vendored ones on that line are already inside, and
# a static archive whose symbols are all defined contributes nothing further,
# so naming them twice is free.
ifdef MAC_UNIVERSAL
$(SHARED): | $(BUILD)
	@echo "checking both architectures"
	@bad=0; \
	for t in $(MAC_SLICES); do \
	  $(MAKE) --no-print-directory DEPS_TARGET=$$t BUILD=$(BUILD)/$$t ready \
	    || bad=1; \
	done; \
	if [ $$bad = 1 ]; then \
	  echo; \
	  echo "nothing was built. See docs/building.md."; \
	  exit 1; \
	fi
	@for t in $(MAC_SLICES); do \
	  echo "== $$t"; \
	  $(MAKE) --no-print-directory DEPS_TARGET=$$t BUILD=$(BUILD)/$$t shared \
	    || exit 1; \
	done
	@rm -f $@
	lipo -create $(MAC_SLICE_LIBS) -output $@
	@echo "shared: $@ (`lipo -archs $@`)"
else ifneq ($(filter windows-%,$(DEPS_TARGET)),)
$(SHARED_INPUT): $(LIB_ALL)
	@cp $< $@
	@res=`$(AR) t $@ | grep -E '\.(rc|res)($$|[._])' || true`; \
	if [ -n "$$res" ]; then \
	  $(AR) d $@ $$res; \
	  $(RANLIB) $@; \
	  echo "archive: $@ (removed `echo $$res | wc -w` resource objects)"; \
	  for r in $$res; do echo "    $$r"; done; \
	else \
	  echo "archive: $@ (no resource objects to remove)"; \
	fi

$(SHARED): $(SHARED_INPUT) $(SHARED_LIST) | $(BUILD)
	$(CXX) $(LDFLAGS) -shared -o $@ \
	      -Wl,--whole-archive $(SHARED_INPUT) -Wl,--no-whole-archive \
	      $(SHARED_LIST) -Wl,--out-implib,$(SHARED_IMP) \
	      $(SHARED_RUNTIME) $(SHARED_DEPS_LIBS) $(SHARED_LDLIBS) \
	      $(SHARED_EXTRA)
	@echo "shared: $@ and $(SHARED_IMP)"
else ifneq ($(filter macos-% ios-%,$(DEPS_TARGET)),)
$(SHARED): $(LIB_ALL) $(SHARED_LIST) | $(BUILD)
	$(CXX) $(LDFLAGS) -dynamiclib -o $@ \
	      -Wl,-force_load,$(LIB_ALL) \
	      -Wl,-exported_symbols_list,$(SHARED_LIST) \
	      -install_name @rpath/$(notdir $(SHARED)) \
	      $(SHARED_RUNTIME) $(SHARED_DEPS_LIBS) $(SHARED_LDLIBS)
	@echo "shared: $@"
else
# relro and now make the relocation tables read only once the loader has
# filled them in, which takes the usual route from an overwritten function
# pointer to an overwritten GOT entry off the table. ELF only, which is why
# it is here rather than in LDFLAGS: the Windows and macOS rules above link
# with different tools that do not understand it.
$(SHARED): $(LIB_ALL) $(SHARED_LIST) | $(BUILD)
	$(CXX) $(LDFLAGS) -shared -o $@ \
	      -Wl,-z,relro -Wl,-z,now \
	      -Wl,--whole-archive $(LIB_ALL) -Wl,--no-whole-archive \
	      -Wl,--version-script=$(SHARED_LIST) \
	      -Wl,-soname,$(notdir $(SHARED)) \
	      -Wl,--as-needed $(SHARED_RUNTIME) $(SHARED_DEPS_LIBS) $(SHARED_LDLIBS)
	@echo "shared: $@"
endif

shared: $(SHARED)

# Everything one target needs, asked before anything is compiled.
#
# This exists because the universal macOS build is two builds, and finding out
# about the second one's missing pieces after the first has compiled for ten
# minutes is a poor way to spend an afternoon. It reports rather than fixes,
# and it reports all of it at once.
.PHONY: ready
ifeq ($(DEPS_FOUND),yes)
ready:
	@ok=1; \
	if [ ! -f "$(ACCESS_TUNNEL_LIB)" ]; then \
	  echo "  $(DEPS_TARGET): AccessTunnel archive not found"; \
	  echo "      $(ACCESS_TUNNEL_LIB)"; \
	  echo "      Build it there, or name another with ACCESS_TUNNEL_LIB="; \
	  ok=0; \
	elif [ -n "$(APPLE_ARCH)" ] && command -v lipo >/dev/null 2>&1; then \
	  have=`lipo -archs "$(ACCESS_TUNNEL_LIB)" 2>/dev/null`; \
	  case " $$have " in \
	    *" $(APPLE_ARCH) "*) ;; \
	    *) echo "  $(DEPS_TARGET): AccessTunnel is the wrong architecture"; \
	       echo "      $(ACCESS_TUNNEL_LIB)"; \
	       echo "      has [$$have], needs $(APPLE_ARCH)"; \
	       ok=0 ;; \
	  esac; \
	fi; \
	if ! PKG_CONFIG_PATH="$(PC_SELF)" PKG_CONFIG_LIBDIR="$(PC_SELF)" \
	     pkg-config --exists $(DEPS_PKGS) 2>/dev/null; then \
	  echo "  $(DEPS_TARGET): its prefix depends on something outside itself"; \
	  PKG_CONFIG_PATH="$(PC_SELF)" PKG_CONFIG_LIBDIR="$(PC_SELF)" \
	    pkg-config --print-errors --exists $(DEPS_PKGS) 2>&1 \
	    | sed 's/^/      /' || true; \
	  echo "      It resolves here only because this is the machine that"; \
	  echo "      built it. Rebuild it: $(DEPS_COMMAND)"; \
	  ok=0; \
	fi; \
	if [ $$ok = 1 ]; then echo "  $(DEPS_TARGET): ready"; else exit 1; fi
else
ready:
	@echo "  $(DEPS_TARGET): its dependencies are not usable"
	@$(PKG_CONFIG) --print-errors --exists $(DEPS_PKGS) 2>&1 \
	  | sed 's/^/      /' || true
	@echo "      Run: $(DEPS_COMMAND)"
	@exit 1
endif

$(BUILD)/schultz.pc: Makefile | $(BUILD)
	@printf '%s\n' \
	  'prefix=$(PREFIX)' \
	  'libdir=$${prefix}/lib' \
	  'includedir=$${prefix}/include' \
	  '' \
	  'Name: schultz' \
	  'Description: Retained mode UI toolkit, drawn in software' \
	  'Version: 0.1.0' \
	  'Cflags: -I$${includedir}' \
	  'Libs: -L$${libdir} -lschultz' \
	  'Libs.private: -L$${libdir} -laccess_tunnel $(filter-out -L%,$(DEPS_LIBS)) $(LDLIBS)' \
	  > $@

# Build with debug symbols and no optimization.
debug: CFLAGS := -std=c11 -Wall -Wextra -g -O0
debug: clean $(BIN)

ifeq ($(DEPS_FOUND),yes)
$(BIN): $(OBJS) | $(BUILD)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(DEPS_RPATH) $(ACCESS_TUNNEL_LIB) \
	      $(DEPS_LIBS) $(LDLIBS)
# Translation units that see vendored headers. Everything else compiles
# against nothing but the standard library.
$(BACKEND_C_OBJS) $(BUILD)/schultz_demo.o: $(BUILD)/%.o: %.c $(TARGET_CONFIG) | $(BUILD)
	$(CC) $(CFLAGS) $(DEPS_CFLAGS) $(ACCESS_TUNNEL_CFLAGS) -isystem $(UB_DIR) \
	      -MMD -MP -c -o $@ $<

# The macOS and iOS accessibility backends. They are Objective-C because
# VoiceOver asks a view questions by calling methods on it, and only
# Objective-C can answer. Manual retain and release, matching AccessTunnel's
# own shells, so -fobjc-arc is deliberately absent: both files refuse to
# compile with it.
$(BACKEND_OBJC_OBJS): $(BUILD)/%.o: %.m $(TARGET_CONFIG) | $(BUILD)
	$(CC) $(CFLAGS) $(DEPS_CFLAGS) $(ACCESS_TUNNEL_CFLAGS) -isystem $(UB_DIR) \
	      -fno-objc-arc -MMD -MP -c -o $@ $<

$(TEXT_OBJS): $(BUILD)/%.o: %.c $(TARGET_CONFIG) | $(BUILD)
	$(CC) $(CFLAGS) $(TEXT_CFLAGS) -isystem $(UB_DIR) -MMD -MP -c -o $@ $<

$(TVG_OBJS): $(BUILD)/%.o: %.c $(TARGET_CONFIG) | $(BUILD)
	$(CC) $(CFLAGS) $(TVG_CFLAGS) $(TEXT_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/schultz_voice.o: schultz_voice.c $(TARGET_CONFIG) | $(BUILD)
	$(CC) $(CFLAGS) $(VOICE_CFLAGS) -MMD -MP -c -o $@ $<

$(VIDEO_OBJS): $(BUILD)/%.o: %.c $(TARGET_CONFIG) | $(BUILD)
	$(CC) $(CFLAGS) $(VIDEO_CFLAGS) $(NE_CFLAGS) -MMD -MP -c -o $@ $<

else

# Nothing that needs a vendored header can be built, so say so here rather
# than letting the compile run on and fail on a missing header.
#
# pkg-config answers for a list or not at all: one package it cannot resolve
# empties DEPS_CFLAGS, every -isystem with it, and the build carries on until
# a backend file asks for SDL3/SDL.h and cannot find it, twenty files later.
# The message that reaches a person then names a header rather than the
# dependency set that is actually short.
#
# Every object that sees a vendored header is listed, which is the backend,
# the text pipeline, the ThorVG painter and the demo. The archives and the
# shared library depend on these, so they stop too.
#
# Leaving the text and painter objects off this list was worth a bug on its
# own. They ask pkg-config for freetype2, harfbuzz and thorvg-1 rather than
# for the whole set, so they have their own flags and their own way of coming
# back empty, and a build with no prefix at all got twelve files in before
# stopping on a missing ft2build.h.
$(BIN) $(BACKEND_C_OBJS) $(BACKEND_OBJC_OBJS) $(TEXT_OBJS) $(TVG_OBJS) \
$(BUILD)/schultz_demo.o:
	@echo "dependencies for $(DEPS_TARGET) not found."
	@echo
	@$(PKG_CONFIG) --print-errors --exists $(DEPS_PKGS) 2>&1 \
	  | sed 's/^/  /' || true
	@echo
	@echo "Run: $(DEPS_COMMAND)"
	@echo "See docs/building.md."
	@exit 1
endif

# Vendored third party source. Compiled without the toolkit's own warning
# flags: it is not our code to fix, and a warning we would never act on is
# noise that hides one we would. VENDOR_CFLAGS keeps the standard and the
# optimisation level while dropping -Wall -Wextra.
VENDOR_CFLAGS := $(filter-out -Wall -Wextra,$(CFLAGS))
$(UB_OBJS): $(BUILD)/%.o: %.c $(TARGET_CONFIG) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(VENDOR_CFLAGS) -MMD -MP -c -o $@ $<

$(NE_OBJS): $(BUILD)/%.o: %.c $(TARGET_CONFIG) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(VENDOR_CFLAGS) $(NE_CFLAGS) -MMD -MP -c -o $@ $<

$(SDSP_OBJS): $(BUILD)/%.o: %.c $(TARGET_CONFIG) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(VENDOR_CFLAGS) $(SDSP_CFLAGS) -MMD -MP -c -o $@ $<

# od and awk, so it runs on the machine doing the build rather than needing a
# compiler for it. A generator built from C would be built with $(CC), which
# is the target's compiler on a cross build and cannot be run here.
$(FONT_SRC): scripts/embed_fonts.sh $(FONT_FILES) | $(BUILD)
	sh scripts/embed_fonts.sh $@
	@echo "fonts: $@"

# -I. because the source sits in $(BUILD) and its header does not.
$(FONT_OBJ): $(FONT_SRC) schultz_font_builtin.h schultz_style.h \
               $(TARGET_CONFIG) | $(BUILD)
	$(CC) $(CFLAGS) -I. -c -o $@ $<

$(BUILD)/%.o: %.c $(TARGET_CONFIG) | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/tests:
	mkdir -p $(BUILD)/tests

run: $(BIN)
	./$(BIN)

# Each test binary links the library objects, never $(MAIN_SRC).
#
# The sound system comes along because a video node plays through it, and it
# brings SDL with it. That is still headless: SDL's dummy driver plays to
# nowhere, so the tests want no sound card, no microphone and no display, and
# a test that never opens the sound system never starts SDL at all.
$(BUILD)/tests/%: tests/%.c $(LIB_OBJS) $(BUILD)/schultz_audio.o \
                  | $(BUILD)/tests
	$(CC) $(TEST_CFLAGS) $(DEPS_CFLAGS) $(TEXT_CFLAGS) $(TVG_CFLAGS) \
		$(VIDEO_CFLAGS) -MMD -MP -o $@ $< \
		$(LIB_OBJS) $(BUILD)/schultz_audio.o \
		$(DEPS_RPATH) $(DEPS_LIBS) $(TEXT_LIBS) $(TVG_LIBS) \
		$(VIDEO_LIBS) $(LDLIBS)

# The accessibility test is the one that needs more than the library objects:
# schultz_a11y.c lives with the backends rather than in the library, and it
# needs AccessTunnel. The test brings its own backend, so no platform one is
# linked and there is nothing to clash with.
$(BUILD)/tests/test_a11y: tests/test_a11y.c $(LIB_OBJS) \
                  $(BUILD)/schultz_audio.o $(BUILD)/schultz_a11y.o \
                  $(ACCESS_TUNNEL_LIB) | $(BUILD)/tests
	$(CC) $(TEST_CFLAGS) $(DEPS_CFLAGS) $(TEXT_CFLAGS) $(TVG_CFLAGS) \
		$(VIDEO_CFLAGS) $(ACCESS_TUNNEL_CFLAGS) -MMD -MP -o $@ $< \
		$(LIB_OBJS) $(BUILD)/schultz_audio.o $(BUILD)/schultz_a11y.o \
		$(ACCESS_TUNNEL_LIB) \
		$(DEPS_RPATH) $(DEPS_LIBS) $(TEXT_LIBS) $(TVG_LIBS) \
		$(VIDEO_LIBS) $(LDLIBS)

test: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
		echo "== $$t"; \
		$$t || fail=1; \
	done; \
	exit $$fail

# Same suite under AddressSanitizer and UndefinedBehaviorSanitizer. For a
# malloc and handle table library this catches more than the assertions do.
#
# This recurses with a separate BUILD directory on purpose: instrumented and
# uninstrumented objects must never share one, or a later plain build links
# sanitizer-instrumented objects without the sanitizer runtime.
#
# alloc_dealloc_mismatch is off because ThorVG's Lottie loader frees a buffer
# with operator delete that its bundled rapidjson allocated with malloc. That
# is upstream C++ we do not compile, it is not a leak, and it only became
# visible once ThorVG was linked as an archive instead of a shared library.
# Every other check stays on.
# ---------------------------------------------------------------- fuzzing
#
# Targets that hand a parser bytes nobody chose, which is the one thing the
# test suite cannot do: a test says what a known input produces, and these say
# nothing about the answer at all. What they watch for is the process coming
# apart, which is why they are built under AddressSanitizer as well.
#
# Where they aim is where bytes from outside enter: a picture somebody sent, a
# font that came with a document. Both reach vendored C parsers, which is
# where the memory safety risk in this library actually lives.
#
# Built with afl-clang-fast rather than the ordinary compiler, because a
# fuzzer needs the binary to tell it which branches an input reached. Without
# that it is guessing; with it, it works its way through a format on its own.
#
#   make fuzz
#
# which ends by printing the afl-fuzz command for each target, with the map
# size that target needs already filled in.
#
# No @@ on those commands. These are persistent mode targets: AFL hands each
# input over shared memory and the target runs thousands of them in one
# process, which is worth doing because starting a process costs more than
# decoding a small picture does. A filename means something else to them,
# which is the next paragraph.
#
# A crash lands in the output directory, and the same binary replays it:
#
#   build-fuzz/fuzz/fuzz_image <the-crashing-file>
#
# The instrumented binary, deliberately, rather than a plain one. A crash is
# only certain to happen again in the binary that produced it.
#
# For the picture target, give it the format dictionaries AFL ships. Random
# edits destroy a magic number almost every time, so without them nearly every
# input is rejected in the first few bytes and the decoders are barely
# reached. Measured here: coverage stopped growing after a minute without
# them.
#
#   D=/usr/share/doc/afl++-doc/afl/dictionaries
#   cat $D/png.dict $D/jpeg.dict $D/webp.dict $D/svg.dict $D/xml.dict \
#       > build-fuzz/image.dict
#   afl-fuzz -x build-fuzz/image.dict ...
#
# They are not vendored here: they belong to AFL, they are only useful on a
# machine that already has it installed, and the path above is where the
# Debian and Ubuntu packages put them.
# The library is rebuilt too, not only these. A fuzzer works by watching which
# branches an input reached, and it can only see that in code the instrumenting
# compiler touched. Linking an instrumented target against an ordinary library
# leaves it guessing, which is the difference between working through a format
# in minutes and never getting past the first header check.
#
# What this does not reach, stated plainly, because it decides how much a
# clean run is worth: the dependency prefix is built by scripts/build_deps.sh
# with the ordinary compiler, so libpng, libwebp, libjpeg-turbo, FreeType and
# HarfBuzz carry neither the instrumentation nor the sanitizer.
#
# That costs both halves. The fuzzer cannot steer, because every well formed
# picture reaches the same handful of edges in our own code and it is told
# nothing about the decoding underneath. And AddressSanitizer cannot see a
# read or write past the end of a buffer inside those libraries either: it
# checks accesses in code it compiled, and it compiled none of them. What it
# still catches anywhere is a double free, a mismatched free, and an access
# far enough out to land on an unmapped page.
#
# So the decoders have to be built the same way, and they are:
#
#   sh scripts/build_deps.sh --fuzz
#
# builds the whole dependency set a second time under the same compiler and
# AddressSanitizer, into build-deps/<target>-fuzz, and `make fuzz` links
# against that prefix rather than the ordinary one. What it is worth, counted
# in instrumented edges the fuzzer can steer by:
#
#   pictures   1,000 without it, 67,939 with
#   fonts      3,968 without it, 138,361 with
#
# The ordinary prefix is untouched, because these libraries are slower and
# only useful under a fuzzer.
#
# One thing is still outside: the accessibility archive from ../access-tunnel
# links in uninstrumented. Nothing an attacker chooses arrives there, so
# there is nothing to search.
FUZZ_SRCS := $(wildcard tests/fuzz_*.c)
FUZZ_BINS := $(FUZZ_SRCS:tests/%.c=$(BUILD)/fuzz/%)

$(BUILD)/fuzz:
	mkdir -p $@

$(BUILD)/fuzz/%: tests/%.c $(LIB_ALL) | $(BUILD)/fuzz
	$(CC) $(TEST_CFLAGS) $(DEPS_CFLAGS) $(TEXT_CFLAGS) \
		$(TVG_CFLAGS) $(VIDEO_CFLAGS) -o $@ $< \
		$(LIB_ALL) $(DEPS_RPATH) $(DEPS_LIBS) $(TEXT_LIBS) \
		$(TVG_LIBS) $(VIDEO_LIBS) $(LDLIBS)

.PHONY: fuzz fuzz-bins fuzz-replay
fuzz-bins: $(FUZZ_BINS)
	@echo
	@echo "fuzz targets, and the command to run each:"
	@# Each binary is asked how many edges it was instrumented with, which
	@# it prints under AFL_DEBUG before main runs. Standard input is closed
	@# for that: with no file named, a target built this way waits for the
	@# fuzzer to hand it one, and inside a make recipe that is a hang.

	@for one in $(FUZZ_BINS); do \
		seeds=tests/corpus/$${one##*/fuzz_}; \
		size=`AFL_DEBUG=1 $$one </dev/null 2>&1 \
		      | sed -n 's/.*__afl_final_loc = //p' | tail -1`; \
		echo; \
		echo "  AFL_MAP_SIZE=$${size:-65536} afl-fuzz -m none \\"; \
		echo "    -i $$seeds -o $${one%%/fuzz/*}/out/$${one##*/} \\"; \
		echo "    -- $$one"; \
	done
	@echo
	@echo "AFL_MAP_SIZE is read out of each binary above rather than"
	@echo "guessed. AFL sizes its coverage map to 65536 edges by default"
	@echo "and these have far more than that, because the decoders are"
	@echo "instrumented too; without it afl-fuzz stops before it starts."

# Runs every seed through its target once, with the ordinary compiler and the
# ordinary prefix. This is not fuzzing: nothing is mutated and it finishes in
# a second. It is what keeps the targets and the seeds honest on a machine
# with no AFL installed, so a change that stops one of them compiling or
# loading is caught here rather than months later by whoever next runs the
# fuzzer. Seeds live in tests/corpus/<name> and feed tests/fuzz_<name>.c.
#
# Deliberately not part of `make test`. The fuzz targets are their own kind of
# thing and are run on purpose:  make fuzz-replay
fuzz-replay: $(FUZZ_BINS)
	@fail=0; \
	for one in $(FUZZ_BINS); do \
		seeds=tests/corpus/$${one##*/fuzz_}; \
		[ -d "$$seeds" ] || continue; \
		echo "== $$one"; \
		for seed in "$$seeds"/*; do \
			$$one "$$seed" || { echo "   failed on $$seed"; fail=1; }; \
		done; \
	done; \
	exit $$fail

# afl-clang-fast rather than afl-gcc-fast: the packaged GCC plugin is built
# against one GCC and refuses to load into another, and the clang driver has
# no such tie.
# The instrumented dependency prefix, built by
#
#   sh scripts/build_deps.sh --fuzz
#
# It is a second prefix beside the ordinary one, and it carries its own
# config.mk naming afl-clang-fast and the sanitizer flags. So this rule sets
# no compiler and no flags of its own: it points the ordinary build at that
# prefix and everything else follows from the file the script wrote.
#
# Nothing is passed as a command line variable here on purpose. A variable set
# on the command line overrides the makefile, and += against it does nothing,
# so a CFLAGS= here would silently throw away the sanitizer flags that
# config.mk contributes.
FUZZ_DEPS_TARGET := $(HOST_TARGET)-fuzz

fuzz:
	@command -v afl-fuzz >/dev/null 2>&1 || \
	  { echo "afl-fuzz not found; install afl++"; exit 1; }
	@[ -d build-deps/$(FUZZ_DEPS_TARGET)/prefix ] || \
	  { echo "no instrumented prefix for $(FUZZ_DEPS_TARGET)."; \
	    echo "build one with:  sh scripts/build_deps.sh --fuzz"; exit 1; }
	AFL_QUIET=1 $(MAKE) BUILD=build-fuzz \
		DEPS_TARGET=$(FUZZ_DEPS_TARGET) fuzz-bins

test-asan:
	ASAN_OPTIONS=alloc_dealloc_mismatch=0 $(MAKE) BUILD=build-asan \
		CFLAGS="-std=c11 -Wall -Wextra $(SAN_FLAGS)" \
		LDLIBS="$(SAN_FLAGS) $(LDLIBS)" test

# API reference from the Doxygen comments in the headers. Doxygen is a build
# time tool only: it is never linked into Schultz, so its GPL license places
# no obligation on the library or on users of it.
#
# The output goes to docs/api and is committed, unlike everything else this
# build produces. That is so a reader can browse the reference without
# installing doxygen and running it, which is most readers. Regenerate it
# whenever a header's comments change; the diff is large because the whole
# site is rewritten, and that is the price of the reference being there.
#
# Headers only. The implementation is not reference material, and reading it
# as well was what made this target fail: doxygen counts an undocumented
# member of an internal struct as a warning, and warnings are errors here.
docs:
	@command -v doxygen >/dev/null 2>&1 || \
		{ echo "doxygen not found; install it to build the API docs"; exit 1; }
	doxygen Doxyfile
	@echo "HTML: docs/api/index.html"

# Reports functions that are declared but never called. Unused code compiles
# and passes tests, so nothing else in the build catches it. Run after each
# roadmap phase.
audit:
	@sh scripts/audit_unused.sh
	@# One accessibility backend is compiled per target, so five of the six
	@# are never built here. This is what notices a function missing from
	@# one of them before whoever builds for that platform does.
	@sh scripts/check_a11y_backends.sh

# Only this machine's build. The cross builds sit in directories under the
# same one, named after their target, and taking the whole of build/ would
# throw away work that has nothing to do with the target being cleaned. Each
# of those has its own command below.
#
# The dependencies are left alone, here and there. They are slow to rebuild
# and have their own command, which builds one target from nothing every time
# it is run:
#   sh scripts/build_deps.sh --target NAME
clean:
	@rm -rf build-asan
	@[ -d $(BUILD) ] || exit 0; \
	for entry in $(BUILD)/*; do \
	  [ -e "$$entry" ] || continue; \
	  keep=0; \
	  for target in $(CROSS_TARGETS); do \
	    [ "$${entry##*/}" = "$$target" ] && keep=1; \
	  done; \
	  [ "$$keep" = 1 ] || rm -rf "$$entry"; \
	done
	@echo "cleaned: $(BUILD)"

# And one per target, for the same reason there is one build command per
# target. Only that target's output; nothing else is touched.
.PHONY: $(CLEAN_TARGETS)
$(CLEAN_TARGETS):
	@rm -rf $(BUILD)/$(patsubst clean-%,%,$@)
	@echo "cleaned: $(BUILD)/$(patsubst clean-%,%,$@)"

# A cross target has no demo to install, for the same reason it has none to
# build, so only the archives and headers go.
ifeq ($(TARGET_CROSS),1)
install: $(LIB) $(LIB_ALL) $(BUILD)/schultz.pc
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCDIR) $(DESTDIR)$(PCDIR)
else
install: $(BIN) $(LIB) $(LIB_ALL) $(BUILD)/schultz.pc
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBDIR) \
	          $(DESTDIR)$(INCDIR) $(DESTDIR)$(PCDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(TARGET)
endif
	install -m 644 $(LIB) $(LIB_ALL) $(DESTDIR)$(LIBDIR)/
	find $(PREFIX_DIR)/lib -name '*.a' -exec install -m 644 {} $(DESTDIR)$(LIBDIR)/ \;
	install -m 644 $(ACCESS_TUNNEL_LIB) $(DESTDIR)$(LIBDIR)/
	install -m 644 $(PUB_HDRS) $(DESTDIR)$(INCDIR)/
	install -m 644 $(BUILD)/schultz.pc $(DESTDIR)$(PCDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(LIBDIR)/libschultz.a $(DESTDIR)$(LIBDIR)/libschultz_all.a
	rm -f $(DESTDIR)$(LIBDIR)/$(notdir $(ACCESS_TUNNEL_LIB))
	@# The same list install copied in, removed by name so nothing else goes.
	find $(PREFIX_DIR)/lib -name '*.a' -printf '%f\n' \
	  | while read a; do rm -f $(DESTDIR)$(LIBDIR)/$$a; done
	rm -f $(DESTDIR)$(PCDIR)/schultz.pc
	rm -rf $(DESTDIR)$(INCDIR)

-include $(DEPS)
