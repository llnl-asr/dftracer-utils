:description: Task-oriented how-tos grouped by area, with co-equal C++ and Python examples, for readers past the getting-started and tutorial material.

Guides
======

Task-oriented how-tos for readers who have finished :doc:`Get started
<../getting-started/index>` and the :doc:`Tutorials <../tutorials/index>`. Each
guide starts from a goal and ends at the solved state. C++ and Python are
co-equal: every task that both languages support shows both, with a C tab where
the ABI is the intended interface.

The guides are grouped by what you want to do. Start with the three below,
then open the group that matches your task.

Start here
----------

.. grid:: 1 1 3 3
   :gutter: 3

   .. grid-item-card:: Choose an API
      :link: choosing-an-api
      :link-type: doc

      Pick TraceViewer, View, DataFrame, a plugin or the C ABI.

   .. grid-item-card:: From traces to a result
      :link: end-to-end
      :link-type: doc

      Follow one job from raw traces to a pandas frame.

   .. grid-item-card:: Troubleshoot
      :link: troubleshooting
      :link-type: doc

      Match a symptom to its cause and fix.

Guides by task
--------------

.. grid:: 1 1 2 2
   :gutter: 3

   .. grid-item-card:: :octicon:`search` Query traces
      :link: query-traces
      :link-type: doc

      Open traces, filter them with duql, keep an index fresh and run many reads over one scan.

   .. grid-item-card:: :octicon:`graph` Analyze traces
      :link: analyze-traces
      :link-type: doc

      Roll events up, measure distributions, compare runs and hand results to other tools.

   .. grid-item-card:: :octicon:`table` DataFrames and Series
      :link: dataframes
      :link-type: doc

      The columnar engine behind every result: build, compute, join and reshape columns natively.

   .. grid-item-card:: :octicon:`plug` Extend the engine
      :link: extend-the-engine
      :link-type: doc

      Add your own analysis that rides the fused scan, from C, C++ or Python.

   .. grid-item-card:: :octicon:`rocket` Runtime and performance
      :link: runtime-and-performance
      :link-type: doc

      Tune threads and memory, and build your own concurrent workflows on the coroutine runtime.

   .. grid-item-card:: :octicon:`server` Scale out and serve
      :link: scale-and-serve
      :link-type: doc

      Run across ranks and dask workers, and serve indexed traces over HTTP.

   .. grid-item-card:: :octicon:`tools` Standalone tools
      :link: standalone-tools
      :link-type: doc

      Command-line tools and building blocks for preparing, replaying and generating traces.

.. toctree::
   :hidden:

   choosing-an-api
   end-to-end
   troubleshooting
   query-traces
   analyze-traces
   dataframes
   extend-the-engine
   runtime-and-performance
   scale-and-serve
   standalone-tools
