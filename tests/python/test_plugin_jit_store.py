#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""``jit.Store[K, V]``: a persistent key-value field lowered to the plugin store
ABI (dftu.svc.store). A plugin writes with explicit keys from compiled code, a
later run reads them back, and Python reads the same namespace as a
``store.Namespace[K, V]``."""

import gzip
import os
import shutil
from pathlib import Path
from typing import Tuple

import pytest

from dftracer.utils import Runtime, jit
from dftracer.utils.jit.plugin import store_plugin_name
from dftracer.utils.plugins import Plugins
from dftracer.utils.store import Store as PyStore

pa = pytest.importorskip("pyarrow")

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)

_N = 12


@pytest.fixture()
def trace(tmp_path):
    d = tmp_path / "trace"
    d.mkdir()
    with gzip.open(d / "t.pfw.gz", "wt", encoding="utf-8") as fh:
        for i in range(_N):
            fh.write(
                f'{{"name":"{"read" if i % 2 else "write"}","cat":"POSIX","pid":{1 + i % 3},'
                f'"tid":1,"ts":{1000 + i},"dur":{10 + i},"ph":1,"args":{{}}}}\n'
            )
    return str(d)


@pytest.fixture()
def idx(tmp_path):
    return str(tmp_path / "idx")


def _run(cls, trace, idx):
    return Plugins([cls]).run(trace, index_dir=idx).results


def _python_store(idx):
    # The index root of an explicit index_dir keeps its store under .dftindex/store.
    return PyStore.open(os.path.join(idx, ".dftindex", "store"))


def _plugin_name(cls):
    return Path(jit.compile_class(cls)).stem


def _scalar_map(results, name):
    cols = pa.table(results[name]).to_pydict()
    return dict(zip(cols["k0"], cols["value"]))


def _counter_plugin(bump):
    # Both bodies share one qualified name, so they are one plugin to the host.
    if bump == 1:

        @jit.plugin
        class RunCounter:
            runs: jit.Store[int, int]
            before = jit.map(key=(jit.i64,), value=jit.min(of=jit.i64))

            @jit.each_event
            def step(self, e):
                self.before[(e.pid,)].observe(self.runs.get(e.pid, 0))
                self.runs.put(e.pid, self.runs.get(e.pid, 0) + 1)

    else:

        @jit.plugin
        class RunCounter:  # type: ignore[no-redef]
            runs: jit.Store[int, int]
            before = jit.map(key=(jit.i64,), value=jit.min(of=jit.i64))

            @jit.each_event
            def step(self, e):
                self.before[(e.pid,)].observe(self.runs.get(e.pid, 0))
                self.runs.put(e.pid, self.runs.get(e.pid, 0) + 10)

    return RunCounter


def test_a_value_put_by_one_run_is_read_by_the_next(trace, idx):
    first = _run(_counter_plugin(1), trace, idx)
    assert _scalar_map(first, "before") == {1: 0, 2: 0, 3: 0}
    second = _run(_counter_plugin(1), trace, idx)
    assert _scalar_map(second, "before") == {1: 1, 2: 1, 3: 1}
    third = _run(_counter_plugin(1), trace, idx)
    assert _scalar_map(third, "before") == {1: 2, 2: 2, 3: 2}


def test_get_reads_the_batch_snapshot_not_this_folds_puts(trace, idx):
    # Within one run every event reads the value as of the batch start, so the
    # four events of a pid all see the previous run's value, not each other's.
    @jit.plugin
    class Snapshot:
        runs: jit.Store[int, int]
        seen = jit.map(key=(jit.i64,), value=jit.max(of=jit.i64))

        @jit.each_event
        def step(self, e):
            self.seen[(e.pid,)].observe(self.runs.get(e.pid, 0))
            self.runs.put(e.pid, self.runs.get(e.pid, 0) + 1)

    assert _scalar_map(_run(Snapshot, trace, idx), "seen") == {1: 0, 2: 0, 3: 0}
    assert _scalar_map(_run(Snapshot, trace, idx), "seen") == {1: 1, 2: 1, 3: 1}


def test_namespace_identity_is_the_plugin_name_and_field_name(trace, idx):
    cls = _counter_plugin(1)
    _run(cls, trace, idx)
    name = _plugin_name(cls)
    identity = jit.provided_names(cls)["before"].rsplit(".", 1)[0]
    assert name == store_plugin_name(identity)
    store = _python_store(idx)
    ns = store.namespace(f"plugin.{name}.runs", 1, durable=True, shared=True)
    snap = store.snapshot()
    assert [snap.get(ns, None, pid) for pid in (1, 2, 3)] == [1, 1, 1]
    assert snap.get(ns, None, 4) is None


def test_the_namespace_survives_a_change_of_the_plugin_body(trace, idx):
    # A new body is a new .so (new digest) of the same plugin name, so the next
    # run still finds the old values.
    _run(_counter_plugin(1), trace, idx)
    changed = _run(_counter_plugin(10), trace, idx)
    assert _scalar_map(changed, "before") == {1: 1, 2: 1, 3: 1}
    again = _run(_counter_plugin(10), trace, idx)
    assert _scalar_map(again, "before") == {1: 11, 2: 11, 3: 11}


def test_two_plugins_keep_separate_data_under_one_field_name(trace, idx):
    @jit.plugin
    class Left:
        runs: jit.Store[int, int]
        got = jit.map(key=(jit.i64,), value=jit.max(of=jit.i64))

        @jit.each_event
        def step(self, e):
            self.got[(e.pid,)].observe(self.runs.get(e.pid, 0))
            self.runs.put(e.pid, 100)

    @jit.plugin
    class Right:
        runs: jit.Store[int, int]
        got = jit.map(key=(jit.i64,), value=jit.max(of=jit.i64))

        @jit.each_event
        def step(self, e):
            self.got[(e.pid,)].observe(self.runs.get(e.pid, 0))
            self.runs.put(e.pid, 7)

    _run(Left, trace, idx)
    assert _scalar_map(_run(Right, trace, idx), "got") == {1: 0, 2: 0, 3: 0}
    assert _scalar_map(_run(Left, trace, idx), "got") == {1: 100, 2: 100, 3: 100}
    assert _scalar_map(_run(Right, trace, idx), "got") == {1: 7, 2: 7, 3: 7}


def test_a_store_only_plugin_with_tuple_and_float_values(trace, idx):
    @jit.plugin
    class OnlyStore:
        weights: jit.Store[Tuple[int, str], float]

        @jit.each_event
        def step(self, e):
            self.weights.put((e.pid, e.name), e.dur * 0.25)

    _run(OnlyStore, trace, idx)
    store = _python_store(idx)
    ns = store.namespace(f"plugin.{_plugin_name(OnlyStore)}.weights", 1, durable=True, shared=True)
    snap = store.snapshot()
    last = {}
    for i in range(_N):
        last[(1 + i % 3, "read" if i % 2 else "write")] = (10 + i) * 0.25
    for key, want in last.items():
        assert snap.get(ns, None, key) == want
    assert snap.get(ns, None, (9, "read")) is None


def test_python_written_values_are_read_by_compiled_code(trace, idx):
    @jit.plugin
    class Reader:
        limits: jit.Store[str, int]
        mine: jit.Store[int, float]
        over = jit.map(key=(jit.i64,), value=jit.max(of=jit.i64))
        half = jit.map(key=(jit.i64,), value=jit.maxf())

        @jit.each_event
        def step(self, e):
            self.over[(e.pid,)].observe(self.limits.get(e.name, 1000))
            self.half[(e.pid,)].observe(self.mine.get(e.pid, 0.5) * 2)

    store = _python_store_for_write(idx, trace)
    name = _plugin_name(Reader)
    limits = store.namespace(f"plugin.{name}.limits", 1, durable=True, shared=True)
    mine = store.namespace(f"plugin.{name}.mine", 1, durable=True, shared=True)
    with store.batch(limits, None) as b:
        b.put("read", 5)
        b.put("write", -300)
    with store.batch(mine, None) as b:
        b.put(2, 1.5)
        b.put(3, -4.0)
    store.flush()
    store.close()

    results = _run(Reader, trace, idx)
    assert _scalar_map(results, "over") == {1: 5, 2: 5, 3: 5}
    assert _scalar_map(results, "half") == {1: 1.0, 2: 3.0, 3: -8.0}


def _python_store_for_write(idx, trace):
    # Build the index first so the store directory exists, then open it.
    @jit.plugin
    class Touch:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    _run(Touch, trace, idx)
    return _python_store(idx)


def test_a_stored_value_of_the_wrong_type_fails_the_run(trace, idx):
    @jit.plugin
    class Typed:
        runs: jit.Store[int, int]
        got = jit.map(key=(jit.i64,), value=jit.max(of=jit.i64))

        @jit.each_event
        def step(self, e):
            self.got[(e.pid,)].observe(self.runs.get(e.pid, 0))

    store = _python_store_for_write(idx, trace)
    ns = store.namespace(f"plugin.{_plugin_name(Typed)}.runs", 1, durable=True, shared=True)
    with store.batch(ns, None) as b:
        b.put(1, 2.5)
    store.flush()
    store.close()
    with pytest.raises(Exception, match="a stored value is not an int"):
        _run(Typed, trace, idx)


def test_a_missing_string_key_fails_the_run(tmp_path, idx):
    # No "name" on the event: the store refuses to key on a missing string.
    d = tmp_path / "noname"
    d.mkdir()
    with gzip.open(d / "t.pfw.gz", "wt", encoding="utf-8") as fh:
        fh.write('{"cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":5,"ph":1,"args":{}}\n')

    @jit.plugin
    class Keyed:
        seen: jit.Store[str, int]

        @jit.each_event
        def step(self, e):
            self.seen.put(e.name, 1)

    with pytest.raises(Exception, match="store seen: a store key is a missing string"):
        _run(Keyed, str(d), idx)


def test_every_worker_slice_keeps_its_puts(tmp_path, idx):
    d = tmp_path / "many"
    d.mkdir()
    for f in range(8):
        with gzip.open(d / f"t{f}.pfw.gz", "wt", encoding="utf-8") as fh:
            for i in range(25):
                pid = f * 100 + i
                fh.write(
                    f'{{"name":"x","cat":"POSIX","pid":{pid},"tid":1,"ts":{1000 + pid},'
                    f'"dur":{pid},"ph":1,"args":{{}}}}\n'
                )

    @jit.plugin
    class Wide:
        by_pid: jit.Store[int, int]

        @jit.each_event
        def step(self, e):
            self.by_pid.put(e.pid, e.dur + 1)

    with Runtime(threads=8) as rt:
        Plugins([Wide], runtime=rt).run(str(d), index_dir=idx)
    store = _python_store(idx)
    ns = store.namespace(f"plugin.{_plugin_name(Wide)}.by_pid", 1, durable=True, shared=True)
    snap = store.snapshot()
    pids = [f * 100 + i for f in range(8) for i in range(25)]
    assert [snap.get(ns, None, pid) for pid in pids] == [pid + 1 for pid in pids]


def test_store_plugin_names_are_injective():
    names = [store_plugin_name(i) for i in ("a/b.c", "a/b_c", "a/b/c", "a.b.c", "a_b/c", "a/b__c")]
    assert len(set(names)) == len(names)
    assert all(all(c in "abcdefghijklmnopqrstuvwxyz0123456789_-" for c in n) for n in names)
