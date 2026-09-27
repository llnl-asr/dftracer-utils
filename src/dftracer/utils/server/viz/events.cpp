#include <dftracer/utils/core/common/field_ref.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/json/json_doc_guard.h>
#include <dftracer/utils/json/json_value.h>
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
#include <dftracer/utils/server/viz/record_event.h>
#include <dftracer/utils/server/viz/scan.h>
#include <dftracer/utils/server/viz/summary_build.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_planner_utility.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <simdjson.h>

#include <algorithm>
#include <cmath>
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
#include <vector>

namespace dftracer::utils::server {

using namespace dftracer::utils::trace;
using namespace dftracer::utils::trace::views;
using dftracer::utils::json::json_number;

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

    auto win = parse_viz_window(params, index, req.path);
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

    const TraceFields fields(index.record_schema());
    ViewDefinition view =
        build_viz_view(params, scan_begin, end, min_dur, fields);

    // Optional limit: 0 (default) means no limit.
    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;

    // Zoom-out fast path: when nothing is filtered and the duration threshold
    // is at least the summary's long-event threshold, the events that survive
    // are exactly a subset of the summary's cached long_events. Serve them from
    // memory instead of scanning every file (minutes for a large trace).
    if (min_dur > 0 && viz_summary_eligible(params, fields)) {
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
            co_await append_app_spans(collected, index, begin, end, params,
                                      fields);
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
        views::View::from_files(to_view_files(target_files))
            .phase(views::Phase::Events)
            .cancel_when([&req]() { return req.cancel_token.cancelled(); });
    if (view.query) v = v.filter(*view.query);
    if (!single_file || fields.by_path) v = v.time_range(scan_begin, end);
    auto scan = co_await v.map_batches<EvAcc>(
        [&fields](EvAcc& a, const std::vector<std::string_view>& events) {
            a.events.reserve(a.events.size() + events.size());
            fields.for_each_event(events, [&a](std::string_view ev) {
                a.events.emplace_back(ev);
            });
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

    co_await append_app_spans(collected_events, index, begin, end, params,
                              fields);

    std::string body = build_viz_events_body(
        collected_events, global_min, original_begin, original_end, limit,
        truncated, index.native_to_us(index.global_min_timestamp_us()),
        index.time_metric());
    co_return HttpResponse::ok(body);
}

namespace {

// A cell as JSON. A `time` cell is a duration: native units scaled to us, or
// `scale` microseconds per unit for a path schema's duration field.
void append_cell(simdjson::builder::string_builder& b,
                 const dataframe::Series& col, std::int64_t r,
                 const TraceIndex& index, bool time, double scale) {
    using dataframe::TypeId;
    if (col.is_null(r)) {
        b.append_raw("null");
        return;
    }
    if (time && scale != 1 &&
        (col.type() == TypeId::Int64 || col.type() == TypeId::Uint64 ||
         col.type() == TypeId::Float64)) {
        const double v = col.type() == TypeId::Int64
                             ? static_cast<double>(col.data<std::int64_t>()[r])
                         : col.type() == TypeId::Uint64
                             ? static_cast<double>(col.data<std::uint64_t>()[r])
                             : col.data<double>()[r];
        b.append_raw(std::to_string(std::llround(v * scale)));
        return;
    }
    switch (col.type()) {
        case TypeId::String:
            b.escape_and_append_with_quotes(col.string_at(r));
            return;
        case TypeId::Int64: {
            const auto v = col.data<std::int64_t>()[r];
            b.append_raw(std::to_string(
                time && v > 0 ? static_cast<std::int64_t>(index.native_to_us(
                                    static_cast<std::uint64_t>(v)))
                              : v));
            return;
        }
        case TypeId::Uint64: {
            const auto v = col.data<std::uint64_t>()[r];
            b.append_raw(std::to_string(time ? index.native_to_us(v) : v));
            return;
        }
        case TypeId::Float64:
            b.append_raw(std::to_string(col.data<double>()[r]));
            return;
        default:
            b.append_raw("null");
    }
}

}  // namespace

// Records written without a clock (ts 0, such as CUDA activity, or a path
// schema's records without a time): events and aggregated records the
// timeline cannot place. `count` covers every match; `events` is the page at
// `offset`, longest first, with dur in us.
coro::CoroTask<HttpResponse> handle_viz_untimed(const HttpRequest& req,
                                                const QueryParams& params,
                                                TraceIndex& index) {
    const int limit = std::clamp(params.get_int("limit", 1000), 0, 10000);
    const int offset = std::max(0, params.get_int("offset", 0));
    const TraceFields fields(index.record_schema());
    if (fields.by_path && fields.time.empty())
        co_return HttpResponse::ok(
            R"({"events":[],"count":0,"offset":0,"limit":0})");
    std::string text =
        fields.by_path ? fields.time + " is missing" : std::string("ts == 0");
    if (const auto user = params.get("duql"); !user.empty()) {
        if (auto refused = refuse_duql(user, req.path))
            co_return std::move(*refused);
        text = "(" + text + ") and (" + std::string(user) + ")";
    }

    // Every match of a query, sorted once and cached as "<count>\n" plus one
    // row object per line, so paging slices lines instead of rescanning. A
    // path schema's rows name its label, entity and duration fields as the
    // trace fields they play.
    const std::string cache_key = std::string("untimed\n") +
                                  std::string(params.get("file")) + "\n" + text;
    std::string all;
    if (auto hit = index.viz_cache().get(cache_key)) {
        all = std::move(*hit);
    } else {
        std::vector<std::string> columns;
        std::vector<std::string> names;
        auto add = [&](const std::string& field, const char* name) {
            if (field.empty()) return;
            columns.push_back(field);
            names.emplace_back(name);
        };
        add(fields.label, "name");
        if (!fields.by_path) add("cat", "cat");
        add(fields.entity, "pid");
        add(fields.lane, "tid");
        add(fields.duration, "dur");
        if (!fields.by_path) add("ph", "ph");
        std::vector<std::string> order;
        std::vector<bool> descending;
        for (const char* key : {"dur", "pid", "tid", "name"})
            for (std::size_t c = 0; c < names.size(); ++c)
                if (names[c] == key) {
                    order.push_back(columns[c]);
                    descending.push_back(names[c] == "dur");
                }
        const auto rows =
            co_await views::View::from_files(
                to_view_files(collect_candidate_files(index, params)))
                .phase(views::Phase::Any)
                .filter(duql::parse_or_throw(text))
                .cancel_when([&req]() { return req.cancel_token.cancelled(); })
                .select(columns)
                .sort_by_multi(order, descending)
                .collect();
        auto& rb = scratch_json_builder();
        rb.append_raw(std::to_string(rows.num_rows()));
        for (std::int64_t r = 0; r < rows.num_rows(); ++r) {
            rb.append_raw("\n");
            rb.start_object();
            for (std::size_t c = 0; c < rows.names.size(); ++c) {
                if (c > 0) rb.append_comma();
                const auto at =
                    std::find(columns.begin(), columns.end(), rows.names[c]);
                const std::string& name =
                    at == columns.end()
                        ? rows.names[c]
                        : names[static_cast<std::size_t>(at - columns.begin())];
                rb.escape_and_append_with_quotes(name);
                rb.append_colon();
                append_cell(rb, rows.columns[c], r, index, name == "dur",
                            fields.by_path ? fields.duration_us : 1);
            }
            if (fields.by_path) {
                if (fields.lane.empty()) rb.append_raw(R"(,"tid":0)");
                rb.append_raw(R"(,"cat":null,"ph":1)");
            }
            rb.end_object();
        }
        all = std::string(rb);
        if (!req.cancel_token.cancelled())
            index.viz_cache().put(cache_key, all);
    }

    const std::string_view text_rows(all);
    const std::size_t head = text_rows.find('\n');
    const std::uint64_t count = std::strtoull(all.c_str(), nullptr, 10);
    // Rows [offset, offset + limit): skip `offset` lines, keep `limit`.
    std::size_t pos = head == std::string_view::npos ? text_rows.size() : head;
    for (int i = 0; i < offset && pos < text_rows.size(); ++i) {
        const std::size_t nl = text_rows.find('\n', pos + 1);
        pos = nl == std::string_view::npos ? text_rows.size() : nl;
    }
    std::size_t end = pos;
    for (int i = 0; i < limit && end < text_rows.size(); ++i) {
        const std::size_t nl = text_rows.find('\n', end + 1);
        end = nl == std::string_view::npos ? text_rows.size() : nl;
    }
    std::string page;
    if (end > pos) {
        page.assign(text_rows.substr(pos + 1, end - pos - 1));
        std::replace(page.begin(), page.end(), '\n', ',');
    }

    auto& b = scratch_json_builder();
    b.start_object();
    b.escape_and_append_with_quotes("events");
    b.append_colon();
    b.append_raw("[" + page + "]");
    b.append_comma();
    b.append_key_value("count", count);
    b.append_comma();
    b.append_key_value("offset", offset);
    b.append_comma();
    b.append_key_value("limit", limit);
    b.end_object();
    co_return HttpResponse::ok(std::string(b));
}

}  // namespace dftracer::utils::server
