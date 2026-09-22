#!/usr/bin/env python3
"""End-to-end test for @jit.plugin authoring.

A plugin authored in Python is AST-compiled to a native C plugin, built to a
cached .so, and run through Plugins. The result must be identical to the
hand-written name_edges plugin: a {pid, event-name} COUNTER map materialized to
an Arrow table [k0:int64, k1:string, value:int64] whose value column sums to the
event count.
"""

import builtins
import gzip

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

from .jit_common import (
    HAS_CXX,
    kv_of,
    sets_of,
    trace_dir,
    write_dur_trace,
    write_homog_trace,
    write_trace,
)

pa = pytest.importorskip("pyarrow")


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_plan_query_narrows_scan(tmp_path):
    posix_dir = tmp_path / "posix"
    stdio_dir = tmp_path / "stdio"
    posix_dir.mkdir()
    stdio_dir.mkdir()
    n = 40
    write_homog_trace(str(posix_dir / "trace.pfw.gz"), n, "read", "POSIX")
    write_homog_trace(str(stdio_dir / "trace.pfw.gz"), n, "fwrite", "STDIO")
    files = [str(posix_dir / "trace.pfw.gz"), str(stdio_dir / "trace.pfw.gz")]

    @jit.plugin
    class Pruned:
        plan_query = 'cat == "POSIX"'
        hits = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.hits[(e.pid,)] += 1

    @jit.plugin
    class Full:
        hits = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.hits[(e.pid,)] += 1

    pruned_plugins = Plugins([Pruned])
    pruned_run = pruned_plugins.run(files)
    pruned_scanned = pruned_run.stats["events_scanned"]

    full_plugins = Plugins([Full])
    full_run = full_plugins.run(files)
    full_scanned = full_run.stats["events_scanned"]

    assert full_scanned == 2 * n
    assert pruned_scanned == n
    assert pruned_scanned < full_scanned

    pruned_val = pa.table(pruned_run.results["hits"]).column("value").to_numpy(zero_copy_only=False)
    full_val = pa.table(full_run.results["hits"]).column("value").to_numpy(zero_copy_only=False)
    assert int(pruned_val.sum()) == n
    assert int(full_val.sum()) == 2 * n


def _write_mixed_trace(path: str, n: int, cats) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i in range(n):
            cat = cats[i % len(cats)]
            f.write(
                f'{{"name":"read","cat":"{cat}","pid":1,"tid":1,'
                f'"ts":{1000 + i * 100},"dur":{10 + i},"ph":"X","args":{{}}}}\n'
            )


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_string_literal_guard_counts_matching():
    @jit.plugin
    class OnlyPosix:
        hits = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            if e.cat == "POSIX":
                self.hits[(e.pid,)] += 1

    n = 60
    cats = ["POSIX", "STDIO", "OTHER"]
    d = trace_dir(_write_mixed_trace, n, cats)
    expected = sum(1 for i in range(n) if cats[i % len(cats)] == "POSIX")

    plugins = Plugins([OnlyPosix])
    run = plugins.run(d)

    val = pa.table(run.results["hits"]).column("value").to_numpy(zero_copy_only=False)
    assert int(val.sum()) == expected


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_string_literal_ne_guard():
    @jit.plugin
    class NotPosix:
        hits = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            if e.cat != "POSIX":
                self.hits[(e.pid,)] += 1

    n = 60
    cats = ["POSIX", "STDIO", "OTHER"]
    d = trace_dir(_write_mixed_trace, n, cats)
    expected = sum(1 for i in range(n) if cats[i % len(cats)] != "POSIX")

    plugins = Plugins([NotPosix])
    run = plugins.run(d)

    val = pa.table(run.results["hits"]).column("value").to_numpy(zero_copy_only=False)
    assert int(val.sum()) == expected


def test_jit_rejects_string_ordering_op():
    with pytest.raises(jit.JitError, match="unsupported in @jit.each_event"):

        @jit.plugin
        class Ordered:
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if e.cat < "POSIX":
                    self.hits[(e.pid,)] += 1


