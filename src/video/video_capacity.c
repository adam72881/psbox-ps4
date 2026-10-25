/* SPDX-License-Identifier: GPL-3.0-only */
#include "video_capacity.h"
#include <string.h>

int x4_video_size_add(size_t a, size_t b, size_t *out)
{
    if (!out) return X4_VIDEO_CAPACITY_ARGUMENT;
    if (a > SIZE_MAX - b) return X4_VIDEO_CAPACITY_OVERFLOW;
    *out = a + b;
    return X4_VIDEO_CAPACITY_OK;
}

int x4_video_size_mul(size_t a, size_t b, size_t *out)
{
    if (!out) return X4_VIDEO_CAPACITY_ARGUMENT;
    if (b && a > SIZE_MAX / b) return X4_VIDEO_CAPACITY_OVERFLOW;
    *out = a * b;
    return X4_VIDEO_CAPACITY_OK;
}

int x4_video_size_align(size_t value, size_t alignment, size_t *out)
{
    size_t sum;
    if (!out || !alignment || (alignment & (alignment - 1)))
        return X4_VIDEO_CAPACITY_ARGUMENT;
    int rc = x4_video_size_add(value, alignment - 1, &sum);
    if (rc) return rc;
    *out = sum & ~(alignment - 1);
    return X4_VIDEO_CAPACITY_OK;
}

int x4_video_capacity_plan(int experimental_enabled, X4VideoCapacity *out)
{
    if (!out) return X4_VIDEO_CAPACITY_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (experimental_enabled != 0 && experimental_enabled != 1)
        return X4_VIDEO_CAPACITY_ARGUMENT;
    if (experimental_enabled && !X4_VIDEO_CAPACITY_1080)
        return X4_VIDEO_CAPACITY_DISABLED;
    X4VideoCapacity result = {
        .visible_width = experimental_enabled ? X4_VIDEO_PREPARED_WIDTH : X4_VIDEO_BASE_WIDTH,
        .visible_height = experimental_enabled ? X4_VIDEO_PREPARED_HEIGHT : X4_VIDEO_BASE_HEIGHT,
        .coded_width = experimental_enabled ? X4_VIDEO_PREPARED_WIDTH : X4_VIDEO_BASE_WIDTH,
        .coded_height = experimental_enabled ? X4_VIDEO_PREPARED_CODED_HEIGHT : X4_VIDEO_BASE_HEIGHT,
        .storage_height = experimental_enabled ? X4_VIDEO_PREPARED_CODED_HEIGHT : X4_VIDEO_BASE_HEIGHT,
        .max_pitch = X4_VIDEO_MAX_PITCH
    };
    size_t y, uv;
    if (x4_video_size_mul(result.visible_width, result.visible_height, &result.rgb_pixels) ||
        x4_video_size_mul(result.rgb_pixels, sizeof(uint32_t), &result.rgb_bytes) ||
        x4_video_size_mul(result.rgb_bytes, X4_VIDEO_RGB_BUFFERS, &result.all_rgb_bytes) ||
        x4_video_size_mul(result.max_pitch, result.storage_height, &y) ||
        x4_video_size_mul(result.max_pitch, result.storage_height / 2, &uv) ||
        x4_video_size_add(y, uv, &result.max_nv12_bytes))
        return X4_VIDEO_CAPACITY_OVERFLOW;
    *out = result;
    return X4_VIDEO_CAPACITY_OK;
}

