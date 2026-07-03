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
           SI["StringIntern<br/>string dedup -> uint32 IDs"]
           BP["BufferPool&lt;T&gt;<br/>zero-alloc reuse"]
           OP["ObjectPool<br/>type-stable freelist"]
           SA["StringArena<br/>bump arena"]
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

- ``T`` - Data type stored per shard (e.g., ``std::unordered_map<K, V>``)
- ``NUM_SHARDS`` - Number of shards (default 64, must be power of 2)

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

    // Intern strings - returns stable uint32_t IDs
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

ObjectPool
----------

Process-wide, lock-free allocator for type-stable object reuse.

``ObjectPool`` is a singleton (``ObjectPool::instance()``) that recycles raw
memory blocks by size class instead of returning them to the system allocator.
Each size class owns a **Treiber-stack freelist** (a lock-free LIFO using an
atomic compare-and-swap on the list head), so ``allocate()`` / ``deallocate()``
are wait-free-ish and contention-tolerant across threads. Small sizes (up to
4096 bytes) hit dense per-size "fast" buckets; larger sizes go through an
open-addressed "slow" table. Freed blocks are kept for reuse, giving zero
system-allocator traffic in steady state.

Blocks are handed out as raw storage; the caller is responsible for
constructing/destroying objects in place. This backs the hot per-object
allocation paths in the runtime rather than being used directly in most code.

**Key signatures:**

- ``static ObjectPool& instance()``
- ``void* allocate(std::size_t size)``
- ``void deallocate(void* ptr, std::size_t size)``

.. code-block:: cpp

    #include <dftracer/utils/core/common/object_pool.h>

    auto& pool = ObjectPool::instance();

    // Acquire raw storage for a T, construct in place, then reclaim.
    void* mem = pool.allocate(sizeof(T));
    T* obj = new (mem) T{...};
    // ... use obj ...
    obj->~T();
    pool.deallocate(mem, sizeof(T));   // returns the block to the freelist

ByteView / MutableByteView
--------------------------

Non-owning views over a contiguous byte range (pointer + size, 16 bytes).

``ByteView`` is read-only; ``MutableByteView`` allows writes and implicitly
converts to ``ByteView`` for reading. Both construct from ``std::byte*``,
``unsigned char*``, ``char*``, ``std::string_view`` / ``std::string``, and the
matching ``std::vector`` byte containers, so they unify the many buffer
representations that cross I/O and compression boundaries without copying. All
accessors are trivial ``reinterpret_cast`` operations inlined to zero overhead.

**Key signatures (ByteView):**

- ``const std::byte* data() const`` / ``std::size_t size() const`` / ``bool empty() const``
- ``template <typename T> const T* as() const``
- ``std::string_view as_string_view() const``
- ``ByteView subspan(std::size_t offset, std::size_t count) const`` (and single-arg overload)

``MutableByteView`` mirrors these with a mutable ``std::byte* data()`` and
``template <typename T> T* as()``, and adds ``operator ByteView()``.

.. code-block:: cpp

    #include <dftracer/utils/core/common/byte_view.h>

    std::string payload = "hello";
    ByteView view(payload);                     // from string_view
    std::string_view sv = view.as_string_view();
    ByteView tail = view.subspan(2);            // "llo"

    std::vector<unsigned char> buf(1024);
    MutableByteView out(buf);                   // writable
    std::memset(out.data(), 0, out.size());
    ByteView ro = out;                          // implicit read-only view

Env
---

Typed access to environment variables.

``Env`` (in ``core/env.h``) wraps ``getenv`` behind a typed, optional-returning
``get<T>()`` so call sites can read configuration without manual parsing.
Explicit specializations exist for ``std::string_view`` (the default) and
``int``; other types are a compile error. A named helper exposes a commonly
read setting.

**Key signatures:**

- ``template <typename T = std::string_view> static std::optional<T> get(std::string_view name)``
- ``static int rocksdb_max_open_files()``

.. code-block:: cpp

    #include <dftracer/utils/core/env.h>

    if (auto v = Env::get("DFTRACER_UTILS_LOG_LEVEL"))  // std::string_view
        set_level(*v);

    int workers = Env::get<int>("DFT_WORKERS").value_or(4);
    int max_files = Env::rocksdb_max_open_files();

