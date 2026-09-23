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
import re
from typing import TYPE_CHECKING, Dict, List, Optional, Sequence, Tuple, Union

from ._apply import Row
from .series import Series, _unwrap

if TYPE_CHECKING:
    from .columnar import Expr
    from .dataframe import DataFrame
    from .lazyframe import LazyFrame

_ROW = "__dftu_loc_row__"
# A plan has no length until it runs; an open slice end / a position bound.
_LAZY_END = 2**62


class Rows:
    """A row selection, normalized: every input spelling lands in one of
    ``all``, ``slice`` (offset, length), ``take`` (positions) or ``mask``
    (a Bool Series or an Expr)."""

    __slots__ = ("kind", "offset", "length", "positions", "mask")

    def __init__(
        self,
        kind: str,
        offset: int = 0,
        length: int = 0,
        positions: Optional[List[int]] = None,
        mask: object = None,
    ) -> None:
        self.kind = kind
        self.offset = offset
        self.length = length
        self.positions = positions or []
        self.mask = mask

    @classmethod
    def all(cls) -> "Rows":
        return cls("all")

    @classmethod
    def slice(cls, offset: int, length: int) -> "Rows":
        return cls("slice", offset=offset, length=length)

    @classmethod
    def take(cls, positions: List[int]) -> "Rows":
        return cls("take", positions=positions)

    @classmethod
    def masked(cls, mask: object) -> "Rows":
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


def _positions(n: int, values: Sequence[object]) -> List[int]:
    out: List[int] = []
    for v in values:
        if isinstance(v, bool) or not isinstance(v, int):
            raise TypeError("iloc: positions must be ints")
        p = v + n if v < 0 else v
        if p < 0 or p >= n:
            raise IndexError(f"iloc: position {v} is out of bounds for {n} rows")
        out.append(p)
    return out


def _is_bool(series: Series) -> bool:
    from .enums import DType

    return series.dtype == DType.BOOL


def rows_by_position(n: int, key: object) -> Tuple[Rows, bool]:
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


def _label_eq(index: Series, value: object) -> Series:
    if isinstance(value, str):
        return index.str_eq(value)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError(f"loc: a label must be a number or a str, not {type(value).__name__}")
    return index.eq(value)


def _numeric_bounds(key: builtins.slice) -> Tuple[Optional[float], Optional[float]]:
    if key.step is not None:
        raise TypeError("loc: a label slice takes no step")
    for bound in (key.start, key.stop):
        if bound is not None and (isinstance(bound, bool) or not isinstance(bound, (int, float))):
            raise TypeError("loc: a label slice needs numeric bounds")
    return key.start, key.stop


def _label_range(index: Series, key: builtins.slice) -> Series:
    lo, hi = _numeric_bounds(key)
    if lo is not None and hi is not None:
        return index.is_between(lo, hi)
    if lo is not None:
        return index >= lo
    assert hi is not None
    return index <= hi


def _and(a: Series, b: Series) -> Series:
    return a.logical(0, b)


def _or(a: Series, b: Series) -> Series:
    return a.logical(1, b)


def _label_tuple(levels: List[Series], key: tuple) -> Series:
    """A label tuple matches the leading levels it names (pandas' partial
    indexing on a MultiIndex)."""
    if not key or len(key) > len(levels):
        raise KeyError(f"loc: a label tuple names 1 to {len(levels)} index levels, got {len(key)}")
    mask = _label_eq(levels[0], key[0])
    for level, value in zip(levels[1:], key[1:]):
        mask = _and(mask, _label_eq(level, value))
    return mask


def rows_by_label(levels: List[Series], key: object) -> Rows:
    """``loc`` rows against the index levels (labels are inclusive on both
    ends of a slice, as pandas). One level takes a value, a slice or a list;
    several take a tuple (or a list of tuples) matching the leading levels,
    or a bare value for the first level."""
    from .columnar import Expr

    index = levels[0]
    if _is_all(key):
        return Rows.all()
    if isinstance(key, Series):
        return Rows.masked(key if _is_bool(key) else index.is_in(key))
    if isinstance(key, Expr):
        return Rows.masked(key)
    if isinstance(key, tuple):
        return Rows.masked(_label_tuple(levels, key))
    if isinstance(key, builtins.slice):
        if len(levels) > 1:
            raise TypeError("loc: a label slice needs a one-column index")
        return Rows.masked(_label_range(index, key))
    if isinstance(key, (list, range, set, frozenset)):
        values = list(key)
        if _bool_list(values):
            return Rows.masked(Series.from_list(values))
        tuples = [v for v in values if isinstance(v, tuple)]
        if tuples and len(tuples) == len(values):
            mask = _label_tuple(levels, tuples[0])
            for v in tuples[1:]:
                mask = _or(mask, _label_tuple(levels, v))
            return Rows.masked(mask)
        return Rows.masked(index.is_in(values))
    return Rows.masked(_label_eq(index, key))


