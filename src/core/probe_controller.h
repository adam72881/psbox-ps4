/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* Local-only entry point. The enabled path never creates authentication,
 * network or streaming sessions. Normal exit uses the established LoadExec
 * lifecycle, never main return/_exit. */
int x4_probe_controller_run(void);

#if X4_VIDEO_PROBE
#include <stdbool.h>
#include <stddef.h>
#include "../video/probe_case.h"
/* Probe-only execution choices. Keeping CURRENT at zero makes the unchanged
 * pipeline the default after every launch. Compatibility belongs to one
 * execution choice: decode-only cannot approve a presentation case. */
enum { X4_PROBE_PIPELINE_CURRENT, X4_PROBE_PIPELINE_DECODE_ONLY,
    X4_PROBE_PIPELINE_TWO, X4_PROBE_PIPELINE_THREE, X4_PROBE_PIPELINE_COUNT };
typedef struct {
    unsigned selected;
    bool blocked[X4_PROBE_PIPELINE_COUNT];
} X4ProbePipelineChoice;
static inline unsigned x4_probe_pipeline_converters(unsigned choice)
{
    return choice == X4_PROBE_PIPELINE_TWO ? 2u :
        choice == X4_PROBE_PIPELINE_THREE ? 3u : 0u;
}
static inline bool x4_probe_pipeline_choice_change(X4ProbePipelineChoice *s, bool idle)
{
    if (!s || !idle || s->selected >= X4_PROBE_PIPELINE_COUNT) return false;
    for (unsigned offset = 1; offset < X4_PROBE_PIPELINE_COUNT; ++offset) {
        unsigned next = (s->selected + offset) % X4_PROBE_PIPELINE_COUNT;
        if (!s->blocked[next]) { s->selected = next; return true; }
    }
    return false;
}
static inline bool x4_probe_pipeline_choice_allowed(const X4ProbePipelineChoice *s,
    int prerequisite, const bool *passed, size_t count)
{
    if (!s || !s->selected || s->selected >= X4_PROBE_PIPELINE_COUNT ||
        s->blocked[s->selected]) return false;
    return prerequisite == -1 || (prerequisite >= 0 && passed &&
        (size_t)prerequisite < count && passed[(size_t)s->selected * count + prerequisite]);
}
static inline void x4_probe_pipeline_choice_failed(X4ProbePipelineChoice *s, unsigned choice)
{
    if (!s || choice >= X4_PROBE_PIPELINE_COUNT || !choice) return;
    if (choice == X4_PROBE_PIPELINE_DECODE_ONLY) s->blocked[choice] = true;
    else s->blocked[X4_PROBE_PIPELINE_TWO] = s->blocked[X4_PROBE_PIPELINE_THREE] = true;
    s->selected = X4_PROBE_PIPELINE_CURRENT;
}
/* UI choices are deliberately not native enum values. The default is slot 0
 * (existing WC_GARLIC); an approval belongs only to its memory slot. */
enum { X4_PROBE_MEMORY_CHOICE_CURRENT, X4_PROBE_MEMORY_CHOICE_CACHEABLE,
    X4_PROBE_MEMORY_CHOICE_COUNT };
typedef struct {
    unsigned selected;
    bool blocked[X4_PROBE_MEMORY_CHOICE_COUNT];
} X4ProbeMemoryChoice;
static inline int x4_probe_memory_choice_type(unsigned choice)
{ return choice == X4_PROBE_MEMORY_CHOICE_CURRENT ? 3 : choice == X4_PROBE_MEMORY_CHOICE_CACHEABLE ? 0 : -1; }
static inline bool x4_probe_memory_choice_change(X4ProbeMemoryChoice *s, bool idle)
{
    if (!s || !idle || s->selected >= X4_PROBE_MEMORY_CHOICE_COUNT) return false;
    unsigned next = 1u - s->selected;
    if (s->blocked[next]) return false;
    s->selected = next; return true;
}
static inline bool x4_probe_memory_choice_allowed(const X4ProbeMemoryChoice *s,
    int prerequisite, const bool *memory_passed, size_t count)
{
    return s && s->selected < X4_PROBE_MEMORY_CHOICE_COUNT && !s->blocked[s->selected] &&
        (prerequisite == -1 || (prerequisite >= 0 && memory_passed &&
         (size_t)prerequisite < count && memory_passed[(size_t)s->selected * count + prerequisite]));
}
static inline bool x4_probe_memory_variant_failed(const X4ProbeStatus *s)
{
    if (!s) return false;
    const int calls[] = {s->query_result, s->create_result, s->decode_result, s->flush_result};
    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i)
        if (calls[i] != X4_PROBE_RC_NOT_RUN && calls[i]) return true;
    /* validate_picture can reject the FIRST native output before the
     * accepted-output counter advances. Never gate a rejection on that count. */
    return s->output_reject_reason || s->layout_verdict == X4_PROBE_LAYOUT_FAILED ||
        s->layout_verdict == X4_PROBE_LAYOUT_INDETERMINATE;
}
static inline bool x4_probe_case_policy_rejected(const X4ProbeStatus *s)
{
    return s && s->native_policy_budget && s->rounded_native_bytes > s->native_policy_budget &&
        s->query_result == 0 && s->create_result == X4_PROBE_RC_NOT_RUN;
}
static inline bool x4_probe_case_stops_session(const X4ProbeStatus *s)
{
    if (!s) return false;
    if (x4_probe_memory_variant_failed(s) || x4_probe_case_policy_rejected(s)) return true;
    const int calls[] = {s->pipeline_result, s->compute_query_result, s->allocate_queue_result,
        s->delete_decoder_result, s->release_queue_result, s->reference_close_result, s->close_result};
    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i)
        if (calls[i] != X4_PROBE_RC_NOT_RUN && calls[i] < 0) return true;
    return false;
}
#endif
