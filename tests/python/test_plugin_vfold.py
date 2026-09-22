#!/usr/bin/env python3
"""End-to-end @jit.vfold: a per-batch column reduction, folded to a scalar and
materialized like any other jit plugin result."""

import gzip
import shutil

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

from .jit_common import trace_dir

pa = pytest.importorskip("pyarrow")
_HAS_CXX = bool(shutil.which("c++") or shutil.which("clang++") or shutil.which("g++"))
_needs_cxx = pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler for the jit backend")


def _write_trace(path, durs, pids=None):
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i, d in enumerate(durs):
            pid = 1 if pids is None else pids[i]
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{1000 + i},"dur":{d},"ph":"X","args":{{}}}}\n'
            )


def _value(result, name):
    return pa.table(result[name]).column("value").to_numpy(zero_copy_only=False)


def _keyed(result, name, key):
    # A keyed vfold now crosses as a native _DataFrame (DFTU_SVC_AGG finalizes to
    # a native frame); read it at the edge as pandas.
    pdf = result[name].to_pandas()
    return dict(zip(pdf[key].tolist(), pdf["value"].tolist()))


@_needs_cxx
def test_vfold_scalar_sum():
    @jit.vfold
    class Busy:
        total = jit.sum()

        @jit.each_batch
        def step(self, df):
            self.total += df["dur"].sum()

    durs = [10, 20, 30, 40, 50]
    d = trace_dir(_write_trace, durs)
    plugins = Plugins([Busy])
    result = plugins.run(d).results
    assert float(_value(result, "total").sum()) == float(sum(durs))  # 150


@_needs_cxx
def test_vfold_scalar_min_max():
    @jit.vfold
    class Bounds:
        lo = jit.min()
        hi = jit.max()

        @jit.each_batch
        def step(self, df):
            self.lo += df["dur"].min()
            self.hi += df["dur"].max()

    durs = [30, 10, 50, 20, 40]
    d = trace_dir(_write_trace, durs)
    plugins = Plugins([Bounds])
    result = plugins.run(d).results
    assert int(_value(result, "lo").sum()) == 10
    assert int(_value(result, "hi").sum()) == 50


@_needs_cxx
def test_vfold_keyed_sum_per_pid():
    @jit.vfold
    class PerPid:
        busy = jit.map(key=jit.i64, value=jit.sum())

        @jit.each_batch
        def step(self, df):
            self.busy[df["pid"]] += df["dur"]

    durs = [10, 20, 30, 40]
    pids = [1, 2, 1, 2]
    d = trace_dir(_write_trace, durs, pids)
    plugins = Plugins([PerPid])
    got = _keyed(plugins.run(d).results, "busy", "pid")
    assert got == {1: 40.0, 2: 60.0}  # pid1: 10+30, pid2: 20+40


@_needs_cxx
def test_vfold_keyed_sum_per_name(tmp_path):
    @jit.vfold
    class ByName:
        dur = jit.map(key=jit.str_, value=jit.sum())

        @jit.each_batch
        def step(self, df):
            self.dur[df["name"]] += df["dur"]

    names = ["read", "write", "read", "write", "read"]
    durs = [10, 100, 20, 200, 30]
    with gzip.open(str(tmp_path / "t.pfw.gz"), "wt", encoding="utf-8") as f:
        for i, (nm, d) in enumerate(zip(names, durs)):
            f.write(
                f'{{"name":"{nm}","cat":"POSIX","pid":1,"tid":1,'
                f'"ts":{1000 + i},"dur":{d},"ph":"X","args":{{}}}}\n'
            )
    plugins = Plugins([ByName])
    got = _keyed(plugins.run(str(tmp_path)).results, "dur", "name")
    assert got == {"read": 60.0, "write": 300.0}


