#ifndef DFTRACER_UTILS_MEMORY_BUDGET_H
#define DFTRACER_UTILS_MEMORY_BUDGET_H

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::memory {

static constexpr std::size_t MIN_MEMORY_BUDGET_BYTES = 64 * 1024 * 1024;

/// Sentinel spill budget meaning "never spill" (an effectively infinite bound):
/// resolve_spill_budget passes it through unchanged, and no in-memory state
/// ever exceeds it. Distinct from 0, which means "auto".
static constexpr std::uint64_t NO_SPILL_BUDGET = ~std::uint64_t{0};

/// Resolve a configured out-of-core spill budget the same way everywhere: 0
/// means "auto" and becomes ~1/3 of available memory (floored at
/// MIN_MEMORY_BUDGET_BYTES); NO_SPILL_BUDGET passes through (never spills); any
/// other value is used as-is. Shared by the View and the lazy DataFrame so both
/// treat 0 identically.
std::uint64_t resolve_spill_budget(std::uint64_t configured);

std::size_t detect_available_memory();

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_BUDGET_H
