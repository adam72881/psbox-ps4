#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate synthetic local probe fixtures outside the source tree.

Requires an explicitly selected FFmpeg with libx264, numpy and Pillow. It
never invokes console/network/SDK APIs and refuses to overwrite samples.
Reference storage is bounded: stream-hash all decoded NV12 frames, retain
eight RGBA references and selected PNGs, rather than a multi-GB full decode.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import time
import numpy as np
from PIL import Image
from inspect_probe_bitstream import INDEX_HEADER, INDEX_MAGIC, FILE_MAX, inspect_file, iter_file_aus, nals, rbsp, write_index

SAMPLE_CASES = (
    ("control-720p30-cb", 1280, 720, 30, "baseline", "3.1", 66, 31),
    ("720p60-cb", 1280, 720, 60, "baseline", "3.2", 66, 32),
    ("1080p60-cb", 1920, 1080, 60, "baseline", "4.2", 66, 42),
    ("1080p60-main", 1920, 1080, 60, "main", "4.2", 77, 42),
    ("1080p60-high", 1920, 1080, 60, "high", "4.2", 100, 42),
)


def sample_plan(seconds: int, sustained: bool, names):
    known = {c[0] for c in SAMPLE_CASES}
    if names and any(name not in known for name in names): raise ValueError("unknown sample case")
    if sustained:
        if not 10 <= seconds <= 900 or not names or len(names) != 1:
            raise ValueError("--sustained requires 10..900 seconds and exactly one --case")
    elif not 1 <= seconds <= 10:
        raise ValueError("initial samples limited to 1..10 seconds; use --sustained for one longer fixture")
    return [c for c in SAMPLE_CASES if not names or c[0] in names]


