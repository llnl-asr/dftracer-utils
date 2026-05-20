"""Dask distributed integration for dftracer-utils."""

import os
from collections import defaultdict
from typing import Any, Dict, List, Optional, Union

try:
    from dask.distributed import Client, WorkerPlugin
except ImportError:
    Client: Optional[Any] = None
    WorkerPlugin: Optional[Any] = None

try:
    import dask
    import dask.dataframe as dd
except ImportError:
    dask = None  # type: ignore[assignment]  # ty: ignore[invalid-assignment]
    dd = None  # type: ignore[assignment]  # ty: ignore[invalid-assignment]

try:
    import pyarrow as pa
except ImportError:
    pa = None  # type: ignore[assignment]  # ty: ignore[invalid-assignment]

from dftracer.utils import Runtime, TraceReader, get_default_runtime, set_default_runtime
from dftracer.utils.indexer import AggregationConfig, Indexer

if WorkerPlugin is not None:

    class DFTracerUtilsDaskWorkerPlugin(WorkerPlugin):
        """Creates a persistent Runtime per Dask worker."""

        def __init__(self, threads=0, io_threads=0):
            self.threads = threads
            self.io_threads = io_threads

        def setup(self, worker):
            worker._dftracer_prev_runtime = get_default_runtime()
            rt = Runtime(threads=self.threads, io_threads=self.io_threads)
            worker.dftracer_utils_runtime = rt
            set_default_runtime(rt)

        def teardown(self, worker):
            if hasattr(worker, "_dftracer_prev_runtime"):
                set_default_runtime(worker._dftracer_prev_runtime)
                del worker._dftracer_prev_runtime
            if hasattr(worker, "dftracer_utils_runtime"):
                # wait=False: don't block on pending tasks during teardown.
                # Dask may be tearing down because of timeout/cancel, and a
                # stuck task would hang the worker process indefinitely.
                try:
                    worker.dftracer_utils_runtime.shutdown(wait=False)
                except Exception:
                    pass
                del worker.dftracer_utils_runtime


def _write_arrow_task(
    file_path: str,
    output_dir: str,
    view: Optional[Union[str, Dict]],
    index_dir: str,
    checkpoint_size: int,
    compression: str,
    batch_size: int,
    chunks: List[Dict],
) -> Dict:
    """Task function for writing chunks on a Dask worker."""
    from dftracer.utils.arrow import write_arrow

    return write_arrow(
        file_path=file_path,
        output_dir=output_dir,
        view=view,
        index_dir=index_dir,
        checkpoint_size=checkpoint_size,
        compression=compression,
        batch_size=batch_size,
        chunks=chunks,
        parallel=True,
    )


def distributed_write_arrow(
    file_path: str,
    output_dir: str,
    view: Optional[Union[str, Dict]] = None,
    index_dir: str = "",
    checkpoint_size: int = 32 * 1024 * 1024,
    compression: str = "zstd",
    batch_size: int = 10000,
    chunks_per_task: int = 0,
) -> Dict:
    """Write trace data to Arrow IPC files using Dask distributed.

    This function:
    1. Gets candidate chunks after bloom filter pruning (coordinator)
    2. Distributes chunk processing to Dask workers
    3. Each worker writes its chunks to Arrow IPC files
    4. Returns paths to all written files for pyarrow reading

    Args:
        file_path: Path to the trace file.
        output_dir: Directory for output Arrow IPC files.
        view: View definition - string ('io', 'compute', 'dlio') or
              dict with 'name' and optional 'query'.
        index_dir: Directory for index files.
        checkpoint_size: Checkpoint size for indexing.
        compression: 'zstd' or 'none'.
        batch_size: Events per batch.
        chunks_per_task: Number of chunks per Dask task. If 0, uses 1 chunk
            per task. Higher values batch chunks per worker, processing
            them in parallel on the worker's Runtime thread pool.

    Returns:
        dict with:
            - files: List of written Arrow IPC file paths
            - total_chunks: Number of chunks processed
            - skipped_chunks: Number of chunks skipped by bloom filter
            - total_rows: Total rows written
            - total_events_matched: Total events matched

    Example:
        >>> import pyarrow.ipc as ipc
        >>> import pyarrow as pa
        >>> from dftracer.utils.dask import distributed_write_arrow
        >>>
        >>> result = distributed_write_arrow(
        ...     "trace.pfw.gz",
        ...     "/output/io_view",
        ...     view="io",
        ...     chunks_per_task=8,  # batch 8 chunks per worker
        ... )
        >>> # Read back with pyarrow
        >>> tables = [ipc.open_file(f).read_all() for f in result["files"]]
        >>> combined = pa.concat_tables(tables)
    """
    if dask is None:
        raise ImportError("dask is required for distributed_write_arrow")

    os.makedirs(output_dir, exist_ok=True)

    reader = TraceReader(file_path, index_dir=index_dir, checkpoint_size=checkpoint_size)
    chunks_result = reader.get_view_chunks(view=view)

    if not chunks_result["file_may_match"]:
        return {
            "files": [],
            "total_chunks": 0,
            "skipped_chunks": chunks_result["skipped_checkpoints"],
            "total_rows": 0,
            "total_events_matched": 0,
        }

    chunks = chunks_result["chunks"]
    if not chunks:
        return {
            "files": [],
            "total_chunks": 0,
            "skipped_chunks": chunks_result["skipped_checkpoints"],
            "total_rows": 0,
            "total_events_matched": 0,
        }

    if chunks_per_task <= 0:
        chunks_per_task = 1

    batches = [chunks[i : i + chunks_per_task] for i in range(0, len(chunks), chunks_per_task)]

    delayed_tasks = [
        dask.delayed(_write_arrow_task)(
            file_path,
            output_dir,
            view,
            index_dir,
            checkpoint_size,
            compression,
            batch_size,
            batch,
        )
        for batch in batches
    ]

    batch_results = dask.compute(*delayed_tasks)

    files = []
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
        "total_chunks": len(chunks),
        "skipped_chunks": chunks_result["skipped_checkpoints"],
        "total_rows": total_rows,
        "total_events_matched": total_events_matched,
    }


