Utilities API
=============

Composable processing utilities. For usage examples, see :doc:`/utilities`.

Call Tree
---------

Build hierarchical call trees from DFTracer traces. See :doc:`/call-tree` for usage guide.

.. doxygenclass:: dftracer::utils::call_tree::CallTree
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::call_tree::CallTreeNodeInfo
   :project: dftracer-utils
   :members:

.. doxygenstruct:: dftracer::utils::call_tree::CallTreeStats
   :project: dftracer-utils
   :members:

Filesystem
----------

Directory scanning utilities.

.. doxygenclass:: dftracer::utils::utilities::filesystem::DirectoryScannerUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::filesystem::PatternDirectoryScannerUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

File I/O
--------

File reading and writing utilities.

.. doxygenclass:: dftracer::utils::utilities::fileio::FileReaderUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::fileio::BinaryFileReaderUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::fileio::StreamingFileReaderUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::fileio::StreamingFileWriterUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::fileio::lines::StreamingLineReader
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::fileio::lines::LineRange
   :project: dftracer-utils
   :members:
   :undoc-members:

Async Generators
^^^^^^^^^^^^^^^^

Non-blocking line and byte generators for coroutine-based pipelines.

.. doxygenfunction:: dftracer::utils::utilities::fileio::lines::sources::async_plain_file_lines
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::utilities::fileio::lines::sources::async_plain_file_bytes
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::utilities::fileio::lines::sources::async_indexed_file_lines
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::utilities::fileio::lines::sources::async_indexed_file_bytes
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::utilities::fileio::lines::sources::async_streaming_gz_lines
   :project: dftracer-utils

Compression
-----------

Zlib compression utilities supporting GZIP, ZLIB, DEFLATE formats.

.. doxygenclass:: dftracer::utils::utilities::compression::zlib::CompressorUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::compression::zlib::DecompressorUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::compression::zlib::StreamingCompressorUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::compression::zlib::StreamingDecompressorUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

Text
----

Text processing utilities.

.. doxygenclass:: dftracer::utils::utilities::text::LineSplitterUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::text::LineFilterUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::text::MultiLinesFilterUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

Statistics
----------

Statistical data structures for percentile estimation and distribution tracking.

DDSketch
^^^^^^^^

Deterministic percentile estimation with bounded relative error, merging is commutative.

.. doxygenclass:: dftracer::utils::utilities::common::statistics::DDSketch
   :project: dftracer-utils
   :members:
   :undoc-members:

Log2Histogram
^^^^^^^^^^^^^

Fixed 65-bin logarithmic histogram for compact distribution representation.

.. doxygenclass:: dftracer::utils::utilities::common::statistics::Log2Histogram
   :project: dftracer-utils
   :members:
   :undoc-members:

Indexing & Aggregation
----------------------

Indexing utilities for efficient trace querying and aggregation.

Chunk Statistics
^^^^^^^^^^^^^^^^

Per-chunk event statistics including counts, timestamp ranges, and duration distributions.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::ChunkStatistics
   :project: dftracer-utils
   :members:

Bloom Filter Cache
^^^^^^^^^^^^^^^^^^

Thread-safe bounded cache for deserialized bloom filters.

.. doxygenclass:: dftracer::utils::utilities::composites::dft::indexing::BloomFilterCache
   :project: dftracer-utils
   :members:
   :undoc-members:

Views & Predicates
------------------

View query support with multi-dimensional filtering.

Predicate Filter
^^^^^^^^^^^^^^^^

Efficiently filters events by dimension sets, time ranges, and duration bounds.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::views::PredicateFilter
   :project: dftracer-utils
   :members:

.. doxygenfunction:: dftracer::utils::utilities::composites::dft::views::build_predicate_filter
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::utilities::composites::dft::views::matches_predicate
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::utilities::composites::dft::views::matches_any_predicate
   :project: dftracer-utils
