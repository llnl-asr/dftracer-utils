"""Type stubs for dftracer_utils_ext module."""

from types import TracebackType
from typing import Any, Dict, Iterable, Iterator, List, Optional, Tuple, Type, Union

from .arrow import ArrowTable

class _ArrowBatchCapsule:
    """Internal Arrow batch wrapper implementing __arrow_c_array__ protocol."""

    @property
    def num_rows(self) -> int: ...
    @property
    def num_columns(self) -> int: ...
    def __arrow_c_array__(self, requested_schema: Any = None) -> Tuple[Any, Any]: ...

class _ArrowBatchStream:
    """Zero-iteration Arrow stream backed by the C++ coroutine channel.

    Implements the Arrow C Data Interface stream protocol. Pass directly
    to ``pyarrow.RecordBatchReader.from_stream()`` or ``pyarrow.table()``.
    Single-use: consuming ``__arrow_c_stream__`` once exhausts the object.
    """

    def __arrow_c_stream__(self, requested_schema: Any = None) -> Any: ...

class JsonDictValue:
    """Zero-copy wrapper over a parsed DFTracer JSON event.

    Supports dict-like access: ``event['name']``, ``event['args']['ret']``.
    Call ``.to_dict()`` to materialize a regular Python dict.
    """

    def __getitem__(self, key: str) -> Any: ...
    def __len__(self) -> int: ...
    def __contains__(self, key: str) -> bool: ...
    def keys(self) -> List[str]: ...
    def values(self) -> List[Any]: ...
    def items(self) -> List[Tuple[str, Any]]: ...
    def get(self, key: str, default: Any = None) -> Any: ...
    def to_dict(self) -> Dict[str, Any]: ...

# ========== INDEXER ==========

class IndexerCheckpoint:
    """Information about a checkpoint in the index."""

    checkpoint_idx: int
    uc_offset: int
    uc_size: int
    c_offset: int
    c_size: int
    bits: int
    num_lines: int

