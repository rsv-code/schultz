/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_a11y_android.c
 * @brief The Android accessibility backend.
 *
 * ## Android answers in Java
 *
 * TalkBack does not talk to native code. It asks an android.view.View for an
 * AccessibilityNodeProvider and then asks that for nodes, and both are Java
 * objects. So the attachment here is not a view pointer or a window message
 * but a Java object installed on SDL's View.
 *
 * AccessTunnel provides both halves. Delegate.java is the Java object, and
 * it decides nothing: every method passes straight through to the JNI
 * functions in access_tunnel_android_jni.c, carrying the adapter's address
 * as a long. What is left for this file is to make an adapter, find SDL's
 * View, and put a Delegate on it.
 *
 * ## What the application has to ship
 *
 * Delegate.java must be on the classpath, in the package
 * dev.accesstunnel.android. It is a source file, not a library, so an
 * application adds it to its own sources. Without it this backend finds no
 * class, reports nothing, and the application otherwise runs normally: an
 * accessibility failure must not be a startup failure.
 *
 * ## Threading
 *
 * Every call here happens on the thread that runs the frame, which under
 * SDL's Android backend is the thread SDL created and attached to the JVM.
 * SDL_GetAndroidJNIEnv returns that thread's environment.
 */

#include <jni.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "access_tunnel_android.h"
#include "schultz_a11y_backend.h"

/** The delegate AccessTunnel ships, named as the JVM names it. */
#define SCHULTZ_A11Y_DELEGATE_CLASS "dev/accesstunnel/android/Delegate"
/** SDL's activity, which is where its View can be had. */
#define SCHULTZ_A11Y_SDL_ACTIVITY_CLASS "org/libsdl/app/SDLActivity"

/** @brief The adapter and the Java View a Delegate was installed on. */
struct schultz_a11y_backend {
    access_tunnel_android_adapter *adapter; /**< The TalkBack side. */
    /**
     * SDL's View, as a global reference.
     *
     * Every update names the View it is about, so the reference has to last
     * as long as the backend. A local reference would not survive the return
     * to Java, which is what a global reference is for.
     */
    jobject host;
};

/* This thread's JNI environment, or NULL when there is not one. */
static JNIEnv *env_now(void)
{
    return (JNIEnv *)SDL_GetAndroidJNIEnv();
}

/*
 * Clears a pending Java exception.
 *
 * A JNI call that failed leaves an exception raised, and the next JNI call
 * made without clearing it aborts the process. Nothing here is worth an
 * abort: accessibility that cannot start should be quiet, not fatal.
 */
static void clear_exception(JNIEnv *env)
{
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
}

/*
 * SDL's content View, as a new global reference, or NULL.
 *
 * SDL hands out its Activity rather than its View, but SDLActivity has a
 * public static getContentView for exactly this. The View it returns is the
 * layout holding SDL's surface, which is the one an assistive technology
 * reads.
 */
