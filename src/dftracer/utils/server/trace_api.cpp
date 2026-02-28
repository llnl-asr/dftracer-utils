#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/server/cursor.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_api.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/predicate_parser_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_query_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>

#include <cstddef>
#include <limits>
#include <string>
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
        body += "\",\"has_bloom_index\":";
        body += f.has_bloom_index ? "true" : "false";
        body += ",\"has_checkpoint_index\":";
        body += f.has_checkpoint_index ? "true" : "false";
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

    // Collect metadata
    auto meta_input = MetadataCollectorUtilityInput::from_file(info->path)
                          .with_index(info->idx_path);
    auto metadata = co_await MetadataCollectorUtility{}.process(meta_input);

    std::string body;
    body.reserve(512);
    body += "{\"path\":\"";
    body += json_escape(info->path);
    body += "\",\"has_bloom_index\":";
    body += info->has_bloom_index ? "true" : "false";
    body += ",\"has_checkpoint_index\":";
    body += info->has_checkpoint_index ? "true" : "false";

    if (metadata.success) {
        body += ",\"size_mb\":";
        body += std::to_string(metadata.size_mb);
        body += ",\"num_lines\":";
        body += std::to_string(metadata.num_lines);
        body += ",\"num_checkpoints\":";
        body += std::to_string(metadata.num_checkpoints);
        body += ",\"compressed_size\":";
        body += std::to_string(metadata.compressed_size);
        body += ",\"uncompressed_size\":";
        body += std::to_string(metadata.uncompressed_size);
        body += ",\"valid_events\":";
        body += std::to_string(metadata.valid_events);
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

    // Determine which files to scan
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
    std::uint64_t total_scanned = 0;
    std::uint64_t total_matched = 0;
    bool limit_reached = false;

    for (auto* file_info : target_files) {
        if (limit_reached) break;

        // Collect metadata
        auto meta_input =
            MetadataCollectorUtilityInput::from_file(file_info->path)
                .with_index(file_info->idx_path);
        auto metadata = co_await MetadataCollectorUtility{}.process(meta_input);
        if (!metadata.success) continue;

        // Run ViewBuilder to get candidate chunks
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

        // Process each candidate chunk
        for (const auto& candidate : build_output.candidates) {
            if (limit_reached) break;

            ViewReaderInput reader_input;
            reader_input.with_file_path(file_info->path)
                .with_idx_path(file_info->idx_path)
                .with_byte_range(candidate.start_byte, candidate.end_byte)
                .with_checkpoint_idx(candidate.checkpoint_idx)
                .with_view(view);

            ViewReaderUtility reader;
            auto read_output = co_await reader.process(reader_input);

            if (read_output.success) {
                total_scanned += read_output.events_scanned;
                total_matched += read_output.events_matched;

                for (auto& event : read_output.events) {
                    if (static_cast<int>(collected_events.size()) >= limit) {
                        limit_reached = true;
                        break;
                    }
                    collected_events.push_back(std::move(event));
                }
            }
        }
    }

    // Build JSON response.  Pre-compute total size to avoid
    // repeated reallocations that fragment the heap.
    std::size_t body_size = 64;  // envelope overhead
    for (const auto& ev : collected_events) body_size += ev.size() + 1;
    std::string body;
    body.reserve(body_size);
    body += "{\"events\":[";
    for (std::size_t i = 0; i < collected_events.size(); ++i) {
        if (i > 0) body += ',';
        body += collected_events[i];  // Already JSON
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

    std::string ndjson_body;
    // Reserve a reasonable initial capacity to reduce reallocation.
    ndjson_body.reserve(64 * 1024);

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
                for (const auto& event : read_output.events) {
                    ndjson_body += event;
                    ndjson_body += '\n';
                }
            }
        }
    }

    co_return HttpResponse::ok(ndjson_body, "application/x-ndjson");
}

// --- GET /api/v1/stats ---
static coro::CoroTask<HttpResponse> handle_stats(const HttpRequest& /*req*/,
                                                 const QueryParams& /*params*/,
                                                 TraceIndex& index) {
    // Aggregate statistics across all files
    std::vector<TraceStatistics> all_stats;

    for (const auto& file_info : index.files()) {
        if (!file_info.has_bloom_index) continue;

        StatisticsAggregatorInput agg_input;
        agg_input.file_path = file_info.path;
        agg_input.bidx_path = file_info.bidx_path;
        agg_input.index_dir = index.index_dir();

        StatisticsAggregatorUtility aggregator;
        auto stats = co_await aggregator.process(agg_input);

        if (stats.success) {
            all_stats.push_back(std::move(stats));
        }
    }

    // Build summary JSON
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

    // Per-file stats
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
        body += "\",\"has_bloom_index\":";
        body += f.has_bloom_index ? "true" : "false";
        body += ",\"has_checkpoint_index\":";
        body += f.has_checkpoint_index ? "true" : "false";
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
