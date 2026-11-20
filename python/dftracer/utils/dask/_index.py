# SPDX-License-Identifier: MIT
"""Distributed index builds on a Dask cluster."""

from __future__ import annotations

import logging
import os
import socket
import time
import zlib
from collections import defaultdict
from importlib.util import find_spec
from typing import TYPE_CHECKING, Callable, List, Literal, Sequence, TypedDict

if TYPE_CHECKING:
    from dask.distributed import Client

from .._enums import coerce_bytes
from ..dftracer_utils_ext import (
    INPUT_FILE_PATTERNS,
    build_index_batch,
    consolidate_shard_set,
    enable_aggregation_deterministic_ids,
    write_shard_set,
)
from ..dftracer_utils_ext import Index as _Index
from ..dftracer_utils_ext import plan_lpt_partition as _plan_lpt_partition
from ..dftracer_utils_ext import scan_files as _scan_files
from ..index import AggregationConfig, Indexer
from ._progress import ProgressAggregator, worker_progress_forwarder
from ._workers import _require_workers


class DistributedIndexResult(TypedDict):
    """Return shape of :func:`distributed_index`."""

    total_files: int
    per_worker: List[int]
    index_dir: str


def assign_files_by_pid(
    file_pids: dict[str, set[int]],
    n_workers: int,
) -> dict[int, list[str]]:
    """Assign files to workers based on majority PID affinity.

    Files with overlapping PIDs are assigned to the same worker to minimize
    cross-worker aggregation during the merge phase. A file with no recorded
    PID goes to the worker its path hashes to, so separate processes agree on
    the assignment.

    Args:
        file_pids: Dict mapping trace path to set of PIDs in that file.
        n_workers: Number of workers to distribute to.

    Returns:
        Dict mapping worker_id to list of trace paths.
    """
    if n_workers <= 0:
        n_workers = 1

    worker_assignments: dict[int, list[str]] = defaultdict(list)

    for path, pids in file_pids.items():
        if not pids:
            # crc32, not hash(): a str hash is salted per process.
            worker_id = zlib.crc32(path.encode()) % n_workers
        else:
            # Files with same PIDs go to same worker.
            majority_pid = min(pids)  # min for determinism
            worker_id = hash(majority_pid) % n_workers
        worker_assignments[worker_id].append(path)

    return dict(worker_assignments)


def _build_index_task(
    files: list[str],
    batch_id: str,
    shard_root: str,
    checkpoint_size: int,
    bloom_dimensions: list[str] | None,
    force: bool,
    parallelism: int,
    build_bloom: bool = True,
    aggregation_config: AggregationConfig | None = None,
) -> None:
    """Dask worker task: index `files` as the shard at `shard_root`.

    The shard is a complete index root of its own. The coordinator catalogs the
    shards and merges them into the index root once every task finished.
    """
    _log = logging.getLogger("dftracer.utils.dask._build_index_task")
    _host = socket.gethostname()

    _progress_cb = worker_progress_forwarder("Indexing", batch_id)

    t0 = time.monotonic()
    # Every shard numbers its aggregation dictionary by content, so the shards
    # of one root agree on the ids their data files carry.
    enable_aggregation_deterministic_ids()

    build_index_batch(
        files,
        shard_root,
        checkpoint_size,
        force,
        build_bloom,
        bloom_dimensions,
        parallelism,
        None,
        aggregation_config,
        progress=_progress_cb,
    )
    _log.info(
        "build host=%s batch=%s n_files=%d total=%.2fs",
        _host,
        batch_id,
        len(files),
        time.monotonic() - t0,
    )


