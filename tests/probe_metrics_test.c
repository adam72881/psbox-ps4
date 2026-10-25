/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/probe_metrics.h"
#include "../src/media/live_trace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; assert(expr); } while (0)
static X4ProbeMetricsSummary summary(X4ProbeMetrics *m, uint64_t at)
{
    X4ProbeMetricsSummary s; x4_probe_metrics_end(m, at, 0);
    CHECK(x4_probe_metrics_summary(m, &s) == 0); return s;
}
static void retained_distribution(void)
{
    X4ProbeMetrics *m = x4_probe_metrics_create(100);
    CHECK(m != NULL); CHECK(x4_probe_metrics_storage_bytes() <= 896 * 1024);
    X4ProbeMetricsSummary premature; CHECK(x4_probe_metrics_summary(m, &premature) < 0);
    x4_probe_metrics_record_duration(m, X4_PROBE_STAGE_DECODE, 1000, 100);
    x4_probe_metrics_note(m, X4_PROBE_COUNT_INPUT_AU, 60);
    CHECK(x4_probe_metrics_begin_measurement(m, 1000));
    for (unsigned i = 1; i <= 5000; ++i)
        x4_probe_metrics_record_duration(m, X4_PROBE_STAGE_DECODE, 20000 + i, i);
    X4ProbeMetricsSummary s = summary(m, 30000);
    X4ProbeStageSummary *d = &s.stages[X4_PROBE_STAGE_DECODE];
    CHECK(d->count == 5000); CHECK(d->retained == 4096); CHECK(d->overwritten == 904);
    CHECK(d->sum_us == 12502500); CHECK(d->maximum_us == 5000);
    CHECK(d->p50_us == 2952); CHECK(d->p95_us == 4796); CHECK(d->p99_us == 4960);
    CHECK(d->retained_maximum_us == 5000);
    CHECK(d->first_observation_us == 20001); CHECK(d->last_observation_us == 25000);
    uint64_t binned = 0; for (unsigned i = 0; i < X4_PROBE_METRIC_BINS; ++i) binned += d->bins[i];
    CHECK(binned == 5000); CHECK(s.total[X4_PROBE_COUNT_INPUT_AU] == 60);
    CHECK(s.measured[X4_PROBE_COUNT_INPUT_AU] == 0); CHECK(s.omitted_events == 0);
    x4_probe_metrics_free(m);
}
static void events_and_identities(void)
{
    X4ProbeMetrics *m = x4_probe_metrics_create(0); CHECK(m);
    x4_probe_metrics_record_event(m, 1, X4_TRACE_DECODE_BEGIN, 0, 42, 123);
    CHECK(x4_probe_metrics_begin_measurement(m, 10));
    x4_probe_metrics_record_event(m, 20, X4_TRACE_DECODE_END, 0, 42, 0);
    x4_probe_metrics_record_event(m, 100, X4_TRACE_DECODE_BEGIN, 0, 7, 321);
    x4_probe_metrics_record_event(m, 137, X4_TRACE_DECODE_END, 0, 7, 0);
    x4_probe_metrics_record_event(m, 140, X4_TRACE_OUTPUT_VALID, 0, 99, 5);
    x4_probe_metrics_record_event(m, 141, X4_TRACE_COPY_BEGIN, 0, 9, 99);
    x4_probe_metrics_record_event(m, 156, X4_TRACE_COPY_END, 0, 9, 15);
    x4_probe_metrics_record_event(m, 160, X4_TRACE_CONVERT_BEGIN, 0, 99, 0);
    x4_probe_metrics_record_event(m, 169, X4_TRACE_CONVERT_END, 0, 99, 9);
    x4_probe_metrics_record_event(m, 170, X4_TRACE_RGB_PUBLICATION, 0, 1000, 99);
    x4_probe_metrics_record_duration(m, X4_PROBE_STAGE_PUBLICATION, 174, 4);
    x4_probe_metrics_record_event(m, 180, X4_TRACE_DRAW_BEGIN, X4_TRACE_F_NEW, 1000, 99);
    x4_probe_metrics_record_event(m, 190, X4_TRACE_DRAW_END, X4_TRACE_F_NEW, 1000, 99);
    x4_probe_metrics_record_event(m, 200, X4_TRACE_FLIP_CALL_BEGIN, X4_TRACE_F_NEW, 1000, 12);
    x4_probe_metrics_record_event(m, 203, X4_TRACE_FLIP_SUBMIT, X4_TRACE_F_NEW, 1000, 12);
    x4_probe_metrics_record_event(m, 205, X4_TRACE_GNM_DONE, X4_TRACE_F_NEW, 1000, 0);
    x4_probe_metrics_record_event(m, 16672, X4_TRACE_FLIP_MATCH, X4_TRACE_F_NEW, 12, 55);
    x4_probe_metrics_record_event(m, 16672, X4_TRACE_PRESENT_NEW, X4_TRACE_F_NEW, 1000, 12);
    x4_probe_metrics_record_event(m, 18000, X4_TRACE_PRESENT_REPEAT, X4_TRACE_F_REPEAT, 1000, 12);
    x4_probe_metrics_record_event(m, 33339, X4_TRACE_PRESENT_NEW, X4_TRACE_F_NEW, 1001, 13);
    x4_probe_metrics_record_event(m, 34000, X4_TRACE_OUTPUT_SUPERSEDED, 0, 99, 100);
    x4_probe_metrics_record_event(m, 34001, X4_TRACE_RGB_SUPERSEDED, 0, 1001, 1002);
    x4_probe_metrics_note(m, X4_PROBE_COUNT_EOF, 1);
    x4_probe_metrics_backlog(m, 3, 21000);
    X4ProbeMetricsSummary s = summary(m, 35000);
    const uint64_t expected[] = {37, 15, 9, 4, 10, 3, 16467, 16667};
    const uint64_t endpoints[] = {137, 156, 169, 174, 190, 203, 16672, 33339};
    for (unsigned i = 0; i < sizeof(expected)/sizeof(expected[0]); ++i) {
        CHECK(s.stages[i].count == 1); CHECK(s.stages[i].p50_us == expected[i]);
        CHECK(s.stages[i].first_observation_us == endpoints[i]);
        CHECK(s.stages[i].last_observation_us == endpoints[i]);
    }
    CHECK(s.measured[X4_PROBE_COUNT_NATIVE_OUTPUT] == 1);
    CHECK(s.measured[X4_PROBE_COUNT_RGB_PUBLICATION] == 1);
    CHECK(s.measured[X4_PROBE_COUNT_RGB_CONVERTED] == 1);
    CHECK(s.measured[X4_PROBE_COUNT_NEW_PRESENT] == 2);
    CHECK(s.measured[X4_PROBE_COUNT_REPEAT_PRESENT] == 1);
    CHECK(s.measured[X4_PROBE_COUNT_OUTPUT_SUPERSEDE] == 1);
    CHECK(s.measured[X4_PROBE_COUNT_RGB_SUPERSEDE] == 1);
    CHECK(s.measured[X4_PROBE_COUNT_RGB_PREPUBLICATION_SUPERSEDE] == 0);
    CHECK(s.timestamped_count[X4_PROBE_COUNT_NATIVE_OUTPUT] == 1);
    CHECK(s.measured_first_us[X4_PROBE_COUNT_NATIVE_OUTPUT] == 140);
    CHECK(s.measured_last_us[X4_PROBE_COUNT_NATIVE_OUTPUT] == 140);
    CHECK(s.timestamped_count[X4_PROBE_COUNT_NEW_PRESENT] == 2);
    CHECK(s.measured_first_us[X4_PROBE_COUNT_NEW_PRESENT] == 16672);
    CHECK(s.measured_last_us[X4_PROBE_COUNT_NEW_PRESENT] == 33339);
    CHECK(s.timestamped_count[X4_PROBE_COUNT_EOF] == 0);
    CHECK(s.measured_first_us[X4_PROBE_COUNT_EOF] == 0);
    CHECK(s.measured_last_us[X4_PROBE_COUNT_EOF] == 0);
    CHECK(s.peak_backlog == 3); CHECK(s.maximum_backlog_age_us == 21000);
    CHECK(s.stages[X4_PROBE_STAGE_DECODE].omitted == 1);
    x4_probe_metrics_free(m);
}
static void omitted_clock_capacity_and_close(const char *json_path)
{
    X4ProbeMetrics *m = x4_probe_metrics_create(100); CHECK(m);
    CHECK(!x4_probe_metrics_begin_measurement(m, 99));
    CHECK(x4_probe_metrics_begin_measurement(m, 100));
    x4_probe_metrics_record_event(m, 200, X4_TRACE_DECODE_BEGIN, 0, 1, 0);
    x4_probe_metrics_test_gate(m, true);
    x4_probe_metrics_record_event(m, 201, X4_TRACE_DECODE_END, 0, 1, 0);
    x4_probe_metrics_test_gate(m, false);
    x4_probe_metrics_record_event(m, 202, X4_TRACE_DECODE_END, 0, 1, 0);
    for (unsigned i = 1; i <= 33; ++i)
        x4_probe_metrics_record_event(m, 300 + i, X4_TRACE_COPY_BEGIN, 0, i, 0);
    x4_probe_metrics_record_event(m, 500, X4_TRACE_COPY_END, 0, 33, 0);
    x4_probe_metrics_record_event(m, 600, X4_TRACE_DECODE_BEGIN, 0, 2, 0);
    x4_probe_metrics_record_event(m, 599, X4_TRACE_DECODE_END, 0, 2, 0);
    x4_probe_metrics_record_event(m, 601, X4_TRACE_PRESENT_NEW, 0, 1, 1);
    x4_probe_metrics_record_event(m, 600, X4_TRACE_PRESENT_NEW, 0, 2, 2);
    x4_probe_metrics_record_duration(m, X4_PROBE_STAGE_PUBLICATION, 120, 30);
    x4_probe_metrics_note(m, X4_PROBE_COUNT_CANCEL, 1);
    X4ProbeMetricsSummary s = summary(m, 1000);
    CHECK(s.omitted_events == 1); CHECK(s.dirty_resets == 1);
    CHECK(s.stages[X4_PROBE_STAGE_DECODE].count == 0);
    CHECK(s.total[X4_PROBE_COUNT_PAIR_CAPACITY] == 1);
    CHECK(s.stages[X4_PROBE_STAGE_COPY].omitted == 2);
    CHECK(s.total[X4_PROBE_COUNT_CLOCK_REJECT] == 3);
    CHECK(s.total[X4_PROBE_COUNT_PHASE_ABANDONED] == 1);
    CHECK(s.measured[X4_PROBE_COUNT_CANCEL] == 1);
    CHECK(s.stages[X4_PROBE_STAGE_PUBLICATION].count == 0);
    CHECK(x4_probe_metrics_dump_file(m, json_path) == 0);
    /* A fresh process may reuse a timestamped path. The second dump must
     * refuse creation and preserve the first fixture for the JSON reader. */
    CHECK(x4_probe_metrics_dump_file(m, json_path) == -3);
    x4_probe_metrics_note(m, X4_PROBE_COUNT_EOF, 1);
    CHECK(x4_probe_metrics_summary(m, &s) == 0);
    CHECK(s.events_after_end == 1); CHECK(s.total[X4_PROBE_COUNT_EOF] == 0);
    CHECK(strcmp(x4_probe_metrics_stage_name(X4_PROBE_STAGE_DECODE), "decode_call") == 0);
    CHECK(strcmp(x4_probe_metrics_counter_name(X4_PROBE_COUNT_INPUT_LATE), "input_late") == 0);
    CHECK(strcmp(x4_probe_metrics_stage_name((X4ProbeStage)100), "unknown") == 0);
    x4_probe_metrics_free(m);
}
typedef struct { X4ProbeMetrics *metrics; } ThreadArgs;
static void *stress_thread(void *data)
{
    ThreadArgs *a = data;
    for (unsigned i = 0; i < 10000; ++i)
        x4_probe_metrics_note(a->metrics, X4_PROBE_COUNT_INPUT_AU, 1);
    return NULL;
}
static void concurrent_observations(void)
{
    X4ProbeMetrics *m = x4_probe_metrics_create(0); CHECK(m);
    CHECK(x4_probe_metrics_begin_measurement(m, 1));
    ThreadArgs args = {.metrics = m}; pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) CHECK(pthread_create(&threads[i], NULL, stress_thread, &args) == 0);
    for (unsigned i = 0; i < 4; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    X4ProbeMetricsSummary s = summary(m, 100000);
    CHECK(s.total[X4_PROBE_COUNT_INPUT_AU] + s.omitted_events == 40000);
    x4_probe_metrics_free(m);
}
static void input_attributes(void)
{
    X4ProbeMetrics *m = x4_probe_metrics_create(0); CHECK(m);
    CHECK(x4_probe_metrics_begin_measurement(m, 1000));
    /* An input's nominal due time can predate stable while its observed
     * delivery is stable. Preserve lateness rather than hiding startup backlog. */
    x4_probe_metrics_record_duration(m, X4_PROBE_STAGE_INPUT_LATENESS, 1500, 1200);
    x4_probe_metrics_record_duration(m, X4_PROBE_STAGE_INPUT_INTERVAL, 1500, 1200);
    x4_probe_metrics_record_duration(m, X4_PROBE_STAGE_INPUT_INTERVAL, 2000, 500);
    X4ProbeMetricsSummary s = summary(m, 3000);
    CHECK(s.stages[X4_PROBE_STAGE_INPUT_LATENESS].count == 1);
    CHECK(s.stages[X4_PROBE_STAGE_INPUT_LATENESS].p99_us == 1200);
    CHECK(s.stages[X4_PROBE_STAGE_INPUT_INTERVAL].count == 1);
    CHECK(s.stages[X4_PROBE_STAGE_INPUT_INTERVAL].p99_us == 500);
    CHECK(s.stages[X4_PROBE_STAGE_INPUT_INTERVAL].omitted == 1);
    CHECK(s.total[X4_PROBE_COUNT_PHASE_ABANDONED] == 1);
    CHECK(s.total[X4_PROBE_COUNT_CLOCK_REJECT] == 0);
    x4_probe_metrics_free(m);
}
static void timestamped_observations(const char *json_path)
{
    X4ProbeMetrics *m = x4_probe_metrics_create(0); CHECK(m);
    x4_probe_metrics_note_at(m, 900, X4_PROBE_COUNT_INPUT_AU, 60);
    CHECK(x4_probe_metrics_begin_measurement(m, 1000));
    /* A genuinely observed timestamp from before the boundary is omitted
     * from stable counting. Untimed notes disclose no denominator. */
    x4_probe_metrics_note_at(m, 900, X4_PROBE_COUNT_INPUT_AU, 1);
    x4_probe_metrics_note(m, X4_PROBE_COUNT_INPUT_AU, 2);
    x4_probe_metrics_note_at(m, 1500, X4_PROBE_COUNT_INPUT_AU, 1);
    x4_probe_metrics_note_at(m, 2500, X4_PROBE_COUNT_INPUT_AU, 1);
    x4_probe_metrics_note_at(m, 2000, X4_PROBE_COUNT_INPUT_AU, 2);
    x4_probe_metrics_note_at(m, 4000, X4_PROBE_COUNT_INPUT_AU, 0);
    x4_probe_metrics_note(m, X4_PROBE_COUNT_EOF, 1);
    x4_probe_metrics_note_at(m, 4500, X4_PROBE_COUNT_BACKPRESSURE, 3);
    x4_probe_metrics_record_duration(m, X4_PROBE_STAGE_COPY, 2600, 20);
    x4_probe_metrics_record_duration(m, X4_PROBE_STAGE_COPY, 2100, 10);
    x4_probe_metrics_record_duration(m, X4_PROBE_STAGE_CONVERT_TO_SUBMIT, 3000, 5);
    x4_probe_metrics_record_duration(m, X4_PROBE_STAGE_CONVERT_TO_SUBMIT, 3100, 9);
    X4ProbeMetricsSummary s = summary(m, 5000);
    CHECK(s.total[X4_PROBE_COUNT_INPUT_AU] == 67);
    CHECK(s.measured[X4_PROBE_COUNT_INPUT_AU] == 6);
    CHECK(s.timestamped_count[X4_PROBE_COUNT_INPUT_AU] == 4);
    CHECK(s.timestamp_omissions[X4_PROBE_COUNT_INPUT_AU] == 1);
    CHECK(s.measured_first_us[X4_PROBE_COUNT_INPUT_AU] == 1500);
    CHECK(s.measured_last_us[X4_PROBE_COUNT_INPUT_AU] == 2500);
    CHECK(s.measured[X4_PROBE_COUNT_EOF] == 1);
    CHECK(s.timestamped_count[X4_PROBE_COUNT_EOF] == 0);
    CHECK(s.measured_first_us[X4_PROBE_COUNT_EOF] == 0);
    CHECK(s.measured_last_us[X4_PROBE_COUNT_EOF] == 0);
    CHECK(s.timestamped_count[X4_PROBE_COUNT_BACKPRESSURE] == 3);
    CHECK(s.measured_first_us[X4_PROBE_COUNT_BACKPRESSURE] == 4500);
    CHECK(s.measured_last_us[X4_PROBE_COUNT_BACKPRESSURE] == 4500);
    CHECK(s.stages[X4_PROBE_STAGE_COPY].first_observation_us == 2100);
    CHECK(s.stages[X4_PROBE_STAGE_COPY].last_observation_us == 2600);
    CHECK(s.stages[X4_PROBE_STAGE_CONVERT_TO_SUBMIT].count == 2);
    CHECK(s.stages[X4_PROBE_STAGE_CONVERT_TO_SUBMIT].sum_us == 14);
    CHECK(s.stages[X4_PROBE_STAGE_CONVERT_TO_SUBMIT].p50_us == 5);
    CHECK(s.stages[X4_PROBE_STAGE_CONVERT_TO_SUBMIT].p95_us == 9);
    CHECK(s.stages[X4_PROBE_STAGE_CONVERT_TO_SUBMIT].p99_us == 9);
    CHECK(s.stages[X4_PROBE_STAGE_CONVERT_TO_SUBMIT].first_observation_us == 3000);
    CHECK(s.stages[X4_PROBE_STAGE_CONVERT_TO_SUBMIT].last_observation_us == 3100);
    CHECK(s.stages[X4_PROBE_STAGE_DECODE].first_observation_us == 0);
    CHECK(s.stages[X4_PROBE_STAGE_DECODE].last_observation_us == 0);
    char path[4096]; int length = snprintf(path, sizeof(path), "%s.timestamps", json_path);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    CHECK(x4_probe_metrics_dump_file(m, path) == 0);
    x4_probe_metrics_free(m);
}
static void stack_reports(const char *json_path)
{
    X4ProbeMetrics *m = x4_probe_metrics_create(1); CHECK(m);
    CHECK(!x4_probe_metrics_stack(m, 0));
    CHECK(!x4_probe_metrics_stack(NULL, 0));
    X4ProbeStackReport r = {.configured_bytes = 262144, .reported_bytes = 262144,
        .guard_bytes = 4096, .painted_bytes = 245760, .untouched_bytes = 221184,
        .high_water_bytes = 36864, .low_excluded_bytes = 4096, .top_excluded_bytes = 12288,
        .entry_used_bytes = 8192,
        .started = true, .finished = true, .pattern_ready = true, .pattern_touched = true};
    snprintf(r.name, sizeof(r.name), "%s", "x4-video");
    for (unsigned slot = 0; slot < 4; ++slot) x4_probe_metrics_record_stack(m, slot, &r);
    r.reported_bytes = 0; /* Owner copied the complete report, not a pointer. */
    CHECK(x4_probe_metrics_stack(m, 0)->reported_bytes == 262144);
    x4_probe_metrics_record_stack(m, 4, &r);
    x4_probe_metrics_record_stack(m, 0, NULL);
    x4_probe_metrics_record_stack(NULL, 0, &r);
    CHECK(!x4_probe_metrics_stack(m, 4));
    r.create_rc = -17; r.started = r.finished = r.pattern_ready = false;
    r.high_water_bytes = r.painted_bytes = r.untouched_bytes = 0;
    snprintf(r.name, sizeof(r.name), "%s", "x4-\"copy");
    x4_probe_metrics_record_stack(m, 3, &r);
    x4_probe_metrics_end(m, 2, 3);
    CHECK(x4_probe_metrics_stack(m, 3)->create_rc == -17);
    char path[4096]; int n = snprintf(path, sizeof(path), "%s.stacks", json_path);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    CHECK(x4_probe_metrics_dump_file(m, path) == 0);
    x4_probe_metrics_free(m);
}
static void independent_copy_stages(void)
{
    X4ProbeMetrics *m = x4_probe_metrics_create(1); CHECK(m);
    CHECK(x4_probe_metrics_begin_measurement(m, 2));
    for (unsigned i = 0; i < 6; ++i) {
        X4ProbeStage stage = (X4ProbeStage)(X4_PROBE_STAGE_COPY_OWNER_LOOP + i);
        x4_probe_metrics_record_duration(m, stage, 100 + i, 10 + i);
        x4_probe_metrics_record_duration(m, stage, 200 + i, 20 + i);
    }
    x4_probe_metrics_end(m, 300, 1);
    X4ProbeMetricsSummary *s = x4_probe_metrics_summary_storage(m);
    CHECK(!x4_probe_metrics_summary(m, s));
    for (unsigned i = 0; i < 6; ++i) {
        const X4ProbeStageSummary *stage = &s->stages[X4_PROBE_STAGE_COPY_OWNER_LOOP + i];
        CHECK(stage->count == 2 && stage->sum_us == 30 + 2 * i);
        CHECK(stage->p50_us == 10 + i && stage->p95_us == 20 + i);
    }
    CHECK(s->stages[X4_PROBE_STAGE_COPY].count == 0); /* Never synthesise total copy wall. */
    x4_probe_metrics_free(m);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    X4ProbeMetrics *scratch_owner = x4_probe_metrics_create(1);
    CHECK(scratch_owner);
    X4ProbeMetricsSummary *scratch = x4_probe_metrics_summary_storage(scratch_owner);
    CHECK(scratch && scratch == x4_probe_metrics_summary_storage(scratch_owner));
    CHECK(!x4_probe_metrics_summary_storage(NULL));
    x4_probe_metrics_end(scratch_owner, 2, 0);
    CHECK(!x4_probe_metrics_summary(scratch_owner, scratch));
    CHECK(scratch->created_us == 1 && scratch->ended_us == 2);
    CHECK(!x4_probe_metrics_summary(scratch_owner, scratch));
    CHECK(scratch == x4_probe_metrics_summary_storage(scratch_owner));
    x4_probe_metrics_free(scratch_owner);
    retained_distribution(); events_and_identities(); omitted_clock_capacity_and_close(argv[1]);
    concurrent_observations(); input_attributes(); timestamped_observations(argv[1]); stack_reports(argv[1]);
    independent_copy_stages();
    printf("{\"result\":\"PASS\",\"checks\":%u,\"allocated_bytes\":%zu}\n", checks, x4_probe_metrics_storage_bytes());
    return 0;
}
