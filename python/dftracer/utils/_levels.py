# SPDX-License-Identifier: MIT
"""The index levels of a :class:`Series` or a :class:`DataFrame`: ``droplevel``,
``swaplevel``, ``reorder_levels``, the level argument of ``sort_index`` and
``reset_index``, and ``add_prefix`` / ``add_suffix`` on the labels. A level is
a name, a position or a negative position, as in pandas; the two classes share
the code through four small hooks (:class:`LevelsMixin`)."""

from __future__ import annotations

from typing import TYPE_CHECKING, Callable, Literal, Sequence, Union, cast

from ._enums import DType
from ._series_index import default_name, public_name

if TYPE_CHECKING:
    from typing_extensions import Self

    from ._series import Series

Level = Union[int, str, None]
LevelArg = Union[Level, Sequence[Level]]


def _plural(n: int) -> str:
    return f"{n} level{'' if n == 1 else 's'}"


def pick_levels(names: Sequence[str], level: LevelArg) -> list[int]:
    """The positions of ``level`` (one level or a list) among the internal level
    ``names``. ``IndexError`` for a position out of range and ``KeyError`` for
    an unknown name, with the pandas messages."""
    wanted = list(level) if isinstance(level, (list, tuple)) else [level]
    out: list[int] = []
    for item in wanted:
        if isinstance(item, int) and not isinstance(item, bool):
            at = item if item >= 0 else item + len(names)
            if not 0 <= at < len(names):
                shown = f"not {item + 1}" if item >= 0 else f"{item} is not a valid level number"
                raise IndexError(f"Too many levels: Index has only {_plural(len(names))}, {shown}")
        else:
            matches = [i for i, n in enumerate(names) if public_name(n) == item]
            if not matches:
                if len(names) == 1:
                    raise KeyError(
                        f"Requested level ({item}) does not match index name "
                        f"({public_name(names[0])})"
                    )
                raise KeyError(f"Level {item} not found")
            at = matches[0]
        out.append(at)
    return out


def kept_levels(names: Sequence[str], level: LevelArg) -> list[int]:
    """The positions that stay after ``droplevel(level)``; ``ValueError`` when
    no level would be left."""
    picked = pick_levels(names, level)
    if len(picked) >= len(names):
        raise ValueError(
            f"Cannot remove {len(picked)} levels from an index with {len(names)} levels: "
            "at least one level must be left."
        )
    return [i for i in range(len(names)) if i not in picked]


def swapped_levels(names: Sequence[str], i: Level, j: Level) -> list[int]:
    a = pick_levels(names, i)[0]
    b = pick_levels(names, j)[0]
    order = list(range(len(names)))
    order[a], order[b] = order[b], order[a]
    return order


def reordered_levels(names: Sequence[str], order: Sequence[Level]) -> list[int]:
    picked = pick_levels(names, list(order))
    if len(picked) != len(names):
        raise ValueError(
            f"Length of order must be same as number of levels ({len(names)}), got {len(picked)}"
        )
    if len(set(picked)) != len(picked):
        raise ValueError("reorder_levels: a level is named twice")
    return picked


def sort_plan(
    names: Sequence[str],
    level: LevelArg,
    ascending: bool | Sequence[bool],
    sort_remaining: bool,
) -> tuple[list[int], list[bool], int]:
    """The level positions ``sort_index`` orders by, one ascending flag for
    each, and how many of them ``key`` applies to (the levels named in ``level``,
    not the remaining ones): ``level`` first (a list takes one flag each), then, with
    ``sort_remaining``, the other levels in order and in the same direction (a
    list of flags sorts the listed levels only, as pandas). A single level
    ignores ``level``, as pandas."""
    every = list(range(len(names)))
    given = list(ascending) if isinstance(ascending, (list, tuple)) else None
    if len(names) == 1:
        if given is not None and len(given) != 1:
            raise TypeError("ascending must be a list of bool values of length 1")
        return every, [bool(given[0]) if given is not None else bool(ascending)], 1
    if level is None:
        picked = every
        named = len(every)
    else:
        picked = pick_levels(names, level)
        named = len(picked)
        if given is not None and len(given) != len(picked):
            raise ValueError("level must have same length as ascending")
        if sort_remaining and given is None:
            picked += [i for i in every if i not in picked]
    if given is None:
        return picked, [bool(ascending)] * len(picked), named
    if len(given) != len(picked) and level is None:
        raise ValueError(
            f"Length of ascending ({len(given)}) != length of the index levels ({len(picked)})"
        )
    flags = [bool(a) for a in given]
    return picked, flags + [flags[-1]] * (len(picked) - len(flags)), named


