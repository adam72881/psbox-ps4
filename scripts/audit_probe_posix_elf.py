#!/usr/bin/env python3
"""Fail a native probe build on known ENOSYS imports or reachable linked stubs.

ELF64 symbol parsing is self-contained. Disassembly comes from llvm-objdump or
objdump, never from executing the ELF. Direct code references and addresses
passed as callbacks are followed; FILE function pointers and worker callbacks
also have explicit roots. This is a bounded static audit, not a proof of all
indirect targets or of runtime filesystem/kernel behavior.
"""
import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

# Pinned OpenOrbis musl PS4 branches; lstat delegates to fstatat. Generic
# syscall is deliberately not here: rtc_pthread.c implements number 20 with
# scePthreadGetthreadid and rejects other numbers. Never whitelist lstat.
STUB_NAMES = frozenset({"fstatat", "fstatat64", "lstat", "lstat64", "fchdir",
    "fdatasync", "fchmodat", "mkdirat", "readlink", "readlinkat", "renameat",
    "unlinkat", "symlink", "symlinkat", "link", "linkat", "umask", "utimensat",
    "mknod", "mknodat", "faccessat", "dup3", "__dup3", "times", "mremap", "__mremap", "futimesat"})
ROOT_NAMES = ("main", "x4_probe_controller_run")
INDIRECT_ROOTS = ("video_worker", "copy_worker", "x4_probe_thread_trampoline", "probe_stack_test_entry",
    "probe_default_stack_entry", "native_allocate", "native_map", "native_unmap", "native_release",
    "native_heap_allocate", "native_heap_free", "native_clock", "memory_progress",
    "probe_pipeline_decode_worker", "probe_pipeline_convert_worker",
    "sampler", "__stdio_read", "__stdio_write", "__stdio_seek", "__stdio_close")
REQUIRED_NATIVE_IMPORTS = ("fstat", "sceKernelFsync")
PATH_FORBIDDEN_NAMES = frozenset({"openat", "openat64", "fchmod", "sceKernelFchmod", "fstatat", "fstatat64"})
OPENAT_DIAGNOSTIC_WRAPPER = "x4_probe_files_openat_info"
OPENAT_DIAGNOSTIC_CALLER = "x4_probe_files_selfcheck"


def canonical_name(name):
    """Known libc/native aliases must not escape the name gate."""
    name = name.removesuffix("@plt").split("@", 1)[0]
    return name.lstrip("_")


def is_stub_name(name):
    return name in STUB_NAMES or canonical_name(name) in STUB_NAMES


def is_path_forbidden(name):
    return canonical_name(name) in PATH_FORBIDDEN_NAMES


def elf_symbols(body):
    if len(body) < 64 or body[:6] != b"\x7fELF\x02\x01":
        raise ValueError("requires a little-endian ELF64")
    h = struct.unpack_from("<16sHHIQQQIHHHHHH", body)
    if h[2] != 62:
        raise ValueError("requires native x86-64 ELF")
    section_at, section_size, section_count = h[6], h[11], h[12]
    if section_size < 64 or section_count == 0:
        raise ValueError("missing section table")
    sections = [struct.unpack_from("<IIQQQQIIQQ", body, section_at+i*section_size)
        for i in range(section_count)]
    result = {}
    for s in sections:
        if s[1] not in (2, 11):
            continue
        offset, size, entry = s[4], s[5], s[9]
        if entry != 24 or size % entry:
            raise ValueError("invalid ELF symbol table")
        strings = sections[s[6]]
        text = body[strings[4]:strings[4]+strings[5]]
        for at in range(offset, offset+size, entry):
            name, info, other, index, value, length = struct.unpack_from("<IBBHQQ", body, at)
            if not name or (info & 15) not in (0, 2):
                continue
            end = text.find(b"\0", name)
            if end < 0:
                raise ValueError("unterminated ELF symbol")
            label = text[name:end].decode("utf-8", errors="strict")
            row = {"name": label, "address": value, "size": length,
                "undefined": index == 0, "symbol_kind": info & 15,
                "symbol_binding": info >> 4}
            if label not in result or result[label]["undefined"]:
                result[label] = row
    return result


