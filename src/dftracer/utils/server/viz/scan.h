#ifndef DFTRACER_UTILS_SERVER_VIZ_SCAN_H
#define DFTRACER_UTILS_SERVER_VIZ_SCAN_H

// Shared viz scan/query plumbing: event ts normalization, the duration
// threshold, viz-query (ViewDefinition) building from request params, and
// target-file selection. Internal to the server.

#include <dftracer/utils/core/common/expected.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz/record_event.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <simdjson.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dftracer::utils::server {

using trace::views::ViewDefinition;

// `value`. No-op when the key is absent. Returns whether it rewrote.
inline bool rewrite_uint_field(std::string& json, std::string_view key,
                               std::uint64_t value) {
    auto pos = json.find(key);
    if (pos == std::string::npos) return false;
    pos += key.size();
    while (pos < json.size() && std::isspace(json[pos])) ++pos;
    auto end_pos = pos;
    while (end_pos < json.size() &&
           (std::isdigit(json[end_pos]) || json[end_pos] == '-')) {
        ++end_pos;
    }
    if (end_pos == pos) return false;
    json.replace(pos, end_pos - pos, std::to_string(value));
    return true;
}

/// Normalize a Chrome Trace Event JSON string: subtract `offset` (native units)
/// from "ts" and scale ts/dur from the trace's native unit `metric` into
/// microseconds. Falls back to the original string on parse failure. For a US
/// trace this only subtracts the offset (scaling is identity).
inline std::string normalize_event_ts(const std::string& event_json,
                                      std::uint64_t offset,
                                      TraceIndex::TimeMetric metric) {
    thread_local dftracer::utils::json::RecordParser tl_parser;
    auto result = tl_parser.parse(event_json);
    if (result.error()) return event_json;

    auto root = result.value_unsafe();
    if (!root.is_object()) return event_json;

    auto ts_result = root["ts"];
    if (ts_result.error()) return event_json;

    std::uint64_t old_ts = 0;
    if (ts_result.is_uint64()) {
        old_ts = ts_result.get_uint64().value_unsafe();
    } else if (ts_result.is_int64()) {
        auto val = ts_result.get_int64().value_unsafe();
        old_ts = val >= 0 ? static_cast<std::uint64_t>(val) : 0;
    } else {
        return event_json;
    }

    using dftracer::utils::trace::scale_between;
    using TM = TraceIndex::TimeMetric;
    std::uint64_t new_ts =
        scale_between(metric, TM::US, old_ts >= offset ? old_ts - offset : 0);

    std::string modified = event_json;
    if (!rewrite_uint_field(modified, "\"ts\":", new_ts)) return event_json;

    if (metric != TM::US) {
        auto dur_result = root["dur"];
        if (!dur_result.error() && dur_result.is_uint64()) {
            rewrite_uint_field(
                modified, "\"dur\":",
                scale_between(metric, TM::US, dur_result.get_uint64().value()));
        }
    }
    return modified;
}

/// Compute the minimum event duration threshold for a given summary level.
/// Level 1 = full detail, higher levels filter shorter events.
inline double duration_threshold(double begin, double end, unsigned level,
                                 unsigned viewport_width = 1920) {
    if (level <= 1) return 0.0;
    double range = end - begin;
    return range /
           (static_cast<double>(viewport_width) * static_cast<double>(level));
}

/// Parsed and normalized time window shared by the /viz handlers.
struct VizWindow {
    double begin;              ///< Absolute native time for the scan predicate.
    double end;                ///< Absolute native time for the scan predicate.
    double original_begin;     ///< Client-sent value (normalized us).
    double original_end;       ///< Client-sent value (normalized us).
    std::uint64_t global_min;  ///< 0 when normalization is off or unavailable.
    bool normalize;
};

/// Parse begin/end, validate the optional duql filter, then apply timestamp
/// normalization and native-unit conversion so begin/end are absolute native
/// timestamps for the scan. Returns a bad_request response when the query is
/// malformed. Callers keep their own required-parameter checks and any
/// per-handler extras (summary/lookback/min_dur/group_by).
// A bad request when `query` is not a duql filter; an endpoint takes no
// pipeline stage.
inline std::optional<HttpResponse> refuse_duql(std::string_view query,
                                               std::string_view endpoint) {
    if (query.empty() || duql::try_parse(query)) return std::nullopt;
    if (auto tree = duql::syntax::parse(query); tree && tree->pipeline) {
        const auto& stages = tree->pipeline->stages;
        for (std::size_t i = 0; i < stages.size(); ++i) {
            if (i == 0 &&
                std::holds_alternative<duql::syntax::Where>(stages[i].node))
                continue;
            std::string_view word = query.substr(stages[i].span.offset);
            word = word.substr(0, word.find_first_of(" \t\r\n("));
            return HttpResponse::bad_request(
                "endpoint " + std::string(endpoint) +
                " takes a filter; stages such as '" + std::string(word) +
                "' are not supported here");
        }
    }
    return HttpResponse::bad_request("Invalid duql: " + std::string(query));
}

