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

// Parse ?group=<field(,field)*> into the flamegraph root-group key.
static std::vector<std::string> parse_group_param(const QueryParams& params) {
    std::vector<std::string> group;
    std::string_view g = params.get("group");
    std::size_t pos = 0;
    while (pos < g.size()) {
        std::size_t comma = g.find(',', pos);
        if (comma == std::string_view::npos) comma = g.size();
        std::string_view f = g.substr(pos, comma - pos);
        if (!f.empty()) group.emplace_back(f);
        pos = comma + 1;
    }
    return group;
}

// Turn a serialized flamegraph arena (from View::flamegraph_partial) into the
// calltree JSON: roll up the "all" root (group nodes are already rolled up by
// the fold), scale native durations to us, and serialize the tree. A plain
// function, so the arena stays out of the handler coroutine's frame.
static std::string calltree_json_from_arena(std::string blob, double dur_us,
                                            int limit, std::int64_t cap) {
    std::vector<FlameNode> arena = dataframe::deserialize_flame_arena(
        reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size());
    if (arena.empty()) {
        arena.emplace_back();
        arena[0].name = "all";
    }
    double root_total = 0;
    std::uint64_t root_count = 0;
    for (std::uint32_t c : arena[0].children) {
        root_total += arena[c].total;
        root_count += arena[c].count;
    }
    arena[0].total = root_total;
    arena[0].count = root_count;
    arena[0].self = 0;
    const bool truncated =
        limit > 0 && static_cast<std::int64_t>(root_count) >= cap;
    if (dur_us != 1.0) {
        for (auto& n : arena) {
            n.total *= dur_us;
            if (n.self > 0) n.self *= dur_us;
        }
    }
    auto& b = scratch_json_builder();
    b.start_object();
    b.append_key_value("truncated", truncated);
    b.append_comma();
    b.escape_and_append_with_quotes("tree");
    b.append_colon();
    serialize_flame_node(b, arena, 0);
    b.end_object();
    return std::string(b);
}

// GET /api/viz/calltree: merge events into a flamegraph tree. The hierarchy
// per pid/tid lane comes from ts/dur containment (same nesting the timeline
// draws); identical name-paths fold together across the whole trace.
coro::CoroTask<HttpResponse> handle_viz_calltree(const HttpRequest& req,
                                                 const QueryParams& params,
                                                 TraceIndex& index) {
    if (!params.has("begin") || !params.has("end"))
        co_return HttpResponse::bad_request(
            "Missing required parameters: begin, end");

    auto win = parse_viz_window(params, index);
    if (!win) co_return std::move(win.error());
    double begin = win->begin;
    double end = win->end;

    ViewDefinition view = build_viz_view(params, begin, end, 0);
    // The flame tree keys on ts/dur containment and ignores ph=M metadata, so
    // drop it at the reader to engage the no-metadata fast path.
    view.with_include_metadata(false);
    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;

    std::vector<const TraceIndex::FileInfo*> target_files =
        select_viz_target_files(index, params, begin, end);

    bool single_file = !params.get("file").empty();
    // Root the flame tree by an arbitrary group key over the raw events:
    // ?group=pid, ?group=cat, ?group=host,pid, ... (empty folds every lane
    // together). Any field works, not just pid.
    std::vector<std::string> group = parse_group_param(params);
    const std::int64_t cap =
        limit > 0 ? limit : std::numeric_limits<std::int64_t>::max();

    const std::string cache_key =
        std::string(req.path) + "?" + params.canonical_key();
    if (auto hit = index.viz_cache().get(cache_key))
        co_return HttpResponse::ok(std::move(*hit));

    static constexpr const char* CANCELLED_TREE =
        R"({"truncated":true,"tree":{"name":"all","total":0,"self":0,"count":0,"children":[]}})";

    // Fold the flamegraph over one shared View scan: the engine does the
    // index-pruned parallel scan, the containment_walk name-path arena, the
    // per-group rooting, and cancellation - no hand-rolled worker here.
    CancelToken cancel = req.cancel_token;
    views::View v =
        views::View::from_files(to_view_files(target_files),
                                &index.bloom_cache())
            .phase(views::Phase::Events)
            .metadata(false)
            .cancel_when([&req]() { return req.cancel_token.cancelled(); });
    if (view.query) v = v.filter(*view.query);
    if (!single_file) v = v.time_range(begin, end);
    if (limit > 0) v = v.limit(static_cast<std::uint64_t>(cap));

    // Hoist the fold arguments to named locals: as co_await full-expression
    // temporaries these vectors/strings would be lifetime-extended into the
    // coroutine frame, which the compiler mishandles.
    std::vector<std::string> partition{"pid", "tid"};
    std::string ts_field{"ts"}, dur_field{"dur"}, name_field{"name"};
    std::string blob = co_await v.flamegraph_partial(
        partition, ts_field, dur_field, name_field, group);
    if (cancel.cancelled()) co_return HttpResponse::ok(CANCELLED_TREE);

    const double dur_us =
        dftracer::utils::trace::time_metric_us_scale(index.time_metric());
    std::string body =
        calltree_json_from_arena(std::move(blob), dur_us, limit, cap);
    index.viz_cache().put(cache_key, body);
    co_return HttpResponse::ok(std::move(body));
}

}  // namespace dftracer::utils::server
