#!/usr/bin/env python3
"""Reviewable x86-64 stack gate for the isolated PS4 probe.

Measured frames (.su plus linked CFG), own-source recursion and dynamic frames
are checked independently of thread capacity. Explicit worker budgets are
checked at 50 percent. Native imports, unknown indirect destinations and
unmeasurable library frames are visible assumptions, charged 16 KiB each along
the worst path. That allowance is a policy premise, NOT a bound proved by the
SDK. Main and the NULL-attributes INFO thread remain unspecified and have no
capacity pass claim. Their own frames/dynamic usage/recursion are still gated.

The policy binds ELF/.su/source evidence hashes. No target ELF is executed.
"""
import argparse
from collections import deque
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

SPEC = importlib.util.spec_from_file_location("probe_posix_gate", Path(__file__).with_name("audit_probe_posix_elf.py"))
ELF_GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ELF_GATE)
SCHEMA = "x4-probe-stack-policy-v2"
FUNCTION_KINDS = {"bounded_source", "measured_same_binary", "bounded_dynamic", "opaque_native"}
THREAD_KINDS = {"configured_attribute", "verified_ps4_default", "elf_thread_parameter"}
SITE_KINDS = {"indirect_targets", "intra_function_jump", "unreachable_probe_branch"}


def sha256(body):
    return hashlib.sha256(body).hexdigest()


def base_name(name):
    return re.sub(r"[+-]0x[0-9a-fA-F]+$", "", name).removesuffix("@plt")


def elf_symbols(body):
    """Preserve local homonyms by address; a linked ELF may have many stage()."""
    result = ELF_GATE.elf_symbols(body)  # Retains the existing strict format gate.
    h = struct.unpack_from("<16sHHIQQQIHHHHHH", body)
    sections = [struct.unpack_from("<IIQQQQIIQQ", body, h[6] + i * h[11]) for i in range(h[12])]
    defined = {}
    for section in sections:
        if section[1] not in (2, 11):
            continue
        strings = sections[section[6]]
        labels = body[strings[4]:strings[4] + strings[5]]
        for at in range(section[4], section[4] + section[5], section[9]):
            label, info, other, index, value, size = struct.unpack_from("<IBBHQQ", body, at)
            if not label or not index or (info & 15) != 2:
                continue
            end = labels.find(b"\0", label)
            if end < 0:
                raise ValueError("unterminated ELF function name")
            name = labels[label:end].decode("utf-8", errors="strict")
            defined.setdefault(name, {})[value] = {"name": name, "address": value, "size": size,
                "undefined": False, "symbol_kind": info & 15, "symbol_binding": info >> 4}
    for name, addresses in defined.items():
        if len(addresses) > 1:
            result.pop(name, None)
            for address, row in addresses.items():
                result[f"{name}@0x{address:x}"] = row
    return result


def parse_disassembly(text):
    """Parse linked GNU/LLVM AT&T or Intel disassembly, including PIE addresses."""
    functions, current, name_addresses = {}, None, {}
    for line in text.splitlines():
        header = re.match(r"^\s*([0-9a-fA-F]+) <(.+)>:\s*$", line)
        if header:
            name, address = header[2], int(header[1], 16)
            if name in functions and functions[name]["address"] != address:
                previous = functions.pop(name)
                functions[f"{name}@0x{previous['address']:x}"] = previous
            seen = name_addresses.setdefault(name, set())
            duplicates = bool(seen - {address})
            seen.add(address)
            current = f"{name}@0x{address:x}" if duplicates else name
            functions[current] = {"name": name, "address": address, "instructions": [], "sites": []}
            continue
        if current is None:
            continue
        inst = re.match(r"^\s*([0-9a-fA-F]+):\s*(.*?)\s*$", line)
        if not inst:
            continue
        body = re.sub(r"^(?:[0-9a-fA-F]{2}\s+)+", "", inst[2]).strip()
        if not re.match(r"[a-zA-Z]", body):
            continue
        mnemonic, _, operands = body.partition(" ")
        mnemonic, operands = mnemonic.lower(), operands.strip()
        address = int(inst[1], 16)
        instruction = {"address": address, "mnemonic": mnemonic, "operands": operands, "text": body}
        functions[current]["instructions"].append(instruction)
        if not re.match(r"^(?:callq?|jmpq?)$", mnemonic):
            continue
        label = re.search(r"<([^<>]+)>", operands)
        is_indirect = bool(re.match(r"\*|(?:qword\s+ptr\s+)?\[", operands, re.I))
        target = base_name(label[1]) if label and not is_indirect else None
        if mnemonic.startswith("jmp") and target == functions[current]["name"]:
            continue
        target_at = re.match(r"(?:0x)?([0-9a-fA-F]+)\s", operands)
        functions[current]["sites"].append({"address": address, "kind": "tail" if mnemonic.startswith("jmp") else "call",
            "target": target, "target_address": int(target_at[1], 16) if target_at else None,
            "indirect": is_indirect or target is None, "instruction": body})
    if not functions:
        raise ValueError("empty or unrecognised full ELF disassembly")
    return functions