def descriptor_header(manifest: dict, output: pathlib.Path, header: pathlib.Path,
                      *, array_name="x4_probe_cases", count_name="X4_PROBE_CASE_COUNT", extra_labels=None,
                      generator_script="generate_probe_samples.py"):
    """Concatenate bounded references and generate trusted source descriptors.

    The clip/index/reference bytes are rehashed before descriptor publication.
    Native paths are fixed application data paths, never external URLs.
    """
    if not re.fullmatch(r"[a-zA-Z_][a-zA-Z_0-9]*", array_name) or not re.fullmatch(r"[a-zA-Z_][a-zA-Z_0-9]*", count_name):
        raise ValueError("invalid descriptor symbol")
    if not re.fullmatch(r"[a-zA-Z_0-9]+\.py", generator_script): raise ValueError("invalid generator name")
    lines = ["/* SPDX-License-Identifier: GPL-3.0-only */",
             "/* Generated from independently inspected synthetic sample manifests.",
             " * Local candidates only: no native acceptance/performance result.",
             f" * Regenerate with scripts/{generator_script} --descriptors-only. */",
             "#pragma once", '#include "probe_case.h"', "", f"static const X4ProbeCase {array_name}[] = {{"]
    labels = {"control-720p30-cb": "Control 720p30 CB", "720p60-cb": "720p60 CB",
              "1080p60-cb": "1080p60 CB", "1080p60-main": "1080p60 Main", "1080p60-high": "1080p60 High"}
    if extra_labels: labels.update(extra_labels)
    for case in manifest["cases"]:
        name = case["name"]
        if name not in labels: raise ValueError("unknown generated fixture ID")
        directory = output / name
        if sha(directory / "clip.h264") != case["sha256"] or sha(directory / case["index"]["path"]) != case["index"]["sha256"]:
            raise ValueError("clip/index differs from verified sample manifest")
        with (directory / case["index"]["path"]).open("rb") as f:
            index_header = INDEX_HEADER.unpack(f.read(INDEX_HEADER.size))
        if index_header[:3] != (INDEX_MAGIC, 1, 96) or index_header[-1] != bytes.fromhex(case["sha256"]):
            raise ValueError("unexpected index source identity")
        fps_num, fps_den = index_header[10], index_header[11]
        observed = inspect_file(directory / "clip.h264", fps_num, fps_den)
        for field in ("sps", "pps", "frame_count", "duration_us", "bytes", "sha256"):
            if observed[field] != case[field]:
                raise ValueError(f"sample manifest disagrees with actual {field}")
        expected_header = (case["bytes"], case["frame_count"], case["duration_us"],
                           case["sps"]["visible_width"], case["sps"]["visible_height"],
                           case["sps"]["coded_width"], case["sps"]["coded_height"])
        if index_header[3:10] != expected_header:
            raise ValueError("index header differs from actual inspected sample")
        timing = observed["sps"]["vui"]
        if timing["time_scale"] * fps_den != 2 * timing["num_units_in_tick"] * fps_num:
            raise ValueError("index rate differs from verified SPS timing")
        references = case["reference"]["saved_rgba"]
        if len(references) != 8 or [r["frame_ordinal"] for r in references] != list(range(8)):
            raise ValueError("descriptors require eight consecutive decoded references")
        joined, joined_hash, joined_bytes = directory / "references.rgba", hashlib.sha256(), 0
        if joined.exists():
            # Deterministic reruns compare rather than blindly replace.
            joined_data = joined.open("rb")
        else: joined_data = None
        temporary = directory / "references.rgba.partial"
        temporary_owned = False
        try:
            with temporary.open("xb") as target:
                temporary_owned = True
                for ref in references:
                    path = directory / ref["path"]
                    expected_reference_bytes = case["sps"]["visible_width"] * case["sps"]["visible_height"] * 4
                    if path.stat().st_size != expected_reference_bytes or ref["bytes"] != expected_reference_bytes or sha(path) != ref["sha256"]:
                        raise ValueError("reference size/hash differs from visible fixture or manifest")
                    with path.open("rb") as f:
                        for chunk in iter(lambda: f.read(1024 * 1024), b""):
                            target.write(chunk); joined_hash.update(chunk); joined_bytes += len(chunk)
                            if joined_data and joined_data.read(len(chunk)) != chunk:
                                raise ValueError("existing concatenated reference differs")
            if joined_data:
                if joined_data.read(1): raise ValueError("existing concatenated reference has trailing bytes")
                joined_data.close(); joined_data = None
                temporary.unlink()
            else: temporary.rename(joined)
        finally:
            if joined_data: joined_data.close()
            if temporary_owned and temporary.exists(): temporary.unlink()
        joined_meta = {"path": joined.name, "bytes": joined_bytes, "sha256": joined_hash.hexdigest(),
                       "frame_count": 8, "format": "tightly packed visible RGBA8, frame ordinals0..7",
                       "native_path": f"/data/xcloud4-probe/{name}.rgba"}
        case["reference"]["concatenated_rgba"] = joined_meta
        single_path = directory / "manifest.json"
        single = json.loads(single_path.read_text(encoding="utf-8"))
        single["reference"]["concatenated_rgba"] = joined_meta
        single_path.write_text(json.dumps(single, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        s, v, crop = case["sps"], case["sps"]["vui"], case["sps"]["crop_offsets_pixels"]
        def array(hex_value): return "{" + ",".join(f"0x{b:02x}" for b in bytes.fromhex(hex_value)) + "}"
        lines += ["    {", f'        .id = "{name}", .label = "{labels[name]}",',
                  f'        .clip_path = "/data/xcloud4-probe/{name}.h264",',
                  f'        .index_path = "/data/xcloud4-probe/{name}.x4au",',
                  f'        .reference_path = "/data/xcloud4-probe/{name}.rgba",',
                  f'        .clip_sha256 = {array(case["sha256"])},',
                  f'        .index_sha256 = {array(case["index"]["sha256"])},',
                  f'        .reference_sha256 = {array(joined_meta["sha256"])},',
                  f'        .visible_width = {s["visible_width"]}, .visible_height = {s["visible_height"]},',
                  f'        .coded_width = {s["coded_width"]}, .coded_height = {s["coded_height"]},',
                  f'        .crop_left = {crop[0]}, .crop_top = {crop[2]}, .crop_right = {crop[1]}, .crop_bottom = {crop[3]},',
                  f'        .fps_num = {fps_num}, .fps_den = {fps_den},',
                  f'        .frame_mbs_only_flag = {s["frame_mbs_only_flag"]},',
                  f'        .profile_idc = {s["profile_idc"]}, .level_idc = {s["level_idc"]},',
                  f'        .max_num_ref_frames = {s["max_num_ref_frames"]}, .max_dec_frame_buffering = {v["max_dec_frame_buffering"]}, .num_reorder_frames = {v["max_num_reorder_frames"]},',
                  f'        .chosen_dpb_frames = {max(s["max_num_ref_frames"], v["max_dec_frame_buffering"])},',
                  f'        .chroma_format_idc = {s["chroma_format_idc"]}, .bit_depth_luma = {s["bit_depth_luma"]}, .bit_depth_chroma = {s["bit_depth_chroma"]},',
                  f'        .matrix_coefficients = {v["matrix_coefficients"]}, .full_range = {int(v["full_range"])}, .reference_count = 8,',
                  f'        .au_count = {case["frame_count"]}ull, .duration_us = {case["duration_us"]}ull', "    },"]
    lines += ["};", f"#define {count_name} (sizeof({array_name}) / sizeof({array_name}[0]))", ""]
    header.write_text("\n".join(lines), encoding="utf-8")
    return {"path": str(header), "sha256": sha(header), "case_count": len(manifest["cases"])}


def sha(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""): h.update(chunk)
    return h.hexdigest()


def encoder_sei(clip: pathlib.Path) -> str:
    # This is synthetic encoder-identification text, not a copied session's
    # arbitrary SEI payload. Inspection reads only the bounded first AU.
    first = next(iter_file_aus(clip))[1]
    for _, _, nal in nals(first):
        if nal[0] & 31 != 6: continue
        text = rbsp(nal)
        pos = text.find(b"x264 - core")
        if pos >= 0:
            return text[pos:].split(b"\0", 1)[0].decode("ascii")
    raise ValueError("generated libx264 sample lacks encoder-identification SEI")


def run(command, log: pathlib.Path):
    with log.open("wb") as output:
        completed = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT)
    if completed.returncode:
        raise RuntimeError(f"tool exit={completed.returncode}; see {log}")
    return completed


def synthetic_filter(width: int, height: int, fps: int, seconds: int) -> str:
    # Distinct edge colors expose wrong row/plane/crop interpretation. The
    # ten binary stripes encode frame ordinal without an external font.
    items = [f"testsrc2=size={width}x{height}:rate={fps}:duration={seconds}",
             "drawgrid=width=64:height=64:thickness=2:color=white@0.18",
             "drawbox=x=0:y=0:w=iw:h=4:color=red:t=fill",
             "drawbox=x=0:y=ih-4:w=iw:h=4:color=yellow:t=fill",
             "drawbox=x=0:y=4:w=4:h=ih-8:color=blue:t=fill",
             "drawbox=x=iw-4:y=4:w=4:h=ih-8:color=green:t=fill",
             "drawbox=x=32:y=16:w=320:h=32:color=black:t=fill"]
    for bit in range(10):
        items.append(f"drawbox=x={32 + bit * 32}:y=16:w=16:h=32:color=white:t=fill:enable='gte(bitand(n,{1 << bit}),1)'")
    items.append("setparams=range=limited:color_primaries=smpte170m:color_trc=smpte170m:colorspace=smpte170m")
    return ",".join(items)


def production_601_rgba(nv12: bytes, width: int, height: int) -> np.ndarray:
    """Explicit integer limited-range BT.601; matches declared production math.

    FFmpeg-decoded NV12 is the compression reference, not a raw input frame.
    This does not prove the native output is NV12 or establish its plane map.
    """
    y = np.maximum(np.frombuffer(nv12, np.uint8, width * height).reshape(height, width).astype(np.int32) - 16, 0)
    uv = np.frombuffer(nv12, np.uint8, width * height // 2, width * height).reshape(height // 2, width)
    u = uv[:, 0::2].repeat(2, 0).repeat(2, 1).astype(np.int32) - 128
    v = uv[:, 1::2].repeat(2, 0).repeat(2, 1).astype(np.int32) - 128
    out = np.empty((height, width, 4), np.uint8)
    out[:, :, 0] = np.clip((298 * y + 409 * v + 128) >> 8, 0, 255)
    out[:, :, 1] = np.clip((298 * y - 100 * u - 208 * v + 128) >> 8, 0, 255)
    out[:, :, 2] = np.clip((298 * y + 516 * u + 128) >> 8, 0, 255)
    out[:, :, 3] = 255
    return out


def frame_barcode(nv12: bytes, width: int) -> int:
    y = np.frombuffer(nv12, np.uint8, width * 48).reshape(48, width)
    return sum((1 << bit) if y[28:36, 36 + bit * 32:44 + bit * 32].mean() > 128 else 0 for bit in range(10))


def reference_decode(ffmpeg: str, clip: pathlib.Path, out_dir: pathlib.Path, width: int, height: int, expected: int):
    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-threads", "2", "-i", str(clip),
               "-map", "0:v:0", "-pix_fmt", "nv12", "-fps_mode", "passthrough", "-f", "rawvideo", "pipe:1"]
    frame_bytes = width * height * 3 // 2
    selected_png = {0, 1, 7, 30, 120, expected - 1}
    decoded_hash, count, refs, barcode_errors = hashlib.sha256(), 0, [], []
    log = out_dir / "reference-decode.log"
    with log.open("wb") as err:
        p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=err)
        try:
            while True:
                frame = p.stdout.read(frame_bytes)
                if not frame: break
                if len(frame) != frame_bytes:
                    raise RuntimeError("truncated reference decoded frame")
                if count >= expected:
                    raise RuntimeError("reference decoded more frames than indexed")
                decoded_hash.update(frame)
                barcode = frame_barcode(frame, width)
                if barcode != (count & 1023):
                    barcode_errors.append({"frame": count, "barcode": barcode})
                if count < 8 or count in selected_png:
                    rgba = production_601_rgba(frame, width, height)
                    if count < 8:
                        target = out_dir / f"reference-{count:04d}.rgba"
                        target.write_bytes(rgba.tobytes())
                        refs.append({"frame_ordinal": count, "path": target.name,
                                     "bytes": target.stat().st_size, "sha256": sha(target),
                                     "decoded_nv12_sha256": hashlib.sha256(frame).hexdigest(),
                                     "barcode": barcode})
                    if count in selected_png:
                        Image.fromarray(rgba, "RGBA").save(out_dir / f"reference-{count:04d}.png")
                count += 1
        except BaseException:
            p.kill(); p.wait()
            raise
        finally:
            if p.stdout: p.stdout.close()
        rc = p.wait()
    if rc or count != expected or barcode_errors:
        raise RuntimeError(f"reference failure rc={rc} count={count} expected={expected} barcode_errors={barcode_errors[:8]}")
    return {"command": command, "frames": count, "whole_decoded_nv12_sha256": decoded_hash.hexdigest(),
            "whole_decoded_nv12_bytes": frame_bytes * count, "saved_rgba": refs,
            "barcode_errors": barcode_errors, "rgba_matrix": "integer limited-range BT.601, coefficients 298/409/-100/-208/516, round128>>8; production clamps luma contribution max(Y-16,0)",
            "reference_native_layout_claim": "none: reference has visible tightly packed NV12, native coded plane layout remains a separate gate"}


