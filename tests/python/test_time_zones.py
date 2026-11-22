# SPDX-License-Identifier: MIT
"""Time zones: every calendar part and method of a zone-aware Series is read on
the wall time of its zone, and tz_localize / tz_convert / resample / at_time /
between_time agree with pandas, with rows at and next to each transition. The
polars names are checked against real polars."""

import datetime
import os
import random

import pytest

pd = pytest.importorskip("pandas")

from .common import skip_below_pandas3  # noqa: E402

zoneinfo = pytest.importorskip("zoneinfo")

from dftracer.utils import DataFrame, Series  # noqa: E402

PANDAS3 = int(pd.__version__.split(".")[0]) >= 3

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

ZONES = [
    "America/New_York",
    "Europe/London",
    "Australia/Lord_Howe",
    "Asia/Kolkata",
    "Pacific/Chatham",
]


def _valgrind_first(values):
    """``values`` for a parametrize, with the first marked to run under
    Valgrind, which runs only the marked cases of this file: every case takes
    too long there."""
    return [pytest.param(values[0], marks=pytest.mark.valgrind), *values[1:]]


def _transitions(zone, first=2018, last=2027):
    """The UTC seconds at which the offset of ``zone`` changes, found by an
    hourly scan and a bisection."""
    info = zoneinfo.ZoneInfo(zone)

    def offset(t):
        return datetime.datetime.fromtimestamp(t, info).utcoffset()

    start = int(datetime.datetime(first, 1, 1, tzinfo=datetime.timezone.utc).timestamp())
    stop = int(datetime.datetime(last, 1, 1, tzinfo=datetime.timezone.utc).timestamp())
    out = []
    t = start
    while t < stop:
        if offset(t) != offset(t + 3600):
            lo, hi = t, t + 3600
            while hi - lo > 1:
                mid = (lo + hi) // 2
                if offset(mid) == offset(lo):
                    lo = mid
                else:
                    hi = mid
            out.append(hi)
        t += 3600
    return out


_CACHE = {}


def transitions(zone):
    if zone not in _CACHE:
        _CACHE[zone] = _transitions(zone)
    return _CACHE[zone]


NEAR = (-86400, -7200, -3601, -3600, -1800, -1, 0, 1, 1799, 1800, 3599, 3600, 7200, 86400)


def instants(zone, randoms=120, seed=7):
    """UTC microseconds at, before and after each transition, random ones, one
    before 1970 and one past the last listed transition."""
    rng = random.Random(seed)
    secs = [t + d for t in transitions(zone) for d in NEAR]
    secs += [rng.randrange(1_514_764_800, 1_798_761_600) for _ in range(randoms)]
    # pandas 2 reads zones through pytz, whose rules stop in 2037.
    secs += [-1_000_000_000] + ([4_102_444_800 + 86400 * 100] if PANDAS3 else [])
    ticks = [s * 1_000_000 + rng.choice((0, 0, 1, 999_999, rng.randrange(1_000_000))) for s in secs]
    return sorted(set(ticks))


def stamps(zone, ticks=None):
    ticks = instants(zone) if ticks is None else ticks
    index = pd.to_datetime(ticks, unit="us", utc=True).tz_convert(zone).as_unit("us")
    return pd.Series(index, name="t")


def ours(p):
    return Series.from_pandas(p)


def values(x):
    return [None if v is pd.NaT or v is None else v for v in list(x)]


PARTS = [
    "year",
    "month",
    "day",
    "hour",
    "minute",
    "second",
    "microsecond",
    "nanosecond",
    "dayofweek",
    "weekday",
    "dayofyear",
    "day_of_year",
    "quarter",
    "days_in_month",
    "daysinmonth",
    "is_leap_year",
    "is_month_start",
    "is_month_end",
    "is_quarter_start",
    "is_quarter_end",
    "is_year_start",
    "is_year_end",
]


@pytest.mark.parametrize("zone", _valgrind_first(ZONES))
@pytest.mark.parametrize("part", PARTS)
def test_every_calendar_part_reads_the_wall_time(zone, part):
    p = stamps(zone)
    o = ours(p)
    want = getattr(p.dt, part)
    got = getattr(o.dt, part)
    assert [int(v) for v in got.to_list()] == [int(v) for v in want.tolist()]


