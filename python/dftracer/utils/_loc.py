# SPDX-License-Identifier: MIT
"""The pandas ``iloc`` / ``loc`` / ``at`` / ``iat`` accessors and ``resample``
over the column model: the index is a named column (``set_index``) or the row
position, so every read is ``slice`` / ``take`` / ``filter`` / ``select`` and
every write is ``with_column`` over a masked ``where``.

Frames stay immutable: ``df.loc[mask, "c"] = 0`` builds a new native frame
(only column ``c`` is rebuilt; the rest is shared) and rebinds the Python
handle, the pandas 3 copy-on-write behaviour. A handle taken earlier
(``df2 = df.head(3)``) never changes; a second name for the same handle
(``df2 = df``) sees the write, as in pandas.
"""

from __future__ import annotations

import builtins
import datetime
import sys
from typing import TYPE_CHECKING, Sequence, cast

from . import dftracer_utils_ext as _ext
from ._apply import Row
from .agg import AggForm, AggKeyword, reduce_specs
from .enums import DType
from .expr import Expr, col, collect_columns, lit, when
from .series import Series

if TYPE_CHECKING:
    import pyarrow as pa

    from .dataframe import DataFrame
    from .expr import Expr
    from .lazyframe import LazyFrame

_ROW = "__dftu_loc_row__"
# A plan has no length until it runs; an open slice end / a position bound.
_LAZY_END = 2**62


class Rows:
    """A row selection, normalized: every input spelling lands in one of
    ``all``, ``slice`` (offset, length), ``take`` (positions) or ``mask``
    (a Bool Series or an Expr)."""

    __slots__ = ("kind", "offset", "length", "positions", "mask", "one", "drop")

    def __init__(
        self,
        kind: str,
        offset: int = 0,
        length: int = 0,
        positions: list[int] | None = None,
        mask: object = None,
        one: bool = False,
        drop: tuple[int, ...] = (),
    ) -> None:
        self.kind = kind
        self.offset = offset
        self.length = length
        self.positions = positions or []
        self.mask = mask
        # A label lookup: whether the key was one whole label (not a list, a
        # slice or a part of a MultiIndex label), and the index levels the
        # result no longer has (the levels a partial key named).
        self.one = one
        self.drop = drop

    @classmethod
    def all(cls) -> Rows:
        return cls("all")

    @classmethod
    def slice(cls, offset: int, length: int) -> Rows:
        return cls("slice", offset=offset, length=length)

    @classmethod
    def take(cls, positions: list[int], one: bool = False, drop: tuple[int, ...] = ()) -> Rows:
        return cls("take", positions=positions, one=one, drop=drop)

    @classmethod
    def masked(cls, mask: object) -> Rows:
        return cls("mask", mask=mask)


def _is_all(key: object) -> bool:
    return key is None or (
        isinstance(key, builtins.slice)
        and key.start is None
        and key.stop is None
        and key.step is None
    )


def _bool_list(values: Sequence[object]) -> bool:
    return len(values) > 0 and all(isinstance(v, bool) for v in values)


def _positions(n: int, values: Sequence[object]) -> list[int]:
    out: list[int] = []
    for v in values:
        if isinstance(v, bool) or not isinstance(v, int):
            raise TypeError("iloc: positions must be ints")
        p = v + n if v < 0 else v
        if p < 0 or p >= n:
            raise IndexError(f"iloc: position {v} is out of bounds for {n} rows")
        out.append(p)
    return out


def _is_bool(series: Series) -> bool:
    return series.dtype == DType.BOOL


def rows_by_position(n: int, key: object) -> tuple[Rows, bool]:
    """``iloc`` rows: the selection and whether it names one row."""
    if _is_all(key):
        return Rows.all(), False
    if isinstance(key, bool):
        raise TypeError("iloc: a bool is not a position")
    if isinstance(key, int):
        return Rows.slice(_positions(n, [key])[0], 1), True
    if isinstance(key, builtins.slice):
        r = range(n)[key]
        if r.step == 1:
            return Rows.slice(r.start, len(r)), False
        return Rows.take(list(r)), False
    if isinstance(key, Series):
        if _is_bool(key):
            return Rows.masked(key), False
        return Rows.take(_positions(n, key.to_list())), False
    if isinstance(key, (list, tuple, range)):
        values = list(key)
        if _bool_list(values):
            return Rows.masked(Series.from_list(values)), False
        return Rows.take(_positions(n, values)), False
    raise TypeError(f"iloc: cannot select rows with {type(key).__name__}")


def _numeric_bounds(key: builtins.slice) -> tuple[float | None, float | None]:
    if key.step is not None:
        raise TypeError("loc: a label slice takes no step")
    for bound in (key.start, key.stop):
        if bound is not None and (isinstance(bound, bool) or not isinstance(bound, (int, float))):
            raise TypeError("loc: a label slice needs numeric bounds")
    return key.start, key.stop


