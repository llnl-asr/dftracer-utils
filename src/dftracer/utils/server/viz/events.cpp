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

static const std::unordered_set<std::string> HASH_METADATA_NAMES = {"FH", "HH",
                                                                    "SH"};

// Rewrite the unsigned integer value of `key` (e.g. "\"dur\":") in `json` to

// Normalize event timestamps (when global_min > 0) and serialize the collected
// events plus metadata into the Chrome Trace Event Format body. `global_min` is
// the de-normalization base (already 0 unless normalization is active);
// `display_global_min` is the value reported in the metadata. Pure/synchronous.
static std::string build_viz_events_body(std::vector<std::string>& events,
                                         std::uint64_t global_min,
                                         double meta_begin, double meta_end,
                                         int limit, bool truncated,
                                         std::uint64_t display_global_min,
                                         TraceIndex::TimeMetric metric) {
    // Even without an offset, a non-US trace still needs ts/dur scaled to us.
    if (global_min > 0 || metric != TraceIndex::TimeMetric::US) {
        for (auto& event : events) {
            event = normalize_event_ts(event, global_min, metric);
        }
    }

    auto& b = scratch_json_builder();
    b.start_object();
    b.escape_and_append_with_quotes("events");
    b.append_colon();
    b.start_array();
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i > 0) b.append_comma();
        b.append_raw(events[i]);  // Already JSON
    }
    b.end_array();
    b.append_comma();
    b.escape_and_append_with_quotes("metadata");
    b.append_colon();
    b.start_object();
    b.append_key_value("begin", meta_begin);
    b.append_comma();
    b.append_key_value("end", meta_end);
    b.append_comma();
    b.append_key_value("count", events.size());
    b.append_comma();
    b.append_key_value("limit", limit);
    b.append_comma();
    b.append_key_value("truncated", truncated);
    b.append_comma();
    b.append_key_value("ts_normalized", global_min > 0);
    b.append_comma();
    b.append_key_value("global_min_timestamp_us", display_global_min);
    b.end_object();
    b.end_object();
    return std::string(b);
}

