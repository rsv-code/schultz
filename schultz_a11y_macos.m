/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_a11y_macos.m
 * @brief The macOS accessibility backend, over NSAccessibility.
 *
 * ## Why this file is Objective-C
 *
 * On macOS accessibility is not a protocol or a message. VoiceOver asks a
 * view questions by calling methods on it, and a view that answers none of
 * them is a blank rectangle. So something has to answer those methods, and
 * only Objective-C can.
 *
 * AccessTunnel already provides the answers, in access_tunnel_ax_shell.m.
 * What is missing is the view they should be attached to.
 *
 * ## The view belongs to SDL
 *
 * Schultz does not create a view. SDL does, and it is an SDL3View inside an
 * SDL3Window. Subclassing is not open to us because the view is made before
 * anything here runs.
 *
 * What is open to us is the same technique AccessTunnel already uses on the
 * window: add the methods to the class at run time. class_addMethod puts
 * accessibilityChildren and its neighbours on SDL3View, and each of them
 * finds its adapter through an associated object on the instance it was
 * called on. SDL's view is left otherwise untouched, and a window with no
 * adapter attached behaves exactly as it did.
 *
 * This is why AccessTunnel ships
 * access_tunnel_macos_add_focus_forwarder_to_window_class: SDL puts keyboard
 * focus on the window rather than the view, so the window has to be taught
 * to pass the question down. That helper names the class it changes for the
 * same reason this file does.
 *
 * MEMORY
 *
 * Written for manual retain and release, matching AccessTunnel's shells, so
 * that the build needs no extra compiler flag.
 */

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <objc/runtime.h>

#include <stdlib.h>

#include <SDL3/SDL.h>

#include "access_tunnel_macos.h"
#include "schultz_a11y_backend.h"

#if __has_feature(objc_arc)
#error "schultz_a11y_macos.m is written for manual retain/release; \
compile it without -fobjc-arc."
#endif

/** @brief The adapter and the view whose class was taught to ask it. */
struct schultz_a11y_backend {
    access_tunnel_macos_adapter *adapter; /**< The VoiceOver side. */
    NSView *view;                         /**< SDL's content view. */
};

/*
 * The key for the association from a view to its backend. Only its address
 * matters, which is the documented way to make one of these.
 */
static const char schultz_a11y_macos_key;

static schultz_a11y_backend *backend_for(id view)
{
    NSValue *held = objc_getAssociatedObject(view, &schultz_a11y_macos_key);

    return (held != nil) ? (schultz_a11y_backend *)[held pointerValue] : NULL;
}

/* ------------------------------------- methods added to SDL's view class */

/*
 * Each of these is installed on SDL3View. They must behave as the unmodified
 * view did when no adapter is attached, because the same class serves every
 * window in the process and only some of them are ours.
 */

static BOOL view_is_accessibility_element(id self, SEL command)
{
    (void)command;
    return backend_for(self) != NULL ? NO : YES;
}

static NSArray *view_accessibility_children(id self, SEL command)
{
    schultz_a11y_backend *backend = backend_for(self);

    (void)command;
    if (backend == NULL) {
        return nil;
    }
    return (NSArray *)access_tunnel_macos_adapter_view_children(
        backend->adapter);
}

static id view_accessibility_focused(id self, SEL command)
{
    schultz_a11y_backend *backend = backend_for(self);

    (void)command;
    if (backend == NULL) {
        return nil;
    }
    return (id)access_tunnel_macos_adapter_focus(backend->adapter);
}

static id view_accessibility_hit_test(id self, SEL command, NSPoint point)
{
    schultz_a11y_backend *backend = backend_for(self);

    (void)command;
    if (backend == NULL) {
        return nil;
    }
    return (id)access_tunnel_macos_adapter_hit_test(backend->adapter,
                                                    (double)point.x,
                                                    (double)point.y);
}

