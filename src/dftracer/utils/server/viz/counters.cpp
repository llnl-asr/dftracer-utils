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

static std::string serialize_counters_body(const std::vector<double>& read,
                                           const std::vector<double>& write,
                                           const std::vector<double>& ops,
                                           double original_begin,
                                           double original_end, int buckets,
                                           double bucket_us, bool truncated) {
    auto& b = scratch_json_builder();
    b.start_object();
    b.append_key_value("begin", original_begin);
    b.append_comma();
    b.append_key_value("end", original_end);
    b.append_comma();
    b.append_key_value("buckets", static_cast<std::int64_t>(buckets));
    b.append_comma();
    b.append_key_value("bucket_us", bucket_us);
    b.append_comma();
    b.append_key_value("truncated", truncated);
    b.append_comma();
    b.append_key_value("read_bytes", read);
    b.append_comma();
    b.append_key_value("write_bytes", write);
    b.append_comma();
    b.append_key_value("ops", ops);
    b.end_object();
    return std::string(b);
}

// Re-aggregate the summary's finest counter buckets into `buckets` output
// buckets over [begin_abs, end_abs] (absolute us).
static std::string serve_counters_from_summary(
    const VizSummary& s, const VizSummary::Level& level, double begin_abs,
    double end_abs, double original_begin, double original_end, int buckets,
    double bucket_us_out, TraceIndex::TimeMetric metric) {
    std::vector<double> read(buckets, 0.0), write(buckets, 0.0),
        ops(buckets, 0.0);
    std::int64_t fb0 = s.bucket_of(begin_abs, level.bucket_us, level.nbuckets);
    std::int64_t fb1 = s.bucket_of(end_abs, level.bucket_us, level.nbuckets);
    if (fb0 < 0) fb0 = 0;
    if (fb1 < 0) fb1 = static_cast<std::int64_t>(level.nbuckets) - 1;
    for (std::int64_t fb = fb0; fb <= fb1; ++fb) {
        double center = static_cast<double>(s.t_begin) +
                        (static_cast<double>(fb) + 0.5) * level.bucket_us;
        long oi = static_cast<long>((center - begin_abs) / bucket_us_out);
        if (oi < 0 || oi >= buckets) continue;
        auto f = static_cast<std::size_t>(fb);
        read[oi] += level.read_bytes[f];
        write[oi] += level.write_bytes[f];
        ops[oi] += level.ops[f];
    }
    return serialize_counters_body(
        read, write, ops, original_begin, original_end, buckets,
        bucket_us_out * dftracer::utils::trace::time_metric_us_scale(metric),
        false);
}

// GET /api/viz/counters: per-bucket read/write bytes and I/O op counts over
// a time range, for bandwidth/IOPS counter tracks. Aggregated server-side in
// parallel (per-worker arrays merged after join).
coro::CoroTask<HttpResponse> handle_viz_counters(const HttpRequest& req,
                                                 const QueryParams& params,
                                                 TraceIndex& index) {
    if (!params.has("begin") || !params.has("end")) {
        co_return HttpResponse::bad_request(
            "Missing required parameters: begin, end");
    }

    int buckets = params.get_int("buckets", 800);
    if (buckets < 16) buckets = 16;
    if (buckets > 4000) buckets = 4000;

    auto win = parse_viz_window(params, index);
    if (!win) co_return std::move(win.error());
    double begin = win->begin;
    double end = win->end;
    double original_begin = win->original_begin;
    double original_end = win->original_end;

    double bucket_us = (end - begin) / static_cast<double>(buckets);
    if (bucket_us <= 0) bucket_us = 1;

    const std::string cache_key =
        std::string(req.path) + "?" + params.canonical_key();
    if (auto hit = index.viz_cache().get(cache_key))
        co_return HttpResponse::ok(std::move(*hit));

    // Zoomed-out, unfiltered counter tracks come from the activity summary.
    if (viz_summary_eligible(params)) {
        const VizSummary* s = co_await ensure_viz_summary(index);
        if (s != nullptr && s->t_end > s->t_begin) {
            if (const VizSummary::Level* level = s->level_for(bucket_us)) {
                co_return HttpResponse::ok(serve_counters_from_summary(
                    *s, *level, begin, end, original_begin, original_end,
                    buckets, bucket_us, index.time_metric()));
            }
        }
    }

    ViewDefinition view = build_viz_view(params, begin, end, 0);
    // No cap: only zoomed-in or filtered counter queries reach the live path.
    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;

    const std::size_t nb = static_cast<std::size_t>(buckets);
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    bool single_file = !params.get("file").empty();

    // Fold per-bucket read/write bytes and op counts over View's scan. Partials
    // init lazily (an unused slot stays empty, the identity for merge below).
    views::View v =
        views::View::from_files(
            to_view_files(select_viz_target_files(index, params, begin, end)),
            &index.bloom_cache())
            .phase(views::Phase::Events)
            .metadata(false)
            .cancel_when([&req]() { return req.cancel_token.cancelled(); });
    if (view.query) v = v.filter(*view.query);
    if (!single_file) v = v.time_range(begin, end);

    auto r = co_await v.map_batches<CounterAcc>(
        [begin, bucket_us, nb](CounterAcc& acc,
                               const std::vector<std::string_view>& events) {
            if (acc.ops.empty()) acc.init(nb);
            for (auto ev : events) fold_counter(ev, begin, bucket_us, nb, acc);
        },
        [](CounterAcc&& x, CounterAcc&& y) {
            if (y.ops.empty()) return std::move(x);
            if (x.ops.empty()) return std::move(y);
            x.merge_from(y);
            return std::move(x);
        },
        slots, limit > 0 ? static_cast<std::uint64_t>(limit) : 0);

    bool truncated = r.stats.truncated;
    CounterAcc total = std::move(r.value);
    if (total.ops.empty()) total.init(nb);

    std::string body = serialize_counters_body(
        total.read_bytes, total.write_bytes, total.ops, original_begin,
        original_end, buckets,
        bucket_us *
            dftracer::utils::trace::time_metric_us_scale(index.time_metric()),
        truncated);
    if (!req.cancel_token.cancelled()) index.viz_cache().put(cache_key, body);
    co_return HttpResponse::ok(std::move(body));
}

}  // namespace dftracer::utils::server
