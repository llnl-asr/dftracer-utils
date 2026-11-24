#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A batch-hook jit plugin against the same computation written by hand.

Two computations over a trace of N events, each keeping the events with
``dur > 20 and pid != 3`` and folding a sum and their count:

  light   ``sum(dur * 0.5 + ts)``                              (about 6 ops)
  heavy   ``sum(abs(c) * 2 + c * c)`` with
          ``c = clip((dur * 0.5 + ts - pid * 3) / (dur + 1), -5, 50)``
          (about 20 ops)

Each runs through the same fused scan (`Plugins.run`) as:

  floor            a batch hook that only counts rows: the scan itself, no compute
  <w>_batch        @jit.each_batch (filter first for light, as the hand-written ops do)
  light_per_event  @jit.each_event, one guard around the updates
  light_hand       a plugin whose batch body is hand-written C over the dataframe
                   kernels: compare, logical, filter, cast, mul, add, reduce
  <w>_hand_expr    a plugin whose batch body builds the ``dftu_expr`` trees by hand
                   and evaluates them in one fused ``dftu_expr_eval_many`` program

on two traces of the same N events: ``big`` (8 files, batches of N/8 rows) and
``small`` (files of ``--small-rows`` rows, so batches of 1e3 to 1e4 rows, and the
expression compiles once per batch).

The bar: the compute of ``batch``, its median wall time minus the ``floor``
median (the scan is the same work in every case), is no more than 5 percent
above that of ``hand_expr``. Runs alternate in rounds so drift in machine load
hits every case alike; medians over ``--runs``.

    python benchmarks/jit_batch_bench.py --events 1_000_000 --runs 5
    python benchmarks/jit_batch_bench.py --trace-dir /path/to/reuse --gate

