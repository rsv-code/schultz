/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * schultz_files_sdl.c - file dialogs for every platform SDL covers.
 *
 * Linux, Windows, macOS and Android. On each of those SDL has a real backend
 * and this file is a translation: our filters into SDL's, our callback shape
 * into SDL's, and nothing else.
 *
 * iOS is the one platform SDL does not cover, and it is answered by
 * schultz_files_ios.m instead. See schultz_files_backend.h.
 */

#include <stdlib.h>

#include "schultz_files_backend.h"

/*
 * What SDL was handed, and has to keep until it answers.
 *
 * SDL_DialogFileFilter is the same pair of strings ours is, but it is not the
 * same type, so the array is rebuilt here. The strings themselves are not
 * copied: they belong to the caller's filters, which outlive the answer.
 *
 * One of these is allocated per dialog and freed by the trampoline below,
 * which runs exactly once however the dialog ended.
 */
typedef struct {
    SDL_DialogFileFilter   *filters;  /**< The array SDL was handed. */
    schultz_files_answer_fn answer;   /**< Who actually wants the result. */
    void                   *userdata; /**< Theirs, not ours. */
} schultz_files_call;

/* SDL's answer, forwarded in our shape, and the last thing this call does. */
static void SDLCALL schultz_files_sdl_answer(void *userdata,
                                             const char *const *filelist,
                                             int filter)
{
    schultz_files_call *call = (schultz_files_call *)userdata;

    if (call == NULL) {
        return;
    }
    call->answer(call->userdata, filelist, (int32_t)filter);
    free(call->filters);
    free(call);
}

/* Builds the per-call state, or NULL when there is no memory for it. */
static schultz_files_call *schultz_files_sdl_call(
    const schultz_file_filter *filters, uint32_t filter_count,
    schultz_files_answer_fn answer, void *userdata)
{
    schultz_files_call *call =
        (schultz_files_call *)calloc(1u, sizeof(*call));
    uint32_t i;

    if (call == NULL) {
        return NULL;
    }
    call->answer   = answer;
    call->userdata = userdata;
    if (filters == NULL || filter_count == 0u) {
        return call;
    }
    call->filters = (SDL_DialogFileFilter *)calloc(filter_count,
                                                   sizeof(*call->filters));
    if (call->filters == NULL) {
        free(call);
        return NULL;
    }
    for (i = 0; i < filter_count; i++) {
        call->filters[i].name    = filters[i].name;
        call->filters[i].pattern = filters[i].pattern;
    }
    return call;
}

void schultz_files_show(schultz_files_kind kind, SDL_Window *window,
                        const schultz_file_options *options,
                        const schultz_file_filter *filters,
                        uint32_t filter_count,
                        schultz_files_answer_fn answer, void *userdata)
{
    schultz_files_call *call;
    const char *location;
    bool many;

    if (answer == NULL) {
        return;
    }
    call = schultz_files_sdl_call(filters, filter_count, answer, userdata);
    if (call == NULL) {
        /* Nothing was shown, so nothing will answer unless this does. The
         * caller is waiting on exactly one call and would otherwise wait
         * forever. */
        answer(userdata, NULL, -1);
        return;
    }
    location = (options == NULL) ? NULL : options->location;
    many = (options != NULL && options->allow_many) ? true : false;

    switch (kind) {
    case SCHULTZ_FILES_SAVE:
        SDL_ShowSaveFileDialog(schultz_files_sdl_answer, call, window,
                               call->filters, (int)filter_count, location);
        break;
    case SCHULTZ_FILES_FOLDER:
        /* No filters: a folder has no extension to match. */
        SDL_ShowOpenFolderDialog(schultz_files_sdl_answer, call, window,
                                 location, many);
        break;
    case SCHULTZ_FILES_OPEN:
    default:
        SDL_ShowOpenFileDialog(schultz_files_sdl_answer, call, window,
                               call->filters, (int)filter_count, location,
                               many);
        break;
    }
}
