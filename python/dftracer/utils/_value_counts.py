# SPDX-License-Identifier: MIT
"""``value_counts`` in the pandas shape: a Series of counts labelled by the
distinct values (one level per column for a frame). The counting is the
engine's group-by; this module orders the result and builds the labels."""

from __future__ import annotations

import math
from typing import TYPE_CHECKING, Sequence, cast

from . import dftracer_utils_ext as _ext
from ._series_index import drop_levels, internal_name, with_levels

if TYPE_CHECKING:
    from .dataframe import DataFrame
    from .series import Series

_ROW = "__dftu_row__"
_COUNT = "__dftu_count__"
_NAME = "count"


def native_counts(s: Series) -> DataFrame:
    """The engine's count of each distinct value: ``value`` and ``count``
    columns, most frequent first, ties by first appearance."""
    from .dataframe import DataFrame

    return DataFrame(_ext.Series.value_counts(s))


def _no_nan(s: Series) -> Series:
    """``s`` with a NaN as a null: pandas counts the two as one missing value."""
    if s.dtype.is_float and bool(s.is_nan().any()):
        return s.where(s.is_nan().logical_not())
    return s


def _counted(
    frame: DataFrame, keys: list[str], sort: bool, ascending: bool
) -> tuple[Series, list[Series]]:
    """The count of each distinct row of ``keys`` and the key columns of those
    rows. Ties keep the order in which a row first appeared, as pandas;
    ``sort=False`` orders the rows by first appearance. The group-by runs over
    positional names, so a key named ``count`` or like a helper column is safe."""
    from .expr import Agg, Col
    from .groupby import GroupBy

    inner = [f"__dftu_key{i}__" for i in range(len(keys))]
    work = frame.select(*keys).rename(dict(zip(keys, inner))).with_row_index(_ROW)
    out = GroupBy(work, inner).agg(Agg("count", None, _COUNT), Agg("min", Col(_ROW), _ROW))
    for k, name in zip(keys, inner):  # the group-by widens a narrow key column to 64 bits
        if out[name].dtype != frame[k].dtype:
            out = out.with_column(name, out[name].astype(frame[k].dtype))
    if sort:
        out = out._sort_values([_COUNT, _ROW], ascending=[ascending, True])
    else:
        out = out._sort_values(_ROW)
    return out[_COUNT], [out[name] for name in inner]


def _labelled(
    counts: Series,
    names: Sequence[str],
    levels: Sequence[Series],
    normalize: bool,
    total: int | None = None,
) -> Series:
    """``counts`` under the ``levels``; ``normalize`` divides by ``total``
    (the sum of the counts when not given)."""
    if normalize:
        counts = counts.astype("float64") / float(counts.sum() if total is None else total)
    named = counts.rename("proportion" if normalize else _NAME)
    return with_levels(named, list(names), [drop_levels(c) for c in levels])


def series_value_counts(
    s: Series,
    normalize: bool,
    sort: bool,
    ascending: bool,
    bins: int | Sequence[float] | None,
    dropna: bool,
) -> Series:
    from .dataframe import DataFrame

    if bins is not None:
        return _binned(s, normalize, sort, ascending, bins, dropna)
    values = _no_nan(drop_levels(s))
    if dropna and values.null_count:
        values = values.filter(values.is_not_null())
    level = internal_name(s.name, 0)
    counts, labels = _counted(DataFrame({"value": values}), ["value"], sort, ascending)
    return _labelled(counts, [level], labels, normalize)


def frame_value_counts(
    frame: DataFrame,
    keys: list[str],
    normalize: bool,
    sort: bool,
    ascending: bool,
    dropna: bool,
) -> Series:
    cleaned = frame
    for k in keys:
        cleaned = cleaned.with_column(k, _no_nan(drop_levels(cleaned[k])))
    if dropna:
        present = None
        for k in keys:
            m = cleaned[k].is_not_null()
            present = m if present is None else present & m
        if present is not None:
            cleaned = cleaned.filter(present)
    counts, labels = _counted(cleaned.select(*keys), keys, sort, ascending)
    return _labelled(counts, keys, labels, normalize)


