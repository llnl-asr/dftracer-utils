:description: Distribute dftracer_view --flamegraph, --counters and --group-by/--agg over MPI ranks by folding an arena partial per rank.

Run across ranks with MPI
=========================

.. admonition:: Goal
   :class: goal

   Run a flamegraph, a counter extraction or an aggregation over a trace set
   too large for one process by spreading the work across MPI ranks. Under
   ``mpirun`` each rank folds a partial over its slice of the input, the
   partials are gathered, and rank 0 merges them into the final output.

This is CLI-only and requires an MPI build (see below). ``dftracer_view``
distributes ``--flamegraph``, ``--counters`` and the aggregate modes
(``--group-by`` and ``--agg``). Every other mode, such as ``--call-tree``, runs
on rank 0 only. See :doc:`distributed-index` for cross-node indexing via dask.

Build with MPI enabled
----------------------

MPI support is off by default. Configure with the option on:

.. code-block:: bash

   cmake -S . -B build -DDFTRACER_UTILS_ENABLE_MPI=ON
   cmake --build build

The same ``dftracer_view`` binary distributes under MPI; there is no separate
``_mpi`` binary.

Invoke it
---------

Launch ``dftracer_view`` under ``mpirun`` (or your scheduler's launcher) with
the rank count:

.. code-block:: bash

   mpirun -n 32 dftracer_view --flamegraph --files ./traces/*.pfw.gz -o flamegraph.ndjson

Each rank indexes and folds its slice of the input into a partial; rank 0
gathers the partials and merges them into the final folded flamegraph (NDJSON).
``--counters`` emits the merged counter events and ``--group-by``/``--agg``
print the merged table in the same way. The output path must live on a
filesystem visible to rank 0. ``--no-auto-index`` skips the per-rank index
build.

See also
--------

- :doc:`../../cli` - ``dftracer_view --call-tree`` / ``--flamegraph`` and the
  full option list.
- :doc:`distributed-index` - build an index across a dask cluster.
- :doc:`../runtime/memory-budget` - ``suggested_nodes`` from the budget advice
  sizes how wide to run.
