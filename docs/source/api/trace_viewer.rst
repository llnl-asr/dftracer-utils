:description: Reference for TraceViewer, the lazy LazyFrame over DFTracer traces to filter, group, aggregate, and collect in a single indexed pass.

TraceViewer (Querying Traces)
=============================

``TraceViewer`` is the primary API for querying DFTracer traces. It is a
:class:`~dftracer.utils.LazyFrame` whose source is a trace scan: builder
methods (``filter``, ``group_by``, ``agg``, ...) each return a new plan and do
no work. ``collect()`` runs the plan in a single pass and returns a
:class:`~dftracer.utils.DataFrame`. Terminals that are not one frame
(``containment``, the partials, ``statistics(lazy=True)``) return a
:class:`~dftracer.utils.LazyResult`, which also runs on ``collect()``. When an
index exists next to the traces, the same query is served from the index
(chunk pruning, aggregation tiers, summaries) instead of a full decompress.

.. code-block:: python

   from dftracer.utils import TraceViewer

   # A directory is scanned recursively for .pfw.gz traces, in parallel.
   view = TraceViewer("traces/")           # index_path optional; sidecar used when present

   # Top I/O calls by total time.
   df = (
       view.filter('cat == "POSIX"')
           .group_by("name")
           .agg("count", "sum:dur", "max:dur")
           .collect()                       # -> DataFrame (runs the plan)
   )
   pdf = df.to_pandas()

Constructing a view
-------------------

.. code-block:: python

   TraceViewer(files, index_path=None, runtime=None)

``files`` is a directory (scanned recursively for ``.pfw.gz`` traces), a single
file path, or a list of file paths. Pass ``index_path`` to point at an index
directory explicitly; omit it to use the sidecar convention. An optional
:class:`~dftracer.utils.Runtime` controls the thread pool.

The fluent builder
------------------

Builder methods are lazy and chainable, and every one below returns a
``TraceViewer``: ``group_by`` / ``agg`` shape the trace scan itself, and the
``LazyFrame`` ops it overrides (``filter``, ``select``, ``with_column``,
``rename``, ``sort_by``, ``topk``, ``head`` / ``limit`` / ``offset`` /
``slice``) are absorbed into the plan. Any other ``LazyFrame`` op (``join``,
``pivot``, ...) returns a plain :class:`~dftracer.utils.LazyFrame`.

The trace builders (``phase``, ``time_range``, ``time_bucket``,
``resolution``, ``time_unit``, ``time_scale``, ``metadata``, ``rollup_root``,
``views_root``, ``group_by``, ``agg``, ``agg_numeric_args``, a query-DSL
``filter``, a ``select`` of names) may only follow filters. After ``sort_by``,
``head``, ``limit``, ``offset``, ``topk``, ``with_column`` and the like they
raise ``DFTUtilsValueError`` naming that op, so put trace settings and
``group_by`` / ``agg`` first. ``memory_budget`` and ``auto_spill`` may go
anywhere. A ``select`` of names after ``group_by`` / ``agg`` projects the
aggregated output.

