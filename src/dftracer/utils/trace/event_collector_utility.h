// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_EVENT_COLLECTOR_H
#define DFTRACER_UTILS_TRACE_EVENT_COLLECTOR_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/event_id.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <dftracer/utils/utilities/reader/internal/line_processor.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::trace {

/**
 * @brief Input for event collection from DFTracer metadata.
 */
struct EventCollectorFromMetadataCollectorUtilityInput {
    dstd::Vector<MetadataCollectorUtilityOutput> metadata;
    bool trim_commas{
        false};  ///< Set to true for JSON array format (with trailing commas)

    static EventCollectorFromMetadataCollectorUtilityInput from_metadata(
        dstd::Vector<MetadataCollectorUtilityOutput> meta) {
        EventCollectorFromMetadataCollectorUtilityInput input;
        input.metadata = std::move(meta);
        return input;
    }
};

/**
 * @brief Output: vector of collected EventIds.
 */
using EventCollectorUtilityOutput = dstd::Vector<EventId>;

/**
 * @brief Workflow for collecting event IDs from DFTracer metadata files.
 *
 * Reads files specified in metadata and extracts @ref EventId (id, pid, tid)
 * from each valid JSON event. A file that cannot be opened or read gives the
 * error of that read.
 */
struct EventCollectorFromMetadataUtility {
    coro::CoroTask<dstd::Result<EventCollectorUtilityOutput>> operator()(
        const EventCollectorFromMetadataCollectorUtilityInput& input) const;
};

}  // namespace dftracer::utils::trace

#endif  // DFTRACER_UTILS_TRACE_EVENT_COLLECTOR_H
