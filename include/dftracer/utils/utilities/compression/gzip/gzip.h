#ifndef DFTRACER_UTILS_UTILITIES_COMPRESSION_GZIP_H
#define DFTRACER_UTILS_UTILITIES_COMPRESSION_GZIP_H

/**
 * @file gzip.h
 * @brief Convenience header for gzip compression utilities.
 *
 * This header provides composable utilities for gzip compression:
 *
 * In-Memory Compression:
 * - Compressor: Compress raw data using gzip (io::RawData ->
 * io::CompressedData)
 * - Decompressor: Decompress gzip-compressed data (io::CompressedData ->
 * io::RawData)
 *
 * Streaming Compression (lazy iterators):
 * - StreamingCompressor: Lazy compression (ChunkRange -> CompressedChunkRange)
 * - ManualStreamingCompressor: Manual chunk-by-chunk compression
 * - StreamingDecompressor: Manual chunk-by-chunk decompression
 *
 * Note: RawData and CompressedData are defined in utilities/io/types/types.h
 *
 * Usage (In-Memory):
 * @code
 * #include <dftracer/utils/utilities/compression/gzip/gzip.h>
 *
 * using namespace dftracer::utils::utilities::compression::gzip;
 * using namespace dftracer::utils::utilities::io;
 *
 * auto compressor = std::make_shared<Compressor>();
 * auto decompressor = std::make_shared<Decompressor>();
 *
 * RawData input("Hello, World!");
 * CompressedData compressed = compressor->process(input);
 * RawData restored = decompressor->process(compressed);
 * @endcode
 *
 * Usage (Streaming):
 * @code
 * #include <dftracer/utils/utilities/compression/gzip/gzip.h>
 * #include <dftracer/utils/utilities/io/streaming_file_reader.h>
 *
 * using namespace dftracer::utils::utilities;
 *
 * auto reader = std::make_shared<io::StreamingFileReader>();
 * auto compressor = std::make_shared<compression::gzip::StreamingCompressor>();
 *
 * io::ChunkRange chunks =
 * reader->process(io::StreamReadInput{"/large/file.txt"});
 * compression::gzip::CompressedChunkRange compressed =
 * compressor->process(chunks);
 *
 * for (const auto& chunk : compressed) {
 *     // Process compressed chunks lazily - constant memory!
 * }
 * @endcode
 */

#include <dftracer/utils/utilities/compression/gzip/compressor.h>
#include <dftracer/utils/utilities/compression/gzip/decompressor.h>
#include <dftracer/utils/utilities/compression/gzip/streaming_compressor.h>
#include <dftracer/utils/utilities/compression/gzip/streaming_decompressor.h>

#endif  // DFTRACER_UTILS_UTILITIES_COMPRESSION_GZIP_H