class Indexer:
    """Indexer with resolve/build pattern for tiered indexing."""

    def __init__(
        self,
        directory: str = "",
        files: Optional[List[str]] = None,
        index_dir: str = "",
        require_checkpoint: bool = True,
        require_bloom: bool = True,
        require_manifest: bool = True,
        require_aggregation: bool = False,
        time_interval_ms: float = 5000.0,
        group_keys: Optional[List[str]] = None,
        custom_metric_fields: Optional[List[str]] = None,
        compute_percentiles: bool = False,
        checkpoint_size: int = 32 * 1024 * 1024,
        parallelism: int = 0,
        force_rebuild: bool = False,
        runtime: Optional["Runtime"] = None,
    ) -> None:
        """Create an indexer for trace files.

        At least one of 'directory' or 'files' must be provided.

        Args:
            directory: Path to the directory containing trace files.
            files: List of specific file paths to index.
            index_dir: Directory for `.dftindex` stores. If empty, uses
                directory-local paths.
            require_checkpoint: If True, build checkpoint index (tier 1).
            require_bloom: If True, build bloom filter data (tier 2).
            require_manifest: If True, build manifest data (tier 2).
            require_aggregation: If True, build aggregation data (tier 3).
            time_interval_ms: Time interval for aggregation in milliseconds.
            group_keys: Keys to group by for aggregation.
            custom_metric_fields: Custom metric fields for aggregation.
            compute_percentiles: If True, compute percentiles during aggregation.
            parallelism: Number of parallel indexers. 0 = auto.
            force_rebuild: If True, rebuild indices even if they exist.
            runtime: Runtime instance for thread pool control.
        """
        ...

    def resolve(self) -> Dict[str, Any]:
        """Resolve which files need indexing.

        Returns:
            Dictionary with 'ready' and 'needs_work' file lists.
        """
        ...

    def build(self) -> Dict[str, Any]:
        """Build indices for files that need work.

        Returns:
            Dictionary with build status and statistics.
        """
        ...

    def ensure_indexed(self) -> Dict[str, Any]:
        """Ensure all files are indexed by calling resolve then build if needed.

        Returns:
            Dictionary with 'ready' and 'needs_work' file lists after indexing.
        """
        ...

    def get_checkpoint_indexer(self, file_path: str) -> "CheckpointIndexer":
        """Get a checkpoint indexer for a specific file.

        Args:
            file_path: Path to the trace file (.pfw/.pfw.gz).

        Returns:
            CheckpointIndexer instance for checkpoint-level operations.
        """
        ...

    def get_hash_table(self, hash_type: str) -> Dict[str, str]:
        """Get hash table mapping hash values to original strings.

        Args:
            hash_type: Type of hash table ('file', 'host', or 'string').

        Returns:
            Dict mapping hash strings to original values.

        Raises:
            ValueError: If hash_type is not valid.
        """
        ...

    def query_file_pids(self, file_id: int) -> set:
        """Query PIDs observed in a specific file.

        Args:
            file_id: File identifier (0-based index).

        Returns:
            Set of PIDs (int) observed in the file.
        """
        ...

    def query_all_file_pids(self) -> Dict[int, set]:
        """Query all file-to-PIDs mappings.

        Returns:
            Dict mapping file_id to set of PIDs observed in that file.
        """
        ...

    def query_file_info(self) -> Tuple[Dict[int, str], Dict[int, set]]:
        """Query file ID to path mapping and per-file PIDs in one call.

        Returns:
            Tuple of (file_id_to_path, file_pids).
        """
        ...

    def iter_aggregation(self, type: str = "events", batch_size: int = 10000) -> Iterator[Any]:
        """Iterate over aggregation data as Arrow batches.

        Args:
            type: 'events', 'profiles', or 'system'
            batch_size: Number of entries per batch (default 10000)

        Returns:
            Iterator over Arrow batch capsules.
        """
        ...

    def iter_arrow_dfanalyzer(
        self,
        type: str = "events",
        batch_size: int = 10000,
        time_granularity: float = 1.0,
        time_resolution: float = 1e6,
        query: Optional[str] = None,
    ) -> Iterator[Any]:
        """Iterate over aggregation data as dfanalyzer-compatible Arrow batches.

        Args:
            type: 'events', 'profiles', or 'system'
            batch_size: Number of entries per batch (default 10000)
            time_granularity: Bucket width in seconds (default 1.0)
            time_resolution: Microseconds per output time unit (default 1e6)
            query: Optional query filter (e.g., "pid == 1234 or pid == 5678")

        Returns:
            Iterator over Arrow batch capsules with dfanalyzer schema.
        """
        ...

    def iter_arrow_dfanalyzer_all(
        self,
        batch_size: int = 10000,
        time_granularity: float = 1.0,
        time_resolution: float = 1e6,
        query: Optional[str] = None,
        group_by: Optional[List[str]] = None,
    ) -> Dict[str, List[Any]]:
        """Iterate over all aggregation types in a single scan.

        Args:
            batch_size: Number of entries per batch (default 10000)
            time_granularity: Bucket width in seconds (default 1.0)
            time_resolution: Microseconds per output time unit (default 1e6)
            query: Optional query filter (e.g., "pid == 1234 or pid == 5678")
            group_by: Optional list of columns to group by for coarse in-scan
                aggregation. When provided, output schema is reduced to the
                requested group columns plus aggregated metrics.

        Returns:
            Dict with 'events', 'profiles', 'system' keys containing Arrow batches.
        """
        ...