class Labels:
    """The row labels of a frame or Series, looked up through the hash the
    native object builds on its first lookup and keeps (see
    ``Series._locate``). A frame or Series with no labels has the positions
    ``0 .. n-1`` as its one level."""

    __slots__ = ("frame", "series", "names")

    def __init__(self, native: DataFrame | Series) -> None:
        self.frame: DataFrame | None = None if isinstance(native, Series) else native
        self.series: Series | None = native if isinstance(native, Series) else None
        self.names: list[str] = native.index_names()

    @property
    def depth(self) -> int:
        return max(len(self.names), 1)

    def locate(self, keys: list[Series]) -> tuple[list[int], list[int]]:
        if self.frame is not None:
            return self.frame._locate(keys)
        assert self.series is not None
        return self.series._locate(keys)

    def locate_range(self, start: list[Series], stop: list[Series]) -> tuple[int, int]:
        if self.frame is not None:
            return self.frame._locate_range(start, stop)
        assert self.series is not None
        return self.series._locate_range(start, stop)

    def level(self, i: int) -> Series | None:
        """Label column ``i``; None for positions."""
        if not self.names:
            return None
        if self.frame is not None:
            return _ext.DataFrame.__getitem__(self.frame, self.names[i])
        assert self.series is not None
        return self.series._index_level(i)

    def dtype(self, i: int) -> DType:
        level = self.level(i)
        return DType.INT64 if level is None else level.dtype


# Label dtypes whose key columns compare by value in the native lookup. The
# others (dates, times, timestamps, durations, decimals) compare by their own
# type, so a key is converted to the label's type first.
_PLAIN = frozenset(d for d in DType if d.is_numeric) | {
    DType.BOOL,
    DType.FLOAT16,
    DType.STRING,
    DType.LARGE_STRING,
    DType.BINARY,
    DType.LARGE_BINARY,
}


class _Unkeyable(Exception):
    """Label values that do not make one key column (mixed types, or a type
    that cannot be the level's)."""


_TEXT = frozenset({DType.STRING, DType.LARGE_STRING, DType.BINARY, DType.LARGE_BINARY})
_NARROW_FLOATS = frozenset({DType.FLOAT16, DType.FLOAT32})


def _awareness_differs(values: list[object], tz: object) -> bool:
    """Whether a datetime key is timezone aware where the level is not, or the
    other way round (pandas raises ``KeyError`` for both)."""
    return any(
        isinstance(v, datetime.datetime) and (v.tzinfo is not None) != (tz is not None)
        for v in values
    )


def _stamp_type(values: list[object], arrow_type: pa.DataType) -> pa.DataType:
    """The type a key column of timestamps is built in. A key keeps all its
    precision (nanoseconds when a value has them, else microseconds) when the
    labels are coarser, and the engine compares them in the finer unit, so a key
    that is not a whole tick of the labels finds nothing instead of rounding
    into a match."""
    unit = getattr(arrow_type, "unit", None)
    if unit not in ("s", "ms", "us") or not hasattr(arrow_type, "tz"):
        return arrow_type
    import pyarrow as pa

    has_nanos = any(getattr(v, "nanosecond", 0) for v in values)
    return pa.timestamp("ns" if has_nanos else "us", tz=arrow_type.tz)


def _key_series(labels: Labels, level: int, values: list[object], scalar: bool = False) -> Series:
    """``values`` as the key column of index level ``level``."""
    own = labels.dtype(level)
    if own not in _TEXT and None in values:
        # As pandas, a None key finds null labels of a text level only; a NaN
        # key finds the NaN labels of a float level.
        raise _Unkeyable
    try:
        series = Series.from_list(values)
    except (TypeError, ValueError, OverflowError):
        series = None
    if series is not None and own in _PLAIN and series.dtype in _PLAIN:
        if scalar and own in _NARROW_FLOATS and series.dtype.is_numeric:
            # As pandas, one key is cast to the level's float type first (a
            # list of keys is not).
            series = series.astype(own)
        return series
    if own in _PLAIN:
        raise _Unkeyable
    lv = labels.level(level)
    assert lv is not None
    arrow_type = lv.slice(0, 0).to_arrow().type
    if _awareness_differs(values, getattr(arrow_type, "tz", None)):
        raise _Unkeyable
    try:
        return Series.from_list(values, dtype=_stamp_type(values, arrow_type))
    except Exception:
        raise _Unkeyable from None


def _key_columns(
    labels: Labels, items: list[object], depth: int, tupled: bool, scalar: bool = False
) -> list[Series]:
    """The key columns of ``items``: labels of one level, or (``tupled``)
    tuples naming the first ``depth`` levels."""
    if not tupled:
        return [_key_series(labels, 0, items, scalar)]
    rows = [cast("tuple[object, ...]", item) for item in items]
    return [_key_series(labels, i, [item[i] for item in rows], scalar) for i in range(depth)]


def _locate_each(
    labels: Labels, items: list[object], depth: int, tupled: bool
) -> tuple[list[int], list[int]]:
    """``locate`` one item at a time: for labels that make no one key column
    (``[1, "a"]``), where an item of another type simply has no row."""
    positions: list[int] = []
    missing: list[int] = []
    for i, item in enumerate(items):
        try:
            found, none = labels.locate(_key_columns(labels, [item], depth, tupled))
        except _Unkeyable:
            missing.append(i)
            continue
        positions.extend(found)
        missing.extend([i] if none else [])
    return positions, missing


