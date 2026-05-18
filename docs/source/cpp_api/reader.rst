Reader Components
=================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/utilities/reader>`.

Trace file reading functionality.
All classes are in the ``dftracer::utils::utilities::reader`` namespace.

.. mermaid:: ../_generated/reader.mmd

Overview
--------

The reader module provides streaming access to compressed trace files,
supporting both sequential and indexed random access modes. When an ``.idx``
sidecar file exists, the reader automatically uses checkpoint-based random
access for line and byte ranges. Otherwise it falls back to sequential
decompression.

The reader also supports query-based event filtering: when a query string is
provided and an index exists, non-matching chunks are pruned entirely, and
per-event filtering is applied to the remaining chunks. Conjunctions of
equality predicates (``cat == 'io' AND name == 'read'``) are compiled into
a vectorized predicate evaluator that runs against the index bloom dimensions
before any line is decompressed.

``TraceReader`` also accepts a **directory** as ``file_path``: when given a
directory, it enumerates trace files inside it, opens one indexed reader per
file, and yields lines / Arrow batches in file order. Batch chunk pruning is
delegated to ``ChunkPrunerUtility``, which evaluates the compiled query
against all candidate chunks in one pass and feeds the resulting line-range
work items back to the per-file readers.

When ``DFTRACER_UTILS_ENABLE_ARROW`` is set, ``TraceReader::read_arrow()``
exports record batches via the Arrow C Data Interface
(``ArrowExportResult``), which can be sent directly across the FFI boundary
to Python / DuckDB / Polars without a copy. The ``ReadConfig::flatten_objects``
flag expands one level of nested JSON objects (e.g. ``args``) into
``parent.child`` columns with native Arrow types instead of serializing them
as JSON strings.

Getting Started
---------------

Read all lines from a trace file sequentially:

.. code-block:: cpp

    #include <dftracer/utils/utilities/reader/trace_reader.h>

    using namespace dftracer::utils::utilities::reader;

    TraceReaderConfig config;
    config.file_path = "trace.pfw.gz";
    config.index_dir = "/tmp/indexes";

    TraceReader reader(config);

    // Stream lines as an async generator
    auto gen = reader.read_lines();
    while (auto line = co_await gen.next()) {
        // line->content is a string_view (valid until next iteration)
        // line->line_number is 1-based
        process(line->content);
    }

Read a specific line range using an index:

.. code-block:: cpp

    TraceReaderConfig config;
    config.file_path = "trace.pfw.gz";
    config.index_dir = "/tmp/indexes";

    TraceReader reader(config);

    ReadConfig rc;
    rc.start_line = 1000;
    rc.end_line = 2000;

    auto gen = reader.read_lines(rc);
    while (auto line = co_await gen.next()) {
        process(line->content);
    }

Read with query-based chunk pruning:

.. code-block:: cpp

    ReadConfig rc;
    rc.query = "name == 'read' AND cat == 'io'";

    auto gen = reader.read_lines(rc);
    while (auto line = co_await gen.next()) {
        // Only lines matching the query are yielded.
        // Non-matching chunks are skipped entirely when an index exists.
        process(line->content);
    }

Read raw byte chunks instead of parsed lines:

.. code-block:: cpp

    ReadConfig rc;
    rc.start_byte = 0;
    rc.end_byte = 1024 * 1024;  // first 1 MB

    auto gen = reader.read_raw(rc);
    while (auto chunk = co_await gen.next()) {
        // chunk is std::span<const char>
        write(output_fd, chunk->data(), chunk->size());
    }

TraceReaderConfig
-----------------

File-level configuration for constructing a ``TraceReader``. Fields:

- ``file_path`` -- path to the trace file (``.pfw.gz`` or plain text)
- ``index_dir`` -- directory where ``.idx`` sidecar files are stored
- ``checkpoint_size`` -- checkpoint interval for index building (default 32 MB)
- ``auto_build_index`` -- automatically build an index if one is missing (default false)
- ``index_threshold`` -- minimum file size before auto-indexing kicks in

ReadConfig
----------

Per-read configuration controlling range selection, buffering, and query
filtering. All fields have sensible defaults; pass a default-constructed
``ReadConfig{}`` for a full sequential read.

- ``start_line`` / ``end_line`` -- line range (1-indexed; 0 means beginning / end)
- ``start_byte`` / ``end_byte`` -- byte range (0 means beginning / end)
- ``line_aligned`` -- align raw byte chunks to line boundaries (default true)
- ``multi_line`` -- allow multiple lines per raw chunk (default true)
- ``buffer_size`` -- internal read buffer size (default 4 MB)
- ``query`` -- query DSL string for event filtering (empty = no filter)
- ``chunk_prune_only`` -- when true, the query is used only for chunk-level
  pruning via the index; per-line filtering is skipped (caller handles it)
- ``skip_pruning`` -- skip the reader's own chunk pruner pass; the caller's
  ``start_line``/``end_line`` window is trusted (used by the checkpoint-level
  work-item dispatcher to avoid re-running ``ChunkPrunerUtility`` per item)
- ``flatten_objects`` -- expand one level of nested JSON objects into
  ``parent.child`` columns with native Arrow types in ``read_arrow()``

Helper methods: ``has_line_range()`` and ``has_byte_range()`` test whether
non-default range bounds have been set.

TraceReader
-----------

High-level reader with automatic format detection and index support.
Constructed from a ``TraceReaderConfig``, it probes for an ``.idx`` sidecar
at construction time and selects the optimal read strategy (sequential or
indexed) based on whether an index exists and what range the caller requests.

**Async generators:**

- ``read_lines(config)`` -- yields ``Line`` structs (``content`` + ``line_number``) with optional query filtering and chunk pruning
- ``read_json(config)`` -- yields ``JsonLine`` records (parsed once with simdjson) for callers that would otherwise re-parse each line
- ``read_raw(config)`` -- yields ``std::span<const char>`` byte chunks
- ``read_arrow(config, batch_size)`` -- yields ``ArrowExportResult`` record batches via the Arrow C Data Interface (requires ``DFTRACER_UTILS_ENABLE_ARROW``)

**Metadata queries:**

- ``has_index()`` -- true if an ``.idx`` sidecar was found
- ``get_max_bytes()`` -- decompressed size (0 if no index for compressed files)
- ``get_num_lines()`` -- total line count (0 if no index)

.. code-block:: cpp

    TraceReader reader(config);

    if (reader.has_index()) {
        std::size_t total = reader.get_num_lines();
        std::size_t bytes = reader.get_max_bytes();
    }

    // Full sequential read with default config
    auto gen = reader.read_lines();
    while (auto line = co_await gen.next()) {
        process(line->content);
    }
