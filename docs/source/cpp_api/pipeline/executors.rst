Executor Classes
=================

Executor classes for running pipeline tasks.

.. mermaid::

   graph TB
       Executor["Executor"]
       Workers["Worker Threads<br/>(N threads)"]
       RunQueue["ConcurrentQueue<br/>(coroutine handles)"]
       IoBack["IoBackend"]
       SqlPool["SQLite ThreadPool"]
       Timer["TimerService"]

       Executor --> Workers
       Executor --> RunQueue
       Executor --> IoBack
       Executor --> SqlPool
       Executor --> Timer
       Workers --> |dequeue + resume| RunQueue

Executor
--------

.. doxygenclass:: dftracer::utils::Executor
   :project: dftracer-utils
   :members:
   :protected-members:
   :undoc-members:

Configuration
-------------

.. doxygenstruct:: dftracer::utils::ExecutorConfig
   :project: dftracer-utils
   :members:
   :undoc-members:

Progress Tracking
-----------------

.. doxygenstruct:: dftracer::utils::TaskInfo
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::TaskProgress
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::ExecutorProgress
   :project: dftracer-utils
   :members:
   :undoc-members:
