#!/usr/bin/env python3
"""Wall-clock of the expression string methods: the native expression
(SIMD kernels) against the same kernels with the byte-at-a-time reference
forced (DFTRACER_UTILS_STRING_SCALAR), the eager ``Series.str`` call and
pandas ``.str``, on short strings (``--rows``, default 5 million, 4 to 24
bytes) and on long ones (``--long-rows`` of about ``--long-len`` bytes).

Besides time it reports memory: the peak physical footprint above the idle
process while the call runs (macOS ``phys_footprint``, else USS) for the
native expression, the eager call and pandas, so an overhead that holds a
copy of the data shows. A third section runs the predicates over long
strings that are all good (every string must be read to the end) next to the
random ones (a bad byte early); a fourth times a numeric expression
``with_columns(z=col(x)+1)`` against the eager add and pandas. The run
prints the machine, the load average and, when the C++ test binary is built,
the Highway target the kernels dispatch to. Set ``DFTRACER_UTILS_STRING_PREDICATE``
to ``bits`` or ``rows`` to force one predicate strategy.

Each method is timed ``--repeat`` times and the best is reported. The native
and forced-scalar runs are separate processes (the switch is read once at
start). ``--check`` also compares, on the first ``--check-rows`` rows, the
native expression with the eager call and with pandas, and exits non-zero on
any difference.

    python benchmarks/string_methods_bench.py
    python benchmarks/string_methods_bench.py --rows 1_000_000 --check
    python benchmarks/string_methods_bench.py --memory
"""

from __future__ import annotations

import argparse
import ctypes
import gc
import json
import os
import platform
import subprocess
import sys
import tempfile
import time
from typing import Callable, Dict, List, Tuple

import numpy as np
import pyarrow as pa

from dftracer.utils import DataFrame, col

ALPHABET = np.frombuffer(
    b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -,._", dtype=np.uint8
)


LOWER = np.frombuffer(b"abcdefghijklmnopqrstuvwxyz", dtype=np.uint8)
PREDICATES = {"isalnum", "isalpha", "islower"}  # all-good lowercase input is true for these


class _RusageV2(ctypes.Structure):
    # struct rusage_info_v2 in full (the kernel writes all of it)
    _fields_ = [("ri_uuid", ctypes.c_uint8 * 16)] + [
        (n, ctypes.c_uint64)
        for n in (
            "ri_user_time ri_system_time ri_pkg_idle_wkups ri_interrupt_wkups ri_pageins "
            "ri_wired_size ri_resident_size ri_phys_footprint ri_proc_start_abstime "
            "ri_proc_exit_abstime ri_child_user_time ri_child_system_time "
            "ri_child_pkg_idle_wkups ri_child_interrupt_wkups ri_child_pageins "
            "ri_child_elapsed_abstime ri_diskio_bytesread ri_diskio_byteswritten"
        ).split()
    ]


def footprint(pid: int = 0) -> int:
    """Physical memory of a process in bytes (macOS phys_footprint, else USS)."""
    pid = pid or os.getpid()
    if sys.platform == "darwin":
        libproc = ctypes.CDLL("/usr/lib/libproc.dylib")
        libproc.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
        info = _RusageV2()
        libproc.proc_pid_rusage(pid, 2, ctypes.byref(info))
        return int(info.ri_phys_footprint)
    import psutil

    return int(psutil.Process(pid).memory_full_info().uss)


def sample(pid: int) -> None:
    """Sampler process: print each new maximum of the footprint of `pid`."""
    top = 0
    while True:
        v = footprint(pid)
        if v > top:
            top = v
            print(top, flush=True)
        time.sleep(0.0005)


def relieve() -> None:
    """Give freed memory back to the system."""
    gc.collect()
    try:
        lib = ctypes.CDLL(None) if sys.platform == "darwin" else ctypes.CDLL("libc.so.6")
        if sys.platform == "darwin":
            lib.malloc_zone_pressure_relief.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            lib.malloc_zone_pressure_relief(None, 0)
        else:
            lib.malloc_trim(0)
    except Exception:
        pass


def peak_extra(fn: Callable[[], object]) -> float:
    """Peak footprint above the idle process while fn runs, in MB. A separate
    process samples it (a native call holds the GIL, so a thread would not run);
    the result is kept alive until the peak is read, then dropped."""
    relieve()
    proc = subprocess.Popen(
        [sys.executable, __file__, "--sample", str(os.getpid())],
        stdout=subprocess.PIPE,
        text=True,
    )
    assert proc.stdout is not None
    base = int(proc.stdout.readline())  # the idle footprint
    out = fn()
    time.sleep(0.005)
    proc.terminate()
    tail = proc.communicate()[0].split()
    peak = max([base] + [int(v) for v in tail])
    del out
    relieve()
    return (peak - base) / 1e6


