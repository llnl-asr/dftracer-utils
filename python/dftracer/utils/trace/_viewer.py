# SPDX-License-Identifier: MIT
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

import json
import os
from enum import Enum
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    Generic,
    Iterator,
    Literal,
    Mapping,
    NamedTuple,
    Sequence,
    TypeVar,
    Union,
    cast,
    overload,
)

from .. import dftracer_utils_ext as _ext
from .._aliases import install as _install_aliases
from .._dataframe import DataFrame, PhaseArg, TimeUnitArg
from .._enums import coerce_bytes, coerce_duration
from .._expr import Agg, Expr, Named, SelectItem
from .._lazyframe import BoundColumn, LazyFrame, LazyResult, collect_all
from ..schemas import JsonValue, RecordSchema, schema_id
from ._index_records import IndexRecords
from ._lazy import LazyOps
from ._partials import agg_specs, export_stats, partial_bytes, stats_row

_S = TypeVar("_S", bound=RecordSchema)
T = TypeVar("T")

# The native calls that `columns` makes, bound once: it is read in loops.
_scan_columns = _ext.TraceViewer.columns
_plan_columns = _ext.LazyFrame.schema

if TYPE_CHECKING:
    from .._enums import DType
    from ..dftracer_utils_ext import Scalar
    from ..plugins import Plugins
    from ..runtime import Runtime
    from ..schemas import JsonValue


DuqlParam = Union[bool, int, float, str, Sequence[Union[bool, int, float, str]]]


class Phase(str, Enum):
    """Which records a view scans; ``phase(Phase.EVENTS)`` is ``phase("events")``."""

    EVENTS = "events"  # ph=1
    COUNTERS = "counters"  # ph=2
    ANY = "any"


class Containment(NamedTuple):
    """Both containment frames of one buffered fold."""

    call_tree: DataFrame
    flamegraph: DataFrame


