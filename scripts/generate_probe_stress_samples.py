#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate separately identified synthetic stress candidates; never PS4 results.

Imports the frozen compatibility helpers without changing them. Generated media
and private tool logs stay outside source. Existing files are never overwritten.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import pathlib
import subprocess
import time
import generate_probe_samples as G
from inspect_probe_bitstream import AU_MAX, FILE_MAX, INDEX_HEADER, INDEX_RECORD, INDEX_MAGIC, inspect_file, iter_file_aus, nals, parse_slice_prefix, write_index

# Name, visible width/height, cadence, x264 profile/level, H264 IDs, target kbps.
STRESS_CASES = (
    ("S720-CB", 1280, 720, 60, "baseline", "3.2", 66, 32, 10000),
    ("S1080-CB", 1920, 1080, 60, "baseline", "4.2", 66, 42, 20000),
    ("S1080-HIGH", 1920, 1080, 60, "high", "4.2", 100, 42, 20000),
)
LABELS = {"S720-CB": "EstrÃ©s 720p60 CB", "S1080-CB": "EstrÃ©s 1080p60 CB", "S1080-HIGH": "EstrÃ©s 1080p60 High"}


def sustained_budget(seconds, kbps):
    """Explicit approximate rate budget, not an encoded-file size guarantee."""
    payload = seconds * kbps * 1000 // 8
    maximum=kbps*5//4
    buffer_bytes=2*maximum*1000//8
    vbv_flow=seconds*maximum*1000//8
    # The ABR target+20% estimate is not the VBV envelope. This conservative
    # planning envelope uses the actual configured maximum rate plus its CPB.
    # It still is not a proven whole Annex-B-file bound; final bytes/complete
    # decode remain mandatory and the real file writer is capped at2 GiB.
    envelope=vbv_flow+buffer_bytes
    return {"target_bytes":payload,"abr_20_percent_estimate_bytes":payload+payload//5+buffer_bytes,
            "vbv_ceiling_kbps":maximum,"vbv_flow_bytes":vbv_flow,"vbv_buffer_bytes":buffer_bytes,
            "planned_upper_bytes":envelope,"reader_limit_bytes":FILE_MAX,
            "remaining_margin_bytes":FILE_MAX-envelope,"whole_encoded_size_bound_proven":False}


def sample_plan(seconds, sustained, names):
    if names and (len(set(names)) != len(names) or any(n not in LABELS for n in names)):
        raise ValueError("unknown or duplicate stress case")
    if sustained:
        if not 10 <= seconds <= 600 or not names or len(names) != 1:
            raise ValueError("sustained requires 10..600 seconds and exactly one case")
    elif not 1 <= seconds <= 10:
        raise ValueError("initial stress limited to 1..10 seconds")
    cases = [c for c in STRESS_CASES if not names or c[0] in names]
    if any(sustained_budget(seconds, c[8])["remaining_margin_bytes"] <= 0 for c in cases):
        raise ValueError("planned encoded size lacks margin below 2 GiB")
    return cases


def stress_filter(width, height, fps, seconds):
    # Add temporally varying texture BEFORE drawing inherited quiet borders and
    # ordinal stripes. References compare the WHOLE decoded image, not an ROI.
    original = G.synthetic_filter(width, height, fps, seconds)
    first, overlays = original.split(",", 1)
    detail = "noise=alls=18:allf=t+u:all_seed=21686771,limiter=min=16:max=235:planes=1,limiter=min=16:max=240:planes=6"
    # The production converter clamps Y-16 before chroma; FFmpeg's independent
    # RGB path does not. Keep a guard band even after high-contrast control
    # marks so lossy ringing cannot turn this stress into a superblack math
    # mismatch. This changes new synthetic content, not any comparison limits.
    return first + "," + detail + "," + overlays + ",limiter=min=48:max=207:planes=1,limiter=min=48:max=208:planes=6"


