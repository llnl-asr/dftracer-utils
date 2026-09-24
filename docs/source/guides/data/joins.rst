:description: Hash-join two frames on one or more key columns: inner, left, right, outer, semi, anti and cross joins, eager or lazy, from C++, C and Python.

Join frames
===========

Combine two frames row-wise on a shared key: line up a per-file summary against a
baseline, attach resolved names to an aggregate, or intersect two result sets.
The join is a hash join keyed by name: ``on`` names the key column(s) both
frames carry, or ``left_on`` / ``right_on`` name each side's keys when they
differ. Keys compare exactly, each pair must share a type, and a null key never
matches (SQL semantics).

The output is the left frame's columns, then the right frame's, except a right
key that shares its left key's name (emitted once). Any other right column whose
name collides with a left column gets a suffix (``_right`` by default). Matched
rows keep the left frame's order; ``right`` / ``outer`` append the unmatched
right rows at the end. ``semi`` / ``anti`` return the left frame's columns only,
one row per left row with / without a match. ``cross`` pairs every left row with
every right row and takes no keys.

Join two DataFrames
-------------------

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/dataframe/dataframe.h>
         using dftracer::utils::dataframe::DataFrame;
         using dftracer::utils::dataframe::JoinHow;

         DataFrame joined = left.join(right, {"fid"});                     // inner
         DataFrame lj = left.join(right, {"pid", "tid"}, JoinHow::Left);
         DataFrame keyed = left.join(right, {"k"}, {"rk"}, JoinHow::Outer, "_r");

      ``JoinHow`` is ``Inner``, ``Left``, ``Right``, ``Outer``, ``Semi``, ``Anti``
      or ``Cross``. An absent key throws ``std::out_of_range``; an empty or
      uneven key list, or a key type mismatch, throws ``std::invalid_argument``.

   .. tab-item:: Python

      .. code-block:: python

         joined = df.join(other, on="fid")                       # inner
         joined = df.join(other, on=["pid", "tid"], how="left")
         joined = df.join(other, left_on="k", right_on="rk", how="outer", suffix="_r")
         joined = df.merge(other, how="left", on="fid")          # pandas order

      ``on`` is the shared key column name(s), or an int count of this frame's
      leading columns. ``how`` is ``inner`` / ``left`` / ``right`` / ``outer``
      (``full`` is an alias) / ``semi`` / ``anti`` / ``cross``. ``merge`` is the
      pandas spelling of the same call (``right`` first, then ``how``); only the
      right side's colliding columns are suffixed. A missing key column raises
      ``KeyError``.

   .. tab-item:: C

      .. code-block:: c

         const char* on[] = {"fid"};
         dftu_dataframe* joined =
             dftu_dataframe_join(left, right, on, on, 1, DFTU_JOIN_INNER, NULL);

      ``dftu_dataframe_join`` returns NULL on an absent key, a key type mismatch,
      an empty key list for a keyed join, or an unknown ``dftu_join_how``. The
      same op is registered as ``dftu.frame.join`` (two frame operands, then
      the two key lists, the join kind and the suffix).

Join lazily
-----------

``LazyFrame::join`` takes another ``LazyFrame`` as the right side. When the plan
runs, the right plan is collected in full (the hash build side, bounded by the
right row count) and the left plan streams through it morsel by morsel: inner,
left, semi, anti and cross hold no left state, and right / outer add one match
bit per right row. The join is an optimizer barrier: no filter or projection
moves across it.

