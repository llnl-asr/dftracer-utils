:description: Build a lazy, composable View over trace events and run it in one pass: filter, aggregate, export, or fold with map_batches and session.

Query traces with the View engine
====================================

.. admonition:: Goal
   :class: goal

   Build a lazy, composable query over trace events and run it - filter,
   aggregate, export, or fold with your own logic - in a single pass over the
   data. ``View`` (C++, ``dftracer/utils/trace/views/view.h``) is the engine;
   ``TraceViewer`` (Python) is its wrapper. Trace inputs are gzip-compressed
   ``.pfw.gz`` files; plain ``.pfw`` is not supported.

For the group-by/aggregate vocabulary in depth, see :doc:`aggregation`; this
page covers building a view, its terminals, and the lower-level escape
hatches (``map_batches``, ``session``) that go beyond ``group_by``/``agg``.

Build a view
------------

Builder methods (``filter``, ``query``, ``phase``, ``time_range``, ...) are
lazy and return a new view; they do no I/O until a terminal runs.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/dataframe/field.h>
         #include <dftracer/utils/trace/views/view.h>
         using namespace dftracer::utils::trace::views;
         using namespace dftracer::utils::dataframe::field;

         // One file, with an explicit index path (empty = sidecar convention).
         View v = View::from_file("trace.pfw.gz");

         // Or scan a directory recursively for .pfw.gz files (a coroutine).
         View v = View::from_directory("traces/").get();

         // The unified F: filter() takes the expression directly (pushdown).
         View filtered = v.filter((F("cat") == "POSIX") && (F("dur") >= 1000));
         // or a Query built from a DSL string:
         View also = v.filter(query::Query::from_string(R"(cat == "POSIX")").value());
         // or the DSL string form:
         View str_form = v.query(R"(cat == "POSIX" and dur >= 1000)");

   .. tab-item:: Python

      ``TraceViewer`` is the Python wrapper over the same engine; see
      :doc:`../../api/trace_viewer` for its full builder/terminal reference.

      .. code-block:: python

         from dftracer.utils import TraceViewer

         view = TraceViewer("traces/")                 # directory, file, or list of files
         filtered = view.filter('cat == "POSIX" and dur >= 1000')

``filter`` and ``fold`` accept the unified ``F`` expression
(``<dftracer/utils/dataframe/field.h>``) directly - the pushdown predicate is
derived via ``.to_query()`` - as well as a ``Query`` built from a DSL string
with ``Query::from_string(...)``. The same ``F`` also builds value/derived
columns (``.apply(df)``); a predicate that mixes in value ops is not pushable
and ``filter`` throws. Plugins instead use the predicate-only ``query::F``
(``<dftracer/utils/query/builder.h>``) finished with ``Expr::build()``, which
links no dataframe engine. See :doc:`../core/query-dsl` for the builder surface.

