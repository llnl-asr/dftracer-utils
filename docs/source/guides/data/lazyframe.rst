:description: Build a deferred plan over any source, read it back before it runs, stream it under a memory budget, push work into the source, and put a plugin's own node in the middle of it.

Plans: the LazyFrame
====================

A ``LazyFrame`` is a plan: a source plus a list of steps, nothing run. The
same ops a ``DataFrame`` has eagerly (:doc:`dataframe`) record a step
instead, and the plan runs when you ``collect`` it, or ``stream`` it a
morsel at a time. This page is about what a plan gives you that an eager
frame cannot: a source that is not in memory, work pushed into that source,
a bounded memory footprint, and a place for a plugin's own operator.

Where a plan comes from
-----------------------

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer, DataFrame, col

         plan = TraceViewer("traces/").filter('cat == "POSIX"')  # a plan over the scan
         plan = df.lazy()                                        # a plan over a frame

         out = (plan.filter(col("dur") > 100)
                    .group_by("name").agg("count", "sum:dur")
                    .sort_by("sum_dur", descending=True)
                    .head(10)
                    .collect())

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/dataframe/lazyframe.h>
         using namespace dftracer::utils::dataframe;

         LazyFrame plan = LazyFrame::scan(source);        // any Source (below)
         LazyFrame plan = df.lazy();                      // an InMemorySource
         DataFrame out = co_await plan.filter(col(1) > std::int64_t{100})
                                      .group_by({"name"}, {{Agg::Count, "", "count"}})
                                      .collect();

   .. tab-item:: C

      .. code-block:: c

         dftu_lazyframe* plan = dftu_dataframe_lazy(df);
         dftu_lazyframe* plan = dftu_lazyframe_from_provider("me.src");  /* a registered source */
         dftu_dataframe* out  = dftu_lazyframe_collect(plan, -1);

      Every builder step is a ``dftu_lazyframe_*`` call and a registered
      ``dftu.lazy.*`` op, so a plugin reaches them through
      ``dftu_svc_ops::run_lazy`` (``OwnedLazyFrame`` in the SDK) without
      linking the engine.

A Python ``TraceViewer`` and a C++ ``View`` are each a plan over the fused
scan (``lazy()`` returns it as a ``LazyFrame``), so the steps you add run
inside the same pull chain as the scan; ``collect()`` on the plan is what reads
the files.

Drop and rename by name
-----------------------

``drop(names)`` and ``rename_columns(from, to)`` (``rename({old: new})`` in
Python) act on columns by name, so they reach every column a stream carries,
including the columns a trace scan adds per batch that the plan's schema does
not list. ``select`` keeps only the columns it names and so removes those. A
name that no column carries is ignored in C++ and C. In Python a plan with a
complete schema raises ``KeyError`` for it, and a ``TraceViewer`` ignores it.
A rename that leaves two columns with one name fails: at planning when both
are in the schema, and on the first batch that carries the other.

Read the plan before it runs
----------------------------

``explain()`` prints the optimized plan, the source then one step per line.
The ``columns`` property gives the output column names and ``schema`` the
typed fields (``schema()`` and ``output_schema()`` in C++), both without
running: the plan walks its steps from the source's
schema. A column whose type a step cannot know statically is ``Unknown``,
never a guess; a step whose columns are data-dependent (``pivot``,
``to_dummies``, ``describe``) leaves both empty until collect.

.. code-block:: python

   print(plan.explain())
   # scan ViewSource[trace.pfw.gz]
   # filter col(5) > 100
   # group_by [name] count, sum(dur)
   # sort_by sum_dur desc
   # head 10
   plan.columns       # ['name', 'count', 'sum_dur']

