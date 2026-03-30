Compression
=====================

Streaming zlib compression and decompression utilities supporting GZIP, ZLIB, and DEFLATE formats.
All compression operates in streaming mode using zero-copy ``ByteView`` chunks.

.. code-block:: cpp

   #include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
   #include <dftracer/utils/utilities/compression/zlib/streaming_decompressor_utility.h>
   #include <dftracer/utils/utilities/compression/zlib/types.h>

Types
-----

.. code-block:: cpp

   // Compression format
   enum class CompressionFormat : int32_t {
       DEFLATE_RAW = -15,   // Raw deflate (no header/trailer)
       ZLIB = 15,           // zlib format
       GZIP = 15 + 16,      // gzip format (default)
       AUTO = 15 + 32,      // Auto-detect gzip/zlib
   };

   // Decompression format
   enum class DecompressionFormat : int32_t {
       DEFLATE_RAW = -15,
       ZLIB = 15,
       GZIP = 15 + 16,
       AUTO = 15 + 32       // Auto-detect (default)
   };

ManualStreamingCompressorUtility
---------------------------------

Zero-copy streaming compression using ``AsyncGenerator<ByteView>``.
Yields compressed chunks as ``ByteView`` references into an internal buffer.

.. code-block:: cpp

   ManualStreamingCompressorUtility compressor(Z_DEFAULT_COMPRESSION,
                                               CompressionFormat::GZIP);

   // Compress input chunks, yielding zero-copy ByteView results
   auto gen = compressor.compress(input_bytes);
   while (auto view = co_await gen.next()) {
       co_await writer.process(*view);
   }

   // Finalize to flush remaining compressed data
   auto fin = compressor.finalize_stream();
   while (auto view = co_await fin.next()) {
       co_await writer.process(*view);
   }

   // Query statistics
   std::size_t bytes_in = compressor.total_bytes_in();
   std::size_t bytes_out = compressor.total_bytes_out();
   double ratio = compressor.compression_ratio();

StreamingDecompressorUtility
----------------------------

Streaming decompression with zlib stream reuse and lazy initialization.

.. code-block:: cpp

   StreamingDecompressorUtility decompressor;

   for (const auto& compressed_chunk : compressed_data) {
       auto decompressed = decompressor.process(compressed_chunk);
       for (const auto& chunk : decompressed) {
           process(chunk);
       }
   }
