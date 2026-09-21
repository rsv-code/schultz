/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_widget.h
 * @brief The widget interface and the paint walk.
 *
 * **Part of the host facing interface.** A host binds to schultz_api.h,
 * which includes this header. It is the one that comes with a caveat: the
 * widget vtable below is for widgets written in C inside the toolkit, and a
 * host does not implement it. A host draws its own widget by recording into a
 * canvas instead, which is why this header pulls in the arena and the draw
 * list that the rest of the interface has no need of.
 *
 * A widget is not a new kind of object. It is a node that has been given a
 * function table and a lump of its own state. The node already carries
 * identity, bounds, state flags, dirty bits and accessibility; the pane
 * already says how big it wants to be; the resolved style already says what
 * it looks like. What is added here is what it draws and how it reacts.
 *
 * Dispatch is through function pointers, exactly as panes work, so an
 * application can add a widget type without modifying Schultz.
 */

#ifndef SCHULTZ_WIDGET_H
#define SCHULTZ_WIDGET_H

#include "schultz_arena.h"
#include "schultz_event.h"
#include "schultz_node.h"
#include "schultz_paint.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Everything declared below is part of the host facing interface, so it is
 * marked visible. The toolkit is compiled with -fvisibility=hidden, which
 * hides everything by default: that is what stops a shared library built
 * from it exporting the whole of FreeType, libpng and zlib alongside, where
 * they would meet the copies already loaded by whatever is hosting it.
 *
 * A pragma rather than an attribute on each declaration, because there are
 * some hundreds of them and one pair of lines per header says the same thing.
 */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif


/**
 * @brief One stretch of text with a look of its own.
 *
 * A label is one face in one colour. A span says that some run of bytes in it
 * is not: that a phrase is bold, that a term is set in the code face, that a
 * word carries a background or a line through it.
 *
 * Spans describe the text rather than act on it, so the order they are given
 * in does not matter and a byte covered by no span is drawn the way the
 * widget's style says. A field left at zero means the same thing: take the
 * widget's. That is what makes the common case short, because a span marking
 * one word bold sets `start`, `end` and `bold` and nothing else.
 *
 * Bold and italic are wishes rather than faces. A program cannot name the
 * bold handle for whatever a phrase happens to be set in, so it asks, and the
 * family answers; see schultz_font_at_style. Setting `font` names a face
 * outright, and bold and italic are then resolved within that face's family.
 */
typedef struct {
    uint32_t       start;         /**< First byte. */
    uint32_t       end;           /**< One past the last byte. */
    schultz_handle font;          /**< A face, or NONE for the widget's. */
    float          size;          /**< Pixel size, or zero for the widget's. */
    uint32_t       bold;          /**< Nonzero for the family's bold member. */
    uint32_t       italic;        /**< Nonzero for its slanted member. */
    uint32_t       underline;     /**< Nonzero for a line under the text. */
    uint32_t       strikethrough; /**< Nonzero for a line through it. */
    /** Text colour. An alpha of zero means the widget's own. */
    schultz_color  color;
    /** Colour painted behind the text. An alpha of zero means none. */
    schultz_color  background;
    /**
     * Where this stretch points, or NULL for text that points nowhere. The
     * string is copied when the span is set, so the caller may free it.
     */
    const char    *link;
    /**
     * Nonzero when pressing this stretch is reported to the host.
     *
     * Separate from `link` on purpose. A document being shown rather than
     * used wants its links to look like links and to copy as links without
     * being live, and a stretch that is not a link at all may still want to
     * be pressed: a keyword in a code editor, a name in a message, a
     * footnote marker.
     *
     * A press arrives as an ordinary SCHULTZ_EVENT_CLICK on the widget's
     * node, with `span` and `span_tag` naming this stretch. The toolkit acts
     * on none of it; what a press means is the host's business.
     */
    uint32_t       clickable;
    /**
     * Whatever the host wants handed back when this stretch is pressed.
     *
     * Untouched by the toolkit. It is how a program says which of its own
     * things this stretch is, without keeping a table beside the span list
     * that has to be repaired every time the text is edited.
     */
    uint64_t       tag;
} schultz_span;

/**
 * @brief What a widget must provide to take part in a selection.
 *
 * A selection area covers a subtree and a drag across it selects whatever is
 * underneath. The area decides which nodes are inside the range and how much
 * of each; the node itself still owns its own span, paints it, and hands back
 * its own text. So a widget that already knew how to be selected on its own,
 * as a label does, needs very little to join in.
 *
 * A widget with no selectable vtable takes no part. That is the right default
 * for a control: a button's caption is a label on a machine part rather than
 * something written to be read, and selecting it when a drag passes over
 * looks broken.
 */
