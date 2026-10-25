/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_contract.h"
#include <stdio.h>
#include <string.h>

typedef struct { unsigned level, fs, mbps, dpb_mbs; } LevelBound;
/* H.264 level resource ceilings (x264 public table / H.264 Annex A).
 * They bound a fixture; they do not describe Videodec2 API capabilities. */
static const LevelBound levels[] = {
    {31, 3600, 108000, 18000},
    {32, 5120, 216000, 20480},
    {42, 8704, 522240, 34816}
};

static int nonzero_hash(const uint8_t hash[32])
{
    unsigned value = 0;
    for (unsigned i = 0; i < 32; ++i) value |= hash[i];
    return value != 0;
}

static int valid_identity(const X4ProbeCase *c)
{
    if (!c->id || !c->label || !c->clip_path || !c->index_path || !c->reference_path)
        return 0;
    size_t length = 0;
    while (length < 48 && c->id[length]) {
        unsigned char ch = (unsigned char)c->id[length++];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-')) return 0;
    }
    if (!length || length == 48) return 0;
    char expected[128];
    static const char *const suffixes[] = {"h264", "x4au", "rgba"};
    const char *paths[] = {c->clip_path, c->index_path, c->reference_path};
    for (unsigned i = 0; i < 3; ++i) {
        int n = snprintf(expected, sizeof(expected), "/data/xcloud4-probe/%s.%s", c->id, suffixes[i]);
        if (n < 0 || (size_t)n >= sizeof(expected) || strcmp(paths[i], expected)) return 0;
    }
    return nonzero_hash(c->clip_sha256) && nonzero_hash(c->index_sha256) && nonzero_hash(c->reference_sha256);
}

int x4_probe_case_validate(const X4ProbeCase *c, const X4VideoCapacity *capacity)
{
    if (!c || !capacity) return X4_PROBE_CONTRACT_ARGUMENT;
    if (!valid_identity(c)) return X4_PROBE_CONTRACT_IDENTITY;
    if ((c->profile_idc != 66 && c->profile_idc != 77 && c->profile_idc != 100) ||
        c->chroma_format_idc != 1 || c->bit_depth_luma != 8 || c->bit_depth_chroma != 8 ||
        c->frame_mbs_only_flag != 1 || c->matrix_coefficients != 6 || c->full_range ||
        c->reference_count != 8)
        return X4_PROBE_CONTRACT_FORMAT;
    if (!c->visible_width || !c->visible_height || !c->coded_width || !c->coded_height ||
        ((c->visible_width | c->visible_height | c->crop_left | c->crop_top |
          c->crop_right | c->crop_bottom) & 1) ||
        (c->coded_width & 15) || (c->coded_height & 15) ||
        c->visible_width > X4_VIDEO_PREPARED_WIDTH || c->visible_height > X4_VIDEO_PREPARED_HEIGHT ||
        c->coded_width > X4_VIDEO_PREPARED_WIDTH || c->coded_height > X4_VIDEO_PREPARED_CODED_HEIGHT ||
        c->visible_width > capacity->visible_width || c->visible_height > capacity->visible_height ||
        c->coded_width > capacity->coded_width || c->coded_height > capacity->coded_height ||
        c->coded_height > capacity->storage_height ||
        (uint64_t)c->crop_left + c->visible_width + c->crop_right != c->coded_width ||
        (uint64_t)c->crop_top + c->visible_height + c->crop_bottom != c->coded_height)
        return X4_PROBE_CONTRACT_GEOMETRY;
    if (!c->fps_num || !c->fps_den || c->fps_num > 240000 || c->fps_den > 10000 ||
        (uint64_t)c->fps_num > (uint64_t)60 * c->fps_den ||
        !c->au_count || c->au_count > X4_PROBE_MAX_RECORDS ||
        !c->duration_us || c->duration_us > X4_PROBE_DURATION_MAX_US ||
        c->duration_us != c->au_count * UINT64_C(1000000) * c->fps_den / c->fps_num)
        return X4_PROBE_CONTRACT_SCHEDULE;
    const LevelBound *bound = NULL;
    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i)
        if (c->level_idc == levels[i].level) bound = &levels[i];
    uint64_t fs = (uint64_t)(c->coded_width / 16) * (c->coded_height / 16);
    if (!bound || !fs || fs > bound->fs || fs * c->fps_num > (uint64_t)bound->mbps * c->fps_den)
        return X4_PROBE_CONTRACT_LEVEL;
    unsigned max_frames = bound->dpb_mbs / (unsigned)fs;
    if (max_frames > 16) max_frames = 16;
    if (!c->max_dec_frame_buffering || c->max_num_ref_frames > c->max_dec_frame_buffering ||
        c->num_reorder_frames > c->max_dec_frame_buffering ||
        !c->chosen_dpb_frames || c->chosen_dpb_frames < c->max_dec_frame_buffering ||
        c->max_dec_frame_buffering > max_frames || c->chosen_dpb_frames > max_frames)
        return X4_PROBE_CONTRACT_DPB;
    return 0;
}

