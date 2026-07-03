Executor Classes
=================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference </cpp_api/api/core>`.

Executor classes for running pipeline tasks.

.. mermaid::

   graph TB
       Executor["Executor"]
       Workers["Worker Threads<br/>(N threads)"]
       RunQueue["ConcurrentQueue<br/>(coroutine handles)"]
       IoBack["IoBackend"]
       Timer["TimerService"]

       Executor --> Workers
       Executor --> RunQueue
       Executor --> IoBack
       Executor --> Timer
       Workers --> |dequeue + resume| RunQueue

Executor
--------

The executor owns worker threads, an I/O backend, and a timer service. It is
the runtime engine that pulls coroutine handles from a
lock-free queue and resumes them on its thread pool. An ``Executor`` is created
automatically by ``Pipeline``, but can also be instantiated directly for testing
or embedding.

**Creating and starting an executor:**

.. code-block:: cpp

   ExecutorConfig config{
       .num_threads = 8,
       .io_backend_type = io::IoBackendType::AUTO
   };
   auto executor = std::make_unique<Executor>(config);
   executor->start();

   // ... submit work ...

   executor->shutdown();

**Submitting work:**

The primary way to submit work is through coroutine handles or tracked
coroutines:

.. code-block:: cpp

   // Low-level: enqueue a raw coroutine handle (~20ns, lock-free)
   executor->enqueue(some_coroutine_handle);

   // Tracked: enqueue a Coro with progress tracking
   TaskIndex id = executor->enqueue_tracked(std::move(my_coro), "parse_file");

   // DAG task: submit a Task with input and parent tracking.
   // input is a std::shared_ptr<std::any>; parent_task_id defaults to -1 (root).
   auto input = std::make_shared<std::any>(42);
   executor->submit_task(task, input, parent_task_id);

**Thread-local access:**

Worker threads can access their executor via the static ``current()`` method:

.. code-block:: cpp

   // Inside a coroutine running on the executor
   Executor* exec = Executor::current();
   if (exec && exec->has_io_backend()) {
       auto& io = exec->io_backend();
       // ... perform async I/O ...
   }

**Lifecycle methods:**

- ``start()`` -- spawn worker threads and I/O backends
- ``shutdown()`` -- graceful shutdown, waits for in-flight work to complete
- ``reset()`` -- prepare for a new execution round after shutdown
- ``request_shutdown()`` -- signal workers to stop accepting new tasks
- ``is_responsive()`` -- used by watchdog to detect hung executors

Configuration
-------------

``ExecutorConfig`` controls thread pool sizing, I/O backend selection, and
timeout thresholds. All fields have sensible defaults.

.. code-block:: cpp

   struct ExecutorConfig {
       std::size_t num_threads = 0;        // 0 = hardware_concurrency
       std::chrono::seconds idle_timeout{5};
       std::chrono::seconds deadlock_timeout{10};
       std::size_t io_pool_size = 0;      // 0 = hardware_concurrency
       io::IoBackendType io_backend_type = io::IoBackendType::AUTO;
       unsigned io_batch_threshold = 16;
   };

**Key fields:**

- ``num_threads`` -- Number of worker threads. Set to ``0`` (the default) to
  use ``std::thread::hardware_concurrency()``.
- ``idle_timeout`` / ``deadlock_timeout`` -- Thresholds for the watchdog to
  detect idle or hung workers.
- ``io_pool_size`` -- Size of the dedicated I/O thread pool backing the
  ``IoBackend``.
- ``io_backend_type`` -- Selects the async I/O implementation
  (``AUTO``, ``IO_URING``, ``EPOLL_THREADPOOL``, ``KQUEUE_THREADPOOL``,
  ``THREADPOOL``).
- ``io_batch_threshold`` -- Minimum number of I/O operations to batch before
  submitting to the backend.

**Example -- high-throughput configuration:**

.. code-block:: cpp

   ExecutorConfig config{
       .num_threads = 16,
       .io_pool_size = 8,
       .io_backend_type = io::IoBackendType::IO_URING,
       .io_batch_threshold = 32,
   };

Progress Tracking
-----------------

The executor maintains a task registry that tracks the state, timing, and
parent-child relationships of every submitted task. This powers the
``get_progress()`` API used for monitoring, debugging, and building progress
bars.

**Querying progress:**

.. code-block:: cpp

   ExecutorProgress progress = executor->get_progress();

   std::cout << "submitted=" << progress.total_tasks_submitted
             << " running=" << progress.tasks_running
             << " completed=" << progress.tasks_completed
             << " failed=" << progress.tasks_failed << "\n";

   for (auto& w : progress.workers) {
       std::cout << "worker " << w.worker_id
                 << (w.is_idle ? " idle" : " busy")
                 << " queue=" << w.local_queue_depth << "\n";
   }

**Task states:**

Each tracked task transitions through these states:

- ``QUEUED`` -- waiting in the run queue
- ``RUNNING`` -- currently executing on a worker thread
- ``WAITING`` -- suspended, waiting for child tasks
- ``COMPLETED`` -- finished successfully
- ``FAILED`` -- finished with an error

**Progress tree:**

``ExecutorProgress::root_tasks`` contains a recursive ``TaskProgress`` tree.
Each node exposes:

- ``progress_percentage`` -- 0--100 based on completed subtasks
- ``queued_duration_ms`` / ``execution_duration_ms`` -- timing breakdown
- ``location`` -- human-readable queue location (e.g. ``"executing_on_worker_3"``)
- ``children`` -- nested subtask progress

**Checking recent errors:**

.. code-block:: cpp

   for (auto& [task_id, msg] : progress.recent_errors) {
       std::cerr << "task " << task_id << " failed: " << msg << "\n";
   }

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference </cpp_api/api/core>`.
