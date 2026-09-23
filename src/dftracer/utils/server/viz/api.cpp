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

// Bind a viz handler to the shared TraceIndex, collapsing the per-route
// registration lambda that only forwards (req, params, index).
template <auto Handler>
static RouteHandler bind_index(TraceIndex& index) {
    return [&index](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
        co_return co_await Handler(req, params, index);
    };
}

void register_viz_api(Router& router, TraceIndex& index) {
    const RouteParam BEGIN{"begin", "Window start (us)", true, "0"};
    const RouteParam END{"end", "Window end (us)", true, "999999999"};
    const RouteParam SUMMARY{"summary", "LOD level (1=full detail)", true, "1"};

    router.get(
        "/api/viz/proctree", bind_index<handle_viz_proctree>(index),
        RouteDoc{
            "Inferred process/fork hierarchy with host, rank, and I/O.",
            "Visualization",
            {{"file", "Limit to one trace file", false, ""}},
            R"({"nodes":[{"pid":100,"parent":-1,"host":"node01","rank":"0",)"
            R"("bytes":16384,"io_ops":4,"io_busy":600.0}]})"});

    router.get(
        "/api/viz/counters", bind_index<handle_viz_counters>(index),
        RouteDoc{"Read/write bytes and I/O op counts per time bucket.",
                 "Visualization",
                 {BEGIN, END, SUMMARY},
                 R"({"buckets":[{"ts":0,"read_bytes":4096,"write_bytes":0,)"
                 R"("read_ops":1,"write_ops":0}]})"});

    router.get(
        "/api/viz/breaks", bind_index<handle_viz_breaks>(index),
        RouteDoc{
            "Globally-idle time gaps and multi-run detection.",
            "Visualization",
            {{"ts_normalize", "Normalize to global min (default on)", false,
              "1"}},
            R"({"gaps":[{"begin":50000,"end":900000}],"multi_run":true})"});

    router.get(
        "/api/viz/columns", bind_index<handle_viz_columns>(index),
        RouteDoc{"Groupable columns present in the trace.",
                 "Visualization",
                 {},
                 R"({"columns":["cat","name","mhost","fhash"],"ready":true})"});

    router.get(
        "/api/viz/events", bind_index<handle_viz_events>(index),
        RouteDoc{"Events for rendering: time-windowed, LOD-aggregated.",
                 "Visualization",
                 {BEGIN,
                  END,
                  SUMMARY,
                  {"pid", "Filter by process id", false, ""},
                  {"cat", "Filter by category", false, ""},
                  {"query", "DSL predicate, e.g. dur >= 1000", false, ""}},
                 R"({"events":[],"metadata":{"begin":0,"end":1000000,)"
                 R"("count":42,"truncated":false,"ts_normalized":true}})"});

    router.get(
        "/api/viz/density", bind_index<handle_viz_density>(index),
        RouteDoc{"Sub-pixel events bucketed into density blocks.",
                 "Visualization",
                 {BEGIN,
                  END,
                  {"summary", "LOD level", true, "2"},
                  {"width", "Canvas width in px (sets the 1px fold cutoff)",
                   false, "1920"}},
                 R"({"events":[],"density":[{"pid":100,"tid":100,"ts":0,)"
                 R"("dur":24457,"count":910,"total":22044,"depth":0}]})"});

    router.get(
        "/api/viz/stats", bind_index<handle_viz_stats>(index),
        RouteDoc{"Per-name aggregation over a time range (Analyze).",
                 "Visualization",
                 {BEGIN, END, SUMMARY},
                 R"({"count":100,"total_dur":5000,"names":[{"name":"read",)"
                 R"("count":50,"total":2500,"avg":50,"min":10,"max":90}]})"});

    router.get(
        "/api/viz/calltree", bind_index<handle_viz_calltree>(index),
        RouteDoc{
            "Merged flamegraph tree from ts/dur containment.",
            "Visualization",
            {BEGIN,
             END,
             SUMMARY,
             {"group", "Set to 'pid' to keep processes separate", false, ""}},
            R"({"name":"root","total":5000,"self":0,"count":0,)"
            R"("children":[{"name":"read","total":2500,"self":2500,)"
            R"("count":50}]})"});

    router.get(
        "/api/viz/histogram", bind_index<handle_viz_histogram>(index),
        RouteDoc{"Duration distribution: percentiles + log-spaced buckets.",
                 "Visualization",
                 {BEGIN,
                  END,
                  SUMMARY,
                  {"query", "DSL predicate to narrow to one op", false, ""}},
                 R"({"min":10,"max":900,"p50":150,"p99":880,"buckets":[]})"});

    router.get(
        "/api/viz/layers", bind_index<handle_viz_layers>(index),
        RouteDoc{"Operation-name to category map; declared vs I/O files.",
                 "Visualization",
                 {},
                 R"({"layers":{"read":"POSIX","write":"POSIX"},)"
                 R"("total_files":2,"io_files":2})"});
}

}  // namespace dftracer::utils::server
