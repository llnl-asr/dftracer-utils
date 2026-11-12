// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_HASH_CONSTANTS_H
#define DFTRACER_UTILS_DSTD_HASH_CONSTANTS_H

#include <cstdint>

namespace dftracer::utils::dstd::hash {

/// 2^64 divided by the golden ratio, as in Knuth's Fibonacci hashing (TAOCP
/// vol. 3, 6.4) and the SplitMix64 increment (Sebastiano Vigna, public domain,
/// https://prng.di.unimi.it/splitmix64.c). The 32-bit variant mixes poorly when
/// size_t is 64-bit, so prefer this everywhere hashes are combined or seeded.
inline constexpr std::uint64_t GOLDEN_RATIO = 0x9e3779b97f4a7c15ULL;

/// SplitMix64 finalizer multipliers, from Sebastiano Vigna's SplitMix64
/// generator (public domain / CC0): https://prng.di.unimi.it/splitmix64.c.
inline constexpr std::uint64_t SPLITMIX64_MUL1 = 0xbf58476d1ce4e5b9ULL;
inline constexpr std::uint64_t SPLITMIX64_MUL2 = 0x94d049bb133111ebULL;

/// PRIME64_2 of xxHash, by Yann Collet (BSD-2-Clause,
/// https://github.com/Cyan4973/xxHash).
inline constexpr std::uint64_t XXH64_PRIME2 = 0xc2b2ae3d27d4eb4fULL;

/// MurmurHash3's fmix64 finalizer multipliers, by Austin Appleby (public
/// domain, https://github.com/aappleby/smhasher).
inline constexpr std::uint64_t MURMUR3_FMIX64_MUL1 = 0xff51afd7ed558ccdULL;
inline constexpr std::uint64_t MURMUR3_FMIX64_MUL2 = 0xc4ceb9fe1a85ec53ULL;

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

}  // namespace dftracer::utils::dstd::hash

#endif  // DFTRACER_UTILS_DSTD_HASH_CONSTANTS_H