def parse_stack_usage(text, source="<fixture>"):
    """Clang/GCC .su format; keep dynamic/unbounded and duplicate records visible."""
    records = {}
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split("\t")
        if len(fields) != 3 or not re.fullmatch(r"\d+", fields[1]):
            raise ValueError(f"malformed .su row {source}:{number}")
        location, count, qualifier = fields
        match = re.match(r"^(.*?):(\d+):(?:(\d+):)?(.+)$", location)
        # Clang emits no source line for generated C++ helpers, e.g. the
        # actual native TU row `...cpp:__clang_call_terminate\t8\tstatic`.
        # The byte count/qualifier still exist. Keep that missing source line
        # explicit, and never treat a malformed count or dynamic frame as 0.
        synthetic = re.match(r"^(.*\.(?:c|cc|cpp|cxx|C|h|hh|hpp|hxx)):(.+)$", location) if not match else None
        if not match and not synthetic:
            raise ValueError(f"malformed .su function {source}:{number}")
        name = match[4] if match else synthetic[2]
        records.setdefault(name, []).append({"bytes": int(count), "qualifier": qualifier,
            "source": source, "line": number, "location": location,
            "source_line_available": match is not None})
    return records


def _direct_target(instruction):
    value = re.match(r"(?:0x)?([0-9a-fA-F]+)\b", instruction["operands"])
    return int(value[1], 16) if value else None


def _stack_change(instruction, delta, rbp):
    """Abstract stack depth. A conflict or unmodelled RSP mutation is unknown."""
    op, args = instruction["mnemonic"], instruction["operands"].split("#", 1)[0].strip()
    if op.startswith("push"):
        return delta + 8, rbp, None
    if op.startswith("pop"):
        if re.search(r"%?rsp\b", args):
            return delta, rbp, "pop into stack pointer"
        return delta - 8, rbp, None
    if op in {"leave", "leaveq"}:
        if rbp is None:
            return delta, rbp, "leave with unknown frame pointer"
        return rbp - 8, None, None
    if op in {"enter", "enterq", "enterw"} or (op.startswith("xchg") and re.search(r"%?rsp\b", args)):
        return delta, rbp, "unmodelled implicit stack pointer mutation: " + instruction["text"]
    att = re.match(r"\$((?:0x)?[0-9a-fA-F]+),\s*%rsp$", args)
    intel = re.match(r"rsp,\s*((?:0x)?[0-9a-fA-F]+)$", args, re.I)
    if op.rstrip("q") in {"add", "sub"} and (att or intel):
        value = int((att or intel)[1], 0 if (att or intel)[1].startswith("0x") else 10)
        return delta + (value if op.startswith("sub") else -value), rbp, None
    if op.rstrip("q") == "mov":
        if args in {"%rsp,%rbp", "rbp, rsp"} or re.fullmatch(r"%rsp,\s*%rbp", args):
            return delta, delta, None
        if args in {"%rbp,%rsp", "rsp, rbp"} or re.fullmatch(r"%rbp,\s*%rsp", args):
            return (rbp, rbp, None) if rbp is not None else (delta, rbp, "unknown frame pointer restore")
    # Destination RSP in either syntax. Reading it, e.g. LEA off RSP into RAX,
    # does not mutate it. Alignment/alloca/VLA need explicit bounded evidence.
    rsp_destination = bool(re.search(r",\s*%rsp\s*$", args) or re.match(r"rsp\s*,", args, re.I))
    if rsp_destination:
        return delta, rbp, "unmodelled/dynamic stack pointer mutation: " + instruction["text"]
    return delta, rbp, None


