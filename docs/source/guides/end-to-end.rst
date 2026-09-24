:description: Follow one pass from raw .pfw.gz traces to an indexed, filtered, aggregated, derived-column result as a pandas/Arrow table or HTTP endpoint.

From raw traces to a result: the full workflow
=================================================

.. admonition:: Goal
   :class: goal

   See how the pieces documented separately elsewhere fit into one pass -
   point at a directory of raw ``.pfw.gz`` traces, get an indexed, filtered,
   aggregated, derived-column result out the other end, as a pandas/Arrow table
   or a served HTTP endpoint. This page is a map with the minimal code at each
   step; for the full option set at any step, follow the linked guide.

Start with :doc:`choosing-an-api` if you have not settled on ``TraceViewer``
vs ``View`` vs a plugin vs the C ABI yet - this page assumes the
``TraceViewer``/``View`` path, the common case.

1. Point at a directory of traces
-----------------------------------

A ``TraceViewer``/``View`` accepts a directory, a single file, or a list of
files. A directory is scanned recursively for ``.pfw.gz`` files only - plain
``.pfw`` is not picked up (see :doc:`troubleshooting` if that scan turns up
empty).

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer

         view = TraceViewer("traces/")

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>
         using namespace dftracer::utils::trace::views;

         View view = View::from_directory("traces/").get();  // coroutine; .get() blocks main()

Full detail: :doc:`analysis/views`.

2. Index (usually automatic)
-------------------------------

You do not have to index explicitly before querying: the first aggregation
query against a genuinely fresh directory builds the index (checkpoints,
bloom filters) as a byproduct of answering itself. Index explicitly instead
when you want to warm the index ahead of a user-facing query, or when the
directory's files may have been replaced or appended to since it was last
indexed (a plain query does not detect that on its own).

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import Indexer

         with Indexer("traces/") as ix:
             ix.ensure_indexed()   # no-op if already built

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/trace/indexing/resolve_and_build.h>
         using namespace dftracer::utils::trace::indexing;

         ResolveAndBuildInput input;
         input.files = {"trace.pfw.gz"};
         auto result = resolve_and_build_index(scope, input);  // co_await inside a coroutine

Full detail, including the bootstrap and staleness rules: :doc:`core/indexing`.

3. Filter and aggregate
--------------------------

Build a predicate with the query DSL and roll matching events up with
``group_by``/``agg``. The predicate is pushed down to the index at scan time,
so only candidate chunks are decompressed.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         df = (
             view.filter('cat == "POSIX" and dur >= 1000')
                 .group_by("name")
                 .agg("count", "sum:dur", "mean:dur")
                 .collect()
         )

   .. tab-item:: C++

      .. code-block:: cpp

         using namespace dftracer::utils::query;   // for Field

         auto df = view.filter((Field("cat") == "POSIX" && Field("dur") >= 1000).build().value())
                        .group_by({GroupKey::name()})
                        .agg({{AggOp::Count, "", "count"},
                              {AggOp::Sum, "dur", "sum_dur"},
                              {AggOp::Mean, "dur", "mean_dur"}})
                        .collect()
                        .get();

Full detail: :doc:`core/query-dsl` (predicates), :doc:`analysis/aggregation`
(group-by/agg vocabulary).

4. Derive columns
--------------------

The result of step 3 is a native ``DataFrame``. Compute a new column from
existing ones without leaving the SIMD engine.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import F

         df = df.with_column("bytes_per_us", (F.sum_dur / F.count).apply(df))

   .. tab-item:: C++

      .. code-block:: cpp

         Series bytes_per_us = df.column("sum_dur") / df.column("count");
         df = df.with_column("bytes_per_us", bytes_per_us);

Full detail: :doc:`core/columnar-ops`, :doc:`data/dataframe`, :doc:`data/series`.

5. Get the result out: export or serve
------------------------------------------

Either convert the ``DataFrame`` to another tool's native type at the edge, or
skip the local script entirely and serve queries over HTTP.

.. tab-set::

   .. tab-item:: Python (export)

      .. code-block:: python

         pdf = df.to_pandas()   # or .to_arrow(), .to_polars()

   .. tab-item:: C++ (export)

      .. code-block:: cpp

         #include <dftracer/utils/dataframe/arrow.h>

         using dftracer::utils::dataframe::OwnedArrow;

         OwnedArrow a = df.to_arrow();   // struct array, one child per column

   .. tab-item:: CLI (serve)

      .. code-block:: console

         $ dftracer_server -d traces/
         # then query the REST API, or open http://127.0.0.1:8080/

Full detail: :doc:`analysis/export` (pandas/polars/Arrow), :doc:`serving/http-server`
and :doc:`../trace-viewer` (interactive UI over the same index).

If something goes wrong along the way
------------------------------------------

- Empty results, a stale-looking index, or a predicate the index cannot prune: :doc:`troubleshooting`.
- A correct but slow query: :doc:`analysis/diagnosing-slow-queries`.

See also
--------

- :doc:`choosing-an-api` for picking a different entry point than
  ``TraceViewer``/``View`` (a plugin, the C ABI, MPI at scale).
- :doc:`../concepts/architecture` for how these stages compose internally.
