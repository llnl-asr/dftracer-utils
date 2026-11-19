# SPDX-License-Identifier: MIT
"""Time on the row index of a :class:`Series` and a :class:`DataFrame`:
``resample``, ``asfreq``, ``at_time``, ``between_time``, ``tz_localize`` and
``tz_convert``, as pandas 2 and 3 have them.

The time is one index level (or ``on=`` for ``resample``) of Timestamp type.
Bins are integers: a row's bin is arithmetic on its tick for a fixed width
(``"5min"``, ``"1h"``, ``"D"``) and a ``cut`` over the few edges for a calendar
rule (``"W"``, ``"ME"``, ``"MS"``, ``"QE"``, ``"QS"``, ``"YE"``, ``"YS"``); the
group-by aggregates over the bin and the result is joined onto the full grid,
so empty bins are listed."""

from __future__ import annotations

import datetime
import re
from typing import TYPE_CHECKING, Callable, Literal, Optional, Sequence, Tuple, Union, cast

from . import dftracer_utils_ext as _ext
from ._convert import _like
from ._levels import pick_levels
from ._series_index import drop_levels, levels_of, names_of, positions, public_name, with_levels
from .enums import UNIT_CODES, UNIT_NS, DType, rule_to_units

if TYPE_CHECKING:
    from typing_extensions import Self

    from .dataframe import DataFrame
    from .expr import Agg
    from .series import Series

Frameish = Union["Series", "DataFrame"]
_Spec = Tuple[str, Optional[str], str]
LevelArg = Union[int, str, None]

_EPOCH_ORDINAL = datetime.date(1970, 1, 1).toordinal()
_BIN = "__dftu_bin__"
_TICK = "__dftu_tick__"
_VALUE = "__dftu_value__"
_UNITS = tuple(UNIT_CODES)
_WEEKDAYS = {d: i for i, d in enumerate(("MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"))}
_MONTHS = {
    m: i
    for i, m in enumerate(
        ("JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC")
    )
}
_COMPOUND = re.compile(r"(\d+)\s*([A-Za-z]+)")
_RULE = re.compile(r"^\s*(\d*)\s*([A-Za-z]+)(?:-([A-Za-z]{3}))?\s*$")
_TIME_FORMATS = (
    "%H:%M",
    "%H%M",
    "%I:%M%p",
    "%I%M%p",
    "%H:%M:%S",
    "%H%M%S",
    "%I:%M:%S%p",
    "%I%M%S%p",
)
_ORIGINS = ("epoch", "start", "start_day", "end", "end_day")


# ---- the time key ----------------------------------------------------------------


def _series() -> type[Series]:
    from .series import Series

    return Series


def is_timestamp(s: Series) -> bool:
    return DType(s.type) == DType.TIMESTAMP


def unit_name(s: Series) -> str:
    return _UNITS[s.time_unit]


# The zones whose offset is always zero, which pandas and tzdata spell many ways.
_UTC = frozenset(
    (
        "",
        "UTC",
        "ETC/UTC",
        "UCT",
        "ETC/UCT",
        "UNIVERSAL",
        "ETC/UNIVERSAL",
        "ZULU",
        "ETC/ZULU",
        "GMT",
        "ETC/GMT",
        "GMT0",
        "ETC/GMT0",
        "ETC/GMT+0",
        "ETC/GMT-0",
        "GMT+0",
        "GMT-0",
        "GREENWICH",
        "ETC/GREENWICH",
    )
)


def zoned(key: Series) -> bool:
    """Whether the Timestamp column is in a zone whose offset can change, so its
    days and times of day are wall-clock ones that the engine reads through the
    zone. A naive column and a UTC one are read directly."""
    return key.timezone.upper() not in _UTC


def wall(key: Series) -> Series:
    """The wall times of a Timestamp column as a naive one (the same column
    when it is naive or in UTC)."""
    return key.dt_tz_localize("") if zoned(key) else key.with_timezone("")


def localize_back(naive: Series, key: Series) -> Series:
    """Wall times as the instants of ``key``'s zone, a repeated wall time at
    its first instant and a skipped one moved to the end of the gap, as the
    pandas resample edges."""
    if not zoned(key):
        return naive.with_timezone(key.timezone)
    return naive.dt_tz_localize(key.timezone, 1, 1)


def _kind_error(shape: str) -> TypeError:
    return TypeError(
        "Only valid with DatetimeIndex, TimedeltaIndex or PeriodIndex, "
        f"but got an instance of '{shape}'"
    )


def _levels(obj: Frameish) -> tuple[list[str], list[Series]]:
    from .series import Series

    if isinstance(obj, Series):
        return names_of(obj), levels_of(obj)
    names = list(obj.index_names())
    return names, [_ext.DataFrame.__getitem__(obj, n) for n in names]


def _level_of(names: list[str], level: LevelArg) -> int:
    """The position of ``level``, with the pandas errors: a single level is 0 or its name (``ValueError`` for any
    other), a MultiIndex needs ``level`` (``KeyError``, as pandas)."""
    if len(names) > 1:
        if level is None:
            raise KeyError("Level None not found")
        return pick_levels(names, level)[0]
    if level is not None:
        named = bool(names) and level == public_name(names[0])
        if not (named or (level == 0 and not isinstance(level, bool))):
            raise ValueError(f"The level {level} is not valid")
    return 0


def index_level(obj: Frameish, level: LevelArg, what: str) -> tuple[int, Series]:
    """The position and the Timestamp column of the one index level an op reads
    (``level`` names it when there are several); ``TypeError`` for any other
    index, as pandas."""
    names, levels = _levels(obj)
    if not names:
        if level is not None:
            _level_of(names, level)
        raise TypeError(what.format("RangeIndex"))
    if level is None and len(names) > 1:
        raise TypeError(what.format("MultiIndex"))
    at = _level_of(names, level)
    if not is_timestamp(levels[at]):
        raise TypeError(what.format("Index"))
    return at, levels[at]


# ---- fixed widths and calendar rules -----------------------------------------------


