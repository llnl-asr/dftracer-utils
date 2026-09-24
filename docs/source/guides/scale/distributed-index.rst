:description: Index a large trace set in parallel across a dask cluster: workers build shards to node-local staging and the coordinator assembles the store.

Build an index across a dask cluster
====================================

.. admonition:: Goal
   :class: goal

   Index a large trace set in parallel across a `dask
   <https://www.dask.org/>`_ cluster, with each worker building shards to
   node-local staging and the coordinator assembling them into one index store.

This is a Python-only API. It lives in ``dftracer.utils.dask`` and has no C++ or
C equivalent; the C++ indexer is single-process (parallel within the node).

The distributed_index function
------------------------------

Import it directly from the ``dask`` submodule (it is not re-exported from the
package root):

.. code-block:: python

   from dftracer.utils.dask import distributed_index

The minimal call runs inline on the coordinator - no cluster needed. Three
arguments are required: a source (``directory`` or ``files``), an
``index_path``, and a ``local_staging`` directory:

.. code-block:: python

   result = distributed_index(
       directory="./traces",
       index_path="/path/to/index.dftindex",
       local_staging="/path/to/staging",
   )
   # {"total_files": 128, "per_worker": [...], "index_path": "...",
   #  "artifact_batches": 4}

With ``client=None`` (the default) every task runs inline on the coordinator. To
fan the work across workers, pass a ``dask.distributed.Client`` and give a
``shared_staging`` path on a filesystem the coordinator can also read:

.. code-block:: python

   from dask.distributed import Client
   from dftracer.utils.dask import distributed_index, register_auto_thread_plugin

   client = Client("tcp://scheduler:8786")
   register_auto_thread_plugin()   # size each worker's C++ runtime threads

   result = distributed_index(
       directory="/shared/traces",
       index_path="/shared/index.dftindex",
       local_staging="/scratch/local",     # node-local scratch on each worker
       shared_staging="/shared/staging",   # visible to the coordinator
       client=client,
       partition="lpt",                    # longest-processing-time balancing
   )

Useful parameters
~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 32 20 48

   * - Parameter
     - Default
     - Meaning
   * - ``directory`` / ``files``
     - ``""`` / ``None``
     - Trace source. Give one of them.
   * - ``index_path``
     - (required)
     - Output index store path.
   * - ``local_staging``
     - (required)
     - Per-worker (node-local) staging directory.
   * - ``shared_staging``
     - = ``local_staging``
     - Staging root reachable by the coordinator (needed with a real cluster).
   * - ``client``
     - ``None``
     - A dask ``Client``; ``None`` runs inline on the coordinator.
   * - ``checkpoint_size``
     - ``33554432`` (32 MB)
     - Gzip checkpoint/member size for the built index.
   * - ``partition``
     - ``"lpt"``
     - File-to-worker assignment: ``"lpt"`` or ``"round_robin"``.
   * - ``build_bloom``
     - ``True``
     - Build bloom filters over ``bloom_dimensions``.
   * - ``force_rebuild``
     - ``False``
     - Rebuild even when an index already exists.
   * - ``progress``
     - ``None``
     - Callback ``(done, total, message)`` for progress reporting.

``distributed_index`` returns a dict with ``total_files``, ``per_worker`` (files
per worker), ``index_path``, and ``artifact_batches``. It raises ``ValueError``
when ``index_path`` or ``local_staging`` is empty or neither ``directory`` nor
``files`` is given, and ``ImportError`` when dask is not installed.

Related helpers
---------------

The same module exposes ``register_auto_thread_plugin()`` (sizes each worker's
native runtime thread count), ``assign_files_by_pid(file_pids, n_workers)``
(worker assignment by majority-PID affinity), and the ``DaskTraceViewer`` /
``DaskAggregatedTraceViewer`` classes for querying across the cluster once the
index exists.

``DaskTraceViewer.flamegraph(partition=("pid", "tid"), ts, dur, name, group)``
builds a flamegraph across the cluster: each file shard folds a serialized arena
partial (``TraceViewer.flamegraph_partial``), and the client tree-reduces them
with ``TraceViewer.merge_flamegraph_partials`` into the final node DataFrame, so
a name folds across shards. Partition by ``pid`` so a lane lives on one shard.
``group`` roots the tree by an arbitrary key (e.g. ``("cat",)``) over the raw
events, as in ``TraceViewer.flamegraph``. It composes with the branch's filter /
phase / time like ``collect``.

Query a shard set
-----------------

