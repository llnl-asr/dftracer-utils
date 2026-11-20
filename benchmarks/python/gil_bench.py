#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cost of releasing the GIL around a cheap Series kernel, by row count.

`without_gil` (src/dftracer/utils/python/core/call.h) releases the GIL when a
call covers at least `GIL_RELEASE_ROWS` rows. This prints the median time in
nanoseconds of `abs` and `sum` for 1 to 65536 rows (median of `--runs` runs of
20000 calls) on the extension that is installed.

Build the extension twice, with `DFTRACER_UTILS_PYTHON_GIL_RELEASE_ROWS` set to
9223372036854775807 (the release never on) and to 0 (always on), run this on
each build and alternate the builds between runs. The release is worth its cost
at the smallest row count where the "always" time is within about 5 percent of
the "never" time.
"""

from __future__ import annotations

import argparse
import statistics
import time

import numpy as np

import dftracer.utils as dft

ROWS = (1, 16, 256, 4096, 8192, 16384, 32768, 65536)
CALLS = 20_000


def per_call_ns(fn, runs_count: int) -> float:
    runs = []
    for _ in range(runs_count):
        t0 = time.perf_counter_ns()
        for _ in range(CALLS):
            fn()
        runs.append((time.perf_counter_ns() - t0) / CALLS)
    return statistics.median(runs)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runs", type=int, default=5)
    runs = parser.parse_args().runs
    print(f"{'rows':>8} {'abs ns':>10} {'sum ns':>10}")
    for rows in ROWS:
        s = dft.Series(np.arange(rows, dtype=np.int64))
        print(f"{rows:>8} {per_call_ns(s.abs, runs):>10.1f} {per_call_ns(s.sum, runs):>10.1f}")


if __name__ == "__main__":
    main()
