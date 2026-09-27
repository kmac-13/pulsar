#!/usr/bin/env python3
"""
aggregate_repeated_runs.py -- run a benchmark executable N times as
separate PROCESSES and synthesize a Google-Benchmark-compatible aggregate
JSON from the results.

Why this exists
----------------
Some executables fail specifically under --benchmark_repetitions=N, which
invokes the benchmark body N times WITHIN ONE PROCESS, while running fine
when invoked N times as N separate process launches instead. Confirmed
case: bench_vdk_ts_contention.exe, which run_benchmarks.sh's normal
invocation (`--benchmark_repetitions=10 --benchmark_report_aggregates_only`)
causes to exit non-zero, while `bench_vdk_ts_contention.exe
--benchmark_format=json --benchmark_out=...` run 5 times in a row as
separate process launches completes cleanly every time. This is a
different, distinct manifestation from vdk's already-documented
thread_local destructor issue (see run_benchmarks.sh's comment on that) --
this one is specific to repeated in-process invocation of the contention
benchmark's setup/teardown, not thread creation/destruction as such -- but
the fix has the same shape: give it its own process per run instead of
relying on Google Benchmark's own in-process repetition.

This script generalizes that workaround so it isn't hand-rolled again if
another executable needs the same treatment later.

What it produces
-----------------
A JSON file in the same schema Google Benchmark's own --benchmark_repetitions
produces: run_type "aggregate", aggregate_name mean/median/stddev/cv, one
row per statistic per benchmark. compare.py needs no special-casing to
consume it -- it's indistinguishable from a normal repeated run's output.

Aggregates are computed from BOTH real_time and cpu_time (mirroring what
Google Benchmark itself would produce), even though compare.py itself now
reads real_time only -- see compare.py's load_results() docstring for why.
items_per_second, when present, is aggregated the same way.

Usage
-----
    python aggregate_repeated_runs.py <executable> <output.json> [N] [-- extra-args...]

    N defaults to 5. Anything after a literal "--" is passed through to
    each invocation of <executable> unchanged (e.g. --benchmark_filter=...).

Exit status: 0 if at least one of the N runs succeeded (a warning is
printed for any that didn't, and the aggregate reflects only the
successful ones); 1 if all N runs failed, in which case no output file is
written -- matching run_benchmarks.sh's own "missing means failed, never
silently stale" convention for its normal invocation path.
"""

import json
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path


def run_once(exe, extra_args, tmp_json):
    cmd = [exe, "--benchmark_format=json", f"--benchmark_out={tmp_json}"] + extra_args
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result.returncode


def aggregate_stat(values):
    mean = statistics.mean(values)
    median = statistics.median(values)
    stddev = statistics.pstdev(values) if len(values) > 1 else 0.0
    cv = (stddev / mean) if mean else 0.0
    return mean, median, stddev, cv


def build_aggregate_rows(name, family_index, entries):
    real_times = [e["real_time"] for e in entries if "real_time" in e]
    cpu_times = [e["cpu_time"] for e in entries if "cpu_time" in e]
    items = [e["items_per_second"] for e in entries if "items_per_second" in e]

    if not real_times:
        return []

    sample = entries[0]
    time_unit = sample.get("time_unit", "ns")
    threads = sample.get("threads", 1)
    n = len(real_times)

    real_mean, real_median, real_stddev, real_cv = aggregate_stat(real_times)
    cpu_mean, cpu_median, cpu_stddev, cpu_cv = (
        aggregate_stat(cpu_times) if cpu_times else (0.0, 0.0, 0.0, 0.0)
    )
    items_mean = items_median = items_stddev = items_cv = None
    if items:
        items_mean, items_median, items_stddev, items_cv = aggregate_stat(items)

    def row(agg_name, real_val, cpu_val, items_val):
        r = {
            "name": f"{name}_{agg_name}",
            "family_index": family_index,
            "per_family_instance_index": 0,
            "run_name": name,
            "run_type": "aggregate",
            "repetitions": n,
            "threads": threads,
            "aggregate_name": agg_name,
            "aggregate_unit": "percentage" if agg_name == "cv" else "time",
            "iterations": n,
            "real_time": real_val,
            "cpu_time": cpu_val,
            "time_unit": time_unit,
        }
        if items_val is not None:
            r["items_per_second"] = items_val
        return r

    rows = [
        row("mean", real_mean, cpu_mean, items_mean),
        row("median", real_median, cpu_median, items_median),
        row("stddev", real_stddev, cpu_stddev, items_stddev),
        row("cv", real_cv, cpu_cv, items_cv),
    ]
    return rows


def main():
    if len(sys.argv) < 3:
        print(
            "Usage: aggregate_repeated_runs.py <executable> <output.json> "
            "[N] [-- extra-args...]",
            file=sys.stderr,
        )
        sys.exit(1)

    exe = sys.argv[1]
    out_path = Path(sys.argv[2])
    rest = sys.argv[3:]

    n = 5
    if rest and rest[0].isdigit():
        n = int(rest[0])
        rest = rest[1:]

    extra_args = []
    if rest and rest[0] == "--":
        extra_args = rest[1:]

    runs = []
    context = None
    failures = 0

    with tempfile.TemporaryDirectory() as tmpdir:
        for i in range(n):
            tmp_json = Path(tmpdir) / f"run_{i}.json"
            status = run_once(exe, extra_args, tmp_json)

            if status != 0:
                print(
                    f"# Warning: run {i + 1}/{n} of {exe} exited with status "
                    f"{status}, skipping this run",
                    file=sys.stderr,
                )
                failures += 1
                continue

            if not tmp_json.exists():
                print(
                    f"# Warning: run {i + 1}/{n} of {exe} produced no output "
                    "file, skipping",
                    file=sys.stderr,
                )
                failures += 1
                continue

            try:
                with open(tmp_json, encoding="utf-8") as f:
                    data = json.load(f)
            except json.JSONDecodeError as e:
                print(
                    f"# Warning: run {i + 1}/{n} of {exe} produced invalid "
                    f"JSON ({e}), skipping this run",
                    file=sys.stderr,
                )
                failures += 1
                continue

            if context is None:
                context = data.get("context", {})
            runs.append(data.get("benchmarks", []))

    if not runs:
        print(
            f"# Error: all {n} runs of {exe} failed; not writing {out_path}",
            file=sys.stderr,
        )
        sys.exit(1)

    if failures:
        print(
            f"# Warning: only {len(runs)}/{n} runs of {exe} succeeded; "
            f"aggregate is based on {len(runs)} sample(s), not {n}",
            file=sys.stderr,
        )

    by_name = {}
    for run in runs:
        for b in run:
            name = b.get("name")
            if name is None:
                continue
            by_name.setdefault(name, []).append(b)

    aggregate_benchmarks = []
    for family_index, name in enumerate(sorted(by_name.keys())):
        aggregate_benchmarks.extend(
            build_aggregate_rows(name, family_index, by_name[name])
        )

    output = {"context": context or {}, "benchmarks": aggregate_benchmarks}

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(output, f, indent=2)
        f.write("\n")

    print(
        f"Wrote {out_path} ({len(runs)}/{n} successful runs, "
        f"{len(by_name)} benchmark(s))",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
