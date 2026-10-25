#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Deterministic inspector syntax/bounds tests; no native decode claim."""
import importlib.util
import pathlib
import struct
import tempfile
import unittest
from unittest import mock

MODULE = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "inspect_probe_bitstream.py"
SPEC = importlib.util.spec_from_file_location("inspector", MODULE)
I = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(I)


class Writer:
    def __init__(self): self.bits = ""
    def u(self, n, value): self.bits += format(value, f"0{n}b"); return self
    def ue(self, value):
        code = format(value + 1, "b")
        self.bits += "0" * (len(code) - 1) + code
        return self
    def finish(self, header):
        bits = self.bits + "1"
        bits += "0" * ((-len(bits)) % 8)
        data = bytes(int(bits[n:n+8], 2) for n in range(0, len(bits), 8))
        escaped, zeros = bytearray(), 0
        for v in data:
            if zeros >= 2 and v <= 3: escaped.append(3); zeros = 0
            escaped.append(v)
            zeros = zeros + 1 if v == 0 else 0
        return bytes([header]) + bytes(escaped)


def sps(profile=66, level=31, width=1280, height=720, crop_bottom=0, refs=4):
    w = Writer().u(8, profile).u(8, 0xc0 if profile == 66 else 0).u(8, level).ue(0)
    if profile == 100: w.ue(1).ue(0).ue(0).u(1, 0).u(1, 0)
    w.ue(0).ue(2).ue(refs).u(1, 0).ue(width // 16 - 1).ue(height // 16 - 1)
    w.u(1, 1).u(1, 1).u(1, bool(crop_bottom))
    if crop_bottom: w.ue(0).ue(0).ue(0).ue(crop_bottom)
    w.u(1, 1).u(1, 0).u(1, 0).u(1, 1).u(3, 5).u(1, 0).u(1, 1)
    w.u(8, 6).u(8, 6).u(8, 6).u(1, 0).u(1, 1).u(32, 1).u(32, 60 if level == 31 else 120)
    w.u(1, 1).u(1, 0).u(1, 0).u(1, 0).u(1, 1).u(1, 1)
    w.ue(0).ue(0).ue(0).ue(0).ue(0).ue(refs)
    return w.finish(0x67)


def pps(cabac=0, sps_id=0, groups=0):
    return Writer().ue(0).ue(sps_id).u(1, cabac).u(1, 0).ue(groups).ue(3).ue(0).finish(0x68)


def slice_nal(idr=True, slice_type=2, pps_id=0):
    return Writer().ue(0).ue(slice_type).ue(pps_id).finish(0x65 if idr else 0x41)


def annex(*items): return b"".join(b"\0\0\0\1" + n for n in items)


def clip(*, n=2, high=False):
    output = annex(b"\x09\xf0", sps(100, 42, 1920, 1088, 4) if high else sps(), pps(high), slice_nal())
    for _ in range(n - 1): output += annex(b"\x09\xf0", slice_nal(False, 0))
    return output


class InspectorTests(unittest.TestCase):
    def test_baseline_sps_dimensions_vui(self):
        r = I.parse_sps(sps())
        self.assertTrue(r["constrained_baseline"])
        self.assertEqual((r["coded_width"], r["coded_height"], r["visible_height"]), (1280, 720, 720))
        self.assertEqual(r["max_num_ref_frames"], 4)
        self.assertEqual(r["vui"]["max_num_reorder_frames"], 0)
        self.assertEqual(r["vui"]["matrix_coefficients"], 6)
        self.assertFalse(r["vui"]["full_range"])

    def test_high_coded_1088_visible_1080(self):
        r = I.parse_sps(sps(100, 42, 1920, 1088, 4))
        self.assertEqual((r["profile_idc"], r["level_idc"]), (100, 42))
        self.assertEqual((r["coded_height"], r["visible_height"]), (1088, 1080))
        self.assertEqual(r["crop_offsets_pixels"], [0, 0, 0, 8])
        self.assertEqual((r["bit_depth_luma"], r["chroma_format_idc"]), (8, 1))

    def test_main_pps_cabac_separate_from_profile(self):
        self.assertEqual(I.parse_sps(sps(77, 42, 1920, 1088, 4))["profile_idc"], 77)
        self.assertTrue(I.parse_pps(pps(1))["entropy_coding_mode_cabac"])
        self.assertFalse(I.parse_pps(pps())["entropy_coding_mode_cabac"])

    def test_all_sps_truncations_rejected(self):
        n = sps(100, 42, 1920, 1088, 4)
        for length in range(len(n) - 1):
            with self.subTest(length=length), self.assertRaises(I.BitstreamError): I.parse_sps(n[:length])

    def test_exp_golomb_excess_and_truncation(self):
        with self.assertRaises(I.BitstreamError): I.Bits(b"\0" * 10).ue()
        with self.assertRaises(I.BitstreamError): I.Bits(b"").u(1)
        with self.assertRaises(I.BitstreamError): I.Bits(b"\xff").u(65)
        with self.assertRaises(I.BitstreamError): I.Bits(b"\x20").ue(1)

    def test_invalid_emulation_prevention(self):
        for data in [b"\x67\0\0\x03", b"\x67\0\0\x03\xff", b"\xe7\x80"]:
            with self.subTest(data=data), self.assertRaises(I.BitstreamError): I.rbsp(data)
        self.assertEqual(I.rbsp(b"\x67\0\0\x03\x01"), b"\0\0\x01")

    def test_annex_empty_garbage_and_empty_nal(self):
        for data in [b"", b"garbage" + clip(), b"\0\0\1", b"\0\0\1\xff"]:
            with self.subTest(data=data), self.assertRaises(I.BitstreamError): list(I.nals(data))

    def test_exact_au_coverage_and_nominal_times(self):
        data = clip(n=3)
        r = I.inspect(data, 60)
        self.assertEqual(r["frame_count"], 3)
        self.assertEqual(r["duration_us"], 50000)
        self.assertEqual([a["pts_us"] for a in r["access_units"]], [0, 16666, 33333])
        self.assertEqual(sum(a["size"] for a in r["access_units"]), len(data))
        self.assertEqual(r["slice_frame_counts"], {"I": 1, "P": 2})

    def test_index_actual_hash_and_96byte_header(self):
        r = I.inspect(clip(), 30)
        with tempfile.TemporaryDirectory() as temp:
            p = pathlib.Path(temp) / "sample.x4au"
            I.write_index(p, r)
            data = p.read_bytes()
            h = I.INDEX_HEADER.unpack(data[:96])
            self.assertEqual((h[0], h[1], h[2]), (I.INDEX_MAGIC, 1, 96))
            self.assertEqual(h[-1], bytes.fromhex(r["sha256"]))
            self.assertEqual(len(data), 96 + 2 * 24)
            self.assertEqual(I.INDEX_RECORD.unpack(data[96:120]), tuple(r["access_units"][0][k] for k in ["offset", "size", "pts_us"]))
            with self.assertRaises(FileExistsError): I.write_index(p, r)

    def test_oversize_au_controlled_rejection(self):
        data = clip(n=1) + b"\xff" * I.AU_MAX
        with self.assertRaisesRegex(I.BitstreamError, "2 MiB"): I.inspect(data, 30)

    def test_pps_unknown_sps_and_groups_rejected(self):
        data = annex(b"\x09\xf0", sps(), pps(sps_id=1), slice_nal())
        with self.assertRaisesRegex(I.BitstreamError, "unknown SPS"): I.inspect(data, 30)
        with self.assertRaises(I.BitstreamError): I.parse_pps(pps(groups=1))

    def test_missing_aud_sps_or_vcl_rejected(self):
        for data in [annex(sps(), pps(), slice_nal()), annex(b"\x09\xf0", pps(), slice_nal()),
                     annex(b"\x09\xf0", sps(), pps())]:
            with self.subTest(data=data), self.assertRaises(I.BitstreamError): I.inspect(data, 30)

    def test_changed_config_duplicate_first_slice_unknown_pps(self):
        for data in [clip(n=1) + annex(sps(level=32)),
                     clip(n=1) + annex(slice_nal()),
                     annex(b"\x09\xf0", sps(), pps(), slice_nal(pps_id=1))]:
            with self.subTest(data=data), self.assertRaises(I.BitstreamError): I.inspect(data, 30)

    def test_invalid_rate(self):
        for n, d in [(0, 1), (30, 0), (240001, 1), (30, 10001)]:
            with self.subTest(n=n, d=d), self.assertRaises(I.BitstreamError): I.inspect(clip(), n, d)

    def test_stream_inspection_identical_to_whole_file(self):
        for data, rate in [(clip(n=1), 30), (clip(n=300), 60), (clip(n=3, high=True), 60)]:
            with tempfile.TemporaryDirectory() as temp:
                path = pathlib.Path(temp) / "clip.h264"
                path.write_bytes(data)
                self.assertEqual(I.inspect_file(path, rate), I.inspect(data, rate))

    def test_stream_aud_split_across_read_chunk(self):
        first = clip(n=1)
        first += b"\xff" * (65534 - len(first))
        data = first + annex(b"\x09\xf0", slice_nal(False, 0))
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp) / "split.h264"
            path.write_bytes(data)
            self.assertEqual(I.inspect_file(path, 60), I.inspect(data, 60))

    def test_stream_oversize_garbage_and_record_limit(self):
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp) / "bad.h264"
            for data in [b"", b"bad" + clip(), clip(n=1) + b"\xff" * I.AU_MAX]:
                path.write_bytes(data)
                with self.subTest(bytes=len(data)), self.assertRaises(I.BitstreamError): I.inspect_file(path, 30)
            path.write_bytes(clip(n=3))
            with mock.patch.object(I, "RECORD_MAX", 2), self.assertRaises(I.BitstreamError): I.inspect_file(path, 30)

    def test_stream_file_size_checked_before_read(self):
        fake = mock.Mock()
        fake.stat.return_value.st_size = I.FILE_MAX + 1
        with self.assertRaises(I.BitstreamError): list(I.iter_file_aus(fake))
        fake.open.assert_not_called()


if __name__ == "__main__": unittest.main(verbosity=2)
