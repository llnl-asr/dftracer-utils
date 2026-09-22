#!/usr/bin/env python3
"""Key shapes of a @jit.plugin: multi-key, integer, float and arg keys, two-key
maps, moments, argmax, top-k and sampling."""

import builtins
import gzip
import statistics

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

from .jit_common import (
    HAS_CXX,
    kv_of,
    sets_of,
    trace_dir,
    write_dur_trace,
    write_pid_tid_trace,
    write_trace,
)

pa = pytest.importorskip("pyarrow")


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_multikey_counts_group_by_both_columns():
    @jit.plugin
    class Two:
        asc = jit.map(key=(jit.i64, jit.i64), value=jit.count())
        plain = jit.map(key=(jit.i64, jit.i64), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.asc[(e.pid, e.tid)] += 1
            self.plain[(e.pid, e.tid)] += 1

    # File order is deliberately not ascending by (pid, tid).
    rows = [(3, 7), (1, 2), (3, 1), (1, 9), (2, 5), (1, 2), (3, 7), (2, 5)]
    d = trace_dir(write_pid_tid_trace, rows)
    plugins = Plugins([Two])
    run = plugins.run(d)

    asc = pa.table(run.results["asc"])
    plain = pa.table(run.results["plain"])

    asc_keys = list(zip(asc.column("k0").to_pylist(), asc.column("k1").to_pylist()))
    asc_vals = dict(zip(asc_keys, asc.column("value").to_pylist()))
    plain_keys = list(zip(plain.column("k0").to_pylist(), plain.column("k1").to_pylist()))
    plain_vals = dict(zip(plain_keys, plain.column("value").to_pylist()))

    # Both maps group the same (pid, tid) pairs to the same counts.
    assert asc_vals == plain_vals
    assert sorted(plain_keys) == sorted(asc_keys)
    assert asc_vals == {(3, 7): 2, (1, 2): 2, (3, 1): 1, (1, 9): 1, (2, 5): 2}


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_integer_key_columns():
    @jit.plugin
    class Typed:
        m = jit.map(key=(jit.u16, jit.i32), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.m[(e.pid, e.tid)] += 1

    rows = [(1000, 7), (1000, 7), (2000, 9), (2000, 9), (2000, 9)]
    d = trace_dir(write_pid_tid_trace, rows)
    plugins = Plugins([Typed])
    run = plugins.run(d)

    tbl = pa.table(run.results["m"])
    assert tbl.column_names == ["k0", "k1", "value"]
    assert pa.types.is_int64(tbl.schema.field("k0").type)
    assert pa.types.is_int64(tbl.schema.field("k1").type)

    kv = {
        (k0, k1): v
        for k0, k1, v in zip(
            tbl.column("k0").to_pylist(),
            tbl.column("k1").to_pylist(),
            tbl.column("value").to_pylist(),
        )
    }
    assert kv[(1000, 7)] == 2
    assert kv[(2000, 9)] == 3


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_f64_key_column():
    @jit.plugin
    class ByDur:
        m = jit.map(key=(jit.f64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.m[(e.dur,)] += 1

    # dur repeats so bit-identical float keys merge: 10.0 -> 2, 20.0 -> 3.
    events = [(1, 10, 1), (1, 10, 2), (1, 20, 3), (1, 20, 4), (1, 20, 5)]
    d = trace_dir(write_dur_trace, events)
    plugins = Plugins([ByDur])
    run = plugins.run(d)

    tbl = pa.table(run.results["m"])
    assert tbl.column_names == ["k0", "value"]
    assert pa.types.is_float64(tbl.schema.field("k0").type)

    kv = {k: v for k, v in zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist())}
    assert kv[10.0] == 2
    assert kv[20.0] == 3


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_min_max_value_columns():
    @jit.plugin
    class Vals:
        mn = jit.map(key=(jit.i64,), value=jit.min(of=jit.i32))
        mx = jit.map(key=(jit.i64,), value=jit.max(of=jit.u32))
        mf = jit.map(key=(jit.i64,), value=jit.min(of=jit.f32))

        @jit.each_event
        def step(self, e):
            self.mn[(e.pid,)].observe(e.dur - 8)
            self.mx[(e.pid,)].observe(e.dur)
            self.mf[(e.pid,)].observe(e.dur * 0.5)

    # dur-8 spans a negative min (3-8=-5); dur holds a max above INT32_MAX;
    # dur*0.5 a float min of 1.5.
    events = [(1, 3, 10), (1, 11, 20), (1, 4000000000, 30)]
    d = trace_dir(write_dur_trace, events)
    plugins = Plugins([Vals])
    run = plugins.run(d)

    mn = pa.table(run.results["mn"])
    assert pa.types.is_integer(mn.schema.field("value").type)
    assert mn.column("value").to_pylist() == [-5]

    mx = pa.table(run.results["mx"])
    assert pa.types.is_integer(mx.schema.field("value").type)
    assert mx.column("value").to_pylist() == [4000000000]

    mf = pa.table(run.results["mf"])
    assert pa.types.is_floating(mf.schema.field("value").type)
    assert mf.column("value").to_pylist() == [1.5]


def _write_name_only_trace(path: str, names) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i, name in enumerate(names):
            f.write(
                f'{{"name":"{name}","cat":"POSIX","pid":1,"tid":1,'
                f'"ts":{1000 + i},"dur":1,"ph":"X","args":{{}}}}\n'
            )


def _write_arg_trace(path: str, n: int, pids, tags) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i in range(n):
            pid = pids[i % len(pids)]
            tag = tags[i % len(tags)]
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{1000 + i},"dur":1,"ph":"X",'
                f'"args":{{"size":{100 + i},"bw":{i * 0.5},'
                f'"rank":{i % 3},"tag":"{tag}"}}}}\n'
            )


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_arg_f64_as_value_sums():
    @jit.plugin
    class Bw:
        bw = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.bw[(e.pid,)] += e.arg_f64("bw")

    # The compiled plugin resolves the "args.bw" dyn column once per batch,
    # then reads it by row index (no per-key interning, no per-row scan).
    src = Bw._jit_plugin.source
    assert 'dftu_jit_resolve(df, "args.bw")' in src
    assert "dftu_jit_arg_f64(&_argcol0, i)" in src

    n = 60
    pids = [1, 2]
    tags = ["ta", "tb", "tc"]
    d = trace_dir(_write_arg_trace, n, pids, tags)
    plugins = Plugins([Bw])
    run = plugins.run(d)

    tbl = pa.table(run.results["bw"])
    assert pa.types.is_float64(tbl.schema.field("value").type)
    val = tbl.column("value").to_numpy(zero_copy_only=False)
    assert float(val.sum()) == float(builtins.sum(i * 0.5 for i in range(n)))


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_arg_i64_into_counter_sums():
    @jit.plugin
    class Sizes:
        tot = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.tot[(e.pid,)] += e.arg_i64("size")

    n = 60
    pids = [1, 2]
    tags = ["ta", "tb", "tc"]
    d = trace_dir(_write_arg_trace, n, pids, tags)
    plugins = Plugins([Sizes])
    run = plugins.run(d)

    val = pa.table(run.results["tot"]).column("value").to_numpy(zero_copy_only=False)
    assert int(val.sum()) == builtins.sum(100 + i for i in range(n))


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_arg_i64_as_key():
    @jit.plugin
    class ByRank:
        cnt = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.cnt[(e.arg_i64("rank"),)] += 1

    n = 60
    pids = [1, 2]
    tags = ["ta", "tb", "tc"]
    d = trace_dir(_write_arg_trace, n, pids, tags)
    plugins = Plugins([ByRank])
    run = plugins.run(d)

    tbl = pa.table(run.results["cnt"])
    kv = kv_of(tbl)
    expected: dict = {}
    for i in range(n):
        expected[i % 3] = expected.get(i % 3, 0) + 1
    assert {int(k): int(v) for k, v in kv.items()} == expected


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_arg_str_set_element():
    @jit.plugin
    class Tags:
        tags = jit.map(key=(jit.i64,), value=jit.set())

        @jit.each_event
        def step(self, e):
            self.tags[(e.pid,)].observe(e.arg_str("tag"))

    n = 60
    pids = [1, 2]
    tags = ["ta", "tb", "tc"]
    d = trace_dir(_write_arg_trace, n, pids, tags)
    plugins = Plugins([Tags])
    run = plugins.run(d)

    tbl = pa.table(run.results["tags"])
    assert pa.types.is_string(tbl.schema.field("value").type)
    by_pid = sets_of(tbl)
    assert by_pid[1] == sorted(tags)
    assert by_pid[2] == sorted(tags)


def test_jit_rejects_non_literal_arg_name():
    with pytest.raises(jit.JitError, match="string literal"):

        @jit.plugin
        class BadName:
            m = jit.map(key=(jit.i64,), value=jit.sum())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)] += e.arg_f64(some_name)  # noqa: F821


