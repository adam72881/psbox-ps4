/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/probe_contract.h"
#include "../src/video/probe_samples.generated.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; assert(c); } while (0)

static void case_bounds(void)
{
    X4VideoCapacity base, prepared;
    CHECK(x4_video_capacity_plan(0, &base) == 0);
    CHECK(x4_probe_case_validate(&x4_probe_cases[0], &base) == 0);
    CHECK(x4_probe_case_validate(&x4_probe_cases[1], &base) == 0);
    for (unsigned i = 2; i < X4_PROBE_CASE_COUNT; ++i)
        CHECK(x4_probe_case_validate(&x4_probe_cases[i], &base) == X4_PROBE_CONTRACT_GEOMETRY);
#if X4_VIDEO_CAPACITY_1080
    CHECK(x4_video_capacity_plan(1, &prepared) == 0);
    for (unsigned i = 0; i < X4_PROBE_CASE_COUNT; ++i)
        CHECK(x4_probe_case_validate(&x4_probe_cases[i], &prepared) == 0);
#else
    prepared = base;
    CHECK(x4_video_capacity_plan(1, &prepared) == X4_VIDEO_CAPACITY_DISABLED);
    prepared = base;
#endif
    CHECK(x4_probe_case_validate(NULL, &base) == X4_PROBE_CONTRACT_ARGUMENT);
    CHECK(x4_probe_case_validate(&x4_probe_cases[0], NULL) == X4_PROBE_CONTRACT_ARGUMENT);
    X4ProbeCase c = x4_probe_cases[0];
    c.id = "../bad";
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_IDENTITY);
    c = x4_probe_cases[0]; c.clip_path = "https://example.invalid/video.h264";
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_IDENTITY);
    c = x4_probe_cases[0]; memset(c.clip_sha256, 0, 32);
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_IDENTITY);
    c = x4_probe_cases[0]; c.reference_count = 0;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_FORMAT);
    c = x4_probe_cases[0]; c.frame_mbs_only_flag = 0;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_FORMAT);
    c = x4_probe_cases[0]; c.bit_depth_luma = 10;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_FORMAT);
    c = x4_probe_cases[0]; c.chroma_format_idc = 2;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_FORMAT);
    c = x4_probe_cases[0]; c.matrix_coefficients = 1;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_FORMAT);
    c = x4_probe_cases[0]; c.full_range = 1;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_FORMAT);
    c = x4_probe_cases[0]; c.profile_idc = 244;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_FORMAT);
    c = x4_probe_cases[0]; c.coded_height = 722;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_GEOMETRY);
    c = x4_probe_cases[0]; c.crop_left = 2;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_GEOMETRY);
    c = x4_probe_cases[0]; c.visible_width = UINT_MAX;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_GEOMETRY);
    c = x4_probe_cases[0]; c.crop_top = UINT_MAX - 1;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_GEOMETRY);
    c = x4_probe_cases[0]; c.level_idc = 40;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_LEVEL);
    c = x4_probe_cases[0]; c.fps_num = 60; c.duration_us /= 2;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_LEVEL);
    c = x4_probe_cases[0]; c.fps_den = 0;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_SCHEDULE);
    c = x4_probe_cases[0]; c.au_count = UINT64_MAX;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_SCHEDULE);
    c = x4_probe_cases[0]; c.duration_us += 1;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_SCHEDULE);
    c = x4_probe_cases[0]; c.max_num_ref_frames = 5;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_DPB);
    c = x4_probe_cases[0]; c.chosen_dpb_frames = 3;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_DPB);
    c = x4_probe_cases[0]; c.chosen_dpb_frames = 6;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_DPB);
    c = x4_probe_cases[0]; c.num_reorder_frames = 5;
    CHECK(x4_probe_case_validate(&c, &base) == X4_PROBE_CONTRACT_DPB);
#if X4_VIDEO_CAPACITY_1080
    c = x4_probe_cases[2]; c.max_dec_frame_buffering = 5; c.chosen_dpb_frames = 5;
    CHECK(x4_probe_case_validate(&c, &prepared) == X4_PROBE_CONTRACT_DPB);
#endif
}

