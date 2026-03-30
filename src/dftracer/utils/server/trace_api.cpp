#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
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
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_query_utility.h>
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

// Hash metadata types that need smart filtering (FH, HH, SH).
static const std::unordered_set<std::string> HASH_METADATA_NAMES = {"FH", "HH",
                                                                    "SH"};

using dftracer::utils::utilities::common::json::JsonDocGuard;
using dftracer::utils::utilities::common::query::Query;

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
    std::vector<const TraceIndex::FileInfo*> files;
    auto file_param = params.get("file");
    if (!file_param.empty()) {
        auto* f = index.find_file(std::string(file_param));
        if (f) files.push_back(f);
    } else {
        for (const auto& f : index.files()) {
            files.push_back(&f);
        }
    }

    if (ts_min > 0 || ts_max > 0) {
        std::vector<const TraceIndex::FileInfo*> filtered;
        filtered.reserve(files.size());
        for (auto* fi : files) {
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
    std::optional<Query> query_opt, double ts_min, double ts_max,
    BloomFilterCache* bloom_cache, int limit) {
    int emitted = 0;
    const Query* query_ptr = query_opt ? &*query_opt : nullptr;

    for (auto* file_info : files) {
        if (limit > 0 && emitted >= limit) break;

        if (file_info->is_small) {
            std::vector<std::string> events;
            std::uint64_t scanned = 0;
            std::uint64_t matched = 0;
            co_await direct_scan_events(
                file_info, query_ptr, ev_view.include_metadata, &events,
                &scanned, &matched, limit > 0 ? limit - emitted : 0);
            std::vector<std::string_view> views;
            for (const auto& event : events) {
                if (limit > 0 && emitted >= limit) break;
                views.push_back(event);
                emitted++;
            }
            if (!views.empty()) {
                co_yield StreamChunk{views};
            }
            continue;
        }

        if (file_info->uncompressed_size == 0 &&
            file_info->num_checkpoints == 0)
            continue;

        ViewBuilderInput builder_input;
        builder_input.with_view(ev_view)
            .with_file_path(file_info->path)
            .with_idx_path(file_info->has_bloom_data ? file_info->idx_path : "")
            .with_uncompressed_size(file_info->uncompressed_size)
            .with_num_checkpoints(file_info->num_checkpoints)
            .with_bloom_cache(bloom_cache)
            .with_time_range(ts_min, ts_max);

        ViewBuilderUtility builder;
        auto build_output = co_await builder.process(builder_input);
        if (!build_output.success || !build_output.file_may_match) continue;

        for (const auto& candidate : build_output.candidates) {
            if (limit > 0 && emitted >= limit) break;

            ViewReaderInput reader_input;
            reader_input.with_file_path(file_info->path)
                .with_idx_path(file_info->idx_path)
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
        const auto* index_dir_ptr = &index_dir;

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
                         index_dir_ptr](CoroScope&) -> coro::CoroTask<void> {
                while (auto fi_opt = co_await file_chan->receive()) {
                    auto* file_info = (*stat_files_ptr)[*fi_opt];

                    StatisticsAggregatorInput agg_input;
                    agg_input.file_path = file_info->path;
                    agg_input.idx_path = file_info->idx_path;
                    agg_input.index_dir = *index_dir_ptr;

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