def crosscheck_rgba(ffmpeg: str, clip: pathlib.Path, out_dir: pathlib.Path, width: int, height: int):
    # A second decoder conversion implementation cross-checks our declared
    # integer reference. Nearest chroma sampling mirrors production; no
    # resizing is performed. This remains a host color check, not native proof.
    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-threads", "2", "-i", str(clip),
               "-vf", "scale=flags=neighbor+bitexact:in_color_matrix=bt601:out_color_matrix=bt601:in_range=tv:out_range=pc,format=rgba",
               "-frames:v", "8", "-fps_mode", "passthrough", "-f", "rawvideo", "pipe:1"]
    results, count = [], 0
    with (out_dir / "reference-rgba-crosscheck.log").open("wb") as err:
        p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=err)
        try:
            for count in range(8):
                frame = p.stdout.read(width * height * 4)
                if len(frame) != width * height * 4:
                    raise RuntimeError("incomplete independent RGBA reference")
                other = np.frombuffer(frame, np.uint8).reshape(height, width, 4)
                ours = np.frombuffer((out_dir / f"reference-{count:04d}.rgba").read_bytes(), np.uint8).reshape(height, width, 4)
                delta = np.abs(other[:, :, :3].astype(np.int16) - ours[:, :, :3].astype(np.int16))
                results.append({"frame": count, "max_abs_channel_difference": int(delta.max()),
                                "mean_abs_channel_difference": float(delta.mean()),
                                "channels_over_3": int((delta > 3).sum()), "channel_count": int(delta.size)})
                if not np.all(other[:, :, 3] == 255): raise RuntimeError("independent alpha differs")
            if p.stdout.read(1): raise RuntimeError("unexpected extra RGBA frames")
        except BaseException:
            p.kill(); p.wait(); raise
        finally:
            if p.stdout: p.stdout.close()
        rc = p.wait()
    if rc: raise RuntimeError(f"independent RGBA conversion failed rc={rc}")
    return {"command": command, "frames": results, "native_result": "not executed"}