inline dftracer::utils::expected<VizWindow, HttpResponse> parse_viz_window(
    const QueryParams& params, TraceIndex& index, std::string_view endpoint) {
    double begin = params.get_double("begin", 0);
    double end = params.get_double("end", 0);

    auto query = params.get("duql");
    if (auto refused = refuse_duql(query, endpoint))
        return dftracer::utils::unexpected(std::move(*refused));

    auto ts_norm_param = params.get("ts_normalize");
    bool normalize = ts_norm_param.empty() || ts_norm_param != "0";
    std::uint64_t global_min = 0;
    if (normalize) {
        global_min = index.global_min_timestamp_us();
        if (global_min == std::numeric_limits<std::uint64_t>::max())
            global_min = 0;
    }
    double original_begin = begin;
    double original_end = end;
    if (index.time_metric() != TraceIndex::TimeMetric::US) {
        begin = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(begin)));
        end = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(end)));
    }
    if (normalize && global_min > 0) {
        begin += static_cast<double>(global_min);
        end += static_cast<double>(global_min);
    }
    return VizWindow{begin,        end,        original_begin,
                     original_end, global_min, normalize};
}

inline std::string extract_json_value(simdjson::dom::element val) {
    if (val.is_string()) {
        return std::string(val.get_string().value_unsafe());
    }
    if (val.is_int64()) {
        return std::to_string(val.get_int64().value_unsafe());
    }
    if (val.is_uint64()) {
        return std::to_string(val.get_uint64().value_unsafe());
    }
    return {};
}

inline void append_lane_clause(std::string& dsl, const char* field,
                               const std::string& val) {
    if (!dsl.empty()) dsl += " and ";
    const std::size_t digits = !val.empty() && val[0] == '-' ? 1 : 0;
    bool numeric =
        val.size() > digits &&
        std::all_of(val.begin() + static_cast<std::ptrdiff_t>(digits),
                    val.end(), [](char c) { return std::isdigit(c); });
    if (numeric) {
        dsl += std::string(field) + " == " + val;
    } else {
        dsl += std::string(field) + " == \"" + val + "\"";
    }
}

inline void apply_lanes(std::string& dsl, std::string_view lanes_str) {
    if (lanes_str.empty()) return;

    thread_local dftracer::utils::json::RecordParser tl_parser;
    auto result = tl_parser.parse(lanes_str.data(), lanes_str.size());
    if (result.error()) return;

    auto root = result.value_unsafe();

    if (root.is_array()) {
        auto arr = root.get_array().value_unsafe();
        for (auto item : arr) {
            if (!item.is_object()) continue;
            auto obj = item.get_object().value_unsafe();

            auto field_result = obj["field"];
            if (field_result.error()) field_result = obj["fields"];
            auto value_result = obj["value"];
            if (field_result.error() || value_result.error()) continue;

            if (!field_result.value_unsafe().is_string()) continue;
            const char* field =
                field_result.value_unsafe().get_c_str().value_unsafe();
            auto val = extract_json_value(value_result.value_unsafe());
            if (!val.empty()) append_lane_clause(dsl, field, val);
        }
    } else if (root.is_object()) {
        auto obj = root.get_object().value_unsafe();

        auto field_result = obj["field"];
        if (field_result.error()) field_result = obj["fields"];
        auto value_result = obj["value"];

        if (!field_result.error() && !value_result.error()) {
            if (field_result.value_unsafe().is_string()) {
                const char* field =
                    field_result.value_unsafe().get_c_str().value_unsafe();
                auto val = extract_json_value(value_result.value_unsafe());
                if (!val.empty()) append_lane_clause(dsl, field, val);
            }
        }
    }
}

