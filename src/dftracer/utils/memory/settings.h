// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_MEMORY_SETTINGS_H
#define DFTRACER_UTILS_MEMORY_SETTINGS_H

#include <dftracer/utils/memory/detail/heap.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::memory::detail {

/// The settings the allocator itself reads. config() resolves the manager as
/// well and must not run on an allocation path.
Backend active_backend() noexcept;
std::uint32_t decay_ms() noexcept;

/// True when constants::DEBUG_ENV selected the debug wrapper.
bool debug_enabled() noexcept;
/// The constants::EXIT_CHECK_ENV value, read once.
bool exit_check_env() noexcept;

/// The hard cap in bytes. Zero is no cap.
std::size_t hard_cap() noexcept;
void set_hard_cap(std::size_t bytes) noexcept;

/// The budget the page heap keeps its backed bytes under when it maps. Zero
/// until the manager has resolved its budget.
std::size_t map_budget() noexcept;
void set_map_budget(std::size_t bytes) noexcept;

}  // namespace dftracer::utils::memory::detail

#endif  // DFTRACER_UTILS_MEMORY_SETTINGS_H
