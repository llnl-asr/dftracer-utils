#ifndef DFTRACER_UTILS_CORE_COMMON_HASH_CONSTANTS_H
#define DFTRACER_UTILS_CORE_COMMON_HASH_CONSTANTS_H

#include <cstdint>

namespace dftracer::utils::hash {

/// 64-bit golden ratio. The 32-bit variant (0x9e3779b9) mixes poorly when
/// size_t is 64-bit, so prefer this everywhere hashes are combined or seeded.
inline constexpr std::uint64_t GOLDEN_RATIO = 0x9e3779b97f4a7c15ULL;

/// SplitMix64 finalizer multipliers, from Sebastiano Vigna's SplitMix64
/// generator (public domain / CC0): https://prng.di.unimi.it/splitmix64.c.
inline constexpr std::uint64_t SPLITMIX64_MUL1 = 0xbf58476d1ce4e5b9ULL;
inline constexpr std::uint64_t SPLITMIX64_MUL2 = 0x94d049bb133111ebULL;

/// FNV-1a, 64-bit, by Glenn Fowler, Landon Curt Noll, and Phong Vo (public
/// domain): http://www.isthe.com/chongo/tech/comp/fnv/
inline constexpr std::uint64_t FNV1A_OFFSET_BASIS = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t FNV1A_PRIME = 0x00000100000001b3ULL;

/// A truncated variant of the FNV-1a offset basis (one digit short of
/// FNV1A_OFFSET_BASIS's decimal form) used by several existing hashers. Kept
/// distinct from FNV1A_OFFSET_BASIS so their outputs - some persisted - stay
/// bit-stable; do not "correct" it to the canonical value.
inline constexpr std::uint64_t FNV1A_OFFSET_BASIS_LEGACY =
    1469598103934665603ULL;

}  // namespace dftracer::utils::hash

#endif  // DFTRACER_UTILS_CORE_COMMON_HASH_CONSTANTS_H
