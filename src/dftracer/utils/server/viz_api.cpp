#include <dftracer/utils/core/common/logging.h>
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
#include <yyjson.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::server {

using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::views;

using dftracer::utils::utilities::common::json::JsonDocGuard;
using dftracer::utils::utilities::common::json::JsonValue;
using dftracer::utils::utilities::common::query::Query;

static const std::unordered_set<std::string> HASH_METADATA_NAMES = {"FH", "HH",
                                                                    "SH"};

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

static std::string extract_json_value(yyjson_val* val) {
    if (yyjson_is_str(val)) return yyjson_get_str(val);
    if (yyjson_is_int(val)) return std::to_string(yyjson_get_int(val));
    if (yyjson_is_uint(val)) return std::to_string(yyjson_get_uint(val));
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

    std::string buf(lanes_str);
    auto* doc = yyjson_read(buf.c_str(), buf.size(), 0);
    if (!doc) return;
    auto doc_guard = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
        doc, yyjson_doc_free);

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root) return;

    if (yyjson_is_arr(root)) {
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
            auto val = extract_json_value(value_val);
            if (!val.empty()) append_lane_clause(dsl, field, val);
        }
    } else if (yyjson_is_obj(root)) {
        auto* field_val = yyjson_obj_get(root, "field");
        if (!field_val) field_val = yyjson_obj_get(root, "fields");
        auto* value_val = yyjson_obj_get(root, "value");
        if (field_val && value_val) {
            const char* field = yyjson_get_str(field_val);
            if (field) {
                auto val = extract_json_value(value_val);
                if (!val.empty()) append_lane_clause(dsl, field, val);
            }
        }
    }
}

