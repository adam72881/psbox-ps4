#!/usr/bin/env python3
"""Canceled native depth retry is a one-attempt history, not executed fallback."""
from pathlib import Path
import importlib.util
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("depth_reader",ROOT/"scripts/analyze_stream_pipeline.py")
reader=importlib.util.module_from_spec(spec);spec.loader.exec_module(reader)
class CancelledFallback(unittest.TestCase):
    def test_query_and_create_rejection_cancel_before_retry(self):
        for at_create in (False,True):
            with self.subTest(create=at_create):
                a=dict(kind=0,depth=2,fallback=False,skipped=False,level_known=True,
                    profile=100,level=42,dpb_minimum=4,dpb_requested=4,dpb_level_cap=4,dpb=4,
                    reason=0,failure_reason=7,failure_stage="CREATE DECODER" if at_create else "QUERY DECODER",
                    query=0 if at_create else -220,create=-221 if at_create else reader.NOT_RUN,cleanup=0)
                s=dict(attempts=[a],attempt_count=1,active_attempt=0,fallback_used=False,
                    requested_depth=2,active_depth=2,depth_fallback_used=False)
                reader.validate_attempts(s,5)
                s['depth_fallback_used']=True
                with self.assertRaisesRegex(ValueError,'depth fallback flag'):
                    reader.validate_attempts(s,5)
if __name__=="__main__":unittest.main()
