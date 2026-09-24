"""``TraceViewer``: a :class:`~dftracer.utils.LazyFrame` over trace files.

A ``TraceViewer`` is a lazy plan whose source is a trace scan. Its trace
builders (``phase``, ``time_range``, ``time_bucket``, trace ``group_by`` /
``agg`` and the scan settings) shape that scan, and the ``LazyFrame`` ops it
overrides (``filter``, ``select``, ``with_column``, ``rename``, ``sort_by``,
``topk``, ``head`` / ``slice``) are absorbed into it, so they keep the
``TraceViewer`` type. Every other op returns a plain ``LazyFrame``. Nothing
runs until ``collect()``, which returns a :class:`~dftracer.utils.DataFrame`.

A trace builder may only follow filters: after any other op it raises and
names that op. A terminal (``flamegraph``, ``sink_json``, ...) likewise raises
naming the first op it cannot take.
"""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    Iterator,
    List,
    Literal,
    NamedTuple,
    Optional,
    Sequence,
    Union,
    overload,
)

from . import dftracer_utils_ext as _ext
from ._units import coerce_bytes, coerce_duration
from .columnar import Agg, Expr, _Col
from .dataframe import DataFrame, PhaseArg, TimeUnitArg
from .lazyframe import LazyFrame, LazyResult, collect_all
from .series import _wrap

if TYPE_CHECKING:
    from .plugins import Plugins
    from .runtime import Runtime

_OCCUPANCY = ("busy", "concurrency", "utilization", "active")
_WILDCARD_OPS = ("sum", "min", "max", "var", "std", "skew", "kurt")


class Containment(NamedTuple):
    """Both containment frames of one buffered fold."""

    call_tree: DataFrame
    flamegraph: DataFrame


def _agg_specs(specs: Sequence[Union[str, Agg]]) -> "tuple[List[str], List[str], Optional[int]]":
    """Trace agg spec strings, ``F.any`` reductions and the occupancy grid of
    ``specs``: spec strings pass through, a bare-field ``Agg`` becomes
    ``"op:field"``, and ``F.any.<op>()`` becomes a numeric-args reduction."""
    from .columnar import _Wildcard

    strings: List[str] = []
    wildcard: List[str] = []
    grids = set()
    for s in specs:
        if isinstance(s, str):
            strings.append(s)
            continue
        if not isinstance(s, Agg):
            raise TypeError(
                f"agg() expects a spec string or an Agg (F.dur.sum()), got {type(s).__name__}"
            )
        if isinstance(s.value, _Wildcard):
            if s.op == "count":
                strings.append("count")
            elif s.op == "mean" or s.op in _WILDCARD_OPS:
                wildcard.append(s.op)
            else:
                raise ValueError(
                    "F.any supports .count()/.mean()/.sum()/.min()/.max()/.var()/"
                    ".std()/.skew()/.kurt(); name a field for argmax or percentile"
                )
            continue
        if s.value is None:
            strings.append(s.op)
            continue
        if not isinstance(s.value, _Col):
            raise TypeError(
                "TraceViewer.agg() needs a bare-field aggregate like F.dur.sum(); "
                "aggregate a computed value with with_column() then group_by()"
            )
        spec = f"{s.op}:{s.value.name}"
        if s.op == "pct":
            spec += f":{s.param}"
        strings.append(spec)
        if s.op in _OCCUPANCY and s.param:
            grids.add(int(s.param))
    if len(grids) > 1:
        raise ValueError("occupancy aggregates in one plan share one resolution")
    return strings, wildcard, (grids.pop() if grids else None)


def _stats_row(frame: DataFrame) -> Dict[str, object]:
    row = {c: v[0] for c, v in frame.to_dict().items()} if frame.height else {}
    return {
        "duration_count": row.get("count", 0),
        "duration_mean_us": row.get("mean_dur", 0.0),
        "duration_stddev_us": row.get("std_dur", 0.0),
        "min_timestamp_us": row.get("min_ts", 0),
        "max_timestamp_us": row.get("max_ts", 0),
    }


