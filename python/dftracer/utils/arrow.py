"""Arrow data interchange and I/O for DFTracer.

Provides:
- ArrowBatch and ArrowTable classes that wrap Arrow C Data Interface
  objects (PyCapsules) with convenience methods for conversion to pandas
  and polars DataFrames.
- write_arrow() and read_arrow() for Arrow IPC file I/O with Runtime
  parallelization.

These wrappers are pure Python. The actual Arrow data is produced by the
C extension (TraceReader.iter_arrow, utility to_arrow methods). Conversion
to pandas requires pyarrow; conversion to polars requires polars. Neither
is a required dependency.
"""

from __future__ import annotations

import os
from typing import TYPE_CHECKING, Any, Dict, Iterator, List, Optional, Tuple, Union

if TYPE_CHECKING:
    import pyarrow as pa
    import pyarrow.ipc as ipc

    from dftracer.utils.dftracer_utils_ext import (
        read_arrow_files_parallel as _cpp_read_parallel,
    )

_HAS_PYARROW = False
_HAS_CPP_READER = False

try:
    import pyarrow as pa
    import pyarrow.ipc as ipc

    _HAS_PYARROW = True
except ImportError:
    pass

try:
    from dftracer.utils.dftracer_utils_ext import (
        read_arrow_files_parallel as _cpp_read_parallel,
    )

    _HAS_CPP_READER = True
except ImportError:
    pass


def _require_pyarrow() -> None:
    """Raise a clear, consistent error when pyarrow is unavailable."""
    if not _HAS_PYARROW:
        raise ImportError("pyarrow is required. Install with: pip install pyarrow")


def batch_to_ipc(batch: Any) -> bytes:
    """Serialize a single pyarrow RecordBatch to Arrow IPC stream bytes."""
    _require_pyarrow()
    sink = pa.BufferOutputStream()
    writer = pa.ipc.new_stream(sink, batch.schema)
    writer.write_batch(batch)
    writer.close()
    return sink.getvalue().to_pybytes()


def ipc_to_table(ipc_bytes: bytes) -> Any:
    """Deserialize Arrow IPC stream bytes into a pyarrow Table."""
    _require_pyarrow()
    return pa.ipc.open_stream(pa.BufferReader(ipc_bytes)).read_all()


def decode_dictionary_columns(table: Any) -> Any:
    """Cast dictionary-encoded columns to plain strings.

    Workers may dictionary-encode independently, so unify before concatenation.
    """
    for i, field in enumerate(table.schema):
        if pa.types.is_dictionary(field.type):
            table = table.set_column(i, field.name, table.column(i).cast(pa.string()))
    return table


def empty_write_result(skipped_chunks: int = 0) -> Dict[str, Any]:
    """A write_arrow result describing no written files."""
    return {
        "files": [],
        "total_chunks": 0,
        "skipped_chunks": skipped_chunks,
        "total_rows": 0,
        "total_events_matched": 0,
    }


def fold_write_results(
    batch_results: Any, total_chunks: int, skipped_chunks: int
) -> Dict[str, Any]:
    """Combine per-batch write_view_chunks results into one summary."""
    files: List[str] = []
    total_rows = 0
    total_events_matched = 0
    for br in batch_results:
        for r in br.get("results", []):
            if r.get("rows_written", 0) > 0:
                files.append(r["output_file"])
        total_rows += br.get("total_rows", 0)
        total_events_matched += br.get("total_events_matched", 0)
    return {
        "files": files,
        "total_chunks": total_chunks,
        "skipped_chunks": skipped_chunks,
        "total_rows": total_rows,
        "total_events_matched": total_events_matched,
    }


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
            import polars as pl  # type: ignore[import-not-found]  # ty: ignore[unresolved-import]
        except ImportError:
            raise ImportError(
                "polars is required for to_polars(). Install with: pip install polars"
            ) from None
        return pl.from_arrow(self._to_pa_batch())


