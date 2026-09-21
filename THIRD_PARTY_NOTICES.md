# Third Party Notices

Schultz is licensed under the GNU General Public License version 3 only. It
uses the third party components listed below, and **each of those keeps its
own licence.**

Those conditions travel with the software whatever terms you hold Schultz
under, because they are not Schultz's to change. Two of them ask for
something active rather than passive: FreeType wants credit in your product
documentation, and the bundled fonts carry three conditions of their own.

This file records what is present in the repository today. A component is
added here as it lands.

## Vendored

### greatest

- Location: `third_party/greatest/greatest.h`
- License: ISC
- Used for: unit test harness
- Distributed with the library: no. greatest is linked only into test
  binaries, which are not shipped.

```
Copyright (c) 2011-2021 Scott Vokes <vokes.s@gmail.com>

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

### Bundled fonts

- Location: `assets/fonts/`, and compiled into the library itself
- Files: `DejaVuSans.ttf`, `DejaVuSans-Bold.ttf`, `DejaVuSans-Oblique.ttf`,
  `DejaVuSans-BoldOblique.ttf`, `DejaVuSansMono.ttf`
- Version: DejaVu 2.37
- License: Bitstream Vera Fonts License, plus public domain DejaVu changes.
  Full text in `assets/fonts/LICENSE-DejaVu.txt`.
- Distributed with the library: yes, twice over. The files ship in
  `assets/fonts/`, and the same five faces are also built into `libschultz.a`
  by `scripts/embed_fonts.sh`, so a program that says nothing about fonts
  still draws text. They are embedded unmodified, byte for byte. This is what
  makes rendering identical on every target without depending on what the
  system provides.

The license permits use, copying, merging, publication, distribution and sale,
free of charge, for commercial and closed source products alike. Three
conditions attach, and they pass on to anyone who redistributes Schultz with
the bundled fonts:

1. **The notice travels with the fonts.** The copyright, trademark and
   permission notices must be included in all copies of the typefaces.
   `assets/fonts/LICENSE-DejaVu.txt` satisfies this.

   Because the faces are also compiled into the library, this condition
   reaches any binary that links Schultz, not only a bundle that carries the
   `assets/fonts/` directory. Anything shipped with Schultz in it therefore
   has to carry that license text somewhere a user can read it, which for an
   application usually means an "acknowledgements" or "open source licenses"
   screen. This file and `LICENSE-DejaVu.txt` are what to put there.
2. **A modified font must be renamed** so its name contains neither
   "Bitstream" nor "Vera". Note that subsetting a font to reduce an
   application bundle counts as modification, which makes this condition
   relevant on mobile targets. Schultz embeds the faces unmodified, so the
   library itself never triggers this; a build that subsets them to save
   space does.
3. **The fonts may not be sold by themselves.** They may be sold as part of a
   larger software package, which is what bundling them in Schultz does, but
   the typefaces alone are not a product.

A fourth, minor condition: the Gnome, Gnome Foundation and Bitstream names may
not be used in advertising without prior written authorization.

These conditions are close to universal among freely licensed fonts. SIL Open
Font License 1.1, which covers Noto and Inter, carries the same no-sale-alone
rule and the same rename-on-modification rule through its Reserved Font Names
clause, so switching families would not remove them.

### Bundled emoji face

- Location: `assets/fonts/`, and compiled into the library itself
- File: `NotoColorEmoji-COLRv1.ttf`
- Version: Noto Color Emoji 2.051, built 2025-08-18, published by Google in
  the `googlefonts/noto-emoji` repository as `Noto-COLRv1.ttf`. Embedded
  unmodified, byte for byte, under a filename that says which format it is.
- Copyright: Copyright 2022 Google Inc.
- License: SIL Open Font License, Version 1.1. Full text in
  `assets/fonts/LICENSE-Noto.txt` and at http://scripts.sil.org/OFL
- Trademark: Noto is a trademark of Google Inc.
- Distributed with the library: yes, the same two ways the text faces are.
  It is the face that draws any character no text face carries, which in
  practice means emoji.

No text face in the world carries emoji, so a toolkit that draws them has to
carry one or go looking for one on the machine it is running on. Looking is
the worse answer: the artwork differs on every platform, the way to ask
differs on every platform, and on some there is nothing to find. One bundled
face draws the same picture everywhere, which is the same reason the text
faces are bundled.

The OFL attaches five conditions. Three of them bear on shipping Schultz:

1. **The font may not be sold by itself.** It may be sold as part of a larger
   software package, which is what bundling it here does.
2. **Every copy must carry the copyright notice and the license.** These may
   travel as a stand-alone file, as a human-readable header, or as
   machine-readable metadata a user can view. All three are satisfied:
   `assets/fonts/LICENSE-Noto.txt` is the stand-alone copy, the generated
   `schultz_font_builtin.c` carries the notice in its header, and the font's
   own name table states its copyright and license, so the statement travels
   inside the bytes wherever they go.

   Because the face is compiled into the library, this reaches any binary
   that links Schultz, not only a bundle carrying the `assets/fonts/`
   directory. Anything shipped with Schultz in it therefore has to carry that
   license text somewhere a user can read it, which for an application
   usually means an "acknowledgements" or "open source licenses" screen.
   This file and `LICENSE-Noto.txt` are what to put there.
3. **The Font Software must stay under this license and no other.** It is not
   relicensed to GPL by being compiled in. The generated source file is
   marked `GPL-3.0-only AND OFL-1.1 AND Bitstream-Vera` rather than
   `GPL-3.0-only` for exactly this reason: the code in it is the toolkit's,
   the font bytes in it are not, and a single license tag on the file would
   have claimed otherwise.

The other two do not arise. **No Reserved Font Name is declared** -- one is
declared by naming it after the copyright statement, and this font's is bare
-- and in any case that condition only binds a modified version, which this
is not. The condition about not using the copyright holder's name to promote
or endorse is met by saying plainly here that **nothing about this bundling
implies Google endorses Schultz**; naming Noto as the source is the
acknowledgement the license explicitly permits.

One consequence worth stating for anyone comparing it with what the toolkit
shipped before: OFL asks for the license to travel, where a Creative Commons
attribution license would ask each application to credit the artwork. The
first is a file in a bundle; the second is a line on a screen somebody has to
remember to add.

## Linked Libraries

Vendored as source in `third_party/src` and built by `scripts/build_deps.sh`,
then linked into Schultz. libunibreak is the exception: its source is also
vendored, unmodified, in `third_party/libunibreak` and compiled directly into
the toolkit rather than linked. `scripts/check_vendored.sh` proves that copy
matches the release archive. None are
copyleft, which is what let each of them be chosen.

| Library | Version | License |
|---|---|---|
| SDL3 | 3.4.16 | zlib |
| SDL_mixer | 3.2.4 | zlib; see the decoder note below |
| ThorVG | 1.1.1 | MIT, and three bundled components of its own; see below |
| FreeType | 2.14.3 | FreeType License (FTL) |
| HarfBuzz | 14.4.0 | Old MIT |
| SheenBidi | 3.0.0 | Apache 2.0 |
| libunibreak | 7.0 | zlib |
| zlib | 1.3.2 | zlib |
| libpng | 1.6.58 | PNG Reference Library License version 2 |
| libwebp | 1.6.0 | BSD 3-Clause (Google) |
| libjpeg-turbo | 3.2.0 | IJG License and BSD 3-Clause, plus a bundled libspng under BSD 2-Clause; see below |
| libogg | 1.3.6 | BSD 3-Clause (Xiph.Org) |
| libvorbis | 1.3.7 | BSD 3-Clause (Xiph.Org) |
| FLAC | 1.5.0 | BSD 3-Clause for the library; **the command line tools in the same archive are GPL and are not built**, see below |
| Opus | 1.5.2 | BSD 3-Clause, plus royalty free patent grants; see below |
| opusfile | 0.12-59-g6dfd29e | BSD 3-Clause (Xiph.Org) |
| WavPack | 5.9.0 | BSD 3-Clause (David Bryant) |
| libvpx | 1.15.0 | BSD 3-Clause (Google), **plus a separate patent grant**; see below |
| speexdsp | 1.2.1 | Revised BSD, **seven copyright holders**; see below |
| AccessTunnel | 0.1.0 | Apache 2.0 or MIT |

AccessTunnel is a separate project, expected beside this one and linked as a
static library. It carries portions derived from Chromium under a BSD style
licence; its own `THIRD_PARTY_NOTICES.md` records them. It also ships a
`NOTICE` file naming AccessKit as the work it was ported from, and Apache 2.0
asks that such a file travel with a redistribution, so it has to be carried
rather than summarised.

### What SDL brings with it

SDL is zlib, and two components inside its tree are not.

| Component | Why it is there | License |
|---|---|---|
| HIDAPI, in `src/hidapi` | `SDL_HIDAPI` is on by default, and this build leaves it on: `SDL_JOYSTICK_HIDAPI` is set in the generated config | **Three licenses, one to be chosen**; see below |
| yuv2rgb, in `src/video/yuv2rgb` | part of SDL's surface conversion | BSD 3-Clause (Adrien Descamps) |

**HIDAPI is offered under three licenses and the choice belongs to whoever
uses it**: GPL version 3, a BSD style license, or the more liberal original
HIDAPI license. Leaving that unstated is the problem, not the licensing: a
component with a GPL option in it needs the election written down, or an
audit has to guess which one was taken.

**Schultz elects the BSD style license**, in `src/hidapi/LICENSE-bsd.txt`
inside the SDL release. It is permissive, it suits the commercial option, and
nothing here needs the terms the GPL choice would add.

Nothing in Schultz itself uses a joystick or a gamepad, so whether to build
HIDAPI at all was asked and answered with a measurement rather than a
preference. **It stays on.** Turning it off is `-DSDL_HIDAPI=OFF` in
`scripts/build_deps.sh`, and what that would save is:

| | |
|---|---|
| HIDAPI objects in `libSDL3.a` | 547 KB of object files |
| Of those, loadable text, data and bss | 246 KB |
| HIDAPI symbols in the linked demo | 165 KB, against a 17.1 MB binary |

That is about one percent, against an embedded emoji face of 4.8 MB in the
same binary. It is also not dead weight: leaving it on is what lets an
application built on Schultz use a gamepad, which the toolkit not needing one
has no bearing on. The numbers are here so that the question is reopened with
evidence rather than from memory.

SDL's `test/` directory carries licenses of its own, including a font under
different terms. None of it is built: `-DSDL_TEST_LIBRARY=OFF` and
`-DSDL_EXAMPLES=OFF`, so nothing from there reaches the binary.

### What ThorVG brings with it

ThorVG is MIT, and the loaders Schultz asks for bring two more components
into the binary. Its PNG, JPEG and WebP loaders each take an outside library
when they find one, and all three are pinned in `third_party/src` so that they
always do: libpng, libjpeg-turbo and libwebp. Those are listed with the other
linked libraries above rather than here.

ThorVG carries decoders of its own for all three, used only when an outside
library is absent. Before libjpeg-turbo was vendored that is what happened to
JPEG on some machines and not others, which is the reason it is vendored. The build asks for
`-Dloaders=svg,lottie,ttf,png,jpg,webp -Dextra=lottie_exp`
(`scripts/build_deps.sh`), and each of these is compiled because of one of
those options:

| Component | Why it is there | License |
|---|---|---|
| JerryScript | `-Dextra=lottie_exp`, which is what runs expressions in a Lottie file | Apache 2.0 |
| RapidJSON | the Lottie loader, which parses JSON | MIT |
| A WebP loader from libwebp | `-Dloaders=webp` | BSD 3-Clause (Google) |

None is a separate download and none is copyleft. JerryScript matters for the
same reason SheenBidi does: Apache 2.0 is compatible with GPLv3 and not with
GPLv2.

Turning the Lottie loader off would remove the first two. Schultz ships a
Lottie widget, so it stays on.

### libjpeg-turbo carries two licences and a third component

The library is covered by two BSD style licences at once, both permissive and
both allowing commercial use:

- **The IJG License** covers the libjpeg API and everything inherited from the
  original Independent JPEG Group code. Its text is `README.ijg`.
- **The Modified 3-clause BSD License** covers the TurboJPEG API, which is the
  part ThorVG calls, and the build system.

The TurboJPEG API wraps the libjpeg one, so both apply to what is linked here.

It also carries **libspng**, under BSD 2-Clause, which the TurboJPEG API uses
to read and write PNG files. Nothing in Schultz calls that part, and it is
compiled in regardless, so it is named here. `-DWITH_SYSTEM_ZLIB=ON` keeps the
copy of zlib it would otherwise compile in as well, so the zlib in the binary
is the one pinned here and there is only one of it.

### The image codecs

zlib, libpng and libwebp are vendored rather than taken from the system,
because Android and iOS have no system copy of any of them and a build there
would silently lose PNG and WebP support. They decode untrusted input, so an
advisory against libpng or libwebp is a reason to bump the pinned version
promptly; nothing patches them on your behalf any more.

libwebp's tree also builds `sharpyuv`, under the same licence. It is not a
separate download.

### On copyleft

There is none in anything that is compiled or linked, other than Schultz's
own code.

Every one of these licences is permissive, so none of them adds an obligation
of its own beyond attribution. Two are worth naming because they are
compatible with GPLv3 and **not** with GPLv2, which is one reason the licence
is version 3 only: the FreeType License, and Apache 2.0 for SheenBidi,
AccessTunnel and JerryScript inside ThorVG.

Every source file that is built, across all three of the newly vendored
libraries, was checked for GPL, LGPL and MPL text: zero matches. The licences
are zlib for zlib, the PNG Reference Library License version 2 for libpng, and
BSD 3-Clause for libwebp. All three are permissive, allow commercial use, and
require only that the notices travel with a redistribution.

Copyleft text does appear in three places that are neither compiled nor
shipped:

- **FLAC's two command line tools**, `flac` and `metaflac`, which are GPL and
  live in the same archive as the BSD licensed library. `BUILD_PROGRAMS=OFF`
  builds the library alone. This one is checked rather than trusted; see
  "FLAC: one archive, two licences" below.
- **Autotools scaffolding**, in libpng's release tarball and in the five
  audio archives that ship one (`config.sub`, `config.guess`, `ltmain.sh`,
  `configure`, and the rest). These are GPL, and they carry the standard
  autoconf exception saying so explicitly: a program configured by them may be
  distributed under whatever terms its own authors chose. Everything here is
  built with cmake, so these files are not even executed.
- **zlib's `contrib/` directory**, which holds bindings for other languages,
  one of which mentions the GPL. Every `contrib` library is an option that
  defaults to off, and Schultz does not turn any of them on.

None of the three reaches the binary.

**FreeType requires credit in the product documentation**, not only in a
notices file. The required wording is:

```
Portions of this software are copyright (c) 2026 The FreeType Project
(www.freetype.org). All rights reserved.
```

## Notes For Components Still To Come

Two obligations are known in advance and are recorded here so they are not
missed at release time.

- **SheenBidi** is under Apache 2.0 with no alternative license offered. It is
  the reason a GPLv2 only project cannot link Schultz, independent of
  Schultz's own license choice. It is now an actual dependency, not a planned
  one, so that constraint is live.

## Audio Decoders

SDL_mixer is vendored and built, and it is zlib licensed like SDL itself.
What matters is which of its decoders are compiled in, because several are
copyleft: fine for the GPL build and wrong for the commercial one, since
everything here links statically.

### What is switched off

These are off in `scripts/build_deps.sh`, each for a licence rather than a
preference:

| Option | What it would have brought | Why it is off |
|---|---|---|
| `SDLMIXER_MOD=OFF` | libxmp, for MOD, XM, S3M and IT | LGPL |
| `SDLMIXER_MP3_MPG123=OFF` | libmpg123 | LGPL |
| `SDLMIXER_MIDI=OFF` | FluidSynth, and the bundled Timidity with it | FluidSynth is LGPL, and Timidity's own licence could not be established |
| `SDLMIXER_GME=OFF` | game-music-emu | copyleft |

`SDLMIXER_VENDORED=OFF` keeps SDL_mixer from fetching sources of its own.

Two more are off for a reason that has nothing to do with licences.
`SDLMIXER_FLAC_DRFLAC` and `SDLMIXER_VORBIS_STB` are the small decoders
bundled inside SDL_mixer, and the build uses the format authors' own
libraries instead. Leaving both on would compile two decoders for each of
those formats and use one.

### What is switched on

Six libraries are vendored in `third_party/src` for this, all built before
SDL_mixer, all BSD 3-Clause:

| Library | What it decodes | Licence holder |
|---|---|---|
| libogg | the container Vorbis, FLAC and Opus packets travel in | Xiph.Org Foundation |
| libvorbis | Ogg Vorbis, through its `vorbisfile` library | Xiph.Org Foundation |
| FLAC | FLAC, through `libFLAC` | Josh Coalson and the Xiph.Org Foundation |
| Opus | Opus packets | Xiph.Org, Skype, Octasic, CSIRO, Mozilla, Amazon and others |
| opusfile | a whole `.opus` file | Xiph.Org Foundation |
| WavPack | WavPack | David Bryant |

Each is the plain three clause BSD licence: keep the copyright notice in
source and binary redistributions, and do not use the authors' names to
endorse anything. Full text in each archive's `COPYING`.

Asked at run time, this is what the binary now carries:

```
WAV VORBIS OPUS FLAC WAVPACK VOC AIFF AU DRMP3 SINEWAVE RAW
```

`DRMP3` is the one bundled decoder left, and it is bundled because the only
other MP3 decoder SDL_mixer knows is libmpg123, which is LGPL.

`tests/test_audio.c` keeps both halves of this by the build rather than by
memory. `the_build_carries_no_copyleft_decoder` fails if `XMP`, `MPG123`,
`FLUIDSYNTH`, `TIMIDITY` or `GME` ever turns up in that list.
`every_format_that_should_decode_does` fails if `DRFLAC` or `STBVORBIS` comes
back, which is what a version bump that quietly reset those two switches
would look like.

### FLAC: one archive, two licences

`flac-1.5.0.tar.xz` is the only vendored source whose contents are not all
under one licence, so this is worth being exact about.

- `src/libFLAC`, the library, is BSD 3-Clause. Its text is `COPYING.Xiph`.
- `src/flac` and `src/metaflac`, the two command line tools, are **GPL**, as
  are the `src/share/utf8` and `src/share/grabbag` helpers they use. Their
  text is `COPYING.GPL`.
- `COPYING.FDL` covers the documentation, and `COPYING.LGPL` covers
  `src/share/getopt`, which only the tools link.

`-DBUILD_PROGRAMS=OFF` in `scripts/build_deps.sh` is what draws that line,
and it was checked rather than assumed: every object file in the built
`libFLAC.a` comes from `src/libFLAC`, and no file in that directory contains
GPL text. `-DBUILD_CXXLIBS=OFF` also drops `libFLAC++`, which is BSD as well
and which nothing here calls.

So the GPL files are present in the repository, in the upstream archive as
published, and none of them is compiled or shipped.

### Opus: patents as well as copyright

Opus is the one component here that says anything about patents. Its
`COPYING` carries the BSD 3-Clause licence and then names three royalty free
patent licences that apply to the codec:

- Xiph.Org Foundation, https://datatracker.ietf.org/ipr/1524/
- Microsoft Corporation, https://datatracker.ietf.org/ipr/1914/
- Broadcom Corporation, https://datatracker.ietf.org/ipr/1526/

Royalty free is the whole point of Opus and the reason it was standardised as
RFC 6716, so this adds no fee and no registration. It is recorded because a
patent grant is a separate thing from a copyright licence, and anyone
reviewing what ships with Schultz should be able to see it named rather than
have to find it.

**Opus is used twice.** SDL_mixer plays a whole `.opus` file through
`opusfile`, and the video widget decodes the Opus packets it finds in a WebM
file by calling `libopus` directly. Both are the same vendored 1.5.2 build and
the same licence and grants apply either way.

### libvpx: the patent grant is the point

libvpx decodes VP8 and VP9. Its `LICENSE` is BSD 3-Clause. Beside it, and
separate from it, sits a `PATENTS` file, and that file is the reason these two
codecs are the ones Schultz can ship.

Every other video codec in common use is covered by a patent pool that charges
for it. VP8 and VP9 are not, because Google granted the patents away, and the
grant below is the instrument that did it. Reproducing the copyright licence
and omitting this file would leave the code lawful and throw away the reason
it was chosen, so it is here in full:

```
Additional IP Rights Grant (Patents)
------------------------------------

