:description: Coroutine concurrency patterns on the runtime: fan-out/fan-in, racing tasks, and streaming with channels, and which run scope to pick.

Concurrency patterns
====================

The runtime is built on C++20 coroutines: a task is a ``CoroTask<T>`` that
suspends on I/O instead of blocking a thread. This guide shows the patterns you
reach for - fan-out/fan-in, racing, and streaming with channels. Read
:doc:`../../concepts/coroutine-caveats` first; the rules there are what keep
these patterns safe.

Run work in a scope
-------------------

Spawned tasks run inside a ``coro::Scope``, which joins every child before it
ends. ``Runtime`` (in ``dftracer/utils/runtime/runtime.h``) has three ways to
run work; pick the one that matches how you need to wait:

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Entry point
     - Use when
   * - ``rt.submit(task)`` -> ``TaskHandle``
     - Non-blocking: start a ``coro::CoroTask`` (a ``coro::with_scope(fn)``
       task gives it a ``Scope``), keep going, and ``.wait()`` (or ``.get()``
       for the typed overload) on the handle later.
   * - ``rt.block_on(task)``
     - Blocking: run a ``coro::CoroTask`` and block the caller until it ends,
       returning its result. The common case for a synchronous entry point
       like ``main()``.
   * - ``rt.parallel_for(n, grain, body)`` / ``rt.parallel_reduce(n, grain, identity, map, combine)``
     - A chunked data-parallel range over ``[0, n)``; both block until every
       chunk is done, no manual scope needed.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/coro/scope.h>
         #include <dftracer/utils/runtime/runtime.h>
         using namespace dftracer::utils;

         Runtime rt;                          // threads = hardware concurrency

         // Blocks the caller until the scope is done.
         int n = rt.block_on(coro::with_scope(
             [](coro::Scope& scope) -> coro::CoroTask<int> {
                 int n = co_await do_work(scope);
                 co_return n;
             }));
         use(n);

         // Non-blocking: fire it, keep going, join later.
         auto handle = rt.submit(coro::with_scope(
             [](coro::Scope& scope) -> coro::CoroTask<void> {
                 co_await do_other_work(scope);
             }));
         handle.wait();

         // Chunked fan-out over a range, no manual scope.
         rt.parallel_for(files.size(), /*grain=*/8,
                          [&](std::int64_t begin, std::int64_t end) {
                              for (auto i = begin; i < end; ++i) process(files[i]);
                          });

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import Runtime

         rt = Runtime()
         handle = rt.submit(lambda x: x * 2, 21)   # a Python callable becomes a task
         result = handle.get()                     # 42 (blocks)

Fan-out / fan-in
----------------

Start many children, then join them. ``when_all`` awaits a whole batch and
returns their results in order.

.. code-block:: cpp

   coro::CoroTask<std::int64_t> total(const std::vector<std::string>& files) {
       dstd::Vector<coro::CoroTask<std::int64_t>> tasks;
       for (const auto& f : files)
           tasks.push_back(count_lines(f));          // one child per file
       auto counts = co_await coro::when_all(std::move(tasks));
       std::int64_t sum = 0;
       for (auto c : counts) sum += c;
       co_return sum;
   }

To start a child inside a scope and await it later, ``scope.spawn_future(task)``
returns a ``SpawnFuture`` you can ``co_await``; ``scope.spawn(fn)`` starts a
``void`` child that the scope joins.

For the common numeric fan-out there is a shortcut: ``Runtime::parallel_for``
and ``parallel_reduce`` run a chunked range across the pool and block until it
finishes, no manual scope needed.

.. note::

   Pass shared read-only state to children as a pointer into the parent's frame
   (the parent outlives the ``when_all``), not by value or dangling reference -
   see :doc:`../../concepts/coroutine-caveats`.

Race: first to finish
---------------------

``when_any`` resolves as soon as one operation completes, and reports which.

.. code-block:: cpp

   auto r = co_await coro::when_any(read_cache(), read_disk());
   switch (r.index) { case 0: /* cache */ break; case 1: /* disk */ break; }
   use(r.result);

Stream with channels
--------------------

A ``Channel<T>`` moves values between tasks without materializing everything.
Producers hold a ``producer()`` slot (the channel closes when the last one
exits); a ``consumer()`` reads until close.

