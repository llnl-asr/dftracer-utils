# SPDX-License-Identifier: MIT
"""Time on the row index: resample, asfreq, at_time, between_time, tz_localize
and tz_convert on a Series and a DataFrame. Every case runs on real pandas and
on ours over the same index (unsorted, with repeated and null instants, one and
two levels, several time units, naive and UTC) and the values, labels, index
names and the error kinds are compared."""

import datetime
import os
import warnings

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series  # noqa: E402

from .common import skip_below_pandas3  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

NAN = float("nan")
VALGRIND = bool(os.environ.get("DFTRACER_UTILS_VALGRIND"))


def _vg(values, step, skip=()):
    """Every `step`-th value, and none of `skip`, under Valgrind, which runs a
    case about 100 times slower, so the whole grid would not end. All values
    everywhere else."""
    values = list(values)
    if not VALGRIND:
        return values
    return [v for v in values if v not in skip][::step]


PANDAS30 = pd.__version__.startswith("3.0.")
STAMPS = [
    "2024-01-01 00:10:00",
    "2024-01-01 00:40:00",
    "2024-01-01 02:15:00",
    "2024-01-01 02:20:00",
    "2024-01-02 09:05:00",
    "2024-01-04 23:59:59.5",
    "2024-02-03 10:00:00",
]
A = [1, 2, 3, 4, 5, 6, 7]
B = [NAN, 2.5, 3.0, NAN, 5.0, 6.0, 7.0]


def _index(stamps, unit="us", tz=None, name="t"):
    idx = pd.to_datetime(stamps, format="mixed").astype(f"datetime64[{unit}]")
    if tz:
        idx = idx.tz_localize(tz)
    return pd.Index(idx, name=name)


def _frame(index, **cols):
    return pd.DataFrame(cols, index=index)


SHUFFLED = [STAMPS[i] for i in (3, 0, 6, 1, 4, 2, 5)]
FRAMES = {
    "sorted": _frame(_index(STAMPS), a=A, b=B),
    "unsorted": _frame(_index(SHUFFLED), a=A, b=B),
    "repeated": _frame(_index([STAMPS[0], STAMPS[0], *STAMPS[2:]]), a=A, b=B),
    "unnamed": _frame(_index(STAMPS, name=None), a=A, b=B),
    "ms": _frame(_index(STAMPS, unit="ms"), a=A, b=B),
    "s": _frame(_index(STAMPS[:4] + STAMPS[4:5] + STAMPS[6:], unit="s"), a=A[:6], b=B[:6]),
    "ns": _frame(_index(STAMPS, unit="ns"), a=A, b=B),
    "utc": _frame(_index(STAMPS, tz="UTC"), a=A, b=B),
    "nat": _frame(_index([STAMPS[0], None, *STAMPS[2:]]), a=A, b=B),
}
SERIES = {k: f["a"] for k, f in FRAMES.items()}
TWO = _frame(
    pd.MultiIndex.from_arrays(
        [["x", "y", "x", "y", "x", "y", "x"], _index(STAMPS)], names=["g", "t"]
    ),
    a=A,
    b=B,
)


def _kind(e):
    for base in (KeyError, IndexError, NotImplementedError, TypeError, ValueError, AssertionError):
        if isinstance(e, base):
            return base.__name__
    return type(e).__name__


def _outcome(fn, obj):
    try:
        with warnings.catch_warnings():
            # Pandas says it ignores origin and offset for a day or a non-Tick rule.
            warnings.filterwarnings("ignore", message=".*keyword does not take effect.*")
            return fn(obj)
    except Exception as e:  # noqa: BLE001 - the kind is compared
        return e


def _norm(v):
    if isinstance(v, tuple):
        return tuple(_norm(i) for i in v)
    return None if v is None or v is pd.NaT or (isinstance(v, float) and v != v) else v


