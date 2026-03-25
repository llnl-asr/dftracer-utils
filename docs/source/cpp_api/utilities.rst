Utilities API
=============

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

.. doxygenclass:: dftracer::utils::utilities::UtilityBase
   :project: dftracer-utils
   :members:
   :undoc-members:

Utility (Materialized)
~~~~~~~~~~~~~~~~~~~~~~

For utilities that compute a single result. ``process()`` returns
``CoroTask<O>`` — the caller ``co_await``\ s the result.

.. doxygenclass:: dftracer::utils::utilities::Utility
   :project: dftracer-utils
   :members:
   :undoc-members:

StreamingUtility
~~~~~~~~~~~~~~~~

For utilities that yield results incrementally. ``process()`` returns
``AsyncGenerator<Batch>`` — the caller iterates with
``co_await gen.next()``.

Batch structs typically provide a ``to_arrow()`` method for Arrow
conversion (e.g., ``ViewReaderBatch::to_arrow()``,
``AggregationBatch::to_arrow()``).

.. doxygenclass:: dftracer::utils::utilities::StreamingUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

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

Tags
~~~~

.. doxygenstruct:: dftracer::utils::utilities::tags::Parallelizable
   :project: dftracer-utils

.. doxygenstruct:: dftracer::utils::utilities::tags::NeedsContext
   :project: dftracer-utils

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
~~~~~~~~~~~~~~~~

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
~~~~~~~~

Deterministic percentile estimation with bounded relative error, merging is commutative.

.. doxygenclass:: dftracer::utils::utilities::common::statistics::DDSketch
   :project: dftracer-utils
   :members:
   :undoc-members:

Log2Histogram
~~~~~~~~~~~~~

Fixed 65-bin logarithmic histogram for compact distribution representation.

.. doxygenclass:: dftracer::utils::utilities::common::statistics::Log2Histogram
   :project: dftracer-utils
   :members:
   :undoc-members:

Indexing & Aggregation
----------------------

Indexing utilities for efficient trace querying and aggregation.

Chunk Statistics & Bloom Filter Cache
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

See :doc:`dft_indexing` for full documentation of ``ChunkStatistics``,
``BloomFilter``, ``BloomFilterCache``, and the complete indexing pipeline.

Views
-----

View definition and event reading with query-based filtering.

ViewDefinition
~~~~~~~~~~~~~~

Defines a named view with an optional ``Query`` for event filtering.
Preset views (``io_view()``, ``compute_view()``, ``dlio_view()``) provide
common filter configurations. Serializable to/from JSON.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::views::ViewDefinition
   :project: dftracer-utils
   :members:
   :undoc-members:

ViewReaderUtility
~~~~~~~~~~~~~~~~~

``StreamingUtility`` that reads events from a trace file. When
``ViewReaderInput.query`` is set, events are filtered per-event via
``Query::evaluate()``. Without a query, all events are yielded.

Yields ``ViewReaderBatch`` objects with ``to_arrow()`` for Arrow conversion.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::views::ViewReaderInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::views::ViewReaderBatch
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::views::ViewReaderUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

Comparator
----------

Hierarchical comparison of trace metrics between baseline and variant
runs. Aggregates events into time-bucketed windows, computes per-window
max across processes, then mean +/- stdev across windows, and classifies
deltas using Cohen's d (NEGLIGIBLE / SMALL / MEDIUM / LARGE).

ComparisonConfig
~~~~~~~~~~~~~~~~

Configuration for the comparison pipeline. Can be constructed from CLI
arguments (``from_cli()``) or loaded from a JSON file
(``from_json_file()``). Supports hierarchical node trees with query
inheritance and per-node percentile overrides.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::ComparisonDefaults
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::ComparisonNode
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::ComparisonConfig
   :project: dftracer-utils
   :members:
   :undoc-members:

ComparisonResult
~~~~~~~~~~~~~~~~

Result types for the comparison pipeline. ``CollapsedMetrics`` holds
per-window-max, cross-window mean/stdev values for a single metric
group. ``ComparisonOutput`` is the top-level result containing the
hierarchical tree of comparison nodes and metadata.

.. doxygenenum:: dftracer::utils::utilities::composites::dft::comparator::Significance
   :project: dftracer-utils

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::MetricComparison
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::GroupComparison
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::NodeResult
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::TraceMetadata
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::CollapsedMetrics
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::ComparisonOutput
   :project: dftracer-utils
   :members:
   :undoc-members:

ComparisonUtility
~~~~~~~~~~~~~~~~~

Joins baseline and variant aggregation outputs, builds the hierarchical
comparison tree (root -> categories -> operations), and computes deltas
with significance classification.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::ComparisonVisitorPair
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::ComparisonUtilityInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::ComparisonUtilityOutput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::comparator::ComparisonUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

TreeTableFormatter
~~~~~~~~~~~~~~~~~~

Renders ``ComparisonOutput`` as an ASCII tree table (``render()``) or
JSON (``render_json()``). The table output uses dynamic column alignment
with UTF-8 display width awareness.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::comparator::FormatterOptions
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::comparator::TreeTableFormatter
   :project: dftracer-utils
   :members:
   :undoc-members:
