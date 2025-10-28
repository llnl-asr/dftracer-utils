#ifndef DFTRACER_UTILS_UTILITIES_COMPRESSION_H
#define DFTRACER_UTILS_UTILITIES_COMPRESSION_H

/**
 * @file compression.h
 * @brief Top-level header for all compression component utilities.
 *
 * This header provides access to all compression utilities:
 * - zlib: ZLIB compression and decompression
 *
 * Usage:
 * @code
 * #include <dftracer/utils/utilities/compression/compression.h>
 *
 * // Use specific compression type
 * using namespace dftracer::utils::utilities::compression;
 *
 * auto compressor = std::make_shared<zlib::Compressor>();
 * io::RawData input("Hello, World!");
 * zlib::CompressedData output = compressor->process(input);
 * @endcode
 */

#include <dftracer/utils/utilities/compression/zlib/zlib.h>

#endif  // DFTRACER_UTILS_UTILITIES_COMPRESSION_H
