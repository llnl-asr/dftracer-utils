:description: Task-oriented how-tos grouped by area, with co-equal C++ and Python examples, for readers past the getting-started and tutorial material.

Guides
======

Task-oriented how-tos for readers who have finished :doc:`Get started
<../getting-started/index>` and the :doc:`Tutorials <../tutorials/index>`. Each
guide starts from a goal and ends at the solved state. C++ and Python are
co-equal: every task that both languages support shows both, with a C tab where
the ABI is the intended interface.

The guides are grouped by the part of dftracer-utils you are working with.

Start here
----------

Cross-cutting guides that span the whole library: pick an API, follow one job
end to end, and find the fix when something goes wrong.

.. toctree::
   :maxdepth: 1

   choosing-an-api
   end-to-end
   troubleshooting

Reading and querying traces
---------------------------

Open trace data and narrow it down to what you care about.

.. toctree::
   :maxdepth: 1

   ../trace-viewer
   core/query-dsl
   core/indexing
   core/c-abi

DataFrames and Series
---------------------

The columnar engine: a ``DataFrame`` is a set of typed ``Series``, computed with
SIMD kernels and bridged zero-copy to Arrow, pandas, NumPy, and polars only at
the edge.

.. toctree::
   :maxdepth: 1

   data/dataframe
   data/lazyframe
   data/series
   data/pandas-polars
   data/ingest
   core/columnar-ops
   data/joins
   data/time-windows
   data/reshape
   ../columnar-engine

Analysis
--------

Compute over traces: aggregate, summarize, compare runs, and get results out.

.. toctree::
   :maxdepth: 1

   analysis/views
   analysis/sessions
   analysis/aggregation
   analysis/statistics
   analysis/comparison
   analysis/diagnosing-slow-queries
   analysis/export

Concurrency and the runtime
---------------------------

Build async workflows on the coroutine runtime. See
:doc:`../concepts/async-runtime` and :doc:`../concepts/coroutine-caveats` for the
model and its rules.

.. toctree::
   :maxdepth: 1

   pipelines/patterns
   runtime/task-graphs
   runtime/memory-budget
   runtime/performance
   ../pipeline

I/O and compression
-------------------

Read and write trace bytes: streaming reads, gzip, and splitting.

.. toctree::
   :maxdepth: 1

   io/compression
   io/reading-files

Extending the engine
--------------------

Add your own compiled or JIT-compiled analytics that ride the one fused scan.

.. toctree::
   :maxdepth: 1

   ../plugins
   plugins/inter-plugin-comms
   plugins/compose-ops
   ../jit

Serving and tools
-----------------

Serve the index over HTTP and reach for the standalone analysis tools.

.. toctree::
   :maxdepth: 1

   serving/http-server
   serving/viz-api
   ../server
   tools/dlio-config
   tools/replay
   tools/logging
   tools/dfanalyzer
   ../utilities

Scaling out
-----------

Run across ranks and machines.

.. toctree::
   :maxdepth: 1

   scale/mpi
   scale/distributed-index
   scale/distributed-aggregation
