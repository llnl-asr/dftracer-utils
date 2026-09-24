#ifndef DFTRACER_UTILS_TRACE_STATISTICS_TRACE_STATISTICS_H
#define DFTRACER_UTILS_TRACE_STATISTICS_TRACE_STATISTICS_H

#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>

#include <cstdint>
#include <string>

namespace dftracer::utils::trace::statistics {

using dftracer::utils::index::schemas::dft::ChunkStatistics;

struct TraceStatistics {
    std::string file_path;
    std::string index_path;
    ChunkStatistics merged;
    std::uint64_t num_chunks = 0;
    bool success = false;
    std::string error_message;

    std::uint64_t total_events() const;
    double time_span_seconds() const;
    double duration_mean_us() const;
    double duration_stddev_us() const;
    std::size_t num_categories() const;
    std::size_t num_unique_names() const;
    std::size_t num_pid_tids() const;

    std::string to_json() const;
};

}  // namespace dftracer::utils::trace::statistics

#endif  // DFTRACER_UTILS_TRACE_STATISTICS_TRACE_STATISTICS_H
