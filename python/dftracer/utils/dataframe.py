# SPDX-License-Identifier: MIT
"""The ``DataFrame`` Python wrapper.

``DataFrame`` wraps the native ``_DataFrame`` handle: native frame ops plus
Python-side Arrow/pandas/polars conversion and pickling. Every relational and
reshape primitive - ``join``/``asof``/``interval``, ``window``/``gap_fill``,
``unnest``/``explode``, ``melt``/``pivot``, ``concat``/``union``,
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
    Literal,
    Mapping,
    Optional,
    Sequence,
    Tuple,
    Union,
    cast,
    get_args,
    overload,
)

from . import dftracer_utils_ext as _ext
from ._aliases import install as _install_aliases
from ._convert import (
    _apply_index,
    _arrow_native,
    _indices,
    _pandas_default,
    _pandas_nullable,
    _polars_via_arrow,
    _require_pyarrow,
    _to_pandas,
    index_columns,
)
from ._eval import Columnar
from ._pandas_frame import FramePandasMixin
from ._polars_frame import FramePolarsMixin
from ._window_spec import norm_window_spec
from .agg import AggForm, AggKeyword, select_aggs
from .enums import DType, rule_to_units
from .expr import Agg, Expr, Lit, Named, SelectItem, col, resolve_selectors, when
from .series import Series

if TYPE_CHECKING:
    import numpy as np
    import pandas as pd
    import polars as pl
    import pyarrow as pa
    from pandas.api.extensions import ExtensionArray
    from typing_extensions import Self

    from ._loc import At, ILoc, Loc, Resampler
    from .dftracer_utils_ext import Scalar
    from .groupby import GroupBy
    from .lazyframe import LazyFrame


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
SessSpec = Union[
    Tuple[Literal["sessionize"], str, float, str],
    Tuple[Literal["sessionize"], str, float, str, Optional[str]],
    Tuple[Literal["sessionize"], str, float, str, Optional[str], float],
]
FrameSpec = Tuple[
    Literal[
        "frame_sum",
        "frame_min",
        "frame_max",
        "frame_count",
        "frame_mean",
        "frame_var",
        "frame_std",
        "frame_count_distinct",
        "frame_collect",
    ],
    str,
    Optional[int],
    Optional[int],
    str,
]
FrameQuantileSpec = Tuple[
    Literal["frame_quantile"],
    str,
    Optional[int],
    Optional[int],
    str,
    int,
    Literal["rows", "range"],
    float,
]
FrameArgSpec = Tuple[
    Literal["frame_arg_max", "frame_arg_min"],
    str,
    Optional[int],
    Optional[int],
    str,
    int,
    Literal["rows", "range"],
    str,
]
NtileSpec = Tuple[Literal["ntile"], int, str]
FrameMinSpec = Tuple[
    Literal[
        "frame_sum",
        "frame_min",
        "frame_max",
        "frame_count",
        "frame_mean",
        "frame_var",
        "frame_std",
        "frame_count_distinct",
        "frame_collect",
    ],
    str,
    Optional[int],
    Optional[int],
    str,
    int,
]
FrameModeSpec = Tuple[
    Literal[
        "frame_sum",
        "frame_min",
        "frame_max",
        "frame_count",
        "frame_mean",
        "frame_var",
        "frame_std",
        "frame_count_distinct",
        "frame_collect",
    ],
    str,
    Optional[int],
    Optional[int],
    str,
    int,
    Literal["rows", "range"],
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
    FrameModeSpec,
    FrameQuantileSpec,
    FrameArgSpec,
    NtileSpec,
    PosSpec,
    NthSpec,
]


JoinHow = Literal[
    "inner", "left", "right", "outer", "full", "semi", "anti", "cross", "lookup", "nest"
]
PivotAgg = Literal[
    "first", "last", "sum", "min", "max", "mean", "count", "var", "std", "skew", "kurt"
]
# The pivot_table aggregates: the native pivot ones and the group-by
# methods that reduce a group to one value.
PivotTableAgg = Literal[
    "first",
    "last",
    "sum",
    "min",
    "max",
    "mean",
    "count",
    "var",
    "std",
    "skew",
    "kurt",
    "median",
    "prod",
    "nunique",
]


def as_names(cols: str | Sequence[str] | None) -> list[str]:
    if cols is None:
        return []
    if isinstance(cols, str):
        return [cols]
    return list(cols)


def single_index(index: Sequence[str], op: str) -> str:
    """The one index column an op reads its time from."""
    if not index:
        raise ValueError(f"{op}: set_index(time) first, or pass on=")
    if len(index) != 1:
        raise ValueError(f"{op}: the index has {len(index)} columns; pass on=")
    return index[0]


def _bound(native: _ext.DataFrame) -> DataFrame:
    """A native result as the public class: the native factories build it."""
    return cast("DataFrame", native)


class DataFrame(FramePandasMixin, FramePolarsMixin, _ext.DataFrame):
    """A named set of columns: native frame ops plus Arrow conversion.

    ``DataFrame({"a": [1, 2]})`` builds one from a ``{name: array-like}``
    mapping, the pandas / polars constructor."""

    __slots__ = ()

    # The native type has no constructor of its own: its factories build this
    # class directly (_bind_types), so only a Python-data call comes through
    # here, and __new__ hands back the frame a native importer built.
    def __new__(cls, data: Mapping[str, object] | _ext.DataFrame) -> DataFrame:
        if cls is not DataFrame:
            raise TypeError(
                "DataFrame cannot be subclassed: the native calls build the class bound by "
                "_bind_types, so an instance of a subclass would come back as a DataFrame"
            )
        if isinstance(data, _ext.DataFrame):
            return cast("DataFrame", data)
        return DataFrame.from_dict(data)

    def __init__(self, data: Mapping[str, object] | _ext.DataFrame) -> None:
        pass

    # -- the index: named columns, or the row position -------------------------
    @property
    def index(self) -> Series | DataFrame:
        """The index column set by :meth:`set_index` as a Series (several
        columns: a frame of them), else the row positions (pandas' default
        ``RangeIndex``)."""
        names = self.index_names()
        if not names:
            return Series.from_list(list(range(len(self))))
        if len(names) == 1:
            return _ext.DataFrame.__getitem__(self, names[0])
        return _bound(_ext.DataFrame.with_index(_ext.DataFrame.select(self, *names), []))

    def index_levels(self) -> list[Series]:
        """The index columns, one Series per level; the row positions when no
        index is set."""
        names = self.index_names()
        if not names:
            return [Series.from_list(list(range(len(self))))]
        return [_ext.DataFrame.__getitem__(self, name) for name in names]

    def set_index(self, names: str | Sequence[str]) -> DataFrame:
        """Name the column(s) ``loc`` / ``at`` / ``resample`` read labels
        from: the native row index of the frame. The columns stay in place;
        nothing is copied. Several names are the pandas ``MultiIndex``
        shape: ``loc[(a, b)]`` matches every level, ``loc[a]`` the first. A
        list or struct column cannot be an index."""
        keys = [names] if isinstance(names, str) else list(names)
        if not keys:
            raise ValueError("set_index: at least one column")
        for name in keys:
            if name not in self:
                raise KeyError(f"set_index: no column named {name!r}")
        return _bound(_ext.DataFrame.with_index(self, keys))

    def reset_index(self, drop: bool = False) -> DataFrame:
        """Forget the index columns (they stay as plain columns unless
        ``drop``); with no index set, prepend the row positions as ``index``,
        as pandas."""
        names = self.index_names()
        if not names:
            return self if drop else _bound(_ext.DataFrame.with_row_index(self, "index"))
        if drop:
            keep = [c for c in self._names() if c not in names]
            return _bound(_ext.DataFrame.select(self, *keep))
        return _bound(_ext.DataFrame.with_index(self, []))

    @property
    def iloc(self) -> ILoc:
        """Rows and columns by position: ``df.iloc[3]``, ``df.iloc[2:5]``,
        ``df.iloc[[0, 4], "c"]``; assignment rebuilds the touched columns."""
        from ._loc import ILoc

        return ILoc(self)

    @property
    def loc(self) -> Loc:
        """Rows by index label (a value, an inclusive slice, a list, a Bool
        mask or an expression), columns by name; assignment rebuilds the
        touched columns (``df.loc[df["a"] > 1, "c"] = 0``)."""
        from ._loc import Loc

        return Loc(self)

    @property
    def at(self) -> At:
        """One value by index label and column name."""
        from ._loc import At

        return At(self, by_position=False)

    @property
    def iat(self) -> At:
        """One value by row position and column position."""
        from ._loc import At

        return At(self, by_position=True)

    def __setitem__(self, key: object, value: object) -> None:
        """``df["c"] = series | scalar`` adds or replaces a column;
        ``df[mask] = value`` writes every column where the mask holds. The
        frame's columns are immutable: the handle is rebound to a new frame
        that shares every untouched column."""
        from ._loc import Labels, Rows, assign_rows, rows_by_label

        if isinstance(key, str):
            assign_rows(self, Rows.all(), [key], value)
        elif isinstance(key, (list, tuple)):
            assign_rows(self, Rows.all(), [str(k) for k in key], value)
        elif isinstance(key, Series):
            assign_rows(self, rows_by_label(Labels(self), key), self.columns, value)
        else:
            raise TypeError("assign to a column name, a list of names or a Bool mask")

    def resample(self, rule: int | str, on: str | None = None, unit: str = "us") -> Resampler:
        """Tumbling time windows of ``rule`` (``"5s"``, ``"100ms"``, ``"1min"``,
        or an int of the column's own units) over the index column or ``on``,
        with the aggregate family (``.sum()``, ``.agg(...)``, ``.size()``);
        ``unit`` is the time column's unit (dftracer timestamps are ``us``).
        Each call is one ``group_by_dynamic`` over the time-sorted frame."""
        from ._loc import Resampler

        time = on if on is not None else single_index(self.index_names(), "resample")
        if time not in self:
            raise KeyError(f"resample: no column named {time!r}")
        return Resampler(self, time, rule_to_units(rule, unit))

    @classmethod
    def from_arrow(cls, table: pa.Table) -> DataFrame:
        """Import a ``pyarrow.Table`` (or any ``__arrow_c_stream__`` provider),
        zero-copy via the Arrow C Data Interface."""
        return _dataframe_from_arrow(table)

    @classmethod
    def from_pandas(cls, df: pd.DataFrame) -> DataFrame:
        """Import a ``pandas.DataFrame`` through NumPy, with no pyarrow (see
        :meth:`Series.from_pandas`). A non-default index becomes columns after the
        data columns, named by its levels or ``__index_level_<i>__``, and the
        frame's row index (:meth:`index_names`); a ``RangeIndex`` is no index."""
        columns = [(str(name), df[name]) for name in df.columns]
        if len({n for n, _ in columns}) != len(columns):
            raise ValueError("from_pandas: column names are not unique")
        levels = index_columns(df)
        names = [n for n, _ in levels]
        if len({n for n, _ in columns + levels}) != len(columns) + len(levels):
            raise ValueError("from_pandas: an index level repeats a column name")
        frame = _frame_of(columns + levels, Series.from_pandas)
        return _bound(_ext.DataFrame.with_index(frame, names)) if names else frame

    @classmethod
    def from_polars(cls, df: pl.DataFrame) -> DataFrame:
        """Import a ``polars.DataFrame`` with no pyarrow (see
        :meth:`Series.from_polars`)."""
        return _frame_of([(name, df[name]) for name in df.columns], Series.from_polars)

    @classmethod
    def from_parquet(cls, path: str, columns: Sequence[str] | None = None) -> DataFrame:
        """Read a Parquet file into a frame (needs pyarrow)."""
        _require_pyarrow()
        import pyarrow.parquet as pq

        table = pq.read_table(path, columns=columns)
        # Dask writes its default index as a column of this name and lists it as
        # an index column; it is not data. Any other stored index stays a column.
        marker = "__null_dask_index__"
        if marker in table.column_names and marker not in (columns or ()):
            import json

            pandas_md = (table.schema.metadata or {}).get(b"pandas")
            if pandas_md and marker in json.loads(pandas_md).get("index_columns", ()):
                table = table.drop_columns([marker])
        return _dataframe_from_arrow(table)

    @classmethod
    def from_dict(cls, mapping: Mapping[str, object]) -> DataFrame:
        """Import a ``{name: array-like}`` mapping: Series, lists of scalars, NumPy
        arrays and pandas or polars Series import natively; anything else (a
        pyarrow array, nested lists) goes through pyarrow."""
        native = _native_columns(mapping)
        if native is not None:
            return DataFrame(_ext._dataframe_from_columns(*native))
        pa = _require_pyarrow()
        return _dataframe_from_arrow(pa.table(mapping))

    @classmethod
    def from_numpy(
        cls,
        arr: np.ndarray | dict[str, np.ndarray],
        columns: Sequence[str],
    ) -> DataFrame:
        """Import NumPy columns into a frame.

        ``arr`` is either a 2-D array (one column per column index) or a
        ``{name: 1-D array}`` mapping; ``columns`` names the columns of a 2-D
        array (ignored for a mapping)."""
        if isinstance(arr, dict):
            cols = {name: Series.from_numpy(col) for name, col in arr.items()}
        else:
            cols = {name: Series.from_numpy(arr[:, i]) for i, name in enumerate(columns)}
        return DataFrame(_ext._dataframe_from_columns(list(cols), [c for c in cols.values()]))

    def to_arrow(self) -> pa.Table:
        """This frame as a ``pyarrow.Table`` (zero-copy via the C Data Interface
        stream)."""
        return _require_pyarrow().table(self)

    def _arrow_for_parquet(self) -> pa.Table:
        """The Arrow table a Parquet writer saves: ``to_arrow`` plus the pandas
        metadata block that makes ``pandas.read_parquet`` restore the nullable
        extension dtype (``Int8`` to ``Int64``, ``UInt8`` to ``UInt64``,
        ``boolean``, ``string``) of every integer, bool and string column that
        holds a null. Other columns read as pandas reads them by default. The
        block is made by pyarrow from a zero-row pandas frame; without pandas
        nothing is added."""
        pa = _require_pyarrow()
        table = self.to_arrow()
        try:
            import pandas  # noqa: F401  (to_pandas below needs it)
        except ImportError:
            return table
        extension: dict[str, str] = {}
        for field in table.schema:
            if table.column(field.name).null_count == 0:
                continue
            t = field.type
            if pa.types.is_integer(t):
                extension[field.name] = ("Int" if pa.types.is_signed_integer(t) else "UInt") + str(
                    t.bit_width
                )
            elif pa.types.is_boolean(t):
                extension[field.name] = "boolean"
            elif pa.types.is_string(t) or pa.types.is_large_string(t):
                extension[field.name] = "string"
        if not extension:
            return table
        empty = table.slice(0, 0).to_pandas()
        for name, dtype in extension.items():
            empty[name] = empty[name].astype(dtype)
        block = pa.Table.from_pandas(empty, schema=table.schema, preserve_index=False)
        pandas_md = (block.schema.metadata or {}).get(b"pandas")
        if pandas_md is None:
            return table
        return table.replace_schema_metadata(
            {**(table.schema.metadata or {}), b"pandas": pandas_md}
        )

    def to_pandas(
        self,
        *,
        arrow: bool = False,
        nullable: bool = False,
        index: str | Sequence[str] | None = None,
    ) -> pd.DataFrame:
        """This frame as a pandas DataFrame. By default the columns are NumPy
        dtypes, which copies (pyarrow packs them into blocks), except that a
        dictionary-encoded string column becomes ``category`` and an integer,
        bool or string column that holds nulls becomes pandas' own nullable
        dtype (``Int8`` .. ``UInt64``, ``boolean``, ``string``), so
        ``DataFrame.from_pandas(df).to_pandas()`` keeps those dtypes. On pandas 3
        a string column with nulls is the default ``str`` dtype, which holds
        missing values already. Float columns keep NaN. ``arrow=True``
        keeps the columns Arrow-backed (``int64[pyarrow]``, ``string[pyarrow]``,
        ...), sharing this frame's buffers with no copy; ``nullable=True`` gives
        the nullable dtypes (``Int8`` .. ``UInt64``, ``Float32``, ``Float64``,
        ``boolean``, ``string``) even for a column with no nulls, so a null is
        ``pd.NA`` and a float NaN stays NaN; a column of another type converts
        as usual. The NumPy and nullable forms are built natively. A dictionary
        column uses pyarrow, where installed, to read its codes without building
        a string per row, and keeps the dictionary order of its used values;
        without pyarrow the categories come out sorted.

        ``index`` names the columns to move into the pandas index, in the order
        given: a flat index for one name, a MultiIndex for several. An unknown
        name raises ``KeyError`` and a repeated one ``ValueError``. Without
        ``index``, the frame's own row index (:meth:`index_names`) becomes the
        pandas index, a MultiIndex for several levels; a level that
        :meth:`from_pandas` named ``__index_level_<i>__`` is unnamed again."""
        if arrow and nullable:
            raise ValueError("to_pandas: pass either arrow=True or nullable=True, not both")
        if arrow:
            out = cast("pd.DataFrame", _to_pandas(self.to_arrow(), arrow))
        else:
            import pandas as pd

            def one(name: str) -> pd.Series | np.ndarray | ExtensionArray:
                col = _ext.DataFrame.__getitem__(self, name)
                values = _pandas_nullable(col) if nullable else None
                return values if values is not None else _pandas_default(col)

            out = pd.DataFrame({name: one(name) for name in self._names()})
        if index is not None:
            return _apply_index(out, index)
        names = self.index_names()
        return _apply_index(out, names, restore_unnamed=True) if names else out

    def to_polars(self) -> pl.DataFrame:
        """This frame as a polars DataFrame. Where the extension has Arrow
        support and no column is Float16 or a decimal, the frame
        comes through the Arrow PyCapsule stream (polars >= 1.3; an older
        polars raises its own error), without pyarrow; otherwise each column
        uses the native conversion. A dictionary column becomes
        ``Categorical``."""
        try:
            import polars as pl
        except ImportError:
            raise ImportError(
                "polars is required for to_polars(). Install with: pip install polars"
            ) from None
        if self.num_rows == 0:
            return pl.DataFrame()
        if hasattr(self, "__arrow_c_stream__") and all(
            _polars_via_arrow(_ext.DataFrame.__getitem__(self, name)) for name in self._names()
        ):
            return pl.DataFrame(self)
        return pl.DataFrame({name: self[name].to_polars() for name in self._names()})

    def __len__(self) -> int:
        return self.num_rows

    def __repr__(self) -> str:
        from ._repr import frame_repr

        return frame_repr(self)

    def _repr_html_(self) -> str:
        """The table that Jupyter shows for ``display(df)``: the rows of ``repr``."""
        from ._repr import frame_html

        return frame_html(self)

    # -- pandas / polars spellings ---------------------------------------------
    def _names(self) -> list[str]:
        """Every column, the index columns included (``columns`` leaves them out)."""
        return list(self.column_names)

    @property
    def columns(self) -> list[str]:
        """The columns, as pandas shows them: the index columns of
        :meth:`index_names` are not among them. A frame with no index lists
        every column, and the Polars names (``schema``, ``width``, ``select``,
        ``col``) see all of them."""
        names = list(self.column_names)
        index = _ext.DataFrame.index_names(self)
        return [c for c in names if c not in index] if index else names

    @overload
    def __getitem__(self, key: str, /) -> Series: ...
    @overload
    def __getitem__(self, key: list[str] | tuple[str, ...] | Series, /) -> Self: ...
    def __getitem__(self, key: str | list[str] | tuple[str, ...] | Series, /) -> Series | Self:
        """A column by name, a frame of the listed columns or a frame of the
        rows a Bool Series keeps. As pandas, a list takes the columns of
        :attr:`columns` and the frame keeps its index."""
        if isinstance(key, (list, tuple)):
            index = _ext.DataFrame.index_names(self)
            if index:
                shown = set(self.columns)
                missing = [c for c in key if c not in shown]
                if missing:
                    raise KeyError(f"None of {missing} are in the columns")
                return cast("Self", _bound(_ext.DataFrame.select(self, *key, *index)))
        return _ext.DataFrame.__getitem__(self, key)

    @property
    def shape(self) -> tuple[int, int]:
        return (self.num_rows, len(self.columns))

    @property
    def height(self) -> int:
        return self.num_rows

    @property
    def width(self) -> int:
        return self.num_columns

    @property
    def dtypes(self) -> list[DType]:
        return [self[name].dtype for name in self.columns]

    @property
    def schema(self) -> dict[str, DType]:
        return {name: self[name].dtype for name in self._names()}

    def drop(
        self,
        *names: str | Sequence[str],
        columns: str | Sequence[str] | None = None,
        axis: int | Literal["index", "columns"] | None = None,
    ) -> DataFrame:
        """Every column except ``names`` (``columns=`` is the pandas spelling,
        which names shown columns only: an index column there is a ``KeyError``,
        while a positional name, the Polars form, may be one). ``axis`` is ``None``, ``1`` or ``"columns"``; any other value raises
        ``ValueError`` (the frame has no row labels for ``axis=0``)."""
        if (
            axis is not None
            and not (axis == 1 and not isinstance(axis, bool))
            and axis != "columns"
        ):
            raise ValueError(
                f"drop: axis must be None, 1 or 'columns', not {axis!r} "
                "(dropping rows by label needs the row index; use filter)"
            )
        pandas_named = set(as_names(columns))
        dropped = set(pandas_named)
        for n in names:
            dropped.update(as_names(n))
        shown = set(self.columns)
        missing = [n for n in dropped if n not in (shown if n in pandas_named else self)]
        if missing:
            raise KeyError(f"drop: no column named {missing[0]!r}")
        return _bound(_ext.DataFrame.select(self, *[c for c in self._names() if c not in dropped]))

    def _constant_column(self, value: bool | int | float | str | None) -> Series:
        """A column of this frame's row count holding ``value``: an Int64, Float64,
        Bool or String column, empty for a frame with no rows. ``None`` gives an
        all-null String column, the type a column of only ``None`` has in
        ``from_dict`` and ``from_pandas``."""
        kind = (
            DType.BOOL
            if isinstance(value, bool)
            else DType.INT64
            if isinstance(value, int)
            else DType.FLOAT64
            if isinstance(value, float)
            else DType.STRING
        )
        n = len(self)
        if n == 0:
            return _ext._series_from_list([], int(kind))
        # Built from the row count alone, so no other column's nulls reach it.
        if value is None:
            return _ext._series_nulls(int(kind), n)
        return _ext._series_nulls(int(kind), n).fillna(value)

    def assign(self, **columns: Series | Expr | bool | int | float | str | None) -> DataFrame:
        """Add or replace columns from Series, column expressions or Python
        scalars (pandas ``assign``; polars ``with_columns``). A scalar, or
        ``lit(value)``, gives a column of the frame's row count holding it;
        ``None`` (or ``lit(None)``) gives an all-null String column."""

        out = self
        for name, value in columns.items():
            if isinstance(value, Lit) and value.value is None:
                value = None  # lit(None) is the null constant, as a bare None is
            if value is None or isinstance(value, (bool, int, float, str)):
                value = out._constant_column(value)
            col = value if isinstance(value, Series) else value.apply(out)
            out = _ext.DataFrame.with_column(out, name, col)
        return out

    def with_columns(
        self, *named: Named, **columns: Series | Expr | bool | int | float | str | None
    ) -> DataFrame:
        """:meth:`assign`, also taking named expressions positionally
        (``with_columns((col("a") * 2).alias("b"))``, the polars spelling)."""

        out = self
        for item in named:
            if not isinstance(item, Named):
                raise TypeError("with_columns: a positional item must be expr.alias(name)")
            out = out.assign(**{item.name: item.expr})
        return out.assign(**columns)

    def cast(
        self, dtype: Mapping[str, str | int | DType | type] | str | int | DType | type
    ) -> DataFrame:
        """Cast columns (``astype`` is the pandas name). A mapping casts
        only the named columns; a single dtype casts every column to it. If a
        column cannot be cast the call raises ``TypeError`` that names the column
        and the two types, and no frame is returned."""
        if not isinstance(dtype, Mapping):
            dtype = {name: dtype for name in self.columns}
        casted: dict[str, Series] = {}
        for name, target in dtype.items():
            try:
                casted[name] = _ext.DataFrame.__getitem__(self, name).cast(target)
            except TypeError as e:
                raise TypeError(f"cast: column {name!r}: {e}") from None
        out = self
        for name, column in casted.items():
            out = _ext.DataFrame.with_column(out, name, column)
        return out

    def duplicated(
        self,
        subset: str | Sequence[str] | None = None,
        keep: Literal["first", "last"] | bool = "first",
    ) -> Series:
        """Bool mask of the rows that repeat an earlier row (pandas
        ``DataFrame.duplicated``), compared on ``subset`` or every column:
        ``keep="first"`` leaves the first of equal rows unmarked, ``"last"``
        the last, and ``False`` marks every one of them. Nulls compare
        equal."""
        cols = list(self.columns) if subset is None else as_names(subset)
        frame = self[cols]
        if keep is False:
            return _ext.DataFrame.is_duplicated(frame)
        if keep == "last":
            back = list(range(frame.num_rows - 1, -1, -1))
            return frame.take(back).duplicated(keep="first").take(back)
        if keep != "first":
            raise ValueError(f"duplicated: keep must be 'first', 'last' or False, not {keep!r}")
        return frame.groupby(cols, dropna=False, as_index=False).cumcount()["cumcount"] > 0

    @overload
    def groupby(
        self,
        by: str | Series | Sequence[str | Series],
        *,
        dropna: bool = True,
        sort: bool = True,
        as_index: bool = True,
    ) -> GroupBy: ...
    @overload
    def groupby(
        self,
        by: str | Series | Sequence[str | Series],
        agg: str | Agg,
        /,
        *aggs: str | Agg,
        dropna: bool = True,
        sort: bool = True,
        as_index: bool = True,
    ) -> DataFrame: ...
    def groupby(
        self,
        by: str | Series | Sequence[str | Series],
        *aggs: str | Agg,
        dropna: bool = True,
        sort: bool = True,
        as_index: bool = True,
    ) -> DataFrame | GroupBy:
        """The pandas ``groupby``: :meth:`group_by` with ``sort`` and
        ``as_index``. With ``sort`` (the pandas default) every aggregate result
        is ordered ascending by the key columns, nulls last; ``sort=False``
        keeps the engine's group order. With ``as_index`` (the pandas default)
        an aggregate result is indexed by the keys, as pandas does, even when
        this frame has no index; ``as_index=False`` keeps the keys as plain
        columns, as :meth:`group_by` always does."""
        given = [by] if isinstance(by, (str, Series)) else list(by)
        source = self
        hidden = [n for n in _ext.DataFrame.index_names(self) if n not in given]
        if hidden:
            # The index levels that are not keys are not value columns, as pandas.
            source = _bound(
                _ext.DataFrame.select(self, *[c for c in self._names() if c not in hidden])
            )
        out = source.group_by(by, *aggs, dropna=dropna)
        if isinstance(out, DataFrame):
            keys = [
                k if isinstance(k, str) else "key" if len(given) == 1 else f"key{i}"
                for i, k in enumerate(given)
            ]
            if sort:
                out = out.sort_by_multi(keys)
            return out.set_index(keys) if as_index else out.reset_index()
        out._sort = sort
        out._as_index = as_index
        if hidden:
            out._origin = self
        return out

    def nlargest(self, n: int, columns: str) -> DataFrame:
        """The pandas ``nlargest``: the ``n`` rows with the largest ``columns``."""
        return _bound(_ext.DataFrame.topk(self, columns, n, True))

    def nsmallest(self, n: int, columns: str) -> DataFrame:
        """The pandas ``nsmallest``: the ``n`` rows with the smallest ``columns``."""
        return _bound(_ext.DataFrame.topk(self, columns, n, False))

    def pivot_table(
        self,
        values: str,
        index: str | Sequence[str],
        columns: str,
        aggfunc: PivotTableAgg = "first",
    ) -> DataFrame:
        """Long to wide with ``aggfunc`` over each (``index``, ``columns``)
        cell: one row for each combination of the ``index`` keys, in sorted
        order, and one column for each value of ``columns``. ``index`` may name
        several keys and ``aggfunc`` may be any group-by reduction, such as
        ``"median"``."""
        keys = as_names(index)
        if len(keys) == 1 and aggfunc in get_args(PivotAgg):
            return self.pivot(keys[0], columns, values, cast("PivotAgg", aggfunc))
        picked = self[keys + [columns, values]]
        cells: DataFrame = getattr(picked.groupby(keys + [columns], as_index=False), aggfunc)()
        row = "__dftu_pivot_row__"
        cells = cells.assign(**{row: cells.groupby(keys, as_index=False).ngroup()["ngroup"]})
        wide = cells[[row, columns, values]].pivot(row, columns, values, "first")
        names = cells[[row] + keys].drop_duplicates()
        out = names.merge(wide, on=row).sort_values(keys)
        return out.drop(columns=[row])

    def to_dict(self) -> dict[str, list[Scalar | None]]:
        """Each shown column as a list; the index columns are not among them,
        as in pandas ``to_dict("list")``."""
        return {name: self[name].to_list() for name in self.columns}

    def __copy__(self) -> Self:
        return self.copy()

    def __deepcopy__(self, memo: object) -> Self:
        """The columns are immutable, so a deep copy is a shallow one."""
        return self.copy()

    # Opaque Arrow C Data Interface capsule; Python has no capsule type.
    def __reduce__(self) -> tuple[Callable[[bytes], DataFrame], tuple[bytes, ...]]:
        return (_dataframe_from_bytes, (_ext.DataFrame.to_bytes(self),))

    def apply(self, func: Expr | Callable[..., object], axis: int = 0) -> Series | DataFrame:
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

        if isinstance(func, Expr):
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
        results = {name: func(_ext.DataFrame.__getitem__(self, name)) for name in self.columns}
        if all(isinstance(r, Series) for r in results.values()):
            out = self
            for name, r in results.items():
                out = out.with_column(name, cast(Series, r))
            return out.select(*self._names())
        if any(isinstance(r, Series) for r in results.values()):
            raise TypeError(
                "apply(): the callable must return a Series for every column, or for none"
            )
        return DataFrame({name: [r] for name, r in results.items()})

    def hash_partition(self, keys: str | list[str], n_parts: int) -> list[DataFrame]:
        """Hash-partition the rows into ``n_parts`` frames by
        :meth:`partition_id` (the shuffle primitive)."""
        return [DataFrame(p) for p in _ext.DataFrame.hash_partition(self, keys, n_parts)]

    def window(
        self,
        partition_by: Sequence[str] | None = None,
        order_by: Sequence[str] | None = None,
        specs: Sequence[WindowSpec] | None = None,
    ) -> DataFrame:
        """SQL window functions, PARTITION BY ``partition_by`` and ORDER BY
        ``order_by``.

        ``specs`` is a list of tuples, one per appended output column:

        - ``("row_number"|"rank"|"dense_rank", out)``
        - ``("lag"|"lead", value_col, offset, out)``
        - ``("running_sum"|"running_min"|"running_max"|"running_count"|"running_prod",
          value_col, out)`` (``running_prod`` is Float64)
        - ``("delta", value_col, out)``
        - ``("rate", value_col, time_col, out[, counter])``
        - ``("sessionize", time_col, gap, out[, end_col[, span]])``: the
          1-based session of each row; a row starts a new session when its
          time is more than ``gap`` after the latest end of the session so
          far (``end_col``, else the time), or more than ``span`` after the
          session's first time; a null time is a null session
        - ``("frame_sum"|"frame_min"|"frame_max"|"frame_count"|"frame_mean",
          value_col, preceding, following, out[, min_periods[, mode]])`` (a bound
          of ``None`` is unbounded; the output is null while the frame holds
          fewer than ``min_periods`` present values; ``mode`` is ``"rows"``
          (default, bounds are row offsets) or ``"range"``, where the bounds
          are value deltas on the single numeric ``order_by`` column and peers
          are included)
        - ``("frame_var"|"frame_std"|"frame_count_distinct"|"frame_collect",
          value_col, preceding, following, out[, min_periods[, mode]])``: the
          sample variance and standard deviation (null below 2 values), the
          distinct count (0 for an empty frame) and the list of the frame's
          values in frame order (an empty list for an empty frame)
        - ``("frame_quantile", value_col, preceding, following, out,
          min_periods, mode, q)``: the exact quantile at level ``q`` in
          [0, 1] with linear interpolation
        - ``("frame_arg_max"|"frame_arg_min", value_col, preceding, following,
          out, min_periods, mode, by_col)``: the ``value_col`` value of the row
          with the largest (smallest) ``by_col`` in the frame, the earliest
          row on a tie (``by_col`` is numeric, string or bool)
        - ``("ntile", n, out)``
        - ``("first_value"|"last_value"|"fill_forward", value_col, out)``
          (``fill_forward`` is the nearest present value at or before the row)
        - ``("nth_value", value_col, k, out)``

        All input columns pass through, then one column per spec, in sorted
        (partition, order) row order."""
        norm = [norm_window_spec(s) for s in (specs or [])]
        return _bound(_ext.DataFrame.window(self, as_names(partition_by), as_names(order_by), norm))

    def gap_fill(
        self,
        partition_by: Sequence[str] | None,
        time: str,
        bucket: int,
        values: str | Sequence[str],
        mode: Literal["none", "locf", "linear"] = "none",
        start: int | None = None,
        end: int | None = None,
    ) -> DataFrame:
        """Materialize a regular time grid of width ``bucket``, PARTITION BY
        ``partition_by``, keyed on the integer ``time`` column.

        ``values`` (a name or list of names) are filled on generated rows per
        ``mode``: ``"none"`` leaves them null, ``"locf"`` carries the last real
        value forward, ``"linear"`` interpolates (emitting the value columns as
        double). ``start``/``end`` set an explicit grid range for every
        partition; pass both or neither."""
        if (start is None) != (end is None):
            raise ValueError("gap_fill: pass both start and end, or neither")
        return _ext.DataFrame.gap_fill(
            self,
            as_names(partition_by),
            time,
            int(bucket),
            as_names(values),
            mode,
            start,
            end,
        )

    def join(
        self,
        other: DataFrame,
        on: int | str | Sequence[str] | None = None,
        how: JoinHow = "inner",
        left_on: str | Sequence[str] | None = None,
        right_on: str | Sequence[str] | None = None,
        suffix: str = "_right",
        nulls_equal: bool = False,
    ) -> DataFrame:
        """Hash join with ``other``.

        ``on`` names the key column(s) shared by both frames (a str, a list of
        names, or an int count of the leading columns); ``left_on`` /
        ``right_on`` name each side's keys instead when they differ. A null key
        never matches (the SQL rule), unless ``nulls_equal=True``: then a null
        key cell matches another null key cell (never a value), as pandas does;
        it is refused for ``"cross"``, ``"lookup"`` and ``"nest"``. ``how`` is ``"inner"``, ``"left"``, ``"right"``,
        ``"outer"`` (alias ``"full"``), ``"semi"``, ``"anti"``, ``"cross"``
        (no keys), ``"lookup"`` or ``"nest"``. Output is this frame's columns,
        then ``other``'s except a key sharing its left key's name; any other
        colliding name gets ``suffix``. Matched rows keep this frame's order;
        right / outer append the unmatched right rows. Semi / anti emit this
        frame's columns only.

        ``"lookup"`` keeps each row of this frame once and adds ``other``'s
        columns but its keys, from the matching row (null when none); rows of
        ``other`` sharing a key must hold equal values, and a column named
        like one of this frame's fills it, which must be null there.
        ``"nest"`` adds one list column named ``suffix`` holding every
        matching row of ``other`` as a struct. Both compare keys by value
        (``1 == 1.0``, ``"4" != 4``). See :meth:`merge` for the pandas
        argument order."""
        if isinstance(on, bool):
            raise TypeError("join: 'on' must be a key name/list or an int count")
        if isinstance(on, int):
            names = list(self.column_names)
            if on < 1 or on > len(names):
                raise ValueError(
                    f"join: 'on' count {on} is out of range for a {len(names)}-column frame"
                )
            on = names[:on]
        return _ext.DataFrame.join(
            self,
            other,
            on=on,
            how=how,
            left_on=left_on,
            right_on=right_on,
            suffix=suffix,
            nulls_equal=nulls_equal,
        )

    def merge(
        self,
        right: DataFrame,
        how: JoinHow = "inner",
        on: str | Sequence[str] | None = None,
        left_on: str | Sequence[str] | None = None,
        right_on: str | Sequence[str] | None = None,
        suffixes: tuple[str, str] = ("_x", "_y"),
        nulls_equal: bool = False,
    ) -> DataFrame:
        """pandas ``merge``: :meth:`join` with ``right`` first, then ``how``.
        With no key given, joins on the columns both frames share. A non-key
        column present on both sides is suffixed on BOTH sides with
        ``suffixes``, as pandas does. A null key does not match another null
        key unless ``nulls_equal=True``, which is the pandas rule."""
        if on is None and left_on is None and right_on is None:
            shared = [c for c in self._names() if c in right]
            if not shared:
                raise ValueError(
                    "merge(): no common columns to join on; pass on= or left_on=/right_on="
                )
            on = shared
        lkeys = as_names(on) if on is not None else as_names(left_on)
        rkeys = as_names(on) if on is not None else as_names(right_on)
        collide = [c for c in self._names() if c in right and c not in lkeys and c not in rkeys]
        left = self
        if collide:
            left = self.rename({c: c + suffixes[0] for c in collide})
            right = right.rename({c: c + suffixes[1] for c in collide})
        return left.join(
            right,
            on=on,
            how=how,
            left_on=left_on,
            right_on=right_on,
            suffix=suffixes[1],
            nulls_equal=nulls_equal,
        )

    def compare_agg(self, variant: DataFrame, on: int | str | Sequence[str] = 1) -> DataFrame:
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
            names = list(self.column_names)
            if on < 1 or on > len(names):
                raise ValueError(
                    f"compare_agg: 'on' count {on} is out of range for a {len(names)}-column frame"
                )
            n_key = on
        else:
            keys = as_names(on)
            if not keys:
                raise ValueError("compare_agg: 'on' must name at least one key column")
            n_key = len(keys)
        return _bound(_ext.DataFrame.compare_agg(self, variant, n_key))

    def asof(
        self,
        other: DataFrame,
        on: str,
        by: str | Sequence[str] | None = None,
        direction: Literal["backward", "forward", "nearest"] = "backward",
        tolerance: float | None = None,
    ) -> DataFrame:
        """Temporal (as-of) join: match each row to the nearest ``other`` row by
        the time column ``on``, within optional equi-key partition ``by``.

        ``direction`` is ``"backward"`` (largest ts <= self.ts), ``"forward"``
        (smallest ts >= self.ts), or ``"nearest"``. ``tolerance`` (if given)
        bounds the allowed time distance. Output is all left columns then the
        right value columns; unmatched left rows get null right values."""
        return _bound(_ext.DataFrame.asof(self, other, on, as_names(by), direction, tolerance))

    def interval(
        self,
        other: DataFrame,
        point: str,
        lo: str,
        hi: str,
        by: str | Sequence[str] | None = None,
        outer: bool = False,
    ) -> DataFrame:
        """Point-in-range join: match each row (the ``point`` column) to every
        ``other`` row whose closed span ``[lo, hi]`` contains it, within optional
        equi-key partition ``by``.

        One output row per (left, matching right); ``outer=True`` also emits an
        unmatched left row once with null right values. Output is all left
        columns then the right value columns."""
        return _bound(
            _ext.DataFrame.interval(self, other, point, lo, hi, as_names(by), bool(outer))
        )

    def unnest(self, column: str, keep_empty: bool = False) -> DataFrame:
        """UNNEST / EXPLODE the list-typed ``column`` into one row per element,
        repeating every other column.

        ``column`` names a ``list<utf8>``, ``list<int64>``, or
        ``list<struct<...>>`` column; a ``list<struct>`` flattens its fields into
        columns (named by the struct fields). Other columns pass through by
        value. An empty or null list drops the row unless ``keep_empty=True``
        (then one row with the exploded column(s) null). See :meth:`explode` for
        the single-list variant that always keeps the row."""
        return _bound(_ext.DataFrame.unnest(self, column, bool(keep_empty)))

    def top_k(self, k: int, *, by: str | Sequence[str], reverse: bool = False) -> DataFrame:
        """The ``k`` rows with the largest ``by`` (the smallest with
        ``reverse=True``), best first; a null never ranks (polars ``top_k``).
        Several ``by`` columns rank lexicographically."""
        if isinstance(by, str):
            return _bound(_ext.DataFrame.topk(self, by, k, not reverse))
        cols = list(by)
        present = self[cols[0]].is_not_null()
        for c in cols[1:]:
            present = present & self[c].is_not_null()
        return self.filter(present).sort(cols, descending=not reverse).head(k)

    def bottom_k(self, k: int, *, by: str | Sequence[str], reverse: bool = False) -> DataFrame:
        """The ``k`` rows with the smallest ``by``, smallest first (polars
        ``bottom_k``): :meth:`top_k` with ``reverse`` flipped."""
        return self.top_k(k, by=by, reverse=not reverse)

    def sort_values(
        self,
        by: str | Sequence[str],
        ascending: bool | Sequence[bool] = True,
        na_position: Literal["first", "last"] = "last",
    ) -> DataFrame:
        """The pandas ``sort_values``: :meth:`sort` with ``ascending`` (a flag
        or one per column in ``by``) and ``na_position`` (nulls last by
        default, unlike :meth:`sort`)."""
        if na_position not in ("first", "last"):
            raise ValueError("na_position must be 'first' or 'last'")
        if isinstance(ascending, bool):
            descending: bool | list[bool] = not ascending
        else:
            descending = [not a for a in ascending]
        return self.sort(by, descending=descending, nulls_last=na_position == "last")

    def sort(
        self,
        by: str | Sequence[str],
        *,
        descending: bool | Sequence[bool] = False,
        nulls_last: bool = False,
    ) -> DataFrame:
        """Order rows by one column (``by`` a name) or lexicographically by
        several (``by`` a list); ``descending`` is a flag or one per column.
        The rows whose key is null come first, or last with
        ``nulls_last=True`` (the polars keywords and default); a NaN sorts
        after every number and apart from the nulls."""
        names = [by] if isinstance(by, str) else list(by)
        flags = descending if isinstance(descending, bool) else list(descending)
        if len(names) == 1 and isinstance(flags, bool) and isinstance(nulls_last, bool):
            return _bound(_ext.DataFrame.sort_by(self, names[0], flags, nulls_last))
        return _bound(_ext.DataFrame.sort_by_multi(self, names, flags, nulls_last))

    def union(self, *others: DataFrame) -> DataFrame:
        """Vertically concatenate with ``others`` and drop duplicate rows (SQL
        UNION); use :meth:`concat` for UNION ALL (keep duplicates)."""
        cat = _ext.DataFrame.concat(self, *[o for o in others])
        return cat.unique()

    # -- native engine ops (SIMD/relational kernels, wrapped from the C extension) --
    def filter(
        self,
        predicate: Series | Expr | None = None,
        *,
        items: Sequence[str] | None = None,
        like: str | None = None,
        regex: str | None = None,
    ) -> DataFrame:
        """Rows or columns. With ``predicate`` (a Bool Series, or a column expression
        evaluated against this frame): the rows where it is true. With the
        pandas keywords ``items`` / ``like`` / ``regex``: the columns whose
        name is listed / contains ``like`` / matches ``regex``."""
        keyed = [k for k in (items, like, regex) if k is not None]
        if len(keyed) > 1 or (keyed and predicate is not None):
            raise TypeError("filter() takes a row mask, or one of items= / like= / regex=")
        if items is not None:
            return _bound(
                _ext.DataFrame.select(
                    self, *[c for c in items if c in self], *_ext.DataFrame.index_names(self)
                )
            )
        if like is not None:
            return _bound(
                _ext.DataFrame.select(
                    self,
                    *[c for c in self.columns if like in c],
                    *_ext.DataFrame.index_names(self),
                )
            )
        if regex is not None:
            import re

            pat = re.compile(regex)
            return _bound(
                _ext.DataFrame.select(
                    self,
                    *[c for c in self.columns if pat.search(c)],
                    *_ext.DataFrame.index_names(self),
                )
            )
        if predicate is None:
            raise TypeError("filter() needs a row mask or one of items= / like= / regex=")
        if not isinstance(predicate, Series):
            predicate = predicate.apply(self)
        return _bound(_ext.DataFrame.filter(self, predicate))

    def select(self, *items: SelectItem) -> DataFrame:
        """Project to ``items``: column names, bare column expressions, or
        named expressions (``(col("a") * 2).alias("b")``), in that order; a
        set of aggregate expressions (``col("a").sum()``) gives the one-row
        frame ``group_by().agg(...)`` does, each unaliased aggregate of a
        column named after that column (the polars ``select``)."""
        from .groupby import GroupBy

        if items and all(isinstance(i, Agg) for i in items):
            return GroupBy(self, []).agg(*select_aggs(items))
        pairs = resolve_selectors(items)
        out = self
        for name, expr in pairs:
            if expr is not None:
                out = out.assign(**{name: expr})
        return _ext.DataFrame.select(out, *[name for name, _ in pairs])

    def rename(
        self,
        mapping: dict[str, str] | None = None,
        *,
        columns: dict[str, str] | None = None,
    ) -> DataFrame:
        """Rename columns by ``{old: new}`` (``columns=`` is the pandas spelling)."""
        if mapping is None:
            mapping = columns
        if mapping is None:
            raise TypeError("rename() needs a {old: new} mapping")
        return _bound(_ext.DataFrame.rename(self, mapping))

    def with_column(self, name: str, col: Series) -> DataFrame:
        return _bound(_ext.DataFrame.with_column(self, name, col))

    def gather(self, indices: Series | Sequence[int]) -> DataFrame:
        """The rows at ``indices`` (a Series or a list; any order, repeats
        allowed); ``take`` is the pandas name."""
        return _bound(_ext.DataFrame.take(self, _indices(indices)))

    def head(self, n: int = 5) -> DataFrame:
        return _bound(_ext.DataFrame.head(self, n))

    def tail(self, n: int = 5) -> DataFrame:
        return _bound(_ext.DataFrame.tail(self, n))

    def reverse(self) -> DataFrame:
        return _bound(_ext.DataFrame.reverse(self))

    def drop_nulls(self) -> DataFrame:
        return _bound(_ext.DataFrame.drop_nulls(self))

    def fill_null(self, value: int | float) -> DataFrame:
        return _bound(_ext.DataFrame.fill_null(self, value))

    def unique(self, subset: str | Sequence[str] | None = None) -> DataFrame:
        """Distinct rows, keeping the first occurrence, keyed on every column
        (the index columns included) or on ``subset``. The pandas name is
        ``drop_duplicates``, which keys on the shown columns."""
        return _bound(_ext.DataFrame.unique(self, subset))

    def _shown_subset(self, subset: str | Sequence[str] | None, who: str) -> list[str] | None:
        """``subset`` as a list of shown columns: a name that is not in
        :attr:`columns` (an index column included) is a ``KeyError``, as in
        pandas. ``None`` stays ``None``."""
        if subset is None:
            return None
        names = as_names(subset)
        shown = set(self.columns)
        missing = [n for n in names if n not in shown]
        if missing:
            raise KeyError(f"{who}: no column named {missing[0]!r} in the columns")
        return names

    def drop_duplicates(self, subset: str | Sequence[str] | None = None) -> DataFrame:
        """The pandas ``drop_duplicates``: distinct rows keyed on ``subset`` or
        on :attr:`columns` (the index columns are not compared), keeping the
        first occurrence and the index with its row. A ``subset`` name that is
        not a shown column is a ``KeyError``."""
        names = self._shown_subset(subset, "drop_duplicates")
        if names is None and _ext.DataFrame.index_names(self):
            names = self.columns
        return _bound(_ext.DataFrame.unique(self, names))

    def dropna(
        self,
        axis: int | Literal["index", "columns"] = 0,
        how: Literal["any", "all"] | None = None,
        thresh: int | None = None,
        subset: str | Sequence[str] | None = None,
    ) -> DataFrame:
        """The pandas ``dropna`` over :attr:`columns` (a null in an index column
        does not drop its row). ``axis=0`` drops the rows with a null in
        ``subset`` (every shown column by default): ``how="any"`` (the default)
        a row with one null, ``"all"`` a row of only nulls, or ``thresh`` a row
        with fewer than ``thresh`` values. ``axis=1`` drops the columns with a
        null (``how="all"``: all null, ``thresh``: fewer than that many values),
        and takes no ``subset``, which would name row labels. ``how`` and
        ``thresh`` are exclusive (``TypeError``); a ``subset`` name that is not a
        shown column is a ``KeyError``."""
        if axis in (0, "index"):
            by_row = True
        elif axis in (1, "columns"):
            by_row = False
        else:
            raise ValueError(f"dropna: no axis named {axis!r}")
        if how is not None and thresh is not None:
            raise TypeError("dropna: you cannot set both the how and thresh arguments")
        if how not in (None, "any", "all"):
            raise ValueError(f"dropna: invalid how option: {how}")
        index = _ext.DataFrame.index_names(self)
        names = self._shown_subset(subset, "dropna")
        if not by_row:
            if names is not None:
                raise KeyError(
                    f"dropna: subset {names} with axis=1 names row labels, which a frame has none of"
                )
            rows = len(self)
            kept = []
            for name in self.columns:
                values = rows - self._col(name).null_count
                keep = (
                    values >= thresh
                    if thresh is not None
                    else (values > 0 or rows == 0 if how == "all" else values == rows)
                )
                if keep:
                    kept.append(name)
            return _bound(_ext.DataFrame.select(self, *kept, *index))
        if names is None:
            names = self.columns
            if not index and how is None and thresh is None:
                return _bound(_ext.DataFrame.drop_nulls(self))
        if not names:
            return self
        counts: Series | None = None
        for name in names:
            present = self._col(name).notna().astype("int64")
            counts = present if counts is None else counts + present
        assert counts is not None
        if thresh is not None:
            mask = counts >= thresh
        elif how == "all":
            mask = counts >= 1
        else:
            mask = counts == len(names)
        return self.filter(mask)

    def fillna(
        self,
        value: int | float | Mapping[str, int | float] | None = None,
        *,
        axis: int | Literal["index", "columns"] | None = None,
    ) -> DataFrame:
        """The pandas ``fillna``: nulls of :attr:`columns` become ``value``, a
        scalar or a ``{column: value}`` mapping (a name that is not a shown
        column is ignored, as in pandas); the index columns are left as they
        are. A mapping with ``axis=1`` is refused (``NotImplementedError``, as
        pandas); a scalar fills the same cells for either axis."""
        if axis not in (None, 0, 1, "index", "columns"):
            raise ValueError(f"fillna: no axis named {axis!r}")
        if value is None:
            raise ValueError("fillna: must specify a fill value")
        names = _ext.DataFrame.index_names(self)
        if isinstance(value, Mapping):
            if axis in (1, "columns"):
                raise NotImplementedError("fillna: a mapping value with axis=1 is not supported")
            out: DataFrame = self
            shown = set(self.columns)
            for name, fill in value.items():
                if name in shown:
                    out = _bound(
                        _ext.DataFrame.with_column(out, name, self._col(name).fillna(fill))
                    )
            return _bound(_ext.DataFrame.share(out)) if out is self else out
        if not names:
            return _bound(_ext.DataFrame.fill_null(self, value))
        out = _ext.DataFrame.fill_null(self._data_only(), value)
        for name in names:
            out = _ext.DataFrame.with_column(out, name, self._col(name))
        return _bound(_ext.DataFrame.with_index(out, names))

    def lazy(self) -> LazyFrame:
        """Start a deferred query over this batch. Ops are recorded and nothing
        runs until :meth:`LazyFrame.collect`."""
        return _ext.DataFrame.lazy(self)

    def sort_by_multi(
        self,
        names: list[str],
        descending: bool | Sequence[bool] = False,
        nulls_last: bool | Sequence[bool] = True,
    ) -> DataFrame:
        """A single flag broadcasts to every key; a sequence maps one flag per
        name (size must be 1 or ``len(names)``). ``nulls_last`` places the
        nulls of each key."""
        return _bound(_ext.DataFrame.sort_by_multi(self, names, descending, nulls_last))

    def sample(self, n: int, seed: int = 0, *, random_state: int | None = None) -> DataFrame:
        """A deterministic ``n``-row sample (``random_state`` is the pandas
        spelling of ``seed``)."""
        if random_state is not None:
            seed = random_state
        return _bound(_ext.DataFrame.sample(self, n, seed))

    def with_row_index(self, name: str = "index") -> DataFrame:
        return _bound(_ext.DataFrame.with_row_index(self, name))

    def describe(self) -> DataFrame:
        """The summary of every column; the index columns are not summarised,
        as pandas does not."""
        return _bound(_ext.DataFrame.describe(self._data_only()))

    def null_count(self) -> DataFrame:
        return _bound(_ext.DataFrame.null_count(self))

    def is_duplicated(self) -> Series:
        return _ext.DataFrame.is_duplicated(self)

    def is_unique(self) -> Series:
        return _ext.DataFrame.is_unique(self)

    def slice(self, offset: int, length: int) -> DataFrame:
        return _bound(_ext.DataFrame.slice(self, offset, length))

    def sort_by(self, name: str, descending: bool = False, nulls_last: bool = True) -> DataFrame:
        return _bound(_ext.DataFrame.sort_by(self, name, descending, nulls_last))

    def topk(self, name: str, k: int, largest: bool = True) -> DataFrame:
        return _bound(_ext.DataFrame.topk(self, name, k, largest))

    def concat(
        self, *others: DataFrame, how: Literal["vertical", "diagonal"] = "vertical"
    ) -> DataFrame:
        """Vertically concatenate with ``others`` (UNION ALL). ``how='vertical'``
        requires a shared schema; ``how='diagonal'`` unions columns, null-filling
        those absent from a part and promoting a mixed-numeric column to float."""
        return _bound(_ext.DataFrame.concat(self, *[x for x in others], how=how))

    def unpivot(
        self,
        id_vars: str | list[str] | None = None,
        value_vars: str | list[str] | None = None,
        *,
        index: str | list[str] | None = None,
        on: str | list[str] | None = None,
    ) -> DataFrame:
        """Wide to long: keep ``id_vars``, stack ``value_vars`` into a
        ``variable`` / ``value`` pair. ``index`` / ``on`` are the polars
        spellings of the same two arguments."""
        ids = id_vars if id_vars is not None else index
        vals = value_vars if value_vars is not None else on
        if vals is None:
            raise TypeError("unpivot() needs value_vars (polars: on=)")
        return _bound(_ext.DataFrame.unpivot(self, as_names(ids), as_names(vals)))

    def melt(
        self,
        id_vars: str | list[str] | None = None,
        value_vars: str | list[str] | None = None,
        *,
        index: str | list[str] | None = None,
        on: str | list[str] | None = None,
    ) -> DataFrame:
        return self.unpivot(id_vars, value_vars, index=index, on=on)

    def explode(self, column: str) -> DataFrame:
        return _bound(_ext.DataFrame.explode(self, column))

    def to_dummies(self, column: str) -> DataFrame:
        return _bound(_ext.DataFrame.to_dummies(self, column))

    def pivot(
        self,
        index: str,
        columns: str | None = None,
        values: str | None = None,
        agg: PivotAgg = "first",
        *,
        on: str | None = None,
        aggregate_function: PivotAgg | None = None,
        aggfunc: PivotAgg | None = None,
    ) -> DataFrame:
        """Long to wide (pandas argument names). ``on`` / ``aggregate_function``
        are the polars spellings of ``columns`` / ``agg``; ``aggfunc`` the
        pandas ``pivot_table`` one."""
        cols = columns if columns is not None else on
        if cols is None or values is None:
            raise TypeError("pivot() needs index, columns (polars: on=) and values")
        agg_name = aggregate_function or aggfunc or agg
        return _bound(_ext.DataFrame.pivot(self, index, cols, values, agg_name))

    def group_by_dynamic(
        self,
        time_col: str,
        every: int,
        period: int | None = None,
        aggs: Sequence[str | Agg | tuple[str, str, list[str]]] | None = None,
        origin: int | Literal["min"] | None = None,
    ) -> DataFrame:
        """Tumbling/sliding time-window aggregation. ``origin`` anchors the
        window grid at ``origin + k*every`` (default: the classic ts-floored
        grid); pass a window's begin, or ``"min"`` to align buckets to the
        minimum timestamp (the frame-native ``time_bucket("min")``)."""
        from .lazyframe import dynamic_aggs

        return _bound(
            _ext.DataFrame.group_by_dynamic(
                self, time_col, every, period, dynamic_aggs(self, aggs), origin
            )
        )

    def eval(self, expr: str) -> Series | DataFrame:
        """Evaluate Python expressions over the columns (pandas ``eval``).
        ``df.eval("a / b")`` gives a Series; ``df.eval("m = a / b")`` gives a new
        frame with column ``m`` added or replaced. Several lines run in order, each
        seeing the columns the lines before it made; every line but the last is an
        assignment, and a last line that is an expression returns its value. The
        text is parsed with ``ast`` and each line lowered by the same code as
        :meth:`apply`'s source tier into one column expression; it is never run as
        Python code.

        Supported (what that lowering supports): column names; number literals;
        ``+ - * /`` (a number may be on either side); ``//`` and ``%`` (integer
        columns give integers, as Python does); ``**`` with a constant exponent
        from 0 to 8 or 0.5; unary ``- +``; ``< <= > >= == !=`` (chains too);
        ``and``, ``or``, ``not`` and the mask operators ``& | ~``; ``x if c else
        y``; ``in`` and ``not in`` a list of constants; ``x is None`` and ``x is not
        None``; ``abs``, ``min``, ``max``, ``int``, ``float``, ``round`` (whole
        numbers) and ``len``; ``math.sqrt``, ``log``, ``exp``, ``floor``, ``ceil``;
        and the string methods ``lower``, ``upper``, ``strip``, ``lstrip``,
        ``rstrip``, ``startswith``, ``endswith`` and ``replace`` with literal
        arguments; and the pandas methods ``fillna`` (a number or a column),
        ``isna``, ``isnull``, ``notna``, ``notnull``, ``abs``, ``clip`` (``lower``,
        ``upper`` or both), ``round`` (``decimals``), ``where`` and ``mask`` (with
        or without ``other``) and ``str.contains`` (``regex``, ``case``), with the
        keyword spellings pandas takes; any other keyword raises, naming it. Nulls propagate as the column operators propagate them. ``&``,
        ``|`` and ``~`` combine and negate masks (comparisons), as ``and``, ``or``
        and ``not`` do. An unknown column raises ``KeyError``; anything else raises
        ``TranspileError`` naming the first unsupported construct (``@name``,
        attribute access, ...)."""
        from ._transpile import NULL_COLUMN, TranspileError, eval_statements, transpile_expr

        statements = eval_statements(expr)
        if not statements:
            raise TranspileError("eval needs an expression")
        frame = self
        for i, text in enumerate(statements):
            all_names = frame._names()
            int_inputs = {c: frame[c].dtype.name.startswith(("INT", "UINT")) for c in all_names}
            target, column_expr = transpile_expr(text, all_names, int_inputs)
            # `where` / `mask` without a replacement read an all-null column (the
            # expression language has no null literal); it is added for this line only.
            with_null = NULL_COLUMN in Columnar(column_expr).columns
            work = (
                frame.with_columns(
                    **{NULL_COLUMN: _ext._series_nulls(int(DType.FLOAT64), len(frame))}
                )
                if with_null
                else frame
            )
            if target is None:
                if i != len(statements) - 1:
                    raise TranspileError("only the last line of eval can be an expression")
                return column_expr.apply(work)
            frame = work.with_columns(**{target: column_expr})
            if with_null:
                frame = frame.select(*[c for c in frame._names() if c != NULL_COLUMN])
        return frame

    @overload
    def group_by(
        self,
        key: str | Series | Sequence[str | Series] | None = None,
        *,
        dropna: bool = True,
    ) -> GroupBy: ...
    @overload
    def group_by(
        self,
        key: str | Series | Sequence[str | Series] | None,
        agg: str | Agg,
        /,
        *aggs: str | Agg,
        dropna: bool = True,
    ) -> DataFrame: ...
    def group_by(
        self,
        key: str | Series | Sequence[str | Series] | None = None,
        *aggs: str | Agg,
        dropna: bool = True,
    ) -> DataFrame | GroupBy:
        """Group by one column name or several (a composite key), then either
        run inline legacy specs or return a lazy :class:`GroupBy` for
        ``.agg(...)``. With no key the whole frame is one group: ``.agg(...)``
        gives a one-row frame of any aggregates (``col("y").corr(col("x"))``
        included) and ``.sum()`` / ``.mean()`` / ... broadcast one. A row
        whose key is null is left out, as pandas; ``dropna=False`` keeps the
        null keys as their own group (polars), the key null in the result."""
        from .groupby import GroupBy

        given = [] if key is None else [key] if isinstance(key, (str, Series)) else list(key)
        if not given:
            whole = GroupBy(self, [])
            return whole.agg(*aggs) if aggs else whole
        # A Series key (``df.groupby(df["ts"] // 1000)``) becomes a column
        # named ``key`` (``key<i>`` among several), as a computed expression
        # key is.
        source = self
        keys: list[str] = []
        for i, k in enumerate(given):
            if isinstance(k, Series):
                name = "key" if len(given) == 1 else f"key{i}"
                source = source.with_column(name, k)
                keys.append(name)
            elif k in self:
                keys.append(k)
            else:
                raise KeyError(f"group_by: no column named {k!r}")
        rows: Series | None = None
        if dropna:
            for k in keys:
                if source._col(k).null_count:
                    if rows is None:
                        rows = DataFrame({"v": source._col(k)}).with_row_index("_i")["_i"]
                    present = source._col(k).notna()
                    rows = rows.filter(present)
                    source = source.filter(present)
        if not aggs:
            # The Python object over this wrapper, so the index names carry.
            grouped = GroupBy(source, keys)
            grouped._rows = rows
            return grouped
        return _ext.DataFrame.group_by(source, *[k for k in keys], *[a for a in aggs])

    def reduce(self, agg: str) -> DataFrame:
        """One row: the engine aggregate named ``agg`` (``sum``, ``mean``,
        ``min``, ``max``, ``count_valid``, ``var``, ``std``, ``skew``,
        ``kurt``, ``sumsq``, ``first``, ``last``, ``prod``) over every
        eligible column, each under its own name. The grouped form is
        :meth:`group_by` ``(keys).reduce(agg)``. The index columns are not
        reduced, as pandas does not."""
        return _bound(_ext.DataFrame.reduce(self._data_only(), agg))

    def _data_only(self) -> DataFrame:
        """This frame without its index columns (itself when it has no index)."""
        if not _ext.DataFrame.index_names(self):
            return self
        return _bound(_ext.DataFrame.select(self, *self.columns))

    def agg(
        self, func: AggForm | None = None, axis: int | str = 0, **named: AggKeyword
    ) -> DataFrame | Series:
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
        from .groupby import GroupBy

        return GroupBy(self._data_only(), []).agg(*([] if func is None else [func]), **named)

    def _reduce_rows(self, func: str) -> Series:
        """One value per row across the numeric columns, nulls skipped: the
        pandas ``axis=1`` reductions, as engine column ops (fill, add, the
        select kernel) and no per-row Python."""
        cols: list[Series] = [_ext.DataFrame.__getitem__(self, name) for name in self.columns]
        cols = [c for c in cols if c.dtype.is_numeric]
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

    # The pandas ``df.sum()`` family: one row, one aggregate per column.
    def sum(self) -> DataFrame:
        return self.reduce(agg="sum")

    def mean(self) -> DataFrame:
        return self.reduce(agg="mean")

    def min(self) -> DataFrame:
        return self.reduce(agg="min")

    def max(self) -> DataFrame:
        return self.reduce(agg="max")

    def count(self) -> DataFrame:
        return self.reduce(agg="count_valid")

    def var(self) -> DataFrame:
        return self.reduce(agg="var")

    def std(self) -> DataFrame:
        return self.reduce(agg="std")

    def skew(self) -> DataFrame:
        """The pandas unbiased skewness of every column, one row (see :meth:`Series.skew`)."""
        return self.reduce("skew_unbiased")

    def kurt(self) -> DataFrame:
        """The pandas unbiased excess kurtosis of every column, one row."""
        return self.reduce("kurt_unbiased")

    # BEGIN GENERATED aliases from _aliases.py by scripts/gen_pyi.py
    if TYPE_CHECKING:
        aggregate = agg
        astype = cast
        take = gather
        divide = FramePandasMixin.div
        truediv = FramePandasMixin.div
        multiply = FramePandasMixin.mul
        subtract = FramePandasMixin.sub
        product = FramePandasMixin.prod
        rtruediv = FramePandasMixin.rdiv
        map = FramePandasMixin.applymap
        isnull = FramePandasMixin.isna
        notnull = FramePandasMixin.notna
        backfill = FramePandasMixin.bfill
        pad = FramePandasMixin.ffill
        limit = head
        clone = FramePandasMixin.copy
        select_seq = select
        with_columns_seq = with_columns
        pass
    # END GENERATED aliases


def _frame_of(columns: list[tuple[str, object]], read: Callable[[Any], Series]) -> DataFrame:
    """A frame of ``read(value)`` for each ``(name, value)``."""
    return DataFrame(
        _ext._dataframe_from_columns([n for n, _ in columns], [read(v) for _, v in columns])
    )


def _native_columns(
    mapping: Mapping[str, object],
) -> tuple[list[str], list[Series]] | None:
    """The names and native columns of a mapping whose values are all Series,
    lists of scalars, NumPy arrays or pandas and polars Series that import
    without Arrow; None otherwise."""
    names: list[str] = []
    cols: list[Series] = []
    for name, value in mapping.items():
        if isinstance(value, Series):
            cols.append(value)
        elif isinstance(value, (list, tuple)):
            items = list(value)
            # no value to infer a type from: a string column of nulls, as from_pandas gives
            kind = (int(DType.STRING),) if all(v is None for v in items) else ()
            try:
                cols.append(_ext._series_from_list(items, *kind))
            except TypeError:
                return None
        elif type(value).__module__.startswith("pandas") and type(value).__name__ == "Series":
            try:
                cols.append(Series.from_pandas(cast(Any, value)))
            except TypeError:
                return None
        elif type(value).__module__.startswith("polars") and type(value).__name__ == "Series":
            try:
                cols.append(Series.from_polars(cast(Any, value)))
            except TypeError:
                return None
        elif type(value).__module__ == "numpy" and type(value).__name__ == "ndarray":
            try:
                cols.append(Series.from_numpy(cast(Any, value)))
            except (TypeError, BufferError, RuntimeError, ImportError):
                return None
        else:
            return None
        names.append(name)
    return names, cols


def _dataframe_from_bytes(data: bytes) -> DataFrame:
    """The DataFrame a pickle holds (``DataFrame.to_bytes`` of the native frame format)."""
    return DataFrame(_ext._dataframe_from_bytes(data))


def _dataframe_from_arrow(table: pa.Table) -> DataFrame:
    """Import a pyarrow Table into a native DataFrame wrapper, with the row
    index its schema metadata names: ``dftracer.utils.index``, else the
    ``index_columns`` of its pandas metadata (a ``RangeIndex`` is none)."""
    frame = DataFrame(_arrow_native("_dataframe_from_arrow")(table))
    names = _arrow_index_names(table.schema.metadata, frame)
    if not names:
        return frame
    try:
        return _bound(_ext.DataFrame.with_index(frame, names))
    except ValueError:
        return frame


def _arrow_index_names(metadata: Mapping[bytes, bytes] | None, frame: DataFrame) -> list[str]:
    """The index column names a pyarrow schema metadata carries that are
    columns of ``frame``, without repeats."""
    if not metadata:
        return []
    import json

    listed: object = None
    if b"dftracer.utils.index" in metadata:
        listed = json.loads(metadata[b"dftracer.utils.index"])
    elif b"pandas" in metadata:
        listed = json.loads(metadata[b"pandas"]).get("index_columns")
    names: list[str] = []
    for name in listed if isinstance(listed, list) else ():
        if isinstance(name, str) and name in frame and name not in names:
            names.append(name)
    return names


_install_aliases("DataFrame", DataFrame)
_ext._bind_types(DataFrame=DataFrame)
