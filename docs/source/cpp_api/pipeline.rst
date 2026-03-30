Pipeline Components
===================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/core>`.

Data processing pipeline for coroutine-based task execution. All classes are in the ``dftracer::utils`` namespace.

.. toctree::
   :maxdepth: 2
   :caption: Pipeline Components:

   pipeline/tasks
   pipeline/executors

Overview
--------

The pipeline is the top-level orchestrator that creates an executor, schedules
tasks, and runs them to completion. It manages the lifecycle of worker threads,
I/O backends, timer services, and the coroutine scheduler.

.. code-block:: cpp

   #include <dftracer/utils/core/pipeline/pipeline.h>
   #include <dftracer/utils/core/pipeline/pipeline_config.h>

   auto config = PipelineConfig()
       .with_name("MyPipeline")
       .with_compute_threads(8);

   auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
       // your coroutine work here
       co_return;
   }, "MainTask");

   Pipeline pipeline(config);
   pipeline.set_source({task});
   pipeline.execute();  // blocks until all tasks complete

Key Classes
-----------

- ``Pipeline`` — Top-level orchestrator; creates executor, runs tasks to completion
- ``PipelineConfig`` — Fluent configuration (thread count, I/O backend, watchdog, etc.)
- ``Executor`` — Coroutine scheduler with worker threads, I/O backend, and timer service
- ``Scheduler`` — Work-stealing task scheduler for coroutine handles
- ``Watchdog`` — Monitors task execution for hangs and deadlocks
- ``CoroScope`` — Structured concurrency scope for spawning child coroutines
- ``Task`` — DAG node with dependency tracking and coroutine body
