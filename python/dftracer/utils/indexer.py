"""Indexer utilities for building and managing trace indexes."""

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple, Union

from .dftracer_utils_ext import CheckpointIndexer as _NativeCheckpointIndexer
from .dftracer_utils_ext import Indexer as _NativeIndexer
from .runtime import Runtime

DEFAULT_CHECKPOINT_SIZE = 32 * 1024 * 1024  # 32MB

FileInfo = Tuple[Dict[int, str], Dict[int, Set[int]]]


@dataclass
class AggregationConfig:
    """Configuration for aggregation tier indexing.

    Attributes:
        time_interval_ms: Time bucket size in milliseconds (default 5000).
        group_keys: Extra grouping dimensions (default None).
        custom_metric_fields: Extra numeric args fields to aggregate (default None).
        compute_percentiles: Enable percentile sketch collection (default False).
    """

    time_interval_ms: float = 5000.0
    group_keys: Optional[List[str]] = None
    custom_metric_fields: Optional[List[str]] = None
    compute_percentiles: bool = False


@dataclass
class IndexStatus:
    """Status of index resolution.

    Attributes:
        total_files: Total number of files discovered.
        ready: Files that are fully indexed for requested tiers.
        needs_work: Files that need indexing.
        index_path: Path to the .dftindex store.
    """

    total_files: int
    ready: List[str] = field(default_factory=list)
    needs_work: List[str] = field(default_factory=list)
    index_path: str = ""


