"""The pandas ``Series`` surface that is a composition of engine ops, mixed
into :class:`Series`: spellings (``between``, ``drop_duplicates``,
``factorize``), the frame-shaped results (``to_frame``, ``describe``,
``reset_index``), positional accessors and the conveniences."""

from __future__ import annotations

import math
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    Iterator,
    List,
    Optional,
    Sequence,
    Tuple,
    Union,
)

if TYPE_CHECKING:
    from .dataframe import DataFrame
    from .series import Series

_Scalar = Union[int, float]


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
        rows = DataFrame({"v": s.to_arrow()}).with_row_index("r")
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
        return int(self._s().to_arrow().nbytes)

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

    def replace(self, to_replace: object, value: object) -> "Series":
        """``to_replace`` -> ``value`` where the value equals ``to_replace``."""
        from .enums import DType
        from .series import Series

        s = self._s()
        if s.dtype == DType.STRING:
            if not isinstance(to_replace, str) or not isinstance(value, str):
                raise TypeError("replace on a String Series takes two strings")
            return s.mask(s.str_eq(to_replace), Series.from_list([value] * len(s)))
        if not isinstance(to_replace, (int, float)) or not isinstance(value, (int, float)):
            raise TypeError("replace on a numeric Series takes two numbers")
        return s.mask(s.eq(to_replace), value)

    def to_frame(self, name: str = "0") -> "DataFrame":
        from .dataframe import DataFrame

        return DataFrame({name: self._s().to_arrow()})

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
        ``corr`` aggregate over one group)."""
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
        frame = DataFrame({"x": s.to_arrow(), "y": other.to_arrow()})
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

        frame = DataFrame({"key": by.to_arrow(), "value": self._s().to_arrow()})
        return frame.group_by("key")

    def explode(self) -> "Series":
        """A List Series flattened to one row per element."""
        from .dataframe import DataFrame
        from .series import Series

        return Series(DataFrame({"v": self._s().to_arrow()}).explode("v")._native["v"])

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
