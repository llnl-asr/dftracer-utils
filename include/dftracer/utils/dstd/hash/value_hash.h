// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_HASH_VALUE_HASH_H
#define DFTRACER_UTILS_DSTD_HASH_VALUE_HASH_H

#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/string.h>

#include <bit>
#include <cmath>
#include <cstdint>

namespace dftracer::utils::dstd::hash {

/// Version of the value hash below. A stored bloom filter or posting list
/// carries it, and a reader that sees another version cannot probe it.
inline constexpr std::uint8_t VALUE_HASH_VERSION = 1;

namespace detail {

inline constexpr double INT64_END = 9223372036854775808.0;    // 2^63
inline constexpr double UINT64_END = 18446744073709551616.0;  // 2^64

inline std::uint64_t tagged_hash(char tag, std::uint64_t bits) noexcept {
    std::uint8_t buf[9];
    buf[0] = static_cast<std::uint8_t>(tag);
    write_u64_le(buf + 1, bits);
    return xxh3_64(buf, sizeof buf);
}

}  // namespace detail

// The canonical hash of one value, as a bloom filter or a posting list keys it.
// Values that are equal as numbers hash alike whatever their type: every
// integer hashes as an int64, a whole double that fits an int64 (or a uint64
// above it) hashes as that integer (-0.0 as 0), any other double by its bits,
// and every NaN as one value. A string hashes as its bytes and does not equal
// a number. Different values may share a hash, which only makes a probe answer
// "may match". The rules follow Apache Iceberg's value equality.

inline std::uint64_t value_hash_int64(std::int64_t value) noexcept {
    return detail::tagged_hash('i', static_cast<std::uint64_t>(value));
}

inline std::uint64_t value_hash_uint64(std::uint64_t value) noexcept {
    if (value <= static_cast<std::uint64_t>(INT64_MAX))
        return value_hash_int64(static_cast<std::int64_t>(value));
    return detail::tagged_hash('u', value);
}

inline std::uint64_t value_hash_double(double value) noexcept {
    if (std::isnan(value)) return detail::tagged_hash('n', 0);
    if (std::trunc(value) == value) {
        if (value >= -detail::INT64_END && value < detail::INT64_END)
            return value_hash_int64(static_cast<std::int64_t>(value));
        if (value >= detail::INT64_END && value < detail::UINT64_END)
            return value_hash_uint64(static_cast<std::uint64_t>(value));
    }
    return detail::tagged_hash('d', std::bit_cast<std::uint64_t>(value));
}

inline std::uint64_t value_hash_bool(bool value) noexcept {
    return value_hash_int64(value ? 1 : 0);
}

inline std::uint64_t value_hash_string(StringView value) noexcept {
    return xxh3_64(value);
}

inline std::uint64_t value_hash_null() noexcept {
    return detail::tagged_hash('z', 0);
}

}  // namespace dftracer::utils::dstd::hash

#endif  // DFTRACER_UTILS_DSTD_HASH_VALUE_HASH_H
