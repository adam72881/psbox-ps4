#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Offline reader contract: missing/failed/partial stack observations stay unknown."""
import copy
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("reader", ROOT / "scripts/analyze_probe_stress_metrics.py")
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


def observation():
    return dict(name="x4-video", configured_bytes=262144, reported_bytes=262144,
                guard_bytes=4096, painted_bytes=245760, untouched_bytes=221184,
                high_water_bytes=36864, low_excluded_bytes=4096, top_excluded_bytes=12288,
                measure_rc=0, join_rc=0, pattern_ready=True, started=True, finished=True)


def report(item=None):
    return {"thread_stacks": {"schema": "X4PROBE_STACK1", "slots": [item, None, None, None]}}


class StackReports(unittest.TestCase):
    def test_previous_reports_have_no_fabricated_peak(self):
        r = reader.thread_stacks({}, {})
        self.assertEqual(r["status"], "unavailable")
        self.assertFalse(r["peak_is_absolute_proof"])

    def test_valid_pattern_has_explicit_coverage_and_no_main_peak(self):
        r = reader.thread_stacks(report(observation()), report(observation()))
        self.assertEqual(r["threads"][0]["pattern_high_water_bytes"], 36864)
        self.assertFalse(r["main_high_water_measured"])
        self.assertEqual(r["threads"][1]["status"], "not recorded")

    def test_native_query_or_join_failures_keep_peak_unknown(self):
        for field in ("measure_rc", "join_rc"):
            o = observation(); o[field] = -42
            r = reader.thread_stacks(report(o), {})
            self.assertIsNone(r["threads"][0]["pattern_high_water_bytes"])

    def test_unfinished_or_unpainted_keeps_peak_unknown(self):
        for field in ("started", "finished", "pattern_ready"):
            o = observation(); o[field] = False
            self.assertIsNone(reader.thread_stacks(report(o), {})["threads"][0]["pattern_high_water_bytes"])

    def test_boolean_native_return_is_not_success(self):
        o = observation(); o["measure_rc"] = False
        self.assertIsNone(reader.thread_stacks(report(o), {})["threads"][0]["pattern_high_water_bytes"])

    def test_invalid_coverage_rejected(self):
        o = observation(); o["painted_bytes"] += 1
        r = reader.thread_stacks(report(o), {})
        self.assertTrue(r["issues"])
        self.assertIsNone(r["threads"][0]["pattern_high_water_bytes"])

    def test_reached_painted_floor_is_disclosed_as_lower_bound(self):
        o = observation(); o["untouched_bytes"] = 0; o["high_water_bytes"] = 258048
        r = reader.thread_stacks(report(o), {})["threads"][0]
        self.assertTrue(r["painted_floor_reached"])
        self.assertTrue(r["high_water_is_lower_bound"])

    def test_disagreement_disclosed(self):
        a = report(observation()); b = copy.deepcopy(a)
        b["thread_stacks"]["slots"][0]["high_water_bytes"] += 1
        self.assertTrue(reader.thread_stacks(a, b)["issues"])

    def test_invalid_slot_count_is_unavailable(self):
        a = report(observation()); a["thread_stacks"]["slots"].append(None)
        self.assertEqual(reader.thread_stacks(a, {})["status"], "unavailable")


if __name__ == "__main__":
    unittest.main()
