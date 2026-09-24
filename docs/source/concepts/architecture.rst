:description: How the codebase is layered into five libraries and the path a query takes from a .pfw.gz file to a result in C++, C, or Python.

Architecture
============

What this explains: how the codebase is layered, and the path a query takes
from a ``.pfw.gz`` trace file on disk to a result the caller can use, in C++,
C, or Python.

Five libraries, one dependency direction
-------------------------------------------

The C++ build produces five layered libraries, each as both a shared and a
static variant (``src/CMakeLists.txt``), aggregated behind
``dftracer::utils``:

- ``dftracer_utils_core`` is the async runtime: coroutines, the task
  executor and scheduler, the io backend, the pipeline machinery, RocksDB
  wrappers, and shared primitives (string interning, object pools, sharded
  maps, the error type). It knows nothing about traces.
- ``dftracer_utils_json`` is the simdjson-backed JSON layer: parsing and
  the JSON value type.
- ``dftracer_utils_query`` is the query DSL: the predicate IR, the string
  codec, and the evaluator.
- ``dftracer_utils_dataframe`` is the columnar SIMD engine: ``Series`` /
  ``DataFrame``, the Highway kernels, and the Arrow bridge.
- ``dftracer_utils_utilities`` depends on the four libraries above and
  holds the top domain logic: trace reading, indexing, aggregation,
  comparison, statistics, plugins, and replay.

The dependency runs one way, down this list. Nothing in ``core`` knows what
a DFTracer event looks like; nothing in ``utilities`` reimplements scheduling,
I/O, JSON parsing, or the columnar kernels. This split is why the coroutine
runtime can be reasoned about (and tested) on its own, and why domain code
never has to touch a raw file descriptor or worry about which io backend is
active.

.. mermaid::

   graph TB
       Core["dftracer_utils_core<br/>runtime, coro, io, pipeline"]
       Json["dftracer_utils_json<br/>simdjson-backed parsing"]
       Query["dftracer_utils_query<br/>predicate IR, evaluator"]
       DF["dftracer_utils_dataframe<br/>Series/DataFrame, SIMD kernels"]
       Util["dftracer_utils_utilities<br/>trace, indexer, plugins"]
       Json --> Core
       Query --> Core
       Query --> Json
       DF --> Core
       Util --> Core
       Util --> Json
       Util --> Query
       Util --> DF

The domain logic itself is grouped by what it does rather than by a shared
base class: readers and the fused scan under ``trace/``, the index under
``index/`` (in layers: ``store`` for RocksDB storage, ``gzip`` for
checkpoints, ``extensions`` for blooms, dimension stats and dictionaries,
``schemas/dft`` for the dftracer-specific statistics and aggregation,
then ``build``, ``plan`` for pruning and ``cache`` for rollups; a CTest check
keeps each layer from including a higher one), comparison under
``trace/comparator``, the columnar engine under
``dataframe/``, the query DSL under ``query/``, and the plugin C ABI under
``plugins/``. See :doc:`fused-scan`, :doc:`indexing-and-pushdown`, and
:doc:`dataframe-model` for each of those in turn.

From trace file to result
--------------------------

A query over a trace directory moves through the same handful of stages
regardless of which entry point started it (a CLI binary, the Python
``TraceViewer``, or a C++ ``View``):

.. mermaid::

   graph LR
       Files[".pfw.gz files"] --> Index["index<br/>(bloom + stats + checkpoints)"]
       Index --> Prune["predicate pushdown<br/>(prune chunks)"]
       Prune --> Scan["fused scan<br/>(parse once, fold N ways)"]
       Scan --> DF["DataFrame<br/>(columnar batch)"]
       DF --> Out["Arrow / pandas / CLI / plugin"]

1. **Index.** ``dftracer_index`` (or an equivalent library call) builds a
   per-file index in a ``.dftindex`` RocksDB store: bloom filters over string
   fields, min/max statistics per chunk, and checkpoints that mark where a
   compressed member starts. See :doc:`indexing-and-pushdown`.
2. **Prune.** Given a query, the chunk pruner consults the index to decide
   which chunks of which files can possibly match, before any decompression
   happens. A query that touches a narrow slice of a large trace set never
   decompresses the rest.
3. **Fused scan.** The surviving chunks are decompressed and parsed once. Every
   analytic that wants a look at the events (built-in aggregation, statistics,
   comparison, or a loaded plugin) rides that single traversal instead of
   re-reading the trace per analytic. See :doc:`fused-scan`.
4. **DataFrame.** The scan folds its output into the native columnar engine
   (``dataframe::DataFrame``), which is what CLI tools print, what the HTTP
   server serves, and what crosses zero-copy into Arrow, pandas, or NumPy at
   the Python boundary. See :doc:`dataframe-model`.

Where C++, C, and Python sit
------------------------------

The engine itself is C++20. Two thinner layers sit on top of it, not
alongside it:

- The **plugin C ABI** (``plugins/abi.h``) is a small set of C structs and
  function pointers. A plugin links nothing from the internal C++ library, so
  it stays compatible across releases; it sees interned string ids and
  Arrow-shaped batches, not internal C++ types. See the
  :doc:`../plugins` guide.
- The **Python extension** (``dftracer_utils_ext``) is a single CPython
  extension, raw C-API, that exposes the runtime, the DataFrame engine, the
  indexer, and ``TraceViewer``. Data crosses the boundary through the Arrow C
  Data Interface with no copy, the same mechanism the plugin ABI uses for
  batches.

Both layers exist so that the compute stays in one place. A Python caller and
a compiled plugin end up running the same C++ kernels over the same columnar
batches; neither reimplements parsing, indexing, or the SIMD kernels on its
own side of the boundary.

See also
--------

- :doc:`async-runtime` for what ``dftracer_utils_core`` actually runs.
- :doc:`fused-scan` for how one pass serves many analytics.
- :doc:`indexing-and-pushdown` for the bloom/stats/checkpoint index and pruning.
- :doc:`dataframe-model` for the columnar result type.
- :doc:`../getting-started/index` for the concrete build and install steps.