The optimizer inserts a projection of the columns the plan reads (a
``select``, or a group-by's keys and aggregate columns) and pushes it with
the filters after it into the source, drops a filter the source applied
exactly, and never moves a step across a join, a group-by, or a plugin
node.

Collect, or stream under a budget
---------------------------------

``collect(morsel_rows)`` drains the plan into one frame. A resident source
runs whole-column, the same as eager, every op included (one with no
eager form runs its own cursor over the frame as a single morsel); a
streaming source runs morsel by morsel. ``stream()`` yields the morsels instead, each a standalone
frame, so a consumer can stop early: dropping the generator is the scan's
early-out.

``memory_budget(bytes)`` bounds every breaker (sort, unique, group-by, join,
pivot, the spools behind reverse / take, the group-wise window and
``compare_agg``): past the budget their state spills to temporary runs and is
merged back at the end, so peak memory stays near the budget, a small multiple
of it for a plan that chains several breakers, and does not grow with the
input. A sort that writes more runs than it can read at once merges groups of
runs into longer ones first. The right plan of a join runs under the budget of
the plan that joins it unless it sets its own. ``0`` is auto (about a third of
available memory); ``auto_spill()`` says that at the call site. The same
budget reaches a plugin node in the plan and a plugin's fold slice, see
:doc:`../runtime/memory-budget`.

Window functions stream under a budget: a chunk of the sorted input may end
inside a partition, so a partition larger than the budget is fine for running
sum, product, min, max and forward fill, ``row_number``, ``rank``,
``dense_rank``, ``lag``, ``lead``, ``delta`` and ``rate``, for row frames and
range frames of min, max, count, integer sum, quantile, distinct count and arg
min and max, for float sums and means of row frames, and for ``sessionize``.
``ntile``, ``percent_rank``, ``cume_dist`` and frames over a whole partition
(count, min, max, integer sum) are answered from per-partition figures
collected while the sort reads its input. That covers ``cumsum``, ``cummax``,
``cummin``, ``cumcount``, ``cumprod``, ``shift``, ``diff``, ``ffill``,
``bfill``, ``rolling_*``, ``rank``, ``head``, ``tail`` and ``nth`` of a
group-by, with or without partition columns. The simple cumulative functions
(``cumsum``, ``cummax``, ``cummin``, ``cumprod``, ``cumcount``, ``shift``,
``diff`` and ``ffill`` over numeric columns) are one native step: a hash pass in
input order when their groups fit the budget, a spilled sort by group and
back by row otherwise.

Frames from the partition start (count, integer sum, min and max over a row or
range frame, float sums, means, variance and standard deviation over a row
frame), variance and standard deviation frames, float sums and means over a
range frame, collect frames, ``first_value``, ``nth_value`` and ``last_value``
stream too, from state carried across the cut. ``last_value`` and the shift a
variance centers on depend on the sorted order of the whole partition, and a
window with more partitions than the figures table holds (an eighth of the
budget) has no table: for these the sorted stream is written once to a spool
(a quarter of the budget in memory, the rest on disk) while the figures of each
partition are collected in sorted order, and the window then reads the spool
and the figures back in step. ``nth_value`` holds back the ``n - 1`` rows
before the row it reads, so a large ``n`` needs that many rows.

A frame that ends at the partition end starts each partition from its figures,
which hold every row entered in the window's order, and drops the rows behind
it one by one, keeping at most the rows the frame reaches back. This covers
count, sum and mean over a row frame or a range frame, and min, max, variance
and standard deviation over a row frame. The sum, mean, variance and standard
deviation over a whole partition come from the figures too. A variance reads the
spool a second time, because the Welford steps start from the shift the first
pass ends with. A min or max to the end needs the best of the rows still ahead,
and a variance needs to know whether the rows left are all equal, so the sorted
spool is also read backwards once and a column per function is written in the
morsels of the rows and read back with them; the chunk keeps those values for
the rows the frame reaches back. A range frame to the end gives the rows whose
order value is missing (null or NaN, which sort last) one frame of their own;
the float sum or mean of that frame depends on the order the kernel adds and
subtracts in, so the second pass over the spool replays it once per partition.
A min or max of a text column from the partition start to the row or a few rows
after it carries its best value across the cut. Range and row frames, and
running functions, may share a window as long as each stays within the limits
below.

Four limits remain. A window with any other function holds one whole partition
in memory, so a single partition larger than the budget needs that much memory,
and with no partition columns that is the whole input: a quantile, distinct
count, arg min, arg max or collect frame from the partition start or to the
partition end (their state or result grows with the partition); a min, max,
variance, standard deviation or order statistic over a range frame that ends at
the partition end; a float sum, mean or variance over a range frame from the
partition start (rows with a null order value would drop the whole prefix, in
float order); and running functions that are mixed with ranks, ``lag``,
``lead``, ``delta``, ``rate`` or a frame the kernel answers from the rows
around the cut, or that read one column twice (a running sum and a running min
of the same column), because they are continued from one seed row that holds a
single value per column. ``nth_value`` holds back ``n - 1`` rows. A range frame
needs one numeric order column, and in a range frame that does not end at the
partition end the rows whose order value is missing sort last and are held
until their partition ends. ``compare_agg`` over plans whose column types are
only known after they run reads the types off the first morsel, so an empty
side gets no ``delta_`` or ``pct_`` columns.

An op that makes more rows than it reads hands them on in morsels that fit a
quarter of the budget: ``join`` (a left morsel that matches many right rows,
and the unmatched right rows of a right or outer join), ``explode``,
``unpivot``, ``to_dummies``, and the results of ``take``,
``group_by_dynamic`` and a ``group_by`` with aggregates that name their own
columns. One input row whose list or matches alone are larger than
that share still comes out whole. ``unpivot`` lists each slice of its input by
value column, so the order of its rows follows the morsels: with a budget the
slices are smaller, and the rows of one value column come out in input order but
interleaved with the other columns differently. A cross join keeps a right side that fits a
quarter of the budget in memory and spools a larger one, joining it chunk by
chunk against each slice of the left, so the rows of one left slice come out by
right chunk, not by left row.

A join whose right side passes the budget is split on the key into partitions
on disk. A partition that one key fills cannot be split, so its right side is
read in blocks that fit the budget and the whole left partition is probed
against each block: a left or outer join emits a left row that no block
matched once, at the end, a semi join emits a matched left row once, and an
anti join the rows that no block matched. A lookup join reads such a partition
in blocks too and keeps, per key, only its first right row and whether the
rows of the key differ, then probes the left partition once; it throws what
the in-memory join throws. That per-key state (a row and the key text for each
distinct key of the partition) is held in memory. A nest join packs the keys
of such a partition into groups that fit a block and joins one group per pass
over the right side: a left row leaves with its list in the pass of its key,
and a left row with no match at the end with an empty list. The list of a row
holds its key's right rows in right order, as in memory, so one key whose rows
pass the budget still makes one list that passes it. Each probe takes as many
left rows as the output share allows from what the last probes made.

``asof`` and ``interval`` stream under a budget. Both plans are sorted by
(``by`` keys, time) with the spilling sort and merged, so the join holds only
what the merge is on: for ``asof`` the right rows around the current left time
(the first and last row of the latest run of equal times), and for ``interval``
the ranges that are open at the current point, copied out of their morsels so a
long range does not keep a whole morsel. The rows come out ordered by (``by``,
time) with ties in input order, the order of the eager ops, in morsels of a
quarter of the budget; the pairs of one point that lies in more ranges than
fit that share are cut into several morsels. A plan with no budget runs under
the automatic one. When a plan's columns are only known after it runs (a
``pivot``, a ``to_dummies``), both plans are collected and the eager op runs. A range that is open at a
point is held in memory whatever the budget: ``interval`` over ranges that
all contain one point needs that many rows.

