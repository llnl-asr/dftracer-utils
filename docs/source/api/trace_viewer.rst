:description: Reference for TraceViewer, the lazy Arrow-first API to filter, group, aggregate, and collect DFTracer traces in a single indexed pass.

TraceViewer (Querying Traces)
=============================

``TraceViewer`` is the primary API for querying DFTracer traces. It is an
Arrow-first, lazy, composable view: builder methods (``filter``, ``group_by``,
``agg``, ...) each return a new view and do no work. ``collect`` builds the
query plan into a :class:`~dftracer.utils.LazyFrame` (still no scan); its own
``.collect()`` runs the plan in a single pass and returns a
:class:`~dftracer.utils.DataFrame`. Other terminals (``collect_typed``,
``stream``, ``export_trace``) run the scan directly. When an index exists next
to the traces, the same query is served from the index (chunk pruning,
aggregation tiers, summaries) instead of a full decompress.

.. code-block:: python

   from dftracer.utils import TraceViewer

   # A directory is scanned recursively for .pfw.gz traces, in parallel.
   view = TraceViewer("traces/")           # index_path optional; sidecar used when present

   # Top I/O calls by total time.
   df = (
       view.filter('cat == "POSIX"')
           .group_by("name")
           .agg("count", "sum:dur", "max:dur")
           .collect()                       # -> LazyFrame (no scan yet)
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

Builder methods are lazy and chainable. Row-shaping operators keep a
``TraceViewer``; ``group_by`` / ``agg`` promote to an
:class:`~dftracer.utils.AggregatedTraceViewer` (which preserves its type
through further builder calls).

.. list-table::
   :header-rows: 1
   :widths: 32 68

   * - Method
     - Effect
   * - ``filter(dsl)`` / ``query(dsl)``
     - Keep events matching the :doc:`query DSL <query>` (e.g. ``'dur >= 1000 and cat == "POSIX"'``).
   * - ``phase(name)``
     - Restrict to a record family: ``"events"`` (``ph="X"``), ``"counters"`` (``ph="C"``), ``"aggregated"`` (rollup records), ``"metadata"`` (``ph="M"``), or ``"any"``.
   * - ``time_range(begin, end)``
     - Keep events whose timestamp falls in ``[begin, end)``.
   * - ``time_bucket(interval_us, origin=...)``
     - Bucket ``ts`` into fixed ``interval_us`` windows (a group key for time series). ``origin`` sets the window anchor; ``"min"`` anchors on the first event's timestamp instead of ``0``.
   * - ``time_unit(unit)`` / ``time_scale(ns_ratio)``
     - Interpret/scale the trace's native time unit (see the :doc:`quickstart <../quickstart>`).
   * - ``select(*cols)``
     - Project a subset of columns.
   * - ``limit(n)`` / ``offset(n)``
     - Paginate the result rows.
   * - ``group_by(*keys)``
     - Group by one or more keys (promotes to ``AggregatedTraceViewer``).
   * - ``agg(*specs)``
     - Aggregate with ``op:field`` specs (promotes to ``AggregatedTraceViewer``).
   * - ``memory_budget(nbytes)`` / ``auto_spill()``
     - Bound in-memory aggregation state, spilling to disk past the budget.

Group keys accept the raw event dimensions - ``name``, ``cat``, ``pid``,
``tid``, ``io_cat``, ``acc_pat``, ``fhash``, ``hhash``, ``file_path``,
``file_name``, ``host_name``, ``rank`` (pid resolved to MPI rank via ``PR``
metadata) - plus ``bucket(file_path, 'sub1', 'sub2', ...)``,
which folds a path to the first listed substring it contains (values matching
none fold to an empty string).

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

``collect`` builds the query plan into a :class:`~dftracer.utils.LazyFrame`;
call its own ``.collect()`` to run the plan and get a native
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
       .collect()                        # -> LazyFrame
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

Other terminals: ``stream(batch_size=...)`` yields Arrow batches for
out-of-core reads; ``statistics()`` returns a summary dict; ``export_trace(path)``
writes a filtered trace (optionally re-compressed and re-indexed);
``columns()`` / ``schema()`` list the columns and their types from the index
(no scan, see below).

Inspecting the schema
---------------------

``columns()`` lists the columns discoverable from the index and ``schema()``
maps each to its type (``"int64"`` / ``"float64"`` / ``"string"``). Both read
index metadata only - no trace scan - so they are cheap and do not need the
whole trace materialized the way ``collect().collect().keys()`` does (which
also only sees the columns present in the collected rows).

.. code-block:: python

   v = TraceViewer(files)
   v.columns()   # ['cat', 'dur', 'hostname', 'name', 'pid', 'pos.x', ...]
   v.schema()    # {'dur': 'int64', 'hostname': 'string', 'pos.x': 'int64', ...}

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
persists a rollup. It is idempotent, and ``mv_source()`` reports which
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

- ``call_tree(partition, ts, dur, name)`` scans the view and returns the events
  plus ``level`` and ``parent_id`` (the containing event per lane).
- ``flamegraph(partition, ts, dur, name)`` folds events by root-to-node
  ``name`` path and returns one row per node: ``node_id``, ``parent``,
  ``name``, ``level``, ``total`` (inclusive), ``self`` (exclusive), ``count``.
- ``containment(partition, ts, dur, name)`` scans once and buffers one fold,
  then returns a :class:`~dftracer.utils.dataframe.Containment` whose ``call_tree()`` and
  ``flamegraph()`` give both frames from the shared scan - cheaper than calling
  both terminals separately.

.. code-block:: python

   c = TraceViewer("traces/").containment()
   tree = c.call_tree()          # events + level, parent_id
   flame = c.flamegraph()        # node_id, parent, name, level, total, self, count

``flamegraph`` / ``containment`` / ``flamegraph_partial`` also take a ``group``
key - any field(s) - that roots the tree by that value over the raw events, so
each group gets its own top-level subtree named by its value. This is *not* the
aggregation ``group_by`` (which would collapse the events the tree is built
from); it is a per-event rooting. ``group=("pid",)`` gives a per-process
flamegraph, ``group=("cat",)`` a per-category one, ``group=("host", "pid")`` a
nested one:

.. code-block:: python

   flame = TraceViewer("traces/").flamegraph(group=("cat",))
   # top-level nodes are "POSIX", "STDIO", ... each holding that layer's tree

For a distributed flamegraph, ``flamegraph_partial(partition, ts, dur, name)``
scans one rank's files into a serialized arena (``bytes``); partition by ``pid``
so each lane lives on one rank. Gather the partials (MPI all-gather or Dask) and
reduce them with the static
``TraceViewer.merge_flamegraph_partials(partials)``, which needs no scan or
viewer and returns the final node DataFrame:

.. code-block:: python

   part = TraceViewer(my_files).flamegraph_partial(partition=("pid",))
   # ... gather every rank's `part` bytes ...
   nodes = TraceViewer.merge_flamegraph_partials(all_partials)   # on rank 0

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

.. autoclass:: dftracer.utils.AggregatedTraceViewer
   :members:
   :undoc-members:

.. autoclass:: dftracer.utils.Session
   :members:
   :undoc-members:

.. autoclass:: dftracer.utils.SessionView
   :members:
   :undoc-members:

.. autoclass:: dftracer.utils.dataframe.Containment
   :members:
   :undoc-members:
