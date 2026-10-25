#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read-only analysis of X4PROBE_PIPELINE1/2/3 plus its bounded metrics report.

Rates describe observed local clip progress. They do not establish streaming
support, native output PTS, full-frame coherence, or a theoretical decoder cap.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import pathlib
import re
from analyze_probe_stress_metrics import Fields, counter, stage, thread_stacks

NOT_RUN = -(1 << 31)
SCANOUT_RETURNS = ("allocate", "map", "register", "rate", "close", "unmap", "release")
RETURNS = ("source_result", "reference_result", "query_result", "compute_query_result",
           "allocate_queue_result", "create_result", "decode_result", "flush_result",
           "delete_decoder_result", "release_queue_result", "reference_close_result",
           "source_close_result", "close_result", "pipeline_result", "runtime_result")
STAGES = ("decode_call", "free_output_wait", "conversion", "stripe0", "stripe1", "stripe2",
          "decode_end_to_flip_observation", "flip_call", "submit_return_to_flip_observation",
          "new_presentation_interval", "input_delivery_interval", "scheduled_input_lateness")


def bounded_json(path):
    path = pathlib.Path(path)
    if not path.is_file() or path.stat().st_size > 8 * 1024 * 1024:
        raise ValueError("missing or excessive report")
    text = path.read_text(encoding="utf-8")
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result: raise ValueError("duplicate JSON key: " + key)
            result[key] = value
        return result
    return json.loads(text, object_pairs_hook=pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError("nonfinite JSON number")))