@_needs_cxx
def test_vfold_keyed_max_per_pid():
    @jit.vfold
    class Peak:
        hi = jit.map(key=jit.i64, value=jit.max())

        @jit.each_batch
        def step(self, df):
            self.hi[df["pid"]] += df["dur"]

    d = trace_dir(_write_trace, [10, 90, 30, 40], [1, 2, 1, 2])
    plugins = Plugins([Peak])
    got = _keyed(plugins.run(d).results, "hi", "pid")
    assert got == {1: 30, 2: 90}  # per-pid max


@_needs_cxx
def test_vfold_keyed_count_per_pid():
    @jit.vfold
    class Hits:
        n = jit.map(key=jit.i64, value=jit.count())

        @jit.each_batch
        def step(self, df):
            self.n[df["pid"]] += 1

    d = trace_dir(_write_trace, [1, 1, 1, 1, 1], [1, 1, 2, 2, 2])
    plugins = Plugins([Hits])
    got = _keyed(plugins.run(d).results, "n", "pid")
    assert got == {1: 2, 2: 3}


@_needs_cxx
def test_vfold_keyed_pct_per_pid():
    # A per-key percentile through DFTU_SVC_AGG's DDSketch quantile.
    @jit.vfold
    class Median:
        p50 = jit.map(key=jit.i64, value=jit.quantiles((0.5,)))

        @jit.each_batch
        def step(self, df):
            self.p50[df["pid"]] += df["dur"]

    durs = [10, 20, 30, 40, 50, 100, 200, 300]
    pids = [1, 1, 1, 1, 1, 2, 2, 2]
    d = trace_dir(_write_trace, durs, pids)
    plugins = Plugins([Median])
    # A quantile accumulator names its columns after the levels it holds (plus
    # the group count), like the multi-quantile case.
    pdf = plugins.run(d).results["p50"].to_pandas()
    assert list(pdf.columns) == ["pid", "count", "p50"]
    got = dict(zip(pdf["pid"].tolist(), pdf["p50"].tolist()))
    counts = dict(zip(pdf["pid"].tolist(), pdf["count"].tolist()))
    assert counts == {1: 5, 2: 3}
    assert got[1] == pytest.approx(30.0, rel=0.05)  # median of pid1
    assert got[2] == pytest.approx(200.0, rel=0.05)  # median of pid2


# pid1 = [10,20,30,40,50] (symmetric, mean 30); pid2 = [100,200,300] (mean 200).
_MOM_DURS = [10, 20, 30, 40, 50, 100, 200, 300]
_MOM_PIDS = [1, 1, 1, 1, 1, 2, 2, 2]


@_needs_cxx
def test_vfold_keyed_moments():
    # var/std/skew/sumsq/count_valid reach vfold through DFTU_SVC_AGG; none was
    # expressible on the legacy group_by/row-fold map path.
    @jit.vfold
    class Moments:
        variance = jit.map(key=jit.i64, value=jit.variance())
        std = jit.map(key=jit.i64, value=jit.stddev())
        skew = jit.map(key=jit.i64, value=jit.skew())
        sumsq = jit.map(key=jit.i64, value=jit.sumsq())
        nvalid = jit.map(key=jit.i64, value=jit.count_valid())

        @jit.each_batch
        def step(self, df):
            self.variance[df["pid"]] += df["dur"]
            self.std[df["pid"]] += df["dur"]
            self.skew[df["pid"]] += df["dur"]
            self.sumsq[df["pid"]] += df["dur"]
            self.nvalid[df["pid"]] += df["dur"]

    d = trace_dir(_write_trace, _MOM_DURS, _MOM_PIDS)
    plugins = Plugins([Moments])
    result = plugins.run(d).results
    var = _keyed(result, "variance", "pid")
    std = _keyed(result, "std", "pid")
    skew = _keyed(result, "skew", "pid")
    sumsq = _keyed(result, "sumsq", "pid")
    nvalid = _keyed(result, "nvalid", "pid")
    assert var[1] == pytest.approx(250.0)  # sample variance, n-1
    assert var[2] == pytest.approx(10000.0)
    assert std[1] == pytest.approx(250.0**0.5)
    assert std[2] == pytest.approx(100.0)
    assert skew[1] == pytest.approx(0.0, abs=1e-9)  # symmetric
    assert skew[2] == pytest.approx(0.0, abs=1e-9)
    assert sumsq[1] == pytest.approx(5500.0)  # 10^2+..+50^2
    assert sumsq[2] == pytest.approx(140000.0)
    assert nvalid[1] == 5
    assert nvalid[2] == 3


