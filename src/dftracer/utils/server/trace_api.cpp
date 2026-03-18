#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/server/cursor.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_api.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/utilities/common/json/json_doc_guard.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/composites/dft/indexing/predicate_parser_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_query_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/predicate_filter.h>
#include <dftracer/utils/utilities/composites/dft/views/view_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <yyjson.h>

#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::server {

using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::composites::dft::statistics;
using namespace dftracer::utils::utilities::composites::dft::views;

// JSON-escape a string value (minimal: quotes, backslash, control chars).
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

using dftracer::utils::utilities::common::json::JsonValue;
using dftracer::utils::utilities::composites::dft::views::
    build_predicate_filter;
using dftracer::utils::utilities::composites::dft::views::matches_any_predicate;
using dftracer::utils::utilities::composites::dft::views::matches_predicate;
using dftracer::utils::utilities::composites::dft::views::
    metadata_matches_identity;
using dftracer::utils::utilities::composites::dft::views::PredicateFilter;

// Hash metadata types that need smart filtering (FH, HH, SH).
static const std::unordered_set<std::string> HASH_METADATA_NAMES = {"FH", "HH",
                                                                    "SH"};

using dftracer::utils::utilities::common::json::JsonDocGuard;

/// Direct-scan a small file without any sidecar index.
/// Streams via async_streaming_gz_lines(), parses JSON, applies
/// predicate filters, collects matching events as raw JSON strings.
static coro::CoroTask<void> direct_scan_events(
    const TraceIndex::FileInfo* file_info,
    const std::vector<PredicateFilter>& filters, bool include_metadata,
    std::vector<std::string>* collected_events, std::uint64_t* total_scanned,
    std::uint64_t* total_matched, int limit) {
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
                        if (metadata_matches_identity(json, filters)) {
                            collected_events->emplace_back(
                                line->content.data(), line->content.size());
                            (*total_matched)++;
                        }
                    }
                } else if (ph != "M") {
                    (*total_scanned)++;
                    if (matches_any_predicate(json, filters)) {
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

// --- GET /api/v1/files ---
static coro::CoroTask<HttpResponse> handle_files(const HttpRequest& /*req*/,
                                                 const QueryParams& /*params*/,
                                                 TraceIndex& index) {
    std::string body;
    body.reserve(128 * index.file_count() + 32);
    body += "{\"files\":[";
    bool first = true;
    for (const auto& f : index.files()) {
        if (!first) body += ',';
        first = false;
        body += "{\"path\":\"";
        body += json_escape(f.path);
        body += "\",\"has_bloom_data\":";
        body += f.has_bloom_data ? "true" : "false";
        body += ",\"has_checkpoint_index\":";
        body += f.has_checkpoint_index ? "true" : "false";
        body += ",\"is_small\":";
        body += f.is_small ? "true" : "false";
        body += '}';
    }
    body += "],\"count\":";
    body += std::to_string(index.file_count());
    body += '}';
    co_return HttpResponse::ok(body);
}

// --- GET /api/v1/files/info ---
static coro::CoroTask<HttpResponse> handle_file_info(const HttpRequest& /*req*/,
                                                     const QueryParams& params,
                                                     TraceIndex& index) {
    auto file_param = params.get("file");
    if (file_param.empty()) {
        co_return HttpResponse::bad_request("Missing required parameter: file");
    }

    std::string file_path(file_param);
    auto* info = index.find_file(file_path);
    if (!info) {
        co_return HttpResponse::not_found();
    }

    std::string body;
    body.reserve(512);
    body += "{\"path\":\"";
    body += json_escape(info->path);
    body += "\",\"has_bloom_data\":";
    body += info->has_bloom_data ? "true" : "false";
    body += ",\"has_checkpoint_index\":";
    body += info->has_checkpoint_index ? "true" : "false";
    body += ",\"is_small\":";
    body += info->is_small ? "true" : "false";

    body += ",\"size_mb\":";
    body += std::to_string(info->size_mb);
    body += ",\"compressed_size\":";
    body += std::to_string(info->compressed_size);
    if (!info->is_small) {
        body += ",\"num_lines\":";
        body += std::to_string(info->num_lines);
        body += ",\"num_checkpoints\":";
        body += std::to_string(info->num_checkpoints);
        body += ",\"uncompressed_size\":";
        body += std::to_string(info->uncompressed_size);
    }

    body += '}';
    co_return HttpResponse::ok(body);
}

// Build a ViewDefinition from query parameters.
static ViewDefinition build_view_from_params(const QueryParams& params) {
    ViewDefinition view;
    view.name = "api_query";
    view.description = "HTTP API query";

    ViewPredicate pred;

    auto cat = params.get("cat");
    if (!cat.empty()) {
        // Split comma-separated categories
        std::vector<std::string> values;
        std::string token;
        for (char c : std::string(cat)) {
            if (c == ',') {
                if (!token.empty()) values.push_back(token);
                token.clear();
            } else {
                token += c;
            }
        }
        if (!token.empty()) values.push_back(token);
        pred.with_bloom_dim("category", values);
    }

    auto name = params.get("name");
    if (!name.empty()) {
        std::vector<std::string> values;
        std::string token;
        for (char c : std::string(name)) {
            if (c == ',') {
                if (!token.empty()) values.push_back(token);
                token.clear();
            } else {
                token += c;
            }
        }
        if (!token.empty()) values.push_back(token);
        pred.with_bloom_dim("name", values);
    }

    auto pid = params.get("pid");
    if (!pid.empty()) {
        pred.with_bloom_dim("process_id", {std::string(pid)});
    }

    double ts_min = params.get_double("ts_min", 0);
    double ts_max = params.get_double("ts_max", 0);
    if (ts_min > 0 || ts_max > 0) {
        pred.with_time_range(ts_min, ts_max);
    }

    double dur_min = params.get_double("dur_min", 0);
    double dur_max = params.get_double("dur_max", 0);
    if (dur_min > 0) pred.with_min_duration(dur_min);
    if (dur_max > 0) pred.with_max_duration(dur_max);

    // Only add predicate if any filter was specified.
    // If no filters, create an empty predicate (match all).
    view.with_predicate(std::move(pred));
    return view;
}

// --- GET /api/v1/events ---
static coro::CoroTask<HttpResponse> handle_events(const HttpRequest& /*req*/,
                                                  const QueryParams& params,
                                                  TraceIndex& index) {
    int limit = params.get_int("limit", 1000);
    if (limit <= 0) limit = 1000;
    if (limit > 100000) limit = 100000;

    auto view = build_view_from_params(params);

    double query_ts_min = 0;
    double query_ts_max = 0;
    if (!view.predicates.empty() && view.predicates[0].time_range) {
        query_ts_min = view.predicates[0].time_range->first;
        query_ts_max = view.predicates[0].time_range->second;
    }

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

    // File-level time range skip
    if (query_ts_min > 0 || query_ts_max > 0) {
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
            if (fi_max < query_ts_min ||
                (query_ts_max > 0 && fi_min > query_ts_max))
                continue;
            filtered.push_back(fi);
        }
        target_files = std::move(filtered);
    }

    std::vector<PredicateFilter> pred_filters;
    for (const auto& predicate : view.predicates) {
        pred_filters.push_back(build_predicate_filter(predicate));
    }

    std::vector<std::string> collected_events;
    std::uint64_t total_scanned = 0;
    std::uint64_t total_matched = 0;

    if (target_files.size() <= 1) {
        bool limit_reached = false;
        for (auto* file_info : target_files) {
            if (limit_reached) break;
            if (file_info->is_small) {
                co_await direct_scan_events(
                    file_info, pred_filters, view.include_metadata,
                    &collected_events, &total_scanned, &total_matched, limit);
                limit_reached =
                    collected_events.size() >= static_cast<std::size_t>(limit);
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
                    .with_time_range(query_ts_min, query_ts_max);

                ViewBuilderUtility builder;
                auto build_output = co_await builder.process(builder_input);
                if (!build_output.success || !build_output.file_may_match)
                    continue;

                for (const auto& candidate : build_output.candidates) {
                    if (limit_reached) break;

                    ViewReaderInput reader_input;
                    reader_input.with_file_path(file_info->path)
                        .with_idx_path(file_info->idx_path)
                        .with_byte_range(candidate.start_byte,
                                         candidate.end_byte)
                        .with_checkpoint_idx(candidate.checkpoint_idx)
                        .with_view(view);

                    ViewReaderUtility reader;
                    auto read_output = co_await reader.process(reader_input);

                    if (read_output.success) {
                        total_scanned += read_output.events_scanned;
                        total_matched += read_output.events_matched;
                        for (auto& event : read_output.events) {
                            if (collected_events.size() >=
                                static_cast<std::size_t>(limit)) {
                                limit_reached = true;
                                break;
                            }
                            collected_events.push_back(std::move(event));
                        }
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
        auto remaining = std::make_shared<std::atomic<int>>(limit);
        auto scanned_atomic = std::make_shared<std::atomic<std::uint64_t>>(0);
        auto matched_atomic = std::make_shared<std::atomic<std::uint64_t>>(0);

        auto* target_files_ptr = &target_files;
        auto* collected_ptr = &collected_events;
        auto* pred_filters_ptr = &pred_filters;
        auto* view_ptr = &view;
        auto* bloom_cache_ptr = &index.bloom_cache();
        double ev_ts_min = query_ts_min;
        double ev_ts_max = query_ts_max;

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
                         collected_ptr, remaining, scanned_atomic,
                         matched_atomic, pred_filters_ptr, view_ptr,
                         bloom_cache_ptr, ev_ts_min,
                         ev_ts_max](CoroScope&) -> coro::CoroTask<void> {
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
                            file_info, *pred_filters_ptr,
                            view_ptr->include_metadata, &local_events,
                            &local_scanned, &local_matched, local_limit);
                        scanned_atomic->fetch_add(local_scanned);
                        matched_atomic->fetch_add(local_matched);
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
                            .with_time_range(ev_ts_min, ev_ts_max);

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
                            auto read_output =
                                co_await reader.process(reader_input);

                            if (read_output.success) {
                                scanned_atomic->fetch_add(
                                    read_output.events_scanned);
                                matched_atomic->fetch_add(
                                    read_output.events_matched);
                                if (!read_output.events.empty()) {
                                    std::lock_guard<std::mutex> lock(
                                        *collected_mutex);
                                    for (auto& event : read_output.events) {
                                        collected_ptr->push_back(
                                            std::move(event));
                                    }
                                    remaining->fetch_sub(static_cast<int>(
                                        read_output.events.size()));
                                }
                            }
                        }
                    }
                }
                co_return;
            });
        }

        co_await scope.join();

        total_scanned = scanned_atomic->load();
        total_matched = matched_atomic->load();
        if (collected_events.size() > static_cast<std::size_t>(limit)) {
            collected_events.resize(static_cast<std::size_t>(limit));
        }
    }

    std::size_t body_size = 64;
    for (const auto& ev : collected_events) body_size += ev.size() + 1;
    std::string body;
    body.reserve(body_size);
    body += "{\"events\":[";
    for (std::size_t i = 0; i < collected_events.size(); ++i) {
        if (i > 0) body += ',';
        body += collected_events[i];
    }
    body += "],\"total_scanned\":";
    body += std::to_string(total_scanned);
    body += ",\"total_matched\":";
    body += std::to_string(total_matched);
    body += ",\"count\":";
    body += std::to_string(collected_events.size());
    body += '}';

    co_return HttpResponse::ok(body);
}