Run from a venv holding pyarrow and the extension, with a C++ compiler.
"""

from __future__ import annotations

import argparse
import gzip
import os
import statistics
import sys
import tempfile
import time
from typing import Dict, List, Tuple

from dftracer.utils import jit
from dftracer.utils.runtime import Runtime
from dftracer.utils.jit import Counter as _Counter
from dftracer.utils.jit import JitPlugin, _emit, _MapDecl
from dftracer.utils.jit import Sum as _Sum
from dftracer.utils.jit._batch import HELPERS, BatchHook
from dftracer.utils.plugins import Plugins

BIG_FILES = 8


def write_trace(root: str, events: int, files: int) -> None:
    os.makedirs(root, exist_ok=True)
    per_file = events // files
    for f in range(files):
        path = os.path.join(root, f"t{f:05d}.pfw.gz")
        with gzip.open(path, "wt", encoding="utf-8", compresslevel=1) as fh:
            for i in range(per_file):
                n = f * per_file + i
                fh.write(
                    f'{{"name":"read","cat":"POSIX","pid":{1 + n % 4},"tid":1,"ts":{1000 + n},'
                    f'"dur":{10 + (n * 7) % 53},"ph":1,"args":{{}}}}\n'
                )


def expected(events: int, files: int) -> Dict[str, Tuple[float, int]]:
    total = events // files * files
    light, heavy, count = 0.0, 0.0, 0
    for n in range(total):
        dur, pid, ts = 10 + (n * 7) % 53, 1 + n % 4, 1000 + n
        if dur > 20 and pid != 3:
            count += 1
            light += dur * 0.5 + ts
            c = min(max((dur * 0.5 + ts - pid * 3) / (dur + 1), -5.0), 50.0)
            heavy += abs(c) * 2 + c * c
    return {"light": (light, count), "heavy": (heavy, count)}


@jit.plugin
class Floor:
    n: int = 0

    @jit.each_batch
    def step(self, df):
        self.n += df["dur"].count()


@jit.plugin
class LightBatch:
    total: float = 0.0
    n: int = 0

    @jit.each_batch
    def step(self, df):
        keep = (df["dur"] > 20) & (df["pid"] != 3)
        dur = df["dur"][keep]
        ts = df["ts"][keep]
        self.total += (dur * 0.5 + ts).sum()
        self.n += keep.sum()


@jit.plugin
class HeavyBatch:
    total: float = 0.0
    n: int = 0

    @jit.each_batch
    def step(self, df):
        dur = df["dur"]
        pid = df["pid"]
        a = dur * 0.5 + df["ts"]
        b = (a - pid * 3) / (dur + 1)
        c = b.clip(-5.0, 50.0)
        d = abs(c) * 2 + c * c
        keep = (dur > 20) & (pid != 3)
        self.total += d[keep].sum()
        self.n += keep.sum()


@jit.plugin
class LightPerEvent:
    total: float = 0.0
    n: int = 0

    @jit.each_event
    def step(self, e):
        if e.dur > 20 and e.pid != 3:
            self.total += e.dur * 0.5 + e.ts
            self.n += 1


# The fold of a hand-written body: one row per field, as the lowering emits.
_FOLD = """\
dftu_scalar _sum = dftu_series_reduce(_sel, DFTU_REDUCE_SUM);
dftu_scalar _cnt = dftu_series_reduce(_t[2], DFTU_REDUCE_SUM);
uint32_t _a = _n_total++;
_v0_total[_a] = dftu_jb_to_f64(_sum);
uint32_t _b = _n_n++;
int64_t _c = 0;
dftu_jb_to_i64(_cnt, &_c);
_v0_n[_b] = _c;
"""

# Kernel by kernel: every intermediate is a dataframe Series and is freed.
_HAND_LIGHT = (
    """\
{
dftu_series* _dur = dftu_dataframe_column(df, "dur");
dftu_series* _pid = dftu_dataframe_column(df, "pid");
dftu_series* _ts = dftu_dataframe_column(df, "ts");
dftu_series* _t[16] = {0};
if (_dur && _pid && _ts) {
_t[0] = dftu_series_compare(_dur, DFTU_CMP_GT, dftu_jb_si(20));
_t[1] = dftu_series_compare(_pid, DFTU_CMP_NE, dftu_jb_si(3));
_t[2] = dftu_series_logical(_t[0], _t[1], DFTU_LOGICAL_AND);
_t[3] = dftu_series_filter(_dur, _t[2]);
_t[4] = dftu_series_filter(_ts, _t[2]);
_t[5] = dftu_series_materialize(_t[3]);
_t[6] = dftu_series_materialize(_t[4]);
_t[7] = dftu_series_cast(_t[5], DFTU_TYPE_FLOAT64);
_t[8] = dftu_series_mul_scalar(_t[7], dftu_jb_sf(0.5));
_t[9] = dftu_series_cast(_t[6], DFTU_TYPE_FLOAT64);
_t[10] = dftu_series_add(_t[8], _t[9]);
dftu_series* _sel = _t[10];
"""
    + _FOLD
    + """\
}
for (int _k = 0; _k < 16; ++_k) dftu_series_free(_t[_k]);
dftu_series_free(_dur);
dftu_series_free(_pid);
dftu_series_free(_ts);
}
"""
)

# The expression trees built by hand and run as one fused program.
_HELPER_X = (
    "static dftu_expr* dftu_hb_keep(dftu_expr** a, int* n, dftu_expr* e) "
    "{ a[(*n)++] = e; return e; }"
)
_HAND_EXPR_PRELUDE = """\
{
dftu_series* _in[3] = {dftu_dataframe_column(df, "dur"), dftu_dataframe_column(df, "pid"),
                       dftu_dataframe_column(df, "ts")};
dftu_series* _t[16] = {0};
dftu_expr* _x[96] = {0};
int _nx = 0;
#define X(e) dftu_hb_keep(_x, &_nx, (e))
if (_in[0] && _in[1] && _in[2]) {
dftu_expr* _dur = X(dftu_expr_cast(DFTU_TYPE_INT64, X(dftu_expr_col(0))));
dftu_expr* _pid = X(dftu_expr_cast(DFTU_TYPE_INT64, X(dftu_expr_col(1))));
dftu_expr* _ts = X(dftu_expr_cast(DFTU_TYPE_INT64, X(dftu_expr_col(2))));
dftu_expr* _keep = X(dftu_expr_logical(
    DFTU_LOGICAL_AND, X(dftu_expr_cmp(DFTU_CMP_GT, _dur, dftu_jb_si(20))),
    X(dftu_expr_cmp(DFTU_CMP_NE, _pid, dftu_jb_si(3)))));
"""
_HAND_EXPR_LIGHT = (
    _HAND_EXPR_PRELUDE
    + """\
dftu_expr* _val = X(dftu_expr_binary(
    0, X(dftu_expr_binary(2, _dur, X(dftu_expr_lit_f64(0.5)))), _ts));
"""
)
_HAND_EXPR_HEAVY = (
    _HAND_EXPR_PRELUDE
    + """\
dftu_expr* _a = X(dftu_expr_binary(
    0, X(dftu_expr_binary(2, _dur, X(dftu_expr_lit_f64(0.5)))), _ts));
dftu_expr* _b = X(dftu_expr_binary(
    3, X(dftu_expr_binary(1, _a, X(dftu_expr_binary(2, _pid, X(dftu_expr_lit_i64(3)))))),
    X(dftu_expr_binary(0, _dur, X(dftu_expr_lit_i64(1))))));
dftu_expr* _c = X(dftu_expr_clip(_b, dftu_jb_sf(-5.0), dftu_jb_sf(50.0)));
dftu_expr* _val = X(dftu_expr_binary(
    0, X(dftu_expr_binary(2, X(dftu_expr_unary(0, _c)), X(dftu_expr_lit_i64(2)))),
    X(dftu_expr_binary(2, _c, _c))));
"""
)
_HAND_EXPR_TAIL = (
    """\
const dftu_expr* _roots[2] = {_val, _keep};
dftu_series* _out[2] = {0, 0};
if (dftu_expr_eval_many(_roots, 2, _in, 3, _out) == 2) {
_t[0] = _out[0];
_t[2] = _out[1];
_t[3] = dftu_series_filter(_t[0], _t[2]);
dftu_series* _sel = _t[3];
"""
    + _FOLD
    + """\
}
}
#undef X
for (int _k = 0; _k < 16; ++_k) dftu_series_free(_t[_k]);
for (int _k = 0; _k < _nx; ++_k) dftu_expr_free(_x[_k]);
for (int _k = 0; _k < 3; ++_k) dftu_series_free(_in[_k]);
}
"""
)


def make_hand(name: str, body: str) -> type:
    hook = BatchHook()
    hook.lines = body.splitlines()
    hook.columns = ["dur", "pid", "ts"]
    hook.rows = {"total": 1, "n": 1}
    maps = {
        "total": _MapDecl((), (_Sum("DFTU_AGG_SUM", elem="f64"),), False),
        "n": _MapDecl((), (_Counter("DFTU_AGG_SUM", elem="i64"),), False),
    }
    ids = {a: a for a in maps}
    source = _emit(
        maps,
        [],
        set(),
        None,
        [],
        [],
        set(),
        hook.rows,
        HELPERS + [_HELPER_X],
        {},
        {},
        map_ids=ids,
        batch=hook,
    )
    cls = type(name, (), {})
    setattr(cls, "_jit_plugin", JitPlugin(name, source, {}, {a: a for a in maps}, False, "batch"))
    return cls


THREADS = 0


def run_once(cls: type, trace: str) -> Tuple[float, Dict[str, float]]:
    import pyarrow as pa

    t0 = time.perf_counter()
    with Runtime(threads=THREADS) if THREADS else Runtime() as rt:
        results = Plugins([cls], runtime=rt).run(trace).results
    dt = time.perf_counter() - t0
    out: Dict[str, float] = {}
    for name in ("total", "n"):
        if name in results:
            out[name] = pa.table(results[name]).to_pydict()["value"][0]
    return dt, out


def measure(
    label: str,
    root: str,
    cases: List[Tuple[str, str, type]],
    runs: int,
    want: Dict[str, Tuple[float, int]],
    files: int,
) -> bool:
    """cases: (name, workload, class). Prints one table; returns the gate verdict."""
    times: Dict[str, List[float]] = {name: [] for name, _, _ in cases}
    for _, _, cls in cases:  # warm-up: builds the index and compiles each plugin
        run_once(cls, root)
    for _ in range(runs):
        for name, workload, cls in cases:
            dt, out = run_once(cls, root)
            times[name].append(dt)
            if name != "floor":
                w_total, w_n = want[workload]
                assert out["n"] == w_n, (name, out, w_n)
                assert abs(out["total"] - w_total) <= 1e-6 * abs(w_total), (name, out, w_total)
    med = {name: statistics.median(ts) for name, ts in times.items()}
    floor = med["floor"]
    print(f"\n{label}: {files} files, median of {runs} runs (seconds)")
    print(f"{'case':<17}{'median':>9}{'net of floor':>14}")
    for name, _, _ in cases:
        print(f"{name:<17}{med[name]:>9.4f}{med[name] - floor:>14.4f}")
    ok = True
    for workload in ("light", "heavy"):
        batch, ref = f"{workload}_batch", f"{workload}_hand_expr"
        net_batch, net_ref = med[batch] - floor, med[ref] - floor
        verdict = net_batch <= 1.05 * net_ref
        ok = ok and verdict
        print(
            f"{workload}: batch net {net_batch:.4f} vs hand_expr net {net_ref:.4f} "
            f"({net_batch / net_ref:.2f}x): {'within 5%' if verdict else 'OVER the 5% bar'}"
        )
    print("all runs:", {k: [round(x, 4) for x in v] for k, v in times.items()})
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--events", type=int, default=1_000_000)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--small-rows", type=int, default=2_000, help="rows per file, small trace")
    ap.add_argument(
        "--threads",
        type=int,
        default=0,
        help="runtime threads (0: all); 1 shows the compute without it hiding under the scan",
    )
    ap.add_argument("--trace-dir", help="reuse (or fill) <dir>/big and <dir>/small")
    ap.add_argument("--gate", action="store_true", help="exit 1 if a batch hook is over the bar")
    args = ap.parse_args()
    global THREADS
    THREADS = args.threads

    tmp = None
    if args.trace_dir:
        base = args.trace_dir
    else:
        tmp = tempfile.TemporaryDirectory(prefix="dftu_jit_batch_bench_")
        base = tmp.name
    traces = {
        "big": (os.path.join(base, "big"), BIG_FILES),
        "small": (os.path.join(base, "small"), args.events // args.small_rows),
    }
    for root, files in traces.values():
        if not (os.path.isdir(root) and any(f.endswith(".pfw.gz") for f in os.listdir(root))):
            write_trace(root, args.events, files)

    tail = _HAND_EXPR_TAIL
    cases: List[Tuple[str, str, type]] = [
        ("floor", "light", Floor),
        ("light_batch", "light", LightBatch),
        ("light_per_event", "light", LightPerEvent),
        ("light_hand", "light", make_hand("HandLight", _HAND_LIGHT)),
        ("light_hand_expr", "light", make_hand("HandExprLight", _HAND_EXPR_LIGHT + tail)),
        ("heavy_batch", "heavy", HeavyBatch),
        ("heavy_hand_expr", "heavy", make_hand("HandExprHeavy", _HAND_EXPR_HEAVY + tail)),
    ]
    ok = True
    print(f"{args.events} events per trace, threads {args.threads or 'all'}")
    for label, (root, files) in traces.items():
        want = expected(args.events, files)
        ok = measure(label, root, cases, args.runs, want, files) and ok
    if args.gate and not ok:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