int x4_video_nv12_layout(const X4VideoNv12Request *request, const X4VideoCapacity *capacity,
    size_t rgb_pixel_capacity, X4VideoNv12Layout *out)
{
    if (!out) return X4_VIDEO_CAPACITY_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!request || !capacity) return X4_VIDEO_CAPACITY_ARGUMENT;
    if (request->format != X4_VIDEO_FORMAT_NV12) return X4_VIDEO_CAPACITY_FORMAT;
    if (request->mapping != X4_VIDEO_MAPPING_CURRENT_UNCROPPED &&
        request->mapping != X4_VIDEO_MAPPING_EXPLICIT_NV12)
        return X4_VIDEO_CAPACITY_MAPPING;
    if (!request->visible_width || !request->visible_height ||
        !request->coded_width || !request->coded_height || !request->storage_height ||
        ((request->visible_width | request->visible_height | request->coded_width |
          request->coded_height | request->storage_height | request->pitch |
          request->crop_x | request->crop_y) & 1) ||
        request->visible_width > capacity->visible_width ||
        request->visible_height > capacity->visible_height ||
        request->coded_width > capacity->coded_width ||
        request->coded_height > capacity->coded_height ||
        request->storage_height > capacity->storage_height ||
        request->storage_height < request->coded_height ||
        request->pitch < request->coded_width || request->pitch > capacity->max_pitch ||
        request->crop_x > request->coded_width || request->crop_y > request->coded_height ||
        request->visible_width > request->coded_width - request->crop_x ||
        request->visible_height > request->coded_height - request->crop_y)
        return X4_VIDEO_CAPACITY_DIMENSIONS;
    if (request->mapping == X4_VIDEO_MAPPING_CURRENT_UNCROPPED &&
        (request->visible_width != request->coded_width ||
         request->visible_height != request->coded_height ||
         request->visible_height != request->storage_height || request->crop_x || request->crop_y))
        return X4_VIDEO_CAPACITY_MAPPING;

    X4VideoNv12Layout result = {
        .visible_width = request->visible_width, .visible_height = request->visible_height,
        .coded_width = request->coded_width, .coded_height = request->coded_height,
        .storage_height = request->storage_height, .pitch = request->pitch
    };
    size_t offset;
    if (x4_video_size_mul(request->pitch, request->storage_height, &result.y_bytes) ||
        x4_video_size_mul(request->pitch, request->storage_height / 2, &result.uv_bytes) ||
        x4_video_size_add(result.y_bytes, result.uv_bytes, &result.total_nv12_bytes) ||
        x4_video_size_mul(request->crop_y, request->pitch, &offset) ||
        x4_video_size_add(offset, request->crop_x, &result.visible_y_offset) ||
        x4_video_size_mul(request->crop_y / 2, request->pitch, &offset) ||
        x4_video_size_add(result.y_bytes, offset, &offset) ||
        x4_video_size_add(offset, request->crop_x, &result.visible_uv_offset) ||
        x4_video_size_mul(request->visible_width, request->visible_height, &result.rgb_pixels) ||
        x4_video_size_mul(result.rgb_pixels, sizeof(uint32_t), &result.rgb_bytes))
        return X4_VIDEO_CAPACITY_OVERFLOW;
    result.uv_plane_offset = result.y_bytes;
    if (result.total_nv12_bytes > capacity->max_nv12_bytes ||
        result.total_nv12_bytes > request->source_bytes ||
        result.total_nv12_bytes > request->staging_bytes)
        return X4_VIDEO_CAPACITY_STORAGE;
    if (result.rgb_pixels > rgb_pixel_capacity)
        return X4_VIDEO_CAPACITY_DESTINATION;
    *out = result;
    return X4_VIDEO_CAPACITY_OK;
}

int x4_video_native_memory_total(const X4VideoMemoryBlock *blocks, size_t count, size_t *out)
{
    return x4_video_native_memory_total_with_budget(blocks, count, X4_VIDEO_NATIVE_BUDGET, out);
}
int x4_video_native_memory_total_with_budget(const X4VideoMemoryBlock *blocks, size_t count, size_t budget, size_t *out)
{
    if (!out || (!blocks && count) || !budget || budget > X4_VIDEO_LEGACY_NATIVE_CEILING)
        return X4_VIDEO_CAPACITY_ARGUMENT;
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!blocks[i].bytes) continue;
        size_t alignment = blocks[i].alignment;
        if (alignment > X4_VIDEO_MAX_ALIGNMENT || (alignment && (alignment & (alignment - 1))))
            return X4_VIDEO_CAPACITY_ARGUMENT;
        if (alignment < X4_VIDEO_MIN_ALIGNMENT) alignment = X4_VIDEO_MIN_ALIGNMENT;
        size_t rounded;
        int rc = x4_video_size_align(blocks[i].bytes, alignment, &rounded);
        if (rc) return rc;
        rc = x4_video_size_add(total, rounded, &total);
        if (rc) return rc;
        if (total > budget) return X4_VIDEO_CAPACITY_MEMORY;
    }
    *out = total;
    return X4_VIDEO_CAPACITY_OK;
}
