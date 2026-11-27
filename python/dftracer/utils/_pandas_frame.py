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
from ._enums import DType, rule_to_units
from ._label_align import LabelAlignMixin
from ._levels import (
    LevelArg,
    LevelsMixin,
    label_text,
    pick_levels,
    sort_plan,
    sorted_positions,
)
from ._pandas_series import (
    OMITTED,
    Omitted,
    edge_label,
    present_edge,
    replace_in,
    replace_pairs,
)
from ._reshape import stack_frame, unstack_frame
from ._series import Series
from ._series_index import (
    column_series,
    default_name,
    drop_levels,
    internal_name,
    positions,
    public_name,
    renamed_names,
)
from ._time_index import TimeIndexMixin
from ._value_counts import frame_value_counts, native_counts

if TYPE_CHECKING:
    import numpy as np
    import numpy.typing as npt
    from typing_extensions import Concatenate, ParamSpec, Self

    from ._apply import Row
    from ._dataframe import DataFrame
    from ._groupby import GroupBy
    from ._pandas_series import ReplaceKey, ReplaceValue
    from .dftracer_utils_ext import Scalar

_R = TypeVar("_R")
if TYPE_CHECKING:
    _P = ParamSpec("_P")

_STRING = {DType.STRING}

Operand = Union[int, float, Series, "DataFrame"]


def is_numeric_series(s: Series) -> bool:
    return s.dtype.is_numeric or s.dtype == DType.BOOL


def _group(frame: DataFrame, keys: Sequence[str]) -> GroupBy:
    from ._groupby import GroupBy

    return GroupBy(frame, list(keys))