typedef struct {
    /**
     * How many bytes of selectable text this node holds.
     *
     * Zero means it is taking no part at the moment, which is how a label
     * that has been told it is not selectable declines without the area
     * needing to know what a label is.
     */
    uint32_t (*length)(schultz_tree *tree, schultz_handle node);

    /**
     * Which byte offset a point lands on, in the node's own coordinates.
     *
     * Used when a drag arrives over this node. An offset past the end is the
     * end, and a point above the first line is the beginning, so a drag that
     * leaves the node still gives a sensible answer.
     */
    uint32_t (*offset_at)(schultz_tree *tree, schultz_handle node,
                          schultz_point local);

    /**
     * Marks the bytes from `low` up to but not including `high` as selected.
     *
     * Equal values mean nothing here is selected, which is what every node
     * outside the range is told. Called whenever the range moves, so it is
     * expected to be cheap and to repaint only when something changed.
     */
    void (*set_range)(schultz_tree *tree, schultz_handle node,
                      uint32_t low, uint32_t high);

    /**
     * What is selected here now, as the pair set_range was last given.
     *
     * The mirror of set_range, and needed because a copy has to ask each
     * node what its share turned out to be rather than working it out a
     * second time and hoping the two agree.
     *
     * Equal values mean nothing is selected here.
     */
    void (*range)(schultz_tree *tree, schultz_handle node,
                  uint32_t *out_low, uint32_t *out_high);

    /**
     * This node's whole text, for the caller to take its share of.
     *
     * NUL terminated, owned by the node, and valid until the node changes.
     * NULL from a picture, which has no text to give.
     */
    const char *(*text)(schultz_tree *tree, schultz_handle node);

    /**
     * Nonzero when this node is a picture rather than text.
     *
     * A picture is in the range or out of it and has no inside, so it counts
     * as one place: a length of one, and an offset of zero or one. It adds
     * nothing to plain text, and is only worth anything to a format that can
     * carry a picture, which is why it costs nothing until one asks.
     *
     * Nothing is needed to hand the picture over. Whoever wants it renders
     * this node, which works for a picture that was loaded and for one that
     * is drawn on the spot.
     */
    int32_t (*is_picture)(schultz_tree *tree, schultz_handle node);
    /**
     * The spans covering this node's text, or NULL when it has none.
     *
     * What lets a copied selection carry its bold and its colours rather than
     * arriving as flat words. Offsets are into the same text `text` returns,
     * so a reader can line the two up without asking the widget anything
     * else.
     *
     * Optional. A widget that leaves it NULL is copied as plain text in the
     * paragraph's own style, which is what every widget did before spans.
     */
    const schultz_span *(*spans)(schultz_tree *tree, schultz_handle node,
                                 uint32_t *out_count);
} schultz_selectable_vtable;

/**
 * @brief What a widget type must provide.
 *
 * Every entry may be NULL. A widget with no `paint` contributes nothing to
 * the frame but still lays out and routes events, which is what a container
 * usually wants.
 */
typedef struct {
    /**
     * Appends this node's drawing to the frame's command list.
     *
     * Called with the node's absolute bounds already computed and any clip
     * already pushed. Coordinates are in window space, so a widget positions
     * itself from schultz_node_absolute_bounds rather than assuming an
     * origin.
     *
     * The arena is per frame scratch and is reset before the next one, so
     * anything allocated from it must not be kept.
     */
    int32_t (*paint)(schultz_tree *tree, schultz_handle node,
                     schultz_draw_list *list, schultz_arena *arena);

    /**
     * Handles an event aimed at this node, before the host sees it.
     *
     * Returning nonzero consumes the event and stops it reaching the host,
     * which is what a text field does with a keystroke. Returning zero lets
     * it through, which is what a button does with its click.
     */
    int32_t (*event)(schultz_tree *tree, schultz_handle node,
                     const schultz_event *event);

    /**
     * Moves this node on by however long has passed.
     *
     * Called from schultz_tree_advance, and only for nodes that asked to be
     * ticked with schultz_node_set_animating. Nothing else in the toolkit
     * knows what time it is, so this is where anything that changes on its
     * own rather than in answer to input does its changing.
     *
     * Return nonzero when something changed and the node needs painting
     * again. A widget that has nothing to do this tick returns zero, and the
     * frame costs nothing.
     */
    int32_t (*tick)(schultz_tree *tree, schultz_handle node,
                    uint64_t now_ms, uint32_t elapsed_ms);

    /** Releases whatever `paint` and `event` were using. */
    void (*destroy)(void *data);

    /**
     * How this widget takes part in a selection that spans widgets, or NULL
     * when it takes none. See schultz_selectable_vtable.
     */
    const schultz_selectable_vtable *selectable;

    /**
     * Which clickable span of this widget's text is at a point, or zero when
     * none is.
     *
     * Asked by the router as a click goes out, so the event that reaches the
     * host can name the stretch that was pressed. One event per click, the
     * same one every other widget produces, with two more fields filled in.
     *
     * Optional. A widget that leaves it NULL reports clicks the way it always
     * did, and a host that does not look at the two fields never notices.
     *
     * @param local     The point, in the node's own coordinates.
     * @param out_index Receives which span, counting from zero.
     * @param out_tag   Receives that span's tag.
     * @return Nonzero when a clickable span is there.
     */
    int32_t (*clickable_span)(schultz_tree *tree, schultz_handle node,
                              schultz_point local, uint32_t *out_index,
                              uint64_t *out_tag);
} schultz_widget_vtable;

