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

// A density block as it appears in the response: the map key/aggregate pair
// flattened with `ts` resolved from the column index. `name` borrows the
// aggregate's storage.
struct DensityBlock {
    std::string_view name;
    std::int64_t pid;
    std::int64_t tid;
    double ts;
    double dur;
    std::uint32_t count;
    double total;
    std::uint32_t depth;
    std::string_view group;  // group_by value; omitted from JSON when empty
    bool counter = false;    // ph="C" block: carries a mean counter value
    double value = 0;
};

template <typename builder_type>
void tag_invoke(simdjson::serialize_tag, builder_type& b,
                const DensityBlock& d) {
    b.start_object();
    if (!d.group.empty()) {
        b.append_key_value("group", d.group);
        b.append_comma();
    }
    b.append_key_value("name", d.name);
    b.append_comma();
    b.append_key_value("pid", d.pid);
    b.append_comma();
    b.append_key_value("tid", d.tid);
    b.append_comma();
    b.append_key_value("ts", d.ts);
    b.append_comma();
    b.append_key_value("dur", d.dur);
    b.append_comma();
    b.append_key_value("count", d.count);
    b.append_comma();
    b.append_key_value("total", d.total);
    b.append_comma();
    b.append_key_value("depth", d.depth);
    if (d.counter) {
        b.append_comma();
        b.append_key_value("counter", true);
        b.append_comma();
        b.append_key_value("value", d.value);
    }
    b.end_object();
}

// Serialize collected density blocks (+ optional individual events) into the
// /viz/density response body. Shared by the live-scan and summary paths.
static std::string serialize_density_body(
    const std::vector<std::string>& big, const DensityMap& dens,
    double original_begin, double original_end, double threshold, int limit,
    bool truncated, bool ts_normalized, std::uint64_t display_global_min,
    double max_dur, TraceIndex::TimeMetric metric,
    const std::vector<std::uint32_t>* big_depth = nullptr,
    const ankerl::unordered_dense::map<std::string, std::string>* group_names =
        nullptr) {
    // Blocks are positioned/sized in native threshold units; scale the visible
    // time fields (block ts/dur, duration sums, max_dur) to microseconds.
    const double us = dftracer::utils::trace::time_metric_us_scale(metric);
    const double threshold_us = threshold * us;
    max_dur *= us;
    auto& b = scratch_json_builder();
    b.start_object();
    b.escape_and_append_with_quotes("events");
    b.append_colon();
    b.start_array();
    for (std::size_t i = 0; i < big.size(); ++i) {
        if (i > 0) b.append_comma();
        // Inject the server-computed depth as a sibling field (events end in
        // }).
        if (big_depth && i < big_depth->size() && !big[i].empty() &&
            big[i].back() == '}') {
            b.append_raw(std::string_view(big[i]).substr(0, big[i].size() - 1));
            b.append_raw(",\"depth\":");
            b.append(static_cast<std::uint64_t>((*big_depth)[i]));
            b.append_raw("}");
        } else {
            b.append_raw(big[i]);
        }
    }
    b.end_array();
    b.append_comma();
    std::vector<DensityBlock> blocks;
    blocks.reserve(dens.size());
    for (const auto& [k, a] : dens) {
        DensityBlock blk{
            a.name, k.pid, k.tid,
            original_begin + static_cast<double>(k.col) * threshold_us,
            threshold_us, a.count,
            // Counter blocks are "busy" for the whole bucket (uniform width);
            // event blocks carry their summed duration.
            a.counter ? threshold_us : a.total * us, a.depth, k.group};
        if (a.counter) {
            blk.counter = true;
            blk.value =
                a.count ? a.value_sum / static_cast<double>(a.count) : 0;
        }
        blocks.push_back(blk);
    }
    b.append_key_value("density", blocks);
    b.append_comma();
    b.escape_and_append_with_quotes("metadata");
    b.append_colon();
    b.start_object();
    b.append_key_value("begin", original_begin);
    b.append_comma();
    b.append_key_value("end", original_end);
    b.append_comma();
    b.append_key_value("count", big.size());
    b.append_comma();
    b.append_key_value("limit", limit);
    b.append_comma();
    b.append_key_value("density_count", dens.size());
    b.append_comma();
    b.append_key_value("truncated", truncated);
    b.append_comma();
    b.append_key_value("ts_normalized", ts_normalized);
    b.append_comma();
    b.append_key_value("global_min_timestamp_us", display_global_min);
    b.append_comma();
    b.append_key_value("max_dur", max_dur);
    if (group_names && !group_names->empty()) {
        b.append_comma();
        b.escape_and_append_with_quotes("group_names");
        b.append_colon();
        b.start_object();
        bool first = true;
        for (const auto& [hash, name] : *group_names) {
            if (!first) b.append_comma();
            first = false;
            b.escape_and_append_with_quotes(hash);
            b.append_colon();
            b.escape_and_append_with_quotes(name);
        }
        b.end_object();
    }
    b.end_object();
    b.end_object();
    return std::string(b);
}

