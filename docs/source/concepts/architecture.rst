:description: How the codebase is layered into five libraries and the path a query takes from a .pfw.gz file to a result in C++, C, or Python.

Architecture
============

What this explains: how the codebase is layered, and the path a query takes
from a ``.pfw.gz`` trace file on disk to a result the caller can use, in C++,
C, or Python.

Eight libraries, one dependency direction
-------------------------------------------

The C++ build produces eight layered libraries, each as both a shared and a
static variant (``src/CMakeLists.txt``), aggregated behind
``dftracer::utils``:

- ``dftracer_utils_runtime`` is the async runtime: coroutines, the task
  executor and scheduler, the io backend, the pipeline machinery, and shared
  primitives (string interning, sharded maps, the error type). It knows
  nothing about traces.
- ``dftracer_utils_store`` is the key-value store engine: data files, the
  writer and its committer, and the snapshot that reads them. It links only
  the runtime and knows nothing about traces.
- ``dftracer_utils_fileio`` is the file format layer: gzip readers and
  writers, line streams, the parallel writers and hashing. It links the
  runtime and the JSON library and knows nothing about traces.
- ``dftracer_utils_json`` is the simdjson-backed JSON layer: parsing and
  the JSON value type.
- ``dftracer_utils_yaml`` is the YAML layer: a reader with a line index for
  block YAML and a flow parser that keeps a flat tape of 12-byte entries and
  no tree, reports errors without exceptions, refuses anchors, aliases, tags,
  complex keys, directives and null keys, and reads a text too large for
  memory in chunks (``Records``), and a block-style writer. It parses and writes
  buffers and does no I/O. It links only memory, dstd and common, and the trace
  and domains libraries link it.
- ``dftracer_utils_duql`` is the duql filter language: the predicate IR, the string
  codec, and the evaluator.
- ``dftracer_utils_dataframe`` is the columnar SIMD engine: ``Series`` /
  ``DataFrame``, the Highway kernels, and the Arrow bridge. It also holds the
  duql parts that call the dataframe (``vectorize``, ``lookup``, ``overlap``
  and ``group_fold``) in ``dataframe/query/``, so it links ``dftracer_utils_duql``.
- ``dftracer_utils_trace`` is the trace layer: the index (build, plan, cache,
  schemas), the views and folds, the trace reader and the Arrow interchange.
  It links every library above it and never includes ``dftracer_utils_domains``.
- ``dftracer_utils_domains`` depends on the libraries above and holds the
  work done with traces: analysis (comparator, statistics, provenance),
  workload (genesis, DLIO, replay, fake traces), the server and the plugin host.

Each library keeps its sources in one folder of the same name under
``src/dftracer/utils/``, and the namespace follows the folder: the comparator
sits in ``domains/analysis/comparator/`` and in the namespace
``dftracer::utils::domains::analysis::comparator``. The CTest check
``library_folders`` fails when a source sits outside the folder of its library.

The dependency runs one way, down this list. Nothing in ``runtime`` knows what
a DFTracer event looks like; nothing in ``trace`` or ``domains`` reimplements scheduling,
I/O, JSON parsing, or the columnar kernels. This split is why the coroutine
runtime can be reasoned about (and tested) on its own, and why domain code
never has to touch a raw file descriptor or worry about which io backend is
active.

.. mermaid::

   graph TB
       Runtime["dftracer_utils_runtime<br/>runtime, coro, io, pipeline"]
       Store["dftracer_utils_store<br/>key-value store engine"]
       Io["dftracer_utils_fileio<br/>gzip, line streams, writers"]
       Json["dftracer_utils_json<br/>simdjson-backed parsing"]
       Yaml["dftracer_utils_yaml<br/>YAML reader and writer"]
       Query["dftracer_utils_duql<br/>predicate IR, evaluator"]
       DF["dftracer_utils_dataframe<br/>Series/DataFrame, SIMD kernels"]
       Trace["dftracer_utils_trace<br/>index, views, trace reader"]
       Util["dftracer_utils_domains<br/>analysis, workload, server"]
       Store --> Runtime
       Io --> Runtime
       Io --> Json
       Json --> Runtime
       Query --> Runtime
       Query --> Json
       DF --> Runtime
       DF --> Query
       Trace --> Runtime
       Trace --> Store
       Trace --> Io
       Trace --> Json
       Trace --> Yaml
       Trace --> Query
       Trace --> DF
       Util --> Trace
       Util --> Yaml

The domain logic itself is grouped by what it does rather than by a shared
base class: readers and the fused scan under ``trace/``, the index under
``trace/index/`` (in layers: ``store`` for index storage, ``gzip`` for
checkpoints, ``extensions`` for blooms, dimension stats and stored row sets,
``schemas/dft`` for the dftracer-specific statistics and aggregation,
then ``build``, ``plan`` for pruning and ``cache`` for rollups; a CTest check
keeps each layer from including a higher one), comparison under
``domains/analysis/comparator``, the columnar engine under
``dataframe/``, the duql filter language under ``duql/``, and the plugin C ABI under
``plugins/`` in ``include/`` and the plugin host under ``domains/plugins/``. See :doc:`fused-scan`, :doc:`indexing-and-pushdown`, and
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
   per-file index in a ``.dftindex`` store: bloom filters over string
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

- :doc:`async-runtime` for what ``dftracer_utils_runtime`` actually runs.
- :doc:`fused-scan` for how one pass serves many analytics.
- :doc:`indexing-and-pushdown` for the bloom/stats/checkpoint index and pruning.
- :doc:`dataframe-model` for the columnar result type.
- :doc:`../getting-started/index` for the concrete build and install steps.
