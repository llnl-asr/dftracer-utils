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

namespace {

enum class GroupBy { Name, Cat, Pid, Fhash };

static GroupBy parse_group_by(std::string_view g) {
    if (g == "cat") return GroupBy::Cat;
    if (g == "pid") return GroupBy::Pid;
    if (g == "fhash" || g == "file") return GroupBy::Fhash;
    return GroupBy::Name;
}

}  // namespace

// One aggregate row, uniform over the live-scan and summary paths.
struct StatRow {
    const std::string* key;
    std::uint64_t count;
    double total;
    double min;
    double max;
    double coverage;  // wall time the group was active (union of intervals)
};

// Coverage resolution: intervals are bucketed at (window / this) so concurrent
// events collapse to their union instead of summing. Sparse, so memory tracks
// distinct occupied buckets, not this count.
static constexpr double COVERAGE_BUCKETS = 8192.0;

// Per-group accumulator that merges real events (from the View) with the
// prorated contribution of ph=3 aggregates overlapping the window.
struct StatAgg {
    double count =
        0;  // fractional: an aggregate contributes dftu_cnt * overlap
    double total = 0;
    double min = std::numeric_limits<double>::infinity();
    double max = 0;
    // Union of active intervals as a difference map: bucket -> net (starts -
    // ends). Coverage is the length of buckets whose running sum stays > 0.
    ankerl::unordered_dense::map<std::int32_t, std::int32_t> cov;
    void add(double cnt, double sum, double dmin, double dmax) {
        count += cnt;
        total += sum;
        if (dmin < min) min = dmin;
        if (dmax > max) max = dmax;
    }
    void mark(double lo, double hi, double begin, double bw) {
        if (bw <= 0) return;
        auto blo = static_cast<std::int32_t>((lo - begin) / bw);
        auto bhi = static_cast<std::int32_t>((hi - begin) / bw);
        if (bhi <= blo) bhi = blo + 1;  // an event occupies at least its bucket
        cov[blo] += 1;
        cov[bhi] -= 1;
    }
    double coverage(double bw) const {
        if (cov.empty()) return 0;
        std::vector<std::pair<std::int32_t, std::int32_t>> pts(cov.begin(),
                                                               cov.end());
        std::sort(pts.begin(), pts.end());
        double buckets = 0;
        int running = 0;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            running += pts[i].second;
            if (running > 0) buckets += pts[i + 1].first - pts[i].first;
        }
        return buckets * bw;
    }
};

template <typename builder_type>
void tag_invoke(simdjson::serialize_tag, builder_type& b, const StatRow& r) {
    b.start_object();
    b.append_key_value("name", *r.key);
    b.append_comma();
    b.append_key_value("count", r.count);
    b.append_comma();
    b.append_key_value("total", r.total);
    b.append_comma();
    b.append_key_value("avg",
                       r.count ? r.total / static_cast<double>(r.count) : 0.0);
    b.append_comma();
    b.append_key_value("min", r.min);
    b.append_comma();
    b.append_key_value("max", r.max);
    b.append_comma();
    b.append_key_value("coverage", r.coverage);
    b.end_object();
}

static std::string serialize_stats_body(std::uint64_t total_count,
                                        double total_dur, double wall,
                                        bool truncated,
                                        const std::vector<StatRow>& rows) {
    auto& b = scratch_json_builder();
    b.start_object();
    b.append_key_value("count", total_count);
    b.append_comma();
    b.append_key_value("total_dur", total_dur);
    b.append_comma();
    b.append_key_value("wall", wall);
    b.append_comma();
    b.append_key_value("truncated", truncated);
    b.append_comma();
    b.append_key_value("names", rows);
    b.end_object();
    return std::string(b);
}

