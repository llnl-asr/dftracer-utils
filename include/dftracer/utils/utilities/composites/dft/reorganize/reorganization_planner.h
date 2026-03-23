#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_REORGANIZATION_PLANNER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_REORGANIZATION_PLANNER_H

#include <dftracer/utils/core/utilities/utility.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::reorganize {

struct PredicateGroup {
    std::string name;
    std::string query;
};

struct SourceFileInfo {
    std::string file_path;
    std::string idx_path;
    std::size_t num_checkpoints = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t checkpoint_size = 0;
};

struct ExtractionTask {
    std::size_t source_file_idx;
    std::uint64_t checkpoint_idx;
    std::string target_group;
    std::vector<std::uint32_t> line_numbers;  // sorted
    std::uint64_t start_byte;
    std::uint64_t end_byte;
};

struct ExtractionPlan {
    std::vector<PredicateGroup> groups;
    std::vector<SourceFileInfo> source_files;
    std::vector<ExtractionTask> tasks;
    std::size_t total_events = 0;
};

struct ReorganizationPlannerInput {
    std::vector<std::string> source_files;
    std::vector<PredicateGroup> groups;
    std::string index_dir;
    std::size_t checkpoint_size = 0;
};

class ReorganizationPlannerUtility
    : public utilities::Utility<ReorganizationPlannerInput, ExtractionPlan> {
   public:
    ReorganizationPlannerUtility() = default;

    coro::CoroTask<ExtractionPlan> process(
        const ReorganizationPlannerInput& input) override;
};

std::vector<PredicateGroup> parse_group_specs(
    const std::vector<std::string>& specs);

}  // namespace
   // dftracer::utils::utilities::composites::dft::reorganize

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_REORGANIZATION_PLANNER_H
