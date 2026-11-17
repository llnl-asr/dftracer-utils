# SPDX-License-Identifier: MIT
"""A group_by aggregate keeps the temporal type of its column where the
aggregate returns a value of that column (min, max, first, last, mode, sum of
a duration, mean of a timestamp), and the lazy plan schema after group_by is
filled from the key types and the aggregate rule."""

import pytest

import dftracer.utils as dft
from dftracer.utils import dftracer_utils_ext as ext
from dftracer.utils.enums import DType

VALUES = [10, 20, 30, 40, 50, 60]
KEYS = ["a", "a", "b", "b", "a", "b"]
UNIT_CODES = {"s": 0, "ms": 1, "us": 2, "ns": 3}


def _retype(kind, unit_code=2, zone=""):
    ints = dft.Series(VALUES, name="t")
    if kind == DType.DATE32:
        ints = ints.astype(DType.INT32)
    return ext._series_retype(ints, int(kind), unit_code, zone)


CASES = {
    **{f"ts_{u}": (lambda c=c: _retype(DType.TIMESTAMP, c)) for u, c in UNIT_CODES.items()},
    "ts_ns_utc": lambda: _retype(DType.TIMESTAMP, 3, "UTC"),
    "ts_ms_zone": lambda: _retype(DType.TIMESTAMP, 1, "America/New_York"),
    "date32": lambda: _retype(DType.DATE32, 0),
    "date64": lambda: _retype(DType.DATE64, 1),
    "duration_ms": lambda: _retype(DType.DURATION, 1),
    "duration_ns": lambda: _retype(DType.DURATION, 3),
}
SAME = ["min", "max", "first", "last"]


def _frame(name):
    return dft.DataFrame({"k": KEYS, "t": CASES[name]()})


def _params(s):
    return (int(s.type), s.time_unit, s.timezone)


@pytest.mark.parametrize("name", CASES)
@pytest.mark.parametrize("op", SAME)
def test_eager_agg_keeps_the_temporal_type(name, op):
    frame = _frame(name)
    out = frame.group_by("k").agg([f"{op}:t"])
    assert _params(out[f"{op}_t"]) == _params(frame["t"]), (name, op)


@pytest.mark.parametrize("name", CASES)
@pytest.mark.parametrize("op", SAME)
def test_lazy_agg_keeps_the_temporal_type_and_the_plan_says_so(name, op):
    frame = _frame(name)
    plan = frame.lazy().group_by("k").agg([f"{op}:t"])
    ((_, _, _, _), (col, kind, unit, zone)) = ext.LazyFrame.output_schema(plan)
    assert (kind, unit, zone) == _params(frame["t"]), (name, op)
    assert _params(plan.collect()[col]) == _params(frame["t"]), (name, op)


@pytest.mark.parametrize("name", ["ts_us", "ts_ns_utc"])
def test_lazy_dt_works_after_group_by(name):
    frame = _frame(name)
    plan = frame.lazy().group_by("k").agg(["max:t"])
    out = plan.with_column("y", dft.col("max_t").dt.year()).collect()
    assert out["y"].to_list() == [1970, 1970]


def test_the_plan_schema_after_group_by_is_filled():
    frame = _frame("ts_ms")
    plan = frame.lazy().group_by("k").agg(["max:t", "count:t:n"])
    schema = plan.schema
    assert list(schema) == ["k", "max_t", "n"]
    assert schema["max_t"] == DType.TIMESTAMP


def test_sum_of_a_duration_is_a_duration():
    frame = _frame("duration_ms")
    out = frame.group_by("k").agg(["sum:t"])
    assert _params(out["sum_t"]) == _params(frame["t"])
    assert sorted(out["sum_t"].astype(DType.INT64).to_list()) == [80, 130]


def test_mean_of_a_timestamp_is_a_timestamp():
    frame = _frame("ts_ms")
    out = frame.group_by("k").agg(["mean:t"])
    assert _params(out["mean_t"]) == _params(frame["t"])


def test_count_stays_an_integer():
    frame = _frame("ts_ms")
    out = frame.group_by("k").agg(["count:t"])
    assert out["count_t"].dtype == DType.INT64


def _null_group_frame():
    ints = dft.Series([10, 20, None, None], name="x")
    cols = {
        "k": ["a", "a", "b", "b"],
        "i": ints,
        "f": dft.Series([1.5, 2.5, None, None], name="x"),
        "s": dft.Series(["p", "q", None, None], name="x"),
        "ts": ext._series_retype(ints, int(DType.TIMESTAMP), 3, "UTC"),
        "dur": ext._series_retype(ints, int(DType.DURATION), 1, ""),
    }
    return dft.DataFrame(cols)