def test_jit_rejects_string_literal_on_numeric_field():
    with pytest.raises(jit.JitError, match="unsupported in @jit.each_event"):

        @jit.plugin
        class BadField:
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if e.pid == "POSIX":
                    self.hits[(e.pid,)] += 1


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_name_edges_matches_handwritten():
    @jit.plugin
    class NameEdges:
        edges = jit.map(key=(jit.i64, jit.str_), value=jit.count())

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.edges[(e.pid, e.name)] += 1

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([NameEdges])
    run = plugins.run(d)

    assert "edges" in run.results
    tbl = pa.table(run.results["edges"])
    assert tbl.column_names == ["k0", "k1", "value"]
    assert pa.types.is_string(tbl.schema.field("k1").type)

    names = set(tbl.column("k1").to_pylist())
    assert names == {"read"}

    val = tbl.column("value").to_numpy(zero_copy_only=False)
    assert int(val.sum()) == n


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_wide_edges_matches_handwritten():
    @jit.plugin
    class WideEdges:
        edges = jit.map(key=(jit.i64, jit.str_), value=(jit.count(), jit.sum()))

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.edges[(e.pid, e.name)][0] += 1
                self.edges[(e.pid, e.name)][1] += e.dur

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([WideEdges])
    run = plugins.run(d)

    assert "edges" in run.results
    tbl = pa.table(run.results["edges"])
    assert tbl.column_names == ["k0", "k1", "v0", "v1"]
    assert pa.types.is_int64(tbl.schema.field("k0").type)
    assert pa.types.is_string(tbl.schema.field("k1").type)
    assert pa.types.is_int64(tbl.schema.field("v0").type)
    assert pa.types.is_float64(tbl.schema.field("v1").type)

    v0 = tbl.column("v0").to_numpy(zero_copy_only=False)
    v1 = tbl.column("v1").to_numpy(zero_copy_only=False)
    assert int(v0.sum()) == n
    assert float(v1.sum()) == float(sum(10 + i for i in range(n)))


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_dict_value_names_columns():
    @jit.plugin
    class Wide:
        edges = jit.map(
            key=(jit.i64, jit.str_),
            value=dict(count=jit.count(), dur=jit.sum()),
        )

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.edges[(e.pid, e.name)].count += 1
                self.edges[(e.pid, e.name)].dur += e.dur

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([Wide])
    run = plugins.run(d)

    tbl = pa.table(run.results["edges"])
    assert tbl.column_names == ["k0", "k1", "count", "dur"]
    assert pa.types.is_int64(tbl.schema.field("count").type)
    assert pa.types.is_float64(tbl.schema.field("dur").type)
    assert int(tbl.column("count").to_numpy(zero_copy_only=False).sum()) == n
    assert float(tbl.column("dur").to_numpy(zero_copy_only=False).sum()) == float(
        sum(10 + i for i in range(n))
    )


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_dict_value_positional_and_named():
    @jit.plugin
    class Wide:
        edges = jit.map(
            key=(jit.i64, jit.str_),
            value=dict(count=jit.count(), dur=jit.sum()),
        )

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.edges[(e.pid, e.name)][0] += 1
                self.edges[(e.pid, e.name)].dur += e.dur

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([Wide])
    run = plugins.run(d)

    tbl = pa.table(run.results["edges"])
    assert tbl.column_names == ["k0", "k1", "count", "dur"]
    assert int(tbl.column("count").to_numpy(zero_copy_only=False).sum()) == n
    assert float(tbl.column("dur").to_numpy(zero_copy_only=False).sum()) == float(
        sum(10 + i for i in range(n))
    )


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_record_value_form():
    @jit.record
    class Stats:
        count: jit.count
        dur: jit.sum

    @jit.plugin
    class Wide:
        edges = jit.map(key=(jit.i64, jit.str_), value=Stats)

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.edges[(e.pid, e.name)].count += 1
                self.edges[(e.pid, e.name)].dur += e.dur

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([Wide])
    run = plugins.run(d)

    tbl = pa.table(run.results["edges"])
    assert tbl.column_names == ["k0", "k1", "count", "dur"]
    assert int(tbl.column("count").to_numpy(zero_copy_only=False).sum()) == n
    assert float(tbl.column("dur").to_numpy(zero_copy_only=False).sum()) == float(
        sum(10 + i for i in range(n))
    )


