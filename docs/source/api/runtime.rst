:description: Reference for the Python Runtime and TaskHandle: a thread pool that runs coroutines and Python callables over the C++ executor.

Runtime Module
==============

The ``Runtime`` manages a thread pool for executing coroutines and Python
callables. It wraps the C++ ``Executor`` and ``Watchdog`` without
Pipeline/DAG overhead.

Runtime Class
-------------

Type relationships
------------------

How the runtime types relate:

.. mermaid:: /_generated/py_runtime.mmd

.. autoclass:: dftracer.utils.Runtime(threads: int = 0, io_threads: int = 0, python_threads: int = 0)
   :members:
   :undoc-members:
   :show-inheritance:
   :special-members: __enter__, __exit__

``threads`` sizes the C++ compute pool; ``io_threads`` sizes a separate pool
dedicated to blocking I/O tasks; ``python_threads`` sizes the Python
``ThreadPoolExecutor`` that runs Python callables submitted to ``submit()``
(default ``min(32, threads)``). All three default to ``0``, which lets the
runtime auto-size based on the host.

TaskHandle Class
----------------

.. autoclass:: dftracer.utils.TaskHandle
   :members:
   :undoc-members:
   :show-inheritance:

Module-level Functions
----------------------

.. autofunction:: dftracer.utils.get_default_runtime

.. autofunction:: dftracer.utils.peek_default_runtime

.. autofunction:: dftracer.utils.set_default_runtime

Submitting Tasks
----------------

``submit()`` is always async -- it returns a ``TaskHandle`` immediately.
Use ``.get()`` to block and retrieve the result, or ``.wait()`` to block
without a return value.

.. code-block:: python

   import dftracer.utils as dft

   rt = dft.Runtime(threads=8, io_threads=4)

   # Submit a Python callable
   h = rt.submit(lambda x, y: x + y, 3, 4)
   result = h.get()  # blocks, returns 7

   # Fire multiple tasks
   handles = [rt.submit(process, f) for f in files]
   rt.wait_all()  # blocks until all complete

   # Check results
   for h in handles:
       print(h.name, h.get())

Task Naming
~~~~~~~~~~~

``submit()`` takes only the callable and its positional/keyword arguments;
there is no ``name`` parameter. The task name is derived automatically from
the callable's qualified name plus, when available, its definition's source
location (``file:line``):

.. code-block:: python

   def my_function(): ...
   rt.submit(my_function)           # name = "my_function (script.py:1)"

   class Pipeline:
       def run(self): ...
   p = Pipeline()
   rt.submit(p.run)                 # name = "Pipeline.run (script.py:5)"

   rt.submit(lambda: None)          # name = "<lambda> (script.py:8)"

Composing Tasks
~~~~~~~~~~~~~~~

Tasks can be composed by passing resolved values or handles:

.. code-block:: python

   # Pattern 1: resolve value, pass to next task
   def compose(filename):
       h1 = rt.submit(index, filename)
       result = h1.get()                    # blocks for index result
       h2 = rt.submit(process, result)      # uses resolved value
       return h2.get()

   h = rt.submit(compose, "trace.pfw.gz")
   print(h.get())

   # Pattern 2: pass handle directly (utility unwraps internally)
   def compose(filename):
       h1 = rt.submit(index, filename)
       h2 = rt.submit(process, h1)          # process calls h1.get()
       return h2.get()

Error Handling
--------------

Per-task errors are propagated via ``.get()`` and ``.wait()``:

.. code-block:: python

   h = rt.submit(failing_function)
   try:
       h.get()
   except ValueError as e:
       print(f"Task failed: {e}")

Batch error handling with ``wait_all()``:

.. code-block:: python

   # Default: wait_all() does NOT raise on errors
   rt.submit(failing_function)
   rt.wait_all()

   # Check failures
   for h in rt.get_failed():
       print(f"{h.name} failed: {h.exception}")

   # Strict mode: raise after all tasks complete
   rt.wait_all(raise_on_error=True)  # RuntimeError: 1 task(s) failed: ...

``wait_all(raise_on_error=True)`` raises a summary ``RuntimeError``, but each
individual failure (``h.exception``, and what ``.get()`` re-raises) is the
task's original typed exception - ``DFTUtilsError`` or a subclass such as
``DFTUtilsIOError``. See the error-handling section of :doc:`../quickstart`.

Error callbacks for async notification:

.. code-block:: python

   rt.set_error_callback(lambda h, e: print(f"FAILED {h.name}: {e}"))
   rt.submit(failing_function)
   rt.wait_all()

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

Logging
-------

Control the native C++ logger from Python. These are process-global.

.. code-block:: python

   from dftracer.utils import set_log_level, get_log_level, set_log_color

   set_log_level("debug")   # trace | debug | info | warn | error | off
   set_log_color("never")   # auto | always | never
   level = get_log_level()  # current level as a string

.. autofunction:: dftracer.utils.set_log_level

.. autofunction:: dftracer.utils.get_log_level

.. autofunction:: dftracer.utils.set_log_color