NULL_SPECS = [f"{op}:{c}" for op in ("first", "last") for c in ("i", "f", "s", "ts", "dur")] + [
    f"{op}:{c}" for op in ("min", "max", "mean") for c in ("i", "f", "ts", "dur")
]


def _rows(frame):
    names = list(frame.columns)
    keys = frame["k"].to_list()
    return {key: {n: frame[n].to_list()[i] for n in names} for i, key in enumerate(keys)}


def _check_null_group(frame):
    rows = _rows(frame)
    for name in frame.columns:
        if name == "k":
            continue
        assert rows["a"][name] is not None, name
        assert rows["b"][name] is None, (name, rows["b"][name])


def test_an_all_null_group_gives_null_eager():
    _check_null_group(_null_group_frame().group_by("k").agg(NULL_SPECS))


def test_an_all_null_group_gives_null_lazy():
    plan = _null_group_frame().lazy().group_by("k").agg(NULL_SPECS)
    _check_null_group(plan.collect())


def test_an_all_null_group_gives_null_after_a_spill():
    plan = _null_group_frame().lazy().memory_budget(1).group_by("k").agg(NULL_SPECS)
    _check_null_group(plan.collect())


def test_an_all_null_group_sums_to_zero_and_counts_zero():
    out = _null_group_frame().group_by("k").agg(["sum:i", "sum:f", "count_valid:i"])
    rows = _rows(out)
    assert rows["b"] == {"k": "b", "sum_i": 0, "sum_f": 0.0, "count_valid_i": 0}


def test_a_spilled_group_by_keeps_the_unit_and_zone_of_a_timestamp_key():
    n = 60_000
    key = ext._series_retype(
        dft.Series([i % 20_000 for i in range(n)], name="t"), int(DType.TIMESTAMP), 3, "UTC"
    )
    frame = dft.DataFrame({"t": key, "v": dft.Series([float(i) for i in range(n)], name="v")})
    out = frame.lazy().memory_budget(1 << 16).group_by("t").agg(["sum:v"]).collect()
    assert len(out) == 20_000
    assert _params(out["t"]) == _params(key)


@pytest.mark.parametrize("name", ["ts_ns_utc", "date32", "date64", "duration_ms"])
@pytest.mark.parametrize("op", ["min", "max", "first", "last"])
def test_the_reduce_shortcut_includes_temporal_columns(name, op):
    frame = _frame(name)
    want = _params(frame["t"])
    eager = getattr(frame.group_by("k"), op)()
    assert _params(eager["t"]) == want
    lazy = getattr(frame.lazy().group_by("k"), op)().collect()
    assert _params(lazy["t"]) == want
    assert eager["t"].astype(DType.INT64).to_list() == lazy["t"].astype(DType.INT64).to_list()


def test_the_reduce_shortcut_sum_and_mean_keep_a_duration_and_a_timestamp():
    dur = _frame("duration_ms")
    assert _params(dur.group_by("k").sum()["t"]) == _params(dur["t"])
    assert _params(dur.group_by("k").mean()["t"]) == _params(dur["t"])
    ts = _frame("ts_ms")
    assert _params(ts.group_by("k").mean()["t"]) == _params(ts["t"])


NS_BASE = 1_700_000_000_000_000_017
NS_ROWS = 1_000_000


def _ns_frame(arange):
    vals = [NS_BASE + (i if arange else 0) for i in range(NS_ROWS)]
    ints = dft.Series(vals, name="t")
    ts = ext._series_retype(ints, int(DType.TIMESTAMP), 3, "")
    return dft.DataFrame({"k": [0] * NS_ROWS, "t": ts, "i": ints})


@pytest.mark.parametrize("arange", [False, True])
@pytest.mark.parametrize("mode", ["eager", "lazy", "spill"])
def test_the_mean_of_nanosecond_values_is_exact(arange, mode):
    frame = _ns_frame(arange)
    specs = ["mean:t", "mean:i"]
    if mode == "eager":
        out = frame.group_by("k").agg(specs)
    else:
        plan = frame.lazy()
        if mode == "spill":
            plan = plan.memory_budget(1 << 16)
        out = plan.group_by("k").agg(specs).collect()
    want = NS_BASE + ((NS_ROWS - 1) // 2 if arange else 0)
    assert out["mean_t"].astype(DType.INT64).to_list() == [want]
    assert out["mean_t"].time_unit == 3
    # The Float64 mean rounds once, so it is within one ulp of the exact value.
    assert abs(out["mean_i"].to_list()[0] - want) <= 512
