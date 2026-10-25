/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { X4_STREAM_AU_ARENA_BYTES = 8 * 1024 * 1024,
    X4_STREAM_AU_ENTRIES = 32, X4_STREAM_AU_MAX_BYTES = 2 * 1024 * 1024,
    X4_STREAM_AU_PARAMETER_BYTES = 4096, X4_STREAM_AU_LAG_US = 250000 };
typedef struct X4StreamAuQueue X4StreamAuQueue;
typedef enum {
    X4_STREAM_AU_OK = 0, X4_STREAM_AU_DROPPED_FULL = 1,
    X4_STREAM_AU_DROPPED_LATE = 2, X4_STREAM_AU_DROPPED_WAIT_IDR = 3,
    X4_STREAM_AU_BUSY = 4, X4_STREAM_AU_STOPPED = 5,
    X4_STREAM_AU_EMPTY = 6, X4_STREAM_AU_FATAL = -7440
} X4StreamAuResult;
typedef enum { X4_STREAM_AU_LOSS_FULL = 1, X4_STREAM_AU_LOSS_LATE = 2,
    X4_STREAM_AU_LOSS_BUSY = 3 } X4StreamAuLoss;
typedef struct {
    const uint8_t *data, *sps, *pps;
    size_t bytes, sps_bytes, pps_bytes;
    uint64_t pts_us, identity, arrival_us;
    uint32_t rtp_timestamp;
    bool rtp_timestamp_valid, is_idr;
} X4StreamAuInput;
typedef struct { uint64_t generation; unsigned slot; } X4StreamAuLease;
typedef struct {
    const uint8_t *data, *sps, *pps;
    size_t bytes, sps_bytes, pps_bytes;
    uint64_t pts_us, identity, arrival_us, enqueued_us;
    uint32_t rtp_timestamp;
    bool rtp_timestamp_valid, is_idr;
} X4StreamAuView;
typedef struct {
    /* A single intention when false->true IDR wait is entered. Not a PLI. */
    bool request_keyframe, waiting_idr;
    uint64_t recovery_generation;
} X4StreamAuOutcome;
typedef struct {
    uint64_t admitted, taken, released;
    /* Admission/result decisions. A consumer late decision can retire several
     * queued AUs; never sum these with the separate READY-retirement counts. */
    uint64_t dropped_full, dropped_late, dropped_wait_idr, dropped_busy;
    uint64_t rejected_incoming_full,rejected_incoming_late,rejected_incoming_wait_idr,rejected_incoming_busy;
    /* READY AUs retired exactly once, attributed to the cause fixed when
     * this recovery episode entered IDR wait. Includes retired old IDRs. */
    uint64_t retired_ready_full,retired_ready_late,retired_ready_busy;
    uint64_t evicted_non_idr, old_idr_retired, recovery_transitions;
    uint64_t request_intentions, request_coalesced, padding_bytes_total;
    uint64_t samples, lag_p50_upper_us, lag_p95_upper_us, lag_max_us;
    uint64_t lag_histogram_overflow;
    unsigned depth_p50, depth_p95, depth_max;
    unsigned resident_entries, ready_entries, inflight_entries;
    size_t resident_bytes, payload_bytes, arena_bytes, metadata_bytes;
    bool waiting_idr, stopped, fatal;
} X4StreamAuStats;

/* Exactly one AU producer and one Decode consumer. Allocate before callbacks;
 * create pre-touches every4KiB span plus the final byte of arena/metadata.
 * This avoids allocation first-touch per AU, not later OS paging/scheduling.
 * Arena is exactly 8 MiB; descriptors own SPS/PPS copies separately. No heap
 * operation, sleep or wait-until-success occurs in any operation below.
 * Payload is contiguous (end padding is charged to resident bytes). */
X4StreamAuQueue *x4_stream_au_queue_create(void);
/* Borrowed input is copied before return. Positive drop/BUSY is NOT admission
 * and NOT a fatal decoder error. A valid fresh incoming IDR is admitted after
 * recovery retirement whenever a descriptor and contiguous arena span fit. */
X4StreamAuResult x4_stream_au_queue_try_push(X4StreamAuQueue *,
    const X4StreamAuInput *, uint64_t now_us, X4StreamAuOutcome *);
/* Lease pins all three view spans until release, including across native
 * Decode. Never evict/reuse INFLIGHT. One outstanding consumer lease maximum. */
X4StreamAuResult x4_stream_au_queue_try_take(X4StreamAuQueue *, uint64_t now_us,
    X4StreamAuLease *, X4StreamAuView *, X4StreamAuOutcome *);
X4StreamAuResult x4_stream_au_queue_release(X4StreamAuQueue *, X4StreamAuLease);
/* Explicit local omission; common PLI limiter belongs to the caller. */
X4StreamAuResult x4_stream_au_queue_require_idr(X4StreamAuQueue *,
    X4StreamAuLoss, X4StreamAuOutcome *);
/* Try-only snapshot; BUSY leaves *out untouched. Queue lag is the maximum of
 * local monotonic residence age and forward modular RTP90k content distance
 * (<2^31 ticks). Backward/half-span-ambiguous RTP uses residence age only;
 * a forward timestamp discontinuity is conservatively treated as lag. Histogram
 * samples are bounded observations at every push and at takes with live
 * entries; EMPTY polls do not sample. These are not per-frame latencies. */
X4StreamAuResult x4_stream_au_queue_try_snapshot(X4StreamAuQueue *, X4StreamAuStats *);
void x4_stream_au_queue_stop(X4StreamAuQueue *);
/* After stop/join only: inspect the same ownership condition as destroy,
 * without releasing storage. READY/DONE cancellation tombstones are safe. */
bool x4_stream_au_queue_quiescent(const X4StreamAuQueue *);
/* Stop and join producer/consumer first. False retains the allocation when a
 * WRITING/INFLIGHT lease or active operation remains; never races ownership. */
bool x4_stream_au_queue_destroy(X4StreamAuQueue *);

#ifdef X4_STREAM_AU_QUEUE_TEST_HOOKS
enum { X4_STREAM_AU_AFTER_RESERVE = 1, X4_STREAM_AU_BEFORE_PUBLISH = 2,
    X4_STREAM_AU_AFTER_TRY_GATE = 3, X4_STREAM_AU_BEFORE_READY_STORE = 4,
    X4_STREAM_AU_BEFORE_TAKE_CLAIM = 5 };
void x4_stream_au_queue_test_hook(X4StreamAuQueue *, unsigned event);
#endif
