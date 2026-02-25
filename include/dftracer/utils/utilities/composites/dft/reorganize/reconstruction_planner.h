#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_RECONSTRUCTION_PLANNER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_RECONSTRUCTION_PLANNER_H

#include <dftracer/utils/core/utilities/utility.h>

#include <map>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::reorganize {

struct ReconstructionSegment {
    std::string reorg_file;
    int output_line_start;
    int output_line_end;
    int source_checkpoint;
    int event_count;
};

struct OriginalFileReconstruction {
    std::string original_path;
    int num_checkpoints;
    std::string event_hash;
    // checkpoint_idx -> list of segments from various
    // reorganized files
    std::map<int, std::vector<ReconstructionSegment>> checkpoint_segments;
};

struct ReconstructionPlan {
    // original_path -> reconstruction info
    std::map<std::string, OriginalFileReconstruction> files;
    std::size_t total_segments = 0;
    std::size_t total_events = 0;
};

struct ReconstructionPlannerInput {
    std::vector<std::string> reorganized_files;
    std::string index_dir;
};

class ReconstructionPlannerUtility
    : public utilities::Utility<ReconstructionPlannerInput,
                                ReconstructionPlan> {
   public:
    ReconstructionPlannerUtility() = default;

    coro::CoroTask<ReconstructionPlan> process(
        const ReconstructionPlannerInput& input) override;
};

}  // namespace
   // dftracer::utils::utilities::composites::dft::reorganize

#endif
