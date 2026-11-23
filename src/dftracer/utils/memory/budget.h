// SPDX-License-Identifier: MIT
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
/// other value is used as-is. Shared by the View and the lazy @ref
/// dftracer::utils::dataframe::DataFrame "DataFrame" so both treat 0
/// identically.
std::uint64_t resolve_spill_budget(std::uint64_t configured);

std::size_t detect_available_memory();

/**
 * @brief Holds the allocator's freed-but-backed memory to a quarter of a job's
 * budget while the job runs.
 *
 * The smallest budget of the scopes alive, rounded down to a power of two,
 * sets the bound, which stays within detail::IDLE_BACKED_MIN and
 * detail::IDLE_BACKED_CAP. A budget whose quarter reaches
 * detail::IDLE_BACKED_CAP changes nothing. Thread-safe and lock-free, so a
 * scope may end on another thread than the one that made it.
 */
class JobBudgetScope {
   public:
    explicit JobBudgetScope(std::uint64_t budget) noexcept;
    ~JobBudgetScope();
    JobBudgetScope(JobBudgetScope&& other) noexcept;
    JobBudgetScope(const JobBudgetScope&) = delete;
    JobBudgetScope& operator=(const JobBudgetScope&) = delete;
    JobBudgetScope& operator=(JobBudgetScope&&) = delete;

   private:
    int bucket_;
};

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_BUDGET_H
