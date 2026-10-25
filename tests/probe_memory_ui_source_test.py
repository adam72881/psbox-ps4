#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Bounded UI wiring checks; executable selection tests live in the C test."""
import pathlib
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'src/core/probe_controller.c').read_text(encoding='utf-8')
class ProbeMemoryUiTests(unittest.TestCase):
    def test_memory_action_precedes_no_decoder_creation(self):
        body = SOURCE[SOURCE.index('if (!exit_requested && !retained && !session_stopped && !files_view && !memory_view &&'):SOURCE.index('if (!exit_requested && !moved && (pad.pressed & ORBIS_PAD_BUTTON_CROSS))')]
        self.assertIn('moved = true', body)
        self.assertIn('if (!files_checked)', body)
        self.assertIn('x4_probe_memory_run', body)
        self.assertIn('x4_probe_memory_export_json', body)
        self.assertNotIn('x4_live_media_probe_create', body)
        self.assertIn('if (!memory_check.result.cleanup_safe) retained = true', body)
        self.assertIn('memory_rc && memory_rc != X4_PROBE_MEMORY_CANCELLED', body)
        self.assertNotIn('memory_rc && !memory_check.result.cancelled', body)
    def test_memory_toggle_blocks_same_frame_start(self):
        body = SOURCE[SOURCE.index('if (!files_view && !memory_view && !retained && !exit_requested'):SOURCE.index('if (!exit_requested && !moved && (pad.pressed & ORBIS_PAD_BUTTON_CROSS))')]
        self.assertIn('ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1', body)
        self.assertIn('moved = true', body)
        self.assertIn('x4_probe_memory_choice_change', body)
    def test_reports_identify_memory(self):
        self.assertIn('output_memory_type', SOURCE)
        self.assertIn('output_memory_protection', SOURCE)
        self.assertIn('output_memory_name', SOURCE)
        self.assertIn('x4_live_media_probe_create_memory(active, active_mode, metrics,', SOURCE)
        self.assertIn('active_memory_choice * X4_PROBE_CASE_COUNT + active_index', SOURCE)
    def test_default_info_nonblocking_except_retention(self):
        self.assertIn('17. INFO atributos NULL', SOURCE)
        self.assertIn('x4_probe_default_stack_check', SOURCE)
        self.assertIn('if (stack_check.default_retained) { retained = true; files_checked = false; }', SOURCE)
        self.assertNotIn('files_checked = stack_check.default_result', SOURCE)
        self.assertIn('default_thread_result', SOURCE)
        self.assertIn('default_thread_retained', SOURCE)
    def test_cacheable_failure_does_not_remove_current(self):
        body = SOURCE[SOURCE.index('if (!stack_failed && active_memory_choice == X4_PROBE_MEMORY_CHOICE_CACHEABLE && variant_failed'):SOURCE.index('} else if (!stack_failed && x4_probe_case_stops_session')]
        self.assertIn('memory_choice.blocked[X4_PROBE_MEMORY_CHOICE_CACHEABLE] = true', body)
        self.assertIn('memory_choice.selected = X4_PROBE_MEMORY_CHOICE_CURRENT', body)
        self.assertNotIn('session_stopped = true', body)
    def test_normal_failure_and_stack_failure_stop_session(self):
        self.assertIn('if (stack_failed) session_stopped = true', SOURCE)
        body = SOURCE[SOURCE.index('} else if (!stack_failed && x4_probe_case_stops_session'):SOURCE.index('x4_probe_metrics_free(metrics); metrics = NULL;')]
        self.assertIn('session_stopped = true', body)
        self.assertIn('Rechazo por presupuesto, no incompatibilidad', body)
if __name__ == '__main__': unittest.main(verbosity=2)
