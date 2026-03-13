Developer's Guide
=================

This guide contains information for developers contributing to dftracer utilities.

For more detailed development information, see the `DEVELOPERS_GUIDE.md <https://github.com/LLNL/dftracer-utils/blob/main/DEVELOPERS_GUIDE.md>`_ in the repository.

Development Setup
-----------------

1. Clone the repository:

   .. code-block:: bash

      git clone https://github.com/LLNL/dftracer-utils.git
      cd dftracer-utils

2. Install development dependencies:

   .. code-block:: bash

      pip install -e ".[dev]"

3. Build the C++ components:

   .. code-block:: bash

      mkdir build && cd build
      cmake ..
      make

Running Tests
-------------

Python Tests
~~~~~~~~~~~~

.. code-block:: bash

   pytest tests/

C++ Tests
~~~~~~~~~

.. code-block:: bash

   cd build
   ctest

Code Coverage
-------------

To run tests with coverage:

.. code-block:: bash

   ./coverage.sh

Building Documentation
----------------------

To build the documentation locally:

.. code-block:: bash

   cd docs
   make html

The built documentation will be in ``docs/build/html/``.

Code Style
----------

Python
~~~~~~

This project uses ``ruff`` for Python code formatting:

.. code-block:: bash

   ruff check .
   ruff format .

C++
~~~

This project uses ``clang-format`` for C++ code formatting:

.. code-block:: bash

   clang-format -i src/**/*.cpp include/**/*.h

Contributing
------------

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run tests and ensure they pass
5. Submit a pull request

Coding Guidelines
-----------------

- Follow the existing code style
- Write tests for new functionality
- Update documentation as needed
- Keep commits atomic and well-described

Coroutine Development Guidelines
---------------------------------

dftracer utilities uses C++20 coroutines extensively for async I/O and concurrent pipeline processing.
Coroutines require careful handling of object lifetimes and capture semantics because coroutine frames
are heap-allocated and may outlive the caller's stack.

Capture Rules for Coroutine Lambdas
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Scalars (int, bool, size_t, enum): capture by value**

Cheap and always safe. The scalar value is copied into the coroutine frame.

.. code-block:: cpp

    int event_id = 42;
    auto task = [event_id](CoroScope& scope) -> coro::CoroTask<void> {
        // Safe: event_id is copied into the coroutine frame
        co_await channel.send(event_id);
    };

**Owning types (std::string, shared_ptr, unique_ptr): capture by value**

Safe because the coroutine owns a copy. Automatic cleanup on coroutine destruction.

.. code-block:: cpp

    std::string filename = "trace.pfw.gz";
    auto task = [filename](CoroScope& scope) -> coro::CoroTask<void> {
        // Safe: coroutine owns a copy of the string
        std::cout << "Processing " << filename << "\n";
        co_await something_async();
    };

**Large containers (std::vector, std::map): use pointer-by-value**

Avoid expensive deep copies. Use pointer-by-value (`auto* ptr = &vec; [ptr](...)`).

.. code-block:: cpp

    std::vector<Event> events = load_events();
    auto* events_ptr = &events;
    auto task = [events_ptr](CoroScope& scope) -> coro::CoroTask<void> {
        // Safe: events_ptr points to the vector in the caller's scope
        for (const auto& e : *events_ptr) {
            co_await process_event(e);
        }
    };
    
    // WRONG: Do NOT capture the entire vector
    // auto task = [events](CoroScope& scope) -> coro::CoroTask<void> { // BAD!
    //     for (const auto& e : events) { ... }
    // };

**Non-owning views (string_view, span<T>, raw T*, iterators): NEVER capture by value**

String_view and span are non-owning views. Capturing by value copies the view but NOT the underlying data.
The underlying data will be freed before the coroutine runs, leading to use-after-free bugs.
Use pointer-by-value instead.

.. code-block:: cpp

    std::string data = "important";
    std::string_view view = data;
    
    // WRONG: view points to freed memory
    // auto task = [view](CoroScope& scope) -> coro::CoroTask<void> {
    //     std::cout << view << "\n";  // Use-after-free!
    // };
    
    // CORRECT: use pointer-by-value
    auto* data_ptr = &data;
    auto task = [data_ptr](CoroScope& scope) -> coro::CoroTask<void> {
        std::string_view safe_view(*data_ptr);
        std::cout << safe_view << "\n";  // Safe
    };

**References (&var): NEVER capture by reference in coroutine lambdas**

References in coroutine lambdas dangle immediately. Use pointer-by-value or value capture instead.