.. list-table::
   :header-rows: 1
   :widths: 32 68

   * - Method
     - Effect
   * - ``filter(pred)`` / ``query(pred)``
     - Keep events matching the :doc:`query DSL <query>` (e.g. ``'dur >= 1000 and cat == "POSIX"'``) or a columnar expression (``col("dur") > 1000``). A plain field predicate with only filters before it is pushed to the index; otherwise it filters the plan's rows.
   * - ``phase(name)``
     - Restrict to a record family: ``"events"`` (``ph="X"``), ``"counters"`` (``ph="C"``), ``"aggregated"`` (rollup records), ``"metadata"`` (``ph="M"``), or ``"any"``.
   * - ``time_range(begin, end)``
     - Keep events whose timestamp falls in ``[begin, end)``.
   * - ``time_bucket(interval_us, normalize_to=None)``
     - Bucket ``ts`` into fixed ``interval_us`` windows (a group key for time series). A number is microseconds; a string such as ``"1ms"`` is converted. ``normalize_to`` sets the window anchor: an int origin, or ``"min"`` for the trace's first timestamp instead of ``0``.
   * - ``resolution(cell)``
     - Grid the occupancy aggregates (``busy``, ``concurrency``, ``utilization``, ``active``) snap interval edges to; microseconds or a string such as ``"1ms"``. ``0`` is the exact union. Also set by ``F.dur.busy(resolution="1ms")`` inside ``agg``.
   * - ``time_unit(unit)`` / ``time_scale(ns_ratio)``
     - Interpret/scale the trace's native time unit (see the :doc:`quickstart <../quickstart>`).
   * - ``select(*cols)``
     - Names on raw events are the fields the scan reads (a bare arg name such as ``"size"`` reads ``args.size``); after ``group_by`` / ``agg`` it projects the output.
   * - ``head(n)`` / ``limit(n)`` / ``offset(n)`` / ``slice(offset, length)``
     - Paginate the result rows.
   * - ``group_by(*keys)``
     - Group by zero or more keys; no keys folds every event into one row.
   * - ``agg(*specs)``
     - Aggregate with ``op:field`` spec strings or bare-field ``Agg`` expressions (``F.dur.sum()``, ``F.dur.busy(resolution="1ms")``, ``F.any.mean()``).
   * - ``agg_numeric_args(*reductions)``
     - Aggregate every discovered numeric arg: one mean column per arg, or one ``<op>_<arg>`` column per named reduction.
   * - ``memory_budget(nbytes)`` / ``auto_spill()``
     - Bound in-memory aggregation state, spilling to disk past the budget. ``nbytes`` takes bytes or a unit string.

Group keys accept the raw event dimensions - ``name``, ``cat``, ``pid``,
``tid``, ``io_cat``, ``acc_pat``, ``fhash``, ``hhash``, ``file_path``,
``file_name``, ``host_name``, ``rank`` (pid resolved to MPI rank via ``PR``
metadata), ``arg:<key>`` and any other field - optionally wrapped in a
transform: ``dirname(...)``, ``basename(...)``, ``lower(...)``, or
``bucket(file_path, 'sub1', 'sub2', ...)``, which folds a path to the first
listed substring it contains (values matching none fold to an empty string).

Aggregation specs are ``op:field`` (or bare ``count``):

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Spec
     - Result column
   * - ``count``
     - row count
   * - ``sum:dur`` / ``sum:size``
     - summed metric (``sum_dur``, ...)
   * - ``min:ts`` / ``max:te`` / ``min:dur`` / ``max:dur``
     - extrema (``min_ts``, ``max_te``, ...)
   * - ``sumsq:dur``
     - sum of squares (for variance/std)
   * - ``p50:dur`` / ``p90:dur``
     - percentiles (from the persisted DDSketch when indexed)
   * - ``hist:dur``
     - histogram buckets as an Arrow ``list<struct<lo, hi, count>>``
   * - ``skew:dur`` / ``kurt:dur``
     - skewness / kurtosis
   * - ``set_union:field``
     - distinct values per group (``set_field``)

Reading the result
-------------------

``collect()`` runs the plan and returns a native
:class:`~dftracer.utils.DataFrame` (a set of typed
:class:`~dftracer.utils.Series`). Convert with ``to_arrow()`` /
``to_pandas()`` / ``to_polars()`` only at the edge, or keep computing on it in
the columnar engine (see :doc:`../columnar-engine`):

.. code-block:: python

   # Per-(name, time bucket) I/O volume as a time series.
   df = (
       TraceViewer(files)
       .filter('cat == "POSIX"')
       .time_bucket(1_000_000)          # 1 s windows (microseconds)
       .group_by("name", "time_bucket")
       .agg("count", "sum:size")
       .collect()                        # -> DataFrame
   )

``collect_typed`` does a single pass over an aggregation index and returns its
three record families at once - useful for building several frames from one
scan:

.. code-block:: python

   typed = TraceViewer(files).group_by("name").agg("count", "sum:dur").collect_typed()
   regular    = typed["regular"]      # ph="X" events
   aggregated = typed["aggregated"]   # aggregated records (incl. extra-key dims)
   counters   = typed["counters"]     # ph="C" counters (incl. system)