def test_jit_rejects_unknown_arg_method():
    with pytest.raises(jit.JitError, match="arg_i64 / arg_f64 / arg_str"):

        @jit.plugin
        class BadMethod:
            m = jit.map(key=(jit.i64,), value=jit.sum())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)] += e.arg_bytes("size")


def test_jit_rejects_arg_f64_into_counter():
    with pytest.raises(jit.JitError, match="u64 monoid"):

        @jit.plugin
        class BadCounter:
            m = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)] += e.arg_f64("bw")


def _two_key_entries(tbl, value="value"):
    """{k0: {k1: value}} for a flat two-key result table."""
    out: dict = {}
    for k0, k1, v in zip(
        tbl.column("k0").to_pylist(),
        tbl.column("k1").to_pylist(),
        tbl.column(value).to_pylist(),
    ):
        out.setdefault(k0, {})[k1] = v
    return out


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_two_key_counter_per_pid_file():
    @jit.plugin
    class PidFileCounts:
        m = jit.map(key=(jit.i64, jit.str_), value=jit.count())

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.m[(e.pid, e.fhash)] += 1

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([PidFileCounts])
    run = plugins.run(d)

    tbl = pa.table(run.results["m"])
    assert tbl.column_names == ["k0", "k1", "value"]
    assert pa.types.is_int64(tbl.schema.field("k0").type)
    assert pa.types.is_string(tbl.schema.field("k1").type)
    assert pa.types.is_int64(tbl.schema.field("value").type)

    # One row per (pid, file), each with its own count.
    expected: dict = {}
    for i in range(n):
        pid = pids[i % len(pids)]
        f = files[i % len(files)]
        expected.setdefault(pid, {})
        expected[pid][f] = expected[pid].get(f, 0) + 1

    by_pid = _two_key_entries(tbl)
    assert by_pid == expected
    assert builtins.sum(v for inner in by_pid.values() for v in inner.values()) == n


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_two_key_product_count_and_sum():
    @jit.plugin
    class PidFileStats:
        m = jit.map(
            key=(jit.i64, jit.str_),
            value=dict(cnt=jit.count(), dur=jit.sum()),
        )

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.m[(e.pid, e.fhash)].cnt += 1
                self.m[(e.pid, e.fhash)].dur += e.dur

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    plugins = Plugins([PidFileStats])
    run = plugins.run(d)

    tbl = pa.table(run.results["m"])
    assert tbl.column_names == ["k0", "k1", "cnt", "dur"]
    assert pa.types.is_int64(tbl.schema.field("cnt").type)
    assert pa.types.is_float64(tbl.schema.field("dur").type)

    exp_cnt: dict = {}
    exp_dur: dict = {}
    for i in range(n):
        pid = pids[i % len(pids)]
        f = files[i % len(files)]
        exp_cnt[(pid, f)] = exp_cnt.get((pid, f), 0) + 1
        exp_dur[(pid, f)] = exp_dur.get((pid, f), 0.0) + (10 + i)

    got_cnt = _two_key_entries(tbl, "cnt")
    got_dur = _two_key_entries(tbl, "dur")
    for (pid, fname), want in exp_cnt.items():
        assert got_cnt[pid][fname] == want
        assert got_dur[pid][fname] == exp_dur[(pid, fname)]
    assert builtins.sum(v for i in got_cnt.values() for v in i.values()) == n
    assert builtins.sum(v for i in got_dur.values() for v in i.values()) == float(
        builtins.sum(10 + i for i in range(n))
    )