def test_jit_rejects_unknown_named_component():
    with pytest.raises(jit.JitError, match="unsupported in @jit.each_event"):

        @jit.plugin
        class Bad:
            edges = jit.map(key=(jit.i64,), value=dict(count=jit.count()))

            @jit.each_event
            def step(self, e):
                self.edges[(e.pid,)].nope += 1


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_single_sum_map():
    @jit.plugin
    class DurSum:
        tot = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.tot[(e.pid,)] += e.dur

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([DurSum])
    run = plugins.run(d)

    tbl = pa.table(run.results["tot"])
    assert tbl.column_names == ["k0", "value"]
    assert pa.types.is_float64(tbl.schema.field("value").type)
    val = tbl.column("value").to_numpy(zero_copy_only=False)
    assert float(val.sum()) == float(sum(10 + i for i in range(n)))


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_arithmetic_doubles_sum():
    @jit.plugin
    class DurSumX2:
        tot = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.tot[(e.pid,)] += e.dur * 2

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([DurSumX2])
    run = plugins.run(d)

    val = pa.table(run.results["tot"]).column("value").to_numpy(zero_copy_only=False)
    assert float(val.sum()) == float(2 * sum(10 + i for i in range(n)))


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_primitive_ilog2_buckets_duration():
    @jit.plugin
    class DurHist:
        hist = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.hist[(jit.ilog2(e.dur),)] += 1

    n = 60
    d = trace_dir(write_trace, n, [1, 2], ["fileA", "fileB"])
    plugins = Plugins([DurHist])
    run = plugins.run(d)

    tbl = pa.table(run.results["hist"])
    got = dict(zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist()))

    expected: dict = {}
    for i in range(n):
        bucket = (10 + i).bit_length() - 1
        expected[bucket] = expected.get(bucket, 0) + 1
    assert got == expected


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_float_primitives_sum():
    @jit.plugin
    class SqrtSum:
        tot = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.tot[(0,)] += jit.sqrt(jit.log2(jit.fma(e.dur, 1.0, 0.0)))

    n = 60
    d = trace_dir(write_trace, n, [1, 2], ["fileA", "fileB"])
    plugins = Plugins([SqrtSum])
    run = plugins.run(d)

    val = pa.table(run.results["tot"]).column("value").to_numpy(zero_copy_only=False)
    import math

    expected = sum(math.sqrt(math.log2(10 + i)) for i in range(n))
    assert float(val.sum()) == pytest.approx(expected, rel=1e-9)


def test_jit_primitive_marker_not_callable_in_python():
    with pytest.raises(jit.JitError, match="plugin primitive"):
        jit.ilog2(1024)
    with pytest.raises(jit.JitError, match="plugin primitive"):
        jit.sqrt(2.0)


def test_jit_cse_hoists_repeated_subexpression():
    @jit.plugin
    class Hoist:
        a = jit.map(key=(jit.i64,), value=jit.count())
        b = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.a[(jit.ilog2(e.dur),)] += 1
            self.b[(jit.ilog2(e.dur),)] += e.dur

    src = Hoist._jit_plugin.source
    # ilog2(e.dur) is used by both maps, so it is computed once into a local.
    assert src.count("dftu_ilog2_u64(") == 1
    assert "_cse0" in src


def test_jit_cse_applies_across_all_op_kinds():
    # The planner is expression-level, not += specific: a shared subexpression
    # is hoisted whether it feeds observe, append, an argmax by=, or quantiles.
    @jit.plugin
    class Mixed:
        avg = jit.map(key=(jit.i64,), value=jit.mean())
        seq = jit.map(key=(jit.i64,), value=jit.list(of=jit.i64))
        worst = jit.map(key=(jit.i64,), value=jit.argmax(of=jit.str_))
        lat = jit.map(key=(jit.i64,), value=jit.quantiles(qs=(0.5, 0.9)))

        @jit.each_event
        def step(self, e):
            self.avg[(jit.mix64(e.hhash),)].observe(e.dur)
            self.seq[(jit.mix64(e.hhash),)].append(e.dur, order_by=e.ts)
            self.worst[(jit.mix64(e.hhash),)].observe(e.name, by=e.dur)
            self.lat[(jit.mix64(e.hhash),)].observe(e.dur)

    src = Mixed._jit_plugin.source
    # mix64(e.hhash) keys all four different op kinds; computed exactly once.
    assert src.count("dftu_mix64(") == 1
    assert "_cse0" in src


