#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic reports only. No fixture is presented as a console observation."""
import copy
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import analyze_probe_pipeline as A


def stack(name):
    return {"name": name, "configured_bytes": 262144, "reported_bytes": 262144, "guard_bytes": 16384,
            "painted_bytes": 253952, "untouched_bytes": 245760, "high_water_bytes": 16384,
            "low_excluded_bytes": 4096, "top_excluded_bytes": 4096,
            "started": True, "finished": True, "pattern_ready": True, "pattern_touched": True,
            "measure_rc": 0, "join_rc": 0}


def fixture(converters=2):
    source = "ab" * 20
    case = {"schema": "X4PROBE_PIPELINE1", "source_id": source,
        "build_id": source + "-n1-g45-r0-vc1-p1-im", "case_id": "1080p60-high",
        "converters": converters, "mode": "paced", "decode_only": converters == 0,
        "decode_pipeline_depth": 1, "output_memory_type": 0, "output_buffers": 4,
        "videoout_buffers": 3 if converters else 0, "input_output_PTS_association": False,
        "native_result_is_not_streaming_support": True,
        "started_us": 100, "measurement_start_us": 1_000_000, "ended_us": 11_000_000,
        "input_begin_us": 1000, "native_producer_done_us": 10_900_000,
        "warmup_count": 60, "warmup_basis": "RGB_publications" if converters else "native_outputs",
        "input_AU": 600, "native_outputs": 600, "measured_decode_outputs": 540,
        "RGB_publications": 300 if converters else 0, "new_presentations": 300 if converters else 0,
        "no_picture_calls": 1, "source_eof": True, "cancelled": False, "retained": False,
        "late_inputs": 0, "maximum_input_late_us": 0,
        "sample": {"clip_sha256": "01" * 32, "index_sha256": "02" * 32, "reference_sha256": "03" * 32,
                   "fps_num": 60, "fps_den": 1, "AU_count": 600, "duration_us": 10_000_000,
                   "coded_width": 1920, "coded_height": 1088, "profile_idc": 100, "level_idc": 42},
        "native_memory": {"query_compute": 1, "query_cpu": 1, "query_gpu": 1, "query_shared": 1,
                          "query_output": 3346432, "decoder_total": 106807296, "scanout_total": 25165824 if converters else 0,
                          "budget": 134217728, "decoder_policy_budget": 109051904, "scanout_policy_reservation": 25165824,
                          "retained_decoder": 0, "staging_bytes": 0,
                          "reference_scratch_only_bytes": 8294400},
        "ring": {"peak_occupied": 4, "peak_ready": 3, "discarded_ready": 0, "cancelled_leases": 0,
                 "waits": 1, "rejected": 0, "final_occupied": 0},
        "layout": {"verdict": 1, "checks": 600, "matches": 3, "visible_width": 1920, "visible_height": 1080,
                   "native_width": 1920, "native_height": 1088, "pitch": 1920, "storage_height": 1088,
                   "UV_offset": 2088960, "returned_bytes": 3346432, "owned_bytes": 3358720},
        "reference_scores": [[{"MAE": 0.0, "max_delta": 0, "alpha_failures": 0, "accepted_references": 1,
                               "identity_ambiguous": False}] for _ in range(3)]}
    case.update({key: 0 for key in A.RETURNS})
    case["scanout"] = {key: 0 if converters else A.NOT_RUN for key in A.SCANOUT_RETURNS}
    case["scanout"]["display_error"] = False
    stacks = {"schema": "X4PROBE_STACK1", "slots": [stack("x4-decode")] +
              [stack("x4-convert%d" % i) if i < converters else None for i in range(3)]}
    case["thread_stacks"] = copy.deepcopy(stacks)
    metrics = {"schema": "X4PROBE_METRICS1", "created_us": 100, "measure_start_us": 1_000_000,
               "ended_us": 11_000_000, "ended": True, "measuring": True, "omitted_events": 0,
               "dirty_resets": 0, "events_after_end": 0, "counters": {}, "stages": {}, "thread_stacks": stacks}
    for name in ("input_AU", "native_output", "RGB_publication", "new_presentation", "native_output_superseded", "errors"):
        total = 600 if name in ("input_AU", "native_output") else 300 if converters and name in ("RGB_publication", "new_presentation") else 0
        measured = max(0, total - 60)
        metrics["counters"][name] = {"total_observed": total, "stable_observed": measured,
            "timestamped_count": measured, "timestamp_omissions": 0, "untimed_stable_units": 0,
            "measured_first_us": 1_010_000 if measured else 0, "measured_last_us": 10_990_000 if measured else 0}
    for name in A.STAGES:
        metrics["stages"][name] = {"accepted_observations": 3, "retained": 3, "overwritten": 0,
            "stage_omitted": 0, "first_observation_us": 1_010_000, "last_observation_us": 10_990_000,
            "retained_samples_us": [30, 10, 20], "retained_p50_us": 20,
            "retained_p95_us": 30, "retained_p99_us": 30, "retained_max_us": 30, "all_observed_max_us": 30}
    return case, metrics