// The summary is unfiltered, so any server-side predicate forces a live scan.
// pid/tid are exempt: they select whole lanes, which the summary can still do.
bool viz_summary_eligible(const QueryParams& params) {
    return params.get("query").empty() && params.get("cat").empty() &&
           params.get("lanes").empty() && params.get("filters").empty() &&
           params.get("file").empty() && params.get("group_by").empty();
}

coro::CoroTask<void> append_app_spans(std::vector<std::string>& out,
                                      TraceIndex& index, double begin,
                                      double end, const QueryParams& params) {
    if (!viz_summary_eligible(params)) co_return;
    const VizSummary* s = co_await ensure_viz_summary(index);
    if (!s) co_return;
    auto pid_s = params.get("pid");
    auto tid_s = params.get("tid");
    bool has_pid = !pid_s.empty();
    bool has_tid = !tid_s.empty();
    std::int64_t want_pid =
        has_pid ? std::strtoll(pid_s.data(), nullptr, 10) : 0;
    std::int64_t want_tid =
        has_tid ? std::strtoll(tid_s.data(), nullptr, 10) : 0;
    for (const auto& sp : s->app_spans) {
        if (has_pid && sp.pid != want_pid) continue;
        if (has_tid && sp.tid != want_tid) continue;
        if (static_cast<double>(sp.end) > begin &&
            static_cast<double>(sp.begin) < end)
            out.push_back(sp.json);
    }
}

