#!/usr/bin/env python3
"""Cross-worker scalar accumulators for @jit.plugin authoring.

A scalar accumulator is a jit.map with an empty key tuple: it groups nothing, so
it folds to one whole-scan row. Each event contributes with += or .observe(); the
host merges the same-named accumulator across every worker slice and finalizes it
to a one-row frame surfaced under the attribute name. These tests cover the
generated C without a compiler and the end-to-end cross-worker reduction with one.
"""

import gzip
import shutil

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

pa = pytest.importorskip("pyarrow")

_HAS_CXX = bool(shutil.which("c++") or shutil.which("clang++") or shutil.which("g++"))


def _write_trace(path: str, durs) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i, d in enumerate(durs):
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{1 + i % 3},"tid":1,'
                f'"ts":{1000 + i},"dur":{d},"ph":"X","args":{{}}}}\n'
            )


def _scalar(results, name):
    tbl = pa.table(results[name])
    assert tbl.column_names == ["value"]
    assert tbl.num_rows == 1
    return tbl.column("value")[0].as_py()


# --- codegen-only checks (no C++ compiler needed) ---------------------------


def test_jit_scalar_accumulator_emits_keyless_agg():
    @jit.plugin
    class Stats:
        per_pid = jit.map(key=(jit.i64,), value=jit.count())
        total_dur = jit.map(key=(), value=jit.sum())
        n_events = jit.map(key=(), value=jit.count())
        max_dur = jit.map(key=(), value=jit.max())

        @jit.each_event
        def step(self, e):
            self.per_pid[(e.pid,)] += 1
            self.total_dur[()] += e.dur
            self.n_events[()] += 1
            self.max_dur[()].observe(e.dur)

    src = Stats._jit_plugin.source
    rn = Stats._jit_plugin.result_names
    wire = {attr: wid for wid, attr in rn.items()}
    assert "DFTU_SVC_AGG" in src
    # A keyed accumulator passes its key columns; a scalar one passes none.
    assert f'agg_new(host->h, "{wire["per_pid"]}", _keys, 1u,' in src
    assert f'agg_new(host->h, "{wire["total_dur"]}", NULL, 0u,' in src
    assert f'agg_new(host->h, "{wire["n_events"]}", NULL, 0u,' in src
    assert '{DFTU_AGG_MAX, "v0", "value", 0.0, NULL}' in src
    # The per-event contribution lands in that accumulator's row buffer.
    assert "_v0_total_dur[_r] = (double)(dftu_jit_u64(&_col_dur, i));" in src
    assert "_v0_n_events[_r] = (int64_t)(1);" in src


def test_jit_rejects_observe_only_reduction_with_aug():
    with pytest.raises(jit.JitError, match="non-additive"):

        @jit.plugin
        class Bad:
            hi = jit.map(key=(), value=jit.max())

            @jit.each_event
            def step(self, e):
                self.hi[()] += e.dur


def test_jit_rejects_unknown_accumulator():
    with pytest.raises(jit.JitError, match="unknown map"):

        @jit.plugin
        class Bad:
            m = jit.map(key=(), value=jit.count())

            @jit.each_event
            def step(self, e):
                self.nope[()] += 1


# --- end-to-end cross-worker reduction (needs a C++ compiler) ---------------


def _stats_plugin():
    @jit.plugin
    class Stats:
        per_pid = jit.map(key=(jit.i64,), value=jit.count())
        total_dur = jit.map(key=(), value=jit.sum())
        n_events = jit.map(key=(), value=jit.count())
        max_dur = jit.map(key=(), value=jit.max())
        min_dur = jit.map(key=(), value=jit.min())

        @jit.each_event
        def step(self, e):
            self.per_pid[(e.pid,)] += 1
            self.total_dur[()] += e.dur
            self.n_events[()] += 1
            self.max_dur[()].observe(e.dur)
            self.min_dur[()].observe(e.dur)

    return Stats


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_scalar_accumulators_merge_across_scan(tmp_path):
    durs = [10 + i for i in range(200)]
    _write_trace(str(tmp_path / "trace.pfw.gz"), durs)

    plugins = Plugins([_stats_plugin()])
    results = plugins.run(str(tmp_path)).results

    # The per-pid accumulator still materializes normally.
    per_pid = pa.table(results["per_pid"])
    assert per_pid.num_rows == 3
    assert sum(per_pid.column("value").to_pylist()) == len(durs)

    # The scalar accumulators merged across every worker slice to one row each.
    assert _scalar(results, "n_events") == len(durs)
    assert _scalar(results, "total_dur") == pytest.approx(float(sum(durs)))
    assert _scalar(results, "max_dur") == max(durs)
    assert _scalar(results, "min_dur") == min(durs)


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_scalar_only_plugin(tmp_path):
    # A plugin that declares only a scalar accumulator (no keyed result) still
    # emits its merged value.
    @jit.plugin
    class CountOnly:
        n = jit.map(key=(), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.n[()] += 1

    durs = [5 + i for i in range(50)]
    _write_trace(str(tmp_path / "trace.pfw.gz"), durs)

    plugins = Plugins([CountOnly])
    results = plugins.run(str(tmp_path)).results

    assert _scalar(results, "n") == len(durs)


def _shared_total_plugin():
    # Nested in this one function so every call's class shares one qualname
    # ("Shared") - two independently authored plugins that happen to reuse
    # the same class and attribute name, the case the wire id must still
    # catch even though it is now package-qualified.
    @jit.plugin
    class Shared:
        shared_total = jit.map(key=(), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.shared_total[()] += e.dur

    return Shared


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_same_named_accumulator_in_two_plugins_fails_the_build(tmp_path):
    # One accumulator wire id is one result name, so two plugins claiming it
    # is a collision the host refuses at load rather than resolving
    # last-writer-wins after a whole scan.
    a = _shared_total_plugin()
    b = _shared_total_plugin()
    assert a._jit_plugin.result_names == b._jit_plugin.result_names

    with pytest.raises(Exception, match="shared_total"):
        Plugins([a, b])
