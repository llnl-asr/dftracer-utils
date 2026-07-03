Task System
===========

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference </cpp_api/api/core>`.

Task-based DAG execution and coroutine scoping for structured concurrency.

.. mermaid:: ../../_generated/pipeline_tasks.mmd

CoroScope
---------

Lightweight structured concurrency scope using Coro + JoinHandle.

CoroScope is the primary context type passed to task lambdas. It provides:

- ``spawn()`` returning ``SpawnFuture<T>`` for all coroutines (void and typed). The return value can be ignored for fire-and-forget usage, or ``co_await``'d to wait for that specific coroutine.

.. note::

   Spawns are **structured, not detached**. Every spawned coroutine is tracked
   by the scope's ``JoinHandle`` and is joined when the scope exits, even when
   the returned ``SpawnFuture`` is discarded. "Fire-and-forget" means only that
   you skipped ``co_await`` on the result - the work is still owned and joined
   by the scope, never leaked. Each spawn runs in its own child scope that
   shares the parent's cancellation token, so it can safely outlive a
   ``when_any``-style early completion.

CoroScope also provides:

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

make_task
---------

Factory that wraps a callable in a ``std::shared_ptr<Task>``:

.. code-block:: cpp

    template <typename Func>
    std::shared_ptr<Task> make_task(
        Func&& func, std::string_view name = "",
        std::source_location loc = std::source_location::current());

The captured function may take ``(CoroScope&)``, ``(CoroScope&, const Input&)``,
or ``(CoroScope&, const std::any&)`` and must return a ``coro::CoroTask<Output>``
(``Output`` may be ``void``). Input and output types are deduced and validated
when edges are created. If ``name`` is empty, the task reports the caller's
function name from ``source_location``.

.. code-block:: cpp

    auto producer = make_task([](CoroScope& ctx) -> coro::CoroTask<int> {
        co_return 42;
    }, "Producer");

Task also carries a few fluent setters beyond ``depends_on`` / ``with_combiner``:

- ``with_name(name)`` - override the display name
- ``with_input(value)`` - provide a root input without a parent edge
- ``with_timeout(ms)`` - per-task timeout consumed by the Watchdog
- ``get<T>()`` / ``wait(timeout)`` / ``when_ready()`` - retrieve results

Composition helpers
-------------------

Tasks compose into chains without manually calling ``depends_on``.

``then()`` creates a downstream task that consumes this task's output::

    auto task2 = task1->then(
        [](CoroScope& ctx, int x) -> coro::CoroTask<std::string> {
            co_return std::to_string(x * 2);
        }, "Stringify");

``tap()`` inserts a pass-through side effect (logging, metrics). It returns a
task that produces the *same* value it received::

    auto logged = task1->tap(
        [](CoroScope& ctx, int x) -> coro::CoroTask<void> {
            std::cout << "value=" << x << "\n";
            co_return;
        }, "Log");

Operator sugar mirrors these:

- ``a > f`` / ``f < a`` - forward/reverse composition (same as ``a->then(f)``)
- ``a & b`` - parallel AND; result task waits for both and yields a tuple of
  their outputs
- ``a ^ tap`` - tee ``a``'s output into ``tap`` as a side effect, continuing
  with ``a``'s output type

.. code-block:: cpp

    auto pipeline = task1
        ->tap(log_fn, "log")
        ->then(double_fn, "double");

    auto combined = task_a & task_b;   // tuple<int, std::string>

TypedTask
---------

``TypedTask<I, O>`` is a class-based alternative to lambda tasks with
compile-time input/output types. Subclass it and override ``apply``:

.. code-block:: cpp

    class Doubler : public TypedTask<int, std::string> {
    public:
        std::string apply(CoroScope& ctx, const int& input) {
            return "Result: " + std::to_string(input * 2);
        }
    };

    auto task = std::make_shared<Doubler>();

``apply`` has four forms selected by whether ``I`` / ``O`` are ``void``:
``O apply(CoroScope&, const I&)``, ``void apply(CoroScope&, const I&)``,
``O apply(CoroScope&)``, and ``void apply(CoroScope&)``. For most cases prefer
``make_task`` with a lambda; use ``TypedTask`` when you want an explicit,
reusable, strongly typed node.

TaskHandle / TypedTaskHandle
----------------------------

Lightweight, non-blocking handles returned by ``Runtime::submit()`` (and
``Runtime::scope()``). They wrap a ``std::shared_future`` plus the task id and
name:

- ``TaskHandle`` - for ``void`` tasks; ``wait()``/``get()`` block and re-raise
  stored exceptions, ``done()`` polls without blocking
- ``TypedTaskHandle<T>`` - adds ``T get()`` to retrieve the value; ``wait()``
  blocks and re-raises but discards the value

.. code-block:: cpp

    TypedTaskHandle<int> h = runtime.submit(some_task(), "compute");
    if (!h.done()) { /* still running */ }
    int value = h.get();   // blocks until ready, re-raises on error

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