Row-shaping builders: ``.phase(p)`` restricts to one ``Phase`` value -
``Phase::Events`` (``ph="X"`` events), ``Phase::Counters`` (``ph="C"``
counters), ``Phase::Aggregated`` (rollup records), ``Phase::Metadata``
(``ph="M"``), or ``Phase::Any``; ``.time_range(begin,
end)`` and ``.time_bucket(interval_us, origin)`` window and bucket by timestamp
(``origin`` anchors the windows; Python's ``normalize_to="min"`` anchors on
the first event's timestamp instead of ``0``);
``.select({...})`` projects columns; ``.limit(n)`` / ``.offset(n)`` paginate;
``.sort_by(column, descending)`` / ``.topk(column, k)`` order the result. All
return a new ``View`` (or, in Python, a new ``TraceViewer``).

``.time_scale(ns_ratio)`` normalizes ``ts``/``dur``/``te`` by
``source_ns_per_unit / target_ns_per_unit`` (``1.0`` = no change), applied
before ``time_bucket``; callers resolve the trace's native unit and the
target themselves. Python adds ``.time_unit("ns"|"us"|"ms"|"sec")``, a
higher-level helper that reads the trace's native time unit from its first
file and derives the ``time_scale`` ratio for you:

.. code-block:: python

   view = TraceViewer("traces/").time_unit("us")   # normalize to microseconds

Aggregate
---------

``.group_by({...})`` and ``.agg({...})`` promote a ``View`` to an
``AggregatedView`` (in Python both return a ``TraceViewer``); see
:doc:`aggregation` for the full ``GroupKey`` / ``AggOp`` vocabulary. The
terminal that runs everything built so far is ``.collect()``:

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         auto df = View::from_file("trace.pfw.gz")
                       .filter(Query::from_string(R"(cat == "POSIX")").value())
                       .group_by({GroupKey::name()})
                       .agg({{AggOp::Count, "", "count"},
                             {AggOp::Sum, "dur", "sum_dur"}})
                       .collect()      // coro::CoroTask<DataFrame>
                       .get();         // block until done (non-coroutine caller)

   .. tab-item:: Python

      .. code-block:: python

         df = (
             TraceViewer("traces/")
             .filter('cat == "POSIX"')
             .group_by("name")
             .agg("count", "sum:dur")
             .collect()               # -> DataFrame
         )

In C++, ``collect()`` returns a native ``dftracer::utils::dataframe::DataFrame``
directly. In Python, ``TraceViewer`` is itself a lazy ``LazyFrame``; nothing
scans until ``collect()``, which runs the plan and returns the ``DataFrame``.
See
:doc:`../data/dataframe` and :doc:`../core/columnar-ops` for what to do with
the frame next.

Inspecting the schema
---------------------

``columns()`` returns the distinct columns discoverable from the view's index
and ``schema()`` returns each with its type. Both read index metadata only (no
trace scan) and read the per-index metadata in parallel:

.. code-block:: cpp

   View v = View::from_file("trace.pfw.gz");
   std::vector<std::string> cols = v.columns();
   for (const View::ColumnInfo& c : v.schema())
       std::printf("%s: %s\n", c.name.c_str(), c.type.c_str());

The set is schemaless: the base axis fields (``pid`` / ``tid`` / ``ts`` /
``dur``), every scalar leaf harvested at index build (top-level fields plus flat
and nested args as dotted paths, e.g. ``pos.x``), and a ``resolved.*`` alias for
each hash column present. Types fold across event names and files. The harvest
happens in the one index-building pass (``BloomFold``, ``wants_schema()``), so
it costs no extra scan. In Python, ``TraceViewer.column_info()`` returns the
same set as a ``{name: type}`` dict; the ``columns`` and ``schema``
properties describe the plan's output instead, as on any ``LazyFrame``.

Export
------

``export_json`` streams matching events verbatim as newline-delimited JSON to
an ``ExportSink`` (a re-indexable dftracer trace if the sink is a file/gzip
writer). ``export_trace`` writes a new multi-member, re-indexable trace
through the parallel writer - the engine behind ``dftracer_view --merge``.
``export_counters`` runs the view's ``group_by`` +
``agg`` and emits each result row as a ``ph="C"`` counter event, which with a
``time_bucket`` set produces the aggregator's counter-trace format:

.. code-block:: cpp

   class StringSink : public ExportSink {
    public:
     void write(std::string_view data) override { buf += data; }
     std::string buf;
   };

   StringSink sink;
   View::from_file("trace.pfw.gz")
       .phase(Phase::Counters)
       .group_by({GroupKey::name()})
       .agg_numeric_args()
       .export_counters(sink)
       .get();

In Python, ``TraceViewer.sink_json(path)`` streams the selected events as
NDJSON and ``TraceViewer.export_trace(path)`` writes a trace file (counter
events when the viewer aggregates); both return the scan stats dict. See
:doc:`../../api/trace_viewer`.

Custom folds: map_batches
--------------------------

``group_by`` + ``agg`` cover the built-in aggregate vocabulary. When a
computation needs its own shape - a custom split, a representative value, a
side output - ``map_batches`` runs your fold over the view's index-pruned
parallel scan without forcing it into that vocabulary. It is C++-only (no
Python binding): ``fold`` mutates one of ``num_slots`` per-worker partials
from each decoded batch of raw event lines, and ``combine`` reduces the
partials into one:

.. code-block:: cpp

   struct Acc {
       std::size_t small = 0;
       double small_sum = 0;
       std::vector<std::string> big;   // events kept verbatim
   };

   auto res =
       View::from_file("trace.pfw.gz")
           .map_batches<Acc>(
               [](Acc& a, const std::vector<std::string_view>& events) {
                   for (auto e : events) {
                       double d = dur_of(e);           // caller-owned parse
                       if (d < 25) { ++a.small; a.small_sum += d; }
                       else a.big.emplace_back(e);
                   }
               },
               [](Acc&& x, Acc&& y) {
                   x.small += y.small;
                   x.small_sum += y.small_sum;
                   for (auto& b : y.big) x.big.emplace_back(std::move(b));
                   return std::move(x);
               },
               /*num_slots=*/4)
           .get();          // BatchResult<Acc>: .value and .stats (ExportStats)

``P`` (here ``Acc``) must be default-constructible, and a default ``P`` must
be the identity for ``combine`` - unused slots fold nothing. ``res.stats``
carries the same ``ExportStats`` (events matched/scanned, chunks skipped,
``truncated``) every terminal reports. When even the partial/combine shape is
too much structure - a fold that keeps one big shared accumulator merged by
hand - use the lower-level ``for_each_batch`` terminal instead.

Sharing one scan: ViewSession
-------------------------------

Each terminal above runs its own scan. When several reads should share a
single pass over the same base view - a custom fold alongside a built-in
aggregate, or several unrelated aggregates at once - open a
``ViewSession``. Register ops (``collect``, ``materialize``, ``fold``,
``export_json``); each returns a ``Deferred<T>`` handle that resolves only
once; then call ``execute()`` to run every registered op together. ``join`` and
``compare`` combine two ``collect`` handles that group the same way after the
one scan (their shared key width is inferred), returning a ``Deferred`` like any
other op:

.. code-block:: cpp

   auto run = View::from_file("trace.pfw.gz").session();

   auto small = run.fold<Small>(
       F("dur") < 25,   // or Query::from_string("dur < 25").value()
       [](Small& s, const json::JsonValue& jv, std::string_view) {
           ++s.n;
           s.sum += jv["dur"].get<double>(0);
       },
       [](Small&& a, Small&& b) { a.n += b.n; a.sum += b.sum; return std::move(a); });

   auto posix = run.collect(
       Query::from_string(R"(cat == "POSIX")").value(), {GroupKey::cat()},
       {{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "sum_dur"}});

   auto stats = run.execute().get();   // runs the one shared scan

   small->n;        // Deferred<T>::get() (or operator*/->) after execute()
   posix->num_rows();

Reading a ``Deferred`` handle before ``execute()`` resolves it throws. From
Python the same fused scan is ``TraceViewer.session()``: ``s.collect(plan)``
registers any lazy plan over the view (an aggregation, a row filter, a
``call_tree`` / ``flamegraph`` plan, the ``containment()`` result), and
``s.sink_json(viewer, path)``, ``s.materialize(viewer)`` and
``s.attach(plugins)`` register the other branches, each returning a ``Handle``
resolved on ``execute()``. :func:`~dftracer.utils.collect_all` shares the scan
the same way without a session. See :doc:`sessions` and
:doc:`../../api/trace_viewer`.

Materialized views
-------------------

``.materialize(checkpoint_size, part_size)`` persists a query's result (a
filtered trace for a row query, a rollup for an aggregation) so a later
matching query is served from it instead of rescanning. ``.run()`` is the
build-only form (side effect only, no result). See :doc:`aggregation` and
:doc:`../../api/trace_viewer` for the Python equivalent.

