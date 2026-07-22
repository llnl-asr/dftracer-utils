#ifndef DFTRACER_UTILS_CORE_COMMON_HASH_FNV1A_H
#define DFTRACER_UTILS_CORE_COMMON_HASH_FNV1A_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dftracer::utils::hash {

inline constexpr std::uint64_t FNV1A_OFFSET_BASIS = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t FNV1A_PRIME = 0x00000100000001B3ULL;

inline std::uint64_t fnv1a_hash(const void* data, std::size_t len) {
    std::uint64_t hash = FNV1A_OFFSET_BASIS;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= FNV1A_PRIME;
    }
    return hash;
}

inline std::uint64_t fnv1a_hash(std::string_view data) {
    return fnv1a_hash(data.data(), data.size());
}

/// FNV-1a avalanches poorly: multiplication only carries low bits upward, so
/// bit i of the result depends on input bits 0..i. Callers taking a bit slice
/// (a bucket index, a truncated id, a sketch register) need this first.
inline std::uint64_t fnv1a_mix(std::uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

}  // namespace dftracer::utils::hash

#endif  // DFTRACER_UTILS_CORE_COMMON_HASH_FNV1A_H
