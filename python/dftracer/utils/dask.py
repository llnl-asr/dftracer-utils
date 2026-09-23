"""Dask distributed integration for dftracer-utils."""

import os
import shutil
from collections import defaultdict, namedtuple
from dataclasses import dataclass, replace
from typing import (
    TYPE_CHECKING,
    Callable,
    Dict,
    List,
    Literal,
    Optional,
    Sequence,
    Set,
    Tuple,
    TypedDict,
    Union,
)

# dask/distributed/pyarrow have no type stubs ty can resolve; TYPE_CHECKING
# imports name the true types for annotations while the runtime `else` branch
# provides real values (or None fallbacks for the optional deps).
if TYPE_CHECKING:
    import pyarrow as pa  # ty: ignore[unresolved-import]
    from dask.distributed import Client, WorkerPlugin, get_client  # ty: ignore[unresolved-import]
    from distributed import Future, Worker  # ty: ignore[unresolved-import]

    from .indexer import AggregationConfig
else:
    try:
        import pyarrow as pa
    except ImportError:
        pa = None

    try:
        from dask.distributed import Client, WorkerPlugin, get_client
    except ImportError:
        Client = None
        WorkerPlugin = None
        get_client = None

try:
    import dask
    import dask.dataframe as dd
except ImportError:
    dask = None  # type: ignore[assignment]  # ty: ignore[invalid-assignment]
    dd = None  # type: ignore[assignment]  # ty: ignore[invalid-assignment]

from dftracer.utils import (
    Runtime,
    peek_default_runtime,
    set_default_runtime,
)
from dftracer.utils._units import coerce_bytes, coerce_duration
from dftracer.utils.dataframe import (
    AggregatedTraceViewer,
    PhaseArg,
    TimeUnitArg,
    TraceViewer,
)

# A per-shard viewer is either the plain or the aggregated wrapper (group_by/agg
# promotes one to the other); the two are sibling wrappers, not a subtype pair.
_AnyViewer = Union[TraceViewer, AggregatedTraceViewer]
# One intra-file work slice: (member_begin, member_end, is_partial, members),
# members being (offset, size) pairs. Matches build_sst_batch's file_slices.
_FileSlice = Tuple[int, int, bool, List[Tuple[int, int]]]

__all__ = [
    "DaskTraceViewer",
    "DaskAggregatedTraceViewer",
    "ProgressAggregator",
    "register_auto_thread_plugin",
    "assign_files_by_pid",
]


if WorkerPlugin is not None:

    class DFTracerUtilsDaskWorkerPlugin(WorkerPlugin):  # ty: ignore[unsupported-base]
        """Creates a persistent Runtime per Dask worker."""

        def __init__(self, threads=0, io_threads=0):
            self.threads = threads
            self.io_threads = io_threads

        def setup(self, worker):
            # peek (do not create): forcing get_default_runtime here would
            # spin up an unused hardware_concurrency-thread runtime per worker.
            worker._dftracer_prev_runtime = peek_default_runtime()
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

else:
    DFTracerUtilsDaskWorkerPlugin = None  # type: ignore[assignment]


_plugin_registered_schedulers: set = set()

# All worker-side progress (parse, scan, ...) is published to one topic with a
# `phase` label; the coordinator aggregates per (phase, batch).
_PROGRESS_TOPIC = "dft-progress"


class _ProgressMsg(TypedDict):
    """The per-event payload published to _PROGRESS_TOPIC."""

    phase: str
    batch: str
    done: int
    total: int


class DistributedIndexResult(TypedDict):
    """Return shape of :func:`distributed_index`."""

    total_files: int
    per_worker: List[int]
    index_path: str
    artifact_batches: int


def _worker_progress_forwarder(phase: str, batch: str) -> Callable[[int, int], None]:
    """Callback that publishes (done, total) progress for `phase`/`batch` to the
    coordinator. Runs on a Dask worker; no-op off-worker (inline mode)."""
    try:
        from distributed import get_worker

        worker = get_worker()
    except (ImportError, ValueError):
        worker = None

    def _cb(done: int, total: int) -> None:
        if worker is None:
            return
        msg: _ProgressMsg = {
            "phase": phase,
            "batch": batch,
            "done": int(done),
            "total": int(total),
        }
        # Called from a C++ worker thread; hop to the IOLoop so the event is
        # sent on the worker's own loop rather than a foreign thread.
        try:
            worker.loop.add_callback(worker.log_event, _PROGRESS_TOPIC, msg)
        except Exception:
            try:
                worker.log_event(_PROGRESS_TOPIC, msg)
            except Exception:
                pass

    return _cb


