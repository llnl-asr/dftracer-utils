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

         plan = TraceViewer("traces/").filter('cat == "POSIX"').collect()  # a plan over the scan
         plan = df.lazy()                                                  # a plan over a frame

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

A trace query's ``collect()`` returns a plan over the fused scan
(``ViewSource``), so the steps you add after it run inside the same pull
chain as the scan; ``collect()`` on that plan is what reads the files.

Read the plan before it runs
----------------------------

``explain()`` prints the optimized plan, the source then one step per line.
``schema()`` gives the output column names and ``output_schema()`` the typed
fields, both without running: the plan walks its steps from the source's
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
   plan.schema()      # ['name', 'count', 'sum_dur']

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
streaming source runs morsel by morsel. ``stream()`` (C++) yields the morsels instead, each a standalone
frame, so a consumer can stop early: dropping the generator is the scan's
early-out.

``memory_budget(bytes)`` bounds every breaker (sort, unique, group-by, the
spools behind reverse / take / pivot): past the budget their state spills to
sorted temporary runs and is merged back at the end, so peak memory is the
budget plus one morsel. ``0`` is auto (about a third of available memory);
``auto_spill()`` says that at the call site. The same budget reaches a
plugin node in the plan and a plugin's fold slice, see
:doc:`../runtime/memory-budget`.

.. code-block:: python

   out = plan.memory_budget(2 * 1024**3).collect(morsel_rows=65536)  # or collect(): auto

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
