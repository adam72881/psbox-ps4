/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../streaming/stream_profile.h"
#include "../core/build_identity.h"
#include "lang.h"
#if X4_STREAM_PROFILE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Shared by the real main/log and session renderer; no remote strings enter
 * this bounded formatter. Dimensions are the rejected SPS's visible values.
 * The full message fits the real 160-byte status buffer even for UINT32_MAX.
 * A zero dimension means there was no dimension rejection to explain. */
static inline int x4_stream_video_error_text(char *out, size_t capacity,
    uint32_t width, uint32_t height, int error)
{
    if (!out || !capacity) return -1;
#if X4_PUBLIC
    int n = width && height ?
        snprintf(out, capacity, x4_tr(X4_TR_VIDEO_REJECTED_DIM_PUBLIC_FMT), width, height) :
        snprintf(out, capacity, x4_tr(X4_TR_VIDEO_STOPPED_PUBLIC_FMT), error);
#else
    int n = width && height ?
        snprintf(out, capacity, x4_tr(X4_TR_VIDEO_REJECTED_DIM_LAB_FMT), width, height) :
        snprintf(out, capacity, x4_tr(X4_TR_VIDEO_STOPPED_LAB_FMT), error);
#endif
    return n >= 0 && (size_t)n < capacity ? 0 : -1;
}
static inline int x4_stream_video_error_caption(char *out, size_t capacity, int error)
{
    if (!out || !capacity) return -1;
#if X4_PUBLIC
    int n = snprintf(out, capacity, x4_tr(X4_TR_VIDEO_STOPPED_PUBLIC_FMT), error);
#else
    int n = snprintf(out, capacity, x4_tr(X4_TR_VIDEO_STOPPED_LAB_FMT), error);
#endif
    return n >= 0 && (size_t)n < capacity ? 0 : -1;
}
#endif
