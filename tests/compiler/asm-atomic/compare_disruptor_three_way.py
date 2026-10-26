#!/usr/bin/env python3
"""Single-runner, balanced three-way comparison of the SAME Disruptor source.

Versions: native C11; external ASM seq_cst; external ASM with order-specialized
loads/stores (still seq_cst RMW/CAS). Each sample contains all three versions.
Six permutations balance position; throughput/latency phase order alternates.

This is observational CI evidence, NOT a benchmark pass/fail threshold or
a proof of a foreign ASM backend's C11/TSan correctness.
"""
import argparse
import datetime
import itertools
import json
import os
import platform
import re
import statistics
import subprocess
from pathlib import Path

BACKENDS = ("native", "seq_cst", "loadstore_ordered")
ORDERS = tuple(itertools.permutations(BACKENDS))
BENCH = re.compile(
    r"^BENCH backend=(native|asm) messages=(\d+) elapsed_ns=(\d+) "
    r"throughput_msg_s=([0-9.]+) mean_wall_ns_per_msg=([0-9.]+)$"
)
LATENCY = re.compile(
    r"^LATENCY backend=(native|asm) messages=(\d+) elapsed_ns=(\d+) "
    r"p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+) max_ns=(\d+)$"
)
QUANTILES = ("p50_ns", "p95_ns", "p99_ns", "max_ns")


def command_result(path, backend, phase, timeout):
    """Fail closed for a missing/erroring binary, malformed text, or mismatch."""
    label = "native" if backend == "native" else "asm"
    try:
        completed = subprocess.run(
            [str(path.resolve()), label], capture_output=True, text=True,
            check=True, timeout=timeout,
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"{backend}/{phase} returned {exc.returncode}:\n"
            f"stdout={exc.stdout!r}\nstderr={exc.stderr!r}"
        ) from exc
    regex = BENCH if phase == "throughput" else LATENCY
    matches = [
        match for line in completed.stdout.splitlines()
        if (match := regex.fullmatch(line.strip())) is not None
    ]
    if len(matches) != 1:
        raise RuntimeError(
            f"{backend}/{phase} expected exactly one verified verdict: "
            f"stdout={completed.stdout!r}, stderr={completed.stderr!r}"
        )
    parsed = matches[0].groups()
    if parsed[0] != label:
        raise RuntimeError(f"Invalid backend label: {parsed[0]} != {label}")
    count, elapsed = int(parsed[1]), int(parsed[2])
    if count <= 0 or elapsed <= 0:
        raise RuntimeError(f"Invalid run count/duration: {parsed}")
    result = {"messages": count, "elapsed_ns": elapsed}
    if phase == "throughput":
        throughput = float(parsed[3])
        amortized = float(parsed[4])
        if not (throughput > 0 and amortized > 0):
            raise RuntimeError(f"Invalid throughput: {parsed}")
        result.update(
            throughput_msg_s=throughput, mean_wall_ns_per_msg=amortized
        )
    else:
        values = [int(v) for v in parsed[3:]]
        if values != sorted(values) or values[0] < 0:
            raise RuntimeError(f"Non-monotonic/negative quantiles: {values}")
        result.update(dict(zip(QUANTILES, values)))
    return result


def percentile(values, numerator):
    """Descriptive interpolated percentile, NOT a confidence interval."""
    seq = sorted(values)
    if len(seq) == 1:
        return seq[0]
    p = (len(seq) - 1) * numerator / 100
    low = int(p)
    high = min(len(seq) - 1, low + 1)
    return seq[low] + (seq[high] - seq[low]) * (p - low)


