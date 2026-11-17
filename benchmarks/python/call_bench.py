#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Per-call cost of the Python binding layer on tiny inputs.

Runs `s.abs()` on a 4-row Series and `df["x"]` on a 1-column DataFrame, 1M
calls each, and prints the median of 5 runs in nanoseconds per call.

    python benchmarks/python/call_bench.py
"""

from __future__ import annotations

import statistics
import time

import dftracer.utils as dft

CALLS = 1_000_000
RUNS = 5


def per_call_ns(fn) -> float:
    runs = []
    for _ in range(RUNS):
        t0 = time.perf_counter_ns()
        for _ in range(CALLS):
            fn()
        runs.append((time.perf_counter_ns() - t0) / CALLS)
    return statistics.median(runs)


def main() -> None:
    s = dft.Series([1, -2, 3, -4])
    df = dft.DataFrame({"x": [1, 2, 3, 4]})
    empty = per_call_ns(lambda: None)
    print(f"empty lambda      {empty:8.1f} ns/call")
    print(f"s.abs()           {per_call_ns(s.abs) - empty:8.1f} ns/call (net)")
    print(f'df["x"]           {per_call_ns(lambda: df["x"]) - empty:8.1f} ns/call (net)')


if __name__ == "__main__":
    main()
