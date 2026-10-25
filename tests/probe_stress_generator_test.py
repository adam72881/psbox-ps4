#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Pure bounded stress generation policy tests; no encoder or PS4 calls."""
import pathlib
import sys
import tempfile
import unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/"scripts"))
import generate_probe_stress_samples as S


class StressTests(unittest.TestCase):
    def test_separate_three_cases(self):
        self.assertEqual([c[0] for c in S.sample_plan(10,False,None)],["S720-CB","S1080-CB","S1080-HIGH"])
        self.assertEqual([c[0] for c in S.G.SAMPLE_CASES],["control-720p30-cb","720p60-cb","1080p60-cb","1080p60-main","1080p60-high"])

    def test_reject_oversize_duration_selection(self):
        for seconds,sustained,names in [(0,False,None),(11,False,None),(900,True,["S1080-HIGH"]),
            (601,True,["S1080-CB"]),(600,True,None),(600,True,["S1080-CB","S720-CB"]),
            (10,False,["unknown"]),(10,False,["S720-CB","S720-CB"])]:
            with self.subTest(seconds=seconds,names=names),self.assertRaises(ValueError):S.sample_plan(seconds,sustained,names)
        self.assertEqual(len(S.sample_plan(600,True,["S1080-HIGH"])),1)

    def test_600s_budget_not_900s_assumption(self):
        budget=S.sustained_budget(600,20000)
        self.assertEqual(budget["target_bytes"],1500000000)
        self.assertEqual(budget["abr_20_percent_estimate_bytes"],1806250000)
        self.assertEqual(budget["vbv_ceiling_kbps"],25000)
        self.assertEqual(budget["vbv_flow_bytes"],1875000000)
        self.assertEqual(budget["vbv_buffer_bytes"],6250000)
        self.assertEqual(budget["planned_upper_bytes"],1881250000)
        self.assertEqual(budget["remaining_margin_bytes"],266233648)
        self.assertFalse(budget["whole_encoded_size_bound_proven"])
        self.assertLess(S.sustained_budget(900,20000)["remaining_margin_bytes"],0)

    def test_abr_and_vbv_no_deliberate_filler(self):
        for case in S.STRESS_CASES:
            command=S.encode_command("ffmpeg",pathlib.Path("clip"),case,10)
            self.assertNotIn("-crf",command)
            self.assertEqual(command[command.index("-b:v")+1],f"{case[8]}k")
            self.assertEqual(command[command.index("-maxrate")+1],f"{case[8]*5//4}k")
            params=command[command.index("-x264-params")+1]
            for expected in ("ref=4","bframes=0","nal-hrd=vbr","filler=0","keyint=60"):
                self.assertIn(expected,params)
            self.assertNotIn("nal-hrd=cbr",params)
            self.assertIn("noise=",command[command.index("-i")+1])

    def test_nal_accounting_is_ebsp_not_semantic_pixels(self):
        data=b"\x00\x00\x00\x01\x09\xf0\x00\x00\x01\x65\x88\x99\x00\x00\x01\x06\x05\xff"
        with tempfile.TemporaryDirectory() as directory:
            clip=pathlib.Path(directory)/"clip.h264";clip.write_bytes(data);result=S.byte_accounting(clip)
        self.assertEqual(result["vcl_ebsp_bytes"],3)
        self.assertEqual(result["signaling_ebsp_bytes"],5)
        self.assertEqual(result["filler_ebsp_bytes"],0)
        self.assertEqual(result["annexb_prefix_bytes"],10)
        self.assertEqual(sum(result[k] for k in ("vcl_ebsp_bytes","signaling_ebsp_bytes","filler_ebsp_bytes","annexb_prefix_bytes")),len(data))
        self.assertEqual(result["IDR_inputs"],[{"input_ordinal":0,"AU_bytes":len(data),"VCL_EBSP_bytes":3}])
        self.assertIn("not pure semantic",result["accounting_definition"])

    def test_filler_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            clip=pathlib.Path(directory)/"clip.h264";clip.write_bytes(b"\x00\x00\x01\x09\xf0\x00\x00\x01\x0c\xff\x80")
            with self.assertRaises(ValueError):S.byte_accounting(clip)

    def test_existing_full_frame_color_thresholds_not_relaxed(self):
        reference={"ffmpeg_rgba_crosscheck":{"frames":[{"mean_abs_channel_difference":3,"max_abs_channel_difference":16}]*8}}
        self.assertTrue(S.validate_color(reference)["all_passed"])
        for mean,maximum in ((3.001,16),(3,17)):
            reference["ffmpeg_rgba_crosscheck"]["frames"][0]={"mean_abs_channel_difference":mean,"max_abs_channel_difference":maximum}
            with self.assertRaises(ValueError):S.validate_color(reference)
        self.assertEqual(S.percentile_nearest_rank(list(range(1,601)),.95),570)

    def test_encoder_sei_requires_exact_abr_vbr_and_no_filler(self):
        valid="x264 core test options: bframes=0 rc=abr nal_hrd=vbr filler=0 ref=4"
        self.assertEqual(S.validate_encoder_sei(valid),{"rc":"abr","nal_hrd":"vbr","filler":"0"})
        for text in (valid.replace("nal_hrd=vbr","nal_hrd=cbr"),valid.replace("nal_hrd=vbr",""),
            valid.replace("rc=abr","rc=cbr"),valid.replace("filler=0","filler=1"),
            valid+" nal_hrd=cbr",valid.replace("nal_hrd=vbr","fake_nal_hrd=vbr"),
            valid.replace("nal_hrd=vbr","nal_hrd=vbr-extra"),"rc=abr nal_hrd=vbr filler=0"):
            with self.subTest(text=text),self.assertRaises(ValueError):S.validate_encoder_sei(text)


if __name__=="__main__":unittest.main(verbosity=2)
