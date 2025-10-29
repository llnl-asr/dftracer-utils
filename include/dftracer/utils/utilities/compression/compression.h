#ifndef DFTRACER_UTILS_UTILITIES_COMPRESSION_H
#define DFTRACER_UTILS_UTILITIES_COMPRESSION_H

/**
 * @file compression.h
 * @brief Top-level header for all compression component utilities.
 *
 * This header provides access to all compression utilities:
 * - gzip: Gzip compression and decompression
 *
 * Usage:
 * @code
 * #include <dftracer/utils/utilities/compression/compression.h>
 *
 * // Use specific compression type
 * using namespace dftracer::utils::utilities::compression;
 *
 * auto compressor = std::make_shared<gzip::Compressor>();
 * gzip::RawData input("Hello, World!");
 * gzip::CompressedData output = compressor->process(input);
 * @endcode
 */

#include <dftracer/utils/utilities/compression/gzip/gzip.h>

#endif  // DFTRACER_UTILS_UTILITIES_COMPRESSION_H
