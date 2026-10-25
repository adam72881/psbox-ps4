/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_samples.generated.h"
#include "probe_stress_samples.generated.h"
#if X4_VIDEO_PROBE
#include "probe_pipeline_samples.generated.h"
#else
#define X4_PROBE_PIPELINE_CASE_COUNT 0
#endif
#include <string.h>

/* Keep the frozen five descriptors and their ordering untouched. Stress lives
 * in another array and another data directory; a rejection is profile-specific. */
#define X4_PROBE_ALL_CASE_COUNT (X4_PROBE_CASE_COUNT + X4_PROBE_STRESS_CASE_COUNT + X4_PROBE_PIPELINE_CASE_COUNT)
static inline const X4ProbeCase *x4_probe_case_at(unsigned index)
{
    if (index < X4_PROBE_CASE_COUNT) return &x4_probe_cases[index];
    index -= (unsigned)X4_PROBE_CASE_COUNT;
    if (index < X4_PROBE_STRESS_CASE_COUNT) return &x4_probe_stress_cases[index];
#if X4_VIDEO_PROBE
    index -= (unsigned)X4_PROBE_STRESS_CASE_COUNT;
    if (index < X4_PROBE_PIPELINE_CASE_COUNT) return &x4_probe_pipeline_cases[index];
#endif
    return NULL;
}
static inline bool x4_probe_case_pipeline_only(unsigned index)
{
    return index >= X4_PROBE_CASE_COUNT + X4_PROBE_STRESS_CASE_COUNT &&
        index < X4_PROBE_ALL_CASE_COUNT;
}
static inline int x4_probe_required_for(const X4ProbeCase *stress,
    const X4ProbeCase *compatibility, size_t compatibility_count)
{
    static const struct { const char *stress_id, *compatibility_id;
        unsigned width, height, profile, level; } rules[] = {
        {"S720-CB", "720p60-cb", 1280, 720, 66, 32},
        {"S1080-CB", "1080p60-cb", 1920, 1080, 66, 42},
        {"S1080-HIGH", "1080p60-high", 1920, 1080, 100, 42},
    };
    if (!stress || !stress->id || !compatibility || compatibility_count > INT32_MAX) return -2;
    for (size_t rule = 0; rule < sizeof(rules)/sizeof(rules[0]); ++rule) {
        if (strcmp(stress->id, rules[rule].stress_id)) continue;
        if (stress->visible_width != rules[rule].width || stress->visible_height != rules[rule].height ||
            stress->profile_idc != rules[rule].profile || stress->level_idc != rules[rule].level ||
            stress->fps_num != 60 || stress->fps_den != 1) return -2;
        int found = -2;
        for (size_t i = 0; i < compatibility_count; ++i) {
            const X4ProbeCase *c = &compatibility[i];
            if (!c->id || strcmp(c->id, rules[rule].compatibility_id)) continue;
            if (found >= 0 || c->profile_idc != stress->profile_idc || c->level_idc != stress->level_idc ||
                c->visible_width != stress->visible_width || c->visible_height != stress->visible_height ||
                c->coded_width != stress->coded_width || c->coded_height != stress->coded_height ||
                c->fps_num != stress->fps_num || c->fps_den != stress->fps_den ||
                c->crop_left != stress->crop_left || c->crop_top != stress->crop_top ||
                c->crop_right != stress->crop_right || c->crop_bottom != stress->crop_bottom) return -2;
            found = (int)i;
        }
        return found;
    }
    return -2; /* Unknown stress ID or absent/mismatched prerequisite. */
}
static inline int x4_probe_required_compatibility(unsigned index)
{
    if (index < X4_PROBE_CASE_COUNT) return -1;
    if (x4_probe_case_pipeline_only(index)) return -1; /* Additive compatibility fixture. */
    return x4_probe_required_for(x4_probe_case_at(index), x4_probe_cases, X4_PROBE_CASE_COUNT);
}
static inline bool x4_probe_gate_allows(int prerequisite, const bool *passed, size_t count)
{
    return prerequisite == -1 || (prerequisite >= 0 && passed && (size_t)prerequisite < count && passed[prerequisite]);
}
static inline bool x4_probe_compatibility_begin(unsigned index, bool *passed, size_t count)
{
    if (!passed || index >= count) return false;
    passed[index] = false; /* A rerun must establish a fresh result. */
    return true;
}
static inline bool x4_probe_compatibility_pass(const X4ProbeCase *sample,
    const X4ProbeStatus *status, uint64_t new_presentations, uint64_t observed_errors)
{
    /* Host success does not grant this gate. The current process must observe
     * native query/create/decode, verified layout, all outputs, actual visible
     * progress and clean native/reference teardown for this exact descriptor. */
    return sample && status && sample->au_count && status->ready &&
        status->source_eof && status->finished && status->pipeline_result == 0 &&
        status->query_result == 0 && status->create_result == 0 && status->decode_result == 0 &&
        status->reference_result == 0 && status->flush_result == 0 && status->close_result == 0 &&
        status->delete_decoder_result == 0 && status->release_queue_result == 0 && status->reference_close_result == 0 &&
        status->layout_verdict == X4_PROBE_LAYOUT_VERIFIED &&
        status->input_aus == sample->au_count && status->native_outputs == sample->au_count &&
        status->rgb_publications && new_presentations && !observed_errors;
}
