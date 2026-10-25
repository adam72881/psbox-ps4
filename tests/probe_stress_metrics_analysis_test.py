#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Deterministic synthetic reports. No native/console measurement is performed."""
import copy
import json
import pathlib
import sys
import tempfile
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
import analyze_probe_stress_metrics as A


class AnalysisTests(unittest.TestCase):
    EARLY_LAG = [100000] * 24 + [84000, 68000, 52000, 36000, 20000, 4000, 0, 0]

    def fixture(self, values=None, mode="paced"):
        values = self.EARLY_LAG + [0] * 32 if values is None else values
        boundary = 1000001
        first = boundary + 1000 + (values[0] if values else 0)
        last = boundary + 1000 + ((len(values) - 1) * 1000000 // 60 if values else 0) + (values[-1] if values else 0)
        source = "1" * 40
        case = {"schema": "X4PROBE_CASE1", "case_id": "S720-CB", "source_id": source,
                "build_id": source + "-n1-g45-r0-vc0-p1-im", "mode": mode,
                "sample": {"fps_num": 60, "fps_den": 1, "clip_sha256": "2" * 64,
                           "index_sha256": "3" * 64, "reference_sha256": "4" * 64},
                "measurement_start_us": boundary, "observed_warmup_us": boundary - 1,
                "finished": True, "close_result": 0, "pipeline_result": 0,
                "native_outputs": 64, "RGB_conversions": 64, "RGB_publications": 60}
        def counter(n):
            return {"total_observed": n, "stable_observed": n, "timestamped_count": n,
                    "timestamp_omissions": 0, "untimed_stable_units": 0,
                    "measured_first_us": first if n else 0, "measured_last_us": last if n else 0}
        counters = {name: counter(0) for name in A.COUNTER_NAMES}
        counters["input_AU"] = counter(len(values))
        stages = {"scheduled_input_lateness": {"accepted_observations": len(values), "retained": len(values),
                  "overwritten": 0, "stage_omitted": 0, "first_observation_us": first,
                  "last_observation_us": last, "retained_samples_us": values},
                  "decode_call": {"accepted_observations": 0, "retained": 0, "overwritten": 0,
                  "stage_omitted": 0, "first_observation_us": 0, "last_observation_us": 0, "retained_samples_us": []}}
        metrics = {"schema": "X4PROBE_METRICS1", "created_us": 1, "measure_start_us": boundary,
                   "ended_us": max(boundary + 10000000, last + 1000000), "ended": True,
                   "end_reason": 1, "omitted_events": 0, "dirty_resets": 0, "events_after_end": 0,
                   "stages": stages, "counters": counters}
        return case, metrics

    def test_recovery_requires_recent_values_trend_coverage_and_identity(self):
        case, metrics = self.fixture()
        result = A.analyze(case, metrics)
        self.assertEqual(result["admission_lateness"]["verdict"], "admission lag recovery observed in the covered recent window")
        self.assertEqual(result["admission_lateness"]["recent_max_us"], 0)
        self.assertTrue(result["admission_lateness"]["input_endpoint_pacing_consistent"])
        self.assertIsNone(result["sustained_performance_proven"])
        self.assertIsNone(result["warmup"]["stable_segment_proven"])
        self.assertIn("indeterminate", result["whole_session_admission_assessment"])

    def test_no_prior_lag_is_not_called_recovery(self):
        case, metrics = self.fixture([0] * 64)
        self.assertIn("no prior lag recovery demonstrated", A.analyze(case, metrics)["admission_lateness"]["verdict"])

    def test_late_final_value_not_hidden_by_zero_median(self):
        case, metrics = self.fixture(self.EARLY_LAG + [0] * 31 + [50000])
        result = A.analyze(case, metrics)["admission_lateness"]
        self.assertEqual(result["last_retained_quarter_median_us"], 0)
        self.assertEqual(result["recent_exceedances"], 1)
        self.assertIn("recovery not established", result["verdict"])

    def test_increasing_recent_tail_blocks_recovery_despite_low_median(self):
        case, metrics = self.fixture(self.EARLY_LAG + [0] * 16 + [8000] * 16)
        result = A.analyze(case, metrics)["admission_lateness"]
        self.assertEqual(result["recent_exceedances"], 0)
        self.assertGreater(result["recent_median_change_us"], 0)
        self.assertIn("increasing", result["verdict"])

    def test_decreasing_but_still_late_is_not_recovered(self):
        early = [90000] * 24 + [84000, 78000, 72000, 66000, 60000, 54000, 48000, 42000]
        case, metrics = self.fixture(early + [40000] * 32)
        result = A.analyze(case, metrics)["admission_lateness"]
        self.assertLess(result["median_change_us"], 0)
        self.assertEqual(result["recent_exceedances"], 32)
        self.assertIn("recovery not established", result["verdict"])

    def test_short_or_empty_samples_indeterminate_not_zero_statistics(self):
        for values in ([], [0] * 8, [100000] * 8 + [0] * 8, [0] * 63):
            with self.subTest(n=len(values)):
                case, metrics = self.fixture(values)
                result = A.analyze(case, metrics)["admission_lateness"]
                self.assertEqual(result["verdict"], "indeterminate")
                if not values:
                    self.assertIsNone(result["recent_max_us"])

    def test_missing_counters_and_fields_remain_unknown(self):
        case, metrics = self.fixture()
        del metrics["counters"]["errors"]
        del metrics["counters"]["input_AU"]["stable_observed"]
        result = A.analyze(case, metrics)
        self.assertIsNone(result["observed_counters"]["errors"]["total"])
        self.assertIsNone(result["observed_counters"]["input_AU"]["measured"])
        self.assertIn("errors", result["missing_counter_coverage"])
        self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")

    def test_genuine_zero_is_distinct_from_missing(self):
        case, metrics = self.fixture()
        result = A.analyze(case, metrics)
        self.assertEqual(result["recording_omissions"], 0)
        self.assertEqual(result["observed_counters"]["errors"]["total"], 0)
        self.assertEqual(result["observed_counters"]["errors"]["complete_measured_count"], 0)
        self.assertIsNone(result["observed_counters"]["errors"]["first_us"])
        self.assertEqual(result["observed_counters"]["errors"]["timestamps_status"], "no timed events observed")
        self.assertEqual(result["measured_Decode_calls_observed"], 0)
        del metrics["stages"]["decode_call"]
        self.assertIsNone(A.analyze(case, metrics)["measured_Decode_calls_observed"])

    def test_zero_events_with_nonzero_timestamps_do_not_prove_zero(self):
        case, metrics = self.fixture()
        error = metrics["counters"]["errors"]
        error["measured_first_us"] = error["measured_last_us"] = case["measurement_start_us"]
        decode = metrics["stages"]["decode_call"]
        decode["first_observation_us"] = decode["last_observation_us"] = case["measurement_start_us"]
        result = A.analyze(case, metrics)
        self.assertEqual(result["observed_counters"]["errors"]["total"], 0)
        self.assertIsNone(result["observed_counters"]["errors"]["complete_measured_count"])
        self.assertIsNone(result["measured_Decode_calls_observed"])

    def test_missing_global_coverage_is_not_assumed_zero(self):
        for key in ("omitted_events", "dirty_resets", "events_after_end", "ended"):
            with self.subTest(key=key):
                case, metrics = self.fixture(); del metrics[key]
                result = A.analyze(case, metrics)
                self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")
                self.assertIsNone(result["observed_counters"]["errors"]["complete_measured_count"])

    def test_bool_nan_and_wrong_numeric_types_are_unknown(self):
        invalids = [True, False, -1, "0", 0.0, float("nan"), float("inf"), 1 << 64, None]
        for value in invalids:
            with self.subTest(value=value):
                case, metrics = self.fixture(); metrics["omitted_events"] = value
                result = A.analyze(case, metrics)
                self.assertIsNone(result["recording_omissions"])
                self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")
                json.dumps(result, allow_nan=False)

    def test_invalid_native_counts_are_unknown(self):
        for value in (True, -1, float("nan"), "64", None):
            case, metrics = self.fixture(); case["native_outputs"] = value
            self.assertIsNone(A.analyze(case, metrics)["native_total_outputs"])

    def test_invalid_samples_are_not_filtered_or_zeroed(self):
        for value in (True, False, -1, 1.0, float("nan"), "1", 1 << 64):
            with self.subTest(value=value):
                case, metrics = self.fixture(); metrics["stages"]["scheduled_input_lateness"]["retained_samples_us"][3] = value
                result = A.analyze(case, metrics)["admission_lateness"]
                self.assertIsNone(result["sample_count"])
                self.assertIsNone(result["recent_max_us"])
                self.assertEqual(result["verdict"], "indeterminate")

    def test_missing_or_invalid_stage_has_no_coverage_claim(self):
        for value in (None, [], "stage", {"retained_samples_us": [0] * 64}):
            case, metrics = self.fixture(); metrics["stages"]["scheduled_input_lateness"] = value
            result = A.analyze(case, metrics)
            self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")
            self.assertIsNone(result["whole_measured_admission_assessment"]["all_observed_measured_lateness_retained"])

    def test_invalid_counter_accounting_is_unknown(self):
        case, metrics = self.fixture(); metrics["counters"]["input_AU"]["total_observed"] = 1
        result = A.analyze(case, metrics)
        self.assertIsNone(result["observed_counters"]["input_AU"]["total"])
        self.assertIsNone(result["observed_counters"]["input_AU"]["measured"])
        self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")
        case, metrics = self.fixture(); metrics["counters"]["input_AU"]["untimed_stable_units"] = 3
        self.assertIsNone(A.analyze(case, metrics)["observed_counters"]["input_AU"]["timestamped"])

    def test_counter_bool_not_number(self):
        case, metrics = self.fixture(); metrics["counters"]["input_AU"]["timestamped_count"] = True
        result = A.analyze(case, metrics)
        self.assertIsNone(result["observed_counters"]["input_AU"]["timestamped"])
        self.assertIsNone(result["observed_counters"]["input_AU"]["coverage_complete"])
        self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")

    def test_invalid_stage_cannot_create_known_zero_decode(self):
        for field in ("retained", "overwritten", "first_observation_us", "retained_samples_us"):
            case, metrics = self.fixture(); del metrics["stages"]["decode_call"][field]
            self.assertIsNone(A.analyze(case, metrics)["measured_Decode_calls_observed"])

    def test_partial_recording_does_not_claim_native_final_totals(self):
        case, metrics = self.fixture(); metrics["omitted_events"] = 1
        self.assertIsNone(A.analyze(case, metrics)["native_total_outputs"])

    def test_missing_or_unsafe_final_state_does_not_prove_coverage(self):
        for key in ("finished", "close_result", "pipeline_result"):
            case, metrics = self.fixture(); del case[key]
            result = A.analyze(case, metrics)
            self.assertIsNone(result["observed_counters"]["errors"]["coverage_complete"])
            self.assertIsNone(result["measured_Decode_calls_observed"])
        case, metrics = self.fixture(); case["finished"] = False
        result = A.analyze(case, metrics)
        self.assertFalse(result["observed_counters"]["errors"]["coverage_complete"])
        self.assertIsNone(result["observed_counters"]["errors"]["complete_measured_count"])
        case, metrics = self.fixture(); case["close_result"] = -4
        self.assertIsNone(A.analyze(case, metrics)["native_total_outputs"])

    def test_counter_timestamps_outside_phase_not_complete(self):
        case, metrics = self.fixture(); metrics["counters"]["input_AU"]["measured_first_us"] = 1
        result = A.analyze(case, metrics)
        self.assertFalse(result["observed_counters"]["input_AU"]["coverage_complete"])
        self.assertIsNone(result["observed_counters"]["input_AU"]["complete_measured_count"])

    def test_temporal_mismatch_not_silently_joined(self):
        for field in ("measurement_start_us", "observed_warmup_us"):
            case, metrics = self.fixture(); case[field] += 1
            result = A.analyze(case, metrics)
            self.assertEqual(result["temporal_identity"], "inconsistent")
            self.assertIsNone(result["measured_interval_us"])
            self.assertIsNone(result["native_total_outputs"])

    def test_missing_clock_and_bool_clock_unknown(self):
        for field in ("created_us", "measure_start_us", "ended_us"):
            case, metrics = self.fixture(); del metrics[field]
            result = A.analyze(case, metrics)
            self.assertEqual(result["temporal_identity"], "unknown")
            self.assertIsNone(result["measured_interval_us"])
            case, metrics = self.fixture(); metrics[field] = True
            self.assertIsNone(A.analyze(case, metrics)["measured_interval_us"])

    def test_invalid_clock_order_not_clamped_to_zero(self):
        case, metrics = self.fixture(); metrics["ended_us"] = 0
        result = A.analyze(case, metrics)
        self.assertEqual(result["temporal_identity"], "inconsistent")
        self.assertIsNone(result["measured_interval_us"])
        self.assertIsNone(result["warmup"]["observed_us"])

    def test_unknown_and_missing_mode_not_max_speed(self):
        for value in (None, "fast", True):
            case, metrics = self.fixture(); case["mode"] = value
            result = A.analyze(case, metrics)
            self.assertIsNone(result["mode"])
            self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")
        case, metrics = self.fixture(); del case["mode"]
        self.assertEqual(A.analyze(case, metrics)["admission_lateness"]["verdict"], "indeterminate")

    def test_known_max_speed_is_not_paced_recovery(self):
        case, metrics = self.fixture(mode="maximum-speed")
        self.assertEqual(A.analyze(case, metrics)["admission_lateness"]["verdict"], "not applicable to maximum-speed mode")

    def test_bad_cadence_unknown(self):
        for value in (0, True, -1, float("nan"), "60"):
            case, metrics = self.fixture(); case["sample"]["fps_num"] = value
            result = A.analyze(case, metrics)
            self.assertIsNone(result["requested_clip_fps"])
            self.assertIsNone(result["admission_lateness"]["one_nominal_frame_us"])
            self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")

    def test_unknown_identity_does_not_grant_recovery(self):
        for field in ("case_id", "source_id", "build_id"):
            case, metrics = self.fixture(); del case[field]
            result = A.analyze(case, metrics)
            self.assertIsNone(result[field])
            self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")
        case, metrics = self.fixture(); case["sample"]["clip_sha256"] = "secret-not-a-hash"
        result = A.analyze(case, metrics)
        self.assertIsNone(result["sample_hashes"]["clip_sha256"])
        self.assertNotIn("secret-not-a-hash", json.dumps(result))

    def test_overwritten_tail_descriptive_only(self):
        case, metrics = self.fixture([100000] * 2048 + [0] * 2048)
        s = metrics["stages"]["scheduled_input_lateness"]
        s["overwritten"] = 32000; s["accepted_observations"] += 32000
        c = metrics["counters"]["input_AU"]
        c["total_observed"] = c["stable_observed"] = c["timestamped_count"] = s["accepted_observations"]
        result = A.analyze(case, metrics)
        self.assertEqual(result["admission_lateness"]["scope"], "retained tail only")
        self.assertEqual(result["admission_lateness"]["recent_max_us"], 0)
        self.assertIsNone(result["admission_lateness"]["retained_start_timestamp_us"])
        self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")
        self.assertFalse(result["whole_measured_admission_assessment"]["all_observed_measured_lateness_retained"])

    def test_omissions_resets_and_late_events_deny_complete_conclusion(self):
        for field in ("omitted_events", "dirty_resets", "events_after_end"):
            case, metrics = self.fixture(); metrics[field] = 1
            self.assertEqual(A.analyze(case, metrics)["admission_lateness"]["verdict"], "indeterminate")
        case, metrics = self.fixture(); metrics["stages"]["scheduled_input_lateness"]["stage_omitted"] = 1
        self.assertEqual(A.analyze(case, metrics)["admission_lateness"]["verdict"], "indeterminate")

    def test_count_ring_and_timestamp_mismatch_deny_recovery(self):
        for field in ("retained", "accepted_observations", "last_observation_us"):
            case, metrics = self.fixture(); metrics["stages"]["scheduled_input_lateness"][field] += 1
            self.assertEqual(A.analyze(case, metrics)["admission_lateness"]["verdict"], "indeterminate")

    def test_endpoint_pacing_inconsistency_is_not_healthy(self):
        case, metrics = self.fixture()
        metrics["stages"]["scheduled_input_lateness"]["last_observation_us"] += 1000
        metrics["counters"]["input_AU"]["measured_last_us"] += 1000
        result = A.analyze(case, metrics)
        self.assertFalse(result["admission_lateness"]["input_endpoint_pacing_consistent"])
        self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")

    def test_interior_backwards_admission_not_hidden_by_valid_endpoints(self):
        case, metrics = self.fixture()
        metrics["stages"]["scheduled_input_lateness"]["retained_samples_us"][20] = 0
        result = A.analyze(case, metrics)
        self.assertFalse(result["admission_lateness"]["input_admission_pacing_consistent"])
        self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")

    def test_rational_cadence_phase_validates_quantized_microseconds(self):
        values = [16667, 0] + [0] * 63
        case, metrics = self.fixture(values)
        # The second step is 16667 us in this phase, not a rounded constant.
        shift = 1
        metrics["stages"]["scheduled_input_lateness"]["last_observation_us"] += shift
        metrics["counters"]["input_AU"]["measured_last_us"] += shift
        result = A.analyze(case, metrics)
        self.assertTrue(result["admission_lateness"]["input_admission_pacing_consistent"])

    def test_unbounded_cadence_phase_is_unknown_not_healthy(self):
        case, metrics = self.fixture([0] * 64)
        case["sample"]["fps_num"] = 131
        result = A.analyze(case, metrics)
        self.assertIsNone(result["admission_lateness"]["input_admission_pacing_consistent"])
        self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")

    def test_lateness_cannot_exceed_observed_run_lifetime(self):
        case, metrics = self.fixture()
        metrics["stages"]["scheduled_input_lateness"]["retained_samples_us"][5] = 100000000000
        result = A.analyze(case, metrics)
        self.assertIsNone(result["admission_lateness"]["sample_count"])
        self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")

    def test_no_measurement_boundary_is_unknown_not_zero_duration(self):
        case, metrics = self.fixture(); case["measurement_start_us"] = metrics["measure_start_us"] = 0
        case["observed_warmup_us"] = 0
        result = A.analyze(case, metrics)
        self.assertFalse(result["warmup"]["boundary_reached"])
        self.assertIsNone(result["warmup"]["observed_us"])
        self.assertIsNone(result["measured_interval_us"])
        self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")

    def test_safe_cancel_has_covered_admission_facts_not_sustained_proof(self):
        case, metrics = self.fixture(); case["pipeline_result"] = 3; metrics["end_reason"] = 2
        self.assertIn("recovery observed", A.analyze(case, metrics)["admission_lateness"]["verdict"])
        for field in ("close_result", "pipeline_result"):
            case, metrics = self.fixture(); case[field] = -4
            self.assertEqual(A.analyze(case, metrics)["admission_lateness"]["verdict"], "indeterminate")

    def test_invalid_schema_objects_never_manufacture_zero(self):
        for case, metrics in ((None, None), ([], []), ({}, {})):
            result = A.analyze(case, metrics)
            self.assertIsNone(result["measured_interval_us"])
            self.assertIsNone(result["recording_omissions"])
            self.assertIsNone(result["measured_Decode_calls_observed"])
            self.assertEqual(result["admission_lateness"]["verdict"], "indeterminate")

    def test_original_input_objects_are_not_mutated(self):
        case, metrics = self.fixture(); before = copy.deepcopy((case, metrics))
        A.analyze(case, metrics)
        self.assertEqual((case, metrics), before)

    def test_json_nan_rejected_and_unknown_output_is_standard_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "diagnostic.json"
            path.write_text('{"omitted_events":NaN}')
            with self.assertRaises(ValueError): A.read_json(path)
        case, metrics = self.fixture(); metrics["dirty_resets"] = float("nan")
        text = json.dumps(A.analyze(case, metrics), allow_nan=False)
        self.assertNotIn("NaN", text)
        self.assertIn("null", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
