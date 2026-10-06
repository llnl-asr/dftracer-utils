:description: Overview of the composable C++ utility building blocks - file I/O, filesystem, hashing, indexer, reader, DLIO, replay - used standalone or in pipelines.

Utilities
=========

dftracer-utils provides a collection of composable utilities for trace file processing. These utilities can be used standalone or combined into pipelines.

.. toctree::
   :maxdepth: 2
   :caption: Available Utilities:

   utilities/filesystem
   utilities/fileio
   utilities/replay
   utilities/hash
   utilities/indexer
   utilities/reader
   utilities/dlio

Overview
--------

The library groups its C++ building blocks by domain. Each group has generated
API reference under :doc:`cpp_api/index`; the pages below add prose and
examples for the areas most often used directly.

- **File I/O** - synchronous readers/writers plus async line and byte
  generators for gzip-compressed trace files.
- **Filesystem** - directory scanning and trace-file discovery.
- **Hash** - incremental FNV-1a 64-bit hashing.
- **Indexer** - checkpoint and bloom-filter indexing for fast queries.
- **Reader** - trace-file reading and line processing.
- **DLIO** - the trace-to-DLIO-config pipeline (barrier simulator, optimizer,
  YAML emit).

File I/O
--------

The ``fileio`` utilities support both synchronous and asynchronous file operations:

- **Gzip line writer**: ``GzipLineWriter`` (and the blocking ``GzipLineWriterBlocking``) is the single gzip writer. Members end at line ends and compress in parallel
- **Synchronous readers**: Full in-memory or streaming chunk-based reading of plain files
- **Async generators**: Non-blocking line/byte generators over gzip-compressed (``.pfw.gz``) archives, using ``co_await`` and coroutines
- **Indexed and streaming access**: indexed random access via a ``.dftindex`` sidecar, or single-pass streaming decompression when no index is present
- **Streaming decompression**: On-the-fly decompression of .gz files without building indexes

See :doc:`/utilities/fileio` for detailed usage.

Statistics
----------

Enhanced statistics collection and distribution fitting for trace analysis:

- **DDSketch**: Deterministic, merge-order-independent percentile estimation with bounded relative error
- **Log2Histogram**: Fixed 65-bin logarithmic histogram for duration and size distributions
- **Statistic**: Min/max/mean/count accumulator that optionally delegates to an attached DDSketch for quantile queries
- **Distributions**: MLE fitting + KS / BIC scoring for Normal, Lognormal, Gamma, Exponential, Weibull; sampler factory backed by ``<random>`` and `Boost.Math standalone <https://www.boost.org/doc/libs/release/libs/math/doc/html/math_toolkit/standalone.html>`_
- **Mixture**: Univariate Gaussian Mixture EM (K=2, K=3) with log-sum-exp responsibilities and BIC-based selection across single + mixture models
- **Chunk statistics**: Per-chunk event tracking with online variance calculation and per-name duration sketches

These are used in indexing and aggregation pipelines to compute event distributions and percentiles efficiently, and by the DLIO config generator to fit per-component timing distributions.

DLIO Config Generation
----------------------

End-to-end pipeline that converts a directory of raw DFTracer logs into a DLIO
training-loop YAML configuration:

- **trace_loader**: pulls the ``AGGREGATION`` column family (re-attaches the
  merge operator at open time) and synthesizes per-rank sample arrays from
  per-(pid, time_bucket) entries.
- **BarrierSimulator**: simulates one DLIO training run across the captured
  ranks/steps, scoring an end-to-end duration, rank variance, and ``fetch.block``
  CDF similarity against the empirical trace.
- **optimizer**: sequential momentum loop refining the ``max_bound`` percentile
  on the fitted sampler to minimize simulator E2E error.
- **yaml_emit**: renders single distributions or Gaussian mixtures into the
  DLIO ``train.computation_time`` / ``reader.preprocess_time`` schema.

See :doc:`/utilities/dlio` for the API and ``dftracer_gen_dlio_config`` in
:doc:`/cli` for the user-facing binary.

Indexing
--------

Advanced indexing utilities for fast trace queries:

- **Bloom filter cache**: Thread-safe bounded cache for deserialized bloom filters with file-level and chunk-level keys
- **Chunk statistics**: Per-chunk aggregates including event counts, timestamp ranges, and duration distributions
- **Chunk pruning**: ``ChunkPruner`` evaluates a compiled ``duql::Query`` against a file's bloom filters and chunk statistics to return the candidate checkpoint list, without decompressing chunks that cannot match

Views
-----

Query views on DFTracer traces run the compiled query against the index
before touching event data:

- **ChunkPruner** (``index/plan/chunk_pruner.h``) - takes an index path, file path, and ``Query``, and returns the subset of checkpoints that may match plus a ``file_may_match`` short-circuit
- The reader's duql filter (see :doc:`/utilities/reader`) compiles AND-of-EQ predicates that ``ChunkPruner`` evaluates against bloom filters and chunk statistics

See :doc:`cpp_api/utilities` for the full generated C++ reference.
