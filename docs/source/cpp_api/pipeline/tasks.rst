Task System
===========

Task-based DAG execution and coroutine scoping for structured concurrency.

CoroScope
---------

Lightweight structured concurrency scope using Coro + JoinHandle.

CoroScope is the primary context type passed to task lambdas. It provides:

- ``spawn()`` returning ``SpawnFuture<T>`` for all coroutines (void and typed). The return value can be ignored for fire-and-forget usage, or ``co_await``'d to wait for that specific coroutine.
- Channel operations (send/receive)
- Producer-consumer patterns with helpers
- Structured cancellation support
- Automatic join() on scope exit

CoroScope replaces the old TaskScope with unified coroutine scheduling,
eliminating the Task/Scheduler overhead for lightweight work.

**Basic spawning:**

Fire-and-forget (return value ignored)::

    scope.spawn([](CoroScope& s) -> CoroTask<void> {
        // do work
        co_return;
    });

Await a void spawn::

    co_await scope.spawn([](CoroScope& s) -> CoroTask<void> {
        // caller suspends until this completes
        co_return;
    });

Typed result with SpawnFuture::

    int result = co_await scope.spawn([](CoroScope& s) -> CoroTask<int> {
        co_return 42;
    });

Or capture the future for later::

    auto future = scope.spawn([](CoroScope& s) -> CoroTask<int> {
        co_return 42;
    });
    // ... do other work ...
    int result = co_await future;

**Channel patterns:**

Spawn producers and consumers::

    auto channel = make_channel<int>(100);
    
    scope.spawn_producers(channel, 2, [](CoroScope& s, size_t id) -> CoroTask<void> {
        for (int i = 0; i < 10; ++i)
            co_await channel.send(i);
        co_return;
    });
    
    scope.spawn_consumers(channel, 2, [](CoroScope& s, int value) -> CoroTask<void> {
        process(value);
        co_return;
    });

**Structured concurrency:**

Must call ``co_await scope.join()`` before the scope is destroyed.
This waits for all spawned coroutines to complete.

**Cancellation:**

Check ``is_cancellation_requested()`` to support graceful cancellation::

    while (!scope.is_cancellation_requested()) {
        // do work
        co_await maybe_yield();
    }

.. doxygenclass:: dftracer::utils::CoroScope
   :project: dftracer-utils
   :members:
   :undoc-members:

TaskResult
----------

Lightweight one-shot result holder for Task completion.

TaskResult supports both blocking wait (for tests) and co_await (for runtime).
It is embedded in Task (~48 bytes) with no heap allocation for shared state.

**States:**

- pending: Not started
- running: Execution in progress
- value: Completed with value
- exception: Completed with exception
- cancelled: Cancelled before completion

**Write API (called once by executor):**

- ``set_value()`` - Mark task complete with result
- ``set_exception()`` - Mark task complete with exception
- ``set_cancelled()`` - Mark task as cancelled
- ``mark_running()`` - Mark task as executing

**Blocking read API (tests, scheduler, pipeline):**

- ``wait()`` - Block until ready (returns false on timeout)
- ``get()`` - Block until ready, return copy of value (throws on exception)
- ``get_ready()`` - Return value without blocking (asserts ready state)
- ``get_exception()`` - Return exception pointer without blocking
- ``is_ready()`` - Query ready state without blocking

**Coroutine read API (runtime):**

- ``when_ready()`` - Awaitable that suspends until result is ready

**Memory optimization:**

Smart value release via reader tracking:

- ``add_reader()`` - Register a consumer (called by depends_on())
- ``release_reader()`` - Signal consumer is done (called by Scheduler)

Value is automatically freed when last reader releases, except for terminal tasks
(no children) where the value persists for user get().

.. doxygenclass:: dftracer::utils::TaskResult
   :project: dftracer-utils
   :members:
   :undoc-members:

Task
----

Self-contained DAG node with dependencies.

Task represents a single node in a directed acyclic graph (DAG) of work.
Each task:

- Owns a TaskResult for result retrieval
- Knows its parents and children (DAG structure)
- Is immutable after construction (blueprint pattern)
- Supports automatic tuple packing for multiple parents
- Validates types during edge creation

**Fluent API for building DAGs:**

Single parent dependency::

    auto task2 = make_task([](CoroScope& ctx, const std::any& input) -> CoroTask<int> {
        // process input
        co_return 42;
    })->depends_on(task1);

Multiple parent dependencies::

    auto task3 = make_task([](CoroScope& ctx, const std::any& input) -> CoroTask<void> {
        // input is a tuple packed by combiner
        co_return;
    })->depends_on(task1, task2, task3);

Custom combiner for typed inputs::

    auto task3 = make_task([](CoroScope& ctx, const std::any& input) -> CoroTask<void> {
        // combine task1 and task2 results
        co_return;
    })->depends_on(task1, task2)
     ->with_combiner([](int a, std::string b) -> std::any {
         return static_cast<std::any>(a + b.size());
     });

**Task lifecycle:**

1. Created via make_task()
2. Dependencies added via depends_on()
3. Optional combiner set via with_combiner()
4. Scheduled by Scheduler
5. Executor runs the task function with CoroScope and input
6. Result stored in TaskResult
7. Children are enqueued when all parents complete

.. doxygenclass:: dftracer::utils::Task
   :project: dftracer-utils
   :members:
   :protected-members:
   :undoc-members:

make_task
---------

Create a new Task with a given function.

.. doxygenfunction:: dftracer::utils::make_task(Func&&, std::string)
   :project: dftracer-utils

Migration from Old API
----------------------

The task system has been significantly redesigned:

- **TaskScope replaced by CoroScope:** Lightweight coroutine scoping with JoinHandle
- **Old Pipeline API removed:** Use coroutine + channel patterns instead
- **Task/Scheduler unified execution:** Tasks dispatch to CoroScope internally
- **SpawnFuture for all spawns:** ``spawn()`` always returns ``SpawnFuture<T>`` (including ``SpawnFuture<void>``), enabling ``co_await`` on any spawn
- **Channel expanded:** Bounded capacity, async send/receive, producer tracking

Breaking changes:

- Old when_all() pattern no longer exists; use scope.spawn() for implicit synchronization
- Tasks no longer support complex continuation chains; use channels for communication
- Type safety is stricter: edge creation validates input/output types
