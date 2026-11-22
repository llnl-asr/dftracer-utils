# SPDX-License-Identifier: MIT
"""The group-by results that pandas labels with the group keys in front
(``group_keys=True``, the default): ``apply``, ``take``, ``nlargest`` and
``nsmallest``. The row label of the result is the key of the row's group, then
the label the row had in the grouped frame (its index, or its position when the
frame has none). Used when the group-by is the pandas ``groupby`` with
``as_index=True``; the groups come in key order (``sort=True``) or in order of
first appearance."""

from __future__ import annotations

from typing import TYPE_CHECKING, Sequence, cast

from . import dftracer_utils_ext as _ext
from ._enums import DType
from ._expr import Agg, Col
from ._series import Series
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
    from ._groupby import GroupBy

ROW = "__dftu_group_row__"
FIRST = "__dftu_group_first__"
ORD = "__dftu_group_ord__"
VALUE = "__dftu_group_value__"


def _column(frame: DataFrame, name: str) -> Series:
    return _ext.DataFrame.__getitem__(frame, name)


def pandas_named(g: GroupBy) -> bool:
    """Whether ``g`` is the pandas ``groupby`` (``as_index`` True or False), as
    against the Polars ``group_by``: its results carry row labels."""
    return g._as_index is not None and bool(g._keys)


def with_keys(g: GroupBy) -> bool:
    """Whether the results of ``g`` lead with the group keys (``as_index``)."""
    return g._as_index is not False


def row_labels(g: GroupBy, positions: Series) -> tuple[list[str], list[Series]]:
    """The labels the rows at ``positions`` of the grouped frame had: the levels
    of the frame's index, or the positions themselves when it has none."""
    origin = g._origin
    if g._rows is not None:
        # The positions are rows of the kept frame; the labels are the ones
        # the rows had before a null key left them out.
        positions = g._rows.gather(positions)
    names = list(_ext.DataFrame.index_names(origin)) if origin is not None else []
    if names and origin is not None:
        return names, [_column(origin, n).gather(positions) for n in names]
    return [default_name(0)], [positions]


def leading_levels(g: GroupBy, positions: Series) -> tuple[list[str], list[Series]]:
    """The key levels (when ``g`` indexes by them) followed by the row labels,
    for the rows at ``positions``; an unnamed label level is numbered after the
    keys."""
    frame = g._frame()
    keys = list(g._keys) if with_keys(g) else []
    label_names, label_levels = row_labels(g, positions)
    names = [
        default_name(len(keys) + j) if public_name(n) is None else n
        for j, n in enumerate(label_names)
    ]
    names = [*keys, *names]
    if len(set(names)) != len(names):
        raise ValueError("a group key has the name of an index level of the frame")
    return names, [*[_column(frame, k).gather(positions) for k in keys], *label_levels]


def attach(frame: DataFrame, names: Sequence[str], levels: Sequence[Series]) -> DataFrame:
    """``frame`` indexed by ``levels`` under ``names``; ``ValueError`` when a
    name is already a column (a frame holds no duplicate names)."""
    taken = frame._names()
    for name in names:
        if name in taken:
            raise ValueError(f"cannot insert {name}, already exists")
    for name, level in zip(names, levels):
        frame = _ext.DataFrame.with_column(frame, name, level)
    return frame.set_index(list(names))


def first_appearance(g: GroupBy, indexed: DataFrame) -> DataFrame:
    """``indexed`` (the grouped frame with the row column ``ROW``) with the
    column ``FIRST``: the first row of each row's group."""
    from ._groupby import GroupBy as _GroupBy

    firsts = _GroupBy(indexed, g._keys).agg(Agg("min", Col(ROW), FIRST))
    return indexed.join(firsts, on=g._keys)


def group_sorted(g: GroupBy, frame: DataFrame, *tie: str) -> DataFrame:
    """``frame`` (with the key columns, and ``FIRST`` when ``g`` does not sort)
    in group order, then in the order of the ``tie`` columns."""
    if g._sort:
        return frame.sort_by_multi([*g._keys, *tie])
    return frame.sort_by_multi([FIRST, *tie])