@_needs_cxx
def test_vfold_keyed_set_union():
    @jit.vfold
    class Distinct:
        vals = jit.map(key=jit.i64, value=jit.set(of=jit.i64))

        @jit.each_batch
        def step(self, df):
            self.vals[df["pid"]] += df["dur"]

    durs = [10, 20, 10, 30, 20, 200, 200, 100]
    pids = [1, 1, 1, 1, 1, 2, 2, 2]
    d = trace_dir(_write_trace, durs, pids)
    plugins = Plugins([Distinct])
    got = _keyed(plugins.run(d).results, "vals", "pid")

    # SET_UNION crosses as one text cell of distinct values, separated by 0x1e.
    def _members(cell):
        return {int(x) for x in cell.split("\x1e") if x}

    assert _members(got[1]) == {10, 20, 30}
    assert _members(got[2]) == {100, 200}


@_needs_cxx
def test_vfold_keyed_first_last():
    @jit.vfold
    class Ends:
        lo = jit.map(key=jit.i64, value=jit.first())
        hi = jit.map(key=jit.i64, value=jit.last())

        @jit.each_batch
        def step(self, df):
            self.lo[df["pid"]] += df["dur"]
            self.hi[df["pid"]] += df["dur"]

    d = trace_dir(_write_trace, _MOM_DURS, _MOM_PIDS)
    plugins = Plugins([Ends])
    result = plugins.run(d).results
    lo = _keyed(result, "lo", "pid")
    hi = _keyed(result, "hi", "pid")
    # first/last land on a real value in each group (row order is scan-dependent).
    assert lo[1] in _MOM_DURS[:5]
    assert hi[1] in _MOM_DURS[:5]
    assert lo[2] in _MOM_DURS[5:]
    assert hi[2] in _MOM_DURS[5:]


def _keyed_arrow(result, name, key):
    # A keyed vfold with a list<struct> output (hist) crosses as a native
    # _DataFrame; read it via Arrow like the columnar hist result.
    tbl = result[name].to_arrow()
    return {r[key]: r["value"] for r in tbl.to_pylist()}


@_needs_cxx
def test_vfold_keyed_hist():
    @jit.vfold
    class Hist:
        h = jit.map(key=jit.i64, value=jit.hist())

        @jit.each_batch
        def step(self, df):
            self.h[df["pid"]] += df["dur"]

    d = trace_dir(_write_trace, _MOM_DURS, _MOM_PIDS)
    plugins = Plugins([Hist])
    rows = _keyed_arrow(plugins.run(d).results, "h", "pid")
    assert set(rows) == {1, 2}
    for pid, bins in rows.items():
        assert len(bins) > 0
        assert set(bins[0].keys()) == {"lo", "hi", "count"}
        # Each group's bin counts sum to its row count.
        assert sum(b["count"] for b in bins) == (5 if pid == 1 else 3)
        # Every observed value falls inside some bin.
        vals = _MOM_DURS[:5] if pid == 1 else _MOM_DURS[5:]
        for v in vals:
            assert any(b["lo"] <= v <= b["hi"] for b in bins)


@_needs_cxx
def test_vfold_keyed_argmax():
    # argmax reads a (value, by) pair: the value at the row maximizing by, per
    # key. Here: the dur of the latest (max ts) event per pid.
    @jit.vfold
    class Peak:
        latest = jit.map(key=jit.i64, value=jit.argmax(of=jit.i64))

        @jit.each_batch
        def step(self, df):
            self.latest[df["pid"]] += df["dur"], df["ts"]

    d = trace_dir(_write_trace, [10, 20, 30, 40], [1, 2, 1, 2])
    plugins = Plugins([Peak])
    # ts = 1000 + i; pid1 rows are i=0 (dur10) and i=2 (dur30); pid2 i=1 (dur20)
    # and i=3 (dur40). The max-ts row's dur is the argmax repr.
    got = _keyed(plugins.run(d).results, "latest", "pid")
    assert got == {1: "30", 2: "40"}


@_needs_cxx
def test_vfold_keyed_occupancy(tmp_path):
    # busy = interval-union length where depth > 0; active = peak overlap depth.
    # Both read the (ts, dur) pair.
    @jit.vfold
    class Occ:
        busy = jit.map(key=jit.i64, value=jit.busy())
        active = jit.map(key=jit.i64, value=jit.active())

        @jit.each_batch
        def step(self, df):
            self.busy[df["pid"]] += df["ts"], df["dur"]
            self.active[df["pid"]] += df["ts"], df["dur"]

    # pid1: [0,100) and [50,150) -> union [0,150)=150, peak depth 2.
    # pid2: [0,50) and [100,150) -> disjoint union 100, peak depth 1.
    events = [(1, 0, 100), (1, 50, 100), (2, 0, 50), (2, 100, 50)]
    with gzip.open(str(tmp_path / "t.pfw.gz"), "wt", encoding="utf-8") as f:
        for pid, ts, dur in events:
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{ts},"dur":{dur},"ph":"X","args":{{}}}}\n'
            )
    plugins = Plugins([Occ])
    result = plugins.run(str(tmp_path)).results
    busy = _keyed(result, "busy", "pid")
    active = _keyed(result, "active", "pid")
    assert busy[1] == 150.0
    assert busy[2] == 100.0
    assert active[1] == 2.0
    assert active[2] == 1.0


@_needs_cxx
def test_vfold_multi_key_groups_by_both_columns(tmp_path):
    @jit.vfold
    class Both:
        dur = jit.map(key=(jit.i64, jit.str_), value=jit.sum())

        @jit.each_batch
        def step(self, df):
            self.dur[(df["pid"], df["name"])] += df["dur"]

    rows = [(1, "read", 10), (1, "write", 100), (2, "read", 20), (1, "read", 5)]
    with gzip.open(str(tmp_path / "t.pfw.gz"), "wt", encoding="utf-8") as f:
        for i, (pid, nm, d) in enumerate(rows):
            f.write(
                f'{{"name":"{nm}","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{1000 + i},"dur":{d},"ph":"X","args":{{}}}}\n'
            )

    plugins = Plugins([Both])
    pdf = plugins.run(str(tmp_path)).results["dur"].to_pandas()
    got = {(p, n): v for p, n, v in zip(pdf["pid"], pdf["name"], pdf["value"])}
    assert got == {(1, "read"): 15.0, (1, "write"): 100.0, (2, "read"): 20.0}


@_needs_cxx
def test_vfold_keyed_multi_quantile():
    @jit.vfold
    class Lat:
        q = jit.map(key=jit.i64, value=jit.quantiles((0.5, 0.9)))

        @jit.each_batch
        def step(self, df):
            self.q[df["pid"]] += df["dur"]

    durs = list(range(1, 101))
    d = trace_dir(_write_trace, durs, [1] * 100)
    plugins = Plugins([Lat])
    pdf = plugins.run(d).results["q"].to_pandas()
    assert list(pdf.columns) == ["pid", "count", "p50", "p90"]
    assert int(pdf["count"][0]) == 100
    assert pdf["p50"][0] == pytest.approx(50.0, rel=0.05)
    assert pdf["p90"][0] == pytest.approx(90.0, rel=0.05)