/**
 * @brief Gives a node a widget type and its state.
 *
 * The node takes ownership of `data`: it is passed to the vtable's `destroy`
 * when the node is destroyed or when a different widget replaces this one.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   The node to turn into a widget.
 * @param vtable The widget's function table, or NULL to remove one. It must
 *               outlive the node; the built in widgets have static storage.
 * @param data   The widget's state, or NULL. Owned by the node from here.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_widget(schultz_tree *tree, schultz_handle node,
                                const schultz_widget_vtable *vtable,
                                void *data);

/**
 * @brief Returns a node's widget state.
 *
 * @param tree The tree holding the node. NULL yields NULL.
 * @param node The node to query.
 * @return The data given to schultz_node_set_widget, or NULL.
 */
void *schultz_node_widget_data(const schultz_tree *tree,
                               schultz_handle node);

/**
 * @brief Returns a node's widget type.
 *
 * Comparing this against a known vtable is how a caller checks what a node
 * is, which is what the widget accessors do before touching their own data.
 *
 * @param tree The tree holding the node. NULL yields NULL.
 * @param node The node to query.
 * @return The vtable, or NULL when the node is not a widget.
 */
const schultz_widget_vtable *schultz_node_widget(const schultz_tree *tree,
                                                 schultz_handle node);

/**
 * @brief Sets whether a node clips its children to its own bounds.
 *
 * Clipping is a flag rather than a layout constraint, so a child may still be
 * arranged outside its parent and simply not be drawn there. Scroll views
 * need this, and so does anything with rounded corners holding content.
 *
 * **It decides input as well as painting.** A press outside a clipping node
 * reaches nothing inside it, because what is not drawn must not be
 * clickable. That matters most for a scroll view, which keeps its rows where
 * they are and moves them by the scroll offset: a row scrolled off the top
 * still has bounds, and without this it would lie over whatever is drawn
 * above the scroll view and swallow presses meant for it.
 *
 * A node that does not clip keeps the opposite behaviour: a child arranged
 * outside it is drawn there and is reachable there.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  The node to configure.
 * @param clips Nonzero to clip its children.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_clips_children(schultz_tree *tree,
                                        schultz_handle node, int32_t clips);

/**
 * @brief Reports whether a node clips its children.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return 1 when it clips, 0 otherwise.
 */
int32_t schultz_node_clips_children(const schultz_tree *tree,
                                    schultz_handle node);

/**
 * @brief Makes a node take its size from the window.
 *
 * A dialog covers the whole window, and so would any other full screen layer.
 * Such a node is not placed by a parent, so nothing tells it when the window
 * changed size and it keeps whatever size it had when it was shown. The
 * window changes size more often than it looks: a phone being turned, a
 * keyboard arriving, a band appearing across the top of the screen.
 *
 * With this set, layout gives the node the viewport, so it follows the window
 * without anything having to remember to tell it.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  The node to configure.
 * @param fills Nonzero to track the window, zero to be placed normally.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_fills_viewport(schultz_tree *tree,
                                        schultz_handle node, int32_t fills);

/**
 * @brief Reports whether a node takes its size from the window.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return 1 when it tracks the window, 0 otherwise.
 */
int32_t schultz_node_fills_viewport(const schultz_tree *tree,
                                    schultz_handle node);

/**
 * @brief Says that pressing this node leaves keyboard focus where it is.
 *
 * A press ordinarily decides focus: it moves to whatever was pressed if that
 * is focusable, and goes away otherwise, which is how pressing the background
 * ends an edit. A few controls act on whatever is focused and must not
 * disturb it, and a key on the on-screen keyboard is the clearest case: a key
 * that took focus, or cleared it, would end the edit it exists to serve
 * before the letter arrived.
 *
 * **It stops both halves**, and is asked before anything else about a press.
 * A focusable node with this set is still reachable by tab, and pressing it
 * still does not move focus to it: that is what a Bold button over a text
 * area wants, since the text it acts on has to keep the caret.
 *
 * **It covers what is inside it.** A press that lands on a child, or in the
 * gap between two of them, keeps focus as well, because the gap belongs to
 * the node that owns it and missing a key by two pixels must not end an
 * edit. A child that can hold focus itself is the exception and still takes
 * it when pressed, so a field inside a container marked this way behaves
 * normally.
 *
 * Off by default, which is what every ordinary node wants.
 *
 * @param tree  The tree holding the node. Must not be NULL.
 * @param node  The node to change.
 * @param keeps Nonzero to leave focus alone when this node is pressed.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_keeps_focus(schultz_tree *tree, schultz_handle node,
                                     int32_t keeps);

/**
 * @brief Reports whether pressing this node leaves focus alone.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return Nonzero when a press on it does not change focus.
 */
int32_t schultz_node_keeps_focus(const schultz_tree *tree,
                                 schultz_handle node);

