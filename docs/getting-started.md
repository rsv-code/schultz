# Getting started

This gets you from a built checkout to an application of your own that opens a
window. If you have not built it yet, start with [Building](building.md).

## Build it first

Building Schultz means building six third party libraries first, and doing it
for whichever target you are working on. That has a page of its
own: **[Building](building.md)**.

On Linux the short version is:

```
sh scripts/build_deps.sh
make
make test
```

Everything below assumes that worked.

## Run the demo

```
./build/schultz_demo
```

The demo is the fastest way to see what the toolkit can do. It has nine pages,
and every widget that ships appears on one of them.

| Option | What it does |
|---|---|
| `--windowed` | Open at 1000 by 700 instead of maximized |
| `--light` | Start in the light theme |
| `--page N` | Open on page N, counting from zero |
| `--frames N` | Draw N frames and exit, which is what a screenshot wants |
| `--screenshot FILE` | Write the window to a PPM file |
| `--shot-scale N` | Render the screenshot at N times the window size |
| `--debug-dirty` | Tint the region being repainted, so you can watch it work |
| `--no-vsync` | Draw as fast as the machine allows, for measuring |

`--debug-dirty` is worth a minute of your time. Move the mouse across a button
and you will see that almost nothing on the screen is being repainted.

## Your first application

Here is a whole program. It opens a window, shows a caption and a button, and
counts clicks.

```c
#include <stdio.h>
#include "schultz_api.h"

static int32_t on_event(void *context, const schultz_event *event)
{
    uint32_t *clicks = (uint32_t *)context;

    if (event->type == SCHULTZ_EVENT_CLICK && event->token == 1u) {
        (*clicks)++;
        printf("clicked %u times\n", *clicks);
    }
    return SCHULTZ_OK;
}

int main(void)
{
    schultz_window_options options;
    schultz_window *window = NULL;
    schultz_tree *tree;
    schultz_theme theme;
    schultz_handle column = SCHULTZ_HANDLE_NONE;
    schultz_handle label = SCHULTZ_HANDLE_NONE;
    schultz_handle button = SCHULTZ_HANDLE_NONE;
    uint32_t clicks = 0;
    int32_t result;

    schultz_window_options_init(&options);
    options.title  = "Hello";
    options.width  = 360;
    options.height = 200;

    result = schultz_window_create(&options, &window);
    if (result != SCHULTZ_OK) {
        fprintf(stderr, "%s\n", schultz_result_string(result));
        return 1;
    }
    tree = schultz_window_tree(window);

    /* Nothing is said about fonts: the toolkit has three built in, and any
     * slot a theme leaves empty falls back to one of them. */
    schultz_theme_preset_light(&theme);
    schultz_tree_set_theme(tree, &theme);

    /* A column holding a caption and a button. */
    schultz_panel_create(tree, schultz_tree_root(tree), &column);
    schultz_node_set_pane(tree, column, schultz_pane_vbox());
    schultz_node_set_spacing(tree, column, 16.0f, 8.0f);
    schultz_node_set_style_property(tree, column, SCHULTZ_PROP_BACKGROUND,
        schultz_value_token(SCHULTZ_TOKEN_COLOR_WINDOW));

    schultz_label_create(tree, column, "Hello from Schultz.", &label);
    schultz_button_create(tree, column, "Press me", &button);
    schultz_node_set_token(tree, button, 1u);

    schultz_events_set_callback(schultz_window_events(window), on_event,
                                &clicks);

    result = schultz_window_run(window, NULL, NULL, 0u);
    schultz_window_destroy(window);
    return (result == SCHULTZ_OK) ? 0 : 1;
}
```

Save it as `hello.c` in the Schultz directory and build it:

```
make lib
g++ -o hello hello.c -I. build/libschultz_all.a -lstdc++ -lm
./hello
```

That is the whole link line. `libschultz_all.a` is the toolkit with every
dependency merged into it, so SDL, ThorVG, FreeType, HarfBuzz and the rest
are already inside and none of them is named here. `-lstdc++` is there
because ThorVG is C++, and `-lm` because the maths library is the system's.

`make lib` also produces `build/libschultz.a`, which is the toolkit on its
own. Link that one when you would rather supply the dependencies yourself,
and ask `pkg-config` for them with the prefix the dependency build wrote:

```
. build-deps/linux-x86_64/env.sh
```

### What that program is doing

**The window driver does the loop.** `schultz_window_create` builds the window,
the rasterizer, the font system and the tree in one call.
`schultz_window_run` then runs frames until the window closes. Without it you
would be writing eight calls per window in a fixed order.

**A theme needs a font, and one is already there.** Schultz has five DejaVu
faces built into it: one per theme font slot, and the two oblique Sans faces
that complete that family so bold and italic have somewhere to come from. Any
slot you leave empty falls back to one of them. That is why the program above never mentions fonts
and still draws text. Load your own face and put it in a slot when you want
your own look; `schultz_font_load_memory` takes bytes rather than a path, for
a face that lives in a resource bundle or an Android asset.

**Emoji draw in colour without being asked for.** A sixth face is built in
for them, because no text face carries emoji and looking for one on the
machine gives different artwork on every platform and none at all on some.
Anything that draws text draws emoji: put one in a label or type one into a
field and it appears, the right size for the text around it.

**A token belongs to you, not to the toolkit.** `schultz_node_set_token`
stamps a node with a number of your choosing, and every event about that node
carries it back. That is how you tell which button was pressed without holding
a pointer to it.

**Events can be pulled instead of pushed.** The callback above is the simple
way. If you would rather not be called at all, turn on
`schultz_events_set_queue` and read the same events once a window with
`schultz_events_drain`. A language binding usually wants that: one crossing
per window instead of one per click.

## Next

[Concepts](concepts.md) explains what the tree is and what happens in a window.
