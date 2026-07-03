#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/event_id_extractor_utility.h>
#include <simdjson.h>

namespace dftracer::utils::utilities::composites::dft {

coro::CoroTask<EventIdExtractionOutput> EventIdExtractor::process(
    const EventIdExtractionInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("extract event ids");
    EventId event;

    simdjson::dom::parser parser;
    auto result = parser.parse(input.json_data.data(), input.json_data.size());
    if (result.error()) {
        co_return event;
    }

    auto root = result.value_unsafe();
    if (!root.is_object()) {
        co_return event;
    }

    extract_event_id(root, event);

    co_return event;
}

}  // namespace dftracer::utils::utilities::composites::dft
