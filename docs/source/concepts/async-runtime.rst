:description: Why the core is built on C++20 coroutines instead of threads or callbacks, and how CoroTask, Scope, the io backend, and Runtime fit together.

Async runtime
=============

What this explains: why ``dftracer_utils_runtime`` is built on C++20 coroutines
instead of a thread-per-request or callback model, and how the pieces
(``CoroTask``, ``Scope``, the io backend, ``Runtime``/``Executor``) fit
together.

Why coroutines, not blocking threads
--------------------------------------

Indexing and scanning a trace directory is I/O-bound: most of the wall-clock
time is spent waiting on reads of many small compressed files, not on CPU
work. Two conventional ways to get concurrency out of that are a thread per
file (expensive once the file count runs into the thousands) or a callback
pyramid (cheap but unreadable and easy to get wrong around cancellation and
error propagation). Coroutines give a third option: code that reads like
straight-line, sequential logic but suspends at an I/O call and gives the
thread back to the runtime instead of blocking it.

.. code-block:: cpp

   coro::CoroTask<Result> process(int fd, char* buf, std::size_t len) {
       auto n = co_await runtime::io::read(fd, buf, len);  // thread freed here
       co_return parse(buf, n);
   }

When ``runtime::io::read`` has no data ready, the coroutine's frame is parked and the
worker thread picks up other runnable work. When the read completes, the
frame is resumed, possibly on a different worker thread. The caller never
sees a callback; it sees a function that looks like it blocked.

``CoroTask``: the unit of async work
---------------------------------------

``coro::CoroTask<T>`` (``coro/task.h``) is the coroutine return type used
throughout the codebase. It is lazy: a ``CoroTask`` does not start running
until it is awaited or spawned, which is what lets composition operators
(``&&`` for concurrent ``when_all``, ``||`` for ``when_any``) build a graph of
work before anything executes. ``coro/channel.h`` adds a
producer/consumer channel for streaming values between coroutines,
and ``coro/async_mutex.h`` / ``async_generator.h`` round out the
primitives a coroutine-based pipeline needs that a plain thread-based one
would reach for a blocking mutex or an iterator to get.

``Scope``: structured concurrency
----------------------------------------

A bare ``CoroTask`` has no notion of "who owns this work" beyond its caller.
``coro::Scope`` (``coro/scope.h``) is the object that owns a group of child tasks.
``coro::with_scope(fn)`` makes one, passes it to ``fn`` and always joins it.
This is what makes concurrency structured rather than fire-and-forget:

- ``scope.spawn(...)`` starts a child coroutine that the scope joins.
  ``scope.spawn_future(...)`` returns a ``SpawnFuture<T>``, which you can
  ``co_await`` to join that specific child.
- ``co_await scope.join()`` waits for every child and raises the first error of a
  ``spawn`` child.
  A scope that spawned and is destroyed without a join ends the process.
  ``with_scope`` joins also when its body raises.
- ``dftracer::utils::runtime::io::read``/``write``/``open``/``close``
  are the async I/O surface: free functions that submit to the executor's io
  backend and suspend, rather than blocking. They need no ``Scope``
  argument - inside an executor worker they suspend on the configured
  backend; outside one they fall back to a blocking syscall.
- Cancellation propagates through the scope, so a cancelled parent can stop
  its children instead of leaving them running past the point anyone still
  wants their result.

This is the same idea structured concurrency gives you in other coroutine
runtimes: a scope's children cannot outlive the scope silently. That guarantee
is also why the type exists at all - see :doc:`coroutine-caveats` for what
goes wrong when a coroutine's lifetime is not tracked this carefully.

The io backend: one abstraction, several implementations
------------------------------------------------------------

``IoBackend`` is the abstract surface async I/O suspends on, with a selectable
concrete implementation (``IoBackendType`` in ``runtime/io/backend_type.h``):

- **io_uring** on Linux, when available - the kernel does the async I/O and
  the coroutine resumes on completion, no worker thread blocks on it. One
  thread owns the ring, because the kernel cancels a request when the thread
  that submitted it exits. A call with no ring opcode, such as a directory
  listing or a ``stat``, runs on an I/O thread.
- **kqueue + thread pool** on macOS/BSD.
- **epoll + thread pool** as a portable fallback where neither of the above
  is usable.

``AUTO`` picks the best available backend at runtime. Domain code never talks
to a backend directly; it goes through the ``runtime::io::read``/``write``/``open``/
``close`` free functions and stays oblivious to which backend is underneath.
That is the same layering ``architecture`` describes for the whole codebase,
applied at the I/O boundary: one abstraction, several platform-specific
implementations, and a compile-time choice of which ones are even compiled
in.

Runtime and Executor
--------------------

Two levels of "run this coroutine" exist, for different callers:

- ``Runtime`` (``runtime/runtime.h``) is the lightweight entry point: an
  ``Executor`` plus a ``Watchdog``, with no DAG or scheduler overhead. It is
  what the Python bindings and other callers use to run
  coroutines on a thread pool. Its worker count is elastic when configured
  with a ``min_workers`` floor below the ``thread_count`` cap, so a
  mostly-idle process does not pin a full thread pool on a shared HPC login
  node.
- ``Executor`` (``coro/executor.h``) is the thread pool and run
  queue that actually resumes coroutine frames.

.. mermaid::

   graph TB
       Runtime["Runtime<br/>(Executor + Watchdog)"]
       Executor["Executor<br/>(thread pool, run queue)"]
       IoBackend["IoBackend<br/>(io_uring / kqueue / epoll)"]
       Runtime --> Executor
       Executor --> IoBackend

A caller that needs "run these coroutines concurrently" reaches for
``Runtime``, which runs on the same ``Executor`` as the rest of the engine, so there
is one thread pool and one io backend selection for the whole process, not
one per subsystem.

See also
--------

- :doc:`coroutine-caveats` for the lifetime rules and compiler workarounds
  this model requires in practice.
- :doc:`architecture` for how the runtime relates to the domain layer above
  it.
- :doc:`../cpp_api/coro` for the generated API reference.
- :doc:`../guides/pipelines/patterns` for task-oriented pipeline recipes.
