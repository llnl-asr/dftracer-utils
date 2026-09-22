"""The pandas ``DataFrame`` surface that is a composition of engine ops:
column-wise arithmetic, comparisons and transforms (one Series kernel per
column), the one-row reductions, and the small conveniences (``pipe``,
``items``, ``T``, ``to_csv`` through pandas). Mixed into :class:`DataFrame`;
each method names the engine op it runs in the parity table
(``scripts/check_op_parity_doxygen.py``)."""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    Iterator,
    List,
    Literal,
    Mapping,
    Optional,
    Sequence,
    Tuple,
    Union,
)

from .enums import DType
from .series import Series, _require_pyarrow, _unwrap

if TYPE_CHECKING:
    from .columnar import GroupBy
    from .dataframe import DataFrame

_NUMERIC = {
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
    DType.BOOL,
}
_STRING = {DType.STRING}

Operand = Union[int, float, Series, "DataFrame"]


def _is_numeric(s: Series) -> bool:
    return s.dtype in _NUMERIC


def _group(frame: "DataFrame", keys: Sequence[str]) -> "GroupBy":
    from .columnar import GroupBy

    return GroupBy(frame, list(keys))


def _series(frame: "DataFrame", name: str) -> Series:
    return Series(frame._native[name])


class _FramePandasMixin:
    """pandas spellings over the engine's Series kernels, one column at a
    time. ``self`` is a :class:`DataFrame`."""

    __slots__ = ()

    # The DataFrame these methods run on; typed loosely so the mixin can be
    # checked without the concrete class.
    _native: Any
    columns: List[str]
    _index: Optional[List[str]]

    def _frame(self) -> "DataFrame":
        from .dataframe import DataFrame

        assert isinstance(self, DataFrame)
        return self

    def _col(self, name: str) -> Series:
        return Series(self._native[name])

    def _rebuild(self, columns: Dict[str, Series], keep_others: bool = True) -> "DataFrame":
        """A frame with ``columns`` replacing (or, with ``keep_others`` off,
        being) this frame's columns, in this frame's order."""
        from .dataframe import DataFrame

        out = self._native
        names = self.columns if keep_others else [c for c in self.columns if c in columns]
        if not keep_others:
            out = out.select(*names)
        for name in names:
            if name in columns:
                out = out.with_column(name, _unwrap(columns[name]))
        frame = DataFrame(out)
        if self._index is not None and all(n in frame for n in self._index):
            frame._index = self._index
        return frame

    def _map_columns(
        self, fn: Callable[[Series], Series], numeric_only: bool = True, keep_others: bool = True
    ) -> "DataFrame":
        """``fn`` over each (numeric) column; other columns kept or dropped."""
        out: Dict[str, Series] = {}
        for name in self.columns:
            s = self._col(name)
            if numeric_only and not _is_numeric(s):
                continue
            out[name] = fn(s)
        return self._rebuild(out, keep_others)

    def _operand(self, other: Operand, name: str) -> Union[int, float, Series]:
        """The right-hand side for column ``name``: a scalar, a row-aligned
        Series (positional), or the same-named column of a frame."""
        from .dataframe import DataFrame

        if isinstance(other, DataFrame):
            if name not in other:
                raise KeyError(f"no column named {name!r} in the other frame")
            return Series(other._native[name])
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

    def _binary(self, other: Operand, op: str, reverse: bool = False) -> "DataFrame":
        from .dataframe import DataFrame

        names = [c for c in self.columns if _is_numeric(self._col(c))]
        if isinstance(other, DataFrame):
            names = [c for c in names if c in other]
        out: Dict[str, Series] = {}
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
    def add(self, other: Operand) -> "DataFrame":
        """Column-wise ``+`` over the numeric columns: a scalar, a row-aligned
        Series, or the same-named columns of another frame. Rows match by
        position (join first for label alignment)."""
        return self._binary(other, "__add__")

    def sub(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__sub__")

    def mul(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__mul__")

    def div(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__truediv__")

    def radd(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__add__", reverse=True)

    def rsub(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__sub__", reverse=True)

    def rmul(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__mul__", reverse=True)

    def rdiv(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__truediv__", reverse=True)

    def floordiv(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__floordiv__")

    def mod(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__mod__")

    def pow(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__pow__")

    def rfloordiv(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__floordiv__", reverse=True)

    def rmod(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__mod__", reverse=True)

    def rpow(self, other: Operand) -> "DataFrame":
        return self._binary(other, "__pow__", reverse=True)

    truediv = div
    rtruediv = rdiv
    subtract = sub
    multiply = mul
    divide = div

    def __floordiv__(self, other: Operand) -> "DataFrame":
        return self.floordiv(other)

    def __mod__(self, other: Operand) -> "DataFrame":
        return self.mod(other)

    def __pow__(self, other: Operand) -> "DataFrame":
        return self.pow(other)

    def __add__(self, other: Operand) -> "DataFrame":
        return self.add(other)

    def __sub__(self, other: Operand) -> "DataFrame":
        return self.sub(other)

    def __mul__(self, other: Operand) -> "DataFrame":
        return self.mul(other)

    def __truediv__(self, other: Operand) -> "DataFrame":
        return self.div(other)

    def __radd__(self, other: Operand) -> "DataFrame":
        return self.radd(other)

    def __rsub__(self, other: Operand) -> "DataFrame":
        return self.rsub(other)

    def __rmul__(self, other: Operand) -> "DataFrame":
        return self.rmul(other)

    def __rtruediv__(self, other: Operand) -> "DataFrame":
        return self.rdiv(other)

    def __neg__(self) -> "DataFrame":
        return self._map_columns(lambda s: s.negate(), keep_others=False)

    # -- comparisons: a Bool frame ---------------------------------------------
    def _compare(self, other: Operand, op: str) -> "DataFrame":
        from .dataframe import DataFrame

        out: Dict[str, Series] = {}
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
                if not _is_numeric(s) or not _is_numeric(rhs):
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

    def eq(self, other: Operand) -> "DataFrame":
        """Column-wise ``==`` as a Bool frame (``ne``, ``lt``, ``le``, ``gt``,
        ``ge`` likewise)."""
        return self._compare(other, "eq")

    def ne(self, other: Operand) -> "DataFrame":
        return self._compare(other, "ne")

    def lt(self, other: Operand) -> "DataFrame":
        return self._compare(other, "lt")

    def le(self, other: Operand) -> "DataFrame":
        return self._compare(other, "le")

    def gt(self, other: Operand) -> "DataFrame":
        return self._compare(other, "gt")

    def ge(self, other: Operand) -> "DataFrame":
        return self._compare(other, "ge")

    # -- column-wise transforms ---------------------------------------------------
    def abs(self) -> "DataFrame":
        return self._map_columns(lambda s: s.abs())

    def round(self, decimals: int = 0) -> "DataFrame":
        return self._map_columns(lambda s: s.round(decimals))

    def clip(
        self, lower: Optional[Union[int, float]] = None, upper: Optional[Union[int, float]] = None
    ) -> "DataFrame":
        return self._map_columns(lambda s: s.clip(lower, upper))

    def cumsum(self) -> "DataFrame":
        return self._map_columns(lambda s: s.cumsum())

    def cummax(self) -> "DataFrame":
        return self._map_columns(lambda s: s.cummax())

    def cummin(self) -> "DataFrame":
        return self._map_columns(lambda s: s.cummin())

    def cumprod(self) -> "DataFrame":
        return self._map_columns(lambda s: s.cumprod())

    def diff(self) -> "DataFrame":
        return self._map_columns(lambda s: s.diff())

    def pct_change(self) -> "DataFrame":
        return self._map_columns(lambda s: s.pct_change())

    def shift(self, periods: int = 1) -> "DataFrame":
        """Every column shifted by ``periods`` rows (negative looks ahead)."""
        return self._map_columns(lambda s: s.shift(periods), numeric_only=False)

    def rank(
        self,
        method: Literal["average", "min", "max", "dense", "ordinal", "first"] = "average",
        ascending: bool = True,
    ) -> "DataFrame":
        return self._map_columns(lambda s: s.rank(method, ascending=ascending))

    def interpolate(self) -> "DataFrame":
        return self._map_columns(lambda s: s.interpolate())

    def rolling(self, window: int) -> "_FrameWindow":
        """A trailing window of ``window`` rows over every numeric column
        (pandas ``DataFrame.rolling``): ``.sum() .mean() .min() .max() .var()
        .std() .median() .quantile(q)``, each the Series rolling kernel per
        column."""
        return _FrameWindow(self, lambda s: s.rolling(window))

    def expanding(self) -> "_FrameWindow":
        """The window from the first row to each row, over every numeric
        column (pandas ``DataFrame.expanding``): ``.sum() .mean() .min() .max()
        .count() .var() .std()``."""
        return _FrameWindow(self, lambda s: s.expanding())

    def ewm(
        self,
        alpha: Optional[float] = None,
        *,
        span: Optional[float] = None,
        com: Optional[float] = None,
        halflife: Optional[float] = None,
    ) -> "_FrameWindow":
        """The exponentially weighted window over every numeric column
        (pandas ``DataFrame.ewm``): ``.mean() .std()``."""
        from .series import ewm_alpha

        a = ewm_alpha(alpha, span, com, halflife)
        return _FrameWindow(self, lambda s: s.ewm(a))

    def ffill(self) -> "DataFrame":
        """Nulls in every column filled from the nearest present value before
        them (``pad``); ``bfill`` / ``backfill`` from after."""
        return self._map_columns(lambda s: s.ffill(), numeric_only=False)

    def bfill(self) -> "DataFrame":
        return self._map_columns(lambda s: s.bfill(), numeric_only=False)

    pad = ffill
    backfill = bfill

    def isna(self) -> "DataFrame":
        """A Bool frame: true where a value is null (``isnull`` / ``notna`` /
        ``notnull`` likewise)."""
        return self._map_columns(lambda s: s.isna(), numeric_only=False)

    isnull = isna

    def notna(self) -> "DataFrame":
        return self._map_columns(lambda s: s.notna(), numeric_only=False)

    notnull = notna

    def where(self, cond: Series, other: Union[int, float] = 0) -> "DataFrame":
        """Each numeric column where the row mask ``cond`` holds, else ``other``
        (``mask`` is the inverse)."""
        return self._map_columns(lambda s: s.where(cond, other))

    def mask(self, cond: Series, other: Union[int, float] = 0) -> "DataFrame":
        return self._map_columns(lambda s: s.mask(cond, other))

    def replace(self, to_replace: object, value: object) -> "DataFrame":
        """``to_replace`` -> ``value`` in every column whose type holds both."""

        def one(s: Series) -> Series:
            if s.dtype in _STRING:
                if not isinstance(to_replace, str) or not isinstance(value, str):
                    return s
                return _replace_str(s, to_replace, value)
            if isinstance(to_replace, str) or isinstance(value, str):
                return s
            if not isinstance(to_replace, (int, float)) or not isinstance(value, (int, float)):
                return s
            return s.mask(s.eq(to_replace), value)

        return self._map_columns(one, numeric_only=False)

    def applymap(self, func: Callable[[object], object]) -> "DataFrame":
        """``func`` over every element, column by column (:meth:`Series.apply`
        per column; the pandas ``map``)."""
        return self._map_columns(lambda s: s.apply(func), numeric_only=False)

    map = applymap

    def transform(self, func: Union[str, Callable[[Series], Series]]) -> "DataFrame":
        """``func`` (a Series -> Series function over every column, or a
        Series method name over the numeric ones), keeping the shape."""
        if isinstance(func, str):
            return self._map_columns(lambda s: getattr(s, func)())
        return self._map_columns(func, numeric_only=False)

    # -- reductions: one row, each numeric column under its own name -----------
    def _reduce_scalar(
        self, fn: Callable[[Series], object], numeric_only: bool = True
    ) -> "DataFrame":
        from .dataframe import DataFrame

        out: Dict[str, List[object]] = {}
        for name in self.columns:
            s = self._col(name)
            if numeric_only and not _is_numeric(s):
                continue
            out[name] = [fn(s)]
        return DataFrame(out)

    def median(self) -> "DataFrame":
        """One row of each numeric column's median (exact: the Series
        quantile sorts; ``quantile(q)`` likewise, linear interpolation)."""
        return self._reduce_scalar(lambda s: s.median())

    def quantile(self, q: float = 0.5) -> "DataFrame":
        return self._reduce_scalar(lambda s: s.quantile(q))

    def nunique(self) -> "DataFrame":
        return self._reduce_scalar(lambda s: s.nunique(), numeric_only=False)

    def prod(self) -> "DataFrame":
        return self._reduce_scalar(lambda s: s.product())

    product = prod

    def sem(self) -> "DataFrame":
        """Standard error of the mean: ``std / sqrt(count)`` per column."""
        return self._reduce_scalar(lambda s: s.stddev() / (s.count() ** 0.5) if s.count() else None)

    def mode(self) -> "DataFrame":
        """One row: each numeric column's most frequent value, the smallest
        on a tie (pandas lists the tied modes sorted)."""

        def one(s: Series) -> object:
            counts = s.value_counts()
            if len(counts) == 0:
                return None
            count = _series(counts, "count")
            top = count.max()
            return _series(counts.filter(count.eq(top)), "value").min()

        return self._reduce_scalar(one)

    def idxmax(self) -> "DataFrame":
        """One row: the position of each numeric column's maximum (``idxmin``
        likewise); the index label when an index is set."""
        return self._positions(lambda s: s.idxmax())

    def idxmin(self) -> "DataFrame":
        return self._positions(lambda s: s.idxmin())

    def _positions(self, fn: Callable[[Series], int]) -> "DataFrame":
        labels = self._index[0] if self._index is not None and len(self._index) == 1 else None
        index = self._col(labels) if labels else None

        def one(s: Series) -> object:
            pos = fn(s)
            return index[pos] if index is not None else pos

        return self._reduce_scalar(one)

    def any(self) -> "DataFrame":
        """One row: whether any value is true / non-zero per column (``all``
        likewise)."""
        return self._reduce_scalar(lambda s: bool(s.astype(DType.FLOAT64).ne(0).any()))

    def all(self) -> "DataFrame":
        return self._reduce_scalar(lambda s: bool(s.astype(DType.FLOAT64).ne(0).all()))

    def corr(self) -> "DataFrame":
        """The correlation matrix of the numeric columns (``cov`` the
        covariance matrix): one row per column, in column order."""
        return self._pairwise("corr")

    def cov(self) -> "DataFrame":
        return self._pairwise("covar_samp")

    def _pairwise(self, agg: str) -> "DataFrame":
        from .columnar import Agg, _Col
        from .dataframe import DataFrame

        names = [c for c in self.columns if _is_numeric(self._col(c))]
        specs = [Agg(agg, _Col(b), f"{a}__{b}", by=_Col(a)) for a in names for b in names]
        if not specs:
            return DataFrame({"column": []})
        row = _group(self._frame(), []).agg(*specs).to_dict()
        table: Dict[str, List[object]] = {"column": list(names)}
        for b in names:
            table[b] = [row[f"{a}__{b}"][0] for a in names]
        return DataFrame(table)

    def value_counts(
        self, subset: Optional[Sequence[str]] = None, ascending: bool = False
    ) -> "DataFrame":
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
    def axes(self) -> List[object]:
        return [self._frame().index, list(self.columns)]

    @property
    def values(self) -> Any:
        """The frame as a 2-D NumPy array (``to_numpy``)."""
        return self.to_numpy()

    def to_numpy(self) -> Any:
        import numpy as np  # ty: ignore[unresolved-import]

        return np.column_stack([self._col(c).to_numpy() for c in self.columns])

    @property
    def T(self) -> "DataFrame":
        return self.transpose()

    def transpose(self) -> "DataFrame":
        """Rows become columns named ``0``.. (``T``); values widen to one type
        as pandas does."""
        from .dataframe import DataFrame

        rows = self._frame().to_dict()
        names = self.columns
        n = len(self._frame())
        mixed = not all(_is_numeric(self._col(c)) for c in names)

        def cell(v: object) -> object:
            return v if v is None or not mixed else str(v)

        return DataFrame(
            {"column": names, **{str(i): [cell(rows[c][i]) for c in names] for i in range(n)}}
        )

    def copy(self, deep: bool = True) -> "DataFrame":
        """A new handle on the same immutable columns (nothing is copied)."""
        from .dataframe import DataFrame

        out = DataFrame(self._native)
        out._index = self._index
        return out

    def pipe(self, func: Callable[..., Any], *args: object, **kwargs: Any) -> Any:
        return func(self, *args, **kwargs)

    def items(self) -> Iterator[Tuple[str, Series]]:
        for name in self.columns:
            yield name, self._col(name)

    def iterrows(self) -> Iterator[Tuple[object, Any]]:
        """``(label, Row)`` per row; the label is the index value or the
        position."""
        from ._apply import Row

        cols = {c: self._col(c).to_list() for c in self.columns}
        labels: List[object] = list(range(len(self._frame())))
        if self._index is not None and len(self._index) == 1:
            labels = _series(self._frame(), self._index[0]).to_list()
        for i in range(len(self._frame())):
            yield labels[i], Row({c: cols[c][i] for c in self.columns})

    def itertuples(self, index: bool = True, name: Optional[str] = "Row") -> Iterator[tuple]:
        from collections import namedtuple

        fields = (["Index"] if index else []) + list(self.columns)
        cols = {c: self._col(c).to_list() for c in self.columns}
        labels = list(range(len(self._frame())))
        if index and self._index is not None and len(self._index) == 1:
            labels = self._col(self._index[0]).to_list()
        Tup = namedtuple(name or "Row", fields, rename=True)  # type: ignore[misc]
        for i in range(len(self._frame())):
            row = ([labels[i]] if index else []) + [cols[c][i] for c in self.columns]
            yield Tup(*row)

    def get(self, key: str, default: object = None) -> object:
        return self._col(key) if key in self._frame() else default

    def pop(self, name: str) -> Series:
        """Remove ``name`` from this handle and return it."""
        out = self._col(name)
        self._native = self._native.select(*[c for c in self.columns if c != name])
        return out

    def insert(self, loc: int, column: str, value: Union[Series, int, float, str]) -> None:
        """Put ``column`` at position ``loc`` (this handle is rebound)."""
        from .indexing import Rows, assign_rows

        if column in self._frame():
            raise ValueError(f"insert: column {column!r} already exists")
        assign_rows(self._frame(), Rows.all(), [column], value)
        order = [c for c in self.columns if c != column]
        order.insert(loc, column)
        self._native = self._native.select(*order)

    def add_prefix(self, prefix: str) -> "DataFrame":
        return self._frame().rename({c: prefix + c for c in self.columns})

    def add_suffix(self, suffix: str) -> "DataFrame":
        return self._frame().rename({c: c + suffix for c in self.columns})

    def set_axis(self, labels: Sequence[str]) -> "DataFrame":
        """New column names, positionally."""
        names = list(labels)
        if len(names) != len(self.columns):
            raise ValueError(f"set_axis: {len(names)} names for {len(self.columns)} columns")
        return self._frame().rename(dict(zip(self.columns, names)))

    def equals(self, other: object) -> bool:
        from .dataframe import DataFrame

        if not isinstance(other, DataFrame) or self.columns != other.columns:
            return False
        return self._frame().to_dict() == other.to_dict()

    def squeeze(self) -> object:
        """A one-column frame as its Series, a one-cell frame as the value."""
        if len(self.columns) == 1:
            s = self._col(self.columns[0])
            return s[0] if len(s) == 1 else s
        return self

    def xs(self, key: object) -> "DataFrame":
        """Rows with index label ``key`` (:attr:`loc`)."""
        from .dataframe import DataFrame

        out = self._frame().loc[key]
        assert isinstance(out, DataFrame)
        return out

    def truncate(
        self, before: Optional[object] = None, after: Optional[object] = None
    ) -> "DataFrame":
        """Rows with index labels in ``[before, after]`` (:attr:`loc`)."""
        from .dataframe import DataFrame

        out = self._frame().loc[slice(before, after)]
        assert isinstance(out, DataFrame)
        return out

    def sort_index(self, ascending: bool = True) -> "DataFrame":
        """Sort by the index columns (a frame with no index is already in
        position order)."""
        if self._index is None:
            return self._frame() if ascending else self._frame().reverse()
        return self._frame().sort_values(list(self._index), ascending=ascending)

    def first_valid_index(self) -> Optional[object]:
        """The label of the first row with any present value (``None`` when
        every row is all-null)."""
        return self._valid_edge(first=True)

    def last_valid_index(self) -> Optional[object]:
        return self._valid_edge(first=False)

    def _valid_edge(self, first: bool) -> Optional[object]:
        any_valid = None
        for name in self.columns:
            m = self._col(name).notna()
            any_valid = m if any_valid is None else any_valid.logical(1, m)
        if any_valid is None:
            return None
        hits = [i for i, v in enumerate(any_valid.to_list()) if v]
        if not hits:
            return None
        pos = hits[0] if first else hits[-1]
        labels = self._frame().index
        return labels[pos] if isinstance(labels, Series) else pos

    def select_dtypes(
        self, include: Optional[Sequence[object]] = None, exclude: Optional[Sequence[object]] = None
    ) -> "DataFrame":
        """Columns by dtype: ``include`` / ``exclude`` take :class:`DType`
        members, dtype names or the words ``"number"`` / ``"string"``."""

        def group(spec: object) -> set:
            if isinstance(spec, str):
                if spec in ("number", "numeric"):
                    return set(_NUMERIC) - {DType.BOOL}
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

    def memory_usage(self) -> "DataFrame":
        """One row: the bytes each column's Arrow buffers hold."""
        from .dataframe import DataFrame

        return DataFrame({c: [self._col(c).to_arrow().nbytes] for c in self.columns})

    def isin(
        self, values: "Union[Sequence[object], Series, Mapping[str, Sequence[object]]]"
    ) -> "DataFrame":
        """A Bool frame: each cell's membership in ``values`` (a list or
        Series for every column, or ``{column: values}`` for some, the rest
        false), pandas ``DataFrame.isin``."""
        out: Dict[str, Series] = {}
        for name in self.columns:
            s = self._col(name)
            chosen: Optional[Sequence[object]]
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

    def combine(self, other: "DataFrame", func: Callable[[Series, Series], Series]) -> "DataFrame":
        """``func(this column, other's same-named column)`` for every column
        both hold (positional), pandas ``DataFrame.combine``."""
        out: Dict[str, Series] = {}
        for name in self.columns:
            if name in other:
                out[name] = func(self._col(name), Series(other._native[name]))
        return self._rebuild(out, keep_others=False)

    def compare(self, other: "DataFrame") -> "DataFrame":
        """The cells that differ (pandas ``DataFrame.compare``, long form):
        one row per differing cell with ``row``, ``column``, ``self`` and
        ``other`` (the two values as strings; a null on one side is a
        difference), by column then row; :meth:`Series.compare` per column."""
        from .dataframe import DataFrame

        if self.columns != other.columns:
            raise ValueError("compare: the frames must hold the same columns")
        rows: List[int] = []
        cols: List[str] = []
        mine: List[Optional[str]] = []
        theirs: List[Optional[str]] = []
        for name in self.columns:
            diff = self._col(name).compare(Series(other._native[name])).to_dict()
            for i, va, vb in zip(diff["index"], diff["self"], diff["other"]):
                assert isinstance(i, int)
                rows.append(i)
                cols.append(name)
                mine.append(None if va is None else str(va))
                theirs.append(None if vb is None else str(vb))
        pa = _require_pyarrow()
        return DataFrame(
            {
                "row": pa.array(rows, pa.int64()),
                "column": pa.array(cols, pa.string()),
                "self": pa.array(mine, pa.string()),
                "other": pa.array(theirs, pa.string()),
            }
        )

    def corrwith(self, other: "DataFrame") -> "DataFrame":
        """One row: each numeric column's Pearson correlation with ``other``'s
        same-named column (positional), pandas ``DataFrame.corrwith``."""
        from .dataframe import DataFrame

        out: Dict[str, List[float]] = {}
        for name in self.columns:
            s = self._col(name)
            if name in other and _is_numeric(s):
                out[name] = [s.corr(Series(other._native[name]))]
        return DataFrame(out)

    def info(self) -> str:
        """A text summary, pandas ``DataFrame.info``: the row count, each
        column's type, present count and bytes; returned, not printed."""
        n = len(self._frame())
        lines = [f"DataFrame: {n} rows, {len(self.columns)} columns"]
        total = 0
        for name in self.columns:
            s = self._col(name)
            nbytes = int(s.to_arrow().nbytes)
            total += nbytes
            lines.append(
                f"  {name}: {s.dtype.name.lower()}, {n - s.null_count} present, {nbytes} bytes"
            )
        lines.append(f"memory: {total} bytes")
        return "\n".join(lines)

    def rename_axis(self, name: str) -> "DataFrame":
        """The single index column renamed to ``name`` (pandas
        ``rename_axis``); needs ``set_index`` first."""
        from .dataframe import _single_index

        current = _single_index(self._index, "rename_axis")
        out = self._frame().rename({current: name})
        out._index = [name]
        return out

    def kurtosis(self) -> "DataFrame":
        return self._frame().kurt()

    def swapaxes(self, axis1: object = 0, axis2: object = 1) -> "DataFrame":
        """pandas ``swapaxes``: the transpose (a frame has the two axes)."""
        return self.transpose()

    def combine_first(self, other: "DataFrame") -> "DataFrame":
        """This frame with nulls filled from ``other``'s same-named columns
        (positional)."""
        out: Dict[str, Series] = {}
        for name in self.columns:
            if name in other:
                s = self._col(name)
                out[name] = s.where(s.notna(), Series(other._native[name]))
        return self._rebuild(out)

    def update(self, other: "DataFrame") -> None:
        """Overwrite this handle's values with ``other``'s present values in
        the same-named columns (positional; the handle is rebound)."""
        out: Dict[str, Series] = {}
        for name in self.columns:
            if name in other:
                o = Series(other._native[name])
                out[name] = o.where(o.notna(), self._col(name))
        self._native = self._rebuild(out)._native

    def dot(self, other: Union[Series, "DataFrame"]) -> Union[Series, "DataFrame"]:
        """Matrix product with a Series (one value per column) or a frame
        (each column of ``other`` against each of ours)."""
        from .dataframe import DataFrame

        names = [c for c in self.columns if _is_numeric(self._col(c))]
        if isinstance(other, Series):
            return Series.from_list([self._col(c).dot(other) for c in names])
        return DataFrame(
            {o: [self._col(c).dot(Series(other._native[o])) for c in names] for o in other.columns}
        )

    @classmethod
    def from_records(
        cls,
        records: Sequence[Union[Dict[str, object], Sequence[object]]],
        columns: Optional[Sequence[str]] = None,
    ) -> "DataFrame":
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
    def _time_column(self, on: Optional[str], op: str) -> Tuple[str, Series]:
        from .dataframe import _single_index

        name = on if on is not None else _single_index(self._index, op)
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
        from .series import _DtAccessor

        dt = _DtAccessor(s)
        return dt.hour * 3600 + dt.minute * 60 + dt.second

    @staticmethod
    def _unit_of(s: Series) -> str:
        from .series import _DtAccessor

        return _DtAccessor(s)._unit

    def tz_localize(self, tz: Optional[str], on: Optional[str] = None) -> "DataFrame":
        """The index column (or ``on``) marked as zone ``tz`` (pandas
        ``DataFrame.tz_localize``; UTC only, see ``Series.dt.tz_localize``)."""
        from .series import _DtAccessor

        name, s = self._time_column(on, "tz_localize")
        return self._frame().with_column(name, _DtAccessor(s).tz_localize(tz))

    def tz_convert(self, tz: Optional[str], on: Optional[str] = None) -> "DataFrame":
        """The index column (or ``on``) shown in zone ``tz`` (pandas
        ``DataFrame.tz_convert``); the instants stay."""
        from .series import _DtAccessor

        name, s = self._time_column(on, "tz_convert")
        return self._frame().with_column(name, _DtAccessor(s).tz_convert(tz))

    def at_time(self, time: str, on: Optional[str] = None) -> "DataFrame":
        """The rows whose time of day is exactly ``time`` (``"HH:MM[:SS]"``),
        by the index column or ``on`` (pandas ``at_time``)."""
        _, s = self._time_column(on, "at_time")
        return self._frame().filter(self._seconds_of_day(s).eq(self._time_of_day(time)))

    def between_time(
        self,
        start_time: str,
        end_time: str,
        inclusive: str = "both",
        on: Optional[str] = None,
    ) -> "DataFrame":
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

    def first(self, offset: Union[int, str], on: Optional[str] = None) -> "DataFrame":
        """The rows within ``offset`` (``"3s"``, ``"1min"``, or a count of the
        column's units) of the earliest time (pandas ``first``); ``last``
        takes the latest."""
        from .indexing import rule_to_units

        _, s = self._time_column(on, "first")
        span = rule_to_units(offset, self._unit_of(s))
        ticks = s.astype("int64")
        return self._frame().filter(ticks < int(ticks.min()) + span)

    def last(self, offset: Union[int, str], on: Optional[str] = None) -> "DataFrame":
        from .indexing import rule_to_units

        _, s = self._time_column(on, "last")
        span = rule_to_units(offset, self._unit_of(s))
        ticks = s.astype("int64")
        return self._frame().filter(ticks > int(ticks.max()) - span)

    def asfreq(
        self,
        freq: Union[int, str],
        method: Optional[str] = None,
        on: Optional[str] = None,
        fill_value: Optional[Union[int, float]] = None,
    ) -> "DataFrame":
        """The frame on a regular grid of ``freq`` from the earliest to the
        latest time: each grid instant takes the row at exactly that time,
        else nulls, filled forward / backward with ``method="ffill"`` /
        ``"bfill"``, or with ``fill_value`` (pandas ``asfreq``). The time
        column must hold each instant once."""
        from .dataframe import DataFrame
        from .indexing import rule_to_units

        name, s = self._time_column(on, "asfreq")
        frame = self._frame()
        if len(frame) == 0:
            return frame
        if s.nunique() != len(frame) - s.null_count:
            raise ValueError("asfreq: the time column holds an instant more than once")
        every = rule_to_units(freq, self._unit_of(s))
        ticks = s.astype("int64")
        lo, hi = int(ticks.min()), int(ticks.max())
        pa = _require_pyarrow()
        grid = DataFrame({name: pa.array(range(lo, hi + 1, every), type=s.to_arrow().type)})
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
                if _is_numeric(self._col_of(out, c)):
                    out = out.with_column(c, self._col_of(out, c).fillna(fill_value))
        return frame._like(out.select(*frame.columns)._native)

    @staticmethod
    def _col_of(frame: "DataFrame", name: str) -> Series:
        return Series(frame._native[name])

    # -- exports through pandas / pyarrow -----------------------------------------
    def to_csv(
        self, path: Optional[str] = None, index: bool = False, **kwargs: Any
    ) -> Optional[str]:
        return self._frame().to_pandas().to_csv(path, index=index, **kwargs)

    def to_json(self, path: Optional[str] = None, **kwargs: Any) -> Optional[str]:
        return self._frame().to_pandas().to_json(path, **kwargs)

    def to_string(self, **kwargs: Any) -> str:
        return self._frame().to_pandas().to_string(index=False, **kwargs)

    def to_markdown(self, **kwargs: Any) -> str:
        return self._frame().to_pandas().to_markdown(index=False, **kwargs)

    def to_html(self, **kwargs: Any) -> str:
        return self._frame().to_pandas().to_html(index=False, **kwargs)

    def to_records(self) -> List[tuple]:
        return list(self.itertuples(index=False, name=None))

    def to_parquet(self, path: str, **kwargs: Any) -> None:
        import pyarrow.parquet as pq  # ty: ignore[unresolved-import]

        pq.write_table(self._frame().to_arrow(), path, **kwargs)

    def to_feather(self, path: str) -> None:
        import pyarrow.feather as feather  # ty: ignore[unresolved-import]

        feather.write_feather(self._frame().to_arrow(), path)


def _replace_str(s: Series, old: str, new: str) -> Series:
    """Whole-value replacement on a String column: ``old`` -> ``new`` where
    the value equals ``old``."""
    hit = s.str_eq(old)
    filled = Series.from_list([new] * len(s))
    return s.mask(hit, filled)


class _FrameWindow:
    """``df.rolling(n)`` / ``expanding()`` / ``ewm(alpha)``: the pandas window
    object over a frame, each reduction the Series window's method per numeric
    column (the other columns drop, pandas ``numeric_only``)."""

    __slots__ = ("_frame", "_window")

    def __init__(self, frame: "_FramePandasMixin", window: Callable[[Series], object]) -> None:
        self._frame = frame
        self._window = window

    def _reduce(self, name: str, *args: object) -> "DataFrame":
        return self._frame._map_columns(
            lambda s: getattr(self._window(s), name)(*args), keep_others=False
        )

    def sum(self) -> "DataFrame":
        return self._reduce("sum")

    def mean(self) -> "DataFrame":
        return self._reduce("mean")

    def min(self) -> "DataFrame":
        return self._reduce("min")

    def max(self) -> "DataFrame":
        return self._reduce("max")

    def count(self) -> "DataFrame":
        return self._reduce("count")

    def var(self) -> "DataFrame":
        return self._reduce("var")

    def std(self) -> "DataFrame":
        return self._reduce("std")

    def median(self) -> "DataFrame":
        return self._reduce("median")

    def quantile(self, q: float) -> "DataFrame":
        return self._reduce("quantile", q)
