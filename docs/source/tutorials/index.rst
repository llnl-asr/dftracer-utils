:description: A guided path from your first query to writing your own engine plugin, with each lesson meant to be read and typed top to bottom.

Tutorials
=========

A guided path from your first query to writing your own engine plugin. Unlike
the :doc:`Guides <../guides/index>` (which answer "how do I do X?"), these
lessons hold your hand: each one is a single sequence that is meant to be read
and typed top to bottom, and each is guaranteed to end somewhere real. Work them
in order - every lesson assumes the one before it.

If you have not built the library yet, do :doc:`../getting-started/installation`
first.

.. grid:: 1 1 3 3
   :gutter: 3

   .. grid-item-card:: 1. Your first analysis
      :link: first-analysis
      :link-type: doc

      **Easy.** Point at a directory of traces, filter to the events you care
      about, aggregate them, and read the result as a pandas DataFrame. The
      whole round trip in a dozen lines.

   .. grid-item-card:: 2. Analysis in depth
      :link: analysis-in-depth
      :link-type: doc

      **Intermediate.** Go beyond one query: derived columns, duql filters,
      DataFrame and Series operations, and exporting. You build
      a small real analysis end to end.

   .. grid-item-card:: 3. Extending the engine
      :link: extending-the-engine
      :link-type: doc

      **Advanced.** Write your own analytic that rides the one fused scan - first
      as a Python JIT plugin, then the C++ / C ABI equivalent - and run it over a
      trace.

   .. grid-item-card:: 4. Find a regression
      :link: find-a-regression
      :link-type: doc

      **Intermediate.** Build a baseline and a slower variant, compare the two
      runs, and read the per-category deltas to locate what got slower.

Specialized tracks
------------------

Once the core path feels comfortable, these stand-alone lessons each take one
audience end to end: C++ callers, the HTTP server, multi-shard scale-out, and
compiled plugins.

.. grid:: 1 1 2 2
   :gutter: 3

   .. grid-item-card:: Analyze traces in C++
      :link: analyze-in-cpp
      :link-type: doc

      The core path in C++: the ``View`` engine, the ``F``/``Field`` predicate
      builder, ``group_by``/``agg``, reading columns as typed ``Scalar``, and a
      custom ``fold`` aggregation.

   .. grid-item-card:: Serve and explore
      :link: serve-and-explore
      :link-type: doc

      Run ``dftracer_server`` over an index and query its REST routes
      (``/api/info``, ``/api/files``, ``/api/viz/*``) from the command line.

   .. grid-item-card:: Scale out
      :link: scale-out
      :link-type: doc

      Build independent index shards and query them together with a
      ``ShardedView``, then point at the Dask path for a real cluster.

   .. grid-item-card:: Write a C plugin
      :link: write-a-c-plugin
      :link-type: doc

      Scaffold, build, and run a compiled plugin that rides the fused scan,
      using the ``Batch``/``Event``/``Map`` SDK wrappers.

.. toctree::
   :hidden:
   :maxdepth: 1

   first-analysis
   analysis-in-depth
   extending-the-engine
   find-a-regression
   analyze-in-cpp
   serve-and-explore
   scale-out
   write-a-c-plugin
