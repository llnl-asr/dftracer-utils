Indexer
=================

Unified indexing and reading infrastructure for compressed trace files. Builds sidecar ``.idx`` files that enable efficient random access, bloom-filter-accelerated queries, and event-level manifest routing — all in a single decompression pass.

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/index_builder.h>
   #include <dftracer/utils/utilities/reader/trace_reader.h>

Overview
--------

The indexer builds sidecar ``.idx`` files with an additive SQLite schema:

- **Checkpoints** — byte offsets and decompression dictionaries for random access
- **Bloom filters** — per-chunk bloom filters for fast event filtering (optional)
- **Chunk statistics** — per-chunk event counts, duration distributions (optional)
- **Manifest** — per-chunk event-to-line routing for reorganization (optional)

A separate ``.pidx`` file stores provenance data for reorganization tracking.

Sidecar files:

- ``.idx`` — Unified content index (checkpoints + bloom filters + chunk statistics + manifest)
- ``.pidx`` — Provenance index (reorganization tracking)

IndexBuilder
------------

Single-pass index builder. Decompresses each file once and builds all requested index data via the visitor pattern.

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/index_builder.h>

   using namespace dftracer::utils::utilities::indexer;

   // Build checkpoint + bloom index in one pass
   auto config = IndexBuildConfig::for_file("trace.pfw.gz")
       .with_bloom(true)
       .with_manifest(false)
       .with_checkpoint_size(32 * 1024 * 1024)
       .with_index_threshold(8 * 1024 * 1024);  // skip .idx for files < 8MB

   IndexBuilder builder;
   auto result = co_await builder.build(config);

   // result.success, result.idx_path, result.total_lines, result.chunks_processed

**Incremental builds:** If ``.idx`` already exists with valid checkpoints, requesting bloom or manifest only runs a streaming decompression pass for the new visitors — no checkpoint rebuild.

.. code-block:: cpp

   // First run: checkpoints only
   auto config1 = IndexBuildConfig::for_file("trace.pfw.gz");
   co_await builder.build(config1);

   // Later: add bloom (reuses existing checkpoints)
   auto config2 = IndexBuildConfig::for_file("trace.pfw.gz")
       .with_bloom(true);
   co_await builder.build(config2);  // one decompression pass for bloom only

   // Later: all features present, skips entirely
   co_await builder.build(config2);  // "Skipping already-indexed file"

IndexDatabase
-------------

Manages the unified ``.idx`` SQLite sidecar with additive schema.

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/index_database.h>

   using namespace dftracer::utils::utilities::indexer;

   IndexDatabase db("trace.pfw.gz.idx");
   db.init_base_schema();    // checkpoints, files, metadata
   db.init_bloom_schema();   // bloom filters, statistics, hash resolutions
   db.init_manifest_schema(); // event ranges, metadata lines

   int fid = db.get_file_info_id("trace.pfw.gz");
   bool has_bloom = db.has_bloom_data(fid);
   bool has_manifest = db.has_manifest_data(fid);

ProvenanceDatabase
------------------

Manages ``.pidx`` files for reorganization provenance tracking.

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/provenance_database.h>

   using namespace dftracer::utils::utilities::indexer;

   ProvenanceDatabase pdb("output.pfw.gz.pidx");
   pdb.init_schema();

   int fid = pdb.get_or_create_file_info("output.pfw.gz", file_hash);
   pdb.begin_transaction();
   pdb.insert_info("version", "1.0");
   pdb.insert_source(fid, 0, "original.pfw.gz", num_checkpoints);
   pdb.commit_transaction();

TraceReader
-----------

Unified reader for all trace file formats. Auto-selects between sequential decompression and indexed random access based on ``.idx`` presence. Supports gzip and plain text files.

Two methods cover all reading modes:

- ``read_lines(ReadConfig)`` — returns parsed ``Line`` objects (``string_view``, zero-copy)
- ``read_raw(ReadConfig)`` — returns raw byte spans (``std::span<const char>``)

``ReadConfig`` controls range (line or byte), alignment, and buffering.

