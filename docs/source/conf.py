# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

import os
import inspect
import importlib
import subprocess
import sys
import types
from pathlib import Path
from types import TracebackType
from typing import Iterator

# Auto-generate Mermaid class diagrams from Doxygen XML before building
_docs_dir = Path(__file__).parent.parent  # docs/
_script = _docs_dir / "scripts" / "generate_class_diagrams.py"
_xml_dir = _docs_dir / "doxygen" / "xml"
_gen_dir = _docs_dir / "source" / "_generated"
if _script.exists() and _xml_dir.exists():
    print("Generating Mermaid class diagrams from Doxygen XML...")
    subprocess.run(
        [
            sys.executable,
            str(_script),
            "--xml-dir",
            str(_xml_dir),
            "--output-dir",
            str(_gen_dir),
        ],
        check=False,
    )

# Auto-generate C++ API reference pages from Doxygen XML
_api_script = _docs_dir / "scripts" / "generate_api_index.py"
_api_out = _docs_dir / "source" / "cpp_api" / "api"
if _api_script.exists() and _xml_dir.exists():
    print("Generating C++ API reference pages from Doxygen XML...")
    subprocess.run(
        [
            sys.executable,
            str(_api_script),
            "--xml-dir",
            str(_xml_dir),
            "--output-dir",
            str(_api_out),
        ],
        check=False,
    )

ON_READTHEDOCS = os.environ.get("READTHEDOCS", "").lower() == "true"
PYTHON_SOURCE_DIR = _docs_dir.parent / "python"
autodoc_mock_imports = []


