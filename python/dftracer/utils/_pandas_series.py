"""The pandas ``Series`` surface that is a composition of engine ops, mixed
into :class:`Series`: spellings (``between``, ``drop_duplicates``,
``factorize``), the frame-shaped results (``to_frame``, ``describe``,
``reset_index``), positional accessors and the conveniences."""

from __future__ import annotations

import math
import numbers
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    Iterator,
    List,
    Mapping,
    Optional,
    Sequence,
    Tuple,
    Union,
    cast,
)

if TYPE_CHECKING:
    from numpy import generic
    from pandas._libs.missing import NAType

    from .dataframe import DataFrame
    from .series import Series

    # A value of ``replace`` as the caller writes it, and as the engine takes it.
    PlainScalar = Union[int, float, str, None, NAType]
    ReplaceScalar = Union[PlainScalar, generic]
    ReplaceValue = Union[ReplaceScalar, Sequence[ReplaceScalar]]
    ReplaceKey = Union[ReplaceValue, Mapping[ReplaceScalar, ReplaceScalar]]

_Scalar = Union[int, float]


class _Omitted:
    """The type of ``_OMITTED``: ``replace``'s ``value`` when the caller gave none."""


_OMITTED = _Omitted()


def _plain(v: "ReplaceScalar") -> "PlainScalar":
    """A NumPy scalar as the Python number the engine accepts."""
    if type(v).__module__ == "numpy" and getattr(v, "ndim", 1) == 0:
        return cast("PlainScalar", v.item())  # ty: ignore[unresolved-attribute]
    return cast("PlainScalar", v)


def _kind(v: "PlainScalar") -> str:
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


def _replace_pairs(
    to_replace: "ReplaceKey", value: "Union[ReplaceValue, _Omitted]"
) -> "List[Tuple[ReplaceScalar, ReplaceScalar]]":
    """The (old, new) pairs of any replace form."""
    if isinstance(to_replace, dict):
        if value is not _OMITTED:
            raise TypeError("replace: value must be omitted when to_replace is a dict")
        return list(to_replace.items())
    if isinstance(value, _Omitted):
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


def _replace_in(
    s: "Series",
    pairs: "List[Tuple[ReplaceScalar, ReplaceScalar]]",
    column: Optional[str],
    strict: bool,
) -> "Series":
    """``s`` with every pair applied together. ``column`` names a frame column in
    errors; ``strict`` False leaves a column of an unsupported type alone."""
    from ._pandas_frame import _STRING, _is_numeric
    from .enums import DType
    from .series import Series

    dtype = s.dtype
    if dtype in _STRING:
        is_str = True
    elif dtype != DType.BOOL and _is_numeric(s):
        is_str = False
    elif strict:
        raise TypeError(f"replace: a {dtype.name.lower()} column is not supported")
    else:
        return s
    floats = dtype in (DType.FLOAT32, DType.FLOAT64)
    steps: "List[Tuple[Series, Union[Series, int, float, None]]]" = []
    for old, new in pairs:
        old, new = _plain(old), _plain(new)
        old_kind, new_kind = _kind(old), _kind(new)
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
            other: Union[Series, int, float, None] = None
        elif (new_kind == "str") != is_str:
            where = f" (column {column!r})" if column is not None else ""
            what = "a string" if new_kind == "str" else "a number"
            raise TypeError(f"replace: cannot put {what} into a {dtype.name.lower()} column{where}")
        else:
            other = Series.from_list([new] * len(s)) if is_str else cast(_Scalar, new)
        steps.append((hit, other))
    out = s
    for hit, other in steps:
        out = out.mask(hit, other)
    return out