static void apply_filters(std::string& dsl, std::string_view filters_str) {
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

        std::string val = extract_json_value(value_val);
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

/// Direct-scan a small file without any sidecar index.
/// Streams via async_streaming_gz_lines(), parses JSON, applies
/// predicate filters, collects matching events as raw JSON strings.
static coro::CoroTask<void> direct_scan_events(
    const TraceIndex::FileInfo* file_info, const Query* query,
    bool include_metadata, std::vector<std::string>* collected_events,
    std::uint64_t* total_scanned, std::uint64_t* total_matched, int limit) {
    using dftracer::utils::utilities::fileio::lines::sources::
        async_streaming_gz_lines;

    try {
        auto gen = async_streaming_gz_lines(file_info->path);

        std::unordered_map<std::string, std::string> pending_metadata;
        std::unordered_set<std::string> emitted_hashes;

        while (auto line = co_await gen.next()) {
            if (limit > 0 &&
                collected_events->size() >= static_cast<std::size_t>(limit)) {
                co_return;
            }
            if (line->content.empty()) continue;

            JsonDocGuard guard{yyjson_read_opts(
                const_cast<char*>(line->content.data()), line->content.size(),
                YYJSON_READ_NOFLAG, nullptr, nullptr)};
            if (!guard.doc) continue;

            yyjson_val* root = yyjson_doc_get_root(guard.doc);
            if (root && yyjson_is_obj(root)) {
                JsonValue json(root);
                // line->content is a string_view valid only for this
                // iteration.  All storage into collected_events and
                // pending_metadata must copy to owning std::string.
                std::string_view ph = json["ph"].get<std::string_view>();

                if (ph == "M" && include_metadata) {
                    std::string name_str = json["name"].get<std::string>();

                    if (HASH_METADATA_NAMES.count(name_str)) {
                        auto args = json["args"];
                        if (args.exists()) {
                            auto val = args["value"];
                            if (val.exists()) {
                                std::string hash_val = val.get<std::string>();
                                if (!emitted_hashes.count(hash_val)) {
                                    pending_metadata[hash_val] =
                                        std::string(line->content.data(),
                                                    line->content.size());
                                }
                            }
                        }
                    } else {
                        collected_events->emplace_back(line->content.data(),
                                                       line->content.size());
                        (*total_matched)++;
                    }
                } else if (ph != "M") {
                    (*total_scanned)++;
                    if (!query || query->evaluate(json)) {
                        // Flush referenced hash metadata first
                        if (include_metadata) {
                            auto args = json["args"];
                            if (args.exists()) {
                                static const char* hash_fields[] = {
                                    "hhash", "fhash", "shash"};
                                for (const char* field : hash_fields) {
                                    auto val = args[field];
                                    if (!val.exists()) continue;
                                    std::string hash_val =
                                        val.get<std::string>();
                                    if (emitted_hashes.count(hash_val))
                                        continue;
                                    auto it = pending_metadata.find(hash_val);
                                    if (it != pending_metadata.end()) {
                                        collected_events->push_back(
                                            std::move(it->second));
                                        (*total_matched)++;
                                        emitted_hashes.insert(hash_val);
                                        pending_metadata.erase(it);
                                    }
                                }
                            }
                        }
                        collected_events->emplace_back(line->content.data(),
                                                       line->content.size());
                        (*total_matched)++;
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_WARN("Direct scan failed for %s: %s",
                                file_info->path.c_str(), e.what());
    }

    co_return;
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

    std::string dsl;
    dsl += "ts >= " + std::to_string(static_cast<uint64_t>(begin));
    dsl += " and ts <= " + std::to_string(static_cast<uint64_t>(end));
    if (min_dur > 0) {
        dsl += " and dur >= " + std::to_string(static_cast<uint64_t>(min_dur));
    }

    apply_lanes(dsl, params.get("lanes"));
    apply_filters(dsl, params.get("filters"));

    auto pid = params.get("pid");
    if (!pid.empty()) {
        dsl += " and pid == " + std::string(pid);
    }

    auto tid = params.get("tid");
    if (!tid.empty()) {
        dsl += " and tid == " + std::string(tid);
    }

    auto cat = params.get("cat");
    if (!cat.empty()) {
        dsl += " and cat == \"" + std::string(cat) + "\"";
    }

    view.with_query(dsl);

    // Optional limit: 0 (default) means no limit.
    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;

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

    // File-level time range skip: remove files whose cached time
    // bounds don't overlap the query window [begin, end].
    if (begin > 0 || end > 0) {
        std::vector<const TraceIndex::FileInfo*> filtered;
        filtered.reserve(target_files.size());
        for (auto* fi : target_files) {
            if (fi->is_small) {
                filtered.push_back(fi);
                continue;
            }
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

    const Query* viz_query_ptr = view.query ? &*view.query : nullptr;

    std::vector<std::string> collected_events;

    bool truncated = false;

    if (target_files.size() <= 1) {
        for (auto* file_info : target_files) {
            if (limit > 0 &&
                static_cast<int>(collected_events.size()) >= limit) {
                truncated = true;
                break;
            }
            if (file_info->is_small) {
                std::uint64_t scanned = 0;
                std::uint64_t matched = 0;
                co_await direct_scan_events(
                    file_info, viz_query_ptr, view.include_metadata,
                    &collected_events, &scanned, &matched, limit);
                if (limit > 0 &&
                    static_cast<int>(collected_events.size()) >= limit)
                    truncated = true;
            } else {
                if (file_info->uncompressed_size == 0 &&
                    file_info->num_checkpoints == 0)
                    continue;

                ViewBuilderInput builder_input;
                builder_input.with_view(view)
                    .with_file_path(file_info->path)
                    .with_idx_path(
                        file_info->has_bloom_data ? file_info->idx_path : "")
                    .with_uncompressed_size(file_info->uncompressed_size)
                    .with_num_checkpoints(file_info->num_checkpoints)
                    .with_bloom_cache(&index.bloom_cache())
                    .with_time_range(begin, end);

                ViewBuilderUtility builder;
                auto build_output = co_await builder.process(builder_input);
                if (!build_output.success || !build_output.file_may_match)
                    continue;

                for (const auto& candidate : build_output.candidates) {
                    if (limit > 0 &&
                        static_cast<int>(collected_events.size()) >= limit) {
                        truncated = true;
                        break;
                    }
                    ViewReaderInput reader_input;
                    reader_input.with_file_path(file_info->path)
                        .with_idx_path(file_info->idx_path)
                        .with_byte_range(candidate.start_byte,
                                         candidate.end_byte)
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
                         collected_ptr, viz_query_ptr, view_ptr,
                         bloom_cache_ptr, remaining, t_begin,
                         t_end](CoroScope&) -> coro::CoroTask<void> {
                while (auto fi_opt = co_await file_chan->receive()) {
                    if (remaining->load(std::memory_order_relaxed) <= 0)
                        co_return;
                    auto* file_info = (*target_files_ptr)[*fi_opt];

                    if (file_info->is_small) {
                        std::vector<std::string> local_events;
                        std::uint64_t local_scanned = 0;
                        std::uint64_t local_matched = 0;
                        int local_limit =
                            remaining->load(std::memory_order_relaxed);
                        if (local_limit <= 0) co_return;
                        co_await direct_scan_events(
                            file_info, viz_query_ptr,
                            view_ptr->include_metadata, &local_events,
                            &local_scanned, &local_matched, local_limit);
                        if (!local_events.empty()) {
                            std::lock_guard<std::mutex> lock(*collected_mutex);
                            for (auto& ev : local_events) {
                                collected_ptr->push_back(std::move(ev));
                            }
                            remaining->fetch_sub(
                                static_cast<int>(local_events.size()));
                        }
                    } else {
                        if (file_info->uncompressed_size == 0 &&
                            file_info->num_checkpoints == 0)
                            continue;

                        ViewBuilderInput builder_input;
                        builder_input.with_view(*view_ptr)
                            .with_file_path(file_info->path)
                            .with_idx_path(file_info->has_bloom_data
                                               ? file_info->idx_path
                                               : "")
                            .with_uncompressed_size(
                                file_info->uncompressed_size)
                            .with_num_checkpoints(file_info->num_checkpoints)
                            .with_bloom_cache(bloom_cache_ptr)
                            .with_time_range(t_begin, t_end);

                        ViewBuilderUtility builder;
                        auto build_output =
                            co_await builder.process(builder_input);
                        if (!build_output.success ||
                            !build_output.file_may_match)
                            continue;

                        for (const auto& candidate : build_output.candidates) {
                            if (remaining->load(std::memory_order_relaxed) <= 0)
                                break;

                            ViewReaderInput reader_input;
                            reader_input.with_file_path(file_info->path)
                                .with_idx_path(file_info->idx_path)
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
    body += ",\"limit\":";
    body += std::to_string(limit);
    body += ",\"truncated\":";
    body += truncated ? "true" : "false";
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
