"""The polars-shaped face of the engine: ``import dftracer.utils.polars as pl``.

``DataFrame``, ``LazyFrame`` and ``Series`` are the engine's own classes with
the polars names on them: ``df.height`` / ``df.width`` / ``df.schema``,
``select`` / ``with_columns(**)`` / ``filter(expr)`` / ``sort(by, descending=)``
/ ``group_by`` / ``join(other, on=, how=)`` / ``unique`` / ``fill_null`` /
``drop_nulls``, ``df.lazy()`` with the same ops plus ``collect()``,
``s.str.to_lowercase()``, ``s.is_in([...])``, ``s.is_null()``. ``col`` and
``lit`` build the expressions ``filter`` / ``with_columns`` take. The module
functions below are the polars top-level ones that map onto engine ops.
"""

from __future__ import annotations

from typing import Any, Optional, Sequence, cast, overload

from .columnar import Expr, col, lit
from .dataframe import DataFrame
from .lazyframe import LazyFrame
from .series import Series

__all__ = [
    "DataFrame",
    "Expr",
    "LazyFrame",
    "Series",
    "col",
    "concat",
    "lit",
    "read_parquet",
    "scan_parquet",
]


def read_parquet(path: str, columns: Optional[Sequence[str]] = None) -> DataFrame:
    """Read a Parquet file into a :class:`DataFrame`."""
    return DataFrame.from_parquet(path, columns=columns)


def scan_parquet(path: str, columns: Optional[Sequence[str]] = None) -> LazyFrame:
    """A :class:`LazyFrame` over a Parquet file. The file is read when the plan
    is built; the ops after it are deferred."""
    return DataFrame.from_parquet(path, columns=columns).lazy()


@overload
def concat(items: Sequence[DataFrame], how: str = "vertical") -> DataFrame: ...
@overload
def concat(items: Sequence[LazyFrame], how: str = "vertical") -> LazyFrame: ...
def concat(
    items: Sequence[DataFrame] | Sequence[LazyFrame], how: str = "vertical"
) -> DataFrame | LazyFrame:
    """Vertically concatenate frames. ``how="diagonal"`` unions the columns,
    null-filling those absent from a part. A sequence of ``LazyFrame`` gives a
    ``LazyFrame`` (vertical only): each plan streams in turn."""
    frames = list(items)
    if not frames:
        raise ValueError("concat() needs at least one frame")
    if how not in ("vertical", "diagonal"):
        raise ValueError("how must be 'vertical' or 'diagonal'")
    first = frames[0]
    if isinstance(first, LazyFrame):
        if how != "vertical":
            raise ValueError("LazyFrame concat is vertical only")
        return first.concat(*cast(Sequence[LazyFrame], frames[1:]))
    return first.concat(*cast(Sequence[DataFrame], frames[1:]), how=cast(Any, how))