def _locate_items(
    labels: Labels, items: list[object], depth: int, tupled: bool = False, scalar: bool = False
) -> tuple[list[int], list[int]]:
    if not items:
        return [], []
    try:
        keys = _key_columns(labels, items, depth, tupled, scalar)
    except _Unkeyable:
        return _locate_each(labels, items, depth, tupled)
    return labels.locate(keys)


def _missing_error(items: list[object], missing: list[int], one: bool) -> KeyError:
    if one:
        return KeyError(items[0])
    return KeyError(f"{[items[i] for i in missing]} not in index")


def _is_labels_list(key: object) -> bool:
    np = sys.modules.get("numpy")
    return isinstance(key, (list, range, set, frozenset)) or (
        np is not None and isinstance(key, np.ndarray)
    )


def _slice_bound(labels: Labels, bound: object) -> list[Series]:
    if bound is None:
        return []
    tupled = isinstance(bound, tuple)
    depth = len(bound) if tupled else 1  # type: ignore[arg-type]
    if tupled and not 1 <= depth <= labels.depth:
        raise KeyError(bound)
    try:
        return _key_columns(labels, [bound], depth, tupled, True)
    except _Unkeyable:
        raise TypeError(f"loc: cannot slice with the label {bound!r}") from None


def _label_slice(labels: Labels, key: builtins.slice) -> Rows:
    if key.step is not None:
        raise TypeError("loc: a label slice takes no step")
    start = _slice_bound(labels, key.start)
    stop = _slice_bound(labels, key.stop)
    try:
        offset, length = labels.locate_range(start, stop)
    except KeyError as e:
        raise KeyError(f"{e}: {key.start!r}:{key.stop!r}") from None
    except ValueError as e:
        raise TypeError(f"loc: {e}") from None
    return Rows.slice(offset, length)


def _named(labels: Labels, depth: int) -> tuple[int, ...]:
    """The levels a key of ``depth`` leading levels leaves out of the result:
    none for a whole label, the leading ones for a part of it."""
    return tuple(range(depth)) if depth < labels.depth else ()


def rows_by_label(labels: Labels, key: object) -> Rows:
    """``loc`` rows against the index of a frame or Series. A label (or a tuple
    naming the leading levels of a MultiIndex) or a list of them selects the
    rows that hold it, a list in its own order with a repeated label giving
    its rows again; a label slice is inclusive on both ends (binary search on
    a sorted index, else both bounds must be single labels); a Bool Series or
    list is a mask. A label no row holds is a ``KeyError``."""

    if isinstance(key, builtins.slice) and _is_all(key):
        return Rows.all()
    if isinstance(key, Expr):
        return Rows.masked(key)
    if isinstance(key, Series):
        if _is_bool(key):
            return Rows.masked(key)
        positions, missing = labels.locate([key])
        if missing:
            raise KeyError(f"{[key.to_list()[i] for i in missing]} not in index")
        return Rows.take(positions, drop=_named(labels, 1))
    if isinstance(key, builtins.slice):
        return _label_slice(labels, key)
    if _is_labels_list(key):
        values = list(cast("Sequence[object]", key))
        if _bool_list(values):
            return Rows.masked(Series.from_list(values))
        tupled = (
            bool(values)
            and isinstance(values[0], tuple)
            and all(isinstance(v, tuple) for v in values)
        )
        depth = 1
        if tupled:
            depth = len(cast("tuple[object, ...]", values[0]))
            if not 1 <= depth <= labels.depth or any(
                len(cast("tuple[object, ...]", v)) != depth for v in values
            ):
                raise KeyError(f"{values} do not name the same 1 to {labels.depth} index levels")
        positions, missing = _locate_items(labels, values, depth, tupled)
        if missing:
            raise _missing_error(values, missing, False)
        return Rows.take(positions, drop=_named(labels, depth))
    if isinstance(key, tuple):
        if not 1 <= len(key) <= labels.depth:
            raise KeyError(key)
        positions, missing = _locate_items(labels, [key], len(key), True, True)
        depth = len(key)
    else:
        positions, missing = _locate_items(labels, [key], 1, False, True)
        depth = 1
    if missing:
        raise KeyError(key)
    return Rows.take(positions, one=depth == labels.depth, drop=_named(labels, depth))


