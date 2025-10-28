#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_COLLECTOR_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_COLLECTOR_H

#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/reader/line_processor.h>
#include <dftracer/utils/utilities/composites/dft/chunk_extractor.h>
#include <dftracer/utils/utilities/composites/dft/event_id_extractor.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft {

// EventId is now defined in event_id_extractor.h

/**
 * @brief Input for event collection from DFTracer metadata.
 */
struct EventCollectorFromMetadataCollectorUtilityInput {
    std::vector<MetadataCollectorUtilityOutput> metadata;
    bool trim_commas{
        false};  // Set to true for JSON array format (with trailing commas)

    static EventCollectorFromMetadataCollectorUtilityInput from_metadata(
        std::vector<MetadataCollectorUtilityOutput> meta) {
        EventCollectorFromMetadataCollectorUtilityInput input;
        input.metadata = std::move(meta);
        return input;
    }

    EventCollectorFromMetadataCollectorUtilityInput& with_trim_commas(
        bool trim) {
        trim_commas = trim;
        return *this;
    }
};

/**
 * @brief Input for event collection from chunk results.
 */
struct EventCollectorFromChunksUtilityInput {
    std::vector<ChunkExtractorUtilityOutput> chunks;
    std::size_t checkpoint_size;

    static EventCollectorFromChunksUtilityInput from_chunks(
        std::vector<ChunkExtractorUtilityOutput> results) {
        EventCollectorFromChunksUtilityInput input;
        input.chunks = std::move(results);
        input.checkpoint_size = 0;
        return input;
    }

    EventCollectorFromChunksUtilityInput& with_checkpoint_size(
        std::size_t size) {
        checkpoint_size = size;
        return *this;
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
class EventCollectorFromMetadataUtility
    : public utilities::Utility<EventCollectorFromMetadataCollectorUtilityInput,
                                EventCollectorUtilityOutput> {
   public:
    EventCollectorUtilityOutput process(
        const EventCollectorFromMetadataCollectorUtilityInput& input) override;
};

/**
 * @brief Workflow for collecting event IDs from output chunk files.
 *
 * Reads chunk output files and extracts EventId from each valid JSON event.
 * Handles both compressed and uncompressed chunk files.
 */
class EventCollectorFromChunksUtility
    : public utilities::Utility<EventCollectorFromChunksUtilityInput,
                                EventCollectorUtilityOutput> {
   public:
    EventCollectorUtilityOutput process(
        const EventCollectorFromChunksUtilityInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_COLLECTOR_H