static int same_layout(const X4VideoNv12Layout *a, const X4VideoNv12Layout *b)
{
    return a->visible_width == b->visible_width && a->visible_height == b->visible_height &&
        a->pitch == b->pitch && a->storage_height == b->storage_height &&
        a->visible_y_offset == b->visible_y_offset && a->visible_uv_offset == b->visible_uv_offset &&
        a->total_nv12_bytes == b->total_nv12_bytes;
}

int x4_probe_layout_candidates(const X4ProbeCase *c, unsigned width, unsigned height,
    unsigned pitch, size_t returned_bytes, size_t owned_bytes, size_t staging_bytes,
    size_t pixel_capacity, const X4VideoCapacity *capacity, X4VideoNv12Layout out[2])
{
    if (!out) return X4_PROBE_CONTRACT_ARGUMENT;
    memset(out, 0, 2 * sizeof(*out));
    int rc = x4_probe_case_validate(c, capacity);
    if (rc) return rc;
    if (!width || !height || (width != c->visible_width && width != c->coded_width) ||
        (height != c->visible_height && height != c->coded_height) ||
        pitch < width || pitch > capacity->max_pitch || pitch > X4_VIDEO_MAX_PITCH || (pitch & 1))
        return X4_PROBE_CONTRACT_NATIVE;
    if (!returned_bytes || !owned_bytes || !staging_bytes || returned_bytes > owned_bytes)
        return X4_PROBE_CONTRACT_BOUNDS;

    X4VideoNv12Request requests[2] = {
        {.visible_width = c->visible_width, .visible_height = c->visible_height,
         .coded_width = c->visible_width, .coded_height = c->visible_height,
         .storage_height = c->visible_height, .pitch = pitch,
         .format = X4_VIDEO_FORMAT_NV12, .mapping = X4_VIDEO_MAPPING_EXPLICIT_NV12,
         .source_bytes = returned_bytes, .staging_bytes = staging_bytes},
        {.visible_width = c->visible_width, .visible_height = c->visible_height,
         .coded_width = c->coded_width, .coded_height = c->coded_height,
         .storage_height = c->coded_height, .pitch = pitch,
         .crop_x = c->crop_left, .crop_y = c->crop_top,
         .format = X4_VIDEO_FORMAT_NV12, .mapping = X4_VIDEO_MAPPING_EXPLICIT_NV12,
         .source_bytes = returned_bytes, .staging_bytes = staging_bytes}
    };
    unsigned count = 0;
    for (unsigned i = 0; i < 2; ++i) {
        /* Already-cropped storage is only a hypothesis when both native
         * dimensions equal the signaled visible rectangle. */
        if (!i && (width != c->visible_width || height != c->visible_height)) continue;
        X4VideoNv12Layout candidate = {0};
        if (x4_video_nv12_layout(&requests[i], capacity, pixel_capacity, &candidate)) continue;
        if (candidate.total_nv12_bytes > owned_bytes) continue;
        if (count && same_layout(&out[0], &candidate)) continue;
        out[count++] = candidate;
    }
    return count ? (int)count : X4_PROBE_CONTRACT_BOUNDS;
}