def rows_by_level(labels: Labels, level: object, key: object) -> Rows:
    """The rows whose index level ``level`` (a position or a name) is ``key``
    (pandas ``xs(key, level=level)``)."""
    if not labels.names:
        if level not in (0, None):
            raise KeyError(f"Level {level!r} not found")
        return rows_by_label(labels, key)
    if isinstance(level, str):
        if level not in labels.names:
            raise KeyError(f"Level {level} not found")
        at = labels.names.index(level)
    else:
        at = int(cast(int, level))
        if not -len(labels.names) <= at < len(labels.names):
            raise IndexError(f"Too many levels: Index has only {len(labels.names)} levels")
        at %= len(labels.names)
    lv = labels.level(at)
    assert lv is not None
    # The hash is over the leading levels, so another level looks up through
    # a one-level view of the labels (built for this call).
    if at == 0:
        rows = rows_by_label(labels, key)
        return Rows.take(rows.positions, drop=(0,))
    if labels.frame is not None:
        found = Labels(labels.frame.set_index(labels.names[at]))
    else:
        from ._series_index import with_levels

        assert labels.series is not None
        found = Labels(with_levels(labels.series, [labels.names[at]], [lv]))
    rows = rows_by_label(found, key)
    return Rows.take(rows.positions, drop=(at,))


def columns_by_position(names: list[str], key: object) -> tuple[list[str], bool]:
    if _is_all(key):
        return list(names), False
    if isinstance(key, bool):
        raise TypeError("iloc: a bool is not a column position")
    if isinstance(key, int):
        return [names[key]], True
    if isinstance(key, str):
        raise TypeError("iloc: columns are selected by position; use loc for names")
    if isinstance(key, builtins.slice):
        return list(names[key]), False
    if isinstance(key, (list, tuple, range)):
        return [names[int(k)] for k in key], False
    raise TypeError(f"iloc: cannot select columns with {type(key).__name__}")


def columns_by_name(names: list[str], key: object, new_ok: bool = False) -> tuple[list[str], bool]:
    if _is_all(key):
        return list(names), False
    if isinstance(key, str):
        if key not in names and not new_ok:
            raise KeyError(f"loc: no column named {key!r}")
        return [key], True
    if isinstance(key, builtins.slice):
        lo = 0 if key.start is None else names.index(key.start)
        hi = len(names) - 1 if key.stop is None else names.index(key.stop)
        return list(names[lo : hi + 1]), False
    if isinstance(key, (list, tuple)):
        out = [str(k) for k in key]
        for k in out:
            if k not in names and not new_ok:
                raise KeyError(f"loc: no column named {k!r}")
        return out, False
    raise TypeError(f"loc: cannot select columns with {type(key).__name__}")


def _split(key: object, levels: int = 1, names: Sequence[str] = ()) -> tuple[object, object]:
    """``[rows]`` or ``[rows, columns]``. On a several-column index a tuple
    of plain labels whose second item is not a column name is one row label
    (pandas reads ``loc[(a, b)]`` the same way); ``loc[(a, b), :]`` is the
    unambiguous spelling."""
    if isinstance(key, tuple):
        if (
            levels > 1
            and 1 <= len(key) <= levels
            and all(isinstance(k, (int, float, str)) and not isinstance(k, bool) for k in key)
            and (len(key) == 1 or key[1] not in names)
        ):
            return key, None
        if len(key) != 2:
            raise TypeError("indexer takes [rows] or [rows, columns]")
        return key[0], key[1]
    return key, None


# ---- eager --------------------------------------------------------------------


def _eager_mask(df: DataFrame, mask: object) -> Series:

    if isinstance(mask, Expr):
        return mask.apply(df)
    assert isinstance(mask, Series)
    return mask


def _apply_rows(df: DataFrame, rows: Rows) -> DataFrame:
    if rows.kind == "all":
        return df
    if rows.kind == "slice":
        return df.slice(rows.offset, rows.length)
    if rows.kind == "take":
        return df.take(rows.positions)
    return df.filter(_eager_mask(df, rows.mask))


def _mask_of(df: DataFrame, rows: Rows) -> Series | None:
    """The full-length Bool mask a selection names, or None for every row."""
    if rows.kind == "all":
        return None
    n = len(df)
    if rows.kind == "slice":
        return Series.from_list(list(range(n))).is_between(
            rows.offset, rows.offset + rows.length - 1
        )
    if rows.kind == "take":
        return Series.from_list(list(range(n))).is_in(rows.positions)
    mask = _eager_mask(df, rows.mask)
    if len(mask) != n:
        raise ValueError(f"mask has {len(mask)} rows; the frame has {n}")
    return mask


_SAME_KIND = {
    DType.STRING: frozenset({DType.STRING, DType.LARGE_STRING}),
    DType.BINARY: frozenset({DType.BINARY, DType.LARGE_BINARY}),
    DType.BOOL: frozenset({DType.BOOL}),
    DType.FLOAT64: frozenset({DType.FLOAT32, DType.FLOAT64}),
    DType.DATE32: frozenset({DType.DATE32, DType.DATE64}),
}


def _fits(value: object, own: DType, column: DType) -> bool:
    """Whether a value of type ``own`` converts to ``column`` without loss."""
    if own in (DType.INT64, DType.UINT64) and isinstance(value, int):
        if column.is_float:
            return True
        bounds = column.int_range
        return bounds is not None and bounds[0] <= value <= bounds[1]
    return column in _SAME_KIND.get(own, frozenset())