"These implementations" means the copyrightable works that implement the WebM
codecs distributed by Google as part of the WebM Project.

Google hereby grants to you a perpetual, worldwide, non-exclusive, no-charge,
royalty-free, irrevocable (except as stated in this section) patent license to
make, have made, use, offer to sell, sell, import, transfer, and otherwise
run, modify and propagate the contents of these implementations of WebM, where
such license applies only to those patent claims, both currently owned by
Google and acquired in the future, licensable by Google that are necessarily
infringed by these implementations of WebM. This grant does not include claims
that would be infringed only as a consequence of further modification of these
implementations. If you or your agent or exclusive licensee institute or order
or agree to the institution of patent litigation or any other patent
enforcement activity against any entity (including a cross-claim or
counterclaim in a lawsuit) alleging that any of these implementations of WebM
or any code incorporated within any of these implementations of WebM
constitute direct or contributory patent infringement, or inducement of
patent infringement, then any patent rights granted to you under this License
for these implementations of WebM shall terminate as of the date such
litigation is filed.
```

Two things worth reading in that text.

**It is irrevocable except on one condition**, stated in the last sentence:
suing somebody over these implementations ends your own grant. That is a
defensive termination clause, it is common, and it costs nothing to anyone who
is not suing Google or a fellow implementer over WebM.

**VP8 has a second layer that VP9 does not.** MPEG LA assembled a pool of
eleven patent holders against VP8 and, in March 2013, licensed all of it to
Google to sublicense royalty free, then discontinued the pool. So VP8's
position rests on a threat that materialised and was extinguished; VP9's rests
on a threat that never materialised at all. That is what made both of them
preferable to AV1, whose equivalent dispute is being argued in court.

**Only the decoders are built.** `scripts/build_deps.sh` configures libvpx
with `--disable-vp8-encoder --disable-vp9-encoder`, so the encoders are not
compiled and not shipped.

### speexdsp has seven copyright holders

speexdsp cleans up microphone sound before it is sent: echo cancellation,
noise suppression, automatic gain and voice detection. Schultz reaches it
through `schultz_voice.h`.

Its licence is the revised BSD, and the notice at the top of `COPYING` names
seven holders rather than one. Reproducing only Xiph's, which is what a
careless reading of "a Xiph library" would produce, would be wrong. All seven,
as upstream writes them:

```
Copyright 2002-2008 	Xiph.org Foundation
Copyright 2002-2008 	Jean-Marc Valin
Copyright 2005-2007	Analog Devices Inc.
Copyright 2005-2008	Commonwealth Scientific and Industrial Research
                        Organisation (CSIRO)
