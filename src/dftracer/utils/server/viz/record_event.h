#ifndef DFTRACER_UTILS_SERVER_VIZ_RECORD_EVENT_H
#define DFTRACER_UTILS_SERVER_VIZ_RECORD_EVENT_H

// The trace fields of the served records, read through their record schema.
// The viz folds read trace events (name, pid, tid, ts, dur in microseconds);
// a path-decoded record becomes one at the scan boundary. Internal to the
// server.

#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/json/record_parser.h>
#include <simdjson.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::server {

// The value at dotted `path` under `root`.
inline std::optional<simdjson::dom::element> find_path(
    simdjson::dom::element root, std::string_view path) {
    simdjson::dom::element cur = root;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto dot = path.find('.', start);
        const auto key = path.substr(start, dot == std::string_view::npos
                                                ? path.size() - start
                                                : dot - start);
        if (!key.empty()) {
            if (!cur.is_object()) return std::nullopt;
            auto next = cur[key];
            if (next.error()) return std::nullopt;
            cur = next.value_unsafe();
        }
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return cur;
}

// A scalar as display text; "" for null, objects and arrays.
inline std::string scalar_text(simdjson::dom::element el) {
    if (el.is_string()) return std::string(el.get_string().value_unsafe());
    if (el.is_int64()) return std::to_string(el.get_int64().value_unsafe());
    if (el.is_uint64()) return std::to_string(el.get_uint64().value_unsafe());
    if (el.is_double()) return std::to_string(el.get_double().value_unsafe());
    if (el.is_bool()) return el.get_bool().value_unsafe() ? "true" : "false";
    return "";
}

// The timeline id of an entity or lane value (index::entity_id).
inline std::int64_t lane_id(simdjson::dom::element el) {
    if (el.is_int64()) return el.get_int64().value_unsafe();
    if (el.is_uint64())
        return static_cast<std::int64_t>(el.get_uint64().value_unsafe());
    if (el.is_double()) {
        const double d = el.get_double().value_unsafe();
        if (auto whole = dftracer::utils::index::whole_int64(d)) return *whole;
    }
    return dftracer::utils::index::entity_id(scalar_text(el));
}

// Where each trace field of the served records lives. dftracer traces keep
// their own names; a path schema maps them to its roles and a label field.
struct TraceFields {
    bool by_path = false;
    std::string time = "ts";
    std::string duration = "dur";
    std::string entity = "pid";
    std::string lane = "tid";
    std::string label = "name";
    // The entity field's name, which keys the source's hosts and ranks rows.
    std::string entity_name = "pid";
    // Microseconds per unit of time and duration.
    double time_us = 1;
    double duration_us = 1;
    // The event name of a record without a label.
    std::string fallback_label;

    explicit TraceFields(const dftracer::utils::index::RecordSchema& s) {
        namespace ix = dftracer::utils::index;
        if (s.decoder != ix::Decoder::PATH) return;
        by_path = true;
        time = s.roles.time;
        duration = s.roles.duration;
        entity = s.roles.entity;
        lane = s.roles.lane;
        time_us = ix::micros_per(s.roles.time_unit);
        duration_us = ix::micros_per(s.roles.duration_unit);
        fallback_label = s.id;
        // The label is the name role, else the field named "name", else the
        // first string field without a role.
        label = s.roles.name;
        entity_name.clear();
        for (const auto& f : s.fields) {
            if (f.role == ix::Role::ENTITY) entity_name = f.name;
            if (label.empty() && f.name == "name") label = f.path;
        }
        for (const auto& f : s.fields)
            if (label.empty() && f.role == ix::Role::NONE &&
                f.type == ix::FieldType::STRING)
                label = f.path;
    }

    // `raw` as a trace event: name (the label), pid (the entity), tid (the
    // lane), ts and dur in microseconds (dur 0 for an instant, ts absent for
    // a record without a time), ph 1 and args (the record itself). False
    // when `raw` is not a JSON object.
    bool to_event(std::string_view raw, std::string& out) const {
        thread_local dftracer::utils::json::RecordParser parser;
        thread_local std::string buf;
        thread_local simdjson::builder::string_builder b;
        buf.assign(raw);
        buf.append(simdjson::SIMDJSON_PADDING, '\0');
        auto res = parser.parse(buf.data(), raw.size(), false);
        if (res.error() || !res.value_unsafe().is_object()) return false;
        const auto root = res.value_unsafe();
        auto at = [&](const std::string& path) {
            return path.empty() ? std::nullopt : find_path(root, path);
        };
        auto number = [](std::optional<simdjson::dom::element> el)
            -> std::optional<double> {
            if (!el || !(el->is_int64() || el->is_uint64() || el->is_double()))
                return std::nullopt;
            return el->get_double().value_unsafe();
        };
        b.clear();
        b.start_object();
        const auto name = at(label);
        const std::string text = name ? scalar_text(*name) : std::string();
        b.append_key_value(
            "name", std::string_view(text.empty() ? fallback_label : text));
        b.append_comma();
        const auto pid = at(entity);
        b.append_key_value("pid", pid ? lane_id(*pid) : std::int64_t{0});
        b.append_comma();
        const auto tid = at(lane);
        b.append_key_value("tid", tid ? lane_id(*tid) : std::int64_t{0});
        if (const auto ts = number(at(time))) {
            b.append_comma();
            b.append_key_value("ts", std::llround(*ts * time_us));
        }
        const auto dur = number(at(duration));
        b.append_comma();
        b.append_key_value("dur", dur ? std::llround(*dur * duration_us) : 0LL);
        b.append_comma();
        b.append_key_value("ph", 1);
        b.append_comma();
        b.escape_and_append_with_quotes("args");
        b.append_colon();
        b.append_raw(raw);
        b.end_object();
        out.assign(std::string_view(b));
        return true;
    }

    // Calls `fn(root, event)` with the path record `raw` read as a trace
    // event, parsed; both last until `fn` returns.
    template <typename Fn>
    void with_event(std::string_view raw, Fn&& fn) const {
        thread_local dftracer::utils::json::RecordParser parser;
        thread_local std::string ev;
        thread_local std::string buf;
        if (!to_event(raw, ev)) return;
        buf.assign(ev);
        buf.append(simdjson::SIMDJSON_PADDING, '\0');
        auto res = parser.parse(buf.data(), ev.size(), false);
        if (!res.error()) fn(res.value_unsafe(), std::string_view(ev));
    }

    // Calls `fn` with each of `events` as a trace event; the view it gets
    // lasts until `fn` returns.
    template <typename Fn>
    void for_each_event(const std::vector<std::string_view>& events,
                        Fn&& fn) const {
        if (!by_path) {
            for (auto e : events) fn(e);
            return;
        }
        thread_local std::string ev;
        for (auto e : events)
            if (to_event(e, ev)) fn(std::string_view(ev));
    }
};

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_RECORD_EVENT_H