def group_order(g: GroupBy) -> Series:
    """The row positions of the grouped frame in group order, each group's rows
    in input order."""
    frame = g._frame()
    indexed = frame.with_row_index(ROW)
    if not g._sort:
        indexed = first_appearance(g, indexed)
    return _column(group_sorted(g, indexed, ROW), ROW)


def labelled_series(g: GroupBy, values: Series) -> Series:
    """``values`` (one for each row of the grouped frame, in input order) in
    group order under the keys (with ``as_index``) and the row labels."""
    order = group_order(g)
    names, levels = leading_levels(g, order)
    return with_levels(drop_levels(values.gather(order)), names, levels)


def labelled_frame(g: GroupBy, values: DataFrame) -> DataFrame:
    """``values`` (one row for each row of the grouped frame, in input order, the
    value columns only) in group order under the keys (with ``as_index``) and
    the row labels: the shape of a pandas ``groupby(...).rolling(...)``."""
    order = group_order(g)
    names, levels = leading_levels(g, order)
    plain = values.select(*values.columns)
    return attach(plain.gather(order), names, levels)


def group_frame(g: GroupBy, rows: list[int]) -> DataFrame:
    """The rows ``rows`` of the grouped frame without the key columns, under
    the labels they had."""
    inner = g._frame().select(*[c for c in g._frame()._names() if c not in g._keys])
    positions = Series.from_list(rows)
    names, levels = row_labels(g, positions)
    return attach(inner.gather(rows), names, levels)


def take(g: GroupBy, indices: Sequence[int]) -> DataFrame:
    """``GroupBy.take``: the rows at ``indices`` within each group, in group
    order and the order of ``indices``, under the keys and the row labels, the
    key columns left out. ``IndexError`` when a group is too small."""
    from ._groupby import GroupBy as _GroupBy

    frame = g._frame()
    rest = [c for c in frame._names() if c not in g._keys]
    if not indices:
        return frame.head(0).select(*rest)
    smallest = int(cast("int", g._size_frame()["size"].min()))
    for i in indices:
        if not -smallest <= int(i) < smallest:
            raise IndexError("indices are out-of-bounds")
    indexed = frame.with_row_index(ROW)
    parts = []
    for j, i in enumerate(indices):
        part = _GroupBy(indexed, g._keys).nth(int(i))
        parts.append(part.with_column(ORD, _column(part, ROW).full_like(j)))
    out = parts[0].concat(*parts[1:]) if len(parts) > 1 else parts[0]
    if not g._sort:
        firsts = first_appearance(g, indexed).select(*g._keys, FIRST).unique(g._keys)
        out = out.join(firsts, on=g._keys)
    out = group_sorted(g, out, ORD)
    names, levels = leading_levels(g, _column(out, ROW))
    return attach(out.select(*rest), names, levels)


def top(g: GroupBy, value: str, n: int, largest: bool) -> Series:
    """``SeriesGroupBy.nlargest`` / ``nsmallest``: per group the ``n`` largest
    (or smallest) present values of ``value`` as a Series named ``value``, each
    group in order, under the keys (when ``g`` indexes by them) and the row
    labels."""
    from ._groupby import GroupBy as _GroupBy

    frame = g._frame()
    indexed = frame.with_row_index(ROW)
    if n <= 0:
        picked = indexed.head(0)
    else:
        # A null or NaN ranks last, so it fills a group that has fewer than
        # ``n`` numbers, as pandas.
        rank = "__dftu_rank__"
        column = _column(indexed, value)
        if column.dtype in (DType.FLOAT32, DType.FLOAT64):
            column = column.where(column.is_nan().logical_not())
        ranked = indexed.with_column(rank, column)
        if not g._sort:
            ranked = first_appearance(g, ranked)
        lead = list(g._keys) if g._sort else [FIRST]
        ranked = ranked.sort_by_multi(
            [*lead, rank], [False] * len(lead) + [largest], nulls_last=True
        )
        picked = _GroupBy(ranked, g._keys).head(n)
    names, levels = leading_levels(g, _column(picked, ROW))
    return with_levels(_column(picked, value), names, levels)