def test_jit_rejects_component_on_single_value_map():
    with pytest.raises(jit.JitError, match="with a component"):

        @jit.plugin
        class Bad:
            m = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)].cnt += 1


def test_jit_rejects_string_collection_as_product_component():
    with pytest.raises(jit.JitError, match="cannot be a product component"):

        @jit.plugin
        class Bad:
            m = jit.map(key=(jit.i64,), value=dict(cnt=jit.count(), names=jit.set()))

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)].cnt += 1


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_flat_multikey_chained_subscript_sugar():
    @jit.plugin
    class Chained:
        m = jit.map(key=(jit.i64, jit.i64), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.m[e.pid][e.tid] += 1

    rows = [(1, 7), (1, 7), (2, 9), (2, 9), (2, 9)]
    d = trace_dir(write_pid_tid_trace, rows)
    plugins = Plugins([Chained])
    run = plugins.run(d)

    tbl = pa.table(run.results["m"])
    assert tbl.column_names == ["k0", "k1", "value"]
    kv = {
        (k0, k1): v
        for k0, k1, v in zip(
            tbl.column("k0").to_pylist(),
            tbl.column("k1").to_pylist(),
            tbl.column("value").to_pylist(),
        )
    }
    assert kv[(1, 7)] == 2
    assert kv[(2, 9)] == 3


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_bare_single_key_matches_tuple_form():
    @jit.plugin
    class Bare:
        m = jit.map(key=jit.i64, value=jit.count())

        @jit.each_event
        def step(self, e):
            self.m[e.pid] += 1

    @jit.plugin
    class Tupled:
        m = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.m[(e.pid,)] += 1

    # Bare key normalizes to the identical 1-tuple decl the compiler reads (up
    # to the class-qualified wire id, which differs because the two classes
    # have different names).
    (bare_wire_id,) = Bare._jit_plugin.result_names.keys()
    (tupled_wire_id,) = Tupled._jit_plugin.result_names.keys()
    bare_src = Bare._jit_plugin.source.replace(bare_wire_id, "m")
    tupled_src = Tupled._jit_plugin.source.replace(tupled_wire_id, "m")
    assert bare_src == tupled_src
    assert 'const char* _keys[1] = {"k0"};' in Bare._jit_plugin.source
    assert "DFTU_TYPE_INT64, _k0_m" in Bare._jit_plugin.source

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    d = trace_dir(write_trace, n, pids, files)
    bare_plugins = Plugins([Bare])
    bare = kv_of(pa.table(bare_plugins.run(d).results["m"]))

    tup_plugins = Plugins([Tupled])
    tup = kv_of(pa.table(tup_plugins.run(d).results["m"]))

    assert {int(k): int(v) for k, v in bare.items()} == {int(k): int(v) for k, v in tup.items()}
    assert int(sum(bare.values())) == n


def test_jit_rejects_bare_subscript_on_multikey():
    with pytest.raises(jit.JitError, match="key components"):

        @jit.plugin
        class Multi:
            m = jit.map(key=(jit.i64, jit.i64), value=jit.count())

            @jit.each_event
            def step(self, e):
                self.m[e.pid] += 1


def test_jit_rejects_bytes_map_key():
    # A key becomes a grouping column, and there is no bytes column form.
    with pytest.raises(jit.JitError, match="bytes map key"):
        jit.map(key=jit.bytes, value=jit.count())


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_str_key_map_groups_by_label():
    @jit.plugin
    class ByName:
        by_name = jit.map(key=(jit.str_,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.by_name[(e.name,)] += 1

    # Names emitted out of alphabetical order: apple x2, banana x1, cat x3.
    names = ["cat", "apple", "cat", "banana", "apple", "cat"]
    d = trace_dir(_write_name_only_trace, names)
    plugins = Plugins([ByName])
    run = plugins.run(d)

    tbl = pa.table(run.results["by_name"])
    keys = tbl.column("k0").to_pylist()
    vals = tbl.column("value").to_pylist()

    # A STR key materializes as its resolved label, one row per distinct name.
    assert sorted(keys) == ["apple", "banana", "cat"]
    assert dict(zip(keys, vals)) == {"apple": 2, "banana": 1, "cat": 3}


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_variance_matches_handcomputed():
    @jit.plugin
    class DurVar:
        dv = jit.map(key=(jit.i64,), value=jit.variance())

        @jit.each_event
        def step(self, e):
            self.dv[(e.pid,)].observe(e.dur)

    events = [(1, 20, 10), (1, 5, 20), (1, 20, 30), (1, 8, 40), (2, 3, 1), (2, 7, 5)]
    d = trace_dir(write_dur_trace, events)
    plugins = Plugins([DurVar])
    run = plugins.run(d)

    tbl = pa.table(run.results["dv"])
    assert tbl.column_names == ["k0", "value"]
    assert pa.types.is_float64(tbl.schema.field("value").type)

    durs: dict = {}
    for pid, dur, _ts in events:
        durs.setdefault(pid, []).append(dur)
    got = kv_of(tbl)
    assert float(got[1]) == pytest.approx(statistics.variance(durs[1]))
    assert float(got[2]) == pytest.approx(statistics.variance(durs[2]))


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_mean_and_stddev_columns():
    @jit.plugin
    class DurStats:
        mu = jit.map(key=(jit.i64,), value=jit.mean())
        sd = jit.map(key=(jit.i64,), value=jit.stddev())

        @jit.each_event
        def step(self, e):
            self.mu[(e.pid,)].observe(e.dur)
            self.sd[(e.pid,)].observe(e.dur)

    events = [(1, 20, 10), (1, 5, 20), (1, 20, 30), (1, 8, 40), (2, 3, 1), (2, 7, 5)]
    d = trace_dir(write_dur_trace, events)
    plugins = Plugins([DurStats])
    run = plugins.run(d)

    assert pa.types.is_float64(pa.table(run.results["mu"]).schema.field("value").type)
    assert pa.types.is_float64(pa.table(run.results["sd"]).schema.field("value").type)

    durs: dict = {}
    for pid, dur, _ts in events:
        durs.setdefault(pid, []).append(dur)
    mu = kv_of(pa.table(run.results["mu"]))
    sd = kv_of(pa.table(run.results["sd"]))
    for pid, vs in durs.items():
        assert float(mu[pid]) == pytest.approx(statistics.mean(vs))
        assert float(sd[pid]) == pytest.approx(statistics.stdev(vs))


def _write_file_dur_trace(path: str, rows) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i, (pid, fhash, dur) in enumerate(rows):
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{1000 + i},"dur":{dur},"ph":"X",'
                f'"args":{{"fhash":"{fhash}"}}}}\n'
            )


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_argmax_argmin_str_payload():
    @jit.plugin
    class HotCold:
        hot = jit.map(key=(jit.i64,), value=jit.argmax(of=jit.str_))
        cold = jit.map(key=(jit.i64,), value=jit.argmin(of=jit.str_))

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.hot[(e.pid,)].observe(e.fhash, by=e.dur)
                self.cold[(e.pid,)].observe(e.fhash, by=e.dur)

    # pid 1: max dur 30 -> "b", min dur 5 -> "a"; pid 2: max 7 -> "x", min 2 -> "y".
    rows = [(1, "a", 5), (1, "b", 30), (1, "c", 10), (2, "x", 7), (2, "y", 2)]
    d = trace_dir(_write_file_dur_trace, rows)
    plugins = Plugins([HotCold])
    run = plugins.run(d)

    hot = pa.table(run.results["hot"])
    cold = pa.table(run.results["cold"])
    assert pa.types.is_string(hot.schema.field("value").type)
    hot_by_pid = {
        k: v for k, v in zip(hot.column("k0").to_pylist(), hot.column("value").to_pylist())
    }
    cold_by_pid = {
        k: v for k, v in zip(cold.column("k0").to_pylist(), cold.column("value").to_pylist())
    }
    assert hot_by_pid == {1: "b", 2: "x"}
    assert cold_by_pid == {1: "a", 2: "y"}


def _write_tid_dur_trace(path: str, rows) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i, (pid, tid, dur) in enumerate(rows):
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":{tid},'
                f'"ts":{1000 + i},"dur":{dur},"ph":"X","args":{{}}}}\n'
            )


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_argmax_i64_payload():
    @jit.plugin
    class SlowTid:
        tid_at_max = jit.map(key=(jit.i64,), value=jit.argmax(of=jit.i64))

        @jit.each_event
        def step(self, e):
            self.tid_at_max[(e.pid,)].observe(e.tid, by=e.dur)

    # pid 1: max dur 30 at tid 8; pid 2: max dur 40 at tid 5.
    rows = [(1, 7, 10), (1, 8, 30), (1, 9, 20), (2, 5, 40), (2, 6, 15)]
    d = trace_dir(_write_tid_dur_trace, rows)
    plugins = Plugins([SlowTid])
    run = plugins.run(d)

    tbl = pa.table(run.results["tid_at_max"])
    # argmax reports the String repr of the payload whatever its column type.
    assert pa.types.is_string(tbl.schema.field("value").type)
    got = dict(zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist()))
    assert got == {1: "8", 2: "5"}