def _assign_files_by_pid(
    file_pids: Dict[int, set],
    n_workers: int,
) -> Dict[int, List[int]]:
    """Assign files to workers based on majority PID affinity.

    Files with overlapping PIDs are assigned to the same worker to minimize
    cross-worker aggregation during the merge phase.

    Args:
        file_pids: Dict mapping file_id to set of PIDs in that file.
        n_workers: Number of workers to distribute to.

    Returns:
        Dict mapping worker_id to list of file_ids.
    """
    if n_workers <= 0:
        n_workers = 1

    # Count PIDs per file and assign to worker by hash(majority_pid) % n_workers
    worker_assignments: Dict[int, List[int]] = defaultdict(list)

    for file_id, pids in file_pids.items():
        if not pids:
            # No PIDs known, round-robin assignment
            worker_id = file_id % n_workers
        else:
            # Use hash of first PID for deterministic assignment
            # Files with same PIDs go to same worker
            majority_pid = min(pids)  # Use min for determinism
            worker_id = hash(majority_pid) % n_workers
        worker_assignments[worker_id].append(file_id)

    return dict(worker_assignments)


def _aggregate_files_task(
    files: List[str],
    index_path: str,
    time_granularity: float,
    time_resolution: float,
    data_type: str,
) -> List[bytes]:
    """Worker task: read pre-indexed data and return Arrow IPC buffers.

    This runs on a Dask worker. It reads from an already-indexed database
    and returns Arrow IPC buffers for the specified files.

    Args:
        files: List of trace file paths (used for filtering, not re-indexing).
        index_path: Path to the .dftindex store (already built by coordinator).
        time_granularity: Output time bucket width in seconds.
        time_resolution: Microseconds per output time unit.
        data_type: 'events', 'profiles', or 'system'.

    Returns:
        List of Arrow IPC buffer bytes.
    """
    if not files:
        return []

    # Use existing index (read-only) - coordinator already built it
    indexer = Indexer(
        files=files,
        index_dir=os.path.dirname(index_path) if index_path else "",
        require_checkpoint=False,  # Don't rebuild
        require_bloom=False,
        require_manifest=False,
        require_aggregation=False,  # Already aggregated
        force_rebuild=False,
    )

    # Collect Arrow batches as IPC buffers
    ipc_buffers = []
    for batch_capsule in indexer.iter_arrow_dfanalyzer(
        data_type,
        time_granularity=time_granularity,
        time_resolution=time_resolution,
    ):
        batch = pa.record_batch(batch_capsule)
        # Serialize to IPC buffer for transfer
        sink = pa.BufferOutputStream()
        writer = pa.ipc.new_stream(sink, batch.schema)
        writer.write_batch(batch)
        writer.close()
        ipc_buffers.append(sink.getvalue().to_pybytes())

    return ipc_buffers


