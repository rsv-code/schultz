# Building Schultz

Schultz has nine build configurations across five platforms. This page is how
you build each of them.

## How the build is arranged

Two directories, and the difference between them matters:

| Directory | In git | What it holds |
|---|---|---|
| `third_party/src/` | **yes** | The upstream source archives, exactly as published. 48 MB |
| `third_party/libunibreak/` | **yes** | One dependency vendored as source and compiled in. Not Schultz code; see below |
| `build-deps/<target>/` | no | Everything a dependency build produces: extracted sources, build trees, and the installed libraries |

The sources are committed on purpose. A build needs no network and no upstream
host has to still be up, and if a project deletes a release tomorrow you can
still build. Each archive's SHA-256 is recorded in `scripts/versions.sh` and
checked every time it is extracted, so the audit trail survives too.

One source set covers every target. There is no per-platform source. What
differs between targets is the *configuration* the libraries are built with,
and that is what `scripts/targets/` holds.

Because output is per target, one checkout can hold an Android build and a
Linux build at the same time without either disturbing the other.

## The dependencies

Sixteen libraries are built by the script and linked. Ten of them are there
for the screen and six for sound:

| Library | What it does | Build system | Licence |
|---|---|---|---|
| SDL3 | Window, mouse, keyboard, touch, clipboard, audio devices | cmake | zlib |
| ThorVG | Turns shapes into pixels, in software on every target | meson | MIT |
| FreeType | Turns glyphs into bitmaps | meson | FTL |
| HarfBuzz | Turns text into positioned glyphs | meson | MIT |
| SheenBidi | Puts mixed direction text in visual order | meson | Apache 2.0 |
| zlib | Decompression, for libpng and for compressed fonts | cmake | zlib |
| libpng | Decodes PNG, for images and colour bitmap fonts | cmake | PNG v2 |
| libwebp | Decodes WebP | cmake | BSD 3-Clause |
| libjpeg-turbo | Decodes JPEG, for ThorVG | cmake | IJG and BSD 3-Clause |
| SDL_mixer | Picks a decoder for a sound file and mixes what plays | cmake | zlib |
| libogg | The container Vorbis, FLAC and Opus travel in | cmake | BSD 3-Clause |
| libvorbis | Decodes Ogg Vorbis | cmake | BSD 3-Clause |
| FLAC | Decodes FLAC | cmake | BSD 3-Clause |
| Opus | Decodes Opus | cmake | BSD 3-Clause |
| opusfile | Reads an `.opus` file, which libopus alone does not | cmake | BSD 3-Clause |
| WavPack | Decodes WavPack | cmake | BSD 3-Clause |

**Python is a build tool here, not a dependency of the result.** The "Built
with" column above names meson four times, and meson is itself a Python
program. ThorVG is why it cannot be avoided: of the four meson libraries it is
the only one that ships no other build system, so there is no arrangement of
these versions that builds Schultz without a Python interpreter on the
machine. Nothing Python touches ends up in the binary.

Everything is built **static**, so the result is one binary with nothing beside
it. On Linux it needs only the C and C++ runtime:

```
libc  libm  libstdc++  libgcc_s
```

libstdc++ is there because ThorVG is C++. Nothing else the toolkit uses is
loaded at run time.

The image codecs are vendored rather than taken from the system for one
reason: Android and iOS have no system copy of any of them, and a build there
would quietly lose PNG and WebP support. The cost is that nothing patches them
on your behalf, so an advisory against libpng or libwebp is a reason to bump
the pinned version promptly.

The build order is not arbitrary, in three places. zlib comes first because
libpng needs it, and both come before FreeType and ThorVG, which look for
them. FreeType is built before HarfBuzz with its own HarfBuzz support
switched off: the two can each use the other, and that is how the cycle is
broken. And libogg comes before Vorbis, FLAC and Opus, which all look for it,
with SDL_mixer after all six because it links them.

FreeType asks for credit in your product documentation, not only in a notices
file. `THIRD_PARTY_NOTICES.md` has the wording it requires.

### One is a separate repository

| Library | What it does | Licence |
|---|---|---|
| AccessTunnel | Describes the interface to a screen reader, on all five platforms | Apache 2.0 or MIT |

**AccessTunnel is a required dependency.** It is a C port of AccessKit and
lives in its own repository, expected beside this one. A toolkit that can be
built without accessibility is a toolkit that ships without it, so there is no
option to leave it out.

