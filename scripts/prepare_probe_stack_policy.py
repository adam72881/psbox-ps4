#!/usr/bin/env python3
"""Bind the explicit-stack probe policy to same-build ELF, .su and source.
256 KiB is a configured request, not a claimed console measurement. Opaque call
costs use explicit 16 KiB allowances, not SDK-derived bounds. Main and the
informational NULL-attributes diagnostic thread remain unknown before PS4.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import audit_probe_stack as gate
STACK_BYTES = 256 * 1024
NATIVE_IMPORTS = ("scePthreadAttrInit", "scePthreadAttrSetstacksize", "scePthreadAttrDestroy",
    "scePthreadAttrGet", "scePthreadAttrGetstacksize", "scePthreadAttrGetstack",
    "scePthreadAttrGetguardsize", "scePthreadSelf", "scePthreadCreate", "scePthreadJoin")
MODULE_SHA = "f2d3d903e8f7928d77c07db0033fbffdbf4c09f4dfb4dbe5e56e56344eb2ec34"
MEMORY_CALLBACKS = ("native_allocate", "native_map", "native_unmap", "native_release",
    "native_heap_allocate", "native_heap_free", "native_clock", "memory_progress")
HASH_CALLBACK = "x4_probe_hash_file"

def read_linked_disassembly(elf_path, supplied=None, objdump=None):
    if supplied is not None:
        return Path(supplied).read_text(encoding="utf-8")
    tool = objdump or shutil.which("objdump") or shutil.which("llvm-objdump")
    if not tool:
        raise ValueError("objdump unavailable; hash callback proof requires linked disassembly")
    return subprocess.run([tool, "-drwC", str(elf_path)], check=True, capture_output=True, text=True).stdout

def _writes_register(ins, reg):
    op = ins["mnemonic"]
    args = ins["operands"].split("#", 1)[0].replace(" ", "")
    if op.startswith(("cmp", "test", "push", "call", "j", "ret", "nop")):
        return False
    aliases = {"r9": r"r9(?:d|w|b)?", "r12": r"r12(?:d|w|b)?", "r15": r"r15(?:d|w|b)?", "rax": r"(?:rax|eax|ax|al|ah)"}
    return bool(re.search(r"(?:^|,)%" + aliases[reg] + r"$", args))

def _prove_callback_register(function, reg, origin, expected_count):
    """Fail-closed CFG proof for the reviewed SysV callee-saved hash register.
    Track every branch to each indirect call, including error paths that write
    a low byte of the saved register. Calls preserve r12/r15 by the ABI; r9 is
    caller-saved. No general pointer analysis or guessed destination is used.
    """
    rows = function["instructions"]
    addresses = {row["address"]: i for i, row in enumerate(rows)}
    if origin == "stack7" and [re.sub(r"\s+", "", r["text"]) for r in rows[:2]] != ["push%rbp", "mov%rsp,%rbp"]:
        raise ValueError("Hash reference callback lacks reviewed frame-pointer prologue")
    pending, seen, calls = [(0, True, False)], set(), set()
    while pending:
        at, argument_valid, callback_valid = pending.pop()
        state = (at, argument_valid, callback_valid)
        if state in seen or at >= len(rows):
            continue
        seen.add(state)
        row = rows[at]; op = row["mnemonic"]; args = row["operands"].split("#", 1)[0].replace(" ", "")
        if _writes_register(row, reg):
            callback_valid = bool(op in ("mov", "movq") and
                ((origin == "r9" and args == "%r9,%" + reg and argument_valid) or
                 (origin == "stack7" and args == "0x10(%rbp),%" + reg and argument_valid)))
        if origin == "r9" and _writes_register(row, "r9"):
            argument_valid = False
        if origin == "stack7" and not op.startswith(("cmp", "test")) and args.endswith(",0x10(%rbp)"):
            argument_valid = False
        if op.startswith("call"):
            if "*" in args:
                if args != "*%" + reg or not callback_valid:
                    raise ValueError("Unproved hash callback register at " + hex(row["address"]))
                calls.add(row["address"])
            if origin == "r9":
                argument_valid = False
        if op.startswith("ret") or op in ("ud2", "hlt"):
            continue
        if op.startswith("j") or op.startswith("loop"):
            target = gate._direct_target(row)
            if target is None or target not in addresses:
                raise ValueError("Unproved branch in hash callback callee")
            pending.append((addresses[target], argument_valid, callback_valid))
            if op in ("jmp", "jmpq"):
                continue
        pending.append((at + 1, argument_valid, callback_valid))
    if len(calls) != expected_count:
        raise ValueError("Unexpected number of hash callback calls")
    return sorted(calls)

def hash_callback_sites(symbols, functions):
    """Resolve only the three reviewed production hash callback call-sites.
    The caller's LEA is checked against the real hash ELF address; the local
    straight-line argument setup, wrapper and callee CFG must all match.
    Compiler changes fail closed and require another review.
    """
    required = (HASH_CALLBACK, "source_open", "x4_probe_source_open_fully_verified",
        "x4_probe_reference_open_verified", "video_worker", "start_configured", "probe_pipeline_decode_worker")
    for name in required:
        if name not in functions or name not in symbols or symbols[name].get("undefined") or functions[name]["address"] != symbols[name]["address"]:
            raise ValueError("Hash proof missing/mismatched linked body: " + name)
    hash_address = symbols[HASH_CALLBACK]["address"]
    for caller, api, register, stack_argument in [
        ("video_worker", "x4_probe_source_open_fully_verified", "r9", False),
        ("probe_pipeline_decode_worker", "x4_probe_source_open_fully_verified", "r9", False),
        ("start_configured", "x4_probe_reference_open_verified", "rax", True)]:
        callsites = [(name, site) for name, fn in functions.items() for site in fn["sites"] if site["target"] == api]
        expected_callers = ["probe_pipeline_decode_worker", "video_worker"] if api == "x4_probe_source_open_fully_verified" else ["start_configured"]
        if sorted(name for name, _ in callsites) != expected_callers:
            raise ValueError("Unreviewed hash API caller: " + api)
        fn = functions[caller]; site = next(site for name, site in callsites if name == caller)
        end = next(i for i, row in enumerate(fn["instructions"]) if row["address"] == site["address"])
        candidates = [(i, row) for i, row in enumerate(fn["instructions"][:end])
            if row["mnemonic"] in ("lea", "leaq") and re.search(r",\s*%" + register + r"\s*#\s*(?:0x)?([0-9a-fA-F]+)\s+<x4_probe_hash_file>", row["operands"])]
        if not candidates:
            raise ValueError("Missing hash address argument in " + caller)
        start, load = candidates[-1]
        address = int(re.search(r"#\s*(?:0x)?([0-9a-fA-F]+)", load["operands"])[1], 16)
        between = fn["instructions"][start + 1:end]
        if address != hash_address or any(_writes_register(row, register) or row["mnemonic"].startswith(("call", "j", "ret")) for row in between):
            raise ValueError("Hash argument overwritten or nonlocal setup in " + caller)
        # No branch may enter halfway through the reviewed setup.
        for row in fn["instructions"]:
            if row["mnemonic"].startswith(("j", "loop")):
                target = gate._direct_target(row)
                if target is not None and load["address"] < target <= site["address"]:
                    raise ValueError("Branch bypasses hash argument setup")
        if stack_argument:
            if not between or not re.fullmatch(r"pushq?\s+%rax", between[-1]["text"]):
                raise ValueError("Reference hash is not the seventh stack argument")
    wrapper = functions["x4_probe_source_open_fully_verified"]
    tail = [s for s in wrapper["sites"] if s["kind"] == "tail" and s["target"] == "source_open"]
    if len(tail) != 1 or any(_writes_register(row, "r9") or row["mnemonic"].startswith("call") for row in wrapper["instructions"]):
        raise ValueError("Hash wrapper does not preserve argument six")
    # The wrapper's only inter-function branch is the reviewed tail call.
    if len(wrapper["sites"]) != 1:
        raise ValueError("Unreviewed call in hash wrapper")
    return [("source_open", at) for at in _prove_callback_register(functions["source_open"], "r15", "r9", 2)] + [
        ("x4_probe_reference_open_verified", at) for at in _prove_callback_register(functions["x4_probe_reference_open_verified"], "r12", "stack7", 1)]

def prepare(elf_path, su_root, output, source_root=ROOT, disassembly=None, objdump=None):
    elf_path, su_root, output, source_root = map(Path, (elf_path, su_root, output, source_root))
    body = elf_path.read_bytes()
    symbols = gate.elf_symbols(body)
    entries = [("main", "main"), ("x4-video", "video_worker"),
        ("x4-copy0", "copy_worker"), ("x4-copy1", "copy_worker"), ("x4-copy2", "copy_worker"),
        ("x4-stack-check", "probe_stack_test_entry"), ("x4-stack-default", "probe_default_stack_entry"),
        ("x4-pipeline-decode", "probe_pipeline_decode_worker"),
        ("x4-pipeline-convert0", "probe_pipeline_convert_worker"),
        ("x4-pipeline-convert1", "probe_pipeline_convert_worker"),
        ("x4-pipeline-convert2", "probe_pipeline_convert_worker")]
    required = {entry for _, entry in entries} | {"_start", "x4_probe_thread_trampoline", HASH_CALLBACK} | set(MEMORY_CALLBACKS)
    missing = sorted(name for name in required if name not in symbols or symbols[name]["undefined"])
    if missing:
        raise ValueError("Missing real linked probe thread entries: " + ", ".join(missing))
    header, source = source_root / "src/video/probe_stack.h", source_root / "src/video/probe_stack.c"
    if not re.search(r"X4_PROBE_THREAD_STACK_BYTES\s*=\s*256\s*\*\s*1024", header.read_text()):
        raise ValueError("Explicit stack constant differs from reviewed 256 KiB policy")
    module = source_root / "src/core/module.c"
    if gate.sha256(module.read_bytes()) != MODULE_SHA:
        raise ValueError("Shared module-loader recursion source changed; re-review its depth before packaging")
    def evidence(path, detail, kind=None):
        try:
            artifact = Path(os.path.relpath(path.resolve(), output.parent.resolve())).as_posix()
        except ValueError:  # Windows test fixture and source may be on distinct drives.
            artifact = path.resolve().as_posix()
        result = {"artifact": artifact, "sha256": gate.sha256(path.read_bytes()), "detail": detail}
        if kind:
            result["kind"] = kind
        return result
    config = evidence(header,
        "X4_PROBE_THREAD_STACK_BYTES requests 256 KiB through native attributes; no default fallback. Console measurement pending.",
        "configured_attribute")
    if not re.search(r'CREATE\(\s*&t\.id\s*,\s*NULL\s*,\s*probe_default_stack_entry\s*,\s*&t\s*,\s*"x4-stack-default"\s*\)',
            source.read_text(encoding="utf-8")):
        raise ValueError("Informational default-stack entry is not created with reviewed NULL attributes")
    default_info = evidence(source,
        "x4_probe_default_stack_check creates probe_default_stack_entry using NULL attributes; no configured capacity is claimed, no paint or explicit-stack trampoline is used; size and guard need native observation.",
        "informational_null_attributes")
    hash_callers = [(source_root / "src/media/live_media.c", "x4_probe_source_open_fully_verified"),
        (source_root / "src/video/probe_pipeline.c", "x4_probe_source_open_fully_verified"),
        (source_root / "src/video/live_h264.c", "x4_probe_reference_open_verified")]
    for caller, api in hash_callers:
        if not re.search(r"\b" + api + r"\s*\([^;]*\bx4_probe_hash_file\b[^;]*\);",
                caller.read_text(encoding="utf-8")):
            raise ValueError("Reviewed hash callback is missing from " + str(caller))
    hash_evidence = [evidence(caller,
        api + " receives x4_probe_hash_file in the probe path. The linked argument setup, wrapper and callee CFG are verified before emitting the three site proofs.")
        for caller, api in hash_callers]
    hash_evidence.append(evidence(source_root / "src/video/probe_hash.c",
        "Real linked x4_probe_hash_file and its mbedTLS/libc callees are also checked as a supplementary root, independently of its resolved three production dispatch sites."))
    for relative, count in [("src/video/probe_source.c", 2), ("src/video/probe_reference.c", 1)]:
        path = source_root / relative
        if len(re.findall(r"\bhash\s*\(", path.read_text(encoding="utf-8"))) != count:
            raise ValueError("Hash invocation count changed in " + str(path))
        hash_evidence.append(evidence(path,
            "Reviewed hash parameter invocation count: " + str(count) + "; linked callee register provenance is checked along every CFG path to each indirect call."))
    dump = read_linked_disassembly(elf_path, disassembly, objdump)
    hash_sites = hash_callback_sites(symbols, gate.parse_disassembly(dump))
    sites = {f"{name}@0x{address:x}": {"kind": "indirect_targets", "targets": [HASH_CALLBACK],
        "evidence": evidence(elf_path,
            "Same ELF: caller loads the defined hash symbol address; SysV argument setup and preserving wrapper verified; every path to this call preserves the callback in its callee-saved register. Source hashes are bound separately.")}
        for name, address in hash_sites}
    module_proof = evidence(module,
        "src/core/module.c:94-109: recursive call uses libSceSysmodule; that name matches no id branch so id=0 and no second recursion. At most two active calls; shared normal-app dependency unchanged.")
    usage = []
    for path in sorted(su_root.rglob("*.su")):
        if not path.is_file():
            continue
        data = path.read_bytes()
        if not gate.parse_stack_usage(data.decode("utf-8"), str(path)):
            raise ValueError("Empty stack-usage evidence: " + str(path))
        usage.append({"path": Path(os.path.relpath(path.resolve(), output.parent.resolve())).as_posix(),
            "sha256": hashlib.sha256(data).hexdigest()})
    if not usage:
        raise ValueError("No same-build .su files; compile the probe with -fstack-usage first")
    threads = []
    for name, entry in entries:
        is_main, is_default_info = name == "main", name == "x4-stack-default"
        threads.append({"name": name, "entry": entry,
            "wrapper_entry": "_start" if is_main else None if is_default_info else "x4_probe_thread_trampoline",
            "stack_bytes": None if is_main or is_default_info else STACK_BYTES,
            "evidence": default_info if is_default_info else None if is_main else config,
            "informational_only": is_default_info,
            "limit_status": "unknown native main stack" if is_main else
                "unknown native NULL-attributes stack; INFO only, no capacity pass" if is_default_info else
                "explicit attribute request; console measurement pending"})
    return {"schema": gate.SCHEMA, "elf_sha256": gate.sha256(body), "max_fraction": .5,
        "individual_frame_limit_bytes": 8192, "opaque_allowance_bytes": 16384,
        "threads": threads,
        "additional_roots": list(MEMORY_CALLBACKS) + [HASH_CALLBACK],
        "stack_usage_files": usage, "sites": sites,
        "hash_callback_disassembly_sha256": gate.sha256(dump.encode()),
        "source_evidence": [config, evidence(source,
            "Native attributes, checked failures and trampoline bound to source; actual imports verified in linked ELF."), module_proof,
            evidence(source_root / "src/video/probe_memory.c",
                "Seven native memory backend callbacks run synchronously on main. Their linked bodies are additional roots for frame, dynamic-stack and recursion checks; unresolved indirect dispatch keeps its allowance."),
            evidence(source_root / "src/core/probe_controller.c",
                "memory_progress is the main-thread callback passed to synchronous x4_probe_memory_run; its own route is audited as an additional root, not a new worker or an explicit-stack capacity claim.")] + hash_evidence,
        "shared_recursion": [{"members": ["x4_module_open"], "max_visits": 2, "evidence": module_proof}],
        "required_native_imports": list(NATIVE_IMPORTS),
        "limitations": ["Unknown imports/indirects carry 16 KiB assumptions, not proved runtime bounds.",
            "Worker route and wrapper envelope are conservatively summed, including opaque dispatch cost.",
            "Main and the NULL-attributes INFO thread have frame/dynamic/recursion checks but no claimed capacity margin.",
            "Memory benchmark callbacks are additional audited roots on main; indirect dispatch still carries an opaque allowance. Their separate known routes do not claim a resolved whole indirect call chain.",
            "Exactly three production hash dispatches are resolved using linked argument/CFG evidence. Other indirect destinations and unmeasured library frames retain visible allowances; the supplementary hash route is not a runtime bound.",
            "Runtime reported sizes and high-water remain console validation."]}

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--su-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--disassembly", type=Path)
    parser.add_argument("--objdump")
    args = parser.parse_args(argv)
    try:
        policy = prepare(args.elf, args.su_root, args.output, disassembly=args.disassembly, objdump=args.objdump)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(policy, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    except (ValueError, OSError, UnicodeError, subprocess.CalledProcessError) as error:
        print("Cannot prepare probe stack policy: " + str(error), file=sys.stderr)
        return 1
    print("Explicit-stack policy prepared; opaque allowances remain assumptions.")
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
