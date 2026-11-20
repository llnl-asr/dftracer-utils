# SPDX-License-Identifier: MIT
"""The row labels of a :class:`Series`: the helpers behind ``index``,
``set_axis``, ``rename_axis``, ``reset_index`` and the pandas round trip.

A labelled Series holds one label column per level (the native ``_with_index``).
A Series with none is positional, the pandas ``RangeIndex``, and nothing is
materialized for it. An unnamed level carries the name ``__index_level_<i>__``,
as :meth:`DataFrame.from_pandas` names one; the pandas faces show it as
``None``."""

from __future__ import annotations

import re
import sys
from typing import TYPE_CHECKING, Sequence, TypeVar, cast

from . import dftracer_utils_ext as _ext
from ._enums import DType

if TYPE_CHECKING:
    import pandas as pd

    from ._dataframe import DataFrame
    from ._levels import LevelArg
    from ._series import Series

_S = TypeVar("_S", bound="Series")
_UNNAMED = re.compile(r"__index_level_\d+__")
_WINDOW = 4096


def default_name(level: int) -> str:
    return f"__index_level_{level}__"


def public_name(name: str) -> str | None:
    """The level name pandas shows: ``None`` for an unnamed level."""
    return None if _UNNAMED.fullmatch(name) else name


def internal_name(name: object, level: int) -> str:
    return default_name(level) if name is None else str(name)


def positions(n: int, start: int = 0) -> Series:
    """The Int64 column ``start .. start + n - 1`` (no name, no labels)."""
    from ._series import Series

    if n <= _WINDOW:
        return Series.from_list(list(range(start, start + n)))
    from ._dataframe import DataFrame

    nulls = _ext._series_nulls(int(DType.INT64), n)
    column = DataFrame({"v": nulls}).with_row_index("i")["i"]
    return (column + start if start else column).rename(None)


def names_of(s: Series) -> list[str]:
    return list(_ext.Series.index_names(s))


def levels_of(s: Series) -> list[Series]:
    return [_ext.Series._index_level(s, i) for i in range(len(names_of(s)))]


def with_levels(s: _S, names: Sequence[str], levels: Sequence[Series]) -> _S:
    return _ext.Series._with_index(s, list(names), list(levels))


def positional(s: _S) -> _S:
    """``s`` under the labels ``0 .. len - 1``: what a pandas method that
    selects rows keeps, so the rows it keeps say where they came from."""
    if names_of(s):
        return s
    return with_levels(s, [default_name(0)], [positions(len(s))])


def drop_levels(s: _S) -> _S:
    return with_levels(s, [], []) if names_of(s) else s


def frame_columns(s: Series, value_name: str) -> tuple[list[str], list[Series]]:
    """The columns of ``s.to_frame``: the levels first, then the values. A
    pandas frame can hold a value column that shares its name with a level; a
    frame here cannot, so ``ValueError``."""
    names = names_of(s)
    if value_name in names:
        raise ValueError(f"the column name {value_name!r} is already an index level name")
    return [*names, value_name], [*levels_of(s), drop_levels(s)]


def to_frame(s: Series, name: str | None) -> DataFrame:
    from ._dataframe import DataFrame

    value = name if name is not None else (s.name if s.name is not None else "0")
    columns, series = frame_columns(s, value)
    frame = DataFrame.from_dict(dict(zip(columns, series)))
    index = columns[:-1]
    return frame.set_index(index) if index else frame


def reset(
    s: Series,
    level: LevelArg,
    drop: bool,
    name: object,
) -> Series | DataFrame:
    """``Series.reset_index``: the chosen levels become columns of a frame that
    leads with them (the pandas names: ``index`` for an unnamed single level,
    ``level_<i>`` for an unnamed one of several), the rest stay the index; with
    ``drop`` the Series keeps its values under the remaining levels."""
    from ._dataframe import DataFrame
    from ._levels import pick_levels

    names = names_of(s)
    effective = names or [default_name(0)]
    every = list(range(len(effective)))
    picked = every if level is None else sorted(set(pick_levels(effective, level)))
    keep = [i for i in every if i not in picked]
    shown = [shown_name(effective, i) for i in picked]
    columns = [_ext.Series._index_level(s, i) for i in picked] if names else [positions(len(s))]
    if drop:
        if not names:
            return s
        kept_names = [names[i] for i in keep]
        kept_levels = [_ext.Series._index_level(s, i) for i in keep]
        return with_levels(s, kept_names, kept_levels)
    value = name if isinstance(name, str) else (s.name if s.name is not None else "0")
    # A frame holds no duplicate column names, so allow_duplicates cannot be honoured.
    if value in shown or len(set(shown)) != len(shown):
        raise ValueError(f"cannot insert {value}, already exists")
    frame = DataFrame.from_dict(dict(zip([*shown, value], [*columns, drop_levels(s)])))
    if keep:
        return _reindex(
            frame, [names[i] for i in keep], [_ext.Series._index_level(s, i) for i in keep]
        )
    return frame


