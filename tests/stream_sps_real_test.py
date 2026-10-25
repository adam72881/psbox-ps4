#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare the real parser with independent FFmpeg trace_headers output.

Encoded libx264 streams stay outside the source tree. These public SPS bytes
are sufficient to repeat every comparison without FFmpeg or a GPU installed.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def compare(binary: Path, fixture: dict) -> dict:
    payload = bytes.fromhex(fixture["sps_hex"])
    assert hashlib.sha256(payload).hexdigest() == fixture["sps_sha256"]
    parsed = json.loads(subprocess.check_output([str(binary), payload.hex()], text=True))
    expected = fixture["trace_fields"]
    assert parsed["rc"] == 0 and parsed["syntax_complete"], (fixture["id"], parsed)
    pairs = {
        "profile_idc": "profile_idc", "level_idc": "level_idc",
        "seq_parameter_set_id": "seq_parameter_set_id", "pic_order_cnt_type": "pic_order_cnt_type",
        "log2_max_frame_num_minus4": "log2_max_frame_num_minus4",
        "log2_max_pic_order_cnt_lsb_minus4": "log2_max_pic_order_cnt_lsb_minus4",
        "max_num_ref_frames": "max_num_ref_frames", "frame_mbs_only_flag": "frame_mbs_only_flag",
        "direct_8x8_inference_flag": "direct_8x8_inference_flag", "vui_present": "vui_parameters_present_flag",
        "video_signal_type_present": "video_signal_type_present_flag",
        "colour_description_present": "colour_description_present_flag", "full_range": "video_full_range_flag",
        "matrix_coefficients": "matrix_coefficients", "colour_primaries": "colour_primaries",
        "transfer_characteristics": "transfer_characteristics", "bitstream_restriction_present": "bitstream_restriction_flag",
        "max_dec_frame_buffering": "max_dec_frame_buffering", "num_reorder_frames": "max_num_reorder_frames",
    }
    for actual, reference in pairs.items():
        if reference in expected:
            assert parsed[actual] == expected[reference], (fixture["id"], actual, parsed[actual], expected[reference])
    constraints = sum(expected.get(f"constraint_set{i}_flag", 0) << (7 - i) for i in range(6))
    assert parsed["constraints"] == constraints
    assert parsed["chroma_format_idc"] == expected.get("chroma_format_idc", 1)
    assert parsed["bit_depth_luma"] == expected.get("bit_depth_luma_minus8", 0) + 8
    assert parsed["bit_depth_chroma"] == expected.get("bit_depth_chroma_minus8", 0) + 8
    width = 16 * (1 + expected["pic_width_in_mbs_minus1"])
    height = 16 * (1 + expected["pic_height_in_map_units_minus1"]) * (2 - expected["frame_mbs_only_flag"])
    assert parsed["coded_width"] == width and parsed["coded_height"] == height
    crop = {side: 2 * expected.get(f"frame_crop_{side}_offset", 0) for side in ("left", "right", "top", "bottom")}
    for side, value in crop.items():
        assert parsed[f"crop_{side}"] == value
    assert parsed["visible_width"] == width - crop["left"] - crop["right"]
    assert parsed["visible_height"] == height - crop["top"] - crop["bottom"]
    return {"id": fixture["id"], "source_sha256": fixture["source_sha256"], "sps_sha256": fixture["sps_sha256"], "parsed": parsed}


def main() -> None:
    binary = Path(sys.argv[1]).resolve()
    fixtures = json.loads(Path(__file__).with_name("stream_sps_real_fixtures.json").read_text())
    results = [compare(binary, fixture) for fixture in fixtures["fixtures"]]
    seen_poc = {r["parsed"]["pic_order_cnt_type"] for r in results}
    assert seen_poc == {0, 2}, seen_poc
    assert {r["parsed"]["profile_idc"] for r in results} == {66, 77, 100}
    assert any("000003" in f["sps_hex"] for f in fixtures["fixtures"])
    print(json.dumps({"status": "PASS", "reference": "FFmpeg 7.1 trace_headers", "fixture_count": len(results), "poc_types_real": sorted(seen_poc), "results": results}, indent=2))


if __name__ == "__main__":
    main()
