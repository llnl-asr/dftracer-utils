:description: Tune threads and memory, and build your own concurrent workflows on the coroutine runtime.

Runtime and performance
=======================

Tune threads and memory, and build your own concurrent workflows on the coroutine runtime.

.. grid:: 1 1 2 2
   :gutter: 3

   .. grid-item-card:: Tune performance
      :link: runtime/performance
      :link-type: doc

      Worker threads and the knobs for scans and aggregations.

   .. grid-item-card:: Bound memory
      :link: runtime/memory-budget
      :link-type: doc

      Keep a scan or aggregation under a memory ceiling.

   .. grid-item-card:: Concurrency patterns
      :link: pipelines/patterns
      :link-type: doc

      Fan-out, fan-in, racing tasks and streaming channels.

   .. grid-item-card:: Build a task graph
      :link: runtime/task-graphs
      :link-type: doc

      Express a computation as a DAG of coroutine tasks.

   .. grid-item-card:: The pipeline
      :link: ../pipeline
      :link-type: doc

      The executor, scopes and structured concurrency.

.. toctree::
   :hidden:

   runtime/performance
   runtime/memory-budget
   pipelines/patterns
   runtime/task-graphs
   ../pipeline
