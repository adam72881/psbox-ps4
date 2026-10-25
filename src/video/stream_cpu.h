/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../streaming/stream_profile.h"

typedef enum { X4_CPU_ROLE_DECODE, X4_CPU_ROLE_CONVERTER, X4_CPU_ROLE_ASSEMBLY } X4StreamCpuRole;
enum { X4_CPU_NOT_RUN = -2147483647, X4_CPU_INVALID = -7461, X4_CPU_OBSERVATION = -7462 };
typedef struct {
    unsigned variant; int decoder_priority; uint64_t decoder_affinity;
} X4StreamCpuConfig;
typedef enum {
    X4_CPU_UNCHANGED, X4_CPU_APPLIED, X4_CPU_SKIPPED_CAPTURE,
    X4_CPU_RESTORED, X4_CPU_PARTIAL
} X4StreamCpuOutcome;
typedef struct {
    bool attempted,valid;
    int priority;uint64_t affinity;
    int priority_result,attr_init_result,attr_get_result,affinity_result,attr_destroy_result;
} X4StreamCpuSnapshot;
typedef struct {
    unsigned variant, role, index;
    int requested_priority, priority_result, affinity_result, result;
    uint64_t requested_affinity;
    bool priority_attempted, affinity_attempted;
    X4StreamCpuOutcome outcome;
    bool partial_applied;
    X4StreamCpuSnapshot before,after;
    bool restore_priority_attempted,restore_affinity_attempted;
    int restore_priority_result,restore_affinity_result;
} X4StreamCpuReport;
X4StreamCpuConfig x4_stream_cpu_config(unsigned variant);
/* Only CPU-2/3 change project-owned threads: Decode caller affinity only;
 * converter/assembly affinity and priority. Native worker priority remains
 * the cpuThreadPriority field of the shared Query/Create configuration.
 * Scheduling is best-effort: a nonzero return belongs to this report, never
 * to the video failure path. Callers continue after logging it. */
int x4_stream_cpu_apply_current_thread(unsigned variant, X4StreamCpuRole role,
    unsigned index, X4StreamCpuReport *report);
void x4_stream_cpu_write(FILE *, const X4StreamCpuReport *);
const char *x4_stream_cpu_outcome_name(X4StreamCpuOutcome);
