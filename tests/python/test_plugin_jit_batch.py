# SPDX-License-Identifier: MIT
"""@jit.each_batch on @jit.plugin: a body of whole-column operations (arithmetic,
comparisons, masks, ``s[mask]``, ``where``, ``abs``, ``clip``, ``isnull`` /
``fill_null``) reduced into plain scan state or a key-less ``jit.map``. Each
test compiles a plugin, runs it over a multi-file trace and compares the result
with the same computation in plain Python."""

import gzip
import os
import shutil

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins
from dftracer.utils.runtime import Runtime

pa = pytest.importorskip("pyarrow")

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)

_FILES = 4
_PER_FILE = 60


def _events():
    """(pid, ts, dur, v, f, w); w is None for every fourth event."""
    out = []
    for n in range(_FILES * _PER_FILE):
        out.append(
            (
                1 + n % 4,
                1000 + n,
                10 + (n * 7) % 53,
                (n * 5) % 17 - 8,
                (n % 11) * 0.5,
                None if n % 4 == 0 else n % 9,
            )
        )
    return out


def _write(path, events):
    with gzip.open(path, "wt", encoding="utf-8") as fh:
        for pid, ts, dur, v, f, w in events:
            w_arg = "" if w is None else f',"w":{w}'
            fh.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,"ts":{ts},'
                f'"dur":{dur},"ph":1,"args":{{"v":{v},"f":{f}{w_arg}}}}}\n'
            )


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("batch_trace")
    events = _events()
    for f in range(_FILES):
        _write(os.path.join(d, f"t{f}.pfw.gz"), events[f * _PER_FILE : (f + 1) * _PER_FILE])
    return str(d)


_UNEVEN = (10, 90, 37)


def _uneven_events():
    """Three files (so three batches) of 10, 90 and 37 events; the batches differ
    in size and in value, so a fold that is not the target's own merge shows."""
    out = []
    for f, size in enumerate(_UNEVEN):
        for i in range(size):
            n = len(out)
            dur = 10 if f == 0 else 100 if f == 1 else 5 + (n * 13) % 71
            out.append((1 + n % 4, 5000 + n, dur, (n * 3) % 11 - 5, 0.5 * (n % 7), 1 + n % 5))
    return out


@pytest.fixture(scope="module")
def uneven_trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("uneven_trace")
    events = _uneven_events()
    start = 0
    for f, size in enumerate(_UNEVEN):
        _write(os.path.join(d, f"t{f}.pfw.gz"), events[start : start + size])
        start += size
    return str(d)


def _run(cls, trace, threads=1):
    with Runtime(threads=threads) as rt:
        return Plugins([cls], runtime=rt).run(trace).results


def _value(results, name):
    cols = pa.table(results[name]).to_pydict()
    assert len(cols["value"]) == 1, cols
    return cols["value"][0]


def _values(results, names):
    return {n: _value(results, n) for n in names}


THREADS = [1, 4]


@pytest.mark.parametrize("threads", THREADS)
def test_arithmetic_and_reduction(trace, threads):
    @jit.plugin
    class Arith:
        ints: int = 0
        floats: float = 0.0
        mixed: float = 0.0

        @jit.each_batch
        def step(self, df):
            self.ints += (df["dur"] * 2 + df["ts"] - 5).sum()
            self.floats += (df["args.f"] * 2.0 + 1.0 - df["args.f"] / 2.0).sum()
            self.mixed += (-df["args.f"] + df["dur"] * 0.5).sum()

    assert Arith._jit_plugin.lowering == "batch"
    ev = _events()
    want = {
        "ints": sum(d * 2 + ts - 5 for _, ts, d, *_ in ev),
        "floats": sum(f * 2.0 + 1.0 - f / 2.0 for *_, f, _ in ev),
        "mixed": sum(-f + d * 0.5 for _, _, d, _, f, _ in ev),
    }
    assert _values(_run(Arith, trace, threads), want) == pytest.approx(want)


