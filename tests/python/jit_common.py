# SPDX-License-Identifier: MIT
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
                f'"ts":{1000 + i},"dur":{10 + i},"ph":1,'
                f'"args":{{"fhash":"{fhash}","ret":{i}}}}}\n'
            )


def write_homog_trace(path: str, n: int, name: str, cat: str) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i in range(n):
            f.write(
                f'{{"name":"{name}","cat":"{cat}","pid":1,"tid":1,'
                f'"ts":{1000 + i * 100},"dur":{10 + i},"ph":1,"args":{{}}}}\n'
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
                f'"ts":{ts},"dur":{dur},"ph":1,"args":{{}}}}\n'
            )


def write_pid_tid_trace(path: str, rows) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i, (pid, tid) in enumerate(rows):
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":{tid},'
                f'"ts":{1000 + i},"dur":1,"ph":1,"args":{{}}}}\n'
            )


def load_module(directory, name: str, source: str):
    """Import ``source`` as module ``name`` from a file in ``directory``, so a
    compiled body can carry a literal path chosen by the test (the compiler reads
    the source of the class from its file)."""
    import importlib.util
    import textwrap

    path = os.path.join(str(directory), f"{name}.py")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(textwrap.dedent(source))
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def plugin_stats(plugins_so: str):
    """The counters a compiled plugin that uses tasks or files keeps (task frames
    started, released, files opened, closed, task frames completed, strings interned, cancels seen at an await). The library
    is the one the host loaded, so call it while the Plugins object is alive."""
    import ctypes

    lib = ctypes.CDLL(plugins_so)
    lib.dftu_jit_stat.restype = ctypes.c_longlong
    lib.dftu_jit_stat.argtypes = [ctypes.c_int]
    names = ("started", "released", "opened", "closed", "completed", "interned", "cancelled")
    return {n: lib.dftu_jit_stat(i) for i, n in enumerate(names)}


def assert_located(err) -> None:
    """A compile error names the file, the 1-based line and column and the source
    line of the construct, and ``str(err)`` has the one stable form."""
    assert err.file is not None and err.line is not None and err.col is not None
    assert err.line >= 1 and err.col >= 1 and err.source is not None
    assert str(err) == f"{err.file}:{err.line}:{err.col}: {err.reason}\n{err.source}"


def compile_error(match=None):
    """``pytest.raises(JitError, match=...)`` that also checks the location."""
    import contextlib

    import pytest

    from dftracer.utils.jit import JitError

    @contextlib.contextmanager
    def check():
        with pytest.raises(JitError, match=match) as info:
            yield info
        assert_located(info.value)

    return check()
