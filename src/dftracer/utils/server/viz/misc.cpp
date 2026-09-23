#include <dftracer/utils/core/common/field_ref.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/json/json_doc_guard.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/json_builder.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/signal_handler.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz/calltree.h>
#include <dftracer/utils/server/viz/density.h>
#include <dftracer/utils/server/viz/handlers.h>
#include <dftracer/utils/server/viz/internal.h>
#include <dftracer/utils/server/viz/scan.h>
#include <dftracer/utils/server/viz/summary_build.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_planner_utility.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <simdjson.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::server {

using namespace dftracer::utils::trace;
using namespace dftracer::utils::trace::views;
using dftracer::utils::json::json_number;

// GET /api/viz/layers: whole-trace reference data - the operation-name ->
// category map (a property of the name, not the view, so fetched once) plus the
// FH file counts: total declared vs. those an I/O event actually touched.
coro::CoroTask<HttpResponse> handle_viz_layers(const HttpRequest& /*req*/,
                                               const QueryParams& /*p*/,
                                               TraceIndex& index) {
    const VizSummary* s = co_await ensure_viz_summary(index);
    auto& b = scratch_json_builder();
    b.start_object();
    b.escape_and_append_with_quotes("layers");
    b.append_colon();
    b.start_object();
    if (s) {
        bool first = true;
        for (const auto& kv : s->name_cats) {
            if (!first) b.append_comma();
            first = false;
            b.escape_and_append_with_quotes(kv.first);
            b.append_colon();
            b.escape_and_append_with_quotes(kv.second);
        }
    }
    b.end_object();
    b.append_comma();
    b.append_key_value("total_files",
                       static_cast<std::int64_t>(s ? s->total_files : 0));
    b.append_comma();
    b.append_key_value("io_files",
                       static_cast<std::int64_t>(s ? s->io_files : 0));
    b.end_object();
    co_return HttpResponse::ok(std::string(b));
}

// GET /api/viz/columns: the complete set of groupable columns in the trace
// (top-level scalar fields + args keys), harvested during the summary scan.
coro::CoroTask<HttpResponse> handle_viz_columns(const HttpRequest& /*req*/,
                                                const QueryParams& /*params*/,
                                                TraceIndex& index) {
    // Schemaless discovery from the index (no scan): base axis fields, every
    // harvested scalar leaf (nested args as dotted paths), and resolved.*
    // aliases, each with its type. Shares View::schema() with the C++/Python
    // API instead of re-implementing a column union here.
    std::vector<const TraceIndex::FileInfo*> all_files;
    all_files.reserve(index.files().size());
    for (const auto& f : index.files()) all_files.push_back(&f);
    views::View v =
        views::View::from_files(to_view_files(all_files), &index.bloom_cache());
    // This endpoint offers groupable columns; the pid/tid/ts/dur axis fields
    // are timeline lanes, not group options, so drop them from View::schema().
    std::vector<views::View::ColumnInfo> schema;
    for (auto& c : v.schema()) {
        if (c.name == "pid" || c.name == "tid" || c.name == "ts" ||
            c.name == "dur")
            continue;
        schema.push_back(std::move(c));
    }

    auto& b = scratch_json_builder();
    b.start_object();
    b.escape_and_append_with_quotes("columns");
    b.append_colon();
    b.start_array();
    bool first = true;
    for (const auto& c : schema) {
        if (!first) b.append_comma();
        first = false;
        b.escape_and_append_with_quotes(c.name);
    }
    b.end_array();
    // types: {column -> "int64"/"float64"/"string"}, additive to columns.
    b.append_comma();
    b.escape_and_append_with_quotes("types");
    b.append_colon();
    b.start_object();
    first = true;
    for (const auto& c : schema) {
        if (!first) b.append_comma();
        first = false;
        b.escape_and_append_with_quotes(c.name);
        b.append_colon();
        b.escape_and_append_with_quotes(c.type);
    }
    b.end_object();
    b.append_comma();
    b.append_key_value("ready", true);
    b.end_object();
    co_return HttpResponse::ok(std::string(b));
}

coro::CoroTask<HttpResponse> handle_viz_breaks(const HttpRequest& /*req*/,
                                               const QueryParams& params,
                                               TraceIndex& index) {
    auto ts_norm_param = params.get("ts_normalize");
    bool normalize = ts_norm_param.empty() || ts_norm_param != "0";
    std::uint64_t global_min = 0;
    if (normalize) {
        global_min = index.global_min_timestamp_us();
        if (global_min == std::numeric_limits<std::uint64_t>::max())
            global_min = 0;
    }
    const VizSummary* s = co_await ensure_viz_summary(index);
    auto& b = scratch_json_builder();
    b.start_object();
    b.escape_and_append_with_quotes("gaps");
    b.append_colon();
    b.start_array();
    if (s) {
        bool first = true;
        for (const auto& g : s->idle_gaps) {
            if (!first) b.append_comma();
            first = false;
            b.start_object();
            b.append_key_value("begin",
                               index.native_to_us(g.first - global_min));
            b.append_comma();
            b.append_key_value("end",
                               index.native_to_us(g.second - global_min));
            b.end_object();
        }
    }
    b.end_array();
    b.append_comma();
    b.append_key_value("multi_run", s && !s->idle_gaps.empty());
    b.end_object();
    co_return HttpResponse::ok(std::string(b));
}

}  // namespace dftracer::utils::server