@pytest.mark.parametrize("threads", THREADS)
def test_comparisons_and_masks(trace, threads):
    @jit.plugin
    class Masks:
        gt: int = 0
        chain: int = 0
        both: int = 0
        either: int = 0
        neither: int = 0
        by_col: int = 0
        flipped: int = 0

        @jit.each_batch
        def step(self, df):
            dur = df["dur"]
            self.gt += (dur > 30).sum()
            self.chain += (20 <= dur < 40).sum()
            self.both += ((dur > 20) & (df["pid"] == 2)).sum()
            self.either += ((dur < 15) | (df["pid"] == 4)).sum()
            self.neither += (~((dur < 40) | (df["pid"] != 1))).sum()
            self.by_col += (dur > df["pid"] * 10).sum()
            self.flipped += (30 < dur).sum()

    ev = _events()
    want = {
        "gt": sum(d > 30 for _, _, d, *_ in ev),
        "chain": sum(20 <= d < 40 for _, _, d, *_ in ev),
        "both": sum(d > 20 and p == 2 for p, _, d, *_ in ev),
        "either": sum(d < 15 or p == 4 for p, _, d, *_ in ev),
        "neither": sum(not (d < 40 or p != 1) for p, _, d, *_ in ev),
        "by_col": sum(d > p * 10 for p, _, d, *_ in ev),
        "flipped": sum(30 < d for _, _, d, *_ in ev),
    }
    assert all(want.values())
    assert _values(_run(Masks, trace, threads), want) == want


@pytest.mark.parametrize("threads", THREADS)
def test_mask_filter_and_reductions(trace, threads):
    @jit.plugin
    class Filtered:
        total: float = 0.0
        n: int = 0
        hi = jit.map(key=(), value=jit.max(of=jit.i64))
        lo = jit.map(key=(), value=jit.min(of=jit.i64))

        @jit.each_batch
        def step(self, df):
            dur = df["dur"]
            big = dur[dur > 30]
            self.total += (big * 0.5).sum()
            self.n += big.count()
            self.hi[()].observe(big.max())
            self.lo[()].observe(df["ts"][dur > 30].min())

    ev = _events()
    sel = [(ts, d) for _, ts, d, *_ in ev if d > 30]
    want = {
        "total": sum(d * 0.5 for _, d in sel),
        "n": len(sel),
        "hi": max(d for _, d in sel),
        "lo": min(ts for ts, _ in sel),
    }
    assert _values(_run(Filtered, trace, threads), want) == pytest.approx(want)


@pytest.mark.parametrize("threads", THREADS)
def test_where_abs_clip(trace, threads):
    @jit.plugin
    class Elementwise:
        picked: int = 0
        mag: int = 0
        clipped: int = 0
        clipped_f: float = 0.0

        @jit.each_batch
        def step(self, df):
            v = df["args.v"]
            self.picked += v.where(v > 0, 0).sum()
            self.mag += abs(v).sum()
            self.mag += v.abs().sum()
            self.clipped += v.clip(-3, 4).sum()
            self.clipped_f += df["args.f"].clip(1.0, 3.5).sum()

    ev = _events()
    want = {
        "picked": sum(v if v > 0 else 0 for _, _, _, v, *_ in ev),
        "mag": 2 * sum(abs(v) for _, _, _, v, *_ in ev),
        "clipped": sum(min(max(v, -3), 4) for _, _, _, v, *_ in ev),
        "clipped_f": sum(min(max(f, 1.0), 3.5) for *_, f, _ in ev),
    }
    assert _values(_run(Elementwise, trace, threads), want) == pytest.approx(want)


@pytest.mark.parametrize("threads", THREADS)
def test_where_with_a_column_as_the_other_branch(trace, threads):
    @jit.plugin
    class WhereCol:
        total: int = 0

        @jit.each_batch
        def step(self, df):
            self.total += df["dur"].where(df["pid"] > 2, df["ts"]).sum()

    want = sum(d if p > 2 else ts for p, ts, d, *_ in _events())
    assert _value(_run(WhereCol, trace, threads), "total") == want


