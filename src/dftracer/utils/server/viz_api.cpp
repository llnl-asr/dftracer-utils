#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>
#include <yyjson.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::server {

using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::views;

/// Normalize the "ts" field in a Chrome Trace Event JSON string by
/// subtracting an offset.  Returns the modified JSON.  Falls back to
/// the original string on parse failure.
static std::string normalize_event_ts(const std::string& event_json,
                                      std::uint64_t offset) {
    auto* doc = yyjson_read(event_json.c_str(), event_json.size(), 0);
    if (!doc) return event_json;

    auto* mdoc = yyjson_doc_mut_copy(doc, nullptr);
    yyjson_doc_free(doc);
    if (!mdoc) return event_json;

    auto* root = yyjson_mut_doc_get_root(mdoc);
    if (root) {
        auto* ts_val = yyjson_mut_obj_get(root, "ts");
        if (ts_val && yyjson_mut_is_uint(ts_val)) {
            std::uint64_t old_ts = yyjson_mut_get_uint(ts_val);
            std::uint64_t new_ts = old_ts >= offset ? old_ts - offset : 0;
            yyjson_mut_set_uint(ts_val, new_ts);
        } else if (ts_val && yyjson_mut_is_int(ts_val)) {
            auto old_ts =
                static_cast<std::uint64_t>(yyjson_mut_get_int(ts_val));
            std::uint64_t new_ts = old_ts >= offset ? old_ts - offset : 0;
            yyjson_mut_set_uint(ts_val, new_ts);
        }
    }

    std::size_t len = 0;
    char* json_str = yyjson_mut_write(mdoc, YYJSON_WRITE_NOFLAG, &len);
    yyjson_mut_doc_free(mdoc);

    if (json_str) {
        std::string result(json_str, len);
        free(json_str);
        return result;
    }
    return event_json;
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

/// Parse the `lanes` query parameter and apply bloom filters to the
/// predicate.  Accepts a JSON object with repeated field/value pairs
/// for sequential filtering:
///   lanes={"fields":"pid","value":1234,"fields":"tid","value":2345}
///
/// Since JSON objects cannot have duplicate keys, we also accept a
/// JSON array of {field, value} pairs:
///   lanes=[{"field":"pid","value":"1234"},{"field":"tid","value":"5678"}]
static void apply_lanes(ViewPredicate& pred, std::string_view lanes_str) {
    if (lanes_str.empty()) return;

    std::string buf(lanes_str);
    auto* doc = yyjson_read(buf.c_str(), buf.size(), 0);
    if (!doc) return;
    auto doc_guard = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
        doc, yyjson_doc_free);

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root) return;

    if (yyjson_is_arr(root)) {
        // Array format: [{"field":"pid","value":"1234"}, ...]
        yyjson_val* item;
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(root, &iter);
        while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
            if (!yyjson_is_obj(item)) continue;
            auto* field_val = yyjson_obj_get(item, "field");
            if (!field_val) field_val = yyjson_obj_get(item, "fields");
            auto* value_val = yyjson_obj_get(item, "value");
            if (!field_val || !value_val) continue;

            const char* field = yyjson_get_str(field_val);
            if (!field) continue;

            std::string val_str;
            if (yyjson_is_str(value_val)) {
                val_str = yyjson_get_str(value_val);
            } else if (yyjson_is_int(value_val)) {
                val_str = std::to_string(yyjson_get_int(value_val));
            } else if (yyjson_is_uint(value_val)) {
                val_str = std::to_string(yyjson_get_uint(value_val));
            } else {
                continue;
            }

            pred.with_bloom_dim(field, {val_str});
        }
    } else if (yyjson_is_obj(root)) {
        // Object format (single field/value pair):
        //   {"fields":"pid","value":1234}
        auto* field_val = yyjson_obj_get(root, "field");
        if (!field_val) field_val = yyjson_obj_get(root, "fields");
        auto* value_val = yyjson_obj_get(root, "value");
        if (field_val && value_val) {
            const char* field = yyjson_get_str(field_val);
            if (field) {
                std::string val_str;
                if (yyjson_is_str(value_val)) {
                    val_str = yyjson_get_str(value_val);
                } else if (yyjson_is_int(value_val)) {
                    val_str = std::to_string(yyjson_get_int(value_val));
                } else if (yyjson_is_uint(value_val)) {
                    val_str = std::to_string(yyjson_get_uint(value_val));
                }
                if (!val_str.empty()) {
                    pred.with_bloom_dim(field, {val_str});
                }
            }
        }
    }
}

