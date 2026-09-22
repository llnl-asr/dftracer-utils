#ifndef DFTRACER_UTILS_CORE_COMMON_MEMORY_BUDGET_H
#define DFTRACER_UTILS_CORE_COMMON_MEMORY_BUDGET_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils {

static constexpr std::size_t DEFAULT_MEMORY_BUDGET_FRACTION_PERCENT = 50;
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

/// Peak resident memory of a read + aggregate + HLM pass is ~this multiple of
/// the aggregated result's own size: at the peak the gathered partials, the
/// group-by intermediate, and the final frame are all live at once.
/// Equivalently, a workload fits in process only when its aggregated size is
/// under 1/this of the available memory.
static constexpr std::size_t PEAK_MEMORY_FACTOR = 3;

static constexpr std::size_t PER_FILE_EXPANSION_FACTOR = 24;
static constexpr std::size_t MIN_PER_FILE_PEAK_BYTES = 64ULL * 1024 * 1024;
static constexpr std::size_t MAX_PER_FILE_PEAK_BYTES =
    16ULL * 1024 * 1024 * 1024;
static constexpr std::size_t PER_FILE_SAMPLE_LIMIT = 1024;

std::size_t detect_available_memory();
std::size_t compute_memory_budget(std::size_t user_override_bytes = 0);
std::size_t estimate_per_file_bytes(const std::vector<std::size_t>& file_sizes,
                                    std::size_t user_override_bytes = 0);

/// Whether an aggregated workload fits in memory, and what to do if not.
/// `required_bytes` is the aggregated footprint (the measured result bytes, or
/// a cheap estimate of the AGGREGATION-CF size), NOT the raw trace size.
struct MemoryBudgetAdvice {
    bool fits = true;                 ///< peak_bytes <= available_bytes
    std::size_t required_bytes = 0;   ///< aggregated footprint (input)
    std::size_t peak_bytes = 0;       ///< required_bytes * PEAK_MEMORY_FACTOR
    std::size_t available_bytes = 0;  ///< memory judged against
    std::size_t suggested_nodes = 1;  ///< nodes to spread peak under available
};

/// `available_bytes == 0` uses detect_available_memory(). suggested_nodes is
/// ceil(peak_bytes / available_bytes), so it exceeds 1 only when it does not
/// fit.
MemoryBudgetAdvice memory_budget_advice(std::size_t required_bytes,
                                        std::size_t available_bytes = 0);

/// One-line warning for advice that does not fit (empty when it fits). Shared
/// so the CLI binaries and the Python/dfanalyzer path surface the same message.
std::string format_memory_budget_warning(const MemoryBudgetAdvice& advice);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_MEMORY_BUDGET_H
