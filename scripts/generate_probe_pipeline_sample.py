#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Create the one additive 15 Mbps fixture; never changes the eight old clips.

Same synthetic source as compatibility fixtures, ten seconds at 1080p60 High
4.2. CBR/VBV describes this raw H.264 fixture only, not network consumption.
No console, SDK or network access. All generated media stays outside source.
"""
from __future__ import annotations
import argparse
import json
import pathlib
import re
import subprocess
import time
import generate_probe_samples as G
from inspect_probe_bitstream import FILE_MAX, inspect_file, write_index

CASE = ("1080p60-high-15M", 1920, 1080, 60, "high", "4.2", 100, 42)
SECONDS = 10
LABEL = "1080p60 High · 15 Mbps"
RATE_BPS = 15_000_000
VBV_BITS = 15_000_000


def encode_command(ffmpeg, clip):
    _, width, height, fps, profile, level, _, _ = CASE
    params = (f"aud=1:repeat-headers=1:annexb=1:keyint={fps}:min-keyint={fps}:"
              "scenecut=0:ref=4:bframes=0:force-cfr=1:nal-hrd=cbr:filler=1:vbv-init=0.9")
    return [str(ffmpeg), "-hide_banner", "-loglevel", "info", "-f", "lavfi", "-i",
            G.synthetic_filter(width, height, fps, SECONDS), "-an", "-c:v", "libx264",
            "-preset", "veryfast", "-b:v", str(RATE_BPS), "-minrate", str(RATE_BPS),
            "-maxrate", str(RATE_BPS), "-bufsize", str(VBV_BITS), "-threads", "2",
            "-profile:v", profile, "-level:v", level, "-pix_fmt", "yuv420p",
            "-color_range", "tv", "-colorspace", "smpte170m", "-color_primaries", "smpte170m",
            "-color_trc", "smpte170m", "-x264-params", params, "-frames:v", str(fps * SECONDS),
            "-fs", str(FILE_MAX), "-f", "h264", str(clip)]


def verify_rate_control(result, trace_text):
    sei = result["encoder_identification_sei"]
    required = {"rc": "cbr", "bitrate": "15000", "vbv_maxrate": "15000",
                "vbv_bufsize": "15000", "nal_hrd": "cbr", "filler": "1",
                "ref": "4", "bframes": "0"}
    for key, value in required.items():
        if not re.search(rf"(?:^|\s){re.escape(key)}={re.escape(value)}(?:\s|$)", sei):
            raise ValueError(f"encoder-identification SEI missing {key}={value}")
    fields = {}
    wanted = {"nal_hrd_parameters_present_flag", "bit_rate_scale", "cpb_size_scale",
              "bit_rate_value_minus1[0]", "cpb_size_value_minus1[0]", "cbr_flag[0]"}
    for line in trace_text.splitlines():
        match = re.search(r"\b([a-zA-Z_0-9]+(?:\[\d+\])?)\s+[01]+\s+=\s+(\d+)\s*$", line)
        if match and match[1] in wanted: fields.setdefault(match[1], set()).add(int(match[2]))
    if any(len(values) != 1 for values in fields.values()) or set(fields) != wanted:
        raise ValueError("missing or inconsistent independent HRD fields")
    field = {key: next(iter(values)) for key, values in fields.items()}
    hrd_rate = (field["bit_rate_value_minus1[0]"] + 1) << (6 + field["bit_rate_scale"])
    hrd_buffer = (field["cpb_size_value_minus1[0]"] + 1) << (4 + field["cpb_size_scale"])
    if field["nal_hrd_parameters_present_flag"] != 1 or field["cbr_flag[0]"] != 1:
        raise ValueError("bitstream does not signal CBR HRD")
    if hrd_rate != RATE_BPS or hrd_buffer != VBV_BITS:
        raise ValueError("signaled HRD values differ from the requested rate/buffer")
    measured_bps = result["bytes"] * 8_000_000 / result["duration_us"]
    if not 14_000_000 <= measured_bps <= 16_000_000:
        raise ValueError("measured raw H.264 rate lies outside 14..16 Mbps")
    return {"requested_bps": RATE_BPS, "requested_VBV_bits": VBV_BITS,
            "requested_VBV_initial_fullness": 0.9, "HRD_bps": hrd_rate,
            "HRD_CPB_bits": hrd_buffer, "measured_raw_H264_bps": measured_bps,
            "measured_raw_H264_decimal_GB_per_hour": measured_bps * 3600 / 8 / 1e9,
            "HRD_fields": field, "includes_filler_SEI_SPS_PPS_and_AUD": True,
            "excludes_audio_RTP_SRTP_RTCP_transport_headers_and_retransmissions": True,
            "network_or_streaming_traffic_measurement": False}


def descriptor(manifest, output, header):
    return G.descriptor_header(manifest, output, header,
        array_name="x4_probe_pipeline_cases", count_name="X4_PROBE_PIPELINE_CASE_COUNT",
        extra_labels={CASE[0]: LABEL}, generator_script="generate_probe_pipeline_sample.py")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ffmpeg", required=True, type=pathlib.Path)
    ap.add_argument("--output", required=True, type=pathlib.Path)
    ap.add_argument("--header", required=True, type=pathlib.Path)
    ap.add_argument("--descriptors-only", action="store_true")
    args = ap.parse_args()
    output = args.output.resolve()
    source = pathlib.Path(__file__).resolve().parents[1]
    if output == source or source in output.parents:
        raise ValueError("generated media must stay outside source")
    if args.descriptors_only:
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        if [case["name"] for case in manifest["cases"]] != [CASE[0]]:
            raise ValueError("unexpected additive sample manifest")
        manifest["generated_descriptor"] = descriptor(manifest, output, args.header)
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        return
    output.mkdir(parents=True, exist_ok=False)
    directory = output / CASE[0]
    directory.mkdir()
    clip, index = directory / "clip.h264", directory / "clip.x4au"
    started = time.monotonic()
    command = encode_command(args.ffmpeg, clip)
    G.run(command, directory / "encode.log")
    result = inspect_file(clip, CASE[3])
    result["encoder_identification_sei"] = G.encoder_sei(clip)
    write_index(index, result)
    header_command = [str(args.ffmpeg), "-hide_banner", "-loglevel", "info", "-i", str(clip),
                      "-map", "0:v:0", "-c:v", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"]
    G.run(header_command, directory / "trace_headers.log")
    values = G.trace_header_values(directory / "trace_headers.log")
    validation = G.validate_observed(result, values, CASE, SECONDS)
    bitrate = verify_rate_control(result, (directory / "trace_headers.log").read_text(encoding="utf-8"))
    reference = G.reference_decode(str(args.ffmpeg), clip, directory, CASE[1], CASE[2], CASE[3] * SECONDS)
    reference["ffmpeg_rgba_crosscheck"] = G.crosscheck_rgba(str(args.ffmpeg), clip, directory, CASE[1], CASE[2])
    result.update({"name": CASE[0], "encode_command": command, "trace_header_command": header_command,
        "trace_header_values": values, "cross_validation": validation, "reference": reference,
        "rate_control_verification": bitrate, "index": {"path": index.name,
        "bytes": index.stat().st_size, "sha256": G.sha(index), "format": "X4AU1 version1 header96 + records24"},
        "generation_wall_seconds": time.monotonic() - started})
    (directory / "manifest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    manifest = {"schema": "X4PROBE_PIPELINE_SAMPLE1", "synthetic_content": True,
        "source_pattern": "generate_probe_samples.synthetic_filter (unchanged compatibility pattern)",
        "tool": {"ffmpeg_path": str(args.ffmpeg), "sha256": G.sha(args.ffmpeg),
                 "version": subprocess.check_output([str(args.ffmpeg), "-version"], text=True)},
        "seconds": SECONDS, "cases": [result], "console_results": "not executed",
        "old_eight_samples_regenerated": False}
    manifest["generated_descriptor"] = descriptor(manifest, output, args.header)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"case": CASE[0], "frames": result["frame_count"], "bytes": result["bytes"],
                      "rate_control": bitrate, "reference_count": len(reference["saved_rgba"]),
                      "header": manifest["generated_descriptor"]}), flush=True)


if __name__ == "__main__": main()