def _body(cls):
    s = cls._jit_plugin.source
    i = s.index("for (int64_t i = 0; i < n; ++i)")
    return s[i : s.index("return NULL;", i)]


def test_jit_cse_shares_a_value_expression():
    @jit.plugin
    class V:
        a = jit.map(key=(jit.i64,), value=jit.sum())
        b = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.a[(e.pid,)] += e.dur * e.dur
            self.b[(e.pid,)] += e.dur * e.dur

    dur = "dftu_jit_u64(&_col_dur, i)"
    assert _body(V).count(f"{dur} * {dur}") == 1


def test_jit_cse_shares_multikey_components():
    @jit.plugin
    class N:
        m = jit.map(
            key=(jit.i64, jit.i64),
            value=dict(cnt=jit.count(), dur=jit.sum()),
        )

        @jit.each_event
        def step(self, e):
            self.m[(jit.mix64(e.pid), jit.ilog2(e.dur))].cnt += 1
            self.m[(jit.mix64(e.pid), jit.ilog2(e.dur))].dur += e.dur

    b = _body(N)
    assert b.count("dftu_mix64(") == 1
    assert b.count("dftu_ilog2_u64(") == 1


def test_jit_cse_shares_arg_and_float_prim():
    @jit.plugin
    class A:
        a = jit.map(key=(jit.i64,), value=jit.count())
        d = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.a[(e.arg_i64("rank"),)] += 1
            self.d[(e.arg_i64("rank"),)] += jit.sqrt(e.dur)

    b = _body(A)
    assert b.count("dftu_jit_arg_i64(") == 1  # arg call hoisted


def test_jit_cse_shares_a_guard_test_with_a_statement():
    @jit.plugin
    class G:
        a = jit.map(key=(jit.i64,), value=jit.count())
        c = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.a[(jit.ilog2(e.dur),)] += 1
            if jit.ilog2(e.dur) > 3:
                self.c[(e.pid,)] += 1

    # The guard test shares ilog2(e.dur) with the statement: computed once at the
    # loop top, above the guard.
    b = _body(G)
    assert b.count("dftu_ilog2_u64(") == 1


def test_jit_cse_stays_inside_a_guard():
    @jit.plugin
    class Guarded:
        a = jit.map(key=(jit.i64,), value=jit.count())
        b = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            if e.dur > 0:
                self.a[(jit.ilog2(e.dur),)] += 1
                self.b[(jit.ilog2(e.dur),)] += e.dur

    src = Guarded._jit_plugin.source
    # The shared expression is guarded, so its local must be defined after the
    # guard opens, never hoisted above it.
    assert src.count("dftu_ilog2_u64(") == 1
    guard = src.index("if (")
    cse = src.index("_cse0 =")
    assert cse > guard


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_cse_result_matches_unfused():
    @jit.plugin
    class Buckets:
        n = jit.map(key=(jit.i64,), value=jit.count())
        tot = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.n[(jit.ilog2(e.dur),)] += 1
            self.tot[(jit.ilog2(e.dur),)] += e.dur

    n = 60
    d = trace_dir(write_trace, n, [1, 2], ["fileA"])
    plugins = Plugins([Buckets])
    run = plugins.run(d)

    counts = dict(
        zip(
            pa.table(run.results["n"]).column("k0").to_pylist(),
            pa.table(run.results["n"]).column("value").to_pylist(),
        )
    )
    exp: dict = {}
    for i in range(n):
        b = (10 + i).bit_length() - 1
        exp[b] = exp.get(b, 0) + 1
    assert counts == exp


def test_jit_each_map_gets_its_own_accumulator():
    @jit.plugin
    class Stats:
        n = jit.map(key=(jit.i64,), value=jit.count())
        tot = jit.map(key=(jit.i64,), value=jit.sum())
        avg = jit.map(key=(jit.i64,), value=jit.mean())

        @jit.each_event
        def step(self, e):
            self.n[(e.pid,)] += 1
            self.tot[(e.pid,)] += e.dur
            self.avg[(e.pid,)].observe(e.dur)

    src = Stats._jit_plugin.source
    b = _body(Stats)
    wire = {attr: wid for wid, attr in Stats._jit_plugin.result_names.items()}
    # Each declared map is its own named DFTU_SVC_AGG accumulator, fed by its own
    # per-batch row buffer.
    for name, op in (("n", "DFTU_AGG_SUM"), ("tot", "DFTU_AGG_SUM"), ("avg", "DFTU_AGG_MEAN")):
        assert f'agg_new(host->h, "{wire[name]}"' in src
        assert f"{{{op}, " in src
        assert f"_n_{name}++" in b


