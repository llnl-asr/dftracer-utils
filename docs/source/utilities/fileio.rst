File I/O
==================

File reading, writing, and streaming utilities supporting both synchronous and asynchronous operations.

Synchronous I/O:

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/file_reader_utility.h>
   #include <dftracer/utils/utilities/fileio/streaming_file_reader_utility.h>
   #include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>
   #include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>

Asynchronous Generators:

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/lines/sources/async_plain_file_line_generator.h>
   #include <dftracer/utils/utilities/fileio/lines/sources/async_plain_file_bytes_generator.h>
   #include <dftracer/utils/utilities/fileio/lines/sources/async_indexed_file_line_generator.h>
   #include <dftracer/utils/utilities/fileio/lines/sources/async_indexed_file_bytes_generator.h>
   #include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>

Types
-----

.. code-block:: cpp

   // Zero-copy byte span (see core_infrastructure)
   class ByteView {
       const void* data();
       std::size_t size();
       template <typename T> const T* as() const;
   };

   // Text content
   struct Text {
       std::string content;
       bool empty() const;
       std::size_t size() const;
   };

   // Line with position
   struct Line {
       std::string_view content;
       std::size_t line_number;  // 1-based
   };

FileReaderUtility
-----------------

Reads entire file into memory as text.

.. code-block:: cpp

   FileReaderUtility reader;
   Text content = reader.process(FileEntry{"/path/to/file.txt"});

BinaryFileReaderUtility
-----------------------

Streaming binary file reader yielding zero-copy ``ByteView`` chunks.

.. code-block:: cpp

   auto gen = read_binary_file("/path/to/file.bin");
   while (auto chunk = co_await gen.next()) {
       process(chunk->as<char>(), chunk->size());
   }

StreamingFileReaderUtility
--------------------------

Reads file in chunks with lazy evaluation.

**Input:**

.. code-block:: cpp

   struct StreamReadInput {
       fs::path path;
       std::size_t chunk_size = 64 * 1024;  // 64KB default
   };

**Example:**

.. code-block:: cpp

   StreamingFileReaderUtility reader;
   auto input = StreamReadInput{"/path/to/large_file.dat", 1024 * 1024};

   // Returns lazy iterator
   ChunkRange chunks = reader.process(input);

   for (const auto& chunk : chunks) {
       process(chunk);
   }

StreamingFileWriterUtility
--------------------------

Writes data in chunks.

.. code-block:: cpp

   StreamingFileWriterUtility writer("/output/file.dat");

   for (const auto& chunk : data_chunks) {
       writer.process(chunk);
   }

   writer.close();
   std::cout << "Wrote " << writer.total_bytes() << " bytes\n";

StreamingLineReader
-------------------

Lazy line-by-line reading for both plain and indexed files.

**Plain text files:**

.. code-block:: cpp

   // Read all lines
   LineRange lines = StreamingLineReader::read_plain("/path/to/file.txt");

   for (const auto& line : lines) {
       std::cout << line.line_number << ": " << line.content << "\n";
   }

   // Read specific line range
   LineRange subset = StreamingLineReader::read_plain(
       "/path/to/file.txt",
       100,   // start_line
       200    // end_line
   );

**Indexed (compressed) files:**

.. code-block:: cpp

   IndexedFileLineIteratorConfig config;
   config.archive_path = "/path/to/file.pfw.gz";
   config.index_path = "/path/to/file.pfw.gz.idx";
   config.start_line = 0;
   config.end_line = 1000;

   LineRange lines = StreamingLineReader::read_indexed(config);

   // Collect all into vector
   std::vector<Line> all_lines = lines.collect();

   // Or take first N
   std::vector<Line> first_100 = lines.take(100);

   // Or filter
    auto filtered = lines.filter([](const Line& l) {
        return l.content.find("error") != std::string_view::npos;
    });

Asynchronous File I/O
---------------------

Async generators provide non-blocking line and byte reading using C++20 coroutines. They are ideal for high-concurrency scenarios and integrating with async task pipelines.

**Plain Text Files**

Read lines from uncompressed files with async I/O:

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/lines/sources/async_plain_file_line_generator.h>

   // Read all lines asynchronously
   auto gen = async_plain_file_lines("data.txt");
   while (auto line = co_await gen.next()) {
       std::cout << line->line_number << ": " << line->content << "\n";
   }

   // Read specific line range
   auto gen = async_plain_file_lines("data.txt", 100, 200);  // lines 100-200
   while (auto line = co_await gen.next()) {
       process(*line);
   }

**Plain Text Files by Byte Range**

