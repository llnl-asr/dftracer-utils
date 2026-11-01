:description: Index a large trace set in parallel across a dask cluster: each worker writes its share of the files straight into the index store.

Build an index across a dask cluster
====================================

.. admonition:: Goal
   :class: goal

   Index a large trace set in parallel across a `dask
   <https://www.dask.org/>`_ cluster, with each worker writing its share of
   the files straight into one index store.

This is a Python-only API. It lives in ``dftracer.utils.dask`` and has no C++ or
C equivalent; the C++ indexer is single-process (parallel within the node).

The distributed_index function
------------------------------

Import it directly from the ``dask`` submodule (it is not re-exported from the
package root):

.. code-block:: python

   from dftracer.utils.dask import distributed_index

The minimal call runs inline on the coordinator - no cluster needed. Two
arguments are required: a source (``directory`` or ``files``) and an
``index_path``:

.. code-block:: python

   result = distributed_index(
       directory="./traces",
       index_path="/path/to/index.dftindex",
   )
   # {"total_files": 128, "per_worker": [...], "index_path": "..."}

With ``client=None`` (the default) every task runs inline on the coordinator. To
fan the work across workers, pass a ``dask.distributed.Client`` and give every
worker write access to ``index_path``:

.. code-block:: python

   from dask.distributed import Client
   from dftracer.utils.dask import distributed_index, register_auto_thread_plugin

   client = Client("tcp://scheduler:8786")
   register_auto_thread_plugin()   # size each worker's C++ runtime threads

   result = distributed_index(
       directory="/shared/traces",
       index_path="/shared/index.dftindex",
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
     - Output index store path, written by every worker.
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
per worker) and ``index_path``. It raises ``ValueError``
when ``index_path`` is empty or neither ``directory`` nor
``files`` is given, and ``ImportError`` when dask is not installed.

The build runs in four steps.
1. Each worker builds its files as a complete index root at
   ``<index_path>/shards/<worker>``, staging the build on node-local disk with
   the scratch helpers in ``dftracer/utils/common/scratch.h`` when
   ``<index_path>`` is a network filesystem,
   and falling back to building in place when there is no node-local scratch.
2. The coordinator writes ``<index_path>/shards.json`` once every task
   finished.
3. The coordinator merges the shards into ``<index_path>/store`` by moving
   their store data files there, then removes the manifest and the emptied
   shard directories.
4. The coordinator records the aggregation config and queries read
   ``<index_path>/store`` like any other index root.

A failed task fails the whole build and leaves the arrived shards in place, so
a later call merges them without rebuilding.

Related helpers
---------------

The same module exposes ``register_auto_thread_plugin()`` (sizes each worker's
native runtime thread count), ``assign_files_by_pid(file_pids, n_workers)``
(worker assignment by majority-PID affinity), and the ``DaskTraceViewer`` /
``DaskAggregatedTraceViewer`` classes for querying across the cluster once the
index exists.

``assign_files_by_pid`` takes a dict that maps each trace path to its set of
PIDs, and returns a dict that maps each worker id to a list of trace paths.
It sends the files of one majority PID to the same worker.
A file with no recorded PID goes to the worker its path hashes to with
``zlib.crc32``, so two runs of the same input give the same assignment.

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

``distributed_index`` merges its shards into the index root, so a reader needs
no shard support. A producer that keeps its per-worker indexes unmerged can
leave them as a set of immutable index shards in ``<root>/shards/<n>`` next to
the ``<root>/shards.json`` manifest. ``ShardedView`` (C++,
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

A shard set is a directory holding a ``shards.json`` manifest.
``IndexShardManifest`` writes that manifest and is internal to the index
module.
The manifest holds a ``format_version`` and a list of ``IndexShardEntry``
records.
Each record names one shard's ``path``, which is relative to the manifest's
directory, and gives the shard's ``num_files`` and ``num_events`` counts.
Each shard is a self-contained read-only index, so a reader opens each shard
independently.
The ``index`` line of ``VERSION`` sets ``format_version``, and a reader
rejects a manifest that holds another version.

Three free functions manage the set:

- ``write_shard_set(root, shard_dirs)`` - catalog an existing set of shard
  directories into a manifest at ``root``. Atomic (write-temp-then-rename), so
  a concurrent reader never sees a torn manifest.
- ``consolidate_shard_set(root)`` - merge every shard into ``root``'s own
  store by moving each shard's store data files there, then remove the
  manifest and the emptied shard directories. A move that fails leaves the
  manifest and the remaining shards in place and throws, so a later call
  retries the merge without a rebuild.
- ``merge_shard_set(root, out_root)`` - merge the shards' aggregation partials
  into one consolidated index at ``out_root`` **without** re-reading traces
  (re-keys each shard's interned strings into a unified dictionary and copies
  the partials and the column blocks). The shards stay where they are.

The shards of a root live in ``<root>/shards/<n>`` and the manifest is
``<root>/shards.json`` (``shard_root`` and ``shard_manifest_path``), so the
shard set is inside the index root that holds it and the directory scanner
never picks it up as trace input.

CLI: dftracer_view autodetects a shard set
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``dftracer_view`` resolves a shard-set root from ``-d``/``--directory`` or
``--index-dir`` (adding ``.dftindex`` to the path when the path is not an
index root yet, then reading ``shards.json``) and, when found, routes the
query through ``ShardedView`` instead of scanning trace files directly. A
shard set only supports aggregate/counter queries:

.. code-block:: bash

   dftracer_view -d /shared/traces --group-by name --agg count,sum:dur \
       > result.ndjson

If ``-d``/``--index-dir`` does not resolve to a shard set (no ``shards.json``
in the index root), ``dftracer_view`` falls back to its normal single-index
path.

See also
--------

- :doc:`../../trace-viewer` - query the index this builds.
- :doc:`distributed-aggregation` - the partial-aggregate fan-in pattern
  ``ShardedView`` runs in-process, and the pattern to use when reducing
  partials yourself (e.g. across MPI ranks).
- :doc:`mpi` - the MPI modes of ``dftracer_view``, the other cross-node path.
- :doc:`../io/compression` - ``checkpoint_size`` is the gzip member size.
