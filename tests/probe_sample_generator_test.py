#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Pure generation policy/reference math tests; no encoder/console execution."""
import pathlib
import sys
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
import generate_probe_samples as G


class GeneratorTests(unittest.TestCase):
    def test_initial_plan_all_cases_and_exact_selection(self):
        self.assertEqual(len(G.sample_plan(10, False, None)), 5)
        self.assertEqual(G.sample_plan(1, False, ["control-720p30-cb"])[0][0], "control-720p30-cb")

    def test_initial_bounds(self):
        for seconds in (0, 11, 900):
            with self.subTest(seconds=seconds), self.assertRaises(ValueError): G.sample_plan(seconds, False, None)

    def test_sustained_one_case_bounds_without_generating(self):
        for seconds in (10, 900):
            self.assertEqual(len(G.sample_plan(seconds, True, ["720p60-cb"])), 1)
        for seconds, names in [(9, ["720p60-cb"]), (901, ["720p60-cb"]), (900, None),
                               (900, ["720p60-cb", "1080p60-high"]), (900, ["unknown"])]:
            with self.subTest(seconds=seconds, names=names), self.assertRaises(ValueError): G.sample_plan(seconds, True, names)

    def test_limited_601_production_superblack_luma_clamp(self):
        # Four Y=0 pixels, neutral U and V=255. The production clamp must
        # suppress a negative luma contribution before applying chroma.
        frame = bytes([0, 0, 0, 0, 128, 255])
        rgba = G.production_601_rgba(frame, 2, 2)
        self.assertTrue((rgba[:, :, 0] == ((409 * 127 + 128) >> 8)).all())
        self.assertTrue((rgba[:, :, 3] == 255).all())


if __name__ == "__main__": unittest.main(verbosity=2)
