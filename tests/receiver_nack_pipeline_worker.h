/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "rtc_receive_diag.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct X4Pipeline X4Pipeline;
typedef struct {
    uint64_t time, identity, source, a, b, c;
    unsigned kind, event, stage, flags;
} X4PipelineEvent;
typedef struct {
    uint64_t pending, admitted, accepted, stale_source, duplicate, held;
    uint64_t au_submitted, feed_calls, dropped_frames, recovery_episodes;
    uint64_t queue_depth, inbox_admitted, inbox_omitted, inbox_stale, event_omitted;
    uint32_t epoch, expected, buffered;
    bool locked, waiting_keyframe;
    /* Actual worker-owned sparse storage/identity observations. No mutation,
     * inferred authentication, output PTS or native picture association. */
    uint32_t resident_bytes, max_forward, discontinuity_rebases;
    uint64_t direct_packets, capacity_ranges, distance_ranges;
    uint64_t canonical_offset, reorder_ticket_omissions;
    uint64_t direct_budget_drops, lost_packets, dropped_packets, worker_decode_calls;
    unsigned budget_reason;
    bool worker_stopped;
} X4PipelineSnapshot;
uint64_t x4_pipeline_clock(void); /* Explicit test clock, supplied by C++ owner. */
X4Pipeline *x4_pipeline_create(bool grace, uint32_t transport_epoch);
void x4_pipeline_receive(X4Pipeline *, const void *, size_t, const X4NackPacket *);
void x4_pipeline_batch(X4Pipeline *);
/* Explicit functional control fixture, through the actual bounded inbox. */
bool x4_pipeline_control(const X4NackGraceControl *);
unsigned x4_pipeline_reorder_code(bool late);
unsigned x4_pipeline_reorder_direct_code(void);
unsigned x4_pipeline_reorder_full_code(void);
unsigned x4_pipeline_reorder_insert_event(void);
unsigned x4_pipeline_reorder_emit_event(void);
enum { X4_PIPELINE_DIRECT_HOOK_TIME = 1, X4_PIPELINE_DIRECT_HOOK_DECODE,
    X4_PIPELINE_DIRECT_HOOK_STOP };
/* Host-only scheduling doubles run immediately after the actual DIRECT
 * admission event and before consume. No production hook is installed. */
void x4_pipeline_arm_direct_hook(X4Pipeline *, unsigned kind, uint64_t value);
unsigned x4_pipeline_direct_hook_count(void);
unsigned x4_pipeline_budget_reason(unsigned hook_kind);
unsigned x4_pipeline_direct_budget_event(void);
void x4_pipeline_set_clock(uint64_t value); /* Supplied by the C++ test clock. */
X4PipelineSnapshot x4_pipeline_snapshot(const X4Pipeline *);
/* Test-only bounded copy of a resident physical slot and its exact ticket.
 * A missing/previously emitted packet returns false; it is never synthesized. */
bool x4_pipeline_stored(const X4Pipeline *, uint16_t sequence,
    void *packet, size_t capacity, size_t *size, X4NackPacket *ticket,
    uint64_t *worker_store_us);
size_t x4_pipeline_event_count(void);
bool x4_pipeline_event(size_t, X4PipelineEvent *);
size_t x4_pipeline_feed_bytes(void *, size_t);
void x4_pipeline_close(X4Pipeline *);
#ifdef __cplusplus
}
#endif
