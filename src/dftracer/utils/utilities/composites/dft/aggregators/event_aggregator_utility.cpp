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

    auto merge_into = [](AggregationMap& dst, AggregationMap& src) {
        for (auto& [key, metrics] : src) {
            auto it = dst.find(key);
            if (it == dst.end()) {
                dst.emplace(key, std::move(metrics));
            } else {
                it->second.merge_from(metrics);
            }
        }
    };
    merge_into(state_.aggregations, chunk_output.aggregations);
    merge_into(state_.profile_aggregations, chunk_output.profile_aggregations);
    merge_into(state_.system_aggregations, chunk_output.system_aggregations);

    if (chunk_output.local_tracker) {
        state_.trackers.push_back(std::move(chunk_output.local_tracker));
    }
}

EventAggregatorUtilityOutput EventAggregatorUtility::finalize() {
    state_.total_files_processed = unique_files_.size();
    state_.success = true;

    DFTRACER_UTILS_LOG_INFO(
        "Aggregation complete: %zu unique keys, %zu total events, %zu files",
        state_.aggregations.size(), state_.total_events_processed,
        state_.total_files_processed);

    return std::move(state_);
}

coro::CoroTask<EventAggregatorUtilityOutput> EventAggregatorUtility::process(
    const EventAggregatorUtilityInput& input) {
    for (auto& output : const_cast<std::vector<ChunkAggregationOutput>&>(
             input.chunk_outputs)) {
        merge_chunk(std::move(output));
    }

    co_return finalize();
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