def _aggregate_files_task_all(
    files: List[str],
    index_path: str,
    time_granularity: float,
    time_resolution: float,
    query: Optional[str] = None,
) -> Dict[str, List[bytes]]:
    """Worker task: read all aggregation types and return Arrow IPC buffers.

    This runs on a Dask worker. It reads from an already-indexed database
    and returns Arrow IPC buffers for all types in a single scan.

    Args:
        files: List of trace file paths (used for filtering, not re-indexing).
        index_path: Path to the .dftindex store (already built by coordinator).
        time_granularity: Output time bucket width in seconds.
        time_resolution: Microseconds per output time unit.
        query: Optional query filter string (e.g., "pid == 1234 or pid == 5678").

    Returns:
        Dict with 'events', 'profiles', 'system' keys, each containing
        a list of Arrow IPC buffer bytes.
    """
    if not files:
        return {"events": [], "profiles": [], "system": []}

    indexer = Indexer(
        files=files,
        index_dir=os.path.dirname(index_path) if index_path else "",
        require_checkpoint=False,
        require_bloom=False,
        require_manifest=False,
        require_aggregation=False,
        force_rebuild=False,
    )

    all_batches = indexer.iter_arrow_dfanalyzer_all(
        time_granularity=time_granularity,
        time_resolution=time_resolution,
        query=query,
    )

    # Convert to IPC buffers for each type
    result = {}
    for data_type in ("events", "profiles", "system"):
        ipc_buffers = []
        for batch_capsule in all_batches.get(data_type, []):
            batch = pa.record_batch(batch_capsule)
            sink = pa.BufferOutputStream()
            writer = pa.ipc.new_stream(sink, batch.schema)
            writer.write_batch(batch)
            writer.close()
            ipc_buffers.append(sink.getvalue().to_pybytes())
        result[data_type] = ipc_buffers

    return result


def _merge_welford(group):
    """Merge mean/variance using parallel Welford algorithm.

    Used for re-aggregating overlapping keys across workers.
    """
    n_total = group["count"].sum()
    if n_total == 0:
        return {
            "count": 0,
            "time": 0.0,
            "size": 0,
            "time_min": 0.0,
            "time_max": 0.0,
            "size_min": 0,
            "size_max": 0,
        }

    # Sum aggregation for count, time, size
    result = {
        "count": n_total,
        "time": group["time"].sum(),
        "size": group["size"].sum(),
        "time_min": group["time_min"].min(),
        "time_max": group["time_max"].max(),
        "size_min": group["size_min"].min(),
        "size_max": group["size_max"].max(),
    }
    return result