class ProgressAggregator:
    """Subscribe to the progress topic and forward aggregated
    (done, total, phase) to `callback`, summing per (phase, batch)."""

    def __init__(
        self, client: "Client", callback: Optional[Callable[[int, int, str], None]]
    ) -> None:
        self._client = client
        self._callback = callback
        self._state: Dict[Tuple[str, str], Tuple[int, int]] = {}

    # event is a distributed topic payload: (timestamp, msg).
    def _handler(self, event: "Tuple[float, _ProgressMsg]") -> None:
        if self._callback is None:
            return
        try:
            _, msg = event
            phase = msg["phase"]
            self._state[(phase, msg["batch"])] = (int(msg["done"]), int(msg["total"]))
            done = sum(d for (p, _), (d, _t) in self._state.items() if p == phase)
            total = sum(t for (p, _), (_d, t) in self._state.items() if p == phase)
            self._callback(done, total, phase)
        except Exception:
            pass

    def __enter__(self) -> "ProgressAggregator":
        # NullClient (in-process) has no pub/sub topic; skip quietly.
        if self._callback is not None and hasattr(self._client, "subscribe_topic"):
            self._client.subscribe_topic(_PROGRESS_TOPIC, self._handler)
        return self

    def __exit__(self, *exc):
        if self._callback is not None and hasattr(self._client, "unsubscribe_topic"):
            try:
                self._client.unsubscribe_topic(_PROGRESS_TOPIC)
            except Exception:
                pass
        return False


def resolve_local_staging(client: "Client") -> str:
    """Derive node-local SST scratch from each Dask worker's own scratch.

    Workers share the path *string* (e.g. ``/scratch/$USER``) but each resolves
    it to its own node-local storage; falls back to ``/tmp`` when nothing is
    reported.
    """
    workers = client.scheduler_info().get("workers", {}) or {}
    if workers:
        worker_local_dir = next(iter(workers.values())).get("local_directory") or "/tmp"
    else:
        worker_local_dir = "/tmp"
    return os.path.join(worker_local_dir, "dftracer-sst-staging")


def _rmtree_quiet(path: str) -> None:
    """Best-effort recursive remove; submitted to workers for staging cleanup."""
    shutil.rmtree(path, ignore_errors=True)