def ratio_summary(rounds, field):
    """Same-round ratios: avoids comparing two separated batch medians."""
    pairs = (
        ("seq_cst", "native"),
        ("loadstore_ordered", "native"),
        ("loadstore_ordered", "seq_cst"),
    )
    results = {}
    for numerator, denominator in pairs:
        values = []
        for sample in rounds:
            a = sample["measurements"][numerator][field]
            b = sample["measurements"][denominator][field]
            if b == 0:
                raise RuntimeError(f"Zero denominator in {field}: {sample}")
            values.append(a / b)
        results[f"{numerator}_over_{denominator}"] = {
            "median_ratio": statistics.median(values),
            "p10_ratio": percentile(values, 10),
            "p90_ratio": percentile(values, 90),
            "raw_paired_ratios": values,
            "ratios_greater_than_one": sum(v > 1 for v in values),
        }
    return results


def metric_summary(rounds, field):
    by_backend = {}
    for backend in BACKENDS:
        samples = [
            sample["measurements"][backend][field] for sample in rounds
        ]
        midpoint = statistics.median(samples)
        by_backend[backend] = {
            "median": midpoint,
            "minimum": min(samples),
            "maximum": max(samples),
            "median_absolute_deviation": statistics.median(
                abs(x - midpoint) for x in samples
            ),
        }
    return by_backend


def balanced_order(round_index, phase):
    # Six permutations, each once per 6 rounds. Use an independent rotation
    # for the latency sequence to limit consistent phase-position coupling.
    shift = 0 if phase == "throughput" else 3
    return ORDERS[(round_index + shift) % len(ORDERS)]


