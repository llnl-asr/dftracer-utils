#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_H

/**
 * @file event.h
 * @brief Common DFTracer event representation and parser.
 *
 * Provides DFTracerEvent, a lightweight struct capturing the core fields
 * of a Chrome Tracing / DFTracer event.  All string fields are string_view
 * into the yyjson document (valid only while the doc lives).
 */

#include <dftracer/utils/utilities/common/json/json_value.h>

#include <cstdint>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft {

using common::json::JsonValue;

/**
 * Parsed DFTracer trace event.
 *
 * All string_view fields point into the yyjson document memory and are
 * only valid while the document is alive.
 *
 * Typical usage:
 *   DFTracerEvent ev;
 *   if (DFTracerEvent::parse(json, ev)) {
 *       if (ev.is_complete()) { ... }
 *   }
 */
struct DFTracerEvent {
    // Core Chrome Tracing fields
    std::uint64_t id = 0;
    std::string_view name;
    std::string_view cat;
    std::string_view ph;  // "X" (complete), "M" (metadata), "B"/"E", etc.
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;

    // Args subtree, lazy, just the yyjson_val* pointer.
    // Access via args["field"] or pass to evaluator/bloom.
    JsonValue args;

    // Convenience predicates
    bool is_metadata() const { return ph == "M"; }
    bool is_counter() const { return ph == "C"; }
    bool is_profile() const { return ph == "C" && cat != "sys"; }
    bool is_system() const { return ph == "C" && cat == "sys"; }
    bool is_event() const { return !is_metadata() && !is_counter(); }
    bool is_complete() const { return ph == "X"; }
    bool has_id() const { return id != 0; }

    /**
     * Parse from a JsonValue (wrapping a yyjson_val* object root).
     * Returns true if the JSON is a valid object with at least "ph".
     * Fields that are absent get their default (0 / empty).
     */
    static bool parse(const JsonValue& json, DFTracerEvent& out) {
        auto ph_val = json["ph"];
        if (!ph_val.exists()) return false;

        out.ph = ph_val.get<std::string_view>();

        auto id_val = json["id"];
        if (id_val.exists()) out.id = id_val.get<std::uint64_t>();

        auto name_val = json["name"];
        if (name_val.exists()) out.name = name_val.get<std::string_view>();

        auto cat_val = json["cat"];
        if (cat_val.exists()) out.cat = cat_val.get<std::string_view>();

        auto pid_val = json["pid"];
        if (pid_val.exists()) out.pid = pid_val.get<std::uint64_t>();

        auto tid_val = json["tid"];
        if (tid_val.exists()) out.tid = tid_val.get<std::uint64_t>();

        auto ts_val = json["ts"];
        if (ts_val.exists()) out.ts = ts_val.get<std::uint64_t>();

        auto dur_val = json["dur"];
        if (dur_val.exists()) out.dur = dur_val.get<std::uint64_t>();

        auto args_val = json["args"];
        if (args_val.exists()) out.args = args_val;

        return true;
    }

    /**
     * Parse from a raw yyjson_val* root (for call sites that don't use
     * JsonValue).  Same semantics as the JsonValue overload.
     */
    static bool parse(yyjson_val* root, DFTracerEvent& out) {
        if (!root || !yyjson_is_obj(root)) return false;
        return parse(JsonValue(root), out);
    }
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_H