def _export_stats(frame: DataFrame) -> Dict[str, object]:
    return {c: v[0] for c, v in frame.to_dict().items()}


def _partial(frame: DataFrame) -> bytes:
    value = frame.to_dict()["partial"][0]
    if not isinstance(value, (bytes, bytearray)):
        raise TypeError(f"a partial is bytes, got {type(value).__name__}")
    return bytes(value)


class TraceViewer(LazyFrame):
    """A lazy view over trace files (a file, a directory or a list of files);
    see the module docstring."""

    __slots__ = ("_tv",)

    @overload
    def __init__(
        self,
        files: Union[str, Sequence[str]],
        index_path: Optional[str] = ...,
        runtime: "Optional[Runtime]" = ...,
    ) -> None: ...
    @overload
    def __init__(self, files: "_ext._TraceViewer", /) -> None: ...

    def __init__(self, files: Any, index_path: Optional[str] = None, runtime: Any = None) -> None:
        native = (
            files if isinstance(files, _ext._TraceViewer) else _ext._TraceViewer(files, index_path)
        )
        super().__init__(native.lazy(), runtime)
        self._tv = native

    # -- constructing results ----------------------------------------------------
    def _trace(self, native: "_ext._TraceViewer") -> "TraceViewer":
        out = TraceViewer(native)
        out._runtime = self._runtime
        return out

    def _absorbed(self, plan: LazyFrame) -> "TraceViewer":
        return self._trace(self._tv.with_lazy(plan._native))

    # -- absorbed LazyFrame ops ----------------------------------------------------
    def filter(self, predicate: object) -> "TraceViewer":
        """Keep events matching ``predicate``: a query-DSL string, or an
        expression. An expression filters events, pushed to the index, while
        it is a plain field predicate and nothing but filters came before it;
        otherwise it filters this plan's rows."""
        if isinstance(predicate, Expr):
            if self._tv.filters_events():
                try:
                    return self._trace(self._tv.filter(predicate.to_query()))
                except TypeError:
                    pass
            return self._absorbed(LazyFrame.filter(self, predicate))
        return self._trace(self._tv.filter(predicate))

    query = filter

    def select(self, *items: object) -> "TraceViewer":
        """Project to ``items``, as :meth:`LazyFrame.select`. Names on raw
        events, with only filters before, are the fields the scan reads: any
        field, indexed or not, and a bare arg name (``"size"``) reads its
        ``args.size`` column."""
        names = [i for i in items if isinstance(i, str)]
        if items and len(names) == len(items):
            return self._trace(self._tv.select(names))
        return self._absorbed(LazyFrame.select(self, *items))

    def with_column(self, name: str, expr: Expr) -> "TraceViewer":
        return self._absorbed(LazyFrame.with_column(self, name, expr))

    def with_columns(self, *named: object, **columns: Expr) -> "TraceViewer":
        return self._absorbed(LazyFrame.with_columns(self, *named, **columns))

    def rename(self, names: Any) -> "TraceViewer":
        return self._absorbed(LazyFrame.rename(self, names))

    def sort_by(self, name: str, descending: bool = False) -> "TraceViewer":
        return self._absorbed(LazyFrame.sort_by(self, name, descending))

    def topk(self, name: str, k: int, largest: bool = True) -> "TraceViewer":
        return self._absorbed(LazyFrame.topk(self, name, k, largest))

    def head(self, n: int = 5) -> "TraceViewer":
        return self._absorbed(LazyFrame.head(self, n))

    def limit(self, n: int = 5) -> "TraceViewer":
        return self._absorbed(LazyFrame.limit(self, n))

    def slice(self, offset: int, length: int) -> "TraceViewer":
        return self._absorbed(LazyFrame.slice(self, offset, length))

    def offset(self, n: int) -> "TraceViewer":
        """Skip the first ``n`` rows."""
        return self._absorbed(LazyFrame.slice(self, n, (1 << 63) - 1))

    def auto_spill(self) -> "TraceViewer":
        return self._absorbed(LazyFrame.auto_spill(self))

    # -- trace builders -------------------------------------------------------------
    def phase(self, phase: PhaseArg) -> "TraceViewer":
        return self._trace(self._tv.phase(phase))

    def time_range(self, begin: float, end: float) -> "TraceViewer":
        return self._trace(self._tv.time_range(float(begin), float(end)))

    def time_bucket(
        self,
        interval_us: Union[int, float, str],
        normalize_to: Union[int, Literal["min"], None] = None,
    ) -> "TraceViewer":
        """Bucket events by time; a bare number is microseconds, a string
        (``"1ms"``) is converted. ``normalize_to`` aligns the buckets: an int
        origin, ``"min"`` for the trace's first timestamp (from the index), or
        ``None`` for 0."""
        us = int(round(coerce_duration(interval_us, 1e6, "interval_us")))
        return self._trace(self._tv.time_bucket(us, normalize_to))

    def resolution(self, cell: Union[int, float, str]) -> "TraceViewer":
        """The grid the occupancy aggregates (busy, concurrency, utilization,
        active) snap interval edges to; a bare number is microseconds. 0 is the
        exact union. Honored with a ``time_range``."""
        us = int(round(coerce_duration(cell, 1e6, "resolution")))
        return self._trace(self._tv.resolution(us))

    def time_unit(self, unit: TimeUnitArg) -> "TraceViewer":
        return self._trace(self._tv.time_unit(unit))

    def time_scale(self, ns_ratio: float) -> "TraceViewer":
        return self._trace(self._tv.time_scale(ns_ratio))

    def metadata(self, include: bool) -> "TraceViewer":
        return self._trace(self._tv.metadata(include))

    def rollup_root(self, path: str) -> "TraceViewer":
        return self._trace(self._tv.rollup_root(path))

    def views_root(self, path: str) -> "TraceViewer":
        return self._trace(self._tv.views_root(path))

    def memory_budget(self, nbytes: Union[int, str]) -> "TraceViewer":
        """Spill budget for the scan and the plan; bytes or a unit string."""
        return self._trace(self._tv.memory_budget(coerce_bytes(nbytes, "nbytes")))

    def group_by(self, *keys: str) -> "TraceViewer":  # ty: ignore[invalid-method-override]
        """Trace group keys: ``name``/``cat``/``pid``/``tid``/``fhash``/
        ``hhash``/``arg:<key>``, any other field, or a transform call such as
        ``basename(fname)``. No keys folds every event into one row."""
        return self._trace(self._tv.group_by(*keys))

    def agg(self, *specs: Union[str, Agg]) -> "TraceViewer":  # ty: ignore[invalid-method-override]
        """Trace aggregates: spec strings (``"count"``, ``"mean:dur"``,
        ``"p99:dur"``, ``"busy:dur"``) or bare-field ``Agg`` expressions
        (``F.dur.sum()``, ``F.dur.busy(resolution="1ms")``, ``F.any.mean()``)."""
        if not specs:
            raise TypeError("agg() needs at least one aggregate")
        strings, wildcard, grid = _agg_specs(specs)
        tv = self._tv
        if grid is not None:
            tv = tv.resolution(grid)
        if strings:
            tv = tv.agg(*strings)
        if wildcard:
            only_mean = wildcard == ["mean"]
            tv = tv.agg_numeric_args() if only_mean else tv.agg_numeric_args(*wildcard)
        return self._trace(tv)

    def agg_numeric_args(self, *reductions: str) -> "TraceViewer":
        """Aggregate every discovered numeric arg: no reductions gives one mean
        column per arg; op names give one ``<op>_<arg>`` column each."""
        return self._trace(self._tv.agg_numeric_args(*reductions))

    # -- inspection -------------------------------------------------------------------
    def column_info(self) -> Dict[str, str]:
        """Every column the index knows, mapped to its type name (``"int64"``
        / ``"float64"`` / ``"string"``), with a ``resolved.*`` alias per hash
        column. Reads index metadata only."""
        return dict(self._tv.column_info())

    def time_metric(self) -> str:
        """The trace's own time unit (``"us"``/``"ns"``/``"ms"``/``"sec"``)."""
        return str(self._tv.time_metric())

    def __repr__(self) -> str:
        try:
            plan = self._native.explain()
        except Exception:  # noqa: BLE001 - repr must never raise
            return "<TraceViewer>"
        return "<TraceViewer>\n" + plan

    # -- terminals ---------------------------------------------------------------------
    def call_tree(
        self,
        partition: Sequence[str] = ("pid", "tid"),
        ts: str = "ts",
        dur: str = "dur",
        name: str = "name",
    ) -> LazyFrame:
        """The events plus ``level`` / ``parent_id``: containment nesting of the
        ``[ts, ts+dur)`` intervals within each lane of ``partition``."""
        return self._new(self._tv.call_tree(list(partition), ts, dur, name))

    def flamegraph(
        self,
        partition: Sequence[str] = ("pid", "tid"),
        ts: str = "ts",
        dur: str = "dur",
        name: str = "name",
        group: Sequence[str] = (),
    ) -> LazyFrame:
        """The events folded by root-to-node ``name`` path: ``node_id``,
        ``parent``, ``name``, ``level``, ``total``, ``self``, ``count``.
        ``group`` roots one subtree per value of those fields."""
        return self._new(self._tv.flamegraph(list(partition), ts, dur, name, list(group)))

    def containment(
        self,
        partition: Sequence[str] = ("pid", "tid"),
        ts: str = "ts",
        dur: str = "dur",
        name: str = "name",
        group: Sequence[str] = (),
    ) -> "LazyResult[Containment]":
        """Both :meth:`call_tree` and :meth:`flamegraph` from one buffered fold."""
        ct, fg = self._tv.containment(list(partition), ts, dur, name, list(group))
        return LazyResult([self._new(ct), self._new(fg)], lambda f: Containment(f[0], f[1]))

    def flamegraph_partial(
        self,
        partition: Sequence[str] = ("pid", "tid"),
        ts: str = "ts",
        dur: str = "dur",
        name: str = "name",
        group: Sequence[str] = (),
    ) -> "LazyResult[bytes]":
        """A mergeable flamegraph of this viewer's files, for a distributed
        run; merge with :meth:`merge_flamegraph_partials`. Partition by pid so
        a lane stays on one rank."""
        plan = self._tv.flamegraph_partial(list(partition), ts, dur, name, list(group))
        return LazyResult([self._new(plan)], lambda f: _partial(f[0]))

    @staticmethod
    def merge_flamegraph_partials(partials: Sequence[bytes]) -> DataFrame:
        """The flamegraph node frame of every rank's partial (no scan)."""
        return _wrap(_ext.merge_flamegraph_partials(list(partials)))

    def aggregate_partial(self) -> "LazyResult[bytes]":
        """A mergeable partial of this aggregation for a distributed run;
        merge with :meth:`merge_partials`."""
        return LazyResult([self._new(self._tv.aggregate_partial())], lambda f: _partial(f[0]))

    def merge_partials(self, partials: Sequence[bytes]) -> DataFrame:
        """This aggregation's result from every rank's partial (no scan)."""
        return _wrap(self._tv.merge_partials(list(partials)))

    def materialize_partials(self, partials: Sequence[bytes]) -> None:
        """Write this aggregation's rollup from every rank's partial."""
        self._tv.materialize_partials(list(partials), self._runtime)

    def reconstruct_if_cached(self) -> Optional[DataFrame]:
        """This aggregation from its rollup, or ``None`` when none serves it."""
        out = self._tv.reconstruct_if_cached()
        return None if out is None else _wrap(out)

    def typed(
        self,
        shard_begin: int = 0,
        shard_end: int = 0,
        progress: Optional[Callable[[int, int], None]] = None,
    ) -> "LazyResult[Dict[str, DataFrame]]":
        """The aggregation index's ``regular`` / ``aggregated`` / ``counters``
        records over shard range ``[shard_begin, shard_end)`` (0 = all)."""
        tv, runtime = self._tv, self._runtime

        def finish(_: List[DataFrame]) -> Dict[str, DataFrame]:
            raw = tv.typed(shard_begin, shard_end, progress, runtime)
            return {k: _wrap(v) for k, v in raw.items()}

        return LazyResult([], finish)

    def collect_typed(
        self,
        shard_begin: int = 0,
        shard_end: int = 0,
        progress: Optional[Callable[[int, int], None]] = None,
    ) -> Dict[str, DataFrame]:
        """:meth:`typed`, run now."""
        return self.typed(shard_begin, shard_end, progress).collect()

    @overload
    def sink_json(self, path: str, lazy: Literal[False] = ...) -> Dict[str, object]: ...
    @overload
    def sink_json(self, path: str, lazy: Literal[True]) -> "LazyResult[Dict[str, object]]": ...

    def sink_json(self, path: str, lazy: bool = False) -> Any:
        """Write the selected events to ``path`` as NDJSON, verbatim (a
        re-indexable trace). Runs now and returns the scan stats; ``lazy=True``
        returns a :class:`LazyResult` for :func:`collect_all` or a session."""
        result = LazyResult([self._new(self._tv.sink_json(path))], lambda f: _export_stats(f[0]))
        return result if lazy else result.collect()

    @overload
    def materialize(
        self,
        checkpoint_size: Union[int, str] = ...,
        part_size: Union[int, str] = ...,
        progress: Optional[Callable[[int, int], None]] = ...,
        lazy: Literal[False] = ...,
    ) -> Dict[str, object]: ...
    @overload
    def materialize(
        self,
        checkpoint_size: Union[int, str] = ...,
        part_size: Union[int, str] = ...,
        progress: Optional[Callable[[int, int], None]] = ...,
        *,
        lazy: Literal[True],
    ) -> "LazyResult[Dict[str, object]]": ...

    def materialize(
        self,
        checkpoint_size: Union[int, str] = 0,
        part_size: Union[int, str] = 0,
        progress: Optional[Callable[[int, int], None]] = None,
        lazy: bool = False,
    ) -> Any:
        """Persist this query so a later matching read is served from it: a
        filtered trace for events, a rollup for an aggregation. Idempotent.
        Sizes take bytes or a unit string."""
        tv, runtime = self._tv, self._runtime
        cs = coerce_bytes(checkpoint_size, "checkpoint_size")
        ps = coerce_bytes(part_size, "part_size")

        def finish(_: List[DataFrame]) -> Dict[str, object]:
            return tv.materialize(cs, ps, progress, runtime)

        result: LazyResult[Dict[str, object]] = LazyResult([], finish)
        return result if lazy else result.collect()

    def export_trace(
        self,
        path: str,
        compress: bool = True,
        index: bool = False,
        member_size: Union[int, str] = 0,
        level: int = 6,
        part_size: Union[int, str] = 0,
    ) -> Dict[str, object]:
        """Write a trace file now: the aggregation as counter events when this
        viewer aggregates, else the selected events. Sizes take bytes or a
        unit string."""
        return self._tv.export_trace(
            path,
            compress,
            index,
            coerce_bytes(member_size, "member_size"),
            level,
            coerce_bytes(part_size, "part_size"),
            self._runtime,
        )

    @overload
    def statistics(self, lazy: Literal[False] = ...) -> Dict[str, object]: ...
    @overload
    def statistics(self, lazy: Literal[True]) -> "LazyResult[Dict[str, object]]": ...

    def statistics(self, lazy: bool = False) -> Any:
        """Count, mean and stddev of ``dur`` and the ``ts`` range of the
        selected events."""
        plan = self.group_by().agg("count", "mean:dur", "std:dur", "min:ts", "max:ts")
        result = LazyResult([plan], lambda f: _stats_row(f[0]))
        return result if lazy else result.collect()

    def compare(self, variant: "TraceViewer") -> LazyFrame:
        """This viewer's ``group_by`` + ``agg`` applied to ``variant``'s events
        too, joined on the group key: the keys, ``l_`` / ``r_`` per metric and
        ``delta_`` / ``pct_``. Both sides over the same files share one scan."""
        return self._new(self._tv.compare(variant._tv))

    def mv_source(self) -> List[str]:
        """The materialized-view files that would serve this query, or ``[]``."""
        return list(self._tv.mv_source())

    def materialize_dir(self) -> str:
        """Create and return the shared materialized-view directory (call on a
        viewer over the full file set)."""
        return self._tv.materialize_dir()

    def register_materialized(self, dir: str) -> None:
        """Write the materialized-view manifest at ``dir``."""
        self._tv.register_materialized(dir)

    def stream(self, batch_size: int = 65536) -> "Iterator[DataFrame]":  # ty: ignore[invalid-method-override]
        """The rows as :class:`DataFrame` chunks of about ``batch_size`` rows,
        holding a bounded amount in memory."""
        return LazyFrame.stream(self, batch_size)

    def session(self) -> "Session":
        """A :class:`Session` whose registered plans run together."""
        return Session(self)


