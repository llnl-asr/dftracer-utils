#!/usr/bin/env python3
"""Time and memory of the analyzer-shaped group-by against pandas.

The frame has two keys (a string and a small int) and 20 nullable value
columns. Two workloads:

  sums      10 sum, 5 min, 5 max columns (20 aggregates)
  analyzer  every column with sum, min, max, mean, std and count (120 aggregates,
            the shape of the analyzer's main-view group-by)

Every case runs in its own process, and that process is watched from outside.
The child builds the frame from NumPy arrays (pandas shares the buffers, the
extension borrows or copies them once, so almost nothing is left over), drops the
arrays, hands freed memory back to the system, tells the parent it is READY and
waits. The parent notes the child's physical memory, says GO, and samples that
memory every millisecond until the child reports. The extension holds the Python
lock for the whole op, so a sampler thread inside the child would see nothing;
sampling from the parent does not depend on it.

What is reported, in GB:
  idle     the child's physical memory before the data exists
  base     the child's physical memory when it is READY: idle + the frame
  peak     the highest physical memory seen between GO and the answer
  rise     peak - base: what the op needed on top of the frame
  out      the output: groups x (columns + keys) x 8 bytes

"Physical memory" is the footprint macOS reports (`ri_phys_footprint` from
`proc_pid_rusage`, which counts compressed pages, so it is not fooled by the
memory compressor the way the resident set is). Elsewhere it is the unique set
size (psutil `memory_full_info().uss`). A frame converted from another frame
leaves freed pages the allocator keeps, which a later op reuses for free and
which then hides what it allocates; building from NumPy buffers avoids that.
`--warmup` runs the op once before measuring, to see how much that changes.

    python benchmarks/groupby_analyzer_bench.py --rows 1_000_000 5_000_000 --groups 20_000 200_000
    python benchmarks/groupby_analyzer_bench.py --workload analyzer --rows 20_000_000 --groups 200_000
    python benchmarks/groupby_analyzer_bench.py --gate        # exits 1 when a gate fails

Run from a venv holding pandas, numpy, psutil and the extension.
"""

from __future__ import annotations

import argparse
import ctypes
import gc
import json
import subprocess
import sys
import threading
import time

KEYS = ["proc_name", "time_range"]
SUM = [f"s{i}" for i in range(10)]
MIN = [f"lo{i}" for i in range(5)]
MAX = [f"hi{i}" for i in range(5)]
VALUES = SUM + MIN + MAX
SUMS_SPEC = {**{c: "sum" for c in SUM}, **{c: "min" for c in MIN}, **{c: "max" for c in MAX}}
ANALYZER_AGGS = ("sum", "min", "max", "mean", "std", "count")
N_TIME = 20
WORKLOADS = ("sums", "analyzer")


def n_outputs(workload: str) -> int:
    return len(VALUES) * (1 if workload == "sums" else len(ANALYZER_AGGS))


# ---- physical memory of a process ------------------------------------------


class _RusageV2(ctypes.Structure):
    _fields_ = [
        ("ri_uuid", ctypes.c_uint8 * 16),
        ("ri_user_time", ctypes.c_uint64),
        ("ri_system_time", ctypes.c_uint64),
        ("ri_pkg_idle_wkups", ctypes.c_uint64),
        ("ri_interrupt_wkups", ctypes.c_uint64),
        ("ri_pageins", ctypes.c_uint64),
        ("ri_wired_size", ctypes.c_uint64),
        ("ri_resident_size", ctypes.c_uint64),
        ("ri_phys_footprint", ctypes.c_uint64),
        ("ri_proc_start_abstime", ctypes.c_uint64),
        ("ri_proc_exit_abstime", ctypes.c_uint64),
        ("ri_child_user_time", ctypes.c_uint64),
        ("ri_child_system_time", ctypes.c_uint64),
        ("ri_child_pkg_idle_wkups", ctypes.c_uint64),
        ("ri_child_interrupt_wkups", ctypes.c_uint64),
        ("ri_child_pageins", ctypes.c_uint64),
        ("ri_child_elapsed_abstime", ctypes.c_uint64),
        ("ri_diskio_bytesread", ctypes.c_uint64),
        ("ri_diskio_byteswritten", ctypes.c_uint64),
    ]


