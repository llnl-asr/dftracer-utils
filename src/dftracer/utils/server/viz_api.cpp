#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/utilities/common/json/json_doc_guard.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/views/view_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <simdjson.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::server {

using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::views;

static const std::unordered_set<std::string> HASH_METADATA_NAMES = {"FH", "HH",
                                                                    "SH"};

/// Normalize the "ts" field in a Chrome Trace Event JSON string by
/// subtracting an offset.  Returns the modified JSON.  Falls back to
/// the original string on parse failure.
static std::string normalize_event_ts(const std::string& event_json,
                                      std::uint64_t offset) {
    thread_local simdjson::dom::parser tl_parser;
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

    std::uint64_t new_ts = old_ts >= offset ? old_ts - offset : 0;

    // simdjson DOM is read-only, so we need to rebuild the JSON with the new ts
    // Find "ts": and replace the value
    std::string modified = event_json;
    auto pos = modified.find("\"ts\":");
    if (pos == std::string::npos) return event_json;

    pos += 5;  // Skip past "ts":
    while (pos < modified.size() && std::isspace(modified[pos])) ++pos;

    auto end_pos = pos;
    while (end_pos < modified.size() &&
           (std::isdigit(modified[end_pos]) || modified[end_pos] == '-')) {
        ++end_pos;
    }

    modified.replace(pos, end_pos - pos, std::to_string(new_ts));
    return modified;
}

/// Compute the minimum event duration threshold for a given summary level.
/// Level 1 = full detail, higher levels filter shorter events.
static double duration_threshold(double begin, double end, unsigned level,
                                 unsigned viewport_width = 1920) {
    if (level <= 1) return 0.0;
    double range = end - begin;
    return range /
           (static_cast<double>(viewport_width) * static_cast<double>(level));
}

