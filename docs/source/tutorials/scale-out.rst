:description: Build two independent indexes and query both as one logical index without merging, the in-process shape of a cluster shard-and-reduce job.

Query a set of index shards
=============================

.. admonition:: Goal
   :class: goal

   Build two independent indexes - as if two workers had each indexed their
   own slice of a trace set - and query both as one logical index without merging
   them into a single store first. This is the in-process shape of the pattern a
   real cluster job uses: each worker aggregates its own shard, and a coordinator
   reduces the partials. It assumes :doc:`first-analysis`.

1. Two shards, two directories
---------------------------------

A shard here is just a directory holding its own trace(s) and its own
``.dftindex``. Put a different trace in each:

.. code-block:: python

   import gzip, os

   os.makedirs("shard-a", exist_ok=True)
   os.makedirs("shard-b", exist_ok=True)

   with gzip.open("shard-a/trace.pfw.gz", "wt") as f:
       for i in range(200):
           f.write(
               f'{{"name":"read","cat":"POSIX","pid":1,"tid":1,'
               f'"ts":{1000 + i},"dur":{10 + i},"ph":"X","args":{{}}}}\n'
           )

   with gzip.open("shard-b/trace.pfw.gz", "wt") as f:
       for i in range(300):
           f.write(
               f'{{"name":"read","cat":"POSIX","pid":2,"tid":1,'
               f'"ts":{2000 + i},"dur":{5 + i},"ph":"X","args":{{}}}}\n'
           )

2. Index each shard independently
-------------------------------------

``dftracer_index -d <dir>`` builds ``<dir>/.dftindex`` for everything under
``<dir>``. Run it once per shard - this is exactly what each worker in a real
job would do to its own slice, with no coordination between them:

.. code-block:: console

   $ dftracer_index -d shard-a
   $ dftracer_index -d shard-b

3. Catalog and query the shard set
--------------------------------------

``ShardedView`` (header ``dftracer/utils/trace/views/sharded_view.h``,
namespace ``dftracer::utils::trace::views``) is C++-only: it queries a set of
immutable index shards as one logical index, without opening or rebuilding a
merged store. ``write_shard_set(root, shard_dirs)`` catalogs the shards into a
``shards.json`` manifest at ``root``; ``ShardedView::from_manifest(root)``
reads that catalog back. Under the hood, ``aggregate()`` runs each shard's
``aggregate_partial()`` and reduces the partials with
``merge_partials_to_table()`` - see :doc:`../guides/scale/distributed-aggregation`
for that pattern used directly (e.g. across MPI ranks):

.. code-block:: cpp

   #include <dftracer/utils/trace/views/sharded_view.h>

   #include <cstdint>
   #include <cstdio>
   #include <filesystem>

   using namespace dftracer::utils::trace::views;

   int main() {
       write_shard_set("shards", {std::filesystem::absolute("shard-a").string(),
                                  std::filesystem::absolute("shard-b").string()});

       ShardedView sv = ShardedView::from_manifest("shards");

       auto configure = [](View v) {
           return v.group_by({GroupKey::cat()})
                   .agg({AggSpec(AggOp::Count), AggSpec(AggOp::Sum, "dur")});
       };
       auto df = sv.aggregate(configure).get();

       auto cat = df.column("cat");
       auto count = df.column("count");
       auto sum_dur = df.column("sum_dur");
       for (std::int64_t i = 0; i < df.num_rows(); ++i)
           std::printf("%-6.*s %4lld %8llu\n",
                       static_cast<int>(cat.string_at(i).size()),
                       cat.string_at(i).data(),
                       static_cast<long long>(count.data<std::int64_t>()[i]),
                       static_cast<unsigned long long>(sum_dur.data<std::uint64_t>()[i]));
   }

Expected output (both shards' 200 + 300 events, reduced from two independent
partials into one row):

.. code-block:: text

   posix   500    68250

No process ever opened both shards' trace files in the same scan, and neither
shard's index was rebuilt or merged on disk - ``ShardedView`` is fully
read-only. ``sum_dur`` here comes back as a ``Uint64`` column: the partial
merge preserves the field's integer domain, so a merged ``Sum`` over a
non-negative integer field matches a plain ``View::collect()`` - read it with
``data<std::uint64_t>()``.

What you learned
-------------------

- A shard is just a directory with its own trace(s) and its own
  ``.dftindex``; build each independently (``dftracer_index -d <dir>``), with
  no coordination required between shards.
- ``write_shard_set(root, shard_dirs)`` catalogs a set of shards into a
  manifest; ``ShardedView::from_manifest(root)`` reads it back and
  ``.aggregate(configure)`` answers a ``group_by``/``agg`` query across every
  shard without merging them.
- A merged partial aggregation preserves each field's numeric domain: a
  ``Sum``/``Min``/``Max`` over an integer field returns the same integer
  (``Uint64``) column as a direct ``View::collect()``, so the two paths agree.

See also
----------

- :doc:`../guides/scale/distributed-index` - the full shard-set format
  (``IndexShardManifest``), ``dftracer_view``'s shard-set autodetection, and
  ``distributed_index()`` (Python, ``dftracer.utils.dask``) for building shards
  across a real dask cluster instead of by hand.
- :doc:`../guides/scale/distributed-aggregation` - the ``aggregate_partial`` /
  ``merge_partials`` (C++ ``merge_partials_to_table``) fan-out/fan-in pattern
  ``ShardedView`` runs
  in-process, spelled out for a caller-owned transport (MPI, dask, ...).
- :doc:`../guides/scale/mpi` - the one distributed CLI binary in the tree,
  for the cluster case this tutorial's two-shard example stands in for.