/*
 * Adds the four methods to the view's own class, once per process.
 *
 * class_addMethod does nothing when the class already answers the selector,
 * so a second call is harmless. The type strings are Objective-C's own
 * encoding: "c@:" is a char returned from a method taking self and the
 * selector, "@@:" an object, and "@@:{CGPoint=dd}" one that also takes a
 * point.
 */
static void install_methods(Class target)
{
    if (target == Nil) {
        return;
    }
    class_addMethod(target, @selector(isAccessibilityElement),
                    (IMP)view_is_accessibility_element, "c@:");
    class_addMethod(target, @selector(accessibilityChildren),
                    (IMP)view_accessibility_children, "@@:");
    class_addMethod(target, @selector(accessibilityFocusedUIElement),
                    (IMP)view_accessibility_focused, "@@:");
    class_addMethod(target, @selector(accessibilityHitTest:),
                    (IMP)view_accessibility_hit_test, "@@:{CGPoint=dd}");
}

/* --------------------------------------------------------- the backend */

/* SDL's content view, or nil when there is not one to be had. */
static NSView *content_view(void *native_window)
{
    SDL_PropertiesID props;
    NSWindow *window;

    if (native_window == NULL) {
        return nil;
    }
    props = SDL_GetWindowProperties((SDL_Window *)native_window);
    window = (NSWindow *)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    return (window != nil) ? [window contentView] : nil;
}

int32_t schultz_a11y_backend_create(
    const schultz_a11y_backend_config *config,
    const access_tunnel_tree_update *initial,
    schultz_a11y_backend **out_backend)
{
    schultz_a11y_backend *backend;
    NSView *view;

    if (config == NULL || initial == NULL || out_backend == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    view = content_view(config->native_window);
    if (view == nil) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend = (schultz_a11y_backend *)calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    backend->adapter = access_tunnel_macos_adapter_new(
        view, initial, config->focused != 0, config->action,
        config->action_userdata);
    if (backend->adapter == NULL) {
        free(backend);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    backend->view = view;

    install_methods(object_getClass(view));
    /*
     * SDL puts keyboard focus on the window, so VoiceOver asks the window
     * what is focused and the window does not know. This teaches it to ask
     * its content view instead. Named by class because the window, like the
     * view, was made before any of this ran.
     */
    access_tunnel_macos_add_focus_forwarder_to_window_class(
        object_getClassName([view window]));

    /*
     * The association is what every added method uses to find its way back
     * here, so it is made last, once there is something worth finding. Until
     * it exists the added methods behave as the unmodified view did.
     */
    objc_setAssociatedObject(view, &schultz_a11y_macos_key,
                             [NSValue valueWithPointer:backend],
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    *out_backend = backend;
    return SCHULTZ_OK;
}

void schultz_a11y_backend_destroy(schultz_a11y_backend *backend)
{
    if (backend == NULL) {
        return;
    }
    /*
     * Broken first. The methods stay on the class, because a method added to
     * a class cannot be taken off again, but with no association they answer
     * exactly as the unmodified view did.
     */
    if (backend->view != nil) {
        objc_setAssociatedObject(backend->view, &schultz_a11y_macos_key, nil,
                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    access_tunnel_macos_adapter_free(backend->adapter);
    free(backend);
}

int32_t schultz_a11y_backend_update(schultz_a11y_backend *backend,
                                    const access_tunnel_tree_update *update)
{
    if (backend == NULL || update == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    access_tunnel_macos_adapter_update(backend->adapter, update);
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_pump(schultz_a11y_backend *backend)
{
    /* VoiceOver calls in. There is nothing to read. */
    (void)backend;
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_set_window(schultz_a11y_backend *backend,
                                        schultz_rect window)
{
    /* The view knows where it is, and reports its own geometry. */
    (void)backend;
    (void)window;
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_set_focused(schultz_a11y_backend *backend,
                                         int32_t focused)
{
    if (backend != NULL && backend->adapter != NULL) {
        access_tunnel_macos_adapter_update_view_focus_state(backend->adapter,
                                                            focused != 0);
    }
    return SCHULTZ_OK;
}