def disassembly_frame(function):
    """CFG depth bound including the return slot and x86-64 leaf red zone.

    .su is preferred. This fallback is strict: incompatible depths at a join,
    dynamic RSP, unknown computed branches, missing branch targets or stack
    growth around a loop produce an unknown frame. It is not a linear sum of
    sub instructions. Leaf red-zone accesses are conservatively charged 128B.
    """
    instructions = function["instructions"]
    if not instructions:
        return None, "no instructions"
    by_address = {item["address"]: index for index, item in enumerate(instructions)}
    pending, states, maximum, red_zone = deque([(0, 0, None)]), {}, 0, False
    while pending:
        index, depth, rbp = pending.popleft()
        previous = states.get(index)
        if previous is not None:
            if previous != (depth, rbp):
                return None, "conflicting stack depth/frame pointer at CFG join"
            continue
        states[index] = (depth, rbp)
        inst = instructions[index]
        if re.search(r"-\s*(?:0x)?[0-9a-fA-F]+\(%rsp\)|\[rsp\s*-", inst["operands"], re.I):
            red_zone = True
        depth, rbp, error = _stack_change(inst, depth, rbp)
        if error:
            return None, error
        if depth < 0:
            return None, "stack restore before entry"
        maximum = max(maximum, depth)
        op = inst["mnemonic"]
        if op.startswith("ret") or op in {"ud2", "hlt", "int3"}:
            continue
        if op.startswith("j") or op.startswith("loop"):
            target = _direct_target(inst)
            label = re.search(r"<([^<>]+)>", inst["operands"])
            outside = label and base_name(label[1]) != function["name"]
            if target not in by_address:
                if op in {"jmp", "jmpq"} and outside:
                    continue  # Tail callee is independently checked in graph.
                return None, "missing/indirect CFG branch target"
            pending.append((by_address[target], depth, rbp))
            if op in {"jmp", "jmpq"}:
                continue
        if index + 1 < len(instructions):
            pending.append((index + 1, depth, rbp))
    return maximum + 8 + (128 if red_zone else 0), "linked CFG conservative upper bound"


def validate_evidence(evidence, base):
    if not isinstance(evidence, dict) or len(str(evidence.get("detail", ""))) < 20:
        raise ValueError("proof needs an artifact and a specific bound justification")
    path = Path(evidence.get("artifact", ""))
    path = path if path.is_absolute() else base / path
    if not path.is_file() or sha256(path.read_bytes()) != evidence.get("sha256"):
        raise ValueError("missing or hash-mismatched proof artifact: " + str(path))
    return {**evidence, "artifact": str(path.resolve())}


def validate_policy(policy, elf_hash, base):
    if policy.get("schema") != SCHEMA or policy.get("elf_sha256") != elf_hash:
        raise ValueError("policy schema/ELF SHA-256 mismatch")
    fraction = policy.get("max_fraction", .5)
    if not isinstance(fraction, (int, float)) or not 0 < fraction <= .5:
        raise ValueError("max_fraction must be positive and no greater than 0.5")
    if policy.get("individual_frame_limit_bytes", 8192) != 8192:
        raise ValueError("individual frame limit must remain 8192")
    if policy.get("opaque_allowance_bytes", 16384) != 16384:
        raise ValueError("opaque allowance must remain the reviewed 16384-byte assumption")
    if not policy.get("threads"):
        raise ValueError("thread entries required")
    names = set()
    for thread in policy["threads"]:
        if thread.get("name") in names or not thread.get("name") or not thread.get("entry"):
            raise ValueError("duplicate/invalid thread entry")
        names.add(thread["name"])
        size = thread.get("stack_bytes")
        if thread.get("name") == "x4-stack-default":
            if (thread.get("entry") != "probe_default_stack_entry" or size is not None or
                    thread.get("wrapper_entry") or thread.get("informational_only") is not True or
                    thread.get("evidence", {}).get("kind") != "informational_null_attributes"):
                raise ValueError("NULL-attributes INFO thread requires exact entry, source evidence and unknown capacity without a wrapper")
            validate_evidence(thread["evidence"], base)
        elif size is not None:
            if type(size) is not int or size <= 0 or thread.get("evidence", {}).get("kind") != "configured_attribute":
                raise ValueError("explicit thread stack needs a positive configured-attribute limit")
            validate_evidence(thread["evidence"], base)
        elif thread.get("name") != "main":
            raise ValueError("only main and the source-bound NULL-attributes INFO thread may retain an unknown configured stack")
    # Resolved indirect edges may expand coverage, never suppress a call site.
    for key, site in policy.get("sites", {}).items():
        if not re.fullmatch(r".+@0x[0-9a-fA-F]+", key) or site.get("kind") != "indirect_targets":
            raise ValueError("invalid indirect call-site proof")
        if not site.get("targets") or any(not isinstance(t, str) or not t for t in site["targets"]):
            raise ValueError("indirect target proof cannot have no targets")
        validate_evidence(site.get("evidence"), base)
    for item in policy.get("source_evidence", []):
        validate_evidence(item, base)
    for item in policy.get("shared_recursion", []):
        if not item.get("members") or type(item.get("max_visits")) is not int or not 1 <= item["max_visits"] <= 4:
            raise ValueError("shared recursion needs a small reviewed source bound")
        validate_evidence(item.get("evidence"), base)