Distributed row-materialize coordinator protocol
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A row-query materialized view (a filtered trace, not an aggregation rollup)
can also be built by several ranks writing into a shared directory, with one
coordinator publishing the result:

1. The coordinator (a ``View`` over the **full** file set, with the query's
   filters/plan applied) calls ``materialize_dir()`` to create and return the
   shared materialized-view directory (empty if the view has no views
   anchor - see ``.views_root()``).
2. Each rank exports its own slice of files' filtered events into a subdir of
   that directory, e.g. via ``export_trace`` with ``build_index`` set, at
   ``<dir>/shard-<rank>/part.pfw.gz``.
3. Once every rank has finished, the coordinator calls
   ``register_materialized(dir)`` to write the manifest describing the full
   base file set. A later matching read gathers the parts across all subdirs.

.. code-block:: cpp

   // Coordinator: full file set, same query plan every rank will export.
   View coordinator = View::from_files(all_files).filter(q);
   std::string mv_dir = coordinator.materialize_dir();   // shared directory

   // Each rank (files_for_rank is that rank's slice of all_files):
   View::from_files(files_for_rank)
       .filter(q)
       .export_trace(TraceWriteOptions{
           .output_path = mv_dir + "/shard-" + std::to_string(rank) + "/part.pfw.gz",
           .build_index = true,
       })
       .get();

   // Coordinator, after every rank has finished exporting:
   coordinator.register_materialized(mv_dir);

In Python the same steps are ``TraceViewer.materialize_dir()``,
``TraceViewer.export_trace(path, index=True)`` and
``TraceViewer.register_materialized(dir)``. Either way it is a building block
for a caller-driven distributed materialize (the caller owns rank coordination
and the barrier between steps 2 and 3, same as the distributed patterns in
:doc:`../scale/distributed-aggregation`).

See also
--------

- :doc:`aggregation` for ``GroupKey`` / ``AggOp`` in full.
- :doc:`../data/dataframe` and :doc:`../core/columnar-ops` for what to do
  with a collected ``DataFrame``.
- :doc:`../../api/trace_viewer` for the complete Python ``TraceViewer`` /
  ``Session`` reference.
- :doc:`../../concepts/fused-scan` for why one pass serves every terminal
  above.