@_needs_cxx
def test_vfold_keyed_regr_slope():
    # dur = 2 * (ts - 1000): a perfect line, so the least-squares slope is 2.
    @jit.vfold
    class Fit:
        slope = jit.map(key=jit.i64, value=jit.regr_slope())

        @jit.each_batch
        def step(self, df):
            self.slope[df["pid"]] += df["dur"], df["ts"]

    durs = [2 * i for i in range(10)]
    d = trace_dir(_write_trace, durs, [1] * 10)
    plugins = Plugins([Fit])
    got = _keyed(plugins.run(d).results, "slope", "pid")
    assert got[1] == pytest.approx(2.0, rel=1e-6)


class TestAuthoring:
    def test_reducer_must_match_accumulator(self):
        with pytest.raises(jit.JitError):

            @jit.vfold
            class Bad:
                total = jit.sum()

                @jit.each_batch
                def step(self, df):
                    self.total += df["dur"].max()  # max into a sum accumulator

    def test_keyed_field_must_be_numeric(self):
        with pytest.raises(jit.JitError):

            @jit.vfold
            class BadKey:
                by_name = jit.map(key=jit.i64, value=jit.sum())

                @jit.each_batch
                def step(self, df):
                    self.by_name[df["name"]] += df["dur"]  # string key not supported

    def test_needs_one_each_batch(self):
        with pytest.raises(jit.JitError):

            @jit.vfold
            class NoBatch:
                total = jit.sum()

    def test_scalar_string_field_rejected(self):
        with pytest.raises(jit.JitError):

            @jit.vfold
            class Bad:
                total = jit.sum()

                @jit.each_batch
                def step(self, df):
                    self.total += df["name"].sum()  # reduce over a string is 0

    def test_counter_map_rejects_a_column(self):
        with pytest.raises(jit.JitError):

            @jit.vfold
            class Bad2:
                hits = jit.map(key=jit.i64, value=jit.count())

                @jit.each_batch
                def step(self, df):
                    self.hits[df["pid"]] += df["dur"]  # a counter needs += 1

    def test_reduction_rejects_a_constant(self):
        with pytest.raises(jit.JitError):

            @jit.vfold
            class Bad3:
                s = jit.map(key=jit.i64, value=jit.skew())

                @jit.each_batch
                def step(self, df):
                    self.s[df["pid"]] += 1  # a reduction needs a value column

    def test_argmax_requires_value_by_pair(self):
        with pytest.raises(jit.JitError):

            @jit.vfold
            class BadAm:
                m = jit.map(key=jit.i64, value=jit.argmax())

                @jit.each_batch
                def step(self, df):
                    self.m[df["pid"]] += df["dur"]  # argmax needs a (value, by) pair

    def test_occupancy_requires_value_by_pair(self):
        with pytest.raises(jit.JitError):

            @jit.vfold
            class BadOcc:
                b = jit.map(key=jit.i64, value=jit.busy())

                @jit.each_batch
                def step(self, df):
                    self.b[df["pid"]] += df["dur"]  # occupancy needs (ts, dur)

    def test_scalar_reduction_rejects_a_pair(self):
        with pytest.raises(jit.JitError):

            @jit.vfold
            class BadPair:
                s = jit.map(key=jit.i64, value=jit.sum())

                @jit.each_batch
                def step(self, df):
                    self.s[df["pid"]] += df["ts"], df["dur"]  # sum takes one column

    def test_aug_rejected_on_a_non_additive_reduction(self):
        with pytest.raises(jit.JitError, match="non-additive"):

            @jit.plugin
            class Bad4:
                m = jit.map(key=jit.i64, value=jit.sumsq())

                @jit.each_event
                def step(self, e):
                    self.m[e.pid] += e.dur  # sumsq takes .observe(v), not +=

    def test_keyed_value_column_must_match_the_reduction(self):
        with pytest.raises(jit.JitError, match="numeric column"):

            @jit.vfold
            class BadCol:
                m = jit.map(key=jit.i64, value=jit.sum())

                @jit.each_batch
                def step(self, df):
                    self.m[df["pid"]] += df["name"]  # sum over a string column