// Re-aggregate one pyramid level's buckets over [begin_abs, end_abs] into
// pixel-column density blocks. `begin_abs`/`end_abs` are absolute us.
static std::string serve_density_from_summary(
    const VizSummary& s, const VizSummary::Level& level,
    const QueryParams& params, double begin_abs, double end_abs,
    double original_begin, double original_end, double threshold,
    bool ts_normalized, std::uint64_t display_global_min,
    TraceIndex::TimeMetric metric) {
    // normalize_event_ts subtracts a NATIVE-unit offset from the (native) event
    // ts, but display_global_min is in us; recover the native value so long
    // events aren't shifted off-screen on non-us traces (the us round-trip
    // loses at most sub-us, invisible on the timeline).
    const std::uint64_t native_global_min =
        dftracer::utils::trace::scale_between(TraceIndex::TimeMetric::US,
                                              metric, display_global_min);
    auto pid_s = params.get("pid");
    auto tid_s = params.get("tid");
    bool has_pid = !pid_s.empty();
    bool has_tid = !tid_s.empty();
    std::int64_t want_pid =
        has_pid ? std::strtoll(pid_s.data(), nullptr, 10) : 0;
    std::int64_t want_tid =
        has_tid ? std::strtoll(tid_s.data(), nullptr, 10) : 0;

    std::int64_t b0 = s.bucket_of(begin_abs, level.bucket_us, level.nbuckets);
    std::int64_t b1 = s.bucket_of(end_abs, level.bucket_us, level.nbuckets);
    if (b0 < 0) b0 = 0;
    if (b1 < 0) b1 = static_cast<std::int64_t>(level.nbuckets) - 1;

    DensityMap dens;
    auto add_cell = [&](std::int64_t pid, std::int64_t tid, std::int64_t col,
                        std::uint32_t count, double total,
                        const VizSummary::Cell& cell) {
        if (count == 0 && total <= 0) return;
        auto& a = dens[DensityKey{pid, tid, col, {}}];
        a.count += count;
        a.total += total;
        if (static_cast<double>(cell.max_dur) > a.max_dur) {
            a.max_dur = static_cast<double>(cell.max_dur);
            if (cell.name_id < s.names.size()) a.name = s.names[cell.name_id];
        }
    };
    for (const auto& lane : level.lanes) {
        if (has_pid && lane.pid != want_pid) continue;
        if (has_tid && lane.tid != want_tid) continue;
        auto lo = std::lower_bound(lane.buckets.begin(), lane.buckets.end(),
                                   static_cast<std::uint32_t>(b0));
        for (auto it = lo;
             it != lane.buckets.end() && *it <= static_cast<std::uint32_t>(b1);
             ++it) {
            const auto& cell =
                lane.cells[static_cast<std::size_t>(it - lane.buckets.begin())];
            // A bucket never exceeds one column, so it falls in one column or
            // straddles two. Split it by overlap rather than dumping it whole
            // into the column holding its center, which would misplace a third
            // of the events once buckets and columns are of similar size.
            double bstart = static_cast<double>(s.t_begin) +
                            static_cast<double>(*it) * level.bucket_us;
            std::int64_t col =
                static_cast<std::int64_t>((bstart - begin_abs) / threshold);
            double edge = begin_abs + static_cast<double>(col + 1) * threshold;
            double head = edge - bstart;
            if (head >= level.bucket_us) {
                add_cell(lane.pid, lane.tid, col, cell.count,
                         static_cast<double>(cell.total), cell);
                continue;
            }
            double f = head / level.bucket_us;
            auto c0 = static_cast<std::uint32_t>(
                std::llround(static_cast<double>(cell.count) * f));
            if (c0 > cell.count) c0 = cell.count;
            double t0 = static_cast<double>(cell.total) * f;
            add_cell(lane.pid, lane.tid, col, c0, t0, cell);
            add_cell(lane.pid, lane.tid, col + 1, cell.count - c0,
                     static_cast<double>(cell.total) - t0, cell);
        }
    }

    // Long events at least one pixel wide are drawn as real spans; the rest
    // fold into density blocks, which is the split a live scan makes. The
    // widest win when there are more than a response can carry.
    std::vector<const VizSummary::AppSpan*> wide;
    auto fold_span = [&](const VizSummary::AppSpan& sp) {
        auto dur = static_cast<double>(sp.end - sp.begin);
        std::int64_t col = static_cast<std::int64_t>(
            (static_cast<double>(sp.begin) - begin_abs) / threshold);
        auto& a = dens[DensityKey{sp.pid, sp.tid, col, {}}];
        a.count += 1;
        a.total += dur;
        if (dur > a.max_dur) {
            a.max_dur = dur;
            if (sp.name_id < s.names.size()) a.name = s.names[sp.name_id];
        }
    };
    for (const auto& sp : s.long_events) {
        if (has_pid && sp.pid != want_pid) continue;
        if (has_tid && sp.tid != want_tid) continue;
        if (static_cast<double>(sp.end) <= begin_abs ||
            static_cast<double>(sp.begin) >= end_abs)
            continue;
        if (static_cast<double>(sp.end - sp.begin) >= threshold)
            wide.push_back(&sp);
        else
            fold_span(sp);
    }
    if (wide.size() > VizSummary::MAX_RESPONSE_SPANS) {
        std::nth_element(
            wide.begin(), wide.begin() + VizSummary::MAX_RESPONSE_SPANS,
            wide.end(),
            [](const VizSummary::AppSpan* a, const VizSummary::AppSpan* b) {
                return a->end - a->begin > b->end - b->begin;
            });
        for (std::size_t i = VizSummary::MAX_RESPONSE_SPANS; i < wide.size();
             ++i)
            fold_span(*wide[i]);
        wide.resize(VizSummary::MAX_RESPONSE_SPANS);
    }

    std::vector<std::string> spans;
    ankerl::unordered_dense::set<std::int64_t> span_lanes;
    for (const auto& sp : s.app_spans) {
        if (has_pid && sp.pid != want_pid) continue;
        if (has_tid && sp.tid != want_tid) continue;
        if (static_cast<double>(sp.end) <= begin_abs ||
            static_cast<double>(sp.begin) >= end_abs)
            continue;
        span_lanes.insert((sp.pid << 20) ^ sp.tid);
        spans.push_back(
            ts_normalized && display_global_min > 0
                ? normalize_event_ts(sp.json, native_global_min, metric)
                : sp.json);
    }
    // App spans get an injected depth 0; long events after them do not, so the
    // client stacks overlapping wide events instead of piling them on one row.
    const std::size_t napp_spans = spans.size();
    spans.reserve(spans.size() + wide.size());
    for (const auto* sp : wide) {
        span_lanes.insert((sp->pid << 20) ^ sp->tid);
        spans.push_back(
            ts_normalized && display_global_min > 0
                ? normalize_event_ts(sp->json, native_global_min, metric)
                : sp->json);
    }
    // Folded child activity sits one row below its app span, matching the
    // containment nesting the live path computes.
    if (!span_lanes.empty())
        for (auto& [k, a] : dens)
            if (span_lanes.count((k.pid << 20) ^ k.tid)) a.depth = 1;

    // Re-bucket the summary's ph="C" series onto this window's density columns
    // and fold them into the same density map, so the unfiltered/summary path
    // renders counters as ordinary event blocks like the live path. The summary
    // carries each series' emitting pid/tid (0 = node-level).
    if (!s.counter_series.empty() && threshold > 0 && s.fine_bucket_us > 0) {
        const std::size_t ncols =
            static_cast<std::size_t>((end_abs - begin_abs) / threshold) + 1;
        for (const auto& cd : s.counter_series) {
            std::string series = cd.name + "." + cd.key;
            for (std::size_t i = 0; i < cd.buckets.size(); ++i) {
                double ts_mid = static_cast<double>(s.t_begin) +
                                (static_cast<double>(cd.buckets[i]) + 0.5) *
                                    s.fine_bucket_us;
                long col = static_cast<long>((ts_mid - begin_abs) / threshold);
                if (col < 0 || col >= static_cast<long>(ncols)) continue;
                add_counter_bucket(dens, cd.pid, cd.tid, col, series, cd.sum[i],
                                   cd.cnt[i]);
            }
        }
    }

    std::vector<std::uint32_t> span_depth(napp_spans, 0);
    // Stack counter series above their lane's spans (the summary path skips
    // assign_view_depths, so counters would otherwise pile on depth 0).
    assign_counter_depths(spans, span_depth, dens);
    return serialize_density_body(
        spans, dens, original_begin, original_end, threshold, 0, false,
        ts_normalized, display_global_min, static_cast<double>(s.max_dur),
        metric, &span_depth, nullptr);
}

