# SPDX-License-Identifier: MIT
"""The pandas ``DataFrame`` surface that is a composition of engine ops:
column-wise arithmetic, comparisons and transforms (one Series kernel per
column), the one-row reductions, and the small conveniences (``pipe``,
``items``, ``T``, ``to_csv`` through pandas). Mixed into :class:`DataFrame`;
each method names the engine op it runs in the parity table
(``scripts/check_op_parity_doxygen.py``)."""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    Callable,
    Iterator,
    List,
    Literal,
    Mapping,
    Sequence,
    TypeVar,
    Union,
    cast,
)

from . import dftracer_utils_ext as _ext
from ._convert import _like
from ._pandas_series import OMITTED, replace_in, replace_pairs, valid_edge
from .enums import DType, rule_to_units
from .series import Series

if TYPE_CHECKING:
    import numpy as np
    from typing_extensions import Concatenate, ParamSpec, Self

    from ._apply import Row
    from ._pandas_series import Omitted, ReplaceKey, ReplaceValue
    from .dataframe import DataFrame
    from .dftracer_utils_ext import Scalar
    from .groupby import GroupBy

_R = TypeVar("_R")
if TYPE_CHECKING:
    _P = ParamSpec("_P")

_STRING = {DType.STRING}

Operand = Union[int, float, Series, "DataFrame"]


def is_numeric_series(s: Series) -> bool:
    return s.dtype.is_numeric or s.dtype == DType.BOOL


def _group(frame: DataFrame, keys: Sequence[str]) -> GroupBy:
    from .groupby import GroupBy

    return GroupBy(frame, list(keys))


def column_series(frame: DataFrame, name: str) -> Series:
    return _ext.DataFrame.__getitem__(frame, name)


