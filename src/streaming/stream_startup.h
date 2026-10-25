/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

enum { X4_STREAM_INITIAL_RESOLUTION_TIMEOUT=-7433,
    X4_STREAM_INITIAL_RESOLUTION_WAIT_US=10000000,
    X4_STREAM_INITIAL_RESOLUTION_RETRY_US=300000 };
/* One object per pipeline/session, initialized before any producer starts.
 * The RTC callback publishes only the local alias-send result/timestamp.
 * The decode owner owns all plain fields and retires this initial policy.
 * Auth/main read only the atomic waiting flag. No optional trace is needed.
 * The pipeline outlives the closed RTC callbacks and joined auth worker. */
typedef struct X4StreamStartup {
    atomic_uint_fast64_t alias_sent_us;
    atomic_int alias_rc;
    atomic_bool alias_attempted,waiting;
    bool enabled,admitted,cancelled,timed_out,elapsed_available;
    uint64_t first_admitted_us,alias_to_sps_us,dropped_aus,last_request_us;
    unsigned last_width,last_height;
} X4StreamStartup;
void x4_stream_startup_init(X4StreamStartup *,bool enabled);
void x4_stream_startup_note_alias(X4StreamStartup *,int result,uint64_t local_return_us);
uint64_t x4_stream_startup_alias_sent_us(const X4StreamStartup *);
int x4_stream_startup_alias_result(const X4StreamStartup *,bool *attempted);
bool x4_stream_startup_waiting(const X4StreamStartup *);
/* Decode owner only. This clock is never reset by another oversized AU.
 * Before a successful alias send there is no invented resolution deadline. */
int x4_stream_startup_poll(X4StreamStartup *,uint64_t now_us,bool *request_keyframe);
bool x4_stream_startup_hold_size(X4StreamStartup *,unsigned coded_width,unsigned coded_height);
void x4_stream_startup_admit(X4StreamStartup *,uint64_t observed_us);
void x4_stream_startup_cancel(X4StreamStartup *);
