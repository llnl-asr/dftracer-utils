:description: How the View engine parses each trace event exactly once and folds many analytics over that single pass instead of one pass per consumer.

The fused scan
==============

What this explains: why dftracer-utils parses each trace event exactly once
no matter how many analytics are running over it, and how that "one pass,
many folds" model is built into ``View``.

The problem: N analytics should not mean N passes
-------------------------------------------------------

A trace directory can hold gigabytes of gzip-compressed JSON-lines events. If
computing statistics, running an aggregation, and running a loaded plugin
each meant decompressing and parsing the trace on their own, the cost of
asking three questions would be three times the cost of asking one - even
though every one of those consumers wants to look at the same events. The
naive per-analytic pass is the thing the engine is built to avoid.

One traversal, many folds
------------------------------

``trace::views::View`` (``include/dftracer/utils/trace/views/view.h``) is a
lazy, composable plan over trace events: builder methods (``filter``,
``phase``, ``time_range``, ``group_by``, ``agg``, ...) return a new ``View``
and do no I/O by themselves. Nothing runs until a terminal call executes the
plan, at which point the surviving chunks (after :doc:`predicate pushdown
<indexing-and-pushdown>`) are decompressed and parsed exactly once.

What happens during that one pass is a fold, not a callback per event: each
worker slice accumulates its own partial state as it walks a chunk, and the
partials are combined once the scan finishes. The session is the surface that
makes many folds share a single scan explicitly: open one off a base view,
register several branches (a ``collect``, an ``export``, a ``materialize``, a
custom fold, or a compiled plugin / JIT op), then run them together. Every
registered branch sees every matching event from the same traversal.

.. admonition:: Recommended
   :class: tip

   When you need more than one read of the same trace - a few unrelated
   aggregations, or an aggregate next to an export - reach for a session
   rather than issuing each read on its own. Separate reads each pay for their
   own decompression and JSON parse; a session pays once and splits the result.

.. tab-set::

   .. tab-item:: C++

      ``TraceSession`` (``base_view.session()``) registers ops - ``collect``
      (over any ``LazyFrame`` or ``LazyResult``: an aggregation view's
      ``.lazy()``, a plugin attached through ``View::branch``, or any other
      plan), ``sink_json``, ``materialize`` - each returning a ``Deferred<T>``,
      then ``execute()`` runs the one scan.

      .. code-block:: cpp

         View counted = base_view.filter(predicate_a)
                            .group_by(group_by_a)
                            .agg(agg_a);

         TraceSession session = base_view.session();
         auto counts = session.collect(counted.lazy());
         auto exported = session.sink_json(base_view.filter(predicate_b), sink);
         co_await session.execute();   // one scan, both branches resolved

         counts->num_rows();           // read a Deferred after execute()

   .. tab-item:: Python

      ``TraceViewer.session()`` registers lazy plans: ``collect`` takes any
      ``LazyFrame`` or ``LazyResult`` over the trace, and ``sink_json``,
      ``materialize``, and ``attach`` (a compiled plugin set) are branches
      too. Each returns a ``Handle``. Leaving the ``with`` block (or the first
      ``Handle.result()``) runs the one scan.

      .. code-block:: python

         from dftracer.utils.plugins import Plugins

         with base_view.session() as s:
             counts = s.collect(base_view.group_by("cat").agg("count"))
             stats  = s.collect(base_view.statistics(lazy=True))
             hist   = s.attach(Plugins(["dur_histogram.so"]))   # plugin/JIT fold
             s.sink_json(base_view.filter('cat == "POSIX"'), "posix.pfw")

         counts_df = counts.result()   # resolved from the shared scan
         stats_dict = stats.result()
         hist_df = hist.result()["dur_histogram"]

      ``collect_all([...])`` does the same without a session: it collects a
      list of plans and lazy results, one value per root, and plans over the
      same trace share one scan.

An aggregation view can be diffed against a second view's events after the one
scan with ``View::compare`` in C++:
``base.group_by(keys).agg(specs).compare(variant)`` returns a ``LazyFrame``
already joined on the group key and carrying ``delta_``/``pct_`` per metric,
and since both views read the same files the comparison still costs one
traversal. In Python the join is part of the plan:
``s.collect(a.join(b, on="cat"))`` over two plans of one trace, or
``base.compare(variant)``, still reads the trace once. See
:doc:`../guides/analysis/aggregation` for the Python how-to and
:doc:`../guides/analysis/views` for the C++ terminals.

This is why a CLI invocation with multiple analytics, or a query that runs a
compiled plugin alongside a built-in aggregation, does not multiply the I/O
and JSON-parsing cost by the number of things being computed. A plugin
(``plugins/plugin.h``) plugs into this same model from the other side: its
``on_batch`` hook receives the already-parsed batch the scan produced,
folding into its own state the same way an internal branch does, not
re-reading or re-parsing anything.

.. mermaid::

   graph LR
       Chunks["Pruned chunks"] --> Parse["Parse once<br/>(per worker slice)"]
       Parse --> A["Fold A<br/>(built-in aggregation)"]
       Parse --> B["Fold B<br/>(statistics)"]
       Parse --> C["Fold C<br/>(plugin on_batch)"]
       A --> Combine["Combine partials"]
       B --> Combine
       C --> Combine
       Combine --> Result["DataFrame(s)"]

Why the parsed event, not raw bytes, is what is shared
------------------------------------------------------------

The scan hands each branch the already-parsed event (and, where useful, the
raw JSON bytes for verbatim passthrough) rather than making every branch
re-parse the JSON itself. Parsing is not free at trace scale, and it is the
part of the per-event cost that is identical no matter which analytic is
looking at the event - so it is the part that is done once and shared, while
each branch's own accumulation logic (which fields it reads, what it keeps)
stays branch-specific and independent.

Where this fits
--------------------

The fused scan is the stage between :doc:`indexing-and-pushdown` (which
decides which chunks are even worth decompressing) and the
:doc:`dataframe-model` (which is what a fold's result becomes once
combined). See :doc:`architecture` for how these stages compose into a full
query.

See also
--------

- :doc:`indexing-and-pushdown` for what happens before the scan starts.
- :doc:`dataframe-model` for the result type a fold produces.
- :doc:`../plugins` for authoring a plugin that rides this scan.
- :doc:`../guides/core/query-dsl` for the predicate language used to filter
  a ``View``.
