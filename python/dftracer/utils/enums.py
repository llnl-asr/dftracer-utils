# SPDX-License-Identifier: MIT
"""Typed vocabularies for the TraceViewer builder API, and the one table of
each fact the package keeps about dtypes and time units.

The ``str`` enums pass straight into the string-accepting builder ops
(``phase``/``group_by``/``agg``) - ``phase(Phase.EVENTS)`` is the same call as
``phase("events")`` - while giving callers a discoverable, typo-safe set instead
of bare strings.

Every other module reads a dtype set from a ``DType`` property and a time unit
from ``UNIT_NS`` or ``UNIT_CODES``; none keeps a table of its own.
"""

import re
from enum import Enum, IntEnum
from typing import Dict, Optional, Tuple, Union

__all__ = [
    "Phase",
    "GroupKey",
    "AggOp",
    "TimeUnit",
    "DType",
]


class Phase(str, Enum):
    """Which records a view scans."""

    EVENTS = "events"  # ph=1
    COUNTERS = "counters"  # ph=2
    ANY = "any"


class GroupKey(str, Enum):
    """A group_by dimension."""

    NAME = "name"
    CAT = "cat"
    PID = "pid"
    TID = "tid"
    FHASH = "fhash"

    @staticmethod
    def arg(key: str) -> str:
        """Group by an args-map entry: ``GroupKey.arg("epoch")`` -> ``"arg:epoch"``."""
        return f"arg:{key}"


class AggOp(str, Enum):
    """An aggregation reducer (the ``op`` in an ``"op:field"`` agg spec)."""

    COUNT = "count"
    SUM = "sum"
    SUMSQ = "sumsq"
    MIN = "min"
    MAX = "max"
    MEAN = "mean"
    VAR = "var"
    STD = "std"
    SKEW = "skew"
    KURT = "kurt"
    HIST = "hist"  # raw histogram: an arrow list<struct<lo, hi, count>> column
    ARGMAX = "argmax"
    # Occupancy: field-less, always measured over ``dur``. See the aggregation
    # guide for what each metric means.
    BUSY = "busy"
    CONCURRENCY = "concurrency"
    UTILIZATION = "utilization"
    ACTIVE = "active"

    def of(self, field: str = "", by: Optional[str] = None) -> str:
        """Build an agg spec: ``AggOp.STD.of("dur")`` -> ``"std:dur"``;
        ``AggOp.ARGMAX.of("name", by="dur")`` -> ``"argmax:name:dur"``. The
        field-less ops (``COUNT`` and the occupancy ops) ignore ``field`` and
        return the bare op name, so ``AggOp.BUSY.of()`` -> ``"busy"``."""
        if self in _FIELDLESS:
            return self.value if not field else f"{self.value}:{field}"
        spec = f"{self.value}:{field}"
        return f"{spec}:{by}" if by else spec


# Ops whose spec is the bare op name: the group count and the occupancy metrics,
# which are always measured over ``dur``.
_FIELDLESS = frozenset(
    {AggOp.COUNT, AggOp.BUSY, AggOp.CONCURRENCY, AggOp.UTILIZATION, AggOp.ACTIVE}
)


class TimeUnit(str, Enum):
    """Target unit for ``ts``/``dur`` scaling."""

    NS = "ns"
    US = "us"
    MS = "ms"
    SEC = "sec"


# Accepts a TimeUnit, its string value, or None (native, no scaling).
TimeUnitLike = Union[str, "TimeUnit", None]


