/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "video_capacity.h"
/* Shared by local probes and network playback. This module owns no native
 * objects, threads, transport or allocator; the caller supplies synchronization. */

enum { X4_PIPELINE_OUTPUTS = 4, X4_PIPELINE_SCANOUTS = 3,
    X4_PIPELINE_WIDTH = 1920, X4_PIPELINE_HEIGHT = 1080,
    X4_PIPELINE_OK = 0, X4_PIPELINE_WAIT = 1,
    X4_PIPELINE_INVALID = -7601, X4_PIPELINE_STALE = -7602,
    X4_PIPELINE_STATE = -7603, X4_PIPELINE_EXHAUSTED = -7604 };

#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
/* Ported from probe depth 2: storage for five; runtime defaults to four.
 * Only an admitted depth-two owner selects five after native Query/Create. */
enum { X4_PIPELINE_OUTPUT_CAPACITY = 5 };
#else
enum { X4_PIPELINE_OUTPUT_CAPACITY = X4_PIPELINE_OUTPUTS };
#endif

typedef struct { unsigned slot; uint64_t epoch, generation; } X4VideoPipelineLease;
typedef enum { X4_PIPELINE_FREE, X4_PIPELINE_DECODING,
    X4_PIPELINE_READY, X4_PIPELINE_CONVERTING } X4VideoPipelineOutputState;
typedef struct {
    X4VideoPipelineOutputState state;
    uint64_t generation, output_id, decode_end_us, ready_order;
} X4VideoPipelineOutput;
typedef struct {
    X4VideoPipelineOutput slots[X4_PIPELINE_OUTPUT_CAPACITY];
#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
    unsigned output_count; /* initialized to four; depth two explicitly selects five */
#endif
    uint64_t epoch, next_generation, next_ready_order;
    uint64_t acquired, published, converted, cancelled, discarded_ready;
    uint64_t discarded_latency;
    uint64_t waits, rejected;
    unsigned occupied, peak_occupied, ready, peak_ready;
} X4VideoPipelineRing;

/* All state functions require ONE caller-owned lock, including readers of
 * the public fields. No function waits, allocates, calls native APIs or logs.
 * Init is only legal before any worker/native reference exists. A new session
 * supplies a different nonzero epoch; no reset may release in-flight buffers.
 * DECODING may outlive its Decode call when the native decoder retains it. */
int x4_video_pipeline_ring_init(X4VideoPipelineRing *, uint64_t epoch);
#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
int x4_video_pipeline_ring_init_outputs(X4VideoPipelineRing *, uint64_t epoch,
    unsigned output_count);
#endif
int x4_video_pipeline_acquire_decode(X4VideoPipelineRing *,
    X4VideoPipelineLease *, uint64_t *discarded_output_id);
/* Find a native-held lease by returned buffer index. This does not publish it. */
int x4_video_pipeline_decoding_lease(X4VideoPipelineRing *, unsigned slot,
    X4VideoPipelineLease *);
int x4_video_pipeline_publish_ready(X4VideoPipelineRing *, X4VideoPipelineLease,
    uint64_t output_id, uint64_t decode_end_us);
typedef struct {
    unsigned count;
    uint64_t output_ids[X4_PIPELINE_OUTPUT_CAPACITY - 1];
} X4VideoPipelineLatencyDrops;
/* Select the newest READY publication (greatest ready_order), then release
 * every older unclaimed READY. Optional drops is cleared on every call and
 * receives at most three output IDs in oldest-to-newest publication order.
 * These drops increment discarded_latency, NOT discarded_ready (ring-full
 * admission), cancelled or converted. Output IDs, slot indices and native
 * decode timestamps do not determine freshness. DECODING/CONVERTING remain
 * owned by native/the converter. Acquire-decode's full-ring policy is unchanged. */
int x4_video_pipeline_take_ready(X4VideoPipelineRing *, X4VideoPipelineLease *,
    X4VideoPipelineLatencyDrops *drops);
int x4_video_pipeline_release_conversion(X4VideoPipelineRing *, X4VideoPipelineLease);
/* Call only after native confirms that this particular lease was NOT accepted,
 * a completed (possibly damaged) output has returned its exact validated slot,
 * or native decoder destruction has released all references to it. */
int x4_video_pipeline_abort_decode(X4VideoPipelineRing *, X4VideoPipelineLease);
unsigned x4_video_pipeline_cancel_ready(X4VideoPipelineRing *);
bool x4_video_pipeline_ring_idle(const X4VideoPipelineRing *);

typedef enum { X4_SCANOUT_FREE, X4_SCANOUT_WRITING, X4_SCANOUT_READY,
    X4_SCANOUT_PENDING, X4_SCANOUT_SCREEN } X4VideoPipelineScanoutState;