Some ops do not follow the budget. The registry frame ops (``gap_fill``, and
a window with no budget) take their input as
one frame, so the whole input and the whole result are in memory at once. A
frame op that has a native lazy op of the same name (``unique``, ``sort_by``,
``group_by``, ``join``, ``topk`` and the like) runs as that op, so it follows
the budget; so do ``filter`` by a mask column (``filter_mask``),
``sort_by_multi_per_col`` (``sort_by_multi``) and ``group_by_dynamic``. A
``column_op`` runs one morsel at a time when its op gives each row
from the same row of its operands (arithmetic, comparison, ``cast``, the string
and date ops). Under a budget, ``cumsum``, ``cum_prod``, ``cummax`` and
``cummin`` of an integer column and ``ffill``, ``diff`` and ``shift`` (by 0 to
1024 rows) of a numeric column also run one morsel at a time, with the running
value or the last rows of the morsel before carried over, so the result is the
op over the whole column, nulls and wrap-around included; the same ops over a
float column (a float sum depends on its order), ``rolling_*`` and any other
sorting, ranking or reducing column op need the whole column. A ``with_column``
of a column takes the matching rows of the column one morsel at a time. Any
other registry frame op (``union``, ``concat``, ``gap_fill``) holds its input
in memory up to a quarter of the budget and maps the rest from a spill file. The
mapped part costs memory only while the op reads it, but an op that needs one
contiguous column (a sort, a rank, a running op) copies that column back into
memory, and the op makes its whole result in memory: such an op holds one
column and its whole result. One group
larger than the budget stays whole: a group is never split, and a ``group_by``
that is still too large after three rounds of splitting its spill parts keeps
the part in memory. ``list_sorted`` and ``set_union`` keep every value of a
group, and their result for the group is as large as the group, so the budget
cannot bound them (``distinct`` and ``pct`` keep a sketch of fixed size). A
``group_by`` with dyn aggregates finalizes each spill part under the dyn column
names of all of them. A plugin node gets the plan's budget, but the host can
only ask it for bytes to give back: it cannot make the node stay under the
budget.