@pytest.mark.parametrize("threads", THREADS)
def test_isnull_and_fill_null(trace, threads):
    @jit.plugin
    class Nulls:
        missing: int = 0
        present: int = 0
        filled: int = 0
        w_sum: int = 0

        @jit.each_batch
        def step(self, df):
            w = df["args.w"]
            self.missing += w.isnull().sum()
            self.present += w.notnull().sum()
            self.filled += w.fill_null(100).sum()
            self.w_sum += w.sum()

    ev = _events()
    want = {
        "missing": sum(w is None for *_, w in ev),
        "present": sum(w is not None for *_, w in ev),
        "filled": sum(100 if w is None else w for *_, w in ev),
        "w_sum": sum(w for *_, w in ev if w is not None),
    }
    assert want["missing"] and want["present"]
    assert _values(_run(Nulls, trace, threads), want) == want


@pytest.mark.parametrize("threads", THREADS)
def test_every_reduction(trace, threads):
    @jit.plugin
    class Reductions:
        s: int = 0
        c: int = 0
        mn = jit.map(key=(), value=jit.min(of=jit.i64))
        mx = jit.map(key=(), value=jit.max(of=jit.i64))
        total = jit.map(key=(), value=jit.sum())

        @jit.each_batch
        def step(self, df):
            dur = df["dur"]
            self.s += dur.sum()
            self.c += dur.count()
            self.mn[()].observe(dur.min())
            self.mx[()].observe(dur.max())
            self.total[()] += dur.sum()

    durs = [d for _, _, d, *_ in _events()]
    want = {
        "s": sum(durs),
        "c": len(durs),
        "mn": min(durs),
        "mx": max(durs),
        "total": sum(durs),
    }
    assert _values(_run(Reductions, trace, threads), want) == pytest.approx(want)


def test_locals_are_shared_and_evaluated_once(trace):
    @jit.plugin
    class Shared:
        a: float = 0.0
        hi = jit.map(key=(), value=jit.max(of=jit.f64))

        @jit.each_batch
        def step(self, df):
            heavy = df["dur"] * 3.0 + df["ts"]
            keep = heavy[df["dur"] > 25]
            self.a += keep.sum()
            self.hi[()].observe(keep.max())

    keep = [d * 3.0 + ts for _, ts, d, *_ in _events() if d > 25]
    got = _run(Shared, trace)
    assert _value(got, "a") == pytest.approx(sum(keep))
    assert _value(got, "hi") == pytest.approx(max(keep))


def test_a_filtered_batch_that_matches_nothing_folds_zero_and_no_extreme(trace):
    @jit.plugin
    class Empty:
        n: int = 0
        hi = jit.map(key=(), value=jit.max(of=jit.i64))

        @jit.each_batch
        def step(self, df):
            none = df["dur"][df["dur"] > 10000]
            self.n += none.count()
            self.hi[()].observe(none.max())

    got = _run(Empty, trace)
    assert _value(got, "n") == 0
    assert pa.table(got["hi"]).to_pydict()["value"] == []


def test_batch_hook_matches_the_per_event_form(trace):
    @jit.plugin
    class PerBatch:
        total: float = 0.0
        n: int = 0

        @jit.each_batch
        def step(self, df):
            keep = df["dur"][(df["dur"] > 20) & (df["pid"] != 3)]
            self.total += (keep * 0.25).sum()
            self.n += keep.count()

    @jit.plugin
    class PerEvent:
        total: float = 0.0
        n: int = 0

        @jit.each_event
        def step(self, e):
            if e.dur > 20 and e.pid != 3:
                self.total += e.dur * 0.25
                self.n += 1

    a, b = _run(PerBatch, trace), _run(PerEvent, trace)
    assert _value(a, "n") == _value(b, "n")
    assert _value(a, "total") == pytest.approx(_value(b, "total"))


# Every reducer into every target it matches, over batches of different sizes,
# compared with plain Python over all the rows.


