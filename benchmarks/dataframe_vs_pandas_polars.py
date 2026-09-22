#!/usr/bin/env python3
"""Wall-clock of the dataframe engine against pandas, polars and DuckDB on
the same data and the same ops. Two tables: a generic one (filter,
group-by, sort, join, rolling mean, a string predicate, a prefix scan, a
filter + group-by) and a trace-shaped one (ts, dur, pid,
tid, name, cat) with the analyses a trace tool runs: time bucketing, the
slowest calls, per-process and per-call statistics, a category filter with
a duration bound, a session-like gap detection.

Every engine is handed the same Arrow table, so the build cost is outside
the timing; each op runs `--repeat` times and the best is reported, with
the ratio to ours (a ratio above 1 means we are faster). `ours` is spelled
exactly as the pandas column (the same code runs on both); `lazy` is the
same op as a plan on our LazyFrame, collected. DuckDB runs one
SQL statement per op over the table registered as a view and materializes
the result to Arrow. After the timing each engine's result is checked
against ours: the same row count, and column by column the same sorted
values (floats within a tolerance), so a fast wrong answer shows as
`differs`, and how many cores each engine kept busy (its CPU time over
the wall time of one run). `--memory` instead runs every (op, engine) in
its own process and reports how far that process's resident set rose
above its level before the op, sampled continuously from the parent (an
op that holds the GIL cannot hide its peak from an outside sampler);
the tables reach the child as memory-mapped Arrow files and the
allocator's free pages are returned to the kernel before the op, so an
engine can hide little of its footprint in pages left behind by building
its frame (pandas, which copies Arrow into blocks, keeps some).
Run from a venv holding pandas, polars, duckdb and the extension:

    python benchmarks/dataframe_vs_pandas_polars.py --rows 10_000_000
    python benchmarks/dataframe_vs_pandas_polars.py --rows 10_000_000 --memory
"""

from __future__ import annotations

import argparse
import gc
import os
import resource
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, Callable, Dict, List, Optional, Sequence

import duckdb
import numpy as np
import pandas as pd
import polars as pl
import pyarrow as pa

from dftracer.utils import DataFrame, Series, col


def make_table(rows: int, groups: int, seed: int) -> pa.Table:
    rng = np.random.default_rng(seed)
    words = np.array([f"path/to/file_{i:04d}.dat" for i in range(1000)])
    return pa.table(
        {
            "k": rng.integers(0, groups, rows, dtype=np.int64),
            "v": rng.random(rows),
            "n": rng.integers(0, 1_000_000, rows, dtype=np.int64),
            "s": pa.array(words[rng.integers(0, len(words), rows)]),
        }
    )


def make_trace(rows: int, seed: int) -> pa.Table:
    """Events as a trace holds them: ts in microseconds, monotone with
    jitter; dur skewed; a few hundred call names; five categories; 64
    processes with 8 threads each."""
    rng = np.random.default_rng(seed + 2)
    ts = np.cumsum(rng.integers(1, 200, rows, dtype=np.int64))
    dur = (rng.pareto(2.0, rows) * 50 + 1).astype(np.int64)
    names = np.array([f"call_{i:03d}" for i in range(300)])
    cats = np.array(["POSIX", "STDIO", "MPI", "HDF5", "CUDA"])
    return pa.table(
        {
            "ts": ts,
            "dur": dur,
            "pid": rng.integers(0, 64, rows, dtype=np.int64),
            "tid": rng.integers(0, 8, rows, dtype=np.int64),
            "name": pa.array(names[rng.integers(0, len(names), rows)]),
            "cat": pa.array(cats[rng.integers(0, len(cats), rows)]),
        }
    )


def make_right(groups: int, seed: int) -> pa.Table:
    rng = np.random.default_rng(seed + 1)
    return pa.table(
        {
            "k": np.arange(groups, dtype=np.int64),
            "w": rng.random(groups),
        }
    )


def best_of(fn: Callable[[], object], repeat: int) -> float:
    best = float("inf")
    for _ in range(repeat):
        gc.collect()
        t = time.perf_counter()
        fn()
        best = min(best, time.perf_counter() - t)
    return best