def _same(x, y):
    if isinstance(y, Exception):
        assert isinstance(x, Exception), f"ours returned, pandas raised {y!r}"
        assert _kind(x) == _kind(y), f"{x!r} vs {y!r}"
        return
    assert not isinstance(x, Exception), f"ours raised {x!r}"
    if not hasattr(y, "index"):
        return  # a resampler: no result to compare
    x = x.to_pandas() if hasattr(x, "to_pandas") else x
    if isinstance(y, pd.DataFrame) and isinstance(y.columns, pd.MultiIndex):
        y = y.set_axis(["_".join(c) for c in y.columns], axis=1)  # ours are flat
    assert list(x.index.names) == list(y.index.names)
    assert [_norm(i) for i in x.index.tolist()] == [_norm(i) for i in y.index.tolist()]
    if isinstance(y, pd.DataFrame):
        assert list(x.columns) == list(y.columns)
        pd.testing.assert_frame_equal(
            x,
            y,
            check_dtype=False,
            check_index_type=False,
            check_column_type=False,
            check_names=False,
            check_freq=False,
        )
    else:
        pd.testing.assert_series_equal(
            x, y, check_dtype=False, check_index_type=False, check_names=False, check_freq=False
        )
        assert x.name == y.name
    assert str(getattr(x.index, "tz", None)) == str(getattr(y.index, "tz", None))


def _check(fn, pandas_obj, ours_obj):
    skip_below_pandas3()
    _same(_outcome(fn, ours_obj), _outcome(fn, pandas_obj))


def _both(frames, name):
    p = frames[name]
    return p, (DataFrame.from_pandas(p) if isinstance(p, pd.DataFrame) else Series.from_pandas(p))


RULES = [
    "1h",
    "30min",
    "2h",
    "1D",
    "7min",
    "5s",
    "W",
    "W-WED",
    "ME",
    "MS",
    "QE",
    "QS",
    "YE",
    "YS",
    "2ME",
    "3D",
    "1h30min",
]
FUNCS = [
    "sum",
    "mean",
    "count",
    "min",
    "max",
    "first",
    "last",
    "std",
    "var",
    "median",
    "size",
    "nunique",
    "prod",
]


@pytest.mark.parametrize("name", _vg(["sorted", "unsorted", "utc"], 2))
# "5s" makes 570,000 bins, which Valgrind takes minutes to run.
@pytest.mark.parametrize("rule", _vg(RULES, 4, skip=("5s",)))
def test_resample_every_rule_and_aggregate(name, rule):
    p, o = _both(FRAMES, name)
    for fn in FUNCS:
        _check(lambda x, rule=rule, fn=fn: getattr(x.resample(rule), fn)(), p, o)


@pytest.mark.parametrize("name", _vg(["repeated", "unnamed", "ms", "s", "ns", "nat"], 5))
@pytest.mark.parametrize("rule", _vg(["1h", "7min", "1D", "W", "ME", "QS"], 4))
def test_resample_other_indexes(name, rule):
    p, o = _both(FRAMES, name)
    for fn in ("sum", "count", "first", "last", "mean", "size"):
        _check(lambda x, rule=rule, fn=fn: getattr(x.resample(rule), fn)(), p, o)


@pytest.mark.parametrize("name", _vg(["unsorted", "utc"], 2))
@pytest.mark.parametrize("rule", _vg(["1h", "1D", "W", "ME", "MS", "QE", "YE", "2ME"], 4))
@pytest.mark.parametrize("closed", _vg([None, "left", "right"], 2))
@pytest.mark.parametrize("label", _vg([None, "left", "right"], 2))
def test_resample_closed_and_label(name, rule, closed, label):
    _skip_pandas_day(name, rule)
    p, o = _both(FRAMES, name)
    for fn in ("sum", "count", "first"):
        _check(lambda x, fn=fn: getattr(x.resample(rule, closed=closed, label=label), fn)(), p, o)


def _skip_pandas_day(name, rule):
    """A day of a naive index is 24 hours from the origin in pandas 3.1;
    pandas 3.0 bins calendar days there and ignores origin and offset. Ours is
    pandas 3.1."""
    if rule == "1D" and PANDAS30:
        pytest.skip("pandas 3.0 bins a day of this index another way than 3.1")


ORIGINS = [
    "epoch",
    "start",
    "start_day",
    "end",
    "end_day",
    pd.Timestamp("2024-01-01 00:03:00"),
    datetime.datetime(2023, 12, 31, 23, 59, 30),
]
OFFSETS = [None, "30s", "7min", datetime.timedelta(minutes=-5), pd.Timedelta("1h")]