ScopedFd
--------

RAII wrapper for a POSIX file descriptor.

``ScopedFd`` owns an ``int`` fd and ``::close()``s it on destruction. It is
move-only (moving transfers ownership and leaves the source as ``-1``); an empty
value of ``-1`` closes nothing. Use it to make fd lifetimes exception- and
early-return-safe.

**Key signatures:**

- ``explicit ScopedFd(int fd)`` / ``ScopedFd()`` (empty, ``value == -1``)
- ``int get() const`` (public member ``int value`` also available)
- ``void reset()`` (close and set to ``-1``)

.. code-block:: cpp

    #include <dftracer/utils/core/common/scoped_fd.h>

    ScopedFd fd(::open(path.c_str(), O_RDONLY));
    if (fd.get() < 0) return error(errno);
    ::read(fd.get(), buf, n);
    // closed automatically at scope exit

StringArena
-----------

Bump-allocation arena for string data that must outlive its source.

``StringArena`` copies bytes into 64 KiB blocks and returns a
``std::string_view`` into arena-owned storage, valid until the next
``clear()``. Use it to keep string_views alive across a later flush point (e.g.
until an Arrow ``builder.finish()``), avoiding per-string heap allocation. Not
thread-safe.

**Key signatures:**

- ``std::string_view push(const char* data, std::size_t len)``
- ``void clear()`` (drops extra blocks, resets to one empty block)

.. code-block:: cpp

    #include <dftracer/utils/core/common/string_arena.h>

    StringArena arena;
    std::string_view stable = arena.push(tmp.data(), tmp.size());
    // 'stable' stays valid even after 'tmp' is destroyed, until arena.clear()

ConstString
-----------

Compile-time string buffer for ``consteval`` string concatenation.

``ConstString<MaxLen>`` builds type signatures and display names entirely at
compile time; the result lives in ``.rodata`` with zero runtime allocation. It
constructs from a ``std::string_view``, supports ``consteval append()``, and
converts to ``std::string_view`` via ``view()`` or the implicit conversion.

**Key signatures:**

- ``consteval ConstString(std::string_view sv)``
- ``consteval ConstString& append(std::string_view sv)``
- ``constexpr std::string_view view() const`` / ``constexpr operator std::string_view() const``

.. code-block:: cpp

    #include <dftracer/utils/core/common/const_string.h>

    consteval auto make_name() {
        ConstString<64> s{"Utility<"};
        s.append("Input").append(">");
        return s;
    }
    constexpr std::string_view name = make_name().view();  // "Utility<Input>"

TransparentStringHash / TransparentStringEqual
----------------------------------------------

Heterogeneous hashing/equality for string-keyed maps.

These functors carry ``is_transparent`` (and ``is_avalanching`` for the hash),
letting an ``unordered_dense`` map keyed by ``std::string`` be looked up with a
``std::string_view`` or ``const char*`` without constructing a temporary
``std::string``. The header also provides ready-made aliases:

- ``StringViewMap<V>`` - map with owned ``std::string`` keys, transparent lookup.
- ``InternedStringViewMap<V>`` - map keyed by ``std::string_view`` (stores no key
  copies; every key MUST outlive the map, e.g. views into an interned pool).
- ``StringViewSet`` - set of ``std::string`` with transparent lookup.

.. code-block:: cpp

    #include <dftracer/utils/core/common/transparent_string_hash.h>

    StringViewMap<int> counts;
    counts["posix"] = 1;

    std::string_view key = "posix";
    auto it = counts.find(key);   // no std::string temporary constructed

PtrHash
-------

Avalanching hash for raw pointers.

``PtrHash`` runs a murmur3/splitmix64 finalizer over a pointer value so that the
poor entropy in aligned low bits is spread out - suitable for both
open-addressing maps and shard selection. Carries ``is_avalanching``.

**Key signature:**

- ``std::size_t operator()(const void* p) const noexcept``

.. code-block:: cpp

    #include <dftracer/utils/core/common/ptr_hash.h>

    ankerl::unordered_dense::map<Node*, State, PtrHash> by_node;
    std::size_t shard = PtrHash{}(ptr) & (num_shards - 1);

hash_combine
------------

Boost-style hash folding for building composite hashes.

