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

This project uses ``ruff`` for linting/formatting and ``ty`` for type checking.
Both are run via ``uvx`` (no install needed):

.. code-block:: bash

   # Lint and format check
   make lint

   # Type check
   make typecheck

   # Or directly
   uvx ruff check python/ tests/python/
   uvx ruff format --check python/ tests/python/
   uvx ty check python/

   # Auto-fix lint issues
   uvx ruff check --fix python/ tests/python/

   # Auto-format
   uvx ruff format python/ tests/python/

Configuration is in ``pyproject.toml`` under ``[tool.ruff]``.

C++
~~~

This project uses ``clang-format`` (v19.1.7) for C++ code formatting:

.. code-block:: bash

   make format        # auto-fix
   make check-format  # check only (CI uses this)

Conventions beyond formatting (checked in review):

- **C++20**, no compiler extensions. Use ``#ifndef`` header guards, not
  ``#pragma once``.
- **Namespaces mirror directories** (``dftracer::utils``,
  ``dftracer::utils::utilities``, ``...::behaviors``, ``...::tags``).
- **Constants** are ``UPPER_SNAKE_CASE`` (not ``kCamelCase``); keep
  module-specific constants with their module, not everything in
  ``common/constants.h``.
- **Fixed-width integers** use the ``std::``-qualified types from
  ``<cstdint>`` / ``<cstddef>`` (``std::int64_t``, ``std::size_t``), not the
  global unqualified names.
- **Comments** explain the non-obvious "why"; do not narrate what the code
  plainly does, and match the surrounding comment density.
- **Pure ASCII** in code, comments, and docs: write ``-`` (not an em-dash),
  plain quotes, and ``->`` (not a unicode arrow).

Logging and Tracing
~~~~~~~~~~~~~~~~~~~~

The library has its own small logger (namespace ``dftracer::utils::logger``).
Configure it once at startup with functions; emit with macros:

.. code-block:: cpp

   #include <dftracer/utils/core/common/logging.h>
   namespace logger = dftracer::utils::logger;

   logger::init();  // reads env, defaults to Info + auto color
   // or: logger::init({.level = logger::Level::Debug,
   //                   .color = logger::ColorMode::Never});

   DFTRACER_UTILS_LOG_ERROR("cannot open %s: errno=%d", path, e);
   DFTRACER_UTILS_LOG_INFO("processed %zu files", n);
   DFTRACER_UTILS_LOG_DEBUG("resolved %d checkpoints", k);

   logger::set_level(logger::Level::Debug);  // change verbosity at runtime

Levels are ``Trace < Debug < Info < Warn < Error < Off``. Each level is
**compile-gated** by the build (``LOGGER_LEVEL_*``) and **runtime-gated** by the
current level: a compiled-out level costs nothing, and a compiled-in but
disabled level costs one predicted branch with its arguments left unevaluated.
All levels including Trace are compiled in by default; build with
``-DDFTRACER_UTILS_LOGGER_LEVEL_TRACE=OFF`` to strip every Trace-level construct
(the scope tracer and coroutine auto-tracing) for a minimal build. Set the
runtime level without a rebuild via ``DFTRACER_UTILS_LOG_LEVEL`` (see
:doc:`installation`), and control color with ``ColorMode`` / ``NO_COLOR`` /
``FORCE_COLOR``.

**Scope tracing.** ``DFTRACER_UTILS_TRACE_SCOPE`` logs ``-> label`` on entry and
``<- label [ms]`` on exit, only when Trace is enabled. With no argument the label
is the function name; extra arguments are printf-formatted and appended:

.. code-block:: cpp

   coro::CoroTask<int> read_chunk(std::size_t off, std::size_t len) {
       DFTRACER_UTILS_TRACE_SCOPE("off=%zu len=%zu", off, len);
       ...  // "-> read_chunk: off=... len=..." now, "<- ... [x ms]" on return
   }

**Coroutine auto-tracing.** When Trace is enabled at runtime, every co_awaited
``CoroTask`` is traced automatically (``-> <function> (file:line)`` on entry,
``<- <function> [ms]`` on exit) with no annotation needed - the hook lives in the
task awaiter and the location is the coroutine's definition site. The output is a
flat stream, not an indented tree: under the multi-threaded executor coroutines
migrate across threads and interleave, so faithful indentation is impossible. For
a true nested call tree use the monitor (``DFTRACER_UTILS_MONITOR=tree``); scope
tracing and auto-tracing are the flat, greppable, per-line view.

Do not put ``DFTRACER_UTILS_LOG_DEBUG`` / ``TRACE`` or ``TRACE_SCOPE`` in a
genuine per-event/per-byte inner loop: the runtime gate is cheap per operation
but not free across billions of iterations. For structural coroutine/task
profiling, prefer the built-in monitor (``DFTRACER_UTILS_MONITOR``) instead.

Git Hooks
~~~~~~~~~

Install the project's pre-commit hooks:

.. code-block:: bash

   ./scripts/git-hooks.sh install

The pre-commit hook runs:

- **C/C++**: ``clang-format`` on staged ``.c/.cpp/.h/.hpp`` files
- **Python**: ``ruff check``, ``ruff format --check``, and ``ty check`` on staged ``.py/.pyi`` files

Python checks require ``uvx`` or ``ruff`` in PATH; they are skipped gracefully if not available.

Contributing
------------

1. Fork the repository
2. Create a feature branch
3. Install git hooks: ``./scripts/git-hooks.sh install``
4. Make your changes
5. Run tests and ensure they pass (``make test && make test-py``)
6. Run lint and type check (``make lint && make typecheck``)
7. Submit a pull request

Coding Guidelines
-----------------

- Follow the existing code style
- Write tests for new functionality
- Update documentation as needed
- Keep commits atomic and well-described
- All Python code must pass ``ruff check`` and ``ty check``
- All C++ code must pass ``clang-format`` check

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

        // Spawn producer -- producer() pre-registers the slot eagerly
        scope.spawn([ch = channel->producer()](CoroScope& s) mutable
                        -> coro::CoroTask<void> {
            auto guard = ch.guard();
            for (int i = 0; i < 100; ++i) {
                co_await ch.send(Event{i});
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

**Storing JsonValue / simdjson views beyond the parser's lifetime**

``JsonValue`` (and the underlying ``simdjson::ondemand::value`` /
``simdjson::dom::element``) is a non-owning view into the parser's buffer.
Never store it across the parser's or the input buffer's lifetime.

.. code-block:: cpp

    #include <simdjson.h>

    // WRONG: parser/buffer destroyed, but view stored
    JsonValue stored_value;
    {
        simdjson::ondemand::parser parser;
        auto padded = simdjson::padded_string::load("config.json");
        auto doc = parser.iterate(padded);
        stored_value = doc.find_field("root").value();
    }
    // stored_value now points into freed parser/buffer memory!

    // CORRECT: copy the data out before the parser goes out of scope
    {
        simdjson::ondemand::parser parser;
        auto padded = simdjson::padded_string::load("config.json");
        auto doc = parser.iterate(padded);
        auto data = serialize_json_value(doc.find_field("root").value());
        // data owns its copy; safe to use after the parser is destroyed
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