def distributed_aggregate(
    directory: str = "",
    files: Optional[List[str]] = None,
    client: Optional["Client"] = None,
    time_interval_ms: float = 5000.0,
    time_granularity: float = 1.0,
    time_resolution: float = 1e6,
    index_dir: str = "",
    data_type: str = "events",
) -> "pa.Table":
    """Aggregate trace data using Dask distributed workers.

    This function:
    1. Indexes all files on coordinator to get PID manifests
    2. Assigns files to workers by PID affinity (minimize cross-worker overlap)
    3. Each worker aggregates its files using iter_arrow_dfanalyzer
    4. Gathers partial Arrow tables from workers
    5. Re-aggregates overlapping keys (same PID/time_range across files)

    Args:
        directory: Directory containing trace files (.pfw/.pfw.gz).
        files: Explicit list of files (alternative to directory).
        client: Dask distributed Client. If None, uses dask.delayed locally.
        time_interval_ms: Aggregation time bucket in milliseconds.
        time_granularity: Output time bucket width in seconds.
        time_resolution: Microseconds per output time unit.
        index_dir: Directory for index storage.
        data_type: Type of data to aggregate - 'events', 'profiles', or 'system'.

    Returns:
        PyArrow Table with aggregated data.

    Example:
        >>> from dask.distributed import Client
        >>> from dftracer.utils.dask import distributed_aggregate
        >>>
        >>> client = Client("scheduler:8786")
        >>> client.register_plugin(DFTracerUtilsDaskWorkerPlugin(threads=48))
        >>>
        >>> table = distributed_aggregate(
        ...     directory="/traces",
        ...     client=client,
        ...     time_interval_ms=5000,
        ... )
        >>> df = table.to_pandas()
    """
    if dask is None:
        raise ImportError("dask is required for distributed_aggregate")
    if pa is None:
        raise ImportError("pyarrow is required for distributed_aggregate")

    # Step 1: Index on coordinator
    indexer = Indexer(
        directory=directory,
        files=files,
        index_dir=index_dir,
        require_checkpoint=True,
        require_bloom=True,
        require_manifest=True,
        require_aggregation=AggregationConfig(
            time_interval_ms=time_interval_ms,
            compute_percentiles=False,
        ),
        force_rebuild=False,
    )
    status = indexer.ensure_indexed()

    if status.total_files == 0:
        return pa.table({})

    # For local execution (no client), just use iter_arrow_dfanalyzer directly
    # This avoids RocksDB locking issues when running in a single process
    if client is None:
        all_batches = []
        for batch_capsule in indexer.iter_arrow_dfanalyzer(
            data_type,
            time_granularity=time_granularity,
            time_resolution=time_resolution,
        ):
            all_batches.append(pa.record_batch(batch_capsule))

        if not all_batches:
            return pa.table({})

        return pa.Table.from_batches(all_batches)

    # Distributed execution: assign files to workers by PID affinity
    all_files = status.ready + status.needs_work
    file_id_to_path, file_pids = indexer.query_file_info()
    index_path = status.index_path

    # Close indexer before distributing (release RocksDB lock)
    indexer.close()

    worker_nthreads = client.nthreads()
    n_workers = len(worker_nthreads) or 1

    all_file_ids = set(file_id_to_path.keys())
    full_file_pids = {fid: file_pids.get(fid, set()) for fid in all_file_ids}
    worker_file_ids = _assign_files_by_pid(full_file_pids, n_workers)

    worker_files: Dict[int, List[str]] = {}
    for worker_id, fids in worker_file_ids.items():
        worker_files[worker_id] = [file_id_to_path[fid] for fid in fids if fid in file_id_to_path]

    futures = []
    worker_list = list(worker_nthreads.keys())
    for worker_id, wfiles in worker_files.items():
        if not wfiles:
            continue
        worker_addr = worker_list[worker_id % len(worker_list)] if worker_list else None
        future = client.submit(
            _aggregate_files_task,
            wfiles,
            index_path,
            time_granularity,
            time_resolution,
            data_type,
            workers=[worker_addr] if worker_addr else None,
            pure=False,
        )
        futures.append(future)

    # Gather results
    all_ipc_buffers = client.gather(futures)

    # Deserialize IPC buffers and combine
    all_batches = []
    for ipc_buffers in all_ipc_buffers:
        for buf_bytes in ipc_buffers:
            reader = pa.ipc.open_stream(pa.BufferReader(buf_bytes))
            for batch in reader:
                all_batches.append(batch)

    if not all_batches:
        return pa.table({})

    combined_table = pa.Table.from_batches(all_batches)

    # Step 6: Re-aggregate overlapping keys using Dask DataFrame
    # This handles cases where the same (pid, tid, time_range, func_name) appears
    # across multiple files assigned to different workers
    if data_type == "system":
        # System metrics: group by host_hash, time_range
        group_cols = ["host_hash", "time_range"]
        agg_dict = {
            "sys_cpu_iowait_pct": "mean",
            "sys_cpu_user_pct": "mean",
            "sys_cpu_system_pct": "mean",
            "sys_cpu_idle_pct": "mean",
            "sys_core_iowait_pct_max": "max",
            "sys_core_iowait_pct_p95": "max",
            "sys_mem_dirty": "mean",
            "sys_mem_cached": "mean",
            "sys_mem_available": "mean",
        }
    else:
        # Events/Profiles: group by all key columns
        group_cols = [
            "cat",
            "func_name",
            "pid",
            "tid",
            "file_hash",
            "host_hash",
            "time_range",
        ]
        agg_dict = {
            "count": "sum",
            "time": "sum",
            "size": "sum",
            "time_min": "min",
            "time_max": "max",
            "size_min": "min",
            "size_max": "max",
        }

    # Check if re-aggregation is needed (more than one file)
    if len(all_files) > 1:
        df = combined_table.to_pandas()

        # Preserve non-aggregated columns
        first_cols = {}
        for col in df.columns:
            if col not in group_cols and col not in agg_dict:
                first_cols[col] = "first"

        agg_dict.update(first_cols)

        # Group and aggregate
        result_df = df.groupby(group_cols, as_index=False).agg(agg_dict)
        return pa.Table.from_pandas(result_df, preserve_index=False)

    return combined_table