def _fill(value: object, n: int, like: Series) -> Series:
    """``value`` repeated ``n`` times as a Series of ``like``'s type. A type a
    native DType does not name in full (list, struct, timestamp, decimal, ...)
    goes through its Arrow type."""
    column = like.dtype
    if (
        column.int_range is None
        and not column.is_float
        and not any(column in kinds for kinds in _SAME_KIND.values())
    ):
        return Series.from_list([value] * n, dtype=like.to_arrow().type)
    if value is not None and not _fits(value, Series.from_list([value]).dtype, column):
        raise TypeError(f"cannot assign {value!r} to a {column.name.lower()} column")
    return Series.from_list([value] * n, dtype=column)


def _broadcast(value: object, n: int, like: Series | None) -> Series:
    """``value`` as an n-row Series of ``like``'s type (or the value's own)."""
    if isinstance(value, Series):
        if len(value) != n:
            raise ValueError(f"value has {len(value)} rows; the frame has {n}")
        return value
    if isinstance(value, (list, tuple)):
        if len(value) != n:
            raise ValueError(f"value has {len(value)} rows; the frame has {n}")
        return Series.from_list(list(value))
    if like is not None:
        return _fill(value, n, like)
    return Series.from_list([value] * n)


def _scatter(value: object, mask: Series, n: int) -> object:
    """A list or Series value sized to the selected rows becomes a full-length list."""
    if isinstance(value, Series) and len(value) != n:
        value = value.to_list()
    if not isinstance(value, (list, tuple)) or len(value) == n:
        return value
    hits = mask.to_list()
    count = sum(1 for h in hits if h)
    if len(value) != count:
        raise ValueError(f"value has {len(value)} rows; the selection has {count}")
    it = iter(value)
    return [next(it) if h else None for h in hits]


def _per_column(value: object) -> list[Series] | None:
    """The columns of a frame, or the Series of a non-empty list or tuple of
    Series: one per target column, by position. None for any other value."""
    from .dataframe import DataFrame

    if isinstance(value, DataFrame):
        return [value[c] for c in value.columns]
    if isinstance(value, (list, tuple)) and value and all(isinstance(v, Series) for v in value):
        return list(value)
    return None


def _check_per_column(
    columns: list[str], series: list[Series], n: int, mask: Series | None
) -> None:
    """The counts and row counts of a multi-column assignment, before any write."""
    if len(columns) != len(series):
        raise ValueError(
            f"assign: {len(columns)} target columns but the value has {len(series)} columns"
        )
    selected = n if mask is None else int(mask.fillna(False).astype("int64").sum())
    for name, s in zip(columns, series):
        if len(s) != n and len(s) != selected:
            what = "the frame has" if mask is None else "the selection has"
            raise ValueError(f"assign: column {name!r} gets {len(s)} rows; {what} {selected}")


def assign_rows(df: DataFrame, rows: Rows, columns: list[str], value: object) -> None:
    """``df.loc[rows, columns] = value``: each column rebuilt where the rows
    select, the handle rebound to the new frame. A frame or a list of Series
    gives each target column the value at the same position."""
    if isinstance(value, dict):
        for name, v in value.items():
            assign_rows(df, rows, [str(name)], v)
        return
    n = len(df)
    mask = _mask_of(df, rows)
    per_column = _per_column(value)
    if per_column is not None:
        _check_per_column(columns, per_column, n, mask)
        before = _ext.DataFrame.share(df)
        try:
            for name, series in zip(columns, per_column):
                assign_rows(df, rows, [name], series)
        except BaseException:
            _ext.DataFrame._replace(df, before)
            raise
        return
    if mask is not None:
        value = _scatter(value, mask, n)
    native: DataFrame = df
    for name in columns:
        existing = _ext.DataFrame.__getitem__(native, name) if name in df else None
        try:
            new = _broadcast(value, n, existing)
        except Exception as e:
            if type(e) in (TypeError, ValueError):
                raise
            # A pyarrow conversion error: the value does not fit the column.
            raise TypeError(f"cannot assign {value!r} to column {name!r}: {e}") from None
        if mask is not None:
            if existing is None:
                blank = _fill(None, n, new)
                new = blank.mask(mask, new)
            else:
                if existing.dtype != new.dtype:
                    new = new.astype(existing.dtype)
                new = existing.mask(mask, new)
        native = _ext.DataFrame.with_column(native, name, new)
    _ext.DataFrame._replace(df, native)


class ILoc:
    """``df.iloc``: rows and columns by position."""

    __slots__ = ("_df",)

    def __init__(self, df: DataFrame) -> None:
        self._df = df

    def __getitem__(self, key: object) -> object:
        df = self._df
        rk, ck = _split(key)
        rows, one_row = rows_by_position(len(df), rk)
        cols, one_col = columns_by_position(df.columns, ck)
        out = _apply_rows(df, rows)
        if one_row and one_col:
            return out[cols[0]][0]
        if one_row:
            return Row({c: out[c][0] for c in cols})
        if one_col:
            return out[cols[0]]
        return out.select(*cols, *df.index_names()) if ck is not None else out

    def __setitem__(self, key: object, value: object) -> None:
        df = self._df
        rk, ck = _split(key)
        rows, _ = rows_by_position(len(df), rk)
        cols, _ = columns_by_position(df.columns, ck)
        assign_rows(df, rows, cols, value)


