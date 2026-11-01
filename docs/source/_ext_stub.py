"""RTD/offline stub for the native ``dftracer_utils_ext`` module.

Installs a lightweight pure-Python stand-in so the docs build (autodoc) works
without compiling the extension. Extracted from conf.py to keep it small.
"""

from __future__ import annotations

import sys
import types
from types import TracebackType
from typing import Generic, Iterator, TypeVar

_T = TypeVar("_T")


def install_extension_stub() -> None:
    """Install a lightweight stub for the native extension on RTD."""

    ext_name = "dftracer.utils.dftracer_utils_ext"
    if ext_name in sys.modules:
        return

    ext = types.ModuleType(ext_name)

    class _BaseNative:
        """RTD stub for native extension classes."""

        pass

    class DFTUtilsError(RuntimeError):
        """Base for all dftracer-utils typed errors (derives RuntimeError)."""

    class DFTUtilsValueError(DFTUtilsError):
        """Invalid argument / value."""

    class DFTUtilsNotFoundError(DFTUtilsError):
        """A required file or resource was not found."""

    class DFTUtilsIOError(DFTUtilsError):
        """An I/O operation failed."""

    class DFTUtilsParseError(DFTUtilsError):
        """Input could not be parsed."""

    class DFTUtilsDuqlError(DFTUtilsError):
        """A duql expression was invalid."""

    class DFTUtilsReaderError(DFTUtilsError):
        """A trace reader operation failed."""

    class DFTUtilsIndexerError(DFTUtilsError):
        """An indexer operation failed."""

    class DFTUtilsPipelineError(DFTUtilsError):
        """A pipeline/task execution failed."""

    class DFTUtilsAggregationError(DFTUtilsError):
        """An aggregation operation failed."""

    class DFTUtilsCompressionError(DFTUtilsError):
        """A (de)compression operation failed."""

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
            time_interval_ms: float = 1000.0,
            group_keys: list[str] | None = None,
            compute_percentiles: bool = True,
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
                time_interval_ms: Time interval for the aggregation tier, which
                    every build makes, in milliseconds.
                group_keys: Keys to group by for aggregation.
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

        def rowset(self, name: str) -> object:
            """The rows the index build stored for row set `name`, as a
            DataFrame.

            Raises:
                ValueError: If the index holds no rows for `name`.
            """
            return None

        def query_file_pids(self, path: str) -> set:
            """Query PIDs observed in a specific file.

            Args:
                path: Path of the trace file, as given to the indexer.

            Returns:
                Set of PIDs (int) observed in the file, empty when the index
                holds no entry for the file.
            """
            return set()

        def query_all_file_pids(self) -> dict[str, set]:
            """Query all file-to-PIDs mappings.

            Returns:
                Dict mapping trace path to set of PIDs observed in that file.
            """
            return {}

        def query_file_info(self) -> dict[str, set]:
            """Query every indexed file with its PIDs in one call.

            Returns:
                Dict mapping trace path to set of PIDs observed in that file.
            """
            return {}

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
            """Release this Python wrapper's native indexer handle."""
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
            """Release this Python wrapper on context exit."""
            return None

    class TaskHandle(_BaseNative, Generic[_T]):
        """Handle to a submitted C++ coroutine task."""

        def get(self) -> _T:
            """Block until task completes and return result. Raises on error."""
            return None  # type: ignore[return-value]

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

    class Index(_BaseNative):
        """Handle to a .dftindex index root.

        Used by the distributed indexer coordinator to check staleness and to
        record the aggregation of a distributed build.
        """

        def __init__(self, index_path: str) -> None:
            self._index_path = index_path

        def find_stale_files(self, paths: list[str]) -> dict[str, object]:
            """Stat-only (mtime + size) staleness check of `paths` against the
            index. Returns a dict with keys `changed`, `added`, `removed`
            (lists of paths) and `stale` (bool)."""
            return {}

        def write_agg_config(self, aggregation_config: object) -> None:
            """Record the aggregation config a distributed build aggregated its
            files with (a ``dftracer.utils.indexer.AggregationConfig``). Each
            file's ``dftracer.agg`` entry comes with its data."""
            return None

    def merge_flamegraph_partials(partials: list[bytes]) -> object:
        """Merge serialized flamegraph partials into the node DataFrame."""
        return None

    def get_default_runtime() -> Runtime:
        """Return the process-wide default runtime."""
        return Runtime()

    def peek_default_runtime() -> Runtime | None:
        """Return the process-wide default runtime without creating one."""
        return None

    def set_log_level(level: str) -> None:
        """Set the C++ logger level ('trace'..'off'). Raises on bad name."""
        return None

    def get_log_level() -> str:
        """Return the current C++ logger level name."""
        return "info"

    def set_log_color(mode: str) -> None:
        """Set logger color mode ('auto', 'always', 'never'). Raises on bad name."""
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

    def build_index_batch(
        files: list[str],
        index_dir: str,
        checkpoint_size: int = 33554432,
        force_rebuild: bool = False,
        build_bloom: bool = True,
        bloom_dimensions: list[str] | None = None,
        parallelism: int = 0,
        runtime: Runtime | object | None = None,
        aggregation_config: object = None,
        progress: object = None,
    ) -> None:
        """Run the indexer pipeline on `files` into the index at `index_dir`,
        which this worker writes directly and numbers its own files in. Its
        aggregation folds are always created; `aggregation_config` customizes
        them and defaults to the standard config."""
        return None

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

    def write_shard_set(index_root: str, shard_dirs: list[str]) -> None:
        """Catalog ``shard_dirs`` as the shards of the index root, in the root's
        ``shards.json``. A shard under the root is stored relative to it."""
        return None

    def consolidate_shard_set(index_root: str) -> int:
        """Merge the shards the root's ``shards.json`` names into the root's own
        store by moving their data files, then remove the manifest and the shard
        directories. Returns the number of trace files the shards indexed."""
        return 0

    class _Series(_BaseNative):
        """Native columnar Series handle (SIMD ops + Arrow C Data Interface).

        The Python :class:`dftracer.utils.Series` wrapper forwards every native
        op to this handle; the ops are documented on the wrapper.
        """

        length = 0

        def __len__(self) -> int:
            return 0

        def __arrow_c_array__(self, requested_schema: object = None) -> tuple[object, object]:
            return (None, None)

    class _DataFrame(_BaseNative):
        """Native columnar DataFrame handle (a set of named :class:`_Series`).

        The Python :class:`dftracer.utils.DataFrame` wrapper forwards to it.
        """

        num_rows = 0
        num_columns = 0

        def column_names(self) -> list[str]:
            return []

        def join(self, other: "_DataFrame", how: str = "inner", on: int = 1) -> "_DataFrame":
            """Equi-join on the leading ``on`` shared key columns."""
            return _DataFrame()

        def compare_agg(self, variant: "_DataFrame", n_key: int) -> "_DataFrame":
            """Compare two aggregations on ``n_key`` group keys (delta_/pct_ columns)."""
            return _DataFrame()

        def __arrow_c_stream__(self, requested_schema: object = None) -> object:
            return None

    def _series_from_arrow(array: object) -> "_Series":
        """Import a pyarrow Array into a native Series (zero-copy)."""
        return _Series()

    def _series_from_numpy(array: object) -> "_Series":
        """Import a 1-D numpy array into a native Series, borrowing its buffer."""
        return _Series()

    def _dataframe_from_arrow(table: object) -> "_DataFrame":
        """Import a pyarrow Table into a native DataFrame (zero-copy)."""
        return _DataFrame()

    def vec_eval(ast: object, columns: object) -> "_Series":
        """Compile and evaluate one column expression on the SIMD engine."""
        return _Series()

    def vec_eval_many(asts: object, columns: object) -> list:
        """Compile several column expressions into one CSE'd pass; return one
        Series per root."""
        return []

    def jit_run_op(so_path: str, in_bytes: bytes, out_size: int) -> bytes:
        """Run a compiled jit_op ``.so`` over ``in_bytes`` on a standalone
        compose host, returning ``out_size`` bytes."""
        return b""

    def memory_budget_advice(required_bytes: int, available_bytes: int = 0) -> dict:
        """Whether an aggregated workload fits in process, with advice if not."""
        return {}

    NUM_SHARDS = 0

    _class_symbols = [
        "_ArrowBatchCapsule",
        "_ArrowBatchStream",
        "_DataFrame",
        "_Series",
        "CheckpointIndexer",
        "DFTUtilsAggregationError",
        "DFTUtilsCompressionError",
        "DFTUtilsError",
        "DFTUtilsIndexerError",
        "DFTUtilsIOError",
        "DFTUtilsNotFoundError",
        "DFTUtilsParseError",
        "DFTUtilsPipelineError",
        "DFTUtilsDuqlError",
        "DFTUtilsReaderError",
        "DFTUtilsValueError",
        "Index",
        "Indexer",
        "IndexerCheckpoint",
        "JsonDictValue",
        "Runtime",
        "TaskHandle",
    ]
    _function_symbols = [
        "_dataframe_from_arrow",
        "_series_from_arrow",
        "_series_from_numpy",
        "build_index_batch",
        "peek_default_runtime",
        "enable_aggregation_deterministic_ids",
        "write_shard_set",
        "consolidate_shard_set",
        "get_default_runtime",
        "get_log_level",
        "jit_run_op",
        "memory_budget_advice",
        "merge_flamegraph_partials",
        "plan_lpt_partition",
        "read_arrow_files_parallel",
        "scan_files",
        "set_log_color",
        "set_log_level",
        "vec_eval",
        "vec_eval_many",
    ]

    _local = locals()
    for _name in _class_symbols + _function_symbols:
        setattr(ext, _name, _local[_name])

    for _name in _class_symbols:
        getattr(ext, _name).__module__ = ext_name
    for _name in _function_symbols:
        getattr(ext, _name).__module__ = ext_name

    ext.__all__ = sorted(_class_symbols + _function_symbols)

    # Module constants accessed by name.
    ext.NUM_SHARDS = 0
    ext.PLUGIN_ABI_VERSION = 0

    # Fallback: any other native symbol (the _Native*/_TraceViewer handles, the
    # arrow ops _join/_window/..., future additions) resolves to a permissive
    # stub class so `from ..._ext import X`, `_ext.X`, and isinstance all work
    # without enumerating every symbol here.
    _generic_stubs: dict = {}

    def _module_getattr(name: str) -> object:
        if name.startswith("__") and name.endswith("__"):
            raise AttributeError(name)
        stub = _generic_stubs.get(name)
        if stub is None:
            stub = type(
                name,
                (_BaseNative,),
                {"__module__": ext_name, "__doc__": "Native extension stub."},
            )
            _generic_stubs[name] = stub
        return stub

    ext.__getattr__ = _module_getattr  # type: ignore[attr-defined]
    sys.modules[ext_name] = ext