def parse_disassembly(text):
    blocks = {}
    current = None
    for line in text.splitlines():
        header = re.match(r"^\s*([0-9a-fA-F]+) <(.+)>:\s*$", line)
        if header:
            current = header[2]
            blocks[current] = {"address": int(header[1], 16), "references": set(),
                "indirect_instructions": 0, "call_sites": []}
            continue
        if current is None:
            continue
        # Includes calls/tail jumps and LEA callback addresses in comments.
        for match in re.finditer(r"<([^<>]+)>", line):
            target = re.sub(r"[+-]0x[0-9a-fA-F]+$", "", match[1]).removesuffix("@plt")
            if target != current:
                blocks[current]["references"].add(target)
        if re.search(r"\b(?:callq?|jmpq?)\s+(?:\*|qword ptr|\[)", line):
            blocks[current]["indirect_instructions"] += 1
        if re.search(r"\b(?:callq?|jmpq?)\s+", line):
            targets = re.findall(r"<([^<>]+)>", line)
            if targets:
                target = re.sub(r"[+-]0x[0-9a-fA-F]+$", "", targets[0]).removesuffix("@plt")
                # A jump within this same function is not an external call.
                if target != current:
                    blocks[current]["call_sites"].append(target)
    if not blocks:
        raise ValueError("empty/unrecognised disassembly")
    return blocks