class ArrowTable:
    """Wrapper around a collection of Arrow RecordBatches.

    Returned by read_arrow() and utility process() methods. Supports
    the Arrow PyCapsule stream protocol (__arrow_c_stream__) for
    zero-copy interchange.

    Accepts either a pre-built list of batches or a lazy iterator.
    When constructed from an iterator, batches are not materialized
    until data access (to_pandas, to_polars, batches, etc.).

    ``num_rows`` is special: if the iterator has not been consumed yet,
    it streams through counting rows without retaining batches (O(1)
    memory). After a streaming ``num_rows``, data access methods will
    return empty results -- use ``iter_arrow`` directly if you need
    both count and data for very large datasets.
    """

    def __init__(
        self,
        batches: Any,
        schema_capsule: Optional[Any] = None,
    ) -> None:
        self._stream: Any = None
        if isinstance(batches, list):
            self._batches: Optional[list[Any]] = batches
            self._iter: Optional[Iterator[Any]] = None
        elif hasattr(batches, "__arrow_c_stream__"):
            self._batches = None
            self._iter = None
            self._stream = batches
        else:
            self._batches = None
            self._iter = iter(batches)
        self._schema_capsule = schema_capsule
        self._pa_table: Any = None

    def _materialize(self) -> list[Any]:
        if self._batches is not None:
            return self._batches
        if self._stream is not None:
            self._to_pa_table()
            if self._pa_table is not None:
                self._batches = list(self._pa_table.to_batches())
                return self._batches
        if self._iter is not None:
            self._batches = list(self._iter)
            self._iter = None
            return self._batches
        self._batches = []
        return self._batches

    def _to_pa_table(self) -> Any:
        """Convert to pyarrow Table, caching the result.

        Arrow C Data Interface export is single-use (ownership transfer),
        so we cache the pyarrow table on first conversion.  After
        conversion the batch capsule references are cleared since pyarrow
        now owns the underlying buffers.

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
        if self._stream is not None:
            self._pa_table = pa.table(self._stream)
            self._stream = None
            return self._pa_table
        batches = self._materialize()
        pa_batches = [pa.record_batch(b) for b in batches]
        self._batches = None
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
        return len(self._materialize())

    @property
    def num_rows(self) -> int:
        """Total number of rows across all batches."""
        if self._pa_table is not None:
            return self._pa_table.num_rows
        return sum(b.num_rows for b in self._materialize())

    @property
    def empty(self) -> bool:
        """True if there are no batches."""
        if self._pa_table is not None:
            return self._pa_table.num_rows == 0
        return len(self._materialize()) == 0

    def batch(self, i: int) -> Any:
        """Get the i-th batch."""
        return self._materialize()[i]

    def batches(self) -> Iterator[Any]:
        """Iterate over batches."""
        return iter(self._materialize())

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
            import polars as pl  # type: ignore[import-not-found]  # ty: ignore[unresolved-import]
        except ImportError:
            raise ImportError(
                "polars is required for to_polars(). Install with: pip install polars"
            ) from None
        table = self._to_pa_table()
        if table.num_rows == 0:
            return pl.DataFrame()
        return pl.from_arrow(table)


def write_arrow(
    file_path: str,
    output_dir: str,
    view: Optional[Union[str, Dict]] = None,
    index_dir: str = "",
    checkpoint_size: int = 32 * 1024 * 1024,
    compression: str = "zstd",
    batch_size: int = 10000,
    chunks: Optional[List[Dict]] = None,
    parallel: bool = True,
) -> Dict:
    """Write trace data to Arrow IPC files.

    If chunks is provided, writes those specific chunks. Otherwise, gets
    all candidate chunks from the file after bloom filter pruning.

    Args:
        file_path: Path to the trace file.
        output_dir: Directory for output Arrow IPC files.
        view: View definition - string ('io', 'compute', 'dlio') or
              dict with 'name' and optional 'query'.
        index_dir: Directory for index files.
        checkpoint_size: Checkpoint size for indexing.
        compression: 'zstd' or 'none'.
        batch_size: Events per batch.
        chunks: Optional list of specific chunks to write. If None,
            gets all candidate chunks from the file.
        parallel: If True (default), process chunks in parallel via Runtime.

    Returns:
        dict with:
            - files: List of written Arrow IPC file paths
            - total_chunks: Number of chunks processed
            - skipped_chunks: Number of chunks skipped by bloom filter
            - total_rows: Total rows written
            - total_events_matched: Total events matched

    Example:
        >>> from dftracer.utils.arrow import write_arrow
        >>> result = write_arrow(
        ...     "trace.pfw.gz",
        ...     "/output/io_view",
        ...     view="io",
        ... )
        >>> print(f"Wrote {len(result['files'])} files")
    """
    from dftracer.utils import TraceReader, get_default_runtime

    os.makedirs(output_dir, exist_ok=True)

    reader = TraceReader(file_path, index_dir=index_dir, checkpoint_size=checkpoint_size)

    if chunks is None:
        chunks_result = reader.get_view_chunks(view=view)

        if not chunks_result["file_may_match"]:
            return empty_write_result(chunks_result["skipped_checkpoints"])

        chunks = chunks_result["chunks"]
        skipped_chunks = chunks_result["skipped_checkpoints"]
    else:
        skipped_chunks = 0

    if not chunks:
        return empty_write_result(skipped_chunks)

    if parallel and len(chunks) > 1:
        runtime = get_default_runtime()

        def write_chunk(chunk: Dict) -> Dict:
            r = TraceReader(file_path, index_dir=index_dir, checkpoint_size=checkpoint_size)
            return r.write_view_chunks(
                chunks=[chunk],
                output_dir=output_dir,
                view=view,
                compression=compression,
                batch_size=batch_size,
            )

        handles = [
            runtime.submit(write_chunk, chunk, name=f"write:chunk_{i}")
            for i, chunk in enumerate(chunks)
        ]
        batch_results = [h.get() for h in handles]
    else:
        result = reader.write_view_chunks(
            chunks=chunks,
            output_dir=output_dir,
            view=view,
            compression=compression,
            batch_size=batch_size,
        )
        batch_results = [result]

    return fold_write_results(batch_results, len(chunks), skipped_chunks)


def read_arrow(
    files: List[str],
    parallel: bool = True,
):
    """Read Arrow IPC files and return a combined pyarrow Table.

    Uses pyarrow for reading with optional parallelization via Runtime.
    Falls back to C++ reader if pyarrow is not available.

    Args:
        files: List of Arrow IPC file paths.
        parallel: If True (default), read files in parallel using Runtime.

    Returns:
        pyarrow.Table with all data combined, or list of batch objects if
        pyarrow is not available.

    Example:
        >>> from dftracer.utils.arrow import read_arrow
        >>> table = read_arrow(["file1.arrow", "file2.arrow"])
        >>> print(f"Read {table.num_rows} rows")
    """
    from dftracer.utils import get_default_runtime

    if not files:
        return None

    valid_files = [f for f in files if os.path.exists(f) and os.path.getsize(f) > 0]
    if not valid_files:
        return None

    if _HAS_PYARROW:

        def read_one(path: str) -> pa.Table:
            return ipc.open_file(path).read_all()

        if parallel and len(valid_files) > 1:
            runtime = get_default_runtime()
            handles = [
                runtime.submit(read_one, f, name=f"read:{os.path.basename(f)}") for f in valid_files
            ]
            tables = [h.get() for h in handles]
        else:
            tables = [read_one(f) for f in valid_files]

        if not tables:
            return None

        return pa.concat_tables(tables)

    if not _HAS_CPP_READER:
        raise ImportError(
            "Neither pyarrow nor C++ Arrow IPC reader available. "
            "Install pyarrow or build dftracer-utils with Arrow IPC support."
        )

    from dftracer.utils import Runtime as PyRuntime

    runtime = get_default_runtime()
    native_rt = runtime._native if isinstance(runtime, PyRuntime) else runtime

    result = _cpp_read_parallel(valid_files, runtime=native_rt)

    batches = []
    for fr in result.get("file_results", []):
        if fr.get("success"):
            batches.extend(fr.get("batches", []))

    return batches
