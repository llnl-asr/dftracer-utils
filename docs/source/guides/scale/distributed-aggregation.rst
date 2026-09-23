:description: Aggregate a sharded trace set without a coordinator rescan: each rank emits a combinable partial and the coordinator merges them.

Combine aggregations across ranks
==================================

.. admonition:: Goal
   :class: goal

   Aggregate a trace set split across N ranks (MPI, dask, or any transport
   you own) without a coordinator rescanning the data. Each rank aggregates its
   own file shard into a small, opaque, combinable partial; the coordinator
   merges the partials into the final result.

This is the fan-out/fan-in pattern behind ``dftracer_view``'s distributed
``--group-by``/``--agg``/``--counters`` modes (see
``src/dftracer/utils/binaries/dftracer_view.cpp``). ``View`` carries no MPI (or
any transport) dependency itself - moving the partial bytes between ranks is
the caller's job.

The pattern
-----------

1. Each rank builds a ``View``/``AggregatedView`` over its own file slice, with
   the same ``group_by``/``agg`` plan as every other rank.
2. Each rank calls ``aggregate_partial()``, which scans its slice once and
   returns an opaque serialized partial (running count/sum/sumsq/sketch
   accumulators, not finalized values - so merging is exact for mean, stddev,
   and percentiles, unlike re-combining already-finalized per-rank numbers).
3. The partials travel to a coordinator (MPI, dask, a shared file, ...).
4. The coordinator combines them with **one** of:

   - ``merge_partials_to_table(partials)`` - a materialized-collect result, as
     a ``DataFrame``.
   - ``merge_counter_partials(partials, sink)`` - the streamed ``ph="C"``
     counter form, written to an ``ExportSink`` (C++-only; no Python binding).

No rank rescans the trace files to produce the combined result.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>

         using namespace dftracer::utils::trace::views;

         // Each rank builds the same group_by/agg plan over its own shard of
         // files (partition files across ranks however your job scheduler does).
         AggregatedView shard_view = View::from_files(my_rank_files)
                                         .group_by({GroupKey::name()})
                                         .agg({{AggOp::Count, "", "count"},
                                               {AggOp::Sum, "dur", "sum_dur"}});

         std::string partial = shard_view.aggregate_partial().get();
         // send `partial` to the coordinator over your transport (MPI, etc.)

         // On the coordinator, once every rank's partial has arrived:
         std::vector<std::string_view> partials = /* gathered from every rank */;
         View merger = View::from_files(my_rank_files)   // same plan shape
                           .group_by({GroupKey::name()})
                           .agg({{AggOp::Count, "", "count"},
                                 {AggOp::Sum, "dur", "sum_dur"}});
         dftracer::utils::dataframe::DataFrame table =
             merger.merge_partials_to_table(partials);

      For the streamed counter-trace form, merge into an ``ExportSink`` instead.
      ``ExportSink`` is abstract (just ``write(std::string_view)``); implement a
      small one, or write to a file/gzip sink - see :doc:`../analysis/views`:

      .. code-block:: cpp

         struct StringSink : ExportSink {
             std::string data;
             void write(std::string_view chunk) override { data.append(chunk); }
         };
         StringSink sink;
         ExportStats stats = merger.merge_counter_partials(partials, sink);

   .. tab-item:: Python

      ``merge_counter_partials`` has no Python binding; use
      ``merge_partials_to_table`` for the materialized form.

      .. code-block:: python

         from dftracer.utils import TraceViewer

         # Each rank: aggregate its own file slice, get back combinable bytes.
         shard = TraceViewer(my_rank_files).group_by("name").agg("count", "sum:dur")
         partial = shard.aggregate_partial()   # bytes
         # send `partial` to the coordinator over your transport

         # Coordinator, once every rank's partial has arrived:
         merger = TraceViewer(my_rank_files).group_by("name").agg("count", "sum:dur")
         table = merger.merge_partials_to_table(all_partials)   # DataFrame

The merging view's ``group_by``/``agg`` plan must match the shape the partials
were produced with; a mismatched plan produces a meaningless (or empty)
result. The files passed to the merging view are not scanned - they are only
needed to resolve schema/index context - so any view built with the same plan
over the same base file set works as the merge target.

Distributed materialized-rollup terminals
------------------------------------------

When the aggregation should also be **persisted** (so a later matching
``collect()`` reads it back instead of rescanning), use the rollup pair
instead of a plain in-memory merge:

- ``materialize_partials(partials)`` (C++: protected on ``View``, public on
  ``AggregatedView``; Python: ``AggregatedTraceViewer.materialize_partials``) -
  reduces the gathered partials and writes the rollup, without any rank
  rescanning.
- ``reconstruct_if_cached()`` - reads a subsuming rollup back as a native
  ``DataFrame``, or returns ``std::nullopt``/``None`` on a cache miss, so a
  caller can choose "read the cached rollup" vs. "recompute" without running a
  scan to find out.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         AggregatedView v = View::from_files(files)
                                 .group_by({GroupKey::name()})
                                 .agg({{AggOp::Count, "", "count"}});

         if (auto cached = v.reconstruct_if_cached()) {
             use(*cached);          // served from the rollup, no scan
         } else {
             v.materialize_partials(partials).get();   // coordinator only
         }

   .. tab-item:: Python

      .. code-block:: python

         v = TraceViewer(files).group_by("name").agg("count")

         cached = v.reconstruct_if_cached()
         if cached is not None:
             use(cached)             # DataFrame, no scan
         else:
             v.materialize_partials(all_partials)   # coordinator only

Reading a distributed aggregation index in shard ranges
---------------------------------------------------------

``collect_typed(shard_begin, shard_end, progress)`` reads an already-built
aggregation index's three record families (``regular``, ``aggregated``,
``counters``) in one pass over a shard range, so distributed callers can fan
disjoint shard ranges across workers and concatenate the results instead of
each worker reading the whole index. Counters are only read when the range
starts at shard 0 (they are not shard-partitioned the same way).

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         // Each worker reads a disjoint shard range of the same index.
         TypedResult part = v.collect_typed(/*shard_begin=*/0,
                                            /*shard_end=*/2048).get();
         // part.regular / part.aggregated / part.counters (DataFrame each)

   .. tab-item:: Python

      .. code-block:: python

         result = v.collect_typed(shard_begin=0, shard_end=2048)
         result["regular"]      # DataFrame
         result["aggregated"]   # DataFrame
         result["counters"]     # DataFrame (only populated when shard_begin == 0)

``shard_end`` defaults to covering every shard (4096 in C++; ``<= 0`` in
Python). ``progress`` is a ``(done, total)`` callback over shard units.

See also
--------

- :doc:`../analysis/views` - the ``View``/``AggregatedView`` builder and
  terminal reference this guide's fan-in pattern sits on top of.
- :doc:`distributed-index` - build and query an index across ranks/workers;
  ``ShardedView::aggregate`` is the in-process form of this same partial
  fan-in, run over immutable index shards.
- :doc:`mpi` - the one distributed CLI binary in the tree
  (``dftracer_view --flamegraph``); this guide's pattern is transport-agnostic
  and not tied to it.