// --- GET /api/v1/events/stream ---
// Returns all matching events as NDJSON (newline-delimited JSON).
static coro::CoroTask<HttpResponse> handle_events_stream(
    const HttpRequest& /*req*/, const QueryParams& params, TraceIndex& index) {
    auto view = build_view_from_params(params);

    double stream_ts_min = 0;
    double stream_ts_max = 0;
    if (!view.predicates.empty() && view.predicates[0].time_range) {
        stream_ts_min = view.predicates[0].time_range->first;
        stream_ts_max = view.predicates[0].time_range->second;
    }

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

    // File-level time range skip
    if (stream_ts_min > 0 || stream_ts_max > 0) {
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
            if (fi_max < stream_ts_min ||
                (stream_ts_max > 0 && fi_min > stream_ts_max))
                continue;
            filtered.push_back(fi);
        }
        target_files = std::move(filtered);
    }

    std::vector<PredicateFilter> pred_filters;
    for (const auto& predicate : view.predicates) {
        pred_filters.push_back(build_predicate_filter(predicate));
    }

    std::string ndjson_body;

    if (target_files.size() <= 1) {
        ndjson_body.reserve(64 * 1024);
        for (auto* file_info : target_files) {
            if (file_info->is_small) {
                std::vector<std::string> events;
                std::uint64_t scanned = 0;
                std::uint64_t matched = 0;
                co_await direct_scan_events(file_info, pred_filters,
                                            view.include_metadata, &events,
                                            &scanned, &matched, 0);
                for (const auto& event : events) {
                    ndjson_body += event;
                    ndjson_body += '\n';
                }
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
                    .with_time_range(stream_ts_min, stream_ts_max);

                ViewBuilderUtility builder;
                auto build_output = co_await builder.process(builder_input);
                if (!build_output.success || !build_output.file_may_match)
                    continue;

                for (const auto& candidate : build_output.candidates) {
                    ViewReaderInput reader_input;
                    reader_input.with_file_path(file_info->path)
                        .with_idx_path(file_info->idx_path)
                        .with_byte_range(candidate.start_byte,
                                         candidate.end_byte)
                        .with_checkpoint_idx(candidate.checkpoint_idx)
                        .with_view(view);

                    ViewReaderUtility reader;
                    auto read_output = co_await reader.process(reader_input);

                    if (read_output.success) {
                        for (const auto& event : read_output.events) {
                            ndjson_body += event;
                            ndjson_body += '\n';
                        }
                    }
                }
            }
        }
    } else {
        std::size_t num_workers =
            std::min(index.max_concurrent(), target_files.size());
        auto* executor = Executor::current();

        auto file_chan = coro::make_channel<std::size_t>(num_workers * 2);
        auto body_mutex = std::make_shared<std::mutex>();
        auto* ndjson_ptr = &ndjson_body;
        auto* target_files_ptr = &target_files;
        auto* pred_filters_ptr = &pred_filters;
        auto* view_ptr = &view;
        auto* bloom_cache_ptr = &index.bloom_cache();
        double st_ts_min = stream_ts_min;
        double st_ts_max = stream_ts_max;

        ndjson_body.reserve(64 * 1024);

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
            scope.spawn([file_chan, target_files_ptr, body_mutex, ndjson_ptr,
                         pred_filters_ptr, view_ptr, bloom_cache_ptr, st_ts_min,
                         st_ts_max](CoroScope&) -> coro::CoroTask<void> {
                while (auto fi_opt = co_await file_chan->receive()) {
                    auto* file_info = (*target_files_ptr)[*fi_opt];
                    std::string local_buf;

                    if (file_info->is_small) {
                        std::vector<std::string> events;
                        std::uint64_t scanned = 0;
                        std::uint64_t matched = 0;
                        co_await direct_scan_events(
                            file_info, *pred_filters_ptr,
                            view_ptr->include_metadata, &events, &scanned,
                            &matched, 0);
                        for (const auto& event : events) {
                            local_buf += event;
                            local_buf += '\n';
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
                            .with_time_range(st_ts_min, st_ts_max);

                        ViewBuilderUtility builder;
                        auto build_output =
                            co_await builder.process(builder_input);
                        if (!build_output.success ||
                            !build_output.file_may_match)
                            continue;

                        for (const auto& candidate : build_output.candidates) {
                            ViewReaderInput reader_input;
                            reader_input.with_file_path(file_info->path)
                                .with_idx_path(file_info->idx_path)
                                .with_byte_range(candidate.start_byte,
                                                 candidate.end_byte)
                                .with_checkpoint_idx(candidate.checkpoint_idx)
                                .with_view(*view_ptr);

                            ViewReaderUtility reader;
                            auto read_output =
                                co_await reader.process(reader_input);

                            if (read_output.success) {
                                for (const auto& event : read_output.events) {
                                    local_buf += event;
                                    local_buf += '\n';
                                }
                            }
                        }
                    }

                    if (!local_buf.empty()) {
                        std::lock_guard<std::mutex> lock(*body_mutex);
                        ndjson_ptr->append(local_buf);
                    }
                }
                co_return;
            });
        }

        co_await scope.join();
    }

    co_return HttpResponse::ok(ndjson_body, "application/x-ndjson");
}