def audit(symbols, blocks, roots=ROOT_NAMES, required_imports=(),
          indirect_roots=None, all_imports_in_scope=True):
    missing = [name for name in roots if name not in symbols or symbols[name]["undefined"]]
    if missing:
        raise ValueError("missing required probe roots: " + ", ".join(missing))
    by_address = {}
    for name, row in symbols.items():
        if not row["undefined"]:
            by_address.setdefault(row["address"], set()).add(name)
    block_alias = {}
    for name, row in blocks.items():
        for alias in by_address.get(row["address"], {name}):
            block_alias[alias] = name
    absent_blocks = [name for name in roots if name not in block_alias]
    if absent_blocks:
        raise ValueError("incomplete disassembly: required probe functions absent: " + ", ".join(absent_blocks))
    def aliases(name):
        if name in symbols and not symbols[name]["undefined"]:
            return by_address.get(symbols[name]["address"], {name})
        return {name}

    def is_openat_implementation(name):
        # Preserve the explicit call target. A weak alias or an arbitrary
        # symbol sharing an address cannot become a diagnostic call target.
        return name in {"openat", "_openat"}

    def passive_openat64_alias(name, referenced_name, diagnostic):
        # This exact SDK alias is bookkeeping, not a second call. It is
        # permitted only while expanding a verified diagnostic reference to
        # strong openat. Explicit references to openat64 still fail below.
        if not diagnostic or referenced_name != "openat" or name != "openat64" or is_stub_name(name):
            return False
        strong, weak = symbols.get("openat"), symbols.get(name)
        return bool(strong and weak and not strong["undefined"] and not weak["undefined"]
            and strong.get("symbol_binding") == 1 and weak.get("symbol_binding") == 2
            and strong["address"] == weak["address"])

    def implementation_names(name):
        return sorted(aliases(name))

    # The only approved exception is a non-inlined, single direct openat call
    # from the selfcheck. Inspect its linked body, not just the function name.
    # Other reachable callers fail even if the diagnostic visited it first.
    wrapper_errors = []
    wrapper_block = blocks.get(block_alias.get(OPENAT_DIAGNOSTIC_WRAPPER, OPENAT_DIAGNOSTIC_WRAPPER))
    wrapper_valid = False
    if OPENAT_DIAGNOSTIC_WRAPPER in symbols:
        if symbols[OPENAT_DIAGNOSTIC_WRAPPER]["undefined"] or not wrapper_block:
            wrapper_errors.append("diagnostic wrapper is not a defined disassembled function")
        elif any(is_stub_name(alias) or is_path_forbidden(alias)
                for alias in aliases(OPENAT_DIAGNOSTIC_WRAPPER)):
            wrapper_errors.append("diagnostic wrapper aliases a forbidden implementation")
        elif wrapper_block["indirect_instructions"]:
            wrapper_errors.append("diagnostic wrapper has an indirect call/jump")
        elif "call_sites" not in wrapper_block:
            wrapper_errors.append("diagnostic wrapper call-site evidence is missing")
        elif (len(wrapper_block["call_sites"]) != 1
                or not is_openat_implementation(wrapper_block["call_sites"][0])
                or len(wrapper_block["references"]) != 1
                or not all(is_openat_implementation(target) for target in wrapper_block["references"])):
            wrapper_errors.append("diagnostic wrapper is not exactly one direct openat call/tail jump")
        else:
            wrapper_valid = True
    visited, visited_states, parent = set(), set(), {}
    extra = [name for name in (INDIRECT_ROOTS if indirect_roots is None else indirect_roots)
        if name in symbols and not symbols[name]["undefined"]]
    absent_callbacks = [name for name in extra if name not in block_alias]
    if absent_callbacks:
        raise ValueError("incomplete disassembly: callback functions absent: " + ", ".join(absent_callbacks))
    # Audit each worker/FILE origin independently. The general traversal's
    # shared visited set must not hide a worker -> selfcheck route merely
    # because main/controller has already reached that same selfcheck.
    indirect_selfcheck_routes = []
    for origin in extra:
        origin_pending, origin_seen, origin_parent = deque([origin]), set(), {}
        while origin_pending:
            name = origin_pending.popleft()
            if name in origin_seen:
                continue
            origin_seen.add(name)
            if OPENAT_DIAGNOSTIC_CALLER in aliases(name):
                chain = [name]
                while chain[-1] in origin_parent:
                    chain.append(origin_parent[chain[-1]])
                chain.reverse()
                if chain[-1] != OPENAT_DIAGNOSTIC_CALLER:
                    chain.append(OPENAT_DIAGNOSTIC_CALLER)
                indirect_selfcheck_routes.append({"origin": origin, "route": chain})
                break
            block = blocks.get(block_alias.get(name, name))
            targets = set(implementation_names(name))
            if block:
                targets.update(block["references"])
            for target in sorted(targets):
                if target != name and target not in origin_seen:
                    origin_parent.setdefault(target, name)
                    origin_pending.append(target)
    pending = deque((name, False) for name in roots + tuple(extra))
    forbidden_states, allowed_diagnostic_states, passive_diagnostic_aliases = set(), set(), set()
    diagnostic_callers = set()
    while pending:
        state = pending.popleft()
        name, diagnostic = state
        if state in visited_states:
            continue
        visited_states.add(state)
        visited.add(name)
        forbidden_aliases = [alias for alias in aliases(name) if is_path_forbidden(alias)
            and not passive_openat64_alias(alias, name, diagnostic)]
        if forbidden_aliases:
            if diagnostic and is_openat_implementation(name) and all(
                    canonical_name(alias) == "openat" for alias in forbidden_aliases):
                allowed_diagnostic_states.add(state)
            else:
                forbidden_states.add(state)
        # A reference may use an unlisted spelling such as __fstatat while
        # sharing the exact implementation address of forbidden fstatat.
        # Traverse every defined alias at that address; banning only the
        # referenced spelling would let the real linked stub escape.
        if name in symbols and not symbols[name]["undefined"]:
            for alias in implementation_names(name):
                if passive_openat64_alias(alias, name, diagnostic):
                    # The strong call target already traverses this body,
                    # including when objdump labels it <openat64>. Do not
                    # promote the passive alias into a new call route.
                    passive_diagnostic_aliases.add((alias, name))
                    visited.add(alias)
                    continue
                alias_state = (alias, diagnostic)
                if alias != name and alias_state not in visited_states:
                    parent.setdefault(alias_state, state)
                    pending.append(alias_state)
        block = blocks.get(block_alias.get(name, name))
        if block:
            for target in sorted(block["references"]):
                target_is_wrapper = OPENAT_DIAGNOSTIC_WRAPPER in aliases(target)
                if target_is_wrapper:
                    diagnostic_callers.add(name)
                    if OPENAT_DIAGNOSTIC_CALLER not in aliases(name):
                        wrapper_errors.append("diagnostic wrapper referenced by unapproved caller: " + name)
                from_wrapper = OPENAT_DIAGNOSTIC_WRAPPER in aliases(name)
                # Context is intentionally restricted to the verified wrapper
                # and libc/native openat tail-wrapper chain. Helpers reached
                # from openat are audited as ordinary functional paths.
                target_diagnostic = (is_openat_implementation(target)
                    and ((from_wrapper and diagnostic and wrapper_valid)
                        or (diagnostic and is_openat_implementation(name))))
                if target_is_wrapper:
                    target_diagnostic = wrapper_valid and OPENAT_DIAGNOSTIC_CALLER in aliases(name)
                target_state = (target, target_diagnostic)
                if target_state not in visited_states:
                    parent.setdefault(target_state, state)
                    pending.append(target_state)
    imports = sorted(name for name, row in symbols.items() if row["undefined"])
    bad_imports = sorted(name for name in imports if is_stub_name(name)
        and (all_imports_in_scope or name in visited))
    bad_linked = sorted(name for name in visited if is_stub_name(name) and name not in imports)
    missing_native = sorted(name for name in required_imports
        if name not in symbols or not symbols[name]["undefined"])
    def route(state):
        chain, seen = [state], {state}
        while chain[-1] in parent:
            previous = parent[chain[-1]]
            if previous in seen:
                break
            seen.add(previous); chain.append(previous)
        return [item[0] for item in reversed(chain)]
    return {"schema": "X4_PROBE_POSIX_AUDIT2", "passed": not (bad_imports or bad_linked or missing_native or forbidden_states or wrapper_errors or indirect_selfcheck_routes),
        "known_ENOSYS_names": sorted(STUB_NAMES), "required_roots": list(roots),
        "import_audit_scope": "whole ELF" if all_imports_in_scope else "explicit reachable routes",
        "outside_scope_known_stub_imports": sorted(name for name in imports
            if is_stub_name(name) and name not in bad_imports),
        "required_native_imports": list(required_imports),
        "missing_required_native_imports": missing_native,
        "explicit_indirect_roots": extra, "forbidden_imports": bad_imports,
        "forbidden_indirect_selfcheck_routes": indirect_selfcheck_routes,
        "forbidden_effective_path_names": sorted(PATH_FORBIDDEN_NAMES),
        "forbidden_effective_paths": [{"name": name, "diagnostic_context": diagnostic,
            "route": route((name, diagnostic))} for name, diagnostic in sorted(forbidden_states)],
        "openat_diagnostic_exception": {"wrapper": OPENAT_DIAGNOSTIC_WRAPPER,
            "allowed_caller": OPENAT_DIAGNOSTIC_CALLER,
            "body_verified": wrapper_valid, "observed_callers": sorted(diagnostic_callers),
            "errors": sorted(set(wrapper_errors)),
            "passive_same_address_weak_aliases": [{"name": alias, "explicit_reference": target,
                "address": hex(symbols[alias]["address"]), "symbol_binding": symbols[alias]["symbol_binding"]}
                for alias, target in sorted(passive_diagnostic_aliases)],
            "allowed_routes": [route(state) for state in sorted(allowed_diagnostic_states)]},
        "reachable_linked_stubs": [{"name": name,
            "address": hex(symbols[name]["address"]) if name in symbols else None,
            "route": route(next(state for state in sorted(visited_states) if state[0] == name))} for name in bad_linked],
        "unreachable_linked_stub_names": sorted(name for name in symbols
            if is_stub_name(name) and name not in visited and name not in imports),
        "all_imports": imports, "observed_probe_references": sorted(visited),
        "reference_symbols": [dict(symbols[name], address=hex(symbols[name]["address"]))
            for name in sorted(visited) if name in symbols],
        "indirect_instruction_count": sum(blocks[block_alias.get(name,name)]["indirect_instructions"]
            for name in visited if block_alias.get(name,name) in blocks),
        "limits": ["Static referenced symbols include conditional paths that may not execute.",
            "Indirect targets outside explicit FILE and worker roots require manual source review.",
            "Import resolution does not prove PS4 filesystem permissions or runtime success.",
            "Known-name gate is not a universal detector of every possible future ENOSYS stub.",
            "The openat exception verifies only the single direct diagnostic wrapper and exact selfcheck caller; it is not a runtime permission guarantee."]}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("elf", type=Path)
    p.add_argument("--disassembly", type=Path, help="full ELF disassembly, not just main")
    p.add_argument("--objdump", help="native llvm-objdump/objdump executable")
    p.add_argument("--require-native-import", action="append", default=None,
        help="require this exact undefined native import (repeat); defaults to new probe filesystem imports")
    p.add_argument("--output", type=Path)
    args = p.parse_args(argv)
    body = args.elf.read_bytes()
    symbols = elf_symbols(body)
    if args.disassembly:
        disassembly = args.disassembly.read_text(encoding="utf-8")
    else:
        executable = args.objdump or shutil.which("llvm-objdump") or shutil.which("objdump")
        if not executable:
            raise ValueError("llvm-objdump or objdump is required; no silent audit skip")
        disassembly = subprocess.run([executable, "-d", str(args.elf)], check=True,
            text=True, stdout=subprocess.PIPE).stdout
    result = audit(symbols, parse_disassembly(disassembly), required_imports=
        tuple(args.require_native_import) if args.require_native_import is not None else REQUIRED_NATIVE_IMPORTS)
    result.update(elf_sha256=hashlib.sha256(body).hexdigest(), elf_bytes=len(body),
        disassembly_sha256=hashlib.sha256(disassembly.encode()).hexdigest())
    payload = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("passed", "elf_sha256", "forbidden_imports",
        "reachable_linked_stubs", "unreachable_linked_stub_names", "missing_required_native_imports",
        "forbidden_effective_paths", "openat_diagnostic_exception", "forbidden_indirect_selfcheck_routes")}))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print("probe POSIX audit failed: " + str(exc), file=sys.stderr)
        sys.exit(2)