static std::string extract_json_value(simdjson::dom::element val) {
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

static void append_lane_clause(std::string& dsl, const char* field,
                               const std::string& val) {
    if (!dsl.empty()) dsl += " and ";
    bool numeric =
        !val.empty() && std::all_of(val.begin(), val.end(),
                                    [](char c) { return std::isdigit(c); });
    if (numeric) {
        dsl += std::string(field) + " == " + val;
    } else {
        dsl += std::string(field) + " == \"" + val + "\"";
    }
}

static void apply_lanes(std::string& dsl, std::string_view lanes_str) {
    if (lanes_str.empty()) return;

    thread_local simdjson::dom::parser tl_parser;
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

static void apply_filters(std::string& dsl, std::string_view filters_str) {
    if (filters_str.empty()) return;

    thread_local simdjson::dom::parser tl_parser;
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
        if (field_str == "begin") field_str = "ts";
        if (field_str == "end") field_str = "ts";
        if (field_str == "duration") field_str = "dur";

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

// --- GET /api/v1/viz/events ---
// Build the query view (time range + lane/filter/pid/tid/cat predicates) for a
// viz request from the parsed parameters.
static ViewDefinition build_viz_view(const QueryParams& params, double begin,
                                     double end, double min_dur) {
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

    dsl += "ts >= ";
    append_u64(static_cast<std::uint64_t>(begin));
    dsl += " and ts <= ";
    append_u64(static_cast<std::uint64_t>(end));
    if (min_dur > 0) {
        dsl += " and dur >= ";
        append_u64(static_cast<std::uint64_t>(min_dur));
    }

    apply_lanes(dsl, params.get("lanes"));
    apply_filters(dsl, params.get("filters"));

    auto pid = params.get("pid");
    if (!pid.empty()) {
        dsl += " and pid == ";
        dsl += pid;
    }

    auto tid = params.get("tid");
    if (!tid.empty()) {
        dsl += " and tid == ";
        dsl += tid;
    }

    auto cat = params.get("cat");
    if (!cat.empty()) {
        dsl += " and cat == \"";
        dsl += cat;
        dsl += '"';
    }

    view.with_query(dsl);
    return view;
}

// Select the files to scan: the explicit ?file= or all indexed files, then drop
// files whose cached time bounds don't overlap [begin, end]. Pure/synchronous.
static std::vector<const TraceIndex::FileInfo*> select_viz_target_files(
    TraceIndex& index, const QueryParams& params, double begin, double end) {
    auto target_files = collect_candidate_files(index, params);

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

// Normalize event timestamps (when global_min > 0) and serialize the collected
// events plus metadata into the Chrome Trace Event Format body. `global_min` is
// the de-normalization base (already 0 unless normalization is active);
// `display_global_min` is the value reported in the metadata. Pure/synchronous.
static std::string build_viz_events_body(std::vector<std::string>& events,
                                         std::uint64_t global_min,
                                         double meta_begin, double meta_end,
                                         int limit, bool truncated,
                                         std::uint64_t display_global_min) {
    if (global_min > 0) {
        for (auto& event : events) {
            event = normalize_event_ts(event, global_min);
        }
    }

    // Pre-compute size to avoid repeated reallocations.
    std::size_t body_size = 256;
    for (const auto& ev : events) body_size += ev.size() + 1;
    std::string body;
    body.reserve(body_size);
    body += "{\"events\":[";
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i > 0) body += ',';
        body += events[i];  // Already JSON
    }
    body += "],\"metadata\":{\"begin\":";
    body += std::to_string(meta_begin);
    body += ",\"end\":";
    body += std::to_string(meta_end);
    body += ",\"count\":";
    body += std::to_string(events.size());
    body += ",\"limit\":";
    body += std::to_string(limit);
    body += ",\"truncated\":";
    body += truncated ? "true" : "false";
    body += ",\"ts_normalized\":";
    body += (global_min > 0) ? "true" : "false";
    body += ",\"global_min_timestamp_us\":";
    body += std::to_string(display_global_min);
    body += "}}";
    return body;
}

static coro::CoroTask<HttpResponse> handle_viz_events(
    const HttpRequest& /*req*/, const QueryParams& params, TraceIndex& index) {
    // Required: begin, end, summary
    if (!params.has("begin") || !params.has("end") || !params.has("summary")) {
        co_return HttpResponse::bad_request(
            "Missing required parameters: begin, end, summary");
    }

    double begin = params.get_double("begin", 0);
    double end = params.get_double("end", 0);
    int summary = params.get_int("summary", 1);
    if (summary < 1) summary = 1;

    // Timestamp normalization: default ON, opt-out with ?ts_normalize=0
    auto ts_norm_param = params.get("ts_normalize");
    bool normalize = ts_norm_param.empty() || ts_norm_param != "0";

    std::uint64_t global_min = 0;
    if (normalize) {
        global_min = index.global_min_timestamp_us();
        if (global_min == std::numeric_limits<std::uint64_t>::max()) {
            global_min = 0;  // No valid bounds, skip normalization
        }
    }

    // When normalization is active the user sends normalized
    // begin/end values (relative to global_min).  De-normalize them
    // so the predicate filters against absolute timestamps.
    double original_begin = begin;
    double original_end = end;
    if (normalize && global_min > 0) {
        begin += static_cast<double>(global_min);
        end += static_cast<double>(global_min);
    }

    double min_dur =
        duration_threshold(begin, end, static_cast<unsigned>(summary));

    ViewDefinition view = build_viz_view(params, begin, end, min_dur);

    // Optional limit: 0 (default) means no limit.
    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;

    std::vector<const TraceIndex::FileInfo*> target_files =
        select_viz_target_files(index, params, begin, end);

    std::vector<std::string> collected_events;

    bool truncated = false;

    if (target_files.size() <= 1) {
        for (auto* file_info : target_files) {
            if (limit > 0 &&
                static_cast<int>(collected_events.size()) >= limit) {
                truncated = true;
                break;
            }
            if (file_info->uncompressed_size == 0 &&
                file_info->num_checkpoints == 0)
                continue;

            ViewBuilderInput builder_input;
            builder_input.with_view(view)
                .with_file_path(file_info->path)
                .with_index_path(
                    file_info->has_bloom_data ? file_info->index_path : "")
                .with_uncompressed_size(file_info->uncompressed_size)
                .with_num_checkpoints(file_info->num_checkpoints)
                .with_bloom_cache(&index.bloom_cache())
                .with_time_range(begin, end);

            ViewBuilderUtility builder;
            auto build_output = co_await builder.process(builder_input);
            if (!build_output || !build_output->file_may_match) continue;

            for (const auto& candidate : build_output->candidates) {
                if (limit > 0 &&
                    static_cast<int>(collected_events.size()) >= limit) {
                    truncated = true;
                    break;
                }
                ViewReaderInput reader_input;
                reader_input.with_file_path(file_info->path)
                    .with_index_path(file_info->index_path)
                    .with_byte_range(candidate.start_byte, candidate.end_byte)
                    .with_checkpoint_idx(candidate.checkpoint_idx)
                    .with_view(view);

                ViewReaderUtility reader;
                auto gen = reader.process(reader_input);
                while (auto batch = co_await gen.next()) {
                    for (auto& event : batch->events) {
                        if (limit > 0 &&
                            static_cast<int>(collected_events.size()) >=
                                limit) {
                            truncated = true;
                            break;
                        }
                        collected_events.emplace_back(event);
                    }
                    if (truncated) break;
                }
            }
        }
    } else {
        std::size_t num_workers =
            std::min(index.max_concurrent(), target_files.size());
        auto* executor = Executor::current();

        auto file_chan = coro::make_channel<std::size_t>(num_workers * 2);
        auto collected_mutex = std::make_shared<std::mutex>();
        auto remaining = std::make_shared<std::atomic<int>>(
            limit > 0 ? limit : std::numeric_limits<int>::max());

        auto* target_files_ptr = &target_files;
        auto* collected_ptr = &collected_events;
        auto* view_ptr = &view;
        auto* bloom_cache_ptr = &index.bloom_cache();
        double t_begin = begin;
        double t_end = end;

        CoroScope scope(executor);

        scope.spawn([ch = file_chan->producer(), target_files_ptr](
                        CoroScope&) mutable -> coro::CoroTask<void> {
            auto guard = ch.guard();
            for (std::size_t i = 0; i < target_files_ptr->size(); ++i) {
                if (!co_await ch.send(i)) co_return;
            }
            co_return;
        });

        for (std::size_t w = 0; w < num_workers; ++w) {
            scope.spawn([file_chan, target_files_ptr, collected_mutex,
                         collected_ptr, view_ptr, bloom_cache_ptr, remaining,
                         t_begin, t_end](CoroScope&) -> coro::CoroTask<void> {
                while (auto fi_opt = co_await file_chan->receive()) {
                    if (remaining->load(std::memory_order_relaxed) <= 0)
                        co_return;
                    auto* file_info = (*target_files_ptr)[*fi_opt];

                    if (file_info->uncompressed_size == 0 &&
                        file_info->num_checkpoints == 0)
                        continue;

                    ViewBuilderInput builder_input;
                    builder_input.with_view(*view_ptr)
                        .with_file_path(file_info->path)
                        .with_index_path(file_info->has_bloom_data
                                             ? file_info->index_path
                                             : "")
                        .with_uncompressed_size(file_info->uncompressed_size)
                        .with_num_checkpoints(file_info->num_checkpoints)
                        .with_bloom_cache(bloom_cache_ptr)
                        .with_time_range(t_begin, t_end);

                    ViewBuilderUtility builder;
                    auto build_output = co_await builder.process(builder_input);
                    if (!build_output || !build_output->file_may_match)
                        continue;

                    for (const auto& candidate : build_output->candidates) {
                        if (remaining->load(std::memory_order_relaxed) <= 0)
                            break;

                        ViewReaderInput reader_input;
                        reader_input.with_file_path(file_info->path)
                            .with_index_path(file_info->index_path)
                            .with_byte_range(candidate.start_byte,
                                             candidate.end_byte)
                            .with_checkpoint_idx(candidate.checkpoint_idx)
                            .with_view(*view_ptr);

                        ViewReaderUtility reader;
                        auto gen = reader.process(reader_input);
                        while (auto batch = co_await gen.next()) {
                            if (!batch->events.empty()) {
                                std::lock_guard<std::mutex> lock(
                                    *collected_mutex);
                                for (auto& event : batch->events) {
                                    collected_ptr->emplace_back(event);
                                }
                                remaining->fetch_sub(
                                    static_cast<int>(batch->events.size()));
                            }
                        }
                    }
                }
                co_return;
            });
        }

        co_await scope.join();

        if (limit > 0 && static_cast<int>(collected_events.size()) > limit) {
            collected_events.resize(static_cast<std::size_t>(limit));
            truncated = true;
        }
    }

    std::string body = build_viz_events_body(
        collected_events, global_min, original_begin, original_end, limit,
        truncated, index.global_min_timestamp_us());
    co_return HttpResponse::ok(body);
}

void register_viz_api(Router& router, TraceIndex& index) {
    auto* index_ptr = &index;

    router.get(
        "/api/v1/viz/events",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_viz_events(req, params, *index_ptr);
        });
}

}  // namespace dftracer::utils::server
