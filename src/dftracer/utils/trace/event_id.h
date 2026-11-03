// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_EVENT_ID_H
#define DFTRACER_UTILS_TRACE_EVENT_ID_H

#include <cstdint>

namespace dftracer::utils::trace {

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

/// Fill id/pid/tid on `event` from a parsed JSON object `root`. Missing or
/// non-integer fields are left untouched. Templated on the element type to keep
/// the simdjson dependency out of this header. Zero-copy.
template <typename Element>
void extract_event_id(const Element& root, EventId& event) {
    auto id_result = root["id"].get_int64();
    if (!id_result.error()) event.id = id_result.value_unsafe();

    auto pid_result = root["pid"].get_int64();
    if (!pid_result.error()) event.pid = pid_result.value_unsafe();

    auto tid_result = root["tid"].get_int64();
    if (!tid_result.error()) event.tid = tid_result.value_unsafe();
}

}  // namespace dftracer::utils::trace

#endif  // DFTRACER_UTILS_TRACE_EVENT_ID_H