def make_strings(rows: int, lo: int, hi: int, seed: int, alphabet=ALPHABET) -> pa.Array:
    rng = np.random.default_rng(seed)
    width = hi
    chars = alphabet[rng.integers(0, len(alphabet), size=(rows, width))]
    lengths = rng.integers(lo, hi + 1, size=rows)
    # Zero the bytes past each length, then read the rows as fixed-width byte strings.
    chars[np.arange(width)[None, :] >= lengths[:, None]] = 0
    fixed = chars.view(f"S{width}").ravel()
    return pa.array(fixed.astype(f"U{width}"), type=pa.string())


# name -> (expression, eager Series.str call, pandas .str call)
def methods() -> Dict[str, Tuple[Callable, Callable, Callable]]:
    c = col("s")
    return {
        "capitalize": (c.capitalize(), lambda s: s.str.capitalize(), lambda p: p.str.capitalize()),
        "title": (c.title(), lambda s: s.str.title(), lambda p: p.str.title()),
        "swapcase": (c.swapcase(), lambda s: s.str.swapcase(), lambda p: p.str.swapcase()),
        "casefold": (c.casefold(), lambda s: s.str.casefold(), lambda p: p.str.casefold()),
        "isalnum": (c.isalnum(), lambda s: s.str.isalnum(), lambda p: p.str.isalnum()),
        "isalpha": (c.isalpha(), lambda s: s.str.isalpha(), lambda p: p.str.isalpha()),
        "isdigit": (c.isdigit(), lambda s: s.str.isdigit(), lambda p: p.str.isdigit()),
        "isspace": (c.isspace(), lambda s: s.str.isspace(), lambda p: p.str.isspace()),
        "islower": (c.islower(), lambda s: s.str.islower(), lambda p: p.str.islower()),
        "isupper": (c.isupper(), lambda s: s.str.isupper(), lambda p: p.str.isupper()),
        "istitle": (c.istitle(), lambda s: s.str.istitle(), lambda p: p.str.istitle()),
        "zfill": (c.zfill(26), lambda s: s.str.zfill(26), lambda p: p.str.zfill(26)),
        "pad_start": (
            c.pad_start(26, "*"),
            lambda s: s.str.pad_start(26, "*"),
            lambda p: p.str.pad(26, "left", "*"),
        ),
        "center": (
            c.center(27, "."),
            lambda s: s.str.center(27, "."),
            lambda p: p.str.center(27, "."),
        ),
        "removeprefix": (
            c.removeprefix("ab"),
            lambda s: s.str.removeprefix("ab"),
            lambda p: p.str.removeprefix("ab"),
        ),
        "removesuffix": (
            c.removesuffix("9"),
            lambda s: s.str.removesuffix("9"),
            lambda p: p.str.removesuffix("9"),
        ),
        "repeat": (c.repeat(2), lambda s: s.str.repeat(2), lambda p: p.str.repeat(2)),
        "slice_replace": (
            c.slice_replace(2, 5, "##"),
            lambda s: s.str.slice_replace(2, 5, "##"),
            lambda p: p.str.slice_replace(2, 5, "##"),
        ),
        "get": (c.get(3), lambda s: s.str.get(3), lambda p: p.str.get(3)),
        "rfind": (c.rfind("a"), lambda s: s.str.rfind("a"), lambda p: p.str.rfind("a")),
        "split": (c.split("-"), lambda s: s.str.split("-"), lambda p: p.str.split("-")),
        "partition": (
            c.partition("-"),
            lambda s: s.str.partition("-", expand=False),
            lambda p: p.str.partition("-", expand=False),
        ),
        "rpartition": (
            c.rpartition("-"),
            lambda s: s.str.rpartition("-", expand=False),
            lambda p: p.str.rpartition("-", expand=False),
        ),
        "join": (
            c.split("-").join("+"),
            lambda s: s.str.split("-").str.join("+"),
            lambda p: p.str.split("-").str.join("+"),
        ),
        "extract": (
            c.extract("([a-z]+)", 1),
            lambda s: s.str.extract("([a-z]+)", 1),
            lambda p: p.str.extract("([a-z]+)", expand=False),
        ),
        "findall": (
            c.findall("[0-9]+"),
            lambda s: s.str.findall("[0-9]+"),
            lambda p: p.str.findall("[0-9]+"),
        ),
        "match": (
            c.match("[a-z]+"),
            lambda s: s.str.match("[a-z]+"),
            lambda p: p.str.match("[a-z]+"),
        ),
        "cat": (c.cat(col("t"), sep="/"), None, lambda p: p.str.cat(p, sep="/")),
    }