``collect_typed`` runs now; ``typed(...)`` is its lazy form, a
:class:`~dftracer.utils.LazyResult`.

Other terminals: ``stream(batch_size=65536)`` yields
:class:`~dftracer.utils.DataFrame` chunks of about ``batch_size`` rows for
out-of-core reads; ``statistics()`` returns a summary dict;
``sink_json(path)`` writes the selected events as NDJSON and
``export_trace(path)`` writes a trace file (optionally re-compressed and
re-indexed), both returning the scan stats dict; ``column_info()`` lists the
columns and their types from the index (no scan, see below).
``statistics``, ``sink_json`` and ``materialize`` take ``lazy=True`` to return
a :class:`~dftracer.utils.LazyResult` instead of running now.

Lazy indexing mirrors :class:`~dftracer.utils.LazyFrame`: ``view["dur"]`` is a
column expression bound to the plan whose reductions return a
:class:`~dftracer.utils.LazyScalar`, ``view[["name", "dur"]]`` selects,
``view[expr]`` filters, and ``view[2:10]`` slices:

.. code-block:: python

   mean_dur = view["dur"].mean().collect()      # a Python float
   rows = view[view["dur"] > 1000][["name", "dur"]][:10].collect()

Several roots run together with :func:`~dftracer.utils.collect_all`, which
returns one value per root in order; plans over the same trace share one scan:

.. code-block:: python

   from dftracer.utils import collect_all

   by_name, flame, stats = collect_all([
       view.group_by("name").agg("count", "sum:dur"),
       view.flamegraph(),
       view.sink_json("posix.json", lazy=True),
   ])

Inspecting the schema
---------------------

``column_info()`` maps every column discoverable from the index to its type
(``"int64"`` / ``"float64"`` / ``"string"``). It reads index metadata only - no
trace scan - so it is cheap and does not need the whole trace materialized the
way ``collect().columns`` does (which also only sees the columns present in
the collected rows).

As on any :class:`~dftracer.utils.LazyFrame`, the ``columns`` and ``schema``
properties describe the plan's output instead: its column names, and a
``{name: DType}`` mapping, without running.

.. code-block:: python

   v = TraceViewer(files)
   v.column_info()   # {'dur': 'int64', 'hostname': 'string', 'pos.x': 'int64', ...}
   v.group_by("name").agg("count").columns   # ['name', 'count']

The set is schemaless: the base axis fields (``pid`` / ``tid`` / ``ts`` /
``dur``), every scalar leaf harvested at index build (top-level fields plus flat
and nested args), and a ``resolved.*`` alias for each hash column present. A
nested-object arg surfaces as its dotted leaf columns (``pos.x``, ``pos.y``) and
an array as its first element (``tags.0``), so no field is silently dropped. The
type is harvested once per event name and folded across names and files
(numeric widens to ``float64``; any mix with a string widens to ``string``).

Materialized views
-------------------

``materialize()`` persists a query so a later matching read reuses it instead of
re-scanning. A row query writes a filtered, re-split trace; an aggregation
persists a rollup. It is idempotent and returns the scan stats dict, and
``mv_source()`` reports which
materialized file(s) would serve the current query (empty if a read would scan
the base).

.. code-block:: python

   v = TraceViewer(files).filter('cat == "POSIX"')
   v.materialize()                 # build once
   v.collect()                     # served from the materialized view

Call trees and flamegraphs (Containment)
----------------------------------------

Three terminals fold events by their ``[ts, ts + dur)`` containment within each
lane (rows sharing ``partition``, ``("pid", "tid")`` by default; ``ts`` /
``dur`` / ``name`` name the interval and label columns):

- ``call_tree(partition, ts, dur, name)`` returns a
  :class:`~dftracer.utils.LazyFrame` of the events plus ``level`` and
  ``parent_id`` (the containing event per lane).
- ``flamegraph(partition, ts, dur, name)`` returns a
  :class:`~dftracer.utils.LazyFrame` that folds events by root-to-node
  ``name`` path, one row per node: ``node_id``, ``parent``, ``name``,
  ``level``, ``total`` (inclusive), ``self`` (exclusive), ``count``.
