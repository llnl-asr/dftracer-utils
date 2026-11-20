# SPDX-License-Identifier: MIT
"""A lazy plan keeps the unit and the zone of a temporal column in its schema,
and the Expr ``dt`` ops on a plan read them, so a plan gives what the eager
frame gives; ``dt.epoch`` returns the unit asked for."""

import pytest

import dftracer.utils as dft
from dftracer.utils import dftracer_utils_ext as ext
from dftracer.utils.enums import DType

MS_PER_DAY = 86_400_000
SECONDS = [0, 365 * 86400, None, -1, 1_700_000_000, 59 * 86400 + 3601]


def _timestamp(unit_code, per_second, zone=""):
    ints = dft.Series([None if v is None else v * per_second for v in SECONDS], name="x")
    return ext._series_retype(ints, int(DType.TIMESTAMP), unit_code, zone)


def _date64():
    ints = dft.Series([None if v is None else v * 1000 for v in SECONDS], name="x")
    return ext._series_retype(ints, int(DType.DATE64), 1, "")


def _date32():
    days = [None if v is None else v // 86400 for v in SECONDS]
    return ext._series_retype(
        dft.Series(days, name="x").astype(DType.INT32), int(DType.DATE32), 0, ""
    )


COLUMNS = {
    "ts_s": (lambda: _timestamp(0, 1), "s"),
    "ts_ms": (lambda: _timestamp(1, 1_000), "ms"),
    "ts_us": (lambda: _timestamp(2, 1_000_000), "us"),
    "ts_ns": (lambda: _timestamp(3, 1_000_000_000), "ns"),
    "date64": (_date64, "ms"),
}
UNIT_CODES = {"s": 0, "ms": 1, "us": 2, "ns": 3}


def _lazy(frame, expr):
    return frame.lazy().with_column("y", expr).collect()["y"].to_list()


@pytest.mark.parametrize("name", COLUMNS)
def test_the_plan_schema_carries_the_unit_and_the_zone(name):
    s = COLUMNS[name][0]()
    lf = dft.DataFrame({"x": s}).lazy()
    ((column, kind, unit, zone),) = ext.LazyFrame.output_schema(lf)
    assert (column, kind, unit, zone) == ("x", int(s.type), s.time_unit, s.timezone)
    moved = lf.with_column("z", dft.col("x")).select(["z"])
    ((_, kind, unit, _),) = ext.LazyFrame.output_schema(moved)
    assert (kind, unit) == (int(s.type), s.time_unit)


def test_the_plan_schema_carries_a_zone():
    s = _timestamp(3, 1_000_000_000, "UTC")
    lf = dft.DataFrame({"x": s}).lazy()
    ((_, _, unit, zone),) = ext.LazyFrame.output_schema(lf)
    assert (unit, zone) == (3, "UTC")


@pytest.mark.parametrize("name", COLUMNS)
@pytest.mark.parametrize("rule", ["1h", "1d", "30min"])
def test_a_lazy_round_and_truncate_equal_the_eager_ones(name, rule):
    s = COLUMNS[name][0]()
    frame = dft.DataFrame({"x": s})
    for method, series_method in (("truncate", "floor"), ("round", "round")):
        expr = getattr(dft.col("x").dt, method)(rule)
        want = getattr(s.dt, series_method)(rule).to_list()
        assert expr.apply(frame).to_list() == want
        assert _lazy(frame, expr) == want, (name, rule, method)


@pytest.mark.parametrize("name", COLUMNS)
def test_lazy_parts_and_strftime_equal_the_eager_ones(name):
    s = COLUMNS[name][0]()
    frame = dft.DataFrame({"x": s})
    for part in ("year", "month", "day", "hour", "minute", "second", "quarter", "week"):
        expr = getattr(dft.col("x").dt, part)()
        assert _lazy(frame, expr) == expr.apply(frame).to_list(), (name, part)
    expr = dft.col("x").dt.strftime("%Y-%m-%d %H:%M:%S")
    assert _lazy(frame, expr) == s.dt.strftime("%Y-%m-%d %H:%M:%S").to_list()


PER_SECOND = {"ns": 10**9, "us": 10**6, "ms": 10**3, "s": 1}


def _expected_epoch(unit):
    """The whole seconds the columns hold, in ``unit``; a day floors."""
    if unit == "d":
        return [None if v is None else v // 86400 for v in SECONDS]
    return [None if v is None else v * PER_SECOND[unit] for v in SECONDS]


@pytest.mark.parametrize("name", [n for n in COLUMNS if n != "date64"])
@pytest.mark.parametrize("unit", ["s", "ms", "us", "ns", "d"])
def test_epoch_returns_the_unit_asked_for(name, unit):
    frame = dft.DataFrame({"x": COLUMNS[name][0]()})
    expr = dft.col("x").dt.epoch(unit)
    assert expr.apply(frame).to_list() == _expected_epoch(unit)
    assert _lazy(frame, expr) == _expected_epoch(unit)


@pytest.mark.parametrize("unit", ["s", "ms", "us", "ns", "d"])
@pytest.mark.parametrize("make", [_date64, _date32])
def test_epoch_of_a_date_column_is_in_the_unit_asked_for(make, unit):
    frame = dft.DataFrame({"x": make()})
    expr = dft.col("x").dt.epoch(unit)
    days = [None if v is None else v // 86400 for v in SECONDS]
    if make is _date32:
        scale = 1 if unit == "d" else 86400 * PER_SECOND[unit]
        want = [None if d is None else d * scale for d in days]
    else:
        # date64 ticks are milliseconds: the seconds the column holds
        want = _expected_epoch(unit)
    assert expr.apply(frame).to_list() == want
    assert _lazy(frame, expr) == want


def test_epoch_defaults_to_microseconds_and_refuses_an_unknown_unit():
    s = _timestamp(3, 1_000_000_000)
    frame = dft.DataFrame({"x": s})
    assert dft.col("x").dt.epoch().apply(frame).to_list() == _expected_epoch("us")
    with pytest.raises(ValueError, match="time_unit"):
        dft.col("x").dt.epoch("h")


def test_an_int64_column_is_read_in_the_unit_the_caller_names():
    frame = dft.DataFrame({"x": dft.Series([1_000_000, 2_000_000, None], name="x")})
    expr = dft.col("x").dt("ns").epoch("us")
    assert expr.apply(frame).to_list() == [1000, 2000, None]
    assert _lazy(frame, expr) == [1000, 2000, None]