class _Calendar:
    """A calendar offset as anchors on day ordinals: ``date_of(a)`` is anchor
    ``a`` and ``floor(day)`` the index of the last anchor on or before ``day``,
    the two things the pandas range edges need."""

    __slots__ = ("n", "at_end", "_kind", "_period", "_base")

    def __init__(self, n: int, kind: str, period: int, base: int, at_end: bool) -> None:
        self.n = n
        self.at_end = at_end
        self._kind = kind
        self._period = period
        self._base = base

    @property
    def is_fixed(self) -> bool:
        """Whether every bin has the same width (a day or a week)."""
        return self._kind in ("day", "week")

    def date_of(self, a: int) -> int:
        if self._kind == "day":
            return a
        if self._kind == "week":
            return self._base + 7 * a
        month = self._base + self._period * a
        year, m = divmod(month, 12)
        if self._kind == "month_start":
            return datetime.date(year, m + 1, 1).toordinal()
        following = datetime.date(year + (m == 11), (m + 1) % 12 + 1, 1)
        return following.toordinal() - 1

    def floor(self, day: int) -> int:
        if self._kind == "day":
            return day
        if self._kind == "week":
            return (day - self._base) // 7
        date = datetime.date.fromordinal(day)
        month = date.year * 12 + date.month - 1
        if self._kind == "month_end" and day != self.date_of_month_end(month):
            month -= 1
        return (month - self._base) // self._period

    def date_of_month_end(self, month: int) -> int:
        year, m = divmod(month, 12)
        following = datetime.date(year + (m == 11), (m + 1) % 12 + 1, 1)
        return following.toordinal() - 1

    def before(self, day: int, closed_left: bool) -> int:
        """The first edge: ``rollback`` for a left-closed bin, else one offset
        back (a day on an anchor goes a whole step)."""
        a = self.floor(day)
        if closed_left:
            return a
        return a - self.n + (0 if self.date_of(a) == day else 1)

    def after(self, day: int) -> int:
        return self.floor(day) + self.n


class _Rule:
    """A parsed ``rule``: a fixed width in nanoseconds, or a calendar offset."""

    __slots__ = ("text", "ns", "calendar")

    def __init__(self, text: str, ns: int, calendar: _Calendar | None) -> None:
        self.text = text
        self.ns = ns
        self.calendar = calendar

    @property
    def at_end(self) -> bool:
        return self.calendar is not None and self.calendar.at_end


def _calendar_of(name: str, n: int, anchor: str | None) -> _Calendar | None:
    if name == "D":
        return None if anchor else _Calendar(n, "day", 1, 0, False)
    if name == "W":
        day = _WEEKDAYS.get((anchor or "SUN").upper())
        return None if day is None else _Calendar(n, "week", 7, day + 1, True)
    spec = {
        "ME": ("month_end", 1, True, "JAN"),
        "MS": ("month_start", 1, False, "JAN"),
        "QE": ("month_end", 3, True, "DEC"),
        "QS": ("month_start", 3, False, "JAN"),
        "YE": ("month_end", 12, True, "DEC"),
        "YS": ("month_start", 12, False, "JAN"),
    }.get(name)
    if spec is None:
        return None
    kind, period, at_end, default = spec
    if anchor is None and name in ("ME", "MS"):
        base = 0
    else:
        month = _MONTHS.get((anchor or default).upper())
        if month is None:
            return None
        base = month
    return _Calendar(n, kind, period, base, at_end)


def parse_rule(rule: object, unit: str, tz_aware: bool) -> _Rule:
    """``rule`` as a width in ``unit`` ticks (``ns`` holds it in nanoseconds) or a
    calendar offset. A day is a fixed 24 hours on a naive column and a
    calendar step on a zone-aware one, as pandas."""
    if isinstance(rule, datetime.timedelta):
        ns = (rule.days * 86400 + rule.seconds) * 1_000_000_000 + rule.microseconds * 1000
        if ns <= 0:
            raise ValueError("resample: rule must be positive")
        return _Rule(str(rule), ns, None)
    if isinstance(rule, bool) or not isinstance(rule, (int, str)):
        raise TypeError(f"Invalid frequency: {rule!r}")
    if isinstance(rule, int):
        return _Rule(str(rule), rule_to_units(rule, unit) * UNIT_NS[unit], None)
    match = _RULE.match(rule)
    if match:
        n = int(match.group(1)) if match.group(1) else 1
        calendar = _calendar_of(match.group(2), n, match.group(3))
        if calendar is not None and not (tz_aware is False and match.group(2) == "D"):
            if n <= 0:
                raise ValueError(f"Invalid frequency: {rule}")
            return _Rule(rule, 0, calendar)
    parts = _COMPOUND.findall(rule)
    if len(parts) > 1 and "".join(f"{n}{u}" for n, u in parts) == "".join(rule.split()):
        total = sum(rule_to_units(f"{n}{u}", "ns") for n, u in parts)
        return _Rule(rule, total, None)
    counted = f"{match.group(1) or 1}{match.group(2)}" if match and not match.group(3) else rule
    ticks = rule_to_units(counted, unit)
    return _Rule(rule, ticks * UNIT_NS[unit], None)


def _timedelta_ns(value: object, what: str) -> int:
    if isinstance(value, datetime.timedelta):
        whole = (value.days * 86400 + value.seconds) * 1_000_000_000 + value.microseconds * 1000
        return whole + int(getattr(value, "nanoseconds", 0))
    if isinstance(value, str):
        text = value.strip()
        sign = -1 if text.startswith("-") else 1
        body = text.lstrip("+-").strip()
        if body:
            if re.fullmatch(r"0+(\.0+)?\s*[A-Za-z]+", body):
                return 0
            try:
                return sign * parse_rule(body, "ns", False).ns
            except ValueError:
                pass
    raise ValueError(f"'{what}' should be a Timedelta convertible type. Got '{value}' instead.")


def _origin_aware(origin: object) -> bool | None:
    """Whether a timestamp ``origin`` carries a zone (None for a keyword)."""
    if isinstance(origin, str):
        if origin in _ORIGINS:
            return None
        try:
            origin = datetime.datetime.fromisoformat(origin)
        except ValueError:
            return None
    return getattr(origin, "tzinfo", None) is not None


