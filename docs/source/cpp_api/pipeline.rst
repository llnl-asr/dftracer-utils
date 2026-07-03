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

- ``Pipeline`` - Top-level orchestrator; creates executor, runs tasks to completion
- ``PipelineConfig`` - Fluent configuration (thread count, I/O backend, watchdog, etc.)
- ``Executor`` - Coroutine scheduler with worker threads, I/O backend, and timer service
- ``Scheduler`` - Work-stealing task scheduler for coroutine handles
- ``Watchdog`` - Monitors task execution for hangs and deadlocks
- ``CoroScope`` - Structured concurrency scope for spawning child coroutines
- ``Task`` - DAG node with dependency tracking and coroutine body

Pipeline
--------

``Pipeline`` validates the DAG (reachability, type compatibility, cycles), then
delegates execution to a ``Scheduler`` running on an internally owned
``Runtime``. Multiple sources or destinations are stitched together with an
auto-created ``NoOpTask``.

**Key methods:**

- ``set_source(task)`` - single source; overloads accept an initializer list,
  a ``std::vector``, or a variadic pack of tasks (auto-wrapped in a ``NoOpTask``)
- ``set_destination(...)`` - optional; if unset, all terminal tasks are outputs
- ``validate()`` - run DAG checks without executing
- ``execute(input = {})`` - blocks until completion, returns ``PipelineOutput``;
  a typed overload ``execute<T>(T&&)`` wraps the input in ``std::any``
- ``set_error_policy(policy)`` / ``set_progress_callback(cb)``
- ``get_source()`` / ``get_destination()`` / ``get_all_tasks()`` / ``get_name()``

.. code-block:: cpp

   Pipeline pipeline(PipelineConfig().with_name("MyPipeline"));
   pipeline.set_source({task_a, task_b});   // NoOpTask parent auto-created
   PipelineOutput out = pipeline.execute(42);

PipelineConfig
--------------

Fluent configuration struct. Every ``with_*`` setter returns ``*this`` for
chaining. Notable fields (all have defaults):

- ``name`` - pipeline name (``with_name``)
- ``executor_threads`` - compute worker count, ``0`` = ``hardware_concurrency``
  (``with_compute_threads``)
- ``error_policy`` / ``error_handler`` - ``with_error_policy`` /
  ``with_error_handler`` (the latter also flips policy to ``CUSTOM``)
- ``enable_watchdog`` (default ``true``, ``with_watchdog``)
- ``global_timeout`` / ``default_task_timeout`` (default ``0`` = forever;
  ``with_global_timeout`` / ``with_task_timeout``, ``std::chrono::seconds``)
- ``watchdog_interval`` (default 1s), ``long_task_warning_threshold`` (default
  300s, ``with_warning_threshold``)
- ``executor_idle_timeout`` (300s) / ``executor_deadlock_timeout`` (600s)
- ``timeslice_duration`` (default 10ms, ``0`` disables cooperative yielding)
- ``io_thread_count``, ``io_backend_type`` (``AUTO``), ``io_batch_threshold`` (16)

Named factories provide common presets:

.. code-block:: cpp

   auto seq  = PipelineConfig::sequential();          // 1 thread, no watchdog
   auto par  = PipelineConfig::parallel(8);            // 8 threads + watchdog
   auto def  = PipelineConfig::default_config();       // hardware_concurrency
   auto tmo  = PipelineConfig::with_timeouts(4,
                   std::chrono::seconds(60),           // global timeout
                   std::chrono::seconds(30));          // per-task timeout

PipelineOutput
--------------

The return type of ``Pipeline::execute()``. It is a
``std::unordered_map<TaskIndex, std::any>`` mapping each terminal task id to its
result, plus convenience accessors:

- ``get()`` / ``get<T>()`` - value of the single output (throws
  ``PipelineError`` if the pipeline has more than one output)
- ``get(id)`` / ``get<T>(id)`` - value for a specific task id
- ``first()`` / ``first<T>()`` - first available output
- implicit ``operator std::any()`` - shorthand for the single-output case

.. code-block:: cpp

   PipelineOutput out = pipeline.execute();
   int result = out.get<int>();                 // single-output pipeline
   auto specific = out.get<std::string>(task->get_id());

PipelineError
-------------

Exception type thrown on pipeline failures (derives from ``DFTUtilsException``).
``get_type()`` returns one of:

- ``TYPE_MISMATCH`` / ``TYPE_MISMATCH_ERROR`` - incompatible edge types
- ``VALIDATION_ERROR`` - DAG validation failed
- ``EXECUTION_ERROR`` - a task threw during execution
- ``INITIALIZATION_ERROR`` - setup failure
- ``OUTPUT_CONVERSION_ERROR`` - bad ``PipelineOutput`` access/cast
- ``TIMEOUT_ERROR`` - pipeline or task timeout
- ``INTERRUPTED`` - graceful shutdown requested
- ``EXECUTOR_UNRESPONSIVE`` - executor hung or crashed
- ``UNKNOWN_ERROR``

.. code-block:: cpp

   try {
       auto out = pipeline.execute();
   } catch (const PipelineError& e) {
       if (e.get_type() == PipelineError::TIMEOUT_ERROR) { /* handle */ }
       std::cerr << e.what() << "\n";
   }