/**
 * @brief Says whether the pointer can land on this node.
 *
 * Nodes are hit testable by default. Turning it off makes the node itself
 * invisible to the pointer while leaving its children reachable, which is
 * what decoration inside a control wants: a button's caption is part of the
 * button, so a click on the words must reach the button and not the text.
 *
 * @param tree     The tree holding the node. Must not be NULL.
 * @param node     The node to change.
 * @param testable Nonzero to let the pointer land on it.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_hit_testable(schultz_tree *tree, schultz_handle node,
                                      int32_t testable);

/**
 * @brief Reports whether the pointer can land on this node.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return 1 when the pointer can land on it, 0 otherwise.
 */
int32_t schultz_node_hit_testable(const schultz_tree *tree,
                                  schultz_handle node);

/**
 * @brief Says how far a node's painting reaches outside its own bounds.
 *
 * Zero by default, which is the assumption the dirty rectangle rests on:
 * repainting a node's bounds repaints everything it drew. A widget that
 * breaks that assumption has to say by how much, or the part outside its
 * bounds is never invalidated and survives on screen until something else
 * happens to cover it. A focus ring drawn around a control is the case this
 * exists for.
 *
 * The margin widens what invalidation marks and what the paint walk considers
 * when culling. It does not widen the node's bounds, so it changes neither
 * layout nor hit testing.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   The node to change.
 * @param margin How far the painting reaches past every edge, in logical
 *               units. Must not be negative.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a negative margin, or
 *         SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_paint_margin(schultz_tree *tree, schultz_handle node,
                                      float margin);

/**
 * @brief Returns how far a node's painting reaches outside its bounds.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return The margin, or 0 when the node is not live.
 */
float schultz_node_paint_margin(const schultz_tree *tree,
                                schultz_handle node);


/**
 * @brief Shifts where a node's children are drawn and hit tested.
 *
 * A positive offset moves the children up and to the left, which is what
 * scrolling down and right looks like. The children's bounds do not change,
 * so scrolling costs a repaint and never a relayout, and it costs nothing to
 * scroll a subtree of any size.
 *
 * Painting, hit testing and invalidation all ask one function where a node
 * is, so they cannot disagree about where a scrolled child ended up.
 *
 * @param tree   The tree holding the node. Must not be NULL.
 * @param node   The node whose children move.
 * @param offset How far the children are shifted, in pixels.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_HANDLE.
 */
int32_t schultz_node_set_scroll_offset(schultz_tree *tree,
                                       schultz_handle node,
                                       schultz_point offset);

/**
 * @brief Returns how far a node's children are shifted.
 *
 * @param tree The tree holding the node. NULL yields the origin.
 * @param node The node to query.
 * @return The offset, or the origin when the node is not live.
 */
schultz_point schultz_node_scroll_offset(const schultz_tree *tree,
                                         schultz_handle node);

/**
 * @brief Asks for a node to be ticked, or stops asking.
 *
 * A ticking node is visited by schultz_tree_advance and its widget's `tick`
 * is called. Nothing ticks by default, so a tree with nothing animating pays
 * nothing for the clock.
 *
 * @param tree      The tree holding the node. Must not be NULL.
 * @param node      The node to tick, whose widget must have a `tick`.
 * @param animating Nonzero to be ticked, zero to stop.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_HANDLE, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_node_set_animating(schultz_tree *tree, schultz_handle node,
                                   int32_t animating);

/**
 * @brief Reports whether a node is being ticked.
 *
 * @param tree The tree holding the node. NULL yields 0.
 * @param node The node to query.
 * @return 1 when it is ticked, 0 otherwise.
 */
int32_t schultz_node_animating(const schultz_tree *tree, schultz_handle node);

/**
 * @brief Moves every animating node on to now.
 *
 * The one clock the toolkit has. A host calls it once a frame, before
 * resolving styles and painting, and passes the time from whatever clock it
 * already keeps. The toolkit owns no thread and no timer, because it runs
 * inside somebody else's loop, so this is where anything that changes on its
 * own gets its chance to change.
 *
 * A node that changes marks itself, so a small animation in a corner repaints
 * a corner rather than the window.
 *
 * The first call sets the starting point and reports nothing changed, since
 * no time has passed yet.
 *
 * @param tree   The tree to advance. Must not be NULL.
 * @param now_ms Milliseconds from any fixed point, so long as it only rises.
 * @return How many nodes changed, or 0 when nothing did. A host that wants to
 *         skip a frame entirely can check this along with the dirty region.
 */
uint32_t schultz_tree_advance(schultz_tree *tree, uint64_t now_ms);

/**
 * @brief Tells the tree which font system its text widgets shape through.
 *
 * A widget's `paint` is handed a list and an arena but no font system, so the
 * tree holds it, the same way it holds the theme. Without it a text widget
 * measures to nothing and draws nothing rather than guessing.
 *
 * @param tree   The tree to configure. Must not be NULL.
 * @param fonts The font system, or NULL to clear it. Not owned, and must
 *              outlive the tree.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when tree is NULL.
 */