/// Parse the `filters` query parameter and apply matching bloom-dim
/// or time/duration filters.  Format:
///   filters=[{"field":"pid","op":"=","value":1234}, ...]
///
/// Supported ops:
///   "="  — equality via bloom dimension (for pid, tid, cat, name, etc.)
///   ">=", "<=" — for ts (maps to time_range) and dur (min/max duration)
static void apply_filters(ViewPredicate& pred, std::string_view filters_str) {
    if (filters_str.empty()) return;

    std::string buf(filters_str);
    auto* doc = yyjson_read(buf.c_str(), buf.size(), 0);
    if (!doc) return;
    auto doc_guard = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
        doc, yyjson_doc_free);

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_arr(root)) return;

    yyjson_val* item;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(root, &iter);
    while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
        if (!yyjson_is_obj(item)) continue;

        auto* field_val = yyjson_obj_get(item, "field");
        auto* op_val = yyjson_obj_get(item, "op");
        auto* value_val = yyjson_obj_get(item, "value");
        if (!field_val || !op_val || !value_val) continue;

        const char* field = yyjson_get_str(field_val);
        const char* op = yyjson_get_str(op_val);
        if (!field || !op) continue;

        std::string field_str(field);
        std::string op_str(op);

        // Get a numeric value for range operators.
        double num_val = 0;
        bool has_num = false;
        if (yyjson_is_int(value_val)) {
            num_val = static_cast<double>(yyjson_get_int(value_val));
            has_num = true;
        } else if (yyjson_is_uint(value_val)) {
            num_val = static_cast<double>(yyjson_get_uint(value_val));
            has_num = true;
        } else if (yyjson_is_real(value_val)) {
            num_val = yyjson_get_real(value_val);
            has_num = true;
        }

        // Get a string value for equality.
        std::string str_val;
        if (yyjson_is_str(value_val)) {
            str_val = yyjson_get_str(value_val);
        } else if (has_num) {
            if (yyjson_is_int(value_val)) {
                str_val = std::to_string(yyjson_get_int(value_val));
            } else if (yyjson_is_uint(value_val)) {
                str_val = std::to_string(yyjson_get_uint(value_val));
            } else {
                str_val = std::to_string(num_val);
            }
        }

        if (op_str == "=") {
            // Equality — use bloom dimension filtering.
            if (!str_val.empty()) {
                pred.with_bloom_dim(field_str, {str_val});
            }
        } else if (op_str == ">=" && has_num) {
            if (field_str == "ts" || field_str == "begin") {
                // Augment time range lower bound.
                if (pred.time_range) {
                    pred.time_range->first =
                        std::max(pred.time_range->first, num_val);
                } else {
                    pred.with_time_range(num_val, 0);
                }
            } else if (field_str == "dur" || field_str == "duration") {
                pred.with_min_duration(num_val);
            }
        } else if (op_str == "<=" && has_num) {
            if (field_str == "ts" || field_str == "end") {
                // Augment time range upper bound.
                if (pred.time_range) {
                    pred.time_range->second =
                        std::min(pred.time_range->second, num_val);
                } else {
                    pred.with_time_range(0, num_val);
                }
            } else if (field_str == "dur" || field_str == "duration") {
                pred.with_max_duration(num_val);
            }
        } else if (op_str == ">" && has_num) {
            if (field_str == "dur" || field_str == "duration") {
                pred.with_min_duration(num_val);
            }
        } else if (op_str == "<" && has_num) {
            if (field_str == "dur" || field_str == "duration") {
                pred.with_max_duration(num_val);
            }
        }
    }
}

