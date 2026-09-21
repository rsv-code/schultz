# Schultz UI Framework

![Schultz Logo](branding/schultz-img-small.png)

Schultz is a retained mode user interface toolkit written in C and based on
SDL. It builds on iOS, Android, Linux, macOS, Windows, and embedded Linux
(aarch64 such as RPI) and will run with a display manager or headless via
frame buffer.

You describe a screen as a tree of nodes; Schultz lays it out, draws it,
and routes input back to you.

It draws every pixel itself. There is no native control anywhere, so a button
on a phone and a button on a desktop are the same code drawing the same
shapes and cannot drift apart.

![The demo application](docs/img/overview.png)

Everything on that screen is Schultz drawing itself. It is the demo that
ships with the toolkit, and it runs in a minute: see
[Getting started](docs/getting-started.md).

## Why you might want it

- **Retained, not immediate.** Build the tree once and change the parts that
  change. Only the region that actually changed is repainted, so a still
  screen costs almost nothing.
- **One interface, many languages.** Everything is C functions taking opaque
  64 bit handles. No structs across the boundary, no pointers to keep alive,
  no callbacks the toolkit owns. Use a handle after its node is gone and you
  get an error code, not a crash.
- **Text is measured inside.** Shaping, wrapping and line breaking use
  HarfBuzz, FreeType, libunibreak and SheenBidi, in the toolkit. Asking the
  host how wide a string is, is the call most toolkits cannot avoid and the
  one that makes a language binding slow.
- **A screen reader can read it.** Every widget carries a role, a name, a
  value and a set of actions, published to the platform's own accessibility
  interface. Requests come back through the same path real input takes.
- **One archive to link.** `make lib` produces `libschultz_all.a` with every
  dependency merged in, which is what a mobile build wants.

## Where it runs

| Platform | Display |
|---|---|
| Linux, x86-64 and 64 bit Arm | X11, Wayland, or straight to the screen with no desktop |
| Raspberry Pi | The same, and the target it was tuned on |
| macOS, Intel and Apple Silicon | Cocoa |
| Windows 10 and later | Win32, built with MinGW |
| iOS and the simulator | UIKit |
| Android | The NDK |

Every dependency is vendored as source and built from it. Nothing is
downloaded during a build. See [Building](docs/building.md).

## What using it looks like

```c
schultz_window_options_init(&options);
options.title = "Hello";
schultz_window_create(&options, &window);

tree = schultz_window_tree(window);
schultz_font_load_file(schultz_window_fonts(window),
                       "assets/fonts/DejaVuSans.ttf", 16.0f, &font);

schultz_panel_create(tree, schultz_tree_root(tree), &shell);
schultz_node_set_pane(tree, shell, schultz_pane_vbox());
schultz_button_create(tree, shell, "Say hello", &button);
schultz_node_set_token(tree, button, 1);

schultz_window_run(window, NULL, NULL, 0);
schultz_window_destroy(window);
```

A whole program, including the event handler, is in
[Getting started](docs/getting-started.md).

## Documentation

Everything below is published at
**[rsv-code.github.io/schultz](https://rsv-code.github.io/schultz/)**, which is
the same pages rendered, and the only place the API reference is readable
without cloning.

| Guide | What it covers |
|---|---|
| [Getting started](docs/getting-started.md) | Running the demo, and a first application that compiles |
| [Building](docs/building.md) | Every target, the vendored sources, and linking Schultz into another project |
| [Concepts](docs/concepts.md) | The tree, panes, the window, events, pixels, and how a repaint is decided |
| [Widgets and containers](docs/widgets.md) | Every widget that ships, with a picture and code for each |
| [Styling](docs/styling.md) | Tokens, properties, states, and the two themes |
| [Audio](docs/audio.md) | Sound, music, streaming and the microphone |
| [Accessibility](docs/accessibility.md) | How the tree reaches a screen reader, and what makes a widget readable |
| [Repainting](docs/repainting.md) | What gets redrawn each frame, and what to check when a pixel goes stale |

**[API reference](https://rsv-code.github.io/schultz/api/)** -- every function,
type and constant, generated from the comments in the headers. It is committed
under `docs/api`, so it is in a clone as well, but read it at that address:
GitHub shows a checked in HTML file as its source rather than as a page.
`make docs` regenerates it after a header's comments change.

Every C snippet in these pages, this one included, is put through the compiler
by `sh scripts/check_doc_code.sh`.

## Third party libraries

Schultz draws, shapes and decodes everything itself, which means it stands on
a fair amount of other people's work. All of it is vendored as source and
built from that source, so a build downloads nothing and a checkout is the
whole thing.

| Library | What it does here |
|---|---|
| SDL3 | The window, the mouse, the keyboard, touch, the clipboard and the audio device |
| SDL_mixer | Picks a decoder for a sound file and mixes what plays |
| ThorVG | Turns shapes into pixels, in software, on every target |
| FreeType | Turns glyphs into bitmaps |
| HarfBuzz | Turns text into positioned glyphs |
| SheenBidi | Puts mixed direction text in visual order |
| libunibreak | Decides where a line may break |
| zlib, libpng, libwebp, libjpeg-turbo | Pictures |
| libvpx | VP8 and VP9, both directions, for video |
| nestegg | Reads WebM: what tracks a file holds and what the next packet is |
| Opus, opusfile, libogg, libvorbis, FLAC, WavPack | Sound |
| speexdsp | Resampling and echo cancellation for the microphone |
| [AccessTunnel](https://github.com/rsv-code/access-tunnel) | Publishes the tree to each platform's accessibility API. A required dependency, and the one thing not vendored here: see [Building](docs/building.md) |

Two more ship as data rather than code: **DejaVu** is the bundled font family,
and **Noto Color Emoji** is what draws emoji.

**None of it is copyleft**, which is what let each one be chosen: Schultz is
GPL and a commercial licence has to remain possible. Exact versions, licence
texts, patent grants, and the components bundled inside these libraries are
in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Versioning

Schultz is **0.1.0-alpha**. While the major number is zero, nothing here is
promised: any release may change or remove anything, so pin an exact version.

From 1.0 the numbers mean what they mean for a C library, which is about what
already-built programs experience rather than about how large the change was:

| What changed | Old source still compiles | Old binaries still run | Bump |
|---|---|---|---|
| Only the implementation | yes | yes | patch |
| Something **added** to a header | yes | yes | minor |
| Something existing **changed or removed** | no | no | major |

The trap worth naming, because it catches anyone arriving from a language
with a package manager: **adding a field to a public struct is a major
change.** A program compiled against the old header allocates the old, smaller
struct, and the new library then reads past the end of it. That is why
everything with state in Schultz is behind an opaque 64 bit handle, and why
the structs that are public are mostly value types with no room to grow.

Three numbers to tell apart:

- `SCHULTZ_VERSION_MAJOR`, `_MINOR`, `_PATCH` in `schultz.h` say what you
  **compiled** against, and being macros they work in `#if`.
- `schultz_version()` says what is **running**, which on a shared library need
  not be the same.
- The soname, `libschultz.so.0`, is what the loader matches on. It follows the
  major number, so every build against any 0.x keeps loading and a 1.0 does
  not.

All of it comes from the three `#define`s in `schultz.h`. The Makefile reads
them, and the shared library's name, the `pkg-config` file and the API
reference all follow, so a release is one edit.

## License

Copyright 2026 Austin Lehman.

Schultz is licensed under the GNU General Public License version 3 only. The
full text is in [LICENSE](LICENSE).

Commercial licensing options are available. Reach out to Austin Lehman.

Schultz also ships and links third party components, and those keep their own
licences. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
