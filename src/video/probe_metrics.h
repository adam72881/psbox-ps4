/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
#include <stdio.h>
#include "probe_stack.h"
#endif

enum { X4_PROBE_METRIC_SAMPLES = 4096, X4_PROBE_METRIC_BINS = 32 };
typedef enum {
    X4_PROBE_STAGE_DECODE, X4_PROBE_STAGE_COPY, X4_PROBE_STAGE_CONVERT,
    X4_PROBE_STAGE_PUBLICATION, X4_PROBE_STAGE_DRAW, X4_PROBE_STAGE_FLIP_CALL,
    X4_PROBE_STAGE_VSYNC_WAIT, X4_PROBE_STAGE_NEW_PRESENT_INTERVAL,
    X4_PROBE_STAGE_INPUT_INTERVAL, X4_PROBE_STAGE_INPUT_LATENESS,
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
    X4_PROBE_STAGE_COPY_OWNER_LOOP, X4_PROBE_STAGE_COPY_HELPER0_LOOP,
    X4_PROBE_STAGE_COPY_HELPER1_LOOP, X4_PROBE_STAGE_COPY_HELPER2_LOOP,
    X4_PROBE_STAGE_COPY_READERS_MAX_LOOP, X4_PROBE_STAGE_COPY_COMPLETION_WAIT,
    X4_PROBE_STAGE_FREE_OUTPUT_WAIT, X4_PROBE_STAGE_STRIPE0,
    X4_PROBE_STAGE_STRIPE1, X4_PROBE_STAGE_STRIPE2,
    X4_PROBE_STAGE_DECODE_TO_FLIP,
    X4_PROBE_STAGE_PIPELINE_FLIP_OBSERVATION,
    X4_PROBE_STAGE_CONVERT_TO_SUBMIT,
#endif
    X4_PROBE_STAGE_COUNT
} X4ProbeStage;
typedef enum {
    X4_PROBE_COUNT_INPUT_AU, X4_PROBE_COUNT_NATIVE_OUTPUT,
    X4_PROBE_COUNT_RGB_PUBLICATION, X4_PROBE_COUNT_RGB_CONVERTED,
    X4_PROBE_COUNT_RGB_PREPUBLICATION_SUPERSEDE, X4_PROBE_COUNT_NEW_PRESENT,
    X4_PROBE_COUNT_REPEAT_PRESENT, X4_PROBE_COUNT_OUTPUT_SUPERSEDE,
    X4_PROBE_COUNT_RGB_SUPERSEDE, X4_PROBE_COUNT_ERRORS,
    X4_PROBE_COUNT_EOF, X4_PROBE_COUNT_CANCEL, X4_PROBE_COUNT_BACKPRESSURE,
    X4_PROBE_COUNT_INPUT_LATE,
    X4_PROBE_COUNT_UNPAIRED, X4_PROBE_COUNT_CLOCK_REJECT,
    X4_PROBE_COUNT_PHASE_ABANDONED, X4_PROBE_COUNT_PAIR_CAPACITY,
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
    X4_PROBE_COUNT_LATENCY_SUPERSEDE,
#endif
    X4_PROBE_COUNT_COUNT
} X4ProbeCounter;
typedef struct X4ProbeMetrics X4ProbeMetrics;
typedef struct {
    uint64_t count, retained, overwritten, omitted, sum_us, maximum_us;
    uint64_t p50_us, p95_us, p99_us, retained_maximum_us;
    uint64_t first_observation_us, last_observation_us;
    uint64_t bins[X4_PROBE_METRIC_BINS];
} X4ProbeStageSummary;
typedef struct {
    uint64_t created_us, measure_start_us, ended_us, omitted_events;
    uint64_t dirty_resets, events_after_end, peak_backlog, maximum_backlog_age_us;
    uint64_t total[X4_PROBE_COUNT_COUNT], measured[X4_PROBE_COUNT_COUNT];
    uint64_t measured_first_us[X4_PROBE_COUNT_COUNT], measured_last_us[X4_PROBE_COUNT_COUNT];
    uint64_t timestamped_count[X4_PROBE_COUNT_COUNT], timestamp_omissions[X4_PROBE_COUNT_COUNT];
    bool measuring, ended;
    unsigned end_reason;
    size_t allocated_bytes;
    X4ProbeStageSummary stages[X4_PROBE_STAGE_COUNT];
} X4ProbeMetricsSummary;

