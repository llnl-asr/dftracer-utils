:description: Run map, combine, shuffle and join over a partitioned DataFrame on a dask cluster, or in memory with no cluster, with DaskFrame.

Work on a partitioned DataFrame
===============================

.. admonition:: Goal
   :class: goal

   Run an analysis over many ``DataFrame`` partitions with `dask
   <https://www.dask.org/>`_ as the executor: apply a function to every
   partition, combine partial aggregates, shuffle by key so a per-group
   operation is exact, and join two partitioned tables. The same calls run in
   memory with no cluster, with equal results.

This is a Python-only helper. It lives in ``dftracer.utils.dask`` and adds no
C++ or C operation: it orchestrates ``hash_partition``, ``concat``, ``join``
and ``group_by``, which already exist.

Build one
---------

.. code-block:: python

   from dask.distributed import Client, LocalCluster
   from dftracer.utils.dask import DaskFrame

   client = Client(LocalCluster(n_workers=4, threads_per_worker=1, processes=True, silence_logs=40))

   # Each partition is built on a worker by fn(arg); nothing is gathered.
   frame = DaskFrame.from_function(load_partition, range(8), client)

   # Or from frames you already hold:
   frame = DaskFrame.from_frames([df0, df1, df2], client)

   # No client: the same methods, in this process.
   local = DaskFrame.from_frames([df0, df1, df2])

The frame owns its partitions. ``close()`` releases them. ``to_frame()`` gathers
and concatenates them in order; ``to_pandas()`` does the same and converts.
Functions you pass must pickle; module-level functions and lambdas work.

The patterns
------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - You want
     - Call
   * - A function on every partition
     - ``map_partitions(fn, *args)``
   * - ``rolling``, ``diff`` or ``shift`` across partition seams
     - ``map_partitions(fn, overlap=k, overlap_next=m)``: ``fn`` also sees the last ``k`` rows of the previous partition and the first ``m`` rows of the next one (a ``lead`` or a centered window needs ``overlap_next``)
   * - Count, sum, min, max, sum of squares, mean, variance, standard deviation per group
     - ``group_by(keys).agg(...)``, computed from per-partition partials
   * - Any other reduction as a tree
     - ``reduce(partial, combine, split_every=8)``
   * - Exact median, quantile, distinct count or set union per group
     - ``group_by(keys).agg(exact=True, ...)``, or ``shuffle(keys)`` and your own ``map_partitions``
   * - A join
     - ``join(other, on, how, strategy)``: ``"broadcast"`` (inner or left) or ``"shuffle"`` (inner, left, right, outer)

``group_by(...).agg(...)`` takes ``col(c).sum()``-style aggregates or
``(column, name)`` pairs. ``var`` and ``std`` are the sample form. An aggregate
that does not combine from partials (``median``, ``quantile``, ``nunique``,
``set_union``) is refused with an error that says to shuffle first:

.. code-block:: python

   stats = frame.group_by(["name", "pid"]).agg(
       n=col("dur").count(), total=col("dur").sum(), mean=("dur", "mean"), sd=("dur", "std"),
   ).to_frame()

   # Exact median, 0.9 quantile, distinct count and set union in one call: it shuffles by the
   # keys once, then computes every aggregate per group. The result stays partitioned by key.
   exact = frame.group_by("k").agg(
       exact=True, med=("x", "median"), p90=("x", "quantile", 0.9),
       nu=("y", "nunique"), tags=("s", "set_union"), total=col("x").sum(),
   )

   # The same by hand: every key lives in one partition after the shuffle.
   def per_group(part):
       g = part.group_by("k")
       return g.quantile(0.5)

   medians = frame.shuffle("k").map_partitions(per_group).to_frame()

Shuffle and join
----------------

``shuffle(keys, n)`` hash-partitions the rows into ``n`` partitions (default: as
many as now). A key goes to the same partition in every worker process, so after
it every group is whole in one partition and any per-group operation is exact,
with no merge of partial states.

