#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/server/cursor.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/json_builder.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_api.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/utilities/common/json/json_doc_guard.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/statistics/shared_index_statistics_reader.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_query_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <simdjson.h>

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

// Hash metadata types that need smart filtering (FH, HH, SH).
static const std::unordered_set<std::string> HASH_METADATA_NAMES = {"FH", "HH",
                                                                    "SH"};

using dftracer::utils::utilities::common::query::Query;

// --- GET /api/v1/files ---
static coro::CoroTask<HttpResponse> handle_files(const HttpRequest& /*req*/,
                                                 const QueryParams& /*params*/,
                                                 TraceIndex& index) {
    auto& b = scratch_json_builder();
    b.start_object();
    b.escape_and_append_with_quotes("files");
    b.append_colon();
    b.start_array();
    bool first = true;
    for (const auto& f : index.files()) {
        if (!first) b.append_comma();
        first = false;
        b.start_object();
        b.append_key_value("path", f.path);
        b.append_comma();
        b.append_key_value("has_bloom_data", f.has_bloom_data);
        b.append_comma();
        b.append_key_value("has_checkpoint_index", f.has_checkpoint_index);
        b.end_object();
    }
    b.end_array();
    b.append_comma();
    b.append_key_value("count", static_cast<std::int64_t>(index.file_count()));
    b.end_object();
    co_return HttpResponse::ok(std::string(b));
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

    auto& b = scratch_json_builder();
    b.start_object();
    b.append_key_value("path", info->path);
    b.append_comma();
    b.append_key_value("has_bloom_data", info->has_bloom_data);
    b.append_comma();
    b.append_key_value("has_checkpoint_index", info->has_checkpoint_index);
    b.append_comma();
    b.append_key_value("size_mb", info->size_mb);
    b.append_comma();
    b.append_key_value("compressed_size",
                       static_cast<std::int64_t>(info->compressed_size));
    b.append_comma();
    b.append_key_value("num_lines", static_cast<std::int64_t>(info->num_lines));
    b.append_comma();
    b.append_key_value("num_checkpoints",
                       static_cast<std::int64_t>(info->num_checkpoints));
    b.append_comma();
    b.append_key_value("uncompressed_size",
                       static_cast<std::int64_t>(info->uncompressed_size));
    b.end_object();
    co_return HttpResponse::ok(std::string(b));
}

static std::vector<std::string> split_csv(std::string_view s) {
    std::vector<std::string> result;
    std::string token;
    for (char c : s) {
        if (c == ',') {
            if (!token.empty()) result.push_back(token);
            token.clear();
        } else {
            token += c;
        }
    }
    if (!token.empty()) result.push_back(token);
    return result;
}

static std::string format_in_clause(const std::string& field,
                                    const std::vector<std::string>& vals) {
    if (vals.size() == 1) return field + " == \"" + vals[0] + "\"";
    std::string s = field + " in [";
    for (std::size_t i = 0; i < vals.size(); ++i) {
        if (i > 0) s += ", ";
        s += "\"" + vals[i] + "\"";
    }
    s += "]";
    return s;
}

static std::optional<Query> build_query_from_params(const QueryParams& params) {
    std::string dsl;

    auto cat = params.get("cat");
    if (!cat.empty()) {
        auto vals = split_csv(cat);
        if (!vals.empty()) dsl += format_in_clause("cat", vals);
    }

    auto name = params.get("name");
    if (!name.empty()) {
        auto vals = split_csv(name);
        if (!vals.empty()) {
            if (!dsl.empty()) dsl += " and ";
            dsl += format_in_clause("name", vals);
        }
    }

    auto pid = params.get("pid");
    if (!pid.empty()) {
        if (!dsl.empty()) dsl += " and ";
        dsl += "pid == " + std::string(pid);
    }

    double ts_min = params.get_double("ts_min", 0);
    double ts_max = params.get_double("ts_max", 0);
    if (ts_min > 0) {
        if (!dsl.empty()) dsl += " and ";
        dsl += "ts >= " + std::to_string(static_cast<uint64_t>(ts_min));
    }
    if (ts_max > 0) {
        if (!dsl.empty()) dsl += " and ";
        dsl += "ts <= " + std::to_string(static_cast<uint64_t>(ts_max));
    }

    double dur_min = params.get_double("dur_min", 0);
    double dur_max = params.get_double("dur_max", 0);
    if (dur_min > 0) {
        if (!dsl.empty()) dsl += " and ";
        dsl += "dur >= " + std::to_string(static_cast<uint64_t>(dur_min));
    }
    if (dur_max > 0) {
        if (!dsl.empty()) dsl += " and ";
        dsl += "dur <= " + std::to_string(static_cast<uint64_t>(dur_max));
    }

    if (dsl.empty()) return std::nullopt;
    auto result = Query::from_string(dsl);
    if (!result) return std::nullopt;
    return std::move(*result);
}

