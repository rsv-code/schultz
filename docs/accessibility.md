# Accessibility

Schultz describes what it draws so that a screen reader can read it. This is
on by default and needs no code from you.

A toolkit draws pixels. A screen reader cannot see pixels, so something has to
say in words what is on screen: this is a button, it says "Save", it has focus
now. Every operating system has its own API for that and no two agree, so
Schultz describes its tree once and **AccessTunnel** presents that description
to whichever platform it is running on.

AccessTunnel is a C port of AccessKit. It is a separate repository expected
beside this one, and a required dependency; see [Building](building.md).

| Platform | What it speaks |
|---|---|
| Linux | AT-SPI, over D-Bus |
| Windows | UI Automation |
| macOS | NSAccessibility |
| iOS | UIAccessibility |
| Android | AccessibilityNodeProvider |

## You get it for free

`schultz_window_options.accessible` defaults to on. A window opened with the
defaults publishes its tree, answers the platform, and turns what an assistive
technology asks for into real input.

```c
schultz_window_options options;

schultz_window_options_init(&options);
options.title    = "My application";
options.app_name = "My application";   /* NULL uses the title */
schultz_window_create(&options, &window);
```

Starting it costs nothing when nothing is listening, and a reader may connect
at any time afterwards.

## What makes a widget readable

Every widget that ships fills this in already. It matters when you build
something of your own out of nodes.

| What | Set with | Read as |
|---|---|---|
| **Role** | `schultz_node_set_role` | What kind of thing it is |
| **Name** | `schultz_node_set_name` | What it is called |
| **Value** | `schultz_node_set_value` | What it currently says |
| **Actions** | `schultz_node_set_actions` | What can be done to it |
| **State** | `schultz_node_set_state` | Enabled, checked, selected |
| **Bounds** | the layout | Where it is on screen |

Two rules are worth knowing because breaking them is silent:

**A control that can be operated must say so.** Adding
`SCHULTZ_ACTION_CLICK` is what tells a reader the thing can be pressed, and it
is also what makes the press arrive back. A node that responds to a click but
never declared one is a node a screen reader user cannot reach.

**A toggle's state is not part of its name.** Set `SCHULTZ_STATE_CHECKED` and
leave the name alone. Saying "Play sounds, checked" is the reader's job and
its user's preference; baking it into the name takes that choice away and gets
read twice.

## Which way the requests go

The point is that both directions use the same path.

```
your tree  ->  the bridge  ->  AccessTunnel  ->  the platform  ->  a reader
                    ^                                                  |
                    +----------------- what it asked for --------------+
```

An assistive technology asking to press a button arrives as an action request,
is queued, and is carried out on the next turn of the loop through
`schultz_events_perform`, which produces exactly the event a real press
produces. A checkbox toggles because the checkbox's own code ran, not because
the accessibility layer described a toggle that never happened.

That symmetry is the whole idea. An accessible application is not one that
describes itself well; it is one where the description and the behaviour come
from the same place and cannot drift apart.

`schultz_events_perform` handles click, focus, increment and decrement.
Increment and decrement are delivered as a right or left arrow key, because
that is the input a range widget already understands.

Setting a value is the one action the bridge finishes itself, because a value
is specific to the widget receiving it while every other action is not. A
number goes to a slider or a scroll bar and a string goes to a text field,
through each widget's own public setter.

## Threading

A platform accessibility API calls in on threads the toolkit did not create.
Requests are queued at the bridge under a lock and drained on the thread that
runs the loop, so no widget is ever touched from a thread it does not expect,
and the live tree is never walked from a foreign one.

## When the tree is republished

Whenever anything was painted, or focus moved.

That is the generous reading rather than the tight one. A hover that repaints
a button marks the tree dirty without changing a word of what a reader would
say, and that costs a rebuild. The opposite mistake is worse and it is silent:
a reader then describes a window that stopped being true several clicks ago,
while the application looks fine. The cheap side is the one to be wrong on
until this has been measured.

## Testing it

Unit tests check one layer against another. They cannot tell you whether a
screen reader can actually read your window, so there is a test that puts a
real assistive technology client on the other end of a real bus:

```
sh scripts/livetest/run.sh
```

It starts the demo, reads its tree back, prints it, and checks that every
published label carries text. On a machine with no accessibility bus it says
so and exits 77.

This is worth the trouble. It is what caught static text being published in
the wrong property, which made every caption in the demo silent while every
unit test still passed.

## What is verified

| Platform | State |
|---|---|
| Linux, AT-SPI | **Read end to end by a real client.** Roles, names, structure, text and word navigation all confirmed |
| Windows, UIA | Written and compiled. Not yet read by a screen reader |
| macOS, iOS, Android | Written. Not yet compiled or read by a screen reader |

## One backend per platform

Describing a tree is the same everywhere. Handing it to the platform is not.

So `schultz_a11y.c` does the first part and knows nothing about any platform:
it walks the widgets and builds a tree update. The second part is behind the
six functions in `schultz_a11y_backend.h`, and exactly one file implements
them per build, chosen in the Makefile from the target's name.

| Target | Backend | How the platform reaches the tree |
|---|---|---|
| `linux-*` | `schultz_a11y_atspi.c` | Schultz pushes over D-Bus, then pumps |
| `windows-*` | `schultz_a11y_uia.c` | The window answers `WM_GETOBJECT` |
| `macos-*` | `schultz_a11y_macos.m` | VoiceOver calls methods on the `NSView` |
| `ios-*` | `schultz_a11y_ios.m` | UIKit calls methods on the `UIView` |
| `android-*` | `schultz_a11y_android.c` | TalkBack calls into Java |
| anything else | `schultz_a11y_none.c` | It does not |