int32_t schultz_tree_set_font_system(schultz_tree *tree, void *fonts);

/**
 * @brief Tells the tree where its widgets' images live.
 *
 * A widget's `paint` is handed a list and an arena but no image table, so the
 * tree holds it, the same way it holds the font system. Without one an image
 * widget measures to nothing and draws nothing rather than guessing.
 *
 * @param tree   The tree to configure. Must not be NULL.
 * @param images The image table, or NULL to clear it. Not owned, and must
 *               outlive the tree.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when tree is NULL.
 */
int32_t schultz_tree_set_image_table(schultz_tree *tree, void *images);

/**
 * @brief Tells the tree where its widgets' rasterized glyphs live.
 *
 * The third of the four things drawing needs, beside the font system and the
 * image table. The tree holds it for the same reason it holds those: nothing
 * else is handed to a widget, and anything that has to draw the tree on its
 * own -- copying a picture out of a selection, for one -- can then assemble
 * what a render wants without the host passing it in again.
 *
 * @param tree   The tree to configure. Must not be NULL.
 * @param glyphs The glyph cache, or NULL to clear it. Not owned, and must
 *               outlive the tree.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when tree is NULL.
 */
int32_t schultz_tree_set_glyph_cache(schultz_tree *tree, void *glyphs);

/**
 * @brief Returns the glyph cache the tree draws through.
 *
 * @param tree The tree to ask. NULL yields NULL.
 * @return The glyph cache, or NULL when none is set.
 */
void *schultz_tree_glyph_cache(const schultz_tree *tree);

/**
 * @brief Tells the tree where its gradients and dash patterns live.
 *
 * The fourth. A widget reaches these through the tree the same way it reaches
 * a font or an image.
 *
 * @param tree      The tree to configure. Must not be NULL.
 * @param resources The resource table, or NULL to clear it. Not owned, and
 *                  must outlive the tree.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when tree is NULL.
 */
int32_t schultz_tree_set_resources(schultz_tree *tree, void *resources);

/**
 * @brief Returns the resource table the tree draws through.
 *
 * @param tree The tree to ask. NULL yields NULL.
 * @return The resource table, or NULL when none is set.
 */
void *schultz_tree_resources(const schultz_tree *tree);

/**
 * @brief Returns the image table the tree draws through.
 *
 * @param tree The tree to query. NULL yields NULL.
 * @return The image table, or NULL when none is set.
 */
void *schultz_tree_image_table(const schultz_tree *tree);

/**
 * @brief Tells the tree which sound system its widgets play through.
 *
 * The same arrangement as the font system and the image table, for the same
 * reason: a widget's `tick` is handed neither. A video node without one shows
 * its pictures and stays silent.
 *
 * @param tree  The tree to configure. Must not be NULL.
 * @param audio The sound system from schultz_audio_create, or NULL to clear
 *              it. Not owned, and must outlive the tree.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when tree is NULL.
 */
int32_t schultz_tree_set_audio(schultz_tree *tree, void *audio);

/**
 * @brief Returns the sound system the tree plays through.
 *
 * @param tree The tree to query. NULL yields NULL.
 * @return The sound system, or NULL when none is set.
 */
void *schultz_tree_audio(const schultz_tree *tree);

/**
 * @brief The format for plain text, which is what a clipboard nearly always
 *        holds.
 *
 * Formats are named the way the platforms name them, as media types. Every
 * clipboard underneath this one negotiates: the side copying says what it can
 * produce, the side pasting picks what it understands. So the same copy can
 * be plain text in a text box and something richer in an editor, and nobody
 * had to choose a mode.
 */
#define SCHULTZ_CLIPBOARD_TEXT "text/plain;charset=utf-8"

/**
 * @brief The format for text that keeps its formatting.
 *
 * What a word processor, a mail client or a browser takes when it is offered
 * something richer than plain text, and what carries pictures along with the
 * words rather than beside them.
 */
#define SCHULTZ_CLIPBOARD_HTML "text/html"

/**
 * @brief Produces the bytes of one format, when something asks for it.
 *
 * Called later than the copy, and only for a format somebody actually wants.
 * That is the point of it: offering an expensive format costs nothing until
 * it is asked for, and it is often never asked for.
 *
 * @param context    The pointer given to schultz_tree_clipboard_offer.
 * @param format     Which of the offered formats is wanted.
 * @param out_length Receives how many bytes. Must not be NULL.
 * @return The bytes, which must stay valid until the next clipboard call, or
 *         NULL when this format cannot be produced after all.
 */
typedef const void *(*schultz_clipboard_make_fn)(void *context,
                                                 const char *format,
                                                 uint64_t *out_length);

/**
 * @brief Offers a set of formats to the system clipboard.
 *
 * @param context     The pointer given when the clipboard was installed.
 * @param formats     The media types on offer, most preferred first.
 * @param count       How many. Never zero.
 * @param make        Produces the bytes of whichever one is asked for.
 * @param make_context Passed to make, unchanged.
 * @return SCHULTZ_OK, or an error the platform reported.
 */
