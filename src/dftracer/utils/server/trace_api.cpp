#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/json/json_doc_guard.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/server/cursor.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/json_builder.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_api.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/statistics/statistics_query_utility.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_planner_utility.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <simdjson.h>

#include <cstddef>
#include <limits>
#include <string>
#include <unordered_set>

namespace dftracer::utils::server {

using namespace dftracer::utils::trace;

using namespace dftracer::utils::trace::statistics;
using namespace dftracer::utils::trace::views;

// Hash metadata types that need smart filtering (FH, HH, SH).
static const std::unordered_set<std::string> HASH_METADATA_NAMES = {"FH", "HH",
                                                                    "SH"};

// --- GET /api/files ---
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

// --- GET /api/files/info ---
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

// --- GET /api/info ---
static coro::CoroTask<HttpResponse> handle_info(const HttpRequest& /*req*/,
                                                const QueryParams& /*params*/,
                                                TraceIndex& index) {
    auto global_min_native = index.global_min_timestamp_us();
    auto global_max_native = index.global_max_timestamp_us();
    bool has_time_range =
        global_max_native > 0 &&
        global_min_native != std::numeric_limits<std::uint64_t>::max();
    // Client works in microseconds; scale native (index-unit) bounds to us.
    auto global_min = index.native_to_us(global_min_native);
    auto global_max = index.native_to_us(global_max_native);

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
                               static_cast<std::int64_t>(
                                   index.native_to_us(f.min_timestamp_us)));
            b.append_comma();
            b.append_key_value("max_timestamp_us",
                               static_cast<std::int64_t>(
                                   index.native_to_us(f.max_timestamp_us)));
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
        "/api/files",
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
        "/api/files/info",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_file_info(req, params, *index_ptr);
        },
        RouteDoc{"Metadata for one trace file.",
                 "Trace data",
                 {{"file", "Trace file path", true, ""}},
                 R"({"path":"trace-0.pfw.gz","has_bloom_data":true})"});

    router.get(
        "/api/info",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_info(req, params, *index_ptr);
        },
        RouteDoc{"Global summary: file count and time bounds.",
                 "Trace data",
                 {},
                 R"({"file_count":2,"global_min_timestamp_us":1000000,)"
                 R"("global_max_timestamp_us":6999732})"});

    router.get(
        "/api/resolve",
        [index_ptr](const HttpRequest& /*req*/,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            std::string hashes(params.get("hash"));
            if (hashes.empty())
                co_return HttpResponse::bad_request("Missing parameter: hash");
            std::string dict(params.get("type"));
            if (dict.empty()) dict = "file";
            const dftracer::utils::index::Dictionary* d = nullptr;
            for (const auto& c : index_ptr->record_schema().dictionaries)
                if (c.name == dict) d = &c;
            if (!d)
                co_return HttpResponse::bad_request("Invalid type: " + dict);
            std::string field(params.get("field"));
            if (field.empty()) field = d->fields.front().first;
            if (!d->has_field(field))
                co_return HttpResponse::bad_request("Invalid field: " + field);

            auto& b = scratch_json_builder();
            b.start_object();
            b.escape_and_append_with_quotes("names");
            b.append_colon();
            b.start_object();
            bool first = true;
            std::size_t start = 0;
            // Comma-separated so one click can resolve its file and host at
            // once; unknown hashes are simply absent from the reply.
            while (start <= hashes.size()) {
                auto end = hashes.find(',', start);
                if (end == std::string::npos) end = hashes.size();
                std::string one = hashes.substr(start, end - start);
                start = end + 1;
                if (one.empty()) continue;
                auto name = index_ptr->resolve(dict, field, one);
                if (name.empty()) continue;
                if (!first) b.append_comma();
                first = false;
                b.escape_and_append_with_quotes(one);
                b.append_colon();
                b.escape_and_append_with_quotes(name);
            }
            b.end_object();
            b.end_object();
            co_return HttpResponse::ok(std::string(b));
        },
        RouteDoc{
            "Resolve dictionary keys (file/host/string) to their values.",
            "Control",
            {{"hash", "Key, or several separated by commas", true, ""},
             {"type", "Dictionary: file (default), host or string", false,
              "file"},
             {"field",
              "Dictionary field: path, name or value (the dictionary's own "
              "by default)",
              false, ""}},
            R"({"names":{"314c1a1cdb22a136":"/data/train/img_0.npz"}})"});

    router.post(
        "/api/cancel",
        [](const HttpRequest& /*req*/,
           const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            std::string id(params.get("id"));
            bool found = CancelRegistry::instance().cancel(id);
            co_return HttpResponse::ok(std::string("{\"cancelled\":") +
                                       (found ? "true" : "false") + "}");
        },
        RouteDoc{"Cancel an in-flight request by its X-Request-Id.",
                 "Control",
                 {{"id", "Request id to cancel", true, ""}},
                 R"({"cancelled":true})"});
}

}  // namespace dftracer::utils::server
