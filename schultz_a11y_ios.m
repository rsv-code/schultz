/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_a11y_ios.m
 * @brief The iOS accessibility backend, over UIAccessibility.
 *
 * The same shape as the macOS backend, and for the same reason: VoiceOver
 * asks a view questions by calling methods on it, SDL owns the view, and the
 * methods are added to SDL's view class at run time. Read
 * schultz_a11y_macos.m first; only the differences are described here.
 *
 * ## iOS is asked, not told
 *
 * A UIKit adapter sits asleep until an assistive technology starts, and then
 * asks for the whole tree at once. That is what the build callback in the
 * backend configuration is for, and iOS is the only platform that uses it.
 * Waking is cheap and safe to repeat, so it is tried on every update rather
 * than subscribed to.
 *
 * ## There is no window focus
 *
 * An iOS application is either frontmost or not running in any sense that
 * matters, so there is no outbound focus to report and set_focused does
 * nothing. VoiceOver's own cursor travels the other way and arrives as an
 * action request, like every other thing a user asks for.
 *
 * MEMORY
 *
 * Manual retain and release, matching AccessTunnel's shells.
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <objc/runtime.h>

#include <stdlib.h>

#include <SDL3/SDL.h>

#include "access_tunnel_ios.h"
#include "schultz_a11y_backend.h"

#if __has_feature(objc_arc)
#error "schultz_a11y_ios.m is written for manual retain/release; \
compile it without -fobjc-arc."
#endif

/** @brief The adapter, the view whose class was taught to ask it, and what the adapter needs to ask back with. */
struct schultz_a11y_backend {
    access_tunnel_ios_view_adapter *adapter; /**< The VoiceOver side. */
    UIView *view;                            /**< SDL's view. */
    /*
     * Copied out of the configuration, which is borrowed for the length of
     * the create call only. The adapter may ask for a tree at any time
     * afterwards, so what it needs to ask with has to live here.
     */
    schultz_a11y_backend_build_fn build; /**< How to ask for a tree. */
    void *build_userdata;                /**< Passed to it. */
    schultz_a11y_backend_action_fn action; /**< Where requests go. */
    void *action_userdata;                 /**< Passed to it. */
};

static const char schultz_a11y_ios_key;

static schultz_a11y_backend *backend_for(id view)
{
    NSValue *held = objc_getAssociatedObject(view, &schultz_a11y_ios_key);

    return (held != nil) ? (schultz_a11y_backend *)[held pointerValue] : NULL;
}

/* ------------------------------------- methods added to SDL's view class */

/*
 * A view is either an element itself or a container of them, never both.
 * Saying NO here is what makes UIKit look at accessibilityElements at all.
 * With no adapter attached both answer as the unmodified view did.
 */
static BOOL view_is_accessibility_element(id self, SEL command)
{
    (void)self;
    (void)command;
    return NO;
}

static NSArray *view_accessibility_elements(id self, SEL command)
{
    schultz_a11y_backend *backend = backend_for(self);

    (void)command;
    if (backend == NULL) {
        return nil;
    }
    return (NSArray *)access_tunnel_ios_view_adapter_elements(
        backend->adapter);
}

static void install_methods(Class target)
{
    if (target == Nil) {
        return;
    }
    class_addMethod(target, @selector(isAccessibilityElement),
                    (IMP)view_is_accessibility_element, "c@:");
    class_addMethod(target, @selector(accessibilityElements),
                    (IMP)view_accessibility_elements, "@@:");
}

/* --------------------------------------------------------- the backend */

/*
 * SDL's view, or nil.
 *
 * SDL hands out its UIWindow rather than the view inside it, so the view has
 * to be reached through the window's root view controller, which is where
 * SDL puts it.
 */
static UIView *sdl_view(void *native_window)
{
    SDL_PropertiesID props;
    UIWindow *window;
    UIViewController *controller;

    if (native_window == NULL) {
        return nil;
    }
    props = SDL_GetWindowProperties((SDL_Window *)native_window);
    window = (UIWindow *)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    if (window == nil) {
        return nil;
    }
    controller = [window rootViewController];
    return (controller != nil) ? [controller view] : nil;
}

/*
 * Asked by UIKit for a whole tree when VoiceOver starts.
 *
 * Called on the main thread, which on iOS is also the thread that runs the
 * frame loop, so this walks the widget tree from the thread that owns it.
 */
/*
 * The adapter carries one userdata for both of its callbacks, and the two
 * want different things: an action belongs to the bridge, an activation to
 * the backend. So the backend is what the adapter holds, and an action is
 * passed along from here.
 */
static void on_action(const access_tunnel_action_request *request,
                      void *userdata)
{
    schultz_a11y_backend *backend = (schultz_a11y_backend *)userdata;

    if (backend != NULL && backend->action != NULL) {
        backend->action(request, backend->action_userdata);
    }
}

static bool on_activation(access_tunnel_tree_update *out_update,
                          void *userdata)
{
    schultz_a11y_backend *backend = (schultz_a11y_backend *)userdata;

    if (backend == NULL || backend->build == NULL) {
        return false;
    }
    return backend->build(out_update, backend->build_userdata);
}

int32_t schultz_a11y_backend_create(
    const schultz_a11y_backend_config *config,
    const access_tunnel_tree_update *initial,
    schultz_a11y_backend **out_backend)
{
    schultz_a11y_backend *backend;
    UIView *view;

    if (config == NULL || initial == NULL || out_backend == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    view = sdl_view(config->native_window);
    if (view == nil) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend = (schultz_a11y_backend *)calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    backend->build           = config->build;
    backend->build_userdata  = config->build_userdata;
    backend->action          = config->action;
    backend->action_userdata = config->action_userdata;

    backend->adapter = access_tunnel_ios_view_adapter_new(
        view, on_activation, on_action, backend);
    if (backend->adapter == NULL) {
        free(backend);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    backend->view = view;

    install_methods(object_getClass(view));
    objc_setAssociatedObject(view, &schultz_a11y_ios_key,
                             [NSValue valueWithPointer:backend],
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    /*
     * The first tree is given straight away rather than waited for. When
     * VoiceOver is already running this is what it reads; when it is not,
     * the adapter stays asleep and asks again through on_activation.
     */
    access_tunnel_ios_view_adapter_wake(backend->adapter);
    access_tunnel_ios_view_adapter_update(backend->adapter, initial);

    *out_backend = backend;
    return SCHULTZ_OK;
}

void schultz_a11y_backend_destroy(schultz_a11y_backend *backend)
{
    if (backend == NULL) {
        return;
    }
    if (backend->view != nil) {
        objc_setAssociatedObject(backend->view, &schultz_a11y_ios_key, nil,
                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    access_tunnel_ios_view_adapter_free(backend->adapter);
    free(backend);
}

int32_t schultz_a11y_backend_update(schultz_a11y_backend *backend,
                                    const access_tunnel_tree_update *update)
{
    if (backend == NULL || update == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    /*
     * Cheap while awake, and the only way to notice that VoiceOver started
     * after the window opened. Doing it here costs one call a frame and
     * saves subscribing to UIKit's status notifications.
     */
    access_tunnel_ios_view_adapter_wake(backend->adapter);
    access_tunnel_ios_view_adapter_update(backend->adapter, update);
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_pump(schultz_a11y_backend *backend)
{
    /* UIKit calls in. There is nothing to read. */
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
    /* An iOS application is frontmost or it is not running. */
    (void)backend;
    (void)focused;
    return SCHULTZ_OK;
}
