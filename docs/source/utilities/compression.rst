Compression
=====================

Zlib compression and decompression utilities supporting GZIP, ZLIB, and DEFLATE formats.

.. code-block:: cpp

   #include <dftracer/utils/utilities/compression/zlib/compressor.h>
   #include <dftracer/utils/utilities/compression/zlib/decompressor.h>
   #include <dftracer/utils/utilities/compression/zlib/streaming_compressor.h>
   #include <dftracer/utils/utilities/compression/zlib/streaming_decompressor.h>

Types
-----

.. code-block:: cpp

   // Binary data
   struct RawData {
       std::vector<unsigned char> data;
   };

   // Compressed data with metadata
   struct CompressedData {
       std::vector<unsigned char> data;
       std::size_t original_size;
   };

   // Compression format
   enum class CompressionFormat {
       DEFLATE_RAW,
       ZLIB,
       GZIP,
       AUTO
   };

CompressorUtility
-----------------

In-memory compression.

.. code-block:: cpp

   CompressorUtility compressor;
   compressor.set_compression_level(6);  // 0-9, higher = better ratio

   RawData input{/* ... */};
   CompressedData output = compressor.process(input);

   double ratio = static_cast<double>(output.data.size()) / input.data.size();

DecompressorUtility
-------------------

In-memory decompression.

.. code-block:: cpp

   DecompressorUtility decompressor;

   CompressedData input{/* ... */};
   RawData output = decompressor.process(input);

StreamingCompressorUtility
--------------------------

Chunk-by-chunk compression with constant memory usage.

.. code-block:: cpp

   StreamingCompressorUtility compressor;

   for (const auto& chunk : input_chunks) {
       CompressedData compressed = compressor.process(chunk);
       write_to_file(compressed);
   }

   // Finalize to flush remaining data
   auto remaining = compressor.finalize();
   for (const auto& chunk : remaining) {
       write_to_file(chunk);
   }

StreamingDecompressorUtility
----------------------------

Chunk-by-chunk decompression.

.. code-block:: cpp

   StreamingDecompressorUtility decompressor;

   for (const auto& compressed_chunk : compressed_data) {
       std::vector<RawData> decompressed = decompressor.process(compressed_chunk);
       for (const auto& chunk : decompressed) {
           process(chunk);
       }
   }