typedef struct {
    X4VideoPipelineScanoutState state;
    uint64_t lease_generation, output_id, decode_end_us, ready_order;
    uint64_t converted_us, flip_arg, submitted_us, observed_us;
} X4VideoPipelineScanout;
typedef struct {
    X4VideoPipelineScanout slots[X4_PIPELINE_SCANOUTS];
    uint64_t epoch, next_generation, next_ready_order, last_submitted_arg;
    uint64_t submitted, observed, rejected;
    unsigned peak_occupied, peak_in_flight;
    int pending, screen;
} X4VideoPipelineDisplay;
typedef struct {
    unsigned free, writing, ready, pending, screen, occupied, in_flight;
} X4VideoPipelineDisplayOccupancy;
int x4_video_pipeline_display_init(X4VideoPipelineDisplay *, uint64_t epoch);
/* Pure snapshot under the same owner lock. SCREEN counts as occupied but is
 * not in flight. WRITING + READY + PENDING must never exceed one. */
int x4_video_pipeline_display_occupancy(const X4VideoPipelineDisplay *,
    X4VideoPipelineDisplayOccupancy *);
/* One frame in flight: WAIT while any conversion, ready scanout or submitted
 * flip owns a slot. Acquire this BEFORE taking the latest native READY. */
int x4_video_pipeline_acquire_scanout(X4VideoPipelineDisplay *, X4VideoPipelineLease *);
int x4_video_pipeline_finish_scanout(X4VideoPipelineDisplay *, X4VideoPipelineLease,
    uint64_t output_id, uint64_t decode_end_us, uint64_t converted_us);
int x4_video_pipeline_abort_scanout(X4VideoPipelineDisplay *, X4VideoPipelineLease);
/* Cancel only unsubmitted READY. Never releases WRITING, PENDING or SCREEN. */
unsigned x4_video_pipeline_cancel_ready_scanout(X4VideoPipelineDisplay *);
/* Call only AFTER VideoOutClose succeeds and all converters have joined.
 * Retires display references while preserving generations and statistics. */
int x4_video_pipeline_display_closed(X4VideoPipelineDisplay *);
/* Returns oldest READY, only while no flip is pending. Caller must submit it
 * to VideoOut before mark_submitted; on failure it remains READY for teardown. */
int x4_video_pipeline_ready_scanout(X4VideoPipelineDisplay *, X4VideoPipelineLease *);
int x4_video_pipeline_mark_submitted(X4VideoPipelineDisplay *, X4VideoPipelineLease,
    uint64_t flip_arg, uint64_t submitted_us);
/* An exact pending flip-arg observation transfers SCREEN ownership and frees
 * only the previous SCREEN. Unrelated/stale observations never free buffers. */
int x4_video_pipeline_observe_flip(X4VideoPipelineDisplay *, uint64_t flip_arg,
    uint64_t observed_us, X4VideoPipelineLease *presented);

typedef struct { unsigned first_row, end_row; } X4VideoPipelineStripe;
int x4_video_pipeline_stripe(unsigned height, unsigned workers, unsigned index,
    X4VideoPipelineStripe *);
/* Restricted to the layouts independently established by the probe: visible
 * 1280x720 in 720 rows, or 1920x1080 in 1088 rows, uncropped NV12. Bounds cover
 * full storage planes, not just visible bytes. Returns before writes on error.
 * Destination is A8B8G8R8, 1920x1080, with pitch in uint32_t pixels. */
int x4_video_pipeline_convert_stripe(const X4VideoNv12Layout *,
    const uint8_t *source, size_t source_bytes, uint32_t *destination,
    size_t destination_bytes, unsigned destination_pitch,
    unsigned workers, unsigned index);
/* Diagnostic-only 1:1 conversion for initial reference comparison. */
int x4_video_pipeline_convert_reference(const X4VideoNv12Layout *,
    const uint8_t *source, size_t source_bytes, uint32_t *destination,
    size_t destination_bytes);

/* Streaming color is selected from the SPS VUI. Matrix 1 is BT.709; 2
 * (unspecified), 5 and 6 retain BT.601 compatibility. Unknown matrix values
 * also use BT.601; the owner must record that assumption. Both limited and
 * full range are implemented. The legacy entry points above keep their
 * original restricted geometry and exact Q8 BT.601 arithmetic. */
typedef struct { unsigned matrix_coefficients; bool full_range; } X4VideoPipelineColor;
enum { X4_PIPELINE_CONVERT_SCALAR = 0, X4_PIPELINE_CONVERT_SSE2 = 1 };
/* Validates progressive 8-bit NV12 storage up to coded 1920x1088. Explicit
 * visible offsets encode cropping; crop coordinates may be odd, in which
 * case chroma is sampled from the absolute source coordinate. Fast conversion
 * requires coded/storage 1280x720 or 1920x1088 (visible 1080), with even crop
 * coordinates. Other safe coded/storage geometry uses scalar scaling even
 * when its visible rectangle happens to be exactly 720p or 1080p.
 * The returned path is the compiled implementation, not a timing claim. */
int x4_video_pipeline_color_path(const X4VideoNv12Layout *, size_t source_bytes,
    unsigned *path);
int x4_video_pipeline_convert_stripe_color(const X4VideoNv12Layout *,
    const uint8_t *source, size_t source_bytes, uint32_t *destination,
    size_t destination_bytes, unsigned destination_pitch,
    unsigned workers, unsigned index, const X4VideoPipelineColor *);
int x4_video_pipeline_convert_reference_color(const X4VideoNv12Layout *,
    const uint8_t *source, size_t source_bytes, uint32_t *destination,
    size_t destination_bytes, const X4VideoPipelineColor *);