class CheckpointIndexer:
    """Checkpoint indexer for single-file checkpoint-level operations."""

    def __init__(
        self,
        gz_path: str,
        index_path: Optional[str] = None,
        checkpoint_size: int = 1048576,
        force_rebuild: bool = False,
        build_bloom: bool = False,
        build_manifest: bool = False,
        runtime: Optional["Runtime"] = None,
    ) -> None:
        """Create a checkpoint indexer for a gzip file.

        Args:
            gz_path: Path to the gzip trace file.
            index_path: Path to the `.dftindex` store. If None, uses the
                root-local `.dftindex` next to ``gz_path``.
            checkpoint_size: Checkpoint size in bytes for index building.
            force_rebuild: If True, rebuild the index even if it exists.
            build_bloom: If True, build bloom filter data in the index.
            build_manifest: If True, build manifest data in the index.
            runtime: Runtime instance for thread pool control.
                If None, uses the default global Runtime.
        """
        ...

    def build(self) -> None:
        """Build the index."""
        ...

    def need_rebuild(self) -> bool:
        """Check if index needs rebuilding."""
        ...

    def exists(self) -> bool:
        """Check if the `.dftindex` store exists."""
        ...

    def get_max_bytes(self) -> int:
        """Get maximum byte position."""
        ...

    def get_num_lines(self) -> int:
        """Get number of lines."""
        ...

    def get_checkpoints(self) -> List[IndexerCheckpoint]:
        """Get all checkpoints."""
        ...

    def find_checkpoint(self, target_offset: int) -> Optional[IndexerCheckpoint]:
        """Find checkpoint for target offset."""
        ...

    def close(self) -> None:
        """Release this Python wrapper's native indexer handle.

        This does not force-close the shared RocksDB instance for the same
        ``.dftindex`` path.
        """
        ...

    @property
    def gz_path(self) -> str:
        """Get gzip path."""
        ...

    @property
    def index_path(self) -> str:
        """Get the `.dftindex` path."""
        ...

    @property
    def checkpoint_size(self) -> int:
        """Get checkpoint size."""
        ...

    @property
    def has_bloom(self) -> bool:
        """Whether bloom filter data exists in the `.dftindex` store."""
        ...

    @property
    def has_manifest(self) -> bool:
        """Whether manifest data exists in the `.dftindex` store."""
        ...

    def __enter__(self) -> "CheckpointIndexer":
        """Enter the runtime context for the with statement."""
        ...

    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> None:
        """Release this Python wrapper on context exit.

        This does not force-close the shared RocksDB instance for the same
        ``.dftindex`` path.
        """
        ...

# ========== TASK HANDLE ==========

class TaskHandle:
    """Handle to a submitted C++ coroutine task."""

    def get(self) -> Any:
        """Block until task completes and return result. Raises on error."""
        ...

    def wait(self) -> None:
        """Block until task completes. Raises on error."""
        ...

    def done(self) -> bool:
        """Return True if task has completed."""
        ...

    @property
    def name(self) -> str:
        """Task name."""
        ...

    @property
    def task_id(self) -> int:
        """Task identifier."""
        ...

# ========== RUNTIME (C++ native) ==========

class Runtime:
    """Lightweight coroutine runtime wrapping Executor + Watchdog.

    Note: For user-facing API, use dftracer.utils.Runtime (Python wrapper)
    which adds submit(), Python callable support, and error handling.
    """

    def __init__(self, threads: int = 0, io_threads: int = 0) -> None: ...
    def shutdown(self) -> None: ...
    def wait_all(self) -> None: ...
    def get_progress(self) -> Dict[str, Any]: ...
    def is_responsive(self) -> bool: ...
    def set_timeout(self, global_ms: int = 0) -> None: ...
    def set_default_task_timeout(self, ms: int = 0) -> None: ...
    @property
    def threads(self) -> int: ...
    @property
    def io_threads(self) -> int: ...
    def __enter__(self) -> "Runtime": ...
    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> None: ...

def get_default_runtime() -> Runtime: ...
def set_default_runtime(runtime: Optional[Runtime]) -> None: ...

# ========== TRACE READER ==========