class DType(IntEnum):
    """A ``Series`` element dtype, mirroring the C++ ``dataframe::TypeId``
    values 1:1 (``include/dftracer/utils/dataframe/types.h``). Pass to
    :meth:`Series.astype` / :meth:`Series.cast`, or read off ``Series.type``."""

    UNKNOWN = 0
    BOOL = 1
    INT8 = 2
    INT16 = 3
    INT32 = 4
    INT64 = 5
    UINT8 = 6
    UINT16 = 7
    UINT32 = 8
    UINT64 = 9
    FLOAT32 = 10
    FLOAT64 = 11
    STRING = 12
    BINARY = 13
    LIST = 14
    STRUCT = 15
    FLOAT16 = 16
    DATE32 = 17
    DATE64 = 18
    TIME32 = 19
    TIME64 = 20
    TIMESTAMP = 21
    DURATION = 22
    DECIMAL128 = 23
    DECIMAL256 = 24
    FIXED_SIZE_BINARY = 25
    LARGE_STRING = 26
    LARGE_BINARY = 27
    LARGE_LIST = 28
    FIXED_SIZE_LIST = 29
    MAP = 30

    @property
    def is_integer(self) -> bool:
        """Whether this is a signed or an unsigned integer type."""
        return self in _INTEGERS

    @property
    def is_float(self) -> bool:
        """Whether this is ``FLOAT32`` or ``FLOAT64``."""
        return self in _FLOATS

    @property
    def is_numeric(self) -> bool:
        """Whether this is an integer or a float type."""
        return self in _INTEGERS or self in _FLOATS

    @property
    def int_range(self) -> Optional[Tuple[int, int]]:
        """The lowest and highest value of an integer type, ``None`` for the
        other types."""
        bits = _SIGNED_BITS.get(self)
        if bits is not None:
            return -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        bits = _UNSIGNED_BITS.get(self)
        return None if bits is None else (0, (1 << bits) - 1)


_SIGNED_BITS = {DType.INT8: 8, DType.INT16: 16, DType.INT32: 32, DType.INT64: 64}
_UNSIGNED_BITS = {DType.UINT8: 8, DType.UINT16: 16, DType.UINT32: 32, DType.UINT64: 64}
_INTEGERS = frozenset(_SIGNED_BITS) | frozenset(_UNSIGNED_BITS)
_FLOATS = frozenset({DType.FLOAT32, DType.FLOAT64})


# Case-insensitive name -> DType, for Series.astype("int64") / .astype("Int64").
DTYPE_BY_NAME: Dict[str, DType] = {d.name.lower(): d for d in DType}
# The types a cast of an expression or a plan names, and the name of each.
CAST_DTYPES: Dict[str, DType] = {
    name: DTYPE_BY_NAME[name] for name in ("int64", "uint64", "float64", "bool", "string", "binary")
}


Bytes = Union[int, str]
Duration = Union[int, float, str]

_NUM = re.compile(r"^\s*([+-]?(?:\d+\.?\d*|\.\d+))\s*([a-zA-Z]*)\s*$")

# Nanoseconds in one unit, by each spelling a rule such as "5min" may use. The
# frequency, epoch and duration tables below take their rows from here.
UNIT_NS: Dict[str, int] = {
    "ns": 1,
    "us": 1_000,
    "µs": 1_000,
    "ms": 1_000_000,
    "s": 1_000_000_000,
    "sec": 1_000_000_000,
    "min": 60_000_000_000,
    "m": 60_000_000_000,
    "t": 60_000_000_000,
    "h": 3_600_000_000_000,
    "d": 86_400_000_000_000,
}
# The native time-unit code of a temporal column, in the order of the C enum; its
# keys are the units a column counts in.
UNIT_CODES: Dict[str, int] = {"s": 0, "ms": 1, "us": 2, "ns": 3}
# Units a ``.dt`` accessor reads a column in: the native ones and ``sec``.
DT_UNIT_CODES: Dict[str, int] = {**UNIT_CODES, "sec": UNIT_CODES["s"]}
# The units ``freq`` strings of the ``.dt`` accessor name, and the units an epoch
# is counted in.
FREQ_NS: Dict[str, int] = {
    u: UNIT_NS[u] for u in ("ns", "us", "ms", "s", "sec", "min", "t", "h", "d")
}
EPOCH_NS: Dict[str, int] = {u: UNIT_NS[u] for u in ("ns", "us", "ms", "s", "d")}
# The duration suffixes of the duql grammar.
DUQL_DURATION_UNITS = ("ns", "us", "ms", "s", "m", "h", "d")
_OWN_UNIT_TYPES = frozenset((DType.TIMESTAMP, DType.DURATION, DType.TIME64))


def dt_unit(kind: int, time_unit: int, given: Optional[str]) -> str:
    """The unit ``.dt`` reads a column of type code ``kind`` in: ``given``, else
    a temporal column's own unit (date64 counts milliseconds), else
    microseconds, as an Int64 is read."""
    if given is not None:
        return given
    if kind in _OWN_UNIT_TYPES:
        return tuple(UNIT_CODES)[time_unit]
    return "ms" if kind == DType.DATE64 else "us"