def test_jit_different_keys_stay_separate():
    @jit.plugin
    class NoFuse:
        by_pid = jit.map(key=(jit.i64,), value=jit.count())
        by_tid = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.by_pid[(e.pid,)] += 1
            self.by_tid[(e.tid,)] += 1

    b = _body(NoFuse)
    assert "_k0_by_pid[_r] = (int64_t)(dftu_jit_u64(&_col_pid, i));" in b
    assert "_k0_by_tid[_r] = (int64_t)(dftu_jit_u64(&_col_tid, i));" in b


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_same_key_maps_match_separately_computed():
    @jit.plugin
    class Stats:
        n = jit.map(key=(jit.i64,), value=jit.count())
        tot = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.n[(e.pid,)] += 1
            self.tot[(e.pid,)] += e.dur

    m = 60
    d = trace_dir(write_trace, m, [1, 2], ["fileA"])
    plugins = Plugins([Stats])
    run = plugins.run(d)

    # Two same-key maps surface as the two separate declared tables.
    ncount = dict(
        zip(
            pa.table(run.results["n"]).column("k0").to_pylist(),
            pa.table(run.results["n"]).column("value").to_pylist(),
        )
    )
    tot = dict(
        zip(
            pa.table(run.results["tot"]).column("k0").to_pylist(),
            pa.table(run.results["tot"]).column("value").to_pylist(),
        )
    )
    assert sum(ncount.values()) == m
    assert sum(tot.values()) == sum(10 + i for i in range(m))


def test_jit_rejects_out_of_range_integer_literal():
    # 18446744073709551616 == 2**64, one past UINT64_MAX; a bare literal (not a
    # ** power expression, which the grammar rejects earlier).
    with pytest.raises(jit.JitError, match="does not fit in 64 bits"):

        @jit.plugin
        class TooBig:
            m = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                self.m[(18446744073709551616,)] += 1


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_accepts_large_in_range_literal():
    # 1099511627776 == 2**40 fits in 64 bits and routes as an ordinary key.
    @jit.plugin
    class BigKey:
        m = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.m[(1099511627776,)] += 1

    n = 5
    d = trace_dir(write_trace, n, [1], ["fileA"])
    plugins = Plugins([BigKey])
    run = plugins.run(d)
    tbl = pa.table(run.results["m"])
    assert tbl.column("k0").to_pylist() == [1099511627776]
    assert int(tbl.column("value")[0].as_py()) == n


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_raw_body_reproduces_name_edges():
    @jit.plugin
    class RawEdges:
        edges = jit.map(key=(jit.i64, jit.str_), value=jit.count())

        @jit.each_event(raw=True)
        def step(self):
            return """
            if (e->fhash == DFTU_STR_NONE) continue;
            uint32_t _r = _n_edges++;
            _k0_edges[_r] = (int64_t)e->pid;
            _k1_edges[_r] = e->name;
            _v0_edges[_r] = 1;
            """

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([RawEdges])
    run = plugins.run(d)

    tbl = pa.table(run.results["edges"])
    assert tbl.column_names == ["k0", "k1", "value"]
    assert set(tbl.column("k1").to_pylist()) == {"read"}
    assert int(tbl.column("value").to_numpy(zero_copy_only=False).sum()) == n


def test_jit_raw_body_must_return_string():
    with pytest.raises(jit.JitError, match="must return a C\\+\\+ body string"):

        @jit.plugin
        class BadRaw:
            edges = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event(raw=True)
            def step(self):
                return 123


