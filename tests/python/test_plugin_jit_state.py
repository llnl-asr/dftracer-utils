#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Plain-container scan state for @jit.plugin: annotated ``Dict``, ``DefaultDict``,
``Set``, ``List`` and scalar fields whose merge across worker slices is inferred
from the update code. Each test compiles a plugin, runs it over a multi-file trace
and compares the result with the same update logic run in plain Python."""

import gzip
import os
import shutil
from collections import defaultdict
from typing import DefaultDict, Dict, List, Set, Tuple

import pytest

from dftracer.utils import Runtime, jit
from dftracer.utils.plugins import Plugins

pa = pytest.importorskip("pyarrow")

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)

_FILES = 6
_PER_FILE = 40
_NAMES = ("read", "write", "open")


def _events():
    """Every event as (pid, name, ts, dur, v); ts is unique across files."""
    out = []
    for f in range(_FILES):
        for i in range(_PER_FILE):
            n = f * _PER_FILE + i
            out.append((1 + n % 4, _NAMES[n % 3], 1000 + n, 10 + (n * 7) % 53, -5 - n % 9))
    return out


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("state_trace")
    events = _events()
    for f in range(_FILES):
        with gzip.open(os.path.join(d, f"t{f}.pfw.gz"), "wt", encoding="utf-8") as fh:
            for pid, name, ts, dur, v in events[f * _PER_FILE : (f + 1) * _PER_FILE]:
                fh.write(
                    f'{{"name":"{name}","cat":"POSIX","pid":{pid},"tid":1,"ts":{ts},'
                    f'"dur":{dur},"ph":1,"args":{{"v":{v}}}}}\n'
                )
    return str(d)


def _run(cls, trace, threads):
    with Runtime(threads=threads) as rt:
        return Plugins([cls], runtime=rt).run(trace).results


def _table(results, name):
    return pa.table(results[name]).to_pydict()


def _dict(results, name, keys):
    cols = _table(results, name)
    rows = zip(*[cols[f"k{i}"] for i in range(keys)], cols["value"])
    return {(r[:-1] if keys > 1 else r[0]): r[-1] for r in rows}


def _same(a, b):
    assert a.keys() == b.keys()
    for k in a:
        assert a[k] == pytest.approx(b[k]), k


def _counts_plugin():
    @jit.plugin
    class CountsPlugin:
        by_pid_name: Dict[Tuple[int, str], int] = defaultdict(int)
        total: int = 0
        secs: float = 0.0
        names: Set[str] = set()
        pids: Set[int] = set()
        hi: DefaultDict[int, int] = defaultdict(int)
        lo: Dict[int, int] = {}
        best: Dict[int, int] = {}
        get_sum: Dict[int, int] = {}
        ts_list: List[int] = []

        @jit.each_event
        def step(self, e):
            self.by_pid_name[(e.pid, e.name)] += 1
            self.total += 1
            self.secs += e.dur * 0.5
            self.names.add(e.name)
            self.pids.add(e.pid)
            self.hi[e.pid] = max(self.hi[e.pid], e.arg_i64("v"))
            self.lo[e.pid] = min(self.lo.get(e.pid, e.arg_i64("v")), e.arg_i64("v"))
            self.best[e.pid] = max(e.arg_i64("v"), self.best.get(e.pid, e.arg_i64("v")))
            self.get_sum[e.pid] = self.get_sum.get(e.pid, 0) + e.dur
            self.ts_list.append(e.ts)

    return CountsPlugin


@pytest.mark.parametrize("threads", [1, 4])
def test_each_inferred_merge_matches_plain_python(trace, threads):
    results = _run(_counts_plugin(), trace, threads)
    ev = _events()

    want_counts = defaultdict(int)
    for pid, name, _, _, _ in ev:
        want_counts[(pid, name)] += 1
    _same(_dict(results, "by_pid_name", 2), dict(want_counts))
    assert _table(results, "total")["value"] == [len(ev)]
    assert _table(results, "secs")["value"] == [pytest.approx(sum(d * 0.5 for *_, d, _ in ev))]
    assert sorted(_table(results, "names")["value"][0].split("\x1e")) == sorted(_NAMES)
    assert sorted(_table(results, "pids")["value"][0].split("\x1e")) == ["1", "2", "3", "4"]

    # defaultdict: a missing key reads as 0, so an all-negative key keeps 0.
    hi = defaultdict(int)
    for pid, _, _, _, v in ev:
        hi[pid] = max(hi[pid], v)
    assert set(hi.values()) == {0}
    assert _dict(results, "hi", 1) == dict(hi)

    # plain dict with .get(k, x): the true extrema, negatives included.
    lo, best, total = {}, {}, {}
    for pid, _, _, dur, v in ev:
        lo[pid] = min(lo.get(pid, v), v)
        best[pid] = max(v, best.get(pid, v))
        total[pid] = total.get(pid, 0) + dur
    assert _dict(results, "lo", 1) == lo
    assert _dict(results, "best", 1) == best
    assert _dict(results, "get_sum", 1) == total

    # Documented list order: by event timestamp, whatever the workers saw.
    got = [int(x) for x in _table(results, "ts_list")["value"][0]]
    assert got == sorted(ts for _, _, ts, _, _ in ev)


def test_merge_is_independent_of_worker_count(trace):
    cls = _counts_plugin()
    one = _run(cls, trace, 1)
    many = _run(cls, trace, 8)
    for name in (
        "by_pid_name",
        "total",
        "secs",
        "names",
        "pids",
        "hi",
        "lo",
        "best",
        "get_sum",
        "ts_list",
    ):
        a, b = _table(one, name), _table(many, name)
        assert a.keys() == b.keys(), name
        if name == "secs":
            assert a["value"] == pytest.approx(b["value"])
            continue
        order = lambda t: sorted(zip(*t.values()), key=repr)  # noqa: E731
        assert order(a) == order(b), name


def test_plain_state_and_descriptor_maps_share_one_plugin(trace):
    @jit.plugin
    class Mixed:
        seen: Set[int] = set()
        hits = jit.map(key=(jit.i64,), value=jit.count())
        spread = jit.map(key=(jit.i64,), value=jit.stddev())

        @jit.each_event
        def step(self, e):
            self.seen.add(e.pid)
            self.hits[(e.pid,)] += 1
            self.spread[(e.pid,)].observe(e.dur)

    results = _run(Mixed, trace, 4)
    assert sorted(_table(results, "seen")["value"][0].split("\x1e")) == ["1", "2", "3", "4"]
    assert sum(_dict(results, "hits", 1).values()) == _FILES * _PER_FILE
    assert set(_dict(results, "spread", 1)) == {1, 2, 3, 4}


def test_state_updates_under_a_guard(trace):
    @jit.plugin
    class Guarded:
        reads: int = 0
        by_pid: Dict[int, int] = defaultdict(int)

        @jit.each_event
        def step(self, e):
            if e.name == "read":
                self.reads += 1
                self.by_pid[e.pid] += e.dur

    results = _run(Guarded, trace, 4)
    ev = [x for x in _events() if x[1] == "read"]
    assert _table(results, "reads")["value"] == [len(ev)]
    want = defaultdict(int)
    for pid, _, _, dur, _ in ev:
        want[pid] += dur
    assert _dict(results, "by_pid", 1) == dict(want)


def test_two_key_str_set_and_list_of_names(trace):
    @jit.plugin
    class NameState:
        order: List[str] = []
        per: Dict[Tuple[str, int], float] = defaultdict(float)

        @jit.each_event
        def step(self, e):
            self.order.append(e.name)
            self.per[(e.name, e.pid)] += e.dur

    results = _run(NameState, trace, 4)
    ev = sorted(_events(), key=lambda x: x[2])
    assert _table(results, "order")["value"][0] == [name for _, name, *_ in ev]
    want = defaultdict(float)
    for pid, name, _, dur, _ in ev:
        want[(name, pid)] += dur
    _same(_dict(results, "per", 2), dict(want))


def test_list_ties_sort_by_value_text_under_any_worker_count(tmp_path):
    d = tmp_path / "ties"
    d.mkdir()
    for f in range(6):
        with gzip.open(d / f"t{f}.pfw.gz", "wt", encoding="utf-8") as fh:
            for i in range(20):
                # Three events share each timestamp; their durations differ.
                n = f * 20 + i
                fh.write(
                    f'{{"name":"x","cat":"POSIX","pid":1,"tid":1,"ts":{5000 + n // 3},'
                    f'"dur":{100 + (n * 37) % 91},"ph":1,"args":{{}}}}\n'
                )

    @jit.plugin
    class Ties:
        durs: List[int] = []

        @jit.each_event
        def step(self, e):
            self.durs.append(e.dur)

    ev = []
    for n in range(120):
        ev.append((5000 + n // 3, str(100 + (n * 37) % 91)))
    want = [v for _, v in sorted(ev)]
    for threads in (1, 8):
        got = _table(_run(Ties, str(d), threads), "durs")["value"][0]
        assert got == want