```
git clone https://github.com/rsv-code/access-tunnel ../access-tunnel
make -C ../access-tunnel
```

That produces `../access-tunnel/build/libaccess_tunnel.a`, which Schultz
links. Build it before Schultz; there is no cross project dependency
tracking. Point elsewhere with `make ACCESS_TUNNEL_DIR=/path/to/it`, or at one
particular archive with `make ACCESS_TUNNEL_LIB=/path/to/libaccess_tunnel.a`.

Which archive Schultz looks for follows that project's own layout, so a
straightforward build of it needs nothing said here:

| Target | Archive |
|---|---|
| macOS | `build/libaccess_tunnel.a`, universal, both architectures in one file |
| iOS | `build-ios/libaccess_tunnel.a`, one build at a time, device or simulator |
| Android | `build/<target>/libaccess_tunnel.a` |
| Linux, Windows | `build/libaccess_tunnel.a` |

`make ready` says whether the one for a target is there and whether it holds
the right architecture.

Its own building guide covers every platform, and it has been built on all of
them. See [Accessibility](accessibility.md) for how Schultz uses it.

### One is compiled in rather than built

| Library | What it does | Licence |
|---|---|---|
| libunibreak | Decides where a line may break, by the Unicode rules | zlib |

**This is not Schultz source code.** It is someone else's library, copied
unmodified into `third_party/libunibreak` and compiled alongside the toolkit's
own files. It is tracked there with its licence and its origin; see
`third_party/libunibreak/README.md`.

It is handled differently because it ships autotools only, while every other
dependency ships cmake or meson. That made it the one library needing a hand
written cross build for Android and iOS. It did not need to be: the source is
plain ISO C with no platform conditionals and no generated `config.h`, so the
build system was doing nothing that mattered. Compiling the files directly
means every target builds it identically.

To confirm the copy is unmodified:

```
sh scripts/check_vendored.sh
```

That re-extracts the release archive from `third_party/src` and diffs it
against the vendored directory, so an accidental edit is caught rather than
assumed away. Worth running in CI beside `fetch_sources.sh --verify`.

### Before building: `make ready`

Asks whether a target has everything it needs, without building anything. It
answers in a second and reports every problem at once rather than the first
one:

```
make ready                              # the machine you are on
make DEPS_TARGET=android-arm64 ready    # another target
```

It checks three things: that the dependency prefix resolves, that it resolves
**without** the system's own packages, and that the AccessTunnel archive is
there and is the right architecture. The middle one is worth having on its
own. A prefix built on this machine can quietly depend on something installed
on this machine, and then it works here and nowhere else.

On macOS `make shared` runs this for both architectures before it compiles
anything, so a missing piece on the second one is known before the first has
been built.

### "dependencies for NAME not found"

```
dependencies for windows-x86_64 not found.
Run: sh scripts/build_deps.sh
```

That target's prefix is missing something the toolkit needs. Run what it says:
the command in the message is the whole of it, and it names a `--target` only
when the target is not this machine.

The usual cause is a prefix built before a library was added to the set, on a
target you do not build every day. It is not partially usable: pkg-config
answers for the whole list or not at all, so one package it cannot resolve
takes every include path with it.

### After a pull that changes a dependency

Run the script again, then rebuild the toolkit:

```
sh scripts/build_deps.sh
make clean
make
```

On Android or iOS, name the target: `sh scripts/build_deps.sh --target
android-arm64`.

**The iOS floor is 14.0**, set in the three `scripts/targets/ios-*.sh` files
and overridable with `TARGET_MIN_VERSION`. It is 14 rather than something
older because the file picker needs it: the document picker and the `UTType`
it names content types with both arrived in iOS 14. That costs no hardware.
iOS 14 supports the same devices iOS 13 did, iPhone 6s and later, so the only
people the floor excludes are those who declined a free update.

There is nothing to decide and no flag to remember. **Every run builds the
target from nothing**, so what comes out always matches what
`scripts/versions.sh` and `scripts/build_deps.sh` currently say.

That is deliberate, and it replaced an earlier version that skipped any
library already installed in the prefix. Skipping is quick and it is wrong the
moment anything changes: the old library is installed, so it is skipped, and
the build succeeds while producing something nobody asked for.

