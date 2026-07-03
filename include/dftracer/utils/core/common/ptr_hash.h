#ifndef DFTRACER_UTILS_CORE_COMMON_PTR_HASH_H
#define DFTRACER_UTILS_CORE_COMMON_PTR_HASH_H

#include <cstddef>
#include <cstdint>

namespace dftracer::utils {

// Avalanching pointer hash (murmur3/splitmix64 finalizer round). Pointer
// values are typically aligned, so their low bits carry poor entropy; mixing
// gives good distribution for both open-addressing maps and shard selection.
struct PtrHash {
    using is_avalanching = void;
    std::size_t operator()(const void* p) const noexcept {
        std::uint64_t x = reinterpret_cast<std::uintptr_t>(p);
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<std::size_t>(x);
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_PTR_HASH_H