def _runtime_threads(worker: "Worker", host_worker_counts: Dict[str, int], total_cpus: int) -> int:
    """C++ Runtime thread count for one Dask worker.

    Dask already divided the node between its workers and the Runtime is shared
    by every task on this worker, so the worker's own thread count is the share
    to match. `host_worker_counts` is a client-side snapshot and only a
    fallback: a worker that started later, or whose address spells the host
    differently, is missing from it, and assuming it is alone on the node
    oversubscribes by however many workers the node really has.
    """
    own = getattr(worker, "nthreads", None) or getattr(
        getattr(worker, "state", None), "nthreads", None
    )
    if own:
        return max(1, min(int(own), total_cpus))
    host = worker.address.split("://")[-1].rsplit(":", 1)[0]
    n_local = host_worker_counts.get(host) or max(host_worker_counts.values(), default=1)
    return max(1, total_cpus // n_local)


def register_auto_thread_plugin() -> None:
    """Register the DFTracer worker plugin on the active distributed client.

    Sizes each worker's C++ Runtime, compute and I/O threads alike, to the
    worker's own Dask thread count, so several workers on one node do not each
    claim every core.

    Idempotent: re-registering the same plugin on the same scheduler triggers a
    teardown+setup round-trip on every worker, which deadlocks if the previous
    Runtime still has in-flight coroutines. Skips if already registered for the
    scheduler address. A no-op when no distributed client is active.
    """
    if DFTracerUtilsDaskWorkerPlugin is None:
        return
    try:
        import logging
        import time
        from collections import Counter

        client = get_client()  # pyright: ignore[reportOptionalCall]  # ty: ignore[call-non-callable]
        sched_addr = getattr(client.scheduler, "address", None) or ""
        if sched_addr in _plugin_registered_schedulers:
            return

        def _addr_to_host(addr: str) -> str:
            return addr.split("://")[-1].rsplit(":", 1)[0]

        nthreads = client.nthreads()
        for _ in range(10):
            nthreads_next = client.nthreads()
            if len(nthreads_next) >= len(nthreads):
                nthreads = nthreads_next
            if len(nthreads) > 0:
                break
            time.sleep(0.5)
        host_counts = Counter(_addr_to_host(a) for a in nthreads.keys())

        logging.getLogger("dftracer.dask_plugin").info(
            "coord register_plugin: host_counts=%s total_workers=%d worker_addr_sample=%s",
            dict(host_counts),
            sum(host_counts.values()),
            list(nthreads.keys())[:8],
        )

        class _AutoThreadPlugin(DFTracerUtilsDaskWorkerPlugin):
            def __init__(self, host_worker_counts):
                super().__init__(threads=0)
                self._host_worker_counts = host_worker_counts

            def setup(self, worker):
                affinity = getattr(os, "sched_getaffinity", None)  # Linux only
                total_cpus = len(affinity(0)) if affinity else (os.cpu_count() or 1)
                my_host = worker.address.split("://")[-1].rsplit(":", 1)[0]
                self.threads = _runtime_threads(worker, self._host_worker_counts, total_cpus)
                # The I/O pool needs the same treatment: left at 0 it defaults
                # to one thread per core in every worker on the node.
                self.io_threads = self.threads
                logging.getLogger("distributed.worker").info(
                    "DFTracer Runtime: host=%s cpus=%d worker_nthreads=%s "
                    "cpp_threads=%d io_threads=%d",
                    my_host,
                    total_cpus,
                    getattr(worker, "nthreads", None),
                    self.threads,
                    self.io_threads,
                )
                super().setup(worker)

        client.register_plugin(_AutoThreadPlugin(dict(host_counts)))
        _plugin_registered_schedulers.add(sched_addr)
        logging.getLogger("dftracer.dask_plugin").info(
            "Registered DFTracerUtilsDaskWorkerPlugin host_worker_counts=%s total_workers=%d",
            dict(host_counts),
            sum(host_counts.values()),
        )
    except (ValueError, ImportError):
        pass


QueryPage = namedtuple("QueryPage", ["table", "next_cursor"])


@dataclass(frozen=True)
class _Plan:
    """Typed, immutable DaskTraceViewer plan. Builder ops evolve it with
    dataclasses.replace, so field names are checked, not string dict keys."""

    filters: Tuple[str, ...] = ()
    phase: Optional[PhaseArg] = None
    time_range: Optional[Tuple[float, float]] = None
    time_unit: Optional[TimeUnitArg] = None
    time_scale: Optional[float] = None
    time_bucket: Optional[int] = None
    time_bucket_normalize: Union[int, Literal["min"], None] = None
    occ_cell: Optional[int] = None
    group_by: Tuple[str, ...] = ()
    agg: Tuple[str, ...] = ()
    select: Tuple[str, ...] = ()
    auto_numeric: bool = False
    memory_budget: Optional[int] = None
    auto_spill: bool = False
    limit: Optional[int] = None
    offset: Optional[int] = None
    rollup_root: Optional[str] = None
    views_root: Optional[str] = None


def _apply_plan(tv: "_AnyViewer", plan: _Plan) -> "_AnyViewer":
    """Apply a DaskTraceViewer plan to a per-shard TraceViewer. group_by/agg
    return the AggregatedTraceViewer subclass, still a TraceViewer."""
    for dsl in plan.filters:
        tv = tv.filter(dsl)
    if plan.phase:
        tv = tv.phase(plan.phase)
    if plan.time_range:
        tv = tv.time_range(float(plan.time_range[0]), float(plan.time_range[1]))
    if plan.time_unit:
        tv = tv.time_unit(plan.time_unit)
    if plan.time_scale is not None:
        tv = tv.time_scale(plan.time_scale)
    if plan.time_bucket is not None:
        tv = tv.time_bucket(plan.time_bucket, plan.time_bucket_normalize)
    if plan.occ_cell is not None:
        tv = tv.occ_cell(plan.occ_cell)
    if plan.group_by:
        tv = tv.group_by(*plan.group_by)
    if plan.agg:
        tv = tv.agg(*plan.agg)
    if plan.select:
        tv = tv.select(*plan.select)
    if plan.auto_numeric:
        tv = tv.agg_numeric_args()
    if plan.memory_budget is not None:
        tv = tv.memory_budget(plan.memory_budget)
    elif plan.auto_spill:
        tv = tv.auto_spill()
    if plan.limit is not None:
        tv = tv.limit(plan.limit)
    if plan.offset is not None:
        tv = tv.offset(plan.offset)
    if plan.rollup_root:
        tv = tv.rollup_root(plan.rollup_root)
    if plan.views_root:
        tv = tv.views_root(plan.views_root)
    return tv


def _make_viewer(files: List[str], index_dir: str, plan: _Plan) -> "_AnyViewer":
    return _apply_plan(TraceViewer(files, index_path=index_dir or None), plan)


def _dv_partial_task(files: List[str], index_dir: str, plan: _Plan) -> bytes:
    """Worker: combinable aggregation partial for this shard (bytes)."""
    return _make_viewer(files, index_dir, plan).aggregate_partial()


def _dv_flamegraph_partial_task(
    files: List[str],
    index_dir: str,
    plan: _Plan,
    partition: Sequence[str],
    ts: str,
    dur: str,
    name: str,
    group: Sequence[str],
) -> bytes:
    """Worker: serialized flamegraph arena partial for this shard (bytes)."""
    return _make_viewer(files, index_dir, plan).flamegraph_partial(
        list(partition), ts, dur, name, list(group)
    )


def _dv_events_task(
    files: List[str],
    index_dir: str,
    plan: _Plan,
    cursor: Optional[int],
    page_size: int,
) -> "Optional[pa.Table]":
    """Worker: this shard's matching events at ts >= cursor, up to page_size."""
    tv = _make_viewer(files, index_dir, plan)
    if cursor is not None:
        # Per-event predicate (time_range only prunes chunks, so it would leak
        # earlier events sharing a boundary chunk); chunk ts-stats still prune.
        tv = tv.filter(f"ts >= {int(cursor)}")
    # Order by ts before the limit so each shard yields its smallest-ts page:
    # stream() fans out and is unordered, so a bare limit would take arbitrary
    # events and the ts cursor would skip the ones a shard dropped.
    tv = tv.sort_by("ts").limit(page_size)
    chunks = list(tv.stream())
    if not chunks:
        return None
    df = chunks[0] if len(chunks) == 1 else chunks[0].concat(*chunks[1:], how="diagonal")
    return df.to_arrow()


def _dv_write_task(
    files: List[str],
    index_dir: str,
    plan: _Plan,
    output_path: str,
    build_index: bool,
) -> Dict[str, str]:
    """Worker: export this shard to one re-indexable .pfw.gz."""
    _make_viewer(files, index_dir, plan).export_trace(output_path, compress=True, index=build_index)
    return {"path": output_path}


def _dv_materialize_shard_task(
    files: List[str],
    index_dir: str,
    plan: _Plan,
    subdir: str,
    checkpoint_size: int,
    part_size: int,
) -> str:
    """Worker: materialize this shard's filtered events into its own MV subdir
    (a self-contained part + index), for a distributed row-MV build."""
    os.makedirs(subdir, exist_ok=True)
    _make_viewer(files, index_dir, plan).export_trace(
        os.path.join(subdir, "part.pfw.gz"),
        compress=True,
        index=True,
        member_size=checkpoint_size,
        part_size=part_size,
    )
    return subdir


def _dv_collect_typed_task(
    files: List[str],
    index_dir: str,
    plan: _Plan,
    shard_begin: int,
    shard_end: int,
) -> "Dict[str, Optional[pa.Table]]":
    """Worker: one CF shard range of the typed read. The tier is index-wide, so
    every worker sees all files but a distinct shard range of the aggregation
    CF - returns {regular, aggregated, counters} pyarrow Tables.

    The C++ terminal yields native VecBatches, which are not picklable; import
    each into a pyarrow Table here so a process cluster can ship them."""
    import pyarrow as pa

    typed = _make_viewer(files, index_dir, plan).collect_typed(
        shard_begin=shard_begin, shard_end=shard_end
    )
    return {
        k: (pa.table(typed[k]) if typed.get(k) is not None else None)
        for k in ("regular", "aggregated", "counters")
    }


def _dv_typed_ipc_task(
    files: List[str],
    index_dir: str,
    time_granularity: float,
    time_resolution: float,
    query: Optional[str],
    shard_begin: int,
    shard_end: int,
    group_keys: Optional[Tuple[str, ...]] = None,
    drop_file_patterns: Tuple[str, ...] = (),
) -> Dict[str, Optional[bytes]]:
    """Worker: one CF shard range read + mapped to the dfanalyzer frame schema,
    returned as ``{events, profiles, system}`` Arrow IPC bytes. Time columns are
    absolute (origin 0); the HLM derives the global origin and rebuckets
    ``time_range``. The read + schema mapping lives in the dialect module so this
    stays the viewer's one distributed typed-read path."""
    from .dfanalyzer import _typed_read_to_ipc
    from .dftracer_utils_ext import NUM_SHARDS

    if not shard_end or shard_end <= 0:
        shard_end = NUM_SHARDS
    # Per-shard progress from the C++ scan, tagged with the "Reading traces"
    # phase so the coordinator's ProgressAggregator can drive the read bar.
    forward = _worker_progress_forwarder("Reading traces", f"{shard_begin}_{shard_end}")
    return _typed_read_to_ipc(
        files,
        index_dir,
        time_granularity,
        time_resolution,
        query,
        shard_begin=shard_begin,
        shard_end=shard_end,
        group_keys=group_keys,
        drop_file_patterns=drop_file_patterns,
        progress=forward,
    )


class DaskTraceViewer:
    """Distributed, arrow-native TraceViewer over a Dask cluster.

    Mirrors TraceViewer's lazy builder API (filter/phase/time_range/group_by/
    agg/select/agg_numeric_args), but each terminal fans file shards across
    Dask workers - each worker runs a TraceViewer over its shard on that
    worker's C++ runtime (already parallel intra-shard). Results are pyarrow;
    Dask ships them (buffer-shared for threaded workers, Arrow-serialized across
    processes). Single-node callers should use TraceViewer directly - Dask only
    distributes across workers/nodes.
    """

    def __init__(
        self,
        files: List[str],
        index_dir: str = "",
        *,
        client: "Optional[Client]" = None,
        files_per_task: int = 1,
        _plan: Optional[_Plan] = None,
    ) -> None:
        if dask is None:
            raise ImportError("dask is required for DaskTraceViewer")
        self._files = list(files)
        self._index_dir = index_dir
        self._client = client
        self._files_per_task = files_per_task
        self._plan: _Plan = _plan if _plan is not None else _Plan()

    def _clone(self, plan: _Plan) -> "DaskTraceViewer":
        # type(self) keeps a DaskAggregatedTraceViewer aggregated across a chain.
        return type(self)(
            self._files,
            self._index_dir,
            client=self._client,
            files_per_task=self._files_per_task,
            _plan=plan,
        )

    def _agg_clone(self, plan: _Plan) -> "DaskAggregatedTraceViewer":
        # group_by/agg promote a plain viewer to the aggregation type that alone
        # exposes the cache terminals (persist/reconstruct/collect(cache=...)).
        return DaskAggregatedTraceViewer(
            self._files,
            self._index_dir,
            client=self._client,
            files_per_task=self._files_per_task,
            _plan=plan,
        )

    def filter(self, dsl: str) -> "DaskTraceViewer":
        return self._clone(replace(self._plan, filters=self._plan.filters + (dsl,)))

    def query(self, dsl: str) -> "DaskTraceViewer":
        return self.filter(dsl)

    def phase(self, phase: PhaseArg) -> "DaskTraceViewer":
        return self._clone(replace(self._plan, phase=phase))

    def time_range(self, begin: float, end: float) -> "DaskTraceViewer":
        return self._clone(replace(self._plan, time_range=(begin, end)))

    def time_unit(self, unit: TimeUnitArg) -> "DaskTraceViewer":
        return self._clone(replace(self._plan, time_unit=unit))

    def time_scale(self, ns_ratio: float) -> "DaskTraceViewer":
        return self._clone(replace(self._plan, time_scale=float(ns_ratio)))

    def time_bucket(
        self,
        interval_us: Union[int, float, str],
        normalize_to: Union[int, Literal["min"], None] = None,
    ) -> "DaskTraceViewer":
        bucket = int(round(coerce_duration(interval_us, 1e6, "interval_us")))
        return self._clone(
            replace(self._plan, time_bucket=bucket, time_bucket_normalize=normalize_to)
        )

    def occ_cell(self, cell_us: Union[int, float, str]) -> "DaskTraceViewer":
        cell = int(round(coerce_duration(cell_us, 1e6, "cell_us")))
        return self._clone(replace(self._plan, occ_cell=cell))

    def group_by(self, *keys: str) -> "DaskAggregatedTraceViewer":
        return self._agg_clone(replace(self._plan, group_by=tuple(keys)))

    def agg(self, *specs: str) -> "DaskAggregatedTraceViewer":
        return self._agg_clone(replace(self._plan, agg=tuple(specs)))

    def select(self, *cols: str) -> "DaskTraceViewer":
        return self._clone(replace(self._plan, select=tuple(cols)))

    def memory_budget(self, nbytes: Union[int, str]) -> "DaskTraceViewer":
        return self._clone(replace(self._plan, memory_budget=coerce_bytes(nbytes, "nbytes")))

    def auto_spill(self) -> "DaskTraceViewer":
        return self._clone(replace(self._plan, auto_spill=True))

    def limit(self, n: int) -> "DaskTraceViewer":
        return self._clone(replace(self._plan, limit=int(n)))

    def offset(self, n: int) -> "DaskTraceViewer":
        return self._clone(replace(self._plan, offset=int(n)))

    def rollup_root(self, path: str) -> "DaskTraceViewer":
        """Override the aggregation-cache root (default: derive from index)."""
        return self._clone(replace(self._plan, rollup_root=path))

    def views_root(self, path: str) -> "DaskTraceViewer":
        """Override the materialized-view root (default: derive from index)."""
        return self._clone(replace(self._plan, views_root=path))

    def agg_numeric_args(self) -> "DaskAggregatedTraceViewer":
        return self._agg_clone(replace(self._plan, auto_numeric=True))

    def _shards(self) -> List[List[str]]:
        step = max(1, self._files_per_task)
        return [self._files[i : i + step] for i in range(0, len(self._files), step)]

    def _resolve_client(self) -> "Client":
        return self._client or get_client()  # pyright: ignore[reportOptionalCall]  # ty: ignore[call-non-callable]

    def collect(self):
        """Distributed group_by+agg -> one native DataFrame.

        Each file shard returns a combinable partial (aggregate_partial),
        merged on the client via merge_partials_to_table so mean/std/percentiles are
        correct.
        """
        client = self._resolve_client()
        partials = self._gather_partials(client)
        merger = _make_viewer(self._files, self._index_dir, self._plan)
        return merger.merge_partials_to_table(partials)

    def flamegraph(
        self,
        partition: Sequence[str] = ("pid", "tid"),
        ts: str = "ts",
        dur: str = "dur",
        name: str = "name",
        group: Sequence[str] = (),
    ):
        """Distributed flamegraph -> folded node DataFrame.

        Each file shard folds a serialized arena partial (flamegraph_partial);
        the client tree-reduces them via TraceViewer.merge_flamegraph_partials,
        so a name folds across shards. Partition by pid so a lane lives on one
        shard. ``group`` roots the tree by an arbitrary key over the raw events,
        as in TraceViewer.flamegraph. Composes with the branch's filter/phase/
        time like collect."""
        client = self._resolve_client()
        futures = [
            client.submit(
                _dv_flamegraph_partial_task,
                s,
                self._index_dir,
                self._plan,
                tuple(partition),
                ts,
                dur,
                name,
                tuple(group),
                pure=False,
            )
            for s in self._shards()
        ]
        partials = [p for p in client.gather(futures) if p]
        return TraceViewer.merge_flamegraph_partials(partials)

    def collect_typed(self):
        """Distributed one-pass typed read -> {regular, aggregated, counters}
        pyarrow Tables, fanning disjoint aggregation-CF shard ranges across
        workers (the index-wide tier can't be file-sharded)."""
        import pyarrow as pa

        from .dftracer_utils_ext import NUM_SHARDS

        client = self._resolve_client()
        n = max(1, len(client.scheduler_info().get("workers") or {}))
        span = (NUM_SHARDS + n - 1) // n
        futures = []
        for i in range(n):
            sb, se = min(NUM_SHARDS, i * span), min(NUM_SHARDS, (i + 1) * span)
            if sb >= se:
                continue
            futures.append(
                client.submit(
                    _dv_collect_typed_task,
                    self._files,
                    self._index_dir,
                    self._plan,
                    sb,
                    se,
                    pure=False,
                )
            )
        parts = client.gather(futures)

        def cat(key):
            # Workers already adopted the capsules into pyarrow Tables; permissive
            # promotion unifies system's dynamic per-metric columns across shards.
            tables = [p[key] for p in parts if p is not None and p.get(key) is not None]
            return pa.concat_tables(tables, promote_options="permissive") if tables else None

        return {k: cat(k) for k in ("regular", "aggregated", "counters")}

    def collect_typed_ipc_futures(
        self,
        time_granularity: float,
        time_resolution: float,
        group_keys: Optional[Tuple[str, ...]] = None,
        drop_file_patterns: Tuple[str, ...] = (),
    ) -> "Tuple[List[Future], List[Optional[str]]]":
        """Distributed typed read as per-worker IPC futures.

        Fans the CF shard space across the cluster (the index-wide tier can't be
        file-sharded); each worker produces ``{events, profiles, system}`` Arrow
        IPC bytes in the dfanalyzer schema for its shard range. Returns
        ``(futures, worker_addrs)`` - ungathered and worker-pinned - for a
        distributed reducer to consume. This is the View's one distributed
        typed-read path.
        """
        from .dftracer_utils_ext import NUM_SHARDS

        client = self._resolve_client()
        # nthreads() works for both a real Client and the in-process NullClient
        # (scheduler_info is Client-only), giving one addr per worker.
        workers = list((client.nthreads() or {}).keys())
        n = max(1, len(workers))
        span = (NUM_SHARDS + n - 1) // n
        query = " and ".join(self._plan.filters) if self._plan.filters else None
        futures, addrs = [], []
        for i in range(n):
            sb = min(NUM_SHARDS, i * span)
            se = min(NUM_SHARDS, (i + 1) * span)
            if sb >= se:
                continue
            addr = workers[i % len(workers)] if workers else None
            futures.append(
                client.submit(
                    _dv_typed_ipc_task,
                    self._files,
                    self._index_dir,
                    time_granularity,
                    time_resolution,
                    query,
                    sb,
                    se,
                    group_keys,
                    drop_file_patterns,
                    workers=[addr] if addr else None,
                    pure=False,
                )
            )
            addrs.append(addr)
        return futures, addrs

    def _gather_partials(self, client: "Client") -> List[bytes]:
        """Fan the aggregation across shards, gather combinable partials."""
        futures = [
            client.submit(_dv_partial_task, s, self._index_dir, self._plan, pure=False)
            for s in self._shards()
        ]
        return [p for p in client.gather(futures) if p]

    def page(self, page_size: int = 100_000, cursor: Optional[int] = None) -> QueryPage:
        """One page of matching events as a pyarrow Table, cursored by ts.

        `cursor` is a ts; the page holds the next `page_size` events at ts >=
        cursor across all shards, and `next_cursor` is where to resume (None at
        the end). Assumes page_size exceeds the events sharing any single ts.
        """
        import pyarrow as pa

        client = self._resolve_client()
        futures = [
            client.submit(
                _dv_events_task, s, self._index_dir, self._plan, cursor, page_size, pure=False
            )
            for s in self._shards()
        ]
        tables = [t for t in client.gather(futures) if t is not None and t.num_rows]
        if not tables:
            return QueryPage(None, None)
        combined = pa.concat_tables(tables).sort_by("ts")
        page = combined.slice(0, page_size)
        # A full page means some shard hit its per-shard limit and may hold
        # more, so advance the cursor; a short page has drained every shard.
        # (Do not compare combined vs page: one shard returning exactly
        # page_size makes them equal yet more rows remain.)
        next_cursor = None
        if page.num_rows >= page_size:
            next_cursor = int(page.column("ts")[-1].as_py()) + 1
        return QueryPage(page, next_cursor)

    def pages(self, page_size: int = 100_000):
        """Iterate all matching events page by page (cursored by ts)."""
        cursor = None
        while True:
            pg = self.page(page_size=page_size, cursor=cursor)
            if pg.table is None or pg.table.num_rows == 0:
                break
            yield pg.table
            if pg.next_cursor is None:
                break
            cursor = pg.next_cursor

    def export_trace(self, output_dir: str, *, build_index: bool = False) -> Dict[str, List[str]]:
        """Export per-shard re-indexable ``<output_dir>/part-<n>.pfw.gz`` files."""
        client = self._resolve_client()
        os.makedirs(output_dir, exist_ok=True)
        futures = [
            client.submit(
                _dv_write_task,
                s,
                self._index_dir,
                self._plan,
                os.path.join(output_dir, f"part-{n}.pfw.gz"),
                build_index,
                pure=False,
            )
            for n, s in enumerate(self._shards())
        ]
        results = client.gather(futures)
        return {"files": [r["path"] for r in results if r["path"]]}

    def materialize(
        self,
        *,
        checkpoint_size: Union[int, str] = 0,
        part_size: Union[int, str] = 0,
    ) -> "DaskTraceViewer":
        """Distributed row-MV materialize: each shard writes its filtered events
        into its own subdir of the shared MV directory (a self-contained part +
        index), then the coordinator writes one manifest over the full base set.
        A later matching read reuses it. Needs a views anchor - a shared index
        location or views_root(). checkpoint_size/part_size accept unit strings
        (e.g. "4MB")."""
        checkpoint_size = coerce_bytes(checkpoint_size, "checkpoint_size")
        part_size = coerce_bytes(part_size, "part_size")
        client = self._resolve_client()
        coord = _make_viewer(self._files, self._index_dir, self._plan)
        mv_dir = coord.materialize_dir()
        if not mv_dir:
            raise RuntimeError("no materialized-view anchor; set views_root(...)")
        futures = [
            client.submit(
                _dv_materialize_shard_task,
                s,
                self._index_dir,
                self._plan,
                os.path.join(mv_dir, f"shard-{n}"),
                checkpoint_size,
                part_size,
                pure=False,
            )
            for n, s in enumerate(self._shards())
        ]
        client.gather(futures)
        coord.register_materialized(mv_dir)
        return self


class DaskAggregatedTraceViewer(DaskTraceViewer):
    """A DaskTraceViewer with a group_by/agg set. Only this type exposes the
    rollup materialized view (materialize()/reconstruct_if_cached() and
    collect(cache=True)); a raw-event viewer cannot, mirroring the C++
    AggregatedView gate. The rollup dir is derived from the plan (files + index
    location + aggregation-grain signature), so callers never pass a path -
    rollup_root() overrides the root.
    """

    def _coordinator(self) -> AggregatedTraceViewer:
        # Single-node viewer over the same files/plan; derives the same rollup
        # dir. merge/reconstruct/materialize_partials never re-scan. The plan
        # always carries a group_by/agg here, so it is aggregated.
        v = _make_viewer(self._files, self._index_dir, self._plan)
        assert isinstance(v, AggregatedTraceViewer)
        return v

    def materialize(
        self,
        *,
        checkpoint_size: Union[int, str] = 0,
        part_size: Union[int, str] = 0,
    ) -> "DaskAggregatedTraceViewer":
        """Distributed materialize of the rollup: ingest the MV from per-shard
        partials, no re-scan. A later collect() reads it back."""
        # checkpoint_size/part_size are row-MV knobs; accepted only to match the
        # base signature.
        del checkpoint_size, part_size
        client = self._resolve_client()
        self._coordinator().materialize_partials(self._gather_partials(client))
        return self

    def reconstruct_if_cached(self):
        """The materialized aggregation as a native DataFrame, or None on a miss."""
        return self._coordinator().reconstruct_if_cached()


def distributed_write_trace(
    files: List[str],
    output_dir: str,
    *,
    index_dir: str = "",
    view: Optional[str] = None,
    files_per_task: int = 1,
    build_index: bool = False,
    client: "Optional[Client]" = None,
) -> Dict[str, List[str]]:
    """Thin wrapper over ``DaskTraceViewer(...).filter(view).export_trace(...)``."""
    dv = DaskTraceViewer(files, index_dir, client=client, files_per_task=files_per_task)
    if view:
        dv = dv.filter(view)
    return dv.export_trace(output_dir, build_index=build_index)


def assign_files_by_pid(
    file_pids: Dict[int, Set[int]],
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

    worker_assignments: Dict[int, List[int]] = defaultdict(list)

    for file_id, pids in file_pids.items():
        if not pids:
            worker_id = file_id % n_workers
        else:
            # Files with same PIDs go to same worker.
            majority_pid = min(pids)  # min for determinism
            worker_id = hash(majority_pid) % n_workers
        worker_assignments[worker_id].append(file_id)

    return dict(worker_assignments)


def _build_sst_task(
    files: List[str],
    file_ids: List[int],
    file_slices: Optional[List[Optional[_FileSlice]]],
    local_staging: str,
    shared_staging: str,
    batch_id: str,
    index_dir: str,
    checkpoint_size: int,
    bloom_dimensions: Optional[List[str]],
    force_rebuild: bool,
    parallelism: int,
    flush_every_files: int,
    build_bloom: bool = True,
    # Opaque native aggregation-config object, forwarded straight to build_sst_batch.
    aggregation_config: "Optional[Union[bool, AggregationConfig]]" = None,
    enable_det_ids: bool = False,
) -> Tuple[List[Dict[str, Optional[str]]], bytes]:
    """Dask worker task: build per-worker SSTs and relocate to shared FS.

    Returns ``(artifact_dicts, tracker_blob)``."""
    import logging as _logging
    import socket as _socket
    import time as _time

    from .dftracer_utils_ext import build_sst_batch, move_artifacts

    _log = _logging.getLogger("dftracer.utils.dask._build_sst_task")
    _host = _socket.gethostname()

    _progress_cb = _worker_progress_forwarder("Indexing", batch_id)

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
        force_rebuild,
        build_bloom,
        bloom_dimensions,
        parallelism,
        flush_every_files,
        None,
        aggregation_config,
        file_slices,
        progress=_progress_cb,
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


def _scan_gzip_members_task(paths: List[str]) -> List[List[Tuple[int, int]]]:
    """Worker task: scan gzip member offsets for its file subset."""
    from .dftracer_utils_ext import enumerate_gzip_members

    return enumerate_gzip_members(paths, None)


def distributed_index(
    directory: str = "",
    files: Optional[List[str]] = None,
    index_path: str = "",
    local_staging: str = "",
    shared_staging: str = "",
    client: "Optional[Client]" = None,
    checkpoint_size: Union[int, str] = 32 * 1024 * 1024,
    bloom_dimensions: Optional[List[str]] = None,
    force_rebuild: bool = False,
    build_bloom: bool = True,
    partition: Literal["lpt", "round_robin"] = "lpt",
    rebuild_root_summaries: bool = True,
    parallelism_per_worker: int = 0,
    flush_every_files: int = 0,
    aggregation_config: "Optional[Union[bool, AggregationConfig]]" = None,
    progress: Optional[Callable[[int, int, str], None]] = None,
) -> DistributedIndexResult:
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
    checkpoint_size = coerce_bytes(checkpoint_size, "checkpoint_size")
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
        return {
            "total_files": 0,
            "per_worker": [],
            "index_path": index_path,
            "artifact_batches": 0,
        }

    n_workers = 1
    if client is not None:
        n_workers = len(client.nthreads()) or 1
    _log.info("distributed_index: %d workers visible", n_workers)

    all_paths = [p for (p, _) in entries]
    n_total_files = len(all_paths)

    if not force_rebuild:
        from .indexer import Indexer

        with Indexer(
            files=all_paths,
            index_dir=index_path,
            require_checkpoint=True,
            require_bloom=True,
            require_aggregation=aggregation_config,
            force_rebuild=False,
        ) as _resolver:
            _status = _resolver.resolve()
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
                "index_path": index_path,
                "artifact_batches": 0,
            }
        if len(_needs) < len(entries):
            entries = [(p, s) for (p, s) in entries if p in _needs]
            all_paths = [p for (p, _) in entries]

    # 2. Register all files once on coordinator (one register_files call;
    #    file_ids are then parallel to `entries`).
    _t1 = _time.monotonic()
    _log.info("distributed_index: opening IndexDatabase at %s", index_path)
    db = _IndexDatabase(index_path)
    db.init_schema()
    all_file_ids = db.register_files(all_paths)
    _log.info(
        "distributed_index: register_files done (%d files, %.1fs)",
        len(all_paths),
        _time.monotonic() - _t1,
    )

    # 3. SCAN: distribute gzip-member scan across workers (round-robin
    #    per file_idx). Each worker sends back only its 1/N member maps;
    #    coordinator stitches into the full map.
    _t2 = _time.monotonic()
    member_map: List[List[Tuple[int, int]]] = [[] for _ in range(len(entries))]
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
    worker_slices: List[List[Optional[_FileSlice]]] = []
    for w, units in enumerate(per_worker_units):
        paths_w: List[str] = []
        ids_w: List[int] = []
        slices_w: List[Optional[_FileSlice]] = []
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
                    bool(mb != 0),
                    [(int(mo), int(ms)) for (mo, ms) in members],
                )
            )
        worker_file_lists.append(paths_w)
        worker_file_ids.append(ids_w)
        worker_slices.append(slices_w)

    _t_build = _time.monotonic()
    worker_ids: List[int] = []
    worker_addrs: List[str] = []
    # Each entry is (artifact_dicts, tracker_blob) returned by _build_sst_task.
    worker_results: List[Tuple[List[Dict[str, Optional[str]]], bytes]] = []
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
                    force_rebuild,
                    parallelism_per_worker,
                    flush_every_files,
                    build_bloom,
                    aggregation_config,
                    False,
                )
            )
    else:
        worker_addrs = list(client.nthreads().keys())
        with ProgressAggregator(client, progress):
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
                        force_rebuild,
                        parallelism_per_worker,
                        flush_every_files,
                        build_bloom,
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

    # bulk_ingest and rebuild_root_summaries are single coordinator calls with
    # no granular counter yet, so report them as indeterminate labelled phases.
    _t_ingest = _time.monotonic()
    if progress is not None:
        progress(0, 0, "Ingesting SSTs")
    db.bulk_ingest(registry)
    if progress is not None:
        progress(1, 1, "Ingesting SSTs")
    _log.info(
        "distributed_index: bulk_ingest done in %.1fs (%d artifacts)",
        _time.monotonic() - _t_ingest,
        total_artifacts,
    )
    if rebuild_root_summaries:
        _t_root = _time.monotonic()
        if progress is not None:
            progress(0, 0, "Building summaries")
        db.rebuild_root_summaries()
        if progress is not None:
            progress(1, 1, "Building summaries")
        _log.info(
            "distributed_index: rebuild_root_summaries done in %.1fs",
            _time.monotonic() - _t_root,
        )

    if aggregation_config is not None and has_aggregation:
        _t_meta = _time.monotonic()
        time_interval_ms = getattr(aggregation_config, "time_interval_ms", 0) or 0
        time_interval_us = int(round(time_interval_ms * 1000.0))
        db.write_agg_global_config(
            time_interval_us=time_interval_us,
            group_by_file=getattr(aggregation_config, "group_by_file", True),
        )
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

    stale = []
    for w in worker_ids:
        stale.append((w, os.path.join(local_staging, f"worker_{w}")))
        if shared_staging and shared_staging != local_staging:
            stale.append((w, os.path.join(shared_staging, f"worker_{w}")))
    if client is not None and worker_addrs:
        try:
            client.gather(
                [
                    client.submit(
                        _rmtree_quiet,
                        path,
                        workers=[worker_addrs[w % len(worker_addrs)]],
                        allow_other_workers=True,
                        pure=False,
                    )
                    for w, path in stale
                ]
            )
        except Exception:
            _log.warning("distributed_index: staging cleanup failed", exc_info=True)
    else:
        for _w, path in stale:
            _rmtree_quiet(path)

    per_worker_file_counts = [len(set(ids)) for ids in worker_file_ids]
    return {
        "total_files": n_total_files,
        "per_worker": per_worker_file_counts,
        "index_path": index_path,
        "artifact_batches": total_artifacts,
    }
