#ifndef DFTRACER_UTILS_TRACE_COMPARATOR_COMPARISON_AGGREGATION_H
#define DFTRACER_UTILS_TRACE_COMPARATOR_COMPARISON_AGGREGATION_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_output.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::trace::comparator {

/// Aggregate `input_files` into one in-memory EventAggregatorOutput on `ctx`:
/// per file, map it into chunks and fan the chunks across `executor_threads`
/// ChunkAggregator workers, all sharing one intern table, then merge the chunk
/// outputs. The single fan-out shared by dftracer_comparator and the Python
/// ComparatorUtility so the two stay in lock step.
coro::CoroTask<dftracer::utils::index::schemas::dft::agg::EventAggregatorOutput>
run_comparison_aggregation(
    CoroScope& ctx, const std::vector<std::string>& input_files,
    const dftracer::utils::index::schemas::dft::agg::AggregationConfig&
        agg_config,
    const std::optional<duql::Query>& query, const std::string& index_dir,
    std::size_t checkpoint_size, bool force_rebuild,
    std::size_t executor_threads);

}  // namespace dftracer::utils::trace::comparator

#endif  // DFTRACER_UTILS_TRACE_COMPARATOR_COMPARISON_AGGREGATION_H
