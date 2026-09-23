"""Trace writers and result readers shared by the jit plugin tests."""

import gzip
import os
import shutil
import tempfile

# A set-union aggregate finalizes to one String cell per group: the distinct
# reprs, sorted, joined by this separator.
SET_SEP = "\x1e"

HAS_CXX = bool(shutil.which("c++") or shutil.which("clang++") or shutil.which("g++"))

_TRACE_DIRS: dict = {}


def trace_dir(write, *args):
    """A directory holding one written trace, shared by every test that asks
    for the same one.

    The tests only read it, and a fresh directory costs a full index build on
    first use: 0.6 s natively, two orders of magnitude more under Valgrind,
    against 0.01 s to reuse one.
    """
    key = (write.__name__,) + tuple(repr(a) for a in args)
    d = _TRACE_DIRS.get(key)
    if d is None:
        d = tempfile.mkdtemp(prefix="dftu_jit_trace_")
        write(os.path.join(d, "trace.pfw.gz"), *args)
        _TRACE_DIRS[key] = d
    return d


def write_trace(path: str, n: int, pids, files) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i in range(n):
            pid = pids[i % len(pids)]
            fhash = files[i % len(files)]
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{1000 + i},"dur":{10 + i},"ph":"X",'
                f'"args":{{"fhash":"{fhash}","ret":{i}}}}}\n'
            )


def write_homog_trace(path: str, n: int, name: str, cat: str) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i in range(n):
            f.write(
                f'{{"name":"{name}","cat":"{cat}","pid":1,"tid":1,'
                f'"ts":{1000 + i * 100},"dur":{10 + i},"ph":"X","args":{{}}}}\n'
            )


def kv_of(tbl, value_col="value"):
    keys = tbl.column("k0").to_pylist()
    vals = tbl.column(value_col).to_numpy(zero_copy_only=False)
    return {k: v for k, v in zip(keys, vals)}


def sets_of(tbl):
    return {
        k: v.split(SET_SEP)
        for k, v in zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist())
    }


def write_dur_trace(path: str, events) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for pid, dur, ts in events:
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{ts},"dur":{dur},"ph":"X","args":{{}}}}\n'
            )


def write_pid_tid_trace(path: str, rows) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i, (pid, tid) in enumerate(rows):
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":{tid},'
                f'"ts":{1000 + i},"dur":1,"ph":"X","args":{{}}}}\n'
            )
