#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_ID_EXTRACTOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_ID_EXTRACTOR_UTILITY_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/utilities.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft {

/**
 * @brief Simple event identifier (id, pid, tid).
 */
struct EventId {
    std::int64_t id;
    std::int64_t pid;
    std::int64_t tid;

    EventId() : id(-1), pid(-1), tid(-1) {}
    EventId(std::int64_t i, std::int64_t p, std::int64_t t)
        : id(i), pid(p), tid(t) {}

    bool operator<(const EventId& other) const {
        if (id != other.id) return id < other.id;
        if (pid != other.pid) return pid < other.pid;
        return tid < other.tid;
    }

    bool operator==(const EventId& other) const {
        return id == other.id && pid == other.pid && tid == other.tid;
    }

    bool is_valid() const { return id > 0; }
};

// Fill id/pid/tid on `event` from a parsed JSON object `root`. Missing or
// non-integer fields are left untouched. Templated on the element type to keep
// the simdjson dependency out of this header. Zero-copy.
template <typename Element>
void extract_event_id(const Element& root, EventId& event) {
    auto id_result = root["id"].get_int64();
    if (!id_result.error()) event.id = id_result.value_unsafe();

    auto pid_result = root["pid"].get_int64();
    if (!pid_result.error()) event.pid = pid_result.value_unsafe();

    auto tid_result = root["tid"].get_int64();
    if (!tid_result.error()) event.tid = tid_result.value_unsafe();
}

/**
 * @brief Input for event ID extraction.
 */
struct EventIdExtractionInput {
    std::string_view json_data;

    static EventIdExtractionInput from_json(std::string_view json) {
        EventIdExtractionInput input;
        input.json_data = json;
        return input;
    }
};

/**
 * @brief Output from event ID extraction.
 */
using EventIdExtractionOutput = EventId;

/**
 * @brief Utility for extracting EventId from JSON.
 *
 * This is a composable utility that follows the Utility<Input, Output> pattern.
 * It has no I/O dependencies and can be easily tested and composed.
 *
 * Intent: "Parse JSON and extract (id, pid, tid)"
 */
class EventIdExtractor : public utilities::Utility<EventIdExtractionInput,
                                                   EventIdExtractionOutput> {
   public:
    coro::CoroTask<EventIdExtractionOutput> process(
        const EventIdExtractionInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_ID_EXTRACTOR_UTILITY_H
