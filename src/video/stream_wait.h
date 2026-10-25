/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#if X4_STREAM_WAIT_HOST
#include <semaphore.h>
#include <pthread.h>
typedef sem_t X4StreamWaitHandle;
typedef pthread_mutex_t X4StreamMutex;
#else
#include <orbis/libkernel.h>
typedef OrbisKernelSema X4StreamWaitHandle;
typedef OrbisPthreadMutex X4StreamMutex;
#endif

enum { X4_STREAM_WAIT_MAX_US=20000, X4_STREAM_WAIT_EVENT=0,
    X4_STREAM_WAIT_TIMEOUT=1, X4_STREAM_WAIT_CLOSED=2 };
typedef struct {
    uint64_t wait_calls,wakeups,timeouts,notifications,coalesced,errors;
    int first_error;
} X4StreamWaitStats;
/* One consumer per endpoint. This is a coalescing notification, never the
 * work queue/predicate. Producers publish the real atomic predicate first,
 * then notify without acquiring any mutex or waiting for the consumer.
 * A token posted before Wait is retained. A notify racing token consumption
 * may coalesce, but the consumer always rechecks its real predicate before
 * the next wait. The bounded timeout also services cancellation/deadlines. */
typedef struct {
    X4StreamWaitHandle handle;
    bool initialized;
    atomic_bool posted,closed;
    atomic_uint_fast64_t wait_calls,wakeups,timeouts,notifications,coalesced,errors;
    atomic_int first_error;
} X4StreamWait;
int x4_stream_wait_init(X4StreamWait *,const char *name);
int x4_stream_wait_notify(X4StreamWait *);
int x4_stream_wait_block(X4StreamWait *,unsigned timeout_us);
int x4_stream_wait_close(X4StreamWait *);
/* Destroy only after the unique waiter and all producers have joined. */
int x4_stream_wait_destroy(X4StreamWait *);
void x4_stream_wait_snapshot(const X4StreamWait *,X4StreamWaitStats *);
int x4_stream_mutex_init(X4StreamMutex *,const char *name);
int x4_stream_mutex_lock(X4StreamMutex *);
int x4_stream_mutex_trylock(X4StreamMutex *);
int x4_stream_mutex_unlock(X4StreamMutex *);
int x4_stream_mutex_destroy(X4StreamMutex *);
#if X4_STREAM_WAIT_HOST
/* Fault injection affects one future init only, not the production ABI. */
void x4_stream_wait_test_fail_init(int error);
/* Main-only fixture controls: fail the nth future destroy without releasing
 * that host object. Remaining objects stay real and must be cleaned up. */
void x4_stream_wait_test_fail_destroy(unsigned ordinal,int error);
void x4_stream_mutex_test_fail_destroy(unsigned ordinal,int error);
uint64_t x4_stream_wait_test_api_calls(void);
void x4_stream_wait_test_fail_notify(X4StreamWait *,int error);
void x4_stream_wait_test_fail_wait(X4StreamWait *,int error);
#endif
