# SPDX-License-Identifier: MIT
"""A distributed, arrow-native trace viewer over a Dask cluster."""

from __future__ import annotations

import os
from collections import namedtuple
from dataclasses import dataclass, replace
from importlib.util import find_spec
from typing import (
    TYPE_CHECKING,
    Dict,
    List,
    Literal,
    Optional,
    Sequence,
    Tuple,
    Union,
)

if TYPE_CHECKING:
    import pyarrow as pa  # ty: ignore[import-not-found]
    from dask.distributed import Client  # ty: ignore[import-not-found]
    from distributed import Future  # ty: ignore[import-not-found]


from ..dataframe import DataFrame, PhaseArg, TimeUnitArg
from ..dftracer_utils_ext import NUM_SHARDS
from ..enums import coerce_bytes, coerce_duration
from ..trace.viewer import TraceViewer
from .progress import worker_progress_forwarder

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
    resolution: Optional[int] = None
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


def _apply_plan(tv: TraceViewer, plan: _Plan) -> TraceViewer:
    """Apply a DaskTraceViewer plan to a per-shard TraceViewer. Trace builders
    may only follow filters, so they and group_by/agg go before select, offset
    and limit whatever order the caller built the plan in."""
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
    if plan.resolution is not None:
        tv = tv.resolution(plan.resolution)
    if plan.rollup_root:
        tv = tv.rollup_root(plan.rollup_root)
    if plan.views_root:
        tv = tv.views_root(plan.views_root)
    if plan.group_by:
        tv = tv.group_by(*plan.group_by)
    if plan.agg:
        tv = tv.agg(*plan.agg)
    if plan.auto_numeric:
        tv = tv.agg_numeric_args()
    if plan.select:
        tv = tv.select(*plan.select)
    if plan.memory_budget is not None:
        tv = tv.memory_budget(plan.memory_budget)
    elif plan.auto_spill:
        tv = tv.auto_spill()
    if plan.offset is not None:
        tv = tv.offset(plan.offset)
    if plan.limit is not None:
        tv = tv.head(plan.limit)
    return tv


def _make_viewer(files: List[str], index_dir: str, plan: _Plan) -> TraceViewer:
    return _apply_plan(TraceViewer(files, index_dir=index_dir or None), plan)


def _dv_partial_task(files: List[str], index_dir: str, plan: _Plan) -> bytes:
    """Worker: combinable aggregation partial for this shard (bytes)."""
    return _make_viewer(files, index_dir, plan).aggregate_partial().collect()


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
    return (
        _make_viewer(files, index_dir, plan)
        .flamegraph_partial(list(partition), ts, dur, name, list(group))
        .collect()
    )


def _dv_events_task(
    files: List[str],
    index_dir: str,
    plan: _Plan,
    cursor: Optional[int],
    page_size: int,
) -> "Optional[pa.Table]":
    """Worker: this shard's matching events at ts >= cursor, up to page_size."""
    if cursor is not None:
        # The page cursor is a lower bound on ts; chunk ts-stats still prune.
        plan = replace(plan, filters=plan.filters + (f"ts >= {int(cursor)}",))
    # Order by ts before the limit so each shard yields its smallest-ts page;
    # the ts cursor would otherwise skip the events a shard dropped.
    df = _make_viewer(files, index_dir, plan).sort_by("ts").head(page_size).collect()
    return df.to_arrow() if df.height else None


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
    """Worker: materialize this shard's filtered events into a rank subdir of
    the view's parts dir, indexed in the view root that holds them all."""
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
    from ..dfanalyzer.frames import typed_read_to_ipc

    if not shard_end or shard_end <= 0:
        shard_end = NUM_SHARDS
    # Per-shard progress from the C++ scan, tagged with the "Reading traces"
    # phase so the coordinator's ProgressAggregator can drive the read bar.
    forward = worker_progress_forwarder("Reading traces", f"{shard_begin}_{shard_end}")
    return typed_read_to_ipc(
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


def _slice_rows(df: DataFrame, plan: _Plan) -> DataFrame:
    if plan.offset is not None:
        df = df.slice(plan.offset, max(0, df.height - plan.offset))
    if plan.limit is not None:
        df = df.head(plan.limit)
    return df


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
        if find_spec("dask") is None:
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

    def duql(self, dsl: str) -> "DaskTraceViewer":
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

    def resolution(self, cell: Union[int, float, str]) -> "DaskTraceViewer":
        """The occupancy grid, as TraceViewer.resolution; a bare number is
        microseconds."""
        us = int(round(coerce_duration(cell, 1e6, "resolution")))
        return self._clone(replace(self._plan, resolution=us))

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

    def head(self, n: int) -> "DaskTraceViewer":
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
        if self._client is not None:
            return self._client
        from dask.distributed import get_client

        return get_client()

    def collect(self):
        """Distributed group_by+agg -> one native DataFrame.

        Each file shard returns a combinable partial (aggregate_partial),
        merged on the client via merge_partials so mean/std/percentiles are
        correct. offset/limit apply to the merged rows.
        """
        client = self._resolve_client()
        partials = self._gather_partials(client)
        return _slice_rows(self._coordinator().merge_partials(partials), self._plan)

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
        pyarrow Tables, fanning disjoint group ranges (hash shards) of the partials across
        workers (the index-wide tier can't be file-sharded)."""
        import pyarrow as pa

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

    def _coordinator(self) -> TraceViewer:
        # Single-node viewer over the same files and aggregation, without
        # offset/limit; derives the same rollup dir. merge/reconstruct/
        # materialize_partials never re-scan.
        plan = replace(self._plan, offset=None, limit=None)
        return _make_viewer(self._files, self._index_dir, plan)

    def _gather_partials(self, client: "Client") -> List[bytes]:
        """Fan the aggregation across shards, gather combinable partials."""
        plan = replace(self._plan, offset=None, limit=None)
        futures = [
            client.submit(_dv_partial_task, s, self._index_dir, plan, pure=False)
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
                os.path.join(mv_dir, "parts", f"shard-{n}"),
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
        out = self._coordinator().reconstruct_if_cached()
        return None if out is None else _slice_rows(out, self._plan)


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