@pytest.mark.parametrize("name", _vg(["unsorted", "utc"], 2))
@pytest.mark.parametrize("rule", _vg(["1h", "7min", "1D"], 3))
@pytest.mark.parametrize("origin", _vg(ORIGINS, 4))
@pytest.mark.parametrize("offset", _vg(OFFSETS, 3))
def test_resample_origin_and_offset(name, rule, origin, offset):
    _skip_pandas_day(name, rule)
    p, o = _both(FRAMES, name)
    if name == "utc" and getattr(origin, "tzinfo", None) is None and not isinstance(origin, str):
        origin = pd.Timestamp(origin).tz_localize("UTC")
    for fn in ("sum", "count"):
        _check(lambda x, fn=fn: getattr(x.resample(rule, origin=origin, offset=offset), fn)(), p, o)


def test_resample_empty_bins_are_zero_or_missing():
    p, o = _both(FRAMES, "sorted")
    r = o.resample("1D")
    assert r.sum().to_pandas()["a"].tolist()[:4] == [10, 5, 0, 6]
    assert r.count().to_pandas()["a"].tolist()[:4] == [4, 1, 0, 1]
    assert r.size().to_pandas().tolist()[:4] == [4, 1, 0, 1]
    mean = r.mean().to_pandas()["a"].tolist()
    assert mean[:2] == [2.5, 5.0] and mean[2] != mean[2]
    assert r.first().to_pandas()["b"].isna().tolist()[:4] == [False, False, True, False]
    _check(lambda x: x.resample("1D").sum(), p, o)


@pytest.mark.parametrize("name", _vg(["sorted", "unsorted", "nat", "utc"], 3))
def test_resample_ohlc_and_agg_forms(name):
    p, o = _both(SERIES, name)
    for rule in ("1h", "1D", "ME"):
        _check(lambda x, rule=rule: x.resample(rule).ohlc(), p, o)
        _check(lambda x, rule=rule: x.resample(rule).agg("sum"), p, o)
        _check(lambda x, rule=rule: x.resample(rule).agg(["sum", "mean"]), p, o)
    pf, of = _both(FRAMES, name)
    _check(lambda x: x.resample("1D").agg({"a": "sum", "b": "mean"}), pf, of)
    _check(lambda x: x.resample("1D").agg({"a": ["sum", "max"]}), pf, of)
    _check(lambda x: x.resample("1D").agg(total=("a", "sum"), top=("b", "max")), pf, of)
    _check(lambda x: x.resample("1D").agg("mean"), pf, of)
    _check(lambda x: x.resample("1D")["a"].sum(), pf, of)
    _check(lambda x: x.resample("1D")[["a"]].mean(), pf, of)
    _check(lambda x: x.resample("1D")["b"].count(), pf, of)
    _check(lambda x: x.resample("1D").agg(["sum", "min"]), pf, of)


def test_resample_on_a_column_and_a_level():
    p = FRAMES["sorted"].reset_index()
    o = DataFrame.from_pandas(FRAMES["sorted"]).reset_index()
    for fn in ("sum", "count", "mean", "first"):
        _check(lambda x, fn=fn: getattr(x.resample("1D", on="t"), fn)(), p, o)
    _check(lambda x: x.resample("1h", on="t", closed="right", label="right").sum(), p, o)
    po, oo = TWO, DataFrame.from_pandas(TWO)
    for fn in ("sum", "count", "mean", "max"):
        _check(lambda x, fn=fn: getattr(x.resample("1D", level="t"), fn)(), po, oo)
        _check(lambda x, fn=fn: getattr(x.resample("1D", level=1), fn)(), po, oo)
    ps, os_ = TWO["a"], Series.from_pandas(TWO["a"])
    _check(lambda x: x.resample("1D", level="t").sum(), ps, os_)
    _check(lambda x: x.resample("1D", level=-1).count(), ps, os_)


ERROR_CALLS = [
    lambda x: x.resample("1D"),
    lambda x: x.resample("1D", level="nope"),
    lambda x: x.resample("1D", level=5),
    lambda x: x.resample("1D", level=-1),
    lambda x: x.resample("1D", level=0).sum(),
    lambda x: x.resample("1D", level="t").sum(),
    lambda x: x.resample("1D", on="nope"),
    lambda x: x.resample("1D", on="t", level="t"),
    lambda x: x.resample("bad").sum(),
    lambda x: x.resample("1D", closed="middle").sum(),
    lambda x: x.resample("1D", label="middle").sum(),
    lambda x: x.resample("1D", origin="nowhere").sum(),
    lambda x: x.resample("1D", offset="bad").sum(),
    lambda x: x.resample("1D").sum().resample("1h").sum(),
]