def test_jit_rejects_argby_observe_without_by():
    with pytest.raises(jit.JitError, match="requires by="):

        @jit.plugin
        class NoBy:
            m = jit.map(key=(jit.i64,), value=jit.argmax(of=jit.str_))

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)].observe(e.fhash)


def test_jit_rejects_moment_observe_with_by():
    with pytest.raises(jit.JitError, match="one value argument"):

        @jit.plugin
        class MomentBy:
            m = jit.map(key=(jit.i64,), value=jit.variance())

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)].observe(e.dur, by=e.ts)


def test_jit_rejects_str_payload_into_i64_argmax():
    with pytest.raises(jit.JitError, match="int64 expr"):

        @jit.plugin
        class Bad:
            m = jit.map(key=(jit.i64,), value=jit.argmax(of=jit.i64))

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)].observe(e.name, by=e.dur)


def test_jit_rejects_str_argmax_as_product_component():
    with pytest.raises(jit.JitError, match="cannot be a product component"):

        @jit.plugin
        class Bad:
            m = jit.map(key=(jit.i64,), value=dict(cnt=jit.count(), who=jit.argmax(of=jit.str_)))

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)].cnt += 1


def _write_row_trace(path: str, rows) -> None:
    """rows: (pid, fhash, tid, dur, ts)."""
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for pid, fhash, tid, dur, ts in rows:
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":{tid},'
                f'"ts":{ts},"dur":{dur},"ph":"X",'
                f'"args":{{"fhash":"{fhash}"}}}}\n'
            )


