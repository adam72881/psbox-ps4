/* SPDX-License-Identifier: GPL-3.0-only */
#define X4_VIDEO_PROBE 1
#include "../src/core/probe_controller.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void)
{
    X4ProbeMemoryChoice choice = {0};
    bool passed[10] = {false};
    CHECK(choice.selected == X4_PROBE_MEMORY_CHOICE_CURRENT);
    CHECK(x4_probe_memory_choice_type(choice.selected) == 3);
    CHECK(x4_probe_memory_choice_type(1) == 0);
    CHECK(x4_probe_memory_choice_type(2) == -1);
    CHECK(!x4_probe_memory_choice_change(&choice, false)); /* Running case. */
    CHECK(choice.selected == 0);
    CHECK(x4_probe_memory_choice_allowed(&choice, -1, passed, 5));
    CHECK(!x4_probe_memory_choice_allowed(&choice, 2, passed, 5));
    passed[2] = true;
    CHECK(x4_probe_memory_choice_allowed(&choice, 2, passed, 5));
    CHECK(x4_probe_memory_choice_change(&choice, true));
    CHECK(choice.selected == 1);
    CHECK(!x4_probe_memory_choice_allowed(&choice, 2, passed, 5)); /* WC success cannot approve WB stress. */
    passed[7] = true;
    CHECK(x4_probe_memory_choice_allowed(&choice, 2, passed, 5));
    CHECK(!x4_probe_memory_choice_allowed(&choice, -2, passed, 5));
    CHECK(!x4_probe_memory_choice_allowed(&choice, 5, passed, 5));
    CHECK(!x4_probe_memory_choice_allowed(&choice, 2, NULL, 5));
    choice.blocked[1] = true;
    CHECK(!x4_probe_memory_choice_allowed(&choice, -1, passed, 5));
    CHECK(x4_probe_memory_choice_change(&choice, true)); /* Current remains available. */
    CHECK(choice.selected == 0 && x4_probe_memory_choice_allowed(&choice, 2, passed, 5));
    CHECK(!x4_probe_memory_choice_change(&choice, true)); /* Failed experimental variant cannot restart. */
    CHECK(choice.selected == 0);
    choice.selected = 2;
    CHECK(!x4_probe_memory_choice_change(&choice, true));
    CHECK(!x4_probe_memory_choice_allowed(&choice, -1, passed, 5));
    CHECK(!x4_probe_memory_choice_change(NULL, true));
    CHECK(!x4_probe_memory_choice_allowed(NULL, -1, passed, 5));
    X4ProbeStatus status = x4_probe_status_pending();
    CHECK(!x4_probe_memory_variant_failed(&status));
    CHECK(!x4_probe_case_stops_session(&status));
    status.query_result = status.create_result = status.decode_result = 0;
    status.native_outputs = 0; status.output_reject_reason = 3;
    CHECK(x4_probe_memory_variant_failed(&status)); /* First output rejected BEFORE accepted count. */
    CHECK(x4_probe_case_stops_session(&status));
    status.output_reject_reason = 0; status.layout_verdict = X4_PROBE_LAYOUT_FAILED;
    CHECK(x4_probe_memory_variant_failed(&status));
    status.layout_verdict = X4_PROBE_LAYOUT_INDETERMINATE;
    CHECK(x4_probe_memory_variant_failed(&status));
    status.layout_verdict = X4_PROBE_LAYOUT_VERIFIED;
    CHECK(!x4_probe_memory_variant_failed(&status));
    status.flush_result = -37;
    CHECK(x4_probe_memory_variant_failed(&status)); /* Decode succeeded, native Flush failed. */
    status.flush_result = 0;
    CHECK(!x4_probe_memory_variant_failed(&status));
    status.pipeline_result = X4_PROBE_CANCELLED;
    CHECK(!x4_probe_case_stops_session(&status)); /* Explicit user cancel is not a native failure. */
    status.pipeline_result = -7;
    CHECK(x4_probe_case_stops_session(&status)); /* Preparation/copy failure stops ordinary memory too. */
    status = x4_probe_status_pending(); status.query_result = 0;
    status.native_policy_budget = 128u * 1024u * 1024u;
    status.rounded_native_bytes = status.native_policy_budget + 1;
    CHECK(x4_probe_case_policy_rejected(&status));
    CHECK(!x4_probe_memory_variant_failed(&status)); /* Budget is not decoder rejection. */
    CHECK(x4_probe_case_stops_session(&status));
    status.rounded_native_bytes = status.native_policy_budget;
    CHECK(!x4_probe_case_policy_rejected(&status));
    puts("probe memory UI state: PASS"); return 0;
}