@pytest.mark.parametrize("which", _vg(range(len(ERROR_CALLS)), 5))
def test_resample_errors(which):
    plain = pd.DataFrame({"a": [1, 2, 3]})
    for p in (FRAMES["sorted"], plain, TWO, FRAMES["sorted"].reset_index()):
        if p is TWO and which in (0, 8, 9, 10, 11, 12, 13):
            continue  # pandas gives a ValueError about a backing array here
        _check(ERROR_CALLS[which], p, DataFrame.from_pandas(p))


def test_resample_refuses_what_pandas_refuses_with_a_clearer_error():
    two = DataFrame.from_pandas(TWO)
    with pytest.raises(TypeError, match="MultiIndex"):
        two.resample("1D")
    with pytest.raises(ValueError):
        DataFrame.from_pandas(FRAMES["sorted"]).resample("0s")


def test_resample_empty_frame():
    p = FRAMES["sorted"].iloc[0:0]
    o = DataFrame.from_pandas(p)
    assert len(o.resample("1h").sum()) == 0
    assert len(p.resample("1h").sum()) == 0


ASFREQ = [
    {},
    {"method": "ffill"},
    {"method": "bfill"},
    {"method": "pad"},
    {"method": "backfill"},
    {"fill_value": 0},
    {"fill_value": -1.5},
    {"normalize": True},
]
FREQS = ["1h", "30min", "10min", "7min", "1D", "ME", "MS", "W", "QE", "YS", "2D"]


@pytest.mark.parametrize("freq", _vg(FREQS, 5))
@pytest.mark.parametrize("kw", _vg(ASFREQ, 3))
def test_asfreq(freq, kw):
    for source in (FRAMES, SERIES):
        p, o = _both(source, "sorted")
        _check(lambda x: x.asfreq(freq, **kw), p, o)


@pytest.mark.parametrize("name", _vg(["unsorted", "ms", "utc", "ns"], 3))
@pytest.mark.parametrize("freq", _vg(["1D", "7min", "ME"], 3))
def test_asfreq_units_and_zones(name, freq):
    for source in (FRAMES, SERIES):
        p, o = _both(source, name)
        _check(lambda x: x.asfreq(freq), p, o)


@pytest.mark.parametrize("name", _vg(["repeated", "nat", "unnamed", "s"], 3))
@pytest.mark.parametrize("freq", _vg(["1h", "1D"], 2))
def test_asfreq_other_indexes(name, freq):
    for source in (FRAMES, SERIES):
        p, o = _both(source, name)
        _check(lambda x: x.asfreq(freq), p, o)
        _check(lambda x: x.asfreq(freq, method="ffill"), p, o)


def test_asfreq_keeps_the_nulls_of_rows_that_are_there():
    p, o = _both(FRAMES, "sorted")
    got = o.asfreq("1h", method="ffill").to_pandas()
    want = p.asfreq("1h", method="ffill")
    assert got["b"].isna().tolist() == want["b"].isna().tolist()
    _check(lambda x: x.asfreq("1h", fill_value=0.0), p, o)
    _check(lambda x: x.asfreq("1h", method="bfill", fill_value=9), p, o)


def test_asfreq_errors():
    ints = _frame(pd.Index([3, 1, 2], name="k"), a=[1, 2, 3])
    plain = pd.DataFrame({"a": [1, 2, 3]})
    for p in (FRAMES["sorted"], TWO, FRAMES["sorted"].iloc[0:0], ints, plain):
        _check(lambda x: x.asfreq("bad"), p, DataFrame.from_pandas(p))
    for p in (
        FRAMES["sorted"],
        TWO,
        FRAMES["sorted"].iloc[0:0],
        FRAMES["repeated"],
        FRAMES["unsorted"],
    ):
        o = DataFrame.from_pandas(p)
        _check(lambda x: x.asfreq("1h"), p, o)
        _check(lambda x: x.asfreq("1h", method="nearest"), p, o)
        _check(lambda x: x.asfreq("1h", method="ffill"), p, o)
        _check(lambda x: x.asfreq("1h", method="sideways"), p, o)
    for p in (ints, plain):  # pandas builds a date_range from the integers
        with pytest.raises(TypeError):
            DataFrame.from_pandas(p).asfreq("1h")


TIMES = [
    "00:10",
    "00:10:00",
    "02:15:00",
    "23:59:59",
    "09:05",
    "12:00AM",
    "12:10AM",
    "9:05AM",
    datetime.time(2, 15),
    datetime.time(23, 59, 59, 500000),
    datetime.time(0, 0),
    "25:00",
    "nine",
]