def encode_command(ffmpeg, clip, case, seconds):
    _, width, height, fps, profile, level, _, _, kbps = case
    params = f"aud=1:repeat-headers=1:annexb=1:keyint={fps}:min-keyint={fps}:scenecut=0:ref=4:bframes=0:force-cfr=1:nal-hrd=vbr:filler=0"
    maximum=kbps*5//4
    return [ffmpeg, "-hide_banner", "-loglevel", "info", "-f", "lavfi", "-i", stress_filter(width, height, fps, seconds),
            "-an", "-c:v", "libx264", "-preset", "veryfast", "-b:v", f"{kbps}k", "-maxrate", f"{maximum}k", "-bufsize", f"{2*maximum}k",
            "-threads", "2", "-profile:v", profile, "-level:v", level, "-pix_fmt", "yuv420p", "-color_range", "tv",
            "-colorspace", "smpte170m", "-color_primaries", "smpte170m", "-color_trc", "smpte170m", "-x264-params", params,
            "-frames:v", str(fps*seconds), "-fs", str(FILE_MAX), "-f", "h264", str(clip)]


def percentile_nearest_rank(values, percentile):
    ordered = sorted(values)
    if not ordered: raise ValueError("empty distribution")
    return ordered[max(0, math.ceil(percentile * len(ordered)) - 1)]


def byte_accounting(clip):
    totals = {"vcl_ebsp_bytes": 0, "signaling_ebsp_bytes": 0, "filler_ebsp_bytes": 0, "annexb_prefix_bytes": 0}
    per_type, sizes, idrs = {}, [], []
    for ordinal, (_, au) in enumerate(iter_file_aus(clip)):
        sizes.append(len(au)); vcl = 0; has_idr = False
        for start, end, nal in nals(au):
            kind = nal[0] & 31
            category = "vcl_ebsp_bytes" if kind in (1, 2, 3, 4, 5) else "filler_ebsp_bytes" if kind == 12 else "signaling_ebsp_bytes"
            totals[category] += len(nal); totals["annexb_prefix_bytes"] += end-start-len(nal)
            key = str(kind); per_type[key] = per_type.get(key, 0) + len(nal)
            if kind in (1, 5): vcl += len(nal)
            if kind == 5: has_idr = True
        if has_idr: idrs.append({"input_ordinal": ordinal, "AU_bytes": len(au), "VCL_EBSP_bytes": vcl})
    size = clip.stat().st_size
    if sum(totals.values()) != size: raise ValueError("Annex-B byte accounting incomplete")
    if totals["filler_ebsp_bytes"]: raise ValueError("stress sample contains filler NALs")
    return {"bytes": size, **totals, "NAL_type_EBSP_bytes": per_type,
            "accounting_definition": "NAL header+escaped payload (EBSP); VCL includes slice syntax and emulation-prevention, not pure semantic pixel bits. Non-VCL includes SPS/PPS/AUD/SEI. Prefix bytes are separate.",
            "AU_max_bytes": max(sizes), "AU_median_bytes": float(__import__("statistics").median(sizes)),
            "AU_p95_nearest_rank_bytes": percentile_nearest_rank(sizes, .95), "IDR_inputs": idrs}


def validate_color(reference):
    checks = reference["ffmpeg_rgba_crosscheck"]["frames"]
    if len(checks) != 8: raise ValueError("eight whole-frame color checks required")
    for check in checks:
        if check["mean_abs_channel_difference"] > 3 or check["max_abs_channel_difference"] > 16:
            raise ValueError("existing whole-frame native reference criterion not met")
    return {"coverage": "all RGB channels of all pixels, including borders, in first eight decoded frames; no ROI",
            "MAE_limit": 3, "maximum_channel_delta_limit": 16, "all_passed": True}


def validate_encoder_sei(text):
    options=text.partition("options:")[2].split()
    expected={"rc":"abr","nal_hrd":"vbr","filler":"0"}
    for name,value in expected.items():
        matches=[token.partition("=")[2] for token in options if token.partition("=")[0]==name]
        if matches!=[value]:raise ValueError(f"encoder did not confirm exactly {name}={value}")
    return expected


