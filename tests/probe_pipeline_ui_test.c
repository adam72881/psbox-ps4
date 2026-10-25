/* SPDX-License-Identifier: GPL-3.0-only */
#define X4_VIDEO_PROBE 1
#include "../src/core/probe_controller.h"
#include "../src/video/probe_case_catalog.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static unsigned index_of(const char *id)
{
    for (unsigned i = 0; i < X4_PROBE_ALL_CASE_COUNT; ++i)
        if (!strcmp(x4_probe_case_at(i)->id, id)) return i;
    return X4_PROBE_ALL_CASE_COUNT;
}
int main(void)
{
    X4ProbePipelineChoice choice = {0};
    bool passed[X4_PROBE_PIPELINE_COUNT * X4_PROBE_CASE_COUNT] = {false};
    CHECK(choice.selected == X4_PROBE_PIPELINE_CURRENT);
    CHECK(!x4_probe_pipeline_choice_allowed(&choice, -1, passed, X4_PROBE_CASE_COUNT));
    CHECK(!x4_probe_pipeline_choice_change(&choice, false));
    CHECK(choice.selected == X4_PROBE_PIPELINE_CURRENT);
    CHECK(x4_probe_pipeline_choice_change(&choice, true));
    CHECK(choice.selected == X4_PROBE_PIPELINE_DECODE_ONLY);
    CHECK(x4_probe_pipeline_converters(choice.selected) == 0);
    /* Compatibility does not require replaying control 720p30. */
    unsigned cb720 = index_of("720p60-cb"), high1080 = index_of("1080p60-high");
    unsigned stress1080 = index_of("S1080-HIGH");
    CHECK(cb720 < X4_PROBE_CASE_COUNT && high1080 < X4_PROBE_CASE_COUNT);
    CHECK(stress1080 < X4_PROBE_ALL_CASE_COUNT);
    CHECK(x4_probe_required_compatibility(cb720) == -1);
    CHECK(x4_probe_required_compatibility(high1080) == -1);
    CHECK(x4_probe_pipeline_choice_allowed(&choice, -1, NULL, 0));
    int s1080 = x4_probe_required_compatibility(stress1080);
    CHECK(s1080 == (int)high1080);
    CHECK(!x4_probe_pipeline_choice_allowed(&choice, s1080, passed, X4_PROBE_CASE_COUNT));
    passed[choice.selected * X4_PROBE_CASE_COUNT + s1080] = true;
    CHECK(x4_probe_pipeline_choice_allowed(&choice, s1080, passed, X4_PROBE_CASE_COUNT));
    CHECK(x4_probe_pipeline_choice_change(&choice, true));
    CHECK(choice.selected == X4_PROBE_PIPELINE_TWO && x4_probe_pipeline_converters(choice.selected) == 2);
    /* Decode-only success cannot approve the presentation/striped path. */
    CHECK(!x4_probe_pipeline_choice_allowed(&choice, s1080, passed, X4_PROBE_CASE_COUNT));
    passed[choice.selected * X4_PROBE_CASE_COUNT + s1080] = true;
    CHECK(x4_probe_pipeline_choice_allowed(&choice, s1080, passed, X4_PROBE_CASE_COUNT));
    CHECK(x4_probe_pipeline_choice_change(&choice, true));
    CHECK(choice.selected == X4_PROBE_PIPELINE_THREE && x4_probe_pipeline_converters(choice.selected) == 3);
    CHECK(!x4_probe_pipeline_choice_allowed(&choice, s1080, passed, X4_PROBE_CASE_COUNT));
    /* A failed new pipeline blocks both stripe counts, not current/decode. */
    x4_probe_pipeline_choice_failed(&choice, X4_PROBE_PIPELINE_TWO);
    CHECK(choice.selected == X4_PROBE_PIPELINE_CURRENT);
    CHECK(choice.blocked[X4_PROBE_PIPELINE_TWO] && choice.blocked[X4_PROBE_PIPELINE_THREE]);
    CHECK(!choice.blocked[X4_PROBE_PIPELINE_DECODE_ONLY] && !choice.blocked[X4_PROBE_PIPELINE_CURRENT]);
    CHECK(x4_probe_pipeline_choice_change(&choice, true));
    CHECK(choice.selected == X4_PROBE_PIPELINE_DECODE_ONLY);
    CHECK(x4_probe_pipeline_choice_change(&choice, true));
    CHECK(choice.selected == X4_PROBE_PIPELINE_CURRENT);
    x4_probe_pipeline_choice_failed(&choice, X4_PROBE_PIPELINE_DECODE_ONLY);
    CHECK(!x4_probe_pipeline_choice_change(&choice, true));
    CHECK(choice.selected == X4_PROBE_PIPELINE_CURRENT);
    X4ProbePipelineChoice decode_failure = {.selected=X4_PROBE_PIPELINE_DECODE_ONLY};
    x4_probe_pipeline_choice_failed(&decode_failure, X4_PROBE_PIPELINE_DECODE_ONLY);
    CHECK(x4_probe_pipeline_choice_change(&decode_failure, true));
    CHECK(decode_failure.selected == X4_PROBE_PIPELINE_TWO);
    CHECK(!x4_probe_pipeline_choice_allowed(&decode_failure, -2, passed, X4_PROBE_CASE_COUNT));
    CHECK(!x4_probe_pipeline_choice_allowed(&decode_failure, X4_PROBE_CASE_COUNT, passed, X4_PROBE_CASE_COUNT));
    CHECK(!x4_probe_pipeline_choice_allowed(&decode_failure, 0, NULL, X4_PROBE_CASE_COUNT));
    decode_failure.selected = X4_PROBE_PIPELINE_COUNT;
    CHECK(!x4_probe_pipeline_choice_change(&decode_failure, true));
    CHECK(!x4_probe_pipeline_choice_allowed(&decode_failure, -1, passed, X4_PROBE_CASE_COUNT));
    CHECK(!x4_probe_pipeline_choice_change(NULL, true));
    CHECK(!x4_probe_pipeline_choice_allowed(NULL, -1, passed, X4_PROBE_CASE_COUNT));
    x4_probe_pipeline_choice_failed(NULL, X4_PROBE_PIPELINE_TWO);
    puts("probe pipeline UI selection/gates: PASS");
    return 0;
}