static jobject sdl_content_view(JNIEnv *env)
{
    jclass activity;
    jmethodID getter;
    jobject local;
    jobject global;

    activity = (*env)->FindClass(env, SCHULTZ_A11Y_SDL_ACTIVITY_CLASS);
    if (activity == NULL) {
        clear_exception(env);
        return NULL;
    }
    getter = (*env)->GetStaticMethodID(env, activity, "getContentView",
                                       "()Landroid/view/View;");
    if (getter == NULL) {
        clear_exception(env);
        (*env)->DeleteLocalRef(env, activity);
        return NULL;
    }
    local = (*env)->CallStaticObjectMethod(env, activity, getter);
    (*env)->DeleteLocalRef(env, activity);
    if (local == NULL) {
        clear_exception(env);
        return NULL;
    }
    global = (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    return global;
}

/*
 * Puts a Delegate wrapping this adapter on the View.
 *
 * Returns whether it worked. It does not when the application did not ship
 * Delegate.java, which is a real possibility and not an error worth failing
 * startup over.
 */
static int attach_delegate(JNIEnv *env, jobject host,
                           access_tunnel_android_adapter *adapter)
{
    jclass delegate_class;
    jclass view_class;
    jmethodID constructor;
    jmethodID setter;
    jobject delegate;

    delegate_class = (*env)->FindClass(env, SCHULTZ_A11Y_DELEGATE_CLASS);
    if (delegate_class == NULL) {
        clear_exception(env);
        return 0;
    }
    constructor = (*env)->GetMethodID(env, delegate_class, "<init>", "(J)V");
    if (constructor == NULL) {
        clear_exception(env);
        (*env)->DeleteLocalRef(env, delegate_class);
        return 0;
    }
    delegate = (*env)->NewObject(env, delegate_class, constructor,
                                 (jlong)(intptr_t)adapter);
    (*env)->DeleteLocalRef(env, delegate_class);
    if (delegate == NULL) {
        clear_exception(env);
        return 0;
    }

    view_class = (*env)->GetObjectClass(env, host);
    setter = (*env)->GetMethodID(
        env, view_class, "setAccessibilityDelegate",
        "(Landroid/view/View$AccessibilityDelegate;)V");
    if (setter == NULL) {
        clear_exception(env);
        (*env)->DeleteLocalRef(env, view_class);
        (*env)->DeleteLocalRef(env, delegate);
        return 0;
    }
    (*env)->CallVoidMethod(env, host, setter, delegate);
    clear_exception(env);
    (*env)->DeleteLocalRef(env, view_class);
    (*env)->DeleteLocalRef(env, delegate);
    return 1;
}

int32_t schultz_a11y_backend_create(
    const schultz_a11y_backend_config *config,
    const access_tunnel_tree_update *initial,
    schultz_a11y_backend **out_backend)
{
    schultz_a11y_backend *backend;
    JNIEnv *env;

    if (config == NULL || initial == NULL || out_backend == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend = (schultz_a11y_backend *)calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    backend->adapter = access_tunnel_android_adapter_new(
        initial, config->action, config->action_userdata);
    if (backend->adapter == NULL) {
        free(backend);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    /*
     * From here on a failure leaves a working adapter with nothing attached
     * to it. That is deliberate: the tree is still built and still handed
     * over every frame, so an application that adds Delegate.java later
     * needs no other change, and one that never does still runs.
     */
    env = env_now();
    if (env != NULL) {
        backend->host = sdl_content_view(env);
        if (backend->host != NULL &&
            !attach_delegate(env, backend->host, backend->adapter)) {
            (*env)->DeleteGlobalRef(env, backend->host);
            backend->host = NULL;
        }
    }

    *out_backend = backend;
    return SCHULTZ_OK;
}

void schultz_a11y_backend_destroy(schultz_a11y_backend *backend)
{
    JNIEnv *env;

    if (backend == NULL) {
        return;
    }
    env = env_now();
    if (env != NULL && backend->host != NULL) {
        (*env)->DeleteGlobalRef(env, backend->host);
    }
    access_tunnel_android_adapter_free(backend->adapter);
    free(backend);
}

int32_t schultz_a11y_backend_update(schultz_a11y_backend *backend,
                                    const access_tunnel_tree_update *update)
{
    JNIEnv *env;

    if (backend == NULL || update == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    if (backend->host == NULL) {
        return SCHULTZ_OK;
    }
    env = env_now();
    if (env == NULL) {
        return SCHULTZ_OK;
    }
    access_tunnel_android_adapter_update(env, backend->adapter,
                                         backend->host, update);
    clear_exception(env);
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_pump(schultz_a11y_backend *backend)
{
    /* TalkBack calls in through Java. There is nothing to read. */
    (void)backend;
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_set_window(schultz_a11y_backend *backend,
                                        schultz_rect window)
{
    /* The View knows where it is, and reports its own bounds. */
    (void)backend;
    (void)window;
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_set_focused(schultz_a11y_backend *backend,
                                         int32_t focused)
{
    /* An Android application is foreground or it is not being read. */
    (void)backend;
    (void)focused;
    return SCHULTZ_OK;
}