def distributed_aggregate_all(
    directory: str = "",
    files: Optional[List[str]] = None,
    client: Optional["Client"] = None,
    time_interval_ms: float = 5000.0,
    time_granularity: float = 1.0,
    time_resolution: float = 1e6,
    index_dir: str = "",
) -> Dict[str, "pa.Table"]:
    """Aggregate all trace data types in a single scan.

    This is ~3x faster than calling distributed_aggregate separately for
    events, profiles, and system because it scans the index only once.

    Args:
        directory: Directory containing trace files (.pfw/.pfw.gz).
        files: Explicit list of files (alternative to directory).
        client: Dask distributed Client. If None, uses local execution.
        time_interval_ms: Aggregation time bucket in milliseconds.
        time_granularity: Output time bucket width in seconds.
        time_resolution: Microseconds per output time unit.
        index_dir: Directory for index storage.

    Returns:
        Dict with 'events', 'profiles', 'system' keys, each containing a
        PyArrow Table with aggregated data.

    Example:
        >>> from dftracer.utils.dask import distributed_aggregate_all
        >>> tables = distributed_aggregate_all("/traces")
        >>> events_df = tables["events"].to_pandas()
        >>> profiles_df = tables["profiles"].to_pandas()
    """
    if dask is None:
        raise ImportError("dask is required for distributed_aggregate_all")
    if pa is None:
        raise ImportError("pyarrow is required for distributed_aggregate_all")

    # Index on coordinator
    indexer = Indexer(
        directory=directory,
        files=files,
        index_dir=index_dir,
        require_checkpoint=True,
        require_bloom=True,
        require_manifest=True,
        require_aggregation=AggregationConfig(
            time_interval_ms=time_interval_ms,
            compute_percentiles=False,
        ),
        force_rebuild=False,
    )
    status = indexer.ensure_indexed()

    if status.total_files == 0:
        return {"events": pa.table({}), "profiles": pa.table({}), "system": pa.table({})}

    # Use fused API for local execution
    if client is None:
        result = indexer.iter_arrow_dfanalyzer_all(
            time_granularity=time_granularity,
            time_resolution=time_resolution,
        )

        tables = {}
        for key in ("events", "profiles", "system"):
            batches = [pa.record_batch(cap) for cap in result.get(key, [])]
            tables[key] = pa.Table.from_batches(batches) if batches else pa.table({})

        return tables

    # Distributed execution: assign files to workers by PID affinity
    file_id_to_path, file_pids = indexer.query_file_info()
    index_path = status.index_path

    # Close indexer before distributing (release RocksDB lock)
    indexer.close()

    worker_nthreads = client.nthreads()
    n_workers = len(worker_nthreads) or 1

    all_file_ids = set(file_id_to_path.keys())
    full_file_pids = {fid: file_pids.get(fid, set()) for fid in all_file_ids}
    worker_file_ids = _assign_files_by_pid(full_file_pids, n_workers)

    worker_files: Dict[int, List[str]] = {}
    worker_pids: Dict[int, set] = {}
    for worker_id, fids in worker_file_ids.items():
        worker_files[worker_id] = [file_id_to_path[fid] for fid in fids if fid in file_id_to_path]
        pids = set()
        for fid in fids:
            if fid in file_pids:
                pids.update(file_pids[fid])
        worker_pids[worker_id] = pids

    futures = []
    worker_list = list(worker_nthreads.keys())
    for worker_id, wfiles in worker_files.items():
        if not wfiles:
            continue
        # Build query filter for this worker's PIDs
        pids = worker_pids.get(worker_id, set())
        query = None
        if pids:
            pid_conditions = " or ".join(f"pid == {pid}" for pid in sorted(pids))
            query = f"({pid_conditions})"
        worker_addr = worker_list[worker_id % len(worker_list)] if worker_list else None
        future = client.submit(
            _aggregate_files_task_all,
            wfiles,
            index_path,
            time_granularity,
            time_resolution,
            query,
            workers=[worker_addr] if worker_addr else None,
            pure=False,
        )
        futures.append(future)

    # Gather results (each is a dict with events/profiles/system)
    all_results = client.gather(futures)

    # Collect batches by type
    batches_by_type: Dict[str, List] = {"events": [], "profiles": [], "system": []}
    for result_dict in all_results:
        for data_type in ("events", "profiles", "system"):
            for buf_bytes in result_dict.get(data_type, []):
                reader = pa.ipc.open_stream(pa.BufferReader(buf_bytes))
                for batch in reader:
                    batches_by_type[data_type].append(batch)

    tables = {}
    for data_type in ("events", "profiles", "system"):
        batches = batches_by_type[data_type]
        if not batches:
            tables[data_type] = pa.table({})
            continue
        table = pa.Table.from_batches(batches)
        # Unify dictionary columns from different workers to plain strings
        for i, field in enumerate(table.schema):
            if pa.types.is_dictionary(field.type):
                table = table.set_column(i, field.name, table.column(i).cast(pa.string()))
        tables[data_type] = table

    return tables


# ---------------------------------------------------------------------------
# Distributed index build (SST sink path)
# ---------------------------------------------------------------------------