class Indexer:
    """High-level indexer for building and managing trace indexes.

    Supports tiered indexing:
    - Tier 1: Checkpoints (for random access)
    - Tier 2: Bloom filters and manifests (for fast filtering)
    - Tier 3: Aggregation data (config-dependent)

    At least one of 'directory' or 'files' must be provided.

    Args:
        directory: Directory containing trace files (.pfw/.pfw.gz).
        files: List of specific file paths to index.
        index_dir: Directory for .dftindex stores (default: next to files).
        require_checkpoint: Build checkpoint tier (default True).
        require_bloom: Build bloom filter tier (default True).
        require_manifest: Build manifest tier (default True).
        require_aggregation: Aggregation config or True for defaults (default None).
        parallelism: Number of parallel workers (0 = all cores).
        force_rebuild: Force rebuild even if index exists.
        runtime: Runtime for executor parallelism (default: global runtime).

    Example:
        >>> indexer = Indexer("/path/to/traces")
        >>> indexer.ensure_indexed()  # builds checkpoint, bloom, manifest

        >>> # With explicit file list
        >>> indexer = Indexer(files=["/path/to/trace1.pfw.gz", "/path/to/trace2.pfw.gz"])
        >>> indexer.ensure_indexed()

        >>> # With aggregation
        >>> indexer = Indexer(
        ...     "/path/to/traces",
        ...     require_aggregation=AggregationConfig(time_interval_ms=1000),
        ... )
        >>> indexer.ensure_indexed()  # fused pass with aggregation
    """

    def __init__(
        self,
        directory: str = "",
        files: Optional[List[str]] = None,
        index_dir: str = "",
        require_checkpoint: bool = True,
        require_bloom: bool = True,
        require_manifest: bool = True,
        require_aggregation: Optional[Union[bool, AggregationConfig]] = None,
        checkpoint_size: int = DEFAULT_CHECKPOINT_SIZE,
        parallelism: int = 0,
        force_rebuild: bool = False,
        runtime: Optional[Runtime] = None,
    ):
        # Normalize aggregation config
        if require_aggregation is True:
            agg_config = AggregationConfig()
        elif isinstance(require_aggregation, AggregationConfig):
            agg_config = require_aggregation
        else:
            agg_config = None

        # Build native indexer
        native_runtime = runtime._native if runtime else None
        self._native = _NativeIndexer(
            directory=directory,
            files=files,
            index_dir=index_dir,
            require_checkpoint=require_checkpoint,
            require_bloom=require_bloom,
            require_manifest=require_manifest,
            require_aggregation=agg_config is not None,
            time_interval_ms=agg_config.time_interval_ms if agg_config else 5000.0,
            group_keys=agg_config.group_keys if agg_config else None,
            custom_metric_fields=agg_config.custom_metric_fields if agg_config else None,
            compute_percentiles=agg_config.compute_percentiles if agg_config else False,
            checkpoint_size=checkpoint_size,
            parallelism=parallelism,
            force_rebuild=force_rebuild,
            runtime=native_runtime,
        )
        self._aggregation_config = agg_config
        self._file_info_cache: Optional[FileInfo] = None
        self._closed = False

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def close(self):
        """Release resources."""
        self._closed = True

    @property
    def aggregation_config(self) -> Optional[AggregationConfig]:
        """Aggregation configuration, if enabled."""
        return self._aggregation_config

    def resolve(self) -> IndexStatus:
        """Check what files exist vs need indexing.

        Returns:
            IndexStatus with total_files, ready, and needs_work lists.
        """
        result = self._native.resolve()
        return IndexStatus(
            total_files=result["total_files"],
            ready=result["ready"],
            needs_work=result["needs_work"],
            index_path=result.get("index_path", ""),
        )

    def build(self) -> None:
        """Build all missing index tiers based on require_* flags.

        This method builds indexes in parallel using the Runtime executor.
        When aggregation is enabled, it performs a fused pass for efficiency.
        """
        self._native.build()

    def ensure_indexed(self) -> IndexStatus:
        """Resolve and build if needed.

        Convenience method that calls resolve() then build() if needed.

        Returns:
            IndexStatus after building.
        """
        result = self._native.ensure_indexed()
        return IndexStatus(
            total_files=result["total_files"],
            ready=result["ready"],
            needs_work=result["needs_work"],
            index_path=result.get("index_path", ""),
        )

    def get_checkpoint_indexer(self, file_path: str) -> _NativeCheckpointIndexer:
        """Get a checkpoint indexer for a specific file.

        Returns an indexer for checkpoint-level operations on a single file,
        such as finding checkpoints for random access.

        Args:
            file_path: Path to the trace file (.pfw/.pfw.gz).

        Returns:
            Indexer instance for checkpoint operations (checkpoints, find_checkpoint, etc).
        """
        return self._native.get_checkpoint_indexer(file_path)

    def get_hash_table(self, hash_type: str) -> dict:
        """Query hash table mappings.

        Returns a dictionary mapping hash values to resolved names for the
        given hash type. This is useful for resolving fhash/hhash values in
        aggregated data.

        Args:
            hash_type: One of 'file', 'host', 'string', or 'proc'.

        Returns:
            dict mapping hash values (str) to resolved names (str).

        Example:
            >>> indexer = Indexer("/path/to/traces")
            >>> indexer.ensure_indexed()
            >>> file_names = indexer.get_hash_table("file")
            >>> # file_names = {"abc123": "/path/to/data.h5", ...}
        """
        return self._native.get_hash_table(hash_type)

    def query_file_pids(self, file_id: int) -> set:
        """Query PIDs observed in a specific file.

        Args:
            file_id: Integer file ID from index.

        Returns:
            set of PIDs (int) observed in the file.
        """
        return self._native.query_file_pids(file_id)

    def query_all_file_pids(self) -> dict:
        """Query PIDs for all indexed files.

        Returns a dictionary mapping file_id to the set of PIDs observed
        in that file. This is useful for distributed aggregation to assign
        files to workers by PID affinity.

        Returns:
            dict mapping file_id (int) to set of PIDs (int).
        """
        return self._native.query_all_file_pids()

    def query_file_info(self) -> FileInfo:
        """Query file distribution info in a single DB open.

        Returns:
            Tuple of (file_id_to_path, file_pids) where:
            - file_id_to_path: dict[int, str] mapping DB file ID to path
            - file_pids: dict[int, set[int]] mapping file ID to PIDs
        """
        if self._file_info_cache is None:
            self._file_info_cache = self._native.query_file_info()
        return self._file_info_cache

    def iter_aggregation(self, type: str = "events", batch_size: int = 10000):
        """Iterate over aggregation data as Arrow batches.

        Requires that the index was built with require_aggregation=True.
        Returns Arrow batches that can be converted to pandas or pyarrow.

        Args:
            type: Type of aggregation data - 'events', 'profiles', or 'system'
            batch_size: Number of entries per Arrow batch (default 10000)

        Yields:
            Arrow batch capsules implementing __arrow_c_array__

        Example:
            >>> import pyarrow as pa
            >>> indexer = Indexer("/traces", require_aggregation=True)
            >>> indexer.ensure_indexed()
            >>> batches = [pa.record_batch(b) for b in indexer.iter_aggregation("events")]
            >>> table = pa.concat_tables([pa.Table.from_batches([b]) for b in batches])
        """
        return self._native.iter_aggregation(type, batch_size)

    def iter_arrow_dfanalyzer(
        self,
        type: str = "events",
        batch_size: int = 10000,
        time_granularity: float = 1.0,
        time_resolution: float = 1e6,
        query: Optional[str] = None,
    ):
        """Iterate over aggregation data as dfanalyzer-compatible Arrow batches.

        Returns Arrow batches with columns matching dfanalyzer schema:

        - Events/Profiles: cat, func_name, pid, tid, file_hash, host_hash,
          file_name, host_name, proc_name, io_cat, acc_pat, count, time, size,
          time_min, time_max, size_min, size_max, time_range, time_start, time_end
        - System: host_hash, time_range, ``sys_cpu_*``, ``sys_mem_*``

        Hash resolution, time normalization, and computed columns (proc_name,
        io_cat) are done in C++ for performance.

        Args:
            type: Type of aggregation data - 'events', 'profiles', or 'system'
            batch_size: Number of entries per Arrow batch (default 10000)
            time_granularity: Bucket width in seconds (default 1.0)
            time_resolution: Microseconds per output time unit (default 1e6)
            query: Optional query filter string (e.g., "pid == 1234 or pid == 5678")

        Yields:
            Arrow batch capsules implementing __arrow_c_array__

        Example:
            >>> import pyarrow as pa
            >>> indexer = Indexer("/traces", require_aggregation=True)
            >>> indexer.ensure_indexed()
            >>> batches = list(indexer.iter_arrow_dfanalyzer("events"))
            >>> table = pa.concat_tables([pa.Table.from_batches([pa.record_batch(b)]) for b in batches])
        """
        if query is not None:
            return self._native.iter_arrow_dfanalyzer(
                type, batch_size, time_granularity, time_resolution, query
            )
        return self._native.iter_arrow_dfanalyzer(
            type, batch_size, time_granularity, time_resolution
        )

    def iter_arrow_dfanalyzer_all(
        self,
        batch_size: int = 10000,
        time_granularity: float = 1.0,
        time_resolution: float = 1e6,
        query: Optional[str] = None,
        group_by: Optional[List[str]] = None,
    ):
        """Iterate over all aggregation types in a single scan.

        This is ~3x faster than calling iter_arrow_dfanalyzer separately for
        events, profiles, and system because it scans the index only once.

        When ``group_by`` is provided, aggregation collapses dimensions during
        the scan and emits a reduced schema containing only the requested
        group columns plus aggregated metrics (``count``, ``time``, ``size``,
        ``time_sq``, ``size_sq``, ``time_min``, ``time_max``, ``size_min``,
        ``size_max``, ``time_call_min``, ``time_call_max``, ``size_call_min``,
        ``size_call_max``, ``time_start``, ``time_end``).

        Args:
            batch_size: Number of entries per Arrow batch (default 10000)
            time_granularity: Bucket width in seconds (default 1.0)
            time_resolution: Microseconds per output time unit (default 1e6)
            query: Optional query filter string (e.g., "pid == 1234 or pid == 5678")
            group_by: Optional list of columns to group by for coarse in-scan
                aggregation. Supported: ``cat``, ``func_name``, ``pid``,
                ``tid``, ``file_hash``, ``host_hash``, ``file_name``,
                ``host_name``, ``proc_name``, ``io_cat``, ``acc_pat``,
                ``time_range``.

        Returns:
            Dict with 'events', 'profiles', 'system' keys, each containing
            a list of Arrow batch capsules.

        Example:
            >>> import pyarrow as pa
            >>> indexer = Indexer("/traces", require_aggregation=True)
            >>> indexer.ensure_indexed()
            >>> all_batches = indexer.iter_arrow_dfanalyzer_all()
            >>> events = [pa.record_batch(b) for b in all_batches["events"]]
        """
        return self._native.iter_arrow_dfanalyzer_all(
            batch_size,
            time_granularity,
            time_resolution,
            query,
            group_by,
        )