# pid 1: a@100, b@300, c@200, a@50; pid 2: x@40, y@70. dur is the by/rank key.
_ROWS = [
    (1, "a", 7, 100, 1000),
    (1, "b", 8, 300, 1001),
    (1, "c", 9, 200, 1002),
    (1, "a", 7, 50, 1003),
    (2, "x", 5, 40, 2000),
    (2, "y", 6, 70, 2001),
]


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_topk_keeps_extreme_payloads_in_order():
    @jit.plugin
    class Hot:
        hot = jit.map(key=jit.i64, value=jit.topk(2, of=jit.str_))
        low = jit.map(key=jit.i64, value=jit.bottomk(2, of=jit.i64))

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.hot[e.pid].observe(e.fhash, by=e.dur)
                self.low[e.pid].observe(e.dur, by=e.dur)

    d = trace_dir(_write_row_trace, _ROWS)
    plugins = Plugins([Hot])
    run = plugins.run(d)

    hot = pa.table(run.results["hot"])
    vtype = hot.schema.field("value").type
    assert pa.types.is_list(vtype)
    assert pa.types.is_string(vtype.value_type)
    hot_by_pid = {
        k: v for k, v in zip(hot.column("k0").to_pylist(), hot.column("value").to_pylist())
    }
    # Top 2 payloads by dur, largest first.
    assert hot_by_pid[1] == ["b", "c"]
    assert hot_by_pid[2] == ["y", "x"]

    low = pa.table(run.results["low"])
    assert pa.types.is_string(low.schema.field("value").type.value_type)
    low_by_pid = {
        k: v for k, v in zip(low.column("k0").to_pylist(), low.column("value").to_pylist())
    }
    # Bottom 2 dur payloads (String reprs), smallest `by` first.
    assert low_by_pid[1] == ["50", "100"]
    assert low_by_pid[2] == ["40", "70"]


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_approx_topk_exact_when_k_ge_distinct():
    @jit.plugin
    class Freq:
        freq = jit.map(key=jit.i64, value=jit.approx_topk(8, of=jit.str_))

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.freq[e.pid].observe(e.fhash)

    d = trace_dir(_write_row_trace, _ROWS)
    plugins = Plugins([Freq])
    run = plugins.run(d)

    tbl = pa.table(run.results["freq"])
    vtype = tbl.schema.field("value").type
    assert pa.types.is_list(vtype)
    assert pa.types.is_struct(vtype.value_type)
    assert [f.name for f in vtype.value_type] == ["value", "count"]

    by_pid = {k: v for k, v in zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist())}
    # k >= distinct so counts are exact: pid 1 sees a twice, b and c once.
    counts1 = {e["value"]: e["count"] for e in by_pid[1]}
    assert counts1 == {"a": 2, "b": 1, "c": 1}
    counts2 = {e["value"]: e["count"] for e in by_pid[2]}
    assert counts2 == {"x": 1, "y": 1}


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_sample_keeps_items_from_input():
    @jit.plugin
    class Samp:
        samp = jit.map(key=jit.i64, value=jit.sample(10, of=jit.i64))
        small = jit.map(key=jit.i64, value=jit.sample(2, of=jit.i64))

        @jit.each_event
        def step(self, e):
            self.samp[e.pid].observe(e.ts)
            self.small[e.pid].observe(e.ts)

    d = trace_dir(_write_row_trace, _ROWS)
    plugins = Plugins([Samp])
    run = plugins.run(d)

    # The sample keeps String reprs of the distinct values.
    ts_by_pid = {1: {"1000", "1001", "1002", "1003"}, 2: {"2000", "2001"}}

    tbl = pa.table(run.results["samp"])
    assert pa.types.is_string(tbl.schema.field("value").type.value_type)
    by_pid = {k: v for k, v in zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist())}
    # k >= distinct: the full distinct set, sorted.
    assert by_pid[1] == sorted(ts_by_pid[1])
    assert by_pid[2] == sorted(ts_by_pid[2])

    small = pa.table(run.results["small"])
    sm_by_pid = {
        k: v for k, v in zip(small.column("k0").to_pylist(), small.column("value").to_pylist())
    }
    for pid, items in sm_by_pid.items():
        assert len(items) <= 2
        assert set(items) <= ts_by_pid[pid]


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_argmax_keeps_each_winning_field():
    @jit.plugin
    class Slow:
        who = jit.map(key=jit.i64, value=jit.argmax(of=jit.str_))
        tid = jit.map(key=jit.i64, value=jit.argmax(of=jit.i64))
        ts = jit.map(key=jit.i64, value=jit.argmax(of=jit.i64))

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.who[e.pid].observe(e.fhash, by=e.dur)
                self.tid[e.pid].observe(e.tid, by=e.dur)
                self.ts[e.pid].observe(e.ts, by=e.dur)

    d = trace_dir(_write_row_trace, _ROWS)
    plugins = Plugins([Slow])
    run = plugins.run(d)

    def col(name):
        tbl = pa.table(run.results[name])
        assert tbl.column_names == ["k0", "value"]
        return dict(zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist()))

    who, tid, ts = col("who"), col("tid"), col("ts")
    for name in ("who", "tid", "ts"):
        assert pa.types.is_string(pa.table(run.results[name]).schema.field("value").type)
    # The (fhash, tid, ts) at the max dur, per pid, as String reprs.
    assert (who[1], tid[1], ts[1]) == ("b", "8", "1001")
    assert (who[2], tid[2], ts[2]) == ("y", "6", "2001")


