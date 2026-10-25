/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "probe_source.h"
#include "probe_metrics.h"

/* Trusted generated descriptors are derived from inspected SPS/tool evidence.
 * They do not advertise capabilities to Xbox or prove native acceptance. */
typedef struct {
    const char *id, *label, *clip_path, *index_path, *reference_path;
    uint8_t clip_sha256[32], reference_sha256[32], index_sha256[32];
    unsigned visible_width, visible_height, coded_width, coded_height;
    unsigned crop_left, crop_top, crop_right, crop_bottom;
    unsigned fps_num, fps_den, profile_idc, level_idc;
    unsigned frame_mbs_only_flag;
    unsigned max_num_ref_frames, max_dec_frame_buffering, num_reorder_frames;
    unsigned chosen_dpb_frames, chroma_format_idc, bit_depth_luma, bit_depth_chroma;
    unsigned matrix_coefficients, full_range, reference_count;
    uint64_t au_count, duration_us;
} X4ProbeCase;

enum { X4_PROBE_LAYOUT_PENDING=0, X4_PROBE_LAYOUT_VERIFIED=1,
    X4_PROBE_LAYOUT_INDETERMINATE=2, X4_PROBE_LAYOUT_FAILED=3 };
#define X4_PROBE_RC_NOT_RUN INT32_MIN
typedef struct {
    uint64_t input_aus, native_outputs, rgb_publications, last_input_pts_us;
    uint64_t rgb_conversions;
    uint64_t late_inputs, max_input_late_us, measured_start_us;
    uint64_t layout_checks, layout_ambiguous, layout_reference_matches;
    unsigned layout_verdict, selected_storage_height, selected_crop_y;
    int source_result, query_result, create_result, decode_result, close_result;
    int pipeline_result;
    int reference_result;
    int compute_query_result, allocate_queue_result, flush_result;
    int delete_decoder_result, release_queue_result, reference_close_result;
    unsigned output_reject_reason;
    uint64_t queried_compute_bytes, queried_cpu_bytes, queried_gpu_bytes, queried_shared_bytes;
    uint64_t queried_frame_bytes, rounded_native_bytes, staging_bytes, rgb_bytes;
    uint64_t allocated_native_bytes;
    int output_memory_type, output_memory_protection;
    uint64_t native_policy_budget;
    uint64_t peak_native_allocated_bytes;
    bool ready, source_eof, draining, finished, cancel_requested;
} X4ProbeStatus;
static inline X4ProbeStatus x4_probe_status_pending(void)
{
    return (X4ProbeStatus){.source_result=X4_PROBE_RC_NOT_RUN, .query_result=X4_PROBE_RC_NOT_RUN,
        .create_result=X4_PROBE_RC_NOT_RUN, .decode_result=X4_PROBE_RC_NOT_RUN,
        .close_result=X4_PROBE_RC_NOT_RUN, .pipeline_result=X4_PROBE_RC_NOT_RUN,
        .reference_result=X4_PROBE_RC_NOT_RUN, .compute_query_result=X4_PROBE_RC_NOT_RUN,
        .allocate_queue_result=X4_PROBE_RC_NOT_RUN, .flush_result=X4_PROBE_RC_NOT_RUN,
        .delete_decoder_result=X4_PROBE_RC_NOT_RUN, .release_queue_result=X4_PROBE_RC_NOT_RUN,
        .reference_close_result=X4_PROBE_RC_NOT_RUN};
}
enum { X4_PROBE_END_CLEAN_EOF=1, X4_PROBE_END_CANCEL=2,
    X4_PROBE_END_SETUP_FAILURE=3, X4_PROBE_END_ERROR=4 };
static inline unsigned x4_probe_end_reason(const X4ProbeStatus *s)
{
    if (!s) return X4_PROBE_END_SETUP_FAILURE;
    if ((s->close_result != X4_PROBE_RC_NOT_RUN && s->close_result < 0) ||
        (s->reference_close_result != X4_PROBE_RC_NOT_RUN && s->reference_close_result < 0))
        return X4_PROBE_END_ERROR;
    if (s->pipeline_result == X4_PROBE_CANCELLED) return X4_PROBE_END_CANCEL;
    if (s->pipeline_result == 0 && s->source_eof && s->close_result == 0 &&
        !(s->reference_close_result != X4_PROBE_RC_NOT_RUN && s->reference_close_result < 0))
        return X4_PROBE_END_CLEAN_EOF;
    return X4_PROBE_END_ERROR;
}
