// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/json/dom_form.h>
#include <dftracer/utils/json/line.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/trace/event_collector_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <simdjson.h>

#include <algorithm>

namespace dftracer::utils::trace {

/**
 * @brief LineProcessor that collects EventIds from JSON events.
 */
class EventIdCollector : public utilities::reader::internal::LineProcessor {
   public:
    dstd::Vector<EventId>& events;
    bool trim_commas;
    std::size_t dropped = 0;
    dftracer::utils::json::RecordParser parser;

    explicit EventIdCollector(dstd::Vector<EventId>& event_list,
                              bool should_trim_commas = false)
        : events(event_list), trim_commas(should_trim_commas) {}

    coro::CoroTask<bool> process(const char* data,
                                 std::size_t length) override {
        const char* trimmed;
        std::size_t trimmed_size;

        bool valid = trim_commas ? json::trim_and_validate_with_comma(
                                       data, length, trimmed, trimmed_size)
                                 : json::trim_and_validate(
                                       data, length, trimmed, trimmed_size);

        if (!valid || trimmed_size <= 8) {
            co_return true;
        }

        auto result = parser.parse(trimmed, trimmed_size);
        if (result.error())
            result = json::parse_refused(
                parser, dstd::StringView(trimmed, trimmed_size));
        if (result.error()) {
            ++dropped;
            co_return true;
        }

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
EventCollectorFromMetadataUtility::operator()(
    const EventCollectorFromMetadataCollectorUtilityInput& input) const {
    dstd::Vector<EventId> events;

    for (const auto& file : input.metadata) {
        if (!file.success) {
            DFTRACER_UTILS_LOG_WARN("Skipping unsuccessful file: {}",
                                    file.file_path);
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
            auto reader = common::unwrap(
                utilities::reader::internal::ReaderFactory::create(
                    file.file_path, file.index_path));
            auto processed = co_await reader->read_lines_with_processor_async(
                file.start_line, file.end_line, collector);
            common::unwrap(std::move(processed));
        } else {
            auto line_gen =
                utilities::fileio::lines::sources::async_streaming_gz_lines(
                    dstd::String(file.file_path), file.start_line,
                    file.end_line);
            for (;;) {
                auto line_opt = co_await line_gen.next();
                if (!line_opt) break;
                if (!*line_opt) common::throw_error(line_opt->error());
                const auto& line = **line_opt;
                co_await collector.process(line.content.data(),
                                           line.content.length());
            }
        }

        if (collector.dropped > 0)
            DFTRACER_UTILS_LOG_WARN("{}: {} line(s) could not be read in full",
                                    file.file_path, collector.dropped);

#if DFTRACER_UTILS_LOGGER_DEBUG_ENABLED == 1
        std::size_t events_collected = events.size() - events_before;
        DFTRACER_UTILS_LOG_DEBUG(
            "Collected {} events from file {} (expected {})", events_collected,
            file.file_path, file.valid_events);
#endif
    }

    // Sort events for consistent hashing
    std::sort(events.begin(), events.end());

    DFTRACER_UTILS_LOG_INFO(
        "EventCollectorFromMetadata: Collected {} events from {} metadata "
        "files",
        events.size(), input.metadata.size());

    co_return events;
}

}  // namespace dftracer::utils::trace