typedef int32_t (*schultz_clipboard_offer_fn)(void *context,
                                              const char *const *formats,
                                              uint32_t count,
                                              schultz_clipboard_make_fn make,
                                              void *make_context);

/**
 * @brief Reads one format off the system clipboard.
 *
 * @param context    The pointer given when the clipboard was installed.
 * @param format     The media type wanted.
 * @param out_length Receives how many bytes. Must not be NULL.
 * @return The bytes, or NULL when the clipboard holds nothing in that format.
 *         They belong to the callback and stay valid until the next call to
 *         any clipboard function.
 */
typedef const void *(*schultz_clipboard_take_fn)(void *context,
                                                 const char *format,
                                                 uint64_t *out_length);

/**
 * @brief Whether the system clipboard holds a format.
 *
 * Asked before taking, so that a caller can pick the best of several without
 * paying to fetch the ones it will not use.
 *
 * @param context The pointer given when the clipboard was installed.
 * @param format  The media type to ask about.
 * @return Nonzero when it is there.
 */
typedef int32_t (*schultz_clipboard_holds_fn)(void *context,
                                              const char *format);

/**
 * @brief Gives the tree a way to reach the system clipboard.
 *
 * A text field without cut, copy and paste is not usable, and nothing inside
 * the toolkit knows what a platform clipboard is, so these three reach one.
 * Without them the editing keys still work on the selection and only the
 * clipboard ones do nothing.
 *
 * **Most hosts never call this.** A window installs its own the moment it is
 * created: SDL's on every platform, and a second set on Windows, where rich
 * text travels in a registered format SDL has no code for. This is for a host
 * driving a tree without one of our windows, or one replacing the platform
 * clipboard with something of its own. Because it takes three function
 * pointers, it is also the one call here a bound language should leave alone.
 *
 * @param tree    The tree to configure. Must not be NULL.
 * @param offer   Puts formats on the clipboard, or NULL to remove it.
 * @param take    Reads one back, or NULL to remove it.
 * @param holds   Says whether a format is there. May be NULL, in which case
 *                asking is answered by taking and throwing the result away.
 * @param context Passed to all three, unchanged.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when tree is NULL.
 */
int32_t schultz_tree_set_clipboard(schultz_tree *tree,
                                   schultz_clipboard_offer_fn offer,
                                   schultz_clipboard_take_fn take,
                                   schultz_clipboard_holds_fn holds,
                                   void *context);

/**
 * @brief Offers a set of formats through whatever the host installed.
 *
 * The general form, for a host putting its own data on the clipboard in more
 * than one format: a spreadsheet offering a range as a table and as tab
 * separated text, say, where the formats come from the host's own model and
 * nothing in the tree knows about them.
 *
 * **Two narrower calls cover what a toolkit host usually wants, and neither
 * takes a callback.** schultz_tree_clipboard_write puts a string on the
 * clipboard. schultz_selection_area_copy puts a selection there as markup,
 * plain text and, where the selection holds one, a picture. Both supply a
 * producer of their own, so a bound language reaches every shipped case with
 * downcalls alone.
 *
 * The callback exists because the bytes are wanted later than this call: a
 * paste may come minutes afterwards or never, and a format nobody asks for
 * should cost nothing. That is also why it cannot be a queue the host drains
 * at its leisure. When the request arrives the platform is inside a paste and
 * waiting, and an answer that comes next frame arrives after that paste has
 * already returned empty.
 *
 * @param tree         The tree to offer through. Must not be NULL.
 * @param formats      The media types, most preferred first. Must not be NULL.
 * @param count        How many, from 1 to SCHULTZ_CLIPBOARD_MAX.
 * @param make         Produces the bytes of one. Must not be NULL.
 * @param make_context Passed to make, unchanged.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad argument, too
 *         many formats, or no clipboard installed, or SCHULTZ_ERR_UNAVAILABLE
 *         when the platform could carry none of the formats offered.
 */
int32_t schultz_tree_clipboard_offer(schultz_tree *tree,
                                     const char *const *formats,
                                     uint32_t count,
                                     schultz_clipboard_make_fn make,
                                     void *make_context);

/**
 * @brief Takes one format off the clipboard.
 *
 * @param tree       The tree to read through. NULL yields NULL.
 * @param format     The media type wanted. NULL yields NULL.
 * @param out_length Receives how many bytes. Must not be NULL.
 * @return The bytes, or NULL when the clipboard holds nothing in that format.
 *         Owned by the host's clipboard and valid until the next clipboard
 *         call.
 */
const void *schultz_tree_clipboard_take(const schultz_tree *tree,
                                        const char *format,
                                        uint64_t *out_length);

/**
 * @brief Whether the clipboard holds a format.
 *
 * @param tree   The tree to ask through. NULL yields 0.
 * @param format The media type to ask about. NULL yields 0.
 * @return Nonzero when it is there.
 */
int32_t schultz_tree_clipboard_holds(const schultz_tree *tree,
                                     const char *format);