class FramePandasMixin(LabelAlignMixin, LevelsMixin, TimeIndexMixin, _ext.DataFrame):
    """pandas spellings over the engine's Series kernels, one column at a
    time. ``self`` is a :class:`DataFrame`."""

    __slots__ = ()

    columns: List[str]

    def _frame(self) -> DataFrame:
        from ._dataframe import DataFrame

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
        from ._dataframe import DataFrame

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
        from ._dataframe import DataFrame

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
        from ._dataframe import DataFrame

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
        from ._dataframe import DataFrame

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
            counts = native_counts(s)
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
        from ._dataframe import DataFrame
        from ._expr import Agg, Col

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
        self,
        subset: Sequence[str] | None = None,
        normalize: bool = False,
        sort: bool = True,
        ascending: bool = False,
        dropna: bool = True,
    ) -> Series:
        """The count of each distinct row of ``subset`` (every column by
        default) as a Series named ``count``, labelled by the row's values (the
        pandas ``value_counts``). ``normalize`` gives fractions, named
        ``proportion``; ``sort=False`` keeps the order of first appearance and
        ``ascending`` flips the count order, ties keeping that order.
        ``dropna=False`` keeps the rows with a null key."""
        keys = list(subset) if subset else self.columns
        return frame_value_counts(self._frame(), keys, normalize, sort, ascending, dropna)

    def stack(
        self,
        level: LevelArg = -1,
        dropna: bool | Omitted = OMITTED,
        sort: bool | Omitted = OMITTED,
        future_stack: bool = True,
    ) -> Series:
        """The pandas ``stack``: the columns become the innermost index level
        of a Series, row by row (each row's columns in column order), under the
        existing index levels (the positions for a frame with no index). The
        default is pandas 3's implementation (``future_stack=True``), which keeps
        every cell, null ones too, and refuses ``dropna`` and ``sort``; with
        ``future_stack=False`` null cells are dropped unless ``dropna=False``
        (``sort`` has no effect on flat column names). The columns must share a
        type or all be numeric. The column names are one level, so ``level`` is
        0, -1 or ``[0]``; pandas 2 differs by defaulting to ``future_stack=False``.
        A frame has no MultiIndex columns, so the result is never a frame."""
        return cast(
            "Series",
            stack_frame(
                self._frame(),
                level,
                None if isinstance(dropna, Omitted) else dropna,
                None if isinstance(sort, Omitted) else sort,
                future_stack,
            ),
        )

    def unstack(
        self, level: LevelArg = -1, fill_value: object = None, sort: bool = True
    ) -> Series | DataFrame:
        """The pandas ``unstack``: with a MultiIndex the row-label ``level`` (a
        name, a position, a negative position or a list) becomes columns, one for
        each shown column and label, named with the text of the tuple
        ``(column, label)`` (a MultiIndex column of pandas); the other levels are
        the result's index, sorted, and a missing cell is null or ``fill_value``.
        With one index level (or none) the result is a Series labelled by the
        column name and the old label. ``ValueError`` for a label pair that
        occurs twice. The name of the column level is lost."""
        return unstack_frame(self._frame(), level, fill_value, sort)

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
    def values(self) -> npt.NDArray[np.generic]:
        """The frame as a 2-D NumPy array (``to_numpy``)."""
        return self.to_numpy()

    def to_numpy(self) -> npt.NDArray[np.generic]:
        import numpy as np

        return np.column_stack([self._col(c).to_numpy() for c in self.columns])

    @property
    def T(self) -> DataFrame:
        return self.transpose()

    def transpose(self) -> DataFrame:
        """Rows become columns named ``0``.. (``T``); values widen to one type
        as pandas does."""
        from ._dataframe import DataFrame

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
        from ._loc import rebind

        out = self._col(name)
        rebind(
            self._frame(),
            _ext.DataFrame.select(self, *[c for c in self._frame()._names() if c != name]),
        )
        return out

    def insert(self, loc: int, column: str, value: Series | int | float | str) -> None:
        """Put ``column`` at position ``loc`` (this handle is rebound)."""
        if column in self._frame():
            raise ValueError(f"insert: column {column!r} already exists")
        from ._loc import Rows, assign_rows, rebind

        assign_rows(self._frame(), Rows.all(), [column], value)
        order = [c for c in self._frame()._names() if c != column]
        order.insert(loc, column)
        rebind(self._frame(), _ext.DataFrame.select(self, *order))

    def _level_names(self) -> list[str]:
        return list(_ext.DataFrame.index_names(self))

    def _keep_levels(self, order: list[int]) -> DataFrame:
        names = self._level_names()
        keep = [names[i] for i in order]
        gone = {n for n in names if n not in keep}
        frame = self._frame()
        out = _ext.DataFrame.select(frame, *[c for c in frame._names() if c not in gone])
        return _ext.DataFrame.with_index(out, keep)

    def _axis_is_rows(self, axis: object) -> bool:
        if axis in (0, "index", "rows"):
            return True
        if axis in (1, "columns"):
            return False
        raise ValueError(f"No axis named {axis} for object type DataFrame")

    def add_prefix(self, prefix: str, axis: int | str | None = None) -> DataFrame:
        """The pandas ``add_prefix``: ``prefix`` in front of every column name
        (``axis=1``, the default) or in front of every row label as text
        (``axis=0``, each level of a ``MultiIndex``; a frame with no index gets
        the positions as labels)."""
        if axis is not None and self._axis_is_rows(axis):
            return self._text_labels(prefix, "")
        return self._frame().rename({c: prefix + c for c in self.columns})

    def add_suffix(self, suffix: str, axis: int | str | None = None) -> DataFrame:
        """As :meth:`add_prefix`, with ``suffix`` after the name or the label."""
        if axis is not None and self._axis_is_rows(axis):
            return self._text_labels("", suffix)
        return self._frame().rename({c: c + suffix for c in self.columns})

    def _text_labels(self, prefix: str, suffix: str) -> DataFrame:
        frame = self._frame()
        names = self._level_names()
        if not names:
            names = [default_name(0)]
            frame = frame.with_column(names[0], positions(len(frame)))
        for name in names:
            level = column_series(frame, name)
            frame = frame.with_column(name, label_text(level, prefix, suffix))
        return _ext.DataFrame.with_index(frame, names)

    def reindex(
        self,
        labels: object = None,
        *,
        index: object = None,
        columns: object = None,
        axis: int | str | None = None,
        method: str | None = None,
        copy: bool | None = None,
        level: object = None,
        fill_value: object = None,
        limit: int | None = None,
        tolerance: object = None,
    ) -> DataFrame:
        """The pandas ``reindex``: the rows on the index labels ``index`` (or
        ``labels`` with ``axis=0``) and the columns on ``columns`` (or
        ``labels`` with ``axis=1``). A row or column that is new gets nulls (an
        integer column that gains rows becomes Float64) or ``fill_value``, which
        keeps a column's type when it fits and raises ``TypeError`` when it does
        not. The rules of :meth:`Series.reindex` apply to the rows, ``method``,
        ``limit`` and ``tolerance`` included; a frame with no index has the
        labels ``0 .. n-1``. A fill ``method`` with ``columns`` and ``level``
        are not supported, and column labels must be strings."""
        from ._reindex import reindex_frame

        return reindex_frame(
            self._frame(),
            labels,
            index=index,
            columns=columns,
            level=level,
            axis=axis,
            tolerance=tolerance,
            method=method,
            limit=limit,
            fill_value=fill_value,
        )

    def set_axis(self, labels: Sequence[str]) -> DataFrame:
        """New column names, positionally."""
        names = list(labels)
        if len(names) != len(self.columns):
            raise ValueError(f"set_axis: {len(names)} names for {len(self.columns)} columns")
        return self._frame().rename(dict(zip(self.columns, names)))

    def equals(self, other: object) -> bool:
        from ._dataframe import DataFrame

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

    def xs(
        self, key: object, axis: int = 0, level: object = None, drop_level: bool = True
    ) -> DataFrame | Series | Row:
        """Rows with index label ``key`` (:attr:`loc`: a Series for a label one
        row holds); with ``level`` the rows
        whose index level of that name or position is ``key``, that level left
        out unless ``drop_level`` is false. A part of a MultiIndex label leaves
        out the levels it names."""
        from ._loc import Labels, _apply_rows, drop_index_levels, rows_by_level

        if axis not in (0, "index"):
            raise ValueError("xs: only the index axis is supported")
        frame = self._frame()
        if level is None:
            return cast("DataFrame | Series | Row", frame.loc[key])
        labels = Labels(frame)
        rows = rows_by_level(labels, level, key)
        out = _apply_rows(frame, rows)
        if drop_level:
            out = drop_index_levels(out, labels.names, rows.drop)
        return out

    def truncate(self, before: object | None = None, after: object | None = None) -> DataFrame:
        """Rows with index labels in ``[before, after]`` (:attr:`loc`)."""
        from ._dataframe import DataFrame

        out = self._frame().loc[slice(before, after)]
        assert isinstance(out, DataFrame)
        return out

    def sort_index(
        self,
        ascending: bool | Sequence[bool] = True,
        *,
        axis: int | str = 0,
        level: LevelArg = None,
        sort_remaining: bool = True,
        na_position: Literal["first", "last"] = "last",
        ignore_index: bool = False,
        kind: str = "quicksort",
        key: Callable[[Series], Series] | None = None,
        inplace: bool = False,
    ) -> DataFrame:
        """Sort by the index columns (a frame with no index is already in
        position order). With ``level`` (a name, a position or a list) the rows
        are ordered by those levels first and, with ``sort_remaining``, by the
        others after them; ``ascending`` is one flag or one for each level in
        ``level``, a null label goes last (or first with ``na_position``),
        ``key`` is called on each level and returns the values to order by, and
        ``ignore_index`` drops the index afterwards. The sort is stable, so
        ``kind`` is accepted and not used. ``axis=1`` (or ``"columns"``) orders
        the columns by name instead, ascending unless ``ascending=False``; the
        values of every column are unchanged and any index columns stay first."""
        if inplace:
            raise ValueError("sort_index: a DataFrame is immutable; use the result")
        frame = self._frame()
        keys = list(self.index_names())
        if not self._axis_is_rows(axis):
            rest = sorted((c for c in self.columns if c not in keys), reverse=not ascending)
            return frame.select(*keys, *rest)
        if not keys and key is None:
            _, flags, _ = sort_plan([default_name(0)], level, ascending, sort_remaining)
            if flags[0]:
                return frame
        if not keys:
            keys = [default_name(0)]
            frame = frame.with_column(keys[0], positions(len(frame))).set_index(keys)
        picked, flags, keyed = sort_plan(keys, level, ascending, sort_remaining)
        if key is None:
            out = frame._sort_values([keys[i] for i in picked], flags, na_position)
        else:
            levels = [column_series(frame, keys[i]) for i in picked]
            order = sorted_positions(
                [keys[i] for i in picked], levels, flags, na_position, key, keyed
            )
            out = frame.gather(order)
        return out.reset_index(drop=True) if ignore_index else out

    def first_valid_index(self) -> object | None:
        """The label of the first row with any present value (``None`` when
        every row is all-null)."""
        return self._valid_edge(first=True)

    def last_valid_index(self) -> object | None:
        return self._valid_edge(first=False)

    def _valid_edge(self, first: bool) -> object | None:
        edges = [present_edge(self._col(name), first) for name in self.columns]
        found = [p for p in edges if p is not None]
        if not found:
            return None
        return edge_label(self._frame().index, min(found) if first else max(found))

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

    def memory_usage(self, index: bool = True, deep: bool = False) -> Series[int]:
        """The bytes each column's buffers hold, labelled by column, as pandas.
        With ``index`` the first row, ``"Index"``, holds the row label columns
        (0 for a frame with none). ``deep`` changes nothing: the buffers
        already hold every string byte."""
        from ._series import Series

        names = list(self.columns)
        sizes = [int(self._col(c).nbytes) for c in names]
        if index:
            shown = set(names)
            labels = [c for c in self._frame()._names() if c not in shown]
            names.insert(0, "Index")
            sizes.insert(0, sum(int(self._col(c).nbytes) for c in labels))
        return cast("Series[int]", Series.from_list(sizes, DType.INT64).set_axis(names))

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
        from ._dataframe import DataFrame

        if self.columns != other.columns:
            raise ValueError("compare: the frames must hold the same columns")
        rows: list[int] = []
        cols: list[str] = []
        mine: list[str | None] = []
        theirs: list[str | None] = []
        for name in self.columns:
            diff = drop_levels(self._col(name)).compare(drop_levels(column_series(other, name)))
            plain = diff.to_dict()
            for i, va, vb in zip(diff.index_levels()[0].to_list(), plain["self"], plain["other"]):
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
        from ._dataframe import DataFrame

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

    def rename_axis(
        self,
        mapper: object = OMITTED,
        *,
        index: object = OMITTED,
        columns: object = OMITTED,
        axis: int | str = 0,
        copy: bool | None = None,
        inplace: bool = False,
    ) -> DataFrame:
        """The pandas ``rename_axis`` on the row labels: a scalar or ``None``
        names a single level, a list names every level, and with ``index=`` a
        dict or a function maps the current names. A frame with no index gains
        its positions as the index under the name. ``ValueError`` when a name is
        already a column or repeats (pandas allows it); the column axis has no
        name here, so ``columns=`` other than ``None`` raises
        ``NotImplementedError``."""
        if inplace:
            raise ValueError("rename_axis: a DataFrame is immutable; use the result")
        rows = self._axis_is_rows(axis)
        if not isinstance(columns, Omitted) and columns is not None:
            raise NotImplementedError("rename_axis: the column axis has no name")
        given = index if not isinstance(index, Omitted) else mapper
        if isinstance(given, Omitted) or (not rows and given is None):
            return self._frame()
        if given is mapper and (callable(given) or isinstance(given, dict)):
            raise ValueError("Use `.rename` to alter labels with a mapper.")
        if not rows:
            raise NotImplementedError("rename_axis: the column axis has no name")
        frame = self._frame()
        current = self._level_names()
        fresh = not current
        new = renamed_names(current or [default_name(0)], given)
        if fresh:
            if new == [default_name(0)]:
                return frame
            frame = frame.with_column(new[0], positions(len(frame))).set_index(new)
            return frame
        if len(set(new)) != len(new) or any(n in self.columns for n in new):
            raise ValueError("rename_axis: an index level cannot take the name of a column")
        return frame.rename({a: b for a, b in zip(current, new) if a != b})

    # -- the pandas Index surface of what ``index`` returns ---------------------------
    @property
    def names(self) -> list[str | None]:
        """The level names when this frame is the ``index`` of another object (the
        levels are its columns)."""
        return [public_name(c) for c in self._frame()._names()]

    @property
    def nlevels(self) -> int:
        """The number of levels when this frame is the ``index`` of another object:
        its columns."""
        return len(self._frame()._names())

    def get_level_values(self, level: int | str | None) -> Series:
        """The level ``level`` (a name or a position) of the ``index`` of another
        object, as a Series: the column of this frame. ``IndexError`` or
        ``KeyError`` for another level, as pandas."""
        names = self._frame()._names()
        return column_series(self._frame(), names[pick_levels(names, level)[0]])

    def set_names(
        self, names: object, *, level: LevelArg = None, inplace: bool = False
    ) -> DataFrame:
        """The levels (the columns) under new names, as ``Index.set_names``: a
        list of one name for each, or with ``level`` one name for it (a list for
        a list of levels). ``ValueError`` for a name that repeats."""
        if inplace:
            raise ValueError("set_names: a DataFrame is immutable; use the result")
        current = self._frame()._names()
        if level is None:
            new = renamed_names(current, names)
        else:
            picked = pick_levels(current, level)
            values = list(names) if isinstance(names, (list, tuple)) else [names]
            if len(values) != len(picked):
                raise ValueError(f"Length of new names must be {len(picked)}, got {len(values)}")
            new = list(current)
            for i, v in zip(picked, values):
                new[i] = internal_name(v, i)
        if len(set(new)) != len(new):
            raise ValueError("set_names: a level name repeats")
        return self._frame().rename(dict(zip(current, new)))

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
        """The pandas ``combine_first``: both frames on the union of their row
        labels and columns, with this frame's value where it has one and
        ``other``'s elsewhere."""
        from ._pandas_series import kept_type, present_or, realigned

        frame = self._frame()
        mine, theirs = frame.align(other, join="outer")
        theirs = cast("DataFrame", theirs)
        names = [*frame.columns, *(n for n in other.columns if n not in frame)]
        mine = mine[names]
        out: dict[str, Series] = {}
        for name in names:
            if name not in other:
                out[name] = realigned(column_series(mine, name), column_series(frame, name))
            elif name not in frame:
                out[name] = realigned(column_series(theirs, name), column_series(other, name))
            else:
                kept = kept_type(column_series(frame, name), column_series(other, name))
                out[name] = present_or(column_series(mine, name), column_series(theirs, name), kept)
        return mine._rebuild(out)

    def update(self, other: DataFrame) -> None:
        """The pandas ``update``: ``other`` on this frame's row labels, and its
        present values over this frame's in the columns both have (the handle is
        rebound)."""
        from ._pandas_series import has_duplicate_labels, kept_by_update, present_or

        frame = self._frame()
        if has_duplicate_labels(other):
            raise ValueError("Update not allowed with duplicate indexes on other.")
        mine, theirs = frame.align(other, join="left", axis=0)
        theirs = cast("DataFrame", theirs)
        out: dict[str, Series] = {}
        for name in mine.columns:
            s = column_series(mine, name)
            if name in theirs:
                s = kept_by_update(present_or(column_series(theirs, name), s), s.dtype)
            out[name] = s
        from ._loc import rebind

        rebind(self._frame(), mine._rebuild(out))

    def dot(self, other: Series | DataFrame) -> Series | DataFrame:
        """Matrix product with a Series (one value per column) or a frame
        (each column of ``other`` against each of ours)."""
        from ._dataframe import DataFrame

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
        from ._dataframe import DataFrame

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
        from ._dataframe import single_index

        name = on if on is not None else single_index(self.index_names(), op)
        if name not in self._frame():
            raise KeyError(f"{op}: no column named {name!r}")
        return name, self._col(name)

    @staticmethod
    def _unit_of(s: Series) -> str:
        return s.dt._unit

    def _rows_where(self, mask: Series) -> DataFrame:
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
