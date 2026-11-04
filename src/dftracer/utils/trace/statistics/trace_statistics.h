// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_STATISTICS_TRACE_STATISTICS_H
#define DFTRACER_UTILS_TRACE_STATISTICS_TRACE_STATISTICS_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>

#include <cstdint>

namespace dftracer::utils::trace::statistics {

using dftracer::utils::index::schemas::dft::ChunkStatistics;

struct TraceStatistics {
    dstd::String file_path;
    dstd::String index_path;
    ChunkStatistics merged;
    std::uint64_t chunk_count = 0;
    bool success = false;
    dstd::String error_message;

    std::uint64_t total_events() const;
    double time_span_seconds() const;
    double duration_mean_us() const;
    double duration_stddev_us() const;
    std::size_t category_count() const;
    std::size_t unique_name_count() const;
    std::size_t pid_tid_count() const;

    dstd::String to_json() const;
};

}  // namespace dftracer::utils::trace::statistics

#endif  // DFTRACER_UTILS_TRACE_STATISTICS_TRACE_STATISTICS_H
