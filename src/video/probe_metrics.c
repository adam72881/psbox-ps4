/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "probe_metrics.h"
#include "probe_report_file.h"
#include "../media/live_trace.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAIRS = 32 };
typedef struct { uint64_t id, begin; bool used; } Pair;
typedef struct {
    uint64_t samples[X4_PROBE_METRIC_SAMPLES], bins[X4_PROBE_METRIC_BINS];
    uint64_t count, sum, maximum, overwritten, omitted;
    uint64_t first_observation, last_observation;
    unsigned next, retained;
    Pair pairs[PAIRS];
} Stage;
struct X4ProbeMetrics {
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
    /* Export/compatibility scratch belongs to the existing heap context.
     * Used only after producers stop; never a large automatic UI frame. */
    X4ProbeMetricsSummary summary_scratch;
    X4ProbeMetricsSummary *summary_storage;
    X4ProbeStackReport stack[4];
    bool stack_recorded[4];
#endif
    atomic_flag gate;
    atomic_bool dirty;
    atomic_uint_fast64_t omitted_events, events_after_end;
    Stage stage[X4_PROBE_STAGE_COUNT];
    uint64_t total[X4_PROBE_COUNT_COUNT], measured[X4_PROBE_COUNT_COUNT];
    uint64_t measured_first[X4_PROBE_COUNT_COUNT], measured_last[X4_PROBE_COUNT_COUNT];
    uint64_t timestamped_count[X4_PROBE_COUNT_COUNT], timestamp_omissions[X4_PROBE_COUNT_COUNT];
    uint64_t created, measure_start, ended_at, dirty_resets;
    uint64_t peak_backlog, max_backlog_age, pending_flip, last_new_present;
    bool pending_flip_valid, last_new_valid, measuring, ended;
    unsigned end_reason;
};
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
_Static_assert(sizeof(X4ProbeMetrics) <= 896 * 1024, "bounded probe metrics including direct pipeline stages");
#else
_Static_assert(sizeof(X4ProbeMetrics) <= 384 * 1024, "bounded probe metrics");
#endif