.. code-block:: cpp

   auto ch = coro::make_channel<Batch>(/*capacity=*/1000);

   auto produce = [pg = ch->producer()]() mutable -> coro::CoroTask<void> {
       auto guard = pg.guard();                       // RAII: releases on exit
       for (auto& b : read_batches()) co_await pg.send(std::move(b));
   };
   auto consume = [c = ch->consumer()]() mutable -> coro::CoroTask<void> {
       while (auto b = co_await c.receive()) write(*b);   // nullopt when closed
   };

   rt.block_on(coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
       scope.spawn(std::move(produce));
       scope.spawn(std::move(consume));
       co_return;
   }));

See :doc:`../../cpp_api/coro` for the full channel, ``when_all``/``when_any``,
and combinator API.

Error handling
--------------

An exception thrown in a task propagates to whoever awaits it: ``Runtime::block_on``
rethrows it, and ``handle.get()`` rethrows it for a submitted task. The library
throws ``common::DFTUtilsException`` and its subclasses, which derive from
``std::runtime_error``; catch ``std::exception`` to handle any failure.

.. code-block:: cpp

   auto task = []() -> coro::CoroTask<void> {
       // Any exception thrown here propagates to the caller of block_on
       if (risky_operation() < 0) {
           throw common::DFTUtilsException(common::ErrorCode::Io,
                                           "Failed to open file");
       }
       co_return;
   };
   try {
       rt.block_on(task());
   } catch (const std::exception& e) {
       std::cerr << "Task failed: " << e.what() << std::endl;
   }

The error of a ``scope.spawn_future`` child goes only to its ``SpawnFuture``,
so wrap the ``co_await`` in a try-catch:

.. code-block:: cpp

   coro::CoroTask<void> spawned(coro::Scope& scope) {
       auto future = scope.spawn_future([]() -> coro::CoroTask<int> {
           int result = risky_operation();   // this might throw
           if (result < 0) {
               throw common::DFTUtilsException(common::ErrorCode::Internal,
                                               "Operation failed");
           }
           co_return result;
       });

       try {
           int result = co_await future;
           use_data(result);
       } catch (const std::exception& e) {
           std::cerr << "Spawned task failed: " << e.what() << std::endl;
       }
   }

On a channel, check the values of ``send()`` and ``receive()``. Both are falsy
on a closed channel, which means the other side exited or failed:

.. code-block:: cpp

   auto channel = coro::make_channel<std::string>(100);

   auto produce = [ch = channel->producer(),
                   items = std::move(items)]() mutable -> coro::CoroTask<void> {
       auto guard = ch.guard();
       for (auto& item : items) {
           if (!co_await ch.send(std::move(item))) {
               // Channel closed (consumer exited or error)
               co_return;
           }
       }
   };

   auto consume = [channel]() -> coro::CoroTask<void> {
       while (auto item = co_await channel->receive()) {
           process(*item);
       }
       // Loop exits when channel closes (all producers exited)
   };

Timeouts and cancellation
-------------------------

A task cancels cooperatively: it checks ``scope.stop_requested()`` (true after
``request_stop()``, a child error, or executor shutdown) and yields so that a
stop can take effect.

.. code-block:: cpp

   coro::CoroTask<void> cancellable(
       coro::Scope& scope, dstd::SharedPtr<coro::Channel<std::string>> channel) {
       // Long-running loop that respects cancellation
       while (!scope.stop_requested()) {
           auto item = co_await channel->receive();
           if (!item) break;  // Channel closed

           process(*item);

           // Yield frequently to allow cancellation checks
           co_await coro::maybe_yield();
       }
   }

To put a time limit on an operation, race it against ``coro::timeout`` with
``when_any``. The losers are cancelled:

.. code-block:: cpp

   coro::CoroTask<void> raced() {
       // Race the cache against the disk, with a 100ms timeout
       auto* timers = coro::Executor::current()->timers();
       auto result = co_await coro::when_any(read_from_cache(), read_from_disk(),
                                             coro::timeout(100ms, timers));
       if (result.index == 2) {
           std::cerr << "Read timeout" << std::endl;
       } else if (result.index == 0) {
           use_data(result.get<0>());
       } else {
           use_data(result.get<1>());
       }
   }

See :doc:`../../cpp_api/coro` for ``timeout`` and the cancellation details.

Bound the time and the fan-out of coroutines
--------------------------------------------

These combinators take ``coro::CoroTask`` values and run on the current executor.

Limit how long a task runs:

.. code-block:: cpp

   #include <dftracer/utils/coro/time.h>

   using namespace std::chrono_literals;

   co_await coro::sleep(10ms);
   int value = co_await coro::with_timeout(read_value(), 5s);