@pytest.mark.parametrize(
    "name", _vg(["sorted", "unsorted", "repeated", "ms", "utc", "nat", "ns", "s"], 4)
)
def test_at_time(name):
    for source in (FRAMES, SERIES):
        p, o = _both(source, name)
        for t in TIMES:
            _check(lambda x, t=t: x.at_time(t), p, o)


@pytest.mark.parametrize("name", _vg(["sorted", "unsorted", "repeated", "utc", "nat", "ms"], 4))
@pytest.mark.parametrize("inclusive", _vg(["both", "neither", "left", "right"], 3))
def test_between_time(name, inclusive):
    pairs = [
        ("00:10", "02:15"),
        ("02:15", "00:40"),
        ("00:00", "23:59:59"),
        ("23:59:59", "00:10"),
        ("09:05", "09:05"),
        (datetime.time(2, 15), datetime.time(23, 59, 59, 500000)),
        ("12:00AM", "9:05AM"),
    ]
    for source in (FRAMES, SERIES):
        p, o = _both(source, name)
        for lo, hi in pairs:
            _check(lambda x, lo=lo, hi=hi: x.between_time(lo, hi, inclusive=inclusive), p, o)


def test_time_of_day_errors():
    ints = _frame(pd.Index([3, 1, 2], name="k"), a=[1, 2, 3])
    plain = pd.DataFrame({"a": [1, 2, 3]})
    for p in (ints, plain, TWO):
        o = DataFrame.from_pandas(p)
        _check(lambda x: x.at_time("00:10"), p, o)
        _check(lambda x: x.between_time("00:10", "01:00"), p, o)
    p, o = _both(FRAMES, "sorted")
    _check(lambda x: x.between_time("00:10", "01:00", inclusive="middle"), p, o)
    with pytest.raises(TypeError):  # pandas leaks an AttributeError for this
        o.at_time(5)
    _check(lambda x: x.at_time("00:10", axis=3), p, o)


@pytest.mark.parametrize(
    "name",
    _vg(["sorted", "unsorted", "repeated", "unnamed", "ms", "ns", "s", "nat"], 4),
)
def test_tz_localize_and_convert_on_a_naive_index(name):
    for source in (FRAMES, SERIES):
        p, o = _both(source, name)
        _check(lambda x: x.tz_localize("UTC"), p, o)
        _check(lambda x: x.tz_localize(None), p, o)
        _check(lambda x: x.tz_localize("UTC", ambiguous="NaT", nonexistent="shift_forward"), p, o)
        _check(lambda x: x.tz_convert("UTC"), p, o)
        _check(lambda x: x.tz_localize("UTC").tz_localize("UTC"), p, o)


@pytest.mark.parametrize("name", _vg(["utc", "nat"], 2))
def test_tz_on_an_aware_index(name):
    p, o = _both(FRAMES if name == "utc" else {"nat": FRAMES["nat"].tz_localize("UTC")}, name)
    for source in (p,):
        pass
    for tz in ("UTC", None):
        _check(lambda x, tz=tz: x.tz_convert(tz), p, o)
        _check(lambda x, tz=tz: x.tz_localize(tz), p, o)
    _check(lambda x: x.tz_localize("UTC"), p, o)
    _check(lambda x: x.tz_localize(None).tz_localize("UTC"), p, o)
    _check(lambda x: x.tz_convert(None).tz_localize("UTC").tz_convert("UTC"), p, o)
    ps, os_ = p["a"], o["a"]
    _check(lambda x: x.tz_convert("UTC"), ps, os_)


def test_tz_on_a_level_and_errors():
    p, o = TWO, DataFrame.from_pandas(TWO)
    _check(lambda x: x.tz_localize("UTC", level="t"), p, o)
    _check(lambda x: x.tz_localize("UTC", level=1), p, o)
    _check(lambda x: x.tz_localize("UTC", level=0), p, o)
    _check(lambda x: x.tz_localize("UTC", level="nope"), p, o)
    _check(lambda x: x.tz_localize("UTC"), p, o)
    _check(lambda x: x.tz_convert("UTC", level="t"), p, o)
    _check(lambda x: x.tz_localize("UTC", level="t").tz_convert("UTC", level="t"), p, o)
    ints = _frame(pd.Index([3, 1, 2], name="k"), a=[1, 2, 3])
    plain = pd.DataFrame({"a": [1, 2, 3]})
    for q in (ints, plain, FRAMES["sorted"]):
        oq = DataFrame.from_pandas(q)
        _check(lambda x: x.tz_localize("UTC", level=0), q, oq)
        _check(lambda x: x.tz_localize("UTC", axis=1), q, oq)
        _check(lambda x: x.tz_convert("UTC", axis=1), q, oq)
        _check(lambda x: x.tz_localize("UTC", axis=7), q, oq)
        _check(lambda x: x.tz_convert("UTC"), q, oq)
        _check(lambda x: x.tz_localize("UTC"), q, oq)


