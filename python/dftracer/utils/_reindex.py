# SPDX-License-Identifier: MIT
"""``reindex``, ``reindex_like`` and ``align`` by row label for a
:class:`Series` and a :class:`DataFrame`, as pandas 2 and 3 have them.

The rows go through the native label index (``_reindex``, ``_align``,
``_align_rows``): a label lookup with the fill methods, and the join of two
label sets. This module turns the pandas arguments into the native ones and
does the column side, which is a list of names."""

from __future__ import annotations

import datetime
import math
import sys
from typing import TYPE_CHECKING, Sequence, Union, cast

from . import dftracer_utils_ext as _ext
from ._convert import _nanos, missing, timedelta_nanos, values_like
from ._enums import UNIT_NS, DType
from ._series import Series
from ._series_index import default_name, internal_name, labels_from, levels_of, names_of

if TYPE_CHECKING:
    from ._dataframe import DataFrame

Frameish = Union[Series, "DataFrame"]

_METHODS = {None: 0, "pad": 1, "ffill": 1, "backfill": 2, "bfill": 2, "nearest": 3}
_JOINS = {"inner": 0, "left": 1, "right": 2, "outer": 3}
_SWAPPED = {"inner": "inner", "left": "right", "right": "left", "outer": "outer"}
_TEXT = frozenset({DType.STRING, DType.LARGE_STRING, DType.BINARY, DType.LARGE_BINARY})
_DAY_NS = UNIT_NS["d"]
_ONE = "__align_series__"
_NO_LIMIT = -1


def method_code(method: str | None) -> int:
    if method not in _METHODS:
        raise ValueError(
            f"Invalid fill method. Expecting pad (ffill), backfill (bfill) or nearest. Got {method}"
        )
    return _METHODS[method]


def join_code(join: str) -> int:
    if join not in _JOINS:
        raise ValueError(f"do not recognize join method {join}")
    return _JOINS[join]


def axis_of(axis: object, kind: str) -> int:
    """0 for the rows, 1 for the columns."""
    if axis in (0, "index", "rows"):
        return 0
    if axis in (1, "columns"):
        return 1
    raise ValueError(f"No axis named {axis} for object type {kind}")


def _scalar(value: object) -> object:
    item = getattr(value, "item", None)
    return item() if callable(item) and not isinstance(value, (str, bytes)) else value


def _fill_series(value: object) -> Series | None:
    if missing(value):
        return None
    return Series.from_list([_scalar(value)])


def _timedelta_nanos(value: object) -> int | None:
    """``value`` as whole nanoseconds when it is a time span (a string such as
    ``"5s"`` is parsed by pandas), else ``None``."""
    if isinstance(value, str):
        try:
            import pandas
        except ImportError:
            raise TypeError("reindex: a string tolerance needs pandas to parse it") from None
        value = pandas.Timedelta(value)
    if isinstance(value, datetime.timedelta):
        return timedelta_nanos(value)
    np = sys.modules.get("numpy")
    if np is not None and isinstance(value, np.timedelta64):
        return _nanos(value)
    return None


def _tolerance(value: object, level: Series | None) -> tuple[float, int | None]:
    """The tolerance as the native call takes it: ``(number, None)`` in the
    units of the labels, or ``(0.0, nanoseconds)`` for a time span against
    timestamp, duration or time labels, which the engine compares exactly in
    the unit of the labels (days for date32 labels, milliseconds for date64)."""
    if value is None:
        return math.inf, None
    if isinstance(value, (list, tuple)) or (
        hasattr(value, "__len__") and not isinstance(value, str)
    ):
        raise NotImplementedError("reindex: a tolerance per label is not supported")
    nanos = _timedelta_nanos(value)
    if nanos is None:
        return float(cast("float", value)), None
    if level is None:
        raise TypeError("reindex: a timedelta tolerance needs timestamp labels")
    if level.dtype in (DType.TIMESTAMP, DType.DURATION, DType.TIME32, DType.TIME64):
        return 0.0, nanos
    if level.dtype == DType.DATE32:
        return nanos / _DAY_NS, None
    if level.dtype == DType.DATE64:
        # date64 counts milliseconds
        return nanos / 1_000_000, None
    raise TypeError("reindex: a timedelta tolerance needs timestamp, date or duration labels")


def _source(obj: Frameish) -> tuple[list[str], list[Series]]:
    """The label names and columns of a Series or frame; empty for none."""
    if isinstance(obj, Series):
        return names_of(obj), levels_of(obj)
    names = list(obj.index_names())
    return names, [_ext.DataFrame.__getitem__(obj, n) for n in names]


def _target(
    labels: object, source: list[str], levels: list[Series]
) -> tuple[list[str], list[Series]]:
    """The names and key columns of the labels a caller gives. A plain
    sequence takes the names of the labels it replaces; a pandas ``Index``, a
    Series or a frame brings its own."""
    names, keys = labels_from(labels, None)
    if len(keys[0]) == 0 and levels and len(keys) == len(levels):
        keys = [level.slice(0, 0) for level in levels]
    elif len(keys) == len(levels):
        keys = [_like_level(key, level) for key, level in zip(keys, levels)]
    if hasattr(labels, "name") and len(keys) == 1:
        names = [internal_name(labels.name, 0)]
    elif not hasattr(labels, "names") and not hasattr(labels, "columns"):
        names = source if len(source) == len(keys) else [default_name(i) for i in range(len(keys))]
    return names, keys


