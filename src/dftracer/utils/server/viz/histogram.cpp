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

// One log-spaced duration bucket of the histogram response.
struct HistBucket {
    double lo;
    double hi;
    std::uint64_t count;
};

template <typename builder_type>
void tag_invoke(simdjson::serialize_tag, builder_type& b, const HistBucket& h) {
    b.start_object();
    b.append_key_value("lo", h.lo);
    b.append_comma();
    b.append_key_value("hi", h.hi);
    b.append_comma();
    b.append_key_value("count", h.count);
    b.end_object();
}

// GET /api/viz/histogram: the distribution of event durations matching the
// query in [begin, end]. Collects each matching dur, then reports exact
// percentiles and a log-spaced histogram of the shape. The caller narrows to
// one operation by folding its predicate (name == "...") into the query.
coro::CoroTask<HttpResponse> handle_viz_histogram(const HttpRequest& req,
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
    view.with_include_metadata(false);  // aggregate only; skip ph=M records
    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;
    int nbuckets = params.get_int("buckets", 40);
    nbuckets = std::clamp(nbuckets, 4, 200);

    const std::string cache_key =
        std::string(req.path) + "?" + params.canonical_key();
    if (auto hit = index.viz_cache().get(cache_key))
        co_return HttpResponse::ok(std::move(*hit));

    std::vector<const TraceIndex::FileInfo*> target_files =
        select_viz_target_files(index, params, begin, end);
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    bool single_file = !params.get("file").empty();

    struct DurAcc {
        std::vector<double> durs;
    };
    views::View dv =
        views::View::from_files(to_view_files(target_files),
                                &index.bloom_cache())
            .phase(views::Phase::Events)
            .metadata(false)
            .cancel_when([&req]() { return req.cancel_token.cancelled(); });
    if (view.query) dv = dv.filter(*view.query);
    if (!single_file) dv = dv.time_range(begin, end);
    auto scan = co_await dv.map_batches<DurAcc>(
        [](DurAcc& a, const std::vector<std::string_view>& events) {
            for (auto e : events) {
                EventScalars s;
                if (parse_event_scalars(e, s) && s.has_dur)
                    a.durs.push_back(s.dur);
            }
        },
        [](DurAcc&& x, DurAcc&& y) {
            x.durs.reserve(x.durs.size() + y.durs.size());
            for (double d : y.durs) x.durs.push_back(d);
            return std::move(x);
        },
        slots, limit > 0 ? static_cast<std::uint64_t>(limit) : 0);

    bool truncated = scan.stats.truncated;
    std::vector<double> all = std::move(scan.value.durs);
    std::sort(all.begin(), all.end());

    // Durations are in the trace's native unit; scale to us for display.
    const double dur_us =
        dftracer::utils::trace::time_metric_us_scale(index.time_metric());
    if (dur_us != 1.0)
        for (double& d : all) d *= dur_us;

    auto& sb = scratch_json_builder();
    if (all.empty()) {
        sb.start_object();
        sb.append_key_value("count", 0);
        sb.append_comma();
        sb.escape_and_append_with_quotes("buckets");
        sb.append_colon();
        sb.start_array();
        sb.end_array();
        sb.append_comma();
        sb.append_key_value("truncated", truncated);
        sb.end_object();
        std::string body(sb);
        if (!req.cancel_token.cancelled())
            index.viz_cache().put(cache_key, body);
        co_return HttpResponse::ok(std::move(body));
    }

    std::size_t n = all.size();
    double vmin = all.front();
    double vmax = all.back();
    double sum = 0;
    for (double d : all) sum += d;
    double mean = sum / static_cast<double>(n);
    auto pct = [&all, n](double p) {
        std::size_t idx =
            static_cast<std::size_t>(p * static_cast<double>(n - 1));
        return all[idx];
    };

    // Log-spaced buckets over [max(vmin,1), vmax]; sub-microsecond durs land in
    // the first bucket.
    double lo = std::max(vmin, 1.0);
    double hi = std::max(vmax, lo * 1.0000001);
    double lr = std::log(hi / lo);
    std::vector<std::uint64_t> counts(static_cast<std::size_t>(nbuckets), 0);
    for (double d : all) {
        double v = d < lo ? lo : d;
        std::size_t bi =
            lr > 0 ? static_cast<std::size_t>(static_cast<double>(nbuckets) *
                                              std::log(v / lo) / lr)
                   : 0;
        if (bi >= static_cast<std::size_t>(nbuckets)) bi = nbuckets - 1;
        counts[bi]++;
    }

    sb.start_object();
    sb.append_key_value("count", n);
    sb.append_comma();
    sb.append_key_value("min", vmin);
    sb.append_comma();
    sb.append_key_value("max", vmax);
    sb.append_comma();
    sb.append_key_value("mean", mean);
    sb.append_comma();
    sb.append_key_value("p50", pct(0.50));
    sb.append_comma();
    sb.append_key_value("p90", pct(0.90));
    sb.append_comma();
    sb.append_key_value("p95", pct(0.95));
    sb.append_comma();
    sb.append_key_value("p99", pct(0.99));
    sb.append_comma();
    sb.append_key_value("truncated", truncated);
    sb.append_comma();
    std::vector<HistBucket> buckets;
    buckets.reserve(static_cast<std::size_t>(nbuckets));
    for (int i = 0; i < nbuckets; ++i) {
        buckets.push_back(
            {lo * std::exp(lr * static_cast<double>(i) / nbuckets),
             lo * std::exp(lr * static_cast<double>(i + 1) / nbuckets),
             counts[static_cast<std::size_t>(i)]});
    }
    sb.append_key_value("buckets", buckets);
    sb.end_object();
    std::string body(sb);
    if (!req.cancel_token.cancelled()) index.viz_cache().put(cache_key, body);
    co_return HttpResponse::ok(std::move(body));
}

}  // namespace dftracer::utils::server