@pytest.mark.parametrize("zone", _vg(["Europe/Paris", "America/New_York", "Asia/Kolkata"], 2))
@pytest.mark.parametrize("name", _vg(["sorted", "unsorted", "ms", "ns", "nat"], 4))
def test_tz_localize_and_convert_use_a_named_zone(zone, name):
    p, o = _both(FRAMES, name)
    _check(lambda x: x.tz_localize(zone), p, o)
    _check(lambda x: x.tz_localize(zone).tz_convert("UTC"), p, o)
    _check(lambda x: x.tz_localize(zone).tz_localize(None), p, o)
    _check(lambda x: x.tz_localize("UTC").tz_convert(zone), p, o)


def test_a_named_zone_reads_the_wall_time_where_wall_time_matters():
    p = FRAMES["utc"].tz_convert("Europe/Paris")
    o = DataFrame.from_pandas(p)
    for call in (
        lambda x: x.resample("1D").sum(),
        lambda x: x.resample("1h").sum(),
        lambda x: x.at_time("01:10"),
        lambda x: x.between_time("01:10", "03:00"),
        lambda x: x.asfreq("D"),
        lambda x: x.asfreq("1h", normalize=True),
    ):
        _check(call, p, o)


@pytest.mark.parametrize("level", _vg([0, -1, 1, 2, -3, "k", "t", "zz", None], 4))
@pytest.mark.parametrize("fn", _vg(["tz_localize", "tz_convert"], 2))
def test_tz_level_argument_on_every_kind_of_index(level, fn):
    frames = {
        "int": _frame(pd.Index([3, 1], name="k"), a=[1, 2]),
        "plain": pd.DataFrame({"a": [1, 2]}),
        "dt": FRAMES["sorted"],
        "two": TWO,
    }
    for p in frames.values():
        for source in (p, p["a"]):
            o = DataFrame.from_pandas(p) if source is p else Series.from_pandas(source)
            _check(lambda x: getattr(x, fn)("UTC", level=level), source, o)


EDGE_BASES = {
    "after": ["2024-02-01", "2024-01-18", "2024-04-01"],
    "before": ["1969-12-25", "1969-10-01", "1970-01-01"],
}


@pytest.mark.parametrize("unit", _vg(["ns", "us", "s"], 2))
@pytest.mark.parametrize("which", _vg(["after", "before"], 2))
@pytest.mark.parametrize("rule", _vg(["ME", "W-WED", "QE", "W", "MS", "YE"], 4))
def test_rows_on_a_bin_edge_and_one_tick_from_it(unit, which, rule):
    step = pd.Timedelta(1, unit=unit)
    stamps = []
    for base in EDGE_BASES[which]:
        edge = pd.Timestamp(base)
        stamps += [edge - step, edge, edge + step]
        stamps += [edge - pd.Timedelta(1, "D") - step, edge - pd.Timedelta(1, "D")]
    index = pd.DatetimeIndex(stamps).astype(f"datetime64[{unit}]")
    p = pd.DataFrame({"a": range(len(index)), "b": [float(i) for i in range(len(index))]})
    p.index = pd.Index(index, name="t")
    o = DataFrame.from_pandas(p)
    for fn in ("sum", "count", "first", "last"):
        for closed in (None, "left", "right"):
            _check(lambda x, fn=fn, c=closed: getattr(x.resample(rule, closed=c), fn)(), p, o)
        _check(lambda x, fn=fn: getattr(x["a"].resample(rule), fn)(), p, o)


UTC_ALIASES = [
    "UTC",
    "utc",
    "Etc/UTC",
    "UCT",
    "Etc/UCT",
    "Universal",
    "Etc/Universal",
    "Zulu",
    "Etc/Zulu",
    "GMT",
    "Etc/GMT",
    "GMT0",
    "Etc/GMT0",
    "Etc/GMT+0",
    "Etc/GMT-0",
    "GMT+0",
    "GMT-0",
    "Greenwich",
    "Etc/Greenwich",
]