def columns_by_position(names: List[str], key: object) -> Tuple[List[str], bool]:
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


def columns_by_name(names: List[str], key: object, new_ok: bool = False) -> Tuple[List[str], bool]:
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


def _split(key: object, levels: int = 1, names: Sequence[str] = ()) -> Tuple[object, object]:
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


def _eager_mask(df: "DataFrame", mask: object) -> Series:
    from .columnar import Expr

    if isinstance(mask, Expr):
        return mask.apply(df)
    assert isinstance(mask, Series)
    return mask


def _apply_rows(df: "DataFrame", rows: Rows) -> "DataFrame":
    if rows.kind == "all":
        return df
    if rows.kind == "slice":
        return df.slice(rows.offset, rows.length)
    if rows.kind == "take":
        return df.take(rows.positions)
    return df.filter(_eager_mask(df, rows.mask))


def _mask_of(df: "DataFrame", rows: Rows) -> Optional[Series]:
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


def _broadcast(value: object, n: int, like: Optional[Series]) -> Series:
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
        return Series.from_list([value] * n, dtype=like.to_arrow().type)
    return Series.from_list([value] * n)


def _scatter(value: object, mask: Series, n: int) -> object:
    """A list value sized to the selected rows becomes a full-length list."""
    if not isinstance(value, (list, tuple)) or len(value) == n:
        return value
    hits = mask.to_list()
    count = sum(1 for h in hits if h)
    if len(value) != count:
        raise ValueError(f"value has {len(value)} rows; the selection has {count}")
    it = iter(value)
    return [next(it) if h else None for h in hits]


def assign_rows(df: "DataFrame", rows: Rows, columns: List[str], value: object) -> None:
    """``df.loc[rows, columns] = value``: each column rebuilt where the rows
    select, the handle rebound to the new frame."""
    if isinstance(value, dict):
        for name, v in value.items():
            assign_rows(df, rows, [str(name)], v)
        return
    n = len(df)
    mask = _mask_of(df, rows)
    if mask is not None:
        value = _scatter(value, mask, n)
    native = df._native
    for name in columns:
        existing = Series(native[name]) if name in df else None
        try:
            new = _broadcast(value, n, existing)
        except Exception as e:
            if type(e) in (TypeError, ValueError):
                raise
            # A pyarrow conversion error: the value does not fit the column.
            raise TypeError(f"cannot assign {value!r} to column {name!r}: {e}") from None
        if mask is not None:
            if existing is None:
                blank = Series.from_list([None] * n, dtype=new.to_arrow().type)
                new = blank.mask(mask, new)
            else:
                if existing.dtype != new.dtype:
                    new = new.astype(existing.dtype)
                new = existing.mask(mask, new)
        native = native.with_column(name, _unwrap(new))
    df._native = native


class ILoc:
    """``df.iloc``: rows and columns by position."""

    __slots__ = ("_df",)

    def __init__(self, df: "DataFrame") -> None:
        self._df = df

    def __getitem__(self, key: object) -> object:
        df = self._df
        rk, ck = _split(key)
        rows, one_row = rows_by_position(len(df), rk)
        cols, one_col = columns_by_position(df.columns, ck)
        out = _apply_rows(df, rows)
        if one_row and one_col:
            return Series(out._native[cols[0]])[0]
        if one_row:
            return Row({c: Series(out._native[c])[0] for c in cols})
        if one_col:
            return Series(out._native[cols[0]])
        return out.select(*cols) if ck is not None else out

    def __setitem__(self, key: object, value: object) -> None:
        df = self._df
        rk, ck = _split(key)
        rows, _ = rows_by_position(len(df), rk)
        cols, _ = columns_by_position(df.columns, ck)
        assign_rows(df, rows, cols, value)