/**
 * @brief Reads the clipboard as text.
 *
 * The short way to say schultz_tree_clipboard_take with
 * SCHULTZ_CLIPBOARD_TEXT, for the case that is nearly all of them.
 *
 * @param tree The tree to read through. NULL yields NULL.
 * @return The text, NUL terminated, or NULL when there is none or no
 *         clipboard is installed.
 */
const char *schultz_tree_clipboard_read(const schultz_tree *tree);

/**
 * @brief Puts text on the clipboard, and nothing else.
 *
 * The short way to offer one format. A copy that has something richer to give
 * uses schultz_tree_clipboard_offer instead.
 *
 * @param tree The tree to write through. Must not be NULL.
 * @param utf8 The text, NUL terminated. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when there is no
 *         clipboard installed.
 */
int32_t schultz_tree_clipboard_write(schultz_tree *tree, const char *utf8);

/** @brief The largest number of formats one clipboard write may carry. */
enum {
    SCHULTZ_CLIPBOARD_MAX = 6
};

/** @brief One format the clipboard is being given, and the bytes for it. */
typedef struct {
    const char *format; /**< The media type, such as "image/png". */
    const void *bytes;  /**< The bytes. Copied, so a local is fine. */
    uint64_t    length; /**< How many. Must not be zero. */
} schultz_clipboard_entry;

/**
 * @brief Puts bytes the caller already has on the clipboard.
 *
 * For content the caller produced rather than the tree: a picture of a widget
 * from schultz_render_encode, a table built from the host's own rows, an
 * export in a format only the host knows how to write.
 *
 * **Copying an image is what this is usually for.** Render the node, then
 * hand the bytes over:
 *
 *     const void *png; uint64_t length;
 *     schultz_clipboard_entry one;
 *     schultz_render_encode(tree, node, 1.0f, &options, SCHULTZ_IMAGE_PNG,
 *                           &png, &length);
 *     one.format = "image/png"; one.bytes = png; one.length = length;
 *     schultz_tree_clipboard_write_bytes(tree, &one, 1u);
 *
 * Every entry goes up at once, most preferred first, and the program pasting
 * picks. Calling this twice does not add a second format: each call replaces
 * what was there, because that is what putting something on a clipboard
 * means.
 *
 * The bytes are copied and held until the next clipboard write, so the
 * caller's may be a local and may be freed straight away. That is the
 * difference between this and schultz_tree_clipboard_offer, which produces
 * bytes only if something asks: here the cost is paid now. For a copy the
 * person explicitly asked for, that is the right trade, and it needs no
 * callback, so a host across a language boundary can use it.
 *
 * **Not every platform carries every format.** Windows puts up markup, plain
 * text and image/png, and ignores anything else; the others offer whatever
 * they are given. So a format outside those three is worth treating as a
 * convenience rather than something to rely on, and worth pairing with one
 * of the three so that a copy still reaches a Windows program.
 *
 * Offering none that the platform can carry is refused with
 * SCHULTZ_ERR_UNAVAILABLE rather than passing quietly: the clipboard is
 * cleared before the new formats go on, so a write that put nothing back
 * would otherwise leave the person having pressed copy and lost what they
 * had, with nothing to say so. A mixture is not refused. Offering markup and
 * a format Windows does not know puts the markup up and returns
 * SCHULTZ_OK.
 *
 * @param tree    The tree to write through. Must not be NULL.
 * @param entries The formats and their bytes. Must not be NULL.
 * @param count   How many, from 1 to SCHULTZ_CLIPBOARD_MAX.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a bad argument or
 *         when there is no clipboard installed, or SCHULTZ_ERR_OUT_OF_MEMORY.
 */
int32_t schultz_tree_clipboard_write_bytes(
    schultz_tree *tree, const schultz_clipboard_entry *entries,
    uint32_t count);

/**
 * @brief Names the node whose text is currently selected.
 *
 * Copy is aimed at this node rather than at whatever holds keyboard focus, so
 * a block of prose can be selected and copied without becoming a tab stop.
 * That is how a browser behaves, and making every paragraph focusable instead
 * would put explanatory text in the middle of a form's tab order.
 *
 * Setting this does not clear the selection the previous owner was holding.
 * The widget taking ownership does that, because only it knows what a
 * selection is made of.
 *
 * The tree forgets the owner when that node is destroyed, so the handle is
 * never left dangling.
 *
 * @param tree The tree to change. Must not be NULL.
 * @param node The new owner, or SCHULTZ_HANDLE_NONE for none.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT when tree is NULL.
 */
int32_t schultz_tree_set_selection_owner(schultz_tree *tree,
                                         schultz_handle node);

/**
 * @brief Returns the node whose text is currently selected.
 *
 * @param tree The tree to query. NULL yields SCHULTZ_HANDLE_NONE.
 * @return The owner, or SCHULTZ_HANDLE_NONE when nothing is selected.
 */
schultz_handle schultz_tree_selection_owner(const schultz_tree *tree);