// Scale duration fields (native trace unit -> us) before serialization. The
// `wall` value is derived from client-us begin/end and is already in us.
static void scale_stat_durations(std::vector<StatRow>& rows, double& total_dur,
                                 TraceIndex::TimeMetric metric) {
    const double us = dftracer::utils::trace::time_metric_us_scale(metric);
    if (us == 1.0) return;
    for (auto& r : rows) {
        r.total *= us;
        r.min *= us;
        r.max *= us;
        r.coverage *= us;
    }
    total_dur *= us;
}

static const std::vector<VizSummary::GroupRow>& summary_group_rows(
    const VizSummary& s, GroupBy g) {
    switch (g) {
        case GroupBy::Cat:
            return s.by_cat;
        case GroupBy::Pid:
            return s.by_pid;
        case GroupBy::Fhash:
            return s.by_fhash;
        case GroupBy::Name:
        default:
            return s.by_name;
    }
}

// Whole-trace, unfiltered Analyze answers come from the prebuilt summary, so no
// live scan is needed. Any predicate or sub-range forces the live path.
static bool viz_stats_summary_eligible(const QueryParams& p, double begin_abs,
                                       double end_abs, TraceIndex& index) {
    if (!p.get("query").empty() || !p.get("cat").empty() ||
        !p.get("lanes").empty() || !p.get("filters").empty() ||
        !p.get("file").empty() || !p.get("pid").empty() ||
        !p.get("tid").empty())
        return false;
    std::uint64_t gmin = index.global_min_timestamp_us();
    std::uint64_t gmax = index.global_max_timestamp_us();
    if (gmin == std::numeric_limits<std::uint64_t>::max() || gmax <= gmin)
        return false;
    return begin_abs <= static_cast<double>(gmin) + 1.0 &&
           end_abs >= static_cast<double>(gmax) - 1.0;
}

