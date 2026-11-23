# SPDX-License-Identifier: MIT
"""``stack`` and ``unstack`` on the row labels, written over the native row
numbering, take and unpivot.

``unstack`` numbers the distinct values of the remaining levels (the result
rows) and of the unstacked ones (the result columns), finds the row that fills
each cell of the result (a cell named twice is an error) and takes each value
column at those rows, so a cell no row fills is null. ``stack``
runs the native unpivot over the shown columns and reads its column-major rows
in row-major order with one take.

A frame has flat column names, so the columns an ``unstack`` of a frame makes
are named with the text of the tuple ``(column, label)``, the name
:meth:`DataFrame.from_pandas` gives a MultiIndex column."""

from __future__ import annotations

from typing import TYPE_CHECKING, Sequence, cast

from . import dftracer_utils_ext as _ext
from ._enums import DType
from ._levels import Level, LevelArg, pick_levels
from ._series_index import (
    default_name,
    drop_levels,
    levels_of,
    names_of,
    positions,
    public_name,
    with_levels,
)

if TYPE_CHECKING:
    from ._dataframe import DataFrame
    from ._series import Series

_NOT_UNIQUE = "Index contains duplicate entries, cannot reshape"


def _first_rows(levels: Sequence[Series], sort: bool) -> tuple[Series, Series, int]:
    """Number the distinct rows of ``levels`` (null equals null): the number of
    each row, the row that first holds each number, and how many numbers there
    are. The numbers follow the sorted order, nulls first, or with ``sort=False``
    the order of first appearance."""
    ids, first = _ext._number_rows([drop_levels(level) for level in levels], sort)
    return ids, first, len(first)


def _take(s: Series, at: Series) -> Series:
    """The rows ``at`` of ``s``, native, without the labels ``take`` gives them."""
    return _ext.Series.take(drop_levels(s), at)


def _text(label: object) -> str:
    if label is None:
        return "nan"
    if isinstance(label, tuple):
        return str(tuple(float("nan") if x is None else x for x in label))
    return str(label)


def _labels(levels: Sequence[Series], first: Series) -> list[object]:
    """The label of each unstacked column: a value, or a tuple for several levels."""
    taken = [_take(lv, first).to_list() for lv in levels]
    if len(taken) == 1:
        return list(taken[0])
    return list(zip(*taken))


def _fill(column: Series, fill_value: object, seen: Series | None) -> Series:
    """``column`` with ``fill_value`` in the cells the Series had no value for
    (``seen`` is null there); a null that the Series itself holds stays. Text
    takes a text fill, Bool a Bool fill and the other types a number (a Bool
    is not a number): ``TypeError`` otherwise, where pandas mixes the types in
    an object column."""
    if fill_value is None:
        return column
    kind = column.dtype
    if kind == DType.STRING:
        fits = isinstance(fill_value, str)
    elif kind == DType.BOOL:
        fits = isinstance(fill_value, bool)
    else:
        fits = isinstance(fill_value, (int, float)) and not isinstance(fill_value, bool)
    if not fits:
        raise TypeError(f"Invalid fill_value {fill_value!r} for a {kind.name.lower()} column")
    if kind in (DType.STRING, DType.BOOL):
        from ._series import Series

        keep = seen.is_not_null() if seen is not None else column.is_not_null()
        return column.where(keep, Series.from_list([fill_value] * len(column)))
    if column.dtype.is_integer and isinstance(fill_value, float) and not fill_value.is_integer():
        column = column.cast(DType.FLOAT64)
    if seen is None:
        return column.fill_null(cast("float", fill_value))
    return column.where(seen.is_not_null(), cast("float", fill_value))


