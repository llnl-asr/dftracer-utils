:description: Get started fast: query a trace directory with TraceViewer in Python and read an aggregated result back as a pandas DataFrame.

Quick Start Guide
=================

This guide will help you get started with dftracer utilities quickly.

Python Quick Start
------------------

Querying with TraceViewer
~~~~~~~~~~~~~~~~~~~~~~~~~~~

:class:`~dftracer.utils.TraceViewer` is the primary way to query a trace. It is
lazy and Arrow-first: builder methods compose a query and a terminal
(``collect``) runs it in one pass, served from the index when one exists.

.. code-block:: python

   from dftracer.utils import TraceViewer

   view = TraceViewer("traces/")           # a directory (scanned recursively), a file, or a list of files

   # Top I/O calls by total time.
   df = (
       view.filter('cat == "POSIX"')
           .group_by("name")
           .agg("count", "sum:dur", "max:dur")
           .collect()                       # -> LazyFrame
           .collect()                       # -> DataFrame
   )
   pdf = df.to_pandas()

   # A bandwidth time series: bytes per 1 s window per call.
   ts = (
       view.filter('cat == "POSIX"')
           .time_bucket(1_000_000)
           .group_by("name", "time_bucket")
           .agg("sum:size")
           .collect()
           .collect()
   )

See :doc:`api/trace_viewer` for the full builder, aggregation specs, and
``collect_typed``; :doc:`api/query` for the filter DSL; and :doc:`guides/index`
for task-oriented recipes. The rest of this page covers ``Runtime``,
``Indexer``, and Dask.

Reading events
~~~~~~~~~~~~~~

``collect()`` builds the group-by/aggregate query plan into a lazy
:class:`~dftracer.utils.LazyFrame`; nothing scans until you call ``.collect()``
on that in turn, which materializes the result as a native
:class:`~dftracer.utils.DataFrame` (``to_arrow()`` / ``to_pandas()`` convert only
at the edge, on the ``DataFrame``, not the ``LazyFrame``). A query with no
``group_by``/``agg`` reduces to a one-row ``count`` - it does **not** return
the raw events. To read matching events as native DataFrames, use
``collect_typed()`` (splits into the ``regular`` / ``counters`` / ``aggregated``
phase families) or ``stream()`` for out-of-core reads.

.. code-block:: python

   view = TraceViewer("traces/")

   # Per-cat aggregate as a pandas DataFrame.
   df = (
       view.filter('cat == "POSIX"').group_by("cat").agg("count", "mean:dur")
           .collect().collect().to_pandas()
   )

   # Raw matching events as native DataFrames, by phase family.
   regular = view.filter('cat == "POSIX"').collect_typed()["regular"]

   # Stream Arrow batches instead of materializing.
   import pyarrow
   for batch in view.filter('cat == "POSIX"').stream(batch_size=10000):
       process(pyarrow.record_batch(batch))

Time-unit normalization
~~~~~~~~~~~~~~~~~~~~~~~~~

By default ``ts``/``dur`` are in the trace's native time unit, declared by its
``CM`` ``time_metric`` metadata event (``NS``/``MS``/``SEC``/``US``; absent
means microseconds). ``time_unit`` / ``time_scale`` rescale them for display:

.. code-block:: python

   view.time_unit("us")     # interpret the native unit, scale ts/dur to microseconds
   view.time_scale(1.0)     # or an explicit nanoseconds-per-unit ratio

Query predicates (e.g. ``ts >= ...``) always match the native index and are
unaffected by the display unit.

Async Task Submission
~~~~~~~~~~~~~~~~~~~~~

``Runtime.submit()`` runs tasks asynchronously and returns a ``TaskHandle``:

.. code-block:: python

   from dftracer.utils import Runtime

   with Runtime(threads=8, python_threads=4) as rt:
       # Submit Python callables -- runs on Python thread pool
       h1 = rt.submit(process_file, "trace1.pfw.gz")
       h2 = rt.submit(process_file, "trace2.pfw.gz")

       # Wait for all tasks
       rt.wait_all()

       # Or get individual results
       result = h1.get()  # blocks until h1 completes

Task names are auto-derived from the callable (there is no ``name`` argument):

.. code-block:: python

   rt.submit(my_function)           # name = "my_function"
   rt.submit(obj.method)            # name = "MyClass.method"
   rt.submit(lambda: None)          # name = "<lambda>"

Composing tasks with dependency chains:

.. code-block:: python

   def compose(filename):
       h1 = rt.submit(index_file, filename)
       result = h1.get()                       # wait for index
       h2 = rt.submit(query_index, result)     # use result
       return h2.get()

   h = rt.submit(compose, "trace.pfw.gz")
   print(h.get())

Error handling:

.. code-block:: python

   # Per-task: .get() re-raises the original exception
   try:
       h.get()
   except ValueError as e:
       print(f"Task failed: {e}")

   # Batch: wait_all(raise_on_error=True) raises after all complete
   rt.wait_all(raise_on_error=True)

   # Callback: async notification on failure
   rt.set_error_callback(lambda h, e: log.error(f"{h.name}: {e}"))

   # Inspect failures
   for h in rt.get_failed():
       print(f"{h.name}: {h.exception}")

