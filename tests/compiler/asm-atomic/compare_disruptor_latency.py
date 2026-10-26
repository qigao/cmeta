#!/usr/bin/env python3
"""Observe real Disruptor publish-to-claim handoff age, never assert speedup.

Uses the *same* 4-publisher/4-worker C harness and production source in both
binaries, but timestamped payloads. This mode is distinct from throughput:
clock_gettime for each event is intentionally included in both backends.
"""
import argparse
import datetime
import json
import os
import platform
import re
import statistics
import subprocess
from pathlib import Path

PATTERN = re.compile(
    r"^LATENCY backend=(native|asm) messages=(\d+) elapsed_ns=(\d+) "
    r"p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+) max_ns=(\d+)$"
)
FIELDS = ("p50_ns", "p95_ns", "p99_ns", "max_ns")


def run_one(executable: Path, backend: str, timeout: float) -> dict:
    completed = subprocess.run(
        [str(executable.resolve()), backend],
        capture_output=True, text=True, timeout=timeout, check=True,
    )
    matched = [
        m for line in completed.stdout.splitlines()
        if (m := PATTERN.fullmatch(line.strip())) is not None
    ]
    if len(matched) != 1:
        raise RuntimeError(
            f"{backend}: expected exactly one verified latency result: "
            f"stdout={completed.stdout!r} stderr={completed.stderr!r}"
        )
    label, count, elapsed, p50, p95, p99, max_ns = matched[0].groups()
    if label != backend:
        raise RuntimeError(f"Backend mismatch: {label} != {backend}")
    values = [int(p50), int(p95), int(p99), int(max_ns)]
    count = int(count)
    elapsed = int(elapsed)
    if count <= 0 or elapsed <= 0 or any(v < 0 for v in values):
        raise RuntimeError("Invalid latency measurement")
    if values != sorted(values):
        raise RuntimeError(f"Invalid non-monotonic percentiles: {values}")
    return {"messages": count, "elapsed_ns": elapsed, **dict(zip(FIELDS, values))}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--asm", type=Path, required=True)
    parser.add_argument("--asm-policy", choices=("seq_cst", "loadstore_ordered"), default="seq_cst")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=50)
    args = parser.parse_args()
    if not 3 <= args.samples <= 40 or not 1 <= args.warmups <= 20:
        parser.error("Require 3..40 samples and 1..20 warmups")

    commands = {"native": args.native, "asm": args.asm}
    trials = {"native": [], "asm": []}
    for run_index in range(args.warmups + args.samples):
        order = ("native", "asm") if run_index % 2 == 0 else ("asm", "native")
        for name in order:
            observed = run_one(commands[name], name, args.timeout)
            if run_index >= args.warmups:
                trials[name].append(observed)

    counts = {t["messages"] for records in trials.values() for t in records}
    if len(counts) != 1:
        raise RuntimeError(f"Different workloads between backends: {sorted(counts)}")

    medians = {
        name: {
            field: statistics.median(t[field] for t in records)
            for field in FIELDS
        }
        for name, records in trials.items()
    }
    ratios = {
        field: (medians["asm"][field] / medians["native"][field]
                if medians["native"][field] != 0 else None)
        for field in FIELDS
    }
    result = {
        "description": (
            "Publisher timestamp immediately BEFORE publish, worker timestamp "
            "immediately AFTER successful claim; includes publish processing "
            "and scheduling, and timestamp overhead. Values are not individual "
            "API call latency or queue-only waiting time. The ASM policy is "
            "specified in asm_policy; ordered mode matches load/store orders "
            "but retains seq_cst CAS/RMW, and C11 uses original ordering."
        ),
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "architecture": platform.machine(),
        "system": platform.platform(),
        "compiler": os.environ.get("CC", "unknown"),
        "asm_policy": args.asm_policy,
        "workload": {"publishers": 4, "workers": 4, "messages": counts.pop()},
        "warmups": args.warmups,
        "samples": args.samples,
        "raw": trials,
        "median_of_run_percentiles_ns": medians,
        "asm_over_native_median_latency_ratio": ratios,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Architecture: {result['architecture']}, messages/round: {result['workload']['messages']}")
    for name in ("native", "asm"):
        summary = medians[name]
        print(f"{name} median-of-runs: p50={summary['p50_ns']:.0f} ns; "
              f"p95={summary['p95_ns']:.0f} ns; p99={summary['p99_ns']:.0f} ns; "
              f"max={summary['max_ns']:.0f} ns")
    print(f"ASM/native p99 age ratio: {ratios['p99_ns']:.3f} (observational)")
    print(f"Evidence JSON: {args.output}")


if __name__ == "__main__":
    main()
