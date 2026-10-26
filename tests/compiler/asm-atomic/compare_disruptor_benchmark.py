#!/usr/bin/env python3
"""Paired, observational comparison of the SAME real Disruptor MPMC algorithm.

Each executable runs 4 real producers + 4 real workers and refuses to print
BENCH on any missing/duplicate/corrupted delivery. Benchmark data does NOT
establish ABI, C11 memory-model or TSan qualification for external ASM.
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
    r"^BENCH backend=(native|asm) messages=(\d+) elapsed_ns=(\d+) "
    r"throughput_msg_s=([0-9.]+) mean_wall_ns_per_msg=([0-9.]+)$"
)


def one_sample(executable: Path, backend: str, timeout: float) -> dict:
    result = subprocess.run(
        [str(executable.resolve()), backend],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=True,
    )
    matches = [PATTERN.fullmatch(line.strip())
               for line in result.stdout.splitlines()]
    matches = [match for match in matches if match is not None]
    if len(matches) != 1:
        raise RuntimeError(
            f"{backend} did not emit exactly one valid BENCH verdict: "
            f"stdout={result.stdout!r}, stderr={result.stderr!r}"
        )
    backend_seen, count, elapsed, throughput, mean_ns = matches[0].groups()
    if backend_seen != backend:
        raise RuntimeError(f"Backend label mismatch: {backend_seen} != {backend}")
    count = int(count)
    elapsed = int(elapsed)
    throughput = float(throughput)
    mean_ns = float(mean_ns)
    if count <= 0 or elapsed <= 0 or throughput <= 0 or mean_ns <= 0:
        raise RuntimeError(f"Invalid measurement from {backend}: {matches[0].group(0)}")
    return {
        "messages": count,
        "elapsed_ns": elapsed,
        "throughput_msg_s": throughput,
        "mean_wall_ns_per_msg": mean_ns,
    }


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
    for r in range(args.warmups + args.samples):
        # Alternate order to reduce (but not eliminate) host/thermal skew.
        order = ("native", "asm") if r % 2 == 0 else ("asm", "native")
        for backend in order:
            measurement = one_sample(commands[backend], backend, args.timeout)
            if r >= args.warmups:
                trials[backend].append(measurement)

    counts = {v["messages"] for row in trials.values() for v in row}
    if len(counts) != 1:
        raise RuntimeError(f"Workload mismatch across backends: {sorted(counts)}")

    native_median = statistics.median(
        x["throughput_msg_s"] for x in trials["native"]
    )
    asm_median = statistics.median(
        x["throughput_msg_s"] for x in trials["asm"]
    )
    result = {
        "description": (
            "Observational, no performance threshold. Native C11 keeps "
            "original relaxed/acquire/release ordering; ASM policy is in "
            "asm_policy. In loadstore_ordered mode, ASM matches load/store "
            "orders while retaining stronger seq_cst CAS and RMW; "
            "external ASM is not fully TSan-visible."
        ),
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "architecture": platform.machine(),
        "system": platform.platform(),
        "compiler": os.environ.get("CC", "unknown"),
        "asm_policy": args.asm_policy,
        "workload": {"producers": 4, "workers": 4, "messages": counts.pop()},
        "warmups": args.warmups,
        "samples": args.samples,
        "raw": trials,
        "median_throughput_msg_s": {"native": native_median, "asm": asm_median},
        "asm_to_native_median_throughput_ratio": asm_median / native_median,
        "native_to_asm_median_throughput_ratio": native_median / asm_median,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Architecture: {result['architecture']}, messages/round: {result['workload']['messages']}")
    print(f"Native C11 median: {native_median:,.0f} messages/s")
    print(f"ASM {args.asm_policy} median: {asm_median:,.0f} messages/s")
    print(f"ASM/native ratio: {asm_median / native_median:.3f} (observational, not release gate)")
    print(f"Evidence JSON: {args.output}")


if __name__ == "__main__":
    main()