def apply_pieces(g: GroupBy, results: list[tuple[object, object]]) -> DataFrame | Series:
    """The per-group results of ``apply`` that are frames or Series as one frame
    or Series under the keys and the labels each result had (a result with no
    labels has the positions ``0 .. n-1``, as a pandas ``RangeIndex``). Series
    that all have the same labels become the rows of one frame, one column for
    each label, as pandas."""
    from ._dataframe import DataFrame

    keys = list(g._keys) if with_keys(g) else []
    frame = g._frame()
    first = results[0][1]
    if isinstance(first, Series) and keys:
        wide = _wide(g, results)
        if wide is not None:
            return wide
    pieces: list[DataFrame] = []
    names: list[str] = []
    for label, result in results:
        body: dict[str, Series]
        if isinstance(result, Series):
            level_names, levels = names_of(result), levels_of(result)
            body = {VALUE: drop_levels(result)}
            length = len(result)
        else:
            assert isinstance(result, DataFrame)
            level_names = list(_ext.DataFrame.index_names(result))
            levels = [_column(result, n) for n in level_names]
            body = {c: _column(result, c) for c in result.columns}
            length = len(result)
        if not level_names:
            level_names, levels = [default_name(0)], [positions(length)]
        parts = label if isinstance(label, tuple) else (label,)
        key_columns = {
            k: Series.from_list([v] * length).cast(_column(frame, k).dtype)
            for k, v in zip(keys, parts)
        }
        labels = {
            (default_name(len(keys) + j) if public_name(n) is None else n): level
            for j, (n, level) in enumerate(zip(level_names, levels))
        }
        columns = {**key_columns, **labels, **body}
        if len(columns) != len(keys) + len(labels) + len(body):
            raise ValueError("a result column has the name of a group key or an index level")
        pieces.append(DataFrame.from_dict(columns))
        names = [*keys, *labels]
    out = pieces[0].concat(*pieces[1:]) if len(pieces) > 1 else pieces[0]
    out = out.set_index(names)
    if isinstance(first, Series):
        return out[VALUE].rename(first.name)
    return out


def _wide(g: GroupBy, results: list[tuple[object, object]]) -> DataFrame | None:
    """One row per group when every result is a Series with the same labels:
    the labels are the columns, the group keys the index."""
    from ._dataframe import DataFrame

    series = [r for _, r in results]
    if not all(isinstance(s, Series) and names_of(s) for s in series):
        return None
    first = series[0]
    assert isinstance(first, Series)
    labels = [lv.to_list() for lv in levels_of(first)]
    for s in series[1:]:
        assert isinstance(s, Series)
        if [lv.to_list() for lv in levels_of(s)] != labels or len(names_of(s)) != 1:
            return None
    if len(labels) != 1:
        return None
    keys = list(g._keys)
    frame = g._frame()
    columns: dict[str, list[object]] = {k: [] for k in keys}
    cells: dict[str, list[object]] = {str(x): [] for x in labels[0]}
    for label, s in results:
        parts = label if isinstance(label, tuple) else (label,)
        for k, v in zip(keys, parts):
            columns[k].append(v)
        assert isinstance(s, Series)
        for x, v in zip(labels[0], s.to_list()):
            cells[str(x)].append(v)
    if any(c in columns for c in cells):
        raise ValueError("a result column has the name of a group key")
    data: dict[str, object] = {
        k: Series.from_list(v).cast(_column(frame, k).dtype) for k, v in columns.items()
    }
    data.update({c: Series.from_list(v) for c, v in cells.items()})
    return DataFrame.from_dict(data).set_index(keys)
