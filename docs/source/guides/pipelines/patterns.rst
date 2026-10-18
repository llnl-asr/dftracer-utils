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

Every task runs inside a ``CoroScope``. ``Runtime`` (in
``dftracer/utils/runtime/runtime.h``) has four entry points; pick the one that
matches how you need to wait:

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Entry point
     - Use when
   * - ``rt.submit(task)`` -> ``TaskHandle``
     - You already have a ``coro::CoroTask<void>`` and just want to fire it
       and get a handle back.
   * - ``rt.scope(name, func)`` -> ``TaskHandle``
     - Non-blocking: start a ``CoroScope``-taking task, keep going, and
       ``.wait()`` (or ``.get()`` for the typed overload) on the handle later.
   * - ``rt.run_blocking(name, func)``
     - Blocking: start a ``CoroScope``-taking task and block the caller until
       the scope finishes. The common case for a synchronous entry point like
       ``main()``.
   * - ``rt.parallel_for(n, grain, body)`` / ``rt.parallel_reduce(n, grain, identity, map, combine)``
     - A chunked data-parallel range over ``[0, n)``; both block until every
       chunk is done, no manual scope needed.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/runtime/runtime.h>
         using namespace dftracer::utils;

         Runtime rt;                          // threads = hardware concurrency

         // Blocks the caller until the scope is done.
         rt.run_blocking("job", [](CoroScope& scope) -> coro::CoroTask<void> {
             int n = co_await do_work(scope);
             use(n);
         });

         // Non-blocking: fire it, keep going, join later.
         auto handle = rt.scope("job2", [](CoroScope& scope) -> coro::CoroTask<void> {
             co_await do_other_work(scope);
         });
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

Spawn many children, then join them. ``scope.spawn`` starts a child immediately
and returns a ``SpawnFuture`` you can ``co_await``; ``when_all`` awaits a whole
batch and returns their results in order.

.. code-block:: cpp

   coro::CoroTask<std::int64_t> total(CoroScope& scope,
                                      const std::vector<std::string>& files) {
       std::vector<coro::CoroTask<std::int64_t>> tasks;
       for (const auto& f : files)
           tasks.push_back(count_lines(scope, f));   // one child per file
       auto counts = co_await coro::when_all(std::move(tasks));
       std::int64_t sum = 0;
       for (auto c : counts) sum += c;
       co_return sum;
   }

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

   auto r = co_await coro::when_any({read(cache_fd, buf, len),
                                     read(disk_fd, buf, len)});
   switch (r.index) { case 0: /* cache */ break; case 1: /* disk */ break; }
   process(r.result);

Stream with channels
--------------------

A ``Channel<T>`` moves values between tasks without materializing everything.
Producers hold a ``producer()`` slot (the channel closes when the last one
exits); a ``consumer()`` reads until close.

.. code-block:: cpp

   auto ch = coro::make_channel<Batch>(/*capacity=*/1000);

   auto produce = [pg = ch->producer()](CoroScope&) mutable -> coro::CoroTask<void> {
       auto guard = pg.guard();                       // RAII: releases on exit
       for (auto& b : read_batches()) co_await pg.send(std::move(b));
   };
   auto consume = [c = ch->consumer()](CoroScope&) mutable -> coro::CoroTask<void> {
       while (auto b = co_await c.receive()) write(*b);   // nullopt when closed
   };

   rt.run_blocking("stream", [&](CoroScope& scope) -> coro::CoroTask<void> {
       co_await coro::when_all(scope.spawn(produce), scope.spawn(consume));
   });

See :doc:`../../cpp_api/coro` for the full channel, ``when_all``/``when_any``,
and combinator API, and :doc:`../../pipeline` for the ``Pipeline`` executor that
runs a declarative task graph.
