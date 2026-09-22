:description: Build the .dftindex sidecar once and keep it fresh so later queries skip decompression, from Python, C++, or the CLI.

Build and use a trace index
============================

Reading a trace cold means decompressing it end to end. An index is a sidecar
built once per trace directory that lets a later query skip most of that work:
see :doc:`../../concepts/indexing-and-pushdown` for what it holds (gzip
checkpoints, per-chunk zone-map statistics, and bloom filters) and how a query
prunes chunks with it. This page is about building one and keeping it fresh,
not the pruning mechanics.

The index lives in a ``.dftindex`` directory next to the trace files it
covers (or wherever you point ``index_dir``); it is a RocksDB store, not a
single file. Building it is idempotent: run the same build again on unchanged
traces and it does nothing, so you can call it at the top of every script
without worrying about redundant work.

.. tab-set::

   .. tab-item:: Python

      ``Indexer`` builds the checkpoint and bloom-filter tiers (and,
      optionally, a pre-aggregated tier). ``ensure_indexed()`` checks what is
      already built and only builds what is missing:

      .. code-block:: python

         import dftracer.utils as dft

         with dft.Indexer(files=["trace.pfw.gz"]) as ix:
             status = ix.ensure_indexed()
             print(status.total_files, status.ready, status.needs_work)

         # Or point at a whole directory:
         with dft.Indexer("traces/") as ix:
             ix.ensure_indexed()

      ``ensure_indexed()`` is ``resolve()`` (check what needs work) followed
      by ``build()`` (build only that) in one call; call ``resolve()`` on its
      own to inspect status without triggering a build. ``force_rebuild=True``
      on the constructor rebuilds every tier regardless of what already
      exists.

      Both ``require_checkpoint`` and ``require_bloom`` default to ``True``.
      Add the pre-aggregated tier with ``require_aggregation``:

      .. code-block:: python

         from dftracer.utils import Indexer, AggregationConfig

         with Indexer(
             "traces/",
             require_aggregation=AggregationConfig(time_interval_ms=1000),
         ) as ix:
             ix.ensure_indexed()  # one fused pass builds all three tiers

   .. tab-item:: C++

      A plain ``View`` query builds the index itself the first time it touches
      a genuinely unindexed file - see :ref:`indexing-first-touch` below. For
      an explicit build (warming the index ahead of serving traffic, or
      refreshing one that already exists), use ``resolve_and_build_index``
      (``dftracer/utils/trace/indexing/resolve_and_build.h``), the same
      resolve-then-build-then-re-resolve path the CLI and Python ``Indexer``
      both drive:

      .. code-block:: cpp

         #include <dftracer/utils/trace/indexing/resolve_and_build.h>

         using namespace dftracer::utils::trace::indexing;

         ResolveAndBuildInput input;
         input.files = {"trace.pfw.gz"};
         input.require_checkpoints = true;
         input.require_bloom = true;

         // scope is a CoroScope*, available inside a Runtime-driven coroutine
         // (see ../runtime/task-graphs); co_await the returned task there.
         coro::CoroTask<ResolverResult> task = resolve_and_build_index(scope, input);

      ``force_rebuild = false`` (the default) makes this idempotent the same
      way the Python and CLI paths are.

   .. tab-item:: CLI

      ``dftracer_index`` builds the index for a directory (or the files it
      discovers under it) without any Python or C++ in your own code:

      .. code-block:: console

         $ dftracer_index --directory traces/
         $ dftracer_index --directory traces/ --force   # rebuild every tier

      Selected flags (see ``dftracer_index --help`` for the full list):

      .. list-table::
         :header-rows: 1
         :widths: 32 68

         * - Flag
           - Meaning
         * - ``-d`` / ``--directory``
           - Directory to scan for ``.pfw``/``.pfw.gz`` files (default ``.``)
         * - ``--index-dir``
           - Directory for the ``.dftindex`` store (default: next to the data)
         * - ``-f`` / ``--force``
           - Force index recreation even if already built
         * - ``--checkpoint-size``
           - Checkpoint size for gzip indexing, in bytes
         * - ``--dimensions``
           - Extra ``args.*`` fields to add to the bloom filter dimensions
             (comma-separated, e.g. ``args.level,args.mode``)
         * - ``--expected-entries``
           - Expected entries per chunk, for bloom filter sizing (default 1024)
         * - ``--false-positive-rate``
           - Bloom filter false positive rate (default 0.01)
         * - ``--executor-threads``
           - Worker threads for parallel indexing (default: CPU core count)

Reuse and incremental builds
-----------------------------

Nothing you write needs to track whether a trace has already been indexed.
Every build entry point - ``Indexer.ensure_indexed()``,
``resolve_and_build_index``, and ``dftracer_index`` without ``--force`` -
checks the existing ``.dftindex`` store first and only does work for files
that are missing or out of date. Add new trace files to a directory and
re-run the same build command: only the new files get indexed, and the
existing ones are left alone.

.. _indexing-first-touch:

A first query on a fresh file builds the index for you
-----------------------------------------------------------

You do not have to index before you query a file that has never been indexed.
The first aggregation query against a genuinely fresh file - no ``.dftindex``
for it yet, and a plan without a time range - takes a one-pass "bootstrap"
that both answers the query and builds the full index (members, bloom
filters, hash tables) as a byproduct, so the query never pays for a separate
eager build:

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer

         # No prior Indexer call needed: the first touch builds the index.
         df = TraceViewer("traces/").group_by("cat").agg("count").collect().collect()

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>

         using namespace dftracer::utils::trace::views;

         // .get() blocks a non-coroutine caller like main(); co_await
         // instead inside async code.
         View view = View::from_directory("traces/").get();
         auto df = view.group_by({GroupKey::cat()})
                       .agg({AggSpec(AggOp::Count)})
                       .collect()   // -> LazyFrame
                       .collect()   // -> coro::CoroTask<DataFrame>
                       .get();

This bootstrap only fires for that clean-first-touch case. Once an index
exists, a plain query reads it as-is: it does **not** re-check whether the
underlying trace changed since the index was built. If you might be reading a
directory whose files get re-indexed, appended to, or replaced, index
explicitly first (as in the previous section) rather than relying on the
bootstrap. Two read paths *do* refresh a possibly-stale index automatically
before every scan: the ``dftracer_view`` CLI (turn it off with
``--no-auto-index``) and the sharded/distributed read path, both through the
same ``ensure_indexes_fresh`` entry point used above.

Upgrading the library can also invalidate an index: each on-disk index records
the ``SCHEMA_VERSION`` it was built with, and a build that raises it treats
older indexes as stale and rebuilds them. An index directory is cheap to
rebuild, so this needs no action from you beyond letting the next build run.

See also
--------

- :doc:`../../concepts/indexing-and-pushdown` for what the index holds and how
  pushdown uses it.
- :doc:`query-dsl` for the predicates that get pushed down.
- :doc:`../../cpp_api/indexer` for the generated C++ indexer API reference.
- :doc:`../../trace-viewer` and :doc:`../data/dataframe` for reading the
  trace once it is indexed.