def audit_native_imports(symbols, functions, required):
    """Undefined native symbol + linked PLT, never the SDK's self-loop body."""
    rows, issues = [], []
    for name in required:
        symbol = symbols.get(name)
        plt = functions.get(name + "@plt")
        passed = bool(symbol and symbol.get("undefined") and plt and
            any(site["indirect"] for site in plt["sites"]))
        rows.append({"name": name, "undefined_symbol": bool(symbol and symbol.get("undefined")),
            "plt_address": plt["address"] if plt else None, "passed": passed,
            "interpretation": "Native dynamic import; the actual PS4 implementation is not in this ELF."})
        if not passed:
            issues.append({"kind": "expected_native_import_not_verified", "function": name,
                "reason": "requires undefined symbol and indirect PLT entry; linked SDK placeholder is forbidden"})
    return rows, issues


def audit(symbols, functions, stack_usage, policy):
    """Known-cost model plus explicit allowances, never a complete native proof."""
    issues, assumptions, frames, graph, used_sites = [], [], {}, {}, set()
    allowance = policy.get("opaque_allowance_bytes", 16384)
    frame_limit = policy.get("individual_frame_limit_bytes", 8192)
    aliases_by_address = {}
    for name, symbol in symbols.items():
        if not symbol.get("undefined"):
            aliases_by_address.setdefault(symbol["address"], []).append(name)
    block_by_address = {f["address"]: name for name, f in functions.items()}

    def resolve(name, address=None):
        if address is not None and address in block_by_address:
            return block_by_address[address]
        if name in symbols and not symbols[name].get("undefined"):
            return block_by_address.get(symbols[name]["address"], name)
        if name in symbols and symbols[name].get("undefined") and name + "@plt" in functions:
            return name + "@plt"
        return name

    def own_records(records):
        # Probe-owned units. Shared project dependencies are labelled separately
        # below, not passed off as external third-party code.
        return any(re.search(r"(?:^|[/\\])src[/\\](?:video[/\\]probe_|core[/\\]probe_controller)", r["location"]) for r in records)

    def add_assumption(kind, name, reason, **extra):
        row = {"kind": kind, "function": name, "allowance_bytes": allowance,
            "reason": reason, "proven_bound": False, **extra}
        assumptions.append(row)
        return len(assumptions) - 1

    roots = []
    for thread in policy["threads"]:
        roots.extend([thread["entry"]] + ([thread["wrapper_entry"]] if thread.get("wrapper_entry") else []))
    roots.extend(policy.get("additional_roots", []))
    pending = deque(resolve(name) for name in roots)
    for name in roots:
        if resolve(name) not in functions:
            issues.append({"kind": "missing_required_entry", "function": name})
    while pending:
        name = pending.popleft()
        if name in frames:
            continue
        fn, symbol = functions.get(name), symbols.get(name)
        aliases = aliases_by_address.get(fn["address"], [name]) if fn else [name]
        records = [r for alias in aliases for r in stack_usage.get(symbols.get(alias, {}).get("name", alias), [])]
        project_source = any(re.search(r"(?:^|[/\\])src[/\\]", r["location"]) for r in records)
        # Probe-only helpers also live under #if X4_VIDEO_PROBE in shared
        # media/video units (probe_video_batch, same_probe_mapping, etc.).
        # They must not become opaque shared-library recursion assumptions.
        probe_helper = project_source and any(re.search(r"(?:^|_)probe(?:_|$)", alias) for alias in aliases)
        own = own_records(records) or probe_helper or name in policy.get("own_functions", []) or name in {"video_worker", "copy_worker"}
        native_name = base_name(name)
        native = symbols.get(native_name, {}).get("undefined", False)
        native_plt = name.endswith("@plt") and native
        assumption_id = None
        dynamic = any(r["qualifier"] != "static" for r in records)
        valid = not (fn and symbol and (symbol.get("undefined") or symbol["address"] != fn["address"]))
        if not valid:
            issues.append({"kind": "disassembly_symbol_mismatch", "function": name})
            measured, source = None, "mismatched symbol/disassembly"
        elif native_plt or (native and not fn):
            measured, source = None, "opaque native implementation beyond dynamic import"
            assumption_id = add_assumption("native_import", name, source)
        elif dynamic:
            measured, source = None, "dynamic/unknown .su qualifier"
            issues.append({"kind": "dynamic_stack_forbidden", "function": name, "reason": source})
        elif not fn:
            measured, source = None, "missing linked body; .su alone does not describe outgoing calls"
            assumption_id = add_assumption("unknown_external", name, source)
        elif records:
            measured, source = max(r["bytes"] for r in records) + 8, ".su static + conservative return slot"
        elif fn:
            measured, source = disassembly_frame(fn)
            if measured is None and ("dynamic stack pointer" in source or "pop into stack" in source or "stack growth" in source):
                issues.append({"kind": "dynamic_stack_forbidden", "function": name, "reason": source})
            elif measured is None:
                assumption_id = add_assumption("unmeasured_linked_frame", name, source)
        else:
            measured, source = None, "missing linked body or unresolved external implementation"
            assumption_id = add_assumption("unknown_external", name, source)
        if measured is not None and measured > frame_limit:
            issues.append({"kind": "individual_frame_limit_exceeded", "function": name,
                "measured_bytes": measured, "limit_bytes": frame_limit})
        if measured is None and own and not dynamic:
            issues.append({"kind": "incomplete_probe_frame_evidence", "function": name,
                "reason": "Probe-owned code must have same-build frame and linked body; opaque-library allowances do not hide missing own-code evidence."})
        if native_name in {"alloca", "__alloca", "__builtin_alloca", "__chkstk", "___chkstk_ms"}:
            issues.append({"kind": "dynamic_stack_forbidden", "function": name, "reason": "alloca/stack-probe call"})
        frames[name] = {"bytes": measured, "source": source, "aliases": sorted(aliases),
            "own_source": own, "project_source": project_source,
            "assumption_id": assumption_id}
        graph[name] = []
        if not fn or native_plt:
            continue  # Never count a PLT shim as the native function body.
        for site in fn["sites"]:
            keys = [f"{alias}@0x{site['address']:x}" for alias in aliases]
            proofs = [(key, policy.get("sites", {})[key]) for key in keys if key in policy.get("sites", {})]
            if len(proofs) > 1:
                issues.append({"kind": "duplicate_alias_site_proofs", "function": name, "address": site["address"]})
            if proofs:
                key, proof = proofs[0]
                used_sites.add(key)
                targets = proof["targets"]
            elif site["indirect"]:
                target = f"<indirect:{name}@0x{site['address']:x}>"
                aid = add_assumption("unresolved_indirect", name, site["instruction"], address=site["address"])
                frames[target] = {"bytes": None, "source": "opaque indirect target",
                    "aliases": [], "own_source": False, "project_source": False, "assumption_id": aid}
                graph[target] = []
                graph[name].append({"callee": target, "address": site["address"], "kind": site["kind"]})
                continue
            else:
                targets = [site["target"]]
            for target in targets:
                callee = resolve(target, site.get("target_address") if not proofs and not site["indirect"] else None)
                graph[name].append({"callee": callee, "address": site["address"], "kind": site["kind"]})
                pending.append(callee)
    for key in sorted(set(policy.get("sites", {})) - used_sites):
        issues.append({"kind": "unused_or_stale_site_proof", "site": key})

    # Condense cycles. Any probe-owned source cycle is prohibited. A library SCC
    # contributes every measurable member once plus a visible opaque allowance;
    # no finite recursion proof is claimed by this policy model.
    index, indices, low, stack, active, components = 0, {}, {}, [], set(), []
    def tarjan(name):
        nonlocal index
        indices[name] = low[name] = index
        index += 1
        stack.append(name); active.add(name)
        for edge in graph[name]:
            target = edge["callee"]
            if target not in indices:
                tarjan(target); low[name] = min(low[name], low[target])
            elif target in active:
                low[name] = min(low[name], indices[target])
        if low[name] == indices[name]:
            members = []
            while True:
                target = stack.pop(); active.remove(target); members.append(target)
                if target == name:
                    break
            components.append(sorted(members))
    for name in graph:
        if name not in indices:
            tarjan(name)
    component_for = {name: i for i, members in enumerate(components) for name in members}
    costs, successors, source_bounds = {}, {}, []
    for i, members in enumerate(components):
        cyclic = len(members) > 1 or any(edge["callee"] == members[0] for edge in graph[members[0]])
        aids = [frames[name]["assumption_id"] for name in members if frames[name]["assumption_id"] is not None]
        multiplier = 1
        if cyclic:
            if any(frames[name]["own_source"] for name in members):
                issues.append({"kind": "own_source_recursion_forbidden", "members": members})
            else:
                proofs = [p for p in policy.get("shared_recursion", []) if sorted(p["members"]) == members]
                if len(proofs) == 1:
                    multiplier = proofs[0]["max_visits"]
                    source_bounds.append({"members": members, "max_visits": multiplier, "evidence": proofs[0].get("evidence")})
                else:
                    kind = "shared_project_recursion" if any(frames[name]["project_source"] for name in members) else "library_recursion"
                    aids.append(add_assumption(kind, ",".join(members),
                        "SCC measured once plus fixed allowance for unknown additional depth; not a recursion bound",
                        members=members))
        costs[i] = (multiplier * sum(frames[name]["bytes"] or 0 for name in members), multiplier * aids)
        successors[i] = sorted({component_for[edge["callee"]] for name in members for edge in graph[name]
            if component_for[edge["callee"]] != i})
    memo = {}
    known_memo = {}
    def known_demand(i):
        if i in known_memo:
            return known_memo[i]
        best, route = 0, []
        for child in successors[i]:
            amount, chain = known_demand(child)
            if amount > best:
                best, route = amount, chain
        known_memo[i] = costs[i][0] + best, components[i] + route
        return known_memo[i]

    def demand(i):
        if i in memo:
            return memo[i]
        best = (0, [], [])
        for child in successors[i]:
            candidate = demand(child)
            if candidate[0] + allowance * len(candidate[1]) > best[0] + allowance * len(best[1]):
                best = candidate
        measured, aids = costs[i]
        memo[i] = measured + best[0], aids + best[1], components[i] + best[2]
        return memo[i]

    native_rows, native_issues = audit_native_imports(symbols, functions, policy.get("required_native_imports", []))
    issues.extend(native_issues)
    rows = []
    for thread in policy["threads"]:
        entry = resolve(thread["entry"])
        amount, aids, route = demand(component_for[entry])
        largest_known, largest_known_route = known_demand(component_for[entry])
        wrapper_known = 0
        wrapper_route = []
        if thread.get("wrapper_entry"):
            wrapper_known, wrapper_aids, wrapper_route = demand(component_for[resolve(thread["wrapper_entry"])])
            amount += wrapper_known
            aids = aids + wrapper_aids
            wrapper_known_max, wrapper_known_max_route = known_demand(component_for[resolve(thread["wrapper_entry"])])
            largest_known += wrapper_known_max
            largest_known_route = wrapper_known_max_route + ["<worker entry>"] + largest_known_route
        # Native startup/trampoline cost is unknown even with explicit capacity.
        # It receives one allowance and is never described as measured usage.
        startup_id = add_assumption("thread_startup", thread["name"], "native thread/CRT startup below entry is not measured")
        aids = aids + [startup_id]
        allowance_total = allowance * len(aids)
        total = amount + allowance_total
        size = thread.get("stack_bytes")
        threshold = math.floor(size * policy.get("max_fraction", .5)) if size is not None else None
        passed = threshold is None or total <= threshold
        if not passed:
            issues.append({"kind": "thread_stack_budget_exceeded", "thread": thread["name"],
                "known_bytes": amount, "assumed_bytes": allowance_total,
                "modeled_total_bytes": total, "allowed_bytes": threshold, "route": route})
        rows.append({"name": thread["name"], "entry": entry, "stack_bytes": size,
            "stack_evidence": thread.get("evidence"), "known_path_bytes": amount,
            "known_path_interpretation": "Measured component of the largest modeled path, including wrapper envelope.",
            "largest_known_only_path_bytes": largest_known, "largest_known_only_route": largest_known_route,
            "wrapper_known_bytes": wrapper_known, "wrapper_route": wrapper_route,
            "assumption_ids": aids, "assumption_bytes": allowance_total,
            "modeled_total_bytes": total, "call_chain_bound_bytes": None,
            "allowed_bytes": threshold, "remaining_to_policy_limit_bytes": threshold - total if threshold is not None else None,
            "remaining_stack_bytes": size - total if size is not None else None,
            "capacity_check": "explicit_stack_policy_model" if size is not None else
                "not_checked_unknown_default_info_stack" if thread["name"] == "x4-stack-default" else "not_checked_unknown_main_stack",
            "worst_route": route, "passed": passed})
    additional_routes = []
    for root in policy.get("additional_roots", []):
        entry = resolve(root)
        amount, aids, route = demand(component_for[entry])
        known, known_route = known_demand(component_for[entry])
        additional_routes.append({"entry": entry, "known_path_bytes": amount,
            "largest_known_only_path_bytes": known, "largest_known_only_route": known_route,
            "assumption_ids": aids, "assumption_bytes": allowance * len(aids),
            "modeled_total_bytes": amount + allowance * len(aids), "worst_route": route,
            "interpretation": "Supplementary callback body on its caller thread; excludes caller/startup frames. Not a separate thread or a capacity check."})
    return {"schema": "x4-probe-stack-audit-v2", "passed": not issues and all(r["passed"] for r in rows),
        "threads": rows, "issues": issues, "functions": frames, "call_graph": graph,
        "assumptions": assumptions, "native_imports": native_rows, "additional_root_routes": additional_routes,
        "shared_source_recursion_bounds": source_bounds,
        "coverage": {"reachable_functions": len(frames),
            "measured_frames": sum(f["bytes"] is not None for f in frames.values()),
            "unmeasured_frames": sum(f["bytes"] is None for f in frames.values()),
            "assumptions": len(assumptions)},
        "interpretation": "Known stack cost plus explicit fixed allowances; not a proven whole-call-chain bound or PS4 measurement. Main and NULL-attributes INFO capacities remain unknown. Runtime high-water is separate evidence."}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--disassembly", type=Path)
    parser.add_argument("--objdump")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    body = args.elf.read_bytes()
    policy = json.loads(args.policy.read_text(encoding="utf-8"))
    try:
        validate_policy(policy, sha256(body), args.policy.parent)
        symbols = elf_symbols(body)
        if args.disassembly:
            dump = args.disassembly.read_text(encoding="utf-8")
        else:
            tool = args.objdump or shutil.which("llvm-objdump") or shutil.which("objdump")
            if not tool:
                raise ValueError("objdump unavailable; full linked disassembly required")
            result = subprocess.run([tool, "-drwC", str(args.elf)], check=True, capture_output=True, text=True)
            dump = result.stdout
        if policy.get("hash_callback_disassembly_sha256") and sha256(dump.encode()) != policy["hash_callback_disassembly_sha256"]:
            raise ValueError("hash callback proof belongs to a different linked disassembly")
        usage = {}
        for item in policy.get("stack_usage_files", []):
            path = Path(item["path"])
            path = path if path.is_absolute() else args.policy.parent / path
            data = path.read_bytes()
            if sha256(data) != item["sha256"]:
                raise ValueError("stale/mismatched .su build artifact: " + str(path))
            for name, records in parse_stack_usage(data.decode("utf-8"), str(path)).items():
                usage.setdefault(name, []).extend(records)
        result = audit(symbols, parse_disassembly(dump), usage, policy)
        result.update({"elf": str(args.elf.resolve()), "elf_sha256": sha256(body),
            "policy_sha256": sha256(args.policy.read_bytes()), "disassembly_sha256": sha256(dump.encode()),
            "stack_usage_files": policy.get("stack_usage_files", [])})
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        result = {"schema": "x4-probe-stack-audit-v2", "passed": False,
            "elf_sha256": sha256(body), "issues": [{"kind": "invalid_or_incomplete_evidence", "reason": str(error)}]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print("probe stack gate: " + ("PASS" if result["passed"] else "FAIL (policy violations block packaging)"))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
