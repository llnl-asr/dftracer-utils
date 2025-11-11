Coroutine API
=============

C++20 coroutine primitives for asynchronous task execution. All classes are in the ``dftracer::utils::coro`` namespace.

For usage examples, see :doc:`/pipeline`.

CoroTask
--------

User-facing coroutine task type with awaitable interface and combinators.

.. doxygenclass:: dftracer::utils::coro::CoroTask
   :project: dftracer-utils
   :members:
   :undoc-members:

Channel
-------

Thread-safe producer-consumer queue for streaming data between tasks.

.. doxygenclass:: dftracer::utils::coro::Channel
   :project: dftracer-utils
   :members:
   :undoc-members:

Generator
---------

Synchronous lazy sequence generator using ``co_yield``.

.. doxygenclass:: dftracer::utils::coro::Generator
   :project: dftracer-utils
   :members:
   :undoc-members:

AsyncGenerator
--------------

Asynchronous lazy sequence generator for async iteration.

.. doxygenclass:: dftracer::utils::coro::AsyncGenerator
   :project: dftracer-utils
   :members:
   :undoc-members:

when_all
--------

Wait for all awaitables to complete.

.. doxygenfunction:: dftracer::utils::coro::when_all
   :project: dftracer-utils

when_any
--------

Race multiple awaitables, return first to complete.

.. doxygenstruct:: dftracer::utils::coro::WhenAnyResult
   :project: dftracer-utils
   :members:

.. doxygenfunction:: dftracer::utils::coro::when_any
   :project: dftracer-utils

TimeoutAwaitable
----------------

Timeout awaitable for use with ``when_any``.

.. doxygenclass:: dftracer::utils::coro::TimeoutAwaitable
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenfunction:: dftracer::utils::coro::timeout
   :project: dftracer-utils