// --- GET /api/v1/stats ---
static coro::CoroTask<HttpResponse> handle_stats(const HttpRequest& /*req*/,
                                                 const QueryParams& /*params*/,
                                                 TraceIndex& index) {
    std::vector<TraceStatistics> all_stats;
    std::size_t skipped_small = 0;

    std::vector<const TraceIndex::FileInfo*> stat_files;
    for (const auto& file_info : index.files()) {
        if (file_info.is_small) {
            skipped_small++;
            continue;
        }
        if (!file_info.has_bloom_data) continue;
        stat_files.push_back(&file_info);
    }

    if (stat_files.size() <= 1) {
        for (auto* file_info : stat_files) {
            StatisticsAggregatorInput agg_input;
            agg_input.file_path = file_info->path;
            agg_input.idx_path = file_info->idx_path;
            agg_input.index_dir = index.index_dir();

            StatisticsAggregatorUtility aggregator;
            auto stats = co_await aggregator.process(agg_input);
            if (stats.success) {
                all_stats.push_back(std::move(stats));
            }
        }
    } else {
        std::size_t num_workers =
            std::min(index.max_concurrent(), stat_files.size());
        auto* executor = Executor::current();

        auto file_chan = coro::make_channel<std::size_t>(num_workers * 2);
        auto stats_mutex = std::make_shared<std::mutex>();
        auto* all_stats_ptr = &all_stats;
        auto* stat_files_ptr = &stat_files;
        std::string index_dir = index.index_dir();

        CoroScope scope(executor);

        scope.spawn([ch = file_chan->producer(), stat_files_ptr](
                        CoroScope&) mutable -> coro::CoroTask<void> {
            auto guard = ch.guard();
            for (std::size_t i = 0; i < stat_files_ptr->size(); ++i) {
                if (!co_await ch.send(i)) co_return;
            }
            co_return;
        });

        for (std::size_t w = 0; w < num_workers; ++w) {
            scope.spawn([file_chan, stat_files_ptr, stats_mutex, all_stats_ptr,
                         index_dir](CoroScope&) -> coro::CoroTask<void> {
                while (auto fi_opt = co_await file_chan->receive()) {
                    auto* file_info = (*stat_files_ptr)[*fi_opt];

                    StatisticsAggregatorInput agg_input;
                    agg_input.file_path = file_info->path;
                    agg_input.idx_path = file_info->idx_path;
                    agg_input.index_dir = index_dir;

                    StatisticsAggregatorUtility aggregator;
                    auto stats = co_await aggregator.process(agg_input);

                    if (stats.success) {
                        std::lock_guard<std::mutex> lock(*stats_mutex);
                        all_stats_ptr->push_back(std::move(stats));
                    }
                }
                co_return;
            });
        }

        co_await scope.join();
    }

    std::uint64_t total_events = 0;
    std::size_t file_count = all_stats.size();
    for (const auto& s : all_stats) {
        total_events += s.total_events();
    }

    std::string body;
    body.reserve(256 * all_stats.size() + 64);
    body += "{\"file_count\":";
    body += std::to_string(file_count);
    body += ",\"total_events\":";
    body += std::to_string(total_events);
    body += ",\"skipped_small_files\":";
    body += std::to_string(skipped_small);
    body += ",\"files\":[";
    for (std::size_t i = 0; i < all_stats.size(); ++i) {
        if (i > 0) body += ',';
        body += all_stats[i].to_json();
    }
    body += "]}";

    co_return HttpResponse::ok(body);
}

