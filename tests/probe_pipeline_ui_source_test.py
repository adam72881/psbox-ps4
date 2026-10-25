#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Probe controller wiring; C test covers deterministic selection and gates."""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/core/probe_controller.c").read_text(encoding="utf-8")


class ProbePipelineUiTests(unittest.TestCase):
    def test_probe_off_entrypoint_unchanged(self):
        self.assertTrue(SOURCE.rstrip().endswith(
            "#else\nint x4_probe_controller_run(void) { return -1; }\n#endif"))
        self.assertIn('#if X4_VIDEO_PROBE\n#include "lifecycle.h"', SOURCE)
        self.assertIn("X4ProbePipelineChoice pipeline_choice = {0}", SOURCE)

    def test_choice_change_cannot_launch_same_frame(self):
        start = SOURCE.index("if (!files_view && !memory_view && !retained && !exit_requested && (pad.pressed & ORBIS_PAD_BUTTON_L2))")
        end = SOURCE.index("if (!exit_requested && !moved && (pad.pressed & ORBIS_PAD_BUTTON_CROSS))")
        body = SOURCE[start:end]
        self.assertIn("moved = true", body)
        self.assertIn("x4_probe_pipeline_choice_change", body)

    def test_runtime_requires_files_then_own_compatibility(self):
        body = SOURCE[SOURCE.index("if (!exit_requested && !moved && (pad.pressed & ORBIS_PAD_BUTTON_CROSS))"):]
        self.assertLess(body.index("if (!files_checked)"), body.index("x4_probe_pipeline_run("))
        self.assertLess(body.index("x4_probe_pipeline_choice_allowed("), body.index("x4_probe_pipeline_run("))
        self.assertLess(body.index("x4_display_close(&display)"), body.index("x4_probe_pipeline_run("))
        self.assertIn("x4_probe_pipeline_converters(run_choice), &pad", body)
        self.assertIn("result.compatibility_passed && !result.report_result", body)
        self.assertIn("continue; /* Never launch a second route on the same X edge. */", body)

    def test_retained_runtime_never_reopens_or_releases(self):
        start = SOURCE.index("if (result.retained)")
        end = SOURCE.index("open_display_or_wait(&display)", start)
        body = SOURCE[start:end]
        self.assertIn("for (;;) sceKernelUsleep(100000)", body)
        self.assertNotIn("x4_exit_request", body)
        self.assertNotIn("x4_display_close", body)
        self.assertNotIn("free(", body)

    def test_legacy_memory_choice_is_not_experimental_memory(self):
        self.assertIn("if (pipeline_choice.selected != X4_PROBE_PIPELINE_CURRENT)", SOURCE)
        self.assertIn("x4_live_media_probe_create_memory(active, active_mode, metrics,", SOURCE)
        self.assertIn("x4_probe_memory_choice_type(active_memory_choice)", SOURCE)
        self.assertIn("Salida WB_ONION (0)", SOURCE)

    def test_new_failure_does_not_stop_current(self):
        start = SOURCE.index("if (result.stopped)")
        end = SOURCE.index("continue; /* Never launch a second route", start)
        body = SOURCE[start:end]
        self.assertIn("x4_probe_pipeline_choice_failed(&pipeline_choice, run_choice)", body)
        self.assertNotIn("session_stopped = true", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