class TraceViewer(LazyOps, _ext.TraceViewer):
    """A lazy view over trace files; see the module docstring.

    ``files`` is a path (``str`` or ``os.PathLike``) or a sequence of paths,
    with the same rule as :class:`~dftracer.utils.index.Indexer`. Each path is a
    file, a directory or a glob, and a sequence may mix them. A directory is
    scanned recursively for trace files (``*.gz``, not the ``.dftindex``,
    ``split`` and ``schemas`` folders). A glob has ``*`` (any run of
    characters), ``?`` (one character), ``[abc]``, ``[a-z]``, ``[!a-z]`` or
    ``[^a-z]``, and ``**`` as a whole path part (zero or more directories); a
    part that starts with ``.`` is matched like any other, there is no escape
    character, and ``**`` does not enter ``.dftindex``, ``split``, ``schemas``
    or a symlink to a directory. A glob names any file it matches whatever the
    file's name, and a directory it matches is scanned like a directory path.
    A path that exists is taken as it is, even when its name has a wildcard.
    The files are those of the paths in order. With several paths each file
    appears once, by device and inode (the first one wins, so a symlink or
    hard link names its file once); one path lists a file as often as it
    names it. One directory or glob gives its files sorted by path. Staging folders and
    files of a table writer (``.<name>.tmp-<id>``, ``.<name>.old-<id>``) and
    ``.dftindex`` folders are left out of a scan or a glob. A glob that
    matches nothing adds no file. Nothing is read
    when the view is built, so a missing path (one with no wildcard that does
    not exist) or a file that is not a trace raises a ``DFTUtilsError`` at
    ``collect()`` (``Indexer`` raises a missing path on its first ``status``,
    ``ensure`` or ``build``, and a non-trace file when it indexes it). An
    empty sequence, an empty directory and a glob with no match are views of
    no rows, so a worker with no shard can still build one (``Indexer`` raises
    ``ValueError`` for an empty sequence).

    ``record_schema`` names a registered record
    schema (an id or a :class:`dftracer.utils.schemas.RecordSchema` class) to
    read the files as, instead of their recorded or detected one; an
    unregistered id raises ``DFTUtilsValueError``.

    ``format`` names the reader of the files, ``"jsonl"``, the built-in trace
    reader. With none, the files' content picks it, and a file that a plugin
    file format claims raises ``DFTUtilsValueError``: a viewer reads trace
    files only, so open such a file with :func:`dftracer.utils.scan_file`. An
    unknown name raises ``DFTUtilsValueError`` listing the known ones."""

    __slots__ = ("_rt", "_pl")

    # The native type builds a viewer from its files; its factories build this
    # class directly (_bind_types), so a call here reads ``files`` and returns
    # the viewer, and __init__ has nothing left to do.
    def __new__(
        cls,
        files: str | os.PathLike[str] | Sequence[str | os.PathLike[str]] | _ext.TraceViewer,
        index_dir: str | None = None,
        runtime: Runtime | None = None,
        record_schema: str | type[RecordSchema] | None = None,
        format: str | None = None,
    ) -> TraceViewer:
        if cls is not TraceViewer:
            raise TypeError(
                "TraceViewer cannot be subclassed: the native calls build the class bound by "
                "_bind_types, so an instance of a subclass would come back as a TraceViewer"
            )
        out: TraceViewer
        if isinstance(files, _ext.TraceViewer):
            out = cast("TraceViewer", files)
        else:
            out = _ext.TraceViewer.__new__(TraceViewer)
            _ext.TraceViewer.__init__(out, files, index_dir, format)
        if record_schema is not None:
            out = _ext.TraceViewer.record_schema(out, schema_id(record_schema))
        if runtime is not None:
            out._runtime = runtime
        return out

    def __init__(
        self,
        files: str | os.PathLike[str] | Sequence[str | os.PathLike[str]] | _ext.TraceViewer,
        index_dir: str | None = None,
        runtime: Runtime | None = None,
        record_schema: str | type[RecordSchema] | None = None,
        format: str | None = None,
    ) -> None:
        pass

    @property
    def _runtime(self) -> Runtime | None:
        return getattr(self, "_rt", None)

    @_runtime.setter
    def _runtime(self, runtime: Runtime | None) -> None:
        self._rt = runtime

    # -- constructing results ----------------------------------------------------
    def _trace(self, out: TraceViewer) -> TraceViewer:
        out._runtime = self._runtime
        return out

    def _new(self, out: LazyFrame) -> LazyFrame:
        out._runtime = self._runtime
        return out

    def _schema_plan(self) -> LazyFrame:
        """The plan that :attr:`columns` and :attr:`schema` read, built once: a
        viewer never changes."""
        plan = getattr(self, "_pl", None)
        if plan is None:
            plan = self._pl = _ext.TraceViewer.lazy(self)
        return plan

    def lazy(self) -> LazyFrame:
        """The plan of this viewer as a :class:`LazyFrame`."""
        return self._new(_ext.TraceViewer.lazy(self))

    def _plan(self) -> LazyFrame:
        """:meth:`lazy`, for an op the viewer absorbs: a scan adds columns per
        batch, so a name the schema does not list may still be a column."""
        out = self.lazy()
        out._open = True
        return out

    def _absorbed(self, plan: LazyFrame) -> TraceViewer:
        return self._trace(_ext.TraceViewer.with_lazy(self, plan))

    # -- absorbed LazyFrame ops ----------------------------------------------------
    def filter(self, predicate: str | Expr) -> TraceViewer:
        """Keep events matching ``predicate``: a duql string, or an
        expression. An expression filters events, pushed to the index, while
        it is a plain field predicate and nothing but filters came before it;
        otherwise it filters this plan's rows."""
        if isinstance(predicate, Expr):
            if _ext.TraceViewer.filters_events(self):
                try:
                    return self._trace(_ext.TraceViewer.filter(self, predicate.to_duql()))
                except TypeError:
                    pass
            return self._absorbed(self._plan().filter(predicate))
        return self._trace(_ext.TraceViewer.filter(self, predicate))

    def duql(self, text: str, **params: DuqlParam) -> TraceViewer:
        """Run the duql pipeline ``text`` with ``params`` bound to its
        ``$name`` parameters (bool, int, float or str, or a list of them for
        ``x in $name``). Leading ``where``
        stages filter events in the scan; later stages run on its columns. A
        query with an ``explain`` prefix returns a one-column ``plan`` view of
        the plan, one row per line."""
        return self._trace(_ext.TraceViewer.duql(self, text, params))

    def explain_duql(self, text: str, **params: DuqlParam) -> str:
        """The plan :meth:`duql` builds, one step per line, without scanning.
        ``text`` starting with ``explain analyze`` also runs the plan and adds
        what each stage measured."""
        return _ext.TraceViewer.explain_duql(self, text, params)

    def select(self, *items: SelectItem) -> TraceViewer:
        """Project to ``items``, as :meth:`LazyFrame.select`. Names on raw
        events, with only filters before, are the fields the scan reads: any
        field, indexed or not, and a name without the ``args.`` prefix that
        is not an envelope field (``"size"``, ``"dur.p99"``) reads its
        ``args.<name>`` column, as in :meth:`filter`."""
        names = [i for i in items if isinstance(i, str)]
        if items and len(names) == len(items):
            return self._trace(_ext.TraceViewer.select(self, names))
        return self._absorbed(self._plan().select(*items))

    def with_column(self, name: str, expr: Expr) -> TraceViewer:
        return self._absorbed(self._plan().with_column(name, expr))

    def with_columns(self, *named: Named, **columns: Expr) -> TraceViewer:
        return self._absorbed(self._plan().with_columns(*named, **columns))

    def _schema_complete(self) -> bool:
        return False

    def drop(self, *names: str | Sequence[str]) -> TraceViewer:
        return self._absorbed(self._plan().drop(*names))

    def rename(self, names: Sequence[str] | Mapping[str, str]) -> TraceViewer:
        return self._absorbed(self._plan().rename(names))

    def sort_by(self, name: str, descending: bool = False, nulls_last: bool = True) -> TraceViewer:
        return self._absorbed(self._plan().sort_by(name, descending, nulls_last))

    def topk(self, name: str, k: int, largest: bool = True) -> TraceViewer:
        return self._absorbed(self._plan().topk(name, k, largest))

    def head(self, n: int = 5) -> TraceViewer:
        return self._absorbed(self._plan().head(n))

    def slice(self, offset: int, length: int) -> TraceViewer:
        return self._absorbed(self._plan().slice(offset, length))

    def __getitem__(self, key: object) -> BoundColumn | TraceViewer:
        """:meth:`LazyFrame.__getitem__` over this viewer: a list of names or a
        slice is absorbed, so the result is a ``TraceViewer``."""
        return cast(
            "BoundColumn | TraceViewer", LazyFrame.__getitem__(cast("LazyFrame", self), key)
        )

    def __copy__(self) -> TraceViewer:
        """A viewer never changes, so a copy, deep or shallow, is the viewer."""
        return self

    def __deepcopy__(self, memo: object) -> TraceViewer:
        return self

    def offset(self, n: int) -> TraceViewer:
        """Skip the first ``n`` rows."""
        return self._absorbed(self._plan().slice(n, (1 << 63) - 1))

    def auto_spill(self) -> TraceViewer:
        return self._absorbed(self._plan().auto_spill())

    # -- trace builders -------------------------------------------------------------
    def phase(self, phase: PhaseArg) -> TraceViewer:
        return self._trace(_ext.TraceViewer.phase(self, phase))

    def time_range(self, begin: float, end: float) -> TraceViewer:
        """Keep the events that start in ``[begin, end)``. Busy, concurrency,
        utilization and active take every event overlapping the window,
        clipped to it; utilization divides by ``end - begin``."""
        return self._trace(_ext.TraceViewer.time_range(self, float(begin), float(end)))

    def time_bucket(
        self,
        interval_us: int | float | str,
        normalize_to: int | Literal["min"] | None = None,
    ) -> TraceViewer:
        """Bucket events by time; a bare number is microseconds, a string
        (``"1ms"``) is converted. ``normalize_to`` aligns the buckets: an int
        origin, ``"min"`` for the trace's first timestamp (from the index), or
        ``None`` for 0."""
        us = int(round(coerce_duration(interval_us, 1e6, "interval_us")))
        return self._trace(_ext.TraceViewer.time_bucket(self, us, normalize_to))

    def resolution(self, cell: int | float | str) -> TraceViewer:
        """The grid the occupancy aggregates (busy, concurrency, utilization,
        active) snap interval edges to; a bare number is microseconds. 0 is the
        exact union."""
        us = int(round(coerce_duration(cell, 1e6, "resolution")))
        return self._trace(_ext.TraceViewer.resolution(self, us))

    def time_unit(self, unit: TimeUnitArg) -> TraceViewer:
        return self._trace(_ext.TraceViewer.time_unit(self, unit))

    def time_scale(self, ns_ratio: float) -> TraceViewer:
        return self._trace(_ext.TraceViewer.time_scale(self, ns_ratio))

    def rollup_root(self, path: str) -> TraceViewer:
        return self._trace(_ext.TraceViewer.rollup_root(self, path))

    def views_root(self, path: str) -> TraceViewer:
        return self._trace(_ext.TraceViewer.views_root(self, path))

    def memory_budget(self, nbytes: int | str) -> TraceViewer:
        """Spill budget for the scan and the plan; bytes or a unit string."""
        return self._trace(_ext.TraceViewer.memory_budget(self, coerce_bytes(nbytes, "nbytes")))

    def group_by(self, *keys: str | Sequence[str]) -> TraceViewer:
        """Trace group keys: ``name``/``cat``/``pid``/``tid``/``fhash``/
        ``hhash``/``arg:<key>``, any other field, or a transform call such as
        ``basename(fname)``, as separate arguments or one list. No keys folds
        every event into one row."""
        flat: list[str] = []
        for k in keys:
            flat.extend([k] if isinstance(k, str) else k)
        return self._trace(_ext.TraceViewer.group_by(self, *flat))

    def agg(self, *specs: str | Agg) -> TraceViewer:
        """Trace aggregates: spec strings (``"count"``, ``"mean:dur"``,
        ``"p99:dur"``, ``"busy:dur"``) or bare-field ``Agg`` expressions
        (``F.dur.sum()``, ``F.dur.busy(resolution="1ms")``, ``F.any.mean()``)."""
        if not specs:
            raise TypeError("agg() needs at least one aggregate")
        strings, wildcard, grid = agg_specs(specs)
        tv = self
        if grid is not None:
            tv = _ext.TraceViewer.resolution(tv, grid)
        if strings:
            tv = _ext.TraceViewer.agg(tv, *strings)
        if wildcard:
            only_mean = wildcard == ["mean"]
            tv = (
                _ext.TraceViewer.agg_numeric_args(tv)
                if only_mean
                else _ext.TraceViewer.agg_numeric_args(tv, *wildcard)
            )
        return self._trace(tv)

    aggregate = agg

    def agg_numeric_args(self, *reductions: str) -> TraceViewer:
        """Aggregate every discovered numeric arg: no reductions gives one mean
        column per arg; op names give one ``<op>_<arg>`` column each."""
        return self._trace(_ext.TraceViewer.agg_numeric_args(self, *reductions))

    def rows(self, cls: type[_S]) -> Iterator[_S]:
        """One ``cls`` instance per selected record, each field set from the
        column at its path (``None`` when absent). Streams :meth:`stream` chunks,
        so only one chunk of rows is held at a time."""
        paths = cls._paths
        for frame in self.select(*dict.fromkeys(paths.values())).stream():
            data = frame.to_dict()
            # A Json field is its JSON text, not the parsed value.
            for name in cls._json:
                path = paths[name]
                if path in data:
                    data[path] = frame[path]._to_pylist()
            columns = {name: data.get(path) for name, path in paths.items()}
            for i in range(len(frame)):
                values = {
                    name: (col[i] if col is not None else None) for name, col in columns.items()
                }
                yield cls(**cast("Dict[str, Scalar]", values))

    # -- inspection -------------------------------------------------------------------
    @property
    def columns(self) -> list[str]:
        """Output column names without running the plan. A file whose index
        holds no path catalog is scanned once first, so every path it holds is
        listed."""
        _scan_columns(self)
        try:
            plan = self._pl
        except AttributeError:
            plan = self._pl = _ext.TraceViewer.lazy(self)
        return _plan_columns(plan)

    @property
    def schema(self) -> dict[str, DType]:
        """Output column names and dtypes without running the plan; raises when
        the files have no one record schema."""
        _ext.TraceViewer.columns(self)
        return self._schema_plan().schema

    def column_info(self) -> dict[str, str]:
        """Every column the index knows, mapped to its type name (``"int64"``
        / ``"float64"`` / ``"string"``), with a ``resolved.*`` alias per hash
        column. Reads index metadata only."""
        return dict(_ext.TraceViewer.column_info(self))

    def schema_tree(self) -> dict[str, JsonValue]:
        """The paths the files hold, from the index without scanning, nested by
        ``.`` segments: each node maps a segment to a dict with ``type`` (the
        observed type), ``count`` (records holding it non-null), ``field`` and
        ``declared_type`` (the record schema's field, when declared) and
        ``children`` (the nodes below it); keys without a value are left out.
        A declared field that no file holds has ``count`` 0."""
        tree: dict[str, Any] = {}
        for leaf in json.loads(_ext.TraceViewer.schema_tree(self)):
            nodes = tree
            *parents, last = leaf["path"].split(".")
            for part in parents:
                nodes = nodes.setdefault(part, {}).setdefault("children", {})
            node = nodes.setdefault(last, {})
            for key in ("type", "count", "field", "declared_type"):
                if leaf[key] is not None:
                    node[key] = leaf[key]
        return cast("dict[str, JsonValue]", tree)

    def time_metric(self) -> str:
        """The trace's own time unit (``"us"``/``"ns"``/``"ms"``/``"sec"``)."""
        return str(_ext.TraceViewer.time_metric(self))

    def __repr__(self) -> str:
        try:
            plan = self.lazy().explain()
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
        return self._new(_ext.TraceViewer.call_tree(self, list(partition), ts, dur, name))

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
        return self._new(
            _ext.TraceViewer.flamegraph(self, list(partition), ts, dur, name, list(group))
        )

    def containment(
        self,
        partition: Sequence[str] = ("pid", "tid"),
        ts: str = "ts",
        dur: str = "dur",
        name: str = "name",
        group: Sequence[str] = (),
    ) -> LazyResult[Containment]:
        """Both :meth:`call_tree` and :meth:`flamegraph` from one buffered fold."""
        ct, fg = _ext.TraceViewer.containment(self, list(partition), ts, dur, name, list(group))
        return LazyResult([self._new(ct), self._new(fg)], lambda f: Containment(f[0], f[1]))

    def flamegraph_partial(
        self,
        partition: Sequence[str] = ("pid", "tid"),
        ts: str = "ts",
        dur: str = "dur",
        name: str = "name",
        group: Sequence[str] = (),
    ) -> LazyResult[bytes]:
        """A mergeable flamegraph of this viewer's files, for a distributed
        run; merge with :meth:`merge_flamegraph_partials`. Partition by pid so
        a lane stays on one rank."""
        plan = _ext.TraceViewer.flamegraph_partial(
            self, list(partition), ts, dur, name, list(group)
        )
        return LazyResult([self._new(plan)], lambda f: partial_bytes(f[0]))

    @staticmethod
    def merge_flamegraph_partials(partials: Sequence[bytes]) -> DataFrame:
        """The flamegraph node frame of every rank's partial (no scan)."""
        return _ext.merge_flamegraph_partials(list(partials))

    def aggregate_partial(self) -> LazyResult[bytes]:
        """A mergeable partial of this aggregation for a distributed run;
        merge with :meth:`merge_partials`."""
        return LazyResult(
            [self._new(_ext.TraceViewer.aggregate_partial(self))], lambda f: partial_bytes(f[0])
        )

    def merge_partials(self, partials: Sequence[bytes]) -> DataFrame:
        """This aggregation's result from every rank's partial (no scan)."""
        return _ext.TraceViewer.merge_partials(self, list(partials))

    def materialize_partials(self, partials: Sequence[bytes]) -> None:
        """Write this aggregation's rollup from every rank's partial."""
        _ext.TraceViewer.materialize_partials(self, list(partials), self._runtime)

    def reconstruct_if_cached(self) -> DataFrame | None:
        """This aggregation from its rollup, or ``None`` when none serves it."""
        out = _ext.TraceViewer.reconstruct_if_cached(self)
        return out

    def index_records(
        self,
        shard_begin: int = 0,
        shard_end: int = 0,
        progress: Callable[[int, int], None] | None = None,
    ) -> LazyResult[IndexRecords[DataFrame]]:
        """The aggregation index's records over shard range
        ``[shard_begin, shard_end)`` (0 = all)."""
        tv, runtime = self, self._runtime

        def finish(_: list[DataFrame]) -> IndexRecords[DataFrame]:
            raw = _ext.TraceViewer._index_records(tv, shard_begin, shard_end, progress, runtime)
            regular, aggregated, counters = cast("tuple[DataFrame, DataFrame, DataFrame]", raw)
            return IndexRecords(regular, aggregated, counters)

        return LazyResult([], finish)

    def collect_index_records(
        self,
        shard_begin: int = 0,
        shard_end: int = 0,
        progress: Callable[[int, int], None] | None = None,
    ) -> IndexRecords[DataFrame]:
        """:meth:`index_records`, run now."""
        return self.index_records(shard_begin, shard_end, progress).collect()

    @overload
    def sink_json(self, path: str, lazy: Literal[False] = ...) -> dict[str, JsonValue]: ...
    @overload
    def sink_json(self, path: str, lazy: Literal[True]) -> LazyResult[dict[str, JsonValue]]: ...

    def sink_json(
        self, path: str, lazy: bool = False
    ) -> dict[str, JsonValue] | LazyResult[dict[str, JsonValue]]:
        """Write the selected events to ``path`` as NDJSON, verbatim (a
        re-indexable trace). Runs now and returns the scan stats; ``lazy=True``
        returns a :class:`LazyResult` for :func:`collect_all` or a session."""
        result = LazyResult(
            [self._new(_ext.TraceViewer.sink_json(self, path))], lambda f: export_stats(f[0])
        )
        return result if lazy else result.collect()

    @overload
    def materialize(
        self,
        checkpoint_size: int | str = ...,
        part_size: int | str = ...,
        progress: Callable[[int, int], None] | None = ...,
        lazy: Literal[False] = ...,
    ) -> dict[str, JsonValue]: ...
    @overload
    def materialize(
        self,
        checkpoint_size: int | str = ...,
        part_size: int | str = ...,
        progress: Callable[[int, int], None] | None = ...,
        *,
        lazy: Literal[True],
    ) -> LazyResult[dict[str, JsonValue]]: ...

    def materialize(
        self,
        checkpoint_size: int | str = 0,
        part_size: int | str = 0,
        progress: Callable[[int, int], None] | None = None,
        lazy: bool = False,
    ) -> dict[str, JsonValue] | LazyResult[dict[str, JsonValue]]:
        """Persist this query so a later matching read is served from it: a
        filtered trace for events, a rollup for an aggregation. Idempotent.
        Sizes take bytes or a unit string."""
        tv, runtime = self, self._runtime
        cs = coerce_bytes(checkpoint_size, "checkpoint_size")
        ps = coerce_bytes(part_size, "part_size")

        def finish(_: list[DataFrame]) -> dict[str, JsonValue]:
            return cast(
                "dict[str, JsonValue]", _ext.TraceViewer.materialize(tv, cs, ps, progress, runtime)
            )

        result: LazyResult[dict[str, JsonValue]] = LazyResult([], finish)
        return result if lazy else result.collect()

    def export_trace(
        self,
        path: str,
        compress: bool = True,
        index: bool = False,
        member_size: int | str = 0,
        level: int = 6,
        part_size: int | str = 0,
    ) -> dict[str, JsonValue]:
        """Write a trace file now: the aggregation as counter events when this
        viewer aggregates, else the selected events. Sizes take bytes or a
        unit string."""
        out = _ext.TraceViewer.export_trace(
            self,
            path,
            compress,
            index,
            coerce_bytes(member_size, "member_size"),
            level,
            coerce_bytes(part_size, "part_size"),
            self._runtime,
        )
        return cast("dict[str, JsonValue]", out)

    @overload
    def statistics(self, lazy: Literal[False] = ...) -> dict[str, JsonValue]: ...
    @overload
    def statistics(self, lazy: Literal[True]) -> LazyResult[dict[str, JsonValue]]: ...

    def statistics(self, lazy: bool = False) -> Any:
        """Count, mean and stddev of ``dur`` and the ``ts`` range of the
        selected events."""
        plan = self.group_by().agg("count", "mean:dur", "std:dur", "min:ts", "max:ts")
        result = LazyResult([plan], lambda f: stats_row(f[0]))
        return result if lazy else result.collect()

    def compare(self, variant: TraceViewer) -> LazyFrame:
        """This viewer's ``group_by`` + ``agg`` applied to ``variant``'s events
        too, joined on the group key: the keys, ``l_`` / ``r_`` per metric and
        ``delta_`` / ``pct_``. Both sides over the same files share one scan."""
        return self._new(_ext.TraceViewer.compare(self, variant))

    def mv_source(self) -> list[str]:
        """The materialized-view files that would serve this query, or ``[]``."""
        return list(_ext.TraceViewer.mv_source(self))

    def explain(self, analyze: bool = False) -> str:
        """The optimized plan as text, for introspection and tests. With
        ``analyze=True`` the plan runs and the text adds what each stage
        measured (see :meth:`LazyFrame.explain`)."""
        return self.lazy().explain(analyze=analyze)

    def stream(self, batch_size: int = 65536) -> Iterator[DataFrame]:
        """The rows as :class:`DataFrame` chunks of about ``batch_size`` rows,
        holding a bounded amount in memory."""
        return self.lazy().stream(batch_size)

    def session(self) -> Session:
        """A :class:`Session` whose registered plans run together."""
        return Session(self)


