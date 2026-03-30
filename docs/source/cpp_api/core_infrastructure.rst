Core Infrastructure
===================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/core>`.


Core infrastructure: thread-safe data structures, services, and memory utilities used throughout the runtime. All classes are in the ``dftracer::utils`` namespace.

.. mermaid::

   graph LR
       subgraph Concurrency["Concurrency"]
           SM["ShardedMutex&lt;T&gt;<br/>sharded locking"]
       end

       subgraph Scheduling["Scheduling"]
           TS["TimerService<br/>timeout callbacks"]
       end

       subgraph Memory["Memory & Strings"]
           SI["StringIntern<br/>string dedup → uint32 IDs"]
           BP["BufferPool&lt;T&gt;<br/>zero-alloc reuse"]
       end

       SM --> |used by| Scheduler["Scheduler"]
       TS --> |used by| Watchdog["Watchdog"]
       SI --> |used by| AggKey["AggregationKey"]
       BP --> |used by| Pipeline["Pipeline stages"]

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

StringIntern
------------

Thread-safe string interning table for deduplicating strings into compact integer IDs.

Used by ``AggregationKey`` to store category, name, hhash, and fhash fields as
``uint32_t`` IDs instead of full strings, reducing memory usage and enabling
faster hashing.

.. code-block:: cpp

    #include <dftracer/utils/core/common/string_intern.h>

    StringIntern intern;

    // Intern strings — returns stable uint32_t IDs
    uint32_t id = intern.get_or_insert("POSIX");   // first call: stores string
    uint32_t id2 = intern.get_or_insert("POSIX");  // cache hit: no alloc
    assert(id == id2);

    // Resolve ID back to string_view
    assert(intern.resolve(id) == "POSIX");

    // Convenience: intern and return string_view in one call
    std::string_view sv = intern.intern("STDIO");

    // Thread safety: uses shared_mutex (concurrent reads, exclusive writes)
    std::size_t count = intern.size();

BufferPool
----------

Thread-safe typed buffer pool for zero-allocation buffer reuse after warmup.

Pre-allocates buffers on construction. ``acquire()`` returns a buffer from the
pool (or creates a new one if empty). ``release()`` returns a buffer to the pool
after applying a reset callable.

.. code-block:: cpp

    #include <dftracer/utils/core/common/buffer_pool.h>

    // Create a pool of 8 reusable string buffers
    auto pool = make_buffer_pool<std::string>(8,
        []() { std::string s; s.reserve(4096); return s; });

    // Acquire a buffer (O(1) from pool, no allocation)
    auto buf = pool->acquire();
    buf += "data";

    // Release back to pool (calls clear() by default)
    pool->release(std::move(buf));

    // Custom reset callable
    auto pool2 = make_buffer_pool<std::vector<int>>(4,
        []() { return std::vector<int>(); },           // init
        [](std::vector<int>& v) { v.clear(); });       // reset


