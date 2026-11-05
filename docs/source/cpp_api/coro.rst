:description: Reference for the C++20 coroutine primitives: CoroTask, Channel, Generator, CoroScope, and the when_all/when_any/timeout combinators.

Coroutines
==========

.. seealso::

   :doc:`../concepts/async-runtime` for the coroutine model and
   :doc:`../guides/pipelines/patterns` for task-oriented usage.

The C++20 coroutine primitives in ``dftracer::utils::coro``: ``CoroTask``, the
``Channel`` and ``Generator`` types, ``CoroScope`` for structured concurrency,
the ``when_all`` / ``when_any`` / ``first_success`` / ``parallel_map`` combinators,
the time operations ``sleep`` / ``with_timeout`` / ``with_deadline`` / ``retry``,
and the waits ``AsyncEvent`` / ``AsyncLatch`` / ``AsyncSemaphore``.

Type relationships
------------------

Composition and inheritance among the coroutine primitives:

.. mermaid:: /_generated/coro.mmd

.. include:: /cpp_api/_generated/coro.rst.inc
