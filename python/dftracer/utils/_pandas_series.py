# SPDX-License-Identifier: MIT
"""The pandas ``Series`` surface that is a composition of engine ops, mixed
into :class:`Series`: spellings (``between``, ``drop_duplicates``,
``factorize``), the frame-shaped results (``to_frame``, ``describe``,
``reset_index``), positional accessors and the conveniences."""

from __future__ import annotations

import math
import numbers
from typing import (
    TYPE_CHECKING,
    Callable,
    Iterator,
    Literal,
    Mapping,
    Sequence,
    TypeVar,
    Union,
    cast,
)

from .enums import DType

if TYPE_CHECKING:
    import numpy as np
    import pyarrow as pa
    from numpy import generic
    from pandas._libs.missing import NAType
    from typing_extensions import Concatenate, ParamSpec

    from .dataframe import DataFrame
    from .dftracer_utils_ext import Scalar
    from .groupby import GroupBy
    from .series import Series

    # A value of ``replace`` as the caller writes it, and as the engine takes it.
    PlainScalar = Union[int, float, str, None, NAType]
    ReplaceScalar = Union[PlainScalar, generic]
    ReplaceValue = Union[ReplaceScalar, Sequence[ReplaceScalar]]
    ReplaceKey = Union[ReplaceValue, Mapping[ReplaceScalar, ReplaceScalar]]

_Scalar = Union[int, float]
_R = TypeVar("_R")
if TYPE_CHECKING:
    _P = ParamSpec("_P")

_CMP_NE = 5  # DFTU_CMP_NE, dataframe/c_api.h

# Dtypes the row-by-row compare kernel takes; the others (nested, decimal)
# compare by value in Python.
_NATIVE_COMPARE = frozenset(
    [
        DType.BOOL,
        DType.INT8,
        DType.INT16,
        DType.INT32,
        DType.INT64,
        DType.UINT8,
        DType.UINT16,
        DType.UINT32,
        DType.UINT64,
        DType.FLOAT32,
        DType.FLOAT64,
        DType.STRING,
        DType.LARGE_STRING,
        DType.BINARY,
        DType.LARGE_BINARY,
        DType.DATE32,
        DType.DATE64,
        DType.TIME32,
        DType.TIME64,
        DType.TIMESTAMP,
        DType.DURATION,
    ]
)


def _kernel_operand(s: Series) -> Series:
    return s.astype("int8") if s.dtype == DType.BOOL else s


_TEMPORAL = frozenset([DType.TIME32, DType.TIME64, DType.TIMESTAMP, DType.DURATION])
_FLOATS = frozenset([DType.FLOAT32, DType.FLOAT64])
_BYTES = frozenset([DType.STRING, DType.LARGE_STRING, DType.BINARY, DType.LARGE_BINARY])


def _is_number(d: DType) -> bool:
    return d.is_numeric or d == DType.BOOL


