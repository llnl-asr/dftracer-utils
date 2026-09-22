:description: Troubleshoot a correct but slow View query: instrument where the time goes, then pick the tuning knob that fixes the slow stage.

Diagnose a slow query
=======================

.. admonition:: Goal
   :class: goal

   A ``TraceViewer``/``View`` query returns correct results but takes
   longer than expected, and you need to work out which knob fixes it. This is a
   troubleshooting flow through the tuning surface documented in depth elsewhere
   (:doc:`../runtime/performance`, :doc:`../runtime/memory-budget`); it does not
   re-explain those knobs, only which one to reach for first.

Instrument first: see where the time goes
-----------------------------------------------

Before reaching for a knob, turn on instrumentation so you tune the stage that
is actually slow rather than guessing. These are environment variables read at
process start (full list: :doc:`../../environment`):

- ``DFTRACER_UTILS_MONITOR=tree`` prints a per-task timing tree to stderr when
  the run finishes; ``deep`` and ``trace`` add more detail, ``summary`` collapses
  it. ``DFTRACER_UTILS_MONITOR_FILE=run.csv`` writes the same data as CSV, and
  ``DFTRACER_UTILS_MONITOR_MIN_US`` hides tasks below a duration so the slow
  stage stands out.
- ``DFTRACER_UTILS_LOG_LEVEL=debug`` surfaces the index resolve/build and scan
  phases in the log, so you can see whether time is going to a one-time index
  build, decompression, or the aggregation itself (see :doc:`../tools/logging`).

.. code-block:: console

   $ DFTRACER_UTILS_MONITOR=tree DFTRACER_UTILS_LOG_LEVEL=debug \
       dftracer_view -d traces/ --query 'cat == "POSIX"'

From Python, ``Runtime.get_progress()`` returns a live snapshot (per-worker
state and per-task durations) you can poll while a run is in flight, without
setting any environment variable. Once the tree or the progress snapshot shows
which stage dominates, the step below tells you the knob for it.

1. Confirm the index exists and pushdown is happening
-----------------------------------------------------------

The single biggest lever is whether the query is reading the index at all. A
query against a directory with no ``.dftindex`` yet pays for a full scan (plus
a one-time index build) the first time; every query after that should be
served from the index.

- Check that a ``.dftindex`` directory exists next to your traces (or at the
  ``index_dir`` you configured). If it does not, and this is not the very
  first query, build it explicitly rather than relying on the bootstrap - see
  :doc:`../core/indexing`.
- Confirm the predicate you filter on is one the index can prune with:
  indexed fields (``cat``, ``name``, ``pid``, ``ts``, ``dur``, and the
  resolved fields) are pushed down at scan time so whole chunks are skipped
  unread; anything else falls back to a SIMD mask over every scanned batch,
  which is correct but does not skip I/O. See "How it runs" in
  :doc:`../core/query-dsl` and the pruning mechanics in
  :doc:`../../concepts/indexing-and-pushdown`.
- If the directory's files were replaced or appended to after the index was
  built, a plain query reads the (now stale) index as-is rather than
  detecting the change - see the staleness note in :doc:`../core/indexing`.
  A query that looks fast but returns wrong-looking results, rather than one
  that is simply slow, points here instead.

2. Narrow with a more selective predicate
-----------------------------------------------

A predicate that touches fewer chunks scans less data, independent of any
other tuning. Push filtering as early as possible in the builder chain (before
``group_by``, not as a post-filter on the collected result) so pruning gets
the chance to skip chunks:

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         # Narrower: prunes chunks outside this pid/time range before scanning.
         view.filter('pid == 1234 and ts >= 1000000 and ts < 2000000')

   .. tab-item:: C++

      .. code-block:: cpp

         view.time_range(1e6, 2e6).filter(F("pid") == 1234);

``.select({...})`` also cuts work once a query is not fully aggregated: it
projects columns before materializing, so a wide row query does not pay to
carry columns you are going to drop anyway. See "Row-shaping builders" in
:doc:`../analysis/views`.

3. Raise the thread count
------------------------------

If the predicate is already selective and the query is still CPU-bound across
many independent chunks, more worker threads parallelize the scan/aggregate
further:

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import Runtime, TraceViewer

         rt = Runtime(threads=16, io_threads=16)
         view = TraceViewer("traces/", runtime=rt)

   .. tab-item:: C++

      .. code-block:: cpp

         Runtime rt(16);   // 0 = hardware_concurrency

Do not raise this blindly on a shared or oversubscribed node - too many
threads hurts more than it helps, and a wider thread count also means more
group maps building in parallel (see step 4). ``DFTRACER_UTILS_THREADS``
overrides any requested count at process start, useful for isolating whether
thread count is even the bottleneck. Full detail: :doc:`../runtime/performance`.

4. Check memory: budget or spill before you add threads
--------------------------------------------------------------

A query that is not CPU-slow but is swapping, or was killed, is a memory
problem, not a thread-count problem - and raising thread count without a
memory budget makes it worse, since more group maps grow in parallel. Cap and
spill instead of scaling threads further:

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         result = (view.memory_budget(4 * 1024**3)
                       .group_by("cat").agg("sum:dur")
                       .collect().collect())
         # or: view.auto_spill().group_by("cat").agg("sum:dur").collect().collect()

   .. tab-item:: C++

      .. code-block:: cpp

         auto result = view.memory_budget(4ull * 1024 * 1024 * 1024)
                            .group_by({GroupKey::cat()})
                            .agg({{AggOp::Sum, "dur", "sum_dur"}})
                            .collect()
                            .collect()
                            .get();

Check a footprint before committing to a run with ``memory_budget_advice`` -
it tells you whether a required byte footprint fits and how many nodes to
spread it across if not. Full detail, including the C++-only
``compute_memory_budget``/``estimate_per_file_bytes`` helpers:
:doc:`../runtime/memory-budget`.

5. When to materialize instead of re-scanning
----------------------------------------------------

If the same filtered view (or the same aggregation rollup) gets queried
repeatedly - a dashboard re-running the same shape, or a batch job re-deriving
the same rollup - stop paying the scan cost every time. ``.materialize()``
persists a query's result (a filtered trace for a row query, a rollup for an
aggregation) so a later matching query is served from it instead of
rescanning; ``.run()`` is the build-only form when you only want the side
effect. This trades one-time build cost for repeated-query speed - reach for
it once step 1-4 confirm the per-query cost itself is already as low as it
can go, not as a first move. See "Materialized views" in
:doc:`../analysis/views` and :doc:`../analysis/aggregation`.

See also
--------

- :doc:`../runtime/performance` - the full tuning reference (threads,
  ``parallel_for``/``parallel_reduce``) this page draws from.
- :doc:`../runtime/memory-budget` - the full memory-budget/spill reference.
- :doc:`../../concepts/indexing-and-pushdown` - how pruning actually decides
  which chunks to skip.
- :doc:`../troubleshooting` - wrong-result symptoms, as opposed to slow-but-correct.