def test_jit_rejects_topk_observe_without_by():
    with pytest.raises(jit.JitError, match="requires by="):

        @jit.plugin
        class NoBy:
            m = jit.map(key=jit.i64, value=jit.topk(3, of=jit.str_))

            @jit.each_event
            def step(self, e):
                self.m[e.pid].observe(e.fhash)


def test_jit_rejects_approx_topk_observe_with_by():
    with pytest.raises(jit.JitError, match="one value argument"):

        @jit.plugin
        class WithBy:
            m = jit.map(key=jit.i64, value=jit.approx_topk(3, of=jit.str_))

            @jit.each_event
            def step(self, e):
                self.m[e.pid].observe(e.fhash, by=e.dur)


def test_jit_rejects_sample_observe_with_by():
    with pytest.raises(jit.JitError, match="one value argument"):

        @jit.plugin
        class WithBy:
            m = jit.map(key=jit.i64, value=jit.sample(3, of=jit.i64))

            @jit.each_event
            def step(self, e):
                self.m[e.pid].observe(e.ts, by=e.dur)


def test_jit_rejects_sample_str_into_i64():
    with pytest.raises(jit.JitError, match="int64 expr"):

        @jit.plugin
        class Bad:
            m = jit.map(key=jit.i64, value=jit.sample(3, of=jit.i64))

            @jit.each_event
            def step(self, e):
                self.m[e.pid].observe(e.name)