// Canonicalize one group column: resolved.*/r.* aliases map to their hash
// column and "args.x" to "x". Sets `rt` to the hash type when the canonical
// column is a hash (so its values can be resolved to names for display).
static std::string canonicalize_group_col(
    std::string col,
    std::optional<utilities::indexer::IndexDatabase::HashType>& rt) {
    using HashType = utilities::indexer::IndexDatabase::HashType;
    col = std::string(strip_args_prefix(col));
    auto is = [&](std::string_view a) { return col == a; };
    if (is("resolved.fpath") || is("r.fpath"))
        col = "fhash";
    else if (is("resolved.cwd") || is("r.cwd"))
        col = "cwd";
    else if (is("resolved.hostname") || is("r.hostname") ||
             is("resolved.host") || is("r.host"))
        col = "hhash";
    else if (is("resolved.exec") || is("r.exec"))
        col = "exec_hash";
    else if (is("resolved.cmd") || is("r.cmd"))
        col = "cmd_hash";
    rt = std::nullopt;
    if (col == "fhash" || col == "cwd")
        rt = HashType::FILE;
    else if (col == "hhash")
        rt = HashType::HOST;
    else if (col == "shash" || col == "exec_hash" || col == "cmd_hash")
        rt = HashType::STRING;
    return col;
}