def _origin_ns(origin: object, unit: str) -> str | int:
    if isinstance(origin, str) and origin in _ORIGINS:
        return origin
    ticks = _timestamp_ns(origin)
    if ticks is None:
        raise ValueError(
            "'origin' should be equal to 'epoch', 'start', 'start_day', 'end', 'end_day' "
            f"or should be a Timestamp convertible type. Got '{origin}' instead."
        )
    return ticks


def _timestamp_ns(value: object) -> int | None:
    if isinstance(value, datetime.datetime):
        base = value if value.tzinfo is None else value.astimezone(datetime.timezone.utc)
        days = base.toordinal() - _EPOCH_ORDINAL
        secs = base.hour * 3600 + base.minute * 60 + base.second
        ns = ((days * 86400 + secs) * 1_000_000 + base.microsecond) * 1000
        return ns + int(getattr(value, "nanosecond", 0))
    if isinstance(value, datetime.date):
        return (value.toordinal() - _EPOCH_ORDINAL) * 86_400_000_000_000
    if isinstance(value, str):
        try:
            return _timestamp_ns(datetime.datetime.fromisoformat(value))
        except ValueError:
            return None
    return None


class _Bins:
    """The bins of one time column: ``ids`` is the bin of every row (null
    for a row outside any bin), ``labels`` the label of each of the ``count``
    bins."""

    __slots__ = ("ids", "labels", "count", "ticks")

    def __init__(self, ids: Series, labels: Series, count: int, ticks: Series) -> None:
        self.ids = ids
        self.labels = labels
        self.count = count
        self.ticks = ticks


