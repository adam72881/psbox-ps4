#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Conservative, read-only local-probe analysis. Unknown is never an implicit zero.

Only observed admissions are interpreted. No output PTS, decoder/IDR cost
association, proven stable stream or native capability is inferred.
"""
import argparse
import json
import math
import pathlib
import re
import statistics

UINT64_MAX = (1 << 64) - 1
RECENT_SAMPLES = 32
MIN_RECOVERY_SAMPLES = 64
MAX_CADENCE_PHASES = 128
COUNTER_NAMES = (
    "input_AU", "native_output", "RGB_converted", "RGB_publication",
    "new_presentation", "repeat_presentation", "native_output_superseded",
    "RGB_superseded", "RGB_prepublication_superseded", "errors",
)


class Fields:
    def __init__(self):
        self.issues = []

    def issue(self, path, reason):
        self.issues.append({"field": path, "reason": reason})

    def node(self, value, path):
        if type(value) is dict:
            return value
        self.issue(path, "missing or invalid object")
        return {}

    def get(self, node, key, path, kind="uint", maximum=UINT64_MAX):
        if key not in node:
            self.issue(path, "missing")
            return None
        value = node[key]
        if kind == "uint":
            valid = type(value) is int and 0 <= value <= maximum
        elif kind == "rc":
            valid = type(value) is int and -(1 << 31) < value < (1 << 31)
        elif kind == "bool":
            valid = type(value) is bool
        else:
            valid = type(value) is str and 0 < len(value) <= maximum
        if not valid:
            self.issue(path, "invalid type/range or unavailable sentinel")
            return None
        return value

    def hex(self, node, key, path, digits):
        value = self.get(node, key, path, "text", digits)
        if value is not None and not re.fullmatch(r"[0-9a-fA-F]{" + str(digits) + "}", value):
            self.issue(path, "invalid hash/identity")
            return None
        return value


def counter(metrics, name, fields=None):
    f = fields or Fields()
    root = f.node(metrics.get("counters"), "metrics.counters")
    value = f.node(root.get(name), "metrics.counters." + name)
    keys = {"total": "total_observed", "measured": "stable_observed",
            "timestamped": "timestamped_count", "timestamp_omissions": "timestamp_omissions",
            "untimed": "untimed_stable_units", "first_us": "measured_first_us", "last_us": "measured_last_us"}
    result = {out: f.get(value, key, "metrics.counters." + name + "." + key) for out, key in keys.items()}
    if result["total"] is not None and result["measured"] is not None and result["measured"] > result["total"]:
        f.issue("metrics.counters." + name, "measured exceeds total")
        result["total"] = result["measured"] = None
    if result["timestamped"] is not None and result["measured"] is not None:
        if result["timestamped"] > result["measured"] or (result["untimed"] is not None and
                result["untimed"] != result["measured"] - result["timestamped"]):
            f.issue("metrics.counters." + name, "timestamp/count accounting inconsistent")
            result["timestamped"] = result["untimed"] = None
    first, last = result["first_us"], result["last_us"]
    if result["timestamped"] == 0 and first == 0 and last == 0:
        result["first_us"] = result["last_us"] = None  # Explicit no-event sentinels, not clocks.
        result["timestamps_status"] = "no timed events observed"
    elif result["timestamped"] == 0 and first is not None and last is not None:
        f.issue("metrics.counters." + name, "no-event count has nonzero timestamps")
        result["first_us"] = result["last_us"] = None
        result["timestamps_status"] = "unknown"
    elif first is not None and last is not None and (first == 0 or last < first):
        f.issue("metrics.counters." + name, "invalid event timestamp interval")
        result["first_us"] = result["last_us"] = None
        result["timestamps_status"] = "unknown"
    else:
        result["timestamps_status"] = "observed" if first is not None and last is not None else "unknown"
    return result


def stage(fields, metrics, name):
    stages = fields.node(metrics.get("stages"), "metrics.stages")
    node = fields.node(stages.get(name), "metrics.stages." + name)
    prefix = "metrics.stages." + name + "."
    result = {key: fields.get(node, key, prefix + key) for key in
              ("accepted_observations", "retained", "overwritten", "stage_omitted",
               "first_observation_us", "last_observation_us")}
    values = node.get("retained_samples_us")
    if type(values) is not list or len(values) > 4096 or any(type(v) is not int or not 0 <= v <= UINT64_MAX for v in values):
        fields.issue(prefix + "retained_samples_us", "missing/invalid bounded integer observations")
        values = None
    result["values"] = values
    if values is not None and result["retained"] is not None and result["retained"] != len(values):
        fields.issue(prefix + "retained", "declared retained count differs from array")
        result["retained"] = None
    accepted, retained, overwritten = (result[k] for k in ("accepted_observations", "retained", "overwritten"))
    if all(v is not None for v in (accepted, retained, overwritten)) and (
            accepted != retained + overwritten or (overwritten > 0 and retained != 4096)):
        fields.issue(prefix, "ring accounting inconsistent")
        result["accepted_observations"] = result["retained"] = result["overwritten"] = None
    return result


def thread_stacks(case, metrics):
    """Optional additive evidence; never infer a capacity or peak from absence."""
    node = case.get("thread_stacks")
    other = metrics.get("thread_stacks")
    result = {"status": "unavailable", "threads": [], "issues": [],
              "peak_is_absolute_proof": False, "main_high_water_measured": False}
    if node is None:
        node = other
    elif other is not None and node != other:
        result["issues"].append("case and metrics stack reports differ")
    if node is None:
        return result
    if not isinstance(node, dict) or node.get("schema") != "X4PROBE_STACK1" or not isinstance(node.get("slots"), list) or len(node["slots"]) != 4:
        result["issues"].append("unsupported or malformed four-slot stack report")
        return result
    result["status"] = "observed fields with stated coverage limits"
    for slot, item in enumerate(node["slots"]):
        entry = {"slot": slot, "status": "not recorded", "pattern_high_water_bytes": None,
                 "high_water_is_lower_bound": True, "painted_floor_reached": None}
        if item is None:
            result["threads"].append(entry)
            continue
        if not isinstance(item, dict):
            result["issues"].append(f"slot {slot}: invalid report")
            result["threads"].append(entry)
            continue
        entry.update(status="recorded", raw=item)
        sizes = ("configured_bytes", "reported_bytes", "guard_bytes", "painted_bytes", "untouched_bytes",
                 "high_water_bytes", "low_excluded_bytes", "top_excluded_bytes")
        valid = all(type(item.get(key)) is int and 0 <= item[key] <= UINT64_MAX for key in sizes)
        if not valid:
            result["issues"].append(f"slot {slot}: invalid byte count")
        elif item["painted_bytes"] + item["low_excluded_bytes"] + item["top_excluded_bytes"] != item["reported_bytes"] or item["untouched_bytes"] > item["painted_bytes"] or item["high_water_bytes"] > item["reported_bytes"]:
            if item.get("pattern_ready") is True:
                valid = False
                result["issues"].append(f"slot {slot}: inconsistent painted coverage")
        native_returns = all(type(item.get(key)) is int and item[key] == 0 for key in ("measure_rc", "join_rc"))
        if valid and item.get("pattern_ready") is True and item.get("started") is True and item.get("finished") is True and native_returns:
            entry["pattern_high_water_bytes"] = item["high_water_bytes"]
            entry["painted_floor_reached"] = item["untouched_bytes"] == 0
        else:
            entry["status"] = "partial or failed observation; no measured peak"
        result["threads"].append(entry)
    return result


def analyze(case, metrics):
    f = Fields()
    case = f.node(case, "case")
    metrics = f.node(metrics, "metrics")
    schemas = case.get("schema") == "X4PROBE_CASE1" and metrics.get("schema") == "X4PROBE_METRICS1"
    if not schemas:
        f.issue("schema", "missing or unsupported case/metrics schema")
    sample = f.node(case.get("sample"), "case.sample")
    fps_num = f.get(sample, "fps_num", "case.sample.fps_num", maximum=(1 << 32) - 1)
    fps_den = f.get(sample, "fps_den", "case.sample.fps_den", maximum=(1 << 32) - 1)
    if fps_num == 0 or fps_den == 0:
        f.issue("case.sample.fps", "zero cadence is invalid")
        fps_num = fps_den = None
    period = 1000000 * fps_den / fps_num if fps_num is not None and fps_den is not None else None
    mode = f.get(case, "mode", "case.mode", "text", 32)
    if mode not in ("paced", "maximum-speed"):
        if mode is not None:
            f.issue("case.mode", "unsupported mode")
        mode = None
    source_id = f.hex(case, "source_id", "case.source_id", 40)
    case_id = f.get(case, "case_id", "case.case_id", "text", 80)
    if case_id is not None and not re.fullmatch(r"[A-Za-z0-9_-]+", case_id):
        f.issue("case.case_id", "invalid identifier")
        case_id = None
    build_id = f.get(case, "build_id", "case.build_id", "text", 128)
    if build_id is not None and (source_id is None or not re.fullmatch(
            re.escape(source_id) + r"-n[01]-g(?:0|45)-r[01]-vc[01]-p[01]-i[mb]", build_id)):
        f.issue("case.build_id", "not a recognized build bound to source_id")
        build_id = None
    hashes = {k: f.hex(sample, k, "case.sample." + k, 64) for k in
              ("clip_sha256", "index_sha256", "reference_sha256")}

    created = f.get(metrics, "created_us", "metrics.created_us")
    boundary = f.get(metrics, "measure_start_us", "metrics.measure_start_us")
    ended = f.get(metrics, "ended_us", "metrics.ended_us")
    case_boundary = f.get(case, "measurement_start_us", "case.measurement_start_us")
    warmup = f.get(case, "observed_warmup_us", "case.observed_warmup_us")
    temporal = "unknown"
    if all(v is not None for v in (created, boundary, ended, case_boundary, warmup)):
        if ended < created or boundary != case_boundary or (boundary > 0 and (boundary < created or boundary > ended)):
            temporal = "inconsistent"
            f.issue("time_identity", "clock order or measurement boundaries disagree")
        elif boundary == 0:
            temporal = "unmarked: pairing before measurement not established"
        elif warmup > boundary or boundary - warmup != created:
            temporal = "inconsistent"
            f.issue("time_identity", "case warmup does not bind metrics.created_us")
        else:
            temporal = "consistent measured interval"
    time_valid = temporal == "consistent measured interval"
    span = ended - boundary if time_valid else None
    ended_flag = f.get(metrics, "ended", "metrics.ended", "bool")
    finished = f.get(case, "finished", "case.finished", "bool")
    close_rc = f.get(case, "close_result", "case.close_result", "rc")
    pipeline_rc = f.get(case, "pipeline_result", "case.pipeline_result", "rc")
    end_reason = f.get(metrics, "end_reason", "metrics.end_reason")
    if end_reason not in (1, 2, 3, 4):
        if end_reason is not None:
            f.issue("metrics.end_reason", "unknown end reason")
        end_reason = None
    omissions = f.get(metrics, "omitted_events", "metrics.omitted_events")
    resets = f.get(metrics, "dirty_resets", "metrics.dirty_resets")
    after_end = f.get(metrics, "events_after_end", "metrics.events_after_end")
    safely_finished = finished is True and close_rc == 0 and ((pipeline_rc == 0 and end_reason == 1) or (pipeline_rc == 3 and end_reason == 2))
    recording = safely_finished and ended_flag is True and omissions == 0 and resets == 0 and after_end == 0
    coverage_known = all(v is not None for v in (finished, close_rc, pipeline_rc, end_reason, ended_flag, omissions, resets, after_end))
    observed = {name: counter(metrics, name, f) for name in COUNTER_NAMES}
    missing = [name for name, entry in observed.items() if any(entry[k] is None for k in
               ("total", "measured", "timestamped", "timestamp_omissions", "untimed"))]
    for entry in observed.values():
        fields_known = all(entry[k] is not None for k in ("total", "measured", "timestamped", "timestamp_omissions", "untimed"))
        event_times_known = entry["timestamps_status"] != "unknown"
        event_times_valid = entry["timestamps_status"] == "no timed events observed" or (
            time_valid and entry["first_us"] is not None and entry["last_us"] is not None and
            boundary <= entry["first_us"] <= entry["last_us"] <= ended)
        covered = bool(recording and time_valid and fields_known and event_times_known and event_times_valid and entry["timestamp_omissions"] == 0)
        entry["complete_measured_count"] = entry["measured"] if covered else None
        entry["coverage_complete"] = covered if coverage_known and fields_known and event_times_known else None

    latency = stage(f, metrics, "scheduled_input_lateness")
    decode = stage(f, metrics, "decode_call")
    values = latency["values"]
    n = len(values) if values is not None else None
    input_count = observed["input_AU"]
    ring_known = all(latency[k] is not None for k in ("accepted_observations", "retained", "overwritten", "stage_omitted"))
    clock_known = latency["first_observation_us"] is not None and latency["last_observation_us"] is not None
    times_match = bool(time_valid and clock_known and n and boundary <= latency["first_observation_us"] <=
                       latency["last_observation_us"] <= ended and
                       input_count["first_us"] == latency["first_observation_us"] and
                       input_count["last_us"] == latency["last_observation_us"])
    count_match = bool(ring_known and input_count["measured"] is not None and input_count["timestamped"] is not None and
                       input_count["measured"] == input_count["timestamped"] == latency["accepted_observations"] and
                       input_count["timestamp_omissions"] == 0 and input_count["untimed"] == 0)
    complete = bool(schemas and recording and time_valid and values is not None and ring_known and
                    latency["overwritten"] == 0 and latency["stage_omitted"] == 0 and times_match and count_match)
    if values is not None and created is not None and ended is not None and ended >= created and any(v > ended - created for v in values):
        f.issue("metrics.stages.scheduled_input_lateness.retained_samples_us", "lateness exceeds the observed run lifetime")
        values = None
        n = None
        complete = False
    pacing_consistent = None
    if complete and mode == "paced" and period is not None and n:
        # Only constant-cadence trusted local input indices are interpreted.
        # Admission time = scheduled input time + admission lateness. This
        # validates every admission and both endpoints, never decoded-output
        # PTS. The first measured AU ordinal is unknown: enumerate only the
        # finite rational-cadence phases (three for the current 30/60 FPS).
        step_num = 1000000 * fps_den
        divisor = math.gcd(step_num, fps_num)
        phases = fps_num // divisor
        if phases > MAX_CADENCE_PHASES:
            f.issue("time_identity.input_observations", "cadence phase count exceeds bounded validation")
            complete = False
        else:
            observed_span = latency["last_observation_us"] - latency["first_observation_us"]
            pacing_consistent = False
            for phase in range(phases):
                residue = phase * divisor
                last_relative = (residue + (n - 1) * step_num) // fps_num + values[-1] - values[0]
                if last_relative != observed_span:
                    continue
                previous = 0
                valid_phase = True
                for i, value in enumerate(values):
                    relative = (residue + i * step_num) // fps_num + value - values[0]
                    if relative < previous:
                        valid_phase = False
                        break
                    previous = relative
                if valid_phase:
                    pacing_consistent = True
                    break
            if not pacing_consistent:
                f.issue("time_identity.input_observations", "count/cadence/lateness admissions or endpoints inconsistent")
                complete = False
    metadata_valid = bool(schemas and case_id and source_id and build_id and all(v is not None for v in hashes.values()))

    trend = {"verdict": "indeterminate", "reasons": [], "admission_is_not_output_latency": True,
             "sample_count": n, "overwritten": latency["overwritten"], "omitted": latency["stage_omitted"],
             "input_endpoint_pacing_consistent": pacing_consistent,
             "input_admission_pacing_consistent": pacing_consistent,
             "scope": "retained tail only" if latency["overwritten"] is not None and latency["overwritten"] > 0 else "retained measured observations" if latency["overwritten"] == 0 else "unknown",
             "retained_start_timestamp_us": latency["first_observation_us"] if complete else None,
             "includes_first_observed_measured_sample": True if complete and n else False if latency["overwritten"] is not None and latency["overwritten"] > 0 else None,
             "one_nominal_frame_us": period, "recent_sample_count": None, "recent_max_us": None,
             "recent_last_us": None, "recent_exceedances": None, "recent_median_change_us": None,
             "first_retained_quarter_median_us": None, "last_retained_quarter_median_us": None,
             "median_change_us": None, "recent_tail_status": "unknown",
             "recovery_rule": "64 observations; last32 all within nominal frame; recent and quartile median changes<=0; explicit complete recording/time/count coverage and safe final state"}
    if values:
        recent = values[-RECENT_SAMPLES:]
        half = len(recent) // 2
        trend.update({"recent_sample_count": len(recent), "recent_max_us": max(recent), "recent_last_us": recent[-1]})
        if half:
            trend["recent_median_change_us"] = statistics.median(recent[-half:]) - statistics.median(recent[:half])
        if len(values) >= 8:
            quarter = len(values) // 4
            first, last = statistics.median(values[:quarter]), statistics.median(values[-quarter:])
            trend.update({"first_retained_quarter_median_us": first, "last_retained_quarter_median_us": last, "median_change_us": last - first})
        if period is not None:
            trend["recent_exceedances"] = sum(v * fps_num > 1000000 * fps_den for v in recent)
            trend["recent_tail_status"] = "deadline exceedances observed in retained recent admissions" if trend["recent_exceedances"] else "retained recent admissions within nominal frame"
    if mode == "maximum-speed":
        trend["verdict"] = "not applicable to maximum-speed mode"
    else:
        checks = ((mode == "paced", "mode unknown"), (period is not None, "cadence unknown"),
                  (metadata_valid, "case/build/sample identity incomplete"), (time_valid, "temporal identity not established"),
                  (complete, "recording/count/time coverage incomplete or overwritten"),
                  (safely_finished, "safe completed run not established"),
                  (n is not None and n >= MIN_RECOVERY_SAMPLES, "fewer than 64 observations"),
                  (span is not None and period is not None and span >= (RECENT_SAMPLES - 1) * period, "recent observation span insufficient"))
        trend["reasons"] = [reason for valid, reason in checks if not valid]
        if not trend["reasons"]:
            if trend["recent_exceedances"]:
                trend["verdict"] = "recent admission deadline exceedances observed; recovery not established"
            elif trend["recent_median_change_us"] > 0 or trend["median_change_us"] > 0:
                trend["verdict"] = "recent/retained admission trend increasing; recovery indeterminate"
            elif max(values[:-RECENT_SAMPLES]) * fps_num > 1000000 * fps_den:
                trend["verdict"] = "admission lag recovery observed in the covered recent window"
            else:
                trend["verdict"] = "recent covered admissions within nominal frame; no prior lag recovery demonstrated"
    whole = {"verdict": "only observed measured admissions assessed" if complete else "indeterminate: measured recording coverage unknown/incomplete",
             "all_observed_measured_lateness_retained": True if complete else False if ring_known and (latency["overwritten"] > 0 or latency["stage_omitted"] > 0) else None,
             "all_session_admissions_proven": None}
    decode_ring_known = all(decode[k] is not None for k in ("accepted_observations", "retained", "overwritten", "stage_omitted")) and decode["values"] is not None
    decode_times_valid = (decode["accepted_observations"] == 0 and decode["first_observation_us"] == 0 and decode["last_observation_us"] == 0) or (
        decode["accepted_observations"] is not None and decode["accepted_observations"] > 0 and
        time_valid and decode["first_observation_us"] is not None and decode["last_observation_us"] is not None and
        boundary <= decode["first_observation_us"] <= decode["last_observation_us"] <= ended)
    decode_count = decode["accepted_observations"] if schemas and recording and time_valid and decode_ring_known and decode_times_valid and decode["stage_omitted"] == 0 else None
    final_snapshot = schemas and recording and finished is True and ended_flag is True and time_valid and close_rc == 0
    native_counts = {key: f.get(case, key, "case." + key) for key in ("native_outputs", "RGB_conversions", "RGB_publications")}
    return {"analysis_schema": "X4PROBE_ANALYSIS2", "case_id": case_id, "source_id": source_id, "build_id": build_id,
            "thread_stacks": thread_stacks(case, metrics),
            "sample_hashes": hashes, "mode": mode, "requested_clip_fps": fps_num / fps_den if period is not None else None,
            "temporal_identity": temporal, "native_output_PTS_association": False,
            "warmup": {"observed_us": warmup if time_valid else None,
                       "boundary_reached": True if time_valid else False if schemas and boundary == case_boundary == 0 and warmup == 0 and created is not None and ended is not None and ended >= created else None,
                       "rule": "60 RGB publications plus verified layout starts recording, not stability proof", "stable_segment_proven": None},
            "measured_interval_us": span, "measured_is_not_automatically_stable": True, "admission_lateness": trend,
            "whole_measured_admission_assessment": whole,
            "whole_session_admission_assessment": "indeterminate: startup/warmup admission timeline not recorded",
            "observed_counters": observed, "missing_counter_coverage": missing,
            "native_total_outputs": native_counts["native_outputs"] if final_snapshot else None,
            "native_total_RGB_conversions": native_counts["RGB_conversions"] if final_snapshot else None,
            "native_total_RGB_publications": native_counts["RGB_publications"] if final_snapshot else None,
            "measured_Decode_calls_observed": decode_count,
            "Decode_count_scope": "observed measured trace pairs including drain; not total AUs or IDR costs",
            "recording_omissions": omissions, "dirty_resets": resets, "events_after_end": after_end,
            "sustained_performance_proven": None, "unknown_fields": f.issues,
            "limitations": ["Recovery describes only covered input admissions, never a stable complete video pipeline.",
                            "No output PTS: no automatic AU/output or Decode/IDR association.",
                            "Timestamp consistency is not an independent proof of sample/build identity in metrics; current metrics have no such IDs.",
                            "No PS4 performance or streaming support is inferred."]}


def read_json(path):
    if path.stat().st_size > 8 * 1024 * 1024:
        raise ValueError("diagnostic JSON exceeds bounded reader")
    def non_json_constant(value):
        raise ValueError("non-finite JSON number is invalid")
    return json.loads(path.read_text(encoding="utf-8"), parse_constant=non_json_constant)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--case", type=pathlib.Path, required=True)
    ap.add_argument("--metrics", type=pathlib.Path, required=True)
    ap.add_argument("--output", type=pathlib.Path)
    args = ap.parse_args()
    text = json.dumps(analyze(read_json(args.case), read_json(args.metrics)), indent=2, sort_keys=True, allow_nan=False) + "\n"
    if args.output:
        with args.output.open("x", encoding="utf-8") as output:
            output.write(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