// GET /api/viz/density: like /viz/events, but instead of dropping sub-pixel
// events it buckets them per (pid, tid, pixel-column) into density blocks so
// zoomed-out views still show where activity is. Returns full-size events (with
// args, for the detail panel) plus the aggregated density blocks.
coro::CoroTask<HttpResponse> handle_viz_density(const HttpRequest& req,
                                                const QueryParams& params,
                                                TraceIndex& index) {
    if (!params.has("begin") || !params.has("end")) {
        co_return HttpResponse::bad_request(
            "Missing required parameters: begin, end");
    }

    double begin = params.get_double("begin", 0);
    double end = params.get_double("end", 0);
    int summary = params.get_int("summary", 2);
    if (summary < 1) summary = 1;

    auto query = params.get("query");
    if (!query.empty() && !query::try_parse(query).has_value()) {
        co_return HttpResponse::bad_request("Invalid query: " +
                                            std::string(query));
    }

    // Optional density grouping column. A well-formed name that matches no
    // event field is fine (all blocks land in the client's "(none)" group);
    // only malformed names are rejected.
    std::string group_col(params.get("group_by"));
    if (group_col.size() > 128)
        co_return HttpResponse::bad_request("group_by too long");
    for (char c : group_col) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' &&
            c != '.' && c != '-' && c != ',')
            co_return HttpResponse::bad_request("Invalid group_by column");
    }

    // Hash columns auto-resolve for display: group values stay raw hashes, and
    // a group_names map (hash -> resolved name) rides along in the metadata.
    // resolved.*/r.* aliases map to their hash columns. group_by may list
    // several comma-separated columns; canonicalize each and record its hash
    // type so the composite value's components resolve independently.
    using HashType = utilities::indexer::IndexDatabase::HashType;
    std::vector<std::optional<HashType>> resolve_types;
    {
        std::string rebuilt;
        std::size_t start = 0;
        while (start <= group_col.size()) {
            auto comma = group_col.find(',', start);
            std::string part(group_col.substr(
                start, comma == std::string::npos ? group_col.size() - start
                                                  : comma - start));
            std::optional<HashType> rt;
            std::string canon = canonicalize_group_col(std::move(part), rt);
            if (!rebuilt.empty()) rebuilt.push_back(',');
            rebuilt += canon;
            resolve_types.push_back(rt);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        group_col = std::move(rebuilt);
    }
    bool any_resolve = false;
    for (const auto& rt : resolve_types)
        if (rt) any_resolve = true;

    auto win = parse_viz_window(params, index);
    if (!win) co_return std::move(win.error());
    begin = win->begin;
    end = win->end;
    double original_begin = win->original_begin;
    double original_end = win->original_end;
    std::uint64_t global_min = win->global_min;

    // Bucket width (== the ~1px cutoff), in the client's actual pixels.
    int width = params.get_int("width", DEFAULT_VIEWPORT_WIDTH);
    width = std::clamp(width, MIN_VIEWPORT_WIDTH, MAX_VIEWPORT_WIDTH);
    double threshold =
        duration_threshold(begin, end, static_cast<unsigned>(summary),
                           static_cast<unsigned>(width));

    const std::string cache_key =
        std::string(req.path) + "?" + params.canonical_key();
    if (auto hit = index.viz_cache().get(cache_key))
        co_return HttpResponse::ok(std::move(*hit));

    // Unfiltered views come from the prebuilt summary pyramid (no event cap),
    // built lazily here; concurrent requests fall through to a live scan, as do
    // zooms finer than the pyramid's finest level.
    double cfg_agg_interval = 0;  // trace-declared aggregation window, us
    bool trace_has_agg = false;
    if (threshold > 0 && viz_summary_eligible(params)) {
        const VizSummary* s = co_await ensure_viz_summary(index);
        if (s && s->t_end > s->t_begin) {
            cfg_agg_interval = s->agg_interval_us;
            trace_has_agg = s->has_aggregated;
            // ph=3 aggregates are absent from the duration-built pyramid, so an
            // aggregated trace falls through to the (cheap) live scan.
            if (!s->has_aggregated) {
                if (const VizSummary::Level* level = s->level_for(threshold)) {
                    DFTRACER_UTILS_LOG_DEBUG(
                        "viz: density threshold %.0f us served from level "
                        "%.0f us",
                        threshold, level->bucket_us);
                    co_return HttpResponse::ok(serve_density_from_summary(
                        *s, *level, params, begin, end, original_begin,
                        original_end, threshold, global_min > 0,
                        index.native_to_us(index.global_min_timestamp_us()),
                        index.time_metric()));
                }
            }
        }
    }

    // summary=1 means "no aggregation", but the viewport is still finite.
    // Fold sub-pixel events into density blocks instead of dropping them.
    if (threshold <= 0 && end > begin)
        threshold = (end - begin) / static_cast<double>(width);

    // Enclosing events are fetched by a separate pass (below) so a large
    // `lookback` can never starve the in-window scan's budget. lookback arrives
    // in us; convert to the trace's native unit so scan_begin (native) is right
    // - otherwise on non-us traces the scan-back is off by the unit scale and
    // long events that enclose the window are never read.
    double lookback = params.get_double("lookback", 0);
    if (lookback < 0) lookback = 0;
    if (lookback > 0 && index.time_metric() != TraceIndex::TimeMetric::US)
        lookback = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(lookback)));

    // The client feeds back the longest event in the trace as `lookback`, so
    // honoring it literally rescans everything before the window. Anything that
    // wide is already in the summary's long-event list, so take enclosers from
    // there and scan back only far enough to catch the ones too short to list.
    // Only whole-run enclosers (>= half the span) are guaranteed complete in
    // the summary's long-event list (compact_long exempts them from its
    // per-bucket quota). Shorter enclosers the quota may have dropped, so the
    // live scan back must reach them instead of trusting the list. All three
    // uses below share this one threshold so there is no gap and no
    // double-serve.
    const VizSummary* enc_summary = nullptr;
    double enc_threshold_us = 0;
    double enc_threshold_native = 0;
    if (lookback > 0 && viz_summary_eligible(params)) {
        const VizSummary* s = co_await ensure_viz_summary(index);
        if (s != nullptr && s->long_threshold_us > 0) {
            enc_summary = s;
            enc_threshold_us = s->long_threshold_us;
            enc_threshold_native =
                index.time_metric() == TraceIndex::TimeMetric::US
                    ? enc_threshold_us
                    : static_cast<double>(index.us_to_native(
                          static_cast<std::uint64_t>(enc_threshold_us)));
            lookback = std::min(lookback, enc_threshold_native);
        }
    }

    double scan_begin = begin - lookback;
    if (scan_begin < 0) scan_begin = 0;

    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;

    // Per-scan partial: sub-pixel events fold into `dens`, the rest are kept
    // raw in `big` (with their durations). `max_dur` tracks the longest event
    // seen (folded ones included).
    struct Acc {
        std::vector<std::string> big;
        std::vector<double> big_dur;  // parallel to `big`
        DensityMap dens;
        double max_dur = 0;
        std::vector<AggRec> agg;      // ph=3 records, kept whole
    };
    auto merge_acc = [](Acc&& x, Acc&& y) {
        if (y.max_dur > x.max_dur) x.max_dur = y.max_dur;
        for (auto& d : y.big_dur) x.big_dur.push_back(d);
        for (auto& s : y.big) x.big.emplace_back(std::move(s));
        for (auto& r : y.agg) x.agg.emplace_back(std::move(r));
        for (auto& kv : y.dens) {
            auto it = x.dens.find(kv.first);
            if (it == x.dens.end())
                x.dens.emplace(kv.first, std::move(kv.second));
            else
                it->second.merge_from(kv.second);
        }
        return std::move(x);
    };
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    const std::uint64_t scan_cap =
        limit > 0 ? static_cast<std::uint64_t>(limit) : 0;
    bool single_file = !params.get("file").empty();
    auto cancel_pred = [&req]() { return req.cancel_token.cancelled(); };
    // Build a window View over the target files. build_viz_view puts the ts
    // window into the per-event query; an explicit ?file= scans every chunk
    // (cached per-chunk bounds can be wrong under clock skew), so skip the
    // time_range chunk pruning then - the per-event filter still applies.
    auto make_window_view = [&](double lo, double hi) {
        ViewDefinition vd = build_viz_view(params, lo, hi, 0);
        views::View v =
            views::View::from_files(
                to_view_files(select_viz_target_files(index, params, lo, hi)),
                &index.bloom_cache())
                .phase(views::Phase::Events)
                .metadata(false)
                .cancel_when(cancel_pred);
        if (vd.query) v = v.filter(*vd.query);
        if (!single_file) v = v.time_range(lo, hi);
        return v;
    };

    // Number of density columns spanning [begin, end]; counter series share
    // this column grid.
    std::size_t ncols =
        threshold > 0 ? static_cast<std::size_t>((end - begin) / threshold) + 1
                      : 0;

    // Pass 1 (in-window): one scan over ph="X" and ph="C". ph="X" events fold
    // into density blocks (small) or stay raw (big); ph="C" events aggregate
    // into per-(name.arg) counter blocks on the same column grid, so counters
    // land in the same density map and render as ordinary events. The two
    // phases split as fused partition branches so a single decode feeds both.
    auto p1run = make_window_view(begin, end)
                     .phase(views::Phase::Any)
                     .limit(scan_cap)
                     .session();
    auto ev_out = p1run.fold<Acc>(
        query::parse_or_throw("ph == 1 or ph == \"X\""),
        [&group_col, threshold, begin](Acc& acc, const auto& jv,
                                       std::string_view raw) {
            double dur = 0;
            if (!fold_density(jv.element(), threshold, begin, acc.dens, &dur,
                              group_col)) {
                acc.big.emplace_back(raw);
                acc.big_dur.push_back(dur);
            }
            if (dur > acc.max_dur) acc.max_dur = dur;
        },
        merge_acc);
    auto cs_out = p1run.fold<Acc>(
        query::parse_or_throw("ph == 2 or ph == \"C\""),
        [begin, threshold, ncols](Acc& acc, const auto& jv, std::string_view) {
            fold_counter_density(jv.element(), begin, threshold, ncols,
                                 acc.dens);
        },
        merge_acc);
    // ph=3 SELECTIVE-aggregation records: the individual events were dropped at
    // capture time, so keep each whole (all args intact for selection) and give
    // it a renderable span below once the aggregation window is known.
    auto ag_out = p1run.fold<Acc>(
        query::parse_or_throw("ph == 3 or ph == \"A\""),
        [](Acc& acc, const auto& jv, std::string_view raw) {
            collect_aggregated(jv.element(), raw, acc.agg);
        },
        merge_acc);
    auto p1 = co_await p1run.execute();
    bool truncated = p1.truncated;

    DensityMap dens = std::move(ev_out->dens);
    // Fold the counter blocks into the same density map (disjoint keys:
    // counters carry the series identity in the density key's group).
    for (auto& [k, a] : cs_out->dens) {
        auto it = dens.find(k);
        if (it == dens.end())
            dens.emplace(k, std::move(a));
        else
            it->second.merge_from(a);
    }
    std::vector<std::string> big = std::move(ev_out->big);
    std::vector<double> big_dur = std::move(ev_out->big_dur);
    // Longest event scanned (folded ones included); the client feeds it back as
    // `lookback` so deep zooms still catch long enclosing events.
    double max_dur = ev_out->max_dur;

    // Aggregation window, from the ts spacing of the in-window aggregates, with
    // the trace-declared cfg as cross-check and single-window fallback.
    double agg_interval = 0;
    if (trace_has_agg || !ag_out->agg.empty()) {
        std::vector<double> ats;
        ats.reserve(ag_out->agg.size());
        for (const auto& r : ag_out->agg) ats.push_back(r.ts);
        agg_interval = infer_agg_interval(std::move(ats));
        double cfg_native = cfg_agg_interval;
        if (cfg_agg_interval > 0 &&
            index.time_metric() != TraceIndex::TimeMetric::US)
            cfg_native = static_cast<double>(index.us_to_native(
                static_cast<std::uint64_t>(cfg_agg_interval)));
        if (agg_interval <= 0) {
            agg_interval = cfg_native;
        } else if (cfg_native > 0) {
            double diff = agg_interval > cfg_native ? agg_interval - cfg_native
                                                    : cfg_native - agg_interval;
            if (diff > 0.5 * cfg_native)
                DFTRACER_UTILS_LOG_WARN(
                    "viz: aggregated interval inferred %.0f differs from cfg "
                    "%.0f",
                    agg_interval, cfg_native);
        }
    }

    // An aggregate's ts is its window start, so one that opened before `begin`
    // still covers the view but the in-window pass skips it (ts < begin). Scan
    // back one window to recover these enclosing aggregates.
    if (agg_interval > 0 && begin > 0) {
        double asb = begin - agg_interval;
        if (asb < 0) asb = 0;
        if (asb < begin) {
            auto er = make_window_view(asb, begin)
                          .phase(views::Phase::Any)
                          .limit(scan_cap)
                          .session();
            auto eo = er.fold<Acc>(
                query::parse_or_throw("ph == 3 or ph == \"A\""),
                [](Acc& acc, const auto& jv, std::string_view raw) {
                    collect_aggregated(jv.element(), raw, acc.agg);
                },
                merge_acc);
            co_await er.execute();
            for (auto& r : eo->agg) ag_out->agg.emplace_back(std::move(r));
        }
    }

    // Pass 2 (enclosers): keep only events still open at `begin`
    // (ts < begin <= ts + dur); these ancestor bars set containment depth.
    if (scan_begin < begin) {
        // Whole-run enclosers come from the summary below; excluding them here
        // keeps them from being served twice. Everything shorter is caught
        // here.
        double skip_from = enc_summary != nullptr
                               ? enc_threshold_native
                               : std::numeric_limits<double>::infinity();
        auto p2 =
            co_await make_window_view(scan_begin, begin)
                .map_batches<Acc>(
                    [begin, skip_from](
                        Acc& acc, const std::vector<std::string_view>& events) {
                        for (auto ev : events) {
                            double ts = 0, dur = 0;
                            if (!parse_ts_dur(ev, ts, dur)) continue;
                            if (dur > acc.max_dur) acc.max_dur = dur;
                            if (ts < begin && ts + dur > begin &&
                                dur < skip_from) {
                                acc.big.emplace_back(ev);
                                acc.big_dur.push_back(dur);
                            }
                        }
                    },
                    merge_acc, slots, scan_cap);
        truncated = truncated || p2.stats.truncated;
        if (p2.value.max_dur > max_dur) max_dur = p2.value.max_dur;
        for (auto d : p2.value.big_dur) big_dur.push_back(d);
        for (auto& s : p2.value.big) big.emplace_back(std::move(s));
    }
    // Enclosers wide enough to be listed in the summary, found without the
    // scan back toward the start of the trace that finding them live takes.
    if (enc_summary != nullptr) {
        auto pid_s = params.get("pid");
        auto tid_s = params.get("tid");
        bool has_pid = !pid_s.empty();
        bool has_tid = !tid_s.empty();
        std::int64_t want_pid =
            has_pid ? std::strtoll(pid_s.data(), nullptr, 10) : 0;
        std::int64_t want_tid =
            has_tid ? std::strtoll(tid_s.data(), nullptr, 10) : 0;
        for (const auto& sp : enc_summary->long_events) {
            auto dur = static_cast<double>(sp.end - sp.begin);
            // Shorter enclosers are caught by the live scan above; serving them
            // here too would double them (and the quota may have dropped some).
            if (dur < enc_threshold_us) continue;
            if (static_cast<double>(sp.begin) >= begin ||
                static_cast<double>(sp.end) <= begin)
                continue;
            if (has_pid && sp.pid != want_pid) continue;
            if (has_tid && sp.tid != want_tid) continue;
            if (dur > max_dur) max_dur = dur;
            big.push_back(sp.json);
            big_dur.push_back(dur);
        }
    }

    // Bound individual events by count. `threshold` is per-pixel time and is
    // blind to how many events share a pixel, so a narrow-but-dense window
    // (e.g. a whole trace compressed into a sliver by bad timestamps) would
    // otherwise return every event whole. Keep the longest - the slices wide
    // enough to see - and fold the rest into density blocks so none are lost.
    constexpr std::size_t MAX_DENSITY_EVENTS = 30000;
    const std::size_t cap =
        limit > 0 ? static_cast<std::size_t>(limit) : MAX_DENSITY_EVENTS;
    if (big.size() > cap) {
        std::vector<std::uint32_t> idx(big.size());
        std::iota(idx.begin(), idx.end(), std::uint32_t{0});
        std::nth_element(idx.begin(), idx.begin() + static_cast<long>(cap),
                         idx.end(),
                         [&big_dur](std::uint32_t a, std::uint32_t b) {
                             return big_dur[a] > big_dur[b];
                         });
        std::vector<std::uint32_t> overflow(
            idx.begin() + static_cast<long>(cap), idx.end());
        fold_overflow_events(big, big_dur, overflow, threshold, begin, dens,
                             group_col);
        idx.resize(cap);
        std::sort(idx.begin(), idx.end());
        std::vector<std::string> kept;
        std::vector<double> kept_dur;
        kept.reserve(cap);
        kept_dur.reserve(cap);
        for (std::uint32_t i : idx) {
            kept.emplace_back(std::move(big[i]));
            kept_dur.push_back(big_dur[i]);
        }
        big.swap(kept);
        big_dur.swap(kept_dur);
        truncated = true;
    }

    // Bound the block count too. A dense window occupies a block per pixel per
    // lane (threshold is per-pixel time, so many lanes x many columns), so
    // coarsen columns - merge pairs, doubling the effective bucket width -
    // until the map fits the budget. This is the trade the summary pyramid
    // makes, applied here for the live path; block ts/width use eff_threshold.
    constexpr std::size_t MAX_DENSITY_BLOCKS = 120000;
    double eff_threshold = threshold;
    while (dens.size() > MAX_DENSITY_BLOCKS && eff_threshold > 0) {
        DensityMap merged;
        merged.reserve(dens.size() / 2 + 1);
        for (auto& [k, a] : dens) {
            DensityKey nk{k.pid, k.tid, k.col >> 1, k.group};
            auto it = merged.find(nk);
            if (it == merged.end())
                merged.emplace(std::move(nk), std::move(a));
            else
                it->second.merge_from(a);
        }
        dens = std::move(merged);
        eff_threshold *= 2;
        truncated = true;
    }

    drop_unreferenced_hash_records(big);

    co_await append_app_spans(big, index, begin, end, params);

    // Aggregated (ph=3) records ride through as whole events so selection keeps
    // every arg (dur_sum, tag_min, ...); we only tag them and give them the
    // aggregation window as `dur` so they draw as a span. dur is left absent
    // when the window cannot be inferred (client falls back to args.dur_sum).
    // Held back from `big` here so they neither enter the greedy depth packer
    // An aggregate has no positions inside its window, but count + dur_sum let
    // us extrapolate uniformly-spaced synthetic events that position and nest
    // by containment like real events. This is the representation at EVERY zoom
    // - the view never re-skins between a merged block and individual events as
    // the user zooms. A per-request budget bounds the count: split it across
    // the aggregates, subsampling them when there are more than the budget so a
    // full-trace view never floods the response.
    if (agg_interval > 0 && !ag_out->agg.empty()) {
        constexpr std::size_t SYNTHETIC_BUDGET = 40000;
        std::size_t nagg = ag_out->agg.size();
        std::size_t per_agg_cap =
            std::min(static_cast<std::size_t>(std::max(1, width)),
                     std::max<std::size_t>(1, SYNTHETIC_BUDGET / nagg));
        std::size_t step =
            nagg > SYNTHETIC_BUDGET ? nagg / SYNTHETIC_BUDGET : 1;
        for (std::size_t i = 0; i < nagg; i += step)
            extrapolate_aggregate(ag_out->agg[i], agg_interval, begin, end,
                                  per_agg_cap, big, big_dur);
        if (agg_interval > max_dur) max_dur = agg_interval;
    }

    // Resolve hash group values (fhash -> path, hhash -> host, ...) for the
    // groups actually present; unresolvable hashes fall back to the raw value
    // on the client.
    ankerl::unordered_dense::map<std::string, std::string> group_names;
    if (any_resolve && !group_col.empty()) {
        // Split each composite group key into its per-column components and
        // collect the ones from hash columns; resolve each distinct hash once.
        ankerl::unordered_dense::map<std::string, HashType> to_resolve;
        auto collect = [&](std::string_view composite) {
            std::size_t start = 0, idx = 0;
            while (start <= composite.size() && idx < resolve_types.size()) {
                auto sep = composite.find(GROUP_SEP, start);
                auto part =
                    composite.substr(start, sep == std::string_view::npos
                                                ? composite.size() - start
                                                : sep - start);
                if (resolve_types[idx] && !part.empty())
                    to_resolve.emplace(std::string(part), *resolve_types[idx]);
                ++idx;
                if (sep == std::string_view::npos) break;
                start = sep + 1;
            }
        };
        for (const auto& [k, a] : dens)
            if (!k.group.empty()) collect(k.group);
        for (const auto& e : big) {
            auto g = extract_group_from_line(e, group_col);
            if (!g.empty()) collect(g);
        }
        for (const auto& [hash, type] : to_resolve) {
            auto name = index.resolve_hash(type, hash);
            if (!name.empty()) group_names.emplace(hash, std::move(name));
        }
    }

    // Stable per-event/-block depth (computed in absolute space, before ts
    // normalization rewrites the big strings; order is preserved in place).
    std::vector<std::uint32_t> big_depth =
        assign_view_depths(big, dens, begin, eff_threshold);
    // Counters get their own rows above the lane's events (assign_view_depths
    // leaves every counter block on depth 0).
    assign_counter_depths(big, big_depth, dens);
    if (global_min > 0 || index.time_metric() != TraceIndex::TimeMetric::US) {
        for (auto& e : big)
            e = normalize_event_ts(e, global_min, index.time_metric());
    }

    std::string body = serialize_density_body(
        big, dens, original_begin, original_end, eff_threshold, limit,
        truncated, global_min > 0,
        index.native_to_us(index.global_min_timestamp_us()), max_dur,
        index.time_metric(), &big_depth,
        group_names.empty() ? nullptr : &group_names);
    if (!req.cancel_token.cancelled()) index.viz_cache().put(cache_key, body);
    co_return HttpResponse::ok(std::move(body));
}

}  // namespace dftracer::utils::server
