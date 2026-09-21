/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 */

/**
 * @file schultz_a11y_none.c
 * @brief The accessibility backend for a platform that has none wired up.
 *
 * Everything above this still runs: the tree is walked, the update is built,
 * and it is handed here and dropped. That costs a little work per frame and
 * buys the property that a target builds and runs before its accessibility
 * is written, rather than failing to link.
 *
 * A build that uses this file announces nothing to anybody. It is a
 * placeholder, not an implementation.
 */

#include <stdlib.h>

#include "schultz_a11y_backend.h"

/** @brief Nothing. There is no platform to hold anything for. */
struct schultz_a11y_backend {
    int32_t unused; /**< C forbids an empty struct. */
};

int32_t schultz_a11y_backend_create(
    const schultz_a11y_backend_config *config,
    const access_tunnel_tree_update *initial,
    schultz_a11y_backend **out_backend)
{
    schultz_a11y_backend *backend;

    (void)config;
    (void)initial;
    if (out_backend == NULL) {
        return SCHULTZ_ERR_INVALID_ARGUMENT;
    }
    backend = (schultz_a11y_backend *)calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return SCHULTZ_ERR_OUT_OF_MEMORY;
    }
    *out_backend = backend;
    return SCHULTZ_OK;
}

void schultz_a11y_backend_destroy(schultz_a11y_backend *backend)
{
    free(backend);
}

int32_t schultz_a11y_backend_update(schultz_a11y_backend *backend,
                                    const access_tunnel_tree_update *update)
{
    (void)backend;
    (void)update;
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_pump(schultz_a11y_backend *backend)
{
    (void)backend;
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_set_window(schultz_a11y_backend *backend,
                                        schultz_rect window)
{
    (void)backend;
    (void)window;
    return SCHULTZ_OK;
}

int32_t schultz_a11y_backend_set_focused(schultz_a11y_backend *backend,
                                         int32_t focused)
{
    (void)backend;
    (void)focused;
    return SCHULTZ_OK;
}
