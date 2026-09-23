#!/usr/bin/env python3
"""Validate exclusive per-thread accounting; never sum threads into frame time."""
import argparse
import json
from pathlib import Path


def check(text):
    windows = {}
    latest = {}
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith(("telemetry_report_overflow", "telemetry_registry_full")):
            raise ValueError(f"line {number}: lost telemetry records")
        if not line.startswith(("telemetry_window ", "telemetry_phase ", "telemetry_sample ", "telemetry_end ")):
            continue
        try:
            event, *tokens = line.split()
            fields = dict(token.split("=", 1) for token in tokens)
            if len(fields) != len(tokens):
                raise ValueError("duplicate field")
            key = (int(fields["thread"]), int(fields["seq"]))
            if event == "telemetry_window":
                if key in windows:
                    raise ValueError("duplicate window")
                begin, end = int(fields["begin_us"]), int(fields["end_us"])
                wall, accounted = int(fields["wall_us"]), int(fields["accounted_us"])
                if begin < 0 or end < begin or wall != end - begin or accounted != wall:
                    raise ValueError("window clock/accounting mismatch")
                if int(fields["errors"]) != 0:
                    raise ValueError("runtime accounting errors")
                prior = latest.get(key[0])
                if not prior and key[1] != 1:
                    raise ValueError("missing first owner window")
                if prior and (key[1] != prior[0] + 1 or begin != prior[1]):
                    raise ValueError("missing or discontinuous owner window")
                latest[key[0]] = (key[1], end)
                if "cpu_valid" in fields:
                    valid = int(fields["cpu_valid"])
                    run, interval = int(fields["cpu_run_us"]), int(fields["cpu_window_us"])
                    if valid not in (0, 1) or min(run, interval) < 0 or (valid and interval == 0):
                        raise ValueError("invalid CPU sample")
                windows[key] = {"header": fields, "phases": {}, "samples": {}, "closed": False}
            else:
                window = windows[key]
                if window["closed"]:
                    raise ValueError("event after window end")
                if event == "telemetry_end":
                    total = sum(p["exclusive_wall_us"] for p in window["phases"].values())
                    if total != int(window["header"]["wall_us"]):
                        raise ValueError("phase partition does not equal owner wall time")
                    window["closed"] = True
                elif event == "telemetry_sample":
                    name = fields["name"]
                    if name in window["samples"]:
                        raise ValueError("duplicate sample phase")
                    values = {k: int(fields[k]) for k in ("entries", "roots", "samples",
                        "inclusive_wall_us", "max_wall_us", "errors", "open_depth")}
                    if (min(values.values()) < 0 or values["errors"] or
                        values["roots"] > values["entries"] or
                        values["max_wall_us"] > values["inclusive_wall_us"] or
                        (not values["samples"] and values["inclusive_wall_us"]) or
                        fields["additive"] != "0" or
                        fields["probability_denominator"] != "1024" or
                        fields["attribution"] != "completion_window"):
                        raise ValueError("invalid sample accounting")
                    window["samples"][name] = values
                else:
                    name = fields["name"]
                    if name in window["phases"]:
                        raise ValueError("duplicate phase")
                    duration, entries = int(fields["exclusive_wall_us"]), int(fields["entries"])
                    if min(duration, entries) < 0:
                        raise ValueError("negative measurement")
                    window["phases"][name] = {"exclusive_wall_us": duration,
                        "entries": entries, "kind": fields["kind"]}
        except (KeyError, ValueError) as exc:
            raise ValueError(f"line {number}: {exc}") from exc
    if not windows:
        raise ValueError("no telemetry windows")
    if any(not w["closed"] for w in windows.values()):
        raise ValueError("truncated telemetry window")
    return {"valid_accounting": True, "threads_overlap": True,
            "coverage_complete": all(w["header"].get("coverage") == "complete" for w in windows.values()),
            "windows": list(windows.values())}


def summarize(result):
    """Rank wall scopes per owner; CPU utilization comes only from kernel samples."""
    owners = {}
    for window in result["windows"]:
        h = window["header"]
        owner = owners.setdefault(h["thread"], {"thread": h["thread"],
            "role": h.get("role", "unknown"), "mode": h.get("mode", "unknown"), "wall_us": 0,
            "cpu_run_us": 0, "cpu_window_us": 0, "phases": {}, "samples": {}})
        owner["wall_us"] += int(h["wall_us"])
        if h.get("cpu_valid") == "1":
            owner["cpu_run_us"] += int(h["cpu_run_us"])
            owner["cpu_window_us"] += int(h["cpu_window_us"])
        for name, phase in window["phases"].items():
            bucket = owner["phases"].setdefault(name, {"name": name,
                "kind": phase["kind"], "exclusive_wall_us": 0, "entries": 0})
            bucket["exclusive_wall_us"] += phase["exclusive_wall_us"]
            bucket["entries"] += phase["entries"]
        for name, sample in window["samples"].items():
            bucket = owner["samples"].setdefault(name, {"name": name, "entries": 0,
                "roots": 0, "samples": 0, "inclusive_wall_us": 0, "max_wall_us": 0})
            for key in ("entries", "roots", "samples", "inclusive_wall_us"):
                bucket[key] += sample[key]
            bucket["max_wall_us"] = max(bucket["max_wall_us"], sample["max_wall_us"])
    for owner in owners.values():
        owner["cpu_percent_of_one_core"] = (100 * owner["cpu_run_us"] / owner["cpu_window_us"]
            if owner["cpu_window_us"] else None)
        for phase in owner["phases"].values():
            phase["owner_wall_percent"] = (100 * phase["exclusive_wall_us"] / owner["wall_us"]
                if owner["wall_us"] else 0)
        owner["phases"] = sorted(owner["phases"].values(),
            key=lambda phase: phase["exclusive_wall_us"], reverse=True)
        owner["samples"] = list(owner["samples"].values())
    return {"coverage_complete": result["coverage_complete"],
        "warning": "Wall phases include preemption. Owners overlap; do not sum them into frame time. GPU execution is not measured.",
        "overview_policy": "Hot inner scopes are folded into parents, not measured separately; use detailed mode for their breakdown.",
        "sampled_policy": "Raw inclusive outermost-call samples overlap each other and coarse scopes. No extrapolated total or CPU percentage. Durations belong to completion windows; counts to start windows. Missing samples do not mean zero cost or inactive hardware.",
        "owners": list(owners.values())}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--summary", action="store_true", help="Rank phase costs separately for each thread")
    args = parser.parse_args()
    try:
        result = check(args.log.read_text(encoding="utf-8"))
        print(json.dumps(summarize(result) if args.summary else result, indent=2))
    except (ValueError, OSError) as exc:
        parser.exit(1, f"Invalid telemetry: {exc}\n")