@pytest.mark.parametrize("threads", THREADS)
def test_each_reducer_into_its_matching_target_over_uneven_batches(uneven_trace, threads):
    @jit.plugin
    class Pairs:
        sum_int: int = 0
        sum_float: float = 0.0
        count_int: int = 0
        count_float: float = 0.0
        sum_desc = jit.map(key=(), value=jit.sum())
        count_desc = jit.map(key=(), value=jit.count())
        lo = jit.map(key=(), value=jit.min(of=jit.i64))
        hi = jit.map(key=(), value=jit.max(of=jit.i64))
        lo_f = jit.map(key=(), value=jit.min(of=jit.f64))
        hi_f = jit.map(key=(), value=jit.max(of=jit.f64))
        avg = jit.map(key=(), value=jit.mean())
        avg_masked = jit.map(key=(), value=jit.mean())

        @jit.each_batch
        def step(self, df):
            dur = df["dur"]
            self.sum_int += dur.sum()
            self.sum_float += dur.sum()
            self.count_int += dur.count()
            self.count_float += dur.count()
            self.sum_desc[()] += dur.sum()
            self.count_desc[()] += dur.count()
            self.lo[()].observe(dur.min())
            self.hi[()].observe(dur.max())
            self.lo_f[()].observe(df["args.f"].min())
            self.hi_f[()].observe(df["args.f"].max())
            self.avg[()].observe(dur.mean())
            self.avg_masked[()].observe(dur[dur > 20].mean())

    ev = _uneven_events()
    durs = [d for _, _, d, *_ in ev]
    fs = [f for *_, f, _ in ev]
    big = [d for d in durs if d > 20]
    want = {
        "sum_int": sum(durs),
        "sum_float": sum(durs),
        "count_int": len(durs),
        "count_float": len(durs),
        "sum_desc": sum(durs),
        "count_desc": len(durs),
        "lo": min(durs),
        "hi": max(durs),
        "lo_f": min(fs),
        "hi_f": max(fs),
        "avg": sum(durs) / len(durs),
        "avg_masked": sum(big) / len(big),
    }
    assert want["avg"] != pytest.approx((10 + 100 + sum(durs[100:]) / 37) / 3)
    assert _values(_run(Pairs, uneven_trace, threads), want) == pytest.approx(want)


_MISMATCH = [
    ("sum", "max", "self.m[()].observe(dur.sum())"),
    ("max", "sum", "self.s[()] += dur.max()"),
    ("max", "plain", "self.n += dur.max()"),
    ("min", "plain", "self.n += dur.min()"),
    ("min", "max", "self.m[()].observe(dur.min())"),
    ("mean", "plain", "self.n += dur.mean()"),
    ("mean", "sum", "self.s[()] += dur.mean()"),
    ("sum", "mean", "self.a[()].observe(dur.sum())"),
    ("count", "min", "self.lo[()].observe(dur.count())"),
]


@pytest.mark.parametrize("reducer,target,stmt", _MISMATCH)
def test_a_reduction_that_does_not_match_its_target_is_an_error(reducer, target, stmt):
    src = (
        "import dftracer.utils.jit as jit\n"
        "@jit.plugin\n"
        "class Bad:\n"
        "    n: float = 0.0\n"
        "    s = jit.map(key=(), value=jit.sum())\n"
        "    m = jit.map(key=(), value=jit.max(of=jit.i64))\n"
        "    lo = jit.map(key=(), value=jit.min(of=jit.i64))\n"
        "    a = jit.map(key=(), value=jit.mean())\n"
        "    @jit.each_batch\n"
        "    def step(self, df):\n"
        '        dur = df["dur"]\n'
        f"        {stmt}\n"
    )
    path = os.path.join(os.path.dirname(__file__), "_batch_mismatch_probe.py")
    ns: dict = {}
    import linecache

    linecache.cache[path] = (len(src), None, src.splitlines(True), path)
    with pytest.raises(jit.JitError, match="folds|not additive"):
        exec(compile(src, path, "exec"), ns)


