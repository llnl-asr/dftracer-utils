#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/event_id_extractor_utility.h>
#include <simdjson.h>

namespace dftracer::utils::utilities::composites::dft {

coro::CoroTask<EventIdExtractionOutput> EventIdExtractor::process(
    const EventIdExtractionInput& input) {
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

    auto id_result = root["id"].get_int64();
    if (!id_result.error()) {
        event.id = id_result.value_unsafe();
    }

    auto pid_result = root["pid"].get_int64();
    if (!pid_result.error()) {
        event.pid = pid_result.value_unsafe();
    }

    auto tid_result = root["tid"].get_int64();
    if (!tid_result.error()) {
        event.tid = tid_result.value_unsafe();
    }

    co_return event;
}

}  // namespace dftracer::utils::utilities::composites::dft
