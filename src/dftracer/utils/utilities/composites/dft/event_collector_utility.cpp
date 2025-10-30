#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/dft/event_collector_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <yyjson.h>

#include <algorithm>
#include <fstream>

namespace dftracer::utils::utilities::composites::dft {

/**
 * @brief LineProcessor that collects EventIds from JSON events.
 */
class EventIdCollector : public reader::internal::LineProcessor {
   public:
    std::vector<EventId>& events;
    bool trim_commas;

    explicit EventIdCollector(std::vector<EventId>& event_list,
                              bool should_trim_commas = false)
        : events(event_list), trim_commas(should_trim_commas) {}

    bool process(const char* data, std::size_t length) override {
        const char* trimmed;
        std::size_t trimmed_length;

        // Use comma-trimming variant if requested (for JSON array format)
        bool valid = trim_commas ? json_trim_and_validate_with_comma(
                                       data, length, trimmed, trimmed_length)
                                 : json_trim_and_validate(data, length, trimmed,
                                                          trimmed_length);

        if (!valid || trimmed_length <= 8) {
            return true;
        }

        yyjson_doc* doc = yyjson_read(trimmed, trimmed_length, 0);
        if (!doc) return true;

        yyjson_val* root = yyjson_doc_get_root(doc);
        if (!yyjson_is_obj(root)) {
            yyjson_doc_free(doc);
            return true;
        }

        EventId event;
        yyjson_val* id_val = yyjson_obj_get(root, "id");
        if (id_val && yyjson_is_int(id_val)) {
            event.id = yyjson_get_int(id_val);
        }

        yyjson_val* pid_val = yyjson_obj_get(root, "pid");
        if (pid_val && yyjson_is_int(pid_val)) {
            event.pid = yyjson_get_int(pid_val);
        }

        yyjson_val* tid_val = yyjson_obj_get(root, "tid");
        if (tid_val && yyjson_is_int(tid_val)) {
            event.tid = yyjson_get_int(tid_val);
        }

        if (event.is_valid()) {
            events.push_back(event);
        }

        yyjson_doc_free(doc);
        return true;
    }
};

EventCollectorUtilityOutput EventCollectorFromMetadataUtility::process(
    const EventCollectorFromMetadataCollectorUtilityInput& input) {
    std::vector<EventId> events;

    for (const auto& file : input.metadata) {
        if (!file.success) {
            DFTRACER_UTILS_LOG_WARN("Skipping unsuccessful file: %s",
                                    file.file_path.c_str());
            continue;
        }

#if DFTRACER_UTILS_LOGGER_DEBUG_ENABLED == 1
        std::size_t events_before = events.size();
#endif
        EventIdCollector collector(events, input.trim_commas);

        if (!file.idx_path.empty()) {
            // Indexed/compressed file
            auto reader = reader::internal::ReaderFactory::create(
                file.file_path, file.idx_path);
            if (!reader) {
                DFTRACER_UTILS_LOG_ERROR("Failed to create reader for file: %s",
                                         file.file_path.c_str());
                continue;
            }
            reader->read_lines_with_processor(file.start_line, file.end_line,
                                              collector);
        } else {
            // Plain text file
            std::ifstream infile(file.file_path);
            if (!infile.is_open()) {
                DFTRACER_UTILS_LOG_WARN("Cannot open file: %s",
                                        file.file_path.c_str());
                continue;
            }

            std::string line;
            std::size_t current_line = 0;
            while (std::getline(infile, line)) {
                current_line++;
                if (current_line < file.start_line) continue;
                if (current_line > file.end_line) break;
                collector.process(line.c_str(), line.length());
            }
        }

#if DFTRACER_UTILS_LOGGER_DEBUG_ENABLED == 1
        std::size_t events_collected = events.size() - events_before;
        DFTRACER_UTILS_LOG_DEBUG(
            "Collected %zu events from file %s (expected %zu)",
            events_collected, file.file_path.c_str(), file.valid_events);
#endif
    }

    // Sort events for consistent hashing
    std::sort(events.begin(), events.end());

    DFTRACER_UTILS_LOG_INFO(
        "EventCollectorFromMetadata: Collected %zu events from %zu metadata "
        "files",
        events.size(), input.metadata.size());

    return events;
}

EventCollectorUtilityOutput EventCollectorFromChunksUtility::process(
    const EventCollectorFromChunksUtilityInput& input) {
    // OPTIMIZATION: No file reading! Just aggregate event IDs from extraction
    // results Event IDs were already collected during chunk extraction

    std::vector<EventId> events;

    for (const auto& chunk : input.chunks) {
        if (!chunk.success) continue;

        // Simply copy the event IDs that were collected during extraction
        events.insert(events.end(), chunk.event_ids.begin(),
                      chunk.event_ids.end());
    }

    // Sort events for consistent hashing
    std::sort(events.begin(), events.end());

    DFTRACER_UTILS_LOG_DEBUG(
        "EventCollectorFromChunks: Aggregated %zu events from %zu chunks (no "
        "file reads)",
        events.size(), input.chunks.size());

    return events;
}

}  // namespace dftracer::utils::utilities::composites::dft