Once the build side is in hand, an inner, right or semi join narrows the
left scan to the keys it will keep: the set of build keys (or their range,
for a large numeric key) goes up the left chain as a ``Cursor::narrow``
offer, through the streaming ops between (filter, select, with_column,
rename, sort, drop_nulls, with_row_index, a plugin node that forwards it)
to the source. A trace scan turns it into an index prune of the chunks not
yet read; any source may ignore it. The offer is advisory: the join still
probes every row the source yields, so honouring it only changes how much
is read, never the result.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         LazyFrame plan = left.lazy().join(right.lazy(), {"fid"}, JoinHow::Left);
         DataFrame out = co_await plan.collect();

   .. tab-item:: Python

      .. code-block:: python

         out = left.lazy().join(right.lazy(), on="fid", how="left").collect()

   .. tab-item:: C

      .. code-block:: c

         dftu_lazyframe* plan =
             dftu_lazyframe_join(left_lf, right_lf, on, on, 1, DFTU_JOIN_LEFT, NULL);

      Registered as ``dftu.lazy.join``, so a plugin reaches it through
      ``OwnedLazyFrame::join`` as well.

Join two aggregated trace queries
---------------------------------

When both sides are trace queries with the same ``group_by``/``agg``, join their
aggregated results directly - each side runs its own pruned scan, then the two
result frames equi-join on the shared group key.

.. tab-set::

   .. tab-item:: C++

      ``View`` has no aggregated-join terminal of its own; call ``.lazy()`` on
      each aggregated view and join the resulting ``LazyFrame``\ s directly, as
      above, naming the shared group key.

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>

         using namespace dftracer::utils::trace::views;

         View base = View::from_file("baseline.pfw.gz")
                         .group_by({GroupKey::cat()})
                         .agg({{AggOp::Count, "", "n"}});
         View vary = View::from_file("variant.pfw.gz")
                         .group_by({GroupKey::cat()})
                         .agg({{AggOp::Count, "", "n"}});

         LazyFrame plan = base.lazy().join(vary.lazy(), {"cat"}, JoinHow::Outer);
         DataFrame joined = plan.collect().get();

   .. tab-item:: Python

      ``TraceViewer`` is a ``LazyFrame``, so ``join`` is the lazy join above:
      name the key with ``on``; a clashing right-side column gets the
      ``_right`` suffix. When both sides read the same trace, the join shares
      one scan.

      .. code-block:: python

         from dftracer.utils import TraceViewer

         base = TraceViewer("baseline.pfw.gz").group_by("cat").agg("count")
         vary = TraceViewer("variant.pfw.gz").group_by("cat").agg("count")

         joined = base.join(vary, on="cat", how="inner").collect()
         # columns: cat, count, count_right

For a ready-made baseline-vs-variant delta on top of this join, see
:doc:`../analysis/comparison`.

Temporal and interval joins (Python)
-------------------------------------

For time-based matching that an equi-join cannot express - "attach the latest
known state as of this event" or "which interval does this point fall in" -
``DataFrame`` carries two more native joins, ``asof`` and ``interval``. Both take
another ``DataFrame`` and return a ``DataFrame`` (no pyarrow), referencing
columns by name:

.. code-block:: python

   # Nearest-by-time match, within an optional equi-key partition.
   matched = left.asof(right, on="ts", by="pid",
                       direction="backward", tolerance=1_000_000)

   # Point-in-range match: left's `point` against right's closed [lo, hi].
   spans = events.interval(phases, point="ts", lo="start", hi="end", by="pid")

``asof`` matches each left row to the nearest right row by the time column
``on`` (``direction`` ``backward`` / ``forward`` / ``nearest``; ``tolerance``
bounds the allowed distance); unmatched left rows get null right values.
``interval`` matches each left row's ``point`` to every right row whose closed
span ``[lo, hi]`` contains it, one output row per match, and ``outer=True`` also
emits an unmatched left row once with null right values. Neither is the same op
as the equi-join above.

See also
--------

- :doc:`dataframe` for the other frame ops (project, filter, sort, reshape) and
  the generated ``asof`` / ``interval`` / ``window`` reference.
- :doc:`time-windows` for window functions and time-grid gap filling.
- :doc:`../analysis/aggregation` for building the aggregated views you join.
- :doc:`../../cpp_api/dataframe` for the full member and C ABI reference.
