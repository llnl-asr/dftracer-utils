#ifndef DFTRACER_UTILS_STORE_OPEN_COUNT_H
#define DFTRACER_UTILS_STORE_OPEN_COUNT_H

#include <cstdint>

namespace dftracer::utils::store::internal {

/// Stores opened in this process so far. A Store::open that finds a live
/// store of the same directory does not count.
std::uint64_t opens() noexcept;

}  // namespace dftracer::utils::store::internal

#endif  // DFTRACER_UTILS_STORE_OPEN_COUNT_H