``distributed_index`` (and any producer that writes disjoint per-worker
indexes) can leave behind a set of immutable index shards instead of one
consolidated store. ``ShardedView`` (C++,
``dftracer/utils/trace/views/sharded_view.h``) queries that set as one logical
index, without opening or rebuilding a merged store: it enumerates every
shard's files, aggregates each shard independently with
``aggregate_partial()``, and reduces the partials - the in-process form of the
:doc:`distributed-aggregation` fan-in pattern.

.. code-block:: cpp

   #include <dftracer/utils/trace/views/sharded_view.h>

   using namespace dftracer::utils::trace::views;

   ShardedView sv = ShardedView::from_manifest("/shared/index-shards");
   // or, bypassing the manifest:
   ShardedView sv2 = ShardedView::from_shard_dirs({"/shared/s0", "/shared/s1"});

   auto configure = [](View v) {
       return v.group_by({GroupKey::name()})
               .agg({{AggOp::Count, "", "count"}, {AggOp::Sum, "dur", "sum_dur"}});
   };
   dftracer::utils::dataframe::DataFrame table = sv.aggregate(configure).get();

   // Or emit the merged aggregation as ph="C" counter events. ExportSink is
   // abstract (just write(std::string_view)), so a caller implements one:
   struct StringSink : ExportSink {
       std::string data;
       void write(std::string_view chunk) override { data.append(chunk); }
   };
   StringSink sink;
   ExportStats stats = sv.aggregate_counters(configure, sink).get();

``ShardedView`` is fully read-only: it never builds, rebuilds, or persists a
rollup, so it takes no write lock and is safe against shards on a read-only
mount or NFS. It only answers aggregate/counter queries (``configure`` must
set a ``group_by``/``agg``) - there is no raw-event read across a shard set.

The shard-set format
~~~~~~~~~~~~~~~~~~~~~

A shard set is a directory holding a ``shards.json`` manifest
(written by ``IndexShardManifest``, which is internal to the index module):
a ``schema_version`` plus a list of ``IndexShardEntry`` records, each naming
one shard's ``path`` (relative to the manifest's directory), its closed
``file_id_min``/``file_id_max`` range, ``num_files``, and ``num_events``. Every
shard's file-id range is disjoint from every other shard's, so a merge reader
can open all of them without key collisions.

Three free functions manage the set:

- ``write_shard_set(root, shard_dirs)`` - catalog an existing set of shard
  directories into a manifest at ``root``. Atomic (write-temp-then-rename), so
  a concurrent reader never sees a torn manifest.
- ``consolidate_shard_set(scope, root, out_root)`` - rebuild one unified index
  (with its aggregation tier) from the shards' trace files at ``out_root``,
  re-reading traces. Requires the trace files to be present with distinct
  logical filenames across shards.
- ``merge_shard_set(root, out_root)`` - merge the shards' aggregation tiers
  into one consolidated tier index at ``out_root`` **without** re-reading
  traces (re-keys each shard's interned strings into a unified dictionary and
  combines them). Answers tier-covered queries from a single index, but does
  not serve scan-fallback queries the way ``consolidate_shard_set`` does.

The default nested location a producer uses is ``<trace-dir>/.dftindex-shards``
(``SHARD_SET_DIRNAME``) so the shard set does not get picked up as trace input
by the directory scanner.

CLI: dftracer_view autodetects a shard set
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``dftracer_view`` resolves a shard-set root from ``-d``/``--directory`` or
``--index-dir`` (the directory itself if it holds ``shards.json``, else its
``.dftindex-shards`` subdirectory) and, when found, routes the query through
``ShardedView`` instead of scanning trace files directly. A shard set only
supports aggregate/counter queries:

.. code-block:: bash

   dftracer_view -d /shared/traces --group-by name --agg count,sum:dur \
       > result.ndjson

If ``-d``/``--index-dir`` does not resolve to a shard set (no ``shards.json``
under it or its ``.dftindex-shards`` subdirectory), ``dftracer_view`` falls
back to its normal single-index path.

See also
--------

- :doc:`../../trace-viewer` - query the index this builds.
- :doc:`distributed-aggregation` - the partial-aggregate fan-in pattern
  ``ShardedView`` runs in-process, and the pattern to use when reducing
  partials yourself (e.g. across MPI ranks).
- :doc:`mpi` - the MPI flamegraph driver, the other cross-node path.
- :doc:`../io/compression` - ``checkpoint_size`` is the gzip member size.
