:description: Reference for the core async runtime: the Executor, the scheduler and watchdog, the Error type, and shared concurrency primitives.

Runtime
=======

.. seealso::

   :doc:`../concepts/async-runtime` for the runtime model, and :doc:`coro` for the primitives it drives.

The core async runtime lives in ``dftracer::utils``: the
``Executor`` that owns worker threads and the coroutine scheduler, the
``Scheduler``, ``Watchdog``, and ``TimerService`` services, the ``Error`` type,
and the shared concurrency and memory primitives (sharded map, object and buffer
pools, string interning) used across the engine.

Type relationships
------------------

How the runtime, executors, tasks, and pipeline relate:

.. mermaid:: /_generated/runtime.mmd

.. include:: /cpp_api/_generated/core.rst.inc
.. include:: /cpp_api/_generated/common.rst.inc
.. include:: /cpp_api/_generated/common.logger.rst.inc
.. include:: /cpp_api/_generated/memory.rst.inc
