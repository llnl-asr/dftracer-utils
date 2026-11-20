# SPDX-License-Identifier: MIT
"""``dt.as_unit`` / ``cast_time_unit``, ``dt.date``, ``dt.time`` and
``dt.timetz`` against real pandas and Polars: native kernels that keep the
instant across units and split a timestamp into a date32 and a time of day, on
nulls, negative ticks (floor, not truncate), empty columns and every unit."""

import datetime as dtm

import pytest

import dftracer.utils as dft
from dftracer.utils import dftracer_utils_ext as ext
from dftracer.utils._enums import DType

pytest.importorskip("dftracer.utils.dftracer_utils_ext")
np = pytest.importorskip("numpy")

UNITS = ("s", "ms", "us", "ns")
POLARS_UNITS = ("ms", "us", "ns")  # polars has no second unit
CODE = {u: i for i, u in enumerate(UNITS)}
PER_SECOND = {"s": 1, "ms": 1_000, "us": 1_000_000, "ns": 1_000_000_000}

# Ticks that cross zero, sit one tick off a day edge, and carry a null.
SECONDS = [0, 1, -1, 86_399, 86_400, -86_400, -86_401, 1_700_000_000, None, 951_782_400]
SUB = [0, 1, -1, 999, -999, 1_500, -1_500, None]


def _ticks(unit, values=None):
    if values is not None:
        return values
    out = []
    for v in SECONDS:
        out.append(
            None if v is None else v * PER_SECOND[unit] + (-7 if unit != "s" and v % 2 else 3)
        )
    return out


def _ours(ticks, unit, kind="timestamp", tz=""):
    s = dft.Series(ticks, name="x").astype("int64")
    code = DType.TIMESTAMP if kind == "timestamp" else DType.DURATION
    return ext._series_retype(s, int(code), CODE[unit], tz)


def _np_ticks(ticks, unit, kind="timestamp"):
    make = np.datetime64 if kind == "timestamp" else np.timedelta64
    return np.array([make("NaT", unit) if v is None else make(v, unit) for v in ticks])


def _as_ticks(series):
    return [None if v is None else int(v) for v in series.astype("int64").to_list()]


def _pd_ticks(ser):
    arr = ser.to_numpy().view("int64")
    mask = ser.isna().to_numpy()
    return [None if m else int(v) for v, m in zip(arr, mask)]


@pytest.mark.parametrize("src", UNITS)
@pytest.mark.parametrize("dst", UNITS)
@pytest.mark.parametrize("kind", ["timestamp", "duration"])
def test_as_unit_matches_pandas(src, dst, kind):
    pd = pytest.importorskip("pandas")
    ticks = _ticks(src)
    if PER_SECOND[dst] > PER_SECOND[src]:
        # Keep the scaled values inside int64 for the finer unit.
        ticks = [None if v is None else v % 10**9 - 5 * 10**8 for v in ticks]
    ours = _ours(ticks, src, kind)
    ser = pd.Series(_np_ticks(ticks, src, kind))
    got = ours.dt.as_unit(dst)
    assert _as_ticks(got) == _pd_ticks(ser.dt.as_unit(dst))
    assert got.time_unit == CODE[dst]
    assert got.name == "x"
    assert str(got.dtype) == str(ours.dtype)


@pytest.mark.parametrize("src", POLARS_UNITS)
@pytest.mark.parametrize("dst", POLARS_UNITS)
def test_cast_time_unit_matches_polars_on_a_timestamp(src, dst):
    pl = pytest.importorskip("polars")
    ticks = _ticks(src)
    if PER_SECOND[dst] > PER_SECOND[src]:
        ticks = [None if v is None else v % 10**9 - 5 * 10**8 for v in ticks]
    ours = _ours(ticks, src).dt.cast_time_unit(dst)
    want = pl.Series(ticks, dtype=pl.Datetime(src)).dt.cast_time_unit(dst).cast(pl.Int64).to_list()
    assert _as_ticks(ours) == want
    frame = dft.DataFrame({"x": _ours(ticks, src)})
    lazy = frame.select(dft.col("x").dt.cast_time_unit(dst).alias("x"))
    assert _as_ticks(lazy["x"]) == want


