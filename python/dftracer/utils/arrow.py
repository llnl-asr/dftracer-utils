"""Arrow data interchange wrappers for DFTracer.

Provides ArrowBatch and ArrowTable classes that wrap Arrow C Data Interface
objects (PyCapsules) with convenience methods for conversion to pandas and
polars DataFrames.

These wrappers are pure Python. The actual Arrow data is produced by the
C extension (TraceReader.iter_arrow, utility to_arrow methods). Conversion
to pandas requires pyarrow; conversion to polars requires polars. Neither
is a hard dependency — a clear ImportError is raised if the library is
not installed.
"""


class ArrowBatch:
    """Wrapper around an Arrow RecordBatch from the C extension.

    Supports the Arrow PyCapsule protocol (__arrow_c_array__) for
    zero-copy interchange with pyarrow, polars, and DuckDB.
    """

    def __init__(self, capsule):
        self._capsule = capsule

    def __arrow_c_array__(self, requested_schema=None):
        """Export via Arrow C Data Interface."""
        return self._capsule.__arrow_c_array__(requested_schema)

    @property
    def num_rows(self):
        """Number of rows in this batch."""
        return self._capsule.num_rows

    @property
    def num_columns(self):
        """Number of columns in this batch."""
        return self._capsule.num_columns

    def to_pandas(self):
        """Convert to pandas DataFrame. Requires pyarrow."""
        try:
            import pyarrow as pa
        except ImportError:
            raise ImportError(
                "pyarrow is required for to_pandas(). Install with: pip install pyarrow"
            ) from None
        return pa.record_batch(self).to_pandas()

    def to_polars(self):
        """Convert to polars DataFrame. Requires polars."""
        try:
            import polars as pl  # type: ignore[import-not-found]
        except ImportError:
            raise ImportError(
                "polars is required for to_polars(). Install with: pip install polars"
            ) from None
        return pl.from_arrow(self)


class ArrowTable:
    """Wrapper around a collection of Arrow RecordBatches.

    Returned by read_arrow() and utility run() methods. Holds multiple
    batches with a shared schema. Supports the Arrow PyCapsule stream
    protocol (__arrow_c_stream__) for zero-copy interchange.

    Empty results (no events matched) return an ArrowTable with
    num_batches=0 and no columns.
    """

    def __init__(self, batches, schema_capsule=None):
        self._batches = list(batches)
        self._schema_capsule = schema_capsule

    def __arrow_c_stream__(self, requested_schema=None):
        """Arrow C Stream Interface -- yields batches to consumers."""
        try:
            import pyarrow as pa
        except ImportError:
            raise ImportError(
                "pyarrow is required for __arrow_c_stream__. Install with: pip install pyarrow"
            ) from None
        pa_batches = [pa.record_batch(b) for b in self._batches]
        if not pa_batches:
            schema = pa.schema([])
            if self._schema_capsule is not None:
                schema = pa.Schema.from_arrow(self._schema_capsule)
            table = pa.table({}, schema=schema)
        else:
            table = pa.Table.from_batches(pa_batches)
        return table.__arrow_c_stream__(requested_schema)

    @property
    def num_batches(self):
        """Number of batches."""
        return len(self._batches)

    @property
    def num_rows(self):
        """Total number of rows across all batches."""
        return sum(b.num_rows for b in self._batches)

    @property
    def empty(self):
        """True if there are no batches."""
        return len(self._batches) == 0

    def batch(self, i):
        """Get the i-th batch."""
        return self._batches[i]

    def batches(self):
        """Iterate over batches."""
        return iter(self._batches)

    def to_pandas(self):
        """Convert all batches to a single pandas DataFrame. Requires pyarrow."""
        try:
            import pyarrow as pa
        except ImportError:
            raise ImportError(
                "pyarrow is required for to_pandas(). Install with: pip install pyarrow"
            ) from None
        pa_batches = [pa.record_batch(b) for b in self._batches]
        if not pa_batches:
            if self._schema_capsule is not None:
                schema = pa.Schema.from_arrow(self._schema_capsule)
                return pa.table({}, schema=schema).to_pandas()
            return pa.table({}).to_pandas()
        return pa.Table.from_batches(pa_batches).to_pandas()

    def to_polars(self):
        """Convert all batches to a single polars DataFrame. Requires polars."""
        try:
            import polars as pl  # type: ignore[import-not-found]
        except ImportError:
            raise ImportError(
                "polars is required for to_polars(). Install with: pip install polars"
            ) from None
        if not self._batches:
            return pl.DataFrame()
        return pl.concat([b.to_polars() for b in self._batches])