def drop_index_levels(out: DataFrame, names: list[str], drop: tuple[int, ...]) -> DataFrame:
    """``out`` without the index levels ``drop`` (positions in ``names``): the
    columns leave the frame, the others stay its index."""
    if not drop:
        return out
    gone = {names[i] for i in drop}
    kept = [c for c in out._names() if c not in gone]
    return out.select(*kept)


class Loc:
    """``df.loc``: rows by index label (or a mask), columns by name."""

    __slots__ = ("_df",)

    def __init__(self, df: DataFrame) -> None:
        self._df = df

    def _rows(self, key: object) -> tuple[Rows, object, Labels]:
        df = self._df
        labels = Labels(df)
        rk, ck = _split(key, labels.depth, df.columns)
        return rows_by_label(labels, rk), ck, labels

    def __getitem__(self, key: object) -> object:
        df = self._df
        rows, ck, labels = self._rows(key)
        cols, one_col = columns_by_name(df.columns, ck)
        out = drop_index_levels(_apply_rows(df, rows), labels.names, rows.drop)
        if one_col:
            return out[cols[0]]
        return out.select(*cols, *out.index_names()) if ck is not None else out

    def __setitem__(self, key: object, value: object) -> None:
        df = self._df
        rows, ck, _ = self._rows(key)
        cols, _ = columns_by_name(df.columns, ck, new_ok=True)
        assign_rows(df, rows, cols, value)


class At:
    """``df.at[label, column]`` / ``df.iat[position, column]``: one value."""

    __slots__ = ("_df", "_by_position")

    def __init__(self, df: DataFrame, by_position: bool) -> None:
        self._df = df
        self._by_position = by_position

    def _cell(self, key: object) -> tuple[Rows, str]:
        if not isinstance(key, tuple) or len(key) != 2:
            raise TypeError("at / iat take [row, column]")
        rk, ck = key
        if self._by_position:
            rows, _ = rows_by_position(len(self._df), rk)
            cols, _ = columns_by_position(self._df.columns, ck)
        else:
            rows = rows_by_label(Labels(self._df), rk)
            if not rows.one:
                raise TypeError("at takes one whole row label")
            cols, _ = columns_by_name(self._df.columns, ck, new_ok=True)
        if len(cols) != 1:
            raise TypeError("at / iat name one column")
        return rows, cols[0]

    def __getitem__(self, key: object) -> object:
        rows, name = self._cell(key)
        out = _apply_rows(self._df, rows)
        if self._by_position:
            return out[name].item(0)
        # As pandas: the value of the one row, the Series of the column over
        # every row a repeated label has.
        return out[name].item(0) if len(out) == 1 else out[name]

    def __setitem__(self, key: object, value: object) -> None:
        rows, name = self._cell(key)
        assign_rows(self._df, rows, [name], value)


def series_by_label(
    s: Series, key: object, level: object = None, drop_level: bool = True
) -> object:
    """``s.loc[key]`` for a Series: the value of the one row a whole label
    names, else a Series of the rows (a labelled Series keeps its labels, a
    positional one is labelled by the positions it keeps), a part of a MultiIndex
    label leaving out the levels it names."""
    from ._series_index import (
        default_name,
        levels_of,
        names_of,
        positions,
        with_levels,
    )

    labels = Labels(s)
    rows = rows_by_label(labels, key) if level is None else rows_by_level(labels, level, key)
    if not drop_level:
        rows.drop = ()
    if rows.kind == "take" and rows.one and len(rows.positions) == 1:
        return s.item(rows.positions[0])
    if rows.kind == "all":
        out = s
    elif rows.kind == "slice":
        out = s.slice(rows.offset, rows.length)
    elif rows.kind == "take":
        out = s.gather(rows.positions)
    else:
        mask = rows.mask
        if not isinstance(mask, Series):
            raise TypeError("loc on a Series takes labels, a slice or a Bool mask")
        out = s.filter(mask)
    if not labels.names:
        # The rows keep their positions as labels, as pandas.
        if rows.kind == "slice":
            return with_levels(out, [default_name(0)], [positions(rows.length, rows.offset)])
        if rows.kind == "take":
            return with_levels(out, [default_name(0)], [Series.from_list(rows.positions)])
        if rows.kind == "mask":
            index = positions(len(s))
            assert isinstance(rows.mask, Series)
            return with_levels(out, [default_name(0)], [index.filter(rows.mask)])
        return out
    if rows.drop:
        names = names_of(out)
        levels = levels_of(out)
        keep = [i for i in range(len(names)) if i not in rows.drop]
        return with_levels(out, [names[i] for i in keep], [levels[i] for i in keep])
    return out


# ---- lazy ---------------------------------------------------------------------


