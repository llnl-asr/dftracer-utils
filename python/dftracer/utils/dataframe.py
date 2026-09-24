"""The ``DataFrame`` Python wrapper.

``DataFrame`` wraps the native ``_DataFrame`` handle: native frame ops plus
Python-side Arrow/pandas/polars conversion and pickling. Every relational and
reshape primitive - ``join``/``asof``/``interval``, ``window``/``gap_fill``,
``unnest``/``explode``, ``melt``/``pivot``, ``concat``/``union``, ``distinct``,
``sample``, ``top_k``, ``sort`` - is a native method that takes and returns
``DataFrame``s, running the SIMD kernels on the native columns; Arrow is crossed
only at the edge (``to_arrow``/``from_arrow``), never as the operating boundary.
See :mod:`dftracer.utils.series` for the column counterpart.
"""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    List,
    Literal,
    Mapping,
    Optional,
    Sequence,
    Tuple,
    Union,
    cast,
)

from . import dftracer_utils_ext as _ext
from ._pandas_frame import _FramePandasMixin
from ._polars_frame import _FramePolarsMixin
from .enums import DType
from .series import (
    Series,
    _indices,
    _register,
    _require_pyarrow,
    _to_pandas,
    _unwrap,
    _wrap,
    _Wrapper,
)

if TYPE_CHECKING:
    import numpy as np  # ty: ignore[unresolved-import]
    import pandas as pd  # ty: ignore[unresolved-import]
    import polars as pl  # ty: ignore[unresolved-import]
    import pyarrow as pa  # ty: ignore[unresolved-import]

    from .columnar import Agg, ColumnExpr, GroupBy
    from .indexing import At, ILoc, Loc, Resampler
    from .lazyframe import LazyFrame

# Matches WINDOW_UNBOUNDED (int64 max): a frame bound of None means that side of
# the ROWS frame runs to the partition edge.
_WINDOW_UNBOUNDED = (1 << 63) - 1

_WINDOW_NULLARY = ("row_number", "rank", "dense_rank")
_WINDOW_VALUE_ONLY = (
    "running_sum",
    "running_min",
    "running_max",
    "running_count",
    "running_prod",
    "delta",
    "first_value",
    "last_value",
    "fill_forward",
)
_WINDOW_FRAME = ("frame_sum", "frame_min", "frame_max", "frame_count", "frame_mean")

# Finite value sets for the viewer builder args (kept as reusable aliases so the
# wrappers, the dask plan, and the stub share one definition).
PhaseArg = Literal["events", "counters", "aggregated", "metadata", "any"]
TimeUnitArg = Literal["ns", "us", "ms", "sec", "s"]

# One window spec per appended output column; the func literal selects the shape.
RankSpec = Tuple[Literal["row_number", "rank", "dense_rank"], str]
OffsetSpec = Tuple[Literal["lag", "lead"], str, int, str]
RunSpec = Tuple[
    Literal["running_sum", "running_min", "running_max", "running_count", "running_prod"], str, str
]
DeltaSpec = Tuple[Literal["delta"], str, str]
RateSpec = Tuple[Literal["rate"], str, str, str]
RateCounterSpec = Tuple[Literal["rate"], str, str, str, bool]
SessSpec = Tuple[Literal["sessionize"], str, float, str]
FrameSpec = Tuple[
    Literal["frame_sum", "frame_min", "frame_max", "frame_count", "frame_mean"],
    str,
    Optional[int],
    Optional[int],
    str,
]
NtileSpec = Tuple[Literal["ntile"], int, str]
FrameMinSpec = Tuple[
    Literal["frame_sum", "frame_min", "frame_max", "frame_count", "frame_mean"],
    str,
    Optional[int],
    Optional[int],
    str,
    int,
]
PosSpec = Tuple[Literal["first_value", "last_value", "fill_forward"], str, str]
NthSpec = Tuple[Literal["nth_value"], str, int, str]
WindowSpec = Union[
    RankSpec,
    OffsetSpec,
    RunSpec,
    DeltaSpec,
    RateSpec,
    RateCounterSpec,
    SessSpec,
    FrameSpec,
    FrameMinSpec,
    NtileSpec,
    PosSpec,
    NthSpec,
]


JoinHow = Literal["inner", "left", "right", "outer", "full", "semi", "anti", "cross"]
PivotAgg = Literal[
    "first", "last", "sum", "min", "max", "mean", "count", "var", "std", "skew", "kurt"
]


def _names(cols: Optional[Union[str, Sequence[str]]]) -> List[str]:
    if cols is None:
        return []
    if isinstance(cols, str):
        return [cols]
    return list(cols)


def _window_bound(v: Optional[int]) -> int:
    return _WINDOW_UNBOUNDED if v is None else int(v)


def _window_arity(spec: Sequence[object], n: int) -> None:
    if len(spec) != n:
        raise ValueError(
            f"window: {spec[0]!r} spec expects {n} elements, got {len(spec)}: {spec!r}"
        )


# A spec is a heterogeneous positional tuple (str, col names, ints, None) read
# by index, so its elements are genuinely Any; the output 9-tuple is mixed too.
def _single_index(index: Optional[List[str]], op: str) -> str:
    """The one index column an op reads its time from."""
    if index is None:
        raise ValueError(f"{op}: set_index(time) first, or pass on=")
    if len(index) != 1:
        raise ValueError(f"{op}: the index has {len(index)} columns; pass on=")
    return index[0]


def _nulls_first_keys(
    names: List[str], descending: Union[bool, Sequence[bool]]
) -> Tuple[List[str], List[bool]]:
    """The sort keys and directions that put null keys first: a hidden 0 / 1
    presence column ahead of each key, ascending."""
    flags = [descending] * len(names) if isinstance(descending, bool) else list(descending)
    if len(flags) == 1:
        flags = flags * len(names)
    if len(flags) != len(names):
        raise ValueError("sort: one direction per key, or a single flag")
    keys: List[str] = []
    dirs: List[bool] = []
    for i, (name, desc) in enumerate(zip(names, flags)):
        keys += [f"__dftu_present_{i}__", name]
        dirs += [False, bool(desc)]
    return keys, dirs


