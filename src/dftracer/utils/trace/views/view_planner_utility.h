// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLANNER_UTILITY_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLANNER_UTILITY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/types.h>
#include <dftracer/utils/trace/views/view_definition.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftracer::utils::index {
class Snapshot;
}  // namespace dftracer::utils::index

namespace dftracer::utils::trace::views {

struct ViewPlannerInput {
    ViewDefinition view;
    dstd::String file_path;
    dstd::String index_path;  // `.dftindex` store path
    std::size_t uncompressed_size = 0;
    std::size_t num_checkpoints = 0;
    dstd::Optional<dstd::Pair<double, double>> time_range;  // {begin, end}
    // Emit every checkpoint (real byte offsets, still parallel) and skip
    // time-based pruning. For when chunk timestamp stats can't be trusted (e.g.
    // multi-node clock skew); the reader still filters events by the query.
    bool scan_all_chunks = false;
    /// Borrowed snapshot of `index_path`, held by the caller across many files.
    /// When null the planner takes its own.
    const index::Snapshot* snapshot = nullptr;

    ViewPlannerInput& with_view(const ViewDefinition& v);
    ViewPlannerInput& with_file_path(const dstd::String& path);
    ViewPlannerInput& with_index_path(const dstd::String& path);
    ViewPlannerInput& with_uncompressed_size(std::size_t s);
    ViewPlannerInput& with_num_checkpoints(std::size_t n);
    ViewPlannerInput& with_time_range(double begin, double end);
    ViewPlannerInput& with_scan_all_chunks(bool v);
    ViewPlannerInput& with_snapshot(const index::Snapshot* s);
};

struct ViewChunkCandidate {
    std::uint64_t checkpoint_idx = 0;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
};

// Success payload; failures are reported via dstd::Result<ViewPlannerOutput>.
struct ViewPlannerOutput {
    bool file_may_match = false;
    dstd::Vector<ViewChunkCandidate> candidates;
    std::uint64_t total_checkpoints = 0;
    std::uint64_t skipped_checkpoints = 0;
};

struct ViewPlannerUtility {
    coro::CoroTask<dstd::Result<ViewPlannerOutput>> operator()(
        const ViewPlannerInput& input);
};

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_PLANNER_UTILITY_H