def _lazy_rows(lf: LazyFrame, rows: Rows) -> LazyFrame:

    if rows.kind == "all":
        return lf
    if rows.kind == "slice":
        return lf.slice(rows.offset, rows.length)
    if rows.kind == "take":
        return lf.take(rows.positions)
    if isinstance(rows.mask, Expr):
        return lf.filter(rows.mask)
    assert isinstance(rows.mask, Series)
    return lf.filter_mask(rows.mask)


def lazy_rows_by_label(lf: LazyFrame, key: object) -> Rows:
    """``loc`` rows on a plan: an Expr mask over the index column."""

    if _is_all(key):
        return Rows.all()
    if isinstance(key, Expr):
        return Rows.masked(key)
    if isinstance(key, Series):
        if _is_bool(key):
            return Rows.masked(key)
        raise TypeError("loc on a LazyFrame takes labels as a list, not a Series")
    names = lf._index_names()
    index = col(names[0])

    def label(value: object) -> Expr:
        if isinstance(value, bool) or not isinstance(value, (int, float, str)):
            raise TypeError(f"loc: a label must be a number or a str, not {type(value).__name__}")
        return index == value

    def label_tuple(values: tuple[object, ...]) -> Expr:
        if not values or len(values) > len(names):
            raise KeyError(
                f"loc: a label tuple names 1 to {len(names)} index levels, got {len(values)}"
            )
        mask = label(values[0])
        for name, value in zip(names[1:], values[1:]):
            if isinstance(value, bool) or not isinstance(value, (int, float, str)):
                raise TypeError(
                    f"loc: a label must be a number or a str, not {type(value).__name__}"
                )
            mask = mask & (col(name) == value)
        return mask

    if isinstance(key, tuple):
        return Rows.masked(label_tuple(key))
    if isinstance(key, builtins.slice):
        if len(names) > 1:
            raise TypeError("loc: a label slice needs a one-column index")
        lo, hi = _numeric_bounds(key)
        if lo is not None and hi is not None:
            return Rows.masked(index.is_between(lo, hi))
        if lo is not None:
            return Rows.masked(index >= lo)
        assert hi is not None
        return Rows.masked(index <= hi)
    if isinstance(key, (list, range, set, frozenset)):
        values = list(key)
        if _bool_list(values):
            return Rows.masked(Series.from_list(values))
        tuples = [v for v in values if isinstance(v, tuple)]
        if tuples and len(tuples) == len(values):
            mask = label_tuple(tuples[0])
            for v in tuples[1:]:
                mask = mask | label_tuple(v)
            return Rows.masked(mask)
        return Rows.masked(index.is_in(values))
    return Rows.masked(label(key))


def _reads_row(mask: object) -> bool:
    """Whether a label mask is over the row position (no index column set)."""

    return isinstance(mask, Expr) and _ROW in collect_columns(mask)


def _lazy_mask_expr(lf: LazyFrame, rows: Rows) -> tuple[LazyFrame, Expr | None]:
    """The plan (with a row index when the selection is positional) and the
    Expr mask the selection names; None for every row."""

    if rows.kind == "all":
        return lf, None
    if rows.kind == "slice":
        last = rows.offset + rows.length - 1
        return lf.with_row_index(_ROW), col(_ROW).is_between(rows.offset, last)
    if rows.kind == "take":
        return lf.with_row_index(_ROW), col(_ROW).is_in(rows.positions)
    if isinstance(rows.mask, Expr):
        return (lf.with_row_index(_ROW) if _reads_row(rows.mask) else lf), rows.mask
    raise TypeError("a lazy assignment takes its rows as labels or an expression, not a Series")


def lazy_assign_rows(lf: LazyFrame, rows: Rows, columns: list[str], value: object) -> None:
    """``lf.loc[rows, columns] = value`` as ``with_column`` plan steps, the
    handle rebound to the longer plan."""

    if isinstance(value, dict):
        for name, v in value.items():
            lazy_assign_rows(lf, rows, [str(name)], v)
        return
    if isinstance(value, Series):
        raise TypeError("a lazy assignment takes a scalar or an expression, not a Series")
    if not isinstance(value, (Expr, int, float)) or isinstance(value, bool):
        raise TypeError(
            f"a lazy assignment takes a number or an expression, not {type(value).__name__}"
        )
    schema = lf.columns
    index = lf.index_names()
    plan, mask = _lazy_mask_expr(lf, rows)
    fill: Expr = value if isinstance(value, Expr) else lit(value)
    for name in columns:
        if name not in schema:
            raise KeyError(
                f"loc: no column named {name!r}; a lazy assignment cannot create one "
                "(use with_column)"
            )
        plan = plan.with_column(
            name, fill if mask is None else when(mask).then(fill).otherwise(col(name))
        )
    if plan.columns != schema:
        plan = plan.select(*schema)
    # The positional steps above add a row index column, which has no index.
    plan = _ext.LazyFrame.with_index(plan, index)
    _ext.LazyFrame._replace(lf, plan)