# Label types that Python values do not build by themselves: a key column of
# Python dates, times or decimals takes the level's type first.
_PARAMETRIC = frozenset(
    {DType.DATE64, DType.TIME32, DType.TIME64, DType.DECIMAL128, DType.DECIMAL256}
)


def _like_level(key: Series, level: Series) -> Series:
    """``key`` in the type of ``level`` when the level has a type that Python
    values do not build (date64, a time unit, a decimal scale); else ``key``."""
    same = (
        key.dtype == level.dtype
        and key.time_unit == level.time_unit
        and key._decimal == level._decimal
    )
    if level.dtype not in _PARAMETRIC or same:
        return key
    try:
        return values_like(key.to_list(), level)
    except (TypeError, ValueError):
        return key


def _renamed(out: Frameish, names: list[str]) -> Frameish:
    """``out`` with the names of its row labels set to ``names``."""
    have = names_of(out) if isinstance(out, Series) else list(out.index_names())
    if have == names:
        return out
    if isinstance(out, Series):
        return _ext.Series._with_index(out, names, levels_of(out))
    return out.rename(dict(zip(have, names)))


def _checks(
    method: str | None, limit: int | None, tolerance: object, depth: int, levels: list[Series]
) -> int:
    code = method_code(method)
    if limit is not None and limit <= 0:
        raise ValueError("Limit must be greater than 0")
    if depth > 1:
        if code == 3:
            raise NotImplementedError(
                "method='nearest' not implemented yet for MultiIndex; see GitHub issue 9365"
            )
        if tolerance is not None and code != 0:
            raise NotImplementedError("tolerance not implemented yet for MultiIndex")
    if levels and levels[0].dtype in _TEXT and (code == 3 or (tolerance is not None and code != 0)):
        raise TypeError(
            "reindex: the nearest method and a tolerance need numeric or temporal labels"
        )
    return code


def _convert(error: ValueError) -> ValueError | TypeError:
    text = str(error)
    if "does not fit a column" in text or "not supported between" in text:
        return TypeError(text)
    return error


def reindex_rows(
    obj: Frameish,
    labels: object,
    method: str | None,
    fill_value: object,
    limit: int | None,
    tolerance: object,
) -> Frameish:
    """``obj`` on the row labels ``labels``, by the native label lookup."""
    source, levels = _source(obj)
    names, keys = _target(labels, source, levels)
    code = _checks(method, limit, tolerance, len(keys), levels)
    ticks, nanos = _tolerance(tolerance, levels[0] if levels else None)
    try:
        out = obj._reindex(
            keys,
            code,
            _NO_LIMIT if limit is None else limit,
            ticks,
            _fill_series(fill_value),
            nanos,
        )
    except ValueError as error:
        raise _convert(error) from None
    return _renamed(out, names)


def reindex_series(
    s: Series,
    index: object,
    *,
    axis: object,
    method: str | None,
    level: object,
    fill_value: object,
    limit: int | None,
    tolerance: object,
) -> Series:
    if axis is not None and axis_of(axis, "Series") != 0:
        raise ValueError(f"No axis named {axis} for object type Series")
    if level is not None:
        raise NotImplementedError("reindex: level is not supported")
    if index is None:
        return s
    return cast(Series, reindex_rows(s, index, method, fill_value, limit, tolerance))


def _names(labels: object) -> list[str]:
    out = [] if labels is None else list(cast("Sequence[object]", labels))
    for name in out:
        if not isinstance(name, str):
            raise TypeError("reindex: column labels must be strings")
    if len(set(out)) != len(out):
        raise ValueError("reindex: a column label repeats")
    return cast("list[str]", out)


def with_columns(df: DataFrame, names: list[str], fill_value: object) -> DataFrame:
    """``df`` with the data columns ``names`` in that order: the columns it has,
    and for each other name a column of nulls (Float64) or of ``fill_value``."""
    index = list(df.index_names())
    have = list(df.columns)
    if have == names:
        return df
    out = df
    rows = len(df)
    fill = _fill_series(fill_value)
    for name in names:
        if name in have:
            continue
        if fill is None:
            column = _ext._series_nulls(int(DType.FLOAT64), rows)
        else:
            column = fill.take([0] * rows)
        out = out.with_column(name, column)
    out = out.select(*names, *index)
    if index and not out.index_names():
        out = out.set_index(index)
    return out