A shuffle uses two stages when ``M * N`` (input partitions times output
partitions) is above 1024 and ``N`` is at least 4. It groups the rows into about
``sqrt(N)`` coarse partitions and then splits those, which submits far fewer
tasks (4,224 become 1,168 at 64 partitions; 16,640 become 3,094 at 128) and moves
the data twice. Every key lands in the same output partition as in the one-stage
form; only the row order inside a partition can differ. ``stages=1`` or
``stages=2`` forces a choice.

``join`` with ``strategy="broadcast"`` copies ``other`` to every worker and joins
each partition where it lives; it supports ``how="inner"`` or ``"left"`` only,
because an unmatched row of the broadcast side would repeat on every partition.
With ``strategy="shuffle"`` both sides are hashed by the join keys and the
matching partitions are joined, for ``inner``, ``left``, ``right`` and ``outer``.
``"auto"`` broadcasts when ``other`` is a ``DataFrame`` or has one partition and
``how`` is inner or left, and shuffles otherwise.

Null keys match as the engine's ``DataFrame.join`` says (a null key matches
nothing). ``join(..., nulls_equal=True)`` passes the option to the engine once the
engine has it, and raises ``NotImplementedError`` before any task runs if it does
not.

Measured, on four worker processes (Dask 2026.8.0), with the data already on the
workers:

.. list-table::
   :header-rows: 1

   * - Case
     - Cluster
     - In process
   * - Shuffle 8 partitions of 400,000 rows, 200,000 keys
     - 0.14 s
     - 0.08 s
   * - Broadcast join, 8 partitions of 300,000 rows with a 100,000-row table
     - 0.06 s
     - 0.01 s
   * - Shuffle join, the same data
     - 0.09 s
     - 0.02 s
   * - Shuffle 64 partitions of 200 rows into 64, one stage / two stages
     - 1.71 s / 0.58 s
     - 0.01 s / 0.00 s
   * - Shuffle 128 partitions of 200 rows into 128, one stage / two stages
     - 26.85 s / 1.38 s
     - 0.03 s / 0.02 s
   * - Shuffle 128 partitions of 20,000 rows into 128, one stage / two stages
     - 5.80 s / 1.44 s
     - 0.06 s / 0.05 s

The last three rows are the median of three runs on a machine that was also
building other software, so read them as an order of magnitude, not a benchmark.

Small data does not need a cluster
----------------------------------

Distribution does not pay at small sizes. On one process, an exact median and
distinct count over 3.2 million rows took 0.38 s in pandas, against 0.80 s for a
four-process shuffle plus the same operations. With no client, ``DaskFrame``
runs everything in this process and gives equal results, so pick the mode by the
size of your data. The helper does not guess a threshold.

Limits
------

- A one-stage ``shuffle`` runs ``M`` splits and ``M * N`` small fetch tasks; the
  two-stage form needs about ``(M + N) * sqrt(N)``. A peer-to-peer shuffle for many
  thousands of partitions is not provided.
- ``overlap`` and ``overlap_next`` take rows from the neighbouring partition only,
  so a window that reaches past a neighbour shorter than the overlap is wrong.
- ``agg(exact=True)`` holds a whole group in one partition, so one very large key
  makes one large partition.
- ``var`` and ``std`` merge per-partition counts, means and sums of squared
  deviations (the Chan merge), taken about a per-group reference (the group
  minimum, found in a first pass), so they match a two-pass computation to a
  relative 1e-9 at any offset. That costs one extra ``reduce`` over the data when
  a ``var`` or ``std`` is requested.
- ``map_partitions(..., overlap=k)`` needs ``fn`` to keep one output row per input
  row, in order.
- Quantiles that merge as sketches are not offered here; use ``shuffle`` for an
  exact result, or the trace ``View`` partials, which merge sketches.