class _SeriesPandasMixin:
    """``self`` is a :class:`Series`."""

    __slots__ = ()

    _native: Any

    def _s(self) -> "Series":
        from .series import Series

        assert isinstance(self, Series)
        return self

    # -- spellings -----------------------------------------------------------------
    def between(self, left: _Scalar, right: _Scalar, inclusive: str = "both") -> "Series":
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

    def drop_duplicates(self) -> "Series":
        """Distinct values, first occurrence order (:meth:`unique`)."""
        return self._s().unique()

    def duplicated(self, keep: Union[str, bool] = "first") -> "Series":
        """Bool mask of the repeated occurrences: with ``keep="first"`` the
        first occurrence is not marked, with ``"last"`` the last is not, with
        ``False`` every occurrence is (:meth:`is_duplicated`). The kept
        occurrence is found by a group-by of the row positions."""
        from .columnar import GroupBy
        from .dataframe import DataFrame
        from .series import Series

        s = self._s()
        every = s.is_duplicated()
        if keep is False:
            return every
        if keep not in ("first", "last"):
            raise ValueError("duplicated: keep must be 'first', 'last' or False")
        rows = DataFrame({"v": s}).with_row_index("r")
        agg = f"{'min' if keep == 'first' else 'max'}:r:r"
        kept = Series(GroupBy(rows, ["v"]).agg(agg)._native["r"])
        first = Series(rows._native["r"]).is_in(kept)
        return every.logical(0, first.logical_not())

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
    def nbytes(self) -> int:
        return int(self._s()._native.nbytes)

    @property
    def values(self) -> Any:
        return self._s().to_numpy()

    @property
    def array(self) -> Any:
        return self._s().to_arrow()

    @property
    def name(self) -> Optional[str]:
        """A Series carries no name; it takes one from the frame column it
        is set into."""
        return None

    def item(self) -> object:
        s = self._s()
        if len(s) != 1:
            raise ValueError(f"item: the Series has {len(s)} elements, not one")
        return s[0]

    def items(self) -> Iterator[Tuple[int, object]]:
        return enumerate(self._s().to_list())

    def keys(self) -> List[int]:
        return list(range(len(self._s())))

    def get(self, key: int, default: object = None) -> object:
        s = self._s()
        if -len(s) <= key < len(s):
            return s[key]
        return default

    def copy(self, deep: bool = True) -> "Series":
        """A new handle on the same immutable buffers (:meth:`share`)."""
        return self._s().share()

    def pipe(self, func: Callable[..., Any], *args: object, **kwargs: Any) -> Any:
        return func(self, *args, **kwargs)

    def factorize(self) -> "Tuple[Series, Series]":
        """``(codes, uniques)``: ``uniques`` is :meth:`unique` (the distinct
        values, sorted, nulls dropped) and ``codes`` each value's position in
        it, -1 for a null (pandas numbers uniques by first occurrence)."""
        s = self._s()
        uniques = s.unique()
        codes = uniques.search_sorted(s.fillna(0) if s.null_count else s)
        if s.null_count:
            codes = codes.mask(s.isna(), -1)
        return codes, uniques

    def repeat(self, repeats: int) -> "Series":
        """Each element repeated ``repeats`` times, in place."""
        s = self._s()
        n = int(repeats)
        if n < 0:
            raise ValueError("repeat: repeats must be non-negative")
        return s.take([i for i in range(len(s)) for _ in range(n)])

    def replace(
        self, to_replace: "ReplaceKey", value: "Union[ReplaceValue, _Omitted]" = _OMITTED
    ) -> "Series":
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
        return _replace_in(self._s(), _replace_pairs(to_replace, value), None, True)

    def to_frame(self, name: str = "0") -> "DataFrame":
        from .dataframe import DataFrame

        return DataFrame({name: self._s()})

    def to_dict(self) -> Dict[int, object]:
        return dict(enumerate(self._s().to_list()))

    def reset_index(self, drop: bool = False, name: str = "0") -> Union["Series", "DataFrame"]:
        """A frame of ``index`` (positions) and the values under ``name``;
        with ``drop`` the Series itself."""
        if drop:
            return self._s()
        return self.to_frame(name).with_row_index("index")

    def truncate(self, before: Optional[int] = None, after: Optional[int] = None) -> "Series":
        """Elements at positions ``[before, after]``, both included."""
        s = self._s()
        lo = 0 if before is None else int(before)
        hi = len(s) - 1 if after is None else int(after)
        return s.slice(lo, max(hi - lo + 1, 0))

    def first_valid_index(self) -> Optional[int]:
        hits = [i for i, v in enumerate(self._s().notna().to_list()) if v]
        return hits[0] if hits else None

    def last_valid_index(self) -> Optional[int]:
        hits = [i for i, v in enumerate(self._s().notna().to_list()) if v]
        return hits[-1] if hits else None

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

    def combine_first(self, other: "Series") -> "Series":
        """This Series with nulls filled from ``other`` (positional)."""
        s = self._s()
        return s.where(s.notna(), other)

    def update(self, other: "Series") -> "Series":
        """``other``'s present values over this Series (positional; pandas
        ``update`` mutates, this returns the new Series)."""
        s = self._s()
        return other.where(other.notna(), s)

    def drop(self, positions: Union[int, Sequence[int]]) -> "Series":
        """Every element except those at ``positions`` (the positional index),
        pandas ``Series.drop``."""
        s = self._s()
        gone = {positions} if isinstance(positions, int) else set(positions)
        for p in gone:
            if p < 0 or p >= len(s):
                raise KeyError(p)
        return s.take([i for i in range(len(s)) if i not in gone])

    def equals(self, other: object) -> bool:
        """Same type, length and values, null for null (pandas ``equals``)."""
        from .series import Series

        s = self._s()
        if not isinstance(other, Series) or s.dtype != other.dtype or len(s) != len(other):
            return False
        return s.to_list() == other.to_list()

    def pop(self, item: object = None) -> object:
        raise TypeError("pop: a Series is immutable; use drop(position) for the rest")

    def sem(self, ddof: int = 1) -> float:
        """Standard error of the mean: ``std(ddof) / sqrt(count)``."""
        s = self._s()
        n = s.count()
        return float("nan") if n == 0 else s.std(ddof) / math.sqrt(n)

    def xs(self, key: int) -> object:
        """The element at position ``key`` (a Series has one axis)."""
        return self._s()[key]

    def sort_index(self, ascending: bool = True) -> "Series":
        """The positional index is already sorted: this Series, or reversed
        for ``ascending=False``."""
        s = self._s()
        return s if ascending else s.reverse()

    def memory_usage(self, index: bool = False, deep: bool = False) -> int:
        """The bytes this column's Arrow buffers hold (``nbytes``)."""
        return self.nbytes

    # -- reductions and stats with a frame shape -------------------------------------
    def corr(self, other: "Series") -> float:
        """Pearson correlation with ``other`` (positional; the group-by
        ``corr`` aggregate over one group). NaN below two complete pairs and
        when a column has no spread, as in pandas."""
        return self._pair(other, "corr")

    def cov(self, other: "Series") -> float:
        return self._pair(other, "covar_samp")

    def _pair(self, other: "Series", agg: str) -> float:
        from .columnar import Agg, GroupBy, _Col
        from .dataframe import DataFrame
        from .series import Series

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
        out = GroupBy(frame, []).agg(Agg(agg, _Col("y"), "r", by=_Col("x")))
        value = Series(out._native["r"])[0]
        assert isinstance(value, float)
        return value

    def describe(self) -> "DataFrame":
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
        return DataFrame({"statistic": [r[0] for r in rows], "value": [float(r[1]) for r in rows]})

    def agg(self, func: Union[str, Sequence[str]]) -> Union[object, "DataFrame"]:
        """A reduction by name (``"sum"``), or a list of names as a
        two-column frame."""
        s = self._s()
        if isinstance(func, str):
            return getattr(s, func)()
        from .dataframe import DataFrame

        names = list(func)
        return DataFrame({"statistic": names, "value": [getattr(s, n)() for n in names]})

    aggregate = agg

    def transform(self, func: Union[str, Callable[["Series"], "Series"]]) -> "Series":
        s = self._s()
        return getattr(s, func)() if isinstance(func, str) else func(s)

    def divide(self, other: Union["Series", _Scalar]) -> "Series":
        return self._s() / other  # type: ignore[operator]

    def multiply(self, other: Union["Series", _Scalar]) -> "Series":
        return self._s() * other  # type: ignore[operator]

    def subtract(self, other: Union["Series", _Scalar]) -> "Series":
        return self._s() - other  # type: ignore[operator]

    def truediv(self, other: Union["Series", _Scalar]) -> "Series":
        return self._s() / other  # type: ignore[operator]

    def radd(self, other: _Scalar) -> "Series":
        return self._s() + other  # type: ignore[operator]

    def rsub(self, other: _Scalar) -> "Series":
        return other - self._s()  # type: ignore[operator]

    def rmul(self, other: _Scalar) -> "Series":
        return self._s() * other  # type: ignore[operator]

    def rdiv(self, other: _Scalar) -> "Series":
        """``other / self``: the scalar broadcast to a Series, then the div
        kernel."""
        s = self._s()
        return s.full_like(other) / s  # type: ignore[operator]

    def __rtruediv__(self, other: _Scalar) -> "Series":
        return self.rdiv(other)

    rtruediv = rdiv

    def case_when(self, caselist: Sequence[Tuple["Series", Union["Series", _Scalar]]]) -> "Series":
        """Replace where each ``(condition, replacement)`` holds, first match
        wins (pandas ``case_when``)."""
        out = self._s()
        for cond, value in reversed(list(caselist)):
            out = out.mask(cond, value)  # type: ignore[arg-type]
        return out

    def groupby(self, by: "Series") -> Any:
        """Group this Series' values by ``by`` (positional): a frame
        ``GroupBy`` over columns ``key`` and ``value``."""
        from .dataframe import DataFrame

        frame = DataFrame({"key": by, "value": self._s()})
        return frame.group_by("key")

    def explode(self) -> "Series":
        """A List Series flattened to one row per element."""
        from .dataframe import DataFrame
        from .series import Series

        return Series(DataFrame({"v": self._s()}).explode("v")._native["v"])

    # -- accessors: a Series is positional, so loc is iloc ----------------------------
    @property
    def iloc(self) -> "_SeriesILoc":
        return _SeriesILoc(self._s())

    loc = iloc

    @property
    def iat(self) -> "_SeriesILoc":
        return _SeriesILoc(self._s())

    at = iat

    def to_csv(self, path: Optional[str] = None, **kwargs: Any) -> Optional[str]:
        return self._s().to_pandas().to_csv(path, index=False, **kwargs)

    def to_json(self, path: Optional[str] = None, **kwargs: Any) -> Optional[str]:
        return self._s().to_pandas().to_json(path, **kwargs)

    def to_string(self, **kwargs: Any) -> str:
        return self._s().to_pandas().to_string(index=False, **kwargs)


class _SeriesILoc:
    """``s.iloc[i]`` / ``s.iloc[a:b]`` / ``s.iloc[[i, j]]`` / ``s.iloc[mask]``;
    a Series has no labels, so ``loc`` is the same accessor."""

    __slots__ = ("_s",)

    def __init__(self, s: "Series") -> None:
        self._s = s

    def __getitem__(self, key: object) -> object:
        from .indexing import rows_by_position
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


__all__ = ["_SeriesPandasMixin", "_SeriesILoc"]
