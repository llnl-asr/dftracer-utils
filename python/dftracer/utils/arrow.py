"""Arrow data interchange wrappers for DFTracer.

Provides ArrowBatch and ArrowTable classes that wrap Arrow C Data Interface
objects (PyCapsules) with convenience methods for conversion to pandas and
polars DataFrames.

These wrappers are pure Python. The actual Arrow data is produced by the
C extension (TraceReader.iter_arrow, utility to_arrow methods). Conversion
to pandas requires pyarrow; conversion to polars requires polars. Neither
is a required dependency.
"""

from __future__ import annotations

from typing import Any, Iterator, Optional, Tuple


class ArrowBatch:
    """Wrapper around an Arrow RecordBatch from the C extension.

    Supports the Arrow PyCapsule protocol (__arrow_c_array__) for
    zero-copy interchange with pyarrow, polars, and DuckDB.

    The underlying C data is exported via ownership transfer on first
    use. The pyarrow RecordBatch is cached so subsequent calls to
    ``to_pandas()``, ``to_polars()``, or ``__arrow_c_array__()`` are safe.
    """

    def __init__(self, capsule: Any) -> None:
        self._capsule = capsule
        self._pa_batch: Any = None  # cached pyarrow.RecordBatch

    def _to_pa_batch(self) -> Any:
        """Convert to pyarrow RecordBatch, caching the result.

        Returns:
            pyarrow.RecordBatch: The converted batch.

        Raises:
            ImportError: If pyarrow is not installed.
        """
        if self._pa_batch is not None:
            return self._pa_batch
        try:
            import pyarrow as pa
        except ImportError:
            raise ImportError("pyarrow is required. Install with: pip install pyarrow") from None
        self._pa_batch = pa.record_batch(self._capsule)
        return self._pa_batch

    def __arrow_c_array__(self, requested_schema: Any = None) -> Tuple[Any, Any]:
        """Export via Arrow C Data Interface."""
        return self._to_pa_batch().__arrow_c_array__(requested_schema)

    @property
    def num_rows(self) -> int:
        """Number of rows in this batch."""
        if self._pa_batch is not None:
            return self._pa_batch.num_rows
        return self._capsule.num_rows

    @property
    def num_columns(self) -> int:
        """Number of columns in this batch."""
        if self._pa_batch is not None:
            return self._pa_batch.num_columns
        return self._capsule.num_columns

    def to_pandas(self) -> Any:
        """Convert to pandas DataFrame.

        Returns:
            pandas.DataFrame: The converted DataFrame.

        Raises:
            ImportError: If pyarrow is not installed.
        """
        return self._to_pa_batch().to_pandas()

    def to_polars(self) -> Any:
        """Convert to polars DataFrame.

        Returns:
            polars.DataFrame: The converted DataFrame.

        Raises:
            ImportError: If polars is not installed.
        """
        try:
            import polars as pl  # type: ignore[import-not-found]
        except ImportError:
            raise ImportError(
                "polars is required for to_polars(). Install with: pip install polars"
            ) from None
        return pl.from_arrow(self._to_pa_batch())


class ArrowTable:
    """Wrapper around a collection of Arrow RecordBatches.

    Returned by read_arrow() and utility process() methods. Holds
    multiple batches with a shared schema. Supports the Arrow PyCapsule
    stream protocol (__arrow_c_stream__) for zero-copy interchange.

    The pyarrow Table is cached on first conversion so subsequent calls
    to ``to_pandas()``, ``to_polars()``, or ``__arrow_c_stream__()``
    are safe.

    Empty results (no events matched) return an ArrowTable with
    num_batches=0 and no columns.
    """

    def __init__(
        self,
        batches: list[Any],
        schema_capsule: Optional[Any] = None,
    ) -> None:
        self._batches = list(batches)
        self._schema_capsule = schema_capsule
        self._pa_table: Any = None  # cached pyarrow.Table

    def _to_pa_table(self) -> Any:
        """Convert to pyarrow Table, caching the result.

        Arrow C Data Interface export is single-use (ownership transfer),
        so we cache the pyarrow table on first conversion.

        Returns:
            pyarrow.Table: The converted table.

        Raises:
            ImportError: If pyarrow is not installed.
        """
        if self._pa_table is not None:
            return self._pa_table
        try:
            import pyarrow as pa
        except ImportError:
            raise ImportError("pyarrow is required. Install with: pip install pyarrow") from None
        pa_batches = [pa.record_batch(b) for b in self._batches]
        if not pa_batches:
            schema = pa.schema([])
            if self._schema_capsule is not None:
                schema = pa.Schema.from_arrow(self._schema_capsule)
            self._pa_table = pa.table({}, schema=schema)
        else:
            self._pa_table = pa.Table.from_batches(pa_batches)
        return self._pa_table

    def __arrow_c_stream__(self, requested_schema: Any = None) -> Any:
        """Arrow C Stream Interface -- yields batches to consumers."""
        return self._to_pa_table().__arrow_c_stream__(requested_schema)

    @property
    def num_batches(self) -> int:
        """Number of batches."""
        return len(self._batches)

    @property
    def num_rows(self) -> int:
        """Total number of rows across all batches."""
        return sum(b.num_rows for b in self._batches)

    @property
    def empty(self) -> bool:
        """True if there are no batches."""
        return len(self._batches) == 0

    def batch(self, i: int) -> Any:
        """Get the i-th batch."""
        return self._batches[i]

    def batches(self) -> Iterator[Any]:
        """Iterate over batches."""
        return iter(self._batches)

    def to_pandas(self) -> Any:
        """Convert all batches to a single pandas DataFrame.

        Returns:
            pandas.DataFrame: The converted DataFrame.

        Raises:
            ImportError: If pyarrow is not installed.
        """
        return self._to_pa_table().to_pandas()

    def to_polars(self) -> Any:
        """Convert all batches to a single polars DataFrame.

        Returns:
            polars.DataFrame: The converted DataFrame.

        Raises:
            ImportError: If polars is not installed.
        """
        try:
            import polars as pl  # type: ignore[import-not-found]
        except ImportError:
            raise ImportError(
                "polars is required for to_polars(). Install with: pip install polars"
            ) from None
        if not self._batches:
            return pl.DataFrame()
        return pl.concat([pl.from_arrow(self._to_pa_table())])
