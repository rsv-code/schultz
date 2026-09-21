# Repainting

How Schultz decides what to redraw each frame, why it is a few rectangles
rather than one, and what to look at when something on screen goes stale.

## The problem a partial repaint solves

A window is mostly still. A clock ticks in the corner, a button lights up
under the pointer, a caret blinks. Redrawing the whole window for any of that
is work proportional to the window rather than to the change, and on a phone
it is battery spent for nothing: the region redrawn is also the region
uploaded to the GPU every frame.

So Schultz tracks what changed. A widget that changes calls
`schultz_node_invalidate`, which adds its bounds to the tree's list of areas
needing paint. Each frame the driver paints, rasterizes and uploads only
those areas, and everything else keeps the pixels it already had.

That last part is the rule everything else follows from:

> **What is not repainted is not cleared.** The buffer still holds the
> previous frame there. A partial repaint is only correct if every pixel it
> skips was already right.

## Why several areas and not one

The obvious design keeps one rectangle: the union of everything that changed.
It is simpler, it is what Schultz did first, and it has a failure that gets
worse the larger the window is.

Two changes at opposite corners have a union of nearly the whole window. A
busy indicator 16 pixels square in the status bar, plus a small animation near
the top of the page, forces every pixel between them to be repainted and
uploaded, sixty times a second.

Measured on the demo's Drawing page at 1000x700, with a sliding block and a
turning square in view:

| what was running | repainted each frame |
|---|---|
| the block alone | 3,940 px |
| the square alone | 4,825 px |
| both | 25,173 px |
| both, plus the 16x16 corner spinner | **482,995 px — 69% of the window** |

About 4,100 pixels of actual change costing 483,000 pixels of work: seventy
times more than the change requires, and the worst offender was the smallest
thing on screen.

With a few rectangles instead, the same scene repaints **6,745 pixels a frame
across 3 areas — 1% of the window**, and the paint and rasterize phases
together went from 2,641 ms per 600 frames to 436 ms. Six times less work, and
seventy times less area uploaded.

## How the areas are chosen

`schultz_tree_add_dirty` in `schultz_node.c` keeps up to
`SCHULTZ_TREE_DIRTY_PARTS` rectangles. Two rules decide what happens to a new
one.

**Merge when it is nearly free.** A rectangle is not free: measured on this
toolkit, one costs about 0.17 ms of fixed work -- the tree walk, the
rasterizer's own setup, the texture upload call -- plus about five nanoseconds
a pixel. So two areas close together genuinely cost less as one, and anything
under a few thousand wasted pixels is worth merging. `SCHULTZ_TREE_DIRTY_WASTE`
is that line, and "wasted" means the area of the covering rectangle less the
area of the two it covers.

The common case is a node repainting in the same place every frame. Its new
rectangle is already inside the one from last frame, wastes nothing, and
merges for free.

**Never exceed the list.** When all the slots are full and a new rectangle
will not merge cheaply into any of them, the two existing rectangles that
waste least by becoming one are merged to make room. That is what bounds the
worst case: however scattered a frame becomes, this degrades towards the
single rectangle it used to be, and never to anything worse.

Picking the pair that wastes least is the same rule an R-tree uses for
insertion. At eight slots it is 28 comparisons of four floats, which is
nothing beside a rasterize.

## What happens each frame

In `schultz_window_update`:

1. Read the areas out of the tree, into the window. This happens **before**
   the drawing, because the drawing clears the tree's list and the upload
   afterwards still needs to know where to look.
2. If there is nothing, skip the drawing entirely and present. An empty list
   means nothing changed.
3. Paint the tree **once**, culled against all the areas at once, with
   `schultz_widget_paint_tree_parts`. One walk whatever the count: the walk
   costs the same testing one rectangle or eight, and walking per area would
   throw away most of the benefit.
4. Play the draw list into the rasterizer once per area, each clipped to its
   own rectangle.
5. Upload each area to the texture with `schultz_sdl_window_upload`, then
   `schultz_sdl_window_present` once. These are two calls rather than one
   because a frame has several changed areas and only one of itself:
   presenting per area would put the window on screen several times a frame.

## Two things that are easy to get wrong

**An empty rectangle means two different things.** To
`schultz_widget_paint_subtree` an empty dirty rectangle means *paint
everything*, which is right for rendering a whole tree offscreen. To a frame
loop it means *nothing changed*. Confusing the two made an idle window repaint
itself completely every frame, which cost four times what a moving one cost.
The frame driver says the empty case out loud and skips the drawing.

**The rectangle handed to the rasterizer is in pixels, not in layout units.**
On a screen with three pixels to the unit, passing the unscaled rectangle
names an area a third of the size, and two thirds of the window is never
written. `scripts/check_full_paint.sh` exists because of that bug.

## When something goes stale

A stale pixel -- part of the window showing what was there a moment ago -- is
almost always an area that was not marked. Three things to reach for:

- **`./build/schultz_demo --debug-dirty`** tints the areas being repainted, so
  you can see what was marked and what was not.
- **`a_partial_repaint_matches_a_full_one`** in `tests/test_render.c` is the
  invariant as a test: paint a scene, change two things at opposite ends,
  repaint only what the tree says changed, and compare every pixel against
  painting the whole thing from scratch. A merge that lost a pixel shows up
  there and nowhere else.
- **`schultz_node_set_bounds`** is worth reading as an example of getting it
  right. It marks the node's area both before and after the move. Marking only
  the new one leaves the old position painted on screen, which is the classic
  form of this bug.

## What this does not do

The areas may overlap, and overlapping pixels are painted twice. That costs a
little and is never wrong, and removing it would mean splitting rectangles,
which creates more of them -- the thing the fixed cost per rectangle says to
avoid.

There is no attempt to track *what* changed within an area, only that
something did. A one pixel caret in the middle of a paragraph repaints the
caret's rectangle, not the paragraph.
