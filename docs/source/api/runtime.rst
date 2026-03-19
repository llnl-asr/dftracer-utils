Runtime Module
==============

The ``Runtime`` manages a thread pool for executing coroutines. It wraps
the C++ ``Executor`` and ``Watchdog`` without Pipeline/DAG overhead.

Runtime Class
-------------

.. autoclass:: dftracer.utils.Runtime
   :members:
   :undoc-members:
   :show-inheritance:
   :special-members: __init__, __enter__, __exit__

Module-level Functions
----------------------

.. autofunction:: dftracer.utils.get_default_runtime

.. autofunction:: dftracer.utils.set_default_runtime

Progress Tracking
-----------------

``get_progress()`` returns a dict with:

.. code-block:: python

   {
       "total": 10,
       "completed": 8,
       "running": 2,
       "queued": 0,
       "failed": 0,
       "workers": [
           {"id": 0, "idle": False, "task": "iter_lines", "queue_depth": 0},
           {"id": 1, "idle": True, "task": "", "queue_depth": 0},
       ],
       "tasks": [
           {"name": "iter_lines", "state": "completed",
            "execution_duration_ms": 12.3, "queued_duration_ms": 0.1,
            "progress_pct": 100.0, ...},
       ],
       "errors": [],
   }

Dask Integration
----------------

For ``dask.distributed``, use ``DFTracerUtilsDaskWorkerPlugin`` to create
a per-worker Runtime:

.. code-block:: python

   from dask.distributed import Client
   from dftracer.utils.dask import DFTracerUtilsDaskWorkerPlugin

   client = Client("scheduler:8786")
   client.register_plugin(DFTracerUtilsDaskWorkerPlugin(threads=48))

Dask is an optional dependency -- the plugin module is only importable
when ``dask.distributed`` is installed.
