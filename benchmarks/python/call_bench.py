#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Per-call cost of the Python binding layer on tiny inputs.

Runs `s.abs()` on a 4-row Series, `df["x"]` and `df.head()` on a 1-column
DataFrame, `lf.collect()` on a plan over it and `tv.columns` on a TraceViewer
over one tiny trace file, and prints the median of 5 runs in nanoseconds per
call. The calls that do real work (`lf.collect()`) run 20k times.

    python benchmarks/python/call_bench.py
"""

from __future__ import annotations

import gzip
import json
import os
import statistics
import tempfile
import time

import dftracer.utils as dft

CALLS = 1_000_000
RUNS = 5


def per_call_ns(fn, calls: int = CALLS) -> float:
    runs = []
    for _ in range(RUNS):
        t0 = time.perf_counter_ns()
        for _ in range(calls):
            fn()
        runs.append((time.perf_counter_ns() - t0) / calls)
    return statistics.median(runs)


def tiny_trace(directory: str) -> str:
    path = os.path.join(directory, "t.pfw.gz")
    with gzip.open(path, "wt") as f:
        for i in range(8):
            event = {"ph": "X", "name": "a", "cat": "C", "pid": 1, "tid": 1, "ts": i, "dur": 2}
            f.write(json.dumps(event) + "\n")
    return path


def main() -> None:
    s = dft.Series([1, -2, 3, -4])
    df = dft.DataFrame({"x": [1, 2, 3, 4]})
    lf = df.lazy()
    empty = per_call_ns(lambda: None)
    print(f"empty lambda      {empty:8.1f} ns/call")
    print(f"s.abs()           {per_call_ns(s.abs) - empty:8.1f} ns/call (net)")
    print(f'df["x"]           {per_call_ns(lambda: df["x"]) - empty:8.1f} ns/call (net)')
    print(f"df.head()         {per_call_ns(df.head) - empty:8.1f} ns/call (net)")
    print(f"lf.collect()      {per_call_ns(lf.collect, 20_000) - empty:8.1f} ns/call (net)")
    with tempfile.TemporaryDirectory() as directory:
        tv = dft.TraceViewer(tiny_trace(directory))
        tv.columns
        print(
            f"tv.columns        {per_call_ns(lambda: tv.columns, 20_000) - empty:8.1f} ns/call (net)"
        )


if __name__ == "__main__":
    main()