`schultz_a11y_none.c` exists so that a new target builds and runs before its
accessibility is written. The tree is still walked and still handed over; the
backend drops it. An application on such a target works and announces nothing.

`scripts/check_a11y_backends.sh` checks that each backend defines the whole
interface. Only one of them is compiled on any one machine, so without that a
function missing from the iOS backend would surface as a link error for
whoever next built for iOS, long after the change that caused it.

### Four of them attach to something

Only Linux announces itself. The other four hang off a native window or view,
which SDL owns, so `schultz_a11y_options` carries the `SDL_Window *` and each
backend asks SDL for what it needs:

- **Windows** takes the `HWND` from
  `SDL_PROP_WINDOW_WIN32_HWND_POINTER`.
- **macOS** takes the `NSWindow` from
  `SDL_PROP_WINDOW_COCOA_WINDOW_POINTER` and then its content view.
- **iOS** takes the `UIWindow` from
  `SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER` and then its root controller's view.
- **Android** calls `SDLActivity.getContentView()` through JNI.

### Answering questions on somebody else's view

macOS and iOS are the awkward pair. VoiceOver asks a view questions by calling
methods on it, and a view answering none of them is a blank rectangle. Schultz
cannot subclass SDL's view, because the view exists before any of this runs.

What it can do is add the methods to SDL's view class at run time.
`class_addMethod` puts `accessibilityChildren` and its neighbours on the class,
and each added method finds its adapter through an associated object on the
instance it was called on. A view with no adapter attached answers exactly as
it did before, which matters because the same class serves every window in the
process.

This is not a trick invented here. AccessTunnel already ships
`access_tunnel_macos_add_focus_forwarder_to_window_class`, for the same
reason and by the same means: SDL puts keyboard focus on the window rather
than the view, so the window has to be taught to pass the question down.

The methods cannot be removed afterwards, because Objective-C has no way to
take a method off a class. Detaching clears the association instead, which
leaves them answering as the unmodified view did.

### Windows replaces the window procedure

SDL offers `SDL_SetWindowsMessageHook`, which sees every message, but it
returns a bool meaning carry on or drop. `WM_GETOBJECT` needs an `LRESULT`
back and the hook has no way to supply one, so it cannot answer this message
however early it sees it.

`SetWindowLongPtr` can. It replaces the window procedure and hands back the
old one, which is the documented way to add a message to a window somebody
else created: answer the one message that is ours, call the previous procedure
for everything else. The old procedure goes back on the way out.

### Android needs a Java class in the application

TalkBack does not talk to native code. It asks a `View` for an
`AccessibilityNodeProvider`, and both are Java objects.

AccessTunnel provides both halves: `Delegate.java` is the Java object, and it
decides nothing, passing every method straight through to JNI with the
adapter's address. The backend makes an adapter, finds SDL's `View`, and puts
a `Delegate` on it.

**An Android application has to ship that file.** It is source, not a library,
so add `java/dev/accesstunnel/android/Delegate.java` from the AccessTunnel
repository to the application's own sources, in the package
`dev.accesstunnel.android`. Without it the backend finds no class, announces
nothing, and the application otherwise runs normally. An accessibility failure
must not be a startup failure.

## Text fields

A field is not one string to a screen reader. It is published as a sequence of
**text runs**, one per line, and each run carries the byte length of every
character in it. That is what lets a reader move a caret by character and by
word and know where it ended up.

The lengths cannot be worked out from the text by the accessibility layer. A
character is the smallest unit the editor lets you select, and only the editor
knows what that is; Schultz steps its caret over whole UTF-8 characters, so
that is what is published and the two agree by construction.

Word boundaries come from libunibreak, which is already here for line
breaking. Using it for both means a reader's idea of a word and the editor's
cannot drift apart.

The field itself carries the selection, as an anchor and a focus that each
name a run and an index within it. A caret is a selection whose two ends are
equal.

### A masked field publishes nothing it holds

Anything on the accessibility bus can read what is published, so a password
field puts out one bullet per character rather than its contents, and takes
the password role so a reader announces it as one and stops echoing what is
typed. The length and every caret position stay honest; the characters never
leave.

## Known gaps

- **Only Linux is verified end to end.** `scripts/livetest/run.sh` reads the
  demo's tree over a real accessibility bus. The other four backends compile
  and are built from AccessTunnel's own adapters, but nothing here has yet run
  a screen reader against them.
- **Android needs `Delegate.java` in the application.** See above.
- **Coordinates are not converted for a scaled desktop.** What is published
  is in the toolkit's units. Apple's platforms want exactly that, and so does
  any screen with nothing to scale, so iOS, macOS and every ordinary monitor
  are right. AT-SPI and UI Automation want screen pixels, so a Linux or
  Windows machine that scales would put a reader's highlight at the wrong
  size. The conversion belongs in those two backends.
- **A list row has no name of its own.** A reader reads the label inside it,
  which works, but naming the row would read better.
- **No per character rectangles.** `character_positions` and
  `character_widths` would let a reader highlight one word on screen. That
  needs the shaped glyph positions, which the bridge does not ask for.
- **Runs are hard lines, not wrapped lines.** A soft wrapped line is a
  property of the layout rather than of the text, so "next line" means the
  next real one.
- **Word starts stop after 255 characters in a run**, because the schema holds
  an index in a byte. Longer lines lose word navigation past that point.