class Handle(Generic[T]):
    """A deferred result of a :class:`Session`; :meth:`result` returns it,
    executing the session first if it has not run."""

    __slots__ = ("_session", "_value", "_resolved")

    def __init__(self, session: Session) -> None:
        self._session = session
        self._value: T | None = None
        self._resolved = False

    def result(self) -> T:
        if not self._resolved:
            self._session.execute()
        return cast("T", self._value)


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
        self._roots: list[Any] = []
        self._handles: list[Handle[Any]] = []
        self._executed = False

    @overload
    def collect(self, root: LazyFrame) -> Handle[DataFrame]: ...
    @overload
    def collect(self, root: LazyResult[T]) -> Handle[T]: ...
    def collect(self, root: LazyFrame | LazyResult[T]) -> Handle[DataFrame] | Handle[T]:
        """Register a plan or a lazy result; its handle resolves on execute."""
        if self._executed:
            raise RuntimeError("cannot add a plan after the session has executed")
        handle: Handle[Any] = Handle(self)
        self._roots.append(root)
        self._handles.append(handle)
        return handle

    def sink_json(self, viewer: TraceViewer, path: str) -> Handle[dict[str, JsonValue]]:
        """``viewer.sink_json(path)``, run with the session."""
        return self.collect(viewer.sink_json(path, lazy=True))

    def materialize(
        self,
        viewer: TraceViewer,
        checkpoint_size: int | str = 0,
        part_size: int | str = 0,
        progress: Callable[[int, int], None] | None = None,
    ) -> Handle[dict[str, JsonValue]]:
        """``viewer.materialize(...)``, run with the session."""
        return self.collect(viewer.materialize(checkpoint_size, part_size, progress, lazy=True))

    def attach(self, plugins: Plugins) -> Handle[dict[str, object]]:
        """Fold every plugin of ``plugins`` over the session's scan of its
        viewer; the result is the ``{name: value}`` mapping of
        ``Plugins.run().results``. The plugins share the scan only when the
        viewer has no filter."""
        native = plugins._native

        def finish(_: list[DataFrame]) -> dict[str, object]:
            raw = _ext.plugin_results(native)
            return {name: plugins._shape(val) for name, val in raw.items()}

        plan = self._viewer._new(_ext.TraceViewer.plugins(self._viewer, native))
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

    def __enter__(self) -> Session:
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if exc_type is None:
            self.execute()


_install_aliases("LazyFrame", TraceViewer, faces=("pandas",))
_ext._bind_types(TraceViewer=TraceViewer)
