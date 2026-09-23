"""Typed vocabularies for the TraceViewer builder API.

These are ``str`` enums, so they pass straight into the string-accepting
builder ops (``phase``/``group_by``/``agg``) - ``phase(Phase.EVENTS)`` is the
same call as ``phase("events")`` - while giving callers a discoverable, typo-safe
set instead of bare strings.
"""

from enum import Enum, IntEnum
from typing import Dict, Optional


class Phase(str, Enum):
    """Which records a view scans."""

    EVENTS = "events"  # ph="X"
    COUNTERS = "counters"  # ph="C"
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


# Case-insensitive name -> DType, for Series.astype("int64") / .astype("Int64").
_DTYPE_BY_NAME: Dict[str, DType] = {d.name.lower(): d for d in DType}