// GET /api/viz/stats: server-side per-name aggregation over a time range.
// Scans in parallel worker coroutines, each folding into its own map, then
// merges single-threaded (no lock). Returns only the small aggregate table.
coro::CoroTask<HttpResponse> handle_viz_stats(const HttpRequest& req,
                                              const QueryParams& params,
                                              TraceIndex& index) {
    if (!params.has("begin") || !params.has("end")) {
        co_return HttpResponse::bad_request(
            "Missing required parameters: begin, end");
    }

    auto win = parse_viz_window(params, index);
    if (!win) co_return std::move(win.error());
    double begin = win->begin;
    double end = win->end;
    double original_begin = win->original_begin;
    double original_end = win->original_end;

    GroupBy group = parse_group_by(params.get("group"));

    const std::string cache_key =
        std::string(req.path) + "?" + params.canonical_key();
    if (auto hit = index.viz_cache().get(cache_key))
        co_return HttpResponse::ok(std::move(*hit));

    // Whole-trace, unfiltered Analyze: answer from the prebuilt summary (built
    // lazily here). Concurrent builds fall through to the live scan below.
    // Aggregated traces skip it so the live fold below prorates their ph=3
    // records (the summary rows omit them), keeping counts consistent with any
    // sub-window selection.
    if (viz_stats_summary_eligible(params, begin, end, index)) {
        const VizSummary* s = co_await ensure_viz_summary(index);
        if (s && !s->has_aggregated) {
            const auto& gr = summary_group_rows(*s, group);
            std::vector<StatRow> rows;
            rows.reserve(gr.size());
            std::uint64_t total_count = 0;
            double total_dur = 0;
            for (const auto& r : gr) {
                // Summary rows are pre-aggregated scalars with no intervals, so
                // coverage is unavailable (0); the live-scan path fills it in.
                rows.push_back({&r.key, r.count, r.total, r.min, r.max, 0});
                total_count += r.count;
                total_dur += r.total;
            }
            scale_stat_durations(rows, total_dur, index.time_metric());
            co_return HttpResponse::ok(serialize_stats_body(
                total_count, total_dur, original_end - original_begin, false,
                rows));
        }
    }

    // "What was active in this window", so include every event that OVERLAPS
    // [begin, end] with its duration clamped to the window (enclosing events
    // that started earlier still count), and prorate ph=3 aggregates by their
    // overlap. A plain ts-in-window sum leaves a small selection empty because
    // nothing starts inside it.
    //
    // The ts window is applied via time_range/clamping, not the query, so the
    // query carries only the user's field filters (a wide-open ts range keeps
    // build_viz_view's own ts clause a no-op) - otherwise it would reject the
    // enclosers, whose ts precedes `begin`.
    ViewDefinition view = build_viz_view(params, 0.0, 4e18, 0);
    std::vector<const TraceIndex::FileInfo*> target_files =
        select_viz_target_files(index, params, begin, end);

    // Whole-run enclosers (started long before the window) are taken from the
    // summary long-event list; shorter ones from a scan back bounded by the
    // same threshold. ph=3 aggregates prorate over the trace's aggregation
    // window.
    const VizSummary* s = co_await ensure_viz_summary(index);
    auto to_native = [&](double us) {
        return index.time_metric() == TraceIndex::TimeMetric::US
                   ? us
                   : static_cast<double>(
                         index.us_to_native(static_cast<std::uint64_t>(us)));
    };
    double enc_threshold_native =
        s && s->long_threshold_us > 0 ? to_native(s->long_threshold_us) : 0;
    double interval_native =
        s && s->agg_interval_us > 0 ? to_native(s->agg_interval_us) : 0;
    double scan_begin = begin - enc_threshold_native;
    if (scan_begin < 0) scan_begin = 0;

    const char* gcol = group == GroupBy::Cat     ? "cat"
                       : group == GroupBy::Pid   ? "pid"
                       : group == GroupBy::Fhash ? "fhash"
                                                 : "name";

    using StatMap = ankerl::unordered_dense::map<std::string, StatAgg>;
    auto merge_stats = [](StatMap&& a, StatMap&& b) {
        for (auto& [k, v] : b) {
            auto& x = a[k];
            x.count += v.count;
            x.total += v.total;
            if (v.min < x.min) x.min = v.min;
            if (v.max > x.max) x.max = v.max;
            for (auto& [bk, d] : v.cov) x.cov[bk] += d;
        }
        return std::move(a);
    };

    const double cov_bw = end > begin ? (end - begin) / COVERAGE_BUCKETS : 0;
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    auto sv = views::View::from_files(to_view_files(target_files),
                                      &index.bloom_cache())
                  .phase(views::Phase::Any)
                  .metadata(false)
                  .time_range(scan_begin, end);
    if (view.query) sv = sv.filter(*view.query);

    auto scan = co_await sv.map_batches<StatMap>(
        [begin, end, scan_begin, interval_native, gcol, cov_bw](
            StatMap& acc, const std::vector<std::string_view>& events) {
            thread_local simdjson::dom::parser parser;
            thread_local std::string buf;
            for (auto ev : events) {
                // simdjson's SIMD stages read up to SIMDJSON_PADDING bytes past
                // the JSON; a bare std::string leaves those uninitialised.
                // Zero- pad the reused buffer and parse only the event's
                // length.
                const std::size_t len = ev.size();
                buf.assign(ev);
                buf.resize(len + simdjson::SIMDJSON_PADDING);
                auto res = parser.parse(buf.data(), len,
                                        /*realloc_if_needed=*/false);
                if (res.error()) continue;
                auto root = res.value_unsafe();
                if (!root.is_object()) continue;
                auto tr = root["ts"];
                if (tr.error()) continue;
                double ts = json_number(tr.value_unsafe());
                // Events opening before the scan-back window are the whole-run
                // enclosers served from long_events below; skip here to not
                // double-count (chunk pruning still reads their chunk).
                if (ts < scan_begin) continue;
                bool is_agg = false;
                auto phr = root["ph"];
                if (!phr.error()) {
                    if (phr.is_int64())
                        is_agg = phr.get_int64().value_unsafe() == 3;
                    else if (phr.is_uint64())
                        is_agg = phr.get_uint64().value_unsafe() == 3;
                    else if (phr.is_string())
                        is_agg = phr.get_string().value_unsafe() == "A";
                }
                std::string key = extract_group_value(root, gcol);
                if (is_agg) {
                    if (interval_native <= 0) continue;
                    double lo = std::max(begin, ts);
                    double hi = std::min(end, ts + interval_native);
                    if (hi <= lo) continue;
                    double f = (hi - lo) / interval_native;
                    double cnt = 1, sum = 0, dmin = 0, dmax = 0;
                    auto args = root["args"];
                    if (!args.error() && args.is_object()) {
                        auto c = args["dftu_cnt"];
                        if (!c.error()) cnt = json_number(c.value_unsafe());
                        auto sm = args["dur_sum"];
                        if (!sm.error())
                            sum = json_number(sm.value_unsafe());
                        else {
                            auto d = args["dur"];
                            if (!d.error()) sum = json_number(d.value_unsafe());
                        }
                        auto mn = args["dur_min"];
                        if (!mn.error()) dmin = json_number(mn.value_unsafe());
                        auto mx = args["dur_max"];
                        if (!mx.error()) dmax = json_number(mx.value_unsafe());
                    }
                    auto& a = acc[key];
                    a.add(cnt * f, sum * f, dmin, dmax);
                    a.mark(lo, hi, begin, cov_bw);
                } else {
                    auto dr = root["dur"];
                    if (dr.error()) continue;
                    double dur = json_number(dr.value_unsafe());
                    double lo = std::max(begin, ts);
                    double hi = std::min(end, ts + dur);
                    if (hi <= lo) continue;
                    auto& a = acc[key];
                    a.add(1.0, hi - lo, dur, dur);
                    a.mark(lo, hi, begin, cov_bw);
                }
            }
        },
        merge_stats, slots, 0);
    StatMap byKey = std::move(scan.value);

    // Whole-run enclosers that opened before the scan-back window. The scan's
    // query filter did not see these, so re-apply it per event.
    if (s) {
        simdjson::dom::parser lp;
        for (const auto& sp : s->long_events) {
            double sb = static_cast<double>(sp.begin);
            if (sb >= scan_begin) continue;  // already covered by the scan
            double lo = std::max(begin, sb);
            double hi = std::min(end, static_cast<double>(sp.end));
            if (hi <= lo) continue;
            auto pr = lp.parse(simdjson::padded_string(sp.json));
            if (pr.error() || !pr.value_unsafe().is_object()) continue;
            auto root = pr.value_unsafe();
            if (view.query && !view.query->evaluate(json::JsonValue(root)))
                continue;
            double dur = static_cast<double>(sp.end - sp.begin);
            auto& a = byKey[extract_group_value(root, gcol)];
            a.add(1.0, hi - lo, dur, dur);
            a.mark(lo, hi, begin, cov_bw);
        }
    }

    std::vector<StatRow> rows;
    rows.reserve(byKey.size());
    std::uint64_t total_count = 0;
    double total_dur = 0;
    for (auto& kv : byKey) {
        auto cnt = static_cast<std::uint64_t>(kv.second.count + 0.5);
        double mn = kv.second.min == std::numeric_limits<double>::infinity()
                        ? 0
                        : kv.second.min;
        rows.push_back({&kv.first, cnt, kv.second.total, mn, kv.second.max,
                        kv.second.coverage(cov_bw)});
        total_count += cnt;
        total_dur += kv.second.total;
    }
    std::sort(rows.begin(), rows.end(), [](const StatRow& a, const StatRow& b) {
        return a.total > b.total;
    });

    scale_stat_durations(rows, total_dur, index.time_metric());
    std::string body = serialize_stats_body(
        total_count, total_dur, original_end - original_begin, false, rows);
    if (!req.cancel_token.cancelled()) index.viz_cache().put(cache_key, body);
    co_return HttpResponse::ok(std::move(body));
}

}  // namespace dftracer::utils::server