def sorted_positions(
    names: Sequence[str],
    levels: Sequence[Series],
    flags: Sequence[bool],
    na_position: Literal["first", "last"],
    key: Callable[[Series], Series] | None,
    keyed: int,
) -> Series:
    """The row positions that order the rows by the label ``levels``, each in
    its direction; ``key`` is applied to the first ``keyed`` levels, a Series named
    like the level."""
    from ._dataframe import DataFrame
    from ._series_index import positions

    if na_position not in ("first", "last"):
        raise ValueError("na_position must be 'first' or 'last'")
    columns = {
        f"l{i}": (key(level.rename(public_name(name))) if key is not None and i < keyed else level)
        for i, (name, level) in enumerate(zip(names, levels))
    }
    keys = list(columns)
    columns["pos"] = positions(len(levels[0]))
    frame = DataFrame.from_dict(columns)
    return frame._sort_values(keys, ascending=list(flags), na_position=na_position)["pos"]


_NATIVE_TEXT = frozenset(
    (DType.BOOL, DType.INT8, DType.INT16, DType.INT32, DType.INT64)
    + (DType.UINT8, DType.UINT16, DType.UINT32, DType.UINT64)
    + (DType.FLOAT32, DType.FLOAT64, DType.STRING)
)


def label_text(level: Series, prefix: str, suffix: str) -> Series:
    """``level`` as text, ``prefix`` and ``suffix`` around each label (a null
    label prints as pandas 3 prints it: ``NaT`` for a time, else ``nan``).
    Numbers, Bool and text go through the string kernels (cast, then a
    concatenation); the other types print each label in Python."""
    from ._series import Series

    if level.dtype not in _NATIVE_TEXT:
        null = "NaT" if level.dtype in (DType.TIMESTAMP, DType.DURATION) else "nan"
        texts = [prefix + (null if x is None else str(x)) + suffix for x in level.to_list()]
        return Series.from_list(texts) if texts else level.cast(DType.STRING)
    text = cast(
        "Series[str]",
        (level if level.dtype == DType.STRING else level.cast(DType.STRING)).fill_null("nan"),
    )
    if prefix:
        text = _cat(_cat(text.str.slice(0, 0), prefix), text)
    return _cat(text, suffix) if suffix else text


def _cat(s: Series[str], other: Series[str] | str) -> Series[str]:
    return cast("Series[str]", s.str.cat(other))


class LevelsMixin:
    """``droplevel``, ``swaplevel`` and ``reorder_levels``. The class gives
    ``_level_names`` (the internal names of the row labels, empty when the
    object has none), ``_keep_levels`` (the object with only the levels at the
    given positions, in that order) and ``_axis_is_rows``."""

    __slots__ = ()

    def _level_names(self) -> list[str]:
        raise NotImplementedError

    def _keep_levels(self, order: list[int]) -> Self:
        raise NotImplementedError

    def _axis_is_rows(self, axis: object) -> bool:
        raise NotImplementedError

    def _axis_levels(self, axis: object) -> list[str]:
        names = self._level_names() if self._axis_is_rows(axis) else []
        return names or [default_name(0)]

    def droplevel(self, level: LevelArg, axis: int | str = 0) -> Self:
        """The pandas ``droplevel``: the row labels without ``level`` (a name, a
        position, a negative position or a list of them), the other levels in
        order. A frame's rows lose the label columns, they are gone and not
        data. ``ValueError`` when no level would be left (a plain index has
        one); ``KeyError`` or ``IndexError`` for a level that does not exist.
        ``axis=1`` is the column labels, which have one level."""
        names = self._axis_levels(axis)
        keep = kept_levels(names, level)
        return self if keep == list(range(len(names))) else self._keep_levels(keep)

    def swaplevel(self, i: Level = -2, j: Level = -1, axis: int | str = 0) -> Self:
        """The pandas ``swaplevel``: levels ``i`` and ``j`` (names or positions,
        the last two by default) exchange places. ``TypeError`` for a plain
        index (pandas raises ``AssertionError`` or ``TypeError``)."""
        names = self._axis_levels(axis)
        if len(names) < 2:
            raise TypeError("Can only swap levels on a hierarchical axis.")
        return self._keep_levels(swapped_levels(names, i, j))

    def reorder_levels(self, order: Sequence[Level], axis: int | str = 0) -> Self:
        """The pandas ``reorder_levels``: the levels in the order given (names or
        positions, each one once). ``TypeError`` for a plain index and
        ``ValueError`` for an order of the wrong length (pandas raises
        ``AssertionError`` or a bare ``Exception``)."""
        names = self._axis_levels(axis)
        if len(names) < 2:
            raise TypeError("Can only reorder levels on a hierarchical axis.")
        return self._keep_levels(reordered_levels(names, order))


__all__ = [
    "LevelsMixin",
    "kept_levels",
    "label_text",
    "pick_levels",
    "reordered_levels",
    "sort_plan",
    "sorted_positions",
    "swapped_levels",
]
