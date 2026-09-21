/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_a11y_uia.c
 * @brief The Windows accessibility backend, over UI Automation.
 *
 * UI Automation reaches an application through one window message. A screen
 * reader sends WM_GETOBJECT to a window; the window returns an object that
 * describes what is inside it; everything else follows from that one answer.
 * So the whole of the platform attachment here is arranging to see that one
 * message and hand back what AccessTunnel produces.
 *
 * ## Why the window procedure is replaced
 *
 * SDL offers SDL_SetWindowsMessageHook, which sees every message, but it
 * returns a bool meaning "carry on" or "drop". WM_GETOBJECT needs an LRESULT
 * back, and the hook has no way to supply one, so it cannot answer this
 * message however early it sees it.
 *
 * What can is the window procedure itself. SetWindowLongPtr replaces it and
 * hands back the old one, which is the documented way to add a message to a
 * window somebody else created: answer the one message that is ours, and
 * call the previous procedure for every other. The old procedure is put back
 * on the way out, so SDL's window is left as it was found.
 */

#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "access_tunnel_uia_adapter.h"
#include "schultz_a11y_backend.h"

/** @brief The adapter, the window it answers for, and the procedure it replaced. */
struct schultz_a11y_backend {
    access_tunnel_uia_adapter *adapter; /**< The UI Automation side. */
    HWND hwnd;                          /**< SDL's window. */
    WNDPROC previous;                   /**< The procedure replaced. */
};

/*
 * The backend for a window, found from the window itself.
 *
 * The replaced procedure is called by Windows with nothing but the window
 * handle, so the backend has to be reachable from that. A window property is
 * the platform's own answer to this and belongs to the window, so it goes
 * away with it even if something skips the tidy up.
 */
static const wchar_t *const SCHULTZ_UIA_PROP = L"SchultzA11yBackend";

static LRESULT CALLBACK schultz_a11y_uia_proc(HWND hwnd, UINT message,
                                              WPARAM wparam, LPARAM lparam)
{
    schultz_a11y_backend *backend =
        (schultz_a11y_backend *)GetPropW(hwnd, SCHULTZ_UIA_PROP);
    WNDPROC previous = (backend != NULL) ? backend->previous : NULL;

    if (backend != NULL && message == WM_GETOBJECT) {
        LRESULT answer = 0;

        if (access_tunnel_uia_adapter_handle_get_object(backend->adapter,
                                                        wparam, lparam,
                                                        &answer)) {
            return answer;
        }
    }
    if (previous != NULL) {
        return CallWindowProcW(previous, hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

/* The window SDL made, or NULL when there is not one to be had. */
static HWND schultz_a11y_uia_hwnd(void *native_window)
{
    SDL_PropertiesID props;

    if (native_window == NULL) {
        return NULL;
    }
    props = SDL_GetWindowProperties((SDL_Window *)native_window);
    return (HWND)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

int32_t schultz_a11y_backend_create(
    const schultz_a11y_backend_config *config,
    const access_tunnel_tree_update *initial,
    schultz_a11y_backend **out_backend)
{
    schultz_a11y_backend *backend;
    access_tunnel_uia_adapter_config native;
    HWND hwnd;
    int32_t result;

    if (config == NULL || initial == NULL || out_backend == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    hwnd = schultz_a11y_uia_hwnd(config->native_window);
    if (hwnd == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend = (schultz_a11y_backend *)calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    memset(&native, 0, sizeof(native));
    native.hwnd            = hwnd;
    native.action          = config->action;
    native.action_userdata = config->action_userdata;
    native.is_host_focused = config->focused != 0;

    result = access_tunnel_uia_adapter_new(&native, initial,
                                           &backend->adapter);
    if (result != ACCESS_TUNNEL_OK) {
        free(backend);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    backend->hwnd = hwnd;
    if (!SetPropW(hwnd, SCHULTZ_UIA_PROP, (HANDLE)backend)) {
        access_tunnel_uia_adapter_free(backend->adapter);
        free(backend);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    backend->previous = (WNDPROC)SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, (LONG_PTR)schultz_a11y_uia_proc);

    *out_backend = backend;
    return SCHULTZ_OK;
}

void schultz_a11y_backend_destroy(schultz_a11y_backend *backend)
{
    if (backend == NULL) {
        return;
    }
    /*
     * Put SDL's procedure back before the adapter goes, so that a message
     * arriving in between is answered by SDL rather than by a freed adapter.
     */
    if (backend->hwnd != NULL) {
        if (backend->previous != NULL) {
            SetWindowLongPtrW(backend->hwnd, GWLP_WNDPROC,
                              (LONG_PTR)backend->previous);
        }
        RemovePropW(backend->hwnd, SCHULTZ_UIA_PROP);
    }
    access_tunnel_uia_adapter_free(backend->adapter);
    free(backend);
}

int32_t schultz_a11y_backend_update(schultz_a11y_backend *backend,
                                    const access_tunnel_tree_update *update)
{
    if (backend == NULL || update == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    access_tunnel_uia_adapter_update(backend->adapter, update);
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_pump(schultz_a11y_backend *backend)
{
    /* UI Automation arrives as a window message. Nothing to read. */
    (void)backend;
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_set_window(schultz_a11y_backend *backend,
                                        schultz_rect window)
{
    /* The window knows where it is; UI Automation asks it, not us. */
    (void)backend;
    (void)window;
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_set_focused(schultz_a11y_backend *backend,
                                         int32_t focused)
{
    if (backend != NULL && backend->adapter != NULL) {
        access_tunnel_uia_adapter_update_host_focus(backend->adapter,
                                                    focused != 0);
    }
    return SCHULTZ_OK;
}