def test_jit_non_raw_return_still_rejects():
    with pytest.raises(jit.JitError, match="unsupported in @jit.each_event"):

        @jit.plugin
        class NotRaw:
            edges = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                return "_v0_edges[_n_edges++] = 1;"


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_quantiles_per_key(tmp_path):
    @jit.plugin
    class Lat:
        lat = jit.map(key=(jit.i64,), value=jit.quantiles(qs=(0.5, 0.9, 0.99)))

        @jit.each_event
        def step(self, e):
            self.lat[(e.pid,)].observe(e.dur)

    # dur = 1..1000 all on pid 1, so quantiles are the percentiles of 1..1000.
    with gzip.open(str(tmp_path / "trace.pfw.gz"), "wt", encoding="utf-8") as f:
        for i in range(1, 1001):
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":1,"tid":1,'
                f'"ts":{1000 + i},"dur":{i},"ph":"X","args":{{}}}}\n'
            )

    plugins = Plugins([Lat])
    run = plugins.run(str(tmp_path))

    tbl = pa.table(run.results["lat"])
    assert tbl.column_names == ["k0", "count", "p50", "p90", "p99"]
    assert int(tbl.column("count")[0].as_py()) == 1000
    # DDSketch is ~1% relative error.
    for col, exp in [("p50", 500), ("p90", 900), ("p99", 990)]:
        got = tbl.column(col)[0].as_py()
        assert abs(got - exp) <= 0.02 * exp, f"{col}={got} vs {exp}"


def test_jit_quantiles_rejected_as_product_component():
    with pytest.raises(jit.JitError, match="top-level only"):

        @jit.plugin
        class Bad:
            m = jit.map(key=(jit.i64,), value=(jit.count(), jit.quantiles()))

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)].observe(e.dur)


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_distinct_count_per_key():
    @jit.plugin
    class OutDeg:
        outdeg = jit.map(key=(jit.i64,), value=jit.distinct())

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.outdeg[(e.pid,)].observe(e.fhash)

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([OutDeg])
    run = plugins.run(d)

    tbl = pa.table(run.results["outdeg"])
    assert tbl.column_names == ["k0", "value"]
    assert pa.types.is_int64(tbl.schema.field("value").type)
    kv = kv_of(tbl)
    assert kv[1] == 3
    assert kv[2] == 3


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_min_max_observe():
    @jit.plugin
    class Lat:
        lo = jit.map(key=(jit.i64,), value=jit.min())
        hi = jit.map(key=(jit.i64,), value=jit.max())

        @jit.each_event
        def step(self, e):
            self.lo[(e.pid,)].observe(e.dur)
            self.hi[(e.pid,)].observe(e.dur)

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([Lat])
    run = plugins.run(d)

    exp_min: dict = {}
    exp_max: dict = {}
    for i in range(n):
        pid = pids[i % len(pids)]
        dur = 10 + i
        exp_min[pid] = builtins.min(exp_min.get(pid, dur), dur)
        exp_max[pid] = builtins.max(exp_max.get(pid, dur), dur)

    lo = kv_of(pa.table(run.results["lo"]))
    hi = kv_of(pa.table(run.results["hi"]))
    assert {k: int(v) for k, v in lo.items()} == exp_min
    assert {k: int(v) for k, v in hi.items()} == exp_max


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_product_mixed_add_and_observe():
    @jit.plugin
    class Mixed:
        m = jit.map(key=(jit.i64,), value=dict(cnt=jit.count(), lo=jit.min()))

        @jit.each_event
        def step(self, e):
            self.m[(e.pid,)].cnt += 1
            self.m[(e.pid,)].lo.observe(e.dur)

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([Mixed])
    run = plugins.run(d)

    tbl = pa.table(run.results["m"])
    assert tbl.column_names == ["k0", "cnt", "lo"]
    assert pa.types.is_int64(tbl.schema.field("cnt").type)
    assert pa.types.is_int64(tbl.schema.field("lo").type)
    assert int(tbl.column("cnt").to_numpy(zero_copy_only=False).sum()) == n

    exp_min: dict = {}
    for i in range(n):
        pid = pids[i % len(pids)]
        dur = 10 + i
        exp_min[pid] = builtins.min(exp_min.get(pid, dur), dur)
    lo = kv_of(tbl, "lo")
    assert {k: int(v) for k, v in lo.items()} == exp_min


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_minf_maxf_observe():
    @jit.plugin
    class LatF:
        lo = jit.map(key=(jit.i64,), value=jit.minf())
        hi = jit.map(key=(jit.i64,), value=jit.maxf())

        @jit.each_event
        def step(self, e):
            self.lo[(e.pid,)].observe(e.dur)
            self.hi[(e.pid,)].observe(e.dur)

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([LatF])
    run = plugins.run(d)

    exp_min: dict = {}
    exp_max: dict = {}
    for i in range(n):
        pid = pids[i % len(pids)]
        dur = 10 + i
        exp_min[pid] = builtins.min(exp_min.get(pid, dur), dur)
        exp_max[pid] = builtins.max(exp_max.get(pid, dur), dur)

    assert pa.types.is_float64(pa.table(run.results["lo"]).schema.field("value").type)
    lo = kv_of(pa.table(run.results["lo"]))
    hi = kv_of(pa.table(run.results["hi"]))
    assert {k: float(v) for k, v in lo.items()} == {k: float(v) for k, v in exp_min.items()}
    assert {k: float(v) for k, v in hi.items()} == {k: float(v) for k, v in exp_max.items()}