It is worst where one library is configured against another. Turning a decoder
on in SDL_mixer only takes effect when SDL_mixer is configured, so a prefix
that already had SDL_mixer kept the decoders it was built with however the
options read. Nothing errored. The format just did not play, and on Android
and iOS no tests run to catch it.

A dependency build happens when somebody bumps a version, adds a library or
sets up a new machine. One slow build is cheaper than a silent one.

`make clean` matters for the same reason one step up: the toolkit's Makefile
does not watch the dependency prefix, and will report nothing to do even
though the libraries underneath it have changed.

## The commands

```
sh scripts/build_deps.sh                      # build for this machine
sh scripts/build_deps.sh --target NAME        # build for Android or iOS
sh scripts/build_deps.sh --fuzz               # a second prefix, for fuzzing
sh scripts/build_deps.sh --list               # what targets exist
```

**`--target` is for Android and iOS.** Linux, macOS and Windows are detected,
so on any of them the command is the same one with nothing after it, and so
is the `make` that follows. Every run builds that target from nothing.

**`--fuzz` is for fuzzing and nothing else.** It builds the same libraries a
second time, with AFL's instrumentation and AddressSanitizer, into
`build-deps/<target>-fuzz`. The ordinary prefix is left alone, and `make fuzz`
is what links against the new one. Skip it unless you are running the
fuzzers; it doubles the build time and the libraries it produces are slower
than the ordinary ones by design. See the fuzzing section of the Makefile.

Then Schultz itself. There is one command per target, named after the target,
which builds the toolkit against that target's dependencies with the compiler
that target needs. The dependencies are not built here: that is the command
above, run once, and the build says so and names it if they are missing.

```
make                                # the machine you are on
make android-arm64                  # Android devices
make android-x86_64                 # the Android emulator
```

Every file in `scripts/targets/` is a command of that name, so adding a
target file adds a command with nothing else to write.

Output goes to `build/<target>`, and the plain build stays in `build/` where
it has always been:

```
build/                              the machine you are on
build/android-arm64/
build/android-x86_64/
build-deps/<target>/                that target's dependencies and prefix
```

Objects for one machine are not objects for another, so each target keeps its
own directory. One directory holding both would produce an archive that links
nowhere.

Cleaning follows the same split. `make clean` removes this machine's build
and leaves every target's directory alone; each target has its own:

```
make clean                          this machine's build
make clean-android-arm64            that target's, and nothing else
make clean-android-x86_64
```

Neither touches the dependencies, which are slow to rebuild and have their
own command:

```
sh scripts/build_deps.sh --target android-arm64
```

A cross target builds the two archives and stops. The demo is a command line
program with a `main`, and on a phone the toolkit links into an application
rather than an executable.

`DEPS_TARGET` and `BUILD` are still there underneath for a build that wants
to place things somewhere else:

```
make DEPS_TARGET=android-arm64 BUILD=somewhere/else
```

Each dependency build writes `build-deps/<target>/env.sh`. Source it to put
that target's libraries on the search path in a shell:

```
. build-deps/linux-x86_64/env.sh
```

## Targets

| Target | Built on | Notes |
|---|---|---|
| `linux-x86_64` | Linux | The one this project develops on |
| `linux-aarch64` | 64 bit Arm Linux | A Raspberry Pi, or an Arm board or server. Built on the board |
| `macos-x86_64` | Intel Mac | Xcode command line tools |
| `macos-arm64` | Apple Silicon Mac | The same, for M series hardware |
| `windows-x86_64` | Windows | MSYS2, MINGW64 shell |
| `android-arm64` | Linux or macOS | NDK. Real devices |
| `android-x86_64` | Linux or macOS | NDK. The emulator |
| `ios-arm64` | Mac | Xcode. Real devices |
| `ios-sim-x86_64` | Intel Mac | The simulator on Intel hardware |
| `ios-sim-arm64` | Apple Silicon Mac | The simulator on M series hardware |

With no `--target`, the host is detected and the matching name is used.

### Linux

```
sudo apt install build-essential cmake meson ninja-build pkg-config python3
sudo apt install libx11-dev libxext-dev libxcursor-dev libxi-dev libxfixes-dev libxrandr-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev
sh scripts/build_deps.sh
make
```

Optional SDL features, worth installing before the dependency build rather
than after: D-Bus with IBus gives an input method for CJK text. The libdrm,
gbm and udev packages in that list are **not** optional on a machine with no
desktop; see below.

