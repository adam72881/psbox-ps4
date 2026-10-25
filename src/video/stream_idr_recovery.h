/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { X4_STREAM_IDR_RETRY_US = 300000, X4_STREAM_IDR_TIMEOUT_US = 5000000,
    X4_STREAM_IDR_WAIT_TIMEOUT = -7432 };
typedef struct { bool waiting; uint64_t begin_us, deadline_us, next_request_us, waits, recoveries, timeouts; } X4StreamIdrRecovery;
/* Decode owner and monotonic clock only. Starting an existing wait never
 * extends its absolute deadline, including repeated malformed input. */
static inline void x4_stream_idr_begin(X4StreamIdrRecovery *r,uint64_t now)
{
    if(r->waiting)return;
    r->waiting=true;r->begin_us=now;r->deadline_us=now+X4_STREAM_IDR_TIMEOUT_US;
    r->next_request_us=now;++r->waits;
}
static inline int x4_stream_idr_poll(X4StreamIdrRecovery *r,uint64_t now,bool *request)
{
    *request=false;if(!r->waiting)return 0;
    if(now>=r->deadline_us){++r->timeouts;return X4_STREAM_IDR_WAIT_TIMEOUT;}
    if(now>=r->next_request_us){*request=true;r->next_request_us=now+X4_STREAM_IDR_RETRY_US;}
    return 0;
}
static inline void x4_stream_idr_validated(X4StreamIdrRecovery *r)
{if(r->waiting){r->waiting=false;++r->recoveries;}}