def _write_named_trace(path: str, n: int, pids, names) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i in range(n):
            pid = pids[i % len(pids)]
            name = names[i % len(names)]
            f.write(
                f'{{"name":"{name}","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{1000 + i},"dur":{10 + i},"ph":"X","args":{{}}}}\n'
            )


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_set_collects_distinct_names_per_pid():
    @jit.plugin
    class Files:
        files = jit.map(key=(jit.i64,), value=jit.set())

        @jit.each_event
        def step(self, e):
            self.files[(e.pid,)].observe(e.name)

    n = 60
    pids = [1, 2]
    names = ["read", "write", "open"]
    d = trace_dir(_write_named_trace, n, pids, names)
    plugins = Plugins([Files])
    run = plugins.run(d)

    tbl = pa.table(run.results["files"])
    assert tbl.column_names == ["k0", "value"]
    assert pa.types.is_string(tbl.schema.field("value").type)

    by_pid = sets_of(tbl)
    assert by_pid[1] == sorted(names)
    assert by_pid[2] == sorted(names)


def _write_seq_trace(path: str, events) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for pid, name, ts in events:
            f.write(
                f'{{"name":"{name}","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{ts},"dur":1,"ph":"X","args":{{}}}}\n'
            )


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_list_collects_ts_ordered_names_per_pid():
    @jit.plugin
    class Seq:
        seq = jit.map(key=(jit.i64,), value=jit.list())

        @jit.each_event
        def step(self, e):
            self.seq[(e.pid,)].append(e.name, order_by=e.ts)

    # File (insertion) order differs from ts order: pid 1 -> [a,b,c] by ts.
    events = [
        (1, "c", 30),
        (1, "a", 10),
        (1, "b", 20),
        (2, "y", 5),
        (2, "x", 1),
    ]
    d = trace_dir(_write_seq_trace, events)
    plugins = Plugins([Seq])
    run = plugins.run(d)

    tbl = pa.table(run.results["seq"])
    assert tbl.column_names == ["k0", "value"]
    vtype = tbl.schema.field("value").type
    assert pa.types.is_list(vtype)
    assert pa.types.is_string(vtype.value_type)

    by_pid = {k: v for k, v in zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist())}
    assert by_pid[1] == ["a", "b", "c"]
    assert by_pid[2] == ["x", "y"]


def test_jit_rejects_append_without_order_by():
    with pytest.raises(jit.JitError, match=r"order_by"):

        @jit.plugin
        class NoOrder:
            m = jit.map(key=(jit.i64,), value=jit.list())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)].append(e.name)


def test_jit_rejects_add_on_list():
    with pytest.raises(jit.JitError, match=r"\.append"):

        @jit.plugin
        class ListAdd:
            m = jit.map(key=(jit.i64,), value=jit.list())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)] += e.name


def test_jit_rejects_observe_on_list():
    with pytest.raises(jit.JitError, match=r"\.append"):

        @jit.plugin
        class ListObserve:
            m = jit.map(key=(jit.i64,), value=jit.list())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)].observe(e.name)


def test_jit_rejects_add_on_set():
    with pytest.raises(jit.JitError, match=r"\.observe"):

        @jit.plugin
        class SetAdd:
            m = jit.map(key=(jit.i64,), value=jit.set())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)] += e.name


