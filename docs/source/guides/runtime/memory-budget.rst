:description: Keep a scan or aggregation inside a memory ceiling: cap and spill a view's group map, and check whether a footprint fits across nodes.

Bound memory usage
==================

.. admonition:: Goal
   :class: goal

   Keep a scan or aggregation inside a memory ceiling instead of letting the
   group map grow until the process is killed. Two tools do this: a builder setting
   on the view that caps and spills intermediate memory, and a free advisory that
   tells you whether a required footprint fits and how many nodes to spread it
   across.

Cap and spill a view
--------------------

``View::memory_budget(bytes)`` (in ``dftracer/utils/trace/views/view.h``,
namespace ``dftracer::utils::trace::views``) sets one ceiling that bounds two
things during a run, both intermediate memory, never the returned result:

- **Scan decode.** The bytes decompressed concurrently across worker slices are
  always held under the ceiling, so a large trace cannot OOM the box. This bound
  is always on: with no explicit budget it defaults to a RAM fraction (about one
  third of detected memory: the cgroup limit, ``/proc/meminfo``, or the free
  and inactive pages on macOS; 1 GB where none can be read), and a single unit larger than the
  whole ceiling still runs once the scan is otherwise idle rather than
  deadlocking. A scan that cannot make progress fails loudly instead of hanging.
- **Group-map spill.** Set an explicit non-zero ``bytes`` and each worker's
  aggregation group map spills to sorted temporary runs past that size. ``0``
  (the default) keeps the group map purely in memory - no spilling - while the
  scan-decode bound above still applies.

``auto_spill()`` is the convenience form: it caps at roughly one third of
available memory, enabling group-map spill at that size. Both are chainable and
available on the Python viewer too. Pick an explicit ``memory_budget`` when an
aggregation's distinct-group count is large enough to want spilling; leave it
unset to rely on the always-on scan-decode default.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>
         using namespace dftracer::utils::trace::views;

         auto df = view
             .memory_budget(2ull * 1024 * 1024 * 1024)   // 2 GiB per worker
             .group_by({GroupKey::cat()})
             .agg({{AggOp::Sum, "dur", "sum_dur"}})
             .collect()
             .get();

         auto spilled = view.auto_spill()
                            .group_by({GroupKey::cat()})
                            .agg({{AggOp::Count, "", "count"}})
                            .collect()
                            .get();

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer

         tv = TraceViewer("./traces")

         result = (
             tv.memory_budget(2 * 1024**3)   # 2 GiB per worker
               .group_by("cat")
               .agg("sum:dur")
               .collect()                    # -> LazyFrame
               .collect()                    # -> DataFrame
         )

         spilled = tv.auto_spill().group_by("cat").agg("count").collect().collect()

Hold a plugin to the budget
---------------------------

The same ceiling reaches a plugin's own state. A registered state
(``dftu_svc_agg``) is spilled by the host through its ``serialize`` pair.
A plugin's fold slice and a plan node it registers are measured and asked
to shrink instead:

- ``dftu_plugin::bytes(slice)`` reports the slice's size; after a step or a
  merge that leaves it past the budget the host calls
  ``dftu_plugin::reclaim(slice, want)``, which frees what it can (writes a
  part out, compacts, drops a cache) and returns the bytes freed. On the
  C++ facade a Slice with ``bytes() const`` and ``reclaim(std::uint64_t)``
  gets both slots from ``make_plugin``.
- ``dftu_cursor_vt::bytes`` and ``::reclaim`` do the same for a plan node's
  cursor: after every morsel the driver sums the resident bytes of the
  chain's stages against the plan's ``memory_budget`` and asks the largest
  holders to reclaim until it fits.

Both are advisory. A slice or node that declares no size is never asked; one
that measures itself but frees nothing is logged once, and the run goes on
over budget rather than stopping, since the result is not in question.

Check a footprint before you run
--------------------------------

``memory_budget_advice`` judges a required byte footprint against available
memory (auto-detected and cgroup-aware when you pass ``0`` or omit it). It
returns whether the footprint fits, the peak it expects, and how many nodes
would spread that peak under the available limit.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/core/common/memory_budget.h>
         using namespace dftracer::utils;

         MemoryBudgetAdvice advice =
             memory_budget_advice(/*required_bytes=*/40ull * 1024 * 1024 * 1024);

         if (!advice.fits) {
             std::cerr << format_memory_budget_warning(advice) << '\n';
             // advice.suggested_nodes: nodes to spread the peak under available
         }

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils.dftracer_utils_ext import memory_budget_advice

         advice = memory_budget_advice(required_bytes=40 * 1024**3)
         # dict: fits, required_bytes, peak_bytes, available_bytes,
         #       suggested_nodes, warning
         if not advice["fits"]:
             print(advice["warning"])
             print("spread across", advice["suggested_nodes"], "nodes")

The returned advice carries ``fits`` (peak within available), ``required_bytes``
(what you passed), ``peak_bytes`` (required times a peak factor),
``available_bytes`` (what it judged against), and ``suggested_nodes``. The C++
struct is ``MemoryBudgetAdvice``; the Python function returns the same fields as
a dict plus a ``warning`` string (empty when it fits).

.. note::

   ``compute_memory_budget`` and ``estimate_per_file_bytes`` (same C++ header)
   compute a budget and a per-file estimate from a list of file sizes. They are
   C++-only; the viewer calls ``compute_memory_budget`` internally to pick a
   default. Only ``memory_budget_advice`` is exposed as a standalone Python
   function.

See also
--------

- :doc:`../../trace-viewer` - the viewer whose builder ``memory_budget`` tunes.
- :doc:`../scale/mpi` - spread a job across ``suggested_nodes`` with the MPI
  binaries.
