// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_MEMORY_CONFIG_H
#define DFTRACER_UTILS_MEMORY_CONFIG_H

#include <dftracer/utils/memory/detail/constants.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::memory {

enum class Backend : std::uint8_t {
    PageHeap,
    /// The C library allocator with a header per block, for sanitizers and
    /// A/B runs.
    System,
};

struct Config {
    Backend backend = Backend::PageHeap;
    /// Milliseconds idle memory stays backed. detail::constants::DECAY_MS_ENV
    /// sets it.
    std::uint32_t decay_ms = detail::constants::DEFAULT_DECAY_MS;
    /// The sum of reservations the root accepts. Zero is a third of the
    /// available memory.
    std::size_t budget = 0;
    /// Bytes of heap the process may hold from the operating system. Zero is
    /// no cap. The system backend checks it only for blocks above 1 MiB.
    std::size_t hard_cap = 0;
    /// Bytes below the process limit where under_pressure turns on. Zero is
    /// 5% of the limit and at least 256 MiB.
    std::size_t limit_headroom = 0;
    /// Bytes past the budget a reclaiming pool may reserve. Zero is 2% of the
    /// budget and at least 32 MiB.
    std::size_t spill_headroom = 0;
};

/**
 * @brief The configuration in effect.
 *
 * The backend and the decay time are fixed at the first call to any function
 * in this header. detail::constants::MEMORY_BACKEND_ENV and
 * detail::constants::DECAY_MS_ENV pick them. The other fields hold resolved
 * values, never zero except hard_cap.
 */
Config config() noexcept;

/**
 * @brief Set the budget, the hard cap and the two headrooms.
 *
 * Every one of those fields is applied and a zero selects its default, so a
 * default Config restores them all. The backend and the decay time are
 * ignored. Safe to call while other threads allocate.
 */
void configure(const Config& config) noexcept;

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_CONFIG_H
