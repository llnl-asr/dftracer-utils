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

These are used in indexing and aggregation pipelines to compute event distributions and percentiles efficiently.

Indexing
--------

Advanced indexing utilities for fast trace queries:

- **Chunk pruning**: a compiled ``duql::Query`` is evaluated against a file's bloom filters and chunk statistics to return the candidate checkpoint list, without decompressing chunks that cannot match

See :doc:`cpp_api/utilities` for the full generated C++ reference.
