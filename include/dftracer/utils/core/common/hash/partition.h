#ifndef DFTRACER_UTILS_CORE_COMMON_HASH_PARTITION_H
#define DFTRACER_UTILS_CORE_COMMON_HASH_PARTITION_H

#include <dftracer/utils/core/common/hash/constants.h>
#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/core/common/hash/splitmix64.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dftracer::utils::hash {

/// The partition, out of `parts`, that a hash falls in at fan-out level
/// `depth`. The same hash lands in the same partition on every call, and each
/// deeper level spreads differently the hashes that shared a partition at the
/// level before, so a recursive split of an oversize partition makes progress.
inline std::size_t partition_of(std::uint64_t h, std::size_t parts,
                                int depth = 0) {
    return static_cast<std::size_t>(
        splitmix64(h ^ (static_cast<std::uint64_t>(depth) * GOLDEN_RATIO)) %
        parts);
}

/// The partition of a byte key.
inline std::size_t partition_of(std::string_view key, std::size_t parts,
                                int depth = 0) {
    return partition_of(fnv1a_hash(key), parts, depth);
}

}  // namespace dftracer::utils::hash

#endif  // DFTRACER_UTILS_CORE_COMMON_HASH_PARTITION_H