def _tick_edges(
    first: int,
    last: int,
    width: int,
    closed_left: bool,
    origin: str | int,
    offset: int,
    day: int,
    anchors: tuple[int, int] | None = None,
) -> tuple[int, int]:
    """The first and last edge of a fixed-width grid (pandas
    ``_adjust_dates_anchored``), in units of ticks. In a zone ``anchors`` holds
    the instants of the local midnight at or before the first row and at or
    after the last row."""
    zero = 0
    if origin == "start_day":
        zero = anchors[0] if anchors else first - first % day
    elif origin == "start":
        zero = first
    elif origin in ("end", "end_day"):
        end = last if origin == "end" else (anchors[1] if anchors else -((-last) // day) * day)
        steps = (end - first) // width
        if closed_left:
            steps += 1
        first = end - steps * width
        zero = first
    elif isinstance(origin, int):
        zero = origin
    zero += offset
    foffset = (first - zero) % width
    loffset = (last - zero) % width
    if closed_left:
        low = first - foffset if foffset > 0 else first
        high = last + (width - loffset) if loffset > 0 else last + width
    else:
        low = first - foffset if foffset > 0 else first - width
        high = last + (width - loffset) if loffset > 0 else last
    return low, high


def _day_anchors(key: Series, low: int, high: int, day: int) -> tuple[int, int]:
    """The instants of the local midnight at or before ``low`` and at or after
    ``high``, two ticks of ``key``."""
    rows = _like(_series().from_list([low, high], DType.INT64), key)
    floor = rows.dt_round(day, 0).astype("int64")
    ceil = rows.dt_round(day, 1).astype("int64")
    return int(cast(int, floor.iloc[0])), int(cast(int, ceil.iloc[1]))


def check_sides(closed: str | None, label: str | None) -> None:
    for name, value in (("closed", closed), ("label", label)):
        if value not in (None, "left", "right"):
            raise ValueError(f"Unsupported value {value} for `{name}`")


def _check_inside(ids: Series, count: int) -> None:
    """``ValueError`` when a row falls outside every bin, as pandas (a bin grid
    anchored at an ``origin`` of ``end`` with an ``offset`` can leave rows out)."""
    low, high = ids.min(), ids.max()
    if low is not None and int(cast(int, low)) < 0:
        raise ValueError("Values falls before first bin")
    if high is not None and int(cast(int, high)) >= count:
        raise ValueError("Values falls after last bin")


def make_bins(
    key: Series,
    rule: _Rule,
    closed: str | None,
    label: str | None,
    origin: str | int,
    offset: int,
) -> _Bins:
    """The bins of the Timestamp column ``key`` for ``rule``, with the pandas
    defaults for ``closed`` and ``label``."""
    unit = unit_name(key)
    per_unit = UNIT_NS[unit]
    day = 86_400_000_000_000 // per_unit
    end_default = rule.at_end or origin in ("end", "end_day")
    closed = closed or ("right" if end_default else "left")
    label = label or ("right" if end_default else "left")
    left = closed == "left"
    ticks = key.astype("int64")
    in_zone = zoned(key)
    # A calendar bin in a zone is a wall-clock one: the first and last day are
    # read on the wall times, and each edge is mapped back to an instant. A
    # fixed width stays a width of instants.
    local = wall(key).astype("int64") if in_zone and rule.calendar is not None else ticks
    if local.null_count == 0 and len(local) and local.is_sorted():
        low, high = local.iloc[0], local.iloc[len(local) - 1]
    else:
        low, high = local.min(), local.max()
    if low is None or high is None:
        empty = _like(_series().from_list([], DType.INT64), key)
        return _Bins(_ext._series_nulls(int(DType.INT64), len(key)), empty, 0, ticks)
    low, high = int(cast(int, low)), int(cast(int, high))
    if rule.calendar is None:
        width = rule.ns // per_unit
        if width * per_unit != rule.ns:
            raise ValueError(f"resample: {rule.text!r} is not a whole number of {unit}")
        if isinstance(origin, int):
            origin = origin // per_unit
        anchors = _day_anchors(key, low, high, day) if in_zone else None
        if in_zone and origin == "epoch":
            # pandas anchors on the local midnight of 1970-01-01 in the zone.
            midnight = _like(_series().from_list([0], DType.INT64), wall(key))
            origin = int(cast(int, localize_back(midnight, key).astype("int64").iloc[0]))
        first, last = _tick_edges(low, high, width, left, origin, offset // per_unit, day, anchors)
        count = (last - first) // width
        shifted = ticks if left else ticks - 1
        ids = (shifted - first).floordiv(width)
        if origin in ("end", "end_day"):
            _check_inside(ids, count)
        start = first + width * (0 if label == "left" else 1)
        labels = _like(positions(count) * width + start, key)
        return _Bins(ids, labels, count, ticks)
    cal = rule.calendar
    first_day = low // day + _EPOCH_ORDINAL
    last_day = high // day + _EPOCH_ORDINAL
    steps = range(cal.before(first_day, left), cal.after(last_day) + 1, cal.n)
    if cal.is_fixed and len(steps) > 1:
        # A day or a week is a fixed number of days from one step to the next.
        first_edge = (cal.date_of(steps[0]) - _EPOCH_ORDINAL) * day
        stride = (cal.date_of(steps[1]) - cal.date_of(steps[0])) * day
        edges = list(range(first_edge, first_edge + len(steps) * stride, stride))
    else:
        edges = [(cal.date_of(a) - _EPOCH_ORDINAL) * day for a in steps]
    bounds = list(edges)
    if cal.at_end and not left:
        bounds = [e + day - 1 for e in edges]
    if cal.at_end and bounds[-2] > high:
        bounds = bounds[:-1]
        edges = edges[:-1]
    count = len(bounds) - 1
    # In a zone a bin is read on the wall time of each row, so a day is one
    # fixed width there (23 or 25 hours of instants on a change of offset) and
    # no edge needs mapping back to find the bin of a row.
    if cal.is_fixed and count >= 1:
        # Days and weeks are a fixed width from the first edge: the bin is
        # arithmetic, with the right-closed edge moved one tick (or a day for
        # the end-of-day edge of a week) as the bounds are.
        shift = 0 if left else (day if cal.at_end else 1)
        ids = (local - (shift + edges[0])).floordiv(edges[1] - edges[0])
    else:
        breaks = _series().from_list(bounds, DType.INT64)
        ids = (local.cut(breaks, right=not left) - 1).astype("int64")
    if in_zone:
        template = _like(_series().from_list([], DType.INT64), wall(key.slice(0, 0)))
        wall_edges = _like(_series().from_list(edges, DType.INT64), template)
        edges = cast("list[int]", localize_back(wall_edges, key).astype("int64").to_list())
    shown = edges[:count] if label == "left" else edges[1 : count + 1]
    return _Bins(ids, _like(_series().from_list(shown, DType.INT64), key), count, ticks)


# ---- resample ---------------------------------------------------------------------


_ZERO = frozenset(("sum", "count", "size", "nunique"))
_SIMPLE = {
    "sum": "sum",
    "mean": "mean",
    "min": "min",
    "max": "max",
    "count": "count",
    "first": "first",
    "last": "last",
    "std": "std",
    "var": "var",
    "prod": "prod",
    "median": "median",
    "nunique": "nunique",
    "size": "size",
}
_OHLC = (("open", "first"), ("high", "max"), ("low", "min"), ("close", "last"))


def _spec(func: str, column: str | None, out: str) -> Agg:
    from .expr import Agg, Col

    if func == "size":
        return Agg("count", None, out)
    return cast("Agg", getattr(Col(cast(str, column)), _SIMPLE[func])().alias(out))


class TimeResampler:
    """``obj.resample(rule, ...)`` over a Timestamp index level or ``on``
    column: the aggregations of a pandas ``Resampler``."""

    __slots__ = ("_data", "_bins", "_name", "_label", "_is_series", "_columns")

    def __init__(
        self,
        data: dict[str, Series],
        bins: _Bins,
        name: str,
        label: str | None,
        is_series: bool,
    ) -> None:
        self._data = data
        self._bins = bins
        self._name = name
        self._label = label
        self._is_series = is_series
        self._columns = list(data)

    def __getitem__(self, columns: str | Sequence[str]) -> TimeResampler:
        picked = [columns] if isinstance(columns, str) else list(columns)
        for c in picked:
            if c not in self._data:
                raise KeyError(f"Column not found: {c}")
        single = isinstance(columns, str)
        return TimeResampler(
            {c: self._data[c] for c in picked},
            self._bins,
            self._name,
            picked[0] if single else None,
            single,
        )

    def _frame(self, ordered: bool) -> DataFrame:
        """The rows with their bin; ``ordered`` puts them in time order (a stable
        sort), which ``first`` and ``last`` read."""
        from .dataframe import DataFrame

        ids = self._bins.ids
        frame = DataFrame.from_dict({**self._data, _BIN: ids})
        ticks = self._bins.ticks
        if ordered and not ticks.is_sorted():
            frame = frame.with_column(_TICK, ticks)
        keep = ids.notna() if ids.null_count else None
        frame = frame if keep is None else frame.filter(keep)
        return frame.sort_by(_TICK) if _TICK in frame else frame

    def _run(self, specs: list[_Spec], as_series: bool = False) -> Series | DataFrame:
        """The aggregates ``(out, column, func)`` per bin, empty bins listed."""
        from .dataframe import DataFrame
        from .groupby import GroupBy

        frame = self._frame(any(f in ("first", "last") for _, _, f in specs))
        plain = [_spec(f, c, o) for o, c, f in specs if f != "nunique"]
        grouped = GroupBy(frame, [_BIN])
        if plain:
            counted = grouped.agg(*plain)
        else:
            sized = grouped.size()
            if not isinstance(sized, DataFrame):
                raise TypeError("group size is not a frame")
            counted = sized.select(_BIN)
        for out, source, func in specs:
            if func == "nunique":
                distinct = GroupBy(frame.select(_BIN, cast(str, source)), [_BIN]).nunique()
                counted = counted.join(
                    distinct.rename({cast(str, source): out}), on=_BIN, how="left"
                )
        count = self._bins.count
        grid = DataFrame(
            {_BIN: positions(count) if count else _series().from_list([], DType.INT64)}
        )
        full = grid.join(counted, on=_BIN, how="left")
        for out, _, func in specs:
            if func in _ZERO or func == "prod":
                filled = _ext.DataFrame.__getitem__(full, out).fillna(1 if func == "prod" else 0)
                full = full.with_column(out, filled)
        outs = [o for o, _, _ in specs]
        out_frame = full.with_column(self._name, self._bins.labels).select(*outs, self._name)
        out_frame = out_frame.set_index([self._name])
        if as_series:
            series = _ext.DataFrame.__getitem__(out_frame, outs[0])
            return series.rename(self._label)
        return out_frame

    def _simple(self, func: str) -> Series | DataFrame:
        return self._run(self._simple_specs(func), self._is_series)

    def sum(self) -> Series | DataFrame:
        return self._simple("sum")

    def prod(self) -> Series | DataFrame:
        return self._simple("prod")

    def mean(self) -> Series | DataFrame:
        return self._simple("mean")

    def median(self) -> Series | DataFrame:
        return self._simple("median")

    def min(self) -> Series | DataFrame:
        return self._simple("min")

    def max(self) -> Series | DataFrame:
        return self._simple("max")

    def count(self) -> Series | DataFrame:
        return self._simple("count")

    def first(self) -> Series | DataFrame:
        return self._simple("first")

    def last(self) -> Series | DataFrame:
        return self._simple("last")

    def nunique(self) -> Series | DataFrame:
        return self._simple("nunique")

    def std(self, ddof: int = 1) -> Series | DataFrame:
        _check_ddof(ddof)
        return self._simple("std")

    def var(self, ddof: int = 1) -> Series | DataFrame:
        _check_ddof(ddof)
        return self._simple("var")

    def size(self) -> Series | DataFrame:
        """The row count of each bin: a Series for a frame too, as pandas."""
        return self._run([("size", None, "size")], True)

    def ohlc(self) -> DataFrame:
        """``open``, ``high``, ``low`` and ``close`` (first, max, min, last) of each
        bin; a frame gets ``<column>_open`` and so on in place of the pandas
        column levels."""
        if self._is_series:
            return cast("DataFrame", self._run([(o, self._columns[0], f) for o, f in _OHLC]))
        return cast(
            "DataFrame",
            self._run([(f"{c}_{o}", c, f) for c in self._columns for o, f in _OHLC]),
        )

    def agg(self, func: object = None, *args: object, **named: object) -> Series | DataFrame:
        """The pandas forms: a name, a list of names, a ``{column: name or names}`` dict
        and ``out=("column", name)`` keywords; a name is one of ``sum``,
        ``prod``, ``mean``, ``median``, ``min``, ``max``, ``count``, ``size``,
        ``first``, ``last``, ``std``, ``var``, ``nunique``."""
        if args:
            raise TypeError("agg takes the functions by name only")
        simple = isinstance(func, str) and not named
        return self._run(self._agg_specs(func, named), simple and self._is_series)

    aggregate = agg

    def _agg_specs(self, func: object, named: dict[str, object]) -> list[_Spec]:
        if named:
            if func is not None:
                raise TypeError("agg takes a function or keyword aggregations, not both")
            specs: list[_Spec] = []
            for out, pair in named.items():
                if not (isinstance(pair, tuple) and len(pair) == 2):
                    raise TypeError(f"{out}: a named aggregation is (column, function)")
                specs.append((out, str(pair[0]), _func_name(pair[1])))
            return specs
        if isinstance(func, str):
            return self._simple_specs(func)
        if isinstance(func, (list, tuple)):
            names = [_func_name(f) for f in func]
            listed: list[_Spec] = [(f, self._columns[0], f) for f in names]
            if not self._is_series:
                listed = [(f"{c}_{f}", c, f) for c in self._columns for f in names]
            return listed
        if isinstance(func, dict):
            specs = []
            for column, f in func.items():
                if column not in self._data:
                    raise KeyError(f"Column(s) ['{column}'] do not exist")
                if isinstance(f, (list, tuple)):
                    specs += [(f"{column}_{_func_name(g)}", column, _func_name(g)) for g in f]
                else:
                    specs.append((column, column, _func_name(f)))
            return specs
        raise TypeError("agg takes a function name, a list, a dict or keyword aggregations")

    def _simple_specs(self, name: str) -> list[_Spec]:
        func = _func_name(name)
        if func == "size":
            return [("size", None, "size")]
        listed: list[_Spec] = [(c, c, func) for c in self._columns]
        return listed


def _check_ddof(ddof: int) -> None:
    if ddof != 1:
        raise NotImplementedError("resample: only ddof=1 is supported")


def _func_name(func: object) -> str:
    if not isinstance(func, str) or func not in _SIMPLE:
        raise TypeError(f"resample aggregation {func!r} is not supported")
    return func


def _column(frame: DataFrame, name: str) -> Series:
    return _ext.DataFrame.__getitem__(frame, name)


def resample(
    obj: Frameish,
    rule: object,
    closed: str | None,
    label: str | None,
    on: str | None,
    level: LevelArg,
    origin: object,
    offset: object,
) -> TimeResampler:
    """The resampler of ``obj`` by its Timestamp index level, or by the column
    ``on`` of a frame."""
    from .series import Series

    series = obj if isinstance(obj, Series) else None
    parse_rule(rule, "ns", False)
    check_sides(closed, label)
    span = _origin_ns(origin, "ns")
    shift = 0 if offset is None else _timedelta_ns(offset, "offset")
    if on is not None:
        frame = obj if not isinstance(obj, Series) else None
        if frame is None or on not in frame.columns:
            raise KeyError(on)
        if level is not None:
            raise ValueError("The Grouper cannot specify both a key and a level!")
        key = _column(frame, on)
        if not is_timestamp(key):
            raise _kind_error("Index")
        label_name = on
        data = {c: _column(frame, c) for c in frame.columns if c != on}
    else:
        what = (
            "Only valid with DatetimeIndex, TimedeltaIndex or PeriodIndex, "
            "but got an instance of '{}'"
        )
        at, key = index_level(obj, level, what)
        label_name = _levels(obj)[0][at]
        if isinstance(obj, Series):
            data = {_VALUE: drop_levels(obj)}
        else:
            data = {c: _column(obj, c) for c in obj.columns}
    parsed = parse_rule(rule, unit_name(key), key.timezone != "")
    aware = _origin_aware(origin)
    if parsed.calendar is None and aware is not None and aware != (key.timezone != ""):
        raise ValueError("The origin must have the same timezone as the index.")
    bins = make_bins(key, parsed, closed, label, span, shift)
    return TimeResampler(
        data, bins, label_name, series.name if series is not None else None, series is not None
    )


# ---- at_time, between_time ---------------------------------------------------------


_CLOCK = re.compile(r"^(\d{1,2}):(\d{2})(?::(\d{2})(?:\.(\d+))?)?\s*([AaPp][Mm])?$")


def _time_us(value: object, strict: bool = False) -> int:
    """A time of day as microseconds since midnight, the resolution pandas
    compares at: a ``datetime.time`` or text (``"09:30"``, ``"9:30:15.5"``,
    ``"9:30AM"``); finer digits are cut off. ``strict`` (``between_time``)
    refuses a fraction with AM or PM, which only ``at_time`` accepts."""
    if isinstance(value, datetime.time):
        secs = (value.hour * 60 + value.minute) * 60 + value.second
        return secs * 1_000_000 + value.microsecond
    if isinstance(value, str):
        text = value.strip()
        for fmt in _TIME_FORMATS:
            try:
                parsed = datetime.datetime.strptime(text, fmt)
            except ValueError:
                continue
            return _time_us(parsed.time())
        match = _CLOCK.match(text)
        if match:
            hour, minute = int(match.group(1)), int(match.group(2))
            second = int(match.group(3) or 0)
            micro = int((match.group(4) or "0")[:6].ljust(6, "0"))
            if strict and match.group(5) and match.group(4):
                raise ValueError(f"Cannot convert arg {[value]} to a time")
            if match.group(5):
                if not 1 <= hour <= 12:
                    raise ValueError(f"Cannot convert arg {[value]} to a time")
                hour = hour % 12 + (12 if match.group(5).lower() == "pm" else 0)
            if hour < 24 and minute < 60 and second < 60:
                return _time_us(datetime.time(hour, minute, second, micro))
        raise ValueError(f"Cannot convert arg {[value]} to a time")
    raise TypeError(f"Cannot convert input [{value}] of type {type(value)} to time")


def _time_of_day_us(key: Series) -> Series:
    """The time of day of each instant, in microseconds (cut off below)."""
    per_unit = UNIT_NS[unit_name(key)]
    day = 86_400_000_000_000 // per_unit
    ticks = wall(key).astype("int64")
    tod = ticks - ticks.floordiv(day) * day
    return tod.floordiv(1000 // per_unit) if per_unit < 1000 else tod * (per_unit // 1000)


def at_time_mask(obj: Frameish, time: object) -> Series:
    """The rows whose time of day is ``time``."""
    _, key = index_level(obj, None, "Index must be DatetimeIndex")
    return _time_of_day_us(key).eq(_time_us(time))


def between_time_mask(
    obj: Frameish, start_time: object, end_time: object, inclusive: str
) -> Series:
    """The rows whose time of day lies between the two, ``inclusive`` as pandas;
    an end before the start wraps past midnight."""
    if inclusive not in ("both", "neither", "left", "right"):
        raise ValueError(
            "Inclusive has to be either string of 'both', 'neither', 'left' or 'right'"
        )
    _, key = index_level(obj, None, "Index must be DatetimeIndex")
    lo, hi = _time_us(start_time, True), _time_us(end_time, True)
    tod = _time_of_day_us(key)
    if key.null_count:
        tod = tod.fillna(-1)  # pandas reads a missing instant as the time -1
    after = tod.ge(lo) if inclusive in ("both", "left") else tod.gt(lo)
    before = tod.le(hi) if inclusive in ("both", "right") else tod.lt(hi)
    return after.logical(0, before) if lo <= hi else after.logical(1, before)


# ---- tz_localize, tz_convert ---------------------------------------------------------


def retime(obj: Frameish, level: LevelArg, convert: Callable[[Series], Series]) -> Frameish:
    """``obj`` with the Timestamp index level run through ``convert``."""
    names, levels = _levels(obj)
    at = _level_of(names, level)
    if not names or not is_timestamp(levels[at]):
        raise TypeError("index is not a valid DatetimeIndex or PeriodIndex")
    from .series import Series

    out = convert(levels[at])
    if isinstance(obj, Series):
        return with_levels(obj, names, [out if i == at else v for i, v in enumerate(levels)])
    return obj.with_column(names[at], out)


def check_zone(tz: str | None) -> None:
    """The errors pandas gives for a zone it does not know: ``KeyError`` for an
    unknown name, ``IndexError`` for an empty one, ``ValueError`` for a
    malformed one. The engine reads the zone files, so it decides."""
    if tz is None:
        return
    if not isinstance(tz, str):
        raise TypeError(f"tz must be a string or None, not {type(tz).__name__}")
    if tz == "":
        raise IndexError("list index out of range")
    if re.match(r"^UTC[+-]", tz, re.I):
        raise ValueError(f"{tz!r} is not a valid time zone")
    if tz in ("Z", "z"):
        # The engine reads Z as UTC; pandas and polars do not know the name.
        raise KeyError(f"'{tz}'")
    _ext._series_nulls(int(DType.TIMESTAMP), 0).with_timezone(tz)


_AMBIGUOUS = {"raise": 0, "earliest": 1, "latest": 2, "NaT": 3, "null": 3, "infer": 4}
_NONEXISTENT = {"raise": 0, "shift_forward": 1, "shift_backward": 2, "NaT": 3, "null": 3}


def _nonexistent_code(nonexistent: object, unit: str) -> tuple[int, int]:
    """The engine code of ``nonexistent`` and its shift in ticks of ``unit``."""
    if isinstance(nonexistent, datetime.timedelta):
        ns = (
            nonexistent.days * 86400 + nonexistent.seconds
        ) * 1_000_000_000 + nonexistent.microseconds * 1000
        return 4, int(ns / UNIT_NS[unit])
    code = _NONEXISTENT.get(nonexistent) if isinstance(nonexistent, str) else None
    if code is None:
        raise ValueError(
            "The nonexistent argument must be one of 'raise', 'NaT', 'shift_forward', "
            "'shift_backward' or a timedelta object"
        )
    return code, 0


def localize(
    key: Series,
    tz: str | None,
    ambiguous: object = "raise",
    nonexistent: object = "raise",
) -> Series:
    """A naive column read as wall times in ``tz``; ``None`` turns an aware
    column into its wall times. ``ambiguous`` is ``"raise"``, ``"infer"``,
    ``"NaT"`` (or the polars ``"earliest"``, ``"latest"``, ``"null"``) or a
    boolean array (``True`` for the first of the two instants);
    ``nonexistent`` is ``"raise"``, ``"shift_forward"``, ``"shift_backward"``,
    ``"NaT"`` or a ``timedelta``."""
    if tz is not None and key.timezone:
        raise TypeError("Already tz-aware, use tz_convert to convert.")
    check_zone(tz)
    code, shift = _nonexistent_code(nonexistent, unit_name(key))
    if tz is None:
        return key.dt_tz_localize("")
    if isinstance(ambiguous, str):
        mode = _AMBIGUOUS.get(ambiguous)
        if mode is None:
            raise ValueError(
                f"'{ambiguous}' is not valid for the ambiguous argument: "
                "'raise', 'infer', 'NaT' or a boolean array"
            )
        return key.dt_tz_localize(tz, mode, code, shift)
    flags = _series().from_list([bool(v) for v in cast("Sequence[bool]", ambiguous)], DType.BOOL)
    if len(flags) != len(key):
        raise ValueError("Length of ambiguous bool-array must be the same size as vals")
    first = key.dt_tz_localize(tz, 1, code, shift)
    second = key.dt_tz_localize(tz, 2, code, shift)
    return _like(
        first.astype("int64").where(flags, second.astype("int64")),
        first,
    )


_POLARS_AMBIGUOUS = ("raise", "earliest", "latest", "null")


def replace_zone(
    key: Series,
    tz: str | None,
    use_earliest: bool | None,
    ambiguous: object,
    non_existent: str,
) -> Series:
    """polars ``replace_time_zone``: the wall times kept under zone ``tz``."""
    if non_existent not in ("raise", "null"):
        raise ValueError(f"`non_existent` must be one of {{'raise', 'null'}}, got {non_existent}")
    if use_earliest is not None:
        ambiguous = "earliest" if use_earliest else "latest"
    if not isinstance(ambiguous, str) or ambiguous not in _POLARS_AMBIGUOUS:
        raise ValueError(
            f"`ambiguous` must be one of {{'raise', 'earliest', 'latest', 'null'}}, got {ambiguous}"
        )
    if tz is not None and key.timezone:
        # polars replaces the zone of an aware column: its wall times stay.
        key = key.dt_tz_localize("")
    return localize(key, tz, ambiguous, "NaT" if non_existent == "null" else "raise")


def convert_time_zone(key: Series, tz: str) -> Series:
    """polars ``convert_time_zone``: a naive column is read as UTC."""
    check_zone(tz)
    return key.with_timezone(tz)


def convert(key: Series, tz: str | None) -> Series:
    """The same instants shown in zone ``tz`` (``None``: naive UTC)."""
    check_zone(tz)
    if not key.timezone:
        raise TypeError("Cannot convert tz-naive timestamps, use tz_localize to localize")
    return key.with_timezone(tz or "")


# ---- asfreq ------------------------------------------------------------------------


def grid_of(key: Series, freq: object) -> Series:
    """The ``date_range(min, max, freq)`` of a Timestamp column."""
    unit = unit_name(key)
    rule = parse_rule(freq, unit, key.timezone != "")
    calendar_zone = rule.calendar is not None and zoned(key)
    # A calendar step in a zone runs on the wall times and is mapped back.
    source = wall(key) if calendar_zone else key
    ticks = source.astype("int64")
    low, high = ticks.min(), ticks.max()
    per_unit = UNIT_NS[unit]
    if low is None or high is None:
        return _like(_series().from_list([], DType.INT64), key)
    low, high = int(cast(int, low)), int(cast(int, high))
    if rule.calendar is None:
        width = rule.ns // per_unit
        if width * per_unit != rule.ns:
            raise ValueError(f"asfreq: {rule.text!r} is not a whole number of {unit}")
        return _like(positions((high - low) // width + 1) * width + low, key)
    cal = rule.calendar
    day = 86_400_000_000_000 // per_unit
    start_day = low // day
    time_of_day = low - start_day * day
    a = cal.floor(start_day + _EPOCH_ORDINAL)
    if cal.date_of(a) < start_day + _EPOCH_ORDINAL:
        a += 1
    values = []
    while True:
        stamp = (cal.date_of(a) - _EPOCH_ORDINAL) * day + time_of_day
        if stamp > high:
            break
        values.append(stamp)
        a += cal.n
    grid = _like(_series().from_list(values, DType.INT64), source)
    return localize_back(grid, key) if calendar_zone else grid


def _is_grid(key: Series, freq: object) -> bool:
    """Whether the Timestamp column already is the grid of a fixed width: sorted,
    no nulls, one row at every step from the first, so a reindex changes
    nothing."""
    unit = unit_name(key)
    rule = parse_rule(freq, unit, key.timezone != "")
    n = len(key)
    if rule.calendar is not None or n < 2 or key.null_count:
        return False
    per_unit = UNIT_NS[unit]
    width = rule.ns // per_unit
    if width * per_unit != rule.ns:
        return False
    ticks = drop_levels(key).astype("int64")
    if not ticks.is_sorted():
        return False
    steps = ticks - int(cast(int, ticks.iloc[0]))
    return not bool((steps - positions(n) * width).ne(0).any())


def _gather(obj: Frameish, name: str, rows: Series, grid: Series) -> Frameish:
    """``obj`` with the row ``rows[i]`` at grid instant ``i`` (a null row for -1)
    under the Timestamp index ``grid`` named ``name``."""
    from .dataframe import DataFrame
    from .series import Series

    if isinstance(obj, Series):
        return with_levels(drop_levels(obj).gather(rows), [name], [grid])
    if not isinstance(obj, DataFrame):
        raise TypeError("asfreq needs a Series or a DataFrame")
    return obj.reset_index(drop=True).gather(rows).with_column(name, grid).set_index(name)


def _grid_rows(key: Series, freq: object, method: str | None, fill_value: object) -> Series | None:
    """The source row of each grid instant, -1 for none, when ``key`` is strictly
    increasing and every instant is on the grid of a fixed width, so the lookup
    is arithmetic and a take replaces the hash; ``None`` when it is not (a
    duplicate, an off-grid row, a null, a ``bfill`` or a ``fill_value``)."""
    if fill_value is not None or method not in (None, "ffill", "pad"):
        return None
    unit = unit_name(key)
    rule = parse_rule(freq, unit, key.timezone != "")
    n = len(key)
    per_unit = UNIT_NS[unit]
    if rule.calendar is not None or n < 2 or key.null_count or rule.ns % per_unit:
        return None
    width = rule.ns // per_unit
    ticks = drop_levels(key).astype("int64")
    if not ticks.is_sorted():
        return None
    steps = ticks - int(cast(int, ticks.iloc[0]))
    slots = steps.floordiv(width)
    if (slots * width - steps).ne(0).any():
        return None
    if (slots.slice(1, n) - slots.slice(0, n - 1)).le(0).any():
        return None
    count = int(cast(int, slots.iloc[n - 1])) + 1
    gaps = drop_levels(positions(count))
    rows = (gaps.cut(slots, right=False) - 1).astype("int64")
    if method is None:
        present = (drop_levels(slots.take(rows)) - gaps).eq(0).astype("int64")
        rows = (rows + 1) * present - 1
    return rows


def _axis_rows(axis: object, kind: str) -> bool:
    """Whether ``axis`` is the rows; ``ValueError`` for an unknown axis."""
    if axis in (0, "index", "rows"):
        return True
    if axis in (1, "columns") and kind == "DataFrame":
        return False
    raise ValueError(f"No axis named {axis} for object type {kind}")


class TimeIndexMixin:
    """The time methods both :class:`Series` and :class:`DataFrame` have over a
    Timestamp index; a class gives the one hook ``_rows_where``."""

    __slots__ = ()

    def _rows_where(self, mask: Series) -> Self:
        raise NotImplementedError

    def _this(self) -> Frameish:
        return cast("Frameish", self)

    def _kind(self) -> str:
        from .series import Series

        return "Series" if isinstance(self, Series) else "DataFrame"

    def resample(
        self,
        rule: int | str | datetime.timedelta,
        closed: Literal["left", "right"] | None = None,
        label: Literal["left", "right"] | None = None,
        on: str | None = None,
        level: LevelArg = None,
        origin: str | datetime.datetime = "start_day",
        offset: str | datetime.timedelta | None = None,
        group_keys: bool = False,
    ) -> TimeResampler:
        """Time bins of ``rule`` over a Timestamp index (or ``level`` of a
        MultiIndex), as pandas ``resample``; see :meth:`DataFrame.resample`."""
        return resample(self._this(), rule, closed, label, on, level, origin, offset)

    def asfreq(
        self,
        freq: str | datetime.timedelta,
        method: Literal["ffill", "pad", "bfill", "backfill"] | None = None,
        how: str | None = None,
        normalize: bool = False,
        fill_value: object = None,
    ) -> Self:
        """The rows on the regular grid of ``freq`` from the earliest to the
        latest time of the Timestamp index (pandas ``asfreq``): each grid
        instant takes the row at exactly that time and is missing otherwise,
        filled by ``method`` or ``fill_value`` (a row that is already there
        keeps its own nulls); ``normalize`` sets the labels to midnight. The
        index must hold each instant once."""
        from ._reindex import reindex_rows

        obj = self._this()
        parse_rule(freq, "ns", False)
        at, key = index_level(obj, None, "asfreq needs a DatetimeIndex, not '{}'")
        if len(key) == 0:
            return cast("Self", obj)
        if key.null_count == len(key):
            raise ValueError("Neither `start` nor `end` can be NaT")
        names = _levels(obj)[0]
        if _is_grid(key, freq):
            out = obj
        elif (rows := _grid_rows(key, freq, method, fill_value)) is not None:
            out = _gather(obj, names[at], rows, grid_of(key, freq))
        else:
            grid = grid_of(key, freq).rename(public_name(names[at]))
            out = reindex_rows(obj, grid, method, fill_value, None, None)
        if normalize:
            out = retime(out, None, lambda k: k.dt.normalize())
        return cast("Self", out)

    def at_time(
        self, time: datetime.time | str, asof: bool = False, axis: int | str | None = None
    ) -> Self:
        """The rows whose time of day is ``time`` (a ``datetime.time`` or
        ``"HH:MM[:SS]"``), keeping the index (pandas ``at_time``)."""
        if axis is not None:
            _axis_rows(axis, self._kind())
        return self._rows_where(at_time_mask(self._this(), time))

    def between_time(
        self,
        start_time: datetime.time | str,
        end_time: datetime.time | str,
        inclusive: Literal["both", "neither", "left", "right"] = "both",
        axis: int | str | None = None,
    ) -> Self:
        """The rows whose time of day lies between the two times, ``inclusive``
        as pandas; an end before the start wraps past midnight."""
        if axis is not None:
            _axis_rows(axis, self._kind())
        return self._rows_where(between_time_mask(self._this(), start_time, end_time, inclusive))

    def tz_localize(
        self,
        tz: str | None,
        axis: int | str = 0,
        level: LevelArg = None,
        copy: bool | None = None,
        ambiguous: str | Sequence[bool] = "raise",
        nonexistent: str | datetime.timedelta = "raise",
    ) -> Self:
        """The Timestamp index read as wall times in zone ``tz`` (pandas
        ``tz_localize``); ``None`` turns an aware index into its wall times.
        ``ambiguous`` and ``nonexistent`` say what a wall time that happens
        twice or never becomes, as pandas; ``shift_forward`` and
        ``shift_backward`` use the instant of the change, not pandas' whole-hour
        rounding."""
        if not _axis_rows(axis, self._kind()):
            raise TypeError("tz_localize: the columns are not a DatetimeIndex")
        return cast(
            "Self",
            retime(self._this(), level, lambda k: localize(k, tz, ambiguous, nonexistent)),
        )

    def tz_convert(
        self,
        tz: str | None,
        axis: int | str = 0,
        level: LevelArg = None,
        copy: bool | None = None,
    ) -> Self:
        """The Timestamp index shown in zone ``tz``, the instants kept (pandas
        ``tz_convert``); the index must be zone-aware."""
        if not _axis_rows(axis, self._kind()):
            raise TypeError("tz_convert: the columns are not a DatetimeIndex")
        return cast("Self", retime(self._this(), level, lambda k: convert(k, tz)))


__all__ = ["TimeIndexMixin", "TimeResampler", "is_timestamp", "resample"]