@pytest.mark.parametrize("tz", _vg(UTC_ALIASES, 4))
def test_the_utc_aliases_pandas_accepts(tz):
    for source in (FRAMES, SERIES):
        p, o = _both(source, "sorted")
        _check(lambda x, tz=tz: x.tz_localize(tz), p, o)
        _check(lambda x, tz=tz: x.tz_localize("UTC").tz_convert(tz), p, o)
        pu, ou = _both(source, "utc")
        _check(lambda x, tz=tz: x.tz_convert(tz), pu, ou)


@pytest.mark.parametrize("tz", _vg(["bogus", "Mars/Olympus", "Z", "z", "", "UTC+1"], 3))
def test_an_unknown_zone_fails_at_the_call_as_pandas(tz):
    for source in (FRAMES, SERIES):
        for name in ("sorted", "utc"):
            p, o = _both(source, name)
            _check(lambda x, tz=tz: x.tz_localize(tz), p, o)
            _check(lambda x, tz=tz: x.tz_convert(tz), p, o)
    p, o = _both(FRAMES, "sorted")
    _check(lambda x, tz=tz: x.tz_localize("UTC").tz_convert(tz), p, o)


@pytest.mark.parametrize("offset", _vg(["-3min", "+3min", "-1h30min", " -30s", "-0s"], 3))
def test_a_negative_offset_parses(offset):
    for name in ("sorted", "utc"):
        p, o = _both(FRAMES, name)
        for rule in ("1h", "7min"):
            _check(lambda x, rule=rule, offset=offset: x.resample(rule, offset=offset).sum(), p, o)


@pytest.mark.parametrize(
    "origin",
    _vg(
        [
            pd.Timestamp("2024-01-01 00:03:00"),
            pd.Timestamp("2024-01-01 00:03:00", tz="UTC"),
            datetime.datetime(2024, 1, 1, 0, 3),
            "2024-01-01 00:03:00",
            "2024-01-01 00:03:00+00:00",
            "epoch",
        ],
        3,
    ),
)
@pytest.mark.parametrize("name", _vg(["sorted", "utc"], 2))
@pytest.mark.parametrize("rule", _vg(["7min", "1D", "ME"], 2))
def test_an_origin_with_another_zone_than_the_index_is_a_value_error(origin, name, rule):
    _skip_pandas_day(name, rule)
    if rule == "1D" and name == "utc":
        pytest.skip("a day of a zone-aware index ignores the origin in pandas 3")
    p, o = _both(FRAMES, name)
    _check(lambda x: x.resample(rule, origin=origin).sum(), p, o)


FRACTIONS = [
    "09:30",
    "09:30:00.5",
    "09:30:00.000001",
    "09:30:00.0000009",
    "09:30:00.000000001",
    "9:30:00.5PM",
    datetime.time(9, 30, 0, 1),
]


@pytest.mark.parametrize("unit", _vg(["ns", "us"], 2))
def test_fractional_times_compare_at_microseconds_as_pandas(unit):
    skip_below_pandas3()
    stamps = [
        "2024-01-01 09:30:00",
        "2024-01-01 09:30:00.000000001",
        "2024-01-01 09:30:00.0000009",
        "2024-01-02 09:30:00.5",
        "2024-01-02 09:29:59.999999999",
        "2024-01-03 09:30:00.000001",
    ]
    index = pd.DatetimeIndex(pd.to_datetime(stamps, format="mixed")).astype(f"datetime64[{unit}]")
    p = pd.Series(range(len(stamps)), index=index)
    o = Series.from_pandas(p)
    for t in FRACTIONS:
        _check(lambda x, t=t: x.at_time(t), p, o)
        for inclusive in ("both", "neither", "left", "right"):
            _check(lambda x, t=t, i=inclusive: x.between_time(t, "09:31", inclusive=i), p, o)
            _check(lambda x, t=t, i=inclusive: x.between_time("09:29", t, inclusive=i), p, o)