def _build_sst_task(
    files: List[str],
    file_ids: List[int],
    file_slices: Optional[List[Any]],
    local_staging: str,
    shared_staging: str,
    batch_id: str,
    index_dir: str,
    checkpoint_size: int,
    bloom_dimensions: Optional[List[str]],
    build_manifest: bool,
    force_rebuild: bool,
    parallelism: int,
    flush_every_files: int,
    aggregation_config: Optional[Any] = None,
    enable_det_ids: bool = False,
) -> tuple:
    """Dask worker task: build per-worker SSTs and relocate to shared FS.

    Returns ``(artifact_dicts, tracker_blob)``."""
    import logging as _logging
    import socket as _socket
    import time as _time

    from .dftracer_utils_ext import build_sst_batch, move_artifacts

    _log = _logging.getLogger("dftracer.utils.dask._build_sst_task")
    _host = _socket.gethostname()

    t0 = _time.monotonic()
    if enable_det_ids:
        from .dftracer_utils_ext import enable_aggregation_deterministic_ids

        enable_aggregation_deterministic_ids()

    artifact_dicts, tracker_blob = build_sst_batch(
        files,
        file_ids,
        local_staging,
        batch_id,
        index_dir,
        checkpoint_size,
        build_manifest,
        force_rebuild,
        bloom_dimensions,
        parallelism,
        flush_every_files,
        None,
        aggregation_config,
        file_slices,
    )
    t_build = _time.monotonic()

    n_moved = 0
    if shared_staging and shared_staging != local_staging:
        # Keep per-sink subdir to avoid aggregation.sst collisions.
        base = os.path.join(shared_staging, batch_id)
        relocated: List[Dict[str, Optional[str]]] = []
        for i, d in enumerate(artifact_dicts):
            relocated.append(move_artifacts(d, os.path.join(base, f"sub_{i}")))
        artifact_dicts = relocated
        n_moved = len(relocated)
    t_move = _time.monotonic()

    _log.info(
        "build host=%s batch=%s n_files=%d n_slices=%d n_artifacts=%d "
        "build=%.2fs move=%.2fs(n=%d) total=%.2fs",
        _host,
        batch_id,
        len(set(file_ids)),
        len(files),
        len(artifact_dicts),
        t_build - t0,
        t_move - t_build,
        n_moved,
        t_move - t0,
    )
    return artifact_dicts, tracker_blob


def _scan_gzip_members_task(paths: List[str]) -> List[List[tuple]]:
    """Worker task: scan gzip member offsets for its file subset."""
    from .dftracer_utils_ext import enumerate_gzip_members

    return enumerate_gzip_members(paths, None)


