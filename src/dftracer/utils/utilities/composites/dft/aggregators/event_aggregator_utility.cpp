#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator_utility.h>

namespace dftracer::utils::utilities::composites::dft::aggregators {

void EventAggregatorUtility::merge_chunk(
    ChunkAggregationOutput&& chunk_output) {
    if (!chunk_output.success) return;

    state_.total_events_processed += chunk_output.events_processed;
    state_.total_bytes_processed += chunk_output.bytes_processed;
    unique_files_.insert(chunk_output.file_path);

    if (chunk_output.local_tracker) {
        state_.trackers.push_back(std::move(chunk_output.local_tracker));
    }

    for (auto& [key, metrics] : chunk_output.aggregations) {
        auto it = state_.aggregations.find(key);
        if (it == state_.aggregations.end()) {
            it = state_.aggregations
                     .emplace(key, AggregationMetrics(metrics.sketch_accuracy))
                     .first;
        }
        auto& merged_metrics = it->second;

        merged_metrics.merge_from(metrics);

        if (merged_metrics.boundary_associations.empty() &&
            !metrics.boundary_associations.empty()) {
            merged_metrics.boundary_associations =
                std::move(metrics.boundary_associations);
        }
        if (merged_metrics.parent_pid == 0 && metrics.parent_pid != 0) {
            merged_metrics.parent_pid = metrics.parent_pid;
        }
    }
}

EventAggregatorUtilityOutput EventAggregatorUtility::finalize() {
    state_.total_files_processed = unique_files_.size();
    state_.success = true;

    DFTRACER_UTILS_LOG_INFO(
        "Aggregation merge complete: %zu unique keys, %zu total events, "
        "%zu files",
        state_.aggregations.size(), state_.total_events_processed,
        state_.total_files_processed);

    return std::move(state_);
}

coro::CoroTask<EventAggregatorUtilityOutput> EventAggregatorUtility::process(
    const EventAggregatorUtilityInput& input) {
    DFTRACER_UTILS_LOG_INFO("Merging %zu chunk aggregations...",
                            input.chunk_outputs.size());

    for (const auto& output : input.chunk_outputs) {
        ChunkAggregationOutput copy = output;
        merge_chunk(std::move(copy));
    }

    co_return finalize();
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