def _write_join_trace(path: str, rows) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i, (pid, cat, dur) in enumerate(rows):
            f.write(
                f'{{"name":"read","cat":"{cat}","pid":{pid},"tid":1,'
                f'"ts":{1000 + i},"dur":{dur},"ph":"X","args":{{}}}}\n'
            )


# pid 1 is always POSIX (present in both accumulators); pid 2 is never POSIX, so
# it lands in counts but not in durs.
_JOIN_ROWS = [
    (1, "POSIX", 10),
    (1, "POSIX", 20),
    (2, "STDIO", 30),
    (2, "STDIO", 40),
    (2, "STDIO", 50),
]


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_guarded_accumulator_keys_are_independent():
    # Two accumulators over the same key: one unconditional, one guarded. This is
    # the AggState form of what the deleted map-to-map join computed.
    @jit.plugin
    class PidStats:
        counts = jit.map(key=jit.i64, value=jit.count())
        durs = jit.map(key=jit.i64, value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.counts[e.pid] += 1
            if e.cat == "POSIX":
                self.durs[e.pid] += e.dur

    d = trace_dir(_write_join_trace, _JOIN_ROWS)
    plugins = Plugins([PidStats])
    run = plugins.run(d)

    assert kv_of(pa.table(run.results["counts"])) == {1: 2, 2: 3}
    # Only pid 1 ever fed the guarded accumulator, so pid 2 has no row there.
    assert kv_of(pa.table(run.results["durs"])) == {1: 30.0}
