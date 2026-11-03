:description: Overview of the C++ utility building blocks for indexing and statistics.

Utilities
=========

dftracer-utils provides composable utilities for trace file processing. The command line tools and the Python API cover the file I/O, replay and DLIO config generation built on them: see :doc:`/cli`.

.. toctree::
   :maxdepth: 2
   :caption: Available Utilities:

   utilities/indexer

Overview
--------

The generated API reference is under :doc:`cpp_api/index`.

- **Indexer** - checkpoint and bloom-filter indexing for fast queries.
- **Statistics** - sketches and histograms used by indexing and aggregation.

Statistics
----------

Statistics collection for trace analysis:

- **DDSketch**: Deterministic, merge-order-independent percentile estimation with bounded relative error
- **Log2Histogram**: Fixed 65-bin logarithmic histogram for duration and size distributions
- **Chunk statistics**: Per-chunk event tracking with online variance calculation and per-name duration sketches

These are used in indexing and aggregation pipelines to compute event distributions and percentiles efficiently.

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

See :doc:`cpp_api/utilities` for the full generated C++ reference.