def _wide(
    names: list[str],
    levels: list[Series],
    picked: list[int],
    values: list[tuple[str | None, Series]],
    fill_value: object,
    sort: bool,
) -> DataFrame:
    """The unstacked frame: the other levels as the index and, for each value
    column, one column per distinct label of the ``picked`` levels (in the
    order given, for a tuple). ``None`` names a lone Series, whose columns are
    the labels themselves. One level's labels are sorted; several are in order
    of first appearance, as pandas has them."""
    from ._dataframe import DataFrame

    keep = [i for i in range(len(names)) if i not in picked]
    rows, row_first, row_count = _first_rows([levels[i] for i in keep], sort)
    cols, col_first, col_count = _first_rows([levels[i] for i in picked], sort and len(picked) == 1)
    try:
        cells = _ext._unstack_positions(rows, cols, row_count, col_count)
    except ValueError as error:
        if "same cell" in str(error):
            raise ValueError(_NOT_UNIQUE) from None
        raise
    labels = _labels([levels[i] for i in picked], col_first)
    out: dict[str, Series] = {names[i]: _take(levels[i], row_first) for i in keep}
    for column, value in values:
        wide = _ext._unstack_columns(value, cells, row_count)
        seen = None
        if fill_value is not None and value.null_count > 0:
            seen = _ext._unstack_columns(rows.is_not_null(), cells, row_count)
        for j, label in enumerate(labels):
            title = _text(label if column is None else (column, *_tuple(label)))
            if title in out:
                raise ValueError(f"unstack: the column {title!r} is also an index level name")
            out[title] = _fill(wide[j], fill_value, None if seen is None else seen[j])
    frame = DataFrame.from_dict(out)
    if row_count != len(frame):
        raise ValueError("unstack: the native pivot lost rows")
    return frame.set_index([names[i] for i in keep])


def _tuple(label: object) -> tuple[object, ...]:
    return label if isinstance(label, tuple) else (label,)


def _empty(names: list[str], levels: list[Series], picked: list[int]) -> DataFrame:
    from ._dataframe import DataFrame

    keep = [names[i] for i in range(len(names)) if i not in picked]
    frame = DataFrame.from_dict({name: levels[names.index(name)][0:0] for name in keep})
    return frame.set_index(keep)


def _split(names: list[str], level: LevelArg) -> list[int]:
    """The positions of ``level`` in the order given, each once."""
    picked: list[int] = []
    for at in pick_levels(names, level):
        if at not in picked:
            picked.append(at)
    return picked


def _check_flat(names: list[str], level: LevelArg) -> None:
    """A one-level index takes any position as its only level, as pandas does,
    but a name must be its own (``KeyError``) and a list of several levels must
    exist (``IndexError``)."""
    items = list(level) if isinstance(level, (list, tuple)) else [level]
    if len(items) > 1:
        pick_levels(names or [default_name(0)], cast("list[Level]", items))
        return
    named = [x for x in items if not isinstance(x, int) or isinstance(x, bool)]
    if named:
        pick_levels(names or [default_name(0)], cast("list[Level]", named))


def unstack_series(
    s: Series, level: LevelArg, fill_value: object, sort: bool
) -> DataFrame | Series:
    """``Series.unstack``: the levels ``level`` become the columns."""
    names = names_of(s)
    if isinstance(level, (list, tuple)) and not level:
        return s
    if len(names) < 2:
        _check_flat(names, level)
        kind = "RangeIndex" if not names else "Index"
        raise ValueError(
            f"index must be a MultiIndex to unstack, <class 'pandas.{kind}'> was passed"
        )
    picked = _split(names, level)
    if len(picked) >= len(names):
        raise ValueError("index must be a MultiIndex to unstack, <class 'pandas.Index'> was passed")
    levels = levels_of(s)
    if len(s) == 0:
        return _empty(names, levels, picked)
    return _wide(names, levels, picked, [(None, drop_levels(s))], fill_value, sort)


def unstack_frame(
    frame: DataFrame, level: LevelArg, fill_value: object, sort: bool
) -> DataFrame | Series:
    """``DataFrame.unstack``: with several index levels each shown column
    spreads its ``level`` into columns named ``(column, label)``; with one level
    (or none), or with every level named, the result is a Series labelled by the
    column and the old labels."""
    names = frame.index_names()
    shown = frame.columns
    if isinstance(level, (list, tuple)) and not level:
        return frame
    if len(names) < 2:
        _check_flat(names, level)
        return _flat(frame, names, list(range(len(names))), shown)
    picked = _split(names, level)
    if len(picked) == len(names):
        return _flat(frame, names, picked, shown)
    levels = [drop_levels(_ext.DataFrame.__getitem__(frame, name)) for name in names]
    if len(frame) == 0:
        return _empty(names, levels, picked)
    values: list[tuple[str | None, Series]] = [
        (c, drop_levels(_ext.DataFrame.__getitem__(frame, c))) for c in shown
    ]
    return _wide(names, levels, picked, values, fill_value, sort)