inline void apply_filters(std::string& dsl, std::string_view filters_str,
                          const TraceFields& fields) {
    if (filters_str.empty()) return;

    thread_local dftracer::utils::json::RecordParser tl_parser;
    auto result = tl_parser.parse(filters_str.data(), filters_str.size());
    if (result.error()) return;

    auto root = result.value_unsafe();
    if (!root.is_array()) return;

    auto arr = root.get_array().value_unsafe();
    for (auto item : arr) {
        if (!item.is_object()) continue;
        auto obj = item.get_object().value_unsafe();

        auto field_result = obj["field"];
        auto op_result = obj["op"];
        auto value_result = obj["value"];
        if (field_result.error() || op_result.error() || value_result.error())
            continue;

        if (!field_result.value_unsafe().is_string() ||
            !op_result.value_unsafe().is_string())
            continue;

        const char* field =
            field_result.value_unsafe().get_c_str().value_unsafe();
        const char* op = op_result.value_unsafe().get_c_str().value_unsafe();

        std::string val = extract_json_value(value_result.value_unsafe());
        if (val.empty()) continue;

        std::string op_str(op);
        std::string field_str(field);
        // Time bounds arrive in microseconds; a path schema's time and
        // duration fields hold their own unit.
        double us = 1;
        if (field_str == "begin" || field_str == "end") {
            field_str = fields.time;
            us = fields.time_us;
        } else if (field_str == "duration") {
            field_str = fields.duration;
            us = fields.duration_us;
        }
        if (field_str.empty()) continue;
        if (us != 1 && !val.empty() &&
            (std::isdigit(static_cast<unsigned char>(val[0])) || val[0] == '-'))
            val = std::to_string(std::strtod(val.c_str(), nullptr) / us);

        std::string query_op;
        if (op_str == "=")
            query_op = "==";
        else if (op_str == ">=")
            query_op = ">=";
        else if (op_str == "<=")
            query_op = "<=";
        else if (op_str == ">")
            query_op = ">";
        else if (op_str == "<")
            query_op = "<";
        else
            continue;

        if (!dsl.empty()) dsl += " and ";
        bool numeric = !val.empty() && (std::isdigit(val[0]) || val[0] == '-');
        if (numeric || query_op != "==") {
            dsl += field_str + " " + query_op + " " + val;
        } else {
            dsl += field_str + " " + query_op + " \"" + val + "\"";
        }
    }
}

// --- GET /api/viz/events ---
// Build the query view (time range + lane/filter/pid/tid/cat predicates) for a
// viz request from the parsed parameters.
// A path schema's records carry their time in their own unit, so a caller
// windows them with View::time_range instead of the ts clause here.
inline ViewDefinition build_viz_view(const QueryParams& params, double begin,
                                     double end, double min_dur,
                                     const TraceFields& fields) {
    ViewDefinition view;
    view.name = "viz_query";
    view.description = "Visualization query";

    // Reserve once and append in place: numbers go through to_chars into a
    // stack buffer (no per-number heap allocation, unlike std::to_string), and
    // literals/string_views are appended directly (no temporary
    // concatenations).
    std::string dsl;
    dsl.reserve(128);
    char numbuf[20];  // max digits of a uint64_t
    auto append_u64 = [&](std::uint64_t v) {
        dsl.append(numbuf, to_chars_u64(numbuf, numbuf + sizeof(numbuf), v));
    };
    auto conjoin = [&dsl]() {
        if (!dsl.empty()) dsl += " and ";
    };

    if (!fields.by_path) {
        dsl += "ts >= ";
        append_u64(static_cast<std::uint64_t>(begin));
        dsl += " and ts <= ";
        append_u64(static_cast<std::uint64_t>(end));
        if (min_dur > 0) {
            dsl += " and dur >= ";
            append_u64(static_cast<std::uint64_t>(min_dur));
        }
    } else if (min_dur > 0 && !fields.duration.empty()) {
        dsl += fields.duration;
        dsl += " >= ";
        dsl += std::to_string(min_dur / fields.duration_us);
    }

    apply_lanes(dsl, params.get("lanes"));
    apply_filters(dsl, params.get("filters"), fields);

    if (auto pid = params.get("pid"); !pid.empty() && !fields.entity.empty())
        append_lane_clause(dsl, fields.entity.c_str(), std::string(pid));
    if (auto tid = params.get("tid"); !tid.empty() && !fields.lane.empty())
        append_lane_clause(dsl, fields.lane.c_str(), std::string(tid));

    auto cat = params.get("cat");
    if (!cat.empty()) {
        conjoin();
        dsl += "cat == \"";
        dsl += cat;
        dsl += '"';
    }

    // Raw duql from the front-end filter box, already validated by the caller.
    auto query = params.get("duql");
    if (!query.empty()) {
        conjoin();
        dsl += '(';
        dsl += query;
        dsl += ')';
    }

    if (!dsl.empty()) view.with_query(dsl);
    return view;
}

// Select the files to scan: the explicit ?file= or all indexed files, then drop
// files whose cached time bounds don't overlap [begin, end]. Pure/synchronous.
inline std::vector<const TraceIndex::FileInfo*> select_viz_target_files(
    TraceIndex& index, const QueryParams& params, double begin, double end) {
    auto target_files = collect_candidate_files(index, params);

    // An explicit ?file= is a direct request for that file; never drop it on
    // cached time bounds (which can be wrong, e.g. multi-node clock skew).
    if (!params.get("file").empty()) return target_files;

    if (begin > 0 || end > 0) {
        std::vector<const TraceIndex::FileInfo*> filtered;
        filtered.reserve(target_files.size());
        for (auto* fi : target_files) {
            if (fi->min_timestamp_us == 0 && fi->max_timestamp_us == 0) {
                filtered.push_back(fi);
                continue;
            }
            double fi_min = static_cast<double>(fi->min_timestamp_us);
            double fi_max = static_cast<double>(fi->max_timestamp_us);
            if (fi_max < begin || fi_min > end) continue;
            filtered.push_back(fi);
        }
        target_files = std::move(filtered);
    }
    return target_files;
}

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_SCAN_H
