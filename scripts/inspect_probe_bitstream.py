#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Bounded independent Annex-B/SPS/PPS/AUD inspector for local probe samples.

This is not a general H.264 validator or a native capability query. Generated
fixtures require AUD, one SPS/PPS configuration, progressive 8-bit 4:2:0 and
one first slice per AU; unsupported syntax is rejected rather than guessed.
"""
from __future__ import annotations
import argparse
import collections
import hashlib
import json
import pathlib
import re
import struct

AU_MAX = 2 * 1024 * 1024
FILE_MAX = 2 * 1024 * 1024 * 1024
RECORD_MAX = 90000
INDEX_HEADER = struct.Struct("<8sIIQQQIIIIII32s")
INDEX_RECORD = struct.Struct("<QQQ")
INDEX_MAGIC = b"X4AU1\0\0\0"


class BitstreamError(ValueError):
    pass


class Bits:
    def __init__(self, data: bytes):
        self.data, self.pos = data, 0

    def u(self, n: int) -> int:
        if n < 0 or n > 64 or self.pos + n > len(self.data) * 8:
            raise BitstreamError("truncated or excessive bit field")
        result = 0
        for _ in range(n):
            result = (result << 1) | ((self.data[self.pos // 8] >> (7 - self.pos % 8)) & 1)
            self.pos += 1
        return result

    def ue(self, maximum: int = (1 << 31) - 1) -> int:
        zeros = 0
        while not self.u(1):
            zeros += 1
            if zeros > 31:
                raise BitstreamError("excessive Exp-Golomb prefix")
        result = (1 << zeros) - 1 + self.u(zeros)
        if result > maximum:
            raise BitstreamError("Exp-Golomb value outside bound")
        return result

    def se(self, maximum: int = 1 << 30) -> int:
        code = self.ue(maximum * 2)
        return (code + 1) // 2 if code & 1 else -(code // 2)


def rbsp(nal: bytes) -> bytes:
    if not nal or nal[0] & 0x80:
        raise BitstreamError("invalid NAL header")
    result, zeros = bytearray(), 0
    i = 1
    while i < len(nal):
        v = nal[i]
        if zeros >= 2 and v == 3:
            if i + 1 >= len(nal) or nal[i + 1] > 3:
                raise BitstreamError("invalid emulation-prevention byte")
            zeros = 0
            i += 1
            continue
        result.append(v)
        zeros = zeros + 1 if v == 0 else 0
        i += 1
    return bytes(result)


def nals(data: bytes):
    if not data or len(data) > FILE_MAX:
        raise BitstreamError("empty or oversized Annex-B file")
    starts = list(re.finditer(b"\x00\x00(?:\x00)?\x01", data))
    if not starts or starts[0].start() != 0:
        raise BitstreamError("Annex-B must begin with a start code")
    if len(starts) > RECORD_MAX * 32:
        raise BitstreamError("excessive NAL count")
    for i, start in enumerate(starts):
        end = starts[i + 1].start() if i + 1 < len(starts) else len(data)
        payload = data[start.end():end]
        if not payload or payload[0] & 0x80 or not 1 <= (payload[0] & 31) <= 23:
            raise BitstreamError("empty or unsupported NAL")
        yield start.start(), end, payload


def _scaling_list(b: Bits, count: int):
    last, following = 8, 8
    for _ in range(count):
        if following:
            following = (last + b.se(128) + 256) % 256
        last = following or last


def _hrd(b: Bits):
    count = b.ue(31)
    b.u(4); b.u(4)
    for _ in range(count + 1):
        b.ue(); b.ue(); b.u(1)
    b.u(5); b.u(5); b.u(5); b.u(5)


def parse_sps(nal: bytes) -> dict:
    if not nal or nal[0] & 31 != 7 or len(nal) > 65536:
        raise BitstreamError("not a bounded SPS")
    b = Bits(rbsp(nal))
    profile, constraints, level, sps_id = b.u(8), b.u(8), b.u(8), b.ue(31)
    chroma, separate, depth_luma, depth_chroma = 1, 0, 8, 8
    if profile in {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135}:
        chroma = b.ue(3)
        if chroma == 3:
            separate = b.u(1)
        depth_luma, depth_chroma = 8 + b.ue(6), 8 + b.ue(6)
        b.u(1)
        if b.u(1):
            for i in range(8 if chroma != 3 else 12):
                if b.u(1):
                    _scaling_list(b, 16 if i < 6 else 64)
    log2_frame_num = b.ue(12) + 4
    poc_type = b.ue(2)
    log2_poc, delta_always_zero = None, None
    if poc_type == 0:
        log2_poc = b.ue(12) + 4
    elif poc_type == 1:
        delta_always_zero = b.u(1)
        b.se(); b.se()
        for _ in range(b.ue(255)):
            b.se()
    refs = b.ue(16)
    gaps_allowed = b.u(1)
    width_mbs, height_units = b.ue(1023) + 1, b.ue(1023) + 1
    frame_only = b.u(1)
    mb_adaptive = b.u(1) if not frame_only else 0
    b.u(1)
    crop_flag = b.u(1)
    crop = [b.ue(16383) for _ in range(4)] if crop_flag else [0, 0, 0, 0]
    chroma_array = 0 if separate else chroma
    sub_width = 1 if chroma_array in {0, 3} else 2
    sub_height = 2 if chroma_array == 1 else 1
    crop_unit_x = 1 if chroma_array == 0 else sub_width
    crop_unit_y = (2 - frame_only) * (1 if chroma_array == 0 else sub_height)
    coded_w, coded_h = width_mbs * 16, height_units * 16 * (2 - frame_only)
    crop_px = [crop[0] * crop_unit_x, crop[1] * crop_unit_x,
               crop[2] * crop_unit_y, crop[3] * crop_unit_y]
    visible_w, visible_h = coded_w - sum(crop_px[:2]), coded_h - sum(crop_px[2:])
    if visible_w <= 0 or visible_h <= 0:
        raise BitstreamError("invalid crop extent")
    vui = {"present": bool(b.u(1)), "color_primaries": None,
           "transfer_characteristics": None, "matrix_coefficients": None,
           "full_range": None, "num_units_in_tick": None, "time_scale": None,
           "fixed_frame_rate_flag": None, "max_num_reorder_frames": None,
           "max_dec_frame_buffering": None}
    if vui["present"]:
        if b.u(1):
            aspect = b.u(8)
            if aspect == 255:
                b.u(16); b.u(16)
        if b.u(1): b.u(1)
        if b.u(1):
            b.u(3)
            vui["full_range"] = bool(b.u(1))
            if b.u(1):
                vui["color_primaries"] = b.u(8)
                vui["transfer_characteristics"] = b.u(8)
                vui["matrix_coefficients"] = b.u(8)
        if b.u(1): b.ue(5); b.ue(5)
        if b.u(1):
            vui["num_units_in_tick"], vui["time_scale"] = b.u(32), b.u(32)
            vui["fixed_frame_rate_flag"] = bool(b.u(1))
            if not vui["num_units_in_tick"] or not vui["time_scale"]:
                raise BitstreamError("invalid VUI timing")
        nal_hrd = b.u(1)
        if nal_hrd: _hrd(b)
        vcl_hrd = b.u(1)
        if vcl_hrd: _hrd(b)
        if nal_hrd or vcl_hrd: b.u(1)
        b.u(1)
        if b.u(1):
            b.u(1); b.ue(16); b.ue(16); b.ue(16); b.ue(16)
            vui["max_num_reorder_frames"] = b.ue(16)
            vui["max_dec_frame_buffering"] = b.ue(16)
            if vui["max_num_reorder_frames"] > vui["max_dec_frame_buffering"]:
                raise BitstreamError("invalid reorder/DPB restriction")
    return {"sps_id": sps_id, "profile_idc": profile, "constraint_flags": constraints,
            "constrained_baseline": profile == 66 and bool(constraints & 0x40),
            "level_idc": level, "chroma_format_idc": chroma, "separate_colour_plane": separate,
            "bit_depth_luma": depth_luma, "bit_depth_chroma": depth_chroma,
            "log2_max_frame_num": log2_frame_num, "pic_order_cnt_type": poc_type,
            "log2_max_pic_order_cnt_lsb": log2_poc, "delta_pic_order_always_zero": delta_always_zero,
            "max_num_ref_frames": refs, "gaps_in_frame_num_allowed": gaps_allowed,
            "frame_mbs_only_flag": frame_only, "mb_adaptive_frame_field_flag": mb_adaptive,
            "coded_width": coded_w, "coded_height": coded_h,
            "visible_width": visible_w, "visible_height": visible_h,
            "crop_offsets_units": crop, "crop_offsets_pixels": crop_px, "vui": vui}


def parse_pps(nal: bytes) -> dict:
    if not nal or nal[0] & 31 != 8 or len(nal) > 65536:
        raise BitstreamError("not a bounded PPS")
    b = Bits(rbsp(nal))
    pps_id, sps_id = b.ue(255), b.ue(31)
    cabac, bottom = bool(b.u(1)), bool(b.u(1))
    groups = b.ue(7)
    if groups:
        raise BitstreamError("slice groups unsupported by sample inspector")
    refs0, refs1 = b.ue(31) + 1, b.ue(31) + 1
    return {"pps_id": pps_id, "sps_id": sps_id, "entropy_coding_mode_cabac": cabac,
            "bottom_field_pic_order_in_frame_present": bottom, "num_slice_groups_minus1": groups,
            "num_ref_idx_l0_default_active": refs0, "num_ref_idx_l1_default_active": refs1}


def parse_slice_prefix(nal: bytes) -> dict:
    if not nal or nal[0] & 31 not in {1, 5}:
        raise BitstreamError("not a supported slice")
    b = Bits(rbsp(nal))
    first_mb, kind, pps_id = b.ue(1048575), b.ue(9), b.ue(255)
    return {"first_mb_in_slice": first_mb, "slice_type": kind % 5, "pps_id": pps_id,
            "idr": nal[0] & 31 == 5}


def inspect(data: bytes, fps_num: int, fps_den: int = 1) -> dict:
    if not 1 <= fps_num <= 240000 or not 1 <= fps_den <= 10000:
        raise BitstreamError("invalid external nominal rate")
    all_nals = list(nals(data))
    starts = [a for a, _, n in all_nals if n[0] & 31 == 9]
    if not starts or starts[0] != 0 or len(starts) > RECORD_MAX:
        raise BitstreamError("fixtures require first/consecutive AUD delimiters")
    ends = starts[1:] + [len(data)]
    aus = [{"offset": start, "size": end - start,
            "pts_us": i * 1000000 * fps_den // fps_num} for i, (start, end) in enumerate(zip(starts, ends))]
    if any(not 0 < a["size"] <= AU_MAX for a in aus):
        raise BitstreamError("AU exceeds existing production 2 MiB cap")
    sps_raw = {n for _, _, n in all_nals if n[0] & 31 == 7}
    pps_raw = {n for _, _, n in all_nals if n[0] & 31 == 8}
    if len(sps_raw) != 1 or len(pps_raw) != 1:
        raise BitstreamError("fixtures require one stable SPS and PPS")
    sps, pps = parse_sps(next(iter(sps_raw))), parse_pps(next(iter(pps_raw)))
    if pps["sps_id"] != sps["sps_id"]:
        raise BitstreamError("PPS references unknown SPS")
    slice_counts, idrs = collections.Counter(), []
    j = 0
    for ordinal, (start, end) in enumerate(zip(starts, ends)):
        slices = []
        while j < len(all_nals) and all_nals[j][0] < end:
            n = all_nals[j][2]
            if n[0] & 31 in {1, 5}:
                slices.append(parse_slice_prefix(n))
            j += 1
        if not slices or sum(s["first_mb_in_slice"] == 0 for s in slices) != 1:
            raise BitstreamError("AU has no unique first VCL slice")
        if any(s["pps_id"] != pps["pps_id"] for s in slices):
            raise BitstreamError("slice references unknown PPS")
        kind = slices[0]["slice_type"]
        if any(s["slice_type"] != kind for s in slices):
            raise BitstreamError("mixed slice types in AU")
        slice_counts[{0: "P", 1: "B", 2: "I", 3: "SP", 4: "SI"}[kind]] += 1
        if slices[0]["idr"]:
            idrs.append(ordinal)
    duration = len(aus) * 1000000 * fps_den // fps_num
    return {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
            "sps": sps, "pps": pps, "nal_type_counts": dict(collections.Counter(str(n[0] & 31) for _, _, n in all_nals)),
            "access_units": aus, "frame_count": len(aus), "fps_num": fps_num, "fps_den": fps_den,
            "duration_us": duration, "real_bitrate_kbps": len(data) * 8 * 1000 / duration,
            "max_au_bytes": max(a["size"] for a in aus), "min_au_bytes": min(a["size"] for a in aus),
            "idr_frame_ordinals": idrs, "gop_frame_distances": [b - a for a, b in zip(idrs, idrs[1:])],
            "slice_frame_counts": dict(slice_counts)}


def iter_file_aus(path: pathlib.Path):
    """AUD-delimited bounded file iterator; at most one AU plus 64 KiB input.

    Generated fixtures use nal_ref_idc zero for AUD. Other syntax is rejected
    rather than attempting unbounded/general slice-to-picture grouping.
    """
    size = path.stat().st_size
    if not size or size > FILE_MAX: raise BitstreamError("empty or oversized Annex-B file")
    delimiter = re.compile(b"\x00\x00(?:\x00)?\x01\x09")
    buffer, offset, records = bytearray(), 0, 0
    with path.open("rb") as f:
        eof = False
        while True:
            if not eof:
                chunk = f.read(65536)
                if chunk: buffer.extend(chunk)
                else: eof = True
            markers = list(delimiter.finditer(buffer))
            if markers and markers[0].start() != 0:
                raise BitstreamError("fixture does not start with nonreference AUD")
            while len(markers) >= 2:
                cut = markers[1].start()
                if cut > AU_MAX: raise BitstreamError("AU exceeds existing production 2 MiB cap")
                records += 1
                if records > RECORD_MAX: raise BitstreamError("excessive AU count")
                yield offset, bytes(buffer[:cut])
                del buffer[:cut]
                offset += cut
                markers = list(delimiter.finditer(buffer))
            if len(buffer) > AU_MAX + 65536:
                raise BitstreamError("AU exceeds existing production 2 MiB cap")
            if eof:
                if not markers or markers[0].start() != 0 or not buffer or len(buffer) > AU_MAX:
                    raise BitstreamError("truncated/missing AUD or oversized final AU")
                records += 1
                if records > RECORD_MAX: raise BitstreamError("excessive AU count")
                yield offset, bytes(buffer)
                offset += len(buffer)
                break
    if offset != size or path.stat().st_size != size:
        raise BitstreamError("file changed or AU coverage incomplete")


def inspect_file(path: pathlib.Path, fps_num: int, fps_den: int = 1) -> dict:
    """Streaming equivalent used by generation, including sustained fixtures."""
    if not 1 <= fps_num <= 240000 or not 1 <= fps_den <= 10000:
        raise BitstreamError("invalid external nominal rate")
    sps_raw = pps_raw = None
    sps = pps = None
    digest, bytes_total = hashlib.sha256(), 0
    aus, idrs, nal_counts, slice_counts = [], [], collections.Counter(), collections.Counter()
    for ordinal, (offset, au) in enumerate(iter_file_aus(path)):
        digest.update(au); bytes_total += len(au)
        au_nals = list(nals(au))
        if len(au_nals) > 32 or au_nals[0][2][0] & 31 != 9:
            raise BitstreamError("excessive NALs or missing first AUD")
        slices = []
        for _, _, n in au_nals:
            kind = n[0] & 31
            nal_counts[str(kind)] += 1
            if kind == 7:
                if sps_raw is not None and sps_raw != n: raise BitstreamError("changed SPS")
                if sps is None: sps_raw, sps = n, parse_sps(n)
            elif kind == 8:
                if pps_raw is not None and pps_raw != n: raise BitstreamError("changed PPS")
                if pps is None: pps_raw, pps = n, parse_pps(n)
            elif kind in {1, 5}: slices.append(parse_slice_prefix(n))
        if sps is None or pps is None or pps["sps_id"] != sps["sps_id"]:
            raise BitstreamError("missing/unknown SPS or PPS")
        if not slices or sum(s["first_mb_in_slice"] == 0 for s in slices) != 1:
            raise BitstreamError("AU has no unique first VCL slice")
        if any(s["pps_id"] != pps["pps_id"] for s in slices):
            raise BitstreamError("slice references unknown PPS")
        kind = slices[0]["slice_type"]
        if any(s["slice_type"] != kind for s in slices):
            raise BitstreamError("mixed slice types in AU")
        slice_counts[{0: "P", 1: "B", 2: "I", 3: "SP", 4: "SI"}[kind]] += 1
        if slices[0]["idr"]: idrs.append(ordinal)
        aus.append({"offset": offset, "size": len(au), "pts_us": ordinal * 1000000 * fps_den // fps_num})
    duration = len(aus) * 1000000 * fps_den // fps_num
    return {"sha256": digest.hexdigest(), "bytes": bytes_total, "sps": sps, "pps": pps,
            "nal_type_counts": dict(nal_counts), "access_units": aus, "frame_count": len(aus),
            "fps_num": fps_num, "fps_den": fps_den, "duration_us": duration,
            "real_bitrate_kbps": bytes_total * 8 * 1000 / duration,
            "max_au_bytes": max(a["size"] for a in aus), "min_au_bytes": min(a["size"] for a in aus),
            "idr_frame_ordinals": idrs, "gop_frame_distances": [b-a for a,b in zip(idrs,idrs[1:])],
            "slice_frame_counts": dict(slice_counts)}


def write_index(path: pathlib.Path, result: dict):
    s = result["sps"]
    header = INDEX_HEADER.pack(INDEX_MAGIC, 1, INDEX_HEADER.size, result["bytes"],
                               result["frame_count"], result["duration_us"],
                               s["visible_width"], s["visible_height"], s["coded_width"], s["coded_height"],
                               result["fps_num"], result["fps_den"], bytes.fromhex(result["sha256"]))
    with path.open("xb") as f:
        f.write(header)
        for au in result["access_units"]:
            f.write(INDEX_RECORD.pack(au["offset"], au["size"], au["pts_us"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("clip", type=pathlib.Path)
    ap.add_argument("--fps", type=int, required=True)
    ap.add_argument("--fps-den", type=int, default=1)
    ap.add_argument("--json", type=pathlib.Path)
    ap.add_argument("--index", type=pathlib.Path)
    a = ap.parse_args()
    if a.clip.stat().st_size > FILE_MAX:
        raise BitstreamError("file exceeds inspector cap")
    result = inspect_file(a.clip, a.fps, a.fps_den)
    if a.index: write_index(a.index, result)
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if a.json: a.json.write_text(text, encoding="utf-8")
    else: print(text)


if __name__ == "__main__":
    main()