class Loc:
    """``df.loc``: rows by index label (or a mask), columns by name."""

    __slots__ = ("_df",)

    def __init__(self, df: "DataFrame") -> None:
        self._df = df

    def _rows(self, key: object) -> Tuple[Rows, object]:
        df = self._df
        levels = df.index_levels()
        rk, ck = _split(key, len(levels), df.columns)
        return rows_by_label(levels, rk), ck

    def __getitem__(self, key: object) -> object:
        df = self._df
        rows, ck = self._rows(key)
        cols, one_col = columns_by_name(df.columns, ck)
        out = _apply_rows(df, rows)
        if one_col:
            return Series(out._native[cols[0]])
        return out.select(*cols) if ck is not None else out

    def __setitem__(self, key: object, value: object) -> None:
        df = self._df
        rows, ck = self._rows(key)
        cols, _ = columns_by_name(df.columns, ck, new_ok=True)
        assign_rows(df, rows, cols, value)


class At:
    """``df.at[label, column]`` / ``df.iat[position, column]``: one value."""

    __slots__ = ("_df", "_by_position")

    def __init__(self, df: "DataFrame", by_position: bool) -> None:
        self._df = df
        self._by_position = by_position

    def _cell(self, key: object) -> Tuple[Rows, str]:
        if not isinstance(key, tuple) or len(key) != 2:
            raise TypeError("at / iat take [row, column]")
        rk, ck = key
        if self._by_position:
            rows, _ = rows_by_position(len(self._df), rk)
            cols, _ = columns_by_position(self._df.columns, ck)
        else:
            rows = rows_by_label(self._df.index_levels(), rk)
            cols, _ = columns_by_name(self._df.columns, ck, new_ok=True)
        if len(cols) != 1:
            raise TypeError("at / iat name one column")
        return rows, cols[0]

    def __getitem__(self, key: object) -> object:
        rows, name = self._cell(key)
        out = _apply_rows(self._df, rows)
        if len(out) != 1:
            raise KeyError(f"at: the row key matches {len(out)} rows, not one")
        return Series(out._native[name])[0]

    def __setitem__(self, key: object, value: object) -> None:
        rows, name = self._cell(key)
        assign_rows(self._df, rows, [name], value)


# ---- lazy ---------------------------------------------------------------------


def _lazy_rows(lf: "LazyFrame", rows: Rows) -> "LazyFrame":
    from .columnar import Expr

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


def lazy_rows_by_label(lf: "LazyFrame", key: object) -> Rows:
    """``loc`` rows on a plan: an Expr mask over the index column."""
    from .columnar import Expr, col

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

    def label_tuple(values: tuple) -> Expr:
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
    from .columnar import Expr, _collect_columns

    return isinstance(mask, Expr) and _ROW in _collect_columns(mask)


def _lazy_mask_expr(lf: "LazyFrame", rows: Rows) -> "Tuple[LazyFrame, Optional[Expr]]":
    """The plan (with a row index when the selection is positional) and the
    Expr mask the selection names; None for every row."""
    from .columnar import Expr, col

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


def lazy_assign_rows(lf: "LazyFrame", rows: Rows, columns: List[str], value: object) -> None:
    """``lf.loc[rows, columns] = value`` as ``with_column`` plan steps, the
    handle rebound to the longer plan."""
    from .columnar import Expr, col, lit, when

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
    schema = lf.schema()
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
    if plan.schema() != schema:
        plan = plan.select(*schema)
    lf._native = plan._native


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

    def __init__(self, lf: "LazyFrame") -> None:
        self._lf = lf

    def __getitem__(self, key: object) -> "LazyFrame":
        rk, ck = _split(key)
        out = _lazy_rows(self._lf, lazy_rows_by_position(rk))
        if ck is None:
            return out
        cols, _ = columns_by_position(self._lf.schema(), ck)
        return out.select(*cols)

    def __setitem__(self, key: object, value: object) -> None:
        rk, ck = _split(key)
        cols, _ = columns_by_position(self._lf.schema(), ck)
        lazy_assign_rows(self._lf, lazy_rows_by_position(rk), cols, value)


class LazyLoc:
    __slots__ = ("_lf",)

    def __init__(self, lf: "LazyFrame") -> None:
        self._lf = lf

    def _rows(self, key: object) -> Tuple[Rows, object]:
        lf = self._lf
        rk, ck = _split(key, len(lf._index_names()), lf.schema())
        return lazy_rows_by_label(lf, rk), ck

    def __getitem__(self, key: object) -> "LazyFrame":
        lf = self._lf
        rows, ck = self._rows(key)
        cols, _ = columns_by_name(lf.schema(), ck)
        # With no index column the labels are row positions: a hidden row
        # index feeds the mask and is projected away again.
        needs_row = rows.kind == "mask" and _reads_row(rows.mask)
        plan = lf.with_row_index(_ROW) if needs_row else lf
        out = _lazy_rows(plan, rows)
        return out.select(*cols) if (ck is not None or needs_row) else out

    def __setitem__(self, key: object, value: object) -> None:
        rows, ck = self._rows(key)
        cols, _ = columns_by_name(self._lf.schema(), ck, new_ok=True)
        lazy_assign_rows(self._lf, rows, cols, value)