def _around(x: float, digits: int) -> float:
    """``numpy.around``: the value scaled, rounded half to even and scaled
    back (Python's ``round`` rounds the exact decimal value instead)."""
    scale = 10.0**digits
    scaled = x * scale
    if not math.isfinite(scaled):
        return x
    return float(round(scaled)) / scale


def _round_frac(x: float, precision: int) -> float:
    if not math.isfinite(x) or x == 0:
        return x
    frac, whole = math.modf(x)
    digits = precision if whole != 0 else -int(math.floor(math.log10(abs(frac)))) - 1 + precision
    return _around(x, digits)


def _edge_labels(edges: list[float]) -> list[str]:
    """The text of pandas' ``cut`` intervals ``(a, b]``: edges rounded to the
    fewest digits (three or more) that keep them apart, the first lowered by
    one unit of that precision to hold the lowest value."""
    precision = 3
    for p in range(3, 20):
        if len({_round_frac(b, p) for b in edges}) == len(edges):
            precision = p
            break
    breaks = [_round_frac(b, precision) for b in edges]
    breaks[0] = breaks[0] - 10 ** (-precision)
    return [f"({a}, {b}]" for a, b in zip(breaks, breaks[1:])]


def _bin_edges(s: Series, bins: int | Sequence[float]) -> list[float]:
    if isinstance(bins, int):
        if bins < 1:
            raise ValueError("`bins` should be a positive integer.")
        if not len(s):
            raise ValueError("Cannot cut empty array")
        present = _no_nan(s).drop_nulls()
        if not len(present):
            raise ValueError("Bin edges must be unique: every value is missing")
        mn, mx = float(cast(float, present.min())), float(cast(float, present.max()))
        if math.isinf(mn) or math.isinf(mx):
            raise ValueError("cannot specify integer `bins` when input data contains infinity")
        if mn == mx:
            mn -= 0.001 * abs(mn) if mn != 0 else 0.001
            mx += 0.001 * abs(mx) if mx != 0 else 0.001
            adj = 0.0
        else:
            adj = (mx - mn) * 0.001
        step = (mx - mn) / bins
        edges = [mn + i * step for i in range(bins)] + [mx]
        edges[0] -= adj
        return edges
    edges = [float(b) for b in bins]
    if any(b >= a for a, b in zip(edges[1:], edges)):
        raise ValueError("bins must increase monotonically.")
    return edges


def _binned(
    s: Series,
    normalize: bool,
    sort: bool,
    ascending: bool,
    bins: int | Sequence[float],
    dropna: bool,
) -> Series:
    """The counts per interval of ``cut(bins)`` as pandas: every interval is
    listed, empty ones with a zero, and the labels are the interval text. With
    ``dropna`` (the default) and no value in any interval the result is empty."""
    from .series import Series

    if not s.dtype.is_numeric:
        raise TypeError("value_counts(bins=) needs a numeric Series")
    values = drop_levels(s).astype("float64")
    edges = _bin_edges(values, bins)
    cut = values.cut(Series.from_list(edges), right=True)
    ids = cut.mask(values.eq(edges[0]), 1)
    inside = ids.filter(ids.gt(0) & ids.lt(len(edges)))
    found: dict[int, int] = {}
    if len(inside):
        table = native_counts(inside)
        found = {
            int(cast("int", v)): int(cast("int", c))
            for v, c in zip(table["value"].to_list(), table["count"].to_list())
        }
    labels = _edge_labels(edges)
    counts = [found.get(i + 1, 0) for i in range(len(labels))]
    if not labels or (dropna and not any(counts)):
        return _labelled(
            Series.from_list([0]).head(0),
            [internal_name(None, 0)],
            [Series.from_list([""]).head(0)],
            normalize,
        )
    order = list(range(len(labels)))
    if sort:
        order.sort(key=lambda i: counts[i] if ascending else -counts[i])
    picked = [counts[i] for i in order]
    result = Series.from_list(picked)
    text = Series.from_list([labels[i] for i in order])
    return _labelled(result, [internal_name(None, 0)], [text], normalize, len(s))