def lazy_rows_by_position(key: object) -> Rows:
    """``iloc`` rows on a plan, whose length is unknown until it runs: a
    position or a unit-step slice must be non-negative (``slice`` / ``take``
    plan steps); a Bool Series or list is a mask."""
    if _is_all(key):
        return Rows.all()
    if isinstance(key, bool):
        raise TypeError("iloc: a bool is not a position")
    if isinstance(key, int):
        if key < 0:
            raise IndexError("iloc on a LazyFrame takes a non-negative row position")
        return Rows.slice(key, 1)
    if isinstance(key, builtins.slice):
        lo = key.start or 0
        if key.step not in (None, 1) or lo < 0 or (key.stop is not None and key.stop < 0):
            raise IndexError("iloc on a LazyFrame takes a non-negative, unit-step slice")
        if key.stop is None:
            return Rows.slice(lo, _LAZY_END)
        return Rows.slice(lo, max(key.stop - lo, 0))
    if isinstance(key, Series):
        if _is_bool(key):
            return Rows.masked(key)
        return Rows.take(_positions(_LAZY_END, key.to_list()))
    if isinstance(key, (list, tuple, range)):
        values = list(key)
        if _bool_list(values):
            return Rows.masked(Series.from_list(values))
        return Rows.take(_positions(_LAZY_END, values))
    raise TypeError(f"iloc: cannot select rows with {type(key).__name__}")


class LazyILoc:
    __slots__ = ("_lf",)

    def __init__(self, lf: LazyFrame) -> None:
        self._lf = lf

    def __getitem__(self, key: object) -> LazyFrame:
        rk, ck = _split(key)
        out = _lazy_rows(self._lf, lazy_rows_by_position(rk))
        if ck is None:
            return out
        cols, _ = columns_by_position(self._lf.columns, ck)
        return out.select(*cols)

    def __setitem__(self, key: object, value: object) -> None:
        rk, ck = _split(key)
        cols, _ = columns_by_position(self._lf.columns, ck)
        lazy_assign_rows(self._lf, lazy_rows_by_position(rk), cols, value)


class LazyLoc:
    __slots__ = ("_lf",)

    def __init__(self, lf: LazyFrame) -> None:
        self._lf = lf

    def _rows(self, key: object) -> tuple[Rows, object]:
        lf = self._lf
        rk, ck = _split(key, len(lf._index_names()), lf.columns)
        return lazy_rows_by_label(lf, rk), ck

    def __getitem__(self, key: object) -> LazyFrame:
        lf = self._lf
        rows, ck = self._rows(key)
        cols, _ = columns_by_name(lf.columns, ck)
        # With no index column the labels are row positions: a hidden row
        # index feeds the mask and is projected away again.
        needs_row = rows.kind == "mask" and _reads_row(rows.mask)
        plan = lf.with_row_index(_ROW) if needs_row else lf
        out = _lazy_rows(plan, rows)
        return out.select(*cols) if (ck is not None or needs_row) else out

    def __setitem__(self, key: object, value: object) -> None:
        rows, ck = self._rows(key)
        cols, _ = columns_by_name(self._lf.columns, ck, new_ok=True)
        lazy_assign_rows(self._lf, rows, cols, value)


# ---- resample -----------------------------------------------------------------


class Resampler:
    """``df.resample(rule, on=...)``: a tumbling time window over the index
    (or ``on``) column, with the ``group_by`` aggregate family. Each call is
    one ``group_by_dynamic`` over the frame (or plan) sorted by time."""

    __slots__ = ("_frame", "_time", "_every")

    def __init__(self, frame: DataFrame | LazyFrame, time: str, every: int) -> None:
        self._frame = frame
        self._time = time
        self._every = every

    def _run(self, specs: Sequence[str | tuple[str, str, list[str]]]) -> DataFrame | LazyFrame:
        return self._frame.sort_by(self._time).group_by_dynamic(
            self._time, self._every, None, specs
        )

    def agg(self, *specs: AggForm, **named: AggKeyword) -> DataFrame | LazyFrame:
        from .lazyframe import agg_spec_strings

        return self._run(agg_spec_strings(self._frame, [self._time], specs, named))

    aggregate = agg

    def reduce(self, agg: str) -> DataFrame | LazyFrame:

        return self._run(reduce_specs(self._frame, agg, [self._time]))

    def sum(self) -> DataFrame | LazyFrame:
        return self.reduce("sum")

    def mean(self) -> DataFrame | LazyFrame:
        return self.reduce("mean")

    def min(self) -> DataFrame | LazyFrame:
        return self.reduce("min")

    def max(self) -> DataFrame | LazyFrame:
        return self.reduce("max")

    def count(self) -> DataFrame | LazyFrame:
        return self.reduce("count_valid")

    def var(self) -> DataFrame | LazyFrame:
        return self.reduce("var")

    def std(self) -> DataFrame | LazyFrame:
        return self.reduce("std")

    def first(self) -> DataFrame | LazyFrame:
        return self.reduce("first")

    def last(self) -> DataFrame | LazyFrame:
        return self.reduce("last")

    def size(self) -> DataFrame | LazyFrame:
        return self._run(["count::size"])


__all__ = ["ILoc", "Loc", "At", "LazyILoc", "LazyLoc", "Resampler", "Rows"]
