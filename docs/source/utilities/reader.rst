Reader
================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference </cpp_api/api/index>`.

Streaming reader for compressed trace files with support for line-based and byte-based access, zero-copy reads, and async I/O.

.. code-block:: cpp

   #include <dftracer/utils/utilities/reader/trace_reader.h>

Overview
--------

The reader provides random access into indexed compressed files (``.pfw.gz`` + ``.idx``). It supports multiple stream types for different access patterns and both synchronous and asynchronous reads.

ReaderFactory
-------------

Creates readers with automatic format detection.

.. code-block:: cpp

   using namespace dftracer::utils::utilities::reader;

   auto reader = ReaderFactory::create(
       "trace.pfw.gz",       // Compressed file
       "trace.pfw.gz.idx"    // Index file
   );

   // Query file metadata
   std::size_t num_lines = reader->get_num_lines();
   std::size_t max_bytes = reader->get_max_bytes();

Stream Types
------------

.. code-block:: cpp

   enum class StreamType {
       RAW,         // Raw compressed bytes
       BYTES,       // Decompressed bytes (no line splitting)
       LINE,        // One complete line per read()
       LINE_BYTES,  // Lines within a byte range
       LINES        // Multiple lines per read()
   };

   enum class RangeType {
       LINE_RANGE,  // Range specified as line numbers (1-based)
       BYTE_RANGE   // Range specified as byte offsets
   };

StreamConfig
~~~~~~~~~~~~

Fluent builder for configuring streams:

.. code-block:: cpp

   StreamConfig config;
   config.stream_type(StreamType::LINE)
         .range_type(RangeType::LINE_RANGE)
         .from(100)    // Start line (1-based)
         .to(200);     // End line (inclusive)

Reading Lines
-------------

**Read lines one at a time:**

.. code-block:: cpp

   auto reader = ReaderFactory::create("trace.pfw.gz", "trace.pfw.gz.idx");

   StreamConfig config;
   config.stream_type(StreamType::LINE)
         .range_type(RangeType::LINE_RANGE)
         .from(1)
         .to(100);

   auto stream = reader->stream(config);
   std::string buffer(1024 * 1024, '\0');

   while (!stream->done()) {
       std::size_t bytes = stream->read(buffer.data(), buffer.size());
       if (bytes == 0) break;

       std::string_view line(buffer.data(), bytes);
       process(line);
   }

**Async reading (coroutine):**

.. code-block:: cpp

   auto stream = reader->stream(config);
   std::string buffer(1024 * 1024, '\0');

   while (!stream->done()) {
       std::size_t bytes = co_await stream->read_async(
           buffer.data(), buffer.size());
       if (bytes == 0) break;

       std::string_view line(buffer.data(), bytes);
       process(line);
   }

Reading Byte Ranges
-------------------

Read lines within a byte range (useful for parallel splitting):

.. code-block:: cpp

   StreamConfig config;
   config.stream_type(StreamType::LINE_BYTES)
         .range_type(RangeType::BYTE_RANGE)
         .from(0)
         .to(1024 * 1024);  // First 1MB

   auto stream = reader->stream(config);
   // Each read() returns one complete line within the byte range

Line Processing Callbacks
-------------------------

Process lines without materializing them into a container:

.. code-block:: cpp

   class MyProcessor : public LineProcessor {
   public:
       coro::CoroTask<bool> process(const char* data,
                                     std::size_t length) override {
           std::string_view line(data, length);
           // Process line...
           co_return true;  // Continue processing
       }
   };

   MyProcessor processor;
   reader->process_lines(processor, 1, 1000);  // Lines 1-1000

C API
-----

Opaque handle-based interface for C interoperability:

.. code-block:: c

   #include <dftracer/utils/utilities/reader/reader.h>

   /* Create reader */
   dft_reader_handle_t reader = dft_reader_create(
       "trace.pfw.gz", "trace.pfw.gz.idx");

   /* Query metadata */
   size_t num_lines = dft_reader_get_num_lines(reader);

   /* Create stream */
   dft_reader_stream_handle_t stream = dft_reader_create_stream(
       reader, DFT_STREAM_LINE, DFT_RANGE_LINE, 1, 100);

   /* Read lines */
   char buffer[1024 * 1024];
   while (!dft_reader_stream_done(stream)) {
       size_t bytes = dft_reader_stream_read(
           stream, buffer, sizeof(buffer));
       if (bytes == 0) break;
       /* process buffer[0..bytes-1] */
   }

   /* Cleanup */
   dft_reader_stream_destroy(stream);
   dft_reader_destroy(reader);

Python API
----------

.. code-block:: python

   from dftracer.utils import TraceReader

   reader = TraceReader("trace.pfw.gz")

   # Read all lines
   lines = reader.read_lines()
   for line in lines:
       print(line)

   # Get metadata (requires index sidecar)
   print(f"Total lines: {reader.get_num_lines()}")
   print(f"Total bytes: {reader.get_max_bytes()}")

See Also
--------

- :doc:`indexer` - Indexer that builds the ``.idx`` files readers depend on
- :doc:`fileio` - Higher-level file I/O with async generators
- :doc:`/api/index` - Python API documentation