static ViewDefinition build_view_from_params(const QueryParams& params) {
    ViewDefinition view;
    view.name = "api_query";
    view.description = "HTTP API query";

    auto q = build_query_from_params(params);
    if (q) view.with_query(std::move(*q));
    return view;
}

// ============================================================================
// Shared helpers for event streaming endpoints
// ============================================================================

static std::vector<const TraceIndex::FileInfo*> resolve_target_files(
    TraceIndex& index, const QueryParams& params, double ts_min = 0,
    double ts_max = 0) {
    auto files = collect_candidate_files(index, params);

    if (ts_min > 0 || ts_max > 0) {
        std::vector<const TraceIndex::FileInfo*> filtered;
        filtered.reserve(files.size());
        for (auto* fi : files) {
            if (fi->min_timestamp_us == 0 && fi->max_timestamp_us == 0) {
                filtered.push_back(fi);
                continue;
            }
            double fi_min = static_cast<double>(fi->min_timestamp_us);
            double fi_max = static_cast<double>(fi->max_timestamp_us);
            if (fi_max < ts_min || (ts_max > 0 && fi_min > ts_max)) continue;
            filtered.push_back(fi);
        }
        files = std::move(filtered);
    }

    return files;
}

using StreamChunk = HttpResponse::StreamChunk;

static coro::AsyncGenerator<StreamChunk> stream_events(
    std::vector<const TraceIndex::FileInfo*> files, ViewDefinition ev_view,
    std::optional<Query> /*query_opt*/, double ts_min, double ts_max,
    BloomFilterCache* bloom_cache, int limit) {
    int emitted = 0;

    for (auto* file_info : files) {
        if (limit > 0 && emitted >= limit) break;

        if (file_info->uncompressed_size == 0 &&
            file_info->num_checkpoints == 0)
            continue;

        ViewBuilderInput builder_input;
        builder_input.with_view(ev_view)
            .with_file_path(file_info->path)
            .with_index_path(file_info->has_bloom_data ? file_info->index_path
                                                       : "")
            .with_uncompressed_size(file_info->uncompressed_size)
            .with_num_checkpoints(file_info->num_checkpoints)
            .with_bloom_cache(bloom_cache)
            .with_time_range(ts_min, ts_max);

        ViewBuilderUtility builder;
        auto build_output = co_await builder.process(builder_input);
        if (!build_output || !build_output->file_may_match) continue;

        for (const auto& candidate : build_output->candidates) {
            if (limit > 0 && emitted >= limit) break;

            ViewReaderInput reader_input;
            reader_input.with_file_path(file_info->path)
                .with_index_path(file_info->index_path)
                .with_byte_range(candidate.start_byte, candidate.end_byte)
                .with_checkpoint_idx(candidate.checkpoint_idx)
                .with_view(ev_view);

            ViewReaderUtility reader;
            auto event_gen = reader.process(reader_input);
            while (auto batch = co_await event_gen.next()) {
                int count = std::min(
                    static_cast<int>(batch->events.size()),
                    limit > 0 ? limit - emitted
                              : static_cast<int>(batch->events.size()));
                if (count > 0) {
                    co_yield StreamChunk{std::span<const std::string_view>(
                        batch->events.data(), static_cast<std::size_t>(count))};
                    emitted += count;
                }
            }
        }
    }
}

// ============================================================================
// Event endpoints
// ============================================================================