def trace_header_values(log: pathlib.Path) -> dict:
    wanted = {"profile_idc", "level_idc", "chroma_format_idc", "bit_depth_luma_minus8",
              "bit_depth_chroma_minus8", "max_num_ref_frames", "pic_width_in_mbs_minus1",
              "pic_height_in_map_units_minus1", "frame_mbs_only_flag", "frame_cropping_flag",
              "frame_crop_left_offset", "frame_crop_right_offset", "frame_crop_top_offset",
              "frame_crop_bottom_offset", "num_units_in_tick", "time_scale", "fixed_frame_rate_flag",
              "video_full_range_flag", "colour_primaries", "transfer_characteristics", "matrix_coefficients",
              "max_num_reorder_frames", "max_dec_frame_buffering", "entropy_coding_mode_flag"}
    found = {}
    with log.open(encoding="utf-8", errors="replace") as source:
        for line in source:
            m = re.search(r"\b([a-zA-Z_0-9]+)\s+[01]+\s+=\s+(-?\d+)\s*$", line)
            if m and m[1] in wanted:
                found.setdefault(m[1], set()).add(int(m[2]))
    return {k: sorted(v) for k, v in found.items()}


def validate_observed(result: dict, values: dict, case, seconds: int):
    name, width, height, fps, _, _, profile, level = case
    s, v = result["sps"], result["sps"]["vui"]
    expected = {"profile_idc": profile, "level_idc": level,
                "visible_width": width, "visible_height": height,
                "coded_width": width, "coded_height": (height + 15) & ~15,
                "chroma_format_idc": 1, "bit_depth_luma": 8, "bit_depth_chroma": 8,
                "frame_mbs_only_flag": 1, "max_num_ref_frames": 4}
    for k, value in expected.items():
        if s[k] != value: raise RuntimeError(f"{name}: parsed {k}={s[k]} expected={value}")
    if profile == 66 and not s["constrained_baseline"]:
        raise RuntimeError("encoder did not produce Constrained Baseline")
    if result["frame_count"] != fps * seconds or result["slice_frame_counts"].get("B", 0):
        raise RuntimeError("wrong frame count or reordered B samples")
    if not v["fixed_frame_rate_flag"] or v["time_scale"] != fps * 2 * v["num_units_in_tick"]:
        raise RuntimeError("VUI timing differs from nominal fixture schedule")
    if v["max_num_reorder_frames"] != 0 or v["max_dec_frame_buffering"] != 4:
        raise RuntimeError("unexpected SPS reorder/DPB restrictions")
    if (v["full_range"], v["color_primaries"], v["transfer_characteristics"], v["matrix_coefficients"]) != (False, 6, 6, 6):
        raise RuntimeError("unexpected signaled matrix/range")
    cross = {"profile_idc": profile, "level_idc": level, "max_num_ref_frames": s["max_num_ref_frames"],
             "pic_width_in_mbs_minus1": width // 16 - 1, "pic_height_in_map_units_minus1": s["coded_height"] // 16 - 1,
             "frame_mbs_only_flag": 1, "num_units_in_tick": v["num_units_in_tick"], "time_scale": v["time_scale"],
             "max_num_reorder_frames": 0, "max_dec_frame_buffering": 4, "video_full_range_flag": 0,
             "colour_primaries": 6, "transfer_characteristics": 6, "matrix_coefficients": 6,
             "entropy_coding_mode_flag": int(result["pps"]["entropy_coding_mode_cabac"])}
    if height == 1080: cross["frame_crop_bottom_offset"] = 4
    for k, value in cross.items():
        if values.get(k) != [value]:
            raise RuntimeError(f"independent trace_headers disagreement: {k} {values.get(k)} != {value}")
    return {"independent_header_fields_checked": len(cross), "all_match": True,
            "native_api_profile_idc_candidate": profile, "native_api_max_level_candidate": level,
            "native_api_query_create_decode": "not executed"}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ffmpeg", required=True)
    ap.add_argument("--output", type=pathlib.Path, required=True)
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--sustained", action="store_true", help="prepare one explicitly selected 10..900-second sample; does not run PS4")
    ap.add_argument("--case", action="append", choices=[c[0] for c in SAMPLE_CASES])
    ap.add_argument("--descriptors-only", action="store_true", help="verify existing sample manifests and generate compiled descriptors")
    ap.add_argument("--header", type=pathlib.Path, help="compiled probe_samples.generated.h output; does not copy media into source")
    args = ap.parse_args()
    cases = sample_plan(args.seconds, args.sustained, args.case)
    source_root = pathlib.Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if output == source_root or source_root in output.parents:
        raise ValueError("generated media/references must stay outside source tree")
    output.mkdir(parents=True, exist_ok=True)
    if args.descriptors_only:
        if not args.header: raise ValueError("--descriptors-only requires --header")
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        manifest["generated_descriptor"] = descriptor_header(manifest, output, args.header)
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps(manifest["generated_descriptor"]))
        return
    if (output / "manifest.json").exists():
        raise ValueError("sample manifest already exists; use a fresh output directory or --descriptors-only")
    version = subprocess.check_output([args.ffmpeg, "-version"], text=True)
    manifest = {"tool": {"ffmpeg_path": args.ffmpeg, "sha256": sha(pathlib.Path(args.ffmpeg)), "version": version},
                "synthetic_content": True, "seconds": args.seconds, "sustained": args.sustained,
                "barcode_modulus": 1024, "cases": [], "console_results": "not executed"}
    for case in cases:
        name, width, height, fps, profile, level, _, _ = case
        directory = output / name
        directory.mkdir(exist_ok=False)
        clip, index = directory / "clip.h264", directory / "clip.x4au"
        params = f"aud=1:repeat-headers=1:annexb=1:keyint={fps}:min-keyint={fps}:scenecut=0:ref=4:bframes=0:force-cfr=1"
        command = [args.ffmpeg, "-hide_banner", "-loglevel", "info", "-f", "lavfi", "-i", synthetic_filter(width, height, fps, args.seconds),
                   "-an", "-c:v", "libx264", "-preset", "veryfast", "-crf", "24", "-threads", "2", "-profile:v", profile,
                   "-level:v", level, "-pix_fmt", "yuv420p", "-color_range", "tv", "-colorspace", "smpte170m",
                   "-color_primaries", "smpte170m", "-color_trc", "smpte170m", "-x264-params", params,
                   "-frames:v", str(fps * args.seconds), "-fs", str(FILE_MAX), "-f", "h264", str(clip)]
        started = time.monotonic()
        run(command, directory / "encode.log")
        if clip.stat().st_size > FILE_MAX: raise ValueError("encoded fixture exceeds 2 GiB reader/inspector bound")
        result = inspect_file(clip, fps)
        result["encoder_identification_sei"] = encoder_sei(clip)
        (directory / "encoder-sei.txt").write_text(result["encoder_identification_sei"] + "\n", encoding="ascii")
        write_index(index, result)
        header_command = [args.ffmpeg, "-hide_banner", "-loglevel", "info", "-i", str(clip),
                          "-map", "0:v:0", "-c:v", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"]
        run(header_command, directory / "trace_headers.log")
        values = trace_header_values(directory / "trace_headers.log")
        validation = validate_observed(result, values, case, args.seconds)
        reference = reference_decode(args.ffmpeg, clip, directory, width, height, fps * args.seconds)
        reference["ffmpeg_rgba_crosscheck"] = crosscheck_rgba(args.ffmpeg, clip, directory, width, height)
        result.update({"name": name, "encode_command": command, "trace_header_command": header_command,
                       "trace_header_values": values, "cross_validation": validation, "reference": reference,
                       "index": {"path": index.name, "bytes": index.stat().st_size, "sha256": sha(index),
                                 "format": "X4AU1 version1, header96 including source SHA256; 24-byte offset/size/pts_us records"},
                       "generation_wall_seconds": time.monotonic() - started})
        (directory / "manifest.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        manifest["cases"].append({k: result[k] for k in ["name", "bytes", "sha256", "sps", "pps", "frame_count", "duration_us", "real_bitrate_kbps", "max_au_bytes", "min_au_bytes", "gop_frame_distances", "slice_frame_counts", "cross_validation", "index", "reference", "encoder_identification_sei"]})
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps({"sample": name, "frames": result["frame_count"], "bytes": result["bytes"], "max_au": result["max_au_bytes"], "elapsed_s": result["generation_wall_seconds"]}), flush=True)
    if args.header:
        manifest["generated_descriptor"] = descriptor_header(manifest, output, args.header)
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__": main()
