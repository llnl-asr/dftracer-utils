#ifndef DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_TYPES_H
#define DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_TYPES_H

#include <cstdint>

namespace dftracer::utils::utilities::compression::zlib {

/**
 * @brief zlib window-bits format selector, shared by (de)compression.
 *
 * The value is the zlib windowBits parameter:
 * - DEFLATE_RAW: -15 (raw deflate, no header/trailer)
 * - ZLIB: 15 (zlib format with header/trailer)
 * - GZIP: 15 + 16 (gzip format with header/trailer)
 * - AUTO: 15 + 32 (auto-detect gzip/zlib; decompression only)
 */
enum class ZlibFormat : std::int32_t {
    DEFLATE_RAW = -15,  // Raw deflate (no header/trailer)
    ZLIB = 15,          // zlib format
    GZIP = 15 + 16,     // gzip format
    AUTO = 15 + 32,     // Auto-detect gzip/zlib (decompression only)
};

// The compression and decompression paths use the same windowBits selector;
// these aliases preserve the directional names at call sites. AUTO is only
// meaningful for decompression.
using CompressionFormat = ZlibFormat;
using DecompressionFormat = ZlibFormat;
}  // namespace dftracer::utils::utilities::compression::zlib

#endif  // DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_SHARED_H