A resident in-memory source with no budget and no ``morsel_rows`` runs each op
over whole columns, as the eager API does, when the frame takes at most half of
the automatic budget. A plan with an op that can make more
than it reads (``join``, ``explode``, ``unpivot``, ``pivot``, ``to_dummies``,
``unnest``, ``concat``, ``take``, a ``group_by_dynamic`` whose period is longer
than its step, a registry frame op or a plugin node) streams under the automatic
budget instead. Group transforms, windows and column ops keep their input rows
and stay whole-column. A ``topk`` whose ``k`` rows, at the width of the first
morsel, do not fit half of the budget is cut from the spilling sort.

``group_by_dynamic`` needs an ascending time column, as the eager op does: a
window is final once a time past its end has been seen, so it leaves then and
the op holds only the open windows (about ``period / every`` of them). A time
below the one before raises ``ValueError`` (``std::invalid_argument``) in both
the lazy and the eager op, which before put such a row in a wrong window or
dropped it. ``to_dummies`` holds one entry and one output column
per distinct value. ``tail(n)``, ``topk(k)``,
``sample(n)`` and ``describe`` hold at most ``n`` or ``k`` rows or one figure
per column. The spill directory is ``DFTRACER_UTILS_SPILL_DIR`` or a
node-local disk, see :doc:`../runtime/memory-budget`.

Bring your own source
---------------------

A ``Source`` is two calls: ``schema()`` without a scan, and
``scan(ScanRequest)`` returning a ``Cursor`` whose ``next(max_rows)`` yields
a morsel or the end. The request carries what the plan wants pushed down:

- ``projection``: the columns, in order. Not advisory: a source that
  accepts it must return exactly those.
- ``filters``: candidate predicates, positional against the projection.
  Per filter the source reports ``Pushed::No`` (the engine applies it),
  ``Inexact`` (the source pruned I/O, the engine re-applies) or ``Exact``
  (the engine drops it). Claiming ``Exact`` wrongly drops rows; claiming
  ``No`` is always sound.
- ``limit``: a slice hint. ``memory_budget``: the plan's, for a source that
  buffers.

.. code-block:: cpp

   class MySource : public Source {
       Schema schema() const override;
       ScanResult scan(const ScanRequest& req) const override;   // a fresh Cursor
   };
   LazyFrame plan = LazyFrame::scan(std::make_shared<MySource>());

The cursor may also take part in two things the plan does while it runs:

``narrow(predicate)``
   An offer that arrives after the scan is open: a join's build side sends
   the keys it will keep, so the source can skip what cannot match (the
   trace scan turns it into an index prune of the chunks not yet read).
   Advisory: the engine still checks every row the cursor yields, so a
   source may ignore it. The offer passes up through filter, select,
   with_column, rename, sort, drop_nulls and with_row_index, each with its
   own column map; see :doc:`joins`.

``resident_bytes()`` / ``reclaim(want)``
   How much the cursor holds beyond one morsel, and a request to free some
   of it. After every morsel the driver sums the resident bytes of the
   plan's stages against the budget and asks the largest holders first.
   Built-in stages bound themselves and report 0; this is for a source or a
   plugin node that holds a table or a cache.

In C, the same contract is ``dftu_source_vt`` / ``dftu_cursor_vt``,
registered by name (``dftu_provider_register``, or ``DFTU_SVC_PROVIDERS``
from a plugin) and opened with ``dftu_lazyframe_from_provider``. A source
declares its column types through ``schema_types`` with the ``dftu_schema``
builders, nested types included; one that declares nothing reports
``Unknown``.

Planning hooks
~~~~~~~~~~~~~~

A source can also take whole ops at plan time, before ``scan``. The
optimizer offers the ops directly above the source, bottom up, through
``apply_filter``, ``apply_projection``, ``apply_aggregation``,
``apply_sort``, ``apply_topn``, ``apply_limit``, ``apply_tail`` and
``apply_join``. Each returns ``std::nullopt`` (the default: unsupported or
no change) or a new immutable source that carries the op, marked:

- ``Exact``: the new source returns what the op returns; the op leaves the
  plan.
- ``Inexact``: the new source only narrowed its rows; the op stays above it.
  After an inexact answer only filters are offered, since they commute.