def _flat(frame: DataFrame, names: list[str], picked: list[int], shown: list[str]) -> Series:
    """The frame read as its transpose stacked: the rows of each column in
    turn, labelled by the column name and the ``picked`` levels (the row
    positions for a frame with no index)."""
    rows = len(frame)
    old = [drop_levels(_ext.DataFrame.__getitem__(frame, names[i])) for i in picked] or [
        positions(rows)
    ]
    if rows and _first_rows(old, False)[2] != rows:
        raise ValueError("Columns with duplicate values are not supported in stack")
    long = _ext.DataFrame.unpivot(_typed(frame, shown), [], shown)
    variable = drop_levels(_ext.DataFrame.__getitem__(long, "variable"))
    value = drop_levels(_ext.DataFrame.__getitem__(long, "value"))
    again = [_take(lv, positions(len(variable)) % rows) if rows else lv for lv in old]
    kept = [names[i] for i in picked] or [default_name(0)]
    labels = [n if public_name(n) is not None else default_name(i + 1) for i, n in enumerate(kept)]
    return with_levels(value.rename(None), [default_name(0), *labels], [variable, *again])


def _typed(frame: DataFrame, shown: list[str]) -> DataFrame:
    """The shown columns, where a column of only nulls (it has no type of its
    own, from_pandas makes it text) takes the type of the first column that holds
    a value, so the unpivot sees one type, as pandas stacks it."""
    from ._dataframe import DataFrame

    columns = {c: drop_levels(_ext.DataFrame.__getitem__(frame, c)) for c in shown}
    live = [s for s in columns.values() if s.null_count < len(s)]
    empty = [c for c, s in columns.items() if s.null_count == len(s) and len(s)]
    if not live or not empty:
        return _ext.DataFrame.select(frame, *shown)
    for c in empty:
        columns[c] = columns[c].cast(live[0].dtype)
    return DataFrame.from_dict(columns)


def stack_frame(
    frame: DataFrame, level: LevelArg, dropna: bool | None, sort: bool | None, future: bool
) -> Series | DataFrame:
    """``DataFrame.stack``: the shown columns become the innermost index level
    and the result is a Series. Rows come row by row, each row's columns in
    column order. With ``future`` every cell is kept and ``dropna`` and ``sort``
    are refused as pandas refuses them; otherwise null cells are dropped unless
    ``dropna=False``. The column names are one level, so ``level`` is 0, -1 or
    that level's (absent) name."""
    from ._dataframe import DataFrame

    if future:
        if dropna is not None:
            raise ValueError(
                "dropna must be unspecified as the new implementation does not introduce "
                "rows of NA values. This argument will be removed in a future version of pandas."
            )
        if sort is not None:
            raise ValueError(
                "Cannot specify sort, this argument will be removed in a future version of "
                "pandas. Sort the result using .sort_index instead."
            )
    pick_levels([default_name(0)], level)
    shown = frame.columns
    if not shown:
        return DataFrame.from_dict({})
    names = frame.index_names()
    rows = len(frame)
    width = len(shown)
    levels = (
        [drop_levels(_ext.DataFrame.__getitem__(frame, name)) for name in names]
        if names
        else [positions(rows)]
    )
    names = names or [default_name(0)]
    long = _ext.DataFrame.unpivot(_typed(frame, shown), [], shown)
    value = drop_levels(_ext.DataFrame.__getitem__(long, "value"))
    variable = drop_levels(_ext.DataFrame.__getitem__(long, "variable"))
    if rows:
        at = positions(rows * width)
        source = (at % width) * rows + at // width
        value = _take(value, source)
        variable = _take(variable, source)
        levels = [_take(lv, at // width) for lv in levels]
    else:
        levels = [lv[0:0] for lv in levels]
    keep = None if future or dropna is False else value.is_not_null()
    levels = [*levels, variable]
    if keep is not None:
        value = drop_levels(value.filter(keep))
        levels = [drop_levels(lv.filter(keep)) for lv in levels]
    return with_levels(value.rename(None), [*names, default_name(len(names))], levels)