// --- GET /api/v1/viz/events ---
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

    // Build view with time range and optional filters
    ViewDefinition view;
    view.name = "viz_query";
    view.description = "Visualization query";

    ViewPredicate pred;
    pred.with_time_range(begin, end);
    if (min_dur > 0) {
        pred.with_min_duration(min_dur);
    }

    // Apply lanes param (JSON) for sequential pid/tid filtering.
    apply_lanes(pred, params.get("lanes"));

    // Apply filters param (JSON array of {field, op, value}).
    apply_filters(pred, params.get("filters"));

    // Backward-compatible individual query params (override lanes/filters).
    auto pid = params.get("pid");
    if (!pid.empty()) {
        pred.with_bloom_dim("process_id", {std::string(pid)});
    }

    auto tid = params.get("tid");
    if (!tid.empty()) {
        pred.with_bloom_dim("thread_id", {std::string(tid)});
    }

    auto cat = params.get("cat");
    if (!cat.empty()) {
        pred.with_bloom_dim("category", {std::string(cat)});
    }

    view.with_predicate(std::move(pred));

    // Determine files
    std::vector<const TraceIndex::FileInfo*> target_files;
    auto file_param = params.get("file");
    if (!file_param.empty()) {
        auto* f = index.find_file(std::string(file_param));
        if (f) target_files.push_back(f);
    } else {
        for (const auto& f : index.files()) {
            target_files.push_back(&f);
        }
    }

    std::vector<std::string> collected_events;

    for (auto* file_info : target_files) {
        auto meta_input =
            MetadataCollectorUtilityInput::from_file(file_info->path)
                .with_index(file_info->idx_path);
        auto metadata = co_await MetadataCollectorUtility{}.process(meta_input);
        if (!metadata.success) continue;

        ViewBuilderInput builder_input;
        builder_input.with_view(view)
            .with_file_path(file_info->path)
            .with_bidx_path(file_info->has_bloom_index ? file_info->bidx_path
                                                       : "")
            .with_uncompressed_size(metadata.uncompressed_size)
            .with_num_checkpoints(metadata.num_checkpoints);

        ViewBuilderUtility builder;
        auto build_output = co_await builder.process(builder_input);
        if (!build_output.success || !build_output.file_may_match) continue;

        for (const auto& candidate : build_output.candidates) {
            ViewReaderInput reader_input;
            reader_input.with_file_path(file_info->path)
                .with_idx_path(file_info->idx_path)
                .with_byte_range(candidate.start_byte, candidate.end_byte)
                .with_checkpoint_idx(candidate.checkpoint_idx)
                .with_view(view);

            ViewReaderUtility reader;
            auto read_output = co_await reader.process(reader_input);

            if (read_output.success) {
                for (auto& event : read_output.events) {
                    collected_events.push_back(std::move(event));
                }
            }
        }
    }

    // Apply normalization to event timestamps.
    if (normalize && global_min > 0) {
        for (auto& event : collected_events) {
            event = normalize_event_ts(event, global_min);
        }
    }

    // Use the original (normalized) begin/end for metadata.
    double meta_begin = original_begin;
    double meta_end = original_end;

    // Build response matching the Chrome Trace Event Format.
    // Pre-compute size to avoid repeated reallocations.
    std::size_t body_size = 256;
    for (const auto& ev : collected_events) body_size += ev.size() + 1;
    std::string body;
    body.reserve(body_size);
    body += "{\"events\":[";
    for (std::size_t i = 0; i < collected_events.size(); ++i) {
        if (i > 0) body += ',';
        body += collected_events[i];  // Already JSON
    }
    body += "],\"metadata\":{\"begin\":";
    body += std::to_string(meta_begin);
    body += ",\"end\":";
    body += std::to_string(meta_end);
    body += ",\"count\":";
    body += std::to_string(collected_events.size());
    body += ",\"ts_normalized\":";
    body += (normalize && global_min > 0) ? "true" : "false";
    body += ",\"global_min_timestamp_us\":";
    body += std::to_string(index.global_min_timestamp_us());
    body += "}}";

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