// --- GET /api/v1/events ---
static coro::CoroTask<HttpResponse> handle_events(const HttpRequest& /*req*/,
                                                  const QueryParams& params,
                                                  TraceIndex& index) {
    int limit = params.get_int("limit", 1000);
    if (limit <= 0) limit = 1000;
    if (limit > 100000) limit = 100000;

    double ts_min = params.get_double("ts_min", 0);
    double ts_max = params.get_double("ts_max", 0);
    auto files = resolve_target_files(index, params, ts_min, ts_max);
    auto view = build_view_from_params(params);
    auto query = build_query_from_params(params);

    auto gen = std::make_unique<HttpResponse::StreamGenerator>(
        stream_events(std::move(files), std::move(view), std::move(query),
                      ts_min, ts_max, &index.bloom_cache(), limit));

    auto resp = HttpResponse::streaming(std::move(gen));
    resp.headers.push_back({"X-Limit", std::to_string(limit)});
    co_return resp;
}

// --- GET /api/v1/events/stream ---
static coro::CoroTask<HttpResponse> handle_events_stream(
    const HttpRequest& /*req*/, const QueryParams& params, TraceIndex& index) {
    double ts_min = params.get_double("ts_min", 0);
    double ts_max = params.get_double("ts_max", 0);
    auto files = resolve_target_files(index, params, ts_min, ts_max);
    auto view = build_view_from_params(params);
    auto query = build_query_from_params(params);
    int limit = params.get_int("limit", 0);

    auto gen = std::make_unique<HttpResponse::StreamGenerator>(
        stream_events(std::move(files), std::move(view), std::move(query),
                      ts_min, ts_max, &index.bloom_cache(), limit));

    co_return HttpResponse::streaming(std::move(gen));
}

// --- GET /api/v1/stats ---
static coro::CoroTask<HttpResponse> handle_stats(const HttpRequest& req,
                                                 const QueryParams& /*params*/,
                                                 TraceIndex& index) {
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, std::string,
                              dftracer::utils::TransparentStringHash,
                              dftracer::utils::TransparentStringEqual>
        stats_cache;

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = stats_cache.find(req.path);
        if (it != stats_cache.end()) {
            co_return HttpResponse::ok(it->second);
        }
    }

    std::vector<TraceStatistics> all_stats;

    // Group files by index_path
    std::unordered_map<std::string,
                       std::vector<std::pair<std::size_t, std::string>>>
        files_by_index;
    std::size_t file_idx = 0;
    for (const auto& file_info : index.files()) {
        if (!file_info.has_bloom_data) continue;
        files_by_index[file_info.index_path].emplace_back(file_idx++,
                                                          file_info.path);
    }

    // Resolve each group and read statistics
    for (auto& [idx_path, files] : files_by_index) {
        std::vector<std::string> file_paths;
        file_paths.reserve(files.size());
        for (const auto& [_, path] : files) {
            file_paths.push_back(path);
        }

        IndexResolverUtility resolver;
        ResolverInput input;
        input.files = std::move(file_paths);
        input.require_checkpoints = false;

        auto result = co_await resolver.process(input);

        if (result.cached.empty()) {
            continue;
        }

        try {
            SharedIndexStatisticsReader reader;
            auto batch_rows = co_await reader.query(
                result.index_path, std::move(result.cached),
                StatisticsQueryType::SUMMARY);
            auto callback = [&all_stats](std::size_t /*file_index*/,
                                         TraceStatistics&& stats) {
                if (stats.success) {
                    all_stats.push_back(std::move(stats));
                }
            };
            SharedIndexStatisticsReader::process_batch_results(batch_rows,
                                                               callback);
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN("Server stats batch read failed for %s: %s",
                                    idx_path.c_str(), e.what());
        }
    }

    std::uint64_t total_events = 0;
    std::size_t file_count = all_stats.size();
    for (const auto& s : all_stats) {
        total_events += s.total_events();
    }

    auto& sb = scratch_json_builder();
    sb.start_object();
    sb.append_key_value("file_count", static_cast<std::int64_t>(file_count));
    sb.append_comma();
    sb.append_key_value("total_events",
                        static_cast<std::int64_t>(total_events));
    sb.append_comma();
    sb.escape_and_append_with_quotes("files");
    sb.append_colon();
    sb.start_array();
    for (std::size_t i = 0; i < all_stats.size(); ++i) {
        if (i > 0) sb.append_comma();
        sb.append_raw(all_stats[i].to_json());  // already JSON
    }
    sb.end_array();
    sb.end_object();
    std::string body(sb);

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        stats_cache.emplace(std::string(req.path), body);
    }
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

    auto& b = scratch_json_builder();
    b.start_object();
    b.append_key_value("file_count",
                       static_cast<std::int64_t>(index.file_count()));

    if (has_time_range) {
        b.append_comma();
        b.escape_and_append_with_quotes("time_range");
        b.append_colon();
        b.start_object();
        b.append_key_value("min_timestamp_us",
                           static_cast<std::int64_t>(global_min));
        b.append_comma();
        b.append_key_value("max_timestamp_us",
                           static_cast<std::int64_t>(global_max));
        b.end_object();
    }

    b.append_comma();
    b.escape_and_append_with_quotes("files");
    b.append_colon();
    b.start_array();
    bool first = true;
    for (const auto& f : index.files()) {
        if (!first) b.append_comma();
        first = false;
        b.start_object();
        b.append_key_value("path", f.path);
        b.append_comma();
        b.append_key_value("has_bloom_data", f.has_bloom_data);
        b.append_comma();
        b.append_key_value("has_checkpoint_index", f.has_checkpoint_index);
        if (f.min_timestamp_us > 0 || f.max_timestamp_us > 0) {
            b.append_comma();
            b.append_key_value("min_timestamp_us",
                               static_cast<std::int64_t>(f.min_timestamp_us));
            b.append_comma();
            b.append_key_value("max_timestamp_us",
                               static_cast<std::int64_t>(f.max_timestamp_us));
        }
        b.end_object();
    }
    b.end_array();
    b.end_object();
    co_return HttpResponse::ok(std::string(b));
}