.. code-block:: cpp

    int counter = 0;
    
    // WRONG: reference dangles
    // auto task = [&counter](CoroScope& scope) -> coro::CoroTask<void> {
    //     counter++;  // Undefined behavior!
    // };
    
    // CORRECT: use pointer-by-value
    auto* counter_ptr = &counter;
    auto task = [counter_ptr](CoroScope& scope) -> coro::CoroTask<void> {
        (*counter_ptr)++;  // Safe
    };

**Default capture ([&]): NEVER use in coroutine lambdas**

Default capture by reference captures all variables by reference, leading to dangling pointers.
Always use explicit capture lists.

.. code-block:: cpp

    int event_id = 42;
    std::string name = "event";
    
    // WRONG: all variables captured by reference
    // auto task = [&](CoroScope& scope) -> coro::CoroTask<void> { ... };
    
    // CORRECT: explicit captures by value or pointer
    auto* name_ptr = &name;
    auto task = [event_id, name_ptr](CoroScope& scope) -> coro::CoroTask<void> {
        co_await channel.send(event_id);
        std::cout << *name_ptr << "\n";
    };

CoroScope Lifetime Rules
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Always ensure the ``CoroScope`` outlives all spawned tasks and channels.

.. code-block:: cpp

    auto task = make_task([](CoroScope& scope) -> coro::CoroTask<void> {
        auto channel = coro::make_channel<Event>(100);

        // Spawn producer with RAII guard
        scope.spawn([channel](CoroScope& s) -> coro::CoroTask<void> {
            auto guard = channel->producer_guard();
            for (int i = 0; i < 100; ++i) {
                co_await channel->send(Event{i});
            }
            // ~ProducerGuard auto-releases; channel closes when last producer exits
            co_return;
        });

        // Consumer reads until channel closes
        while (auto event = co_await channel->receive()) {
            process(*event);
        }

        co_return;
    });

Channel Patterns
~~~~~~~~~~~~~~~~

Use bounded channels for backpressure control:

.. code-block:: cpp

    // Bounded channel: send() blocks if queue is full
    coro::Channel<Event> bounded_ch(1000);
    
    // Unbounded channel: send() never blocks (use carefully)
    coro::Channel<Event> unbounded_ch(0);

HasherUtility Pattern
~~~~~~~~~~~~~~~~~~~~~

For hot loops, reuse a single ``HasherUtility`` instance with ``reset()``:

.. code-block:: cpp

    // Create once, reuse many times
    HasherUtility hasher;
    
    for (const auto& event : events) {
        hasher.reset();  // Clear state before each hash
        hasher.update(event.data);
        auto hash = hasher.finalize();
        // ... use hash ...
    }
    
    // WRONG: allocating per-event is expensive
    // for (const auto& event : events) {
    //     HasherUtility temp_hasher;  // BAD!
    //     temp_hasher.update(event.data);
    //     auto hash = temp_hasher.finalize();
    // }

Anti-Patterns to Avoid
~~~~~~~~~~~~~~~~~~~~~~

**Storing JsonValue beyond yyjson_doc lifetime**

``JsonValue`` is a non-owning view into a ``yyjson_doc``. Never store it across the document's lifetime.

.. code-block:: cpp

    // WRONG: doc destroyed, but view stored
    JsonValue stored_value;
    {
        yyjson_doc* doc = yyjson_read_file("config.json", NULL);
        stored_value = yyjson_get_obj(doc);
        yyjson_doc_free(doc);
    }
    // stored_value now points to freed memory!
    
    // CORRECT: copy data before doc destruction
    {
        yyjson_doc* doc = yyjson_read_file("config.json", NULL);
        auto data = serialize_json_value(yyjson_get_obj(doc));
        yyjson_doc_free(doc);
        // data is now safe
    }

**Instantiating IOExecutor directly**

``IOExecutor`` is internal to the Pipeline. Never create it directly; use ``Pipeline`` or task framework instead.

**Per-event SQL indexing**

Avoid querying the database for every event. Use bloom filters and per-chunk statistics instead.

.. code-block:: cpp

    // WRONG: N database queries for N events
    for (const auto& event : events) {
        auto result = db.query(event.key);  // BAD!
    }
    
    // CORRECT: batch statistics with bloom filters
    BloomIndex bloom;
    for (const auto& chunk : chunks) {
        bloom.add_chunk_stats(chunk);
    }

**Old Pipeline API**

All new binaries must use the coroutine + channel pattern. Do not use the old synchronous ``Pipeline`` API.

**Batch materialization**

Stream through channels incrementally; avoid materializing entire batches into vectors.

.. code-block:: cpp

    // WRONG: materializes entire batch
    std::vector<Event> batch;
    while (auto event = co_await channel.receive()) {
        batch.push_back(*event);
    }
    // process batch...
    
    // CORRECT: process incrementally
    while (auto event = co_await channel.receive()) {
        co_await process_event(*event);
    }