# ---- resample -----------------------------------------------------------------

_UNIT_NS: Dict[str, int] = {
    "ns": 1,
    "us": 1_000,
    "µs": 1_000,
    "ms": 1_000_000,
    "s": 1_000_000_000,
    "sec": 1_000_000_000,
    "min": 60_000_000_000,
    "m": 60_000_000_000,
    "t": 60_000_000_000,
    "h": 3_600_000_000_000,
    "d": 86_400_000_000_000,
}

_RULE = re.compile(r"^\s*(\d+(?:\.\d+)?)\s*([a-zA-Zµ]+)\s*$")


def rule_to_units(rule: Union[int, str], unit: str) -> int:
    """A pandas offset string (``"5s"``, ``"100ms"``, ``"1min"``, ``"2h"``)
    or an int of the column's own units, as a count of ``unit``."""
    if isinstance(rule, bool):
        raise TypeError("resample: rule must be a str or an int")
    if isinstance(rule, int):
        if rule <= 0:
            raise ValueError("resample: rule must be positive")
        return rule
    m = _RULE.match(rule)
    if not m:
        raise ValueError(f"resample: cannot parse rule {rule!r}")
    amount, name = float(m.group(1)), m.group(2).lower()
    if name not in _UNIT_NS:
        raise ValueError(f"resample: unknown unit {m.group(2)!r} in {rule!r}")
    per = _UNIT_NS.get(unit.lower())
    if per is None:
        raise ValueError(f"resample: unknown column unit {unit!r}")
    every = amount * _UNIT_NS[name] / per
    if every < 1 or every != int(every):
        raise ValueError(f"resample: {rule!r} is not a whole number of {unit}")
    return int(every)


class Resampler:
    """``df.resample(rule, on=...)``: a tumbling time window over the index
    (or ``on``) column, with the ``group_by`` aggregate family. Each call is
    one ``group_by_dynamic`` over the frame (or plan) sorted by time."""

    __slots__ = ("_frame", "_time", "_every")

    def __init__(self, frame: "Union[DataFrame, LazyFrame]", time: str, every: int) -> None:
        self._frame = frame
        self._time = time
        self._every = every

    def _run(self, specs: List[str]) -> "Union[DataFrame, LazyFrame]":
        return self._frame.sort_by(self._time).group_by_dynamic(
            self._time, self._every, None, specs
        )

    def agg(self, *specs: object, **named: object) -> "Union[DataFrame, LazyFrame]":
        from .lazyframe import agg_spec_strings

        return self._run(agg_spec_strings(self._frame, [self._time], specs, named))

    aggregate = agg

    def reduce(self, agg: str) -> "Union[DataFrame, LazyFrame]":
        return self._run(list(self._frame._native.reduce_specs(agg, [self._time])))

    def sum(self) -> "Union[DataFrame, LazyFrame]":
        return self.reduce("sum")

    def mean(self) -> "Union[DataFrame, LazyFrame]":
        return self.reduce("mean")

    def min(self) -> "Union[DataFrame, LazyFrame]":
        return self.reduce("min")

    def max(self) -> "Union[DataFrame, LazyFrame]":
        return self.reduce("max")

    def count(self) -> "Union[DataFrame, LazyFrame]":
        return self.reduce("count_valid")

    def var(self) -> "Union[DataFrame, LazyFrame]":
        return self.reduce("var")

    def std(self) -> "Union[DataFrame, LazyFrame]":
        return self.reduce("std")

    def first(self) -> "Union[DataFrame, LazyFrame]":
        return self.reduce("first")

    def last(self) -> "Union[DataFrame, LazyFrame]":
        return self.reduce("last")

    def size(self) -> "Union[DataFrame, LazyFrame]":
        return self._run(["count::size"])


__all__ = ["ILoc", "Loc", "At", "LazyILoc", "LazyLoc", "Resampler", "Rows", "rule_to_units"]