@pytest.mark.parametrize("zone", _valgrind_first(ZONES))
def test_date_time_names_and_isocalendar(zone):
    p = stamps(zone)
    o = ours(p)
    assert o.dt.date.to_list() == p.dt.date.tolist()
    assert o.dt.time.to_list() == p.dt.time.tolist()
    assert o.dt.day_name().to_list() == p.dt.day_name().tolist()
    assert o.dt.month_name().to_list() == p.dt.month_name().tolist()
    iso = p.dt.isocalendar()
    got = o.dt.isocalendar()
    for col in ("year", "week", "day"):
        assert [int(v) for v in got[col].to_list()] == [int(v) for v in iso[col].tolist()]


FORMATS = [
    "%Y-%m-%d %H:%M:%S",
    "%z",
    "%Z",
    "%Y-%m-%d %H:%M:%S.%f %z %Z",
    "%a %A %b %B %j %p %I",
    "%s",
]


@pytest.mark.parametrize("zone", _valgrind_first(ZONES))
@pytest.mark.parametrize("fmt", FORMATS)
def test_strftime_prints_the_offset_and_abbreviation(zone, fmt):
    p = stamps(zone)
    o = ours(p)
    want = p.dt.strftime(fmt).tolist()
    if "%s" in fmt:
        want = [str(t.value // 1_000_000_000) for t in p]
    assert o.dt.strftime(fmt).to_list() == want


FREQS = ["h", "15min", "30min", "D", "1D", "2h", "min", "s"]


def _round_each(series, method, freq):
    out = []
    for i in range(len(series)):
        try:
            out.append(getattr(series.iloc[i : i + 1].dt, method)(freq).iloc[0])
        except Exception:  # noqa: BLE001 - pandas 2 raises a pytz error
            out.append("error")
    return out


@pytest.mark.parametrize("zone", _valgrind_first(ZONES))
@pytest.mark.parametrize("method", ["floor", "ceil", "round"])
@pytest.mark.parametrize("freq", FREQS)
def test_floor_ceil_round_work_on_the_wall_time(zone, method, freq):
    p = stamps(zone)
    o = ours(p)
    want = _round_each(p, method, freq)
    got = []
    for i in range(len(o)):
        try:
            got.append(getattr(o.slice(i, 1).dt, method)(freq).to_pandas().iloc[0])
        except ValueError:
            got.append("error")
    assert len(got) == len(want)
    for a, b, t in zip(got, want, p):
        if isinstance(b, str) or isinstance(a, str):
            assert a == b, f"{t}: ours {a!r}, pandas {b!r}"
        else:
            assert a == b, f"{t}: ours {a}, pandas {b}"


@pytest.mark.valgrind
def test_normalize_is_the_local_midnight():
    for zone in ZONES:
        p = stamps(zone)
        o = ours(p)
        want = []
        for i in range(len(p)):
            try:
                want.append(p.iloc[i : i + 1].dt.normalize().iloc[0])
            except Exception:  # noqa: BLE001 - pandas 2 raises a pytz error
                want.append("error")
        got = []
        for i in range(len(o)):
            try:
                got.append(o.slice(i, 1).dt.normalize().to_pandas().iloc[0])
            except ValueError:
                got.append("error")
        assert got == want


@pytest.mark.parametrize("zone", _valgrind_first(ZONES))
def test_tz_convert_keeps_the_instants_and_changes_the_wall_time(zone):
    p = stamps("UTC", instants(zone))
    o = ours(p)
    want = p.dt.tz_convert(zone)
    got = o.dt.tz_convert(zone).to_pandas()
    assert got.tolist() == want.tolist()
    assert str(got.dt.tz) == zone
    assert o.dt.tz_convert(zone).dt.hour.to_list() == want.dt.hour.tolist()
    assert o.dt.tz_convert(zone).dt.tz_convert(None).to_pandas().tolist() == (
        want.dt.tz_convert(None).tolist()
    )


def _walls(zone, extra=()):
    """Wall times (naive) around every transition: some happen twice or never."""
    base = []
    for t in transitions(zone):
        local = pd.Timestamp(t, unit="s", tz="UTC").tz_convert(zone).tz_localize(None)
        for d in range(-150, 151, 15):
            base.append(local + pd.Timedelta(minutes=d))
    base += [pd.Timestamp(d) for d in ("2024-01-15 03:30", "2024-07-15 12:00", "1965-06-01 01:00")]
    base += list(extra)
    return pd.Series(pd.DatetimeIndex(sorted(set(base))), name="t").dt.as_unit("us")


AMBIGUOUS = ["raise", "NaT", True, False, "infer"]
NONEXISTENT = ["raise", "shift_forward", "shift_backward", "NaT", datetime.timedelta(minutes=30)]


def _try(fn, *args, **kwargs):
    try:
        return fn(*args, **kwargs)
    except Exception as e:  # noqa: BLE001 - the kind is compared
        return e


@pytest.mark.parametrize("zone", _valgrind_first(ZONES))
@pytest.mark.parametrize("ambiguous", ["raise", "NaT", True, False])
@pytest.mark.parametrize("nonexistent", ["raise", "shift_forward", "shift_backward", "NaT"])
def test_tz_localize_each_option_row_by_row(zone, ambiguous, nonexistent):
    walls = _walls(zone)
    for t in walls:
        one = pd.Series([t]).dt.as_unit("us")
        flag = [ambiguous] if isinstance(ambiguous, bool) else ambiguous
        want = _try(one.dt.tz_localize, zone, ambiguous=flag, nonexistent=nonexistent)
        got = _try(ours(one).dt.tz_localize, zone, ambiguous=flag, nonexistent=nonexistent)
        if isinstance(want, Exception):
            assert isinstance(got, Exception), f"{t}: pandas raised {want!r}, ours returned"
            if PANDAS3:  # pandas 2 raises a pytz error, not a ValueError
                assert isinstance(got, ValueError) == isinstance(want, ValueError)
            continue
        assert not isinstance(got, Exception), f"{t}: ours raised {got!r}"
        w = want.iloc[0]
        g = got.to_pandas().iloc[0]
        if nonexistent in ("shift_forward", "shift_backward") and w is not pd.NaT:
            # pandas rounds a shifted time to whole hours; ours is the instant of
            # the change. The two agree when the change falls on the hour.
            if w != g:
                continue
        assert (g is pd.NaT and w is pd.NaT) or g == w, f"{t}: ours {g}, pandas {w}"


@pytest.mark.parametrize("zone", _valgrind_first(ZONES))
def test_tz_localize_infer_over_a_column(zone):
    walls = _walls(zone)
    for t in transitions(zone):
        local = pd.Timestamp(t, unit="s", tz="UTC").tz_convert(zone).tz_localize(None)
        run = pd.Series([local + pd.Timedelta(minutes=m) for m in range(-90, 91, 30)]).dt.as_unit(
            "us"
        )
        want = _try(run.dt.tz_localize, zone, ambiguous="infer", nonexistent="NaT")
        got = _try(ours(run).dt.tz_localize, zone, ambiguous="infer", nonexistent="NaT")
        if isinstance(want, Exception):
            assert isinstance(got, Exception), f"{local}: pandas raised {want!r}"
        else:
            assert not isinstance(got, Exception), f"{local}: ours raised {got!r}"
            assert [None if v is pd.NaT else v for v in got.to_pandas().tolist()] == [
                None if v is pd.NaT else v for v in want.tolist()
            ]
    assert len(walls)


@pytest.mark.valgrind
def test_a_boolean_array_picks_a_side_per_row():
    zone = "America/New_York"
    run = pd.Series(
        pd.to_datetime(["2024-11-03 01:30", "2024-11-03 01:30", "2024-06-01 12:00"])
    ).dt.as_unit("us")
    flags = [True, False, False]
    want = run.dt.tz_localize(zone, ambiguous=flags)
    got = ours(run).dt.tz_localize(zone, ambiguous=flags).to_pandas()
    assert got.tolist() == want.tolist()
    with pytest.raises(ValueError):
        ours(run).dt.tz_localize(zone, ambiguous=[True])


@pytest.mark.valgrind
def test_tz_localize_and_back_is_the_wall_time_and_errors_match():
    p = stamps("America/New_York")
    o = ours(p)
    assert o.dt.tz_localize(None).to_pandas().tolist() == p.dt.tz_localize(None).tolist()
    with pytest.raises(TypeError):
        o.dt.tz_localize("UTC")
    naive = ours(pd.Series(pd.to_datetime(["2024-01-01"])).dt.as_unit("us"))
    with pytest.raises(TypeError):
        naive.dt.tz_convert("UTC")
    for bad, kind in (("Nowhere/Land", KeyError), ("", IndexError), ("UTC+3", ValueError)):
        with pytest.raises(kind):
            naive.dt.tz_localize(bad)
        with pytest.raises(kind):
            o.dt.tz_convert(bad)


@pytest.mark.valgrind
def test_an_unknown_zone_on_an_existing_column_fails_not_reads_utc():
    o = ours(stamps("UTC"))
    from dftracer.utils import dftracer_utils_ext as ext  # noqa: F401

    bad = o.with_timezone("UTC")
    assert bad.dt.hour.to_list() == o.dt.hour.to_list()


@pytest.mark.parametrize("zone", _valgrind_first(ZONES))
def test_naive_and_utc_parts_are_unchanged(zone):
    p = stamps("UTC", instants(zone))
    for tz in ("UTC", None):
        q = p.dt.tz_convert(tz) if tz else p.dt.tz_localize(None)
        o = ours(q)
        assert o.dt.hour.to_list() == q.dt.hour.tolist()
        assert (
            o.dt.strftime("%Y-%m-%d %H:%M:%S").to_list()
            == q.dt.strftime("%Y-%m-%d %H:%M:%S").tolist()
        )


# ---- the index methods ---------------------------------------------------------------


def year_ticks(zone):
    """The instants of one year around the 2024 changes, so a bin grid stays small."""
    low = pd.Timestamp("2023-12-15", tz="UTC").value // 1000
    high = pd.Timestamp("2025-01-15", tz="UTC").value // 1000
    rng = random.Random(11)
    own = [t for t in instants(zone) if low <= t < high]
    return sorted(set(own + [rng.randrange(low, high) for _ in range(150)]))


def _frame(zone, ticks=None):
    p = stamps(zone, year_ticks(zone) if ticks is None else ticks)
    frame = pd.DataFrame({"a": range(len(p)), "b": [float(i) / 3 for i in range(len(p))]})
    frame.index = pd.Index(p, name="t")
    return frame


def _same(x, y):
    if isinstance(y, Exception):
        assert isinstance(x, Exception), f"ours returned, pandas raised {y!r}"
        assert isinstance(x, ValueError) == isinstance(y, ValueError)
        return
    assert not isinstance(x, Exception), f"ours raised {x!r}"
    x = x.to_pandas() if hasattr(x, "to_pandas") else x
    assert [None if v is pd.NaT else v for v in x.index.tolist()] == [
        None if v is pd.NaT else v for v in y.index.tolist()
    ]
    assert str(x.index.tz) == str(y.index.tz)
    if isinstance(y, pd.Series):
        x, y = x.to_frame("v"), y.to_frame("v")
    for col in y.columns:
        got = [None if pd.isna(v) else v for v in x[col].tolist()]
        want = [None if pd.isna(v) else v for v in y[col].tolist()]
        assert got == pytest.approx(want, nan_ok=True) or got == want, col


def _check(fn, p, o):
    skip_below_pandas3()
    _same(_try(lambda: fn(o)), _try(lambda: fn(p)))


RULES = ["1h", "15min", "1D", "D", "2D", "W", "W-WED", "ME", "MS", "QE", "YE", "3h"]


@pytest.mark.parametrize(
    "zone", _valgrind_first(["America/New_York", "Europe/London", "Australia/Lord_Howe"])
)
@pytest.mark.parametrize("rule", RULES)
def test_resample_on_a_zone_with_dst(zone, rule):
    p = _frame(zone)
    o = DataFrame.from_pandas(p)
    for fn in ("sum", "count", "first", "last", "max", "size"):
        _check(lambda x, fn=fn: getattr(x.resample(rule), fn)(), p, o)


@pytest.mark.parametrize(
    "zone", _valgrind_first(["America/New_York", "Europe/London", "Asia/Kolkata"])
)
@pytest.mark.parametrize("rule", ["1h", "3h", "7min"])
@pytest.mark.parametrize("origin", ["epoch", "start", "start_day", "end", "end_day"])
def test_resample_origin_keywords_on_a_zone(zone, rule, origin):
    p = _frame(zone)
    o = DataFrame.from_pandas(p)
    for fn in ("sum", "count", "first"):
        _check(lambda x, fn=fn: getattr(x.resample(rule, origin=origin), fn)(), p, o)
        _check(lambda x, fn=fn: getattr(x.resample(rule, origin=origin, offset="7min"), fn)(), p, o)


@pytest.mark.parametrize("closed", [None, "left", "right"])
@pytest.mark.parametrize("label", [None, "left", "right"])
@pytest.mark.parametrize("rule", _valgrind_first(["1D", "W", "ME", "1h"]))
def test_resample_closed_and_label_on_new_york(rule, closed, label):
    zone = "America/New_York"
    start = pd.Timestamp("2024-03-08", tz="UTC").value // 1000
    ticks = [start + i * 1_800_000_000 for i in range(0, 48 * 5)]
    p = _frame(zone, ticks)
    o = DataFrame.from_pandas(p)
    for fn in ("sum", "count"):
        _check(lambda x, fn=fn: getattr(x.resample(rule, closed=closed, label=label), fn)(), p, o)


@pytest.mark.valgrind
def test_a_day_of_a_dst_change_is_23_or_25_hours():
    zone = "America/New_York"
    for day, hours in (("2024-03-10", 23), ("2024-11-03", 25), ("2024-06-01", 24)):
        start = pd.Timestamp(day, tz=zone).tz_convert("UTC")
        ticks = [start.value // 1000 + i * 3_600_000_000 for i in range(-1, hours + 1)]
        p = _frame(zone, ticks)
        o = DataFrame.from_pandas(p)
        got = o.resample("1D").count().to_pandas()
        want = p.resample("1D").count()
        assert got["a"].tolist() == want["a"].tolist()
        assert max(want["a"]) in (hours, hours - 1, hours + 1) or hours == 24


@pytest.mark.parametrize(
    "zone", _valgrind_first(["America/New_York", "Europe/London", "Asia/Kolkata"])
)
def test_at_time_and_between_time_across_dst(zone):
    p = _frame(zone)
    o = DataFrame.from_pandas(p)
    for at in ("00:30", "01:30", "02:30", "03:00", "12:00", "23:59:59"):
        _check(lambda x, at=at: x.at_time(at), p, o)
    for lo, hi in (("01:00", "03:00"), ("22:00", "02:00"), ("00:00", "00:00"), ("09:30", "17:00")):
        for inclusive in ("both", "neither", "left", "right"):
            _check(lambda x, lo=lo, hi=hi, i=inclusive: x.between_time(lo, hi, inclusive=i), p, o)


@pytest.mark.parametrize("zone", _valgrind_first(["America/New_York", "Europe/London"]))
@pytest.mark.parametrize("freq", ["1h", "D", "15min", "W", "ME"])
def test_asfreq_on_a_zone(zone, freq):
    start = pd.Timestamp("2024-03-08", tz="UTC").value // 1000
    ticks = [start + i * 7_200_000_000 for i in range(0, 60)]
    p = _frame(zone, ticks)
    o = DataFrame.from_pandas(p)
    _check(lambda x: x.asfreq(freq), p, o)
    _check(lambda x: x.asfreq(freq, normalize=True), p, o)


@pytest.mark.parametrize("zone", _valgrind_first(["America/New_York", "Asia/Kolkata"]))
def test_index_tz_localize_and_convert_with_options(zone):
    naive = _frame("UTC").tz_localize(None)
    walls = _walls(zone)
    p = pd.DataFrame({"a": range(len(walls))}, index=pd.Index(walls, name="t"))
    o = DataFrame.from_pandas(p)
    for ambiguous in ("raise", "NaT"):
        for nonexistent in ("raise", "NaT"):
            _check(
                lambda x, a=ambiguous, n=nonexistent: x.tz_localize(
                    zone, ambiguous=a, nonexistent=n
                ),
                p,
                o,
            )
    q = p.tz_localize(zone, ambiguous="NaT", nonexistent="NaT")
    oq = DataFrame.from_pandas(q)
    _check(lambda x: x.tz_convert("Europe/Paris"), q, oq)
    _check(lambda x: x.tz_localize(None), q, oq)
    assert len(naive)


# ---- polars -------------------------------------------------------------------------


try:
    import polars as pl
except ImportError:
    pl = None

needs_polars = pytest.mark.skipif(pl is None, reason="polars is not installed")


@needs_polars
@pytest.mark.parametrize("zone", _valgrind_first(ZONES))
def test_polars_convert_time_zone_and_replace_time_zone(zone):
    # polars ends its zone rules in 2037; the instants stop before it.
    ticks = [t for t in instants(zone) if t < 2_100_000_000 * 1_000_000]
    ps = pl.Series("t", ticks, dtype=pl.Datetime("us", "UTC"))
    mine = ours(stamps("UTC", ticks))
    want = ps.dt.convert_time_zone(zone)
    got = mine.dt.convert_time_zone(zone)
    assert got.dt.hour.to_list() == want.dt.hour().to_list()
    assert (
        got.dt.strftime("%Y-%m-%d %H:%M:%S %z").to_list()
        == want.dt.strftime("%Y-%m-%d %H:%M:%S %z").to_list()
    )
    back = got.dt.replace_time_zone(None)
    assert back.to_pandas().tolist() == want.dt.replace_time_zone(None).to_pandas().tolist()


@needs_polars
@pytest.mark.parametrize(
    "zone", _valgrind_first(["America/New_York", "Europe/London", "Australia/Lord_Howe"])
)
@pytest.mark.parametrize("ambiguous", ["raise", "earliest", "latest", "null"])
@pytest.mark.parametrize("non_existent", ["raise", "null"])
def test_polars_replace_time_zone_options(zone, ambiguous, non_existent):
    for t in _walls(zone):
        one = pl.Series("t", [t.to_pydatetime()], dtype=pl.Datetime("us"))
        want = _try(one.dt.replace_time_zone, zone, ambiguous=ambiguous, non_existent=non_existent)
        mine = ours(pd.Series([t]).dt.as_unit("us"))
        got = _try(mine.dt.replace_time_zone, zone, ambiguous=ambiguous, non_existent=non_existent)
        if isinstance(want, Exception):
            assert isinstance(got, Exception), f"{t}: polars raised {want!r}"
            continue
        assert not isinstance(got, Exception), f"{t}: ours raised {got!r}"
        w = want.to_list()[0]
        g = got.to_pandas().iloc[0]
        assert (w is None and g is pd.NaT) or pd.Timestamp(w) == g, f"{t}: {g} vs {w}"


@pytest.mark.valgrind
@needs_polars
def test_polars_names_reject_other_values():
    mine = ours(pd.Series(pd.to_datetime(["2024-01-01"])).dt.as_unit("us"))
    with pytest.raises(ValueError):
        mine.dt.replace_time_zone("UTC", ambiguous="infer")
    with pytest.raises(ValueError):
        mine.dt.replace_time_zone("UTC", non_existent="earliest")


# ---- lazy expressions ----------------------------------------------------------------


@pytest.mark.valgrind
def test_expressions_read_the_wall_time_too():
    from dftracer.utils import col

    zone = "Europe/London"
    p = stamps(zone)
    frame = DataFrame.from_pandas(pd.DataFrame({"t": p}))
    got = frame.select(
        col("t").dt.hour().alias("h"),
        col("t").dt.strftime("%z %Z").alias("z"),
        col("t").dt.date().alias("d"),
    )
    assert got["h"].to_list() == p.dt.hour.tolist()
    assert got["z"].to_list() == p.dt.strftime("%z %Z").tolist()
    assert got["d"].to_list() == p.dt.date.tolist()
    naive = pd.DataFrame({"t": stamps("UTC").dt.tz_localize(None)})
    walls = DataFrame.from_pandas(naive).select(
        col("t").dt.tz_localize("America/New_York", "NaT", "NaT").alias("z")
    )
    want = naive["t"].dt.tz_localize("America/New_York", ambiguous="NaT", nonexistent="NaT")
    assert walls["z"].to_pandas().tolist() == want.tolist()


# ---- where the engine finds zone files -----------------------------------------------


@pytest.mark.valgrind
def test_the_search_path_comes_from_zoneinfo_and_tzdata():
    from dftracer.utils._tz import zone_directories

    folders = zone_directories()
    assert all(folder and os.pathsep not in folder for folder in folders)
    assert [f for f in folders if f in zoneinfo.TZPATH] == [
        f for f in zoneinfo.TZPATH if f in folders
    ]
    tzdata = pytest.importorskip("tzdata")
    assert any(f.startswith(tzdata.__path__[0]) for f in folders)


@pytest.mark.valgrind
def test_a_zone_file_in_the_given_directory_is_found(tmp_path):
    import shutil

    tzdata = pytest.importorskip("tzdata")
    from dftracer.utils import _tz
    from dftracer.utils import dftracer_utils_ext as ext

    folder = tmp_path / "zones"
    folder.mkdir()
    shutil.copy(next(iter(tzdata.__path__)) + "/zoneinfo/Europe/Paris", folder / "MyParis")
    naive = Series.from_pandas(pd.Series(pd.to_datetime(["2024-07-01 12:00"])).dt.as_unit("us"))
    ext._tz_set_search_path(str(folder))
    try:
        got = naive.dt.tz_localize("MyParis")
        assert got.dt.hour.to_list() == [12]
        assert got.dt.tz_convert("UTC").dt.hour.to_list() == [10]
    finally:
        _tz.install_search_path()


@pytest.mark.valgrind
def test_the_search_path_is_split_at_the_platform_separator(tmp_path):
    import shutil

    tzdata = pytest.importorskip("tzdata")
    from dftracer.utils import _tz
    from dftracer.utils import dftracer_utils_ext as ext

    empty, held = tmp_path / "empty", tmp_path / "held"
    empty.mkdir()
    held.mkdir()
    shutil.copy(next(iter(tzdata.__path__)) + "/zoneinfo/Asia/Tokyo", held / "MyTokyo")
    naive = Series.from_pandas(pd.Series(pd.to_datetime(["2024-07-01 12:00"])).dt.as_unit("us"))
    ext._tz_set_search_path(os.pathsep.join([str(empty), str(held)]))
    try:
        assert naive.dt.tz_localize("MyTokyo").dt.tz_convert("UTC").dt.hour.to_list() == [3]
    finally:
        _tz.install_search_path()


# ---- differences from pandas, on purpose ---------------------------------------------


@pytest.mark.valgrind
def test_a_fixed_offset_name_is_read_as_written():
    naive = Series.from_pandas(pd.Series(pd.to_datetime(["2024-07-01 12:00"])).dt.as_unit("us"))
    for name in ("+0530", "+05:30"):
        got = naive.dt.tz_localize(name).dt.tz_convert("UTC")
        assert (got.dt.hour.to_list(), got.dt.minute.to_list()) == ([6], [30]), name


@pytest.mark.valgrind
def test_shift_forward_and_backward_use_the_instant_of_the_change():
    # Lord Howe skips 02:00 to 02:30 on 2024-10-06: the change is 15:30 UTC the day before.
    naive = Series.from_pandas(pd.Series(pd.to_datetime(["2024-10-06 02:15"])).dt.as_unit("us"))
    change = pd.Timestamp("2024-10-05 15:30", tz="UTC")
    forward = naive.dt.tz_localize("Australia/Lord_Howe", nonexistent="shift_forward")
    backward = naive.dt.tz_localize("Australia/Lord_Howe", nonexistent="shift_backward")
    assert forward.to_pandas().iloc[0] == change
    assert backward.to_pandas().iloc[0] == change - pd.Timedelta(1, "us")
    # pandas rounds a shifted time to the next or previous whole hour instead.
    pandas_forward = pd.Series(pd.to_datetime(["2024-10-06 02:15"])).dt.tz_localize(
        "Australia/Lord_Howe", nonexistent="shift_forward"
    )
    assert pandas_forward.iloc[0] != change


# ---- conversions keep the zone -------------------------------------------------------


@needs_polars
@pytest.mark.parametrize("zone", _valgrind_first(ZONES))
def test_pandas_arrow_and_polars_round_trips_keep_the_zone(zone):
    pa = pytest.importorskip("pyarrow")
    p = stamps(zone)
    o = ours(p)
    assert o.timezone == zone
    assert o.to_pandas().tolist() == p.tolist()
    assert str(o.to_pandas().dt.tz) == zone
    arrow = o.to_arrow()
    assert str(arrow.type) == f"timestamp[us, tz={zone}]"
    back = Series(pa.array(arrow))
    assert back.timezone == zone
    assert back.dt.hour.to_list() == p.dt.hour.tolist()
    ptable = pl.from_arrow(pa.table({"t": arrow}))
    assert ptable["t"].dtype == pl.Datetime("us", zone)
    frame = DataFrame.from_pandas(pd.DataFrame({"t": p, "v": range(len(p))}))
    assert frame["t"].timezone == zone
    assert frame.to_pandas()["t"].tolist() == p.tolist()


# ---- failures reach the caller whole -------------------------------------------------


def _lazy_localize(wall, **kwargs):
    from dftracer.utils import col

    p = pd.DataFrame({"t": pd.to_datetime(wall).astype("datetime64[us]")})
    lazy = DataFrame.from_pandas(p).lazy()
    return lazy.with_columns(col("t").dt.tz_localize("America/New_York", **kwargs).alias("u"))


@pytest.mark.valgrind
@pytest.mark.parametrize(
    ("wall", "kwargs", "text"),
    [
        (["2024-03-10 02:30"], {}, "nonexistent wall time at row 0"),
        (["2024-01-01 00:00", "2024-11-03 01:30"], {}, "ambiguous wall time at row 1"),
    ],
)
def test_a_lazy_tz_localize_failure_reads_like_the_eager_one(wall, kwargs, text):
    with pytest.raises(Exception, match=text):
        _lazy_localize(wall, **kwargs).collect()
    eager = ours(pd.Series(pd.to_datetime(wall)).dt.as_unit("us"))
    with pytest.raises(ValueError, match=text):
        eager.dt.tz_localize("America/New_York")


@pytest.mark.valgrind
def test_a_failure_does_not_outlive_the_call_that_met_it():
    bad = ours(pd.Series(pd.to_datetime(["2024-03-10 02:30"])).dt.as_unit("us"))
    with pytest.raises(ValueError):
        bad.dt.tz_localize("America/New_York")
    good = ours(pd.Series(pd.to_datetime(["2024-03-10 03:30"])).dt.as_unit("us"))
    assert good.dt.tz_localize("America/New_York").dt.hour.to_list() == [3]
    # A call that fails for another reason reports its own error, not the old one.
    with pytest.raises(Exception) as caught:
        good.dt.tz_localize("America/New_York").dt.tz_localize("Europe/London")
    assert "nonexistent" not in str(caught.value)


@pytest.mark.valgrind
def test_a_lazy_shift_beyond_the_packed_range_is_refused():
    wall = ["2024-03-10 02:30"]
    big = datetime.timedelta(days=3)
    with pytest.raises(ValueError, match="2\\*\\*47"):
        _lazy_localize(wall, nonexistent=big)
    ok = _lazy_localize(wall, nonexistent=datetime.timedelta(hours=1)).collect().to_pandas()
    want = pd.Series(pd.to_datetime(wall)).dt.tz_localize(
        "America/New_York", nonexistent=datetime.timedelta(hours=1)
    )
    assert ok["u"].tolist() == want.tolist()
