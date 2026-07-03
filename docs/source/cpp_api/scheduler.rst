Scheduler & Watchdog
====================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/core>`.


Task scheduling, dependency tracking, and timeout monitoring.
All classes are in the ``dftracer::utils`` namespace.

For executor configuration and progress tracking, see :doc:`pipeline/executors`.
For task creation and DAG building, see :doc:`pipeline/tasks`.

.. mermaid::

   graph TB
       subgraph Scheduling["Task Scheduling"]
           Scheduler["Scheduler"]
           Watchdog["Watchdog"]
           Executor["Executor"]
       end

       subgraph Policies["Error Handling"]
           ErrorPolicy["ErrorPolicy enum"]
           ErrorHandler["ErrorHandler callback"]
       end

       subgraph Monitoring["Monitoring"]
           TaskExec["TaskExecution"]
           TimeoutCB["TimeoutCallback"]
           WarningCB["WarningCallback"]
       end

       Scheduler --> Executor
       Scheduler --> Watchdog
       Scheduler --> ErrorPolicy
       Scheduler --> ErrorHandler
       Watchdog --> TaskExec
       Watchdog --> TimeoutCB
       Watchdog --> WarningCB

Scheduler
---------

DAG task scheduler with dependency tracking and error handling.

The Scheduler manages the lifecycle of tasks in a directed acyclic graph (DAG):

1. Validates task dependencies and input types
2. Prepares inputs from parent task results (using combiners)
3. Submits ready tasks to the Executor
4. Tracks completion and propagates results to children
5. Handles errors according to the configured ErrorPolicy

**Error policies:**

- ``FAIL_FAST`` - Stop immediately on first error (default)
- ``CONTINUE`` - Continue other branches, skip children of the failed task
- ``CUSTOM`` - Delegate to a user-provided ``ErrorHandler`` callback

Defined in ``core/pipeline/pipeline_config.h``. There is no ``RETRY`` policy at
this layer; retry is a utility-level behavior. ``ErrorHandler`` is
``std::function<void(std::shared_ptr<Task>, std::exception_ptr)>``.

**Construction:**

.. code-block:: cpp

    // Minimal: scheduler over an executor (no watchdog)
    explicit Scheduler(Executor* executor);

    // Full: bind a watchdog and take timeouts/policy from PipelineConfig
    Scheduler(Executor* executor, Watchdog* watchdog, const PipelineConfig& config);

In practice ``Pipeline`` owns the ``Runtime`` (executor + watchdog) and the
``Scheduler``; you rarely construct one directly. Construct it manually only
when embedding the runtime or writing tests.

**Key methods:**

- ``schedule(source, input)`` - begin execution from a source task; also a
  typed overload ``schedule<T>(source, T&&)``
- ``submit_dynamic_task(task, input)`` - submit a task discovered at runtime
  (intra-task parallelism)
- ``set_error_policy(policy)`` / ``set_error_handler(handler)``
- ``set_progress_callback(cb)`` - ``cb(completed, total)``
- ``set_global_timeout(ms)`` / ``set_default_task_timeout(ms)`` (both take
  ``std::chrono::milliseconds``; ``0`` = wait forever)
- ``request_shutdown()`` / ``is_shutdown_requested()`` - cooperative shutdown
- ``reset()`` - clear state for a fresh execution round

**Timeout support:**

- Global timeout for entire pipeline execution
- Per-task timeout with configurable defaults
- Watchdog thread monitors for deadlocks and stalls

Usage example:

.. code-block:: cpp

    auto executor = std::make_unique<Executor>(ExecutorConfig{.num_threads = 4});
    Scheduler scheduler(executor.get());

    scheduler.set_error_policy(ErrorPolicy::FAIL_FAST);
    scheduler.set_global_timeout(std::chrono::seconds(300));
    scheduler.set_progress_callback([](size_t completed, size_t total) {
        std::cout << completed << "/" << total << " tasks done\n";
    });

    // Schedule a root task
    auto task = make_task([](CoroScope& ctx, const std::any& input) -> CoroTask<int> {
        co_return 42;
    });
    scheduler.schedule(task, std::any{});

Watchdog
--------

Independent monitoring thread for timeout detection and responsiveness checks.

The Watchdog runs on a separate thread and periodically checks:

- **Global timeout** - Has the entire pipeline exceeded its time budget?
- **Per-task timeout** - Has any individual task exceeded its timeout?
- **Responsiveness** - Is the executor still making progress?

When a timeout is detected, the Watchdog invokes the configured callback and
can request executor shutdown for graceful termination.

Responsiveness (whether the executor is still making progress) is judged
against the executor's own ``ExecutorConfig::idle_timeout`` and
``ExecutorConfig::deadlock_timeout`` (see :doc:`pipeline/executors`), not
against the Watchdog's per-task or global timeouts.

**Constructor:**

.. code-block:: cpp

    explicit Watchdog(
        std::chrono::milliseconds check_interval = std::chrono::milliseconds(100),
        std::chrono::milliseconds global_timeout = std::chrono::milliseconds(0),
        std::chrono::milliseconds default_task_timeout = std::chrono::milliseconds(0),
        std::chrono::milliseconds warning_threshold = std::chrono::milliseconds(10000));

All four parameters have defaults; ``0`` for a timeout means "no timeout". The
``warning_threshold`` (default 10s) controls the long-running-task warning and
has no default of zero, so warnings fire unless you disable the callback.

**Timeout hierarchy:**

1. Per-task timeout (from ``Task::with_timeout()`` or ``set_default_task_timeout()``)
2. Global timeout (set via ``set_global_timeout()``)
3. Long-task warning threshold (``warning_threshold``, configured at construction)

Typically the Watchdog is owned by the ``Runtime`` and configured from
``PipelineConfig`` (``with_watchdog``, ``with_watchdog_interval``,
``with_global_timeout``, ``with_task_timeout``, ``with_warning_threshold``); you
seldom construct it directly.

Usage example:

.. code-block:: cpp

    using namespace std::chrono_literals;

    Watchdog watchdog(
        100ms,   // check interval
        30s,     // global timeout
        10s,     // default task timeout
        10s      // warning threshold for long-running tasks
    );

    watchdog.set_timeout_callback([](const std::string& msg) {
        std::cerr << "TIMEOUT: " << msg << "\n";
    });

    watchdog.set_warning_callback([](const std::string& task, int64_t ms) {
        std::cerr << "WARNING: " << task << " running for " << ms << "ms\n";
    });

    watchdog.start();
    // ... run pipeline ...
    watchdog.stop();

TaskExecution
~~~~~~~~~~~~~

Active task execution metadata tracked by the Watchdog:

- ``task`` - Shared pointer to the tracked ``Task``
- ``start_time`` - When the task started executing
- ``timeout`` - Task-specific timeout (0 = no timeout)
- ``warning_logged`` - Whether a slow-task warning has been logged

See :doc:`api/core` for full struct definition.