class TraceReader:
    """Smart trace file reader that auto-selects sequential vs indexed reading."""

    def __init__(
        self,
        path: str,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        auto_build_index: bool = False,
        runtime: Optional[Union[Runtime, object]] = None,
    ) -> None:
        """Create a TraceReader.

        Args:
            path: Path to a trace file (.pfw/.pfw.gz) or a directory.
                When a directory is given, all iter/read methods discover
                .pfw and .pfw.gz files recursively and process them in
                parallel on the Runtime thread pool.
            index_dir: Directory to search for ``.dftindex`` stores.
                Empty string (default) searches next to the trace file.
            checkpoint_size: Checkpoint interval in bytes for index
                building (default 32 MB).
            auto_build_index: If True, automatically build an index
                when none exists.
            runtime: Runtime instance for thread pool control.
                If None, uses the default global Runtime.

        Raises:
            RuntimeError: If *file_path* does not exist or cannot be opened.
        """
        ...

    def read_lines(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
    ) -> List[memoryview]:
        """Read lines from the trace file and return as a list.

        Lines are 1-indexed. Pass ``start_line=0, end_line=0`` (the
        defaults) to read all lines. Out-of-range values are clamped
        to the actual file bounds.
        """
        ...

    def iter_lines(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        memory_budget: int = 0,
    ) -> Iterator[memoryview]:
        """Return a streaming iterator over decoded lines.

        The C++ coroutine runs on the Runtime thread pool and pushes
        lines into a bounded queue; Python ``__next__`` pops from it.
        """
        ...

    def iter_json(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        batch_size: int = 1024,
        memory_budget: int = 0,
    ) -> Iterator["JsonDictValue"]:
        """Return a streaming iterator over parsed JSON events.

        Each event is parsed once in C++ and yielded as a zero-copy
        :class:`JsonDictValue` wrapper. No double-parsing overhead.
        """
        ...

    def read_json(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        batch_size: int = 1024,
    ) -> List["JsonDictValue"]:
        """Read all events as parsed :class:`JsonDictValue` wrappers (list).

        Equivalent to ``list(iter_json(...))``.
        """
        ...

    def iter_raw(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        line_aligned: bool = True,
        multi_line: bool = True,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        memory_budget: int = 0,
    ) -> Iterator[memoryview]:
        """Return a streaming iterator over raw byte chunks.

        When ``query`` is set and an index exists, chunk-level pruning
        skips non-matching chunks. No per-event filtering is applied.
        """
        ...

    def read_raw(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        line_aligned: bool = True,
        multi_line: bool = True,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
    ) -> List[memoryview]:
        """Read raw byte chunks and return as a list.

        When ``query`` is set and an index exists, chunk-level pruning
        skips non-matching chunks. No per-event filtering is applied.
        """
        ...

    def iter_arrow(
        self,
        batch_size: int = 10000,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        flatten_objects: bool = False,
        normalize: bool = False,
        memory_budget: int = 0,
    ) -> Iterator["_ArrowBatchCapsule"]:
        """Return iterator over Arrow record batches.

        Each batch is an ``_ArrowBatchCapsule`` implementing the Arrow
        PyCapsule protocol (``__arrow_c_array__``).  Wrap with
        :class:`~dftracer.utils.arrow.ArrowBatch` for convenience
        methods, or pass directly to ``pyarrow.record_batch()``.
        """
        ...

    def iter_arrow_stream(
        self,
        batch_size: int = 10000,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        flatten_objects: bool = False,
        normalize: bool = False,
        memory_budget: int = 0,
    ) -> "_ArrowBatchStream":
        """Return an Arrow C Data Interface stream over record batches.

        PyArrow can drain the producer channel in a single C-side call:

            rbr = pa.RecordBatchReader.from_stream(reader.iter_arrow_stream())
            for batch in rbr:
                ...

        Equivalent data to :meth:`iter_arrow`, but without per-batch
        Python ↔ C transitions.
        """
        ...

    def read_arrow(
        self,
        batch_size: int = 10000,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
        query: Optional[str] = None,
        flatten_objects: bool = False,
        normalize: bool = False,
    ) -> "ArrowTable":
        """Read all events as an ArrowTable.

        Equivalent to collecting all batches from :meth:`iter_arrow`
        into an :class:`~dftracer.utils.arrow.ArrowTable`.
        """
        ...

    def get_max_bytes(self) -> int:
        """Get the maximum byte position in the decompressed trace.

        Returns the decompressed size for indexed files, file size for
        plain text files, or 0 for compressed files without an index.
        """
        ...

    def get_num_lines(self) -> int:
        """Get the total number of lines in the trace.

        Returns the line count for indexed files, or 0 for files
        without an index (use :attr:`num_lines` property for fallback
        counting).
        """
        ...

    @property
    def path(self) -> str:
        """Path to the trace file or directory."""
        ...

    @property
    def index_dir(self) -> str:
        """Directory searched for `.dftindex` stores."""
        ...

    @property
    def has_index(self) -> bool:
        """True if a checkpoint index was found at construction time."""
        ...

    @property
    def num_lines(self) -> int:
        """Total line count (reads all lines to compute if needed)."""
        ...

    def write_arrow(
        self,
        path: str,
        views: Optional[List[Union[str, Dict[str, Any]]]] = None,
        chunk_size_mb: int = 32,
        compression: str = "zstd",
        batch_size: int = 10000,
    ) -> Dict[str, Any]:
        """Write trace data to Arrow IPC files with optional view-based partitioning.

        Args:
            path: Output directory for Arrow IPC files.
            views: List of view definitions. Each can be:
                - A string: predefined view name ('io', 'compute', 'dlio')
                - A dict with 'name' and optional 'query', 'include_metadata'
                If None, writes all events to 'all' partition.
            chunk_size_mb: Maximum uncompressed size per file in MB.
            compression: 'zstd' or 'none'.
            batch_size: Events per Arrow batch.

        Returns:
            Dict with partitions, total_rows, total_bytes, chunks_scanned, chunks_skipped.
        """
        ...

    def get_view_chunks(
        self,
        view: Optional[Union[str, Dict[str, Any]]] = None,
    ) -> Dict[str, Any]:
        """Get candidate chunks for a view after bloom filter pruning.

        Args:
            view: View definition (string or dict with 'name' and optional 'query').

        Returns:
            Dict with chunks list, total_checkpoints, skipped_checkpoints, file_may_match.
        """
        ...

    def write_view_chunk(
        self,
        output_file: str,
        checkpoint_idx: int,
        start_byte: int,
        end_byte: int,
        view: Optional[Union[str, Dict[str, Any]]] = None,
        compression: str = "zstd",
        batch_size: int = 10000,
    ) -> Dict[str, Any]:
        """Write a single chunk to an Arrow IPC file.

        Args:
            output_file: Path to output Arrow IPC file.
            checkpoint_idx: Checkpoint index.
            start_byte: Start byte offset.
            end_byte: End byte offset.
            view: View definition.
            compression: 'zstd' or 'none'.
            batch_size: Events per batch.

        Returns:
            Dict with output_file, events_matched, rows_written, bytes_written.
        """
        ...

    def write_view_chunks(
        self,
        chunks: List[Dict[str, Any]],
        output_dir: str,
        view: Optional[Union[str, Dict[str, Any]]] = None,
        compression: str = "zstd",
        batch_size: int = 10000,
    ) -> Dict[str, Any]:
        """Write multiple chunks to Arrow IPC files in parallel.

        All chunks are processed concurrently on the Runtime thread pool.

        Args:
            chunks: List of dicts with checkpoint_idx, start_byte, end_byte.
            output_dir: Directory for output Arrow IPC files.
            view: View definition.
            compression: 'zstd' or 'none'.
            batch_size: Events per batch.

        Returns:
            Dict with results list, total_rows, total_events_matched.
        """
        ...

    def __enter__(self) -> "TraceReader":
        """Enter the runtime context for the with statement."""
        ...

    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> None:
        """Exit the runtime context for the with statement."""
        ...

class StatisticsQueryUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        file_path: str,
        query_type: str = "summary",
        top_n: int = 10,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        file_path: str,
        query_type: str = "summary",
        top_n: int = 10,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...

class StatisticsAggregatorUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        file_path: str,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        file_path: str,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...

class MetadataCollectorUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        file_path: str,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        file_path: str,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...

class ReorganizationPlannerUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        source_files: List[str],
        groups: Optional[List[Dict[str, str]]] = None,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        source_files: List[str],
        groups: Optional[List[Dict[str, str]]] = None,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...

class ReconstructionPlannerUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        reorganized_files: List[str],
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        reorganized_files: List[str],
        index_dir: str = "",
    ) -> Dict[str, Any]: ...

class AggregatorUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        directory: str,
        time_interval_ms: float = 5000.0,
        group_keys: Optional[List[str]] = None,
        categories: Optional[List[str]] = None,
        names: Optional[List[str]] = None,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        force_rebuild: bool = False,
        chunk_size_mb: int = 64,
        batch_size_mb: int = 4,
        event_batch_size: int = 10000,
        custom_metric_fields: Optional[List[str]] = None,
        compute_percentiles: bool = False,
    ) -> Any: ...
    def __call__(
        self,
        directory: str,
        time_interval_ms: float = 5000.0,
        group_keys: Optional[List[str]] = None,
        categories: Optional[List[str]] = None,
        names: Optional[List[str]] = None,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        force_rebuild: bool = False,
        chunk_size_mb: int = 64,
        batch_size_mb: int = 4,
        event_batch_size: int = 10000,
        custom_metric_fields: Optional[List[str]] = None,
        compute_percentiles: bool = False,
    ) -> Any: ...
    def iter_arrow(
        self,
        directory: str,
        time_interval_ms: float = 5000.0,
        group_keys: Optional[List[str]] = None,
        categories: Optional[List[str]] = None,
        names: Optional[List[str]] = None,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        force_rebuild: bool = False,
        chunk_size_mb: int = 64,
        batch_size_mb: int = 4,
        event_batch_size: int = 10000,
        custom_metric_fields: Optional[List[str]] = None,
        compute_percentiles: bool = False,
    ) -> Iterator[Any]: ...

class ComparatorUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def compare(
        self,
        baseline: str,
        variant: str,
        query: str = "",
        group_by: str = "",
        format: str = "table",
        time_interval_ms: float = 5000.0,
        threshold: float = 0.0,
        executor_threads: int = 0,
        index_dir: str = "",
        force_rebuild: bool = False,
        config: str = "",
    ) -> Any: ...
    def __call__(
        self,
        baseline: str,
        variant: str,
        query: str = "",
        group_by: str = "",
        format: str = "table",
        time_interval_ms: float = 5000.0,
        threshold: float = 0.0,
        executor_threads: int = 0,
        index_dir: str = "",
        force_rebuild: bool = False,
        config: str = "",
    ) -> Any: ...
    def compare_json(
        self,
        baseline: str,
        variant: str,
        query: str = "",
        group_by: str = "",
        format: str = "table",
        time_interval_ms: float = 5000.0,
        threshold: float = 0.0,
        executor_threads: int = 0,
        index_dir: str = "",
        force_rebuild: bool = False,
        config: str = "",
    ) -> str: ...
    def compare_table(
        self,
        baseline: str,
        variant: str,
        query: str = "",
        group_by: str = "",
        format: str = "table",
        time_interval_ms: float = 5000.0,
        threshold: float = 0.0,
        executor_threads: int = 0,
        index_dir: str = "",
        force_rebuild: bool = False,
        config: str = "",
    ) -> str: ...