class FramePandasMixin(_ext.DataFrame):
    """pandas spellings over the engine's Series kernels, one column at a
    time. ``self`` is a :class:`DataFrame`."""

    __slots__ = ()

    columns: List[str]

    def _frame(self) -> DataFrame:
        from .dataframe import DataFrame

        assert isinstance(self, DataFrame)
        return self

    def _col(self, name: str) -> Series:
        return _ext.DataFrame.__getitem__(self, name)

    def _rebuild(self, columns: dict[str, Series], keep_others: bool = True) -> DataFrame:
        """A frame with ``columns`` replacing (or, with ``keep_others`` off,
        being) this frame's columns, in this frame's order."""

        out: DataFrame = self._frame()
        names = self.columns if keep_others else [c for c in self.columns if c in columns]
        if not keep_others:
            out = _ext.DataFrame.select(out, *names, *_ext.DataFrame.index_names(self))
        for name in names:
            if name in columns:
                out = _ext.DataFrame.with_column(out, name, columns[name])
        return _ext.DataFrame.share(out) if out is self else out

    def _map_columns(
        self, fn: Callable[[Series], Series], numeric_only: bool = True, keep_others: bool = True
    ) -> DataFrame:
        """``fn`` over each (numeric) column; other columns kept or dropped."""
        out: dict[str, Series] = {}
        for name in self.columns:
            s = self._col(name)
            if numeric_only and not is_numeric_series(s):
                continue
            out[name] = fn(s)
        return self._rebuild(out, keep_others)

    def _operand(self, other: Operand, name: str) -> int | float | Series:
        """The right-hand side for column ``name``: a scalar, a row-aligned
        Series (positional), or the same-named column of a frame."""
        from .dataframe import DataFrame

        if isinstance(other, DataFrame):
            if name not in other:
                raise KeyError(f"no column named {name!r} in the other frame")
            return column_series(other, name)
        if isinstance(other, Series):
            if len(other) != len(self._frame()):
                raise ValueError(
                    f"the Series has {len(other)} rows; the frame has {len(self._frame())}"
                )
            return other
        if isinstance(other, bool) or not isinstance(other, (int, float)):
            raise TypeError(
                f"expected a number, a Series or a DataFrame, not {type(other).__name__}"
            )
        return other

    def _binary(self, other: Operand, op: str, reverse: bool = False) -> DataFrame:
        from .dataframe import DataFrame

        names = [c for c in self.columns if is_numeric_series(self._col(c))]
        if isinstance(other, DataFrame):
            names = [c for c in names if c in other]
        out: dict[str, Series] = {}
        for name in names:
            s = self._col(name)
            rhs = self._operand(other, name)
            if reverse:
                if isinstance(rhs, Series):
                    out[name] = getattr(rhs, op)(s)
                else:
                    out[name] = getattr(s.full_like(rhs), op)(s)
            else:
                out[name] = getattr(s, op)(rhs)
        return self._rebuild(out, keep_others=False)

    # -- arithmetic (positional; join first for label alignment) ---------------
    def add(self, other: Operand) -> DataFrame:
        """Column-wise ``+`` over the numeric columns: a scalar, a row-aligned
        Series, or the same-named columns of another frame. Rows match by
        position (join first for label alignment)."""
        return self._binary(other, "__add__")

    def sub(self, other: Operand) -> DataFrame:
        return self._binary(other, "__sub__")

    def mul(self, other: Operand) -> DataFrame:
        return self._binary(other, "__mul__")

    def div(self, other: Operand) -> DataFrame:
        return self._binary(other, "__truediv__")

    def radd(self, other: Operand) -> DataFrame:
        return self._binary(other, "__add__", reverse=True)

    def rsub(self, other: Operand) -> DataFrame:
        return self._binary(other, "__sub__", reverse=True)

    def rmul(self, other: Operand) -> DataFrame:
        return self._binary(other, "__mul__", reverse=True)

    def rdiv(self, other: Operand) -> DataFrame:
        return self._binary(other, "__truediv__", reverse=True)

    def floordiv(self, other: Operand) -> DataFrame:
        return self._binary(other, "__floordiv__")

    def mod(self, other: Operand) -> DataFrame:
        return self._binary(other, "__mod__")

    def pow(self, other: Operand) -> DataFrame:
        return self._binary(other, "__pow__")

    def rfloordiv(self, other: Operand) -> DataFrame:
        return self._binary(other, "__floordiv__", reverse=True)

    def rmod(self, other: Operand) -> DataFrame:
        return self._binary(other, "__mod__", reverse=True)

    def rpow(self, other: Operand) -> DataFrame:
        return self._binary(other, "__pow__", reverse=True)

    def __floordiv__(self, other: Operand) -> DataFrame:
        return self.floordiv(other)

    def __mod__(self, other: Operand) -> DataFrame:
        return self.mod(other)

    def __pow__(self, other: Operand) -> DataFrame:
        return self.pow(other)

    def __add__(self, other: Operand) -> DataFrame:
        return self.add(other)

    def __sub__(self, other: Operand) -> DataFrame:
        return self.sub(other)

    def __mul__(self, other: Operand) -> DataFrame:
        return self.mul(other)

    def __truediv__(self, other: Operand) -> DataFrame:
        return self.div(other)

    def __radd__(self, other: Operand) -> DataFrame:
        return self.radd(other)

    def __rsub__(self, other: Operand) -> DataFrame:
        return self.rsub(other)

    def __rmul__(self, other: Operand) -> DataFrame:
        return self.rmul(other)

    def __rtruediv__(self, other: Operand) -> DataFrame:
        return self.rdiv(other)

    def __neg__(self) -> DataFrame:
        return self._map_columns(lambda s: s.negate(), keep_others=False)

    # -- comparisons: a Bool frame ---------------------------------------------
    def _compare(self, other: Operand, op: str) -> DataFrame:
        from .dataframe import DataFrame

        out: dict[str, Series] = {}
        for name in self.columns:
            s = self._col(name)
            if isinstance(other, DataFrame) and name not in other:
                continue
            if isinstance(other, str):
                # A string label matches String columns only; a numeric
                # column is never equal to it.
                if s.dtype in _STRING:
                    mask = s.str_eq(other)
                    out[name] = mask if op == "eq" else mask.logical_not()
                elif op in ("eq", "ne"):
                    out[name] = Series.from_list([op == "ne"] * len(s))
                continue
            rhs = self._operand(other, name)
            if isinstance(rhs, Series):
                # Column against column: numeric only (a - b <op> 0).
                if not is_numeric_series(s) or not is_numeric_series(rhs):
                    continue
                out[name] = getattr(s - rhs, op)(0)
            elif s.dtype in _STRING:
                if op not in ("eq", "ne"):
                    raise TypeError(f"{op}: a String column compares with == / != only")
                mask = s.str_eq(str(rhs))
                out[name] = mask if op == "eq" else mask.logical_not()
            else:
                out[name] = getattr(s, op)(rhs)
        return self._rebuild(out, keep_others=False)

    def eq(self, other: Operand) -> DataFrame:
        """Column-wise ``==`` as a Bool frame (``ne``, ``lt``, ``le``, ``gt``,
        ``ge`` likewise)."""
        return self._compare(other, "eq")

    def ne(self, other: Operand) -> DataFrame:
        return self._compare(other, "ne")

    def lt(self, other: Operand) -> DataFrame:
        return self._compare(other, "lt")

    def le(self, other: Operand) -> DataFrame:
        return self._compare(other, "le")

    def gt(self, other: Operand) -> DataFrame:
        return self._compare(other, "gt")

    def ge(self, other: Operand) -> DataFrame:
        return self._compare(other, "ge")

    # -- column-wise transforms ---------------------------------------------------
    def abs(self) -> DataFrame:
        return self._map_columns(lambda s: s.abs())

    def round(self, decimals: int = 0) -> DataFrame:
        return self._map_columns(lambda s: s.round(decimals))

    def clip(self, lower: int | float | None = None, upper: int | float | None = None) -> DataFrame:
        return self._map_columns(lambda s: s.clip(lower, upper))

    def cumsum(self) -> DataFrame:
        return self._map_columns(lambda s: s.cumsum())

    def cummax(self) -> DataFrame:
        return self._map_columns(lambda s: s.cummax())

    def cummin(self) -> DataFrame:
        return self._map_columns(lambda s: s.cummin())

    def cumprod(self) -> DataFrame:
        return self._map_columns(lambda s: s.cumprod())

    def diff(self) -> DataFrame:
        return self._map_columns(lambda s: s.diff())

    def pct_change(self) -> DataFrame:
        return self._map_columns(lambda s: s.pct_change())

    def shift(self, n: int = 1, *, periods: int | None = None) -> DataFrame:
        """Every column shifted by ``n`` rows (negative looks ahead);
        ``periods`` is the pandas keyword."""
        rows = n if periods is None else periods
        return self._map_columns(lambda s: s.shift(rows), numeric_only=False)

    def rank(
        self,
        method: Literal["average", "min", "max", "dense", "ordinal", "first"] = "average",
        ascending: bool = True,
    ) -> DataFrame:
        return self._map_columns(lambda s: s.rank(method, ascending=ascending))

    def interpolate(self) -> DataFrame:
        return self._map_columns(lambda s: s.interpolate())

    def rolling(self, window: int) -> _FrameWindow:
        """A trailing window of ``window`` rows over every numeric column
        (pandas ``DataFrame.rolling``): ``.sum() .mean() .min() .max() .var()
        .std() .median() .quantile(q)``, each the Series rolling kernel per
        column."""
        return _FrameWindow(self, lambda s: s.rolling(window))

    def expanding(self) -> _FrameWindow:
        """The window from the first row to each row, over every numeric
        column (pandas ``DataFrame.expanding``): ``.sum() .mean() .min() .max()
        .count() .var() .std()``."""
        return _FrameWindow(self, lambda s: s.expanding())

    def ewm(
        self,
        alpha: float | None = None,
        *,
        span: float | None = None,
        com: float | None = None,
        halflife: float | None = None,
    ) -> _FrameWindow:
        """The exponentially weighted window over every numeric column
        (pandas ``DataFrame.ewm``): ``.mean() .std()``."""
        a = _ext._ewm_alpha(alpha, span=span, com=com, halflife=halflife)
        return _FrameWindow(self, lambda s: s.ewm(a))

    def ffill(self) -> DataFrame:
        """Nulls in every column filled from the nearest present value before
        them (``pad``); ``bfill`` / ``backfill`` from after."""
        return self._map_columns(lambda s: s.ffill(), numeric_only=False)

    def bfill(self) -> DataFrame:
        return self._map_columns(lambda s: s.bfill(), numeric_only=False)

    def isna(self) -> DataFrame:
        """A Bool frame: true where a value is null (``isnull`` / ``notna`` /
        ``notnull`` likewise)."""
        return self._map_columns(lambda s: s.isna(), numeric_only=False)

    def notna(self) -> DataFrame:
        return self._map_columns(lambda s: s.notna(), numeric_only=False)

    def where(self, cond: Series, other: int | float | None = None) -> DataFrame:
        """Each numeric column where the row mask ``cond`` holds, else ``other``
        (null by default, as in pandas; ``mask`` is the inverse)."""
        return self._map_columns(lambda s: s.where(cond, other))

    def mask(self, cond: Series, other: int | float | None = None) -> DataFrame:
        return self._map_columns(lambda s: s.mask(cond, other))

    def replace(self, to_replace: ReplaceKey, value: ReplaceValue | Omitted = OMITTED) -> DataFrame:
        """:meth:`Series.replace` in every column. A column that no old value can
        match, or of a bool, list or temporal type, is left as it is; a column whose
        old value matches but whose new value it cannot hold raises ``TypeError``
        naming the column."""
        pairs = replace_pairs(to_replace, value)
        out = {name: replace_in(self._col(name), pairs, name, False) for name in self.columns}
        return self._rebuild(out, True)

    def applymap(self, func: Callable[[Scalar], Scalar]) -> DataFrame:
        """``func`` over every element, column by column (:meth:`Series.apply`
        per column; the pandas ``map``)."""
        return self._map_columns(lambda s: s.apply(func), numeric_only=False)

    def transform(self, func: str | Callable[[Series], Series]) -> DataFrame:
        """``func`` (a Series -> Series function over every column, or a
        Series method name over the numeric ones), keeping the shape."""
        if isinstance(func, str):
            return self._map_columns(lambda s: getattr(s, func)())
        return self._map_columns(func, numeric_only=False)

    # -- reductions: one row, each numeric column under its own name -----------
    def _reduce_scalar(
        self, fn: Callable[[Series], object], numeric_only: bool = True
    ) -> DataFrame:
        from .dataframe import DataFrame

        out: dict[str, list[object]] = {}
        for name in self.columns:
            s = self._col(name)
            if numeric_only and not is_numeric_series(s):
                continue
            out[name] = [fn(s)]
        return DataFrame(out)

    def median(self) -> DataFrame:
        """One row of each numeric column's median (exact: the Series
        quantile sorts; ``quantile(q)`` likewise, linear interpolation unless
        ``interpolation`` says otherwise)."""
        return self._reduce_scalar(lambda s: s.median())

    def quantile(
        self,
        q: float = 0.5,
        interpolation: Literal["linear", "nearest", "lower", "higher", "midpoint"] = "linear",
    ) -> DataFrame:
        return self._reduce_scalar(lambda s: s.quantile(q, interpolation))

    def nunique(self) -> DataFrame:
        return self._reduce_scalar(lambda s: s.nunique(), numeric_only=False)

    def prod(self) -> DataFrame:
        return self._reduce_scalar(lambda s: s.product())

    def sem(self) -> DataFrame:
        """Standard error of the mean: ``std / sqrt(count)`` per column."""
        return self._reduce_scalar(lambda s: s.stddev() / (s.count() ** 0.5) if s.count() else None)

    def mode(self) -> DataFrame:
        """One row: each numeric column's most frequent value, the smallest
        on a tie (pandas lists the tied modes sorted)."""

        def one(s: Series) -> object:
            counts = s.value_counts()
            if len(counts) == 0:
                return None
            count = column_series(counts, "count")
            top = int(
                cast(int, count.max())
            )  # a count column: min()/max() can also be str for text
            return column_series(counts.filter(count.eq(top)), "value").min()

        return self._reduce_scalar(one)

    def idxmax(self) -> DataFrame:
        """One row: the position of each numeric column's maximum (``idxmin``
        likewise); the index label when an index is set."""
        return self._positions(lambda s: s.idxmax())

    def idxmin(self) -> DataFrame:
        return self._positions(lambda s: s.idxmin())

    def _positions(self, fn: Callable[[Series], int]) -> DataFrame:
        names = self.index_names()
        labels = names[0] if len(names) == 1 else None
        index = self._col(labels) if labels else None

        def one(s: Series) -> object:
            pos = fn(s)
            return index.item(pos) if index is not None else pos

        return self._reduce_scalar(one)

    def any(self) -> DataFrame:
        """One row: whether any value is true / non-zero per column (``all``
        likewise)."""
        return self._reduce_scalar(lambda s: bool(s.astype(DType.FLOAT64).ne(0).any()))

    def all(self) -> DataFrame:
        return self._reduce_scalar(lambda s: bool(s.astype(DType.FLOAT64).ne(0).all()))

    def corr(self) -> DataFrame:
        """The correlation matrix of the numeric columns (``cov`` the
        covariance matrix): one row per column, in column order."""
        return self._pairwise("corr")

    def cov(self) -> DataFrame:
        return self._pairwise("covar_samp")

    def _pairwise(self, agg: str) -> DataFrame:
        from .dataframe import DataFrame
        from .expr import Agg, Col

        names = [c for c in self.columns if is_numeric_series(self._col(c))]
        specs = [Agg(agg, Col(b), f"{a}__{b}", by=Col(a)) for a in names for b in names]
        if not specs:
            return DataFrame({"column": []})
        row = _group(self._frame(), []).agg(*specs).to_dict()
        table: dict[str, list[object]] = {"column": list(names)}
        for b in names:
            table[b] = [row[f"{a}__{b}"][0] for a in names]
        if agg == "corr":
            # The aggregate reads 0 for a column with no spread; pandas gives
            # NaN. An exact 0 is re-read through Series.corr, which tells them
            # apart over the complete pairs.
            for b in names:
                table[b] = [
                    self._col(a).corr(self._col(b)) if v == 0.0 else v
                    for a, v in zip(names, table[b])
                ]
        return DataFrame(table)

    def value_counts(
        self, subset: Sequence[str] | None = None, ascending: bool = False
    ) -> DataFrame:
        """Distinct rows of ``subset`` (every column by default) with their
        count, most frequent first, as a frame with a ``count`` column."""
        keys = list(subset) if subset else self.columns
        frame = self._frame()
        # pandas drops a row with a null key (dropna=True).
        present = None
        for k in keys:
            m = self._col(k).notna()
            present = m if present is None else present.logical(0, m)
        if present is not None:
            frame = frame.filter(present)
        counts = _group(frame, keys).size().rename({"size": "count"})
        return counts.sort_values("count", ascending=ascending)

    # -- shape, iteration, conveniences -------------------------------------------
    @property
    def empty(self) -> bool:
        return len(self._frame()) == 0 or not self.columns

    @property
    def ndim(self) -> int:
        return 2

    @property
    def size(self) -> int:
        return len(self._frame()) * len(self.columns)

    @property
    def axes(self) -> list[object]:
        return [self._frame().index, list(self.columns)]

    @property
    def values(self) -> np.ndarray:
        """The frame as a 2-D NumPy array (``to_numpy``)."""
        return self.to_numpy()

    def to_numpy(self) -> np.ndarray:
        import numpy as np

        return np.column_stack([self._col(c).to_numpy() for c in self.columns])

    @property
    def T(self) -> DataFrame:
        return self.transpose()

    def transpose(self) -> DataFrame:
        """Rows become columns named ``0``.. (``T``); values widen to one type
        as pandas does."""
        from .dataframe import DataFrame

        rows = self._frame().to_dict()
        names = self.columns
        n = len(self._frame())
        mixed = not all(is_numeric_series(self._col(c)) for c in names)

        def cell(v: object) -> object:
            return v if v is None or not mixed else str(v)

        return DataFrame(
            {"column": names, **{str(i): [cell(rows[c][i]) for c in names] for i in range(n)}}
        )

    def copy(self, deep: bool = True) -> Self:
        """A new handle on the same immutable columns (nothing is copied)."""
        return _ext.DataFrame.share(self)

    def convert_dtypes(
        self,
        infer_objects: bool = True,
        convert_string: bool = True,
        convert_integer: bool = True,
        convert_boolean: bool = True,
        convert_floating: bool = True,
    ) -> DataFrame:
        """:meth:`Series.convert_dtypes` on every column."""
        f = self._frame()
        out = f.copy()
        for name in f.columns:
            converted = column_series(f, name).convert_dtypes(
                infer_objects, convert_string, convert_integer, convert_boolean, convert_floating
            )
            out = out.with_column(name, converted)
        return out

    def infer_objects(self) -> DataFrame:
        """:meth:`copy`: an engine column is already typed."""
        return self._frame().copy()

    def clear(self, n: int = 0) -> DataFrame:
        """The frame with its columns and no rows (polars ``clear``; ``n > 0``,
        null rows, is not taken)."""
        if n != 0:
            raise ValueError("clear: only n=0 is taken")
        return self._frame().head(0)

    def iter_columns(self) -> Iterator[Series]:
        """Each column as a Series, in order (polars ``iter_columns``)."""
        for name in self._frame().columns:
            yield self._col(name)

    def pipe(
        self,
        func: Callable[Concatenate[DataFrame, _P], _R],
        *args: _P.args,
        **kwargs: _P.kwargs,
    ) -> _R:
        """``func(self, *args, **kwargs)``."""
        return func(self._frame(), *args, **kwargs)

    def items(self) -> Iterator[tuple[str, Series]]:
        for name in self.columns:
            yield name, self._col(name)

    def iterrows(self) -> Iterator[tuple[object, Row]]:
        """``(label, Row)`` per row; the label is the index value or the
        position."""
        from ._apply import Row

        cols = {c: self._col(c).to_list() for c in self.columns}
        labels: Sequence[object] = list(range(len(self._frame())))
        names = self.index_names()
        if len(names) == 1:
            labels = column_series(self._frame(), names[0]).to_list()
        for i in range(len(self._frame())):
            yield labels[i], Row({c: cols[c][i] for c in self.columns})

    def itertuples(
        self, index: bool = True, name: str | None = "Row"
    ) -> Iterator[tuple[Scalar, ...]]:
        from collections import namedtuple

        fields = (["Index"] if index else []) + list(self.columns)
        cols = {c: self._col(c).to_list() for c in self.columns}
        labels: Sequence[object] = list(range(len(self._frame())))
        names = self.index_names()
        if index and len(names) == 1:
            labels = self._col(names[0]).to_list()
        make_tuple: Callable[..., Callable[..., tuple[Scalar, ...]]] = namedtuple
        Tup = make_tuple(name or "Row", fields, rename=True)
        for i in range(len(self._frame())):
            row = ([labels[i]] if index else []) + [cols[c][i] for c in self.columns]
            yield Tup(*row)

    def get(self, key: str, default: object = None) -> object:
        return self._col(key) if key in self._frame() else default

    def pop(self, name: str) -> Series:
        """Remove ``name`` from this handle and return it."""
        out = self._col(name)
        _ext.DataFrame._replace(
            self, _ext.DataFrame.select(self, *[c for c in self._frame()._names() if c != name])
        )
        return out

    def insert(self, loc: int, column: str, value: Series | int | float | str) -> None:
        """Put ``column`` at position ``loc`` (this handle is rebound)."""
        if column in self._frame():
            raise ValueError(f"insert: column {column!r} already exists")
        from ._loc import Rows, assign_rows

        assign_rows(self._frame(), Rows.all(), [column], value)
        order = [c for c in self._frame()._names() if c != column]
        order.insert(loc, column)
        _ext.DataFrame._replace(self, _ext.DataFrame.select(self, *order))

    def add_prefix(self, prefix: str) -> DataFrame:
        return self._frame().rename({c: prefix + c for c in self.columns})

    def add_suffix(self, suffix: str) -> DataFrame:
        return self._frame().rename({c: c + suffix for c in self.columns})

    def set_axis(self, labels: Sequence[str]) -> DataFrame:
        """New column names, positionally."""
        names = list(labels)
        if len(names) != len(self.columns):
            raise ValueError(f"set_axis: {len(names)} names for {len(self.columns)} columns")
        return self._frame().rename(dict(zip(self.columns, names)))

    def equals(self, other: object) -> bool:
        from .dataframe import DataFrame

        if not isinstance(other, DataFrame) or self.columns != other.columns:
            return False
        return len(self._frame()) == len(other) and all(
            self._col(c).equals(column_series(other, c)) for c in self.columns
        )

    def squeeze(self) -> object:
        """A one-column frame as its Series, a one-cell frame as the value."""
        if len(self.columns) == 1:
            s = self._col(self.columns[0])
            return s[0] if len(s) == 1 else s
        return self

    def xs(self, key: object) -> DataFrame:
        """Rows with index label ``key`` (:attr:`loc`)."""
        from .dataframe import DataFrame

        out = self._frame().loc[key]
        assert isinstance(out, DataFrame)
        return out

    def truncate(self, before: object | None = None, after: object | None = None) -> DataFrame:
        """Rows with index labels in ``[before, after]`` (:attr:`loc`)."""
        from .dataframe import DataFrame

        out = self._frame().loc[slice(before, after)]
        assert isinstance(out, DataFrame)
        return out

    def sort_index(self, ascending: bool = True, *, axis: int | str = 0) -> DataFrame:
        """Sort by the index columns (a frame with no index is already in
        position order). ``axis=1`` (or ``"columns"``) orders the columns by
        name instead, ascending unless ``ascending=False``; the values of every
        column are unchanged and any index columns stay first."""
        if axis in (1, "columns"):
            keys = list(self.index_names())
            rest = sorted((c for c in self.columns if c not in keys), reverse=not ascending)
            return self._frame().select(*keys, *rest)
        if axis not in (0, "index"):
            raise ValueError(f"sort_index: axis must be 0/'index' or 1/'columns', got {axis!r}")
        keys = list(self.index_names())
        if not keys:
            return self._frame() if ascending else self._frame().reverse()
        return self._frame().sort_values(keys, ascending=ascending)

    def first_valid_index(self) -> object | None:
        """The label of the first row with any present value (``None`` when
        every row is all-null)."""
        return self._valid_edge(first=True)

    def last_valid_index(self) -> object | None:
        return self._valid_edge(first=False)

    def _valid_edge(self, first: bool) -> object | None:
        any_valid = None
        for name in self.columns:
            m = self._col(name).notna()
            any_valid = m if any_valid is None else any_valid.logical(1, m)
        if any_valid is None:
            return None
        pos = valid_edge(any_valid, first)
        if pos is None:
            return None
        labels = self._frame().index
        return labels[pos] if isinstance(labels, Series) else pos

    def select_dtypes(
        self, include: Sequence[object] | None = None, exclude: Sequence[object] | None = None
    ) -> DataFrame:
        """Columns by dtype: ``include`` / ``exclude`` take :class:`DType`
        members, dtype names or the words ``"number"`` / ``"string"``."""

        def group(spec: object) -> set[DType]:
            if isinstance(spec, str):
                if spec in ("number", "numeric"):
                    return {d for d in DType if d.is_numeric}
                if spec in ("string", "object", "str"):
                    return set(_STRING)
                if spec == "bool":
                    return {DType.BOOL}
                return {DType[spec.upper()]}
            if isinstance(spec, DType):
                return {spec}
            return {DType(spec)} if isinstance(spec, int) else set()

        if isinstance(include, (str, int)):
            include = [include]
        if isinstance(exclude, (str, int)):
            exclude = [exclude]
        inc = set().union(*(group(s) for s in include)) if include else None
        exc = set().union(*(group(s) for s in exclude)) if exclude else set()
        keep = [
            c
            for c in self.columns
            if (inc is None or self._col(c).dtype in inc) and self._col(c).dtype not in exc
        ]
        return self._frame().select(*keep)

    def memory_usage(self) -> DataFrame:
        """One row: the bytes each column's Arrow buffers hold."""
        from .dataframe import DataFrame

        return DataFrame({c: [self._col(c).nbytes] for c in self.columns})

    def isin(self, values: Sequence[object] | Series | Mapping[str, Sequence[object]]) -> DataFrame:
        """A Bool frame: each cell's membership in ``values`` (a list or
        Series for every column, or ``{column: values}`` for some, the rest
        false), pandas ``DataFrame.isin``."""
        out: dict[str, Series] = {}
        for name in self.columns:
            s = self._col(name)
            chosen: Sequence[object] | None
            if isinstance(values, Mapping):
                chosen = values.get(name)
            elif isinstance(values, Series):
                chosen = values.to_list()
            else:
                chosen = values
            # Only the values of the column's own family can match.
            text = s.dtype in _STRING
            fit = [
                v
                for v in (chosen or [])
                if v is not None
                and (isinstance(v, str) if text else isinstance(v, (int, float)))
                and not isinstance(v, bool)
            ]
            out[name] = s.is_in(fit)
        return self._rebuild(out)

    def combine(self, other: DataFrame, func: Callable[[Series, Series], Series]) -> DataFrame:
        """``func(this column, other's same-named column)`` for every column
        both hold (positional), pandas ``DataFrame.combine``."""
        out: dict[str, Series] = {}
        for name in self.columns:
            if name in other:
                out[name] = func(self._col(name), column_series(other, name))
        return self._rebuild(out, keep_others=False)

    def compare(self, other: DataFrame) -> DataFrame:
        """The cells that differ (pandas ``DataFrame.compare``, long form):
        one row per differing cell with ``row``, ``column``, ``self`` and
        ``other`` (the two values as strings; a null on one side is a
        difference), by column then row; :meth:`Series.compare` per column."""
        from .dataframe import DataFrame

        if self.columns != other.columns:
            raise ValueError("compare: the frames must hold the same columns")
        rows: list[int] = []
        cols: list[str] = []
        mine: list[str | None] = []
        theirs: list[str | None] = []
        for name in self.columns:
            diff = self._col(name).compare(column_series(other, name)).to_dict()
            for i, va, vb in zip(diff["index"], diff["self"], diff["other"]):
                assert isinstance(i, int)
                rows.append(i)
                cols.append(name)
                mine.append(None if va is None else str(va))
                theirs.append(None if vb is None else str(vb))
        return DataFrame(
            {
                "row": Series.from_list(rows, DType.INT64),
                "column": Series.from_list(cols, DType.STRING),
                "self": Series.from_list(mine, DType.STRING),
                "other": Series.from_list(theirs, DType.STRING),
            }
        )

    def corrwith(self, other: DataFrame) -> DataFrame:
        """One row: each numeric column's Pearson correlation with ``other``'s
        same-named column (positional), pandas ``DataFrame.corrwith``."""
        from .dataframe import DataFrame

        out: dict[str, list[float]] = {}
        for name in self.columns:
            s = self._col(name)
            if name in other and is_numeric_series(s):
                out[name] = [s.corr(column_series(other, name))]
        return DataFrame(out)

    def info(self) -> str:
        """A text summary, pandas ``DataFrame.info``: the row count, each
        column's type, present count and bytes; returned, not printed."""
        n = len(self._frame())
        lines = [f"DataFrame: {n} rows, {len(self.columns)} columns"]
        total = 0
        for name in self.columns:
            s = self._col(name)
            nbytes = int(s.nbytes)
            total += nbytes
            lines.append(
                f"  {name}: {s.dtype.name.lower()}, {n - s.null_count} present, {nbytes} bytes"
            )
        lines.append(f"memory: {total} bytes")
        return "\n".join(lines)

    def rename_axis(self, name: str) -> DataFrame:
        """The single index column renamed to ``name`` (pandas
        ``rename_axis``); needs ``set_index`` first."""
        from .dataframe import single_index

        current = single_index(self.index_names(), "rename_axis")
        return self._frame().rename({current: name})

    def kurtosis(self, fisher: bool = True, bias: bool = True) -> DataFrame:
        """The polars ``kurtosis`` of every numeric column, one row (see
        :meth:`Series.kurtosis`); ``kurt`` is the pandas estimator."""
        frame = self._frame()
        names = frame.reduce("kurt").columns
        return type(frame)({n: [frame[n].kurtosis(fisher, bias)] for n in names})

    def swapaxes(self, axis1: object = 0, axis2: object = 1) -> DataFrame:
        """pandas ``swapaxes``: the transpose (a frame has the two axes)."""
        return self.transpose()

    def combine_first(self, other: DataFrame) -> DataFrame:
        """This frame with nulls filled from ``other``'s same-named columns
        (positional)."""
        out: dict[str, Series] = {}
        for name in self.columns:
            if name in other:
                s = self._col(name)
                out[name] = s.where(s.notna(), column_series(other, name))
        return self._rebuild(out)

    def update(self, other: DataFrame) -> None:
        """Overwrite this handle's values with ``other``'s present values in
        the same-named columns (positional; the handle is rebound)."""
        out: dict[str, Series] = {}
        for name in self.columns:
            if name in other:
                o = column_series(other, name)
                out[name] = o.where(o.notna(), self._col(name))
        _ext.DataFrame._replace(self, self._rebuild(out))

    def dot(self, other: Series | DataFrame) -> Series | DataFrame:
        """Matrix product with a Series (one value per column) or a frame
        (each column of ``other`` against each of ours)."""
        from .dataframe import DataFrame

        names = [c for c in self.columns if is_numeric_series(self._col(c))]
        if isinstance(other, Series):
            return Series.from_list([self._col(c).dot(other) for c in names])
        return DataFrame(
            {o: [self._col(c).dot(column_series(other, o)) for c in names] for o in other.columns}
        )

    @classmethod
    def from_records(
        cls,
        records: Sequence[dict[str, object] | Sequence[object]],
        columns: Sequence[str] | None = None,
    ) -> DataFrame:
        """A frame from row dicts or row tuples (with ``columns``)."""
        from .dataframe import DataFrame

        rows = list(records)
        if not rows:
            return DataFrame({c: [] for c in (columns or [])})
        first = rows[0]
        if isinstance(first, dict):
            dicts = [r for r in rows if isinstance(r, dict)]
            names = list(columns) if columns else list(first.keys())
            return DataFrame({c: [r.get(c) for r in dicts] for c in names})
        if not columns:
            raise ValueError("from_records: row tuples need columns=")
        tuples = [list(r) for r in rows if not isinstance(r, dict)]
        return DataFrame({c: [r[i] for r in tuples] for i, c in enumerate(columns)})

    # -- the datetime-index methods, over the one index column (or `on`) ------
    def _time_column(self, on: str | None, op: str) -> tuple[str, Series]:
        from .dataframe import single_index

        name = on if on is not None else single_index(self.index_names(), op)
        if name not in self._frame():
            raise KeyError(f"{op}: no column named {name!r}")
        return name, self._col(name)

    @staticmethod
    def _time_of_day(text: str) -> int:
        """``"HH:MM"`` or ``"HH:MM:SS"`` as seconds since midnight."""
        parts = text.split(":")
        if len(parts) not in (2, 3) or not all(p.isdigit() for p in parts):
            raise ValueError(f"expected a time as HH:MM or HH:MM:SS, got {text!r}")
        h, m = int(parts[0]), int(parts[1])
        sec = int(parts[2]) if len(parts) == 3 else 0
        if h > 23 or m > 59 or sec > 59:
            raise ValueError(f"time out of range: {text!r}")
        return h * 3600 + m * 60 + sec

    @staticmethod
    def _seconds_of_day(s: Series) -> Series:
        dt = s.dt
        return dt.hour * 3600 + dt.minute * 60 + dt.second

    @staticmethod
    def _unit_of(s: Series) -> str:
        return s.dt._unit

    def tz_localize(self, tz: str | None, on: str | None = None) -> DataFrame:
        """The index column (or ``on``) marked as zone ``tz`` (pandas
        ``DataFrame.tz_localize``; UTC only, see ``Series.dt.tz_localize``)."""
        name, s = self._time_column(on, "tz_localize")
        return self._frame().with_column(name, s.dt.tz_localize(tz))

    def tz_convert(self, tz: str | None, on: str | None = None) -> DataFrame:
        """The index column (or ``on``) shown in zone ``tz`` (pandas
        ``DataFrame.tz_convert``); the instants stay."""
        name, s = self._time_column(on, "tz_convert")
        return self._frame().with_column(name, s.dt.tz_convert(tz))

    def at_time(self, time: str, on: str | None = None) -> DataFrame:
        """The rows whose time of day is exactly ``time`` (``"HH:MM[:SS]"``),
        by the index column or ``on`` (pandas ``at_time``)."""
        _, s = self._time_column(on, "at_time")
        return self._frame().filter(self._seconds_of_day(s).eq(self._time_of_day(time)))

    def between_time(
        self,
        start_time: str,
        end_time: str,
        inclusive: str = "both",
        on: str | None = None,
    ) -> DataFrame:
        """The rows whose time of day lies in ``[start_time, end_time]`` (an
        end before the start wraps past midnight), ``inclusive`` as pandas:
        ``both``, ``neither``, ``left``, ``right``."""
        _, s = self._time_column(on, "between_time")
        lo, hi = self._time_of_day(start_time), self._time_of_day(end_time)
        secs = self._seconds_of_day(s)
        if inclusive not in ("both", "neither", "left", "right"):
            raise ValueError("between_time: inclusive must be both, neither, left or right")
        after = secs.ge(lo) if inclusive in ("both", "left") else secs.gt(lo)
        before = secs.le(hi) if inclusive in ("both", "right") else secs.lt(hi)
        mask = after.logical(0, before) if lo <= hi else after.logical(1, before)
        return self._frame().filter(mask)

    def first(self, offset: int | str, on: str | None = None) -> DataFrame:
        """The rows within ``offset`` (``"3s"``, ``"1min"``, or a count of the
        column's units) of the earliest time (pandas ``first``); ``last``
        takes the latest."""
        _, s = self._time_column(on, "first")
        span = rule_to_units(offset, self._unit_of(s))
        ticks = s.astype("int64")
        return self._frame().filter(ticks < int(cast(int, ticks.min())) + span)

    def last(self, offset: int | str, on: str | None = None) -> DataFrame:
        _, s = self._time_column(on, "last")
        span = rule_to_units(offset, self._unit_of(s))
        ticks = s.astype("int64")
        return self._frame().filter(ticks > int(cast(int, ticks.max())) - span)

    def asfreq(
        self,
        freq: int | str,
        method: str | None = None,
        on: str | None = None,
        fill_value: int | float | None = None,
    ) -> DataFrame:
        """The frame on a regular grid of ``freq`` from the earliest to the
        latest time: each grid instant takes the row at exactly that time,
        else nulls, filled forward / backward with ``method="ffill"`` /
        ``"bfill"``, or with ``fill_value`` (pandas ``asfreq``). The time
        column must hold each instant once."""
        from .dataframe import DataFrame

        name, s = self._time_column(on, "asfreq")
        frame = self._frame()
        if len(frame) == 0:
            return frame
        if s.nunique() != len(frame) - s.null_count:
            raise ValueError("asfreq: the time column holds an instant more than once")
        every = rule_to_units(freq, self._unit_of(s))
        ticks = s.astype("int64")
        lo, hi = int(cast(int, ticks.min())), int(cast(int, ticks.max()))
        grid = DataFrame(
            {name: _like(Series.from_list(list(range(lo, hi + 1, every)), DType.INT64), s)}
        )
        out = grid.join(frame, on=name, how="left")
        others = [c for c in out.columns if c != name]
        if method in ("ffill", "pad"):
            out = out.ffill()
        elif method in ("bfill", "backfill"):
            out = out.bfill()
        elif method is not None:
            raise ValueError("asfreq: method must be ffill, bfill or None")
        if fill_value is not None:
            for c in others:
                if is_numeric_series(self._col_of(out, c)):
                    out = out.with_column(c, self._col_of(out, c).fillna(fill_value))
        return _ext.DataFrame.with_index(
            _ext.DataFrame.select(out, *frame._names()), frame.index_names()
        )

    @staticmethod
    def _col_of(frame: DataFrame, name: str) -> Series:
        return column_series(frame, name)

    # -- exports through pandas / pyarrow -----------------------------------------
    def _writes_index(self, index: bool | None) -> bool:
        """Whether a text export writes the row index: as asked, else when the
        frame has one (a frame with no index writes none)."""
        return bool(_ext.DataFrame.index_names(self)) if index is None else index

    def to_csv(
        self, path: str | None = None, index: bool | None = None, **kwargs: object
    ) -> str | None:
        to_csv = cast("Callable[..., str | None]", self._frame().to_pandas().to_csv)
        return to_csv(path, index=self._writes_index(index), **kwargs)

    def to_json(self, path: str | None = None, **kwargs: object) -> str | None:
        to_json = cast("Callable[..., str | None]", self._frame().to_pandas().to_json)
        return to_json(path, **kwargs)

    def to_string(self, index: bool | None = None, **kwargs: object) -> str:
        to_string = cast("Callable[..., str]", self._frame().to_pandas().to_string)
        return to_string(index=self._writes_index(index), **kwargs)

    def to_markdown(self, index: bool | None = None, **kwargs: object) -> str:
        to_markdown = cast("Callable[..., str]", self._frame().to_pandas().to_markdown)
        return to_markdown(index=self._writes_index(index), **kwargs)

    def to_html(self, index: bool | None = None, **kwargs: object) -> str:
        to_html = cast("Callable[..., str]", self._frame().to_pandas().to_html)
        return to_html(index=self._writes_index(index), **kwargs)

    def to_records(self) -> list[tuple[Scalar, ...]]:
        return list(self.itertuples(index=False, name=None))

    def to_parquet(self, path: str, **kwargs: object) -> None:
        import pyarrow.parquet as pq

        pq.write_table(self._frame()._arrow_for_parquet(), path, **kwargs)

    def to_feather(self, path: str) -> None:
        import pyarrow.feather as feather

        feather.write_feather(self._frame().to_arrow(), path)


class _FrameWindow:
    """``df.rolling(n)`` / ``expanding()`` / ``ewm(alpha)``: the pandas window
    object over a frame, each reduction the Series window's method per numeric
    column (the other columns drop, pandas ``numeric_only``)."""

    __slots__ = ("_frame", "_window")

    def __init__(self, frame: FramePandasMixin, window: Callable[[Series], object]) -> None:
        self._frame = frame
        self._window = window

    def _reduce(self, name: str, *args: object) -> DataFrame:
        return self._frame._map_columns(
            lambda s: getattr(self._window(s), name)(*args), keep_others=False
        )

    def sum(self) -> DataFrame:
        return self._reduce("sum")

    def mean(self) -> DataFrame:
        return self._reduce("mean")

    def min(self) -> DataFrame:
        return self._reduce("min")

    def max(self) -> DataFrame:
        return self._reduce("max")

    def count(self) -> DataFrame:
        return self._reduce("count")

    def var(self) -> DataFrame:
        return self._reduce("var")

    def std(self) -> DataFrame:
        return self._reduce("std")

    def median(self) -> DataFrame:
        return self._reduce("median")

    def quantile(self, q: float) -> DataFrame:
        return self._reduce("quantile", q)
