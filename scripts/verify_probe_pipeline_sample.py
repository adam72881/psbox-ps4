#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read-only validation and three-file future transfer inventory. No network."""
from __future__ import annotations
import argparse
import hashlib
import json
import pathlib
import generate_probe_pipeline_sample as G
import inventory_probe_samples as I


def verify_files(directory, desc):
    rows = []
    for role, leaf, remote_field, hash_field in (
        ("clip", "clip.h264", "clip_path", "clip_sha256"),
        ("index", "clip.x4au", "index_path", "index_sha256"),
        ("reference", "references.rgba", "reference_path", "reference_sha256")):
        path = directory / leaf
        size, sha = I.sha_file(path)
        if sha != desc[hash_field]: raise ValueError(role + " hash differs from compiled descriptor")
        rows.append({"role": role, "local_path": str(path.resolve()), "remote_path": desc[remote_field],
                     "bytes": size, "sha256": sha})
    return rows


def verify(output: pathlib.Path, header: pathlib.Path):
    manifest = I.bounded_json(output / "manifest.json")
    if len(manifest["cases"]) != 1 or manifest["cases"][0]["name"] != G.CASE[0]:
        raise ValueError("wrong additive case identity")
    case = manifest["cases"][0]
    descriptors = I.read_descriptors(header)
    if len(descriptors) != 1 or descriptors[0]["id"] != G.CASE[0]:
        raise ValueError("wrong compiled descriptor identity")
    desc = descriptors[0]
    directory = output / G.CASE[0]
    rows = verify_files(directory, desc)
    observed = I.bitstream.inspect_file(directory / "clip.h264", 60, 1)
    I.verify_index(directory / "clip.x4au", observed)
    for field in ("sha256", "bytes", "frame_count", "duration_us", "sps", "pps"):
        if observed[field] != case[field]: raise ValueError("actual sample differs from manifest: " + field)
    s, v, crop = observed["sps"], observed["sps"]["vui"], observed["sps"]["crop_offsets_pixels"]
    actual = dict(s, crop_left=crop[0], crop_right=crop[1], crop_top=crop[2], crop_bottom=crop[3],
                  max_dec_frame_buffering=v["max_dec_frame_buffering"], num_reorder_frames=v["max_num_reorder_frames"],
                  matrix_coefficients=v["matrix_coefficients"], full_range=v["full_range"])
    if any(actual.get(field) != desc[field] for field in I.FIELDS):
        raise ValueError("SPS/descriptor disagreement")
    if desc["fps_num"] != 60 or desc["fps_den"] != 1 or desc["reference_count"] != 8 or desc["au_count"] != 600:
        raise ValueError("descriptor cadence/reference count differs")
    refs = case["reference"]["saved_rgba"]
    if [r["frame_ordinal"] for r in refs] != list(range(8)): raise ValueError("missing reference ordinals")
    with (directory / "references.rgba").open("rb") as joined:
        for ref in refs:
            path = directory / ref["path"]
            size, sha = I.sha_file(path)
            if size != 1920 * 1080 * 4 or sha != ref["sha256"]:
                raise ValueError("individual reference hash/size differs")
            with path.open("rb") as source:
                for chunk in iter(lambda: source.read(I.BLOCK), b""):
                    if joined.read(len(chunk)) != chunk: raise ValueError("concatenated reference differs")
        if joined.read(1): raise ValueError("trailing reference bytes")
    rate = G.verify_rate_control(case, (directory / "trace_headers.log").read_text(encoding="utf-8"))
    if rate != case["rate_control_verification"]: raise ValueError("recorded rate control differs")
    return {"schema": "X4PROBE_PIPELINE_SAMPLE_VERIFICATION1", "state": "PASS",
            "case_id": G.CASE[0], "file_count": len(rows), "files": rows,
            "total_bytes": sum(row["bytes"] for row in rows), "rate_control": rate,
            "frame_count": observed["frame_count"], "duration_us": observed["duration_us"],
            "references_byte_compared": len(refs), "index_records_byte_compared": observed["frame_count"],
            "descriptor": {"path": str(header.resolve()), "sha256": I.sha_file(header)[1]},
            "network_executed": False, "console_results": "not executed", "old_files_to_copy": 0}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--samples", required=True, type=pathlib.Path)
    ap.add_argument("--header", required=True, type=pathlib.Path)
    ap.add_argument("--output", required=True, type=pathlib.Path)
    args = ap.parse_args()
    result = verify(args.samples, args.header)
    with args.output.open("x", encoding="utf-8") as target:
        target.write(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)


if __name__ == "__main__": main()