Copyright 1993, 2002, 2006 David Rowe
Copyright 2003 		EpicGames
Copyright 1992-1994	Jutta Degener, Carsten Bormann
```

The conditions are the ordinary three: keep the copyright notice in source and
binary redistributions, and do not use the Xiph.org Foundation's name or its
contributors' names to endorse anything. Full text in
`third_party/speexdsp/COPYING`, vendored beside the source, along with
`AUTHORS`.

**Compiled in rather than linked**, like libunibreak and nestegg, and for one
extra reason: it is the only dependency here that is autotools only, while
every other library builds with cmake. Three files in
`third_party/speexdsp` are not upstream's -- `config.h`, the generated
`speexdsp_config_types.h`, and a README explaining both.
`scripts/check_vendored.sh` names those three and checks everything else byte
for byte. `third_party/speexdsp/README.md` says why each one is what it is.

**From the release tarball**, `downloads.xiph.org/releases/speex/`, the same
host Opus, FLAC and libogg already come from -- not the archive
gitlab.xiph.org generates on demand, which cannot be pinned.

### nestegg reads WebM, and is pinned to a commit

nestegg is the demuxer: it opens a WebM or Matroska file, says what tracks are
in it, and hands back packets with their timestamps. It decodes nothing.

It is ISC, Copyright 2010 Mozilla Foundation, which asks for the copyright
notice and the permission notice to travel with any copy. Both are in
`third_party/nestegg/LICENSE`, which is vendored beside the source.

Like libunibreak it is compiled into the toolkit rather than linked, because
it is one C file with no configuration step. `third_party/nestegg/README.md`
records where it came from.

**It has never made a release.** The repository carries no tags, so it is
pinned to commit `d6ec55624d8187a2699ac890b83460b973331400`. opusfile is the
only other dependency in that position and the next section explains the
reasoning, which applies here unchanged: the archive is vendored with its
SHA-256 recorded, so a commit is as reproducible as a tag.

### opusfile is pinned to a commit, not a release

Every other vendored source is a published release archive. opusfile is the
one exception in the repository, so here is the whole of it.

**What it is.** Opus splits in two. `libopus` decodes Opus packets and knows
nothing about files; `opusfile` opens an `.opus` file, walks the Ogg
container inside it, and feeds those packets to `libopus`. SDL_mixer calls
`opusfile`, so there is no way to support Opus without it.

**Why a commit.** Xiph has published no opusfile release since 0.12 in 2020,
and 0.12 builds with autotools only: there is no `CMakeLists.txt` anywhere in
that tarball. Every other library here builds with CMake, and the Android,
iOS and Windows targets are cross compiled by handing CMake a toolchain file.
Autotools cannot read one. Using 0.12 would have meant a second, parallel
cross compilation path for one library, exercised only on the three platforms
that are hardest to test. Writing a `CMakeLists.txt` of our own was the other
option, and that is a build file we would then maintain forever for a library
we did not write.

CMake support landed upstream after 0.12 and has simply never been released.
So what is vendored is that work, taken from Xiph's own repository:

| | |
|---|---|
| Commit | `6dfd29e7adb87f2e193575fc3fa88cbf1a0b27df` |
| `git describe --tags` | `v0.12-59-g6dfd29e`, meaning 59 commits past the v0.12 tag |
| File | `third_party/src/opusfile-0.12-59-g6dfd29e.tar.gz` |

**Upstream, not a fork.** SDL_mixer hits the same wall and answers it with
`github.com/libsdl-org/opusfile`, branch `v0.13-git-SDL`, which is the SDL
project's fork of a post-0.12 snapshot. This takes the upstream commit
directly instead. Same reason, one less party in the chain.

**What was checked before accepting it.** The worry with a snapshot is that it
drifts from the terms of the release it came after. It has not:

- `COPYING` is byte for byte identical to the 0.12 release's.
- The four files added under `cmake/` are thin wrappers around
  `find_package` and a version parser. No third party code and no licence
  text of their own.
- The C sources barely moved. `internal.c`, `stream.c` and `internal.h` are
  unchanged from 0.12; `info.c` and `opusfile.c` differ by 28 and 76 lines.

**Two things that follow from this and will look odd later.**

GitHub does not promise that a commit archive is byte stable, and has changed
its compression before. So `sh scripts/fetch_sources.sh --verify` may one day
report a mismatch on a fresh download even though the commit has not changed.
Nothing breaks: the archive is in git and no build reaches the network. It
means re-checking the contents by hand before recording a new hash, rather
than trusting the download.

A release tarball carries a `package_version` file that the project generates
at release time, and a commit archive does not. Without it CMake warns and
calls the library version `0.0`. `scripts/build_deps.sh` writes that one line
using the `git describe` string above, which is why the build summary reports
`opusfile 0.12-59-g6dfd29e` rather than a question mark.

**When to undo this.** If Xiph publishes an opusfile release that ships a
`CMakeLists.txt`, this exception should go: point `OPUSFILE_URL` at the
release tarball, drop `OPUSFILE_COMMIT`, and delete the `package_version`
line from `scripts/build_deps.sh`. Nothing else here depends on the pin.

### MIDI is absent on purpose

Switching `SDLMIXER_MIDI` off is what removes the bundled Timidity, which was
the one component here whose licence could not be established. A MIDI file
also needs a patch set on disk to make any sound, so revisiting it would mean
settling the licence and shipping the patches.

### AAC is absent too

No AAC decoder exists with a licence that fits, and the format carries patent
claims that Opus was designed to avoid.
