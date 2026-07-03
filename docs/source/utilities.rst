Utilities
=========

dftracer-utils provides a collection of composable utilities for trace file processing. These utilities can be used standalone or combined into pipelines.

.. toctree::
   :maxdepth: 2
   :caption: Available Utilities:

   utilities/filesystem
   utilities/fileio
   utilities/compression
   utilities/text
   utilities/composites
   utilities/replay
   utilities/hash
   utilities/indexer
   utilities/reader
   utilities/common
   utilities/dlio
   call-tree

Overview
--------

Utilities follow a consistent pattern:

- **Input types**: Configuration structs with fluent builder API
- **Output types**: Result structs with success status and data
- **process() method**: Main entry point that transforms input to output
- **Tags**: Compile-time markers like ``NeedsContext`` that opt into
  cross-cutting features (e.g. CoroScope access)

.. mermaid::

   graph TB
       subgraph Base["Utility Pattern"]
           Utility["Utility&lt;I, O, Tags...&gt;<br/>process(I) -> CoroTask&lt;O&gt;"]
       end

       subgraph Categories["Utility Categories"]
           FileIO["File I/O<br/>FileReader, StreamingReader"]
           Compression["Compression<br/>Compressor, Decompressor"]
           Text["Text<br/>LineSplitter, LineFilter"]
           Hash["Hash<br/>FNV1a, Std, MurmurHash3"]
           Indexer["Indexer<br/>Checkpoint, BloomFilter"]
           Reader["Reader<br/>Stream, LineProcessor"]
           Common["Common<br/>JSON, DDSketch, Statistic, Distributions, Mixture"]
           Composites["Composites<br/>DFTracer-specific pipelines"]
           Dlio["DLIO<br/>BarrierSimulator, TraceLoader, Optimizer, YAML emit"]
       end

       Utility --> FileIO
       Utility --> Compression
       Utility --> Text
       Utility --> Hash
       Utility --> Indexer
       Utility --> Reader
       Utility --> Common
       Utility --> Composites
       Utility --> Dlio

File I/O
--------

The ``fileio`` utilities support both synchronous and asynchronous file operations:

- **Synchronous readers**: Full in-memory or streaming chunk-based reading
- **Async generators**: Non-blocking line/byte generators using ``co_await`` and coroutines
- **Plain and indexed files**: Support for both raw text files and compressed archives with sidecar indexes
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
- **Predicate filtering**: Efficient multi-dimensional filtering for view queries on dimensions like time range and duration bounds

Views and Predicates
--------------------

Query views on DFTracer traces with multi-dimensional filtering:

- **PredicateFilter**: Efficiently filters events by dimension sets, time ranges, and duration bounds
- **Supports multiple predicates**: Match events against OR'd lists of predicates

See :doc:`cpp_api/utilities` for the full API reference.