/**
 * @brief The part of the window content may occupy.
 *
 * The root's own bounds. A window layer gives the root whatever is left once
 * a camera notch and a home indicator are taken off, and everything below the
 * root is arranged inside it, so the root's box is the safe area by
 * construction. A root that has not been given bounds yet answers with the
 * whole window, which is what the toolkit did before there was a safe area.
 *
 * Anything that places itself against the screen wants this rather than the
 * viewport. On a desktop the two are the same rectangle; on a phone the
 * difference is a menu that opens under the notch or a toast below the bottom
 * of the glass.
 *
 * @param tree     The tree to ask. Must not be NULL.
 * @param out_area Receives the area, in window coordinates. Must not be NULL.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_tree_safe_area(const schultz_tree *tree,
                               schultz_rect *out_area);

/**
 * @brief Records where the pointer was last seen.
 *
 * The event router calls this as it routes a move. A widget that has to
 * appear near the pointer reads it back with schultz_tree_pointer; it is kept
 * on the tree because a widget is ticked with the tree and has no way to
 * reach the router.
 *
 * @param tree  The tree to record on. Must not be NULL.
 * @param point Where the pointer is, in the tree's own coordinates.
 * @return SCHULTZ_OK, or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_tree_set_pointer(schultz_tree *tree, schultz_point point);

/**
 * @brief Where the pointer was last seen.
 *
 * A touch screen may never have seen one, and neither has a program that has
 * only just started, so this can fail and a caller has to have an answer for
 * that: a tooltip falls back to the middle of what it explains.
 *
 * @param tree      The tree to ask. Must not be NULL.
 * @param out_point Receives the position. Must not be NULL.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer, or
 *         SCHULTZ_ERR_EXHAUSTED when no pointer has ever been seen.
 */
int32_t schultz_tree_pointer(const schultz_tree *tree,
                             schultz_point *out_point);

/**
 * @brief Returns the font system the tree shapes through.
 *
 * @param tree The tree to query. NULL yields NULL.
 * @return The font system, or NULL when none is set.
 */
void *schultz_tree_font_system(const schultz_tree *tree);

/**
 * @brief Walks the tree and emits every widget's drawing.
 *
 * Depth first, parents before children, so a child paints over its parent.
 * Three tests decide whether a node is visited: it is skipped along with its
 * subtree when it is not visible, and when its bounds do not touch the dirty
 * rectangle. A node that clips pushes a clip that is popped on the way out.
 *
 * Culling against the dirty rectangle is what keeps a one pixel change from
 * rebuilding the whole command list.
 *
 * @param tree  The tree to paint. Must not be NULL.
 * @param list  The command list to append to. Must not be NULL.
 * @param arena Per frame scratch for widgets that need it. Must not be NULL.
 * @param dirty The region being repainted, in window coordinates. An empty
 *              rectangle means paint everything, which is what the first
 *              frame and a full repaint want.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer, or a
 *         widget's own error, which stops the walk.
 */
int32_t schultz_widget_paint_tree(schultz_tree *tree, schultz_draw_list *list,
                                  schultz_arena *arena, schultz_rect dirty);

/**
 * @brief Paints a tree into a list, culled against several areas at once.
 *
 * What a frame driver uses. A frame usually has a few separate things that
 * changed rather than one, and the rectangle covering all of them can be most
 * of the window while what actually changed is a fraction of it. Naming each
 * area separately is what keeps a small change small; docs/repainting.md has
 * the measurements.
 *
 * One walk whatever the count, because the walk costs the same whether it is
 * testing one rectangle or eight, and doing it once per area would throw away
 * most of what this is for.
 *
 * @param tree  The tree to paint. Must not be NULL.
 * @param list  The list to append to. Must not be NULL.
 * @param arena Per frame scratch. Must not be NULL.
 * @param dirty The areas that changed, or NULL for all of it.
 * @param parts How many, or zero for all of it. Above
 *              SCHULTZ_TREE_DIRTY_PARTS the rest are ignored.
 * @return SCHULTZ_OK or SCHULTZ_ERR_INVALID_ARGUMENT.
 */
int32_t schultz_widget_paint_tree_parts(schultz_tree *tree,
                                        schultz_draw_list *list,
                                        schultz_arena *arena,
                                        const schultz_rect *dirty,
                                        uint32_t parts);

/**
 * @brief Walks one subtree and emits its widgets' drawing.
 *
 * The same walk, started somewhere other than the root, which is what
 * rendering one widget on its own needs.
 *
 * @param tree  The tree to walk. Must not be NULL.
 * @param node  The node to start at. Its own painting is included.
 * @param list  Where the commands go. Must not be NULL.
 * @param arena Per frame scratch. Must not be NULL.
 * @param dirty The region being repainted. Empty means paint everything.
 * @return SCHULTZ_OK, SCHULTZ_ERR_INVALID_ARGUMENT for a NULL pointer, or a
 *         widget's own error, which stops the walk.
 */
int32_t schultz_widget_paint_subtree(schultz_tree *tree, schultz_handle node,
                                     schultz_draw_list *list,
                                     schultz_arena *arena,
                                     schultz_rect dirty);


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCHULTZ_WIDGET_H */
