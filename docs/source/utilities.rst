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
   call-tree

Overview
--------

Utilities follow a consistent pattern:

- **Input types**: Configuration structs with fluent builder API
- **Output types**: Result structs with success status and data
- **process() method**: Main entry point that transforms input to output
- **Tags**: Metadata like ``Parallelizable`` for thread-safe utilities

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

Enhanced statistics collection for trace analysis:

- **DDSketch**: Deterministic, merge-order-independent percentile estimation with bounded relative error
- **Log2Histogram**: Fixed 65-bin logarithmic histogram for duration and size distributions
- **Chunk statistics**: Per-chunk event tracking with online variance calculation and per-name duration sketches

These are used in indexing and aggregation pipelines to compute event distributions and percentiles efficiently.

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
