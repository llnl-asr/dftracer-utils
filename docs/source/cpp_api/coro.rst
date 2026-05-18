Coroutine API
=============

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/coro>`.


C++20 coroutine primitives for asynchronous task execution. All classes are in the ``dftracer::utils::coro`` namespace.

For usage examples and task scheduling, see :doc:`/pipeline` and :doc:`pipeline/tasks`.

.. note::

   GCC 12 may corrupt large coroutine frames at ``-O2`` and above, especially
   when frames contain references, ``string_view``, or captured lambdas. The
   project mitigates this by heap-allocating per-task state in a
   ``shared_ptr`` (or ``unique_ptr``) and capturing only the smart pointer in
   coroutine lambdas, instead of capturing complex state by value. New
   coroutines should follow the same pattern; see ``coroutine-caveats.md`` at
   the repo root for the full discussion.

.. mermaid::

   graph TD
       Coro["Coro\nfire-and-forget primitive"]
       CoroTask["CoroTask&lt;T&gt;\nawaitable task result"]
       SpawnFuture["SpawnFuture&lt;T&gt;\nresult of CoroScope::spawn()"]
       JoinHandle["JoinHandle\nstructured join barrier"]
       Channel["Channel&lt;T&gt;\nasync producer/consumer queue"]
       Producer["ChannelProducer / ProducerGuard\nproducer lifetime management"]
       Generator["Generator&lt;T&gt;\nsynchronous lazy sequence"]
       AsyncGenerator["AsyncGenerator&lt;T&gt;\nasynchronous lazy sequence"]
       Yield["yield() / maybe_yield()\ncooperative scheduling"]

       CoroTask --> Coro
       SpawnFuture --> CoroTask
       JoinHandle --> Coro
       Channel --> Producer
       CoroTask --> Channel
       Coro --> Yield
       CoroTask --> Yield
       AsyncGenerator --> CoroTask
       Generator --> Coro

Coro
----

Lightweight fire-and-forget coroutine type with no return value.

Coro is the internal execution primitive used by the runtime. Unlike CoroTask<T>, Coro:

- Has no return value (communicate through channels)
- Has no continuation chain (flat scheduling)
- Integrates with JoinHandle for structured concurrency
- Automatically manages its own lifetime via release() semantics

Users typically interact with Task and CoroScope instead of using Coro directly.

JoinHandle
----------

Stack-allocated join barrier for coordinating multiple Coro instances.

Uses a thread-safe counter pattern to synchronize completion of a group of coroutines.
When all tracked coroutines complete, the awaiter is resumed via symmetric transfer.

**Stack-bound lifetime:** JoinHandle is non-copyable, non-movable, and must outlive all tracked coroutines.

Usage example:

.. code-block:: cpp

   JoinHandle jh;
   jh.track(coro1);
   jh.track(coro2);
   // ... enqueue coroutines to executor ...
   co_await jh.join();  // suspends until all tracked coroutines complete

SpawnFuture
-----------

Future returned by ``CoroScope::spawn()`` for all coroutines (both void and typed).

``spawn()`` always returns ``SpawnFuture<T>`` (where ``T`` is ``void`` for void coroutines).
The future is awaitable and suspends the caller until the spawned coroutine completes,
then returns the typed result. It uses a lock-free shared state (``SharedState<T>``) with one
heap allocation per spawn.

For fire-and-forget usage, the return value can simply be discarded.

**Awaitable interface:**

- ``await_ready()`` - Returns true if result is already available
- ``await_suspend()`` - Registers the awaiter to be resumed on completion
- ``await_resume()`` - Returns the result or re-throws any exception
- ``is_done()`` - Check if the spawned coroutine has completed without blocking
- ``detach()`` - Prevent automatic resumption (used by when_any)

Usage examples:

.. code-block:: cpp

   // Await a typed spawn directly:
   int result = co_await scope.spawn([](CoroScope& s) -> CoroTask<int> {
       co_return 42;
   });

   // Or capture the future for later:
   SpawnFuture<int> future = scope.spawn([](CoroScope& s) -> CoroTask<int> {
       co_return 42;
   });
   int result = co_await future;

   // Await a void spawn:
   co_await scope.spawn([](CoroScope& s) -> CoroTask<void> {
       // caller suspends until this completes
       co_return;
   });

   // Fire-and-forget (discard the SpawnFuture):
   scope.spawn([](CoroScope& s) -> CoroTask<void> {
       co_return;
   });

Yield Primitives
----------------

Control coroutine scheduling and timeslice behavior.

``yield()`` unconditionally suspends and re-enqueues the coroutine on the executor.

``maybe_yield()`` conditionally yields only if the current thread's timeslice has been exceeded.
This is a low-cost operation (~25ns clock read) when the timeslice is not exceeded.

**Timeslice management:**

- ``reset_timeslice()`` - Reset the current thread's timeslice clock to now
- ``timeslice_exceeded()`` - Check whether the current thread has exceeded its timeslice
- ``set_timeslice_duration()`` - Set the timeslice duration for the current thread
- ``get_timeslice_duration()`` - Get the timeslice duration for the current thread
- ``DEFAULT_TIMESLICE`` - Default timeslice duration (10ms)

Usage example:

.. code-block:: cpp

    for (auto& item : large_dataset) {
        process(item);
        co_await maybe_yield();  // Yield only if timeslice exceeded
    }

    // Custom timeslice configuration
    set_timeslice_duration(std::chrono::milliseconds(5));
    
    for (auto& chunk : process_chunks()) {
        process_chunk(chunk);
        co_await maybe_yield();  // Yields after 5ms instead of 10ms
    }

CoroTask
--------

User-facing coroutine task type with awaitable interface and combinators.

CoroTask<T> supports:

- Awaiting to get the result of type T
- Composing with channel operations
- Exception propagation
- Integration with the task scheduler

Usage example:

.. code-block:: cpp

    CoroTask<int> compute_value() {
        co_return 42;
    }

    CoroTask<void> use_value() {
        int result = co_await compute_value();
        // result == 42
        co_return;
    }

Channel
-------

Thread-safe producer-consumer queue for streaming data between tasks.

Channel<T> supports bounded capacity, async send/receive, and producer tracking.
Multiple producers can register themselves, and the channel automatically closes
when the last producer exits (via ProducerGuard RAII).

**Features:**

- Bounded or unbounded capacity
- Async send() and receive() with awaitable interface
- ProducerGuard RAII for automatic close on producer exit
- Producer pre-registration for CoroScope spawn patterns
- Thread-safe waiters queue for back-pressure coordination

Usage example:

.. code-block:: cpp

    auto channel = make_channel<int>(100);

    // Producer: channel->producer() increments the producer count
    // immediately, then .guard() adopts the slot for RAII cleanup.
    auto task = make_task(
        [ch = channel->producer()](CoroScope& scope) mutable
            -> CoroTask<void> {
            auto guard = ch.guard();
            for (int i = 0; i < 10; ++i) {
                co_await ch.send(i);
            }
            // ~ProducerGuard auto-releases; channel closes when last exits
            co_return;
        }, "Producer");

    // Consumer: receive until channel closes
    while (auto value = co_await channel->receive()) {
        std::cout << *value << "\n";  // value is std::optional<int>
    }

**Multiple producers pattern:**

.. code-block:: cpp

    auto channel = make_channel<Chunk>(0);

    // channel->producer() increments the count eagerly on the caller's
    // thread, so the channel never transiently sees zero producers
    // while coroutines are still being scheduled.
    for (std::size_t i = 0; i < 4; ++i) {
        scope.spawn([ch = channel->producer(),
                     i](CoroScope& s) mutable -> CoroTask<void> {
            auto guard = ch.guard();
            for (auto chunk : read_my_chunks(i)) {
                co_await ch.send(std::move(chunk));
            }
            // ~ProducerGuard releases the slot; channel closes when all exit
        });
    }

Generator
---------

Synchronous lazy sequence generator using ``co_yield``.

Usage example:

.. code-block:: cpp

    Generator<int> fibonacci(int n) {
        int a = 0, b = 1;
        for (int i = 0; i < n; ++i) {
            co_yield a;
            auto next = a + b;
            a = b;
            b = next;
        }
    }

    // Lazy iteration - only computes values as needed
    for (int fib : fibonacci(10)) {
        std::cout << fib << " ";  // 0 1 1 2 3 5 8 13 21 34
    }

AsyncGenerator
--------------

Asynchronous lazy sequence generator for async iteration.

Allows coroutines to produce a sequence of values asynchronously.
Use ``co_await gen.next()`` to await the next value.

Usage example:

.. code-block:: cpp

    AsyncGenerator<std::string> read_lines(const std::string& path) {
        auto fd = co_await io::async_open(path.c_str(), O_RDONLY);
        std::string line;
        while (co_await io::async_readline(fd, line)) {
            co_yield line;
        }
    }

    // Async iteration - each line read asynchronously
    auto gen = read_lines("data.txt");
    while (auto line = co_await gen.next()) {
        process(*line);
    }

when_all
--------

Wait for all awaitables to complete.

Suspends until all provided awaitables have completed, then returns their results
as a tuple.

Usage example:

.. code-block:: cpp

    // Race multiple tasks and wait for all to complete
    auto [result_a, result_b, result_c] = co_await when_all({
        compute_async_a(),
        compute_async_b(),
        compute_async_c()
    });

    // Or with a vector of awaitables
    std::vector<CoroTask<int>> tasks;
    for (int i = 0; i < 10; ++i) {
        tasks.push_back(compute_async(i));
    }
    auto results = co_await when_all(std::move(tasks));
    // results is std::vector<int>

when_any
--------

Race multiple awaitables, return first to complete.

Suspends until at least one awaitable completes, then returns the index and result
of the first one.

Usage example:

.. code-block:: cpp

    // Race three I/O operations, use whichever completes first
    auto result = co_await when_any({
        io::async_read(cache_fd, buf, len),
        io::async_read(disk_fd, buf, len),
        io::async_read(network_fd, buf, len)
    });

    // result.index tells which completed first
    switch (result.index) {
        case 0:
            std::cout << "Cache hit\n";
            break;
        case 1:
            std::cout << "Local disk\n";
            break;
        case 2:
            std::cout << "Network fetch\n";
            break;
    }
    process(result.result);

Heterogeneous when_all and when_any
------------------------------------

The variadic overloads of ``when_all`` and ``when_any`` accept awaitables of
different types. Return types are deduced automatically: ``when_all`` returns a
``std::tuple`` of each awaitable's result type, and ``when_any`` returns a
``WhenAnyTupleResult`` with index-based ``get<N>()`` access to the winning
result. ``void`` results map to ``std::monostate`` in both cases.

**Heterogeneous when_all:**

.. code-block:: cpp

    // Wait for tasks returning different types
    auto f_int = scope.spawn([](CoroScope&) -> CoroTask<int> {
        co_return 42;
    });
    auto f_str = scope.spawn([](CoroScope&) -> CoroTask<std::string> {
        co_return std::string("hello");
    });

    // Returns std::tuple<int, std::string>
    auto [num, text] = co_await when_all(std::move(f_int), std::move(f_str));

**Void handling in when_all:**

.. code-block:: cpp

    // Void results map to std::monostate in the tuple
    auto f_void = scope.spawn([](CoroScope&) -> CoroTask<void> {
        co_return;
    });
    auto f_int = scope.spawn([](CoroScope&) -> CoroTask<int> {
        co_return 99;
    });

    // Returns std::tuple<std::monostate, int>
    auto [_, val] = co_await when_all(std::move(f_void), std::move(f_int));

**Heterogeneous when_any:**

.. code-block:: cpp

    // Race tasks returning different types
    auto f_int = scope.spawn([](CoroScope&) -> CoroTask<int> {
        co_return 42;
    });
    auto f_str = scope.spawn([](CoroScope&) -> CoroTask<std::string> {
        co_return std::string("hello");
    });

    // Returns WhenAnyTupleResult — use get<N>() for index-based access
    auto result = co_await when_any(std::move(f_int), std::move(f_str));

    // result.index tells which awaitable completed first (0-based).
    // get<N>() works correctly even when types repeat.
    if (result.index == 0) {
        int val = result.get<0>();
    } else {
        std::string val = result.get<1>();
    }
    result.cancel_remaining();

**Overload resolution:**

The correct overload is selected automatically via ``requires`` constraints:

- All arguments share the same type → homogeneous (vector-based) overload,
  returning ``std::vector<T>`` or ``WhenAnyResult<T>``.
- Arguments have different types → heterogeneous overload,
  returning ``std::tuple<...>`` or ``WhenAnyTupleResult<...>`` (with ``get<N>()`` access).

No explicit template arguments are needed; the compiler resolves the overload
based on the argument types.

AsyncMutex
----------

Lock-free async mutex for coroutines. Ownership is not tied to any thread —
a coroutine holding the lock can migrate freely. Waiting coroutines suspend
without blocking the OS thread and are resumed in approximate FIFO order.

Used by the reorganization pipeline for serializing writes to shared
``ChunkWriter`` instances across parallel event routing coroutines.

.. code-block:: cpp

    #include <dftracer/utils/core/coro/async_mutex.h>

    AsyncMutex mutex;

    // Manual lock/unlock
    co_await mutex.lock();
    co_await writer.write_line(data);
    mutex.unlock();

    // RAII scoped lock (recommended)
    {
        auto guard = co_await mutex.scoped_lock();
        co_await writer.write_line(data);
    }  // automatically unlocks

    // Non-blocking try_lock
    if (mutex.try_lock()) {
        // acquired
        mutex.unlock();
    }

TimeoutAwaitable
----------------

Timeout awaitable for use with ``when_any``.

Allows racing a task against a timeout to implement deadline-based cancellation.

Usage example:

.. code-block:: cpp

    using namespace std::chrono_literals;
    
    auto& timer_service = executor->get_timer_service();
    auto result = co_await when_any({
        slow_operation(),
        timeout(5s, &timer_service)
    });

    if (result.index == 1) {
        std::cerr << "Operation timed out\n";
    } else {
        process(result.result);
    }