def _norm_window_spec(spec: Sequence[Any]) -> Tuple[object, ...]:
    # Normalize to the fixed 9-tuple the native kernel reads: (func, value|None,
    # offset, name, time|None, threshold, counter, frame_pre, frame_post).
    if not isinstance(spec, (tuple, list)) or not spec:
        raise ValueError(f"window: bad spec {spec!r}")
    func = spec[0]
    value: Optional[str] = None
    offset = 0
    name: Optional[str] = None
    time: Optional[str] = None
    threshold = 0.0
    counter = False
    pre = 0
    post = 0
    if func in _WINDOW_NULLARY:
        _window_arity(spec, 2)
        name = spec[1]
    elif func in ("lag", "lead"):
        _window_arity(spec, 4)
        value, offset, name = spec[1], int(spec[2]), spec[3]
    elif func in _WINDOW_VALUE_ONLY:
        _window_arity(spec, 3)
        value, name = spec[1], spec[2]
    elif func == "rate":
        if len(spec) not in (4, 5):
            raise ValueError(f"window: 'rate' spec expects 4 or 5 elements: {spec!r}")
        value, time, name = spec[1], spec[2], spec[3]
        counter = bool(spec[4]) if len(spec) == 5 else False
    elif func == "sessionize":
        _window_arity(spec, 4)
        time, threshold, name = spec[1], float(spec[2]), spec[3]
    elif func in _WINDOW_FRAME:
        if len(spec) not in (5, 6):
            raise ValueError(f"window: {func!r} spec expects 5 or 6 elements: {spec!r}")
        value, pre, post, name = spec[1], _window_bound(spec[2]), _window_bound(spec[3]), spec[4]
        offset = int(spec[5]) if len(spec) == 6 else 0
    elif func == "ntile":
        _window_arity(spec, 3)
        offset, name = int(spec[1]), spec[2]
    elif func == "nth_value":
        _window_arity(spec, 4)
        value, offset, name = spec[1], int(spec[2]), spec[3]
    else:
        raise ValueError(f"window: unknown function {func!r}")
    if not isinstance(name, str):
        raise ValueError(f"window: output name must be a str: {spec!r}")
    return (func, value, offset, name, time, threshold, counter, pre, post)


