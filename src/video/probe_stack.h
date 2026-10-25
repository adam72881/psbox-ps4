/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { X4_PROBE_THREAD_STACK_BYTES = 256 * 1024,
    X4_PROBE_STACK_LOW_RESERVE = 16 * 1024,
    X4_PROBE_STACK_TOP_RESERVE = 4096,
    X4_PROBE_STACK_NOT_RUN = -2147483647,
    X4_PROBE_STACK_LAYOUT = -7301, X4_PROBE_STACK_TIMEOUT = -7302,
    X4_PROBE_STACK_RETAINED = -7303 };

typedef struct {
    char name[24];
    uint64_t configured_bytes, reported_bytes, guard_bytes, painted_bytes;
    uint64_t untouched_bytes, high_water_bytes, low_excluded_bytes, top_excluded_bytes, entry_used_bytes;
    int attr_init_rc, set_size_rc, create_rc, attr_destroy_rc;
    int getattr_init_rc, getattr_rc, getstacksize_rc, getstack_rc, getguardsize_rc;
    int query_destroy_rc, measure_rc, join_rc, wrapper_result;
    bool started, finished, pattern_ready, pattern_touched;
} X4ProbeStackReport;

#if (X4_VIDEO_PROBE || X4_STREAM_PROFILE) && (defined(X4_OPENORBIS) || X4_PROBE_STACK_HOST)
/* Report readers and metrics-only host tests need no native kernel types. */
#include <stdatomic.h>
#include <orbis/libkernel.h>
/* Zero-initialize before the first create. Owned by the case and retained
 * until the actual native thread is joined. No concurrent creators. */
typedef struct {
    X4ProbeStackReport report;
    atomic_int command, startup_result;
    atomic_bool ready;
    void *(*entry)(void *);
    void *argument;
    uintptr_t stack_low, stack_high, paint_low, paint_high;
    bool created;
} X4ProbeThread;
int x4_probe_thread_create(OrbisPthread *, X4ProbeThread *, void *(*)(void *), void *, const char *);
void x4_probe_thread_joined(X4ProbeThread *, int result);
void x4_probe_stack_query_current(X4ProbeStackReport *);
/* Single UI owner. No decoder. Persistent context retained on failed join. */
int x4_probe_stack_check(X4ProbeStackReport *main_report, X4ProbeStackReport *test_report);
/* Informational observation of a real NULL-attributes thread. No stack paint
 * and no fallback from the explicit-stack path. A failed join preserves the
 * static context; retained=true requires the UI to avoid starting a case or
 * exiting as if cleanup succeeded. A subsequent call retries only that join.
 * Creation/query failures with retained=false are informational, not a gate. */
int x4_probe_default_stack_check(X4ProbeStackReport *report, bool *retained);
#if X4_PROBE_STACK_HOST
void x4_probe_stack_test_fail(unsigned step, int result);
#endif
#endif
