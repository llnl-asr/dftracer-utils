Core Common Utilities
=====================

Thread-safe data structures and services used throughout the runtime.
All classes are in the ``dftracer::utils`` namespace.

.. mermaid:: ../_generated/pipeline_executor.mmd

ShardedMutex
------------

Generic sharded mutex for reducing lock contention on concurrent data structures.

``ShardedMutex<T, NUM_SHARDS>`` distributes data across ``NUM_SHARDS`` independent
shards, each protected by its own mutex. Shard selection uses bit masking (O(1))
on the provided key, so ``NUM_SHARDS`` must be a power of 2.

This is used internally by the Scheduler for task completion callbacks and by
other components that need concurrent map-like access without a single global lock.

**Template parameters:**

- ``T`` — Data type stored per shard (e.g., ``std::unordered_map<K, V>``)
- ``NUM_SHARDS`` — Number of shards (default 64, must be power of 2)

Usage example:

.. code-block:: cpp

    // Sharded map with 64 shards (default)
    ShardedMutex<std::unordered_map<int, std::string>> sharded_map;

    // Exclusive access to one shard
    sharded_map.with_shard(key, [&](auto& map) {
        map[key] = "value";
    });

    // Non-blocking try
    bool acquired = sharded_map.try_with_shard(key, [&](auto& map) {
        map[key] = "value";
    });

    // Iterate all shards (acquires each lock in sequence)
    sharded_map.for_each_shard([](auto& map) {
        for (auto& [k, v] : map) {
            process(k, v);
        }
    });

    // Aggregate operations (if T supports .size(), .empty(), .clear())
    size_t total = sharded_map.size();   // sum of all shard sizes
    bool empty = sharded_map.empty();    // true if all shards empty
    sharded_map.clear();                 // clear all shards

.. doxygenclass:: dftracer::utils::ShardedMutex
   :project: dftracer-utils
   :members:
   :undoc-members:

TimerService
------------

Async timeout scheduler for deadline-based operations.

TimerService runs a dedicated thread that processes timer registrations and
fires callbacks when timeouts expire. It is used by the ``TimeoutAwaitable``
(see :doc:`coro`) and by the Watchdog for periodic checks.

**Thread safety:** ``register_timeout()`` and ``cancel_timeout()`` are thread-safe
and can be called from any thread or coroutine.

Usage example:

.. code-block:: cpp

    TimerService timer_service;
    timer_service.start();

    // Register a timeout
    auto id = timer_service.register_timeout(
        std::chrono::seconds(5),
        []() { std::cerr << "Timeout fired!\n"; }
    );

    // Cancel before it fires
    timer_service.cancel_timeout(id);

    timer_service.stop();

.. doxygenclass:: dftracer::utils::TimerService
   :project: dftracer-utils
   :members:
   :undoc-members:

CoroPromise
-----------

Coroutine promise type for the fire-and-forget ``Coro`` type.

CoroPromise manages the lifecycle of a ``Coro`` coroutine:

- Captures unhandled exceptions
- Integrates with ``JoinHandle`` via atomic counter/continuation
- References the current ``Executor`` for scheduling
- Uses symmetric transfer in ``FinalAwaiter`` for efficient resumption

Users typically do not interact with CoroPromise directly. It is the
``promise_type`` for ``Coro`` and is managed by the coroutine machinery.

.. doxygenstruct:: dftracer::utils::coro::CoroPromise
   :project: dftracer-utils
   :members:
   :undoc-members:

Type Definitions
----------------

Core type aliases used throughout the runtime.

.. doxygentypedef:: dftracer::utils::TaskIndex
   :project: dftracer-utils