class DataFrame(_FramePandasMixin, _FramePolarsMixin, _Wrapper["_ext._DataFrame"]):
    """A named set of columns: native frame ops plus Arrow conversion.

    ``DataFrame({"a": [1, 2]})`` builds one from a ``{name: array-like}``
    mapping, the pandas / polars constructor; ``DataFrame(native)`` wraps a
    native handle."""

    __slots__ = ("_index",)

    def __init__(self, data: "Union[_ext._DataFrame, Mapping[str, object]]") -> None:
        if isinstance(data, Mapping):
            data = _unwrap(DataFrame.from_dict(data))
        super().__init__(data)
        self._index: Optional[List[str]] = None

    def _like(self, native: object) -> Any:
        """Wrap a native result, carrying this frame's index column names when
        the result still holds every one of them."""
        out = _wrap(native)
        if isinstance(out, DataFrame) and self._index is not None:
            if all(name in out for name in self._index):
                out._index = self._index
        return out

    # -- the index: named columns, or the row position -------------------------
    @property
    def index(self) -> "Union[Series, DataFrame]":
        """The index column set by :meth:`set_index` as a Series (several
        columns: a frame of them), else the row positions (pandas' default
        ``RangeIndex``)."""
        if self._index is None:
            return Series.from_list(list(range(len(self))))
        if len(self._index) == 1:
            return Series(self._native[self._index[0]])
        return _wrap(self._native.select(*self._index))

    def index_levels(self) -> List[Series]:
        """The index columns, one Series per level; the row positions when no
        index is set."""
        if self._index is None:
            return [Series.from_list(list(range(len(self))))]
        return [Series(self._native[name]) for name in self._index]

    def set_index(self, names: "Union[str, Sequence[str]]") -> "DataFrame":
        """Name the column(s) ``loc`` / ``at`` / ``resample`` read labels
        from. The columns stay in place; nothing is copied (the polars model
        of an index: a column). Several names are the pandas ``MultiIndex``
        shape: ``loc[(a, b)]`` matches every level, ``loc[a]`` the first."""
        keys = [names] if isinstance(names, str) else list(names)
        if not keys:
            raise ValueError("set_index: at least one column")
        for name in keys:
            if name not in self:
                raise KeyError(f"set_index: no column named {name!r}")
        out = DataFrame(self._native)
        out._index = keys
        return out

    def reset_index(self, drop: bool = False) -> "DataFrame":
        """Forget the index columns (they stay as plain columns unless
        ``drop``); with no index set, prepend the row positions as ``index``,
        as pandas."""
        if self._index is None:
            return self if drop else self._like(self._native.with_row_index("index"))
        if drop:
            keep = [c for c in self.columns if c not in self._index]
            return _wrap(self._native.select(*keep))
        return DataFrame(self._native)

    @property
    def iloc(self) -> "ILoc":
        """Rows and columns by position: ``df.iloc[3]``, ``df.iloc[2:5]``,
        ``df.iloc[[0, 4], "c"]``; assignment rebuilds the touched columns."""
        from .indexing import ILoc

        return ILoc(self)

    @property
    def loc(self) -> "Loc":
        """Rows by index label (a value, an inclusive slice, a list, a Bool
        mask or an expression), columns by name; assignment rebuilds the
        touched columns (``df.loc[df["a"] > 1, "c"] = 0``)."""
        from .indexing import Loc

        return Loc(self)

    @property
    def at(self) -> "At":
        """One value by index label and column name."""
        from .indexing import At

        return At(self, by_position=False)

    @property
    def iat(self) -> "At":
        """One value by row position and column position."""
        from .indexing import At

        return At(self, by_position=True)

    def __setitem__(self, key: object, value: object) -> None:
        """``df["c"] = series | scalar`` adds or replaces a column;
        ``df[mask] = value`` writes every column where the mask holds. The
        frame's columns are immutable: the handle is rebound to a new frame
        that shares every untouched column."""
        from .indexing import Rows, assign_rows, rows_by_label

        if isinstance(key, str):
            assign_rows(self, Rows.all(), [key], value)
        elif isinstance(key, (list, tuple)):
            assign_rows(self, Rows.all(), [str(k) for k in key], value)
        elif isinstance(key, Series):
            assign_rows(self, rows_by_label(self.index_levels(), key), self.columns, value)
        else:
            raise TypeError("assign to a column name, a list of names or a Bool mask")

    def resample(
        self, rule: "Union[int, str]", on: Optional[str] = None, unit: str = "us"
    ) -> "Resampler":
        """Tumbling time windows of ``rule`` (``"5s"``, ``"100ms"``, ``"1min"``,
        or an int of the column's own units) over the index column or ``on``,
        with the aggregate family (``.sum()``, ``.agg(...)``, ``.size()``);
        ``unit`` is the time column's unit (dftracer timestamps are ``us``).
        Each call is one ``group_by_dynamic`` over the time-sorted frame."""
        from .indexing import Resampler, rule_to_units

        time = on if on is not None else _single_index(self._index, "resample")
        if time not in self:
            raise KeyError(f"resample: no column named {time!r}")
        return Resampler(self, time, rule_to_units(rule, unit))

    @classmethod
    def from_arrow(cls, table: "pa.Table") -> "DataFrame":
        """Import a ``pyarrow.Table`` (or any ``__arrow_c_stream__`` provider),
        zero-copy via the Arrow C Data Interface."""
        return _dataframe_from_arrow(table)

    @classmethod
    def from_pandas(cls, df: "pd.DataFrame") -> "DataFrame":
        """Import a ``pandas.DataFrame`` (via pyarrow)."""
        pa = _require_pyarrow()
        return _dataframe_from_arrow(pa.Table.from_pandas(df))

    @classmethod
    def from_polars(cls, df: "pl.DataFrame") -> "DataFrame":
        """Import a ``polars.DataFrame`` (zero-copy via its Arrow buffers)."""
        return _dataframe_from_arrow(df.to_arrow())

    @classmethod
    def from_parquet(cls, path: str, columns: Optional[Sequence[str]] = None) -> "DataFrame":
        """Read a Parquet file into a frame (needs pyarrow)."""
        _require_pyarrow()
        import pyarrow.parquet as pq  # ty: ignore[unresolved-import]

        return _dataframe_from_arrow(pq.read_table(path, columns=columns))

    @classmethod
    def from_dict(cls, mapping: Mapping[str, object]) -> "DataFrame":
        """Import a ``{name: array-like}`` mapping (via pyarrow)."""
        pa = _require_pyarrow()
        return _dataframe_from_arrow(pa.table(mapping))

    @classmethod
    def from_numpy(
        cls,
        arr: "Union[np.ndarray, Dict[str, np.ndarray]]",
        columns: Sequence[str],
    ) -> "DataFrame":
        """Import NumPy columns into a frame.

        ``arr`` is either a 2-D array (one column per column index) or a
        ``{name: 1-D array}`` mapping; ``columns`` names the columns of a 2-D
        array (ignored for a mapping)."""
        pa = _require_pyarrow()
        if isinstance(arr, dict):
            data = {name: Series.from_numpy(col).to_arrow() for name, col in arr.items()}
        else:
            data = {name: Series.from_numpy(arr[:, i]).to_arrow() for i, name in enumerate(columns)}
        return _dataframe_from_arrow(pa.table(data))

    def to_arrow(self) -> "pa.Table":
        """This frame as a ``pyarrow.Table`` (zero-copy via the C Data Interface
        stream)."""
        return _require_pyarrow().table(self._native)

    def to_ipc(self) -> bytes:
        """This frame serialized as an Arrow IPC stream (schema + one record
        batch + EOS) - bytes any Arrow IPC reader opens, no pyarrow needed."""
        return self._native.to_ipc()

    def to_pandas(self, *, arrow: bool = False) -> "pd.DataFrame":
        """This frame as a pandas DataFrame. By default the columns are NumPy
        dtypes, which copies (pyarrow packs them into blocks); ``arrow=True``
        keeps them Arrow-backed (``int64[pyarrow]``, ``string[pyarrow]``, ...),
        sharing this frame's buffers with no copy."""
        return _to_pandas(self.to_arrow(), arrow)

    def to_polars(self) -> "pl.DataFrame":
        """This frame as a polars DataFrame."""
        try:
            import polars as pl  # ty: ignore[unresolved-import]
        except ImportError:
            raise ImportError(
                "polars is required for to_polars(). Install with: pip install polars"
            ) from None
        table = self.to_arrow()
        if table.num_rows == 0:
            return pl.DataFrame()
        return pl.DataFrame(pl.from_arrow(table))

    def __getitem__(self, key: "Union[str, Sequence[str], Series]") -> "Union[Series, DataFrame]":
        """``df["a"]`` is a column; ``df[["a", "b"]]`` a projection; ``df[mask]``
        with a Bool Series the matching rows (the pandas spellings)."""
        if isinstance(key, str):
            return Series(self._native[key])
        if isinstance(key, Series):
            return self._like(self._native.filter(_unwrap(key)))
        return self._like(self._native.select(*[str(k) for k in key]))

    def __contains__(self, name: str) -> bool:
        return name in self._native

    def __len__(self) -> int:
        return self._native.num_rows

    # -- pandas / polars spellings ---------------------------------------------
    @property
    def columns(self) -> List[str]:
        return list(self._native.column_names)

    @property
    def shape(self) -> Tuple[int, int]:
        return (self._native.num_rows, self._native.num_columns)

    @property
    def height(self) -> int:
        return self._native.num_rows

    @property
    def width(self) -> int:
        return self._native.num_columns

    @property
    def dtypes(self) -> List[DType]:
        return [self[name].dtype for name in self.columns]

    @property
    def schema(self) -> "Dict[str, DType]":
        return {name: self[name].dtype for name in self.columns}

    def limit(self, n: int = 5) -> "DataFrame":
        return self._like(self._native.head(_unwrap(n)))

    def drop(
        self, *names: "Union[str, Sequence[str]]", columns: Union[str, Sequence[str], None] = None
    ) -> "DataFrame":
        """Every column except ``names`` (``columns=`` is the pandas spelling)."""
        dropped = set(_names(columns))
        for n in names:
            dropped.update(_names(n))
        missing = [n for n in dropped if n not in self]
        if missing:
            raise KeyError(f"drop: no column named {missing[0]!r}")
        return self._like(self._native.select(*[c for c in self.columns if c not in dropped]))

    def assign(self, **columns: "Union[Series, ColumnExpr]") -> "DataFrame":
        """Add or replace columns from Series or column expressions (pandas
        ``assign``; polars ``with_columns``)."""
        out = self
        for name, value in columns.items():
            col = value if isinstance(value, Series) else value.apply(out)
            out = _wrap(out._native.with_column(name, _unwrap(col)))
        return out

    def with_columns(self, *named: object, **columns: "Union[Series, ColumnExpr]") -> "DataFrame":
        """:meth:`assign`, also taking named expressions positionally
        (``with_columns((col("a") * 2).alias("b"))``, the polars spelling)."""
        from .columnar import Named

        out = self
        for item in named:
            if not isinstance(item, Named):
                raise TypeError("with_columns: a positional item must be expr.alias(name)")
            out = out.assign(**{item.name: item.expr})
        return out.assign(**columns)

    def cast(self, dtypes: "Mapping[str, Union[str, int, DType]]") -> "DataFrame":
        """Cast the named columns (polars ``cast``; pandas ``astype``)."""
        out = self
        for name, dtype in dtypes.items():
            casted = Series(out._native[name]).astype(dtype)
            out = _wrap(out._native.with_column(name, _unwrap(casted)))
        return out

    def astype(self, dtypes: "Mapping[str, Union[str, int, DType]]") -> "DataFrame":
        return self.cast(dtypes)

    def dropna(self) -> "DataFrame":
        return self._like(self._native.drop_nulls())

    def fillna(self, value: Union[int, float]) -> "DataFrame":
        return self._like(self._native.fill_null(_unwrap(value)))

    def duplicated(self) -> "Series":
        return self._like(self._native.is_duplicated())

    def gather(self, indices: "Union[Series, Sequence[int]]") -> "DataFrame":
        return self.take(indices)

    def groupby(
        self,
        by: "Union[str, Series, Sequence[Union[str, Series]]]",
        *aggs: "Union[str, Agg]",
        dropna: bool = True,
    ) -> "Any":
        """The pandas spelling of :meth:`group_by`."""
        return self.group_by(by, *aggs, dropna=dropna)

    def nlargest(self, n: int, columns: str) -> "DataFrame":
        return self._like(self._native.topk(columns, n, True))

    def nsmallest(self, n: int, columns: str) -> "DataFrame":
        return self._like(self._native.topk(columns, n, False))

    def pivot_table(
        self,
        values: str,
        index: str,
        columns: str,
        aggfunc: "PivotAgg" = "first",
    ) -> "DataFrame":
        return self.pivot(index, columns, values, aggfunc)

    def to_dict(self) -> "Dict[str, List[object]]":
        return {name: self[name].to_list() for name in self.columns}

    # Opaque Arrow C Data Interface capsule; Python has no capsule type.
    def __arrow_c_stream__(self, requested_schema: Optional[object] = None) -> object:
        return self._native.__arrow_c_stream__(requested_schema)

    def __reduce__(self) -> "Tuple[Callable[[object], DataFrame], Tuple[object, ...]]":
        return (_dataframe_from_arrow, (self.to_arrow(),))

    def apply(
        self, func: "Union[ColumnExpr, Callable[..., object]]", axis: int = 0
    ) -> "Union[Series, DataFrame]":
        """With a column expression: evaluate it against this frame and return
        the resulting :class:`~dftracer.utils.Series` (``df.apply(F.a / F.b)``
        equals ``(F.a / F.b).apply(df)``).

        With a callable, as pandas ``apply``: ``axis=0`` calls it once per
        column with that column's Series (results that are all Series come
        back as a frame under the same names; scalar results as a one-row
        frame, since there is no index to hold them in a Series); ``axis=1``
        calls it per row. The row form is engine-first: the function is
        traced once with a symbolic row whose fields are column expressions
        (``row["a"] + row.b``), and when that yields an expression the whole
        thing runs fused in the engine. Otherwise it runs in Python per row,
        with a warning naming why. See ``_apply``."""
        from ._apply import apply_rows
        from .columnar import ColumnExpr

        if isinstance(func, ColumnExpr):
            return func.apply(self)
        if not callable(func):
            raise TypeError(
                "DataFrame.apply expects a column expression (col()/F./lit()) or a "
                f"callable, got {type(func).__name__}"
            )
        if axis == 1:
            return apply_rows(self, func, "DataFrame.apply(axis=1)")
        if axis != 0:
            raise ValueError("axis must be 0 (per column) or 1 (per row)")
        results = {name: func(Series(self._native[name])) for name in self.columns}
        if all(isinstance(r, Series) for r in results.values()):
            out = self
            for name, r in results.items():
                out = out.with_column(name, cast(Series, r))
            return out.select(*self.columns)
        if any(isinstance(r, Series) for r in results.values()):
            raise TypeError(
                "apply(): the callable must return a Series for every column, or for none"
            )
        return DataFrame({name: [r] for name, r in results.items()})

    def partition_id(self, keys: Union[str, List[str]], n_parts: int) -> "Series":
        """Int32 column: the part in ``[0, n_parts)`` each row lands in under a
        stable hash of ``keys`` (equal keys share a part)."""
        return Series(self._native.partition_id(keys, n_parts))

    def hash_partition(self, keys: Union[str, List[str]], n_parts: int) -> "list[DataFrame]":
        """Hash-partition the rows into ``n_parts`` frames by
        :meth:`partition_id` (the shuffle primitive)."""
        return [DataFrame(p) for p in self._native.hash_partition(keys, n_parts)]

    def window(
        self,
        partition_by: Optional[Sequence[str]] = None,
        order_by: Optional[Sequence[str]] = None,
        specs: Optional[Sequence[WindowSpec]] = None,
    ) -> "DataFrame":
        """SQL window functions, PARTITION BY ``partition_by`` and ORDER BY
        ``order_by``.

        ``specs`` is a list of tuples, one per appended output column:

        - ``("row_number"|"rank"|"dense_rank", out)``
        - ``("lag"|"lead", value_col, offset, out)``
        - ``("running_sum"|"running_min"|"running_max"|"running_count"|"running_prod",
          value_col, out)`` (``running_prod`` is Float64)
        - ``("delta", value_col, out)``
        - ``("rate", value_col, time_col, out[, counter])``
        - ``("sessionize", time_col, threshold, out)``
        - ``("frame_sum"|"frame_min"|"frame_max"|"frame_count"|"frame_mean",
          value_col, preceding, following, out[, min_periods])`` (a bound of
          ``None`` is unbounded; the output is null while the frame holds fewer
          than ``min_periods`` present values)
        - ``("ntile", n, out)``
        - ``("first_value"|"last_value"|"fill_forward", value_col, out)``
          (``fill_forward`` is the nearest present value at or before the row)
        - ``("nth_value", value_col, k, out)``

        All input columns pass through, then one column per spec, in sorted
        (partition, order) row order."""
        norm = [_norm_window_spec(s) for s in (specs or [])]
        return self._like(self._native.window(_names(partition_by), _names(order_by), norm))

    def gap_fill(
        self,
        partition_by: Optional[Sequence[str]],
        time: str,
        bucket: int,
        values: Union[str, Sequence[str]],
        mode: Literal["none", "locf", "linear"] = "none",
        start: Optional[int] = None,
        end: Optional[int] = None,
    ) -> "DataFrame":
        """Materialize a regular time grid of width ``bucket``, PARTITION BY
        ``partition_by``, keyed on the integer ``time`` column.

        ``values`` (a name or list of names) are filled on generated rows per
        ``mode``: ``"none"`` leaves them null, ``"locf"`` carries the last real
        value forward, ``"linear"`` interpolates (emitting the value columns as
        double). ``start``/``end`` set an explicit grid range for every
        partition; pass both or neither."""
        if (start is None) != (end is None):
            raise ValueError("gap_fill: pass both start and end, or neither")
        return _wrap(
            self._native.gap_fill(
                _names(partition_by),
                time,
                int(bucket),
                _names(values),
                mode,
                start,
                end,
            )
        )

    def join(
        self,
        other: "DataFrame",
        on: Union[int, str, Sequence[str], None] = None,
        how: JoinHow = "inner",
        left_on: Union[str, Sequence[str], None] = None,
        right_on: Union[str, Sequence[str], None] = None,
        suffix: str = "_right",
    ) -> "DataFrame":
        """Hash join with ``other``.

        ``on`` names the key column(s) shared by both frames (a str, a list of
        names, or an int count of the leading columns); ``left_on`` /
        ``right_on`` name each side's keys instead when they differ. A null key
        never matches. ``how`` is ``"inner"``, ``"left"``, ``"right"``,
        ``"outer"`` (alias ``"full"``), ``"semi"``, ``"anti"`` or ``"cross"``
        (no keys). Output is this frame's columns, then ``other``'s except a
        key sharing its left key's name; any other colliding name gets
        ``suffix``. Matched rows keep this frame's order; right / outer append
        the unmatched right rows. Semi / anti emit this frame's columns only.
        See :meth:`merge` for the pandas argument order."""
        if isinstance(on, bool):
            raise TypeError("join: 'on' must be a key name/list or an int count")
        if isinstance(on, int):
            names = list(self._native.column_names)
            if on < 1 or on > len(names):
                raise ValueError(
                    f"join: 'on' count {on} is out of range for a {len(names)}-column frame"
                )
            on = names[:on]
        return _wrap(
            self._native.join(
                _unwrap(other),
                on=_unwrap(on),
                how=how,
                left_on=_unwrap(left_on),
                right_on=_unwrap(right_on),
                suffix=suffix,
            )
        )

    def merge(
        self,
        right: "DataFrame",
        how: JoinHow = "inner",
        on: Union[str, Sequence[str], None] = None,
        left_on: Union[str, Sequence[str], None] = None,
        right_on: Union[str, Sequence[str], None] = None,
        suffixes: Tuple[str, str] = ("_x", "_y"),
    ) -> "DataFrame":
        """pandas ``merge``: :meth:`join` with ``right`` first, then ``how``.
        With no key given, joins on the columns both frames share. A non-key
        column present on both sides is suffixed on BOTH sides with
        ``suffixes``, as pandas does."""
        if on is None and left_on is None and right_on is None:
            shared = [c for c in self.columns if c in right]
            if not shared:
                raise ValueError(
                    "merge(): no common columns to join on; pass on= or left_on=/right_on="
                )
            on = shared
        lkeys = _names(on) if on is not None else _names(left_on)
        rkeys = _names(on) if on is not None else _names(right_on)
        collide = [c for c in self.columns if c in right and c not in lkeys and c not in rkeys]
        left = self
        if collide:
            left = self.rename({c: c + suffixes[0] for c in collide})
            right = right.rename({c: c + suffixes[1] for c in collide})
        return left.join(
            right, on=on, how=how, left_on=left_on, right_on=right_on, suffix=suffixes[1]
        )

    def compare_agg(
        self, variant: "DataFrame", on: Union[int, str, Sequence[str]] = 1
    ) -> "DataFrame":
        """Compare two aggregation results: FULL-join on the shared leading key
        columns, then append ``delta_<m>``/``pct_<m>`` for each numeric
        ``l_``/``r_`` metric pair.

        ``on`` is an int count of the leading key columns both frames share, or
        the shared key column name(s). Output is [key columns, ``l_``/``r_`` per
        metric, ``delta_``/``pct_`` per metric]. Both frames must group and
        aggregate the same way."""
        if isinstance(on, bool):
            raise TypeError("compare_agg: 'on' must be a key name/list or an int count")
        if isinstance(on, int):
            names = list(self._native.column_names)
            if on < 1 or on > len(names):
                raise ValueError(
                    f"compare_agg: 'on' count {on} is out of range for a {len(names)}-column frame"
                )
            n_key = on
        else:
            keys = _names(on)
            if not keys:
                raise ValueError("compare_agg: 'on' must name at least one key column")
            n_key = len(keys)
        return self._like(self._native.compare_agg(_unwrap(variant), n_key))

    def asof(
        self,
        other: "DataFrame",
        on: str,
        by: Optional[Union[str, Sequence[str]]] = None,
        direction: Literal["backward", "forward", "nearest"] = "backward",
        tolerance: Optional[int] = None,
    ) -> "DataFrame":
        """Temporal (as-of) join: match each row to the nearest ``other`` row by
        the time column ``on``, within optional equi-key partition ``by``.

        ``direction`` is ``"backward"`` (largest ts <= self.ts), ``"forward"``
        (smallest ts >= self.ts), or ``"nearest"``. ``tolerance`` (if given)
        bounds the allowed time distance. Output is all left columns then the
        right value columns; unmatched left rows get null right values."""
        return self._like(self._native.asof(_unwrap(other), on, _names(by), direction, tolerance))

    def interval(
        self,
        other: "DataFrame",
        point: str,
        lo: str,
        hi: str,
        by: Optional[Union[str, Sequence[str]]] = None,
        outer: bool = False,
    ) -> "DataFrame":
        """Point-in-range join: match each row (the ``point`` column) to every
        ``other`` row whose closed span ``[lo, hi]`` contains it, within optional
        equi-key partition ``by``.

        One output row per (left, matching right); ``outer=True`` also emits an
        unmatched left row once with null right values. Output is all left
        columns then the right value columns."""
        return self._like(
            self._native.interval(_unwrap(other), point, lo, hi, _names(by), bool(outer))
        )

    def unnest(self, column: str, keep_empty: bool = False) -> "DataFrame":
        """UNNEST / EXPLODE the list-typed ``column`` into one row per element,
        repeating every other column.

        ``column`` names a ``list<utf8>``, ``list<int64>``, or
        ``list<struct<...>>`` column; a ``list<struct>`` flattens its fields into
        columns (named by the struct fields). Other columns pass through by
        value. An empty or null list drops the row unless ``keep_empty=True``
        (then one row with the exploded column(s) null). See :meth:`explode` for
        the single-list variant that always keeps the row."""
        return self._like(self._native.unnest(column, bool(keep_empty)))

    def top_k(self, name: str, k: int, largest: bool = True) -> "DataFrame":
        """The ``k`` best rows by column ``name`` (alias of the native
        :meth:`topk`)."""
        return self._like(self._native.topk(name, k, largest))

    def distinct(self, subset: "str | Sequence[str] | None" = None) -> "DataFrame":
        """Drop duplicate rows, keeping the first (alias of :meth:`unique`)."""
        return self._like(self._native.unique(subset))

    def sort_values(
        self,
        by: Union[str, Sequence[str]],
        ascending: Union[bool, Sequence[bool]] = True,
        na_position: Literal["first", "last"] = "last",
    ) -> "DataFrame":
        """Order rows by one column or several, pandas-style. ``by`` and
        ``ascending`` each accept a scalar or a sequence; a sequence
        ``ascending`` maps one flag per column in ``by`` (a single flag
        broadcasts to every key). ``na_position="first"`` puts the rows whose
        key is null before the rest (one key only)."""
        names = [by] if isinstance(by, str) else list(by)
        if isinstance(ascending, bool):
            descending: Union[bool, "list[bool]"] = not ascending
        else:
            descending = [not a for a in ascending]
        if na_position == "first":
            return self._sort_nulls_first(names, descending)
        if na_position != "last":
            raise ValueError("na_position must be 'first' or 'last'")
        if len(names) == 1 and isinstance(descending, bool):
            return self._like(self._native.sort_by(names[0], descending))
        return self._like(self._native.sort_by_multi(names, descending))

    def sort(
        self,
        by: Union[str, Sequence[str]],
        descending: bool = False,
        nulls_last: bool = True,
    ) -> "DataFrame":
        """Order rows by one column (``by`` a name) or lexicographically by
        several (``by`` a list); the fluent spelling over the native
        :meth:`sort_by` / :meth:`sort_by_multi`. See :meth:`sort_values` for
        the pandas-style ``ascending`` spelling (accepts a per-column list).
        ``nulls_last=False`` puts the null-keyed rows first (one key only)."""
        if not nulls_last:
            return self._sort_nulls_first([by] if isinstance(by, str) else list(by), descending)
        if isinstance(by, str):
            return self._like(self._native.sort_by(by, descending))
        return self._like(self._native.sort_by_multi(list(by), descending))

    def _sort_nulls_first(
        self, names: List[str], descending: Union[bool, "list[bool]"]
    ) -> "DataFrame":
        # The sort kernels place nulls last. Nulls first is a lexicographic
        # sort over (present(key), key) per key: the 0 / 1 presence column
        # sorts the null rows ahead, then the key orders the rest.
        keys, flags = _nulls_first_keys(names, descending)
        native = self._native
        for name, key in zip(keys[::2], names):
            native = native.with_column(
                name, _unwrap(Series(native[key]).is_not_null().astype("int64"))
            )
        return _wrap(native.sort_by_multi(keys, flags).select(*self.columns))

    def union(self, *others: "DataFrame") -> "DataFrame":
        """Vertically concatenate with ``others`` and drop duplicate rows (SQL
        UNION); use :meth:`concat` for UNION ALL (keep duplicates)."""
        cat = self._native.concat(*[_unwrap(o) for o in others])
        return _wrap(cat.unique())

    # -- native engine ops (SIMD/relational kernels, wrapped from the C extension) --
    def keys(self) -> List[str]:
        return self._native.keys()

    def filter(
        self,
        mask: "Union[Series, ColumnExpr, None]" = None,
        *,
        items: Optional[Sequence[str]] = None,
        like: Optional[str] = None,
        regex: Optional[str] = None,
    ) -> "DataFrame":
        """Rows or columns. With ``mask`` (a Bool Series, or a column expression
        evaluated against this frame): the rows where it is true. With the
        pandas keywords ``items`` / ``like`` / ``regex``: the columns whose
        name is listed / contains ``like`` / matches ``regex``."""
        keyed = [k for k in (items, like, regex) if k is not None]
        if len(keyed) > 1 or (keyed and mask is not None):
            raise TypeError("filter() takes a row mask, or one of items= / like= / regex=")
        if items is not None:
            return self._like(self._native.select(*[c for c in items if c in self]))
        if like is not None:
            return self._like(self._native.select(*[c for c in self.columns if like in c]))
        if regex is not None:
            import re

            pat = re.compile(regex)
            return self._like(self._native.select(*[c for c in self.columns if pat.search(c)]))
        if mask is None:
            raise TypeError("filter() needs a row mask or one of items= / like= / regex=")
        if not isinstance(mask, Series):
            mask = mask.apply(self)
        return self._like(self._native.filter(_unwrap(mask)))

    def select(self, *items: object) -> "DataFrame":
        """Project to ``items``: column names, bare column expressions, or
        named expressions (``(col("a") * 2).alias("b")``), in that order; a
        set of aggregate expressions (``col("a").sum()``) gives the one-row
        frame ``group_by().agg(...)`` does, each unaliased aggregate of a
        column named after that column (the polars ``select``)."""
        from .columnar import Agg, GroupBy, resolve_selectors, select_aggs

        if items and all(isinstance(i, Agg) for i in items):
            return GroupBy(self, []).agg(*select_aggs(items))
        pairs = resolve_selectors(items)
        out = self
        for name, expr in pairs:
            if expr is not None:
                out = out.assign(**{name: expr})
        return _wrap(out._native.select(*[name for name, _ in pairs]))

    def rename(
        self,
        mapping: "Optional[dict[str, str]]" = None,
        *,
        columns: "Optional[dict[str, str]]" = None,
    ) -> "DataFrame":
        """Rename columns by ``{old: new}`` (``columns=`` is the pandas spelling)."""
        if mapping is None:
            mapping = columns
        if mapping is None:
            raise TypeError("rename() needs a {old: new} mapping")
        return self._like(self._native.rename(_unwrap(mapping)))

    def with_column(self, name: str, col: "Series") -> "DataFrame":
        return self._like(self._native.with_column(_unwrap(name), _unwrap(col)))

    def take(self, indices: "Union[Series, Sequence[int]]") -> "DataFrame":
        """The rows at ``indices`` (a Series or a list; any order, repeats
        allowed)."""
        return self._like(self._native.take(_indices(indices)))

    def column_index(self, name: str) -> int:
        return self._native.column_index(_unwrap(name))

    def head(self, n: int = 5) -> "DataFrame":
        return self._like(self._native.head(_unwrap(n)))

    def tail(self, n: int = 5) -> "DataFrame":
        return self._like(self._native.tail(_unwrap(n)))

    def reverse(self) -> "DataFrame":
        return self._like(self._native.reverse())

    def drop_nulls(self) -> "DataFrame":
        return self._like(self._native.drop_nulls())

    def fill_null(self, value: Union[int, float]) -> "DataFrame":
        return self._like(self._native.fill_null(_unwrap(value)))

    def unique(self, subset: "str | Sequence[str] | None" = None) -> "DataFrame":
        """Distinct rows, keeping the first occurrence, keyed on every column
        or on ``subset`` (pandas ``drop_duplicates(subset=)``)."""
        return self._like(self._native.unique(subset))

    def drop_duplicates(self, subset: "str | Sequence[str] | None" = None) -> "DataFrame":
        return self._like(self._native.drop_duplicates(subset))

    def lazy(self) -> "LazyFrame":
        """Start a deferred query over this batch. Ops are recorded and nothing
        runs until :meth:`LazyFrame.collect`."""
        from .lazyframe import LazyFrame

        out = LazyFrame(self._native.lazy())
        out._index = self._index
        return out

    def sort_by_multi(
        self, names: "list[str]", descending: Union[bool, Sequence[bool]] = False
    ) -> "DataFrame":
        """A single flag broadcasts to every key; a sequence maps one flag per
        name (size must be 1 or ``len(names)``)."""
        return self._like(self._native.sort_by_multi(_unwrap(names), _unwrap(descending)))

    def sample(self, n: int, seed: int = 0, *, random_state: Optional[int] = None) -> "DataFrame":
        """A deterministic ``n``-row sample (``random_state`` is the pandas
        spelling of ``seed``)."""
        if random_state is not None:
            seed = random_state
        return self._like(self._native.sample(_unwrap(n), _unwrap(seed)))

    def with_row_index(self, name: str = "index") -> "DataFrame":
        return self._like(self._native.with_row_index(_unwrap(name)))

    def describe(self) -> "DataFrame":
        return self._like(self._native.describe())

    def null_count(self) -> "DataFrame":
        return self._like(self._native.null_count())

    def is_duplicated(self) -> "Series":
        return self._like(self._native.is_duplicated())

    def is_unique(self) -> "Series":
        return self._like(self._native.is_unique())

    def slice(self, offset: int, length: int) -> "DataFrame":
        return self._like(self._native.slice(_unwrap(offset), _unwrap(length)))

    def sort_by(self, name: str, descending: bool = False) -> "DataFrame":
        return self._like(self._native.sort_by(_unwrap(name), _unwrap(descending)))

    def topk(self, name: str, k: int, largest: bool = True) -> "DataFrame":
        return self._like(self._native.topk(_unwrap(name), _unwrap(k), _unwrap(largest)))

    def concat(
        self, *others: "DataFrame", how: Literal["vertical", "diagonal"] = "vertical"
    ) -> "DataFrame":
        """Vertically concatenate with ``others`` (UNION ALL). ``how='vertical'``
        requires a shared schema; ``how='diagonal'`` unions columns, null-filling
        those absent from a part and promoting a mixed-numeric column to float."""
        return self._like(self._native.concat(*[_unwrap(x) for x in others], how=how))

    def unpivot(
        self,
        id_vars: "str | list[str] | None" = None,
        value_vars: "str | list[str] | None" = None,
        *,
        index: "str | list[str] | None" = None,
        on: "str | list[str] | None" = None,
    ) -> "DataFrame":
        """Wide to long: keep ``id_vars``, stack ``value_vars`` into a
        ``variable`` / ``value`` pair. ``index`` / ``on`` are the polars
        spellings of the same two arguments."""
        ids = id_vars if id_vars is not None else index
        vals = value_vars if value_vars is not None else on
        if vals is None:
            raise TypeError("unpivot() needs value_vars (polars: on=)")
        return self._like(self._native.unpivot(_names(ids), _names(vals)))

    def melt(
        self,
        id_vars: "str | list[str] | None" = None,
        value_vars: "str | list[str] | None" = None,
        *,
        index: "str | list[str] | None" = None,
        on: "str | list[str] | None" = None,
    ) -> "DataFrame":
        return self.unpivot(id_vars, value_vars, index=index, on=on)

    def explode(self, column: str) -> "DataFrame":
        return self._like(self._native.explode(_unwrap(column)))

    def to_dummies(self, column: str) -> "DataFrame":
        return self._like(self._native.to_dummies(_unwrap(column)))

    def pivot(
        self,
        index: str,
        columns: "str | None" = None,
        values: "str | None" = None,
        agg: "PivotAgg" = "first",
        *,
        on: "str | None" = None,
        aggregate_function: "PivotAgg | None" = None,
        aggfunc: "PivotAgg | None" = None,
    ) -> "DataFrame":
        """Long to wide (pandas argument names). ``on`` / ``aggregate_function``
        are the polars spellings of ``columns`` / ``agg``; ``aggfunc`` the
        pandas ``pivot_table`` one."""
        cols = columns if columns is not None else on
        if cols is None or values is None:
            raise TypeError("pivot() needs index, columns (polars: on=) and values")
        agg_name = aggregate_function or aggfunc or agg
        return self._like(self._native.pivot(_unwrap(index), cols, _unwrap(values), agg_name))

    def group_by_dynamic(
        self,
        time_col: str,
        every: int,
        period: "int | None" = None,
        aggs: "list[str] | None" = None,
        origin: "int | Literal['min'] | None" = None,
    ) -> "DataFrame":
        """Tumbling/sliding time-window aggregation. ``origin`` anchors the
        window grid at ``origin + k*every`` (default: the classic ts-floored
        grid); pass a window's begin, or ``"min"`` to align buckets to the
        minimum timestamp (the frame-native ``time_bucket("min")``)."""
        return self._like(self._native.group_by_dynamic(time_col, every, period, aggs, origin))

    def query(
        self,
        dsl: "str | None" = None,
        *,
        group_by: "str | None" = None,
        aggs: "list[str] | None" = None,
        select: "list[str] | None" = None,
        order_by: "str | None" = None,
        descending: bool = False,
        limit: "int | None" = None,
    ) -> "DataFrame":
        return _wrap(
            self._native.query(
                dsl,
                group_by=group_by,
                aggs=aggs,
                select=select,
                order_by=order_by,
                descending=descending,
                limit=limit,
            )
        )

    def group_by(
        self,
        key: "Union[str, Series, Sequence[Union[str, Series]], None]" = None,
        *aggs: "Union[str, Agg]",
        dropna: bool = True,
    ) -> "Union[DataFrame, GroupBy]":
        """Group by one column name or several (a composite key), then either
        run inline legacy specs or return a lazy :class:`GroupBy` for
        ``.agg(...)``. With no key the whole frame is one group: ``.agg(...)``
        gives a one-row frame of any aggregates (``col("y").corr(col("x"))``
        included) and ``.sum()`` / ``.mean()`` / ... broadcast one. A row
        whose key is null is left out, as pandas; ``dropna=False`` keeps the
        null keys as their own group (polars), the key null in the result."""
        from .columnar import GroupBy

        given = [] if key is None else [key] if isinstance(key, (str, Series)) else list(key)
        if not given:
            whole = GroupBy(self, [])
            return whole.agg(*aggs) if aggs else whole
        # A Series key (``df.groupby(df["ts"] // 1000)``) becomes a column
        # named ``key`` (``key<i>`` among several), as a computed expression
        # key is.
        source = self
        keys: List[str] = []
        for i, k in enumerate(given):
            if isinstance(k, Series):
                name = "key" if len(given) == 1 else f"key{i}"
                source = source.with_column(name, k)
                keys.append(name)
            elif k in self:
                keys.append(k)
            else:
                raise KeyError(f"group_by: no column named {k!r}")
        if dropna:
            for k in keys:
                if source._col(k).null_count:
                    source = source.filter(source._col(k).notna())
        if not aggs:
            # The Python object over this wrapper, so the index names carry.
            return GroupBy(source, keys)
        return _wrap(
            source._native.group_by(*[_unwrap(k) for k in keys], *[_unwrap(a) for a in aggs])
        )

    def reduce(self, agg: str) -> "DataFrame":
        """One row: the engine aggregate named ``agg`` (``sum``, ``mean``,
        ``min``, ``max``, ``count_valid``, ``var``, ``std``, ``skew``,
        ``kurt``, ``sumsq``, ``first``, ``last``, ``bit_or``, ``prod``) over every
        eligible column, each under its own name. The grouped form is
        :meth:`group_by` ``(keys).reduce(agg)``."""
        return self._like(self._native.reduce(agg))

    def agg(
        self, func: object = None, axis: "Union[int, str]" = 0, **named: object
    ) -> "Union[DataFrame, Series]":
        """The pandas ``DataFrame.agg`` forms. ``axis=0`` reduces each column
        over the whole frame, one row: a name (``"sum"``), a list of names
        (flat ``<column>_<name>`` columns), a ``{column: name | [names]}``
        dict, ``out=("column", name)`` keywords, or aggregate expressions.
        ``axis=1`` reduces each row across its numeric columns into one
        Series: ``sum``, ``mean``, ``min``, ``max`` or ``count`` (nulls
        skipped, as pandas)."""
        if axis in (1, "columns"):
            if named or not isinstance(func, str):
                raise TypeError("agg(axis=1) takes one aggregate name")
            return self._reduce_rows(func)
        if axis not in (0, "index"):
            raise ValueError(f"agg: axis must be 0/'index' or 1/'columns', got {axis!r}")
        from .columnar import GroupBy

        return GroupBy(self, []).agg(*([] if func is None else [func]), **named)

    def _reduce_rows(self, func: str) -> "Series":
        """One value per row across the numeric columns, nulls skipped: the
        pandas ``axis=1`` reductions, as engine column ops (fill, add, the
        select kernel) and no per-row Python."""
        numeric = {
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
        }
        cols: List[Series] = [Series(self._native[name]) for name in self.columns]
        cols = [c for c in cols if c.dtype in numeric]
        if not cols:
            raise ValueError("agg(axis=1): no numeric columns")
        if func == "count":
            acc: Series = cols[0].is_not_null().astype("int64")
            for c in cols[1:]:
                acc = acc + c.is_not_null().astype("int64")
            return acc
        if func in ("sum", "mean"):
            total: Series = cols[0].fill_null(0)
            for c in cols[1:]:
                total = total + c.fill_null(0)
            if func == "sum":
                return total
            return total.astype("float64") / self._reduce_rows("count").astype("float64")
        if func in ("min", "max"):
            from .columnar import col, when

            if len({c.dtype for c in cols}) > 1:
                cols = [c.astype("float64") for c in cols]
            best: Series = cols[0]
            for c in cols[1:]:
                pair = DataFrame.from_dict({"a": best, "b": c})
                diff = col("a") - col("b")
                pick = (diff <= 0) if func == "min" else (diff >= 0)
                both = when(pick).then(col("a")).otherwise(col("b")).apply(pair)
                best = best.where(c.isna(), both)
                best = c.where(best.isna(), best)
            return best
        raise ValueError(
            f"agg({func!r}, axis=1): the per-row reductions are sum, mean, min, max and count"
        )

    aggregate = agg

    # The pandas ``df.sum()`` family: one row, one aggregate per column.
    def sum(self) -> "DataFrame":
        return self.reduce("sum")

    def mean(self) -> "DataFrame":
        return self.reduce("mean")

    def min(self) -> "DataFrame":
        return self.reduce("min")

    def max(self) -> "DataFrame":
        return self.reduce("max")

    def count(self) -> "DataFrame":
        return self.reduce("count_valid")

    def var(self) -> "DataFrame":
        return self.reduce("var")

    def std(self) -> "DataFrame":
        return self.reduce("std")

    def skew(self) -> "DataFrame":
        return self.reduce("skew")

    def kurt(self) -> "DataFrame":
        return self.reduce("kurt")


_register(_ext._DataFrame, DataFrame)


def _dataframe_from_arrow(table: "pa.Table") -> DataFrame:
    """Import a pyarrow Table into a native DataFrame wrapper."""
    return DataFrame(_ext._dataframe_from_arrow(table))
