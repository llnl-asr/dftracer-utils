Indexer
=================

Checkpoint indexing for compressed trace files, enabling efficient random access into ``.pfw.gz`` archives without full decompression.

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>

Overview
--------

The indexer builds sidecar ``.idx`` files that store byte offsets, decompression dictionaries, and line ranges for each checkpoint in a compressed file. This enables seeking to any line or byte range without decompressing from the beginning.

Sidecar files:

- ``.idx`` — Checkpoint index (byte offsets, decompression dictionaries, line ranges)
- ``.bidx`` — Bloom index (per-chunk bloom filters + chunk statistics)
- ``.midx`` — Manifest index (event-level line routing + provenance)

IndexerFactory
--------------

Creates indexers with automatic format detection (GZIP vs TAR.GZ).

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>

   using namespace dftracer::utils::utilities::indexer::internal;

   // Create indexer (auto-detects format)
   auto indexer = IndexerFactory::create(
       "trace.pfw.gz",     // Input file
       "trace.pfw.gz.idx", // Output index path (empty = auto)
       32 * 1024 * 1024,   // Checkpoint size (32MB default)
       true                // Force rebuild
   );

   // Build the index
   indexer->build();

**Async build (coroutine):**

.. code-block:: cpp

   co_await indexer->build_async();

**Query the index:**

.. code-block:: cpp

   // Get file metadata
   std::size_t num_lines = indexer->get_num_lines();
   std::size_t max_bytes = indexer->get_max_bytes();

   // Check if rebuild needed
   if (indexer->need_rebuild()) {
       indexer->build();
   }

   // Find checkpoint for a specific line
   auto checkpoint = indexer->find_checkpoint(500);
   // checkpoint.offset, checkpoint.size, checkpoint.start_line, etc.

   // Get all checkpoints
   auto checkpoints = indexer->get_checkpoints();
   for (const auto& cp : checkpoints) {
       printf("Lines %zu-%zu at offset %zu\n",
              cp.start_line, cp.end_line, cp.offset);
   }

IndexerCheckpoint
-----------------

.. code-block:: cpp

   struct IndexerCheckpoint {
       std::size_t offset;           // Byte offset in compressed file
       std::size_t size;             // Compressed size of this checkpoint
       std::size_t uncompressed_size;
       std::size_t start_line;       // First line number (1-based)
       std::size_t end_line;         // Last line number
       std::size_t num_lines;        // Number of lines in checkpoint
       std::vector<unsigned char> dictionary;  // Decompression dictionary
   };

ChunkIndexerUtility
-------------------

Builds bloom filters and per-chunk statistics for trace events. Used by the indexing pipeline to create ``.bidx`` sidecar files.

.. code-block:: cpp

   #include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>

   using namespace dftracer::utils::utilities::composites::dft::indexing;

   // Configure what to index
   ChunkIndexerConfig config;
   config.index_names = true;
   config.index_categories = true;
   config.index_timestamps = true;
   config.collect_statistics = true;

   // Build input
   auto input = ChunkIndexerInput()
       .with_events(events)
       .with_config(config)
       .with_bloom_expected_entries(1024)
       .with_bloom_fp_rate(0.01);

   // Process
   ChunkIndexerUtility indexer;
   auto output = co_await indexer.process(input);

   // Access results
   auto& bloom = output.bloom_filter;    // BloomFilter for this chunk
   auto& stats = output.statistics;      // ChunkStatistics

Python API
----------

.. code-block:: python

   from dftracer.utils import Indexer

   # Create and build index
   with Indexer("trace.pfw.gz") as indexer:
       indexer.build()

       print(f"Lines: {indexer.get_num_lines()}")
       print(f"Bytes: {indexer.get_max_bytes()}")

       # Query checkpoints
       for cp in indexer.get_checkpoints():
           print(f"Lines {cp.start_line}-{cp.end_line}")

       # Find checkpoint for a line
       cp = indexer.find_checkpoint(500)

See Also
--------

- :doc:`reader` - Reader utility that uses indexes for random access
- :doc:`/cli` - Command-line tools (``dftracer_index``)
- :doc:`composites` - Bloom filter and chunk indexing composites
