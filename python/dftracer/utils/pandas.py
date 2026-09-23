"""The pandas-shaped face of the engine: ``import dftracer.utils.pandas as pd``.

``DataFrame`` and ``Series`` are the engine's own classes (every op runs in
the native SIMD engine), with the pandas names and argument spellings on them:
``df.columns`` / ``df.shape`` / ``df.dtypes``, ``df[["a", "b"]]``, ``df[mask]``,
``sort_values(by, ascending=)``, ``merge(right, how=, on=)``, ``assign(**)``,
``drop(columns=)``, ``dropna()`` / ``fillna()``, ``astype({...})``,
``nlargest`` / ``nsmallest``, ``pivot_table``, ``s.str.lower()``,
``s.rolling(n).mean()``, ``s.var(ddof=)``, ``s.isin([...])``, ``s.isna()``.
The module functions below are the pandas top-level ones that map onto engine
ops. Anything not listed here is not part of the drop-in surface.
"""

from __future__ import annotations

from typing import Optional, Sequence, Tuple, Union

from .dataframe import DataFrame, JoinHow
from .series import Series

__all__ = [
    "DataFrame",
    "Series",
    "concat",
    "get_dummies",
    "isna",
    "isnull",
    "merge",
    "notna",
    "notnull",
    "read_parquet",
]


def read_parquet(path: str, columns: Optional[Sequence[str]] = None) -> DataFrame:
    """Read a Parquet file into a :class:`DataFrame`."""
    return DataFrame.from_parquet(path, columns=columns)


def concat(objs: Sequence[DataFrame], how: str = "vertical") -> DataFrame:
    """Vertically concatenate frames (UNION ALL). ``how="diagonal"`` unions the
    columns, null-filling those absent from a part."""
    frames = list(objs)
    if not frames:
        raise ValueError("concat() needs at least one frame")
    if how not in ("vertical", "diagonal"):
        raise ValueError("how must be 'vertical' or 'diagonal'")
    return frames[0].concat(*frames[1:], how=how)  # type: ignore[arg-type]


def merge(
    left: DataFrame,
    right: DataFrame,
    how: JoinHow = "inner",
    on: Union[str, Sequence[str], None] = None,
    left_on: Union[str, Sequence[str], None] = None,
    right_on: Union[str, Sequence[str], None] = None,
    suffixes: Tuple[str, str] = ("_x", "_y"),
) -> DataFrame:
    """Join two frames (:meth:`DataFrame.merge`): with no key, on the columns
    both share; colliding non-key columns are suffixed on both sides."""
    return left.merge(right, how=how, on=on, left_on=left_on, right_on=right_on, suffixes=suffixes)


def get_dummies(df: DataFrame, columns: Union[str, Sequence[str]]) -> DataFrame:
    """One-hot encode ``columns`` into one Int8 column per distinct value."""
    out = df
    for name in [columns] if isinstance(columns, str) else list(columns):
        out = out.to_dummies(name)
    return out


def isna(s: Series) -> Series:
    return s.isna()


def isnull(s: Series) -> Series:
    return s.isna()


def notna(s: Series) -> Series:
    return s.notna()


def notnull(s: Series) -> Series:
    return s.notna()
