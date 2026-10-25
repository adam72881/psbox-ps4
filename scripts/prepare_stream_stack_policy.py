#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Bind stack evidence to new normal-video routes; legacy UI/auth is not audited."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import audit_probe_stack as gate
from prepare_probe_stack_policy import NATIVE_IMPORTS, MODULE_SHA
ROOT=Path(__file__).resolve().parents[1]
STACK_BYTES=256*1024
ENTRIES=(("x4-video","video_worker"),("x4-stream-decode","stream_pipeline_decode_worker"),
    ("x4-stream-convert0","stream_pipeline_convert_worker"),("x4-stream-convert1","stream_pipeline_convert_worker"),
    ("x4-copy0","copy_worker"),("x4-copy1","copy_worker"),("x4-copy2","copy_worker"))
ROOTS=("x4_stream_pipeline_create","x4_stream_pipeline_start","x4_stream_pipeline_submit",
    "x4_stream_pipeline_display_open","x4_stream_pipeline_close","x4_stream_pipeline_export",
    "x4_stream_pipeline_free","x4_stream_pipeline_wait_present","x4_display_close_checked",
    "x4_live_video_stream_raw_start","x4_live_video_stream_raw_start_cpu",
    "x4_live_video_stream_raw_start_cpu_depth","x4_live_video_stream_output_held_mask",
    "x4_live_video_stream_raw_decode","x4_stream_request_resolve_depth","x4_stream_sps_parse",
    "x4_stream_sdp_limit_video","x4_stream_sdp_video_bandwidth")
OWN_UNITS={"decoder_depth.c","stream_profile.c","stream_sdp_bandwidth.c","stream_wait.c","stream_cpu.c","stream_au_queue.c","keyframe_limiter.c","stream_pipeline.c","stream_diagnostics.c","stream_sps.c","video_pipeline.c","probe_stack.c","live_media.c","live_h264.c","display.c"}
STREAM_NATIVE_WAIT_IMPORTS=("sceKernelCreateSema","sceKernelSignalSema","sceKernelWaitSema","sceKernelDeleteSema",
    "scePthreadMutexInit","scePthreadMutexLock","scePthreadMutexTrylock","scePthreadMutexUnlock","scePthreadMutexDestroy")

def prepare(elf_path,su_root,output,source_root=ROOT):
    elf_path,su_root,output,source_root=map(Path,(elf_path,su_root,output,source_root))
    body=elf_path.read_bytes();symbols=gate.elf_symbols(body)
    required={entry for _,entry in ENTRIES}|set(ROOTS)|{"x4_probe_thread_trampoline","x4_stream_pipeline_present"}
    missing=sorted(name for name in required if name not in symbols or symbols[name]["undefined"])
    if missing:raise ValueError("Missing linked streaming entries: "+", ".join(missing))
    header=source_root/"src/video/probe_stack.h";stack_source=source_root/"src/video/probe_stack.c"
    if not re.search(r"X4_PROBE_THREAD_STACK_BYTES\s*=\s*256\s*\*\s*1024",header.read_text()):
        raise ValueError("Explicit stack differs from reviewed 256 KiB policy")
    source_text=stack_source.read_text(encoding="utf8")
    if "scePthreadAttrSetstacksize" not in source_text or "X4_PROBE_THREAD_STACK_BYTES" not in source_text:
        raise ValueError("Configured stack wrapper evidence missing")
    module=source_root/"src/core/module.c"
    if gate.sha256(module.read_bytes())!=MODULE_SHA:raise ValueError("Re-review module-loader recursion source")
    def evidence(path,detail,kind=None):
        try: artifact=Path(os.path.relpath(path.resolve(),output.parent.resolve())).as_posix()
        except ValueError: artifact=path.resolve().as_posix()
        result={"artifact":artifact,"sha256":gate.sha256(path.read_bytes()),"detail":detail}
        if kind:result["kind"]=kind
        return result
    config=evidence(header,"256 KiB requested by native thread attributes with checked creation and no silent default fallback; console validation remains pending.","configured_attribute")
    module_proof=evidence(module,"Reviewed shared module loader calls itself only for libSceSysmodule; that name selects no ID and cannot recurse again. At most two active calls.")
    usage=[];empty=[];own=set()
    for path in sorted(su_root.rglob("*.su")):
        data=path.read_bytes();records=gate.parse_stack_usage(data.decode("utf8"),str(path))
        rel=Path(os.path.relpath(path.resolve(),output.parent.resolve())).as_posix()
        if not records:
            empty.append({"path":rel,"sha256":hashlib.sha256(data).hexdigest()});continue
        usage.append({"path":rel,"sha256":hashlib.sha256(data).hexdigest()})
        for name,items in records.items():
            if any(any(unit in r.get("location","") for unit in OWN_UNITS) for r in items):own.add(name)
    if not usage:raise ValueError("No same-build stack usage evidence")
    threads=[{"name":name,"entry":entry,"wrapper_entry":"x4_probe_thread_trampoline","stack_bytes":STACK_BYTES,"evidence":config,
        "limit_status":"explicit native attribute request; actual high-water reported by console"} for name,entry in ENTRIES]
    threads.append({"name":"main","entry":"x4_stream_pipeline_present","stack_bytes":None,"wrapper_entry":None,"evidence":None,
        "limit_status":"Only new presentation route audited; no claim about all legacy main/UI/auth or main stack capacity"})
    return {"schema":gate.SCHEMA,"elf_sha256":gate.sha256(body),"max_fraction":.5,"individual_frame_limit_bytes":8192,
        "opaque_allowance_bytes":16384,"threads":threads,"additional_roots":list(ROOTS),"own_functions":sorted(own),
        "stack_usage_files":usage,"empty_disabled_translation_units":empty,"sites":{},"required_native_imports":list(NATIVE_IMPORTS)+list(STREAM_NATIVE_WAIT_IMPORTS),
        "source_evidence":[config,evidence(stack_source,"Same-source native explicit-stack wrapper, startup validation and failure handling; native imports checked in this exact ELF."),module_proof],
        "shared_recursion":[{"members":["x4_module_open"],"max_visits":2,"evidence":module_proof}],
        "limitations":["Native imports and unresolved indirect calls carry a fixed 16 KiB allowance, not a measured upper bound.",
        "Worker entry and wrapper envelope are conservatively added, including opaque dispatch allowance.",
        "No capacity result is claimed for legacy main/UI/auth; only new main-video routes receive frame, dynamic-stack and recursion checks.",
        "All same-build translation units have VLA/alloca rejected by compiler flags; disabled empty units are recorded explicitly.",
        "Runtime reported stack size, guard and high-water require console verification; compilation alone does not prove runtime capacity."]}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--elf",type=Path,required=True);p.add_argument("--su-root",type=Path,required=True);p.add_argument("--output",type=Path,required=True)
    a=p.parse_args()
    try:
        result=prepare(a.elf,a.su_root,a.output);a.output.write_text(json.dumps(result,indent=2)+"\n",encoding="utf8")
    except (ValueError,OSError,UnicodeError) as e:print("Streaming stack policy failed: "+str(e),file=sys.stderr);return 1
    print("Explicit-stack streaming policy prepared; opaque costs remain assumptions.");return 0
if __name__=="__main__":raise SystemExit(main())
