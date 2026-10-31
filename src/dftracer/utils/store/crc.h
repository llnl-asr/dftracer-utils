#ifndef DFTRACER_UTILS_STORE_CRC_H
#define DFTRACER_UTILS_STORE_CRC_H

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::store::engine {

/// The CRC-32 of zlib's crc32() over `len` bytes, continuing from `crc` (0 to
/// start). Thread-safe.
std::uint32_t crc32(std::uint32_t crc, const void* data, std::size_t len);

}  // namespace dftracer::utils::store::engine

#endif  // DFTRACER_UTILS_STORE_CRC_H