``hash_combine`` mixes a value hash into a running ``seed`` using the 64-bit
golden ratio constant ``HASH_GOLDEN_RATIO``; ``hash_combine_value`` first runs
``std::hash<T>`` on the value. Use them to hash multi-field keys.

**Key signatures:**

- ``void hash_combine(std::size_t& seed, std::size_t value)``
- ``template <typename T> void hash_combine_value(std::size_t& seed, const T& value)``

.. code-block:: cpp

    #include <dftracer/utils/core/common/hash_combine.h>

    std::size_t seed = 0;
    hash_combine_value(seed, key.category);
    hash_combine_value(seed, key.name);   // seed now hashes both fields

little_endian codec
-------------------

Fixed-width little-endian read/write helpers.

Free functions that encode/decode a ``std::uint32_t`` to/from a little-endian
byte buffer, used on hot serialization paths (e.g. index records). Callers own
the buffer and are responsible for bounds; no checks are performed on decode.

**Key signatures:**

- ``std::uint32_t read_u32_le(const std::uint8_t* p)``
- ``void write_u32_le(std::uint8_t* p, std::uint32_t val)``

.. code-block:: cpp

    #include <dftracer/utils/core/common/little_endian.h>

    std::uint8_t buf[4];
    write_u32_le(buf, 0xDEADBEEF);
    std::uint32_t v = read_u32_le(buf);   // 0xDEADBEEF

str_format / to_chars
---------------------

Portable string formatting helpers.

``str_cat`` (``str_format.h``) concatenates heterogeneous arguments into one
``std::string``, routing integers/floats through ``to_chars`` (faster than
``std::to_string``, portable unlike ``std::format``); strings and ``char``
append directly and ``bool`` becomes ``"true"``/``"false"``. ``string_format``
(and its ``va_list`` core ``vstring_format``) does printf-style formatting into a
``std::string`` for cold, mixed-content messages. The lower-level
``to_chars.h`` provides ``to_chars_double`` / ``to_chars_u64``, which wrap
``std::to_chars`` with an snprintf fallback where Apple libc++ availability-gates
the floating-point overload.

**Key signatures:**

- ``template <typename... Args> std::string str_cat(const Args&... args)``
- ``std::string string_format(const char* fmt, ...)`` / ``std::string vstring_format(const char* fmt, va_list ap)``
- ``char* to_chars_double(char* first, char* last, double v) noexcept``
- ``char* to_chars_u64(char* first, char* last, std::uint64_t v) noexcept``

.. code-block:: cpp

    #include <dftracer/utils/core/common/str_format.h>

    std::string msg = str_cat("Cannot open ", path, ": errno=", errno);
    std::string hdr = string_format("chunk %d/%d", i, n);

    #include <dftracer/utils/core/common/to_chars.h>
    char buf[32];
    char* end = to_chars_double(buf, buf + sizeof(buf), 3.14);
    std::string_view s(buf, end - buf);

MemoryBudget
------------

Free functions for sizing memory-bounded work.

Declared in ``memory_budget.h``, these compute a process memory budget and
derive from it the channel capacities, per-file batch sizes, and per-file peak
estimates that keep pipelines within available RAM. ``detect_available_memory``
probes the system; ``compute_memory_budget`` applies the default fraction
(``DEFAULT_MEMORY_BUDGET_FRACTION_PERCENT``) unless a user override is given.

**Key signatures:**

- ``std::size_t detect_available_memory()``
- ``std::size_t compute_memory_budget(std::size_t user_override_bytes = 0)``
- ``std::size_t compute_channel_capacity(std::size_t memory_budget_bytes, std::size_t estimated_batch_bytes, std::size_t num_workers)``
- ``std::size_t compute_file_batch_size(std::size_t memory_budget_bytes, std::size_t estimated_file_bytes, std::size_t min_files = 4)``
- ``std::size_t estimate_per_file_bytes(const std::vector<std::size_t>& file_sizes, std::size_t user_override_bytes = 0)``

.. code-block:: cpp

    #include <dftracer/utils/core/common/memory_budget.h>

    std::size_t budget = compute_memory_budget();          // default fraction of RAM
    std::size_t per_file = estimate_per_file_bytes(file_sizes);
    std::size_t batch = compute_file_batch_size(budget, per_file);
    std::size_t cap = compute_channel_capacity(budget, per_file, num_workers);


