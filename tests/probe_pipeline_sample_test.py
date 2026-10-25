#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Offline generation controls and deterministic corrupt-index/hash checks."""
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import generate_probe_pipeline_sample as G
import inventory_probe_samples as I
import verify_probe_pipeline_sample as V


class PipelineSampleTests(unittest.TestCase):
    def test_fixed_rate_source_profile_and_schedule(self):
        command = G.encode_command("ffmpeg", pathlib.Path("clip.h264"))
        self.assertIn(G.G.synthetic_filter(1920, 1080, 60, 10), command)
        for key, value in (("-b:v", "15000000"), ("-minrate", "15000000"),
                           ("-maxrate", "15000000"), ("-bufsize", "15000000"),
                           ("-profile:v", "high"), ("-level:v", "4.2"), ("-frames:v", "600")):
            self.assertEqual(command[command.index(key) + 1], value)
        params = command[command.index("-x264-params") + 1]
        for token in ("ref=4", "bframes=0", "nal-hrd=cbr", "filler=1", "vbv-init=0.9"):
            self.assertIn(token, params.split(":"))

    def test_old_default_plan_stays_five_compatibility_samples(self):
        self.assertEqual(len(G.G.sample_plan(10, False, None)), 5)
        self.assertNotIn(G.CASE[0], [case[0] for case in G.G.SAMPLE_CASES])

    def test_descriptor_names_reject_code_injection(self):
        for args in ({"array_name": "x;"}, {"count_name": "#bad"}, {"generator_script": "../bad.py"}):
            with self.assertRaises(ValueError):
                G.G.descriptor_header({}, pathlib.Path("."), pathlib.Path("bad.h"), **args)

    def test_index_rejects_identity_offset_size_pts_and_trailing_bytes(self):
        observed = {"bytes": 64, "frame_count": 2, "duration_us": 33333, "fps_num": 60, "fps_den": 1,
            "sps": {"visible_width": 1920, "visible_height": 1080, "coded_width": 1920, "coded_height": 1088},
            "sha256": "12" * 32, "access_units": [{"offset": 0, "size": 32, "pts_us": 0},
                                                    {"offset": 32, "size": 32, "pts_us": 16666}]}
        head = I.HEAD.pack(b"X4AU1\0\0\0", 1, 96, 64, 2, 33333, 1920, 1080, 1920, 1088, 60, 1, bytes.fromhex("12" * 32))
        good = head + I.REC.pack(0, 32, 0) + I.REC.pack(32, 32, 16666)
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp) / "clip.x4au"
            path.write_bytes(good)
            I.verify_index(path, observed)
            for offset in (64, 96, 104, 112):
                bad = bytearray(good); bad[offset] ^= 1; path.write_bytes(bad)
                with self.assertRaises(ValueError): I.verify_index(path, observed)
            path.write_bytes(good + b"extra")
            with self.assertRaises(ValueError): I.verify_index(path, observed)

    def test_clip_index_and_reference_tamper_block_validation(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = pathlib.Path(temp)
            desc = {}
            roles = (("clip", "clip.h264"), ("index", "clip.x4au"), ("reference", "references.rgba"))
            for role, leaf in roles:
                path = directory / leaf; path.write_bytes(bytes(range(256)))
                desc[role + "_sha256"] = I.sha_file(path)[1]
                desc[role + "_path"] = "/data/xcloud4-probe/" + leaf
            self.assertEqual(len(V.verify_files(directory, desc)), 3)
            for role, leaf in roles:
                path = directory / leaf
                path.write_bytes(b"\xff" + bytes(range(1, 256)))
                with self.assertRaisesRegex(ValueError, role + " hash differs"):
                    V.verify_files(directory, desc)
                path.write_bytes(bytes(range(256)))


if __name__ == "__main__": unittest.main(verbosity=2)
