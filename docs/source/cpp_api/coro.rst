Coroutine API
=============

C++20 coroutine primitives for asynchronous task execution. All classes are in the ``dftracer::utils::coro`` namespace.

For usage examples and task scheduling, see :doc:`/pipeline` and :doc:`pipeline/tasks`.

Coro
----

Lightweight fire-and-forget coroutine type with no return value.

Coro is the internal execution primitive used by the runtime. Unlike CoroTask<T>, Coro:

- Has no return value (communicate through channels)
- Has no continuation chain (flat scheduling)
- Integrates with JoinHandle for structured concurrency
- Automatically manages its own lifetime via release() semantics

Users typically interact with Task and CoroScope instead of using Coro directly.

.. doxygenclass:: dftracer::utils::coro::Coro
   :project: dftracer-utils
   :members:
   :undoc-members:

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

.. doxygenclass:: dftracer::utils::coro::JoinHandle
   :project: dftracer-utils
   :members:
   :undoc-members:

SpawnFuture
-----------

Typed future returned by CoroScope::spawn() for retrieving results from spawned coroutines.

SpawnFuture<T> is awaitable and suspends the caller until the spawned coroutine completes,
then returns the typed result. It uses a lock-free shared state (SharedState<T>) with one
heap allocation per spawn.

**Awaitable interface:**

- ``await_ready()`` - Returns true if result is already available
- ``await_suspend()`` - Registers the awaiter to be resumed on completion
- ``await_resume()`` - Returns the result or re-throws any exception
- ``is_done()`` - Check if the spawned coroutine has completed without blocking
- ``detach()`` - Prevent automatic resumption (used by when_any)

Usage example:

.. code-block:: cpp

   SpawnFuture<int> future = scope.spawn([](CoroScope& s) -> CoroTask<int> {
       co_return 42;
   });
   int result = co_await future;  // suspends until spawn completes

Void specialization (SpawnFuture<void>) is also provided for coroutines that don't return a value.

.. doxygenclass:: dftracer::utils::coro::SpawnFuture
   :project: dftracer-utils
   :members:
   :undoc-members:

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

.. doxygenstruct:: dftracer::utils::coro::YieldAwaitable
    :project: dftracer-utils
    :members:

.. doxygenfunction:: dftracer::utils::coro::yield
    :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::coro::maybe_yield
    :project: dftracer-utils

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

.. doxygenclass:: dftracer::utils::coro::CoroTask
    :project: dftracer-utils
    :members:
    :undoc-members:

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
    
    // Producer: send values until done
    CoroTask<void> producer = [](auto ch) -> CoroTask<void> {
        auto guard = ch->producer_guard();  // auto-releases on exit
        for (int i = 0; i < 10; ++i) {
            co_await ch->send(i);
        }
        // ~ProducerGuard auto-closes when exiting
    }(channel);
    
    // Consumer: receive until channel closes
    while (auto value = co_await channel->receive()) {
        std::cout << *value << "\n";  // value is std::optional<int>
    }

**Pre-registration pattern:**

.. code-block:: cpp

    auto channel = make_channel<Chunk>(0);
    
    // Pre-register N producers before spawning
    for (std::size_t i = 0; i < 4; ++i)
        channel->register_producer();
    
    // Spawn N tasks; each adopts a registration for RAII cleanup
    for (std::size_t i = 0; i < 4; ++i) {
        scope.spawn([ch = channel](CoroScope& s) -> CoroTask<void> {
            auto guard = ch->adopt_producer();  // no counter increment
            for (auto chunk : read_my_chunks(i)) {
                co_await ch->send(std::move(chunk));
            }
            // ~ProducerGuard releases the slot; channel closes when all exit
        });
    }

.. doxygenclass:: dftracer::utils::coro::Channel
    :project: dftracer-utils
    :members:
    :undoc-members:

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

.. doxygenclass:: dftracer::utils::coro::Generator
    :project: dftracer-utils
    :members:
    :undoc-members:

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

.. doxygenclass:: dftracer::utils::coro::AsyncGenerator
    :project: dftracer-utils
    :members:
    :undoc-members:

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

.. doxygenfunction:: dftracer::utils::coro::when_all
    :project: dftracer-utils

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

.. doxygenstruct:: dftracer::utils::coro::WhenAnyResult
    :project: dftracer-utils
    :members:

.. doxygenfunction:: dftracer::utils::coro::when_any
    :project: dftracer-utils

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

.. doxygenclass:: dftracer::utils::coro::TimeoutAwaitable
    :project: dftracer-utils
    :members:
    :undoc-members:

.. doxygenfunction:: dftracer::utils::coro::timeout
    :project: dftracer-utils