def distributed_index(
    files: str | os.PathLike[str] | Sequence[str | os.PathLike[str]],
    index_dir: str = "",
    client: Client | None = None,
    checkpoint_size: int | str = 32 * 1024 * 1024,
    bloom_dimensions: list[str] | None = None,
    force: bool = False,
    build_bloom: bool = True,
    partition: Literal["lpt", "round_robin"] = "lpt",
    parallelism_per_worker: int = 0,
    aggregation_config: AggregationConfig | None = None,
    progress: Callable[[int, int, str], None] | None = None,
) -> DistributedIndexResult:
    """Index a set of trace files using Dask workers that write in parallel.

    Steps (all O(1) on the coordinator except the fan-out):
      1. Enumerate files + sizes via parallel scan.
      2. LPT bin-pack whole files into one bucket per Dask worker. A file is
         never split across workers: every part of its index is built from the
         whole file, by one worker with its own threads.
      3. Submit one Dask task per non-empty worker. Each task builds its files
         as the shard `<index_dir>/shards/<worker>`, a complete index root of
         its own, on node-local scratch when the destination warrants it.
      4. The coordinator catalogs the arrived shards in `<index_dir>/shards.json`
         and merges them into `<index_dir>/store` by moving their data files,
         which leaves the manifest and the shard directories gone. A failed task
         fails the build and leaves the arrived shards in place, so a later call
         retries the merge without a rebuild.
      5. The coordinator writes the aggregation config and refreshes its view of
         the index.

    Args:
        files: A trace file, a directory scanned for trace files, or a list of
            file paths, as for :class:`~dftracer.utils.index.Indexer`.
        index_dir: Target .dftindex path (coordinator-writable).
        client: Dask distributed Client. None -> run tasks inline.
        partition: "lpt" (greedy longest-processing-time bin-pack) or
            "round_robin".
        parallelism_per_worker: 0 -> let the plugin/default Runtime choose
            (one coroutine thread per core).

    Returns:
        dict with total_files, per_worker sizes and index_dir.
    """
    if find_spec("dask") is None:
        raise ImportError("dask is required for distributed_index")
    checkpoint_size = coerce_bytes(checkpoint_size, "checkpoint_size")
    if not index_dir:
        raise ValueError("index_dir is required")

    agg_config = aggregation_config if aggregation_config is not None else AggregationConfig()
    _log = logging.getLogger("dftracer.utils.dask.distributed_index")
    if not _log.handlers:
        _log.setLevel(logging.INFO)

    # 1. Enumerate files + sizes.
    _t0 = time.monotonic()
    if isinstance(files, (str, os.PathLike)) and os.path.isdir(os.fspath(files)):
        directory = os.fspath(files)
        _log.info("distributed_index: scan_files(%s)", directory)
        entries = _scan_files(directory, list(INPUT_FILE_PATTERNS), True, None)
    else:
        listed = (
            [os.fspath(files)]
            if isinstance(files, (str, os.PathLike))
            else [os.fspath(f) for f in files]
        )
        if not listed:
            raise ValueError("files must name at least one path")
        _log.info("distributed_index: sizing %d pre-listed files", len(listed))
        entries = [(p, os.path.getsize(p)) for p in listed]
    _log.info("distributed_index: scanned %d files in %.1fs", len(entries), time.monotonic() - _t0)

    if not entries:
        return {
            "total_files": 0,
            "per_worker": [],
            "index_dir": index_dir,
        }

    n_workers = 1
    if client is not None:
        _require_workers(client)
        n_workers = len(client.nthreads())
    _log.info("distributed_index: %d workers visible", n_workers)

    all_paths = [p for (p, _) in entries]
    n_total_files = len(all_paths)

    if not force:
        with Indexer(
            files=all_paths,
            index_dir=index_dir,
            require_checkpoint=True,
            require_bloom=True,
            aggregation_config=agg_config,
        ) as _resolver:
            _status = _resolver.status()
        _needs = set(_status.needs_work)
        _log.info(
            "distributed_index: resolver reports %d/%d files need work",
            len(_needs),
            len(all_paths),
        )
        if not _needs:
            return {
                "total_files": n_total_files,
                "per_worker": [],
                "index_dir": index_dir,
            }
        if len(_needs) < len(entries):
            entries = [(p, s) for (p, s) in entries if p in _needs]
            all_paths = [p for (p, _) in entries]

    # A file is never split across workers, since each part of its index is
    # built from the whole file.
    _t3 = time.monotonic()
    if partition == "lpt":
        path_index = {p: i for i, p in enumerate(all_paths)}
        per_worker_files = [
            [path_index[p] for (p, _sz) in bucket]
            for bucket in _plan_lpt_partition(list(entries), n_workers)
        ]
    elif partition == "round_robin":
        per_worker_files = [[] for _ in range(n_workers)]
        for i in range(len(all_paths)):
            per_worker_files[i % n_workers].append(i)
    else:
        raise ValueError(f"unknown partition={partition}")
    _log.info(
        "distributed_index: planned in %.2fs (per-worker files=%s)",
        time.monotonic() - _t3,
        [len(u) for u in per_worker_files],
    )

    index_dir = index_dir.rstrip("/")
    worker_file_lists: list[list[str]] = [
        [all_paths[i] for i in files_w] for files_w in per_worker_files
    ]
    shard_dirs = [os.path.join(index_dir, "shards", str(w)) for w in range(len(worker_file_lists))]

    _t_build = time.monotonic()
    worker_ids: list[int] = []
    if client is None:
        for w, paths_w in enumerate(worker_file_lists):
            if not paths_w:
                continue
            worker_ids.append(w)
            _build_index_task(
                paths_w,
                f"worker_{w}",
                shard_dirs[w],
                checkpoint_size,
                bloom_dimensions,
                force,
                parallelism_per_worker,
                build_bloom,
                agg_config,
            )
    else:
        worker_addrs = list(client.nthreads().keys())
        with ProgressAggregator(client, progress):
            futures = []
            for w, paths_w in enumerate(worker_file_lists):
                if not paths_w:
                    continue
                target = [worker_addrs[w % len(worker_addrs)]] if worker_addrs else None
                worker_ids.append(w)
                futures.append(
                    client.submit(
                        _build_index_task,
                        paths_w,
                        f"worker_{w}",
                        shard_dirs[w],
                        checkpoint_size,
                        bloom_dimensions,
                        force,
                        parallelism_per_worker,
                        build_bloom,
                        agg_config,
                        workers=target,
                        pure=False,
                    )
                )
            client.gather(futures)
    _log.info(
        "distributed_index: build dispatch+gather done in %.1fs (%d workers)",
        time.monotonic() - _t_build,
        len(worker_ids),
    )

    built = bool(worker_ids)
    if built:
        _t_merge = time.monotonic()

        arrived = [shard_dirs[w] for w in worker_ids if os.path.isdir(shard_dirs[w])]
        write_shard_set(index_dir, arrived)
        consolidate_shard_set(index_dir)
        _log.info(
            "distributed_index: merged %d shards into %s in %.1fs",
            len(arrived),
            index_dir,
            time.monotonic() - _t_merge,
        )

        _t_meta = time.monotonic()
        db = _Index(index_dir)
        db.write_agg_config(agg_config)
        _log.info(
            "distributed_index: agg meta writes done in %.2fs",
            time.monotonic() - _t_meta,
        )

    per_worker_file_counts = [len(set(paths)) for paths in worker_file_lists]
    return {
        "total_files": n_total_files,
        "per_worker": per_worker_file_counts,
        "index_dir": index_dir,
    }