def _scaled_differs(a: Series, b: Series) -> Series[bool]:
    """``differs`` for a duration or time in two units: the finer side ``f``
    times coarser equals the finer value exactly when it divides evenly and
    the quotient matches, so no value is scaled up and none can overflow."""
    if a.time_unit > b.time_unit:
        return _scaled_differs(b, a)
    factor = 1000 ** (b.time_unit - a.time_unit)
    fine, coarse = b.astype("int64"), a.astype("int64")
    same = ((fine % factor) == 0) & ((fine // factor) == coarse)
    ne = (~same).fillna(False)
    nulls = _kernel_operand(a.is_null()).compare_series(_kernel_operand(b.is_null()), _CMP_NE)
    return cast("Series[bool]", ne | nulls)


def differs(a: Series, b: Series) -> Series[bool]:
    """Bool, never null: true where the two Series (one length) hold different
    values; a null equals a null, a null differs from a value and a NaN equals
    a NaN only at the same position. A number against text differs on every
    present row. Times of one type but another unit or zone compare as
    instants (a timestamp), differ on every present row (a timestamp, aware
    against naive) or exactly in integers (a duration or time of two units); a
    pair of dtypes the row compare kernel does not take (nested, decimal,
    mixed time kinds) also compares by value in Python."""
    da, db = a.dtype, b.dtype
    if (_is_number(da) and db in _BYTES) or (da in _BYTES and _is_number(db)):
        return ~(a.is_null() & b.is_null())
    by_value = not ((da == db and da in _NATIVE_COMPARE) or (_is_number(da) and _is_number(db)))
    if da == db and da in _TEMPORAL and (a.time_unit, a.timezone) != (b.time_unit, b.timezone):
        if da != DType.TIMESTAMP:
            return _scaled_differs(a, b)
        elif bool(a.timezone) != bool(b.timezone):
            return ~(a.is_null() & b.is_null())
        else:
            a, b = a.dt.epoch("ns"), b.dt.epoch("ns")
    if by_value:
        from .series import Series

        return cast(
            "Series[bool]",
            Series.from_list(
                [
                    not (x == y or (x is None and y is None))
                    for x, y in zip(a.to_list(), b.to_list())
                ],
                DType.BOOL,
            ),
        )
    ne = _kernel_operand(a).compare_series(_kernel_operand(b), _CMP_NE).fillna(False)
    if da in _FLOATS and db in _FLOATS:
        ne = ne & ~(a.is_nan().fillna(False) & b.is_nan().fillna(False))
    if a.null_count == 0 and b.null_count == 0:
        return ne
    nulls = _kernel_operand(a.is_null()).compare_series(_kernel_operand(b.is_null()), _CMP_NE)
    return ne | nulls


def valid_edge(mask: Series, first: bool) -> int | None:
    """The position of the first (or last) true row of a Bool mask, ``None``
    when none is true."""
    if not mask.any():
        return None
    flags = mask.astype("int8")
    if first:
        return flags.arg_max()
    return len(flags) - 1 - flags.reverse().arg_max()


class Omitted:
    """The type of ``OMITTED``: ``replace``'s ``value`` when the caller gave none."""


OMITTED = Omitted()


def _plain(v: ReplaceScalar) -> PlainScalar:
    """A NumPy scalar as the Python number the engine accepts."""
    if type(v).__module__ == "numpy" and getattr(v, "ndim", 1) == 0:
        return cast("PlainScalar", getattr(v, "item")())
    raw: object = v
    return cast("PlainScalar", raw)


def _kind(v: PlainScalar) -> str:
    """What a replace value is: ``null``, ``nan``, ``str`` or ``num``."""
    if v is None or type(v).__name__ == "NAType":  # pd.NA, without importing pandas
        return "null"
    if isinstance(v, str):
        return "str"
    if type(v).__name__ in ("bool", "bool_"):
        raise TypeError("replace: bool values are not supported")
    if isinstance(v, numbers.Real):
        return "nan" if v != v else "num"
    raise TypeError(f"replace: cannot replace a {type(v).__name__} value")


def replace_pairs(
    to_replace: ReplaceKey, value: ReplaceValue | Omitted
) -> list[tuple[ReplaceScalar, ReplaceScalar]]:
    """The (old, new) pairs of any replace form."""
    if isinstance(to_replace, dict):
        if value is not OMITTED:
            raise TypeError("replace: value must be omitted when to_replace is a dict")
        return list(to_replace.items())
    if isinstance(value, Omitted):
        raise TypeError("replace() missing required argument: 'value'")
    if isinstance(to_replace, (list, tuple)):
        keys = cast("Sequence[ReplaceScalar]", to_replace)
        if isinstance(value, (list, tuple)):
            if len(value) != len(keys):
                raise ValueError(
                    f"replace: to_replace has {len(keys)} values but value has {len(value)}"
                )
            return list(zip(keys, cast("Sequence[ReplaceScalar]", value)))
        return [(old, cast("ReplaceScalar", value)) for old in keys]
    if isinstance(value, (list, tuple)):
        raise TypeError("replace: value is a list but to_replace is a scalar")
    return [(cast("ReplaceScalar", to_replace), cast("ReplaceScalar", value))]


def replace_in(
    s: Series,
    pairs: list[tuple[ReplaceScalar, ReplaceScalar]],
    column: str | None,
    strict: bool,
) -> Series:
    """``s`` with every pair applied together. ``column`` names a frame column in
    errors; ``strict`` False leaves a column of an unsupported type alone."""
    from .enums import DType
    from .series import Series

    dtype = s.dtype
    if dtype == DType.STRING:
        is_str = True
    elif dtype.is_numeric:
        is_str = False
    elif strict:
        raise TypeError(f"replace: a {dtype.name.lower()} column is not supported")
    else:
        return s
    floats = dtype in (DType.FLOAT32, DType.FLOAT64)
    steps: list[tuple[Series, Series | int | float | None]] = []
    for old, new in pairs:
        old, new = _plain(old), _plain(new)
        old_kind, new_kind = _kind(old), _kind(new)
        hit: Series[bool] | None
        if old_kind == "null":
            hit = s.isna()
        elif old_kind == "nan":
            hit = (s != s) if floats else None
        elif is_str == (old_kind == "str"):
            hit = s.str_eq(cast(str, old)) if is_str else s.eq(cast(_Scalar, old))
        else:
            hit = None  # an old value the column type cannot hold matches nothing
        if hit is None:
            continue
        if new_kind == "null":
            other: Series | int | float | None = None
        elif (new_kind == "str") != is_str:
            where = f" (column {column!r})" if column is not None else ""
            what = "a string" if new_kind == "str" else "a number"
            raise TypeError(f"replace: cannot put {what} into a {dtype.name.lower()} column{where}")
        else:
            other = Series.from_list([new] * len(s)) if is_str else cast(_Scalar, new)
        steps.append((hit, other))
    out: Series = s
    for step_hit, step_other in steps:
        out = out.mask(step_hit, step_other)
    return out


class SeriesPandasMixin:
    """``self`` is a :class:`Series`."""

    __slots__ = ()

    def _s(self) -> Series:
        from .series import Series

        assert isinstance(self, Series)
        return self

    # -- spellings -----------------------------------------------------------------
    def between(self, left: _Scalar, right: _Scalar, inclusive: str = "both") -> Series:
        """``left <= x <= right`` as a Bool mask; ``inclusive`` is ``both``
        (default), ``neither``, ``left`` or ``right``."""
        s = self._s()
        if inclusive == "both":
            return s.is_between(left, right)
        lo = s.ge(left) if inclusive == "left" else s.gt(left)
        hi = s.le(right) if inclusive == "right" else s.lt(right)
        if inclusive not in ("left", "right", "neither"):
            raise ValueError("between: inclusive must be both, neither, left or right")
        return lo.logical(0, hi)

    def drop_duplicates(self, keep: Literal["first", "last"] | bool = "first") -> Series:
        """The pandas ``drop_duplicates``: the values that are not a repeat,
        in order, one null kept. ``keep`` is ``"first"`` (default), ``"last"``
        or ``False`` (drop every value that repeats). See :meth:`unique`."""
        s = self._s()
        return s.filter(~s.duplicated(keep))

    @property
    def is_monotonic_increasing(self) -> bool:
        return self._s().is_sorted()

    @property
    def is_monotonic_decreasing(self) -> bool:
        return self._s().is_sorted(descending=True)

    @property
    def hasnans(self) -> bool:
        return self._s().null_count > 0

    @property
    def empty(self) -> bool:
        return len(self._s()) == 0

    @property
    def ndim(self) -> int:
        return 1

    @property
    def values(self) -> np.ndarray:
        return self._s().to_numpy()

    @property
    def array(self) -> pa.Array:
        return self._s().to_arrow()

    def items(self) -> Iterator[tuple[int, object]]:
        return enumerate(self._s().to_list())

    def keys(self) -> list[int]:
        return list(range(len(self._s())))

    def get(self, key: int, default: object = None) -> object:
        s = self._s()
        if -len(s) <= key < len(s):
            return s[key]
        return default

    def copy(self, deep: bool = True) -> Series:
        """A new handle on the same immutable buffers (:meth:`share`)."""
        return self._s().share()

    def convert_dtypes(
        self,
        infer_objects: bool = True,
        convert_string: bool = True,
        convert_integer: bool = True,
        convert_boolean: bool = True,
        convert_floating: bool = True,
    ) -> Series:
        """The pandas rules that map to engine types. A column is typed and
        nullable already, so only ``convert_integer`` changes it: a Float64
        column whose valid values are all whole numbers (nulls allowed)
        becomes Int64. A float ``NaN`` is a value here and not a null, so a
        column holding one stays Float64, where pandas reads it as missing and
        converts. The other keywords change nothing."""
        s = self._s()
        if not convert_integer or s.dtype not in (DType.FLOAT32, DType.FLOAT64):
            return self.copy()
        if not (s.floor() == s).fill_null(True).all():
            return self.copy()
        largest = s.abs().max()
        if largest is not None and cast(float, largest) >= 2.0**63:
            return self.copy()
        return s.astype("int64")

    def infer_objects(self) -> Series:
        """:meth:`copy`: an engine column is already typed."""
        return self.copy()

    def squeeze(self) -> Scalar | Series:
        """The one element of a one-element Series, else the Series itself."""
        s = self._s()
        return cast("Scalar | Series", s[0] if len(s) == 1 else s)

    @property
    def T(self) -> Series:
        """The Series itself (it has one dimension)."""
        return self._s()

    def transpose(self) -> Series:
        """The Series itself (it has one dimension)."""
        return self._s()

    @property
    def dtypes(self) -> DType:
        """:attr:`dtype` (the pandas Series spelling)."""
        return self._s().dtype

    def pipe(
        self, func: Callable[Concatenate[Series, _P], _R], *args: _P.args, **kwargs: _P.kwargs
    ) -> _R:
        return func(self._s(), *args, **kwargs)

    def factorize(self) -> tuple[Series, Series]:
        """``(codes, uniques)``: ``uniques`` is :meth:`unique` (the distinct
        values, sorted, nulls dropped) and ``codes`` each value's position in
        it, -1 for a null (pandas numbers uniques by first occurrence)."""
        s = self._s()
        uniques = s.drop_nulls().unique()
        codes = cast("Series", uniques.search_sorted(s.fillna(0) if s.null_count else s))
        if s.null_count:
            codes = codes.mask(s.isna(), -1)
        return codes, uniques

    def repeat(self, repeats: int) -> Series:
        """Each element repeated ``repeats`` times, in place."""
        s = self._s()
        n = int(repeats)
        if n < 0:
            raise ValueError("repeat: repeats must be non-negative")
        if n == 0:
            return s.head(0)
        from .dataframe import DataFrame

        frame = DataFrame({"v": s}).with_row_index("_i")
        out = frame.concat(*[frame] * (n - 1)).sort_values("_i")["v"]
        return out.rename(s.name)

    def replace(self, to_replace: ReplaceKey, value: ReplaceValue | Omitted = OMITTED) -> Series:
        """Replace values, pandas' forms: ``replace(old, new)``,
        ``replace([old, ...], new)``, ``replace([old, ...], [new, ...])`` (paired
        by position) and ``replace({old: new, ...})``. Every match is found on the
        original values and applied together, so ``{1: 2, 2: 1}`` swaps. A new value
        of ``None`` or ``pd.NA`` makes the value null; a NaN makes it NaN. An old
        ``None`` or ``pd.NA`` matches the nulls, an old NaN the NaN values. The
        column keeps its type (a float into an integer column widens it, as
        :meth:`where` does); a string needs a string column and a number a numeric
        one, else ``TypeError``. An old value its column type cannot hold matches
        nothing. Bool, list and temporal columns raise ``TypeError``."""
        return replace_in(self._s(), replace_pairs(to_replace, value), None, True)

    def to_frame(self, name: str | None = None) -> DataFrame:
        """A one-column frame; the column is ``name``, else this Series' name,
        else ``"0"``."""
        from .dataframe import DataFrame

        if name is None:
            own = self._s().name
            name = own if own is not None else "0"
        return DataFrame({name: self._s()})

    def to_dict(self) -> dict[int, object]:
        return dict(enumerate(self._s().to_list()))

    def reset_index(self, drop: bool = False, name: str | None = None) -> Series | DataFrame:
        """A frame of ``index`` (positions) and the values under ``name``
        (default as :meth:`to_frame`); with ``drop`` the Series itself."""
        if drop:
            return self._s()
        return self.to_frame(name).with_row_index("index")

    def truncate(self, before: int | None = None, after: int | None = None) -> Series:
        """Elements at positions ``[before, after]``, both included."""
        s = self._s()
        lo = 0 if before is None else int(before)
        hi = len(s) - 1 if after is None else int(after)
        return s.slice(lo, max(hi - lo + 1, 0))

    def first_valid_index(self) -> int | None:
        return valid_edge(self._s().notna(), first=True)

    def last_valid_index(self) -> int | None:
        return valid_edge(self._s().notna(), first=False)

    def asof(self, where: int) -> object:
        """The last present value at or before position ``where`` (pandas
        ``Series.asof`` over the positional index): ``ffill`` then the cell."""
        s = self._s()
        if where < 0 or where >= len(s):
            raise IndexError(f"asof: position {where} out of range")
        return s.ffill()[where]

    def autocorr(self, lag: int = 1) -> float:
        """Pearson correlation with itself shifted by ``lag`` (pandas
        ``autocorr``); the shifted-in nulls drop from the pair."""
        s = self._s()
        shifted = s.shift(lag)
        keep = shifted.notna()
        return s.filter(keep).corr(shifted.filter(keep))

    def combine_first(self, other: Series) -> Series:
        """This Series with nulls filled from ``other`` (positional)."""
        s = self._s()
        return s.where(s.notna(), other)

    def update(self, other: Series) -> Series:
        """``other``'s present values over this Series (positional; pandas
        ``update`` mutates, this returns the new Series)."""
        s = self._s()
        return other.where(other.notna(), s)

    def drop(self, positions: int | Sequence[int]) -> Series:
        """Every element except those at ``positions`` (the positional index),
        pandas ``Series.drop``."""
        s = self._s()
        gone = {positions} if isinstance(positions, int) else set(positions)
        for p in gone:
            if p < 0 or p >= len(s):
                raise KeyError(p)
        from .dataframe import DataFrame

        index = DataFrame({"v": s}).with_row_index("_i")["_i"]
        return s.filter(~index.is_in(sorted(gone)))

    def equals(self, other: object) -> bool:
        """Same type, length and values, null for null (pandas ``equals``)."""
        from .series import Series

        s = self._s()
        if not isinstance(other, Series) or s.dtype != other.dtype or len(s) != len(other):
            return False
        if s.dtype not in _NATIVE_COMPARE:
            return s.to_list() == other.to_list()
        return not differs(s, other).any()

    def pop(self, item: object = None) -> object:
        raise TypeError("pop: a Series is immutable; use drop(position) for the rest")

    def sem(self, ddof: int = 1) -> float:
        """Standard error of the mean: ``std(ddof) / sqrt(count)``."""
        s = self._s()
        n = s.count()
        return float("nan") if n == 0 else cast(float, s.std(ddof)) / math.sqrt(n)

    def xs(self, key: int) -> object:
        """The element at position ``key`` (a Series has one axis)."""
        return self._s()[key]

    def sort_index(self, ascending: bool = True) -> Series:
        """The positional index is already sorted: this Series, or reversed
        for ``ascending=False``."""
        s = self._s()
        return s if ascending else s.reverse()

    def memory_usage(self, index: bool = False, deep: bool = False) -> int:
        """The bytes this column's Arrow buffers hold (``nbytes``)."""
        return int(self._s().nbytes)

    # -- reductions and stats with a frame shape -------------------------------------
    def corr(self, other: Series) -> float:
        """Pearson correlation with ``other`` (positional; the group-by
        ``corr`` aggregate over one group). NaN below two complete pairs and
        when a column has no spread, as in pandas."""
        return self._pair(other, "corr")

    def cov(self, other: Series) -> float:
        return self._pair(other, "covar_samp")

    def _pair(self, other: Series, agg: str) -> float:
        from .dataframe import DataFrame
        from .expr import Agg, Col
        from .groupby import GroupBy

        s = self._s()
        # pandas gives NaN below two complete pairs; the engine's readout 0.
        pairs = s.notna().logical(0, other.notna())
        if int(pairs.astype("int64").sum()) < 2:
            return float("nan")
        frame = DataFrame({"x": s, "y": other})
        if agg == "corr":
            # pandas gives NaN when a column has no spread over the complete
            # pairs; the aggregate's readout is 0.
            if s.filter(pairs).var() == 0 or other.filter(pairs).var() == 0:
                return float("nan")
        out = GroupBy(frame, []).agg(Agg(agg, Col("y"), "r", by=Col("x")))
        value = out["r"][0]
        assert isinstance(value, float)
        return value

    def describe(self) -> DataFrame:
        """count / mean / std / min / 25% / 50% / 75% / max as a two-column
        frame (``statistic``, ``value``); the quantiles are the engine's
        sketch."""
        from .dataframe import DataFrame

        s = self._s()
        rows = [
            ("count", s.count()),
            ("mean", s.mean()),
            ("std", s.stddev()),
            ("min", s.min()),
            ("25%", s.quantile(0.25)),
            ("50%", s.quantile(0.5)),
            ("75%", s.quantile(0.75)),
            ("max", s.max()),
        ]
        return DataFrame(
            {"statistic": [r[0] for r in rows], "value": [float(cast(float, r[1])) for r in rows]}
        )

    def agg(self, func: str | Sequence[str]) -> Scalar | DataFrame:
        """A reduction by name (``"sum"``), or a list of names as a
        two-column frame."""
        s = self._s()
        if isinstance(func, str):
            return cast("Scalar", getattr(s, func)())
        from .dataframe import DataFrame

        names = list(func)
        return DataFrame({"statistic": names, "value": [getattr(s, n)() for n in names]})

    def transform(self, func: str | Callable[[Series], Series]) -> Series:
        s = self._s()
        return getattr(s, func)() if isinstance(func, str) else func(s)

    def radd(self, other: _Scalar) -> Series:
        return self._s() + other

    def rsub(self, other: _Scalar) -> Series:
        return other - self._s()

    def rmul(self, other: _Scalar) -> Series:
        return self._s() * other

    def rdiv(self, other: _Scalar) -> Series:
        """``other / self``: a Series operand divides directly, a scalar is
        broadcast to a Series first."""
        s = self._s()
        if isinstance(other, type(s)):
            return other / s
        return s.full_like(other) / s

    def __rtruediv__(self, other: _Scalar) -> Series:
        return self.rdiv(other)

    def case_when(self, caselist: Sequence[tuple[Series, Series | _Scalar]]) -> Series:
        """Replace where each ``(condition, replacement)`` holds, first match
        wins (pandas ``case_when``)."""
        out = self._s()
        for cond, value in reversed(list(caselist)):
            out = out.mask(cond, value)
        return out

    def groupby(self, by: Series) -> GroupBy:
        """Group this Series' values by ``by`` (positional): a frame
        ``GroupBy`` over columns ``key`` and ``value``."""
        from .dataframe import DataFrame

        frame = DataFrame({"key": by, "value": self._s()})
        return frame.group_by("key")

    def explode(self) -> Series:
        """A List Series flattened to one row per element."""
        from .dataframe import DataFrame

        return DataFrame({"v": self._s()}).explode("v")["v"]

    # -- accessors: a Series is positional, so loc is iloc ----------------------------
    @property
    def iloc(self) -> _SeriesILoc:
        return _SeriesILoc(self._s())

    def to_csv(self, path: str | None = None, **kwargs: object) -> str | None:
        to_csv = cast("Callable[..., str | None]", self._s().to_pandas().to_csv)
        return to_csv(path, index=False, **kwargs)

    def to_json(self, path: str | None = None, **kwargs: object) -> str | None:
        to_json = cast("Callable[..., str | None]", self._s().to_pandas().to_json)
        return to_json(path, **kwargs)

    def to_string(self, **kwargs: object) -> str:
        to_string = cast("Callable[..., str]", self._s().to_pandas().to_string)
        return to_string(index=False, **kwargs)


class _SeriesILoc:
    """``s.iloc[i]`` / ``s.iloc[a:b]`` / ``s.iloc[[i, j]]`` / ``s.iloc[mask]``;
    a Series has no labels, so ``loc`` is the same accessor."""

    __slots__ = ("_s",)

    def __init__(self, s: Series) -> None:
        self._s = s

    def __getitem__(self, key: object) -> object:
        from ._loc import rows_by_position
        from .series import Series

        s = self._s
        if isinstance(key, bool):
            raise TypeError("iloc: a bool is not a position")
        if isinstance(key, int):
            if not -len(s) <= key < len(s):
                raise IndexError(f"iloc: position {key} is out of bounds for {len(s)} elements")
            return s[key]
        rows, _ = rows_by_position(len(s), key)
        if rows.kind == "all":
            return s
        if rows.kind == "slice":
            return s.slice(rows.offset, rows.length)
        if rows.kind == "take":
            return s.take(rows.positions)
        mask = rows.mask
        assert isinstance(mask, Series)
        return s.filter(mask)

    def __setitem__(self, key: object, value: object) -> None:
        """A Series is immutable; assign through a frame column
        (``df.loc[rows, "c"] = value``)."""
        raise TypeError("a Series is immutable; assign through df.loc[rows, column] = value")


__all__ = ["SeriesPandasMixin", "_SeriesILoc"]