class Handle:
    """A deferred result of a :class:`Session`; :meth:`result` returns it,
    executing the session first if it has not run."""

    __slots__ = ("_session", "_value", "_resolved")

    def __init__(self, session: "Session") -> None:
        self._session = session
        self._value: object = None
        self._resolved = False

    def result(self) -> Any:
        if not self._resolved:
            self._session.execute()
        return self._value


class Session:
    """Plans registered here run together on :meth:`execute` (or at the end of
    a ``with`` block, or on the first :meth:`Handle.result`), as
    :func:`collect_all` runs them: plans over the same trace share one scan.

    ::

        with tv.session() as s:
            by_cat = s.collect(tv.group_by("cat").agg("count", "mean:dur"))
            s.sink_json(tv.filter("dur > 1000000"), "slow.json")
        by_cat.result()
    """

    def __init__(self, viewer: TraceViewer) -> None:
        self._viewer = viewer
        self._roots: List[Any] = []
        self._handles: List[Handle] = []
        self._executed = False

    def collect(self, root: "Union[LazyFrame, LazyResult[Any]]") -> Handle:
        """Register a plan or a lazy result; its handle resolves on execute."""
        if self._executed:
            raise RuntimeError("cannot add a plan after the session has executed")
        handle = Handle(self)
        self._roots.append(root)
        self._handles.append(handle)
        return handle

    def sink_json(self, viewer: TraceViewer, path: str) -> Handle:
        """``viewer.sink_json(path)``, run with the session."""
        return self.collect(viewer.sink_json(path, lazy=True))

    def materialize(self, viewer: TraceViewer, **kwargs: Any) -> Handle:
        """``viewer.materialize(...)``, run with the session."""
        return self.collect(viewer.materialize(lazy=True, **kwargs))

    def attach(self, plugins: "Plugins") -> Handle:
        """Fold every plugin of ``plugins`` over the session's scan of its
        viewer; the result is the ``{name: value}`` mapping of
        ``Plugins.run().results``. The plugins share the scan only when the
        viewer has no filter."""
        native = plugins._native

        def finish(_: List[DataFrame]) -> Dict[str, object]:
            raw = _ext.plugin_results(native)
            return {name: plugins._shape(val) for name, val in raw.items()}

        plan = self._viewer._new(self._viewer._tv.plugins(native))
        return self.collect(LazyResult([plan], finish))

    def execute(self) -> None:
        """Run every registered plan once; a second call does nothing."""
        if self._executed:
            return
        values = collect_all(self._roots)
        for handle, value in zip(self._handles, values):
            handle._value = value
            handle._resolved = True
        self._executed = True

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if exc_type is None:
            self.execute()