.. code-block:: cpp

   #include <dftracer/utils/utilities/reader/trace_reader.h>

   using namespace dftracer::utils::utilities::reader;

   TraceReader reader({.file_path = "trace.pfw.gz"});

   // Read all lines (default)
   auto gen = reader.read_lines();
   while (auto line = co_await gen.next()) {
       // line->content is string_view, valid until next iteration
   }

   // Line range
   ReadConfig rc;
   rc.start_line = 100;
   rc.end_line = 200;
   auto range = reader.read_lines(rc);

   // Raw bytes — line-aligned, multi-line chunks (fastest for bulk processing)
   auto raw = reader.read_raw();
   while (auto chunk = co_await raw.next()) {
       // chunk is std::span<const char>
   }

   // Raw bytes — single line per yield
   ReadConfig single;
   single.line_aligned = true;
   single.multi_line = false;
   auto line_bytes = reader.read_raw(single);

   // Raw bytes — no line awareness
   ReadConfig raw_cfg;
   raw_cfg.line_aligned = false;
   auto bytes = reader.read_raw(raw_cfg);

   // Byte range
   ReadConfig byte_range;
   byte_range.start_byte = 0;
   byte_range.end_byte = 1024 * 1024;
   auto chunk_gen = reader.read_raw(byte_range);

**ReadConfig to StreamType mapping:**

.. list-table::
   :header-rows: 1

   * - ``read_raw`` flags
     - Internal StreamType
   * - ``line_aligned=true, multi_line=true`` (default)
     - ``MULTI_LINES_BYTES``
   * - ``line_aligned=true, multi_line=false``
     - ``LINE_BYTES``
   * - ``line_aligned=false``
     - ``BYTES``

IndexVisitor
------------

Interface for processing decompressed lines during index building. Implementations receive each line and its checkpoint index.

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/index_visitor.h>

   class IndexVisitor {
   public:
       virtual ~IndexVisitor() = default;
       virtual void begin(std::size_t num_checkpoints) = 0;
       virtual void on_checkpoint(std::size_t checkpoint_idx) = 0;
       virtual void on_line(std::string_view line,
                            std::size_t checkpoint_idx) = 0;
       virtual void finalize(IndexDatabase& db, int file_id) = 0;
   };

Built-in visitors:

- **BloomVisitor** — parses JSON events, populates bloom filters and chunk statistics
- **ManifestVisitor** — tracks (category, name) to line number mappings per checkpoint

Low-level IndexerFactory
------------------------

Creates checkpoint indexers with automatic format detection (GZIP vs TAR.GZ). Used internally by ``IndexBuilder``.

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>

   using namespace dftracer::utils::utilities::indexer::internal;

   auto indexer = IndexerFactory::create(
       "trace.pfw.gz",     // Input file
       "trace.pfw.gz.idx", // Output index path
       32 * 1024 * 1024,   // Checkpoint size (32MB)
       true                // Force rebuild
   );

   co_await indexer->build_async();

   std::size_t num_lines = indexer->get_num_lines();
   auto checkpoints = indexer->get_checkpoints();

Python API
----------

**Indexer:**

.. code-block:: python

   from dftracer.utils import Indexer

   # Checkpoint-only build
   with Indexer("trace.pfw.gz") as indexer:
       indexer.build()
       print(f"Lines: {indexer.get_num_lines()}")

   # Single-pass build with bloom + manifest
   with Indexer("trace.pfw.gz", build_bloom=True, build_manifest=True) as indexer:
       indexer.build()
       assert indexer.has_bloom
       assert indexer.has_manifest

   # Incremental: add bloom to existing index
   with Indexer("trace.pfw.gz", build_bloom=True) as indexer:
       indexer.build()  # reuses checkpoints, adds bloom only

**TraceReader:**

.. code-block:: python

   from dftracer.utils import TraceReader

   # Auto-selects sequential vs indexed reading
   reader = TraceReader("trace.pfw.gz")
   lines = reader.read_lines()

   # Line range
   partial = reader.read_lines(start_line=100, end_line=200)

   # Properties
   print(reader.has_index)    # True if .idx exists
   print(reader.num_lines)    # precise line count

   # Context manager
   with TraceReader("trace.pfw.gz") as reader:
       for line in reader.read_lines():
           event = json.loads(line)

See Also
--------

- :doc:`/cli` - Command-line tools (``dftracer_index``, ``dftracer_reader``)
- :doc:`composites` - Bloom filter and chunk indexing composites
- :doc:`/cpp_api/dft_indexing` - C++ API reference for indexing classes