def columns_of(x: Any) -> List[np.ndarray]:
    """A result of any engine as its columns, a scalar as one cell."""
    if isinstance(x, DataFrame):
        x = x.to_pandas()
    elif isinstance(x, Series):
        x = x.to_pandas()
    elif isinstance(x, pl.DataFrame):
        x = x.to_pandas()
    elif isinstance(x, pa.Table):
        x = x.to_pandas()
    if isinstance(x, (pd.DataFrame, pd.Series)):
        # A group key lives in a named index; a filter's index is only the
        # surviving row positions.
        if x.index.names != [None]:
            x = x.reset_index()
        elif isinstance(x, pd.Series):
            return [x.to_numpy()]
        return [x[c].to_numpy() for c in x.columns]
    return [np.asarray([x])]


def compare(ours: Any, theirs: Any, rtol: float, ordered: Optional[int]) -> str:
    """ "ok", or why the two results differ: the row count, then each column's
    sorted values (nulls counted, floats within rtol); `ordered` names a
    column that must be nondecreasing in both."""
    a, b = columns_of(ours), columns_of(theirs)
    if len(a) != len(b):
        return f"differs: {len(a)} vs {len(b)} columns"
    if a and len(a[0]) != len(b[0]):
        return f"differs: {len(a[0])} vs {len(b[0])} rows"
    for i, (x, y) in enumerate(zip(a, b)):
        if ordered == i and (
            np.any(np.diff(x.astype(float)) < 0) or np.any(np.diff(y.astype(float)) < 0)
        ):
            return f"differs: column {i} not sorted"
        if x.dtype.kind in "biuf" and y.dtype.kind in "biuf":
            x, y = x.astype(float), y.astype(float)
            if np.isnan(x).sum() != np.isnan(y).sum():
                return f"differs: column {i} null count"
            x, y = np.sort(x[~np.isnan(x)]), np.sort(y[~np.isnan(y)])
            if not np.allclose(x, y, rtol=rtol, atol=1e-9):
                worst = np.max(np.abs(x - y) / np.maximum(np.abs(y), 1e-9))
                return f"differs: column {i} values (max rel err {worst:.2e})"
        else:
            xs = np.sort(np.asarray([str(v) for v in x]))
            ys = np.sort(np.asarray([str(v) for v in y]))
            if not np.array_equal(xs, ys):
                return f"differs: column {i} values"
    return "ok"


def cores_used(fn: Callable[[], object]) -> float:
    """CPU time over wall time for one run: how many cores the op kept busy."""
    gc.collect()
    r0 = resource.getrusage(resource.RUSAGE_SELF)
    t = time.perf_counter()
    fn()
    wall = time.perf_counter() - t
    r1 = resource.getrusage(resource.RUSAGE_SELF)
    cpu = (r1.ru_utime - r0.ru_utime) + (r1.ru_stime - r0.ru_stime)
    return cpu / wall if wall > 0 else 0.0


def peak_rss() -> int:
    """The process's peak resident set in bytes."""
    r = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return r if sys.platform == "darwin" else r * 1024


def make_tables(args: argparse.Namespace) -> Dict[str, pa.Table]:
    return {
        "table": make_table(args.rows, args.groups, args.seed),
        "right": make_right(args.groups, args.seed),
        "trace": make_trace(args.rows, args.seed),
    }


def mapped_tables(directory: str) -> Dict[str, pa.Table]:
    """The tables a parent wrote as Arrow IPC files, memory-mapped: no
    generation garbage in this process's heap for an op to reuse unseen."""
    out = {}
    for name in ("table", "right", "trace"):
        source = pa.memory_map(os.path.join(directory, name + ".arrow"), "r")
        out[name] = pa.ipc.open_file(source).read_all()
    return out


