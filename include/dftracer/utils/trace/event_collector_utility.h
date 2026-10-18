#ifndef DFTRACER_UTILS_TRACE_EVENT_COLLECTOR_H
#define DFTRACER_UTILS_TRACE_EVENT_COLLECTOR_H

#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/trace/event_id.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <dftracer/utils/utilities/reader/internal/line_processor.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::trace {

/**
 * @brief Input for event collection from DFTracer metadata.
 */
struct EventCollectorFromMetadataCollectorUtilityInput {
    std::vector<MetadataCollectorUtilityOutput> metadata;
    bool trim_commas{
        false};  ///< Set to true for JSON array format (with trailing commas)

    static EventCollectorFromMetadataCollectorUtilityInput from_metadata(
        std::vector<MetadataCollectorUtilityOutput> meta) {
        EventCollectorFromMetadataCollectorUtilityInput input;
        input.metadata = std::move(meta);
        return input;
    }
};

/**
 * @brief Output: vector of collected EventIds.
 */
using EventCollectorUtilityOutput = std::vector<EventId>;

/**
 * @brief Workflow for collecting event IDs from DFTracer metadata files.
 *
 * Reads files specified in metadata and extracts EventId (id, pid, tid)
 * from each valid JSON event.
 */
struct EventCollectorFromMetadataUtility {
    coro::CoroTask<EventCollectorUtilityOutput> operator()(
        const EventCollectorFromMetadataCollectorUtilityInput& input) const;
};

}  // namespace dftracer::utils::trace

#endif  // DFTRACER_UTILS_TRACE_EVENT_COLLECTOR_H
