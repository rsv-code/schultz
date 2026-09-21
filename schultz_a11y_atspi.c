/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_a11y_atspi.c
 * @brief The Linux accessibility backend, over AT-SPI.
 *
 * AT-SPI is a D-Bus protocol. The application registers itself on the
 * accessibility bus and answers questions about its tree; Orca and the rest
 * are clients on the other end. Nothing attaches to a window, which is why
 * this is the only backend with nothing platform specific to find first.
 *
 * Two consequences shape everything here. The connection belongs to this
 * process, so somebody has to read it: that is what pump is, and the frame
 * driver calls it every frame. And the window is not a native object anyone
 * else can measure, so its position has to be published by hand.
 */

#include <stdlib.h>
#include <string.h>

#include "access_tunnel_atspi_adapter.h"
#include "schultz_a11y_backend.h"

/* AccessTunnel names a rectangle by its two corners; Schultz by one corner
 * and a size. This is the only place in the backend that has to care. */
static access_tunnel_rect schultz_a11y_atspi_rect(schultz_rect r)
{
    access_tunnel_rect out;

    out.x0 = (double)r.x;
    out.y0 = (double)r.y;
    out.x1 = (double)(r.x + r.width);
    out.y1 = (double)(r.y + r.height);
    return out;
}

/** @brief The bus connection this application registered on. */
struct schultz_a11y_backend {
    access_tunnel_atspi_adapter *adapter; /**< The bus side. */
};

int32_t schultz_a11y_backend_create(
    const schultz_a11y_backend_config *config,
    const access_tunnel_tree_update *initial,
    schultz_a11y_backend **out_backend)
{
    schultz_a11y_backend *backend;
    access_tunnel_atspi_adapter_config native;
    int32_t result;

    if (config == NULL || initial == NULL || out_backend == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend = (schultz_a11y_backend *)calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }

    memset(&native, 0, sizeof(native));
    native.app_name        = config->app_name;
    native.toolkit_name    = config->toolkit_name;
    native.toolkit_version = config->toolkit_version;
    native.action          = config->action;
    native.action_userdata = config->action_userdata;
    native.is_host_focused = config->focused != 0;
    native.window.outer = schultz_a11y_atspi_rect(config->window);
    native.window.inner = native.window.outer;

    /*
     * Succeeds whether or not anything is listening. There may be no
     * accessibility bus at all, and the right behaviour then is to sit
     * quietly rather than to fail an application's startup.
     */
    result = access_tunnel_atspi_adapter_new(&native, initial,
                                             &backend->adapter);
    if (result != ACCESS_TUNNEL_OK) {
        free(backend);
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    *out_backend = backend;
    return SCHULTZ_OK;
}

void schultz_a11y_backend_destroy(schultz_a11y_backend *backend)
{
    if (backend == NULL) {
        return;
    }
    access_tunnel_atspi_adapter_free(backend->adapter);
    free(backend);
}

int32_t schultz_a11y_backend_update(schultz_a11y_backend *backend,
                                    const access_tunnel_tree_update *update)
{
    if (backend == NULL || update == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    access_tunnel_atspi_adapter_update(backend->adapter, update);
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_pump(schultz_a11y_backend *backend)
{
    if (backend != NULL && backend->adapter != NULL) {
        access_tunnel_atspi_adapter_pump(backend->adapter, 0);
    }
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_set_window(schultz_a11y_backend *backend,
                                        schultz_rect window)
{
    if (backend != NULL && backend->adapter != NULL) {
        access_tunnel_atspi_window_bounds bounds;

        memset(&bounds, 0, sizeof(bounds));
        bounds.outer = schultz_a11y_atspi_rect(window);
        bounds.inner = bounds.outer;
        access_tunnel_atspi_adapter_set_window_bounds(backend->adapter,
                                                      bounds);
    }
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_set_focused(schultz_a11y_backend *backend,
                                         int32_t focused)
{
    if (backend != NULL && backend->adapter != NULL) {
        access_tunnel_atspi_adapter_update_host_focus(backend->adapter,
                                                      focused != 0);
    }
    return SCHULTZ_OK;
}