// --- GET /api/v1/info ---
static coro::CoroTask<HttpResponse> handle_info(const HttpRequest& /*req*/,
                                                const QueryParams& /*params*/,
                                                TraceIndex& index) {
    auto global_min = index.global_min_timestamp_us();
    auto global_max = index.global_max_timestamp_us();
    bool has_time_range =
        global_max > 0 &&
        global_min != std::numeric_limits<std::uint64_t>::max();

    std::string body;
    body.reserve(256 * index.file_count() + 128);
    body += "{\"file_count\":";
    body += std::to_string(index.file_count());

    if (has_time_range) {
        body += ",\"time_range\":{\"min_timestamp_us\":";
        body += std::to_string(global_min);
        body += ",\"max_timestamp_us\":";
        body += std::to_string(global_max);
        body += "}";
    }

    body += ",\"files\":[";
    bool first = true;
    for (const auto& f : index.files()) {
        if (!first) body += ',';
        first = false;
        body += "{\"path\":\"";
        body += json_escape(f.path);
        body += "\",\"has_bloom_data\":";
        body += f.has_bloom_data ? "true" : "false";
        body += ",\"has_checkpoint_index\":";
        body += f.has_checkpoint_index ? "true" : "false";
        body += ",\"is_small\":";
        body += f.is_small ? "true" : "false";
        if (f.min_timestamp_us > 0 || f.max_timestamp_us > 0) {
            body += ",\"min_timestamp_us\":";
            body += std::to_string(f.min_timestamp_us);
            body += ",\"max_timestamp_us\":";
            body += std::to_string(f.max_timestamp_us);
        }
        body += '}';
    }
    body += "]}";
    co_return HttpResponse::ok(body);
}

void register_trace_api(Router& router, TraceIndex& index) {
    auto* index_ptr = &index;

    router.get(
        "/api/v1/files",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_files(req, params, *index_ptr);
        });

    router.get(
        "/api/v1/files/info",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_file_info(req, params, *index_ptr);
        });

    router.get(
        "/api/v1/events",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_events(req, params, *index_ptr);
        });

    router.get(
        "/api/v1/events/stream",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_events_stream(req, params, *index_ptr);
        });

    router.get(
        "/api/v1/stats",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_stats(req, params, *index_ptr);
        });

    router.get(
        "/api/v1/info",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_info(req, params, *index_ptr);
        });
}

}  // namespace dftracer::utils::server