void register_trace_api(Router& router, TraceIndex& index) {
    auto* index_ptr = &index;

    router.get(
        "/api/v1/files",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_files(req, params, *index_ptr);
        },
        RouteDoc{
            "List the indexed trace files.",
            "Trace data",
            {},
            R"({"files":[{"path":"trace-0.pfw.gz","has_bloom_data":true}],)"
            R"("count":1})"});

    router.get(
        "/api/v1/files/info",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_file_info(req, params, *index_ptr);
        },
        RouteDoc{"Metadata for one trace file.",
                 "Trace data",
                 {{"file", "Trace file path", true, ""}},
                 R"({"path":"trace-0.pfw.gz","has_bloom_data":true})"});

    router.get(
        "/api/v1/events",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_events(req, params, *index_ptr);
        },
        RouteDoc{"Query raw events as NDJSON (filtered, limited).",
                 "Trace data",
                 {{"file", "Trace file path", false, ""},
                  {"name", "Filter by operation name", false, "read"},
                  {"dur_min", "Minimum duration (us)", false, ""},
                  {"limit", "Max events (0 = all)", false, "20"}},
                 R"({"id":1,"name":"read","cat":"POSIX","pid":100,"tid":100,)"
                 R"("ts":1000,"dur":150,"args":{"ret":4096}})"});

    router.get(
        "/api/v1/events/stream",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_events_stream(req, params, *index_ptr);
        },
        RouteDoc{"Stream all matching events as NDJSON (no limit).",
                 "Trace data",
                 {{"name", "Filter by operation name", false, ""}},
                 ""});

    router.get(
        "/api/v1/stats",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_stats(req, params, *index_ptr);
        },
        RouteDoc{"Aggregate statistics over the index.", "Trace data", {}, ""});

    router.get(
        "/api/v1/info",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_info(req, params, *index_ptr);
        },
        RouteDoc{"Global summary: file count and time bounds.",
                 "Trace data",
                 {},
                 R"({"file_count":2,"global_min_timestamp_us":1000000,)"
                 R"("global_max_timestamp_us":6999732})"});
}

}  // namespace dftracer::utils::server