/* Allocate once before producers. No call below allocates or writes a file
 * during recording. Caller retains storage until every producer has joined. */
X4ProbeMetrics *x4_probe_metrics_create(uint64_t created_us);
void x4_probe_metrics_free(X4ProbeMetrics *);
/* Explicit warmup boundary, chosen by the local-probe owner. Returns false
 * on try-gate contention; caller may try at the next ordinary owner boundary.
 * Crossing spans are retired rather than misclassified as stable durations. */
bool x4_probe_metrics_begin_measurement(X4ProbeMetrics *, uint64_t now_us);
/* Hook the production numeric trace BEFORE its independent retention gate.
 * Decode/output and AU identities stay separate: no output PTS is invented. */
void x4_probe_metrics_record_event(X4ProbeMetrics *, uint64_t now_us,
    uint16_t event, uint16_t flags, uint64_t a, uint64_t b);
/* Direct duration for observed work without a trace begin/end pair (e.g. the
 * actual mailbox publication operation). Never add overlapping helper spans. */
void x4_probe_metrics_record_duration(X4ProbeMetrics *, X4ProbeStage,
    uint64_t now_us, uint64_t duration_us);
void x4_probe_metrics_note(X4ProbeMetrics *, X4ProbeCounter, uint64_t value);
/* Real observed timestamp only. Untimed note() does not invent a clock.
 * Aggregated values share one observed notification time, not individual PTS. */
void x4_probe_metrics_note_at(X4ProbeMetrics *, uint64_t now_us, X4ProbeCounter, uint64_t value);
void x4_probe_metrics_backlog(X4ProbeMetrics *, uint64_t depth, uint64_t age_us);
void x4_probe_metrics_end(X4ProbeMetrics *, uint64_t now_us, unsigned reason);
/* Summary/dump only AFTER all recording producers are quiescent. Temporary
 * sort scratch is bounded to 4096 uint64_t, allocated only during summary. */
int x4_probe_metrics_summary(const X4ProbeMetrics *, X4ProbeMetricsSummary *);
/* Case-owned heap scratch, reserved with metrics_create and released with
 * metrics_free. Exclusive UI/export use AFTER producer quiescence only. */
X4ProbeMetricsSummary *x4_probe_metrics_summary_storage(const X4ProbeMetrics *);
int x4_probe_metrics_dump_file(const X4ProbeMetrics *, const char *path);
const char *x4_probe_metrics_stage_name(X4ProbeStage);
const char *x4_probe_metrics_counter_name(X4ProbeCounter);
size_t x4_probe_metrics_storage_bytes(void);
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
/* Four independent, case-owned slots: video owner, copy helpers 0..2.
 * A slot has one writer, after its thread has stopped. Read/export only after
 * all writers have joined (or a failed creation proved there is no writer).
 * This does not take the recording gate or allocate on a worker stack. */
void x4_probe_metrics_record_stack(X4ProbeMetrics *, unsigned slot, const X4ProbeStackReport *);
const X4ProbeStackReport *x4_probe_metrics_stack(const X4ProbeMetrics *, unsigned slot);
void x4_probe_stack_report_json(FILE *, const X4ProbeStackReport *);
void x4_probe_metrics_stacks_json(FILE *, const X4ProbeMetrics *);
#endif
#ifdef X4_PROBE_METRICS_TEST
void x4_probe_metrics_test_gate(X4ProbeMetrics *, bool held);
#endif
