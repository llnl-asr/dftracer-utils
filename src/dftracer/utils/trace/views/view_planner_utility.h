#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLANNER_UTILITY_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLANNER_UTILITY_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/index/store/types.h>
#include <dftracer/utils/trace/views/view_definition.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::views {

struct ViewPlannerInput {
    ViewDefinition view;
    std::string file_path;
    std::string index_path;  // `.dftindex` store path
    std::size_t uncompressed_size = 0;
    std::size_t num_checkpoints = 0;
    std::optional<std::pair<double, double>> time_range;  // {begin, end}
    // Emit every checkpoint (real byte offsets, still parallel) and skip
    // time-based pruning. For when chunk timestamp stats can't be trusted (e.g.
    // multi-node clock skew); the reader still filters events by the query.
    bool scan_all_chunks = false;

    ViewPlannerInput& with_view(const ViewDefinition& v);
    ViewPlannerInput& with_file_path(const std::string& path);
    ViewPlannerInput& with_index_path(const std::string& path);
    ViewPlannerInput& with_uncompressed_size(std::size_t s);
    ViewPlannerInput& with_num_checkpoints(std::size_t n);
    ViewPlannerInput& with_time_range(double begin, double end);
    ViewPlannerInput& with_scan_all_chunks(bool v);
};

struct ViewChunkCandidate {
    std::uint64_t checkpoint_idx = 0;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
};

// Success payload; failures are reported via Result<ViewPlannerOutput>.
struct ViewPlannerOutput {
    bool file_may_match = false;
    std::vector<ViewChunkCandidate> candidates;
    std::uint64_t total_checkpoints = 0;
    std::uint64_t skipped_checkpoints = 0;
};

struct ViewPlannerUtility {
    coro::CoroTask<Result<ViewPlannerOutput>> operator()(
        const ViewPlannerInput& input);
};

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLANNER_UTILITY_H