def _reindex(frame: DataFrame, names: list[str], levels: list[Series]) -> DataFrame:
    """``frame`` indexed by the label columns ``levels`` under ``names``."""
    for name, column in zip(names, levels):
        frame = frame.with_column(name, column)
    return frame.set_index(names)


def shown_name(names: list[str], i: int) -> str:
    shown = public_name(names[i])
    if shown is not None:
        return shown
    return "index" if len(names) == 1 else f"level_{i}"


# -- set_axis, rename_axis ---------------------------------------------------------


def labels_from(labels: object, n: int | None) -> tuple[list[str], list[Series]]:
    """The level names and columns of the new labels a caller gives
    ``set_axis``: a Series (one level, named like it), a frame (one level per
    column), a pandas ``Index`` or ``MultiIndex``, or a sequence (of tuples for
    several levels). ``ValueError`` when the length is not ``n`` (any length
    for ``None``)."""
    from ._dataframe import DataFrame
    from ._series import Series

    pd = sys.modules.get("pandas")
    np = sys.modules.get("numpy")
    names: list[str]
    levels: list[Series]
    if isinstance(labels, DataFrame):
        names = labels._names()
        levels = [labels[c] for c in names]
    elif isinstance(labels, Series):
        names = [labels.name if labels.name is not None else default_name(0)]
        levels = [drop_levels(labels)]
    elif pd is not None and isinstance(labels, pd.MultiIndex):
        names = [internal_name(labels.names[i], i) for i in range(labels.nlevels)]
        levels = [
            Series._from_pandas_values(pd.Series(labels.get_level_values(i)))
            for i in range(labels.nlevels)
        ]
    elif pd is not None and isinstance(labels, pd.Index):
        names = [internal_name(labels.name, 0)]
        levels = [Series._from_pandas_values(pd.Series(labels))]
    elif pd is not None and isinstance(labels, pd.Series):
        names = [internal_name(labels.name, 0)]
        levels = [Series._from_pandas_values(labels.reset_index(drop=True))]
    elif (
        np is not None
        and isinstance(labels, np.ndarray)
        and labels.ndim == 1
        and labels.dtype.kind in "biuf"
    ):
        names = [default_name(0)]
        levels = [Series.from_numpy(labels)]
    else:
        items = list(cast("Sequence[object]", labels))
        if items and all(isinstance(x, tuple) for x in items):
            width = len(cast("tuple[object, ...]", items[0]))
            if any(len(cast("tuple[object, ...]", x)) != width for x in items):
                raise ValueError("set_axis: tuples of different lengths")
            names = [default_name(i) for i in range(width)]
            levels = [
                Series.from_list([cast("tuple[object, ...]", x)[i] for x in items])
                for i in range(width)
            ]
        else:
            names = [default_name(0)]
            levels = [Series.from_list(items)]
    if n is not None and len(levels[0]) != n:
        raise ValueError(
            f"Length mismatch: Expected axis has {n} elements, new values have {len(levels[0])} elements"
        )
    return names, levels


def renamed_names(current: list[str], given: object) -> list[str]:
    """The level names after ``rename_axis(mapper)`` / ``rename_axis(index=...)``
    over the ``current`` ones: a scalar or ``None`` names a single level, a list
    names every level, a dict or a callable maps the current names."""
    if callable(given) or isinstance(given, dict):
        out = []
        for i, name in enumerate(current):
            shown = public_name(name)
            if callable(given):
                new = given(shown)
            else:
                new = given.get(shown, shown)
            out.append(internal_name(new, i))
        return out
    if isinstance(given, (list, tuple)):
        if len(given) != len(current):
            raise ValueError(f"Length of new names must be {len(current)}, got {len(given)}")
        return [internal_name(x, i) for i, x in enumerate(given)]
    if len(current) != 1:
        raise TypeError("Must pass list-like as `names`.")
    return [internal_name(given, 0)]


def rename_levels(s: Series, new_names: list[str]) -> Series:
    names = names_of(s)
    if not names:
        if new_names == [default_name(0)]:
            return s
        return with_levels(s, new_names, [positions(len(s))])
    return with_levels(s, new_names, levels_of(s))


# -- pandas ---------------------------------------------------------------------------


def pandas_labels(s: pd.Series) -> tuple[list[str], list[Series]] | None:
    """The levels of a pandas Series' index; ``None`` for the default
    ``RangeIndex``."""
    import pandas as pd

    from ._series import Series

    index = s.index
    if isinstance(index, pd.RangeIndex) and index.start == 0 and index.step == 1:
        return None
    names = [internal_name(index.names[i], i) for i in range(index.nlevels)]
    levels = [
        Series._from_pandas_values(pd.Series(index.get_level_values(i)))
        for i in range(index.nlevels)
    ]
    return names, levels


def pandas_index(s: Series) -> pd.Index:
    """The pandas ``Index`` (a ``MultiIndex`` for several levels) of ``s``."""
    import pandas as pd

    names = [public_name(n) for n in names_of(s)]
    arrays = [level.to_pandas() for level in levels_of(s)]
    if len(arrays) == 1:
        return pd.Index(arrays[0], name=names[0])
    return pd.MultiIndex.from_arrays(arrays, names=names)