# ========== ARROW PARALLEL READER ==========

def read_arrow_files_parallel(
    paths: List[str],
    runtime: Optional[Runtime] = None,
) -> Dict[str, Any]:
    """Read multiple Arrow IPC files in parallel using the Runtime.

    Args:
        paths: List of file paths to read.
        runtime: Optional Runtime object. Uses default if not provided.

    Returns:
        dict with:
            - file_results: List of per-file results, each with:
                - path: File path
                - success: True if read succeeded
                - error: Error message if failed, else None
                - total_rows: Number of rows in file
                - batches: List of ArrowBatch objects
            - total_rows: Total rows across all files
            - total_batches: Total batches across all files
            - files_read: Number of files read successfully
            - files_failed: Number of files that failed
    """
    ...

# ========== DISTRIBUTED INDEX (SST-based) ==========

class IndexDatabase:
    """Handle to a .dftindex RocksDB store.

    Used by the distributed indexer coordinator to pre-register files,
    reserve file_id ranges, bulk-ingest worker-produced SSTs, and rebuild
    root summaries.
    """

    def __init__(self, index_path: str) -> None: ...
    def init_schema(self) -> None: ...
    def register_files(self, paths: List[str], build_manifest: bool = False) -> List[int]:
        """Register each path in the DEFAULT-CF file registry and return
        the assigned file_ids (parallel to `paths`). Idempotent for files
        with matching hash."""
        ...

    def reserve_file_id_range(self, count: int) -> int:
        """Atomically reserve `count` contiguous file_ids; return first."""
        ...

    def bulk_ingest(
        self,
        registry: "SstArtifactRegistry",
        skip_cfs: Optional[Iterable[str]] = None,
    ) -> None:
        """Ingest all SSTs collected in the registry.

        skip_cfs is an optional iterable of CF names whose SSTs are left
        outside the unified DB. Distributed builds pass
        {"aggregation", "system_metrics"} to keep per-worker AGG/SYS SSTs
        addressable via `agg_manifest.json` for parallel reads at analyze
        time. See `dftracer.utils.dask.consolidate_index` to fold them
        back into the unified DB later.
        """
        ...

    def rebuild_root_summaries(self) -> None:
        """Recompute ROOT_* summary column families from per-file CFs."""
        ...

    def write_agg_global_config(self, time_interval_us: int, config_hash: int = 0) -> None:
        """Write the aggregation global-config marker into the AGGREGATION CF.

        Required for `Indexer.iter_arrow_dfanalyzer_all` on distributed
        builds (which never materialise the key via worker SSTs) and
        post-consolidate indices.
        """
        ...

    def write_agg_file_markers(self, file_ids: Iterable[int]) -> None:
        """Write per-file aggregation completion markers into the AGGREGATION CF.

        Each marker is ``\\xFF\\xFF + file_id_be32``. The index resolver uses
        their presence to decide whether each file has aggregated data; if
        missing, ``ensure_indexed()`` concludes the aggregation tier is
        incomplete and re-runs the entire build. Distributed_index must
        call this after ``bulk_ingest`` so subsequent ``read_trace`` calls
        do not redundantly re-aggregate.
        """
        ...

    def write_aggregation_tracker(self, blobs: List[bytes]) -> None:
        """Merge serialized AssociationTracker blobs and write the result
        to the AGGREGATION CF under the ``__tracker__`` key."""
        ...

class SstArtifactRegistry:
    """Thread-safe collector for SST artifact paths produced by workers."""

    def __init__(self) -> None: ...
    def append(self, artifacts_dict: Dict[str, Optional[str]]) -> None:
        """Add a per-batch Artifacts dict as returned by `build_sst_batch`."""
        ...

