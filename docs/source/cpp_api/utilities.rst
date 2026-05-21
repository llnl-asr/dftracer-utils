Utilities API
=============

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/index>`.

Composable processing utilities. For usage examples, see :doc:`/utilities`.

Base Classes
------------

All utilities inherit from ``UtilityBase``, which provides tag introspection,
context management, and naming. Two derived templates define the ``process()``
contract:

- ``Utility<I, O, Tags...>`` — materialized output: ``process()`` returns
  ``CoroTask<O>``
- ``StreamingUtility<I, Batch, Tags...>`` — streaming output: ``process()``
  returns ``AsyncGenerator<Batch>``

.. mermaid::

   classDiagram
       class UtilityBase~I, Tags~ {
           +has_tag~Tag~() bool
           +get_tag~Tag~() Tag
           +get_name() string
           +set_name(string)
           #context() CoroScope
       }
       class Utility~I, O, Tags~ {
           +process(I) CoroTask~O~
       }
       class StreamingUtility~I, Batch, Tags~ {
           +process(I) AsyncGenerator~Batch~
       }
       UtilityBase <|-- Utility
       UtilityBase <|-- StreamingUtility

UtilityBase
~~~~~~~~~~~

Shared base for all utilities. Provides tag introspection (``has_tag<>``,
``get_tag<>``), context management (``context()`` for ``NeedsContext``
utilities), and name/type signature generation.

Utility (Materialized)
~~~~~~~~~~~~~~~~~~~~~~

For utilities that compute a single result. ``process()`` returns
``CoroTask<O>`` — the caller ``co_await``\ s the result.

StreamingUtility
~~~~~~~~~~~~~~~~

For utilities that yield results incrementally. ``process()`` returns
``AsyncGenerator<Batch>`` — the caller iterates with
``co_await gen.next()``.

Batch structs typically provide a ``to_arrow()`` method for Arrow
conversion (e.g., ``ViewReaderBatch::to_arrow()``,
``AggregationBatch::to_arrow()``).

.. code-block:: cpp

   // Consuming a StreamingUtility
   ViewReaderUtility reader;
   auto gen = reader.process(input);
   while (auto batch = co_await gen.next()) {
       // Use C++ data directly
       for (const auto& event : batch->events) { ... }

       // Or convert to Arrow
       auto arrow = batch->to_arrow();
   }

Module Reference
----------------

Each module below has detailed class documentation in the API Reference:

.. list-table::
   :header-rows: 1
   :widths: 30 50 20

   * - Module
     - Description
     - API Reference
   * - Call Tree
     - Build hierarchical call trees from DFTracer traces
     - :doc:`api/call_tree`
   * - Filesystem
     - Directory scanning utilities
     - :doc:`api/utilities/filesystem`
   * - File I/O
     - File reading, writing, chunk writing, async line generators
     - :doc:`api/utilities/fileio/index`
   * - Compression
     - Streaming zlib compression (GZIP, ZLIB, DEFLATE)
     - :doc:`api/utilities/composites/index`
   * - Text
     - Line splitting, filtering, text processing
     - :doc:`api/utilities/text`
   * - Hash
     - FNV1a, std::hash, MT-safe hasher utilities
     - :doc:`api/utilities/hash`
   * - Statistics
     - DDSketch (percentiles), Log2Histogram (distributions)
     - :doc:`api/utilities/composites/dft/statistics`
   * - Indexer
     - Bloom filter indexes, manifests, provenance tracking
     - :doc:`api/utilities/indexer`
   * - Reader
     - Streaming trace file reader with index support
     - :doc:`api/utilities/reader`
   * - Views
     - View definitions and predicate-based event filtering
     - :doc:`api/utilities/composites/dft/views`
   * - Aggregation
     - Time-bucketed aggregation pipeline with Arrow output
     - :doc:`api/utilities/composites/dft/aggregators`
   * - Comparator
     - Baseline vs variant trace comparison with Cohen's d
     - :doc:`api/utilities/composites/dft/comparator`
   * - Reorganization
     - Parallel event routing and chunked output
     - :doc:`api/utilities/composites/dft/reorganize`
   * - Replay
     - Replay I/O operations from traces
     - :doc:`api/utilities/replay`
