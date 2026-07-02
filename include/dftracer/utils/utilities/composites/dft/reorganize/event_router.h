#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_EVENT_ROUTER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_EVENT_ROUTER_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>

#include <cstddef>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::reorganize {

struct EventRouterConfig {
    ExtractionPlan plan;
    std::string output_dir;
    std::string index_dir;
    std::size_t chunk_size_bytes = 256 * 1024 * 1024;
    std::size_t checkpoint_size = 0;
    std::size_t executor_threads = 4;
    bool compress = true;
    bool verify = false;
};

struct EventRouterResult {
    std::size_t total_events_written = 0;
    std::size_t total_bytes_written = 0;
    std::size_t chunks_created = 0;
    std::size_t source_files_processed = 0;
    std::vector<std::string> output_files;
};

coro::CoroTask<Result<EventRouterResult>> route_events(
    CoroScope& scope, const EventRouterConfig& config);

}  // namespace dftracer::utils::utilities::composites::dft::reorganize

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_EVENT_ROUTER_H
