/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Storage preparation only: this switch neither changes SDP/requested media
 * nor establishes decoder profile/level or returned-buffer ABI support. */
#ifndef X4_VIDEO_CAPACITY_1080
#define X4_VIDEO_CAPACITY_1080 0
#endif
#if X4_VIDEO_CAPACITY_1080 != 0 && X4_VIDEO_CAPACITY_1080 != 1
#error X4_VIDEO_CAPACITY_1080 must be 0 or 1
#endif

enum {
    X4_VIDEO_BASE_WIDTH = 1280, X4_VIDEO_BASE_HEIGHT = 720,
    X4_VIDEO_PREPARED_WIDTH = 1920, X4_VIDEO_PREPARED_HEIGHT = 1080,
    X4_VIDEO_PREPARED_CODED_HEIGHT = 1088,
    X4_VIDEO_MAX_PITCH = 8192,
    X4_VIDEO_RGB_BUFFERS = 3,
    X4_VIDEO_NATIVE_BUDGET = 64 * 1024 * 1024,
    /* Existing per-block ceilings: compute/CPU/GPU/shared/output ring each
     * 64 MiB, plus two 2 MiB inputs. This is an upper bound of unchanged
     * normal allocations, not a physical PS4 memory allowance. */
    X4_VIDEO_LEGACY_NATIVE_CEILING = 324 * 1024 * 1024,
    X4_VIDEO_MIN_ALIGNMENT = 0x4000, X4_VIDEO_MAX_ALIGNMENT = 0x200000
};

typedef enum {
    X4_VIDEO_CAPACITY_OK = 0,
    X4_VIDEO_CAPACITY_ARGUMENT = -1,
    X4_VIDEO_CAPACITY_OVERFLOW = -2,
    X4_VIDEO_CAPACITY_DIMENSIONS = -3,
    X4_VIDEO_CAPACITY_FORMAT = -4,
    X4_VIDEO_CAPACITY_MAPPING = -5,
    X4_VIDEO_CAPACITY_STORAGE = -6,
    X4_VIDEO_CAPACITY_DESTINATION = -7,
    X4_VIDEO_CAPACITY_MEMORY = -8,
    X4_VIDEO_CAPACITY_DISABLED = -9
} X4VideoCapacityResult;

typedef struct {
    unsigned visible_width, visible_height;
    unsigned coded_width, coded_height, storage_height;
    unsigned max_pitch;
    size_t rgb_pixels, rgb_bytes, all_rgb_bytes, max_nv12_bytes;
} X4VideoCapacity;

typedef enum { X4_VIDEO_FORMAT_UNKNOWN = 0, X4_VIDEO_FORMAT_NV12 = 1,
    X4_VIDEO_FORMAT_I420 = 2, X4_VIDEO_FORMAT_P010 = 3 } X4VideoFormat;
typedef enum {
    /* Current adapted output ABI exposes no crop/format/storage-height fields.
     * This mapping is only usable for the previously established uncropped
     * NV12 path. A caller must not infer 1088->1080 from buffer size alone. */
    X4_VIDEO_MAPPING_CURRENT_UNCROPPED = 1,
    /* Requires independently verified format, storage and crop semantics.
     * Availability of this arithmetic path is not evidence of native support. */
    X4_VIDEO_MAPPING_EXPLICIT_NV12 = 2
} X4VideoMapping;

typedef struct {
    unsigned visible_width, visible_height;
    unsigned coded_width, coded_height;
    unsigned storage_height, pitch, crop_x, crop_y;
    X4VideoFormat format;
    X4VideoMapping mapping;
    size_t source_bytes, staging_bytes;
} X4VideoNv12Request;

typedef struct {
    unsigned visible_width, visible_height;
    unsigned coded_width, coded_height, storage_height, pitch;
    size_t y_bytes, uv_bytes, uv_plane_offset, total_nv12_bytes;
    size_t visible_y_offset, visible_uv_offset;
    size_t rgb_pixels, rgb_bytes;
} X4VideoNv12Layout;

typedef struct { size_t bytes, alignment; } X4VideoMemoryBlock;

/* Checked arithmetic does not alter *out on failure. Zero byte inputs are
 * allowed; an alignment must be a nonzero power of two. */
int x4_video_size_add(size_t, size_t, size_t *out);
int x4_video_size_mul(size_t, size_t, size_t *out);
int x4_video_size_align(size_t, size_t alignment, size_t *out);

/* experimental_enabled must be 0/1; 1 requires the compile-time switch. The
 * returned capacity is an allocation bound, not an admission/SDP capability. */
int x4_video_capacity_plan(int experimental_enabled, X4VideoCapacity *out);

/* Includes full padded planes, not merely visible pixels. Output is cleared
 * on rejection; no pointers, allocations, decoder or platform APIs are used. */
int x4_video_nv12_layout(const X4VideoNv12Request *, const X4VideoCapacity *,
    size_t rgb_pixel_capacity, X4VideoNv12Layout *out);

/* Round every native reservation individually (including all output/input
 * ring entries and queried compute/decoder memory) before the aggregate
 * bound. Zero-sized reservations need no alignment. This budget excludes
 * CPU-owned RGB/staging memory and the rest of the application. */
int x4_video_native_memory_total(const X4VideoMemoryBlock *, size_t count,
    size_t *out);
/* Explicit finite policy, used to preserve existing normal-mode bounds. */
int x4_video_native_memory_total_with_budget(const X4VideoMemoryBlock *, size_t count,
    size_t budget, size_t *out);
