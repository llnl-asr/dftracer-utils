#include <dftracer/utils/common/memory_budget.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/memory/budget.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace dftracer::utils {

std::size_t compute_memory_budget(std::size_t user_override_bytes) {
    if (user_override_bytes > 0) return user_override_bytes;
    std::size_t avail = memory::detect_available_memory();
    std::size_t budget = avail * DEFAULT_MEMORY_BUDGET_FRACTION_PERCENT / 100;
    return std::max(budget, memory::MIN_MEMORY_BUDGET_BYTES);
}

std::uint64_t share_spill_budget(std::uint64_t configured, std::size_t ways) {
    if (ways <= 1 || configured == memory::NO_SPILL_BUDGET) return configured;
    const std::uint64_t share = memory::resolve_spill_budget(configured) / ways;
    return share == 0 ? 1 : share;
}

std::size_t concurrent_spill_ways(std::uint64_t configured, std::size_t n) {
    if (n <= 1) return 1;
    if (configured == memory::NO_SPILL_BUDGET) return n;
    const std::uint64_t fit = memory::resolve_spill_budget(configured) /
                              memory::MIN_MEMORY_BUDGET_BYTES;
    if (fit <= 1) return 1;
    return fit < n ? static_cast<std::size_t>(fit) : n;
}

std::size_t estimate_per_file_bytes(const std::vector<std::size_t> &file_sizes,
                                    std::size_t user_override_bytes) {
    if (user_override_bytes > 0) return user_override_bytes;
    if (file_sizes.empty()) return MIN_PER_FILE_PEAK_BYTES;

    const std::size_t total = file_sizes.size();
    const std::size_t sample_count = std::min(total, PER_FILE_SAMPLE_LIMIT);
    const std::size_t stride = std::max(total / sample_count, std::size_t(1));

    std::vector<std::size_t> sizes;
    sizes.reserve(sample_count);
    for (std::size_t i = 0; i < total && sizes.size() < sample_count;
         i += stride) {
        if (file_sizes[i] > 0) sizes.push_back(file_sizes[i]);
    }

    if (sizes.empty()) return MIN_PER_FILE_PEAK_BYTES;

    std::size_t idx = (sizes.size() * 95) / 100;
    if (idx >= sizes.size()) idx = sizes.size() - 1;
    std::nth_element(sizes.begin(), sizes.begin() + idx, sizes.end());
    const std::size_t p95 = sizes[idx];

    std::size_t estimate = p95 * PER_FILE_EXPANSION_FACTOR;
    estimate = std::max(estimate, MIN_PER_FILE_PEAK_BYTES);
    estimate = std::min(estimate, MAX_PER_FILE_PEAK_BYTES);
    return estimate;
}

MemoryBudgetAdvice memory_budget_advice(std::size_t required_bytes,
                                        std::size_t available_bytes) {
    if (available_bytes == 0)
        available_bytes = memory::detect_available_memory();
    MemoryBudgetAdvice a;
    a.required_bytes = required_bytes;
    a.available_bytes = available_bytes;
    a.peak_bytes = required_bytes * PEAK_MEMORY_FACTOR;
    a.fits = a.peak_bytes <= available_bytes;
    a.suggested_nodes =
        a.fits ? 1 : (a.peak_bytes + available_bytes - 1) / available_bytes;
    return a;
}

std::string format_memory_budget_warning(const MemoryBudgetAdvice &advice) {
    if (advice.fits) return std::string();
    auto hb = [](std::size_t b) { return human_bytes(static_cast<double>(b)); };
    return "workload needs ~" + hb(advice.peak_bytes) + " at peak (" +
           std::to_string(PEAK_MEMORY_FACTOR) + "x the ~" +
           hb(advice.required_bytes) + " aggregated size); ~" +
           hb(advice.available_bytes) + " available. Increase memory to ~" +
           hb(advice.peak_bytes) +
           ", or split across >=" + std::to_string(advice.suggested_nodes) +
           " nodes.";
}

}  // namespace dftracer::utils