``with_timeout`` raises an error whose condition is ``dstd::Condition::Timeout``.
It wakes the task if the task is parked on a channel, an event, a semaphore, a mutex, a sleep or a spawned future.
A task that computes without a wait, or that waits on I/O, ends only when it reaches its next wait.

Retry a failing task:

.. code-block:: cpp

   #include <dftracer/utils/coro/retry.h>

   coro::RetryPolicy policy;
   policy.max_attempts = 5;
   policy.initial_backoff = 50ms;
   policy.jitter = 0.2;
   auto data = co_await coro::retry([&]() { return fetch(path); }, std::move(policy));

``retry`` takes a function, since a task runs once.
It never retries a cancelled error.

Run many jobs with a limit:

.. code-block:: cpp

   #include <dftracer/utils/coro/combinators.h>

   coro::ParallelOptions options;
   options.max_in_flight = 8;
   auto sizes = co_await coro::parallel_map(std::move(paths), measure, options);

``parallel_map`` gives results in item order.
The first error ends running jobs as cancelled at their next wait and is then raised.
``parallel_map_unordered`` returns an ``AsyncGenerator`` that yields each result as it finishes.

Use ``when_all_settled`` to get each outcome as a ``dstd::Expected``, and ``first_success`` to take the first task that does not fail and cancel the others.

Stop a tree of tasks
--------------------

A ``coro::Scope`` stops its children when one fails, when ``request_stop`` runs or when the task that holds it is cancelled.
A stopped child ends as cancelled at its next wait, and a parked child wakes at once.
``with_scope`` links the scope to its task, so nested scopes stop together.

.. code-block:: cpp

   co_await coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
       scope.spawn(read_all());
       scope.spawn(write_all());
       co_return;
   });

Pass ``coro::ScopeMode::Supervisor`` to ``with_scope`` when a failure must not stop the siblings.
``join`` then waits for all of them and raises the first error.

Use a ``coro::StopSource`` to stop work from code that holds no scope:

.. code-block:: cpp

   coro::StopSource source;
   co_await coro::with_stop_token(run_server(), source.token());
   // elsewhere: source.request_stop();

A task that computes without waits reads ``co_await coro::this_coro::stop_requested``.

Shape a stream
--------------

``coro/stream.h`` holds every operation on ``coro::AsyncGenerator``.
Each one is a function and an adaptor, so ``filter(gen, pred)`` and ``gen | filter(pred)`` do the same.

- ``map``, ``flat_map``, ``filter``, ``take``, ``take_until``, ``concat``, ``chunks``, ``zip`` and ``fold`` read one item at a time.
  A function that they call may return a ``CoroTask``, which they await.
- ``merge`` reads many generators at once and yields items as they come.
- ``flat_map_merge`` does the same for the generators a function returns, with a limit on how many run.
- ``buffer`` reads ahead of the reader by a bounded number of items.
- ``with_item_timeout`` bounds the wait for each item.

A sync ``coro::Generator`` has the same ``map``, ``flat_map``, ``filter``, ``take``, ``take_until``, ``concat``, ``chunks``, ``zip`` and ``fold``, and the same ``next``.
Its functions give plain values, because it cannot wait, so ``merge``, ``flat_map_merge``, ``buffer`` and ``with_item_timeout`` are only on ``AsyncGenerator``.

A stop or a cancel of the task that reads a generator also ends a wait inside the generator.
The read then raises ``dstd::Condition::Cancelled``.

``merge``, ``flat_map_merge`` and ``buffer`` run their sources in the background, so a source must own what it uses.
Each of them keeps the order of one source, and raises the first error from the next read.
Dropping the stream ends those sources at their current wait.

Run a range or a set of tasks
-----------------------------

``coro/parallel.h`` has ``parallel_for``, ``parallel_reduce`` and ``parallel_scan``, which wait without holding a thread.
``Partition::Dynamic`` balances uneven work, and ``Partition::Static`` gives the same chunks on each run with the same ``max_in_flight``.

``coro/join_set.h`` has ``with_join_set`` for tasks that are added as work comes, and ``as_completed`` for a known list.
Both give outcomes as they finish, with errors as values.
``JoinSet::next_within`` bounds a wait, and a result that comes at that moment stays for the next read.

``stage | coro::ordered(n)`` and ``stage | coro::unordered(n)`` turn a batch stage into a stream stage that runs ``n`` items at once.
The ordered form holds its output in input order, and ``n`` also bounds the results that wait for a slow item.