def analyze(case, metrics):
    f = Fields()
    case = f.node(case, "pipeline")
    metrics = f.node(metrics, "metrics")
    oneflight = case.get("schema") == "X4PROBE_PIPELINE3"
    latest = oneflight or case.get("schema") == "X4PROBE_PIPELINE2"
    if case.get("schema") not in ("X4PROBE_PIPELINE1", "X4PROBE_PIPELINE2", "X4PROBE_PIPELINE3") or metrics.get("schema") != "X4PROBE_METRICS1":
        f.issue("schema", "unsupported pipeline/metrics schema")
    ready_policy = "legacy_fifo"
    if latest:
        ready_policy = f.get(case, "ready_policy", "pipeline.ready_policy", "text", 32)
        if ready_policy != "latest":
            f.issue("pipeline.ready_policy", "schema 2/3 requires latest-ready selection")
    elif "ready_policy" in case:
        f.issue("pipeline.ready_policy", "legacy schema cannot claim the new policy")
    scanout_policy = "legacy_conversion_ahead"
    if oneflight:
        scanout_policy = f.get(case, "scanout_policy", "pipeline.scanout_policy", "text", 32)
        if scanout_policy != "one_frame_in_flight":
            f.issue("pipeline.scanout_policy", "schema 3 requires one frame in flight")
    elif "scanout_policy" in case:
        f.issue("pipeline.scanout_policy", "legacy schema cannot claim the one-flight policy")
    source = f.hex(case, "source_id", "pipeline.source_id", 40)
    build = f.get(case, "build_id", "pipeline.build_id", "text", 160)
    if build is not None and (source is None or not build.startswith(source + "-")):
        f.issue("pipeline.build_id", "build does not bind source_id")
    case_id = f.get(case, "case_id", "pipeline.case_id", "text", 80)
    if case_id is not None and not re.fullmatch(r"[A-Za-z0-9_-]+", case_id):
        f.issue("pipeline.case_id", "invalid fixed fixture identity")
    sample = f.node(case.get("sample"), "pipeline.sample")
    sample_result = {key: f.hex(sample, key, "pipeline.sample." + key, 64) for key in
                     ("clip_sha256", "index_sha256", "reference_sha256")}
    sample_result.update({key: f.get(sample, key, "pipeline.sample." + key) for key in
        ("fps_num", "fps_den", "AU_count", "duration_us", "coded_width", "coded_height", "profile_idc", "level_idc")})
    if sample_result["fps_num"] == 0 or sample_result["fps_den"] == 0:
        f.issue("pipeline.sample", "zero nominal cadence")
    converters = f.get(case, "converters", "pipeline.converters", maximum=3)
    decode_only = f.get(case, "decode_only", "pipeline.decode_only", "bool")
    if converters not in (0, 2, 3) or decode_only != (converters == 0):
        f.issue("pipeline.converters", "unsupported count or decode-only disagreement")
    mode = f.get(case, "mode", "pipeline.mode", "text", 32)
    flags = {key: f.get(case, key, "pipeline." + key, "bool") for key in ("source_eof", "cancelled", "retained")}
    if mode not in ("paced", "maximum-speed"): f.issue("pipeline.mode", "unsupported pacing mode")
    for key, expected in (("output_memory_type", 0), ("output_buffers", 4),
                          ("videoout_buffers", 0 if decode_only else 3), ("decode_pipeline_depth", 1)):
        if f.get(case, key, "pipeline." + key) != expected:
            f.issue("pipeline." + key, "outside this candidate configuration")
    for key, expected in (("input_output_PTS_association", False), ("native_result_is_not_streaming_support", True)):
        if f.get(case, key, "pipeline." + key, "bool") is not expected:
            f.issue("pipeline." + key, "interpretation contract differs")
    times = {key: f.get(case, key, "pipeline." + key) for key in
             ("started_us", "measurement_start_us", "ended_us", "input_begin_us", "native_producer_done_us")}
    start, boundary, end = (times[key] for key in ("started_us", "measurement_start_us", "ended_us"))
    interval = None
    paired = (start == f.get(metrics, "created_us", "metrics.created_us") and
              boundary == f.get(metrics, "measure_start_us", "metrics.measure_start_us") and
              end == f.get(metrics, "ended_us", "metrics.ended_us"))
    if not paired: f.issue("time_identity", "pipeline and metrics do not identify the same run")
    if all(type(value) is int for value in (start, boundary, end)):
        if end < start or (boundary and not start <= boundary < end):
            f.issue("time_order", "invalid warmup/measurement/end order")
        elif paired and boundary: interval = end - boundary
    if metrics.get("ended") is not True: f.issue("metrics.ended", "producers were not closed for export")
    if case.get("warmup_count") != 60 or case.get("warmup_basis") != ("native_outputs" if decode_only else "RGB_publications"):
        f.issue("warmup", "wrong candidate warmup basis/count")
    counters = {name: counter(metrics, name, f) for name in
                ("input_AU", "native_output", "RGB_publication", "new_presentation", "native_output_superseded", "errors")}
    if latest:
        counters["native_output_latency_discarded"] = counter(metrics, "native_output_latency_discarded", f)
    totals = {key: f.get(case, key, "pipeline." + key) for key in
              ("input_AU", "native_outputs", "measured_decode_outputs", "RGB_publications", "new_presentations", "no_picture_calls", "late_inputs", "maximum_input_late_us")}
    if (totals["measured_decode_outputs"] is not None and totals["native_outputs"] is not None and
        totals["measured_decode_outputs"] > totals["native_outputs"]):
        f.issue("pipeline.measured_decode_outputs", "measured count exceeds whole-run native count")
    if all(totals[key] is not None for key in ("native_outputs", "RGB_publications", "new_presentations")) and not totals["new_presentations"] <= totals["RGB_publications"] <= totals["native_outputs"]:
        f.issue("pipeline.owner_counts", "presentations/publications exceed their preceding stage")
    if flags["source_eof"] is True and case.get("runtime_result") == 0 and sample_result["AU_count"] is not None:
        if any(totals[key] != sample_result["AU_count"] for key in ("input_AU", "native_outputs")):
            f.issue("pipeline.owner_counts", "successful EOF requires exact AU/native output counts for these fixed no-B-frame clips")
    for metric_name, case_name in (("input_AU", "input_AU"), ("native_output", "native_outputs"),
                                   ("RGB_publication", "RGB_publications"), ("new_presentation", "new_presentations")):
        observed, actual = counters[metric_name]["total"], totals[case_name]
        if observed is not None and actual is not None and observed > actual:
            f.issue("metrics.counters." + metric_name, "observations exceed owner count")
    if decode_only and any(totals[key] not in (0, None) for key in ("RGB_publications", "new_presentations")):
        f.issue("pipeline.decode_only", "decode-only reported presentation/conversion work")

    stages = {}
    for name in STAGES + (("conversion_end_to_flip_submit",) if oneflight else ()):
        entry = stage(f, metrics, name)
        raw = metrics.get("stages", {}).get(name, {})
        result = {key: value for key, value in entry.items() if key != "values"}
        result["percentile_scope"] = "last retained measured observations, at most 4096"
        for field in ("retained_p50_us", "retained_p95_us", "retained_p99_us", "retained_max_us", "all_observed_max_us"):
            result[field] = f.get(raw, field, "metrics.stages." + name + "." + field)
        values = entry["values"]
        if values is not None:
            sorted_values = sorted(values)
            for percent in (50, 95, 99):
                expected = sorted_values[(len(values) * percent + 99) // 100 - 1] if values else 0
                if result["retained_p%d_us" % percent] != expected:
                    f.issue("metrics.stages." + name, "declared percentile differs from retained samples")
            if result["retained_max_us"] != (max(values) if values else 0):
                f.issue("metrics.stages." + name, "declared maximum differs from retained samples")
            if result["all_observed_max_us"] is not None and values and result["all_observed_max_us"] < max(values):
                f.issue("metrics.stages." + name, "whole-run stage maximum below retained maximum")
        stages[name] = result
    stages["submit_return_to_flip_observation"]["actual_pipeline_span"] = "SubmitFlip return to later matching flip status observation; not precise scanout timestamp"
    stages["decode_end_to_flip_observation"]["identity"] = "native output lease only; no AU/output PTS association"
    if oneflight:
        stages["conversion_end_to_flip_submit"]["actual_pipeline_span"] = "conversion completion observed by owner to start of successful local SubmitFlip call; excludes call duration and display wait"

    returns = {}
    for key in RETURNS:
        value = case.get(key)
        if type(value) is not int or not -(1 << 31) <= value < (1 << 31):
            f.issue("pipeline." + key, "missing or invalid native/local return")
            returns[key] = {"return": None, "status": "unavailable"}
        else:
            returns[key] = {"return": value, "status": "not_run" if value == NOT_RUN else "ok" if value == 0 else "nonzero"}
    memory = f.node(case.get("native_memory"), "pipeline.native_memory")
    memory_fields = {key: f.get(memory, key, "pipeline.native_memory." + key) for key in
        ("query_compute", "query_cpu", "query_gpu", "query_shared", "query_output",
         "decoder_total", "scanout_total", "budget", "decoder_policy_budget", "scanout_policy_reservation",
         "retained_decoder", "staging_bytes", "reference_scratch_only_bytes")}
    native_total = None
    if memory_fields["decoder_total"] is not None and memory_fields["scanout_total"] is not None:
        native_total = memory_fields["decoder_total"] + memory_fields["scanout_total"]
    policy_over = (native_total is not None and memory_fields["budget"] is not None and native_total > memory_fields["budget"])
    if memory_fields["decoder_total"] is not None and memory_fields["decoder_policy_budget"] is not None:
        policy_over |= memory_fields["decoder_total"] > memory_fields["decoder_policy_budget"]
    policy_rejected = bool(policy_over and case.get("create_result") == NOT_RUN)
    if policy_over and case.get("create_result") == 0:
        f.issue("native_memory", "native allocation plan exceeded policy despite Create")
    if memory_fields["staging_bytes"] not in (0, None): f.issue("native_memory.staging_bytes", "intermediate copy staging present")
    if all(memory_fields[key] is not None for key in ("budget", "decoder_policy_budget", "scanout_policy_reservation")) and memory_fields["decoder_policy_budget"] + memory_fields["scanout_policy_reservation"] > memory_fields["budget"]:
        f.issue("native_memory", "decoder policy plus reserved scanout exceeds combined budget")
    scanout = f.node(case.get("scanout"), "pipeline.scanout")
    scanout_result = {}
    for key in SCANOUT_RETURNS:
        value = scanout.get(key)
        if type(value) is not int or not -(1 << 31) <= value < (1 << 31):
            f.issue("pipeline.scanout." + key, "missing or invalid native return")
            scanout_result[key] = {"return": None, "status": "unavailable"}
        else:
            scanout_result[key] = {"return": value, "status": "not_run" if value == NOT_RUN else "ok" if value == 0 else "nonzero"}
    scanout_result["display_error"] = f.get(scanout, "display_error", "pipeline.scanout.display_error", "bool")
    scanout_errors = [key for key in SCANOUT_RETURNS if scanout_result[key]["status"] == "nonzero"]
    if decode_only and any(scanout_result[key]["status"] != "not_run" for key in SCANOUT_RETURNS):
        f.issue("pipeline.scanout", "decode-only unexpectedly executed scanout operations")
    occupancy = None
    if oneflight:
        raw = f.node(case.get("videoout_occupancy"), "pipeline.videoout_occupancy")
        occupancy = {key: f.get(raw, key, "pipeline.videoout_occupancy." + key) for key in
                     ("observations", "omitted", "peak_occupied", "peak_in_flight", "final_occupied", "final_in_flight")}
        occupancy["scope"] = f.get(raw, "scope", "pipeline.videoout_occupancy.scope", "text", 80)
        if occupancy["scope"] != "main_loop_observations_not_time_weighted":
            f.issue("pipeline.videoout_occupancy.scope", "unknown occupancy sampling scope")
        if raw.get("final_result") != 0:
            f.issue("pipeline.videoout_occupancy.final_result", "owner could not inspect final display state")
        for key in ("occupied_samples", "in_flight_samples"):
            values = raw.get(key)
            valid = isinstance(values, list) and len(values) == 4 and all(type(v) is int and v >= 0 for v in values)
            if not valid:
                f.issue("pipeline.videoout_occupancy." + key, "expected four nonnegative owner buckets")
                occupancy[key] = None
            else:
                occupancy[key] = values
                if occupancy["observations"] is not None and sum(values) != occupancy["observations"]:
                    f.issue("pipeline.videoout_occupancy." + key, "buckets disagree with owner observations")
                if values[3] or (key == "in_flight_samples" and values[2]):
                    f.issue("pipeline.videoout_occupancy." + key, "one-flight ownership exceeded")
        if occupancy["peak_occupied"] is not None and occupancy["peak_occupied"] > 2:
            f.issue("pipeline.videoout_occupancy.peak_occupied", "more than screen plus one in-flight frame")
        if occupancy["peak_in_flight"] is not None and occupancy["peak_in_flight"] > 1:
            f.issue("pipeline.videoout_occupancy.peak_in_flight", "more than one frame in flight")
        if case.get("retained") is False and case.get("scanout", {}).get("close") in (0, NOT_RUN) and any(
                occupancy[key] not in (0, None) for key in ("final_occupied", "final_in_flight")):
            f.issue("pipeline.videoout_occupancy", "released VideoOut retains ledger ownership")
        if decode_only and any(occupancy[key] not in (0, None) for key in
                              ("observations", "peak_occupied", "peak_in_flight")):
            f.issue("pipeline.videoout_occupancy", "decode-only unexpectedly used scanout")
    ring = f.node(case.get("ring"), "pipeline.ring")
    ring_result = {key: f.get(ring, key, "pipeline.ring." + key) for key in
        ("peak_occupied", "peak_ready", "discarded_ready", "cancelled_leases", "waits", "rejected", "final_occupied")}
    if latest:
        for key in ("discarded_ring_full", "discarded_latency"):
            ring_result[key] = f.get(ring, key, "pipeline.ring." + key)
        if ring_result["discarded_ring_full"] != ring_result["discarded_ready"]:
            f.issue("pipeline.ring", "legacy ring-full alias does not match")
        for metric_name, owner_key in (("native_output_superseded", "discarded_ring_full"),
                                       ("native_output_latency_discarded", "discarded_latency")):
            observed, owner = counters[metric_name]["total"], ring_result[owner_key]
            if observed is not None and owner is not None and observed > owner:
                f.issue("metrics.counters." + metric_name, "observed discards exceed owner count")
        if (not decode_only and flags["source_eof"] is True and flags["cancelled"] is False and
            case.get("runtime_result") == 0 and all(type(v) is int for v in
                (totals["native_outputs"], totals["RGB_publications"], ring_result["discarded_latency"], ring_result["discarded_ring_full"]))):
            accounted = totals["RGB_publications"] + ring_result["discarded_latency"] + ring_result["discarded_ring_full"] + 2
            if accounted != totals["native_outputs"]:
                f.issue("pipeline.owner_counts", "native outputs do not match RGB plus both discard reasons plus two reference-only outputs")
    if any(ring_result[key] is not None and ring_result[key] > 4 for key in ("peak_occupied", "peak_ready", "final_occupied")):
        f.issue("pipeline.ring", "occupancy exceeds four owned outputs")
    layout = f.node(case.get("layout"), "pipeline.layout")
    layout_result = {key: f.get(layout, key, "pipeline.layout." + key) for key in
        ("verdict", "checks", "matches", "visible_width", "visible_height", "native_width", "native_height", "pitch", "storage_height", "UV_offset", "returned_bytes", "owned_bytes")}
    if layout_result["verdict"] == 1:
        w, h, pitch, sh, uv, owned, returned = (layout_result[key] for key in
            ("visible_width", "visible_height", "pitch", "storage_height", "UV_offset", "owned_bytes", "returned_bytes"))
        if all(value is not None for value in (w, h, pitch, sh, uv, owned, returned)):
            if (w, h) not in ((1280, 720), (1920, 1080)) or pitch < w or sh < h or uv != pitch * sh or uv + pitch * ((h + 1) // 2) > returned or returned > owned:
                f.issue("pipeline.layout", "verified layout has inconsistent visible/stride/extent bounds")
    score_rows = case.get("reference_scores")
    exact_rows = 0
    if not isinstance(score_rows, list) or len(score_rows) != 3:
        f.issue("pipeline.reference_scores", "three validation outputs required")
        score_rows = []
    for row in score_rows:
        if not isinstance(row, list) or len(row) > 16:
            f.issue("pipeline.reference_scores", "invalid bounded layout candidates")
            continue
        exact = False
        for score in row:
            if not isinstance(score, dict):
                f.issue("pipeline.reference_scores", "invalid reference score"); continue
            mae = score.get("MAE")
            if type(mae) not in (int, float) or not math.isfinite(mae) or mae < 0:
                f.issue("pipeline.reference_scores.MAE", "invalid finite nonnegative score"); continue
            if mae == 0 and score.get("max_delta") == 0 and score.get("alpha_failures") == 0 and score.get("accepted_references") == 1 and score.get("identity_ambiguous") is False:
                exact = True
        exact_rows += int(exact)
    stacks = thread_stacks(case, metrics)
    for issue in stacks["issues"]: f.issue("thread_stacks", issue)
    for slot in range(1 + (converters or 0)):
        entries = stacks["threads"]
        if slot >= len(entries) or entries[slot].get("status") != "recorded":
            if case.get("create_result") == 0: f.issue("thread_stacks", "started case lacks complete active thread evidence")
            continue
        raw = entries[slot].get("raw", {})
        if raw.get("configured_bytes") != 262144 or raw.get("reported_bytes") != 262144:
            f.issue("thread_stacks", "active thread does not report explicit 256 KiB")
    presentation_rate = None
    measured_new = counters["new_presentation"]["measured"]
    if not decode_only and interval and measured_new is not None:
        presentation_rate = measured_new * 1_000_000 / interval
    decode_rate = None
    if interval and totals["measured_decode_outputs"] is not None:
        decode_rate = totals["measured_decode_outputs"] * 1_000_000 / interval
    producer_interval = None
    producer_rate = None
    done = times["native_producer_done_us"]
    if interval and type(done) is int and boundary < done <= end:
        producer_interval = done - boundary
        producer_rate = totals["measured_decode_outputs"] * 1_000_000 / producer_interval if totals["measured_decode_outputs"] is not None else None
    outcome = "indeterminate"
    nonzero = [key for key, value in returns.items() if value["status"] == "nonzero"]
    errors = [key for key in nonzero if not (key in ("source_result", "reference_result", "pipeline_result", "runtime_result") and case.get(key) == 3)]
    if flags["retained"] is True or memory_fields["retained_decoder"]:
        outcome = "resources_retained_stop"
    elif policy_rejected: outcome = "policy_rejected_not_decoder_incompatibility"
    elif errors or scanout_errors or scanout_result["display_error"]: outcome = "native_or_local_error"
    elif flags["cancelled"] is True: outcome = "cancelled"
    elif all(row["status"] == "ok" for row in returns.values()) and all(scanout_result[key]["status"] == ("not_run" if decode_only else "ok") for key in SCANOUT_RETURNS) and flags["source_eof"] is True and layout_result["verdict"] == 1 and exact_rows == 3 and ring_result["final_occupied"] == 0:
        outcome = "reported_local_completion_with_three_exact_references"
    coverage = {key: f.get(metrics, key, "metrics." + key) for key in ("omitted_events", "dirty_resets", "events_after_end")}
    if f.issues and outcome == "reported_local_completion_with_three_exact_references":
        outcome = "indeterminate_invalid_or_incomplete_report"
    result = {
        "schema": "X4PROBE_PIPELINE_ANALYSIS1", "report_validation": "PASS" if not f.issues else "INVALID_OR_INCOMPLETE",
        "issues": f.issues, "outcome": outcome, "source_id": source, "build_id": build,
        "case_id": case_id, "sample": sample_result, "converters": converters, "mode": mode, "decode_only": decode_only, "flags": flags,
        "ready_policy": ready_policy,
        "streaming_support_proven": False, "console_execution_independently_confirmed": False,
        "input_output_PTS_association": False, "timing": dict(times, measured_interval_us=interval,
            end_includes_teardown=True, warmup_basis=case.get("warmup_basis"), measured_producer_interval_us=producer_interval),
        "rates": {"observed_new_presentations_per_second": presentation_rate,
                  "native_outputs_per_second_same_end_interval": decode_rate,
                  "native_outputs_per_second_before_producer_end": producer_rate,
                  "new_presentation_formula": "metrics.new_presentation.stable_observed * 1e6 / (ended_us - measurement_start_us)",
                  "decode_formula": "measured_decode_outputs * 1e6 / denominator stated above",
                  "paced_input_limits_decode_only_ceiling": mode == "paced", "rates_are_observations_not_guaranteed_capacity": True},
        "counts": totals, "counters": counters, "stages_us": stages, "coverage": coverage,
        "returns": returns, "nonzero_returns": nonzero, "native_memory": dict(memory_fields, planned_total=native_total),
        "scanout": scanout_result, "scanout_nonzero_returns": scanout_errors,
        "scanout_policy": scanout_policy, "videoout_occupancy": occupancy,
        "ring": ring_result, "layout": layout_result, "color": {"exact_first_outputs": exact_rows,
            "reference_scores": score_rows, "later_frames_only_geometry_checked": True,
            "scanout_color_requires_console_observation": True}, "thread_stacks": stacks,
        "limitations": ["High-water is a pattern observation with excluded stack regions, not proof of absolute peak.",
                        "Decode calls with no picture are recorded; no AU is associated with an output without output PTS.",
                        "Flip timing ends at a polled matching status, not a measured physical display timestamp.",
                        "Diagnostic omissions can reduce observed counters; report owner counters and omissions separately.",
                        "Decode-only has no presentation FPS; paced mode cannot alone establish an unlimited decode ceiling."]}
    if f.issues:
        result["rates"] = {key: None if key.endswith("per_second") or key.startswith("native_outputs_per_second") else value
                           for key, value in result["rates"].items()}
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--case", required=True, type=pathlib.Path)
    ap.add_argument("--metrics", required=True, type=pathlib.Path)
    ap.add_argument("--output", required=True, type=pathlib.Path)
    args = ap.parse_args()
    result = analyze(bounded_json(args.case), bounded_json(args.metrics))
    result["input_files"] = [{"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                             for path in (args.case, args.metrics)]
    with args.output.open("x", encoding="utf-8") as output:
        output.write(json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False) + "\n")
    print(json.dumps({"report_validation": result["report_validation"], "outcome": result["outcome"], "rates": result["rates"]}, ensure_ascii=False))
    if result["report_validation"] != "PASS": raise SystemExit(2)


if __name__ == "__main__": main()