def build_cases(
    args: argparse.Namespace, engines: Sequence[str], tables: Optional[Dict[str, pa.Table]] = None
) -> Dict[str, Dict[str, Any]]:
    """The ops per engine over the same Arrow tables, only the named
    engines' frames built. Each case also carries `rtol` (the tolerance for
    its float columns) and `ordered` (a column the result must be sorted
    by) for the correctness check."""
    tables = tables or make_tables(args)
    table, right, trace = tables["table"], tables["right"], tables["trace"]

    if "ours" in engines or "lazy" in engines:
        ours = DataFrame.from_arrow(table)
        ours_r = DataFrame.from_arrow(right)
        ours_t = DataFrame.from_arrow(trace)
    if "pandas" in engines:
        pdf = table.to_pandas()
        pdf_r = right.to_pandas()
        pdf_t = trace.to_pandas()
    if "polars" in engines:
        plf = pl.from_arrow(table)
        plf_r = pl.from_arrow(right)
        plf_t = pl.from_arrow(trace)
        assert isinstance(plf, pl.DataFrame) and isinstance(plf_r, pl.DataFrame)
        assert isinstance(plf_t, pl.DataFrame)
    if "duckdb" in engines:
        con = duckdb.connect()
        con.register("t", table)
        con.register("r", right)
        con.register("tr", trace)

    def sql(q: str) -> Callable[[], object]:
        return lambda: con.execute(q).fetch_arrow_table()

    cases: Dict[str, Dict[str, Any]] = {
        "filter v > 0.5": {
            "ours": lambda: ours[ours["v"] > 0.5],
            "lazy": lambda: ours.lazy().filter(col("v") > 0.5).collect(),
            "pandas": lambda: pdf[pdf["v"] > 0.5],
            "polars": lambda: plf.filter(pl.col("v") > 0.5),
            "duckdb": sql("SELECT * FROM t WHERE v > 0.5"),
        },
        "group_by k: sum v, mean v": {
            "ours": lambda: ours.groupby("k")["v"].agg(["sum", "mean"]),
            "lazy": lambda: ours.lazy().group_by("k", "sum:v", "mean:v").collect(),
            "pandas": lambda: pdf.groupby("k")["v"].agg(["sum", "mean"]),
            "polars": lambda: plf.group_by("k").agg(
                pl.col("v").sum().alias("sum_v"), pl.col("v").mean().alias("mean_v")
            ),
            "duckdb": sql("SELECT k, sum(v), avg(v) FROM t GROUP BY k"),
        },
        "sort by v": {
            "ordered": 1,
            "ours": lambda: ours.sort_values("v"),
            "lazy": lambda: ours.lazy().sort_by("v").collect(),
            "pandas": lambda: pdf.sort_values("v"),
            "polars": lambda: plf.sort("v"),
            "duckdb": sql("SELECT * FROM t ORDER BY v"),
        },
        "join on k (inner)": {
            "ours": lambda: ours.merge(ours_r, on="k", how="inner"),
            "lazy": lambda: ours.lazy().join(ours_r.lazy(), on="k", how="inner").collect(),
            "pandas": lambda: pdf.merge(pdf_r, on="k", how="inner"),
            "polars": lambda: plf.join(plf_r, on="k", how="inner"),
            "duckdb": sql("SELECT * FROM t JOIN r USING (k)"),
        },
        "rolling(100).mean of v": {
            "ours": lambda: ours["v"].rolling(100).mean(),
            "lazy": lambda: ours.lazy().select(col("v").rolling_mean(100).alias("v")).collect(),
            "pandas": lambda: pdf["v"].rolling(100).mean(),
            "polars": lambda: plf.select(pl.col("v").rolling_mean(100)),
            "duckdb": sql(
                "SELECT CASE WHEN row_number() OVER () >= 100 THEN "
                "avg(v) OVER (ROWS BETWEEN 99 PRECEDING AND CURRENT ROW) END FROM t"
            ),
        },
        "s contains 'file_00'": {
            "ours": lambda: ours["s"].str.contains("file_00", regex=False),
            "lazy": lambda: (
                ours.lazy().select(col("s").str.contains("file_00").alias("s")).collect()
            ),
            "pandas": lambda: pdf["s"].str.contains("file_00", regex=False),
            "polars": lambda: plf.select(pl.col("s").str.contains("file_00", literal=True)),
            "duckdb": sql("SELECT contains(s, 'file_00') FROM t"),
        },
        "cumsum of n": {
            "ours": lambda: ours["n"].cumsum(),
            "lazy": lambda: ours.lazy().select(col("n").cum_sum().alias("n")).collect(),
            "pandas": lambda: pdf["n"].cumsum(),
            "polars": lambda: plf.select(pl.col("n").cum_sum()),
            "duckdb": sql("SELECT sum(n) OVER (ROWS UNBOUNDED PRECEDING) FROM t"),
        },
        "filter then group_by": {
            "ours": lambda: ours[ours["v"] > 0.5].groupby("k")["v"].sum(),
            "lazy": lambda: ours.lazy().filter(col("v") > 0.5).group_by("k", "sum:v").collect(),
            "pandas": lambda: pdf[pdf["v"] > 0.5].groupby("k")["v"].sum(),
            "polars": lambda: (
                plf.lazy().filter(pl.col("v") > 0.5).group_by("k").agg(pl.col("v").sum()).collect()
            ),
            "duckdb": sql("SELECT k, sum(v) FROM t WHERE v > 0.5 GROUP BY k"),
        },
        # ---- trace analyses ------------------------------------------------
        "trace: 1s time buckets, count + sum dur": {
            "ours": lambda: ours_t.groupby(ours_t["ts"] // 1_000_000 * 1_000_000)["dur"].agg(
                ["count", "sum"]
            ),
            "lazy": lambda: (
                ours_t.lazy().group_by_dynamic("ts", 1_000_000, aggs=["count", "sum:dur"]).collect()
            ),
            "pandas": lambda: pdf_t.groupby(pdf_t["ts"] // 1_000_000 * 1_000_000)["dur"].agg(
                ["count", "sum"]
            ),
            "polars": lambda: plf_t.group_by(
                (pl.col("ts") // 1_000_000 * 1_000_000).alias("b")
            ).agg(pl.len(), pl.col("dur").sum()),
            "duckdb": sql(
                "SELECT ts // 1000000 * 1000000 AS b, count(*), sum(dur) FROM tr GROUP BY b"
            ),
        },
        "trace: slowest 100 calls": {
            "ours": lambda: ours_t.nlargest(100, "dur"),
            "lazy": lambda: ours_t.lazy().topk("dur", 100).collect(),
            "pandas": lambda: pdf_t.nlargest(100, "dur"),
            "polars": lambda: plf_t.top_k(100, by="dur"),
            "duckdb": sql("SELECT * FROM tr ORDER BY dur DESC LIMIT 100"),
        },
        "trace: per (pid, name) mean/max dur": {
            "ours": lambda: ours_t.groupby(["pid", "name"])["dur"].agg(["mean", "max"]),
            "lazy": lambda: (
                ours_t.lazy().group_by(["pid", "name"], "mean:dur", "max:dur").collect()
            ),
            "pandas": lambda: pdf_t.groupby(["pid", "name"])["dur"].agg(["mean", "max"]),
            "polars": lambda: plf_t.group_by("pid", "name").agg(
                pl.col("dur").mean().alias("m"), pl.col("dur").max().alias("x")
            ),
            "duckdb": sql("SELECT pid, name, avg(dur), max(dur) FROM tr GROUP BY pid, name"),
        },
        "trace: POSIX calls over 100us, per name count": {
            "ours": lambda: (
                ours_t[(ours_t["cat"] == "POSIX") & (ours_t["dur"] > 100)].groupby("name").size()
            ),
            "lazy": lambda: (
                ours_t.lazy()
                .filter((col("cat") == "POSIX") & (col("dur") > 100))
                .group_by("name", "count")
                .collect()
            ),
            "pandas": lambda: (
                pdf_t[(pdf_t["cat"] == "POSIX") & (pdf_t["dur"] > 100)].groupby("name").size()
            ),
            "polars": lambda: (
                plf_t.lazy()
                .filter((pl.col("cat") == "POSIX") & (pl.col("dur") > 100))
                .group_by("name")
                .agg(pl.len())
                .collect()
            ),
            "duckdb": sql(
                "SELECT name, count(*) FROM tr WHERE cat = 'POSIX' AND dur > 100 GROUP BY name"
            ),
        },
        "trace: per-pid busy time (sum dur)": {
            "ours": lambda: ours_t.groupby("pid")["dur"].sum(),
            "lazy": lambda: ours_t.lazy().group_by("pid", "sum:dur").collect(),
            "pandas": lambda: pdf_t.groupby("pid")["dur"].sum(),
            "polars": lambda: plf_t.group_by("pid").agg(pl.col("dur").sum()),
            "duckdb": sql("SELECT pid, sum(dur) FROM tr GROUP BY pid"),
        },
        "trace: gaps between calls (ts diff), p99": {
            "rtol": 1e-2,
            "ours": lambda: ours_t["ts"].diff().quantile(0.99),
            # The plan's quantile is the streaming sketch, exact to about 1e-3.
            "lazy": lambda: (
                ours_t.lazy()
                .select(col("ts").diff().alias("d"))
                .select(col("d").quantile(0.99))
                .collect()
            ),
            "pandas": lambda: pdf_t["ts"].diff().quantile(0.99),
            "polars": lambda: plf_t.select(pl.col("ts").diff().quantile(0.99)),
            "duckdb": sql(
                "SELECT quantile_cont(d, 0.99) FROM (SELECT ts - lag(ts) OVER () AS d FROM tr)"
            ),
        },
    }
    return cases


LIBS = ["ours", "lazy", "pandas", "polars", "duckdb"]


def run_timing(args: argparse.Namespace) -> None:
    cases = build_cases(args, LIBS)
    print(f"rows={args.rows:,} groups={args.groups:,} repeat={args.repeat} (best of)")
    print(
        f"{'op':44s} "
        + " ".join(f"{lib:>10s}" for lib in LIBS)
        + "   "
        + " ".join(f"{lib + '/ours':>11s}" for lib in LIBS[1:])
    )
    checks: List[str] = []
    cores: List[str] = []
    for name, case in cases.items():
        times: List[Optional[float]] = []
        for lib in LIBS:
            try:
                times.append(best_of(case[lib], args.repeat))
            except Exception as e:  # noqa: BLE001 - report, keep going
                print(f"{name}: {lib} failed: {e}")
                times.append(None)
        cells = " ".join(f"{t * 1000:9.1f}ms" if t is not None else f"{'fail':>11s}" for t in times)
        o = times[0]
        ratios = " ".join(f"{t / o:10.2f}x" if o and t else f"{'-':>11s}" for t in times[1:])
        print(f"{name:44s} {cells} {ratios}")
        used = []
        for lib in LIBS:
            try:
                used.append(f"{cores_used(case[lib]):10.1f}")
            except Exception:  # noqa: BLE001
                used.append(f"{'fail':>10s}")
        cores.append(f"{name:44s} " + " ".join(used))
        mine = case["ours"]()
        verdicts = []
        for lib in LIBS[1:]:
            try:
                verdicts.append(
                    compare(mine, case[lib](), case.get("rtol", 1e-9), case.get("ordered"))
                )
            except Exception as e:  # noqa: BLE001
                verdicts.append(f"check failed: {e}")
        checks.append(
            f"{name:44s} " + " ".join(f"{lib}: {v}" for lib, v in zip(LIBS[1:], verdicts))
        )
    print()
    print(f"cores kept busy during the op (cpu time / wall, {os.cpu_count()} on this machine):")
    print(f"{'op':44s} " + " ".join(f"{lib:>10s}" for lib in LIBS))
    for line in cores:
        print(line)
    print()
    print("correctness against ours (row count, sorted values per column):")
    for line in checks:
        print(line)


def run_memory_one(args: argparse.Namespace) -> None:
    """Child of --memory: one (op, engine). Builds, says READY, waits for
    a line, runs the op and says DONE; the parent samples this process's
    resident set in between, from outside, so an op that holds the GIL
    cannot hide its peak from the sampler. After DONE it prints its own
    lower bound: the resident set with the result still held, or the
    kernel's high-water mark, whichever rose more."""
    import psutil

    tables = mapped_tables(args.memory_input)
    case = build_cases(args, [args.memory_engine], tables)[args.memory_case]
    gc.collect()
    release_free_heap()
    me = psutil.Process()
    print("READY", flush=True)
    sys.stdin.readline()
    base = me.memory_info().rss
    before = peak_rss()
    result = case[args.memory_engine]()
    grown = max(peak_rss() - before, me.memory_info().rss - base)
    del result
    print("DONE", flush=True)
    print(grown, flush=True)


def release_free_heap() -> None:
    """Hand the allocator's free pages back to the kernel, so the resident
    set before the op is what the frames take and not what building them
    left behind (which the op would otherwise reuse unseen)."""
    import ctypes

    try:
        libc = ctypes.CDLL(None)
        if sys.platform == "darwin":
            libc.malloc_zone_pressure_relief(None, 0)
        else:
            libc.malloc_trim(0)
    except (OSError, AttributeError):
        pass


def memory_of(cmd: List[str]) -> Optional[int]:
    """Run a --memory-one child and return its op's peak resident growth
    in bytes: the largest sample while it runs minus its level at READY."""
    import psutil

    child = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True
    )
    assert child.stdin is not None and child.stdout is not None
    proc = psutil.Process(child.pid)
    line = child.stdout.readline()
    while line and line.strip() != "READY":
        line = child.stdout.readline()
    if not line:
        child.wait()
        return None
    base = proc.memory_info().rss
    peak = [base]
    done = threading.Event()

    def sample() -> None:
        while not done.is_set():
            try:
                peak[0] = max(peak[0], proc.memory_info().rss)
            except psutil.Error:
                return

    t = threading.Thread(target=sample, daemon=True)
    t.start()
    child.stdin.write("go\n")
    child.stdin.flush()
    line = child.stdout.readline()
    while line and line.strip() != "DONE":
        line = child.stdout.readline()
    done.set()
    t.join()
    grown = child.stdout.readline().strip()
    child.wait()
    if not line:
        return None
    return max(peak[0] - base, int(grown) if grown.isdigit() else 0)


