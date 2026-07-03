#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/dft/event_collector_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_plain_file_line_generator.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <simdjson.h>

#include <algorithm>

namespace dftracer::utils::utilities::composites::dft {

/**
 * @brief LineProcessor that collects EventIds from JSON events.
 */
class EventIdCollector : public reader::internal::LineProcessor {
   public:
    std::vector<EventId>& events;
    bool trim_commas;
    simdjson::dom::parser parser;

    explicit EventIdCollector(std::vector<EventId>& event_list,
                              bool should_trim_commas = false)
        : events(event_list), trim_commas(should_trim_commas) {}

    coro::CoroTask<bool> process(const char* data,
                                 std::size_t length) override {
        const char* trimmed;
        std::size_t trimmed_length;

        bool valid = trim_commas ? json_trim_and_validate_with_comma(
                                       data, length, trimmed, trimmed_length)
                                 : json_trim_and_validate(data, length, trimmed,
                                                          trimmed_length);

        if (!valid || trimmed_length <= 8) {
            co_return true;
        }

        auto result = parser.parse(trimmed, trimmed_length);
        if (result.error()) co_return true;

        auto root = result.value_unsafe();
        if (!root.is_object()) co_return true;

        EventId event;
        extract_event_id(root, event);

        if (event.is_valid()) {
            events.push_back(event);
        }

        co_return true;
    }
};

coro::CoroTask<EventCollectorUtilityOutput>
EventCollectorFromMetadataUtility::process(
    const EventCollectorFromMetadataCollectorUtilityInput& input) {
    std::vector<EventId> events;

    for (const auto& file : input.metadata) {
        if (!file.success) {
            DFTRACER_UTILS_LOG_WARN("Skipping unsuccessful file: %s",
                                    file.file_path.c_str());
            continue;
        }

        // Skip files with no valid line range (e.g., empty files)
        if (file.start_line == 0 || file.end_line == 0) {
            continue;
        }

#if DFTRACER_UTILS_LOGGER_DEBUG_ENABLED == 1
        std::size_t events_before = events.size();
#endif
        EventIdCollector collector(events, input.trim_commas);

        if (!file.index_path.empty()) {
            // Indexed/compressed file
            auto reader = reader::internal::ReaderFactory::create(
                file.file_path, file.index_path);
            if (!reader) {
                DFTRACER_UTILS_LOG_ERROR("Failed to create reader for file: %s",
                                         file.file_path.c_str());
                continue;
            }
            co_await reader->read_lines_with_processor_async(
                file.start_line, file.end_line, collector);
        } else {
            // Plain text file — async line generator
            auto line_gen = fileio::lines::sources::async_plain_file_lines(
                file.file_path, file.start_line, file.end_line);
            while (auto line_opt = co_await line_gen.next()) {
                co_await collector.process(line_opt->content.data(),
                                           line_opt->content.length());
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

    co_return events;
}

}  // namespace dftracer::utils::utilities::composites::dft
