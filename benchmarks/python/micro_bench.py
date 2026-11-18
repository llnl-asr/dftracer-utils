#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Micro timings of the hot Python calls, standard library only.

    python benchmarks/python/micro_bench.py --rounds 5

Each round runs every case once, in a fresh process, so the cases alternate and
drift in machine load hits all of them alike. The report is the median over the
rounds, in milliseconds. ``--case NAME`` runs one case; ``--json`` prints the
medians as a JSON object. Run it from an environment that holds the extension
and pandas (``to_pandas``) and a C++ compiler (``run_op``).
"""

import argparse
import json
import statistics
import subprocess
import sys
import time

ROWS = 1_000_000
CALLS = 1_000_000


def _import_total_ms() -> float:
    out = subprocess.run(
        [sys.executable, "-X", "importtime", "-c", "import dftracer.utils"],
        capture_output=True,
        text=True,
        check=True,
    ).stderr
    for line in reversed(out.splitlines()):
        if line.rstrip().endswith("dftracer.utils"):
            return float(line.split("|")[1]) / 1000.0
    raise RuntimeError("no importtime line for dftracer.utils")


def _timed(fn) -> float:
    start = time.perf_counter()
    fn()
    return (time.perf_counter() - start) * 1000.0


def _case_series_abs() -> float:
    from dftracer.utils import Series

    s = Series.from_list([-1, 2, -3, 4])

    def run() -> None:
        for _ in range(CALLS):
            s.abs()

    return _timed(run)


def _case_df_getitem() -> float:
    from dftracer.utils import DataFrame

    df = DataFrame({"x": [1, 2, 3], "y": [4, 5, 6]})

    def run() -> None:
        for _ in range(CALLS):
            df["x"]

    return _timed(run)


def _case_compare() -> float:
    from dftracer.utils import Series

    a = Series.from_list(list(range(ROWS)))
    b = Series.from_list([v if v % 7 else -v for v in range(ROWS)])
    return _timed(lambda: a.compare(b))


def _case_equals() -> float:
    from dftracer.utils import DataFrame

    a = DataFrame({"x": list(range(ROWS)), "y": [float(v) for v in range(ROWS)]})
    b = DataFrame({"x": list(range(ROWS)), "y": [float(v) for v in range(ROWS)]})
    return _timed(lambda: (a.equals(b), a["x"].equals(b["x"])))


def _case_valid_index() -> float:
    from dftracer.utils import DataFrame, Series

    vals = [None if v < 10 or v > ROWS - 10 else v for v in range(ROWS)]
    s = Series.from_list(vals)
    df = DataFrame({"x": vals})
    return _timed(
        lambda: (
            s.first_valid_index(),
            s.last_valid_index(),
            df.first_valid_index(),
            df.last_valid_index(),
        )
    )


def _case_groups() -> float:
    from dftracer.utils import DataFrame

    df = DataFrame({"k": [v % 1000 for v in range(ROWS)], "v": list(range(ROWS))})
    gb = df.groupby("k")
    return _timed(lambda: gb.groups)


def _double_op():
    from dftracer.utils import jit

    @jit.op
    def double(x: jit.i64) -> jit.i64:
        return x * 2

    return double


def _case_run_op() -> float:
    from dftracer.utils import jit_op

    double = _double_op()
    jit_op.run_op(double, 1)

    def run() -> None:
        for _ in range(200):
            jit_op.run_op(double, 21)

    return _timed(run)


def _case_to_pandas() -> float:
    from dftracer.utils import DataFrame

    df = DataFrame({"x": list(range(ROWS)), "y": [float(v) for v in range(ROWS)]})
    return _timed(df.to_pandas)


CASES = {
    "series_abs_1M_calls": _case_series_abs,
    "df_getitem_1M_calls": _case_df_getitem,
    "series_compare_1M_rows": _case_compare,
    "equals_1M_rows": _case_equals,
    "valid_index_1M_rows": _case_valid_index,
    "groupby_groups_1M_rows": _case_groups,
    "jit_run_op_200_warm_calls": _case_run_op,
    "to_pandas_1M_rows": _case_to_pandas,
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--case", choices=["import", *CASES])
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    if args.case:
        fn = _import_total_ms if args.case == "import" else CASES[args.case]
        print(fn())
        return

    names = ["import", *CASES]
    samples = {n: [] for n in names}
    for _ in range(args.rounds):
        for n in names:
            out = subprocess.run(
                [sys.executable, __file__, "--case", n],
                capture_output=True,
                text=True,
                check=True,
            ).stdout
            samples[n].append(float(out.strip().splitlines()[-1]))
    med = {n: statistics.median(v) for n, v in samples.items()}
    if args.json:
        print(json.dumps(med))
        return
    for n, v in med.items():
        print(f"{n:32s} {v:12.2f} ms")


if __name__ == "__main__":
    main()