The first refused op ends the walk. A projection, aggregation or join
changes the schema, so it may only be ``Exact``; the new source's
``schema()`` is its output. Offering the same op to the new source again
must return ``std::nullopt``. Aggregation keys and inputs arrive as
positional expressions (``AggregateSpec``), and a join arrives with the
other side already absorbed into its own source (``JoinSpec``); it is only
offered when nothing is left to run above that side. ``with_column`` steps
directly before a ``group_by`` are offered with it as one aggregation, each
key and input written as its expression over the source's columns, so
``with_column("big", col("dur") > 250).group_by("big")`` reaches the source
whole. ``explain()`` shows the plan after these hooks ran.

In C the hooks are one optional ``apply`` callback on ``dftu_source_vt``: a
``dftu_apply_request`` whose ``kind`` selects one ``dftu_apply_*_args``
member of its union, answered with a ``dftu_apply_result``
(``DFTU_APPLY_NO_CHANGE`` / ``EXACT`` / ``INEXACT`` plus a new
``self``/``vt`` the host owns and destroys once). A ``NULL`` callback keeps
the ``scan`` path alone.

Collecting several plans
~~~~~~~~~~~~~~~~~~~~~~~~

``collect_all(plans)`` returns one frame per plan, in order. Every plan is
optimized at every level (a join's or concat's other side and a frame op's
frame operands included), and the sources at all those leaves that report
the same ``batch_key()`` run through one ``collect_batch()`` call, so their
data is read once; each leaf then runs its remaining ops over its result.
``collect()`` does the same within one plan, so ``a.join(b)`` over one trace
reads it once. A trace source keys on its files and scan settings, so several
aggregations and row queries over one trace share a single scan.

When the plans of a group are the top-level plans of ``collect_all`` (not a
join's or concat's other side) and a source supports ``open_batch()``, the
shared read streams instead: each plan gets its own bounded cursor and runs its
remaining ops as rows arrive, and ``collect_all`` drains all the plans
together, so memory stays bounded by the budget rather than by the matching
rows. A plan that stops early (``head``) releases its share of the read. Other
groups (a child plan, or a plan starting with a projection) read through
``collect_batch()`` into memory. ``stream()`` does not batch: each leaf
streams on its own.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         auto frames = co_await collect_all({by_cat, posix_rows, by_name});

   .. tab-item:: Python

      .. code-block:: python

         by_cat, posix_rows, by_name = dft.collect_all([by_cat, posix_rows, by_name])

Results that are not one frame
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A ``LazyResult<T>`` is a lazy value of any type: the plans it reads and a
step that turns their frames into a ``T``. Its ``collect()`` returns the
``T``. A source uses it for a terminal that is not a frame. For example, a
trace's ``containment()`` gives both containment frames, a partial gives
mergeable bytes, and a sink gives its write stats. The C++ variadic
``collect_all(roots...)`` takes ``LazyFrame`` and ``LazyResult`` roots
together. It returns a tuple with one value per root, in order, and batches
the plans of all roots as above. Python's ``collect_all([...])`` takes both
kinds of root in one list and returns a list. A reduction of a bound column
(``tv["dur"].mean()``) is a ``LazyScalar``, a ``LazyResult`` of one value.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         auto [fg, both, stats] = co_await collect_all(
             tv.flamegraph(), tv.containment(), tv.sink_json(sink, LAZY));

   .. tab-item:: Python

      .. code-block:: python

         fg, both, stats, mean_dur = dft.collect_all([
             tv.flamegraph(), tv.containment(),
             tv.sink_json("out.pfw", lazy=True), tv["dur"].mean(),
         ])
         both.call_tree, both.flamegraph   # the Containment fields

A plugin's own step
-------------------

``LazyFrame::op(name, args)`` (``dftu_lazyframe_op`` in C) appends a plan
node a plugin registered with ``dftu_node_register``: a cursor that takes
the input cursor and returns its own, so a streaming operator (one morsel
in, one out, state kept between them) or a breaker (drain, then emit one
frame) sits in the plan like a built-in step. The node declares its output
schema from its input's; the optimizer types the plan around it and never
pushes a step through it. ``explain`` prints it as ``op <name>``.
:doc:`../../plugins` section 14 has the registration side.

Joining and stacking plans
--------------------------

``join(other, on, how)`` collects the right plan as the build side and
streams the left through it; ``concat(other)`` runs one plan after the
other. Both take another ``LazyFrame``, with its own source and steps.
:doc:`joins` has the join kinds and the narrowing a join sends its scan.

See also
--------

- :doc:`dataframe` for the ops themselves, eager or as steps.
- :doc:`../analysis/views` for the trace query a plan usually starts from.
- :doc:`../runtime/memory-budget` for the budget end to end.
- :doc:`../../concepts/fused-scan` for how a plan over a trace shares the
  scan with other work.
