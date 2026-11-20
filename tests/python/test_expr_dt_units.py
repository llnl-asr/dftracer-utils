# SPDX-License-Identifier: MIT
"""Expr ``dt`` ops read the unit of the column they run on, as the Series ones
do: date64 counts milliseconds, a timestamp its own unit."""

import datetime as dtm

import pytest

import dftracer.utils as dft
from dftracer.utils import dftracer_utils_ext as ext
from dftracer.utils._enums import DType

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

MS_PER_DAY = 86_400_000


def _date64():
    ints = dft.Series([0, 365 * MS_PER_DAY, None, -1, 400 * MS_PER_DAY + 5000], name="x")
    return ext._series_retype(ints, int(DType.DATE64), 1, "")


def _timestamp(unit_code, per_second):
    secs = [0, 365 * 86400, None, -1, 1_700_000_000]
    ints = dft.Series([None if v is None else v * per_second for v in secs], name="x")
    return ext._series_retype(ints, int(DType.TIMESTAMP), unit_code, "")


def _date32():
    ints = dft.Series([0, 365, None, -1, 400], name="x").astype(DType.INT32)
    return ext._series_retype(ints, int(DType.DATE32), 0, "")


COLUMNS = {
    "date64": _date64,
    "timestamp_s": lambda: _timestamp(0, 1),
    "timestamp_ms": lambda: _timestamp(1, 1_000),
    "timestamp_us": lambda: _timestamp(2, 1_000_000),
    "timestamp_ns": lambda: _timestamp(3, 1_000_000_000),
}


@pytest.mark.parametrize("name", COLUMNS)
@pytest.mark.parametrize("rule", ["1h", "1d", "30min"])
def test_expr_round_and_truncate_match_the_series_ones(name, rule):
    s = COLUMNS[name]()
    frame = dft.DataFrame({"x": s})
    for expr_name, series_name in (("truncate", "floor"), ("round", "round")):
        got = getattr(dft.col("x").dt, expr_name)(rule).apply(frame)
        want = getattr(s.dt, series_name)(rule)
        assert got.to_list() == want.to_list(), (name, rule, expr_name)


def test_a_date64_column_one_year_after_the_epoch_stays_on_that_day():
    frame = dft.DataFrame({"x": _date64()})
    got = dft.col("x").dt.round("1h").apply(frame).to_list()
    assert got[1] == dtm.date(1971, 1, 1)


@pytest.mark.parametrize("name", [*COLUMNS, "date32"])
def test_expr_parts_match_the_series_ones(name):
    s = _date32() if name == "date32" else COLUMNS[name]()
    frame = dft.DataFrame({"x": s})
    for part in ("year", "month", "day", "hour", "minute", "second", "quarter"):
        got = getattr(dft.col("x").dt, part)().apply(frame).to_list()
        assert got == getattr(s.dt, part).to_list(), (name, part)
    got = dft.col("x").dt.strftime("%Y-%m-%d %H:%M:%S").apply(frame).to_list()
    assert got == s.dt.strftime("%Y-%m-%d %H:%M:%S").to_list()


def test_the_lazy_plan_rounds_a_date64_column_like_the_eager_one():
    frame = dft.DataFrame({"x": _date64()})
    eager = dft.col("x").dt.round("1h").apply(frame).to_list()
    lazy = frame.lazy().with_column("y", dft.col("x").dt.round("1h")).collect()["y"].to_list()
    assert lazy == eager
