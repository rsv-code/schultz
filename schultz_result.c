/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * schultz_result.c - result code names.
 */

#include "schultz.h"

const char *schultz_result_string(int32_t result)
{
    switch (result) {
    case SCHULTZ_OK:                   return "SCHULTZ_OK";
    case SCHULTZ_ERR_INVALID_HANDLE:   return "SCHULTZ_ERR_INVALID_HANDLE";
    case SCHULTZ_ERR_INVALID_ARGUMENT: return "SCHULTZ_ERR_INVALID_ARGUMENT";
    case SCHULTZ_ERR_OUT_OF_MEMORY:    return "SCHULTZ_ERR_OUT_OF_MEMORY";
    case SCHULTZ_ERR_EXHAUSTED:        return "SCHULTZ_ERR_EXHAUSTED";
    case SCHULTZ_ERR_UNREADABLE:       return "SCHULTZ_ERR_UNREADABLE";
    case SCHULTZ_ERR_UNAVAILABLE:      return "SCHULTZ_ERR_UNAVAILABLE";
    default:                      return "SCHULTZ_ERR_UNKNOWN";
    }
}