def best(fn: Callable[[], object], repeat: int) -> float:
    out = float("inf")
    for _ in range(repeat):
        t0 = time.perf_counter()
        fn()
        out = min(out, time.perf_counter() - t0)
    return out


SECTIONS = (
    # label, rows attr, lo, hi attr, alphabet, only these methods
    ("short", "rows", 4, 24, ALPHABET, None),
    ("long", "long_rows", None, None, ALPHABET, None),
    ("long-good", "long_rows", None, None, LOWER, PREDICATES),
)


def section_args(args: argparse.Namespace, label: str):
    for lab, rows_attr, lo, hi, alphabet, only in SECTIONS:
        if lab == label:
            if lo is None:
                lo, hi = args.long_len // 2, args.long_len
            return getattr(args, rows_attr), lo, hi, alphabet, only
    raise KeyError(label)


def child(args: argparse.Namespace) -> None:
    """One run in this process: native expression and eager timings and peaks."""
    result: Dict[str, Dict[str, float]] = {}
    for label, *_ in SECTIONS:
        rows, lo, hi, alphabet, only = section_args(args, label)
        table = pa.table({"s": make_strings(rows, lo, hi, 1, alphabet)})
        table = table.append_column("t", table["s"])
        df = DataFrame.from_arrow(table)
        s = df["s"]
        result[label] = {}
        for name, (expr, eager, _) in methods().items():
            if only is not None and name not in only:
                continue
            if label != "short" and name in {"extract", "findall", "match"} and rows > 200_000:
                continue
            result[label][name + ".expr"] = best(lambda: df.with_columns(o=expr), args.repeat)
            if eager is not None:
                result[label][name + ".eager"] = best(lambda: eager(s), args.repeat)
    # numeric micro-measurement: a constant add through the expression engine
    rng = np.random.default_rng(2)
    x = rng.integers(0, 1_000_000, size=args.rows)
    nd = DataFrame.from_arrow(pa.table({"x": pa.array(x)}))
    nx = nd["x"]
    result["numeric"] = {
        "expr": best(lambda: nd.with_columns(z=col("x") + 1), args.repeat),
        "eager": best(lambda: nx + 1, args.repeat),
    }
    json.dump(result, sys.stdout)


def mem_one(args: argparse.Namespace, spec: str) -> None:
    """A fresh process measures one call's peak. The column is read from a
    memory-mapped Arrow file so that building it leaves no freed pages for the
    call to reuse (which would show as 0), and a process that already ran the
    call keeps its freed pages, so each call gets its own process."""
    name, path, file = spec.split(",")
    table = pa.ipc.open_file(pa.memory_map(file)).read_all()
    expr, eager, pand = methods()[name]
    if path == "pandas":
        pser = table["s"].to_pandas()
        fn = lambda: pand(pser)  # noqa: E731
    else:
        df = DataFrame.from_arrow(table)
        s = df["s"]
        fn = (lambda: df.with_columns(o=expr)) if path == "expr" else (lambda: eager(s))  # noqa: E731
    del table
    print(peak_extra(fn))


def memory_mb(args: argparse.Namespace, name: str, path: str, file: str) -> float:
    cmd = [sys.executable, __file__, f"--mem-one={name},{path},{file}"]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout.split()
    return float(out[-1]) if out else float("nan")


def target_line() -> str:
    exe = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "build/build-tests/tests/dataframe/test_string_simd",
    )
    if os.path.exists(exe):
        out = subprocess.run(
            [exe, "-tc=reports the dispatched target"], capture_output=True, text=True
        ).stdout
        for line in out.splitlines():
            if "Highway target" in line:
                return line.strip()
    return "Highway target: build the tests to print it (test_string_simd)"