@pytest.mark.parametrize("src", ["us", "ns"])
@pytest.mark.parametrize("dst", ["ms"])
def test_cast_time_unit_truncates_a_duration_as_polars_and_as_unit_floors(src, dst):
    pl = pytest.importorskip("polars")
    pd = pytest.importorskip("pandas")
    ticks = SUB[:-1] + [None]
    ours = _ours(ticks, src, "duration")
    want = pl.Series(ticks, dtype=pl.Duration(src)).dt.cast_time_unit(dst).cast(pl.Int64).to_list()
    assert _as_ticks(ours.dt.cast_time_unit(dst)) == want
    floored = _pd_ticks(pd.Series(_np_ticks(ticks, src, "duration")).dt.as_unit(dst))
    assert _as_ticks(ours.dt.as_unit(dst)) == floored
    assert want != floored


def test_as_unit_keeps_the_zone_and_rejects_other_columns_and_units():
    ours = _ours([1, None, -1], "us", tz="UTC")
    got = ours.dt.as_unit("ms")
    assert got.timezone == "UTC"
    with pytest.raises(ValueError, match="unit must be one of"):
        ours.dt.as_unit("minutes")
    with pytest.raises(dft.errors.DFTUtilsValueError, match="not a Timestamp or Duration"):
        dft.Series([1, 2, 3]).dt.as_unit("ms")
    assert _as_ticks(_ours([], "us").dt.as_unit("ns")) == []


def test_as_unit_overflow_is_an_error_not_a_wrap():
    pd = pytest.importorskip("pandas")
    ticks = [10**12, 5]
    with pytest.raises(ValueError):
        pd.Series(_np_ticks(ticks, "s")).dt.as_unit("ns")
    with pytest.raises(dft.errors.DFTUtilsValueError, match="does not fit"):
        _ours(ticks, "s").dt.as_unit("ns")
    frame = dft.DataFrame({"x": _ours(ticks, "s")})
    with pytest.raises(Exception, match="dt_as_unit"):
        frame.select(dft.col("x").dt.cast_time_unit("ns").alias("x"))
    # The same column in a coarser unit is fine.
    assert _as_ticks(_ours(ticks, "s").dt.as_unit("s")) == ticks


YEAR_5000_US = 95_617_584_000_000_000


def test_overflow_gives_the_same_reason_on_every_path():
    ours = _ours([YEAR_5000_US], "us")
    frame = dft.DataFrame({"x": ours})
    expr = dft.col("x").dt.cast_time_unit("ns").alias("y")
    reason = "a value does not fit 64 bits in the finer unit"
    with pytest.raises(Exception, match=reason):
        ours.dt.cast_time_unit("ns")
    with pytest.raises(Exception, match=reason):
        ours.dt.as_unit("ns")
    with pytest.raises(Exception, match=reason):
        frame.select(expr)
    with pytest.raises(Exception, match=reason):
        frame.lazy().select(expr).collect()


def test_cast_time_unit_refuses_seconds_as_polars_does():
    ours = _ours([1], "us")
    message = "`time_unit` must be one of {'ns', 'us', 'ms'}, got s"
    with pytest.raises(ValueError) as series_error:
        ours.dt.cast_time_unit("s")
    with pytest.raises(ValueError) as expr_error:
        dft.col("x").dt.cast_time_unit("s")
    assert str(series_error.value) == str(expr_error.value) == message
    pl = pytest.importorskip("polars")
    with pytest.raises(ValueError) as polars_error:
        pl.col("x").dt.cast_time_unit("s")
    assert str(polars_error.value) == message
    assert _as_ticks(ours.dt.as_unit("s")) == [0]