Failures raised by library operations are typed (``DFTUtilsError`` and its
subclasses); see `Error Handling`_ below.

Using with Dask
~~~~~~~~~~~~~~~

For distributed processing with ``dask.distributed``:

.. code-block:: python

   from dask.distributed import Client
   from dftracer.utils.dask import DFTracerUtilsDaskWorkerPlugin

   client = Client("scheduler:8786")
   client.register_plugin(DFTracerUtilsDaskWorkerPlugin(threads=48))

   def count_events(path):
       from dftracer.utils import TraceViewer
       return TraceViewer([path]).agg("count").collect().collect().to_pandas()["count"].sum()

   futures = client.map(count_events, file_paths)
   results = client.gather(futures)

For a distributed aggregation that fans one query across the cluster, use
:class:`~dftracer.utils.dask.DaskTraceViewer` (see :doc:`guides/index`).

Working with Indexer
~~~~~~~~~~~~~~~~~~~~

Create and use indexes for faster access:

.. code-block:: python

   from dftracer.utils import Indexer

   # Create an indexer over a directory of traces (or pass files=[...])
   indexer = Indexer("/path/to/traces")

   # Resolve what is already indexed vs. what needs work
   status = indexer.resolve()
   print(f"Total files: {status.total_files}")
   print(f"Ready: {len(status.ready)}, Needs work: {len(status.needs_work)}")

   # Build any missing tiers (resolve + build in one call)
   status = indexer.ensure_indexed()

   # Get a checkpoint indexer for a single file
   ci = indexer.get_checkpoint_indexer("/path/to/traces/trace.pfw.gz")
   print(f"Max bytes: {ci.get_max_bytes()}")
   print(f"Num lines: {ci.get_num_lines()}")

Error Handling
~~~~~~~~~~~~~~

Operations raise typed exceptions so failures can be caught by category. Every
exception derives ``DFTUtilsError``, which derives the built-in ``RuntimeError``
(so ``except RuntimeError`` still catches everything):

.. code-block:: python

   from dftracer.utils import (
       TraceViewer,
       DFTUtilsError,        # base of all library exceptions
       DFTUtilsIOError,      # bad I/O / missing file
       DFTUtilsNotFoundError,
       DFTUtilsParseError,
       DFTUtilsQueryError,
   )

   try:
       TraceViewer(["missing.pfw.gz"]).agg("count").collect().collect()
   except DFTUtilsIOError as e:
       print(f"I/O failed: {e}")
   except DFTUtilsError as e:
       # Catches any other library error (parse, query, indexer, ...)
       print(f"dftracer error: {e}")

The full set is ``DFTUtilsError`` (base) plus ``DFTUtilsValueError``,
``DFTUtilsNotFoundError``, ``DFTUtilsIOError``, ``DFTUtilsParseError``,
``DFTUtilsCompressionError``, ``DFTUtilsQueryError``, ``DFTUtilsReaderError``,
``DFTUtilsIndexerError``, ``DFTUtilsPipelineError``, and
``DFTUtilsAggregationError``. See :doc:`cpp_api/runtime` for the
underlying C++ model.

Controlling Log Output
~~~~~~~~~~~~~~~~~~~~~~~~

The C++ logger is initialized when ``dftracer.utils`` is imported. Change the
verbosity from Python at any time, or set the ``DFTRACER_UTILS_LOG_LEVEL``
environment variable before running (see :doc:`getting-started/installation`):

.. code-block:: python

   import dftracer.utils as du

   du.set_log_level("debug")   # trace, debug, info (default), warn, error, off
   print(du.get_log_level())   # -> "debug"
   du.set_log_color("never")   # auto (default), always, never

The levels and color modes mirror the CLI ``--log-level`` flag and the
``DFTRACER_UTILS_LOG_*`` environment variables. An unknown level or color name
raises ``ValueError``.

C++ Quick Start
---------------

Building Parallel Pipelines
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create and execute parallel data processing tasks using coroutines:

.. code-block:: cpp

   #include <dftracer/utils/core/pipeline/pipeline.h>
   #include <dftracer/utils/core/pipeline/pipeline_config.h>
   #include <dftracer/utils/core/tasks/task.h>
   #include <dftracer/utils/core/coro/channel.h>
   #include <iostream>

   using namespace dftracer::utils;
   using namespace dftracer::utils::coro;

   int main() {
       // Configure executor
       auto config = PipelineConfig()
           .with_name("MyPipeline")
           .with_compute_threads(4);

       // Create channel for data streaming
       auto channel = make_channel<std::string>(100);

       // Producer task - reads data and sends through channel
       auto producer = make_task(
           [ch = channel->producer()](CoroScope& scope) mutable
               -> CoroTask<void> {
               auto guard = ch.guard();

               for (int i = 0; i < 100; i++) {
                   std::string data = "item-" + std::to_string(i);
                   co_await ch.send(std::move(data));
               }
               co_return;
           }, "Producer");

       // Consumer task - reads from channel and processes
       auto consumer = make_task([channel](CoroScope& scope) -> CoroTask<void> {
           while (auto item = co_await channel->receive()) {
               std::cout << "Processing: " << *item << std::endl;
           }
           co_return;
       }, "Consumer");

       // Execute pipeline
       Pipeline pipeline(config);
       pipeline.set_source({producer});
       pipeline.set_destination(consumer);
       pipeline.execute();

       return 0;
   }