@pytest.mark.parametrize("freq", _vg(["1h", "30min", "10min", "1D"], 3))
def test_asfreq_on_a_key_that_already_is_the_grid(freq):
    stamps = pd.date_range("2024-01-01", periods=50, freq=freq).astype("datetime64[us]")
    for p in (
        pd.DataFrame(
            {"a": range(50), "b": [NAN if i % 3 == 0 else 1.5 for i in range(50)]}, index=stamps
        ),
        pd.Series(range(50), index=stamps),
    ):
        o = DataFrame.from_pandas(p) if isinstance(p, pd.DataFrame) else Series.from_pandas(p)
        _check(lambda x, f=freq: x.asfreq(f), p, o)
        _check(lambda x, f=freq: x.asfreq(f, method="ffill", normalize=True), p, o)
        # one row moved off the grid, or doubled, is not the grid
        for q in (p.iloc[[0, 2, 3]], p.iloc[[0, 1, 1, 2]]):
            oq = DataFrame.from_pandas(q) if isinstance(q, pd.DataFrame) else Series.from_pandas(q)
            _check(lambda x, f=freq: x.asfreq(f), q, oq)


@pytest.mark.parametrize("unit", _vg(["ns", "us", "s"], 2))
@pytest.mark.parametrize("method", _vg([None, "ffill", "pad", "bfill"], 3))
def test_asfreq_on_a_sparse_strictly_increasing_key(unit, method):
    keep = [0, 1, 4, 5, 6, 11, 12, 19]
    stamps = pd.date_range("2024-01-01", periods=20, freq="5s").astype(f"datetime64[{unit}]")[keep]
    frame = pd.DataFrame(
        {"a": range(8), "b": [NAN, 1.5, NAN, 2.5, 3.5, NAN, 4.5, 5.5], "c": list("abcdefgh")},
        index=stamps,
    )
    for p in (frame, frame["b"], frame.iloc[::-1]):
        o = DataFrame.from_pandas(p) if isinstance(p, pd.DataFrame) else Series.from_pandas(p)
        _check(lambda x, m=method: x.asfreq("5s", method=m), p, o)
        _check(lambda x, m=method: x.asfreq("5s", method=m, normalize=True), p, o)


def test_cut_reads_filtered_sliced_and_constant_columns_like_flat_ones():
    from dftracer.utils import DType

    frame = DataFrame.from_dict(
        {
            "a": Series.from_list(list(range(12)), DType.INT64),
            "f": Series.from_list([i + 0.5 for i in range(12)], DType.FLOAT64),
        }
    )
    breaks = Series.from_list([3, 6, 9], DType.INT64)
    float_breaks = Series.from_list([3.5, 6.5], DType.FLOAT64)
    kept = frame.filter(frame["a"].gt(2))
    parts = {
        "filtered": kept,
        "sliced": frame.slice(2, 7),
        "head": frame.head(8),
        "sorted": frame.sort("a", descending=True),
    }
    for name, part in parts.items():
        flat_a = Series.from_list(part["a"].to_list(), DType.INT64)
        flat_f = Series.from_list(part["f"].to_list(), DType.FLOAT64)
        assert part["a"].cut(breaks).to_list() == flat_a.cut(breaks).to_list(), name
        assert part["f"].cut(float_breaks).to_list() == flat_f.cut(float_breaks).to_list(), name
        assert part["a"].cut(float_breaks).to_list() == flat_a.cut(float_breaks).to_list(), name
    constant = frame["a"].full_like(4)
    assert constant.cut(breaks).to_list() == [1] * 12
    assert flat_breaks_ok(frame, kept)


def flat_breaks_ok(frame, kept):
    from dftracer.utils import DType

    wanted = kept["a"].head(3)
    flat = Series.from_list(wanted.to_list(), DType.INT64)
    return frame["a"].cut(wanted).to_list() == frame["a"].cut(flat).to_list()


@pytest.mark.parametrize(
    "stamps",
    _vg(
        [
            [pd.NaT, pd.NaT, pd.NaT],
            [pd.NaT],
            [pd.Timestamp("2024-01-01"), pd.NaT, pd.Timestamp("2024-01-02")],
            [pd.NaT, pd.Timestamp("2024-01-01"), pd.Timestamp("2024-01-03")],
        ],
        3,
    ),
)
def test_asfreq_refuses_only_an_index_that_is_all_nat(stamps):
    index = pd.DatetimeIndex(stamps).astype("datetime64[us]")
    for p in (
        pd.DataFrame({"a": range(len(stamps))}, index=index),
        pd.Series(range(len(stamps)), index=index),
    ):
        o = DataFrame.from_pandas(p) if isinstance(p, pd.DataFrame) else Series.from_pandas(p)
        _check(lambda x: x.asfreq("1D"), p, o)
