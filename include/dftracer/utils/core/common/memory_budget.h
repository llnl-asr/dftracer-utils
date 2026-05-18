#ifndef DFTRACER_UTILS_CORE_COMMON_MEMORY_BUDGET_H
#define DFTRACER_UTILS_CORE_COMMON_MEMORY_BUDGET_H

#include <cstddef>
#include <vector>

namespace dftracer::utils {

static constexpr std::size_t DEFAULT_MEMORY_BUDGET_FRACTION_PERCENT = 50;
static constexpr std::size_t MIN_MEMORY_BUDGET_BYTES = 64 * 1024 * 1024;

static constexpr std::size_t PER_FILE_EXPANSION_FACTOR = 24;
static constexpr std::size_t MIN_PER_FILE_PEAK_BYTES = 64ULL * 1024 * 1024;
static constexpr std::size_t MAX_PER_FILE_PEAK_BYTES =
    16ULL * 1024 * 1024 * 1024;
static constexpr std::size_t PER_FILE_SAMPLE_LIMIT = 1024;

std::size_t detect_available_memory();
std::size_t compute_memory_budget(std::size_t user_override_bytes = 0);
std::size_t compute_channel_capacity(std::size_t memory_budget_bytes,
                                     std::size_t estimated_batch_bytes,
                                     std::size_t num_workers);
std::size_t compute_file_batch_size(std::size_t memory_budget_bytes,
                                    std::size_t estimated_file_bytes,
                                    std::size_t min_files = 4);

std::size_t estimate_per_file_bytes(const std::vector<std::size_t>& file_sizes,
                                    std::size_t user_override_bytes = 0);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_MEMORY_BUDGET_H