Spawning Parallel Work
~~~~~~~~~~~~~~~~~~~~~~

Spawn multiple tasks to run in parallel:

.. code-block:: cpp

   #include <dftracer/utils/core/tasks/task.h>
   #include <dftracer/utils/core/tasks/coro_scope.h>

   auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
       std::vector<SpawnFuture<int>> futures;
       
       // Spawn 10 parallel workers
       for (int i = 0; i < 10; ++i) {
           auto future = scope.spawn([i](CoroScope& s) -> CoroTask<int> {
               // Each worker processes item i
               int result = compute(i);
               co_return result;
           });
           futures.push_back(std::move(future));
       }
       
       // Collect results
       int total = 0;
       for (auto& fut : futures) {
           total += co_await fut;
       }
       
       std::cout << "Total: " << total << std::endl;
       co_return;
   });

Using Async Generators for Streaming Data
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Process data lazily without materializing everything in memory:

.. code-block:: cpp

   #include <dftracer/utils/core/tasks/task.h>
   #include <dftracer/utils/core/tasks/coro_scope.h>
   #include <dftracer/utils/core/coro/async_generator.h>

   // Streaming data source (lazy evaluation)
   AsyncGenerator<std::string> read_lines_async(
       const std::string& file_path) {
       // Opens file, yields lines on-demand
       // ... streaming implementation ...
   }

   auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
       auto gen = read_lines_async("trace.pfw.gz");
       
       // Iterate asynchronously - each call to next() yields one item
       while (auto line = co_await gen.next()) {
           // Process line lazily as it's read
           parse_and_process(*line);
       }
       
       co_return;
   });

C Quick Start
-------------

Reading Trace Files
~~~~~~~~~~~~~~~~~~~

Using the C API for reading trace files. This header now lives under
``internal/`` and is not part of the public API.

.. code-block:: c

   #include <dftracer/utils/utilities/reader/internal/reader.h>
   #include <stdio.h>
   #include <stdlib.h>

   int main() {
       // Create reader
       dftu_reader_handle_t reader = dftu_reader_create(
           "trace.pfw.gz",
           "trace.pfw.gz.idx",
           1048576  // checkpoint_size
       );

       // Allocate buffer
       char *buffer = malloc(1024 * 1024);  // 1MB buffer

       // Read lines 1-100
       size_t bytes_written = 0;
       int result = dftu_reader_read_lines(
           reader,
           1, 100,              // start_line, end_line
           buffer,
           1024 * 1024,         // buffer_size
           &bytes_written
       );

       if (result == 0) {
           printf("%.*s", (int)bytes_written, buffer);
       }

       // Cleanup
       free(buffer);
       dftu_reader_destroy(reader);

       return 0;
   }

Working with Indexer
~~~~~~~~~~~~~~~~~~~~

Creating and using an indexer. This header now lives under ``internal/``
and is not part of the public API.

.. code-block:: c

   #include <dftracer/utils/utilities/indexer/internal/indexer.h>
   #include <stdio.h>

   int main() {
       // Create indexer
       dftu_indexer_handle_t indexer = dftu_indexer_create(
           "trace.pfw.gz",
           "trace.pfw.gz.idx",
           1048576,  // checkpoint_size
           0         // force_rebuild
       );

       // Build index if needed
       if (dftu_indexer_need_rebuild(indexer)) {
           printf("Building index...\n");
           dftu_indexer_build(indexer);
       }

       // Get index information
       uint64_t max_bytes = dftu_indexer_get_max_bytes(indexer);
       uint64_t num_lines = dftu_indexer_get_num_lines(indexer);

       printf("Max bytes: %llu\n", (unsigned long long)max_bytes);
       printf("Num lines: %llu\n", (unsigned long long)num_lines);

       // Cleanup
       dftu_indexer_destroy(indexer);

       return 0;
   }

Next Steps
----------

- Read :doc:`pipeline` for comprehensive coroutine pipeline guide
- Check :doc:`api/index` for detailed Python API documentation
- See :doc:`cpp_api/index` for C++ API reference
- Visit :doc:`utilities` for built-in composable components
- Read :doc:`developers` for development guidelines

Key Resources:

- **Pipeline patterns**: :doc:`pipeline` covers CoroScope, channels, fan-out/fan-in, async generators
- **CLI tools**: :doc:`cli` lists all available command-line utilities
- **Python bindings**: Use ``from dftracer.utils import TraceViewer, Indexer`` for Python scripts
- **C++ integration**: Link ``dftracer-utils`` library and include headers from ``include/dftracer/utils/``