def build_sst_batch(
    files: List[str],
    file_ids: List[int],
    staging_dir: str,
    batch_id: str,
    index_dir: str = "",
    checkpoint_size: int = 33554432,
    build_manifest: bool = False,
    force_rebuild: bool = False,
    bloom_dimensions: Optional[List[str]] = None,
    parallelism: int = 0,
    flush_every_files: int = 0,
    runtime: Optional[Union[Runtime, object]] = None,
    aggregation_config: Optional[Any] = None,
    file_slices: Optional[List[Optional[Tuple[int, int, int, bool, List[Tuple[int, int]]]]]] = None,
) -> Tuple[List[Dict[str, Optional[str]]], bytes]:
    """Run the indexer pipeline with an SST sink. Returns
    `(artifact_dicts, tracker_blob)`. `tracker_blob` is the serialized
    merged AssociationTracker for the batch (empty bytes when
    `aggregation_config` is None). `file_slices` enables intra-file
    parallelism; entries are `None` (whole file) or
    `(member_begin, member_end, checkpoint_idx_base,
    skip_file_scoped_writes, members)`."""
    ...

def plan_lpt_partition(
    entries: List[Tuple[str, int]], num_workers: int
) -> List[List[Tuple[str, int]]]:
    """Greedy LPT bin-packing of (path, size) tuples into num_workers
    buckets, minimising the maximum per-worker total size."""
    ...

def scan_files(
    directory: str,
    patterns: Optional[List[str]] = None,
    recursive: bool = False,
    runtime: Optional[Union[Runtime, object]] = None,
) -> List[Tuple[str, int]]:
    """Parallel directory scan returning (path, size) tuples for regular
    files matching the patterns."""
    ...

def enable_aggregation_deterministic_ids() -> None:
    """Flip the global aggregation StringIntern into deterministic-id mode
    so the same string maps to the same 32-bit id in every worker process."""
    ...

def move_artifacts(artifacts: Dict[str, Optional[str]], dest_dir: str) -> Dict[str, Optional[str]]:
    """Move every populated SST in `artifacts` into `dest_dir` via the
    C++ rename/copy helper, returning a fresh dict with the new paths."""
    ...

def enumerate_gzip_members(
    files: List[str],
    runtime: Optional[Union[Runtime, object]] = None,
) -> List[List[Tuple[int, int]]]:
    """Cooperative async scan of gzip member offsets. Returns lists of
    `(c_offset, c_size)` parallel to `files`; empty for non-gzip files."""
    ...

def plan_work_units(
    member_map: List[List[Tuple[int, int]]],
    num_workers: int,
    target_c_size: int = 0,
) -> List[List[Tuple[int, int, int, int]]]:
    """Deterministic LPT assignment of intra-file gzip-member slices across
    workers. Returns per-worker lists of
    `(file_idx, member_begin, member_end, c_size)`."""
    ...

def scan_aggregation_manifest(
    agg_ssts: List[str],
    sys_ssts: List[str],
    scratch_dir: str,
    meta_index_path: str,
    batch_size: int = 10000,
    time_granularity: float = 1.0,
    time_resolution: float = 1e6,
    query: Optional[str] = None,
    group_by: Optional[List[str]] = None,
    shard_begin: int = 0,
    shard_end: int = 4096,
    runtime: Optional[Union[Runtime, object]] = None,
    file_hashes: Optional[Dict[str, str]] = None,
    host_hashes: Optional[Dict[str, str]] = None,
) -> Dict[str, List[_ArrowBatchCapsule]]:
    """Scan a worker's slice of the distributed aggregation manifest.

    Ingests `agg_ssts` + `sys_ssts` into a scratch IndexDatabase at
    `scratch_dir` (caller owns the directory lifecycle) and runs the
    dfanalyzer aggregation scan over `[shard_begin, shard_end)`.
    `meta_index_path` is the unified .dftindex used to resolve file /
    host hashes.

    Returns the same dict shape as `Indexer.iter_arrow_dfanalyzer_all`:
    `{"events": [...], "profiles": [...], "system": [...]}`.
    """
    ...