def self_test():
    for phase in ("throughput", "latency"):
        for start in (0, 6):
            rotated = [
                balanced_order(i, phase) for i in range(start, start + 6)
            ]
            assert len(set(rotated)) == 6, rotated
            for offset in range(3):
                assert sorted(order[offset] for order in rotated) == sorted(
                    BACKENDS
                )
    dummy = [
        {"measurements": {
            "native": {"throughput_msg_s": 100},
            "seq_cst": {"throughput_msg_s": 80},
            "loadstore_ordered": {"throughput_msg_s": 90},
        }}
        for _ in range(6)
    ]
    ratios = ratio_summary(dummy, "throughput_msg_s")
    assert abs(
        ratios["seq_cst_over_native"]["median_ratio"] - 0.8
    ) < 1e-9
    assert abs(
        ratios["loadstore_ordered_over_seq_cst"]["median_ratio"] - 1.125
    ) < 1e-9
    assert percentile((1, 2, 3, 4), 50) == 2.5
    print("Balanced Latin-order three-way driver self-test: PASS")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--self-test", action="store_true")
    for phase in ("throughput", "latency"):
        for backend in BACKENDS:
            p.add_argument(f"--{phase}-{backend.replace('_', '-')}",
                           type=Path)
    p.add_argument("--output", type=Path)
    p.add_argument("--warmups", type=int, default=6)
    p.add_argument("--samples", type=int, default=12)
    p.add_argument("--expected-throughput", type=int, default=80000)
    p.add_argument("--expected-latency", type=int, default=16000)
    p.add_argument("--timeout", type=float, default=50.0)
    args = p.parse_args()

    if args.self_test:
        self_test()
        return
    if args.samples < 6 or args.samples > 60 or args.samples % 6 != 0:
        p.error("--samples must be a positive multiple of six in [6, 60]")
    if args.warmups < 6 or args.warmups > 24 or args.warmups % 6 != 0:
        p.error("--warmups must be a positive multiple of six in [6, 24]")
    if args.timeout <= 0 or args.expected_throughput <= 0 or args.expected_latency <= 0:
        p.error("timeout and expected message counts must be positive")
    if args.output is None:
        p.error("--output is required")

    commands = {}
    for phase in ("throughput", "latency"):
        commands[phase] = {}
        for backend in BACKENDS:
            path = getattr(args, f"{phase}_{backend}")
            if path is None or not path.is_file():
                p.error(f"Missing {phase}/{backend} executable: {path}")
            commands[phase][backend] = path

    raw = {"throughput": [], "latency": []}
    warmup_counts = {"throughput": 0, "latency": 0}
    for run_index in range(args.warmups + args.samples):
        # Alternate phase precedence; both phases still use each permutation
        # equally for the 12 measured rounds.
        phases = ("throughput", "latency") if run_index % 2 == 0 else (
            "latency", "throughput"
        )
        for phase in phases:
            order = balanced_order(run_index, phase)
            measures = {}
            count_expected = (
                args.expected_throughput if phase == "throughput"
                else args.expected_latency
            )
            for backend in order:
                measure = command_result(
                    commands[phase][backend], backend, phase, args.timeout
                )
                if measure["messages"] != count_expected:
                    raise RuntimeError(
                        f"{phase}/{backend}: expected {count_expected} "
                        f"unique verified records, got {measure['messages']}"
                    )
                measures[backend] = measure
            if run_index >= args.warmups:
                raw[phase].append({
                    "round": run_index - args.warmups,
                    "order": list(order),
                    "measurements": measures,
                })
            else:
                warmup_counts[phase] += 1

    summary = {
        "throughput_msg_s": {
            "by_backend": metric_summary(raw["throughput"], "throughput_msg_s"),
            "paired_ratios": ratio_summary(raw["throughput"], "throughput_msg_s"),
        },
        "latency_ns": {
            metric: {
                "by_backend": metric_summary(raw["latency"], metric),
                "paired_ratios": ratio_summary(raw["latency"], metric),
            }
            for metric in QUANTILES
        },
    }
    evidence = {
        "experiment": "Disruptor three-way balanced interleaved observational comparison",
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": os.environ.get("GITHUB_SHA"),
        "compiler": os.environ.get("CC", "unknown"),
        "architecture": platform.machine(),
        "system": platform.platform(),
        "available_cpus": os.cpu_count(),
        "algorithms": {
            "native": "original Disruptor C11 atomics",
            "seq_cst": "external GAS, all operations seq_cst",
            "loadstore_ordered": "external GAS, exact load/store order; seq_cst RMW/CAS",
        },
        "interpretation": (
            "Each ratio is formed within a contemporaneous 3-way round. "
            "Run order is balanced across six permutations. Figures are "
            "descriptive, not statistical confidence intervals or release "
            "thresholds. C11 source sanitizers cannot observe all foreign "
            "ASM atomic memory accesses. Publish-to-claim age includes "
            "scheduling and timestamp overhead; not per-call latency."
        ),
        "topology": {
            "publishers": 4, "workers": 4, "capacity": 64,
            "throughput_messages_per_run": args.expected_throughput,
            "timestamped_messages_per_run": args.expected_latency,
        },
        "warmup_triplets": warmup_counts,
        "measured_triplets": args.samples,
        "orders": list(map(list, ORDERS)),
        "results": summary,
        "raw": raw,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")

    print(f"Three-way balanced comparison on {evidence['architecture']} / {evidence['compiler']}")
    print(f"{args.warmups} warmup + {args.samples} measured triplets, all orders balanced")
    for backend in BACKENDS:
        throughput = summary["throughput_msg_s"]["by_backend"][backend]["median"]
        p99 = summary["latency_ns"]["p99_ns"]["by_backend"][backend]["median"]
        print(f"{backend}: median throughput {throughput:,.0f} msg/s; "
              f"median-of-run p99 age {p99:,.0f} ns")
    for numerator in ("seq_cst", "loadstore_ordered"):
        throughput = summary["throughput_msg_s"]["paired_ratios"][
            f"{numerator}_over_native"
        ]["median_ratio"]
        p99 = summary["latency_ns"]["p99_ns"]["paired_ratios"][
            f"{numerator}_over_native"
        ]["median_ratio"]
        print(f"{numerator}/native paired median: throughput {throughput:.3f}x, "
              f"p99 age {p99:.3f}x")
    ordered_ratio = summary["throughput_msg_s"]["paired_ratios"][
        "loadstore_ordered_over_seq_cst"
    ]["median_ratio"]
    print(f"ordered/seq_cst paired throughput median {ordered_ratio:.3f}x")
    print(f"Unthresholded raw evidence: {args.output}")


if __name__ == "__main__":
    main()
