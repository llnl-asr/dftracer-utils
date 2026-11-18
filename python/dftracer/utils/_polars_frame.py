# SPDX-License-Identifier: MIT
"""The polars frame spellings that the pandas surface does not already
cover, each a thin name over an engine op or an Arrow export: the
horizontal reductions, the stacks, `partition_by`, `gather_every`,
`join_asof`, the row iterators and the writers."""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    Dict,
    Iterator,
    List,
    Literal,
    Optional,
    Sequence,
    Tuple,
    Union,
)

from ._convert import _require_pyarrow
from ._pandas_frame import column_series, is_numeric_series
from .series import Series

if TYPE_CHECKING:
    from .dataframe import DataFrame
    from .enums import DType
    from .expr import Expr


class FramePolarsMixin:
    """Mixed into :class:`DataFrame`; every method reads the frame through
    the wrapper's own API."""

    __slots__ = ()

    _index: Optional[List[str]]

    def _frame(self) -> "DataFrame":
        from .dataframe import DataFrame

        assert isinstance(self, DataFrame)
        return self

    def _numeric_columns(self) -> List[str]:
        f = self._frame()
        return [c for c in f.columns if is_numeric_series(column_series(f, c))]

    # -- horizontal reductions --------------------------------------------------
    def sum_horizontal(self) -> Series:
        """The row-wise sum of the numeric columns (a null counts as 0 when
        another value is present; polars ``sum_horizontal``)."""
        cols = self._numeric_columns()
        if not cols:
            raise ValueError("sum_horizontal: no numeric column")
        f = self._frame()
        out = column_series(f, cols[0]).astype("float64").fillna(0.0)
        present = column_series(f, cols[0]).notna()
        for c in cols[1:]:
            out = out + column_series(f, c).astype("float64").fillna(0.0)
            present = present.logical(1, column_series(f, c).notna())
        return out.where(present, out.shift(len(out)))

    def mean_horizontal(self) -> Series:
        cols = self._numeric_columns()
        if not cols:
            raise ValueError("mean_horizontal: no numeric column")
        f = self._frame()
        count = column_series(f, cols[0]).notna().astype("int64")
        for c in cols[1:]:
            count = count + column_series(f, c).notna().astype("int64")
        total = self.sum_horizontal()
        return total / count.where(count.ge(1), count.shift(len(count)))

    def min_horizontal(self) -> Series:
        return self._extreme_horizontal(min=True)

    def max_horizontal(self) -> Series:
        return self._extreme_horizontal(min=False)

    def _extreme_horizontal(self, min: bool) -> Series:
        cols = self._numeric_columns()
        if not cols:
            raise ValueError("min_horizontal: no numeric column")
        f = self._frame()
        out = column_series(f, cols[0]).astype("float64")
        for c in cols[1:]:
            other = column_series(f, c).astype("float64")
            # Fill each side from the other so a lone value wins, then pick.
            other = other.where(other.notna(), out)
            out = out.where(out.notna(), other)
            diff = other - out
            pick_other = diff.lt(0) if min else diff.gt(0)
            out = other.where(pick_other, out)
        return out

    # -- stacks and picks -------------------------------------------------------
    def hstack(
        self, columns: "Union[DataFrame, Sequence[Series], Dict[str, Series]]"
    ) -> "DataFrame":
        """This frame with ``columns`` (a frame, a ``{name: Series}`` or
        Series with names) appended on the right (polars ``hstack``)."""
        from .dataframe import DataFrame

        out = self._frame()
        if isinstance(columns, DataFrame):
            items: List[Tuple[str, Series]] = [
                (c, column_series(columns, c)) for c in columns.columns
            ]
        elif isinstance(columns, dict):
            items = list(columns.items())
        else:
            items = [(f"column_{i}", s) for i, s in enumerate(columns)]
        for name, s in items:
            out = out.with_column(name, s)
        return out

    def vstack(self, other: "DataFrame") -> "DataFrame":
        """``other``'s rows below this frame's (polars ``vstack``)."""
        return self._frame().concat(other)

    def is_sorted(
        self,
        by: "Union[str, Sequence[str]]",
        *more_by: str,
        descending: bool = False,
        nulls_last: bool = False,
    ) -> bool:
        """Whether the rows already are in the order ``sort(by, ...)`` gives
        (polars ``is_sorted``)."""
        f = self._frame()
        keys = [by] if isinstance(by, str) else list(by)
        keys += more_by
        ordered = f.select(*keys)
        return ordered.equals(ordered.sort(keys, descending=descending, nulls_last=nulls_last))

    def iter_slices(self, n_rows: int = 10_000) -> Iterator["DataFrame"]:
        """The frame in consecutive slices of ``n_rows`` rows, the last one
        shorter (polars ``iter_slices``)."""
        if n_rows < 1:
            raise ValueError("iter_slices: n_rows must be at least 1")
        f = self._frame()
        for offset in range(0, len(f), n_rows):
            yield f.slice(offset, n_rows)

    def remove(self, predicate: "Union[Series, Expr]") -> "DataFrame":
        """The rows where ``predicate`` is not true: false and null rows stay
        (polars ``remove``), where :meth:`filter` of the negation drops the
        null ones."""
        f = self._frame()
        mask = predicate if isinstance(predicate, Series) else predicate.apply(f)
        return f.filter(~mask.fill_null(False))

    def gather_every(self, n: int, offset: int = 0) -> "DataFrame":
        """Every ``n``-th row from ``offset`` (polars ``gather_every``)."""
        if n < 1:
            raise ValueError("gather_every: n must be at least 1")
        f = self._frame()
        return f.take(list(range(offset, len(f), n)))

    def partition_by(
        self, by: "Union[str, Sequence[str]]", *, as_dict: bool = False
    ) -> "Union[List[DataFrame], Dict[object, DataFrame]]":
        """One frame per distinct key (polars ``partition_by``), in first-seen
        key order; ``as_dict`` keys them by the key value (a tuple for several
        columns)."""
        from .groupby import GroupBy

        f = self._frame()
        keys = [by] if isinstance(by, str) else list(by)
        groups = GroupBy(f, keys).groups
        parts = {label: f.take(rows) for label, rows in groups.items()}
        return parts if as_dict else list(parts.values())

    def join_asof(
        self,
        other: "DataFrame",
        on: str,
        by: Optional[Union[str, Sequence[str]]] = None,
        strategy: Literal["backward", "forward", "nearest"] = "backward",
        tolerance: Optional[float] = None,
    ) -> "DataFrame":
        """The polars spelling of :meth:`asof`."""
        return self._frame().asof(other, on, by, strategy, tolerance)

    # -- rows and cells ---------------------------------------------------------
    def iter_rows(
        self, *, named: bool = False
    ) -> Iterator[Union[Tuple[object, ...], Dict[str, object]]]:
        """The rows as tuples, or dicts with ``named`` (polars ``iter_rows``; a Python object
        per cell, so not a fast path)."""
        frame = self._frame()
        names = list(frame.columns)
        columns = [frame[c].to_list() for c in names]
        for i in range(len(frame)):
            row = tuple(col[i] for col in columns)
            yield dict(zip(names, row)) if named else row

    def rows(self, *, named: bool = False) -> List[Union[Tuple[object, ...], Dict[str, object]]]:
        return list(self.iter_rows(named=named))

    def to_dicts(self) -> List[Dict[str, object]]:
        return [r for r in self.iter_rows(named=True) if isinstance(r, dict)]

    def row(
        self, index: int, *, named: bool = False
    ) -> Union[Tuple[object, ...], Dict[str, object]]:
        f = self._frame()
        if index < 0:
            index += len(f)
        if not 0 <= index < len(f):
            raise IndexError(f"row: {index} out of range")
        values = tuple(column_series(f, c)[index] for c in f.columns)  # type: ignore[index]
        return dict(zip(f.columns, values)) if named else values

    def item(self, row: Optional[int] = None, column: Optional[Union[int, str]] = None) -> object:
        """The one cell of a 1x1 frame, or the cell at ``row`` / ``column``."""
        f = self._frame()
        if row is None and column is None:
            if f.shape != (1, 1):
                raise ValueError(f"item: the frame is {f.shape}, not 1x1; pass row and column")
            row, column = 0, 0
        if row is None or column is None:
            raise ValueError("item: pass both row and column")
        name = f.columns[column] if isinstance(column, int) else column
        return column_series(f, name)[row]

    def get_column(self, name: str) -> Series:
        return column_series(self._frame(), name)

    def get_columns(self) -> List[Series]:
        f = self._frame()
        return [column_series(f, c) for c in f.columns]  # type: ignore[misc]

    def get_column_index(self, name: str) -> int:
        return self._frame().column_index(name)

    def to_series(self, index: int = 0) -> Series:
        f = self._frame()
        return column_series(f, f.columns[index])

    def is_empty(self) -> bool:
        return len(self._frame()) == 0

    def collect_schema(self) -> "Dict[str, DType]":
        """:attr:`schema` (polars ``collect_schema``)."""
        return self._frame().schema

    def n_unique(self, subset: Optional[Union[str, Sequence[str]]] = None) -> int:
        """The number of distinct rows (or distinct ``subset`` rows)."""
        f = self._frame()
        return len(f.unique(subset))

    def fill_nan(self, value: Union[int, float, None]) -> "DataFrame":
        """Each float column's NaN replaced by ``value`` (null for ``None``)."""
        f = self._frame()
        out = f
        for c in f.columns:
            s = column_series(f, c)
            if not is_numeric_series(s):
                continue
            nan = s.is_nan()
            if value is None:
                out = out.with_column(c, s.where(nan.logical_not(), s.shift(len(s))))
            else:
                out = out.with_column(c, s.where(nan.logical_not(), float(value)))
        return out

    def drop_nans(self, subset: Optional[Union[str, Sequence[str]]] = None) -> "DataFrame":
        """The rows with no NaN in the float columns (or in ``subset``)."""
        f = self._frame()
        names = [subset] if isinstance(subset, str) else list(subset) if subset else list(f.columns)
        keep = None
        for c in names:
            s = column_series(f, c)
            if not is_numeric_series(s):
                continue
            ok = s.is_nan().logical_not()
            keep = ok if keep is None else keep.logical(0, ok)
        return f if keep is None else f.filter(keep)

    # -- writers, through pyarrow -------------------------------------------------
    def write_parquet(self, path: str, **kwargs: object) -> None:
        pa = _require_pyarrow()
        import pyarrow.parquet as pq  # ty: ignore[import-not-found]

        pq.write_table(self._frame()._arrow_for_parquet(), path, **kwargs)  # type: ignore[arg-type]
        del pa

    def write_csv(self, path: str, **kwargs: object) -> None:
        _require_pyarrow()
        import pyarrow.csv as pcsv  # ty: ignore[import-not-found]

        pcsv.write_csv(self._frame().to_arrow(), path, **kwargs)  # type: ignore[arg-type]

    def write_ipc(self, path: str) -> None:
        with open(path, "wb") as f:
            f.write(self._frame().to_ipc())
