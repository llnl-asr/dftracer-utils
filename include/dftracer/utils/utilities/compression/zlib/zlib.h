#ifndef DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_H
#define DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_H

/**
 * @file zlib.h
 * @brief Convenience header for zlib compression utilities.
 *
 * Streaming compression utilities using ByteView and AsyncGenerator:
 * - ManualStreamingCompressorUtility: chunk-by-chunk compression
 * - StreamingDecompressorUtility: chunk-by-chunk decompression
 *
 * Both yield zero-copy ByteView into internal buffers.
 */

#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_decompressor_utility.h>
#include <dftracer/utils/utilities/compression/zlib/types.h>

#endif  // DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_H