```
sudo apt install libxss-dev libxtst-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libibus-1.0-dev libdecor-0-dev libdrm-dev libgbm-dev
```

### Linux on 64 bit Arm, including a Raspberry Pi

The same, on the board. The host detects as `linux-aarch64` and the packages
above are the same names on a Pi, since it runs Debian.

```
sh scripts/build_deps.sh
make
```

Two things to expect. The dependency build compiles ThorVG, SDL and HarfBuzz
from source, which on a four core board takes the better part of an hour and
wants more memory than a Pi has cores: if a compiler is killed, build with
fewer jobs.

```
JOBS=2 sh scripts/build_deps.sh
```

### Running with no desktop

A board used as an appliance has no X or Wayland running, and SDL draws
straight to the display instead. In SDL 3 that is the **KMSDRM** driver.
There is no `/dev/fb0` driver: the old Linux framebuffer is not one of the
drivers SDL 3 has, and KMSDRM is what replaced it.

Schultz needs nothing for this. It rasterizes into its own buffer and hands
that to SDL, so it does not know or care what is showing it; it runs under
the `offscreen` driver with no window system at all.

A program can ask for a driver rather than relying on the environment:

```c
schultz_window_options options;

schultz_window_options_init(&options);
options.video_driver = "offscreen";
```

SDL settles on a driver while it starts and never revisits the choice, so
this is read once, when the first window opens. What the environment says
still wins: `SDL_VIDEO_DRIVER` set by whoever is running the program
overrides it, which is the right way round. A program says what it would
like, and a person running it on hardware nobody anticipated gets the last
word.

What it does need is for SDL to have been **built** for it, and SDL decides
that when it is configured, from what was installed at the time. Three
packages, all before the dependency build:

| Package | Without it |
|---|---|
| `libdrm-dev` | No KMSDRM. Nothing on the screen |
| `libgbm-dev` | The same |
| `libudev-dev` | The screen works and **there is no keyboard or mouse** |

That third one is the one that catches people. A desktop hands input to its
clients; with no desktop, SDL has to find the input devices itself, and it
uses udev to do it. Built without it the application starts, draws correctly,
and answers nothing. It looks exactly like a hang.

Adding the packages afterwards is not enough, because SDL is not rebuilt once
it is installed. Running the script again rebuilds it from nothing, which is
what picks them up:

```
sh scripts/build_deps.sh
```

The build reports what it got, and says plainly when either is missing:

```
  SDL video drivers: dummy kmsdrm offscreen wayland x11
```

On the board itself:

- SDL picks KMSDRM on its own when there is no `DISPLAY` or
  `WAYLAND_DISPLAY`. `SDL_VIDEODRIVER=kmsdrm` forces it.
- **Permissions, which is the usual cause of a screen that works and a
  keyboard that does not.** Input devices are `root:input` and readable only
  by their group, so the user has to be in `input`:

  ```
  sudo usermod -aG input $USER
  ```

  Then log out and back in, since a group is picked up at login. A log full
  of "could not open /dev/input/event*: Permission denied" is this, and it
  reads as a hang: the screen is right and nothing answers. Add `video` and
  `render` the same way if the display is refused as well.

- **Log in on the board, not over SSH.** The display and its devices are
  granted to whoever is logged in at the machine, by an access list the login
  manager attaches to the active session. An SSH session is not that session
  and gets neither, whatever groups the user is in.

- **Ctrl+Alt+F1 will not switch away, and that is the right trade.** SDL
  mutes the console keyboard while it runs, so what is typed reaches the
  application and not the shell behind it. The kernel never sees the
  combination, so switching terminals is gone with it.

  The alternative is worse. `SDL_MUTE_CONSOLE_KEYBOARD=0` gives switching
  back and everything typed then goes to the terminal underneath as well: a
  password typed into a field is echoed at a shell prompt, and the shell acts
  on whatever it was sent. Schultz leaves the muting alone, and an
  application should give the user its own way out rather than relying on a
  terminal behind it.

- **Leave a way out.** With nothing to close the window with, an application
  needs its own: the demo quits with Ctrl+Q or File then Quit, and a host
  does the same by calling `schultz_window_request_close`. Escape does not quit
  and should not be made to: it closes a dialog, and an application that
  ended on it would disappear the first time somebody cancelled something.
- Nothing else may hold the display. A running X server or compositor owns
  it, so stop the desktop first or boot to a console.