@pytest.mark.parametrize("threads", THREADS)
def test_clip_with_a_float_bound_is_a_float(trace, threads):
    @jit.plugin
    class ClipFloat:
        halves: float = 0.0
        ints: float = 0.0

        @jit.each_batch
        def step(self, df):
            self.halves += df["args.v"].clip(1, 2.5).sum()
            self.ints += df["dur"].clip(20.5, 40).sum()

    ev = _events()
    want = {
        "halves": sum(min(max(v, 1), 2.5) for _, _, _, v, *_ in ev),
        "ints": sum(min(max(d, 20.5), 40) for _, _, d, *_ in ev),
    }
    assert _values(_run(ClipFloat, trace, threads), want) == pytest.approx(want)


def test_clip_with_a_float_bound_is_refused_by_an_int_field():
    with pytest.raises(jit.JitError, match="integer field and this reduction is a float"):

        @jit.plugin
        class ClipInt:
            total: int = 0

            @jit.each_batch
            def step(self, df):
                self.total += df["dur"].clip(1, 2.5).sum()


def test_missing_values_differ_between_the_forms(trace):
    # A missing args key is null in a batch hook (comparisons and sums skip it)
    # and reads as 0 in a per-event body.
    @jit.plugin
    class Batch:
        absent: int = 0
        zero: int = 0

        @jit.each_batch
        def step(self, df):
            w = df["args.w"]
            self.absent += w.isnull().sum()
            self.zero += (w == 0).sum()

    @jit.plugin
    class PerEvent:
        zero_or_absent: int = 0

        @jit.each_event
        def step(self, e):
            if e.arg_i64("w") == 0:
                self.zero_or_absent += 1

    ev = _events()
    absent = sum(w is None for *_, w in ev)
    zero = sum(w == 0 for *_, w in ev)
    assert absent and zero
    got = _run(Batch, trace)
    assert _values(got, ["absent", "zero"]) == {"absent": absent, "zero": zero}
    assert _value(_run(PerEvent, trace), "zero_or_absent") == absent + zero


def test_a_missing_column_does_not_stop_the_other_folds(trace):
    @jit.plugin
    class TwoFields:
        gone: int = 0
        total: int = 0

        @jit.each_batch
        def step(self, df):
            self.gone += df["args.nonexistent"].sum()
            self.total += df["dur"].sum()

    got = _run(TwoFields, trace)
    assert _value(got, "total") == sum(d for _, _, d, *_ in _events())
    assert sum(pa.table(got["gone"]).to_pydict()["value"]) == 0


@pytest.mark.parametrize("threads", THREADS)
def test_reflected_arithmetic_and_abs(trace, threads):
    @jit.plugin
    class Reflected:
        total: float = 0.0

        @jit.each_batch
        def step(self, df):
            dur = df["dur"]
            self.total += (2 * dur + (1 - dur) + 0.5 * dur + 10.0 / dur + abs(-dur)).sum()

    want = sum(2 * d + (1 - d) + 0.5 * d + 10.0 / d + abs(-d) for _, _, d, *_ in _events())
    assert _value(_run(Reflected, trace, threads), "total") == pytest.approx(want)


@pytest.mark.parametrize("threads", THREADS)
def test_unsigned_scan_columns_subtract_below_zero(trace, threads):
    @jit.plugin
    class Below:
        diff: int = 0
        neg: int = 0
        gap: int = 0

        @jit.each_batch
        def step(self, df):
            self.diff += (df["dur"] - 40).sum()
            self.neg += (df["dur"] - 40 < 0).sum()
            self.gap += (df["pid"] - df["dur"]).sum()

    ev = _events()
    want = {
        "diff": sum(d - 40 for _, _, d, *_ in ev),
        "neg": sum(d - 40 < 0 for _, _, d, *_ in ev),
        "gap": sum(p - d for p, _, d, *_ in ev),
    }
    assert want["diff"] < 0
    assert _values(_run(Below, trace, threads), want) == want
