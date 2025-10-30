#ifndef DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_TYPES_H
#define DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_TYPES_H

#include <cstdint>

namespace dftracer::utils::utilities::compression::zlib {

/**
 * @brief Compression format for streaming compression.
 *
 * The windowBits parameter determines the format:
 * - DEFLATE_RAW: windowBits = -15 (raw deflate, no header/trailer)
 * - ZLIB: windowBits = 15 (zlib format with header/trailer)
 * - GZIP: windowBits = 15 + 16 (gzip format with header/trailer)
 */
enum class CompressionFormat : std::int32_t {
    DEFLATE_RAW = -15,  // Raw deflate (no header/trailer)
    ZLIB = 15,          // zlib format
    GZIP = 15 + 16,     // gzip format (default)
    AUTO = 15 + 32,     // Auto-detect gzip/zlib (default)
};

/**
 * @brief Decompression format for streaming decompression.
 *
 * The windowBits parameter determines the format:
 * - DEFLATE_RAW: windowBits = -15 (raw deflate, no header/trailer)
 * - ZLIB: windowBits = 15 (zlib format with header/trailer)
 * - GZIP: windowBits = 15 + 16 (gzip format with header/trailer)
 * - AUTO: windowBits = 15 + 32 (auto-detect gzip/zlib)
 */
enum class DecompressionFormat : std::int32_t {
    DEFLATE_RAW = -15,  // Raw deflate (no header/trailer)
    ZLIB = 15,          // zlib format
    GZIP = 15 + 16,     // gzip format
    AUTO = 15 + 32      // Auto-detect gzip/zlib (default)
};
}  // namespace dftracer::utils::utilities::compression::zlib

#endif  // DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_SHARED_H