Cross compiling to this target from an x86-64 machine is not set up. It needs
a toolchain and a meson cross file, and the machinery here has those only for
Android and iOS.

### macOS, Intel

Xcode's command line tools give you clang. The build tools come from
Homebrew:

```
xcode-select --install
brew install cmake meson ninja pkg-config python3
sh scripts/build_deps.sh
make
```

macOS has no `sha256sum`; the script uses `shasum -a 256` there instead, and
falls back to `openssl` after that. It also exports `DYLD_LIBRARY_PATH` rather
than `LD_LIBRARY_PATH`.

Check before you rely on this: whether the Xcode version you have still runs
on Intel hardware. Apple has been winding Intel support down, and that is the
one thing here outside the project's control.

### Windows

**Git Bash is not enough.** It gives you a shell, `curl` and `tar`, but no
compiler and no build tools, so none of the vendored libraries can be built in
it. Install [MSYS2](https://www.msys2.org/) and work in its **MINGW64** shell,
which is a different shell from the plain MSYS one and has a different set of
packages on its path:

```
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-meson mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-python make tar
sh scripts/build_deps.sh
make
```

`ninja` is the one to watch on Windows. cmake there defaults to the Ninja
generator where cmake elsewhere defaults to makefiles, so a missing ninja
stops the very first library rather than waiting for the meson builds. The
script checks for it, and for everything else it needs, before it starts.

Windows has no rpath. A built DLL is found through `PATH`, which is what
`build-deps/windows-x86_64/env.sh` sets rather than a library path variable.

MSVC is a different path entirely and is not supported here: its flags have
nothing in common with the ones the Makefile passes. It is less far out of
reach than it was, though, now that no dependency needs autotools.

### Android

Cross compiled, from either Linux or a Mac. Install the NDK, point at it, and
build:

```
export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/30.0.16138531
make android-arm64
make android-x86_64
```

`arm64` is for devices and `x86_64` is for the emulator; you usually want
both. API level 24 is the default, which is the oldest SDL3 supports; override
it with `TARGET_ANDROID_API`. Only the NDK is needed, not the rest of the
Android SDK.

The script writes a meson cross file into `build-deps/<target>/meson-cross.txt`
from the NDK's clang wrappers, and passes the NDK's own cmake toolchain file
to SDL.

SDL is built with `SDL_ANDROID_JAR=OFF`. What is built here is the native
side: static archives for an application to link. On Android SDL also
compiles its own Java activity classes into a jar, and those belong in the
application's project next to its manifest, built against the `compileSdk`
that project chose. Left on, the build stops, because the jar is compiled
against whichever `android.jar` cmake finds and SDL's Java is newer than the
platform an NDK-only install has lying about.

The result is two archives per ABI:

```
build/android-arm64/libschultz.a          the toolkit
build/android-arm64/libschultz_all.a      the toolkit and its dependencies
```

`libschultz_all.a` is the one to hand to an application: it carries SDL,
ThorVG, FreeType, HarfBuzz and the rest, so the application links one file.

### Which way up

`allow_rotate` in the window options lets the screen turn with the device,
and `orientation` holds it to portrait or to landscape. Both are read when
the window is made.

They reach the platform through one SDL flag, `SDL_WINDOW_RESIZABLE`, which
iOS and Android read as "these orientations are permitted" and a desktop
reads as "the edges may be dragged". Schultz keeps the two apart and sets the
flag from whichever of `resizable` and `allow_rotate` the platform in front
of it is actually asking about.

Both only narrow what the application already permits. An activity that
declares `android:screenOrientation`, or an Info.plist naming one
orientation, is not overruled from here.

**A panel mounted the other way round is a different problem.** No video
driver but the two mobile ones takes any notice of orientation: SDL's hint is
read by uikit and android and by nothing else, and Wayland only reports which
way a screen is, without offering to change it. An embedded board with a
portrait display fitted sideways needs the output itself turned, and that is
what the third option, `turn`, does.

`turn` names the quarter turn between what Schultz draws and what reaches the
panel: `SCHULTZ_TURN_NONE`, `_90`, `_180`, `_270`, or `SCHULTZ_TURN_AUTO`,
which is the default and asks `orientation` instead. Auto turns a quarter
when the panel's shape does not match the shape `orientation` asked for, and
leaves anything already the right shape alone. Naming a turn is the only way
to say which quarter of the two, and the only way to say upside down at all:
a panel fitted rotated 180 degrees needs `SCHULTZ_TURN_180`, and no
description of the shape wanted can imply it.

The interface is built in the shape it wants, so a quarter turn on a 1024 by
600 panel gives a 600 by 1024 buffer to lay out in. The turn is applied once,
on the way to the panel, and a pointer is turned back on the way in, so
nothing above the platform layer knows the panel is mounted sideways.

`turn` is ignored on phones and tablets, where the device turns the screen
itself and turning it again would only fight it. `allow_rotate` and
`orientation` are the options for those.

All three are options read when the window is made. Two of them can also be
changed while it is open:

```c
schultz_window_set_turn(window, SCHULTZ_TURN_90);
schultz_window_set_orientation(window, SCHULTZ_SCREEN_PORTRAIT);
```

`schultz_window_set_turn` rebuilds the buffer in the shape the new turn calls
for and everything lays out again on the next redraw. It is exact everywhere,
and it is the one a fixed panel needs.

`schultz_window_set_orientation` is exact on a fixed panel too, where
`SCHULTZ_TURN_AUTO` resolves against it. **On a phone it reaches less than
the option does, and the limit is SDL's.** SDL reads the orientations hint
when it makes the window. iOS asks for it again whenever the system
re-examines the view controller, so a change is picked up at the next
rotation; Android reads it again only when the window's resizability changes,
so a change may not be noticed at all. Where a phone is the target, say it in
the options.

The demo takes `--turn 0|90|180|270` for trying this on a desktop.

The application itself lives in its own repository. This one produces the
libraries; the manifest, the Gradle project and the Java side belong with the
application, along with `Delegate.java` from the AccessTunnel project if
accessibility is wanted.

### iOS

Built on a Mac with Xcode.

```
sh scripts/build_deps.sh --target ios-arm64        # devices
sh scripts/build_deps.sh --target ios-sim-x86_64   # simulator, Intel Mac
```

An Intel Mac builds device binaries for arm64 without trouble: the compiler
targets what you ask for, not what it runs on. The simulator is the exception,
because it runs the host architecture, which is why an Intel Mac needs the
x86-64 simulator target and an Apple Silicon Mac would need an arm64 one.

## Accessibility is one file per platform

The Makefile picks an accessibility backend from the target's name, so
`DEPS_TARGET` chooses that too. On macOS and iOS the backend is Objective-C,
which is the only Objective-C in the project, and it is compiled without
`-fobjc-arc`: it uses manual retain and release to match AccessTunnel's own
shells, and refuses to compile with automatic reference counting on.

An Android application also has to add `Delegate.java` from the AccessTunnel
repository to its own sources. See `docs/accessibility.md`.

## Going into a shared library

Everything is compiled `-fPIC`, the toolkit and every vendored dependency, so
the archives can go inside a `.so`, `.dylib` or `.dll`. Ordinary code cannot:
the linker refuses it.

Everything is also compiled `-fvisibility=hidden`, and the public headers put
the visibility back for what they declare, with a pragma around their
declarations. So a shared library built from these archives exports the
toolkit's own interface and nothing else. That matters when the library is
loaded into a process that has its own FreeType, libpng and zlib, which is
what happens under a Java virtual machine: two copies of a symbol in one
process is one too many, and whichever the loader picks, somebody's calls
land in the wrong implementation.

`scripts/check_exports.sh` builds a shared library from `libschultz.a` and
checks both halves: that nothing but `schultz_*` comes out, and that every
function the public headers declare does. The second is the quiet failure. A
header that loses its pragma hides everything it declares, which is tens of
functions out of hundreds, and the library is then useless to a host rather
than dangerous to one.

```
  exported: 527 symbols, 0 of them not schultz_*
  declared in the public headers: 527, missing: 0
```

**A new public header needs the pragma.** Copy it from any of the others; the
check fails by name if it is forgotten.

## Using Schultz from another project

`make` produces the demo and two archives:

| File | Holds | Use it when |
|---|---|---|
| `build/libschultz.a` | Schultz only, about 7 MB, most of it the two built in fonts | You already manage the dependencies yourself |
| `build/libschultz_all.a` | Schultz plus every dependency, about 26 MB | You want to link one file |

`make lib` builds just the two archives and skips the demo.

Both archives contain the whole toolkit apart from the demo's `main`: the
widgets, the layout engine, the ThorVG painter, the SDL window and its loop,
the accessibility bridge, and the compiled in libunibreak. `libschultz_all.a`
adds AccessTunnel, SDL3, SDL_mixer, ThorVG, FreeType, HarfBuzz, SheenBidi,
libpng, libwebp, libsharpyuv, zlib and the six audio decoders, merged in as
objects.

### Linking the single archive

This is the shorter path, and the one a mobile build wants, because Xcode and
the Android NDK are both happier with one archive than with ten.

```
make install PREFIX=/opt/schultz

cc -I/opt/schultz/include app.c /opt/schultz/lib/libschultz_all.a -lstdc++ -lm -lpthread -ldl
```

Without an install, point the compiler at the source tree's parent instead,
so that `<schultz/schultz_api.h>` still resolves, and name the archive where
it was built:

```
cc -I/path/to app.c /path/to/schultz/build/libschultz_all.a -lstdc++ -lm -lpthread -ldl
```

`-lstdc++` is not optional. ThorVG is C++, and an archive does not carry a
note saying which runtime it needs, so the consuming link line has to say it.
The trailing system libraries differ by platform, and the target file says
which: macOS and iOS want `-lc++`, because Apple dropped libstdc++ with Xcode
10, plus `Foundation` and either `AppKit` or `UIKit` for the accessibility
backend. Windows adds the libraries MinGW's SDL names. Ask `pkg-config`
rather than guessing:

```
pkg-config --libs --static sdl3
```

### The shared library

For a consumer that would rather load one file than link one, which usually
means a language binding:

```
make shared
```

It is the merged archive as a shared library, and it is self contained the
same way: SDL, ThorVG, FreeType, the audio decoders and AccessTunnel are all
inside it. The name follows the platform, `libschultz.so`, `libschultz.dylib`
or `schultz.dll`, and Windows gets an import library beside it.

**It exports `schultz_*` and nothing else.** That matters more here than for
an archive. An archive offers the linker a menu and it takes what it needs; a
shared library has everything in it and publishes a list, so without one it
would publish `SDL_*`, `FT_*`, `png_*` and `z_*` as well. A JVM that already
has its own zlib would then have two, and whichever the dynamic linker binds
first wins for both. The list is a version script on Linux and Android, an
exported symbols list on macOS and iOS, and a generated `.def` on Windows.

What it needs beside it is the platform and nothing else. On Linux:

```
libm.so.6  libstdc++.so.6  libc.so.6
```

On Android and Windows the C++ runtime goes inside rather than beside, because
the NDK's libc++ is not on a phone and MinGW's runtime DLLs are not on a
Windows machine. Windows takes `-Wl,-Bstatic` with it, so that the runtime's
own dependency on `libwinpthread-1.dll` resolves to the archive as well. What
a Windows build ends up importing is the Windows API and nothing else: no
`libwinpthread-1.dll`, no `libstdc++-6.dll`, no `zlib1.dll`. An Android build asks only for `libm`, `libc`, `libdl`,
`liblog`, `libandroid`, `libOpenSLES` and the GLES libraries, all of which are
the platform.

**On macOS it builds both architectures and joins them.** Apple Silicon and
Intel are separate targets here, each with its own prefix, so a library that
runs on both is two builds and a `lipo`. `make shared` does all three, and
`lipo -archs` on the result reports `x86_64 arm64`.

It cannot be one pass: ThorVG, FreeType, HarfBuzz and SheenBidi are built with
meson, and meson gives one architecture per build directory. So both
dependency prefixes have to exist first, which is once per machine:

```
sh scripts/build_deps.sh --target macos-arm64
sh scripts/build_deps.sh --target macos-x86_64
make shared
```

If either is missing the build stops and names the command for it. The two
slices land in `build/macos-arm64/` and `build/macos-x86_64/`, and the
universal library in `build/libschultz.dylib`.

AccessTunnel has to cover both architectures too, and its own default build
on macOS does exactly that: it builds each architecture and joins them with
`lipo` into one universal archive. Both slices here point at that one file, so
there is nothing to pass.

To use an archive somewhere else, name it once. A variable given on the
command line reaches the slice builds:

```
make ACCESS_TUNNEL_LIB=/somewhere/libaccess_tunnel.a shared
```

For another target, name it:

```
make DEPS_TARGET=android-arm64 BUILD=build/android-arm64 shared
```

Naming a target builds that one architecture alone, which is also how the two
macOS slices are built, so `make DEPS_TARGET=macos-arm64 shared` is a single
Apple Silicon library rather than a universal one.

### Linking the split archives

`make install` lays out a prefix that `pkg-config` can read:

```
make install PREFIX=/opt/schultz
```

That writes the headers to `/opt/schultz/include/schultz/`, `libschultz.a`
and every dependency archive to `/opt/schultz/lib/`, and a `schultz.pc` to
`/opt/schultz/lib/pkgconfig/`. A consumer then asks for the whole line:

```
export PKG_CONFIG_PATH=/opt/schultz/lib/pkgconfig
cc $(pkg-config --cflags schultz) app.c $(pkg-config --libs --static schultz)
```

`DESTDIR` works as usual for staging into a package tree.

### The headers

The umbrella header pulls in everything a host needs:

```c
#include <schultz/schultz_api.h>
```

Nothing below that is private in the sense of being hidden, but
`schultz_api.h` is the surface that is kept stable. The internal headers, such
as `schultz_sdl.h` and `schultz_a11y.h`, are not installed.

### One archive per target

An archive holds object code for one architecture, so there is one per
target, in that target's own directory:

```
make ios-arm64          ->  build/ios-arm64/libschultz.a
make android-arm64      ->  build/android-arm64/libschultz.a
```

The target name picks the toolchain as well as the dependencies. Each
dependency build writes `build-deps/<target>/config.mk` naming the compiler
and the flags that target needs, and the Makefile reads it. Without that the
dependencies would be built for the target and the toolkit for the machine
doing the building, and the two would only fail to fit at link time.

On a cross target `make` builds the two archives and stops. The demo is a
command line program with a `main`, and an iOS or Android build wants the
toolkit to link into an app rather than an executable it cannot run.

On Apple platforms, combine the device and simulator archives into an
`.xcframework` with `xcodebuild -create-xcframework` rather than trying to
merge them into one file.

Merging archives has no portable command, so `scripts/merge_archives.sh` does
it: GNU binutils `ar` reads a script in what it calls MRI mode, Apple's `ar`
has no such mode, and macOS ships `libtool` for the job instead. The script
picks whichever the machine has.

## What is actually verified

Being straight about this, because the difference matters:

| Target | State |
|---|---|
| `linux-x86_64` | **Built and tested.** The whole dependency set builds from the vendored sources, and Schultz builds, tests and runs against it |
| `android-arm64` | **Built.** The dependency set and both archives build, cross compiled from Linux with the NDK, and the objects are aarch64. Not yet run on a device |
| `android-x86_64` | **Built.** The same, and the objects are x86-64. Not yet run on the emulator |
| everything else | **Written, not run.** The configuration is considered and the cross files are generated, but no one has executed these on real hardware yet |

Expect the first run on each new target to need fixing. The likely places, in
order: SDL's platform feature detection, and pkg-config finding the right
libdir. The autotools cross build that used to head this list is gone, because
libunibreak is compiled in now.

Android bore that warning out on its first run, and it is worth recording
what actually broke, because it was none of the above. Everything the cross
build itself does worked: the meson cross file, the NDK toolchain, pkg-config
and the per target `config.mk`. What stopped it was SDL compiling its own
Java classes against an `android.jar` older than those classes need, which is
why `SDL_ANDROID_JAR` is off. Nothing else needed changing.

There is no application shell in this repository, and there is not meant to
be. No `AndroidManifest.xml`, no Gradle project, no Xcode project. These
targets build the *libraries*; the applications that use them live in their
own repositories.

## Bumping a dependency

The archives are committed, so an upgrade is a deliberate act:

1. Change the version and SHA-256 in `scripts/versions.sh`.
2. `sh scripts/fetch_sources.sh` downloads the new archive and verifies it.
3. Delete the old archive from `third_party/src`.
4. `sh scripts/build_deps.sh --target NAME`, on every target you ship, then `make clean` and `make`.
5. Commit the archive change with everything else.

For libunibreak there is a sixth step, because its source is vendored as well
as archived: refresh `third_party/libunibreak` from the new archive and run
`sh scripts/check_vendored.sh`. Its README has the detail.

`sh scripts/fetch_sources.sh --verify` downloads nothing and just checks the
vendored archives against the recorded hashes. That is the one to run in CI.
