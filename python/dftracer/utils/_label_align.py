# SPDX-License-Identifier: MIT
"""``reindex_like`` and ``align``, the pandas methods that Series and DataFrame
share, written once and mixed into both (:mod:`dftracer.utils._reindex` does
the work)."""

from __future__ import annotations

from typing import TYPE_CHECKING, Literal, cast

if TYPE_CHECKING:
    from typing_extensions import Self

    from ._dataframe import DataFrame
    from ._series import Series


class LabelAlignMixin:
    """``reindex_like`` and ``align`` of a :class:`Series` or a
    :class:`DataFrame`."""

    __slots__ = ()

    def reindex_like(
        self,
        other: Series | DataFrame,
        method: str | None = None,
        copy: bool | None = None,
        limit: int | None = None,
        tolerance: object = None,
    ) -> Self:
        """:meth:`reindex` on the row labels of ``other`` (and, for a frame,
        its columns), names included."""
        from ._reindex import reindex_like

        mine = cast("Series | DataFrame", self)
        return cast("Self", reindex_like(mine, other, method, limit, tolerance))

    def align(
        self,
        other: Series | DataFrame,
        join: Literal["outer", "inner", "left", "right"] = "outer",
        axis: int | str | None = None,
        level: object = None,
        copy: bool | None = None,
        fill_value: object = None,
    ) -> tuple[Self, Series | DataFrame]:
        """The pandas ``align``: this object and ``other`` on the join of their
        row labels, so they have the same labels. ``outer`` is the sorted union
        (nulls last), ``inner`` the common labels in this object's order,
        ``left`` this object's labels and ``right`` ``other``'s; labels that are
        equal stay as they are, and a repeated label meets every row that has
        it. A side that misses a row has a null there (an integer column becomes
        Float64). A Series fills every null of the result with ``fill_value``, a
        frame only the new rows and columns, as pandas.

        A DataFrame aligns its columns too (``axis=None``), only the rows
        (``axis=0``) or only the column names (``axis=1``); against a Series it
        needs ``axis``, and its labels meet the rows or the column names. A
        Series aligns with a Series or a frame on the rows. ``level`` is not
        supported."""
        from ._reindex import align

        mine = cast("Series | DataFrame", self)
        aligned = align(mine, other, join, axis, level, fill_value)
        return cast("tuple[Self, Series | DataFrame]", aligned)