static const char *const names[X4_PROBE_STAGE_COUNT] = {
    "decode_call", "copy_including_synchronization", "conversion", "publication_operation",
    "draw", "flip_call", "post_GnmDone_to_flip_status_match", "new_presentation_interval",
    "input_delivery_interval", "scheduled_input_lateness"
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
    , "copy_owner_loop", "copy_helper0_loop", "copy_helper1_loop", "copy_helper2_loop",
    "copy_readers_max_loop", "copy_completion_wait",
    "free_output_wait", "stripe0", "stripe1", "stripe2", "decode_end_to_flip_observation",
    "submit_return_to_flip_observation", "conversion_end_to_flip_submit"
#endif
};
static const char *const counters[X4_PROBE_COUNT_COUNT] = {
    "input_AU", "native_output", "RGB_publication", "RGB_converted", "RGB_prepublication_superseded",
    "new_presentation", "repeat_presentation",
    "native_output_superseded", "RGB_superseded", "errors", "EOF", "cancel",
    "backpressure", "input_late", "unpaired_observation", "clock_rejected", "phase_spans_abandoned", "pair_capacity"
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
    , "native_output_latency_discarded"
#endif
};
const char *x4_probe_metrics_stage_name(X4ProbeStage s)
{ return (unsigned)s < X4_PROBE_STAGE_COUNT ? names[s] : "unknown"; }
const char *x4_probe_metrics_counter_name(X4ProbeCounter c)
{ return (unsigned)c < X4_PROBE_COUNT_COUNT ? counters[c] : "unknown"; }
size_t x4_probe_metrics_storage_bytes(void) { return sizeof(X4ProbeMetrics); }
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
void x4_probe_metrics_record_stack(X4ProbeMetrics *m, unsigned slot, const X4ProbeStackReport *r)
{
    if (!m || !r || slot >= 4) return;
    m->stack[slot] = *r; m->stack_recorded[slot] = true;
}
const X4ProbeStackReport *x4_probe_metrics_stack(const X4ProbeMetrics *m, unsigned slot)
{ return m && slot < 4 && m->stack_recorded[slot] ? &m->stack[slot] : NULL; }
void x4_probe_stack_report_json(FILE *f, const X4ProbeStackReport *r)
{
    if (!r) { fputs("null", f); return; }
    /* Names are internal; escape even unexpected bytes rather than emitting
     * invalid JSON. Raw addresses and stack bytes are never exported. */
    fputs("{\"name\":\"", f);
    for (size_t i = 0; i < sizeof(r->name) && r->name[i]; ++i) {
        unsigned char c = (unsigned char)r->name[i];
        if (c >= 32 && c < 127 && c != '"' && c != '\\') fputc(c, f);
        else fprintf(f, "\\u%04x", (unsigned)c);
    }
    fprintf(f, "\",\"configured_bytes\":%llu,\"reported_bytes\":%llu,\"guard_bytes\":%llu,"
        "\"painted_bytes\":%llu,\"untouched_bytes\":%llu,\"high_water_bytes\":%llu,"
        "\"low_excluded_bytes\":%llu,\"top_excluded_bytes\":%llu,\"entry_used_bytes\":%llu,"
        "\"attr_init_rc\":%d,\"set_size_rc\":%d,\"create_rc\":%d,\"attr_destroy_rc\":%d,"
        "\"getattr_init_rc\":%d,\"getattr_rc\":%d,\"getstacksize_rc\":%d,\"getstack_rc\":%d,"
        "\"getguardsize_rc\":%d,\"query_destroy_rc\":%d,\"measure_rc\":%d,\"join_rc\":%d,\"wrapper_result\":%d,"
        "\"started\":%s,\"finished\":%s,\"pattern_ready\":%s,\"pattern_touched\":%s,"
        "\"high_water_scope\":\"entry stack span plus pattern-changed region; excludes pre-paint startup peaks and post-scan teardown; pattern coincidence or changes only in excluded regions may undercount\"}",
        (unsigned long long)r->configured_bytes, (unsigned long long)r->reported_bytes,
        (unsigned long long)r->guard_bytes, (unsigned long long)r->painted_bytes,
        (unsigned long long)r->untouched_bytes, (unsigned long long)r->high_water_bytes,
        (unsigned long long)r->low_excluded_bytes, (unsigned long long)r->top_excluded_bytes,
        (unsigned long long)r->entry_used_bytes,
        r->attr_init_rc, r->set_size_rc, r->create_rc, r->attr_destroy_rc,
        r->getattr_init_rc, r->getattr_rc, r->getstacksize_rc, r->getstack_rc,
        r->getguardsize_rc, r->query_destroy_rc, r->measure_rc, r->join_rc, r->wrapper_result,
        r->started ? "true" : "false", r->finished ? "true" : "false", r->pattern_ready ? "true" : "false",
        r->pattern_touched ? "true" : "false");
}
void x4_probe_metrics_stacks_json(FILE *f, const X4ProbeMetrics *m)
{
    fputs("{\"schema\":\"X4PROBE_STACK1\",\"slots\":[", f);
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (slot) fputc(',', f);
        x4_probe_stack_report_json(f, x4_probe_metrics_stack(m, slot));
    }
    fputs("],\"missing_slot_is_unknown\":true}", f);
}
#endif
X4ProbeMetricsSummary *x4_probe_metrics_summary_storage(const X4ProbeMetrics *m)
{
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
    return m ? m->summary_storage : NULL;
#else
    (void)m; return NULL;
#endif
}
static void count(X4ProbeMetrics *m, X4ProbeCounter c, uint64_t n)
{ m->total[c] += n; if (m->measuring) m->measured[c] += n; }
static void count_at(X4ProbeMetrics *m, uint64_t now, X4ProbeCounter c, uint64_t n)
{
    m->total[c] += n;
    if (!m->measuring || !n) return;
    if (now < m->measure_start) { m->timestamp_omissions[c] += n; return; }
    m->measured[c] += n;
    if (!m->timestamped_count[c] || now < m->measured_first[c]) m->measured_first[c] = now;
    if (!m->timestamped_count[c] || now > m->measured_last[c]) m->measured_last[c] = now;
    m->timestamped_count[c] += n;
}
static unsigned clear_pairs(X4ProbeMetrics *m)
{
    unsigned retired = 0;
    for (unsigned s = 0; s < X4_PROBE_STAGE_COUNT; ++s)
        for (unsigned i = 0; i < PAIRS; ++i) {
            retired += m->stage[s].pairs[i].used;
            m->stage[s].pairs[i].used = false;
        }
    m->pending_flip_valid = m->last_new_valid = false;
    return retired;
}
static bool enter(X4ProbeMetrics *m)
{
    if (!m) return false;
    if (atomic_flag_test_and_set_explicit(&m->gate, memory_order_acquire)) {
        atomic_fetch_add_explicit(&m->omitted_events, 1, memory_order_relaxed);
        atomic_store_explicit(&m->dirty, true, memory_order_release);
        return false;
    }
    if (m->ended) {
        atomic_fetch_add_explicit(&m->events_after_end, 1, memory_order_relaxed);
        atomic_flag_clear_explicit(&m->gate, memory_order_release); return false;
    }
    if (atomic_exchange_explicit(&m->dirty, false, memory_order_acq_rel)) {
        ++m->dirty_resets;
        count(m, X4_PROBE_COUNT_UNPAIRED, clear_pairs(m));
    }
    return true;
}
static void leave(X4ProbeMetrics *m)
{ atomic_flag_clear_explicit(&m->gate, memory_order_release); }
X4ProbeMetrics *x4_probe_metrics_create(uint64_t now)
{
    X4ProbeMetrics *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
    m->summary_storage = &m->summary_scratch;
#endif
    atomic_flag_clear(&m->gate); atomic_init(&m->dirty, false);
    atomic_init(&m->omitted_events, 0); atomic_init(&m->events_after_end, 0);
    m->created = now; return m;
}
void x4_probe_metrics_free(X4ProbeMetrics *m) { free(m); }
bool x4_probe_metrics_begin_measurement(X4ProbeMetrics *m, uint64_t now)
{
    if (!enter(m)) return false;
    if (m->measuring) { leave(m); return true; }
    if (now < m->created) { count(m, X4_PROBE_COUNT_CLOCK_REJECT, 1); leave(m); return false; }
    count(m, X4_PROBE_COUNT_PHASE_ABANDONED, clear_pairs(m));
    m->measuring = true; m->measure_start = now; leave(m); return true;
}
static void duration(X4ProbeMetrics *m, X4ProbeStage id, uint64_t now, uint64_t value)
{
    if (!m->measuring) return;
    Stage *s = &m->stage[id];
    if (UINT64_MAX - s->sum < value || s->count == UINT64_MAX) { ++s->omitted; return; }
    if (!s->count || now < s->first_observation) s->first_observation = now;
    if (!s->count || now > s->last_observation) s->last_observation = now;
    s->sum += value; ++s->count; if (value > s->maximum) s->maximum = value;
    unsigned bucket = 0; uint64_t n = value;
    while (n > 1 && bucket + 1 < X4_PROBE_METRIC_BINS) { n >>= 1; ++bucket; }
    ++s->bins[bucket];
    if (s->retained < X4_PROBE_METRIC_SAMPLES) ++s->retained; else ++s->overwritten;
    s->samples[s->next] = value; s->next = (s->next + 1) % X4_PROBE_METRIC_SAMPLES;
}
static void begin(X4ProbeMetrics *m, X4ProbeStage id, uint64_t key, uint64_t now)
{
    if (!m->measuring) return;
    Stage *s = &m->stage[id]; Pair *vacant = NULL;
    for (unsigned i = 0; i < PAIRS; ++i) {
        Pair *p = &s->pairs[i];
        if (!p->used && !vacant) vacant = p;
        if (p->used && p->id == key) {
            count(m, X4_PROBE_COUNT_UNPAIRED, 1); p->begin = now; return;
        }
    }
    if (!vacant) { ++s->omitted; count(m, X4_PROBE_COUNT_PAIR_CAPACITY, 1); return; }
    *vacant = (Pair){.id = key, .begin = now, .used = true};
}
static void finish(X4ProbeMetrics *m, X4ProbeStage id, uint64_t key, uint64_t now)
{
    if (!m->measuring) return;
    Stage *s = &m->stage[id];
    for (unsigned i = 0; i < PAIRS; ++i) {
        Pair *p = &s->pairs[i];
        if (p->used && p->id == key) {
            uint64_t started = p->begin; p->used = false;
            if (now < started) { ++s->omitted; count(m, X4_PROBE_COUNT_CLOCK_REJECT, 1); }
            else duration(m, id, now, now - started);
            return;
        }
    }
    ++s->omitted; count(m, X4_PROBE_COUNT_UNPAIRED, 1);
}
void x4_probe_metrics_record_duration(X4ProbeMetrics *m, X4ProbeStage id, uint64_t now, uint64_t us)
{
    if ((unsigned)id >= X4_PROBE_STAGE_COUNT || !enter(m)) return;
    if (now < m->measure_start || us > now) {
        if (m->measuring) { ++m->stage[id].omitted; count(m, X4_PROBE_COUNT_CLOCK_REJECT, 1); }
    } else if (id != X4_PROBE_STAGE_INPUT_LATENESS && now - us < m->measure_start) {
        if (m->measuring) { ++m->stage[id].omitted; count(m, X4_PROBE_COUNT_PHASE_ABANDONED, 1); }
    } else duration(m, id, now, us);
    leave(m);
}
void x4_probe_metrics_note(X4ProbeMetrics *m, X4ProbeCounter c, uint64_t n)
{ if ((unsigned)c < X4_PROBE_COUNT_COUNT && enter(m)) { count(m, c, n); leave(m); } }
void x4_probe_metrics_note_at(X4ProbeMetrics *m, uint64_t now, X4ProbeCounter c, uint64_t n)
{ if ((unsigned)c < X4_PROBE_COUNT_COUNT && enter(m)) { count_at(m, now, c, n); leave(m); } }
void x4_probe_metrics_backlog(X4ProbeMetrics *m, uint64_t depth, uint64_t age)
{
    if (!enter(m)) return;
    if (depth > m->peak_backlog) m->peak_backlog = depth;
    if (age > m->max_backlog_age) m->max_backlog_age = age;
    leave(m);
}
void x4_probe_metrics_record_event(X4ProbeMetrics *m, uint64_t now, uint16_t event,
    uint16_t flags, uint64_t a, uint64_t b)
{
    /* Ignore helper/transport events before the gate; helper spans overlap
     * selected copy and are not another additive stage. */
    switch (event) {
    case X4_TRACE_DECODE_BEGIN: case X4_TRACE_DECODE_END: case X4_TRACE_COPY_BEGIN:
    case X4_TRACE_COPY_END: case X4_TRACE_CONVERT_BEGIN: case X4_TRACE_CONVERT_END:
    case X4_TRACE_DRAW_BEGIN: case X4_TRACE_DRAW_END: case X4_TRACE_FLIP_CALL_BEGIN:
    case X4_TRACE_FLIP_SUBMIT: case X4_TRACE_GNM_DONE: case X4_TRACE_FLIP_MATCH:
    case X4_TRACE_OUTPUT_VALID: case X4_TRACE_OUTPUT_SUPERSEDED: case X4_TRACE_RGB_PUBLICATION:
    case X4_TRACE_RGB_SUPERSEDED: case X4_TRACE_PRESENT_NEW: case X4_TRACE_PRESENT_REPEAT:
    case X4_TRACE_OUTPUT_REJECT: case X4_TRACE_FLIP_FAIL: break;
    default: return;
    }
    if (!enter(m)) return;
    switch (event) {
    case X4_TRACE_DECODE_BEGIN: begin(m, X4_PROBE_STAGE_DECODE, a, now); break;
    case X4_TRACE_DECODE_END:
        finish(m, X4_PROBE_STAGE_DECODE, a, now);
        if ((int32_t)(uint32_t)b < 0) count_at(m, now, X4_PROBE_COUNT_ERRORS, 1);
        break;
    case X4_TRACE_COPY_BEGIN: begin(m, X4_PROBE_STAGE_COPY, a, now); break;
    case X4_TRACE_COPY_END: finish(m, X4_PROBE_STAGE_COPY, a, now); break;
    case X4_TRACE_CONVERT_BEGIN: begin(m, X4_PROBE_STAGE_CONVERT, a, now); break;
    case X4_TRACE_CONVERT_END:
        finish(m, X4_PROBE_STAGE_CONVERT, a, now); count_at(m, now, X4_PROBE_COUNT_RGB_CONVERTED, 1); break;
    case X4_TRACE_DRAW_BEGIN: begin(m, X4_PROBE_STAGE_DRAW, a, now); break;
    case X4_TRACE_DRAW_END: finish(m, X4_PROBE_STAGE_DRAW, a, now); break;
    case X4_TRACE_FLIP_CALL_BEGIN: begin(m, X4_PROBE_STAGE_FLIP_CALL, b, now); break;
    case X4_TRACE_FLIP_SUBMIT:
        finish(m, X4_PROBE_STAGE_FLIP_CALL, b, now);
        m->pending_flip = b; m->pending_flip_valid = true; break;
    case X4_TRACE_GNM_DONE:
        if (m->pending_flip_valid) begin(m, X4_PROBE_STAGE_VSYNC_WAIT, m->pending_flip, now);
        break;
    case X4_TRACE_FLIP_MATCH:
        finish(m, X4_PROBE_STAGE_VSYNC_WAIT, a, now); m->pending_flip_valid = false; break;
    case X4_TRACE_OUTPUT_VALID: count_at(m, now, X4_PROBE_COUNT_NATIVE_OUTPUT, 1); break;
    case X4_TRACE_RGB_PUBLICATION: count_at(m, now, X4_PROBE_COUNT_RGB_PUBLICATION, 1); break;
    case X4_TRACE_OUTPUT_SUPERSEDED: count_at(m, now, X4_PROBE_COUNT_OUTPUT_SUPERSEDE, 1); break;
    case X4_TRACE_RGB_SUPERSEDED: count_at(m, now, X4_PROBE_COUNT_RGB_SUPERSEDE, 1); break;
    case X4_TRACE_OUTPUT_REJECT: case X4_TRACE_FLIP_FAIL: count_at(m, now, X4_PROBE_COUNT_ERRORS, 1); break;
    case X4_TRACE_PRESENT_REPEAT: count_at(m, now, X4_PROBE_COUNT_REPEAT_PRESENT, 1); break;
    case X4_TRACE_PRESENT_NEW:
        count_at(m, now, X4_PROBE_COUNT_NEW_PRESENT, 1);
        if (m->measuring) {
            if (m->last_new_valid && now >= m->last_new_present)
                duration(m, X4_PROBE_STAGE_NEW_PRESENT_INTERVAL, now, now - m->last_new_present);
            else if (m->last_new_valid) count(m, X4_PROBE_COUNT_CLOCK_REJECT, 1);
            m->last_new_present = now; m->last_new_valid = true;
        }
        break;
    default: break;
    }
    (void)flags; leave(m);
}
void x4_probe_metrics_end(X4ProbeMetrics *m, uint64_t now, unsigned reason)
{
    if (!enter(m)) return;
    count(m, X4_PROBE_COUNT_UNPAIRED, clear_pairs(m));
    if (now < m->created || (m->measuring && now < m->measure_start))
        count(m, X4_PROBE_COUNT_CLOCK_REJECT, 1);
    else m->ended_at = now;
    m->end_reason = reason; m->ended = true; leave(m);
}
static int compare_u64(const void *a, const void *b)
{ uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
static uint64_t percentile(const uint64_t *v, unsigned n, unsigned p)
{ return n ? v[((uint64_t)n * p + 99) / 100 - 1] : 0; }
int x4_probe_metrics_summary(const X4ProbeMetrics *m, X4ProbeMetricsSummary *out)
{
    if (!m || !out || !m->ended) return -1;
    uint64_t *scratch = malloc(X4_PROBE_METRIC_SAMPLES * sizeof(*scratch));
    if (!scratch) return -2;
    memset(out, 0, sizeof(*out));
    out->created_us = m->created; out->measure_start_us = m->measure_start; out->ended_us = m->ended_at;
    out->omitted_events = atomic_load(&m->omitted_events); out->dirty_resets = m->dirty_resets;
    out->events_after_end = atomic_load(&m->events_after_end);
    out->peak_backlog = m->peak_backlog; out->maximum_backlog_age_us = m->max_backlog_age;
    out->measuring = m->measuring; out->ended = m->ended; out->end_reason = m->end_reason;
    out->allocated_bytes = sizeof(*m); memcpy(out->total, m->total, sizeof(out->total));
    memcpy(out->measured, m->measured, sizeof(out->measured));
    memcpy(out->measured_first_us, m->measured_first, sizeof(out->measured_first_us));
    memcpy(out->measured_last_us, m->measured_last, sizeof(out->measured_last_us));
    memcpy(out->timestamped_count, m->timestamped_count, sizeof(out->timestamped_count));
    memcpy(out->timestamp_omissions, m->timestamp_omissions, sizeof(out->timestamp_omissions));
    for (unsigned id = 0; id < X4_PROBE_STAGE_COUNT; ++id) {
        const Stage *s = &m->stage[id]; X4ProbeStageSummary *d = &out->stages[id];
        d->count = s->count; d->retained = s->retained; d->overwritten = s->overwritten;
        d->omitted = s->omitted; d->sum_us = s->sum; d->maximum_us = s->maximum;
        d->first_observation_us = s->first_observation; d->last_observation_us = s->last_observation;
        memcpy(d->bins, s->bins, sizeof(d->bins));
        memcpy(scratch, s->samples, (size_t)s->retained * sizeof(*scratch));
        qsort(scratch, s->retained, sizeof(*scratch), compare_u64);
        d->p50_us = percentile(scratch, s->retained, 50);
        d->p95_us = percentile(scratch, s->retained, 95);
        d->p99_us = percentile(scratch, s->retained, 99);
        d->retained_maximum_us = s->retained ? scratch[s->retained - 1] : 0;
    }
    free(scratch); return 0;
}
static void field(FILE *f, const char *name, uint64_t value, bool comma)
{ fprintf(f, "\"%s\":%llu%s", name, (unsigned long long)value, comma ? "," : ""); }
int x4_probe_metrics_dump_file(const X4ProbeMetrics *m, const char *path)
{
    if (!path) return -1;
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
    X4ProbeMetricsSummary *s = x4_probe_metrics_summary_storage(m);
#else
    X4ProbeMetricsSummary local; X4ProbeMetricsSummary *s = &local;
#endif
    int rc = x4_probe_metrics_summary(m, s); if (rc) return rc;
    FILE *f = x4_probe_report_create(path);
    if (!f) {
#if X4_VIDEO_PROBE || defined(X4_PROBE_FILES_TESTING)
        int saved = errno;
        printf("XCloud4 probe: metrics report create=-3 errno=%d path=%s\n", saved, path);
        errno = saved;
#endif
        return -3;
    }
#if X4_VIDEO_PROBE || defined(X4_PROBE_FILES_TESTING)
    errno = 0; /* File export only: never retain an earlier successful-call errno. */
#endif
    fprintf(f, "{\"schema\":\"X4PROBE_METRICS1\",\"exact_percentile_scope\":\"last_retained_stable_observations\",\"bin_contract\":\"bin0=[0,1]; bin_i=[2^i,2^(i+1)-1]; bin31=[2^31,infinity]\",");
    field(f, "created_us", s->created_us, true); field(f, "measure_start_us", s->measure_start_us, true);
    field(f, "ended_us", s->ended_us, true); field(f, "allocated_bytes", s->allocated_bytes, true);
    field(f, "omitted_events", s->omitted_events, true); field(f, "dirty_resets", s->dirty_resets, true);
    field(f, "events_after_end", s->events_after_end, true); field(f, "peak_backlog", s->peak_backlog, true);
    field(f, "maximum_backlog_age_us", s->maximum_backlog_age_us, true); field(f, "end_reason", s->end_reason, true);
    fprintf(f, "\"measuring\":%s,\"ended\":true,\"counters\":{", s->measuring ? "true" : "false");
    for (unsigned i = 0; i < X4_PROBE_COUNT_COUNT; ++i) {
        fprintf(f, "%s\"%s\":{", i ? "," : "", counters[i]);
        field(f, "total_observed", s->total[i], true); field(f, "stable_observed", s->measured[i], true);
        field(f, "timestamped_count", s->timestamped_count[i], true);
        field(f, "timestamp_omissions", s->timestamp_omissions[i], true);
        field(f, "untimed_stable_units", s->measured[i] - s->timestamped_count[i], true);
        field(f, "measured_first_us", s->measured_first_us[i], true);
        field(f, "measured_last_us", s->measured_last_us[i], false); fprintf(f, "}");
    }
    fprintf(f, "},\"stages\":{");
    for (unsigned id = 0; id < X4_PROBE_STAGE_COUNT; ++id) {
        X4ProbeStageSummary *d = &s->stages[id]; fprintf(f, "%s\"%s\":{", id ? "," : "", names[id]);
        field(f, "accepted_observations", d->count, true); field(f, "retained", d->retained, true);
        field(f, "overwritten", d->overwritten, true); field(f, "stage_omitted", d->omitted, true);
        field(f, "sum_us", d->sum_us, true); field(f, "all_observed_max_us", d->maximum_us, true);
        field(f, "first_observation_us", d->first_observation_us, true);
        field(f, "last_observation_us", d->last_observation_us, true);
        field(f, "retained_p50_us", d->p50_us, true); field(f, "retained_p95_us", d->p95_us, true);
        field(f, "retained_p99_us", d->p99_us, true); field(f, "retained_max_us", d->retained_maximum_us, true);
        fprintf(f, "\"all_observed_bins\":[");
        for (unsigned i = 0; i < X4_PROBE_METRIC_BINS; ++i)
            fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)d->bins[i]);
        fprintf(f, "],\"retained_samples_us\":[");
        const Stage *raw = &m->stage[id];
        unsigned start = raw->retained == X4_PROBE_METRIC_SAMPLES ? raw->next : 0;
        for (unsigned i = 0; i < raw->retained; ++i)
            fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)raw->samples[(start + i) % X4_PROBE_METRIC_SAMPLES]);
        fprintf(f, "]}");
    }
#if X4_VIDEO_PROBE || defined(X4_PROBE_METRICS_TEST)
    fprintf(f, "}");
    fputs(",\"thread_stacks\":", f); x4_probe_metrics_stacks_json(f, m);
    fprintf(f, "}\n"); bool failed = ferror(f);
#else
    fprintf(f, "}}\n"); bool failed = ferror(f);
#endif
#if X4_VIDEO_PROBE || defined(X4_PROBE_FILES_TESTING)
    int write_error = failed ? (errno ? errno : EIO) : 0;
    if (x4_probe_report_finish(f)) { failed = true; if (!write_error) write_error = errno; }
    if (failed) errno = write_error;
#else
    if (fclose(f)) failed = true;
#endif
    return failed ? -4 : 0;
}
#ifdef X4_PROBE_METRICS_TEST
void x4_probe_metrics_test_gate(X4ProbeMetrics *m, bool held)
{ if (held) (void)atomic_flag_test_and_set(&m->gate); else atomic_flag_clear(&m->gate); }
#endif