static void layouts(void)
{
    X4VideoCapacity capacity;
    CHECK(x4_video_capacity_plan(X4_VIDEO_CAPACITY_1080, &capacity) == 0);
    X4VideoNv12Layout out[2];
    memset(out, 0xee, sizeof(out));
    size_t size720 = 1280u * 720u * 3u / 2u;
    CHECK(x4_probe_layout_candidates(&x4_probe_cases[0], 1280, 720, 1280,
        size720, size720, size720, capacity.rgb_pixels, &capacity, out) == 1);
    CHECK(out[0].storage_height == 720 && out[0].uv_plane_offset == 1280u * 720u);
    CHECK(out[1].total_nv12_bytes == 0);
    CHECK(x4_probe_layout_candidates(&x4_probe_cases[0], 1280, 720, 1281,
        size720, size720, size720, capacity.rgb_pixels, &capacity, out) == X4_PROBE_CONTRACT_NATIVE);
    CHECK(out[0].total_nv12_bytes == 0 && out[1].total_nv12_bytes == 0);
    CHECK(x4_probe_layout_candidates(&x4_probe_cases[0], 1280, 720, 1280,
        size720 + 1, size720, size720, capacity.rgb_pixels, &capacity, out) == X4_PROBE_CONTRACT_BOUNDS);
    CHECK(x4_probe_layout_candidates(&x4_probe_cases[0], 1280, 720, 1280,
        size720 - 1, size720, size720, capacity.rgb_pixels, &capacity, out) == X4_PROBE_CONTRACT_BOUNDS);
    CHECK(x4_probe_layout_candidates(&x4_probe_cases[0], 1280, 720, 1280,
        size720, size720, size720 - 1, capacity.rgb_pixels, &capacity, out) == X4_PROBE_CONTRACT_BOUNDS);
    CHECK(x4_probe_layout_candidates(&x4_probe_cases[0], 1280, 720, 1280,
        size720, size720, size720, 1280u * 720u - 1, &capacity, out) == X4_PROBE_CONTRACT_BOUNDS);
    CHECK(x4_probe_layout_candidates(&x4_probe_cases[0], 1280, 718, 1280,
        size720, size720, size720, capacity.rgb_pixels, &capacity, out) == X4_PROBE_CONTRACT_NATIVE);
    CHECK(x4_probe_layout_candidates(NULL, 1280, 720, 1280,
        size720, size720, size720, capacity.rgb_pixels, &capacity, out) == X4_PROBE_CONTRACT_ARGUMENT);
    CHECK(x4_probe_layout_candidates(&x4_probe_cases[0], 1280, 720, 1280,
        size720, size720, size720, capacity.rgb_pixels, &capacity, NULL) == X4_PROBE_CONTRACT_ARGUMENT);
#if X4_VIDEO_CAPACITY_1080
    const X4ProbeCase *c = &x4_probe_cases[2];
    size_t native = 2048u * 1088u * 3u / 2u, visible = 2048u * 1080u * 3u / 2u;
    CHECK(x4_probe_layout_candidates(c, 1920, 1080, 2048, native, native,
        native, capacity.rgb_pixels, &capacity, out) == 2);
    CHECK(out[0].storage_height == 1080 && out[1].storage_height == 1088);
    CHECK(out[0].uv_plane_offset == 2048u * 1080u && out[1].uv_plane_offset == 2048u * 1088u);
    CHECK(out[0].visible_y_offset == 0 && out[1].visible_y_offset == 0);
    CHECK(x4_probe_layout_candidates(c, 1920, 1088, 2048, native, native,
        native, capacity.rgb_pixels, &capacity, out) == 1);
    CHECK(out[0].storage_height == 1088 && out[1].total_nv12_bytes == 0);
    CHECK(x4_probe_layout_candidates(c, 1920, 1080, 2048, visible, native,
        native, capacity.rgb_pixels, &capacity, out) == 1);
    CHECK(out[0].storage_height == 1080);
    CHECK(x4_probe_layout_candidates(c, 1920, 1088, 2048, visible, native,
        native, capacity.rgb_pixels, &capacity, out) == X4_PROBE_CONTRACT_BOUNDS);
    CHECK(x4_probe_layout_candidates(c, 1920, 1080, 2047, native, native,
        native, capacity.rgb_pixels, &capacity, out) == X4_PROBE_CONTRACT_NATIVE);
    CHECK(x4_probe_layout_candidates(c, 1920, 1080, 1918, native, native,
        native, capacity.rgb_pixels, &capacity, out) == X4_PROBE_CONTRACT_NATIVE);
    CHECK(x4_probe_layout_candidates(c, 1920, 1080, 8194, native, native,
        native, capacity.rgb_pixels, &capacity, out) == X4_PROBE_CONTRACT_NATIVE);
    CHECK(x4_probe_layout_candidates(c, 1920, 1080, 2048, native, native,
        visible - 1, capacity.rgb_pixels, &capacity, out) == X4_PROBE_CONTRACT_BOUNDS);
    X4ProbeCase top = *c; top.crop_top = 8; top.crop_bottom = 0;
    CHECK(x4_probe_layout_candidates(&top, 1920, 1080, 2048, native, native,
        native, capacity.rgb_pixels, &capacity, out) == 2);
    CHECK(out[0].visible_y_offset == 0 && out[1].visible_y_offset == 8u * 2048u);
    CHECK(out[1].visible_uv_offset == 1088u * 2048u + 4u * 2048u);
#endif
}

int main(void)
{
    case_bounds(); layouts();
    printf("PASS probe contract capacity=%u checks=%u; bounded hypotheses only, no native support claim\n",
        (unsigned)X4_VIDEO_CAPACITY_1080, checks);
    return 0;
}