def _np_dates(ticks, unit):
    per_day = 86_400 * PER_SECOND[unit]
    return [
        None if v is None else dtm.date(1970, 1, 1) + dtm.timedelta(days=int(v // per_day))
        for v in ticks
    ]


def _np_times(ticks, unit):
    per_day = 86_400 * PER_SECOND[unit]
    return [None if v is None else int(v % per_day) for v in ticks]


@pytest.mark.parametrize("unit", UNITS)
def test_date_matches_pandas(unit):
    pd = pytest.importorskip("pandas")
    ticks = _ticks(unit)
    got = _ours(ticks, unit).dt.date
    assert got.dtype == DType.DATE32
    assert got.name == "x"
    assert got.to_list() == _np_dates(ticks, unit)
    want = pd.Series(_np_ticks(ticks, unit)).dt.date.tolist()
    assert got.to_list() == [None if w is pd.NaT else w for w in want]


@pytest.mark.parametrize("unit", POLARS_UNITS)
def test_date_matches_polars_as_a_series_and_an_expression(unit):
    pl = pytest.importorskip("polars")
    ticks = _ticks(unit)
    ours = _ours(ticks, unit)
    pl_want = pl.Series(ticks, dtype=pl.Datetime(unit)).dt.date().to_list()
    assert ours.dt.date.to_list() == pl_want
    frame = dft.DataFrame({"x": ours})
    assert frame.select(dft.col("x").dt.date().alias("d"))["d"].to_list() == pl_want


@pytest.mark.parametrize("unit", UNITS)
def test_time_matches_pandas(unit):
    ticks = _ticks(unit)
    ours = _ours(ticks, unit)
    got = ours.dt.time
    assert got.dtype == (DType.TIME32 if unit in ("s", "ms") else DType.TIME64)
    assert got.time_unit == CODE[unit]
    assert _as_ticks(got.astype("int64")) == _np_times(ticks, unit)
    expr = dft.DataFrame({"x": ours}).select(dft.col("x").dt.time().alias("x"))["x"]
    for same in (ours.dt.timetz, expr):
        assert _as_ticks(same.astype("int64")) == _np_times(ticks, unit)
    if unit != "ns":
        pd = pytest.importorskip("pandas")
        want = pd.Series(_np_ticks(ticks, unit)).dt.time.tolist()
        assert got.to_list() == [None if w is pd.NaT else w for w in want]


@pytest.mark.parametrize("unit", POLARS_UNITS)
def test_time_matches_polars(unit):
    pl = pytest.importorskip("polars")
    ticks = _ticks(unit)
    pl_want = pl.Series(ticks, dtype=pl.Datetime(unit)).dt.time().to_list()
    assert _ours(ticks, unit).dt.time.to_list() == pl_want


def test_timetz_of_a_utc_column_is_the_time_of_day_without_the_tzinfo():
    pd = pytest.importorskip("pandas")
    ticks = [0, 86_400_000_000 - 1, None, -1]
    ours = _ours(ticks, "us", tz="UTC")
    want = pd.Series(_np_ticks(ticks, "us")).dt.tz_localize("UTC").dt.timetz.tolist()
    assert ours.dt.timetz.to_list() == [
        None if w is pd.NaT else w.replace(tzinfo=None) for w in want
    ]


def test_date_and_time_on_empty_and_all_null_columns():
    for ticks in ([], [None, None]):
        ours = _ours(ticks, "us")
        assert ours.dt.date.to_list() == [None] * len(ticks)
        assert ours.dt.time.to_list() == [None] * len(ticks)
        assert len(ours.dt.date) == len(ticks)


def test_date_and_time_refuse_other_columns():
    with pytest.raises(dft.errors.DFTUtilsValueError, match="dt_date: not a Timestamp"):
        dft.Series([1, 2]).dt.date
    with pytest.raises(dft.errors.DFTUtilsValueError, match="dt_time: not a Timestamp"):
        dft.Series([1, 2]).dt.time
    date = dft.Series([1, 2]).astype("int32")
    date = ext._series_retype(date, int(DType.DATE32), 0, "")
    with pytest.raises(dft.errors.DFTUtilsValueError, match="dt_time: not a Timestamp"):
        date.dt.time