def test_jit_rejects_add_on_distinct():
    with pytest.raises(jit.JitError, match=r"\.observe"):

        @jit.plugin
        class DistAdd:
            m = jit.map(key=(jit.i64,), value=jit.distinct())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)] += e.fhash


def test_jit_rejects_observe_float_into_u64():
    with pytest.raises(jit.JitError, match="u64 monoid"):

        @jit.plugin
        class BadF:
            m = jit.map(key=(jit.i64,), value=jit.min())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)].observe(e.dur * 2.0)


def test_jit_rejects_min_monoid():
    with pytest.raises(jit.JitError, match=r"\.observe"):

        @jit.plugin
        class MinMap:
            m = jit.map(key=(jit.i64,), value=jit.min())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)] += e.dur


def test_jit_rejects_arithmetic_in_key():
    with pytest.raises(jit.JitError, match="unsupported in @jit.each_event"):

        @jit.plugin
        class KeyMath:
            m = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid + 1,)] += 1


def test_jit_rejects_bare_add_on_product():
    with pytest.raises(jit.JitError, match="unsupported in @jit.each_event"):

        @jit.plugin
        class Prod:
            m = jit.map(key=(jit.i64,), value=(jit.count(), jit.sum()))

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)] += 1


def test_jit_rejects_for_loop():
    with pytest.raises(jit.JitError, match="unsupported in @jit.each_event"):

        @jit.plugin
        class Loopy:
            edges = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                for _ in range(3):
                    self.edges[(e.pid,)] += 1


def test_jit_rejects_float_arithmetic():
    with pytest.raises(jit.JitError, match="unsupported in @jit.each_event"):

        @jit.plugin
        class Floaty:
            edges = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                self.edges[(e.pid,)] += e.dur * 2.0


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_set_i64_collects_distinct_durs_sorted():
    @jit.plugin
    class Durs:
        durs = jit.map(key=(jit.i64,), value=jit.set(of=jit.i64))

        @jit.each_event
        def step(self, e):
            self.durs[(e.pid,)].observe(e.dur)

    events = [
        (1, 20, 10),
        (1, 5, 20),
        (1, 20, 30),
        (1, 8, 40),
        (2, 3, 1),
        (2, 7, 5),
    ]
    d = trace_dir(write_dur_trace, events)
    plugins = Plugins([Durs])
    run = plugins.run(d)

    tbl = pa.table(run.results["durs"])
    assert tbl.column_names == ["k0", "value"]
    assert pa.types.is_string(tbl.schema.field("value").type)

    # The distinct durs as reprs, sorted as strings.
    by_pid = sets_of(tbl)
    assert by_pid[1] == ["20", "5", "8"]
    assert by_pid[2] == ["3", "7"]


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_list_i64_collects_ts_ordered_durs():
    @jit.plugin
    class Durs:
        durs = jit.map(key=(jit.i64,), value=jit.list(of=jit.i64))

        @jit.each_event
        def step(self, e):
            self.durs[(e.pid,)].append(e.dur, order_by=e.ts)

    events = [
        (1, 20, 10),
        (1, 5, 20),
        (1, 20, 30),
        (1, 8, 40),
        (2, 3, 1),
        (2, 7, 5),
    ]
    d = trace_dir(write_dur_trace, events)
    plugins = Plugins([Durs])
    run = plugins.run(d)

    tbl = pa.table(run.results["durs"])
    assert tbl.column_names == ["k0", "value"]
    vtype = tbl.schema.field("value").type
    assert pa.types.is_list(vtype)
    assert pa.types.is_string(vtype.value_type)

    by_pid = {k: v for k, v in zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist())}
    assert by_pid[1] == ["20", "5", "20", "8"]
    assert by_pid[2] == ["3", "7"]


def test_jit_rejects_str_field_into_i64_set():
    with pytest.raises(jit.JitError, match=r"int64"):

        @jit.plugin
        class Bad:
            m = jit.map(key=(jit.i64,), value=jit.set(of=jit.i64))

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)].observe(e.name)


def test_jit_rejects_i64_expr_into_str_set():
    with pytest.raises(jit.JitError, match=r"string field"):

        @jit.plugin
        class Bad:
            m = jit.map(key=(jit.i64,), value=jit.set())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)].observe(e.dur)