def reindex_frame(
    df: DataFrame,
    labels: object,
    *,
    index: object,
    columns: object,
    axis: object,
    method: str | None,
    level: object,
    fill_value: object,
    limit: int | None,
    tolerance: object,
) -> DataFrame:
    if level is not None:
        raise NotImplementedError("reindex: level is not supported")
    rows: object = None
    cols: object = None
    if labels is not None:
        if axis is not None:
            if index is not None or columns is not None:
                raise TypeError("Cannot specify both 'axis' and any of 'index' or 'columns'")
            if axis_of(axis, "DataFrame") == 0:
                rows = labels
            else:
                cols = labels
        elif index is not None and columns is not None:
            raise TypeError("Cannot specify all of 'labels', 'index', 'columns'")
        elif index is not None:
            cols = labels
        else:
            rows = labels
    elif axis is not None and (index is not None or columns is not None):
        raise TypeError("Cannot specify both 'axis' and any of 'index' or 'columns'")
    if index is not None:
        rows = index
    if columns is not None:
        cols = columns
    method_code(method)
    out = df
    if cols is not None:
        if method is not None:
            raise NotImplementedError("reindex: a fill method applies to the row index only")
        out = with_columns(out, _names(cols), fill_value)
    if rows is not None:
        out = cast("DataFrame", reindex_rows(out, rows, method, fill_value, limit, tolerance))
    return out


def label_keys(other: Frameish) -> object:
    """The row labels of ``other``, as a ``reindex`` target that keeps their
    names: a Series for one level, a frame of the levels for several, the
    positions for none."""
    if isinstance(other, Series):
        names, levels = names_of(other), levels_of(other)
    else:
        names, levels = _source(other)
    if not names:
        from ._series_index import positions

        return positions(len(other))
    from ._dataframe import DataFrame

    if len(names) == 1:
        return levels[0].rename(names[0])
    return DataFrame.from_dict(dict(zip(names, levels)))


def reindex_like(
    obj: Frameish, other: Frameish, method: str | None, limit: int | None, tolerance: object
) -> Frameish:
    """``obj`` on the row labels of ``other`` and, for a frame, its columns."""
    from ._dataframe import DataFrame

    out = obj
    if not isinstance(obj, Series):
        if not isinstance(other, DataFrame):
            raise TypeError(f"reindex_like: expected a DataFrame, got {type(other).__name__}")
        out = with_columns(obj, list(other.columns), None)
    return reindex_rows(out, label_keys(other), method, None, limit, tolerance)


def _joined_names(a: list[str], b: list[str], how: str) -> list[str]:
    if a == b or how == "left":
        return a
    if how == "right":
        return b
    if how == "inner":
        keep = set(b)
        return [n for n in a if n in keep]
    return sorted(set(a) | set(b))


def _filled(s: Series, fill_value: object) -> Series:
    return s if missing(fill_value) else s.fill_null(cast("float | str", _scalar(fill_value)))


def align(
    obj: Frameish, other: Frameish, join: str, axis: object, level: object, fill_value: object
) -> tuple[Frameish, Frameish]:
    """``obj.align(other, ...)`` for a Series or a frame."""
    if isinstance(obj, Series):
        return align_series(obj, other, join, axis, level, fill_value)
    return align_frame(obj, other, join, axis, level, fill_value)


def align_series(
    s: Series, other: Frameish, join: str, axis: object, level: object, fill_value: object
) -> tuple[Series, Frameish]:
    if axis is not None and axis_of(axis, "Series") != 0:
        raise ValueError(f"No axis named {axis} for object type Series")
    if level is not None:
        raise NotImplementedError("align: level is not supported")
    code = join_code(join)
    if isinstance(other, Series):
        a, b = s._align(other, code)
        return _filled(a, fill_value), _filled(b, fill_value)
    from ._dataframe import DataFrame

    if not isinstance(other, DataFrame):
        raise TypeError(f"unsupported type: {type(other)}")
    frame, one = align_frame(other, s, _SWAPPED[join], 0, None, fill_value)
    return cast(Series, one), frame


def align_frame(
    df: DataFrame, other: Frameish, join: str, axis: object, level: object, fill_value: object
) -> tuple[DataFrame, Frameish]:
    if level is not None:
        raise NotImplementedError("align: level is not supported")
    code = join_code(join)
    which = None if axis is None else axis_of(axis, "DataFrame")
    fill = _fill_series(fill_value)
    if isinstance(other, Series):
        if which is None:
            raise ValueError("Must specify axis=0 or 1")
        if which == 0:
            from ._series_index import to_frame

            left, right = _ext.DataFrame._align_rows(df, to_frame(other, _ONE), code, fill)
            return left, right[_ONE].rename(other.name)
        labels = other.index
        if not isinstance(labels, Series):
            raise NotImplementedError("align: a Series with several index levels against columns")
        columns = _joined_names(list(df.columns), [str(x) for x in labels.to_list()], join)
        return with_columns(df, columns, fill_value), cast(
            Series, reindex_rows(other, columns, None, fill_value, None, None)
        )
    from ._dataframe import DataFrame

    if not isinstance(other, DataFrame):
        raise TypeError(f"unsupported type: {type(other)}")
    left, right = df, other
    if which != 0:
        columns = _joined_names(list(left.columns), list(right.columns), join)
        left = with_columns(left, columns, fill_value)
        right = with_columns(right, columns, fill_value)
    if which != 1:
        left, right = _ext.DataFrame._align_rows(left, right, code, fill)
    return left, right
