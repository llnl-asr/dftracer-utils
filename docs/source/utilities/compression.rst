Compression
=====================

Streaming zlib compression and decompression utilities supporting GZIP, ZLIB, and DEFLATE formats.
All compression operates in streaming mode using zero-copy ``ByteView`` chunks.

.. note::

   The default gzip level used by the writer pipeline (``dftracer_aggregator``,
   ``dftracer_organize``, parallel writers) is ``1`` (fastest); previous
   releases defaulted to ``Z_DEFAULT_COMPRESSION`` (6). Override per-call with
   the ``compression_level`` field on ``ManualStreamingCompressorUtility``.

.. note::

   The build defaults to zlib-ng (compat ABI) when the ``DFTRACER_USE_ZLIB_NG``
   CMake option is ``ON`` (the default), falling back to ``madler/zlib`` if
   zlib-ng cannot be added. The compressor sources are unchanged: the same
   ``deflate``/``inflate`` symbols are linked against whichever backend was
   selected at configure time.

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

Buffered Compression
--------------------

Writer pipelines (parallel writer, perfetto trace writer, organize group
writers) buffer compressed payloads and flush at a configurable
``flush_threshold``. The threshold is computed by
``compute_writer_sizing()`` from the detected filesystem layout
(``LayoutInfo``): on Lustre/GPFS the threshold is sized to the PFS stripe
so each compressed flush fits one stripe; on local FS it is ``max(default,
stripe_size)``. Buffer capacity is always ``flush_threshold +
buffer_headroom``.

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