def run_memory(args: argparse.Namespace) -> None:
    tables = make_tables(args)
    names = list(build_cases(args, [], tables).keys())
    print(f"rows={args.rows:,} groups={args.groups:,}: peak RSS growth during the op, MB")
    print(f"{'op':44s} " + " ".join(f"{lib:>10s}" for lib in LIBS))
    with tempfile.TemporaryDirectory() as directory:
        for name, t in tables.items():
            with pa.OSFile(os.path.join(directory, name + ".arrow"), "wb") as sink:
                with pa.ipc.new_file(sink, t.schema) as writer:
                    writer.write_table(t)
        del tables
        for name in names:
            cells = []
            for lib in LIBS:
                cmd = [
                    sys.executable,
                    __file__,
                    "--rows",
                    str(args.rows),
                    "--groups",
                    str(args.groups),
                    "--seed",
                    str(args.seed),
                    "--memory-one",
                    name,
                    lib,
                    "--memory-input",
                    directory,
                ]
                grown = memory_of(cmd)
                cells.append(f"{grown / 2**20:10.1f}" if grown is not None else f"{'fail':>10s}")
            print(f"{name:44s} " + " ".join(cells))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=10_000_000)
    ap.add_argument("--groups", type=int, default=10_000)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--memory", action="store_true", help="peak RSS growth per (op, engine)")
    ap.add_argument("--memory-one", nargs=2, metavar=("CASE", "ENGINE"), help=argparse.SUPPRESS)
    ap.add_argument("--memory-input", help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.memory_one:
        args.memory_case, args.memory_engine = args.memory_one
        run_memory_one(args)
    elif args.memory:
        run_memory(args)
    else:
        run_timing(args)


if __name__ == "__main__":
    main()
