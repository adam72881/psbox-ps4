/* SPDX-License-Identifier: GPL-3.0-only */
/* Generated from independently inspected synthetic sample manifests.
 * Local candidates only: no native acceptance/performance result.
 * Regenerate with scripts/generate_probe_pipeline_sample.py --descriptors-only. */
#pragma once
#include "probe_case.h"

static const X4ProbeCase x4_probe_pipeline_cases[] = {
    {
        .id = "1080p60-high-15M", .label = "1080p60 High · 15 Mbps",
        .clip_path = "/data/xcloud4-probe/1080p60-high-15M.h264",
        .index_path = "/data/xcloud4-probe/1080p60-high-15M.x4au",
        .reference_path = "/data/xcloud4-probe/1080p60-high-15M.rgba",
        .clip_sha256 = {0xc4,0x51,0xed,0x4f,0x4f,0x61,0x05,0x7f,0xdf,0xb9,0xb7,0x9a,0xd0,0xdd,0x3a,0x13,0x8d,0x26,0x74,0xa7,0x90,0xeb,0x2c,0x06,0x18,0x03,0xa0,0x88,0x73,0x3e,0xaa,0x2d},
        .index_sha256 = {0x11,0xc1,0xa7,0x19,0x4d,0xc6,0xfe,0xe2,0xad,0x81,0xd5,0xad,0x9f,0x57,0x97,0x09,0x41,0x00,0x1d,0x7f,0x88,0xf0,0x46,0xdc,0xe8,0x16,0x8f,0xcb,0xdb,0x45,0x7e,0x0a},
        .reference_sha256 = {0x2e,0x1a,0x22,0xc8,0xaf,0x2a,0x2a,0x95,0x1d,0x05,0x71,0x40,0x36,0x07,0x2f,0x71,0x61,0x14,0xa4,0x9c,0x36,0x9c,0x74,0x84,0x98,0xb9,0xad,0x65,0x2a,0x16,0x0e,0x17},
        .visible_width = 1920, .visible_height = 1080,
        .coded_width = 1920, .coded_height = 1088,
        .crop_left = 0, .crop_top = 0, .crop_right = 0, .crop_bottom = 8,
        .fps_num = 60, .fps_den = 1,
        .frame_mbs_only_flag = 1,
        .profile_idc = 100, .level_idc = 42,
        .max_num_ref_frames = 4, .max_dec_frame_buffering = 4, .num_reorder_frames = 0,
        .chosen_dpb_frames = 4,
        .chroma_format_idc = 1, .bit_depth_luma = 8, .bit_depth_chroma = 8,
        .matrix_coefficients = 6, .full_range = 0, .reference_count = 8,
        .au_count = 600ull, .duration_us = 10000000ull
    },
};
#define X4_PROBE_PIPELINE_CASE_COUNT (sizeof(x4_probe_pipeline_cases) / sizeof(x4_probe_pipeline_cases[0]))
