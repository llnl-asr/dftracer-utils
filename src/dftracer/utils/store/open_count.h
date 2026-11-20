// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_STORE_OPEN_COUNT_H
#define DFTRACER_UTILS_STORE_OPEN_COUNT_H

#include <cstdint>

namespace dftracer::utils::store::internal {

/// Stores opened in this process so far. A Store::open that finds a live
/// store of the same directory does not count.
std::uint64_t opens() noexcept;

#if DFTRACER_UTILS_TEST_PROBES
/// Sets a function that Store::open calls between its look at the registry and
/// its check that the store it found is current, or clears it with nullptr.
/// For tests: it lets a test drop the last owner of that store at that point.
void set_open_probe(void (*probe)()) noexcept;
#endif

}  // namespace dftracer::utils::store::internal

#endif  // DFTRACER_UTILS_STORE_OPEN_COUNT_H