def stress_crosscheck_rgba(ffmpeg, clip, directory, width, height):
    # Production's explicit max(Y-16,0) differs from swscale's treatment of
    # decoded superblack. A public FFmpeg lutyuv clamp expresses that same
    # declared math BEFORE its independent nearest-neighbor BT601 converter.
    # No crop, ROI, output adjustment or acceptance threshold is changed.
    command=[ffmpeg,"-hide_banner","-loglevel","error","-threads","2","-i",str(clip),
        "-vf","lutyuv=y='max(val,16)',scale=flags=neighbor+bitexact:in_color_matrix=bt601:out_color_matrix=bt601:in_range=tv:out_range=pc,format=rgba",
        "-frames:v","8","-fps_mode","passthrough","-f","rawvideo","pipe:1"]
    results=[]
    with (directory/"reference-rgba-crosscheck.log").open("wb") as err:
        process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=err)
        try:
            for ordinal in range(8):
                frame=process.stdout.read(width*height*4)
                if len(frame)!=width*height*4: raise RuntimeError("independent color frame truncated")
                other=G.np.frombuffer(frame,G.np.uint8).reshape(height,width,4)
                ours=G.np.frombuffer((directory/f"reference-{ordinal:04d}.rgba").read_bytes(),G.np.uint8).reshape(height,width,4)
                delta=G.np.abs(other[:,:,:3].astype(G.np.int16)-ours[:,:,:3].astype(G.np.int16))
                results.append({"frame":ordinal,"max_abs_channel_difference":int(delta.max()),
                    "mean_abs_channel_difference":float(delta.mean()),"channels_over_3":int((delta>3).sum()),"channel_count":int(delta.size)})
                if not G.np.all(other[:,:,3]==255):raise RuntimeError("independent alpha differs")
            if process.stdout.read(1):raise RuntimeError("extra independent color frames")
        except BaseException:
            process.kill();process.wait();raise
        finally:process.stdout.close()
        result=process.wait()
    if result:raise RuntimeError(f"independent color conversion exit={result}")
    return {"command":command,"frames":results,"native_result":"not executed",
        "luma_contract":"lutyuv max(Y,16) before independent swscale; matches production max(Y-16,0) for decoded superblack"}


def verify_case(directory, case):
    clip, index = directory / "clip.h264", directory / case["index"]["path"]
    if G.sha(clip) != case["sha256"] or G.sha(index) != case["index"]["sha256"]: raise ValueError("clip/index changed")
    observed = inspect_file(clip, case["fps_num"], case["fps_den"])
    actual_sei=G.encoder_sei(clip)
    if actual_sei!=case["encoder_identification_sei"]:raise ValueError("encoder SEI changed")
    validate_encoder_sei(actual_sei)
    for key in ("sps", "pps", "frame_count", "duration_us", "bytes", "sha256"):
        if observed[key] != case[key]: raise ValueError(f"manifest {key} differs from inspected stream")
    with index.open("rb") as source:
        header = INDEX_HEADER.unpack(source.read(INDEX_HEADER.size))
        s = case["sps"]
        expected = (INDEX_MAGIC, 1, 96, case["bytes"], case["frame_count"], case["duration_us"],
                    s["visible_width"], s["visible_height"], s["coded_width"], s["coded_height"],
                    case["fps_num"], case["fps_den"], bytes.fromhex(case["sha256"]))
        if header != expected: raise ValueError("index header differs")
        for au in observed["access_units"]:
            if INDEX_RECORD.unpack(source.read(INDEX_RECORD.size)) != (au["offset"], au["size"], au["pts_us"]):
                raise ValueError("index AU boundary/time differs")
        if source.read(1): raise ValueError("index has trailing bytes")
    if byte_accounting(clip) != case["stress_bytes"]: raise ValueError("byte accounting changed")
    validate_color(case["reference"])
    return observed