_BYTE_POWER = {"": 0, "k": 1, "m": 2, "g": 3, "t": 4, "p": 5}

_DURATION_SPELLINGS = {
    "s": "s",
    "sec": "sec",
    "secs": "sec",
    "ns": "ns",
    "us": "us",
    "ms": "ms",
    "m": "m",
    "min": "min",
    "mins": "min",
    "h": "h",
    "hr": "h",
    "hrs": "h",
    "d": "d",
    "day": "d",
    "days": "d",
}
_DURATION_SECONDS = {"": 1.0, **{k: UNIT_NS[v] / 1e9 for k, v in _DURATION_SPELLINGS.items()}}


def _parse_bytes(text: str) -> Optional[int]:
    """Parse a size such as ``"512"``, ``"64KB"``, ``"1.5GiB"``, ``"8kb"`` into
    a byte count, or return None on malformed input."""
    match = _NUM.match(text)
    if not match:
        return None
    value = float(match.group(1))
    unit = match.group(2)
    if value < 0:
        return None
    bits = False
    if unit[-1:] == "B":
        unit = unit[:-1]
    elif unit[-1:] == "b":
        bits = True
        unit = unit[:-1]
    if unit[-1:] in ("i", "I"):
        unit = unit[:-1]
    power = _BYTE_POWER.get(unit.lower())
    if power is None:
        return None
    result = value * (1024**power)
    if bits:
        result /= 8.0
    return int(result)


def _parse_duration_seconds(text: str) -> Optional[float]:
    """Parse a duration such as ``"30"``, ``"500ms"``, ``"1.5h"`` into seconds
    (a bare number is seconds), or return None on malformed input."""
    match = _NUM.match(text)
    if not match:
        return None
    value = float(match.group(1))
    if value < 0:
        return None
    factor = _DURATION_SECONDS.get(match.group(2).lower())
    if factor is None:
        return None
    return value * factor


def coerce_bytes(value: Bytes, name: str = "value") -> int:
    """Return a byte count from an int (already bytes) or a unit string.

    Raises ValueError on a malformed string.
    """
    if isinstance(value, str):
        parsed = _parse_bytes(value)
        if parsed is None:
            raise ValueError(
                f"Invalid byte size for {name}: {value!r} (use e.g. 65536, 64KB, 1.5GB)"
            )
        return parsed
    return int(value)


def coerce_duration(value: Duration, native_per_second: float, name: str = "value") -> float:
    """Return a duration in the caller's native unit from a number (already in
    that unit) or a unit string (converted from seconds).

    ``native_per_second`` is how many native units are in one second, e.g. 1e3
    for milliseconds. Raises ValueError on a malformed string.
    """
    if isinstance(value, str):
        seconds = _parse_duration_seconds(value)
        if seconds is None:
            raise ValueError(f"Invalid duration for {name}: {value!r} (use e.g. 30, 30s, 5m, 1.5h)")
        return seconds * native_per_second
    return float(value)


_RULE = re.compile(r"^\s*(\d+(?:\.\d+)?)?\s*([a-zA-Zµ]+)\s*$")


def rule_to_units(rule: Union[int, str], unit: str) -> int:
    """A pandas offset string (``"5s"``, ``"100ms"``, ``"1min"``, ``"2h"``)
    or an int of the column's own units, as a count of ``unit``."""
    if isinstance(rule, bool):
        raise TypeError("resample: rule must be a str or an int")
    if isinstance(rule, int):
        if rule <= 0:
            raise ValueError("resample: rule must be positive")
        return rule
    m = _RULE.match(rule)
    if not m:
        raise ValueError(f"resample: cannot parse rule {rule!r}")
    amount, name = float(m.group(1) or 1), m.group(2).lower()
    if m.group(1) is None and name not in UNIT_NS:
        raise ValueError(f"resample: cannot parse rule {rule!r}")
    if name not in UNIT_NS:
        raise ValueError(f"resample: unknown unit {m.group(2)!r} in {rule!r}")
    per = UNIT_NS.get(unit.lower())
    if per is None:
        raise ValueError(f"resample: unknown column unit {unit!r}")
    every = amount * UNIT_NS[name] / per
    if every < 1 or every != int(every):
        raise ValueError(f"resample: {rule!r} is not a whole number of {unit}")
    return int(every)