def parent(args: argparse.Namespace) -> int:
    print(f"machine {platform.machine()} {platform.platform()}")
    print(f"load average {', '.join(f'{v:.2f}' for v in os.getloadavg())} (1, 5, 15 min)")
    print(target_line())
    print(
        "predicate strategy:",
        os.environ.get("DFTRACER_UTILS_STRING_PREDICATE", "auto (by mean length)"),
    )
    runs = {}
    for mode in ("simd", "scalar"):
        env = dict(os.environ)
        env.pop("DFTRACER_UTILS_STRING_SCALAR", None)
        if mode == "scalar":
            env["DFTRACER_UTILS_STRING_SCALAR"] = "1"
        cmd = [sys.executable, __file__, "--child"] + [
            f"--{k.replace('_', '-')}={v}"
            for k, v in vars(args).items()
            if k in {"rows", "long_rows", "long_len", "repeat"}
        ]
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
        if proc.returncode != 0:
            print(proc.stderr, file=sys.stderr)
            return proc.returncode
        runs[mode] = json.loads(proc.stdout)

    failures: List[str] = []
    for label, *_ in SECTIONS:
        rows, lo, hi, alphabet, only = section_args(args, label)
        table = pa.table({"s": make_strings(rows, lo, hi, 1, alphabet)})
        table = table.append_column("t", table["s"])
        pser = table["s"].to_pandas()
        print(
            f"\n{label} strings: {rows:,} rows, {lo}-{hi} bytes   (seconds, best of {args.repeat}; "
            "ratio = forced-scalar time over native time; pd/x = pandas time over x)"
        )
        mem = args.memory and label == "short"
        if mem:
            mem_file = os.path.join(tempfile.mkdtemp(prefix="strbench"), "s.arrow")
            with pa.OSFile(mem_file, "wb") as sink:
                with pa.ipc.new_file(sink, table.schema) as w:
                    w.write_table(table)
        print(
            f"{'method':14}{'expr':>8}{'expr-sc':>9}{'ratio':>7}  {'eager':>8}{'eager-sc':>9}{'ratio':>7}"
            f"  {'pandas':>8}{'pd/expr':>8}{'pd/eager':>9}"
            + (f"  {'expr MB':>8}{'eager MB':>9}{'pandas MB':>10}" if mem else "")
        )
        for name, (expr, eager, pand) in methods().items():
            key = name + ".expr"
            if key not in runs["simd"][label]:
                continue
            native = runs["simd"][label][key]
            scalar = runs["scalar"][label][key]
            e_simd = runs["simd"][label].get(name + ".eager")
            e_scal = runs["scalar"][label].get(name + ".eager")
            t_pd = best(lambda: pand(pser), 1 if rows >= 1_000_000 else args.repeat)
            if e_simd is not None and e_scal is not None:
                eager_cols = f"{e_simd:8.4f}{e_scal:9.4f}{e_scal / e_simd:6.1f}x"
                eager_vs = f"{t_pd / e_simd:8.1f}x"
            else:
                eager_cols = f"{'-':>8}{'-':>9}{'-':>7}"
                eager_vs = f"{'-':>9}"
            mem_cols = ""
            if mem:
                m_e = memory_mb(args, name, "expr", mem_file)
                m_g = (
                    memory_mb(args, name, "eager", mem_file) if eager is not None else float("nan")
                )
                m_p = memory_mb(args, name, "pandas", mem_file)
                mem_cols = f"  {m_e:8.0f}{m_g:9.0f}{m_p:10.0f}"
            print(
                f"{name:14}{native:8.4f}{scalar:9.4f}{scalar / native:6.1f}x  {eager_cols}"
                f"  {t_pd:8.4f}{t_pd / native:7.1f}x{eager_vs}{mem_cols}"
            )
            if args.check and label == "short" and name in methods():
                n = args.check_rows
                small = DataFrame.from_arrow(table.slice(0, n))
                got = small.with_columns(o=expr)["o"].to_list()
                if eager is not None and got != eager(small["s"]).to_list():
                    failures.append(f"{name}: expression differs from eager")
                want = pand(pser.iloc[:n])
                want = want.tolist() if hasattr(want, "tolist") else list(want)
                want = [list(v) if isinstance(v, tuple) else v for v in want]
                want = [None if isinstance(v, float) and v != v else v for v in want]
                if name == "cat":
                    want = [a + "/" + a for a in pser.iloc[:n].tolist()]
                if got != want:
                    failures.append(f"{name}: expression differs from pandas")
    num = runs["simd"]["numeric"]
    xs = np.random.default_rng(2).integers(0, 1_000_000, size=args.rows)
    import pandas as pd

    px = pd.Series(xs)
    t_pd = best(lambda: px + 1, args.repeat)
    print(
        f"\nnumeric: {args.rows:,} int64 rows, z = x + 1 (seconds)   expression "
        f"{num['expr']:.4f}   eager {num['eager']:.4f}   pandas {t_pd:.4f}   "
        f"expression/eager {num['expr'] / num['eager']:.2f}x"
    )
    if args.check:
        print("\ncheck:", "all equal" if not failures else "; ".join(failures))
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=lambda x: int(x.replace("_", "")), default=5_000_000)
    ap.add_argument("--long-rows", type=lambda x: int(x.replace("_", "")), default=100_000)
    ap.add_argument("--long-len", type=int, default=2000)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--memory", action="store_true", help="add peak-memory columns (short strings)")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--check-rows", type=int, default=200_000)
    ap.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--sample", type=int, help=argparse.SUPPRESS)
    ap.add_argument("--mem-one", help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.mem_one:
        mem_one(args, args.mem_one)
        return 0
    if args.sample:
        sample(args.sample)
        return 0
    if args.child:
        child(args)
        return 0
    return parent(args)


if __name__ == "__main__":
    sys.exit(main())