def _install_rtd_extension_stub() -> None:
    """Install a lightweight stub for the native extension on RTD."""

    ext_name = "dftracer.utils.dftracer_utils_ext"
    if ext_name in sys.modules:
        return

    ext = types.ModuleType(ext_name)

    class _BaseNative:
        """RTD stub for native extension classes."""
        pass

    class _ArrowBatchCapsule(_BaseNative):
        """Internal Arrow batch wrapper implementing __arrow_c_array__ protocol."""

        @property
        def num_rows(self) -> int:
            return 0

        @property
        def num_columns(self) -> int:
            return 0

        def __arrow_c_array__(self, requested_schema: object = None) -> tuple[object, object]:
            return (None, None)

    class _ArrowBatchStream(_BaseNative):
        """Zero-iteration Arrow stream backed by the C++ coroutine channel.

        Implements the Arrow C Data Interface stream protocol. Pass directly
        to ``pyarrow.RecordBatchReader.from_stream()`` or ``pyarrow.table()``.
        Single-use: consuming ``__arrow_c_stream__`` once exhausts the object.
        """

        def __arrow_c_stream__(self, requested_schema: object = None) -> object:
            return None

    class JsonDictValue(_BaseNative):
        """Zero-copy wrapper over a parsed DFTracer JSON event.

        Supports dict-like access: ``event['name']``, ``event['args']['ret']``.
        Call ``.to_dict()`` to materialize a regular Python dict.
        """

        def __getitem__(self, key: str) -> object:
            raise KeyError(key)

        def __len__(self) -> int:
            return 0

        def __contains__(self, key: str) -> bool:
            return False

        def keys(self) -> list[str]:
            return []

        def values(self) -> list[object]:
            return []

        def items(self) -> list[tuple[str, object]]:
            return []

        def get(self, key: str, default: object = None) -> object:
            return default

        def to_dict(self) -> dict[str, object]:
            return {}

    class IndexerCheckpoint(_BaseNative):
        """Information about a checkpoint in the index."""

        checkpoint_idx = 0
        uc_offset = 0
        uc_size = 0
        c_offset = 0
        c_size = 0
        bits = 0
        num_lines = 0

    class Runtime(_BaseNative):
        """Lightweight coroutine runtime wrapping Executor + Watchdog.

        Note: For user-facing API, use dftracer.utils.Runtime (Python wrapper)
        which adds submit(), Python callable support, and error handling.
        """

        def __init__(self, threads: int = 0, io_threads: int = 0) -> None:
            self._threads = threads
            self._io_threads = io_threads

        def shutdown(self) -> None:
            return None

        def wait_all(self) -> None:
            return None

        def get_progress(self) -> dict[str, object]:
            return {}

        def is_responsive(self) -> bool:
            return True

        def set_timeout(self, global_ms: int = 0) -> None:
            return None

        def set_default_task_timeout(self, ms: int = 0) -> None:
            return None

        @property
        def threads(self) -> int:
            return self._threads

        @property
        def io_threads(self) -> int:
            return self._io_threads

        def __enter__(self) -> "Runtime":
            return self

        def __exit__(
            self,
            exc_type: type[BaseException] | None,
            exc_val: BaseException | None,
            exc_tb: TracebackType | None,
        ) -> None:
            return None

    class Indexer(_BaseNative):
        """Indexer with resolve/build pattern for tiered indexing."""

        def __init__(
            self,
            directory: str = "",
            files: list[str] | None = None,
            index_dir: str = "",
            require_checkpoint: bool = True,
            require_bloom: bool = True,
            require_manifest: bool = True,
            require_aggregation: bool = False,
            time_interval_ms: float = 5000.0,
            group_keys: list[str] | None = None,
            custom_metric_fields: list[str] | None = None,
            compute_percentiles: bool = False,
            checkpoint_size: int = 32 * 1024 * 1024,
            parallelism: int = 0,
            force_rebuild: bool = False,
            runtime: Runtime | None = None,
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
            return None

        def resolve(self) -> dict[str, object]:
            """Resolve which files need indexing.

            Returns:
                Dictionary with 'ready' and 'needs_work' file lists.
            """
            return {}

        def build(self) -> dict[str, object]:
            """Build indices for files that need work.

            Returns:
                Dictionary with build status and statistics.
            """
            return {}

        def ensure_indexed(self) -> dict[str, object]:
            """Ensure all files are indexed by calling resolve then build if needed.

            Returns:
                Dictionary with 'ready' and 'needs_work' file lists after indexing.
            """
            return {}

        def get_checkpoint_indexer(self, file_path: str) -> "CheckpointIndexer":
            """Get a checkpoint indexer for a specific file.

            Args:
                file_path: Path to the trace file (.pfw/.pfw.gz).

            Returns:
                CheckpointIndexer instance for checkpoint-level operations.
            """
            return CheckpointIndexer(file_path)

        def get_hash_table(self, hash_type: str) -> dict[str, str]:
            """Get hash table mapping hash values to original strings.

            Args:
                hash_type: Type of hash table ('file', 'host', or 'string').

            Returns:
                Dict mapping hash strings to original values.

            Raises:
                ValueError: If hash_type is not valid.
            """
            return {}

        def query_file_pids(self, file_id: int) -> set:
            """Query PIDs observed in a specific file.

            Args:
                file_id: File identifier (0-based index).

            Returns:
                Set of PIDs (int) observed in the file.
            """
            return set()

        def query_all_file_pids(self) -> dict[int, set]:
            """Query all file-to-PIDs mappings.

            Returns:
                Dict mapping file_id to set of PIDs observed in that file.
            """
            return {}

        def query_file_info(self) -> tuple[dict[int, str], dict[int, set]]:
            """Query file ID to path mapping and per-file PIDs in one call.

            Returns:
                Tuple of (file_id_to_path, file_pids).
            """
            return ({}, {})

        def iter_aggregation(
            self,
            type: str = "events",
            batch_size: int = 10000,
        ) -> Iterator[object]:
            """Iterate over aggregation data as Arrow batches.

            Args:
                type: 'events', 'profiles', or 'system'
                batch_size: Number of entries per batch (default 10000)

            Returns:
                Iterator over Arrow batch capsules.
            """
            return iter(())

        def iter_arrow_dfanalyzer(
            self,
            type: str = "events",
            batch_size: int = 10000,
            time_granularity: float = 1.0,
            time_resolution: float = 1e6,
            query: str | None = None,
        ) -> Iterator[object]:
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
            return iter(())

        def iter_arrow_dfanalyzer_all(
            self,
            batch_size: int = 10000,
            time_granularity: float = 1.0,
            time_resolution: float = 1e6,
            query: str | None = None,
            group_by: list[str] | None = None,
        ) -> dict[str, list[object]]:
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
            return {"events": [], "profiles": [], "system": []}

    class CheckpointIndexer(_BaseNative):
        """Checkpoint indexer for single-file checkpoint-level operations."""

        def __init__(
            self,
            gz_path: str,
            index_path: str | None = None,
            checkpoint_size: int = 1048576,
            force_rebuild: bool = False,
            build_bloom: bool = False,
            build_manifest: bool = False,
            runtime: Runtime | None = None,
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
            self._gz_path = gz_path
            self._index_path = index_path or ""
            self._checkpoint_size = checkpoint_size
            self._has_bloom = build_bloom
            self._has_manifest = build_manifest

        def build(self) -> None:
            """Build the index."""
            return None

        def need_rebuild(self) -> bool:
            """Check if index needs rebuilding."""
            return False

        def exists(self) -> bool:
            """Check if the `.dftindex` store exists."""
            return False

        def get_max_bytes(self) -> int:
            """Get maximum byte position."""
            return 0

        def get_num_lines(self) -> int:
            """Get number of lines."""
            return 0

        def get_checkpoints(self) -> list[IndexerCheckpoint]:
            """Get all checkpoints."""
            return []

        def find_checkpoint(self, target_offset: int) -> IndexerCheckpoint | None:
            """Find checkpoint for target offset."""
            return None

        def close(self) -> None:
            """Release this Python wrapper's native indexer handle.

            This does not force-close the shared RocksDB instance for the same
            ``.dftindex`` path.
            """
            return None

        @property
        def gz_path(self) -> str:
            """Get gzip path."""
            return self._gz_path

        @property
        def index_path(self) -> str:
            """Get the `.dftindex` path."""
            return self._index_path

        @property
        def checkpoint_size(self) -> int:
            """Get checkpoint size."""
            return self._checkpoint_size

        @property
        def has_bloom(self) -> bool:
            """Whether bloom filter data exists in the `.dftindex` store."""
            return self._has_bloom

        @property
        def has_manifest(self) -> bool:
            """Whether manifest data exists in the `.dftindex` store."""
            return self._has_manifest

        def __enter__(self) -> "CheckpointIndexer":
            """Enter the runtime context for the with statement."""
            return self

        def __exit__(
            self,
            exc_type: type[BaseException] | None,
            exc_val: BaseException | None,
            exc_tb: TracebackType | None,
        ) -> None:
            """Release this Python wrapper on context exit.

            This does not force-close the shared RocksDB instance for the same
            ``.dftindex`` path.
            """
            return None

    class TaskHandle(_BaseNative):
        """Handle to a submitted C++ coroutine task."""

        def get(self) -> object:
            """Block until task completes and return result. Raises on error."""
            return None

        def wait(self) -> None:
            """Block until task completes. Raises on error."""
            return None

        def done(self) -> bool:
            """Return True if task has completed."""
            return True

        @property
        def name(self) -> str:
            """Task name."""
            return ""

        @property
        def task_id(self) -> int:
            """Task identifier."""
            return 0

    class TraceReader(_BaseNative):
        """Smart trace file reader that auto-selects sequential vs indexed reading."""

        def __init__(
            self,
            path: str,
            index_dir: str = "",
            checkpoint_size: int = 33554432,
            auto_build_index: bool = False,
            runtime: Runtime | object | None = None,
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
            self._path = path
            self._index_dir = index_dir

        def read_lines(
            self,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
        ) -> list[memoryview]:
            """Read lines from the trace file and return as a list.

            Lines are 1-indexed. Pass ``start_line=0, end_line=0`` (the
            defaults) to read all lines. Out-of-range values are clamped
            to the actual file bounds.
            """
            return []

        def iter_lines(
            self,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
            memory_budget: int = 0,
        ) -> Iterator[memoryview]:
            """Return a streaming iterator over decoded lines.

            The C++ coroutine runs on the Runtime thread pool and pushes
            lines into a bounded queue; Python ``__next__`` pops from it.
            """
            return iter(())

        def iter_json(
            self,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
            batch_size: int = 1024,
            memory_budget: int = 0,
        ) -> Iterator["JsonDictValue"]:
            """Return a streaming iterator over parsed JSON events.

            Each event is parsed once in C++ and yielded as a zero-copy
            :class:`JsonDictValue` wrapper. No double-parsing overhead.
            """
            return iter(())

        def read_json(
            self,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
            batch_size: int = 1024,
        ) -> list["JsonDictValue"]:
            """Read all events as parsed :class:`JsonDictValue` wrappers (list).

            Equivalent to ``list(iter_json(...))``.
            """
            return []

        def iter_raw(
            self,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            line_aligned: bool = True,
            multi_line: bool = True,
            buffer_size: int = 4194304,
            query: str | None = None,
            memory_budget: int = 0,
        ) -> Iterator[memoryview]:
            """Return a streaming iterator over raw byte chunks.

            When ``query`` is set and an index exists, chunk-level pruning
            skips non-matching chunks. No per-event filtering is applied.
            """
            return iter(())

        def read_raw(
            self,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            line_aligned: bool = True,
            multi_line: bool = True,
            buffer_size: int = 4194304,
            query: str | None = None,
        ) -> list[memoryview]:
            """Read raw byte chunks and return as a list.

            When ``query`` is set and an index exists, chunk-level pruning
            skips non-matching chunks. No per-event filtering is applied.
            """
            return []

        def iter_arrow(
            self,
            batch_size: int = 10000,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
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
            return iter(())

        def iter_arrow_stream(
            self,
            batch_size: int = 10000,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
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
            Python <-> C transitions.
            """
            return _ArrowBatchStream()

        def read_arrow(
            self,
            batch_size: int = 10000,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
            flatten_objects: bool = False,
            normalize: bool = False,
        ) -> object:
            """Read all events as an ArrowTable.

            Equivalent to collecting all batches from :meth:`iter_arrow`
            into an :class:`~dftracer.utils.arrow.ArrowTable`.
            """
            return None

        def get_max_bytes(self) -> int:
            """Get the maximum byte position in the decompressed trace.

            Returns the decompressed size for indexed files, file size for
            plain text files, or 0 for compressed files without an index.
            """
            return 0

        def get_num_lines(self) -> int:
            """Get the total number of lines in the trace.

            Returns the line count for indexed files, or 0 for files
            without an index (use :attr:`num_lines` property for fallback
            counting).
            """
            return 0

        @property
        def path(self) -> str:
            """Path to the trace file or directory."""
            return self._path

        @property
        def index_dir(self) -> str:
            """Directory searched for `.dftindex` stores."""
            return self._index_dir

        @property
        def has_index(self) -> bool:
            """True if a checkpoint index was found at construction time."""
            return False

        @property
        def num_lines(self) -> int:
            """Total line count (reads all lines to compute if needed)."""
            return 0

        def write_arrow(
            self,
            path: str,
            views: list[str | dict[str, object]] | None = None,
            chunk_size_mb: int = 32,
            compression: str = "zstd",
            batch_size: int = 10000,
        ) -> dict[str, object]:
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
            return {}

        def get_view_chunks(
            self,
            view: str | dict[str, object] | None = None,
        ) -> dict[str, object]:
            """Get candidate chunks for a view after bloom filter pruning.

            Args:
                view: View definition (string or dict with 'name' and optional 'query').

            Returns:
                Dict with chunks list, total_checkpoints, skipped_checkpoints, file_may_match.
            """
            return {}

        def write_view_chunk(
            self,
            output_file: str,
            checkpoint_idx: int,
            start_byte: int,
            end_byte: int,
            view: str | dict[str, object] | None = None,
            compression: str = "zstd",
            batch_size: int = 10000,
        ) -> dict[str, object]:
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
            return {}

        def write_view_chunks(
            self,
            chunks: list[dict[str, object]],
            output_dir: str,
            view: str | dict[str, object] | None = None,
            compression: str = "zstd",
            batch_size: int = 10000,
        ) -> dict[str, object]:
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
            return {}

        def __enter__(self) -> "TraceReader":
            """Enter the runtime context for the with statement."""
            return self

        def __exit__(
            self,
            exc_type: type[BaseException] | None,
            exc_val: BaseException | None,
            exc_tb: TracebackType | None,
        ) -> None:
            """Exit the runtime context for the with statement."""
            return None

    class StatisticsQueryUtility(_BaseNative):
        def __init__(self, runtime: Runtime | None = None) -> None:
            self.runtime = runtime

        def process(
            self,
            file_path: str,
            query_type: str = "summary",
            top_n: int = 10,
            index_dir: str = "",
        ) -> dict[str, object]:
            return {}

        def __call__(
            self,
            file_path: str,
            query_type: str = "summary",
            top_n: int = 10,
            index_dir: str = "",
        ) -> dict[str, object]:
            return {}

    class StatisticsAggregatorUtility(_BaseNative):
        def __init__(self, runtime: Runtime | None = None) -> None:
            self.runtime = runtime

        def process(
            self,
            file_path: str,
            index_dir: str = "",
        ) -> dict[str, object]:
            return {}

        def __call__(
            self,
            file_path: str,
            index_dir: str = "",
        ) -> dict[str, object]:
            return {}

    class MetadataCollectorUtility(_BaseNative):
        def __init__(self, runtime: Runtime | None = None) -> None:
            self.runtime = runtime

        def process(
            self,
            file_path: str,
            index_dir: str = "",
        ) -> dict[str, object]:
            return {}

        def __call__(
            self,
            file_path: str,
            index_dir: str = "",
        ) -> dict[str, object]:
            return {}

    class ReorganizationPlannerUtility(_BaseNative):
        def __init__(self, runtime: Runtime | None = None) -> None:
            self.runtime = runtime

        def process(
            self,
            source_files: list[str],
            groups: list[dict[str, str]] | None = None,
            index_dir: str = "",
        ) -> dict[str, object]:
            return {}

        def __call__(
            self,
            source_files: list[str],
            groups: list[dict[str, str]] | None = None,
            index_dir: str = "",
        ) -> dict[str, object]:
            return {}

    class ReconstructionPlannerUtility(_BaseNative):
        def __init__(self, runtime: Runtime | None = None) -> None:
            self.runtime = runtime

        def process(
            self,
            reorganized_files: list[str],
            index_dir: str = "",
        ) -> dict[str, object]:
            return {}

        def __call__(
            self,
            reorganized_files: list[str],
            index_dir: str = "",
        ) -> dict[str, object]:
            return {}

    class AggregatorUtility(_BaseNative):
        def __init__(self, runtime: Runtime | None = None) -> None:
            self.runtime = runtime

        def process(
            self,
            directory: str,
            time_interval_ms: float = 5000.0,
            group_keys: list[str] | None = None,
            categories: list[str] | None = None,
            names: list[str] | None = None,
            index_dir: str = "",
            checkpoint_size: int = 33554432,
            force_rebuild: bool = False,
            chunk_size_mb: int = 64,
            batch_size_mb: int = 4,
            event_batch_size: int = 10000,
            custom_metric_fields: list[str] | None = None,
            compute_percentiles: bool = False,
        ) -> object:
            return None

        def __call__(
            self,
            directory: str,
            time_interval_ms: float = 5000.0,
            group_keys: list[str] | None = None,
            categories: list[str] | None = None,
            names: list[str] | None = None,
            index_dir: str = "",
            checkpoint_size: int = 33554432,
            force_rebuild: bool = False,
            chunk_size_mb: int = 64,
            batch_size_mb: int = 4,
            event_batch_size: int = 10000,
            custom_metric_fields: list[str] | None = None,
            compute_percentiles: bool = False,
        ) -> object:
            return None

        def iter_arrow(
            self,
            directory: str,
            time_interval_ms: float = 5000.0,
            group_keys: list[str] | None = None,
            categories: list[str] | None = None,
            names: list[str] | None = None,
            index_dir: str = "",
            checkpoint_size: int = 33554432,
            force_rebuild: bool = False,
            chunk_size_mb: int = 64,
            batch_size_mb: int = 4,
            event_batch_size: int = 10000,
            custom_metric_fields: list[str] | None = None,
            compute_percentiles: bool = False,
        ) -> Iterator[object]:
            return iter(())

    class ComparatorUtility(_BaseNative):
        def __init__(self, runtime: Runtime | None = None) -> None:
            self.runtime = runtime

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
        ) -> object:
            return None

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
        ) -> object:
            return None

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
        ) -> str:
            return "{}"

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
        ) -> str:
            return ""

    class IndexDatabase(_BaseNative):
        """Handle to a .dftindex RocksDB store.

        Used by the distributed indexer coordinator to pre-register files,
        reserve file_id ranges, bulk-ingest worker-produced SSTs, and rebuild
        root summaries.
        """

        def __init__(self, index_path: str) -> None:
            self._index_path = index_path

        def init_schema(self) -> None:
            return None

        def register_files(self, paths: list[str], build_manifest: bool = False) -> list[int]:
            """Register each path in the DEFAULT-CF file registry and return
            the assigned file_ids (parallel to `paths`). Idempotent for files
            with matching hash."""
            return []

        def reserve_file_id_range(self, count: int) -> int:
            """Atomically reserve `count` contiguous file_ids; return first."""
            return 0

        def bulk_ingest(
            self,
            registry: "SstArtifactRegistry",
            skip_cfs: object = None,
        ) -> None:
            """Ingest all SSTs collected in the registry.

            skip_cfs is an optional iterable of CF names whose SSTs are left
            outside the unified DB. Distributed builds pass
            {"aggregation", "system_metrics"} to keep per-worker AGG/SYS SSTs
            addressable via `agg_manifest.json` for parallel reads at analyze
            time. See `dftracer.utils.dask.consolidate_index` to fold them
            back into the unified DB later.
            """
            return None

        def rebuild_root_summaries(self) -> None:
            """Recompute ROOT_* summary column families from per-file CFs."""
            return None

        def write_agg_global_config(self, time_interval_us: int, config_hash: int = 0) -> None:
            """Write the aggregation global-config marker into the AGGREGATION CF.

            Required for `Indexer.iter_arrow_dfanalyzer_all` on distributed
            builds (which never materialise the key via worker SSTs) and
            post-consolidate indices.
            """
            return None

        def write_agg_file_markers(self, file_ids: object) -> None:
            """Write per-file aggregation completion markers into the AGGREGATION CF.

            Each marker is ``\\xFF\\xFF + file_id_be32``. The index resolver uses
            their presence to decide whether each file has aggregated data; if
            missing, ``ensure_indexed()`` concludes the aggregation tier is
            incomplete and re-runs the entire build. Distributed_index must
            call this after ``bulk_ingest`` so subsequent ``read_trace`` calls
            do not redundantly re-aggregate.
            """
            return None

        def write_aggregation_tracker(self, blobs: list[bytes]) -> None:
            """Merge serialized AssociationTracker blobs and write the result
            to the AGGREGATION CF under the ``__tracker__`` key."""
            return None

    class SstArtifactRegistry(_BaseNative):
        """Thread-safe collector for SST artifact paths produced by workers."""

        def __init__(self) -> None:
            pass

        def append(self, artifacts_dict: dict[str, str | None]) -> None:
            """Add a per-batch Artifacts dict as returned by `build_sst_batch`."""
            return None

    def get_default_runtime() -> Runtime:
        """Return the process-wide default runtime."""
        return Runtime()

    def set_default_runtime(runtime: Runtime | None = None) -> None:
        """Replace or clear the process-wide default runtime."""
        return None

    def read_arrow_files_parallel(
        paths: list[str],
        runtime: Runtime | None = None,
    ) -> dict[str, object]:
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
        return {}

    def build_sst_batch(
        files: list[str],
        file_ids: list[int],
        staging_dir: str,
        batch_id: str,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        build_manifest: bool = False,
        force_rebuild: bool = False,
        bloom_dimensions: list[str] | None = None,
        parallelism: int = 0,
        flush_every_files: int = 0,
        runtime: Runtime | object | None = None,
        aggregation_config: object = None,
        file_slices: object = None,
    ) -> tuple[list[dict[str, str | None]], bytes]:
        """Run the indexer pipeline with an SST sink. Returns
        `(artifact_dicts, tracker_blob)`. `tracker_blob` is the serialized
        merged AssociationTracker for the batch (empty bytes when
        `aggregation_config` is None). `file_slices` enables intra-file
        parallelism; entries are `None` (whole file) or
        `(member_begin, member_end, checkpoint_idx_base,
        skip_file_scoped_writes, members)`."""
        return ([], b"")

    def plan_lpt_partition(
        entries: list[tuple[str, int]], num_workers: int
    ) -> list[list[tuple[str, int]]]:
        """Greedy LPT bin-packing of (path, size) tuples into num_workers
        buckets, minimising the maximum per-worker total size."""
        return []

    def scan_files(
        directory: str,
        patterns: list[str] | None = None,
        recursive: bool = False,
        runtime: Runtime | object | None = None,
    ) -> list[tuple[str, int]]:
        """Parallel directory scan returning (path, size) tuples for regular
        files matching the patterns."""
        return []

    def enable_aggregation_deterministic_ids() -> None:
        """Flip the global aggregation StringIntern into deterministic-id mode
        so the same string maps to the same 32-bit id in every worker process."""
        return None

    def move_artifacts(
        artifacts: dict[str, str | None], dest_dir: str
    ) -> dict[str, str | None]:
        """Move every populated SST in `artifacts` into `dest_dir` via the
        C++ rename/copy helper, returning a fresh dict with the new paths."""
        return {}

    def enumerate_gzip_members(
        files: list[str],
        runtime: Runtime | object | None = None,
    ) -> list[list[tuple[int, int]]]:
        """Cooperative async scan of gzip member offsets. Returns lists of
        `(c_offset, c_size)` parallel to `files`; empty for non-gzip files."""
        return []

    def plan_work_units(
        member_map: list[list[tuple[int, int]]],
        num_workers: int,
        target_c_size: int = 0,
    ) -> list[list[tuple[int, int, int, int]]]:
        """Deterministic LPT assignment of intra-file gzip-member slices across
        workers. Returns per-worker lists of
        `(file_idx, member_begin, member_end, c_size)`."""
        return []

    def scan_aggregation_manifest(
        agg_ssts: list[str],
        sys_ssts: list[str],
        scratch_dir: str,
        meta_index_path: str,
        batch_size: int = 10000,
        time_granularity: float = 1.0,
        time_resolution: float = 1e6,
        query: str | None = None,
        group_by: list[str] | None = None,
        shard_begin: int = 0,
        shard_end: int = 4096,
        runtime: Runtime | object | None = None,
        file_hashes: dict[str, str] | None = None,
        host_hashes: dict[str, str] | None = None,
    ) -> dict[str, list["_ArrowBatchCapsule"]]:
        """Scan a worker's slice of the distributed aggregation manifest.

        Ingests `agg_ssts` + `sys_ssts` into a scratch IndexDatabase at
        `scratch_dir` (caller owns the directory lifecycle) and runs the
        dfanalyzer aggregation scan over `[shard_begin, shard_end)`.
        `meta_index_path` is the unified .dftindex used to resolve file /
        host hashes.

        Returns the same dict shape as `Indexer.iter_arrow_dfanalyzer_all`:
        `{"events": [...], "profiles": [...], "system": [...]}`.
        """
        return {"events": [], "profiles": [], "system": []}

    _class_symbols = [
        "_ArrowBatchCapsule",
        "_ArrowBatchStream",
        "AggregatorUtility",
        "CheckpointIndexer",
        "ComparatorUtility",
        "IndexDatabase",
        "Indexer",
        "IndexerCheckpoint",
        "JsonDictValue",
        "MetadataCollectorUtility",
        "ReconstructionPlannerUtility",
        "ReorganizationPlannerUtility",
        "Runtime",
        "SstArtifactRegistry",
        "StatisticsAggregatorUtility",
        "StatisticsQueryUtility",
        "TaskHandle",
        "TraceReader",
    ]
    _function_symbols = [
        "build_sst_batch",
        "enable_aggregation_deterministic_ids",
        "enumerate_gzip_members",
        "get_default_runtime",
        "move_artifacts",
        "plan_lpt_partition",
        "plan_work_units",
        "read_arrow_files_parallel",
        "scan_aggregation_manifest",
        "scan_files",
        "set_default_runtime",
    ]

    _local = locals()
    for _name in _class_symbols + _function_symbols:
        setattr(ext, _name, _local[_name])

    for _name in _class_symbols:
        getattr(ext, _name).__module__ = ext_name
    for _name in _function_symbols:
        getattr(ext, _name).__module__ = ext_name

    ext.__all__ = sorted(_class_symbols + _function_symbols)
    sys.modules[ext_name] = ext


def _repo_url() -> str:
    """Return the GitHub repository URL used for source links."""
    repo = os.environ.get("READTHEDOCS_GIT_REPOSITORY")
    if repo:
        repo = repo.removesuffix(".git")
        if repo.startswith("git@github.com:"):
            repo = repo.replace("git@github.com:", "https://github.com/", 1)
        elif repo.startswith("https://github.com/"):
            return repo
        if repo.startswith("github.com/"):
            return f"https://{repo}"

    repo = os.environ.get("GITHUB_REPOSITORY")
    if repo:
        return f"https://github.com/{repo}"

    try:
        remote = (
            subprocess.check_output(
                ["git", "remote", "get-url", "origin"],
                cwd=_docs_dir.parent,
                text=True,
            )
            .strip()
            .removesuffix(".git")
        )
        if remote.startswith("git@github.com:"):
            return remote.replace("git@github.com:", "https://github.com/", 1)
        if remote.startswith("https://github.com/"):
            return remote
    except Exception:
        pass

    return "https://github.com/LLNL/dftracer-utils"


def _source_ref() -> str:
    """Return the git ref used for source links."""
    for env_name in ("READTHEDOCS_GIT_COMMIT_HASH", "GITHUB_SHA"):
        value = os.environ.get(env_name)
        if value:
            return value
    try:
        return (
            subprocess.check_output(
                ["git", "rev-parse", "HEAD"],
                cwd=_docs_dir.parent,
                text=True,
            )
            .strip()
        )
    except Exception:
        return "develop"

REPO_URL = _repo_url()
SOURCE_REF = _source_ref()


def _pyi_target_for_extension(fullname: str) -> tuple[Path, list[str]] | None:
    """Map extension-exported objects to their public type-stub file."""
    top = fullname.split(".", 1)[0]
    utility_map = {
        "AggregatorUtility": "python/dftracer/utils/utilities/_aggregator.pyi",
        "ComparatorUtility": "python/dftracer/utils/utilities/_comparator.pyi",
        "MetadataCollectorUtility": (
            "python/dftracer/utils/utilities/_metadata_collector.pyi"
        ),
        "StatisticsQueryUtility": (
            "python/dftracer/utils/utilities/_statistics_query.pyi"
        ),
        "StatisticsAggregatorUtility": (
            "python/dftracer/utils/utilities/_statistics_aggregator.pyi"
        ),
        "ReorganizationPlannerUtility": (
            "python/dftracer/utils/utilities/_reorganization_planner.pyi"
        ),
        "ReconstructionPlannerUtility": (
            "python/dftracer/utils/utilities/_reconstruction_planner.pyi"
        ),
    }
    rel_path = utility_map.get(top, "python/dftracer/utils/dftracer_utils_ext.pyi")
    return (_docs_dir.parent / rel_path, fullname.split("."))


def _find_symbol_lines(path: Path, parts: list[str]) -> tuple[int, int] | None:
    """Find source lines for a class/function/method in a Python source or stub file."""
    try:
        tree = ast.parse(path.read_text())
    except Exception:
        return None

    node = tree
    current_body = tree.body
    for part in parts:
        match = None
        for child in current_body:
            if isinstance(child, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                if child.name == part:
                    match = child
                    break
        if match is None:
            return None
        node = match
        current_body = getattr(match, "body", [])

    start = getattr(node, "lineno", None)
    end = getattr(node, "end_lineno", start)
    if start is None:
        return None
    return (start, end or start)


def _github_url(path: Path, lines: tuple[int, int] | None) -> str | None:
    """Build a GitHub blob URL for a repo-relative path and optional lines."""
    try:
        rel = path.resolve().relative_to(_docs_dir.parent.resolve()).as_posix()
    except Exception:
        return None
    url = f"{REPO_URL}/blob/{SOURCE_REF}/{rel}"
    if lines is not None:
        start, end = lines
        url += f"#L{start}"
        if end != start:
            url += f"-L{end}"
    return url


def linkcode_resolve(domain: str, info: dict[str, str]) -> str | None:
    """Resolve Python objects to GitHub source links."""
    if domain != "py":
        return None

    module_name = info.get("module")
    fullname = info.get("fullname")
    if not module_name or not fullname:
        return None

    try:
        module = importlib.import_module(module_name)
    except Exception:
        return None

    obj = module
    for part in fullname.split("."):
        obj = getattr(obj, part, None)
        if obj is None:
            return None

    obj_module = getattr(obj, "__module__", module_name)
    if obj_module == "dftracer.utils.dftracer_utils_ext":
        target = _pyi_target_for_extension(fullname)
        if target is None:
            return None
        path, parts = target
        lines = _find_symbol_lines(path, parts)
        return _github_url(path, lines)

    try:
        source_file = Path(inspect.getsourcefile(obj) or inspect.getfile(obj))
        _, start = inspect.getsourcelines(obj)
        end = start + max(len(inspect.getsource(obj).splitlines()) - 1, 0)
        return _github_url(source_file, (start, end))
    except Exception:
        return None


if ON_READTHEDOCS:
    sys.path.insert(0, str(PYTHON_SOURCE_DIR))
    _install_rtd_extension_stub()
    autodoc_mock_imports = [
        "pyarrow",
        "dask",
        "dask.distributed",
    ]

try:
    import dftracer.utils

    print("✓ dftracer.utils package found and imported successfully.")
except (ImportError, ModuleNotFoundError) as e:
    if not ON_READTHEDOCS and PYTHON_SOURCE_DIR.exists():
        print(f"Warning: installed dftracer.utils package not found: {e}")
        print("Falling back to source package with RTD extension stubs.")
        sys.path.insert(0, str(PYTHON_SOURCE_DIR))
        _install_rtd_extension_stub()
        autodoc_mock_imports = [
            "pyarrow",
            "dask",
            "dask.distributed",
        ]
        import dftracer.utils
    else:
        print(f"Warning: dftracer.utils package not found: {e}")
        print("API documentation will have limited information.")
        print("To generate full API docs, install the package: pip install -e .")

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information


project = "dftracer-utils"
copyright = "%Y, Ray Andrew Sinurat, Hariharan Devarajan"
author = "Ray Andrew Sinurat, Hariharan Devarajan"

# The version info for the project
# Try to get version from the package
try:
    from importlib.metadata import version

    release = version("dftracer-utils")
    version = ".".join(release.split(".")[:2])
except Exception:
    version = "0.1"
    release = "0.1.0"

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = [
    "sphinx.ext.autodoc",
    # "sphinx.ext.autosummary",  # Disabled: manual docs in api/reader.rst and api/indexer.rst
    "sphinx.ext.napoleon",
    "sphinx.ext.viewcode",
    "sphinx.ext.intersphinx",
    "sphinx.ext.todo",
    "sphinx.ext.coverage",
    "sphinx.ext.mathjax",
    # sphinx_autodoc_typehints disabled: it strips types from signatures
    # and loses C extension __text_signature__. Sphinx's built-in autodoc
    # handles both Python type hints and C extension __text_signature__.
    "myst_parser",  # For Markdown support
    "breathe",  # Always enable breathe
    "sphinx.ext.ifconfig",  # For conditional inclusion
    "sphinxcontrib.mermaid",  # Mermaid diagrams
]

# Mermaid configuration
mermaid_version = "11"
mermaid_init_js = "mermaid.initialize({startOnLoad:true, theme:'neutral'});"
mermaid_d3_zoom = True

# Check if Doxygen XML output exists and set up Breathe config
doxygen_xml_path = Path(__file__).parent.parent / "doxygen" / "xml"
if doxygen_xml_path.exists():
    cpp_api_enabled = True
    # Breathe configuration for C++ documentation
    breathe_projects = {"dftracer-utils": str(doxygen_xml_path)}
    breathe_default_project = "dftracer-utils"
else:
    cpp_api_enabled = False
    print("Warning: Doxygen XML output not found. C++ API documentation will be skipped.")
    print(f"Expected path: {doxygen_xml_path}")
    print("Run 'doxygen Doxyfile' in the docs directory to generate C++ documentation.")

# Napoleon settings for Google/NumPy style docstrings
napoleon_google_docstring = True
napoleon_numpy_docstring = True
napoleon_include_init_with_doc = True
napoleon_include_private_with_doc = False
napoleon_include_special_with_doc = True
napoleon_use_admonition_for_examples = False
napoleon_use_admonition_for_notes = False
napoleon_use_admonition_for_references = False
napoleon_use_ivar = False
napoleon_use_param = True
napoleon_use_rtype = True
napoleon_preprocess_types = False
napoleon_type_aliases = None
napoleon_attr_annotations = True

# Add mappings for intersphinx - link to main DFTracer docs and Python docs
intersphinx_mapping = {
    "python": ("https://docs.python.org/3", None),
    "dftracer": ("https://dftracer.readthedocs.io/en/latest/", None),
}

templates_path = ["_templates"]
exclude_patterns = ["api/_autosummary"]

# The suffix(es) of source filenames.
source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

# The master toctree document.
master_doc = "index"

# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = "furo"
html_static_path = ["_static"]
html_css_files = ["custom.css"]

# Search configuration
html_search_language = "en"

# Theme options
html_theme_options = {
    "navigation_with_keys": True,
}

# -- Options for autodoc -----------------------------------------------------
autodoc_default_options = {
    "members": True,
    "member-order": "bysource",
    "undoc-members": True,
    "exclude-members": "__weakref__",
}

# Type annotations in both signature and description
autodoc_typehints = "both"
autodoc_typehints_description_target = "documented"

# -- Options for todo extension ----------------------------------------------
todo_include_todos = True

# -- Options for autosummary -------------------------------------------------
autosummary_generate = False