def latest_fixture(converters=2):
    case, metrics = fixture(converters)
    case.update(schema="X4PROBE_PIPELINE2", ready_policy="latest")
    case["ring"].update(discarded_ring_full=0, discarded_latency=298 if converters else 0)
    observed = copy.deepcopy(metrics["counters"]["native_output_superseded"])
    # Diagnostic omissions may leave fewer observations than owner totals.
    metrics["counters"]["native_output_latency_discarded"] = observed
    return case, metrics


def oneflight_fixture(converters=2):
    case,metrics=latest_fixture(converters)
    case.update(schema="X4PROBE_PIPELINE3",scanout_policy="one_frame_in_flight")
    case["videoout_occupancy"]={"scope":"main_loop_observations_not_time_weighted",
        "observations":3 if converters else 0,"omitted":0,"peak_occupied":2 if converters else 0,
        "peak_in_flight":1 if converters else 0,"final_result":0,"final_occupied":0,"final_in_flight":0,
        "occupied_samples":[1,0,2,0] if converters else [0]*4,
        "in_flight_samples":[1,2,0,0] if converters else [0]*4}
    metrics["stages"]["conversion_end_to_flip_submit"]=copy.deepcopy(metrics["stages"]["flip_call"])
    return case,metrics


class PipelineAnalysisTests(unittest.TestCase):
    def test_oneflight_occupancy_and_submit_span(self):
        for converters in (0,2,3):
            result=A.analyze(*oneflight_fixture(converters))
            self.assertEqual(result["issues"],[])
            self.assertEqual(result["scanout_policy"],"one_frame_in_flight")
            self.assertEqual(result["stages_us"]["conversion_end_to_flip_submit"]["retained_p50_us"],20)
            self.assertEqual(result["videoout_occupancy"]["peak_in_flight"],int(bool(converters)))

    def test_oneflight_rejects_overlap_missing_or_miscounted_occupancy(self):
        for change in (dict(peak_in_flight=2),dict(peak_occupied=3),dict(observations=4),
                       dict(final_occupied=1),dict(final_result=-1),dict(in_flight_samples=[0,1,2,0])):
            case,metrics=oneflight_fixture();case["videoout_occupancy"].update(change)
            self.assertNotEqual(A.analyze(case,metrics)["report_validation"],"PASS")
        case,metrics=oneflight_fixture();del case["videoout_occupancy"]
        self.assertNotEqual(A.analyze(case,metrics)["report_validation"],"PASS")

    def test_oneflight_new_stage_and_schema_are_required(self):
        case,metrics=oneflight_fixture();del metrics["stages"]["conversion_end_to_flip_submit"]
        self.assertNotEqual(A.analyze(case,metrics)["report_validation"],"PASS")
        case,metrics=oneflight_fixture();case["schema"]="X4PROBE_PIPELINE2"
        self.assertNotEqual(A.analyze(case,metrics)["report_validation"],"PASS")
        case,metrics=oneflight_fixture();del case["scanout_policy"]
        self.assertNotEqual(A.analyze(case,metrics)["report_validation"],"PASS")

    def test_latest_policy_separates_latency_and_ring_full_discards(self):
        case, metrics = latest_fixture()
        case["ring"].update(discarded_ready=98, discarded_ring_full=98, discarded_latency=200)
        result = A.analyze(case, metrics)
        self.assertEqual(result["issues"], [])
        self.assertEqual(result["ready_policy"], "latest")
        self.assertEqual(result["ring"]["discarded_latency"], 200)
        self.assertEqual(result["ring"]["discarded_ring_full"], 98)
        self.assertEqual(A.analyze(*fixture())["ready_policy"], "legacy_fifo")

    def test_latest_missing_or_false_drop_accounting_is_not_success(self):
        for field in ("discarded_latency", "discarded_ring_full"):
            case, metrics = latest_fixture()
            del case["ring"][field]
            self.assertEqual(A.analyze(case, metrics)["report_validation"], "INVALID_OR_INCOMPLETE")
        case, metrics = latest_fixture(); case["ring"]["discarded_latency"] -= 1
        self.assertEqual(A.analyze(case, metrics)["report_validation"], "INVALID_OR_INCOMPLETE")
        case, metrics = latest_fixture(); case["ring"]["discarded_ring_full"] = 1
        self.assertEqual(A.analyze(case, metrics)["report_validation"], "INVALID_OR_INCOMPLETE")

    def test_latest_policy_requires_explicit_schema_and_counter(self):
        case, metrics = latest_fixture(); del case["ready_policy"]
        self.assertEqual(A.analyze(case, metrics)["report_validation"], "INVALID_OR_INCOMPLETE")
        case, metrics = latest_fixture(); del metrics["counters"]["native_output_latency_discarded"]
        self.assertEqual(A.analyze(case, metrics)["report_validation"], "INVALID_OR_INCOMPLETE")
        case, metrics = latest_fixture(); case["schema"] = "X4PROBE_PIPELINE1"
        self.assertEqual(A.analyze(case, metrics)["report_validation"], "INVALID_OR_INCOMPLETE")

    def test_latest_decode_only_does_not_invent_publication_drops(self):
        result = A.analyze(*latest_fixture(0))
        self.assertEqual(result["issues"], [])
        self.assertIsNone(result["rates"]["observed_new_presentations_per_second"])

    def test_complete_report_is_only_reported_local_success(self):
        result = A.analyze(*fixture())
        self.assertEqual(result["issues"], [])
        self.assertEqual(result["report_validation"], "PASS")
        self.assertEqual(result["rates"]["observed_new_presentations_per_second"], 24)
        self.assertFalse(result["streaming_support_proven"])
        self.assertFalse(result["console_execution_independently_confirmed"])
        self.assertEqual(result["color"]["exact_first_outputs"], 3)

    def test_decode_only_uses_native_basis_and_never_reports_display_fps(self):
        result = A.analyze(*fixture(0))
        self.assertEqual(result["issues"], [])
        self.assertIsNone(result["rates"]["observed_new_presentations_per_second"])
        self.assertEqual(result["rates"]["native_outputs_per_second_same_end_interval"], 54)
        self.assertTrue(result["rates"]["paced_input_limits_decode_only_ceiling"])
        self.assertEqual(result["timing"]["warmup_basis"], "native_outputs")

    def test_pair_mismatch_suppresses_rates(self):
        case, metrics = fixture(); metrics["measure_start_us"] += 1
        result = A.analyze(case, metrics)
        self.assertEqual(result["report_validation"], "INVALID_OR_INCOMPLETE")
        self.assertIsNone(result["rates"]["observed_new_presentations_per_second"])

    def test_omissions_are_preserved_without_inventing_full_coverage(self):
        case, metrics = fixture(); metrics["omitted_events"] = 7; metrics["dirty_resets"] = 2
        metrics["counters"]["new_presentation"]["total_observed"] -= 3
        result = A.analyze(case, metrics)
        self.assertEqual(result["report_validation"], "PASS")
        self.assertEqual(result["coverage"]["omitted_events"], 7)
        self.assertEqual(result["coverage"]["dirty_resets"], 2)

    def test_wrong_percentile_is_rejected(self):
        case, metrics = fixture(); metrics["stages"]["decode_call"]["retained_p95_us"] = 500
        self.assertEqual(A.analyze(case, metrics)["report_validation"], "INVALID_OR_INCOMPLETE")

    def test_missing_field_is_unknown_not_zero(self):
        case, metrics = fixture(); del metrics["omitted_events"]
        result = A.analyze(case, metrics)
        self.assertIsNone(result["coverage"]["omitted_events"])
        self.assertEqual(result["report_validation"], "INVALID_OR_INCOMPLETE")

    def test_eof_does_not_approve_incomplete_au_or_native_output_count(self):
        for key, metric in (("input_AU", "input_AU"), ("native_outputs", "native_output")):
            case, metrics = fixture(); case[key] -= 1
            metrics["counters"][metric]["total_observed"] -= 1
            result = A.analyze(case, metrics)
            self.assertEqual(result["report_validation"], "INVALID_OR_INCOMPLETE")
            self.assertEqual(result["outcome"], "indeterminate_invalid_or_incomplete_report")
            self.assertIsNone(result["rates"]["observed_new_presentations_per_second"])

    def test_error_is_not_masked_by_cancel_flag(self):
        case, metrics = fixture(); case["decode_result"] = -77; case["cancelled"] = True
        self.assertEqual(A.analyze(case, metrics)["outcome"], "native_or_local_error")

    def test_policy_rejection_is_not_decoder_incompatibility(self):
        case, metrics = fixture(); case["native_memory"]["decoder_total"] = 200_000_000
        case["create_result"] = A.NOT_RUN
        result = A.analyze(case, metrics)
        self.assertEqual(result["outcome"], "policy_rejected_not_decoder_incompatibility")

    def test_retention_stops_even_with_zero_native_calls(self):
        case, metrics = fixture(); case["retained"] = True
        self.assertEqual(A.analyze(case, metrics)["outcome"], "resources_retained_stop")

    def test_decode_only_conservative_budget_is_reported_as_policy(self):
        case, metrics = fixture(0); case["native_memory"]["decoder_total"] = 120_000_000
        case["create_result"] = A.NOT_RUN
        self.assertEqual(A.analyze(case, metrics)["outcome"], "policy_rejected_not_decoder_incompatibility")

    def test_scanout_error_prevents_success(self):
        case, metrics = fixture(); case["scanout"]["map"] = -13
        self.assertEqual(A.analyze(case, metrics)["outcome"], "native_or_local_error")
        case, metrics = fixture(); case["scanout"]["display_error"] = True
        self.assertEqual(A.analyze(case, metrics)["outcome"], "native_or_local_error")

    def test_wrong_layout_and_stack_are_not_success(self):
        for section, key, value in (("layout", "UV_offset", 1920 * 1080), ("ring", "peak_occupied", 5)):
            case, metrics = fixture(); case[section][key] = value
            self.assertEqual(A.analyze(case, metrics)["report_validation"], "INVALID_OR_INCOMPLETE")
        case, metrics = fixture(); case["thread_stacks"]["slots"][0]["reported_bytes"] = 65536
        self.assertEqual(A.analyze(case, metrics)["report_validation"], "INVALID_OR_INCOMPLETE")

    def test_color_not_exact_is_not_reported_as_exact(self):
        case, metrics = fixture(); case["reference_scores"][1][0]["MAE"] = 0.1
        result = A.analyze(case, metrics)
        self.assertEqual(result["color"]["exact_first_outputs"], 2)
        self.assertEqual(result["outcome"], "indeterminate")

    def test_maximum_speed_has_no_paced_ceiling_assertion(self):
        case, metrics = fixture(0); case["mode"] = "maximum-speed"
        result = A.analyze(case, metrics)
        self.assertFalse(result["rates"]["paced_input_limits_decode_only_ceiling"])
        self.assertTrue(result["rates"]["rates_are_observations_not_guaranteed_capacity"])

    def test_duplicate_and_nonfinite_json_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "bad.json"
            for content in ('{"x":1,"x":2}', '{"MAE":NaN}'):
                path.write_text(content)
                with self.assertRaises(ValueError): A.bounded_json(path)


if __name__ == "__main__": unittest.main(verbosity=2)