def footprint_reader(pid: int):
    """A function returning the physical memory of process `pid` in bytes, and
    the name of what it reads."""
    if sys.platform == "darwin":
        libproc = ctypes.CDLL("/usr/lib/libproc.dylib")
        libproc.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
        info = _RusageV2()

        def read() -> int:
            libproc.proc_pid_rusage(pid, 2, ctypes.byref(info))  # RUSAGE_INFO_V2
            return int(info.ri_phys_footprint)

        return read, "phys_footprint (macOS proc_pid_rusage)"
    import psutil

    p = psutil.Process(pid)
    try:
        p.memory_full_info()
        return (lambda: int(p.memory_full_info().uss)), "uss (psutil)"
    except Exception:
        return (lambda: int(p.memory_info().rss)), "rss (psutil)"


def relieve():
    """Give freed memory back to the system."""
    gc.collect()
    try:
        if sys.platform == "darwin":
            lib = ctypes.CDLL(None)
            lib.malloc_zone_pressure_relief.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            lib.malloc_zone_pressure_relief(None, 0)
        else:
            ctypes.CDLL("libc.so.6").malloc_trim(0)
    except Exception:
        pass


# ---- the child: build a frame, wait, run the op ------------------------------


def arrays(n: int, g: int, seed: int = 0):
    import numpy as np

    rng = np.random.default_rng(seed)
    n_proc = max(1, g // N_TIME)
    names = np.array([f"app#host#{i}#{i}" for i in range(n_proc)], dtype=object)
    key = names[rng.integers(0, n_proc, n)]  # 8 bytes a row, the strings shared
    time_range = rng.integers(0, N_TIME, n)
    values, masks = {}, {}
    for i, c in enumerate(VALUES):
        values[c] = rng.integers(0, 1000, n) if i % 2 else rng.random(n) * 100
        masks[c] = rng.random(n) < 0.05
    return key, time_range, values, masks


def build_pandas(n, g):
    import pandas as pd

    key, time_range, values, masks = arrays(n, g)
    cols = {"proc_name": pd.array(key, dtype="string"), "time_range": time_range}
    for i, c in enumerate(VALUES):
        cols[c] = (pd.arrays.IntegerArray if i % 2 else pd.arrays.FloatingArray)(
            values[c], masks[c]
        )
    return pd.DataFrame(cols)


def build_utils(n, g):
    import numpy as np

    from dftracer.utils import DataFrame, Series

    key, time_range, values, masks = arrays(n, g)
    cols = {"proc_name": Series.from_numpy(key), "time_range": Series.from_numpy(time_range)}
    for c in VALUES:
        cols[c] = Series.from_numpy(np.ma.masked_array(values[c], mask=masks[c]))
    return DataFrame.from_dict(cols)


def pandas_op(df, workload):
    spec = SUMS_SPEC if workload == "sums" else {c: list(ANALYZER_AGGS) for c in VALUES}
    return df.groupby(KEYS).agg(spec)


def utils_aggs(workload):
    from dftracer.utils import col

    if workload == "sums":
        return {c: getattr(col(c), h)() for c, h in SUMS_SPEC.items()}
    return {f"{c}_{h}": getattr(col(c), h)() for c in VALUES for h in ANALYZER_AGGS}


def child(engine: str, op: str, workload: str, n: int, g: int, warmup: bool) -> None:
    # The parent reads the physical memory of this process from outside.
    relieve()
    if engine == "pandas":
        df = build_pandas(n, g)
        run = lambda: pandas_op(df, workload)  # noqa: E731
    else:
        df = build_utils(n, g)
        aggs = utils_aggs(workload) if op != "to_pandas" else None
        if op == "groupby":
            run = lambda: df.group_by(KEYS).agg(**aggs)  # noqa: E731
        elif op == "groupby+to_pandas":
            run = lambda: df.group_by(KEYS).agg(**aggs).to_pandas(nullable=True)  # noqa: E731
        elif op == "to_pandas":
            run = lambda: df.to_pandas(nullable=True)  # noqa: E731
        else:
            raise SystemExit(f"unknown op {op}")
    if warmup:
        r = run()
        del r
    relieve()
    print("READY", flush=True)
    sys.stdin.readline()  # GO
    t = time.perf_counter()
    r = run()
    dt = time.perf_counter() - t
    out_groups = len(r) if op != "to_pandas" else n
    print(json.dumps({"seconds": dt, "out_groups": out_groups}), flush=True)
    sys.stdin.readline()  # the parent reads the memory once more, then ends this process


def run_child(engine, op, workload, n, g, warmup=False, timeline=False):
    cmd = [sys.executable, __file__, "--child", engine, op, workload, str(n), str(g)]
    if warmup:
        cmd.append("--warmup")
    base_cmd = dict(
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    p = subprocess.Popen(cmd, **base_cmd)
    out = {"engine": engine, "op": op, "workload": workload, "rows": n, "groups": g}
    try:
        line = p.stdout.readline()
        if line.strip() != "READY":
            p.kill()
            out["error"] = (line + p.stderr.read())[-300:]
            return out
        read, what = footprint_reader(p.pid)
        base = read()
        peak = base
        samples = []
        stop = threading.Event()
        t0 = time.perf_counter()

        def sample():
            nonlocal peak
            while not stop.is_set():
                v = read()
                if v > peak:
                    peak = v
                if timeline:
                    samples.append((round(time.perf_counter() - t0, 4), round(v / 1e9, 4)))

        th = threading.Thread(target=sample, daemon=True)
        th.start()
        p.stdin.write("GO\n")
        p.stdin.flush()
        answer = p.stdout.readline()
        stop.set()
        th.join()
        after = read()
        peak = max(peak, after)
        p.stdin.write("END\n")
        p.stdin.flush()
        p.wait(timeout=60)
        if not answer.strip().startswith("{"):
            out["error"] = (answer + p.stderr.read())[-300:]
            return out
        res = json.loads(answer)
        result_cols = n_outputs(workload) + len(KEYS)
        out.update(
            seconds=res["seconds"],
            base_gb=round(base / 1e9, 3),
            peak_gb=round(peak / 1e9, 3),
            after_gb=round(after / 1e9, 3),
            rise_gb=round((peak - base) / 1e9, 3),
            out_groups=res["out_groups"],
            out_gb=round(res["out_groups"] * result_cols * 8 / 1e9, 3),
            measure=what,
            warmup=warmup,
        )
        if timeline:
            out["timeline"] = samples
        return out
    finally:
        if p.poll() is None:
            p.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--child", nargs=5, metavar=("ENGINE", "OP", "WORKLOAD", "ROWS", "GROUPS"))
    ap.add_argument("--warmup", action="store_true", help="run the op once before the measured run")
    ap.add_argument(
        "--timeline",
        action="store_true",
        help="keep a memory sample every millisecond in the jsonl",
    )
    ap.add_argument("--rows", nargs="+", default=["1_000_000", "5_000_000"])
    ap.add_argument("--groups", nargs="+", default=["20_000", "200_000"])
    ap.add_argument("--engines", nargs="+", default=["pandas", "utils"])
    ap.add_argument("--ops", nargs="+", default=["groupby"])
    ap.add_argument("--workload", nargs="+", default=["analyzer"], choices=WORKLOADS)
    ap.add_argument(
        "--repeat", type=int, default=1, help="runs per case; the report keeps the fastest"
    )
    ap.add_argument("--jsonl", default=None)
    ap.add_argument("--gate", action="store_true")
    a = ap.parse_args()
    if a.child:
        e, op, w, n, g = a.child
        child(e, op, w, int(n), int(g), a.warmup)
        return
    import psutil

    results = []
    for w in a.workload:
        for n in (int(x) for x in a.rows):
            for g in (int(x) for x in a.groups):
                need = 20 * n * 9 * 1.0e-9 * 3  # the frame, plus the op, plus slack
                if psutil.virtual_memory().available / 1e9 < need:
                    print(
                        f"skip {w} {n:,} rows {g:,} groups: needs about {need:.0f} GB free, "
                        f"have {psutil.virtual_memory().available / 1e9:.0f} GB"
                    )
                    continue
                for op in a.ops:
                    for e in a.engines:
                        if e == "pandas" and op != "groupby":
                            continue
                        runs = [
                            run_child(e, op, w, n, g, a.warmup, a.timeline) for _ in range(a.repeat)
                        ]
                        good = [r for r in runs if "error" not in r]
                        r = dict(min(good, key=lambda x: x["seconds"])) if good else runs[0]
                        if good:
                            # The time is the fastest run's; the memory is the median of the runs,
                            # one run's peak being noisy.
                            rises = sorted(x["rise_gb"] for x in good)
                            r["rise_gb"] = rises[len(rises) // 2]
                        results.append(r)
                        if "error" in r:
                            print(f"{e:7} {w:8} {op:18} {n:>11,} {g:>8,}  ERROR {r['error']}")
                        else:
                            print(
                                f"{e:7} {w:8} {op:18} {n:>11,} {g:>8,}  {r['seconds']:7.2f} s  "
                                f"rise +{r['rise_gb']:5.2f} GB (out {r['out_gb']:.2f}, base {r['base_gb']:.2f}, "
                                f"peak {r['peak_gb']:.2f}, after {r['after_gb']:.2f})",
                                flush=True,
                            )
    if a.jsonl:
        with open(a.jsonl, "w") as f:
            for r in results:
                f.write(json.dumps(r) + "\n")
    if a.gate:
        bad = []
        by = {
            (r["engine"], r["op"], r["workload"], r["rows"], r["groups"]): r
            for r in results
            if "error" not in r
        }
        for (e, op, w, n, g), r in by.items():
            if e != "utils" or op != "groupby":
                continue
            p = by.get(("pandas", "groupby", w, n, g))
            if not p:
                continue
            mine, theirs = r["rise_gb"], p["rise_gb"]
            # Gate 1: with many groups at 5M rows and up, the analyzer workload takes no more
            # memory on top of the frame than pandas, and the sums workload at most 1.5x.
            if n >= 5_000_000 and g >= 200_000:
                limit = theirs if w == "analyzer" else 1.5 * theirs
                if mine > limit:
                    bad.append(
                        f"memory {w} {n:,} rows {g:,} groups: utils +{mine:.3f} GB > "
                        f"{'pandas' if w == 'analyzer' else '1.5x pandas'} +{limit:.3f} GB"
                    )
            # Gate 2: never more than twice pandas, at any size.
            if mine > 2 * theirs + 0.05:
                bad.append(
                    f"memory {w} {n:,} rows {g:,} groups: utils +{mine:.3f} GB > 2x pandas +{theirs:.3f} GB"
                )
            # Gate 3: no slower than pandas with many rows and many groups.
            if n >= 5_000_000 and g >= 200_000 and r["seconds"] > 1.0 * p["seconds"]:
                bad.append(
                    f"time {w} {n:,} rows {g:,} groups: utils {r['seconds']:.2f} s > pandas {p['seconds']:.2f} s"
                )
        # Gate 4: for a fixed number of groups the extra memory does not grow with the rows:
        # at the most rows it is within 25 percent (and 0.03 GB) of what it is at the fewest.
        flat = {}
        for (e, op, w, n, g), r in by.items():
            if e == "utils" and op == "groupby":
                flat.setdefault((w, g), []).append((n, r["rise_gb"]))
        for (w, g), pts in flat.items():
            pts.sort()
            if len(pts) >= 2 and pts[-1][1] > 1.25 * pts[0][1] + 0.03:
                bad.append(
                    f"flat in rows {w} {g:,} groups: +{pts[0][1]:.3f} GB at {pts[0][0]:,} rows, "
                    f"+{pts[-1][1]:.3f} GB at {pts[-1][0]:,} rows"
                )
        if bad:
            print("GATE FAIL")
            for b in bad:
                print("  " + b)
            sys.exit(1)
        print("GATE PASS")


if __name__ == "__main__":
    main()