Read lines within a byte range from plain files, with automatic line-boundary alignment:

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/lines/sources/async_plain_file_bytes_generator.h>

   auto gen = async_plain_file_bytes("data.txt", 1000, 5000);  // bytes 1000-5000
   while (auto line = co_await gen.next()) {
       process(*line);  // Yields complete lines within the byte range
   }

**Indexed (Compressed) Files**

Read lines from ``.gz.idx`` indexed archive files asynchronously:

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/lines/sources/async_indexed_file_line_generator.h>

   auto config = IndexedFileLineIteratorConfig()
       .with_file("trace.pfw.gz", "trace.pfw.gz.idx")
       .with_line_range(1, 1000);

   auto gen = async_indexed_file_lines(config);
   while (auto line = co_await gen.next()) {
       process(*line);
   }

**Indexed Files by Byte Range**

Read lines within a byte range from indexed archives:

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/lines/sources/async_indexed_file_bytes_generator.h>

   auto reader = ReaderFactory::create("trace.pfw.gz", "trace.pfw.gz.idx");
   auto gen = async_indexed_file_bytes(reader, 1000, 5000);  // bytes 1000-5000
   while (auto line = co_await gen.next()) {
       process(*line);
   }

**Streaming Gzip Decompression**

Read lines from ``.gz`` files without building an index, using streaming decompression:

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>

   // Decompress and stream lines without building a sidecar index
   auto gen = async_streaming_gz_lines("data.pfw.gz");
   while (auto line = co_await gen.next()) {
       process(*line);
   }

   // With line range filtering
   auto gen = async_streaming_gz_lines("data.pfw.gz", 100, 200);  // lines 100-200
   while (auto line = co_await gen.next()) {
       process(*line);
   }

Parallel Writers
----------------

Layout-aware parallel writers for multi-worker output. The ``ParallelWriter``
interface is implemented by three concrete layouts under
``fileio/parallel/``:

- **StripedWriter** - single output file, atomic-offset ``pwrite`` per
  worker. Used on local FS and PFS without padded stripes.
- **PaddedStripedWriter** - single output file where each worker chunk is
  padded to a full PFS stripe so per-stripe writes never cross workers.
  Recommended for Lustre/GPFS when the stripe size is at least
  ``MIN_PADDED_STRIPE_BYTES`` (1 MiB).
- **ShardedWriter** - N output files, one per worker, glob-named by
  ordinal. Used on NFS where atomic-offset ``pwrite`` is not reliable.

.. code-block:: cpp

    #include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
    #include <dftracer/utils/utilities/fileio/parallel/layout.h>

    using namespace dftracer::utils::utilities::fileio::parallel;

    auto info   = detect_layout("/lustre/.../output.pfw.gz");
    auto sizing = compute_writer_sizing(info, /*baseline_workers=*/64,
                                        /*default_flush=*/4 << 20,
                                        /*headroom=*/1 << 20,
                                        /*padded=*/true);

    WriterConfig cfg{
        .layout = info.layout,
        .stripe_size = info.stripe_size,
        .gzip = true,
    };
    auto writer = make_writer(cfg);
    co_await writer->open("output.pfw.gz", sizing.num_workers,
                          /*gzip_extension=*/true, scope);

    co_await writer->write_header(header_bytes);
    co_await writer->write_chunk(worker_id, chunk_bytes);
    auto member = writer->last_member(worker_id);  // offset+length of the gzip member
    co_await writer->write_footer(footer_bytes);
    co_await writer->close();

The writer collects per-chunk ``MemberSpan`` entries (offset + length of
each independently decompressable gzip member) and exposes them via
``member_layout()`` after close. ``shard_base_offsets()`` remaps shard-local
offsets to merged-file offsets for sharded layouts.

Layout detection (``detect_layout``) classifies a path's filesystem as
Lustre, GPFS, BeeGFS, NFS, or LOCAL and picks ``SHARDED`` on NFS,
``STRIPED`` elsewhere; ``compute_writer_sizing`` caps worker count at the
PFS stripe count and sets ``flush_threshold`` to the stripe size for
padded layouts so each compressed flush coalesces into one stripe.

.. note::

    Compressor generators consumed by the parallel writer are wrapped in
    smart pointers (``std::unique_ptr<ManualStreamingCompressorUtility>``)
    so they can be moved across coroutine frames without leaking the
    underlying zlib stream.

Async vs Synchronous
--------------------

Use async generators when:

- Integrating with coroutine-based pipelines (TaskGraph, Channel-based streaming)
- Processing multiple files concurrently without blocking threads
- Operating in high-concurrency environments (many tasks sharing thread pools)

Use synchronous readers when:

- Sequential file processing is acceptable
- Working outside of coroutine contexts
- Simpler error handling is preferred (no need to handle resumable failures)
