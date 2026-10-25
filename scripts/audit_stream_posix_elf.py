#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Audit the new normal-video routes without claiming an audit of all old UI/auth code."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

import audit_probe_posix_elf as audit

ROOTS = ("video_worker", "stream_pipeline_decode_worker", "stream_pipeline_convert_worker",
    "x4_stream_pipeline_create", "x4_stream_pipeline_start", "x4_stream_pipeline_submit",
    "x4_stream_pipeline_display_open", "x4_stream_pipeline_present", "x4_stream_pipeline_wait_present",
    "x4_stream_pipeline_close", "x4_stream_pipeline_export", "x4_stream_pipeline_free",
    "x4_probe_thread_trampoline", "x4_display_close_checked",
    "x4_live_video_stream_raw_start", "x4_live_video_stream_raw_start_cpu", "x4_live_video_stream_raw_decode", "x4_stream_sps_parse",
    "x4_stream_sdp_limit_video", "x4_stream_sdp_video_bandwidth")
INDIRECT = ("copy_worker", "sampler", "__stdio_read", "__stdio_write", "__stdio_seek", "__stdio_close")


def run(elf, objdump=None, supplied=None):
    body = Path(elf).read_bytes()
    symbols = audit.elf_symbols(body)
    tool = objdump or shutil.which("objdump") or shutil.which("llvm-objdump")
    if supplied is None and not tool:
        raise ValueError("Linked disassembly is required; no silent gate skip")
    text = supplied if supplied is not None else subprocess.check_output([tool, "-d", str(elf)], text=True)
    result = audit.audit(symbols, audit.parse_disassembly(text), roots=ROOTS,
        required_imports=("fstat", "sceKernelFsync", "sceKernelCreateSema", "sceKernelSignalSema",
            "sceKernelWaitSema", "sceKernelDeleteSema", "scePthreadMutexInit", "scePthreadMutexLock",
            "scePthreadMutexTrylock", "scePthreadMutexUnlock", "scePthreadMutexDestroy"), indirect_roots=INDIRECT,
        all_imports_in_scope=False)
    result.update(schema="X4_STREAM_POSIX_AUDIT1", elf_sha256=hashlib.sha256(body).hexdigest(),
        elf_bytes=len(body), disassembly_sha256=hashlib.sha256(text.encode()).hexdigest(),
        scope="New video ingress/decoder/conversion/presentation/export paths and explicit FILE callbacks; existing unrelated UI/auth imports are reported outside scope.")
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("elf", type=Path)
    p.add_argument("--objdump")
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    try:
        result = run(args.elf, args.objdump)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({k: result[k] for k in ("passed", "elf_sha256", "forbidden_imports",
            "reachable_linked_stubs", "forbidden_effective_paths", "outside_scope_known_stub_imports")}))
        return 0 if result["passed"] else 1
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("Streaming POSIX gate failed: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