def distributed_index(
    directory: str = "",
    files: Optional[List[str]] = None,
    index_path: str = "",
    local_staging: str = "",
    shared_staging: str = "",
    client: Optional["Client"] = None,
    checkpoint_size: int = 32 * 1024 * 1024,
    bloom_dimensions: Optional[List[str]] = None,
    build_manifest: bool = True,
    force_rebuild: bool = False,
    partition: str = "lpt",
    rebuild_root_summaries: bool = True,
    parallelism_per_worker: int = 0,
    flush_every_files: int = 0,
    aggregation_config: Optional[Any] = None,
) -> Dict[str, Any]:
    """Index a set of trace files using Dask workers writing SSTs in parallel.

    Steps (all O(1) on the coordinator except the fan-out):
      1. Enumerate files + sizes via parallel scan.
      2. LPT bin-pack files into one bucket per Dask worker.
      3. Register all files on the coordinator's IndexDatabase (pre-assigns
         file_ids and writes DEFAULT-CF entries once).
      4. Submit one Dask task per non-empty worker that runs the existing
         indexer pipeline with an SST sink, writing SSTs to `local_staging`
         and (if different) moving them to `shared_staging`.
      5. Collect artifact dicts into an SstArtifactRegistry; coordinator
         calls bulk_ingest + rebuild_root_summaries.

    Args:
        directory: Directory containing trace files.
        files: Explicit file list (alternative to directory).
        index_path: Target .dftindex path (coordinator-writable).
        local_staging: Per-worker SST build dir. If equal to shared_staging,
            no post-build move.
        shared_staging: Shared FS dir the coordinator reads SSTs from during
            ingest. Must be on the same filesystem as index_path for the
            cheapest ingest.
        client: Dask distributed Client. None -> run tasks inline.
        partition: "lpt" (greedy longest-processing-time bin-pack) or
            "round_robin".
        rebuild_root_summaries: If True, recompute ROOT_* CFs after ingest.
        parallelism_per_worker: 0 -> let the plugin/default Runtime choose
            (one coroutine thread per core).
        flush_every_files: 0 -> build SSTs once per worker; >0 -> flush
            mid-batch to bound peak memory.

    Returns:
        dict with total_files, per_worker sizes, index_path, artifact_count.
    """
    if dask is None:
        raise ImportError("dask is required for distributed_index")
    if not index_path:
        raise ValueError("index_path is required")
    if not local_staging:
        raise ValueError("local_staging is required")
    if not shared_staging:
        shared_staging = local_staging

    import logging as _logging
    import time as _time

    from .dftracer_utils_ext import (
        IndexDatabase as _IndexDatabase,
    )
    from .dftracer_utils_ext import (
        SstArtifactRegistry as _SstArtifactRegistry,
    )
    from .dftracer_utils_ext import (
        enumerate_gzip_members as _enumerate_gzip_members,
    )
    from .dftracer_utils_ext import (
        plan_work_units as _plan_work_units,
    )
    from .dftracer_utils_ext import (
        scan_files as _scan_files,
    )

    _log = _logging.getLogger("dftracer.utils.dask.distributed_index")
    if not _log.handlers:
        _log.setLevel(_logging.INFO)

    # 1. Enumerate files + sizes.
    _t0 = _time.monotonic()
    if files is None:
        if not directory:
            raise ValueError("either directory or files is required")
        _log.info("distributed_index: scan_files(%s)", directory)
        entries = _scan_files(directory, [".pfw", ".pfw.gz"], True, None)
    else:
        _log.info("distributed_index: sizing %d pre-listed files", len(files))
        entries = [(p, os.path.getsize(p)) for p in files]
    _log.info("distributed_index: scanned %d files in %.1fs", len(entries), _time.monotonic() - _t0)

    if not entries:
        return {"total_files": 0, "per_worker": [], "index_path": index_path}

    n_workers = 1
    if client is not None:
        n_workers = len(client.nthreads()) or 1
    _log.info("distributed_index: %d workers visible", n_workers)

    all_paths = [p for (p, _) in entries]

    # 2. Register all files once on coordinator (one register_files call;
    #    file_ids are then parallel to `entries`).
    _t1 = _time.monotonic()
    _log.info("distributed_index: opening IndexDatabase at %s", index_path)
    db = _IndexDatabase(index_path)
    db.init_schema()
    all_file_ids = db.register_files(all_paths, build_manifest)
    _log.info(
        "distributed_index: register_files done (%d files, %.1fs)",
        len(all_paths),
        _time.monotonic() - _t1,
    )

    # 3. SCAN: distribute gzip-member scan across workers (round-robin
    #    per file_idx). Each worker sends back only its 1/N member maps;
    #    coordinator stitches into the full map.
    _t2 = _time.monotonic()
    member_map: List[List[tuple]] = [[] for _ in range(len(entries))]
    if client is None:
        member_map = list(_enumerate_gzip_members(all_paths, None))
    else:
        worker_addrs = list(client.nthreads().keys())
        scan_buckets: List[List[int]] = [[] for _ in range(n_workers)]
        for i in range(len(all_paths)):
            scan_buckets[i % n_workers].append(i)
        scan_futs = []
        scan_idx_lists: List[List[int]] = []
        for w, idxs in enumerate(scan_buckets):
            if not idxs:
                continue
            sub_paths = [all_paths[i] for i in idxs]
            target = [worker_addrs[w % len(worker_addrs)]] if worker_addrs else None
            scan_idx_lists.append(idxs)
            scan_futs.append(
                client.submit(_scan_gzip_members_task, sub_paths, workers=target, pure=False)
            )
        scan_results = client.gather(scan_futs)
        for idxs, res in zip(scan_idx_lists, scan_results):
            for i, members in zip(idxs, res):
                member_map[i] = list(members)
    _log.info(
        "distributed_index: gzip-member scan done in %.1fs",
        _time.monotonic() - _t2,
    )

    # 4. PLAN: deterministic LPT of work units across workers (mirrors MPI).
    _t3 = _time.monotonic()
    if partition == "lpt":
        per_worker_units = _plan_work_units(member_map, n_workers, 0)
    elif partition == "round_robin":
        # Whole-file fallback for round_robin (no intra-file slicing).
        per_worker_units = [[] for _ in range(n_workers)]
        for i, mv in enumerate(member_map):
            mlen = max(1, len(mv))
            per_worker_units[i % n_workers].append((i, 0, mlen, 0))
    else:
        raise ValueError(f"unknown partition={partition}")
    _log.info(
        "distributed_index: planned in %.2fs (per-worker units=%s)",
        _time.monotonic() - _t3,
        [len(u) for u in per_worker_units],
    )

    # 5. BUILD: each worker receives its (paths, file_ids, file_slices)
    #    parallel lists. A file split across workers appears once per slice.
    index_dir = os.path.dirname(index_path.rstrip("/"))
    os.makedirs(local_staging, exist_ok=True)
    os.makedirs(shared_staging, exist_ok=True)

    worker_file_lists: List[List[str]] = []
    worker_file_ids: List[List[int]] = []
    worker_slices: List[List[Any]] = []
    CKPT_STRIDE = 1 << 20
    for w, units in enumerate(per_worker_units):
        paths_w: List[str] = []
        ids_w: List[int] = []
        slices_w: List[Any] = []
        for file_idx, mb, me, _csz in units:
            paths_w.append(all_paths[file_idx])
            ids_w.append(int(all_file_ids[file_idx]))
            members = member_map[file_idx] or [(0, 0)]
            # Clamp [mb, me) into the actual member vector. plan_work_units
            # may have synthesised a single (0, 0) for a non-gzip file; in
            # that case mb=0, me=1 and the slice is "whole file".
            if me > len(members):
                me = len(members)
            if mb > me:
                mb = me
            slices_w.append(
                (
                    int(mb),
                    int(me),
                    int(mb) * CKPT_STRIDE,
                    bool(mb != 0),
                    [(int(mo), int(ms)) for (mo, ms) in members],
                )
            )
        worker_file_lists.append(paths_w)
        worker_file_ids.append(ids_w)
        worker_slices.append(slices_w)

    _t_build = _time.monotonic()
    worker_ids: List[int] = []
    # Each entry is (artifact_dicts, tracker_blob) returned by _build_sst_task.
    worker_results: List[Any] = []
    if client is None:
        for w, (paths_w, ids_w, slices_w) in enumerate(
            zip(worker_file_lists, worker_file_ids, worker_slices)
        ):
            if not paths_w:
                continue
            worker_ids.append(w)
            worker_results.append(
                _build_sst_task(
                    paths_w,
                    ids_w,
                    slices_w,
                    local_staging,
                    shared_staging,
                    f"worker_{w}",
                    index_dir,
                    checkpoint_size,
                    bloom_dimensions,
                    build_manifest,
                    force_rebuild,
                    parallelism_per_worker,
                    flush_every_files,
                    aggregation_config,
                    False,
                )
            )
    else:
        worker_addrs = list(client.nthreads().keys())
        futures = []
        for w, (paths_w, ids_w, slices_w) in enumerate(
            zip(worker_file_lists, worker_file_ids, worker_slices)
        ):
            if not paths_w:
                continue
            target = [worker_addrs[w % len(worker_addrs)]] if worker_addrs else None
            worker_ids.append(w)
            futures.append(
                client.submit(
                    _build_sst_task,
                    paths_w,
                    ids_w,
                    slices_w,
                    local_staging,
                    shared_staging,
                    f"worker_{w}",
                    index_dir,
                    checkpoint_size,
                    bloom_dimensions,
                    build_manifest,
                    force_rebuild,
                    parallelism_per_worker,
                    flush_every_files,
                    aggregation_config,
                    True,
                    workers=target,
                    pure=False,
                )
            )
        worker_results = client.gather(futures)
    _log.info(
        "distributed_index: build dispatch+gather done in %.1fs (%d workers)",
        _time.monotonic() - _t_build,
        len(worker_ids),
    )

    # 5. Bulk-ingest on coordinator (all CFs).
    _t_collect = _time.monotonic()
    registry = _SstArtifactRegistry()
    total_artifacts = 0
    tracker_blobs: List[bytes] = []
    has_aggregation = False
    for wres in worker_results:
        if isinstance(wres, tuple) and len(wres) == 2:
            dicts, tracker_blob = wres
        else:
            dicts, tracker_blob = wres, b""
        if tracker_blob:
            tracker_blobs.append(tracker_blob)
        for d in dicts:
            registry.append(d)
            total_artifacts += 1
            if isinstance(d, dict) and (d.get("aggregation_sst") or d.get("system_metrics_sst")):
                has_aggregation = True
    _log.info(
        "distributed_index: collected %d artifacts in %.2fs",
        total_artifacts,
        _time.monotonic() - _t_collect,
    )

    _t_ingest = _time.monotonic()
    db.bulk_ingest(registry)
    _log.info(
        "distributed_index: bulk_ingest done in %.1fs (%d artifacts)",
        _time.monotonic() - _t_ingest,
        total_artifacts,
    )
    if rebuild_root_summaries:
        _t_root = _time.monotonic()
        db.rebuild_root_summaries()
        _log.info(
            "distributed_index: rebuild_root_summaries done in %.1fs",
            _time.monotonic() - _t_root,
        )

    if aggregation_config is not None and has_aggregation:
        _t_meta = _time.monotonic()
        time_interval_ms = getattr(aggregation_config, "time_interval_ms", 0) or 0
        time_interval_us = int(round(time_interval_ms * 1000.0))
        db.write_agg_global_config(time_interval_us=time_interval_us)
        if all_file_ids:
            db.write_agg_file_markers(list(all_file_ids))
        if tracker_blobs:
            db.write_aggregation_tracker(tracker_blobs)
        _log.info(
            "distributed_index: agg meta writes done in %.2fs (markers=%d, trackers=%d)",
            _time.monotonic() - _t_meta,
            len(all_file_ids),
            len(tracker_blobs),
        )

    per_worker_file_counts = [len(set(ids)) for ids in worker_file_ids]
    return {
        "total_files": len(entries),
        "per_worker": per_worker_file_counts,
        "index_path": index_path,
        "artifact_batches": total_artifacts,
    }