coro::CoroTask<HttpResponse> handle_viz_events(const HttpRequest& req,
                                               const QueryParams& params,
                                               TraceIndex& index) {
    // Required: begin, end, summary
    if (!params.has("begin") || !params.has("end") || !params.has("summary")) {
        co_return HttpResponse::bad_request(
            "Missing required parameters: begin, end, summary");
    }

    int summary = params.get_int("summary", 1);
    if (summary < 1) summary = 1;

    auto win = parse_viz_window(params, index);
    if (!win) co_return std::move(win.error());
    double begin = win->begin;
    double end = win->end;
    double original_begin = win->original_begin;
    double original_end = win->original_end;
    std::uint64_t global_min = win->global_min;

    double min_dur =
        duration_threshold(begin, end, static_cast<unsigned>(summary));
    // Full detail (summary=1) has no duration floor, so a wide window would
    // return every sub-pixel event (millions on a large trace, hanging the
    // client). Bound it to ~1px at the client width; sub-pixel events cannot
    // be drawn anyway, and zooming in shrinks the window so detail returns.
    if (min_dur <= 0 && end > begin) {
        int px_width = params.get_int("width", DEFAULT_VIEWPORT_WIDTH);
        px_width = std::clamp(px_width, MIN_VIEWPORT_WIDTH, MAX_VIEWPORT_WIDTH);
        min_dur = (end - begin) / static_cast<double>(px_width);
    }

    // Overlap, bounded: look back at most `lookback` (the longest event's
    // duration, supplied by the client) so events that started before the
    // window but extend into it are included, without scanning to time 0.
    // lookback is in us; convert to native so scan_begin (native) is right.
    double lookback = params.get_double("lookback", 0);
    if (lookback < 0) lookback = 0;
    if (lookback > 0 && index.time_metric() != TraceIndex::TimeMetric::US)
        lookback = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(lookback)));
    double scan_begin = begin - lookback;
    if (scan_begin < 0) scan_begin = 0;

    ViewDefinition view = build_viz_view(params, scan_begin, end, min_dur);

    // Optional limit: 0 (default) means no limit.
    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;

    // Zoom-out fast path: when nothing is filtered and the duration threshold
    // is at least the summary's long-event threshold, the events that survive
    // are exactly a subset of the summary's cached long_events. Serve them from
    // memory instead of scanning every file (minutes for a large trace).
    if (min_dur > 0 && viz_summary_eligible(params)) {
        const VizSummary* s = co_await ensure_viz_summary(index);
        if (s != nullptr && s->long_threshold_us > 0 &&
            min_dur >= s->long_threshold_us) {
            auto pid_s = params.get("pid");
            auto tid_s = params.get("tid");
            bool has_pid = !pid_s.empty();
            bool has_tid = !tid_s.empty();
            std::int64_t want_pid =
                has_pid ? std::strtoll(pid_s.data(), nullptr, 10) : 0;
            std::int64_t want_tid =
                has_tid ? std::strtoll(tid_s.data(), nullptr, 10) : 0;
            std::vector<const VizSummary::AppSpan*> hits;
            for (const auto& ev : s->long_events) {
                if (static_cast<double>(ev.end - ev.begin) < min_dur) continue;
                if (static_cast<double>(ev.end) <= begin ||
                    static_cast<double>(ev.begin) >= end)
                    continue;
                if (has_pid && ev.pid != want_pid) continue;
                if (has_tid && ev.tid != want_tid) continue;
                hits.push_back(&ev);
            }
            // Keep the widest: a shallow zoom can match more of the list than
            // any response should carry.
            if (hits.size() > VizSummary::MAX_RESPONSE_SPANS) {
                std::nth_element(
                    hits.begin(), hits.begin() + VizSummary::MAX_RESPONSE_SPANS,
                    hits.end(),
                    [](const VizSummary::AppSpan* a,
                       const VizSummary::AppSpan* x) {
                        return a->end - a->begin > x->end - x->begin;
                    });
                hits.resize(VizSummary::MAX_RESPONSE_SPANS);
            }
            std::vector<std::string> collected;
            collected.reserve(hits.size());
            for (const auto* ev : hits) collected.push_back(ev->json);
            co_await append_app_spans(collected, index, begin, end, params);
            std::string body = build_viz_events_body(
                collected, global_min, original_begin, original_end, limit,
                false, index.native_to_us(index.global_min_timestamp_us()),
                index.time_metric());
            co_return HttpResponse::ok(body);
        }
    }

    std::vector<const TraceIndex::FileInfo*> target_files =
        select_viz_target_files(index, params, scan_begin, end);
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    bool single_file = !params.get("file").empty();

    struct EvAcc {
        std::vector<std::string> events;
    };
    views::View v =
        views::View::from_files(to_view_files(target_files),
                                &index.bloom_cache())
            .phase(views::Phase::Events)
            .cancel_when([&req]() { return req.cancel_token.cancelled(); });
    if (view.query) v = v.filter(*view.query);
    if (!single_file) v = v.time_range(scan_begin, end);
    auto scan = co_await v.map_batches<EvAcc>(
        [](EvAcc& a, const std::vector<std::string_view>& events) {
            a.events.reserve(a.events.size() + events.size());
            for (auto ev : events) a.events.emplace_back(ev);
        },
        [](EvAcc&& x, EvAcc&& y) {
            x.events.reserve(x.events.size() + y.events.size());
            for (auto& s : y.events) x.events.emplace_back(std::move(s));
            return std::move(x);
        },
        slots, limit > 0 ? static_cast<std::uint64_t>(limit) : 0);

    bool truncated = scan.stats.truncated;
    std::vector<std::string> collected_events = std::move(scan.value.events);
    if (limit > 0 && static_cast<int>(collected_events.size()) > limit) {
        collected_events.resize(static_cast<std::size_t>(limit));
        truncated = true;
    }

    co_await append_app_spans(collected_events, index, begin, end, params);

    std::string body = build_viz_events_body(
        collected_events, global_min, original_begin, original_end, limit,
        truncated, index.native_to_us(index.global_min_timestamp_us()),
        index.time_metric());
    co_return HttpResponse::ok(body);
}

}  // namespace dftracer::utils::server