def descriptor_header(manifest, output, header):
    lines = ["/* SPDX-License-Identifier: GPL-3.0-only */", "/* Generated stress candidates, not native compatibility/performance results. */",
             "#pragma once", '#include "probe_case.h"', "", "static const X4ProbeCase x4_probe_stress_cases[] = {"]
    for case in manifest["cases"]:
        name = case["name"]
        if name not in LABELS: raise ValueError("unknown stress descriptor")
        directory = output/name; verify_case(directory, case)
        references = case["reference"]["saved_rgba"]
        if len(references)!=8 or [r["frame_ordinal"] for r in references]!=list(range(8)): raise ValueError("reference ordinals invalid")
        expected_bytes=case["sps"]["visible_width"]*case["sps"]["visible_height"]*4
        joined=directory/"references.rgba"; temporary=directory/"references.rgba.partial"; digest=hashlib.sha256()
        if joined.exists():
            if joined.stat().st_size!=8*expected_bytes: raise ValueError("joined references size invalid")
            with joined.open("rb") as existing:
                for ref in references:
                    path=directory/ref["path"]
                    if path.stat().st_size!=expected_bytes or G.sha(path)!=ref["sha256"]: raise ValueError("reference changed")
                    with path.open("rb") as source:
                        for block in iter(lambda:source.read(1024*1024),b""):
                            if existing.read(len(block))!=block: raise ValueError("joined reference differs")
                            digest.update(block)
        else:
            owned=False
            try:
                with temporary.open("xb") as target:
                    owned=True
                    for ref in references:
                        path=directory/ref["path"]
                        if path.stat().st_size!=expected_bytes or G.sha(path)!=ref["sha256"]: raise ValueError("reference changed")
                        with path.open("rb") as source:
                            for block in iter(lambda:source.read(1024*1024),b""): target.write(block);digest.update(block)
                temporary.rename(joined)
            finally:
                if owned and temporary.exists(): temporary.unlink()
        meta={"path":joined.name,"bytes":8*expected_bytes,"sha256":digest.hexdigest(),"frame_count":8,
              "format":"tightly packed visible RGBA8, frame ordinals 0..7"}
        case["reference"]["concatenated_rgba"]=meta
        (directory/"manifest.json").write_text(json.dumps(case,indent=2,sort_keys=True)+"\n",encoding="utf-8")
        s,v,crop=case["sps"],case["sps"]["vui"],case["sps"]["crop_offsets_pixels"]
        def array(hex_value): return "{"+",".join(f"0x{b:02x}" for b in bytes.fromhex(hex_value))+"}"
        lines += ["    {",f'        .id="{name}", .label="{LABELS[name]}",',
            f'        .clip_path="/data/xcloud4-probe/{name}.h264",',f'        .index_path="/data/xcloud4-probe/{name}.x4au",',
            f'        .reference_path="/data/xcloud4-probe/{name}.rgba",',f'        .clip_sha256={array(case["sha256"])},',
            f'        .index_sha256={array(case["index"]["sha256"])},',f'        .reference_sha256={array(meta["sha256"])},',
            f'        .visible_width={s["visible_width"]}, .visible_height={s["visible_height"]}, .coded_width={s["coded_width"]}, .coded_height={s["coded_height"]},',
            f'        .crop_left={crop[0]}, .crop_top={crop[2]}, .crop_right={crop[1]}, .crop_bottom={crop[3]},',
            f'        .fps_num={case["fps_num"]}, .fps_den={case["fps_den"]}, .frame_mbs_only_flag={s["frame_mbs_only_flag"]},',
            f'        .profile_idc={s["profile_idc"]}, .level_idc={s["level_idc"]}, .max_num_ref_frames={s["max_num_ref_frames"]},',
            f'        .max_dec_frame_buffering={v["max_dec_frame_buffering"]}, .num_reorder_frames={v["max_num_reorder_frames"]},',
            f'        .chosen_dpb_frames={max(s["max_num_ref_frames"],v["max_dec_frame_buffering"])}, .chroma_format_idc={s["chroma_format_idc"]},',
            f'        .bit_depth_luma={s["bit_depth_luma"]}, .bit_depth_chroma={s["bit_depth_chroma"]}, .matrix_coefficients={v["matrix_coefficients"]},',
            f'        .full_range={int(v["full_range"])}, .reference_count=8, .au_count={case["frame_count"]}ull, .duration_us={case["duration_us"]}ull',"    },"]
    lines += ["};","#define X4_PROBE_STRESS_CASE_COUNT (sizeof(x4_probe_stress_cases)/sizeof(x4_probe_stress_cases[0]))", ""]
    text="\n".join(lines)
    if header.exists() and header.read_text(encoding="utf-8")!=text: raise ValueError("descriptor exists with different bytes")
    if not header.exists(): header.write_text(text,encoding="utf-8")
    return {"path":str(header),"sha256":G.sha(header),"case_count":len(manifest["cases"])}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ffmpeg",required=True);ap.add_argument("--output",type=pathlib.Path,required=True)
    ap.add_argument("--seconds",type=int,default=10);ap.add_argument("--sustained",action="store_true")
    ap.add_argument("--case",action="append",choices=list(LABELS));ap.add_argument("--header",type=pathlib.Path)
    ap.add_argument("--descriptors-only",action="store_true");a=ap.parse_args()
    cases=sample_plan(a.seconds,a.sustained,a.case);output=a.output.resolve();root=pathlib.Path(__file__).resolve().parents[1]
    if output==root or root in output.parents: raise ValueError("media must stay outside source")
    output.mkdir(parents=True,exist_ok=True);manifest_path=output/"manifest.json"
    if a.descriptors_only:
        if not a.header: raise ValueError("descriptors-only requires header")
        manifest=json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["generated_descriptor"]=descriptor_header(manifest,output,a.header)
        manifest_path.write_text(json.dumps(manifest,indent=2,sort_keys=True)+"\n",encoding="utf-8");return
    if manifest_path.exists(): raise ValueError("manifest exists; use a fresh output directory")
    manifest={"schema":"X4PROBE_STRESS1","synthetic_content":True,"seconds":a.seconds,"sustained":a.sustained,
        "tool":{"ffmpeg_path":a.ffmpeg,"sha256":G.sha(pathlib.Path(a.ffmpeg)),"version":subprocess.check_output([a.ffmpeg,"-version"],text=True),
                "numpy_version":G.np.__version__,"Pillow_version":__import__("PIL").__version__},
        "content":"testsrc2 motion + seeded temporal uniform grain, grid, inherited border/ordinal shapes; final luma48..207/chroma48..208 guard band avoids superblack ringing in independent color comparison",
        "reference_region":"whole decoded frame, no ROI; same MAE<=3/max<=16 criteria",
        "console_results":"not executed","cases":[],"status":"running"}
    manifest_path.write_text(json.dumps(manifest,indent=2,sort_keys=True)+"\n",encoding="utf-8")
    try:
        for case in cases:
            name,width,height,fps,_,_,_,_,kbps=case;directory=output/name;directory.mkdir(exist_ok=False)
            clip,index=directory/"clip.h264",directory/"clip.x4au";command=encode_command(a.ffmpeg,clip,case,a.seconds);start=time.monotonic()
            print(json.dumps({"sample":name,"stage":"encoding","seconds":a.seconds}),flush=True)
            G.run(command,directory/"encode.log");result=inspect_file(clip,fps)
            if result["max_au_bytes"]>AU_MAX: raise ValueError("AU exceeds unchanged 2 MiB")
            write_index(index,result);result["encoder_identification_sei"]=G.encoder_sei(clip)
            validate_encoder_sei(result["encoder_identification_sei"])
            trace=[a.ffmpeg,"-hide_banner","-loglevel","info","-i",str(clip),"-map","0:v:0","-c:v","copy","-bsf:v","trace_headers","-f","null","-"]
            G.run(trace,directory/"trace_headers.log");values=G.trace_header_values(directory/"trace_headers.log")
            validation=G.validate_observed(result,values,case[:8],a.seconds)
            if result["idr_frame_ordinals"]!=list(range(0,fps*a.seconds,fps)): raise ValueError("GOP not exactly one second")
            accounting=byte_accounting(clip)
            print(json.dumps({"sample":name,"stage":"software_decode","bytes":result["bytes"],"bitrate_kbps":result["real_bitrate_kbps"]}),flush=True)
            reference=G.reference_decode(a.ffmpeg,clip,directory,width,height,fps*a.seconds)
            reference["ffmpeg_rgba_crosscheck"]=stress_crosscheck_rgba(a.ffmpeg,clip,directory,width,height)
            reference["stress_color_validation"]=validate_color(reference)
            result.update({"name":name,"encode_command":command,"trace_header_command":trace,"trace_header_values":values,
                "cross_validation":validation,"reference":reference,"stress_bytes":accounting,"target_kbps":kbps,
                "rate_control":"ABR; VBV maxrate 125% of target, 2-second maximum-rate buffer, nal-hrd=vbr, filler=0",
                "future_size_plan":sustained_budget(a.seconds,kbps),"index":{"path":index.name,"bytes":index.stat().st_size,"sha256":G.sha(index)},
                "generation_wall_seconds":time.monotonic()-start,"status":"passed offline; native not executed"})
            (directory/"manifest.json").write_text(json.dumps(result,indent=2,sort_keys=True)+"\n",encoding="utf-8")
            manifest["cases"].append(result);manifest_path.write_text(json.dumps(manifest,indent=2,sort_keys=True)+"\n",encoding="utf-8")
            print(json.dumps({"sample":name,"stage":"verified","frames":result["frame_count"],"max_AU":result["max_au_bytes"]}),flush=True)
        if a.header: manifest["generated_descriptor"]=descriptor_header(manifest,output,a.header)
        manifest["status"]="passed offline; native not executed"
    except BaseException as error:
        manifest["status"]="failed";manifest["failure"]={"type":type(error).__name__,"message":str(error)}
        raise
    finally: manifest_path.write_text(json.dumps(manifest,indent=2,sort_keys=True)+"\n",encoding="utf-8")


if __name__=="__main__": main()
