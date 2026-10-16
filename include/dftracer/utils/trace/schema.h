#ifndef DFTRACER_UTILS_TRACE_SCHEMA_H
#define DFTRACER_UTILS_TRACE_SCHEMA_H

#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/json/parser.h>

#include <cstdint>
#include <string_view>

/// DFTracer on-disk format codecs. The current format writes "ph" and "type" as
/// small integers; legacy traces wrote "ph" as a single-letter Chrome-tracing
/// string and had no "type". Every reader routes both encodings through the
/// read_* helpers here so format handling lives in exactly one place, and every
/// writer emits the current integer form through the same enums.
///
/// Both numberings are append-only per the DFTracer spec: values are never
/// renumbered or reused, and an unrecognised value maps to Unknown rather than
/// failing.
namespace dftracer::utils::trace {

/// The "ph" column: what kind of record a line is.
enum class RecordPhase : std::uint8_t {
    Unknown = 0,
    Complete = 1,    ///< legacy "X"
    Counter = 2,     ///< legacy "C"
    Aggregated = 3,  ///< new; folded Complete events (was written as Counter)
    Metadata = 4,    ///< legacy "M"
};

/// The "type" column: the instrumentation layer that produced the event.
enum class EventType : std::uint8_t {
    Unknown = 0,
    Dftracer = 1,
    CApp = 2,
    LibcIo = 3,
    Hip = 4,
    Hdf5 = 5,
    Python = 6,
    Psutil = 7,
    Finstrument = 8,
    CppApp = 9,
    Mpi = 10,
};

constexpr RecordPhase phase_from_int(std::uint64_t v) {
    return v <= 4 ? static_cast<RecordPhase>(v) : RecordPhase::Unknown;
}

constexpr RecordPhase phase_from_letter(std::string_view s) {
    if (s.size() != 1) return RecordPhase::Unknown;
    switch (s[0]) {
        case 'X':
            return RecordPhase::Complete;
        case 'C':
            return RecordPhase::Counter;
        case 'A':
            return RecordPhase::Aggregated;
        case 'M':
            return RecordPhase::Metadata;
        default:
            return RecordPhase::Unknown;
    }
}

constexpr int phase_to_int(RecordPhase p) { return static_cast<int>(p); }

/// The Chrome-tracing letter a phase superseded, for human-readable output that
/// still wants the old glyph; '\0' for Unknown.
constexpr char phase_to_letter(RecordPhase p) {
    switch (p) {
        case RecordPhase::Complete:
            return 'X';
        case RecordPhase::Counter:
            return 'C';
        case RecordPhase::Aggregated:
            return 'A';
        case RecordPhase::Metadata:
            return 'M';
        default:
            return '\0';
    }
}

constexpr EventType event_type_from_int(std::uint64_t v) {
    return v <= 10 ? static_cast<EventType>(v) : EventType::Unknown;
}

constexpr int event_type_to_int(EventType t) { return static_cast<int>(t); }

/// Best-effort layer inference from "cat" for legacy traces that carry no
/// "type". Only the unambiguous, closed-vocabulary cats map; anything else
/// stays Unknown so a reader downstream can decide.
constexpr EventType event_type_from_cat(std::string_view cat) {
    if (cat == "POSIX" || cat == "STDIO") return EventType::LibcIo;
    if (cat == "MPI" || cat == "MPIIO") return EventType::Mpi;
    if (cat == "sys" || cat == "net" || cat == "io") return EventType::Psutil;
    if (cat == "HDF5") return EventType::Hdf5;
    if (cat == "FUNC") return EventType::Finstrument;
    return EventType::Unknown;
}

// ---- read_phase: accept the integer (current) or letter (legacy) form ----

inline RecordPhase read_phase(const json::JsonValue& v) {
    if (v.is_uint()) return phase_from_int(v.get<std::uint64_t>());
    if (v.is_string()) return phase_from_letter(v.get<std::string_view>());
    return RecordPhase::Unknown;
}

inline RecordPhase read_phase(simdjson::dom::element v) {
    if (v.is_uint64()) return phase_from_int(v.get_uint64().value_unsafe());
    if (v.is_string()) return phase_from_letter(v.get_string().value_unsafe());
    return RecordPhase::Unknown;
}

/// On-demand values are single-use; type() is checked before the one consuming
/// accessor so the parser cursor advances exactly once.
inline RecordPhase read_phase(simdjson::ondemand::value& v) {
    auto t = v.type();
    if (t.error()) return RecordPhase::Unknown;
    if (t.value_unsafe() == simdjson::ondemand::json_type::number) {
        auto r = v.get_uint64();
        if (!r.error()) return phase_from_int(r.value_unsafe());
    } else if (t.value_unsafe() == simdjson::ondemand::json_type::string) {
        auto r = v.get_string();
        if (!r.error()) return phase_from_letter(r.value_unsafe());
    }
    return RecordPhase::Unknown;
}

inline RecordPhase read_phase(json::JsonParser& parser,
                              std::string_view key = "ph") {
    auto v = parser.get_value(key);
    if (!v) return RecordPhase::Unknown;
    return read_phase(*v);
}

// ---- read_event_type: integer form, or inferred from cat when absent ----

inline EventType read_event_type(const json::JsonValue& v) {
    if (v.is_uint()) return event_type_from_int(v.get<std::uint64_t>());
    return EventType::Unknown;
}

inline EventType read_event_type(simdjson::dom::element v) {
    if (v.is_uint64())
        return event_type_from_int(v.get_uint64().value_unsafe());
    return EventType::Unknown;
}

inline EventType read_event_type(simdjson::ondemand::value& v) {
    auto r = v.get_uint64();
    if (!r.error()) return event_type_from_int(r.value_unsafe());
    return EventType::Unknown;
}

}  // namespace dftracer::utils::trace

#endif  // DFTRACER_UTILS_TRACE_SCHEMA_H
