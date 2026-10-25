/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

enum {
    X4_KEYFRAME_INTERVAL_US = 500000,
    X4_KEYFRAME_WINDOW_US = 5000000,
    X4_KEYFRAME_WINDOW_LIMIT = 10,
    X4_KEYFRAME_ASSEMBLY = 1u << 0,
    X4_KEYFRAME_PIPELINE = 1u << 1,
    X4_KEYFRAME_STARTUP = 1u << 2,
    X4_KEYFRAME_AU_QUEUE = 1u << 3,
    X4_KEYFRAME_NO_MEDIA = 1u << 4,
    X4_KEYFRAME_LEGACY = 1u << 5,
    X4_KEYFRAME_ALL_REASONS = (1u << 6) - 1
};

typedef struct {
    uint64_t sequence, at_us, gap_generation, idr_generation;
    unsigned reasons;
    bool exception;
} X4KeyframeGrant;

typedef struct {
    uint64_t requested, coalesced, deferred, granted, retired_on_idr;
    uint64_t interval_deferred, cap_deferred, clock_deferred;
    uint64_t gap_generation, idr_generation, granted_gap_generation, last_granted_us;
    uint64_t transport_decisions;
    uint64_t control_attempts, control_local_successes, control_errors;
    uint64_t pli_attempts, pli_local_successes, pli_errors;
    unsigned pending_reasons;
    bool counter_overflow;
} X4KeyframeLimiterSnapshot;

typedef struct X4KeyframeLimiter {
    atomic_uint pending_reasons;
    atomic_uint_fast64_t requested, coalesced, deferred, granted, retired_on_idr;
    atomic_uint_fast64_t interval_deferred, cap_deferred, clock_deferred;
    atomic_uint_fast64_t gap_generation, idr_generation, granted_gap_generation, last_granted_us;
    atomic_uint_fast64_t transport_decisions;
    atomic_uint_fast64_t control_attempts, control_local_successes, control_errors;
    atomic_uint_fast64_t pli_attempts, pli_local_successes, pli_errors;
    atomic_bool counter_overflow;
    /* Auth/session owner only. Neither IDR nor loss resets this history. */
    uint64_t owner_times[X4_KEYFRAME_WINDOW_LIMIT];
    uint64_t owner_last_us, owner_gap_generation, owner_idr_generation;
    unsigned owner_head, owner_count;
    bool owner_has_grant;
    void (*observer)(void *,uint64_t);
    void *observer_user;
} X4KeyframeLimiter;

/* Initialize once before publishing the session to any producer. No heap,
 * clocks, native calls, mutexes, spinning, or callbacks are used here. */
void x4_keyframe_limiter_init(X4KeyframeLimiter *);

/* Init-only registration, before publishing any producer/owner thread. The
 * sole take owner invokes this after a successful reservation, before I/O.
 * Never called by request/observe producers or denied takes. The callback
 * must not call take/set_observer recursively. NULL unregisters at init. */
void x4_keyframe_limiter_set_observer(X4KeyframeLimiter *,
    void (*observer)(void *,uint64_t),void *user);

/* Any producer may post a nonzero subset of ALL_REASONS. One call is one
 * intent, even when its mask contains multiple reasons. A call finding an
 * already-pending mask is counted as coalesced. If an IDR races a fresh gap,
 * restoring/merging two pending batches also coalesces one of those batches.
 * Unknown bits reject the whole call. Posting/taking may use different threads. */
void x4_keyframe_limiter_request(X4KeyframeLimiter *, unsigned reasons);

/* Publish new evidence only: a real newly observed local omission/loss, or a
 * useful IDR. Repeated RESET_WAIT/non-IDR filtering is NOT a new gap. These
 * calls alone do not create an intent. Useful IDR atomically retires pending
 * old retry work, unless there is an unserved new loss (including a racing
 * gap). Publish observe_gap BEFORE its associated request. A request posted
 * after the IDR exchange survives. New evidence can authorize an early grant,
 * but never bypasses the rolling-window hard cap. */
void x4_keyframe_limiter_observe_gap(X4KeyframeLimiter *);
void x4_keyframe_limiter_observe_idr(X4KeyframeLimiter *);

/* Auth/session owner ONLY; now_us is a monotonic clock, including zero.
 * Denial retains all pending reasons. Success reserves one logical decision
 * BEFORE control/RTCP I/O; failed sends must not undo it. There is no rollback
 * API. At most ten grants occupy (now_us - 5s, now_us], across all reasons and
 * recovery episodes. A grant can dispatch both control and RTCP once each.
 * A regressing clock fails closed. No IDR/recovery deadline is changed. */
bool x4_keyframe_limiter_take(X4KeyframeLimiter *, uint64_t now_us,
    X4KeyframeGrant *);

/* RTC owner records the actual subattempts once per granted logical decision.
 * rc >= 0 is LOCAL success, never remote receipt or ACK. No attempted channel
 * means no transport decision is counted. This never refunds a reservation. */
void x4_keyframe_limiter_note_transport(X4KeyframeLimiter *,
    bool control_attempted, int control_rc, bool pli_attempted, int pli_rc);

/* Every field is read atomically; a concurrent snapshot is not a transaction
 * across fields. deferred counts take() calls denied WITH pending work, not
 * unique deferred intents. retired_on_idr counts removed pending batches, not
 * individual source intents. Counts are exact after the producers/owner stop;
 * counter_overflow explicitly invalidates exact cumulative counter claims. */
void x4_keyframe_limiter_snapshot(const X4KeyframeLimiter *,
    X4KeyframeLimiterSnapshot *);