- ``containment(partition, ts, dur, name)`` returns a
  :class:`~dftracer.utils.LazyResult` that buffers one fold; its ``collect()``
  gives a :class:`~dftracer.utils.Containment` whose ``call_tree`` and
  ``flamegraph`` fields hold both frames from the shared scan - cheaper than
  collecting both terminals separately.

These terminals take the trace scan and filters only; one after an op they
cannot take (such as ``sort_by``) raises naming that op. Sort or cut the
result after the terminal instead.

.. code-block:: python

   c = TraceViewer("traces/").containment().collect()
   tree = c.call_tree            # events + level, parent_id
   flame = c.flamegraph          # node_id, parent, name, level, total, self, count

``flamegraph`` / ``containment`` / ``flamegraph_partial`` also take a ``group``
key - any field(s) - that roots the tree by that value over the raw events, so
each group gets its own top-level subtree named by its value. This is *not* the
aggregation ``group_by`` (which would collapse the events the tree is built
from); it is a per-event rooting. ``group=("pid",)`` gives a per-process
flamegraph, ``group=("cat",)`` a per-category one, ``group=("host", "pid")`` a
nested one:

.. code-block:: python

   flame = TraceViewer("traces/").flamegraph(group=("cat",)).collect()
   # top-level nodes are "POSIX", "STDIO", ... each holding that layer's tree

For a distributed flamegraph, ``flamegraph_partial(partition, ts, dur, name)``
returns a :class:`~dftracer.utils.LazyResult` whose ``collect()`` scans one
rank's files into a serialized partial (``bytes``); partition by ``pid``
so each lane lives on one rank. Gather the partials (MPI all-gather or Dask) and
reduce them with the static
``TraceViewer.merge_flamegraph_partials(partials)``, which needs no scan or
viewer and returns the final node DataFrame:

.. code-block:: python

   part = TraceViewer(my_files).flamegraph_partial(partition=("pid",)).collect()
   # ... gather every rank's `part` bytes ...
   nodes = TraceViewer.merge_flamegraph_partials(all_partials)   # on rank 0

Sessions
--------

``view.session()`` returns a :class:`~dftracer.utils.Session`. Plans and lazy
results registered on it run together, as :func:`~dftracer.utils.collect_all`
runs them, at the end of the ``with`` block or on the first
:meth:`Handle.result <dftracer.utils.Handle.result>`:

.. code-block:: python

   with view.session() as s:
       by_cat = s.collect(view.group_by("cat").agg("count", "mean:dur"))
       tree = s.collect(view.containment())
       stats = s.sink_json(view.filter('cat == "POSIX"'), "posix.json")
   by_cat.result()                  # DataFrame
   tree.result().flamegraph         # Containment field

``s.materialize(viewer)`` and ``s.attach(plugins)`` register a materialization
and a plugin set the same way. See :doc:`../guides/analysis/sessions`.

Distributed use
---------------

For Dask, :class:`~dftracer.utils.dask.DaskTraceViewer` fans the same builder
API across workers; the dfanalyzer bridge builds its high-level metrics on
:class:`~dftracer.utils.dfanalyzer.DFAnalyzerAggregatedTraceViewer`, a subclass
that composes the HLM as one View aggregation. See :doc:`dfanalyzer`.

Reference
---------

Type relationships
------------------

How the viewer types relate and what they return:

.. mermaid:: /_generated/py_trace_viewer.mmd

.. autoclass:: dftracer.utils.TraceViewer
   :members:
   :undoc-members:

.. autoclass:: dftracer.utils.Containment
   :members:
   :undoc-members:

.. autoclass:: dftracer.utils.Session
   :members:
   :undoc-members:

.. autoclass:: dftracer.utils.Handle
   :members:
   :undoc-members:

.. autoclass:: dftracer.utils.LazyResult
   :members:
   :undoc-members:

.. autoclass:: dftracer.utils.LazyScalar
   :members:
   :undoc-members:

.. autofunction:: dftracer.utils.collect_all
