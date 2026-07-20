#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/json_builder.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/utilities/common/json/json_doc_guard.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/views/view_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::server {

using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::views;

static const std::unordered_set<std::string> HASH_METADATA_NAMES = {"FH", "HH",
                                                                    "SH"};

static constexpr int DEFAULT_VIEWPORT_WIDTH = 1920;
static constexpr int MIN_VIEWPORT_WIDTH = 320;
static constexpr int MAX_VIEWPORT_WIDTH = 8192;

// Rewrite the unsigned integer value of `key` (e.g. "\"dur\":") in `json` to
// `value`. No-op when the key is absent. Returns whether it rewrote.
static bool rewrite_uint_field(std::string& json, std::string_view key,
                               std::uint64_t value) {
    auto pos = json.find(key);
    if (pos == std::string::npos) return false;
    pos += key.size();
    while (pos < json.size() && std::isspace(json[pos])) ++pos;
    auto end_pos = pos;
    while (end_pos < json.size() &&
           (std::isdigit(json[end_pos]) || json[end_pos] == '-')) {
        ++end_pos;
    }
    if (end_pos == pos) return false;
    json.replace(pos, end_pos - pos, std::to_string(value));
    return true;
}

/// Normalize a Chrome Trace Event JSON string: subtract `offset` (native units)
/// from "ts" and scale ts/dur from the trace's native unit `metric` into
/// microseconds. Falls back to the original string on parse failure. For a US
/// trace this only subtracts the offset (scaling is identity).
static std::string normalize_event_ts(const std::string& event_json,
                                      std::uint64_t offset,
                                      TraceIndex::TimeMetric metric) {
    thread_local simdjson::dom::parser tl_parser;
    auto result = tl_parser.parse(event_json);
    if (result.error()) return event_json;

    auto root = result.value_unsafe();
    if (!root.is_object()) return event_json;

    auto ts_result = root["ts"];
    if (ts_result.error()) return event_json;

    std::uint64_t old_ts = 0;
    if (ts_result.is_uint64()) {
        old_ts = ts_result.get_uint64().value_unsafe();
    } else if (ts_result.is_int64()) {
        auto val = ts_result.get_int64().value_unsafe();
        old_ts = val >= 0 ? static_cast<std::uint64_t>(val) : 0;
    } else {
        return event_json;
    }

    using dftracer::utils::utilities::composites::dft::scale_between;
    using TM = TraceIndex::TimeMetric;
    std::uint64_t new_ts =
        scale_between(metric, TM::US, old_ts >= offset ? old_ts - offset : 0);

    std::string modified = event_json;
    if (!rewrite_uint_field(modified, "\"ts\":", new_ts)) return event_json;

    if (metric != TM::US) {
        auto dur_result = root["dur"];
        if (!dur_result.error() && dur_result.is_uint64()) {
            rewrite_uint_field(
                modified, "\"dur\":",
                scale_between(metric, TM::US, dur_result.get_uint64().value()));
        }
    }
    return modified;
}

/// Compute the minimum event duration threshold for a given summary level.
/// Level 1 = full detail, higher levels filter shorter events.
static double duration_threshold(double begin, double end, unsigned level,
                                 unsigned viewport_width = 1920) {
    if (level <= 1) return 0.0;
    double range = end - begin;
    return range /
           (static_cast<double>(viewport_width) * static_cast<double>(level));
}

static std::string extract_json_value(simdjson::dom::element val) {
    if (val.is_string()) {
        return std::string(val.get_string().value_unsafe());
    }
    if (val.is_int64()) {
        return std::to_string(val.get_int64().value_unsafe());
    }
    if (val.is_uint64()) {
        return std::to_string(val.get_uint64().value_unsafe());
    }
    return {};
}

static void append_lane_clause(std::string& dsl, const char* field,
                               const std::string& val) {
    if (!dsl.empty()) dsl += " and ";
    bool numeric =
        !val.empty() && std::all_of(val.begin(), val.end(),
                                    [](char c) { return std::isdigit(c); });
    if (numeric) {
        dsl += std::string(field) + " == " + val;
    } else {
        dsl += std::string(field) + " == \"" + val + "\"";
    }
}

static void apply_lanes(std::string& dsl, std::string_view lanes_str) {
    if (lanes_str.empty()) return;

    thread_local simdjson::dom::parser tl_parser;
    auto result = tl_parser.parse(lanes_str.data(), lanes_str.size());
    if (result.error()) return;

    auto root = result.value_unsafe();

    if (root.is_array()) {
        auto arr = root.get_array().value_unsafe();
        for (auto item : arr) {
            if (!item.is_object()) continue;
            auto obj = item.get_object().value_unsafe();

            auto field_result = obj["field"];
            if (field_result.error()) field_result = obj["fields"];
            auto value_result = obj["value"];
            if (field_result.error() || value_result.error()) continue;

            if (!field_result.value_unsafe().is_string()) continue;
            const char* field =
                field_result.value_unsafe().get_c_str().value_unsafe();
            auto val = extract_json_value(value_result.value_unsafe());
            if (!val.empty()) append_lane_clause(dsl, field, val);
        }
    } else if (root.is_object()) {
        auto obj = root.get_object().value_unsafe();

        auto field_result = obj["field"];
        if (field_result.error()) field_result = obj["fields"];
        auto value_result = obj["value"];

        if (!field_result.error() && !value_result.error()) {
            if (field_result.value_unsafe().is_string()) {
                const char* field =
                    field_result.value_unsafe().get_c_str().value_unsafe();
                auto val = extract_json_value(value_result.value_unsafe());
                if (!val.empty()) append_lane_clause(dsl, field, val);
            }
        }
    }
}

static void apply_filters(std::string& dsl, std::string_view filters_str) {
    if (filters_str.empty()) return;

    thread_local simdjson::dom::parser tl_parser;
    auto result = tl_parser.parse(filters_str.data(), filters_str.size());
    if (result.error()) return;

    auto root = result.value_unsafe();
    if (!root.is_array()) return;

    auto arr = root.get_array().value_unsafe();
    for (auto item : arr) {
        if (!item.is_object()) continue;
        auto obj = item.get_object().value_unsafe();

        auto field_result = obj["field"];
        auto op_result = obj["op"];
        auto value_result = obj["value"];
        if (field_result.error() || op_result.error() || value_result.error())
            continue;

        if (!field_result.value_unsafe().is_string() ||
            !op_result.value_unsafe().is_string())
            continue;

        const char* field =
            field_result.value_unsafe().get_c_str().value_unsafe();
        const char* op = op_result.value_unsafe().get_c_str().value_unsafe();

        std::string val = extract_json_value(value_result.value_unsafe());
        if (val.empty()) continue;

        std::string op_str(op);
        std::string field_str(field);
        if (field_str == "begin") field_str = "ts";
        if (field_str == "end") field_str = "ts";
        if (field_str == "duration") field_str = "dur";

        std::string query_op;
        if (op_str == "=")
            query_op = "==";
        else if (op_str == ">=")
            query_op = ">=";
        else if (op_str == "<=")
            query_op = "<=";
        else if (op_str == ">")
            query_op = ">";
        else if (op_str == "<")
            query_op = "<";
        else
            continue;

        if (!dsl.empty()) dsl += " and ";
        bool numeric = !val.empty() && (std::isdigit(val[0]) || val[0] == '-');
        if (numeric || query_op != "==") {
            dsl += field_str + " " + query_op + " " + val;
        } else {
            dsl += field_str + " " + query_op + " \"" + val + "\"";
        }
    }
}

// --- GET /api/v1/viz/events ---
// Build the query view (time range + lane/filter/pid/tid/cat predicates) for a
// viz request from the parsed parameters.
static ViewDefinition build_viz_view(const QueryParams& params, double begin,
                                     double end, double min_dur) {
    ViewDefinition view;
    view.name = "viz_query";
    view.description = "Visualization query";

    // Reserve once and append in place: numbers go through to_chars into a
    // stack buffer (no per-number heap allocation, unlike std::to_string), and
    // literals/string_views are appended directly (no temporary
    // concatenations).
    std::string dsl;
    dsl.reserve(128);
    char numbuf[20];  // max digits of a uint64_t
    auto append_u64 = [&](std::uint64_t v) {
        dsl.append(numbuf, to_chars_u64(numbuf, numbuf + sizeof(numbuf), v));
    };

    dsl += "ts >= ";
    append_u64(static_cast<std::uint64_t>(begin));
    dsl += " and ts <= ";
    append_u64(static_cast<std::uint64_t>(end));
    if (min_dur > 0) {
        dsl += " and dur >= ";
        append_u64(static_cast<std::uint64_t>(min_dur));
    }

    apply_lanes(dsl, params.get("lanes"));
    apply_filters(dsl, params.get("filters"));

    auto pid = params.get("pid");
    if (!pid.empty()) {
        dsl += " and pid == ";
        dsl += pid;
    }

    auto tid = params.get("tid");
    if (!tid.empty()) {
        dsl += " and tid == ";
        dsl += tid;
    }

    auto cat = params.get("cat");
    if (!cat.empty()) {
        dsl += " and cat == \"";
        dsl += cat;
        dsl += '"';
    }

    // Raw DSL from the front-end query box, already validated by the caller.
    auto query = params.get("query");
    if (!query.empty()) {
        dsl += " and (";
        dsl += query;
        dsl += ')';
    }

    view.with_query(dsl);
    return view;
}

// Select the files to scan: the explicit ?file= or all indexed files, then drop
// files whose cached time bounds don't overlap [begin, end]. Pure/synchronous.
static std::vector<const TraceIndex::FileInfo*> select_viz_target_files(
    TraceIndex& index, const QueryParams& params, double begin, double end) {
    auto target_files = collect_candidate_files(index, params);

    // An explicit ?file= is a direct request for that file; never drop it on
    // cached time bounds (which can be wrong, e.g. multi-node clock skew).
    if (!params.get("file").empty()) return target_files;

    if (begin > 0 || end > 0) {
        std::vector<const TraceIndex::FileInfo*> filtered;
        filtered.reserve(target_files.size());
        for (auto* fi : target_files) {
            if (fi->min_timestamp_us == 0 && fi->max_timestamp_us == 0) {
                filtered.push_back(fi);
                continue;
            }
            double fi_min = static_cast<double>(fi->min_timestamp_us);
            double fi_max = static_cast<double>(fi->max_timestamp_us);
            if (fi_max < begin || fi_min > end) continue;
            filtered.push_back(fi);
        }
        target_files = std::move(filtered);
    }
    return target_files;
}

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

// One checkpoint byte-range to read from a specific file. Work is distributed
// at this granularity (not per file) so a single large file still fans out
// across every worker.
struct ScanWorkItem {
    const TraceIndex::FileInfo* file;
    std::uint64_t start_byte;
    std::uint64_t end_byte;
    std::size_t checkpoint_idx;
};

// Scan events matching `view` within [begin, end] across `target_files`,
// invoking `on_batch(slot, events)` for each batch. `on_batch` must be
// thread-safe across slots: it is called concurrently from worker coroutines,
// but each worker owns its own slot so per-slot state needs no lock. Returns
// true if scanning stopped early because `limit` (0 = unlimited) was reached.
static coro::CoroTask<bool> scan_view_events(
    TraceIndex& index, std::vector<const TraceIndex::FileInfo*>& target_files,
    ViewDefinition& view, double begin, double end, int limit,
    std::size_t num_slots,
    const std::function<void(std::size_t,
                             const std::vector<std::string_view>&)>& on_batch,
    CancelToken cancel, bool scan_all_chunks = false) {
    const std::int64_t cap =
        limit > 0 ? limit : std::numeric_limits<std::int64_t>::max();
    std::atomic<std::int64_t> produced{0};

    std::size_t num_workers =
        std::max<std::size_t>(1, std::min(num_slots, index.max_concurrent()));
    auto* executor = Executor::current();
    auto chan = coro::make_channel<ScanWorkItem>(num_workers * 4);
    auto* target_files_ptr = &target_files;
    auto* view_ptr = &view;
    auto* index_ptr = &index;
    auto* produced_ptr = &produced;
    auto* on_batch_ptr = &on_batch;

    CoroScope scope(executor);

    // Producer: prune each file to its matching checkpoints (bloom + time) and
    // feed those byte-ranges as work items. Building is cheap next to the
    // reads.
    scope.spawn([ch = chan->producer(), target_files_ptr, view_ptr, index_ptr,
                 produced_ptr, cap, begin, end, scan_all_chunks,
                 cancel](CoroScope&) mutable -> coro::CoroTask<void> {
        auto guard = ch.guard();
        for (auto* file_info : *target_files_ptr) {
            if (cancel.cancelled()) co_return;
            if (produced_ptr->load(std::memory_order_relaxed) >= cap) co_return;
            if (file_info->uncompressed_size == 0 &&
                file_info->num_checkpoints == 0)
                continue;

            ViewBuilderInput builder_input;
            builder_input.with_view(*view_ptr)
                .with_file_path(file_info->path)
                .with_index_path(
                    file_info->has_bloom_data ? file_info->index_path : "")
                .with_uncompressed_size(file_info->uncompressed_size)
                .with_num_checkpoints(file_info->num_checkpoints)
                .with_bloom_cache(&index_ptr->bloom_cache())
                .with_time_range(begin, end)
                .with_scan_all_chunks(scan_all_chunks);

            ViewBuilderUtility builder;
            auto build_output = co_await builder.process(builder_input);
            if (!build_output || !build_output->file_may_match) continue;

            for (const auto& c : build_output->candidates) {
                if (cancel.cancelled()) co_return;
                if (produced_ptr->load(std::memory_order_relaxed) >= cap)
                    co_return;
                if (!co_await ch.send(ScanWorkItem{
                        file_info, c.start_byte, c.end_byte, c.checkpoint_idx}))
                    co_return;
            }
        }
        co_return;
    });

    for (std::size_t w = 0; w < num_workers; ++w) {
        // Worker `w` writes only to slot `w`; a coroutine never runs
        // concurrently with itself, so per-slot output needs no lock.
        scope.spawn([w, chan, view_ptr, produced_ptr, on_batch_ptr, cap,
                     cancel](CoroScope&) -> coro::CoroTask<void> {
            while (auto item = co_await chan->receive()) {
                if (cancel.cancelled()) co_return;
                if (produced_ptr->load(std::memory_order_relaxed) >= cap)
                    co_return;

                ViewReaderInput reader_input;
                reader_input.with_file_path(item->file->path)
                    .with_index_path(item->file->index_path)
                    .with_byte_range(item->start_byte, item->end_byte)
                    .with_checkpoint_idx(item->checkpoint_idx)
                    .with_view(*view_ptr);

                ViewReaderUtility reader;
                auto gen = reader.process(reader_input);
                while (auto batch = co_await gen.next()) {
                    if (batch->events.empty()) continue;
                    if (cancel.cancelled()) co_return;
                    if (produced_ptr->load(std::memory_order_relaxed) >= cap)
                        break;
                    (*on_batch_ptr)(w, batch->events);
                    produced_ptr->fetch_add(
                        static_cast<std::int64_t>(batch->events.size()),
                        std::memory_order_relaxed);
                }
            }
            co_return;
        });
    }

    co_await scope.join();
    co_return produced.load(std::memory_order_relaxed) >= cap;
}

static coro::CoroTask<void> append_app_spans(std::vector<std::string>& out,
                                             TraceIndex& index, double begin,
                                             double end,
                                             const QueryParams& params);

static coro::CoroTask<HttpResponse> handle_viz_events(const HttpRequest& req,
                                                      const QueryParams& params,
                                                      TraceIndex& index) {
    // Required: begin, end, summary
    if (!params.has("begin") || !params.has("end") || !params.has("summary")) {
        co_return HttpResponse::bad_request(
            "Missing required parameters: begin, end, summary");
    }

    double begin = params.get_double("begin", 0);
    double end = params.get_double("end", 0);
    int summary = params.get_int("summary", 1);
    if (summary < 1) summary = 1;

    // Validate the optional raw DSL query before it is spliced into the view.
    auto query = params.get("query");
    if (!query.empty() &&
        !utilities::common::query::try_parse(query).has_value()) {
        co_return HttpResponse::bad_request("Invalid query: " +
                                            std::string(query));
    }

    // Timestamp normalization: default ON, opt-out with ?ts_normalize=0
    auto ts_norm_param = params.get("ts_normalize");
    bool normalize = ts_norm_param.empty() || ts_norm_param != "0";

    std::uint64_t global_min = 0;
    if (normalize) {
        global_min = index.global_min_timestamp_us();
        if (global_min == std::numeric_limits<std::uint64_t>::max()) {
            global_min = 0;  // No valid bounds, skip normalization
        }
    }

    // When normalization is active the user sends normalized
    // begin/end values (relative to global_min).  De-normalize them
    // so the predicate filters against absolute timestamps.
    double original_begin = begin;
    double original_end = end;
    // Client sends us; the scan matches the native index. Convert first, then
    // de-normalize against the native base. Identity for US traces.
    if (index.time_metric() != TraceIndex::TimeMetric::US) {
        begin = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(begin)));
        end = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(end)));
    }
    if (normalize && global_min > 0) {
        begin += static_cast<double>(global_min);
        end += static_cast<double>(global_min);
    }

    double min_dur =
        duration_threshold(begin, end, static_cast<unsigned>(summary));

    // Overlap, bounded: look back at most `lookback` (the longest event's
    // duration, supplied by the client) so events that started before the
    // window but extend into it are included, without scanning to time 0.
    double lookback = params.get_double("lookback", 0);
    if (lookback < 0) lookback = 0;
    double scan_begin = begin - lookback;
    if (scan_begin < 0) scan_begin = 0;

    ViewDefinition view = build_viz_view(params, scan_begin, end, min_dur);

    // Optional limit: 0 (default) means no limit.
    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;

    std::vector<const TraceIndex::FileInfo*> target_files =
        select_viz_target_files(index, params, scan_begin, end);

    // Each worker slot collects into its own vector (no shared state, no lock);
    // the slots are concatenated single-threaded after the scan.
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    std::vector<std::vector<std::string>> partials(slots);
    auto collect = [&partials](std::size_t w,
                               const std::vector<std::string_view>& events) {
        auto& out = partials[w];
        out.reserve(out.size() + events.size());
        for (auto ev : events) out.emplace_back(ev);
    };

    bool truncated = co_await scan_view_events(
        index, target_files, view, scan_begin, end, limit, slots, collect,
        req.cancel_token, !params.get("file").empty());

    std::vector<std::string> collected_events;
    for (auto& p : partials) {
        for (auto& s : p) collected_events.emplace_back(std::move(s));
    }
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

namespace {

struct NameStat {
    std::uint64_t count = 0;
    double total = 0;
    double min = 0;
    double max = 0;
    void merge_from(const NameStat& o) {
        if (count == 0) {
            min = o.min;
            max = o.max;
        } else if (o.count > 0) {
            min = std::min(min, o.min);
            max = std::max(max, o.max);
        }
        count += o.count;
        total += o.total;
    }
};

using NameMap = dftracer::utils::StringViewMap<NameStat>;

static double json_number(simdjson::dom::element el);

enum class GroupBy { Name, Cat, Pid, Fhash };

static GroupBy parse_group_by(std::string_view g) {
    if (g == "cat") return GroupBy::Cat;
    if (g == "pid") return GroupBy::Pid;
    if (g == "fhash" || g == "file") return GroupBy::Fhash;
    return GroupBy::Name;
}

// Parse one event's dur and grouping key, folding it into a worker-local map.
static void fold_event(std::string_view event, GroupBy group, NameMap& local) {
    thread_local simdjson::dom::parser parser;
    thread_local std::string buf;
    thread_local std::string keybuf;
    buf.assign(event);
    auto res = parser.parse(buf);
    if (res.error()) return;
    auto root = res.value_unsafe();
    if (!root.is_object()) return;

    auto dur_r = root["dur"];
    if (dur_r.error()) return;  // skip instant/metadata events (no duration)
    double dur = 0;
    if (dur_r.is_uint64())
        dur = static_cast<double>(dur_r.get_uint64().value_unsafe());
    else if (dur_r.is_int64())
        dur = static_cast<double>(dur_r.get_int64().value_unsafe());
    else if (dur_r.is_double())
        dur = dur_r.get_double().value_unsafe();
    else
        return;

    std::string_view name;
    if (group == GroupBy::Name) {
        auto r = root["name"];
        if (!r.error() && r.is_string()) name = r.get_string().value_unsafe();
    } else if (group == GroupBy::Cat) {
        auto r = root["cat"];
        if (!r.error() && r.is_string()) name = r.get_string().value_unsafe();
    } else if (group == GroupBy::Pid) {
        auto r = root["pid"];
        if (!r.error()) {
            keybuf = std::to_string(
                static_cast<std::int64_t>(json_number(r.value_unsafe())));
            name = keybuf;
        }
    } else {  // Fhash
        auto args = root["args"];
        if (!args.error() && args.is_object()) {
            auto r = args["fhash"];
            if (!r.error() && r.is_string())
                name = r.get_string().value_unsafe();
        }
        if (name.empty()) return;  // only I/O events have a file hash
    }

    auto it = local.find(name);
    if (it == local.end()) {
        it = local.emplace(std::string(name), NameStat{}).first;
        it->second.min = dur;
        it->second.max = dur;
    } else {
        it->second.min = std::min(it->second.min, dur);
        it->second.max = std::max(it->second.max, dur);
    }
    it->second.count += 1;
    it->second.total += dur;
}

// Sub-pixel events are bucketed by (pid, tid, pixel-column) instead of dropped,
// so zoomed-out views still show where activity is. The live path also assigns
// each bucket a containment depth (see assign_view_depths) so blocks stack
// under their enclosing events instead of collapsing to row 0.
struct DensityKey {
    std::int64_t pid;
    std::int64_t tid;
    std::int64_t col;
    std::string group;  // group_by value; "" when grouping is off or missing
    bool operator==(const DensityKey& o) const {
        return pid == o.pid && tid == o.tid && col == o.col && group == o.group;
    }
};

struct DensityKeyHash {
    std::uint64_t operator()(const DensityKey& k) const noexcept {
        std::uint64_t h = 1469598103934665603ULL;
        auto mix = [&](std::uint64_t v) {
            h ^= v;
            h *= 1099511628211ULL;
        };
        mix(static_cast<std::uint64_t>(k.pid));
        mix(static_cast<std::uint64_t>(k.tid));
        mix(static_cast<std::uint64_t>(k.col));
        mix(std::hash<std::string>{}(k.group));
        return h;
    }
};

struct DensityAgg {
    std::uint32_t count = 0;
    double total = 0;
    double max_dur = -1;
    std::uint32_t depth = 0;  // containment depth, set by assign_view_depths
    std::string
        name;  // representative: name of the longest event in the bucket
    void merge_from(const DensityAgg& o) {
        count += o.count;
        total += o.total;
        if (o.max_dur > max_dur) {
            max_dur = o.max_dur;
            name = o.name;
        }
    }
};

using DensityMap =
    ankerl::unordered_dense::map<DensityKey, DensityAgg, DensityKeyHash>;

static double json_number(simdjson::dom::element el) {
    if (el.is_uint64())
        return static_cast<double>(el.get_uint64().value_unsafe());
    if (el.is_int64())
        return static_cast<double>(el.get_int64().value_unsafe());
    if (el.is_double()) return el.get_double().value_unsafe();
    return 0;
}

// Value of `col` in the event for group_by, as a display string. Dotted paths
// walk nested objects; bare names fall back into "args" (the canonical home of
// domain fields). Anything missing or non-scalar yields "" so events are never
// dropped - the client renders those under "(none)".
static std::string extract_group_value(simdjson::dom::element root,
                                       std::string_view col) {
    auto scalar = [](simdjson::dom::element el) -> std::string {
        if (el.is_string()) return std::string(el.get_string().value_unsafe());
        if (el.is_int64()) return std::to_string(el.get_int64().value_unsafe());
        if (el.is_uint64())
            return std::to_string(el.get_uint64().value_unsafe());
        if (el.is_double())
            return std::to_string(el.get_double().value_unsafe());
        if (el.is_bool())
            return el.get_bool().value_unsafe() ? "true" : "false";
        return "";
    };
    auto walk = [&](std::string_view path,
                    simdjson::dom::element& out) -> bool {
        simdjson::dom::element cur = root;
        std::size_t start = 0;
        while (start <= path.size()) {
            auto dot = path.find('.', start);
            auto key = path.substr(start, dot == std::string_view::npos
                                              ? path.size() - start
                                              : dot - start);
            if (!key.empty()) {
                auto next = cur[key];
                if (next.error()) return false;
                cur = next.value_unsafe();
            }
            if (dot == std::string_view::npos) break;
            start = dot + 1;
        }
        out = cur;
        return true;
    };
    simdjson::dom::element v;
    if (walk(col, v)) return scalar(v);
    if (col.find('.') == std::string_view::npos) {
        auto nested = root["args"][col];
        if (!nested.error()) return scalar(nested.value_unsafe());
    }
    return "";
}

static std::string extract_group_from_line(std::string_view event,
                                           std::string_view col) {
    thread_local simdjson::dom::parser parser;
    thread_local std::string buf;
    buf.assign(event);
    auto res = parser.parse(buf);
    if (res.error()) return "";
    auto root = res.value_unsafe();
    if (!root.is_object()) return "";
    return extract_group_value(root, col);
}

// Fold a small event into the density map. Returns false (keep as an individual
// event) when the event has no duration or is at/above the `threshold`.
static bool fold_density(std::string_view event, double threshold, double begin,
                         DensityMap& dens, double* out_dur = nullptr,
                         std::string_view group_col = {}) {
    thread_local simdjson::dom::parser parser;
    thread_local std::string buf;
    buf.assign(event);
    auto res = parser.parse(buf);
    if (res.error()) return false;
    auto root = res.value_unsafe();
    if (!root.is_object()) return false;

    auto dr = root["dur"];
    if (dr.error()) return false;
    double dur = json_number(dr.value_unsafe());
    if (out_dur) *out_dur = dur;
    if (threshold <= 0 || dur >= threshold) return false;

    double ts = 0;
    auto tr = root["ts"];
    if (!tr.error()) ts = json_number(tr.value_unsafe());
    std::int64_t pid = 0;
    auto pr = root["pid"];
    if (!pr.error())
        pid = static_cast<std::int64_t>(json_number(pr.value_unsafe()));
    std::int64_t tid = 0;
    auto tir = root["tid"];
    if (!tir.error())
        tid = static_cast<std::int64_t>(json_number(tir.value_unsafe()));
    std::string_view name;
    auto nr = root["name"];
    if (!nr.error() && nr.is_string()) name = nr.get_string().value_unsafe();

    std::int64_t col = static_cast<std::int64_t>((ts - begin) / threshold);
    std::string group;
    if (!group_col.empty()) group = extract_group_value(root, group_col);
    DensityKey k{pid, tid, col, std::move(group)};
    auto it = dens.find(k);
    if (it == dens.end()) it = dens.emplace(std::move(k), DensityAgg{}).first;
    auto& a = it->second;
    a.count += 1;
    a.total += dur;
    if (dur > a.max_dur) {
        a.max_dur = dur;
        a.name.assign(name);
    }
    return true;
}

// Parse just the ts and dur of an event. Returns false for metadata/instant
// events that lack either field.
static bool parse_ts_dur(std::string_view event, double& ts, double& dur) {
    thread_local simdjson::dom::parser parser;
    thread_local std::string buf;
    buf.assign(event);
    auto res = parser.parse(buf);
    if (res.error()) return false;
    auto root = res.value_unsafe();
    if (!root.is_object()) return false;
    auto dr = root["dur"];
    auto tr = root["ts"];
    if (dr.error() || tr.error()) return false;
    dur = json_number(dr.value_unsafe());
    ts = json_number(tr.value_unsafe());
    return true;
}

// Parse pid, tid, ts, dur of an event. Returns false for metadata/instant
// events that lack ts or dur.
static bool parse_lane_ts_dur(std::string_view event, std::int64_t& pid,
                              std::int64_t& tid, double& ts, double& dur) {
    thread_local simdjson::dom::parser parser;
    thread_local std::string buf;
    buf.assign(event);
    auto res = parser.parse(buf);
    if (res.error()) return false;
    auto root = res.value_unsafe();
    if (!root.is_object()) return false;
    auto dr = root["dur"];
    auto tr = root["ts"];
    if (dr.error() || tr.error()) return false;
    dur = json_number(dr.value_unsafe());
    ts = json_number(tr.value_unsafe());
    auto pr = root["pid"];
    pid = pr.error()
              ? 0
              : static_cast<std::int64_t>(json_number(pr.value_unsafe()));
    auto tir = root["tid"];
    tid = tir.error()
              ? 0
              : static_cast<std::int64_t>(json_number(tir.value_unsafe()));
    return true;
}

// Containment depth per event/block, stable across zoom because an event's
// ancestors are always longer and so survive any threshold that kept it. A
// block is nested only under big events that fully cover its bucket interval
// [ts, ts+threshold]; a bucket-sized sibling that merely overlaps it is not an
// ancestor, so folded events stay on their sibling's row.
static std::vector<std::uint32_t> assign_view_depths(
    const std::vector<std::string>& big, DensityMap& dens, double begin,
    double threshold) {
    std::vector<std::uint32_t> depth(big.size(), 0);

    struct Item {
        double ts;          // interval start (big) or bucket left edge (block)
        double end;         // big end, or bucket right edge for a block query
        int big_idx;        // index into `big`, or -1 for a density query
        DensityAgg* block;  // set for a density query, null for a big event
    };
    struct LaneKey {
        std::int64_t pid, tid;
        bool operator==(const LaneKey& o) const {
            return pid == o.pid && tid == o.tid;
        }
    };
    struct LaneHash {
        std::uint64_t operator()(const LaneKey& k) const noexcept {
            std::uint64_t h = 1469598103934665603ULL;
            h = (h ^ static_cast<std::uint64_t>(k.pid)) * 1099511628211ULL;
            h = (h ^ static_cast<std::uint64_t>(k.tid)) * 1099511628211ULL;
            return h;
        }
    };
    ankerl::unordered_dense::map<LaneKey, std::vector<Item>, LaneHash> lanes;

    for (std::size_t i = 0; i < big.size(); ++i) {
        std::int64_t pid = 0, tid = 0;
        double ts = 0, dur = 0;
        if (!parse_lane_ts_dur(big[i], pid, tid, ts, dur)) continue;
        double end = ts + (dur > 0 ? dur : 0);
        lanes[LaneKey{pid, tid}].push_back(
            Item{ts, end, static_cast<int>(i), nullptr});
    }
    if (threshold > 0) {
        for (auto& kv : dens) {
            double lo = begin + static_cast<double>(kv.first.col) * threshold;
            lanes[LaneKey{kv.first.pid, kv.first.tid}].push_back(
                Item{lo, lo + threshold, -1, &kv.second});
        }
    }

    for (auto& kv : lanes) {
        auto& items = kv.second;
        // Opens sort before queries at equal ts so a block sitting exactly at a
        // parent's start counts that parent as an ancestor.
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.ts != b.ts) return a.ts < b.ts;
            return (a.block == nullptr) && (b.block != nullptr);
        });
        // Greedy lowest-free-row packing: staggered overlapping spans reuse
        // rows (real concurrency) instead of one row each, while nested events
        // still cascade.
        std::vector<double> row_end;  // end time occupying each row
        for (auto& it : items) {
            if (it.block) {
                std::size_t r = 0;
                while (r < row_end.size() && row_end[r] > it.end) ++r;
                it.block->depth = static_cast<std::uint32_t>(r);
            } else {
                std::size_t r = 0;
                while (r < row_end.size() && row_end[r] > it.ts) ++r;
                if (r == row_end.size())
                    row_end.push_back(it.end);
                else
                    row_end[r] = it.end;
                depth[static_cast<std::size_t>(it.big_idx)] =
                    static_cast<std::uint32_t>(r);
            }
        }
    }
    return depth;
}

// --- Counters (bandwidth / IOPS over time) ---
// Per-bucket read/write bytes and I/O op counts, aggregated from POSIX/STDIO/IO
// events (bytes come from args.ret).
struct CounterAcc {
    std::vector<double> read_bytes;
    std::vector<double> write_bytes;
    std::vector<double> ops;
    void init(std::size_t n) {
        read_bytes.assign(n, 0.0);
        write_bytes.assign(n, 0.0);
        ops.assign(n, 0.0);
    }
    void merge_from(const CounterAcc& o) {
        for (std::size_t i = 0; i < ops.size(); ++i) {
            read_bytes[i] += o.read_bytes[i];
            write_bytes[i] += o.write_bytes[i];
            ops[i] += o.ops[i];
        }
    }
};

static void fold_counter(std::string_view event, double begin, double bucket_us,
                         std::size_t buckets, CounterAcc& acc) {
    thread_local simdjson::dom::parser parser;
    thread_local std::string buf;
    buf.assign(event);
    auto res = parser.parse(buf);
    if (res.error()) return;
    auto root = res.value_unsafe();
    if (!root.is_object()) return;

    auto cat_r = root["cat"];
    if (cat_r.error() || !cat_r.is_string()) return;
    std::string_view cat = cat_r.get_string().value_unsafe();
    if (cat != "POSIX" && cat != "STDIO" && cat != "IO") return;

    auto ts_r = root["ts"];
    if (ts_r.error()) return;
    double ts = json_number(ts_r.value_unsafe());
    long col = static_cast<long>((ts - begin) / bucket_us);
    if (col < 0 || col >= static_cast<long>(buckets)) return;

    acc.ops[col] += 1.0;

    std::string_view name;
    auto name_r = root["name"];
    if (!name_r.error() && name_r.is_string())
        name = name_r.get_string().value_unsafe();

    double bytes = 0;
    auto args = root["args"];
    if (!args.error() && args.is_object()) {
        auto ret = args["ret"];
        if (!ret.error()) bytes = json_number(ret.value_unsafe());
    }
    if (bytes <= 0) return;
    if (name.find("write") != std::string_view::npos)
        acc.write_bytes[col] += bytes;
    else if (name.find("read") != std::string_view::npos)
        acc.read_bytes[col] += bytes;
}

}  // namespace

// --- Activity summary ("mipmap") build -------------------------------------
// Cells are shared and updated with relaxed atomics with no per-event lock:
// workers scan disjoint checkpoint (time) ranges, so they touch mostly-disjoint
// buckets. Only first-time lane and name creation take a brief lock.

namespace {

struct PidTid {
    std::int64_t pid;
    std::int64_t tid;
    bool operator==(const PidTid& o) const {
        return pid == o.pid && tid == o.tid;
    }
};

struct PidTidHash {
    std::uint64_t operator()(const PidTid& k) const noexcept {
        std::uint64_t h = 1469598103934665603ULL;
        h = (h ^ static_cast<std::uint64_t>(k.pid)) * 1099511628211ULL;
        h = (h ^ static_cast<std::uint64_t>(k.tid)) * 1099511628211ULL;
        return h;
    }
};

struct SumLane {
    std::int64_t pid;
    std::int64_t tid;
    std::vector<std::atomic<std::uint32_t>> count;
    std::vector<std::atomic<std::uint64_t>> total;
    std::vector<std::atomic<std::uint32_t>> maxd;
    std::vector<std::atomic<std::uint32_t>> nameid;
    SumLane(std::size_t nb, std::int64_t p, std::int64_t t)
        : pid(p), tid(t), count(nb), total(nb), maxd(nb), nameid(nb) {
        for (auto& x : nameid)
            x.store(std::numeric_limits<std::uint32_t>::max(),
                    std::memory_order_relaxed);
    }
};

struct SumBuild {
    std::size_t nb = 0;
    double bucket_us = 1;
    std::uint64_t t0 = 0;

    std::mutex lane_mtx;
    ankerl::unordered_dense::map<PidTid, std::size_t, PidTidHash> lane_of;
    std::deque<SumLane> lanes;

    std::vector<std::atomic<double>> cread;
    std::vector<std::atomic<double>> cwrite;
    std::vector<std::atomic<double>> cops;
    std::atomic<std::uint64_t> gmax_dur{0};

    std::mutex name_mtx;
    dftracer::utils::StringViewMap<std::uint32_t> name_of;
    std::vector<std::string> names;

    // Per-worker caches so the hot path never locks.
    std::vector<ankerl::unordered_dense::map<PidTid, SumLane*, PidTidHash>>
        lane_cache;
    std::vector<dftracer::utils::StringViewMap<std::uint32_t>> name_cache;

    // Per-worker Analyze aggregates (no lock); merged single-threaded after the
    // scan. One map per GroupBy dimension.
    std::vector<NameMap> g_name, g_cat, g_pid, g_fhash;

    // Per-worker FH resolution (file-hash -> path) from metadata records, so
    // the "By file" grouping can show real paths without depending on the
    // client.
    std::vector<dftracer::utils::StringViewMap<std::string>> fh_parts;

    std::vector<dftracer::utils::StringViewMap<std::string>> sh_parts;
    std::vector<ankerl::unordered_dense::map<std::int64_t, std::string>>
        app_start, app_end;

    // Per-worker events wider than one bucket, kept whole (see
    // VizSummary::long_events).
    std::vector<std::vector<VizSummary::AppSpan>> long_evs;

    // Per-worker column discovery: distinct column names, and the event names
    // already harvested (schema is per event name).
    std::vector<dftracer::utils::StringViewSet> cols, col_seen;

    // Per-worker operation-name -> category (first seen); merged after the scan
    // into VizSummary::name_cats for real layer labels.
    std::vector<dftracer::utils::StringViewMap<std::string>> name_cat;

    // Per-worker file-hashes seen on a read/write op (files with real data
    // I/O).
    std::vector<dftracer::utils::StringViewSet> io_fh;

    SumLane* get_lane(std::size_t w, std::int64_t pid, std::int64_t tid) {
        PidTid key{pid, tid};
        auto& cache = lane_cache[w];
        auto ci = cache.find(key);
        if (ci != cache.end()) return ci->second;
        std::lock_guard<std::mutex> lk(lane_mtx);
        auto it = lane_of.find(key);
        SumLane* lane;
        if (it != lane_of.end()) {
            lane = &lanes[it->second];
        } else {
            if (lanes.size() * nb >= VizSummary::MAX_CELLS)
                return nullptr;  // budget reached; drop further lanes
            lane_of.emplace(key, lanes.size());
            lanes.emplace_back(nb, pid, tid);
            lane = &lanes.back();
        }
        cache.emplace(key, lane);
        return lane;
    }

    std::uint32_t intern(std::size_t w, std::string_view name) {
        auto& cache = name_cache[w];
        auto ci = cache.find(name);
        if (ci != cache.end()) return ci->second;
        std::lock_guard<std::mutex> lk(name_mtx);
        auto it = name_of.find(name);
        std::uint32_t id;
        if (it != name_of.end()) {
            id = it->second;
        } else {
            id = static_cast<std::uint32_t>(names.size());
            names.emplace_back(name);
            name_of.emplace(std::string(name), id);
        }
        cache.emplace(std::string(name), id);
        return id;
    }
};

template <class T>
static void atomic_max(std::atomic<T>& a, T v) {
    T cur = a.load(std::memory_order_relaxed);
    while (v > cur &&
           !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

static void atomic_add_double(std::atomic<double>& a, double v) {
    double cur = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {
    }
}

// Fold one (key, dur) sample into a group aggregate, mirroring fold_event.
static void fold_group(NameMap& m, std::string_view key, double dur) {
    auto it = m.find(key);
    if (it == m.end()) {
        it = m.emplace(std::string(key), NameStat{}).first;
        it->second.min = dur;
        it->second.max = dur;
    } else {
        it->second.min = std::min(it->second.min, dur);
        it->second.max = std::max(it->second.max, dur);
    }
    it->second.count += 1;
    it->second.total += dur;
}

static void fold_summary(std::size_t w, std::string_view event, SumBuild& b) {
    thread_local simdjson::dom::parser parser;
    thread_local std::string buf;
    buf.assign(event);
    auto res = parser.parse(buf);
    if (res.error()) return;
    auto root = res.value_unsafe();
    if (!root.is_object()) return;

    std::string_view name0;
    {
        auto nr = root["name"];
        if (!nr.error() && nr.is_string())
            name0 = nr.get_string().value_unsafe();
    }
    if (name0 == "FH" || name0 == "SH") {
        auto args = root["args"];
        if (!args.error() && args.is_object()) {
            auto v = args["value"];
            auto n = args["name"];
            if (!v.error() && v.is_string() && !n.error() && n.is_string()) {
                auto& tbl = name0 == "FH" ? b.fh_parts[w] : b.sh_parts[w];
                tbl.emplace(std::string(v.get_string().value_unsafe()),
                            std::string(n.get_string().value_unsafe()));
            }
        }
        return;
    }

    auto dr = root["dur"];
    if (dr.error()) return;
    double dur = json_number(dr.value_unsafe());

    // Column discovery: harvest the schema once per distinct event name (same
    // name => same keys), so this is O(distinct names), not O(events).
    if (b.col_seen[w].find(name0) == b.col_seen[w].end()) {
        b.col_seen[w].emplace(name0);
        static const ankerl::unordered_dense::set<std::string_view> SKIP = {
            "pid", "tid", "ts", "dur", "ph", "id", "args"};
        auto obj = root.get_object();
        if (!obj.error())
            for (auto kv : obj.value_unsafe())
                if (SKIP.find(kv.key) == SKIP.end()) b.cols[w].emplace(kv.key);
        auto ar = root["args"];
        if (!ar.error() && ar.is_object())
            for (auto kv : ar.get_object().value_unsafe())
                b.cols[w].emplace(kv.key);
    }

    if (name0 == "start" || name0 == "end") {
        auto cr = root["cat"];
        auto pp = root["pid"];
        if (!cr.error() && cr.is_string() &&
            cr.get_string().value_unsafe() == "dftracer" && !pp.error()) {
            auto p = static_cast<std::int64_t>(json_number(pp.value_unsafe()));
            (name0 == "start" ? b.app_start[w] : b.app_end[w])
                .emplace(p, std::string(event));
        }
    }

    std::int64_t pid = 0, tid = 0;
    auto pr = root["pid"];
    if (!pr.error())
        pid = static_cast<std::int64_t>(json_number(pr.value_unsafe()));

    // Analyze aggregates: whole-trace, ts-independent so they match a live
    // scan.
    {
        auto nr = root["name"];
        auto cr = root["cat"];
        if (!nr.error() && nr.is_string()) {
            std::string_view nm = nr.get_string().value_unsafe();
            fold_group(b.g_name[w], nm, dur);
            if (!cr.error() && cr.is_string() &&
                b.name_cat[w].find(nm) == b.name_cat[w].end())
                b.name_cat[w].emplace(
                    std::string(nm),
                    std::string(cr.get_string().value_unsafe()));
        }
        if (!cr.error() && cr.is_string())
            fold_group(b.g_cat[w], cr.get_string().value_unsafe(), dur);
        thread_local std::string pidkey;
        pidkey.assign(std::to_string(pid));
        fold_group(b.g_pid[w], pidkey, dur);
        auto ar = root["args"];
        if (!ar.error() && ar.is_object()) {
            auto fr = ar["fhash"];
            if (!fr.error() && fr.is_string())
                fold_group(b.g_fhash[w], fr.get_string().value_unsafe(), dur);
        }
    }

    auto tr = root["ts"];
    if (tr.error()) return;  // bucketing/counters below need a timestamp
    double ts = json_number(tr.value_unsafe());
    std::int64_t bucket = static_cast<std::int64_t>(
        (ts - static_cast<double>(b.t0)) / b.bucket_us);
    if (bucket < 0) return;
    if (bucket >= static_cast<std::int64_t>(b.nb)) bucket = b.nb - 1;
    auto bi = static_cast<std::size_t>(bucket);

    auto tir = root["tid"];
    if (!tir.error())
        tid = static_cast<std::int64_t>(json_number(tir.value_unsafe()));

    if (dur >= b.bucket_us && dur > 0) {
        // Wider than one bucket: folded at the start bucket this would render
        // as a 1px sliver at zoom-out. Keep the event whole; the summary
        // density path serves it as a real span.
        b.long_evs[w].push_back(
            VizSummary::AppSpan{static_cast<std::uint64_t>(ts),
                                static_cast<std::uint64_t>(ts + dur), pid, tid,
                                std::string(event)});
    } else if (SumLane* lane = b.get_lane(w, pid, tid)) {
        lane->count[bi].fetch_add(1, std::memory_order_relaxed);
        lane->total[bi].fetch_add(static_cast<std::uint64_t>(dur < 0 ? 0 : dur),
                                  std::memory_order_relaxed);
        std::uint32_t d = dur >= 4294967295.0
                              ? 4294967295u
                              : static_cast<std::uint32_t>(dur < 0 ? 0 : dur);
        if (d > lane->maxd[bi].load(std::memory_order_relaxed)) {
            atomic_max(lane->maxd[bi], d);
            std::string_view name;
            auto nr = root["name"];
            if (!nr.error() && nr.is_string())
                name = nr.get_string().value_unsafe();
            lane->nameid[bi].store(b.intern(w, name),
                                   std::memory_order_relaxed);
        }
    }
    atomic_max(b.gmax_dur, static_cast<std::uint64_t>(dur < 0 ? 0 : dur));

    // Counters: read/write bytes and op counts for POSIX/STDIO/IO events.
    auto cat_r = root["cat"];
    if (cat_r.error() || !cat_r.is_string()) return;
    std::string_view cat = cat_r.get_string().value_unsafe();
    if (cat != "POSIX" && cat != "STDIO" && cat != "IO") return;
    atomic_add_double(b.cops[bi], 1.0);
    double bytes = 0;
    auto args = root["args"];
    if (!args.error() && args.is_object()) {
        auto ret = args["ret"];
        if (!ret.error()) bytes = json_number(ret.value_unsafe());
    }
    if (bytes <= 0) return;
    std::string_view name;
    auto nr = root["name"];
    if (!nr.error() && nr.is_string()) name = nr.get_string().value_unsafe();
    bool is_write = name.find("write") != std::string_view::npos;
    bool is_read = name.find("read") != std::string_view::npos;
    if (is_write)
        atomic_add_double(b.cwrite[bi], bytes);
    else if (is_read)
        atomic_add_double(b.cread[bi], bytes);
    if ((is_read || is_write) && !args.error() && args.is_object()) {
        auto fr = args["fhash"];
        if (!fr.error() && fr.is_string())
            b.io_fh[w].emplace(std::string(fr.get_string().value_unsafe()));
    }
}

}  // namespace

// Build the activity summary by scanning every event once. Blocks the caller
// (the first overview request) for the scan; cached for the server's lifetime.
static coro::CoroTask<void> build_viz_summary(TraceIndex& index) {
    auto summary = std::make_unique<VizSummary>();
    std::uint64_t gmin = index.global_min_timestamp_us();
    std::uint64_t gmax = index.global_max_timestamp_us();
    if (gmin == std::numeric_limits<std::uint64_t>::max() || gmax <= gmin) {
        index.set_viz_summary(std::move(summary));
        co_return;
    }

    std::size_t est_lanes = std::max<std::size_t>(1, index.file_count()) * 8;
    std::size_t nb = VizSummary::MAX_CELLS / est_lanes;
    nb = std::clamp(nb, VizSummary::MIN_BUCKETS_PER_LANE,
                    VizSummary::MAX_BUCKETS_PER_LANE);

    SumBuild b;
    b.nb = nb;
    b.t0 = gmin;
    b.bucket_us = static_cast<double>(gmax - gmin) / static_cast<double>(nb);
    b.cread = std::vector<std::atomic<double>>(nb);
    b.cwrite = std::vector<std::atomic<double>>(nb);
    b.cops = std::vector<std::atomic<double>>(nb);
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    b.lane_cache.resize(slots);
    b.name_cache.resize(slots);
    b.g_name.resize(slots);
    b.g_cat.resize(slots);
    b.g_pid.resize(slots);
    b.g_fhash.resize(slots);
    b.fh_parts.resize(slots);
    b.sh_parts.resize(slots);
    b.app_start.resize(slots);
    b.app_end.resize(slots);
    b.long_evs.resize(slots);
    b.cols.resize(slots);
    b.col_seen.resize(slots);
    b.name_cat.resize(slots);
    b.io_fh.resize(slots);

    ViewDefinition view;
    view.name = "viz_summary";
    view.description = "Activity summary build";
    std::vector<const TraceIndex::FileInfo*> files;
    files.reserve(index.files().size());
    for (const auto& f : index.files()) files.push_back(&f);

    auto on_batch = [&b](std::size_t w,
                         const std::vector<std::string_view>& events) {
        for (auto ev : events) fold_summary(w, ev, b);
    };
    co_await scan_view_events(index, files, view, static_cast<double>(gmin),
                              static_cast<double>(gmax), 0, slots, on_batch,
                              CancelToken{});

    summary->t_begin = gmin;
    summary->t_end = gmax;
    summary->nbuckets = nb;
    summary->bucket_us = b.bucket_us;
    summary->max_dur = b.gmax_dur.load(std::memory_order_relaxed);
    summary->names = std::move(b.names);
    summary->read_bytes.assign(nb, 0.0);
    summary->write_bytes.assign(nb, 0.0);
    summary->ops.assign(nb, 0.0);
    for (std::size_t i = 0; i < nb; ++i) {
        summary->read_bytes[i] = b.cread[i].load(std::memory_order_relaxed);
        summary->write_bytes[i] = b.cwrite[i].load(std::memory_order_relaxed);
        summary->ops[i] = b.cops[i].load(std::memory_order_relaxed);
    }
    summary->lanes.reserve(b.lanes.size());
    for (auto& sl : b.lanes) {
        VizSummary::Lane lane;
        lane.pid = sl.pid;
        lane.tid = sl.tid;
        lane.cells.resize(nb);
        for (std::size_t i = 0; i < nb; ++i) {
            lane.cells[i].count = sl.count[i].load(std::memory_order_relaxed);
            lane.cells[i].total = sl.total[i].load(std::memory_order_relaxed);
            lane.cells[i].max_dur = sl.maxd[i].load(std::memory_order_relaxed);
            lane.cells[i].name_id =
                sl.nameid[i].load(std::memory_order_relaxed);
        }
        summary->lanes.push_back(std::move(lane));
    }

    for (auto& part : b.long_evs)
        for (auto& ev : part) summary->long_events.push_back(std::move(ev));
    if (summary->long_events.size() > VizSummary::MAX_LONG_EVENTS) {
        // Keep the widest; fold the overflow into start-bucket cells (the
        // pre-cap behavior) so no activity is lost.
        auto& lv = summary->long_events;
        std::sort(
            lv.begin(), lv.end(),
            [](const VizSummary::AppSpan& a, const VizSummary::AppSpan& x) {
                return a.end - a.begin > x.end - x.begin;
            });
        ankerl::unordered_dense::map<PidTid, std::size_t, PidTidHash> lane_idx;
        for (std::size_t i = 0; i < summary->lanes.size(); ++i)
            lane_idx.emplace(
                PidTid{summary->lanes[i].pid, summary->lanes[i].tid}, i);
        for (std::size_t i = VizSummary::MAX_LONG_EVENTS; i < lv.size(); ++i) {
            const auto& ev = lv[i];
            auto it = lane_idx.find(PidTid{ev.pid, ev.tid});
            if (it == lane_idx.end()) continue;
            auto bi = summary->bucket_of(static_cast<double>(ev.begin));
            if (bi < 0) continue;
            auto& cell =
                summary->lanes[it->second].cells[static_cast<std::size_t>(bi)];
            cell.count += 1;
            cell.total += ev.end - ev.begin;
        }
        lv.resize(VizSummary::MAX_LONG_EVENTS);
    }

    {
        ankerl::unordered_dense::set<std::string> all_cols;
        for (auto& c : b.cols)
            for (auto& k : c) all_cols.emplace(k);
        summary->columns.assign(all_cols.begin(), all_cols.end());
        std::sort(summary->columns.begin(), summary->columns.end());
    }

    // Merge the per-worker Analyze aggregates and sort each by total desc.
    auto finalize_group = [](std::vector<NameMap>& parts) {
        NameMap merged;
        for (auto& p : parts) {
            for (auto& kv : p) {
                auto it = merged.find(kv.first);
                if (it == merged.end())
                    merged.emplace(kv.first, kv.second);
                else
                    it->second.merge_from(kv.second);
            }
        }
        std::vector<VizSummary::GroupRow> rows;
        rows.reserve(merged.size());
        for (auto& kv : merged)
            rows.push_back({kv.first, kv.second.count, kv.second.total,
                            kv.second.min, kv.second.max});
        std::sort(rows.begin(), rows.end(),
                  [](const VizSummary::GroupRow& lhs,
                     const VizSummary::GroupRow& rhs) {
                      return lhs.total > rhs.total;
                  });
        return rows;
    };
    summary->by_name = finalize_group(b.g_name);
    summary->by_cat = finalize_group(b.g_cat);
    summary->by_pid = finalize_group(b.g_pid);
    summary->by_fhash = finalize_group(b.g_fhash);

    // Resolve file-hash keys to real paths where a metadata record was seen.
    dftracer::utils::StringViewMap<std::string> fh;
    for (auto& part : b.fh_parts)
        for (auto& kv : part) fh.emplace(kv.first, kv.second);
    for (auto& r : summary->by_fhash) {
        auto it = fh.find(r.key);
        if (it != fh.end()) r.key = it->second;
    }
    summary->total_files = fh.size();

    {
        dftracer::utils::StringViewMap<std::string> sh;
        for (auto& part : b.sh_parts)
            for (auto& kv : part) sh.emplace(kv.first, kv.second);
        ankerl::unordered_dense::map<std::int64_t, std::string> starts, ends;
        for (auto& part : b.app_start)
            for (auto& kv : part) starts.emplace(kv.first, kv.second);
        for (auto& part : b.app_end)
            for (auto& kv : part) ends.emplace(kv.first, kv.second);

        auto resolve = [](dftracer::utils::StringViewMap<std::string>& tbl,
                          const std::string& h) -> std::string {
            auto it = tbl.find(h);
            return it != tbl.end() ? it->second : h;
        };
        simdjson::dom::parser sp, ep;
        for (auto& [pid, sjson] : starts) {
            auto eit = ends.find(pid);
            if (eit == ends.end()) continue;
            std::string sbuf(sjson), ebuf(eit->second);
            auto sr = sp.parse(sbuf);
            auto er = ep.parse(ebuf);
            if (sr.error() || er.error()) continue;
            auto se = sr.value_unsafe();
            auto ee = er.value_unsafe();

            auto num = [](simdjson::dom::element r, const char* k) -> double {
                auto v = r[k];
                return v.error() ? 0.0 : json_number(v.value_unsafe());
            };
            auto sarg = [](simdjson::dom::element r,
                           const char* k) -> std::string {
                auto a = r["args"];
                if (a.error()) return "";
                auto v = a[k];
                return (!v.error() && v.is_string())
                           ? std::string(v.get_string().value_unsafe())
                           : "";
            };
            auto narg = [](simdjson::dom::element r, const char* k) -> double {
                auto a = r["args"];
                if (a.error()) return 0.0;
                auto v = a[k];
                return v.error() ? 0.0 : json_number(v.value_unsafe());
            };

            auto start_ts = static_cast<std::uint64_t>(num(se, "ts"));
            auto end_ts = static_cast<std::uint64_t>(num(ee, "ts"));
            if (end_ts <= start_ts) continue;
            auto tid = static_cast<std::int64_t>(num(se, "tid"));
            std::string app = resolve(sh, sarg(se, "exec_hash"));
            std::string cmd = resolve(sh, sarg(se, "cmd_hash"));
            std::string cwd = resolve(fh, sarg(se, "cwd"));
            std::string version = sarg(se, "version");
            std::string date = sarg(se, "date");
            auto ppid = static_cast<std::int64_t>(narg(se, "ppid"));
            auto num_events = static_cast<std::int64_t>(narg(ee, "num_events"));
            if (app.empty()) app = "app " + std::to_string(pid);

            auto& jb = scratch_json_builder();
            jb.start_object();
            jb.append_key_value("name", app);
            jb.append_comma();
            jb.append_key_value("cat", "dftracer");
            jb.append_comma();
            jb.append_key_value("pid", pid);
            jb.append_comma();
            jb.append_key_value("tid", tid);
            jb.append_comma();
            jb.append_key_value("ts", start_ts);
            jb.append_comma();
            jb.append_key_value("dur", end_ts - start_ts);
            jb.append_comma();
            jb.append_key_value("ph", "X");
            jb.append_comma();
            jb.escape_and_append_with_quotes("args");
            jb.append_colon();
            jb.start_object();
            jb.append_key_value("app", app);
            jb.append_comma();
            jb.append_key_value("cmd", cmd);
            jb.append_comma();
            jb.append_key_value("cwd", cwd);
            jb.append_comma();
            jb.append_key_value("ppid", ppid);
            jb.append_comma();
            jb.append_key_value("version", version);
            jb.append_comma();
            jb.append_key_value("date", date);
            jb.append_comma();
            jb.append_key_value("num_events", num_events);
            jb.end_object();
            jb.end_object();
            summary->app_spans.push_back(
                {start_ts, end_ts, pid, tid, std::string(jb)});
        }
    }

    dftracer::utils::StringViewSet io_fh;
    for (auto& part : b.io_fh)
        for (auto& k : part) io_fh.emplace(k);
    summary->io_files = io_fh.size();

    // Merge the per-worker name -> category maps (first writer wins).
    dftracer::utils::StringViewMap<std::string> nc;
    for (auto& part : b.name_cat)
        for (auto& kv : part) nc.emplace(kv.first, kv.second);
    summary->name_cats.reserve(nc.size());
    for (auto& kv : nc) summary->name_cats.emplace_back(kv.first, kv.second);

    if (!summary->app_spans.empty()) {
        // Break = dead time between runs: merge app-spans into run clusters and
        // emit every gap between them, no size threshold.
        std::vector<const VizSummary::AppSpan*> spans;
        spans.reserve(summary->app_spans.size());
        for (const auto& sp : summary->app_spans) spans.push_back(&sp);
        std::sort(spans.begin(), spans.end(),
                  [](const auto* lhs, const auto* rhs) {
                      return lhs->begin < rhs->begin;
                  });
        std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
        for (const auto* sp : spans) {
            if (!runs.empty() && sp->begin <= runs.back().second)
                runs.back().second = std::max(runs.back().second, sp->end);
            else
                runs.emplace_back(sp->begin, sp->end);
        }
        for (std::size_t i = 1; i < runs.size(); ++i)
            if (runs[i].first > runs[i - 1].second)
                summary->idle_gaps.emplace_back(runs[i - 1].second,
                                                runs[i].first);
    } else if (nb > 0 && summary->bucket_us > 0) {
        // No app-spans (malformed trace): fall back to a lane-activity scan,
        // requiring a gap to be a fraction of active (not total) time.
        std::vector<bool> active(nb, false);
        for (const auto& lane : summary->lanes)
            for (std::size_t i = 0; i < nb; ++i)
                if (lane.cells[i].count) active[i] = true;
        std::size_t first = 0, last = 0, active_count = 0;
        bool any = false;
        for (std::size_t i = 0; i < nb; ++i)
            if (active[i]) {
                if (!any) {
                    first = i;
                    any = true;
                }
                last = i;
                ++active_count;
            }
        if (any) {
            const double active_us =
                static_cast<double>(active_count) * summary->bucket_us;
            const double min_gap_us =
                std::max(3.0 * summary->bucket_us, 0.05 * active_us);
            for (std::size_t i = first; i <= last;) {
                if (active[i]) {
                    ++i;
                    continue;
                }
                std::size_t j = i;
                while (j <= last && !active[j]) ++j;
                if (static_cast<double>(j - i) * summary->bucket_us >=
                    min_gap_us) {
                    auto g0 = summary->t_begin +
                              static_cast<std::uint64_t>(
                                  static_cast<double>(i) * summary->bucket_us);
                    auto g1 = summary->t_begin +
                              static_cast<std::uint64_t>(
                                  static_cast<double>(j) * summary->bucket_us);
                    summary->idle_gaps.emplace_back(g0, g1);
                }
                i = j;
            }
        }
    }

    DFTRACER_UTILS_LOG_INFO(
        "viz: built activity summary (%zu lanes, %zu buckets/lane, %zu idle "
        "gaps)",
        summary->lanes.size(), nb, summary->idle_gaps.size());
    index.set_viz_summary(std::move(summary));
}

// The summary, building it on first use. Null when another request is already
// building it, in which case the caller falls back to a live scan.
static coro::CoroTask<const VizSummary*> ensure_viz_summary(TraceIndex& index) {
    const VizSummary* s = index.viz_summary();
    if (!s && index.try_begin_summary_build()) {
        co_await build_viz_summary(index);
        s = index.viz_summary();
    }
    co_return s;
}

// One aggregate row, uniform over the live-scan and summary paths.
struct StatRow {
    const std::string* key;
    std::uint64_t count;
    double total;
    double min;
    double max;
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
    const double us =
        dftracer::utils::utilities::composites::dft::time_metric_us_scale(
            metric);
    if (us == 1.0) return;
    for (auto& r : rows) {
        r.total *= us;
        r.min *= us;
        r.max *= us;
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

// GET /api/v1/viz/stats: server-side per-name aggregation over a time range.
// Scans in parallel worker coroutines, each folding into its own map, then
// merges single-threaded (no lock). Returns only the small aggregate table.
static coro::CoroTask<HttpResponse> handle_viz_stats(const HttpRequest& req,
                                                     const QueryParams& params,
                                                     TraceIndex& index) {
    if (!params.has("begin") || !params.has("end")) {
        co_return HttpResponse::bad_request(
            "Missing required parameters: begin, end");
    }

    double begin = params.get_double("begin", 0);
    double end = params.get_double("end", 0);

    auto query = params.get("query");
    if (!query.empty() &&
        !utilities::common::query::try_parse(query).has_value()) {
        co_return HttpResponse::bad_request("Invalid query: " +
                                            std::string(query));
    }

    auto ts_norm_param = params.get("ts_normalize");
    bool normalize = ts_norm_param.empty() || ts_norm_param != "0";
    std::uint64_t global_min = 0;
    if (normalize) {
        global_min = index.global_min_timestamp_us();
        if (global_min == std::numeric_limits<std::uint64_t>::max())
            global_min = 0;
    }
    double original_begin = begin;
    double original_end = end;
    if (index.time_metric() != TraceIndex::TimeMetric::US) {
        begin = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(begin)));
        end = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(end)));
    }
    if (normalize && global_min > 0) {
        begin += static_cast<double>(global_min);
        end += static_cast<double>(global_min);
    }

    GroupBy group = parse_group_by(params.get("group"));

    // Whole-trace, unfiltered Analyze: answer from the prebuilt summary (built
    // lazily here). Concurrent builds fall through to the live scan below.
    if (viz_stats_summary_eligible(params, begin, end, index)) {
        const VizSummary* s = co_await ensure_viz_summary(index);
        if (s) {
            const auto& gr = summary_group_rows(*s, group);
            std::vector<StatRow> rows;
            rows.reserve(gr.size());
            std::uint64_t total_count = 0;
            double total_dur = 0;
            for (const auto& r : gr) {
                rows.push_back({&r.key, r.count, r.total, r.min, r.max});
                total_count += r.count;
                total_dur += r.total;
            }
            scale_stat_durations(rows, total_dur, index.time_metric());
            co_return HttpResponse::ok(serialize_stats_body(
                total_count, total_dur, original_end - original_begin, false,
                rows));
        }
    }

    // Full detail for stats: no min-duration threshold.
    ViewDefinition view = build_viz_view(params, begin, end, 0);

    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;

    std::vector<const TraceIndex::FileInfo*> target_files =
        select_viz_target_files(index, params, begin, end);

    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    std::vector<NameMap> partials(slots);
    auto aggregate = [&partials, group](
                         std::size_t w,
                         const std::vector<std::string_view>& events) {
        NameMap& local = partials[w];  // owned by worker w only, no lock
        for (auto ev : events) fold_event(ev, group, local);
    };

    bool truncated = co_await scan_view_events(
        index, target_files, view, begin, end, limit, slots, aggregate,
        req.cancel_token, !params.get("file").empty());

    // Single-threaded reduce of the disjoint per-worker maps.
    NameMap merged;
    for (auto& p : partials) {
        for (auto& kv : p) {
            auto it = merged.find(kv.first);
            if (it == merged.end())
                merged.emplace(kv.first, kv.second);
            else
                it->second.merge_from(kv.second);
        }
    }

    std::vector<StatRow> rows;
    rows.reserve(merged.size());
    std::uint64_t total_count = 0;
    double total_dur = 0;
    for (const auto& kv : merged) {
        rows.push_back({&kv.first, kv.second.count, kv.second.total,
                        kv.second.min, kv.second.max});
        total_count += kv.second.count;
        total_dur += kv.second.total;
    }
    std::sort(rows.begin(), rows.end(), [](const StatRow& a, const StatRow& b) {
        return a.total > b.total;
    });

    scale_stat_durations(rows, total_dur, index.time_metric());
    co_return HttpResponse::ok(
        serialize_stats_body(total_count, total_dur,
                             original_end - original_begin, truncated, rows));
}

// GET /api/v1/viz/layers: whole-trace reference data - the operation-name ->
// category map (a property of the name, not the view, so fetched once) plus the
// FH file counts: total declared vs. those an I/O event actually touched.
static coro::CoroTask<HttpResponse> handle_viz_layers(
    const HttpRequest& /*req*/, const QueryParams& /*p*/, TraceIndex& index) {
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

// One scanned event, reduced to what the call-tree needs.
struct FlameEv {
    std::int64_t pid = 0;
    std::int64_t tid = 0;
    double ts = 0;
    double dur = 0;
    std::string name;
};

// A node in the merged call tree: identical name-paths across all lanes fold
// into one node. `total` is inclusive; `self` is total minus nested children.
struct FlameNode {
    std::string name;
    double total = 0;
    double self = 0;
    std::uint64_t count = 0;
    dftracer::utils::StringViewMap<std::uint32_t> kids;
    std::vector<std::uint32_t> children;
};

static bool parse_flame_ev(std::string_view event, FlameEv& out) {
    thread_local simdjson::dom::parser parser;
    thread_local std::string buf;
    buf.assign(event);
    auto res = parser.parse(buf);
    if (res.error()) return false;
    auto root = res.value_unsafe();
    if (!root.is_object()) return false;
    auto dr = root["dur"];
    if (dr.error()) return false;  // no duration: nothing to place in the tree
    out.dur = json_number(dr.value_unsafe());
    auto tr = root["ts"];
    if (tr.error()) return false;
    out.ts = json_number(tr.value_unsafe());
    auto pr = root["pid"];
    out.pid = pr.error()
                  ? 0
                  : static_cast<std::int64_t>(json_number(pr.value_unsafe()));
    auto tir = root["tid"];
    out.tid = tir.error()
                  ? 0
                  : static_cast<std::int64_t>(json_number(tir.value_unsafe()));
    auto nr = root["name"];
    if (!nr.error() && nr.is_string())
        out.name.assign(nr.get_string().value_unsafe());
    else
        out.name.clear();
    return true;
}

static void serialize_flame_node(simdjson::builder::string_builder& sb,
                                 std::vector<FlameNode>& arena,
                                 std::uint32_t idx) {
    FlameNode& n = arena[idx];
    sb.start_object();
    sb.append_key_value("name", n.name);
    sb.append_comma();
    sb.append_key_value("total", n.total);
    sb.append_comma();
    sb.append_key_value("self", n.self < 0 ? 0.0 : n.self);
    sb.append_comma();
    sb.append_key_value("count", n.count);
    std::sort(n.children.begin(), n.children.end(),
              [&arena](std::uint32_t a, std::uint32_t b) {
                  return arena[a].total > arena[b].total;
              });
    sb.append_comma();
    sb.escape_and_append_with_quotes("children");
    sb.append_colon();
    sb.start_array();
    for (std::size_t i = 0; i < n.children.size(); ++i) {
        if (i > 0) sb.append_comma();
        serialize_flame_node(sb, arena, n.children[i]);
    }
    sb.end_array();
    sb.end_object();
}

// Fold one event under `base` in `arena` via the open-stack containment walk.
// kids are keyed by owned name copies (StringViewMap), so the source events may
// be discarded afterwards.
static void fold_flame_event(
    std::vector<FlameNode>& arena,
    std::vector<std::pair<double, std::uint32_t>>& open, std::uint32_t base,
    const FlameEv& ev) {
    double e_end = ev.ts + (ev.dur > 0 ? ev.dur : 0);
    while (!open.empty() && open.back().first <= ev.ts) open.pop_back();
    std::uint32_t parent = open.empty() ? base : open.back().second;
    std::uint32_t mi;
    auto it = arena[parent].kids.find(ev.name);
    if (it == arena[parent].kids.end()) {
        mi = static_cast<std::uint32_t>(arena.size());
        arena.emplace_back();
        arena[mi].name = ev.name;
        arena[parent].kids.emplace(ev.name, mi);
        arena[parent].children.push_back(mi);
    } else {
        mi = it->second;
    }
    arena[mi].total += ev.dur;
    arena[mi].self += ev.dur;
    arena[mi].count += 1;
    if (parent != 0) arena[parent].self -= ev.dur;
    open.push_back({e_end, mi});
}

// Sort one file's events by (pid,tid,ts,dur) and fold each lane into `arena`.
// Lanes never cross files (one pid per rank file), so this is a complete,
// self-contained partial tree for the file.
static void fold_file_events(
    std::vector<FlameNode>& arena,
    ankerl::unordered_dense::map<std::int64_t, std::uint32_t>& proc_of,
    std::vector<std::pair<double, std::uint32_t>>& open,
    std::vector<FlameEv>& evs, bool by_process) {
    std::sort(evs.begin(), evs.end(), [](const FlameEv& a, const FlameEv& b) {
        if (a.pid != b.pid) return a.pid < b.pid;
        if (a.tid != b.tid) return a.tid < b.tid;
        if (a.ts != b.ts) return a.ts < b.ts;
        return a.dur > b.dur;
    });
    std::size_t i = 0;
    while (i < evs.size()) {
        std::int64_t pid = evs[i].pid, tid = evs[i].tid;
        std::uint32_t base = 0;
        if (by_process) {
            auto pit = proc_of.find(pid);
            if (pit == proc_of.end()) {
                base = static_cast<std::uint32_t>(arena.size());
                arena.emplace_back();
                arena[base].name = "P" + std::to_string(pid);
                proc_of.emplace(pid, base);
                arena[0].children.push_back(base);
                arena[0].kids.emplace(arena[base].name, base);
            } else {
                base = pit->second;
            }
        }
        open.clear();
        for (; i < evs.size() && evs[i].pid == pid && evs[i].tid == tid; ++i)
            fold_flame_event(arena, open, base, evs[i]);
    }
}

// Merge partial tree `src` (subtree si) into `dst` (node di), summing stats per
// name-path. src node names stay alive for the whole merge.
static void merge_flame_arena(std::vector<FlameNode>& dst, std::uint32_t di,
                              const std::vector<FlameNode>& src,
                              std::uint32_t si) {
    dst[di].total += src[si].total;
    dst[di].self += src[si].self;
    dst[di].count += src[si].count;
    for (std::uint32_t sc : src[si].children) {
        std::uint32_t dc;
        auto it = dst[di].kids.find(src[sc].name);
        if (it == dst[di].kids.end()) {
            dc = static_cast<std::uint32_t>(dst.size());
            dst.emplace_back();
            dst[dc].name = src[sc].name;
            dst[di].kids.emplace(src[sc].name, dc);
            dst[di].children.push_back(dc);
        } else {
            dc = it->second;
        }
        merge_flame_arena(dst, dc, src, sc);
    }
}

// Streaming call-tree worker: claims whole files (work-stealing via
// `next_file`) and folds each file's events into `out` as a partial tree,
// discarding events per file so peak memory is one file's events, not the whole
// trace. Heavy locals are heap-allocated to keep the coroutine frame small.
static coro::CoroTask<void> calltree_stream_worker(
    const std::vector<const TraceIndex::FileInfo*>* files,
    std::atomic<std::size_t>* next_file, TraceIndex* index,
    const ViewDefinition* view, double begin, double end, bool scan_all_chunks,
    bool by_process, std::int64_t cap, std::atomic<std::int64_t>* produced,
    CancelToken cancel, std::vector<FlameNode>* out) {
    auto& arena = *out;
    arena.emplace_back();  // partial root (index 0)
    arena[0].name = "all";
    auto file_buf = std::make_unique<std::vector<FlameEv>>();
    auto open =
        std::make_unique<std::vector<std::pair<double, std::uint32_t>>>();
    auto proc_of = std::make_unique<
        ankerl::unordered_dense::map<std::int64_t, std::uint32_t>>();

    while (true) {
        if (cancel.cancelled()) co_return;
        if (produced->load(std::memory_order_relaxed) >= cap) co_return;
        std::size_t fi = next_file->fetch_add(1, std::memory_order_relaxed);
        if (fi >= files->size()) co_return;
        auto* file_info = (*files)[fi];
        if (file_info->uncompressed_size == 0 &&
            file_info->num_checkpoints == 0)
            continue;

        ViewBuilderInput builder_input;
        builder_input.with_view(*view)
            .with_file_path(file_info->path)
            .with_index_path(file_info->has_bloom_data ? file_info->index_path
                                                       : "")
            .with_uncompressed_size(file_info->uncompressed_size)
            .with_num_checkpoints(file_info->num_checkpoints)
            .with_bloom_cache(&index->bloom_cache())
            .with_time_range(begin, end)
            .with_scan_all_chunks(scan_all_chunks);
        ViewBuilderUtility builder;
        auto build_output = co_await builder.process(builder_input);
        if (!build_output || !build_output->file_may_match) continue;

        file_buf->clear();
        for (const auto& c : build_output->candidates) {
            if (cancel.cancelled()) co_return;
            ViewReaderInput reader_input;
            reader_input.with_file_path(file_info->path)
                .with_index_path(file_info->index_path)
                .with_byte_range(c.start_byte, c.end_byte)
                .with_checkpoint_idx(c.checkpoint_idx)
                .with_view(*view);
            ViewReaderUtility reader;
            auto gen = reader.process(reader_input);
            while (auto batch = co_await gen.next()) {
                FlameEv ev;
                for (auto e : batch->events)
                    if (parse_flame_ev(e, ev)) file_buf->push_back(ev);
            }
        }

        fold_file_events(arena, *proc_of, *open, *file_buf, by_process);
        produced->fetch_add(static_cast<std::int64_t>(file_buf->size()),
                            std::memory_order_relaxed);
        file_buf->clear();
    }
}

// GET /api/v1/viz/calltree: merge events into a flamegraph tree. The hierarchy
// per pid/tid lane comes from ts/dur containment (same nesting the timeline
// draws); identical name-paths fold together across the whole trace.
static coro::CoroTask<HttpResponse> handle_viz_calltree(
    const HttpRequest& req, const QueryParams& params, TraceIndex& index) {
    if (!params.has("begin") || !params.has("end"))
        co_return HttpResponse::bad_request(
            "Missing required parameters: begin, end");

    double begin = params.get_double("begin", 0);
    double end = params.get_double("end", 0);

    auto query = params.get("query");
    if (!query.empty() &&
        !utilities::common::query::try_parse(query).has_value())
        co_return HttpResponse::bad_request("Invalid query: " +
                                            std::string(query));

    auto ts_norm_param = params.get("ts_normalize");
    bool normalize = ts_norm_param.empty() || ts_norm_param != "0";
    std::uint64_t global_min = 0;
    if (normalize) {
        global_min = index.global_min_timestamp_us();
        if (global_min == std::numeric_limits<std::uint64_t>::max())
            global_min = 0;
    }
    if (index.time_metric() != TraceIndex::TimeMetric::US) {
        begin = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(begin)));
        end = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(end)));
    }
    if (normalize && global_min > 0) {
        begin += static_cast<double>(global_min);
        end += static_cast<double>(global_min);
    }

    ViewDefinition view = build_viz_view(params, begin, end, 0);
    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;

    std::vector<const TraceIndex::FileInfo*> target_files =
        select_viz_target_files(index, params, begin, end);

    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    bool by_process = params.get("group") == "pid";
    bool scan_all_chunks = !params.get("file").empty();
    const std::int64_t cap =
        limit > 0 ? limit : std::numeric_limits<std::int64_t>::max();

    static constexpr const char* CANCELLED_TREE =
        R"({"truncated":true,"tree":{"name":"all","total":0,"self":0,"count":0,"children":[]}})";

    // Stream the tree: each worker claims whole files and folds them into its
    // own partial tree, discarding events per file (bounded memory), then the
    // partials merge. Lanes never cross files, so each partial is complete.
    std::size_t nworkers = std::max<std::size_t>(
        1, std::min(slots, target_files.empty() ? std::size_t{1}
                                                : target_files.size()));
    std::vector<std::vector<FlameNode>> arenas(nworkers);
    std::atomic<std::size_t> next_file{0};
    std::atomic<std::int64_t> produced{0};
    CancelToken cancel = req.cancel_token;

    {
        CoroScope scope(Executor::current());
        auto* files_ptr = &target_files;
        auto* index_ptr = &index;
        auto* view_ptr = &view;
        auto* next_ptr = &next_file;
        auto* produced_ptr = &produced;
        for (std::size_t w = 0; w < nworkers; ++w) {
            auto* out = &arenas[w];
            scope.spawn([files_ptr, next_ptr, index_ptr, view_ptr, begin, end,
                         scan_all_chunks, by_process, cap, produced_ptr, cancel,
                         out](CoroScope&) -> coro::CoroTask<void> {
                co_await calltree_stream_worker(files_ptr, next_ptr, index_ptr,
                                                view_ptr, begin, end,
                                                scan_all_chunks, by_process,
                                                cap, produced_ptr, cancel, out);
            });
        }
        co_await scope.join();
    }

    if (cancel.cancelled()) co_return HttpResponse::ok(CANCELLED_TREE);
    bool truncated = limit > 0 && produced.load() >= cap;

    for (std::size_t t = 1; t < arenas.size(); ++t)
        merge_flame_arena(arenas[0], 0, arenas[t], 0);
    std::vector<FlameNode> arena = std::move(arenas[0]);

    // Process frames are synthetic containers: total/count roll up from their
    // children, self is 0.
    if (by_process) {
        for (std::uint32_t pnode : arena[0].children) {
            double t = 0;
            std::uint64_t c = 0;
            for (std::uint32_t ch : arena[pnode].children) {
                t += arena[ch].total;
                c += arena[ch].count;
            }
            arena[pnode].total = t;
            arena[pnode].count = c;
            arena[pnode].self = 0;
        }
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

    // Node total/self are summed native durations; scale to us for display.
    const double dur_us =
        dftracer::utils::utilities::composites::dft::time_metric_us_scale(
            index.time_metric());
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
    co_return HttpResponse::ok(std::string(b));
}

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

// GET /api/v1/viz/histogram: the distribution of event durations matching the
// query in [begin, end]. Collects each matching dur, then reports exact
// percentiles and a log-spaced histogram of the shape. The caller narrows to
// one operation by folding its predicate (name == "...") into the query.
static coro::CoroTask<HttpResponse> handle_viz_histogram(
    const HttpRequest& req, const QueryParams& params, TraceIndex& index) {
    if (!params.has("begin") || !params.has("end"))
        co_return HttpResponse::bad_request(
            "Missing required parameters: begin, end");

    double begin = params.get_double("begin", 0);
    double end = params.get_double("end", 0);

    auto query = params.get("query");
    if (!query.empty() &&
        !utilities::common::query::try_parse(query).has_value())
        co_return HttpResponse::bad_request("Invalid query: " +
                                            std::string(query));

    auto ts_norm_param = params.get("ts_normalize");
    bool normalize = ts_norm_param.empty() || ts_norm_param != "0";
    std::uint64_t global_min = 0;
    if (normalize) {
        global_min = index.global_min_timestamp_us();
        if (global_min == std::numeric_limits<std::uint64_t>::max())
            global_min = 0;
    }
    if (index.time_metric() != TraceIndex::TimeMetric::US) {
        begin = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(begin)));
        end = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(end)));
    }
    if (normalize && global_min > 0) {
        begin += static_cast<double>(global_min);
        end += static_cast<double>(global_min);
    }

    ViewDefinition view = build_viz_view(params, begin, end, 0);
    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;
    int nbuckets = params.get_int("buckets", 40);
    nbuckets = std::clamp(nbuckets, 4, 200);

    std::vector<const TraceIndex::FileInfo*> target_files =
        select_viz_target_files(index, params, begin, end);

    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    std::vector<std::vector<double>> partials(slots);
    auto collect = [&partials](std::size_t w,
                               const std::vector<std::string_view>& events) {
        thread_local simdjson::dom::parser parser;
        thread_local std::string buf;
        auto& out = partials[w];  // worker-owned slot, no lock
        for (auto e : events) {
            buf.assign(e);
            auto res = parser.parse(buf);
            if (res.error()) continue;
            auto root = res.value_unsafe();
            if (!root.is_object()) continue;
            auto dr = root["dur"];
            if (dr.error()) continue;
            out.push_back(json_number(dr.value_unsafe()));
        }
    };

    bool truncated = co_await scan_view_events(
        index, target_files, view, begin, end, limit, slots, collect,
        req.cancel_token, !params.get("file").empty());

    std::vector<double> all;
    std::size_t total_n = 0;
    for (auto& p : partials) total_n += p.size();
    all.reserve(total_n);
    for (auto& p : partials)
        for (double d : p) all.push_back(d);
    std::sort(all.begin(), all.end());

    // Durations are in the trace's native unit; scale to us for display.
    const double dur_us =
        dftracer::utils::utilities::composites::dft::time_metric_us_scale(
            index.time_metric());
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
        co_return HttpResponse::ok(std::string(sb));
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
    co_return HttpResponse::ok(std::string(sb));
}

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
    const double us =
        dftracer::utils::utilities::composites::dft::time_metric_us_scale(
            metric);
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
        blocks.push_back(
            {a.name, k.pid, k.tid,
             original_begin + static_cast<double>(k.col) * threshold_us,
             threshold_us, a.count, a.total * us, a.depth, k.group});
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
static bool viz_summary_eligible(const QueryParams& params) {
    return params.get("query").empty() && params.get("cat").empty() &&
           params.get("lanes").empty() && params.get("filters").empty() &&
           params.get("file").empty() && params.get("group_by").empty();
}

static coro::CoroTask<void> append_app_spans(std::vector<std::string>& out,
                                             TraceIndex& index, double begin,
                                             double end,
                                             const QueryParams& params) {
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

// Re-aggregate the summary's finest per-lane buckets over [begin_abs, end_abs]
// into pixel-column density blocks. `begin_abs`/`end_abs` are absolute us.
static std::string serve_density_from_summary(
    const VizSummary& s, const QueryParams& params, double begin_abs,
    double end_abs, double original_begin, double original_end,
    double threshold, bool ts_normalized, std::uint64_t display_global_min,
    TraceIndex::TimeMetric metric) {
    auto pid_s = params.get("pid");
    auto tid_s = params.get("tid");
    bool has_pid = !pid_s.empty();
    bool has_tid = !tid_s.empty();
    std::int64_t want_pid =
        has_pid ? std::strtoll(pid_s.data(), nullptr, 10) : 0;
    std::int64_t want_tid =
        has_tid ? std::strtoll(tid_s.data(), nullptr, 10) : 0;

    std::int64_t b0 = s.bucket_of(begin_abs);
    std::int64_t b1 = s.bucket_of(end_abs);
    if (b0 < 0) b0 = 0;
    if (b1 < 0) b1 = static_cast<std::int64_t>(s.nbuckets) - 1;

    DensityMap dens;
    for (const auto& lane : s.lanes) {
        if (has_pid && lane.pid != want_pid) continue;
        if (has_tid && lane.tid != want_tid) continue;
        for (std::int64_t bk = b0; bk <= b1; ++bk) {
            const auto& cell = lane.cells[static_cast<std::size_t>(bk)];
            if (cell.count == 0) continue;
            double center = static_cast<double>(s.t_begin) +
                            (static_cast<double>(bk) + 0.5) * s.bucket_us;
            std::int64_t col =
                static_cast<std::int64_t>((center - begin_abs) / threshold);
            DensityKey key{lane.pid, lane.tid, col, {}};
            auto& a = dens[key];
            a.count += cell.count;
            a.total += static_cast<double>(cell.total);
            if (static_cast<double>(cell.max_dur) > a.max_dur) {
                a.max_dur = static_cast<double>(cell.max_dur);
                if (cell.name_id < s.names.size())
                    a.name = s.names[cell.name_id];
            }
        }
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
                ? normalize_event_ts(sp.json, display_global_min, metric)
                : sp.json);
    }
    // App spans get an injected depth 0; long events after them do not, so the
    // client stacks overlapping wide events instead of piling them on one row.
    const std::size_t napp_spans = spans.size();
    for (const auto& sp : s.long_events) {
        if (has_pid && sp.pid != want_pid) continue;
        if (has_tid && sp.tid != want_tid) continue;
        if (static_cast<double>(sp.end) <= begin_abs ||
            static_cast<double>(sp.begin) >= end_abs)
            continue;
        span_lanes.insert((sp.pid << 20) ^ sp.tid);
        spans.push_back(
            ts_normalized && display_global_min > 0
                ? normalize_event_ts(sp.json, display_global_min, metric)
                : sp.json);
    }
    // Folded child activity sits one row below its app span, matching the
    // containment nesting the live path computes.
    if (!span_lanes.empty())
        for (auto& [k, a] : dens)
            if (span_lanes.count((k.pid << 20) ^ k.tid)) a.depth = 1;

    std::vector<std::uint32_t> span_depth(napp_spans, 0);
    return serialize_density_body(
        spans, dens, original_begin, original_end, threshold, 0, false,
        ts_normalized, display_global_min, static_cast<double>(s.max_dur),
        metric, &span_depth);
}

// GET /api/v1/viz/density: like /viz/events, but instead of dropping sub-pixel
// events it buckets them per (pid, tid, pixel-column) into density blocks so
// zoomed-out views still show where activity is. Returns full-size events (with
// args, for the detail panel) plus the aggregated density blocks.
static coro::CoroTask<HttpResponse> handle_viz_density(
    const HttpRequest& req, const QueryParams& params, TraceIndex& index) {
    if (!params.has("begin") || !params.has("end")) {
        co_return HttpResponse::bad_request(
            "Missing required parameters: begin, end");
    }

    double begin = params.get_double("begin", 0);
    double end = params.get_double("end", 0);
    int summary = params.get_int("summary", 2);
    if (summary < 1) summary = 1;

    auto query = params.get("query");
    if (!query.empty() &&
        !utilities::common::query::try_parse(query).has_value()) {
        co_return HttpResponse::bad_request("Invalid query: " +
                                            std::string(query));
    }

    // Optional density grouping column. A well-formed name that matches no
    // event field is fine (all blocks land in the client's "(none)" group);
    // only malformed names are rejected.
    std::string group_col(params.get("group_by"));
    if (group_col.size() > 64)
        co_return HttpResponse::bad_request("group_by too long");
    for (char c : group_col) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' &&
            c != '.' && c != '-')
            co_return HttpResponse::bad_request("Invalid group_by column");
    }

    // Hash columns auto-resolve for display: group values stay raw hashes, and
    // a group_names map (hash -> resolved name) rides along in the metadata.
    // resolved.*/r.* aliases from the query DSL map to their hash columns.
    using HashType = utilities::indexer::IndexDatabase::HashType;
    std::optional<HashType> resolve_type;
    {
        // "args.fhash" behaves like "fhash": bare names fall back into args
        // during extraction, and the resolve mapping matches either spelling.
        if (group_col.rfind("args.", 0) == 0 &&
            group_col.find('.', 5) == std::string::npos)
            group_col = group_col.substr(5);
        auto is = [&](std::string_view a) { return group_col == a; };
        if (is("resolved.fpath") || is("r.fpath"))
            group_col = "fhash";
        else if (is("resolved.cwd") || is("r.cwd"))
            group_col = "cwd";
        else if (is("resolved.hostname") || is("r.hostname") ||
                 is("resolved.host") || is("r.host"))
            group_col = "hhash";
        else if (is("resolved.exec") || is("r.exec"))
            group_col = "exec_hash";
        else if (is("resolved.cmd") || is("r.cmd"))
            group_col = "cmd_hash";
        if (group_col == "fhash" || group_col == "cwd")
            resolve_type = HashType::FILE;
        else if (group_col == "hhash")
            resolve_type = HashType::HOST;
        else if (group_col == "shash" || group_col == "exec_hash" ||
                 group_col == "cmd_hash")
            resolve_type = HashType::STRING;
    }

    auto ts_norm_param = params.get("ts_normalize");
    bool normalize = ts_norm_param.empty() || ts_norm_param != "0";
    std::uint64_t global_min = 0;
    if (normalize) {
        global_min = index.global_min_timestamp_us();
        if (global_min == std::numeric_limits<std::uint64_t>::max())
            global_min = 0;
    }
    double original_begin = begin;
    double original_end = end;
    if (index.time_metric() != TraceIndex::TimeMetric::US) {
        begin = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(begin)));
        end = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(end)));
    }
    if (normalize && global_min > 0) {
        begin += static_cast<double>(global_min);
        end += static_cast<double>(global_min);
    }

    // Bucket width (== the ~1px cutoff), in the client's actual pixels.
    int width = params.get_int("width", DEFAULT_VIEWPORT_WIDTH);
    width = std::clamp(width, MIN_VIEWPORT_WIDTH, MAX_VIEWPORT_WIDTH);
    double threshold =
        duration_threshold(begin, end, static_cast<unsigned>(summary),
                           static_cast<unsigned>(width));

    // Zoomed-out, unfiltered views come from the prebuilt summary (no event
    // cap), built lazily here; concurrent requests fall through to a live scan.
    if (threshold > 0 && viz_summary_eligible(params)) {
        const VizSummary* s = co_await ensure_viz_summary(index);
        if (s && s->bucket_us > 0 && s->t_end > s->t_begin &&
            threshold >= s->bucket_us) {
            co_return HttpResponse::ok(serve_density_from_summary(
                *s, params, begin, end, original_begin, original_end, threshold,
                global_min > 0,
                index.native_to_us(index.global_min_timestamp_us()),
                index.time_metric()));
        }
    }

    // summary=1 means "no aggregation", but the viewport is still finite.
    // Fold sub-pixel events into density blocks instead of dropping them.
    if (threshold <= 0 && end > begin)
        threshold = (end - begin) / static_cast<double>(width);

    // Enclosing events are fetched by a separate pass (below) so a large
    // `lookback` can never starve the in-window scan's budget.
    double lookback = params.get_double("lookback", 0);
    if (lookback < 0) lookback = 0;
    double scan_begin = begin - lookback;
    if (scan_begin < 0) scan_begin = 0;

    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;

    struct Acc {
        std::vector<std::string> big;
        std::vector<double> big_dur;  // parallel to `big`
        DensityMap dens;
        double max_dur = 0;
    };
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    std::vector<Acc> accs(slots);

    // Pass 1 (in-window): scan [begin, end] with the full budget so earlier
    // events never consume it. Small events fold into density blocks.
    auto on_batch = [&accs, &group_col, threshold, begin](
                        std::size_t w,
                        const std::vector<std::string_view>& events) {
        Acc& acc = accs[w];  // worker-owned slot, no lock
        for (auto ev : events) {
            double dur = 0;
            if (!fold_density(ev, threshold, begin, acc.dens, &dur,
                              group_col)) {
                acc.big.emplace_back(ev);
                acc.big_dur.push_back(dur);
            }
            if (dur > acc.max_dur) acc.max_dur = dur;
        }
    };
    ViewDefinition win_view = build_viz_view(params, begin, end, 0);
    std::vector<const TraceIndex::FileInfo*> win_files =
        select_viz_target_files(index, params, begin, end);
    bool single_file = !params.get("file").empty();
    bool truncated = co_await scan_view_events(
        index, win_files, win_view, begin, end, limit, slots, on_batch,
        req.cancel_token, single_file);

    // Pass 2 (enclosers): keep only events still open at `begin`
    // (ts < begin <= ts + dur); these ancestor bars set containment depth.
    if (scan_begin < begin) {
        auto on_batch_enc = [&accs, begin](
                                std::size_t w,
                                const std::vector<std::string_view>& events) {
            Acc& acc = accs[w];
            for (auto ev : events) {
                double ts = 0, dur = 0;
                if (!parse_ts_dur(ev, ts, dur)) continue;
                if (dur > acc.max_dur) acc.max_dur = dur;
                if (ts < begin && ts + dur > begin) {
                    acc.big.emplace_back(ev);
                    acc.big_dur.push_back(dur);
                }
            }
        };
        ViewDefinition enc_view = build_viz_view(params, scan_begin, begin, 0);
        std::vector<const TraceIndex::FileInfo*> enc_files =
            select_viz_target_files(index, params, scan_begin, begin);
        bool enc_trunc = co_await scan_view_events(
            index, enc_files, enc_view, scan_begin, begin, limit, slots,
            on_batch_enc, req.cancel_token, single_file);
        truncated = truncated || enc_trunc;
    }

    std::vector<std::string> big;
    std::vector<double> big_dur;
    DensityMap dens;
    // Longest event scanned (folded ones included); the client feeds it back as
    // `lookback` so deep zooms still catch long enclosing events.
    double max_dur = 0;
    for (auto& acc : accs) {
        if (acc.max_dur > max_dur) max_dur = acc.max_dur;
        for (auto& d : acc.big_dur) big_dur.push_back(d);
        for (auto& s : acc.big) big.emplace_back(std::move(s));
        for (auto& kv : acc.dens) {
            auto it = dens.find(kv.first);
            if (it == dens.end())
                dens.emplace(kv.first, std::move(kv.second));
            else
                it->second.merge_from(kv.second);
        }
    }
    // scan_view_events overshoots its cap (checked per batch), so clamp here.
    // Keep the longest events: those are the slices wide enough to see.
    if (limit > 0 && big.size() > static_cast<std::size_t>(limit)) {
        const auto keep = static_cast<std::size_t>(limit);
        std::vector<std::size_t> idx(big.size());
        std::iota(idx.begin(), idx.end(), std::size_t{0});
        std::nth_element(idx.begin(), idx.begin() + static_cast<long>(keep),
                         idx.end(), [&big_dur](std::size_t a, std::size_t b) {
                             return big_dur[a] > big_dur[b];
                         });
        idx.resize(keep);
        std::sort(idx.begin(), idx.end());
        std::vector<std::string> kept;
        kept.reserve(keep);
        for (std::size_t i : idx) kept.emplace_back(std::move(big[i]));
        big.swap(kept);
        truncated = true;
    }

    co_await append_app_spans(big, index, begin, end, params);

    // Resolve hash group values (fhash -> path, hhash -> host, ...) for the
    // groups actually present; unresolvable hashes fall back to the raw value
    // on the client.
    ankerl::unordered_dense::map<std::string, std::string> group_names;
    if (resolve_type && !group_col.empty()) {
        ankerl::unordered_dense::set<std::string> raw_groups;
        for (const auto& [k, a] : dens)
            if (!k.group.empty()) raw_groups.insert(k.group);
        for (const auto& e : big) {
            auto g = extract_group_from_line(e, group_col);
            if (!g.empty()) raw_groups.insert(std::move(g));
        }
        if (!raw_groups.empty()) {
            try {
                ankerl::unordered_dense::set<std::string> roots;
                for (const auto* f : win_files)
                    if (!f->index_path.empty()) roots.insert(f->index_path);
                for (const auto& r : roots) {
                    utilities::indexer::IndexDatabase db(
                        r, rocksdb::RocksDatabase::OpenMode::ReadOnly);
                    auto table = db.query_hash_table(*resolve_type);
                    for (const auto& g : raw_groups) {
                        auto it = table.find(g);
                        if (it != table.end())
                            group_names.emplace(g, it->second);
                    }
                }
            } catch (...) {
                // No usable hash table: raw hashes still group correctly.
            }
        }
    }

    // Stable per-event/-block depth (computed in absolute space, before ts
    // normalization rewrites the big strings; order is preserved in place).
    std::vector<std::uint32_t> big_depth =
        assign_view_depths(big, dens, begin, threshold);
    if (global_min > 0 || index.time_metric() != TraceIndex::TimeMetric::US) {
        for (auto& e : big)
            e = normalize_event_ts(e, global_min, index.time_metric());
    }

    co_return HttpResponse::ok(serialize_density_body(
        big, dens, original_begin, original_end, threshold, limit, truncated,
        global_min > 0, index.native_to_us(index.global_min_timestamp_us()),
        max_dur, index.time_metric(), &big_depth,
        group_names.empty() ? nullptr : &group_names));
}

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
static std::string serve_counters_from_summary(const VizSummary& s,
                                               double begin_abs, double end_abs,
                                               double original_begin,
                                               double original_end, int buckets,
                                               double bucket_us_out,
                                               TraceIndex::TimeMetric metric) {
    std::vector<double> read(buckets, 0.0), write(buckets, 0.0),
        ops(buckets, 0.0);
    std::int64_t fb0 = s.bucket_of(begin_abs);
    std::int64_t fb1 = s.bucket_of(end_abs);
    if (fb0 < 0) fb0 = 0;
    if (fb1 < 0) fb1 = static_cast<std::int64_t>(s.nbuckets) - 1;
    for (std::int64_t fb = fb0; fb <= fb1; ++fb) {
        double center = static_cast<double>(s.t_begin) +
                        (static_cast<double>(fb) + 0.5) * s.bucket_us;
        long oi = static_cast<long>((center - begin_abs) / bucket_us_out);
        if (oi < 0 || oi >= buckets) continue;
        auto f = static_cast<std::size_t>(fb);
        read[oi] += s.read_bytes[f];
        write[oi] += s.write_bytes[f];
        ops[oi] += s.ops[f];
    }
    return serialize_counters_body(
        read, write, ops, original_begin, original_end, buckets,
        bucket_us_out *
            dftracer::utils::utilities::composites::dft::time_metric_us_scale(
                metric),
        false);
}

// GET /api/v1/viz/counters: per-bucket read/write bytes and I/O op counts over
// a time range, for bandwidth/IOPS counter tracks. Aggregated server-side in
// parallel (per-worker arrays merged after join).
static coro::CoroTask<HttpResponse> handle_viz_counters(
    const HttpRequest& req, const QueryParams& params, TraceIndex& index) {
    if (!params.has("begin") || !params.has("end")) {
        co_return HttpResponse::bad_request(
            "Missing required parameters: begin, end");
    }

    double begin = params.get_double("begin", 0);
    double end = params.get_double("end", 0);
    int buckets = params.get_int("buckets", 800);
    if (buckets < 16) buckets = 16;
    if (buckets > 4000) buckets = 4000;

    auto query = params.get("query");
    if (!query.empty() &&
        !utilities::common::query::try_parse(query).has_value()) {
        co_return HttpResponse::bad_request("Invalid query: " +
                                            std::string(query));
    }

    auto ts_norm_param = params.get("ts_normalize");
    bool normalize = ts_norm_param.empty() || ts_norm_param != "0";
    std::uint64_t global_min = 0;
    if (normalize) {
        global_min = index.global_min_timestamp_us();
        if (global_min == std::numeric_limits<std::uint64_t>::max())
            global_min = 0;
    }
    double original_begin = begin;
    double original_end = end;
    if (index.time_metric() != TraceIndex::TimeMetric::US) {
        begin = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(begin)));
        end = static_cast<double>(
            index.us_to_native(static_cast<std::uint64_t>(end)));
    }
    if (normalize && global_min > 0) {
        begin += static_cast<double>(global_min);
        end += static_cast<double>(global_min);
    }

    double bucket_us = (end - begin) / static_cast<double>(buckets);
    if (bucket_us <= 0) bucket_us = 1;

    // Zoomed-out, unfiltered counter tracks come from the activity summary.
    if (viz_summary_eligible(params)) {
        const VizSummary* s = co_await ensure_viz_summary(index);
        if (s && s->bucket_us > 0 && s->t_end > s->t_begin &&
            bucket_us >= s->bucket_us) {
            co_return HttpResponse::ok(serve_counters_from_summary(
                *s, begin, end, original_begin, original_end, buckets,
                bucket_us, index.time_metric()));
        }
    }

    ViewDefinition view = build_viz_view(params, begin, end, 0);
    // No cap: only zoomed-in or filtered counter queries reach the live path.
    int limit = params.get_int("limit", 0);
    if (limit < 0) limit = 0;

    std::vector<const TraceIndex::FileInfo*> target_files =
        select_viz_target_files(index, params, begin, end);

    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    std::vector<CounterAcc> accs(slots);
    for (auto& a : accs) a.init(static_cast<std::size_t>(buckets));
    auto on_batch = [&accs, begin, bucket_us, buckets](
                        std::size_t w,
                        const std::vector<std::string_view>& events) {
        CounterAcc& acc = accs[w];  // worker-owned, no lock
        for (auto ev : events)
            fold_counter(ev, begin, bucket_us,
                         static_cast<std::size_t>(buckets), acc);
    };

    bool truncated = co_await scan_view_events(
        index, target_files, view, begin, end, limit, slots, on_batch,
        req.cancel_token, !params.get("file").empty());

    CounterAcc total;
    total.init(static_cast<std::size_t>(buckets));
    for (auto& a : accs) total.merge_from(a);

    co_return HttpResponse::ok(serialize_counters_body(
        total.read_bytes, total.write_bytes, total.ops, original_begin,
        original_end, buckets,
        bucket_us *
            dftracer::utils::utilities::composites::dft::time_metric_us_scale(
                index.time_metric()),
        truncated));
}

// Process-spawning calls: dftracer POSIX (exact "fork"/"clone"/...) and kernel
// syscalls ("__arm64_sys_clone"). Excludes library helpers like ibv_*fork* and
// register_tm_clones, which contain "fork"/"clone" but don't spawn.
static bool is_fork_syscall(std::string_view name) {
    return name == "fork" || name == "vfork" || name == "clone" ||
           name == "clone3" || name == "posix_spawn" ||
           name == "posix_spawnp" ||
           name.find("sys_clone") != std::string_view::npos ||
           name.find("sys_fork") != std::string_view::npos ||
           name.find("sys_vfork") != std::string_view::npos;
}

// One process in the proctree response. `host` borrows the hostname table;
// `rank` is null when the trace carries no "PR" metadata for the pid, and the
// key is then omitted.
struct ProcNode {
    std::int64_t pid;
    std::int64_t parent;
    std::uint64_t spawn_ts;
    std::uint64_t first_ts;
    std::string_view host;
    std::uint64_t bytes;
    std::uint64_t io_ops;
    double io_busy;
    const std::string* rank;
};

template <typename builder_type>
void tag_invoke(simdjson::serialize_tag, builder_type& b, const ProcNode& n) {
    b.start_object();
    b.append_key_value("pid", n.pid);
    b.append_comma();
    b.append_key_value("parent", n.parent);
    b.append_comma();
    b.append_key_value("spawn_ts", n.spawn_ts);
    b.append_comma();
    b.append_key_value("first_ts", n.first_ts);
    b.append_comma();
    b.append_key_value("host", n.host);
    b.append_comma();
    b.append_key_value("bytes", n.bytes);
    b.append_comma();
    b.append_key_value("io_ops", n.io_ops);
    b.append_comma();
    b.append_key_value("io_busy", n.io_busy);
    if (n.rank) {
        b.append_comma();
        b.append_key_value("rank", *n.rank);
    }
    b.end_object();
}

// GET /api/v1/viz/proctree: infer the process fork hierarchy. The traces record
// the fork/clone in the parent but not the child pid, so link each process to
// the nearest preceding clone in another process (child start follows the clone
// by microseconds). Respects ?file= for per-node trees on multi-node traces.
static coro::CoroTask<HttpResponse> handle_viz_proctree(
    const HttpRequest& req, const QueryParams& params, TraceIndex& index) {
    std::uint64_t gmin = index.global_min_timestamp_us();
    std::uint64_t gmax = index.global_max_timestamp_us();
    if (gmin == std::numeric_limits<std::uint64_t>::max() || gmax <= gmin)
        co_return HttpResponse::ok("{\"nodes\":[]}");

    auto ts_norm_param = params.get("ts_normalize");
    bool normalize = ts_norm_param.empty() || ts_norm_param != "0";
    std::uint64_t base = normalize ? gmin : 0;

    struct Acc {
        ankerl::unordered_dense::map<std::int64_t, std::uint64_t> first_ts;
        // Explicit parent from the process metadata's args.ppid.
        ankerl::unordered_dense::map<std::int64_t, std::int64_t> ppid;
        // (ts, parent_pid, child_pid): child_pid is args.ret when the fork
        // event records it (dftracer POSIX), else -1 to fall back to inference.
        std::vector<std::tuple<std::uint64_t, std::int64_t, std::int64_t>>
            forks;
        ankerl::unordered_dense::map<std::int64_t, std::uint64_t> bytes;
        // I/O operation count and busy time (us) per process, over I/O-category
        // (POSIX/STDIO/IO) events only.
        ankerl::unordered_dense::map<std::int64_t, std::uint64_t> io_ops;
        ankerl::unordered_dense::map<std::int64_t, double> io_busy;
        ankerl::unordered_dense::map<std::int64_t, std::string> pid_hhash;
        // pid -> rank, from "PR" metadata (args.name == "rank").
        ankerl::unordered_dense::map<std::int64_t, std::string> rank;
        dftracer::utils::StringViewMap<std::string> hh;
    };
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    std::vector<Acc> accs(slots);
    auto on_batch = [&accs](std::size_t w,
                            const std::vector<std::string_view>& events) {
        thread_local simdjson::dom::parser parser;
        thread_local std::string buf;
        Acc& acc = accs[w];
        for (auto ev : events) {
            buf.assign(ev);
            auto res = parser.parse(buf);
            if (res.error()) continue;
            auto root = res.value_unsafe();
            if (!root.is_object()) continue;
            // HH metadata (hhash -> hostname) carries no ts; handle it first.
            {
                auto nm = root["name"];
                if (!nm.error() && nm.is_string() &&
                    nm.get_string().value_unsafe() == "HH") {
                    auto a = root["args"];
                    if (!a.error() && a.is_object()) {
                        auto v = a["value"];
                        auto n = a["name"];
                        if (!v.error() && v.is_string() && !n.error() &&
                            n.is_string())
                            acc.hh.emplace(
                                std::string(v.get_string().value_unsafe()),
                                std::string(n.get_string().value_unsafe()));
                    }
                    continue;
                }
            }
            // PR metadata (pid -> rank) also carries no ts.
            {
                auto nm = root["name"];
                if (!nm.error() && nm.is_string() &&
                    nm.get_string().value_unsafe() == "PR") {
                    auto a = root["args"];
                    auto pp = root["pid"];
                    if (!a.error() && a.is_object() && !pp.error()) {
                        auto an = a["name"];
                        auto av = a["value"];
                        if (!an.error() && an.is_string() &&
                            an.get_string().value_unsafe() == "rank" &&
                            !av.error() && av.is_string())
                            acc.rank.emplace(
                                static_cast<std::int64_t>(
                                    json_number(pp.value_unsafe())),
                                std::string(av.get_string().value_unsafe()));
                    }
                    continue;
                }
            }
            auto pr = root["pid"];
            auto tr = root["ts"];
            if (pr.error() || tr.error()) continue;
            auto pid =
                static_cast<std::int64_t>(json_number(pr.value_unsafe()));
            auto ts =
                static_cast<std::uint64_t>(json_number(tr.value_unsafe()));
            auto it = acc.first_ts.find(pid);
            if (it == acc.first_ts.end())
                acc.first_ts.emplace(pid, ts);
            else if (ts < it->second)
                it->second = ts;

            auto nr = root["name"];
            std::string_view name;
            if (!nr.error() && nr.is_string())
                name = nr.get_string().value_unsafe();

            std::int64_t child = -1;
            double ret = 0;
            auto args = root["args"];
            if (!args.error() && args.is_object()) {
                auto ppr = args["ppid"];
                if (!ppr.error()) {
                    auto pp = static_cast<std::int64_t>(
                        json_number(ppr.value_unsafe()));
                    if (pp > 0 && pp != pid) acc.ppid[pid] = pp;
                }
                auto rr = args["ret"];
                if (!rr.error()) {
                    ret = json_number(rr.value_unsafe());
                    child = static_cast<std::int64_t>(ret);
                }
                auto hr = args["hhash"];
                if (!hr.error() && hr.is_string() &&
                    acc.pid_hhash.find(pid) == acc.pid_hhash.end())
                    acc.pid_hhash.emplace(
                        pid, std::string(hr.get_string().value_unsafe()));
            }
            // I/O bytes per process: args.ret on read/write ops.
            if (ret > 0 && (name.find("read") != std::string_view::npos ||
                            name.find("write") != std::string_view::npos))
                acc.bytes[pid] += static_cast<std::uint64_t>(ret);

            auto cr = root["cat"];
            std::string_view cat;
            if (!cr.error() && cr.is_string())
                cat = cr.get_string().value_unsafe();
            if (cat == "POSIX" || cat == "STDIO" || cat == "IO") {
                acc.io_ops[pid] += 1;
                auto dr = root["dur"];
                if (!dr.error())
                    acc.io_busy[pid] += json_number(dr.value_unsafe());
            }

            if (!name.empty() && is_fork_syscall(name))
                acc.forks.emplace_back(ts, pid, child > 0 ? child : -1);
        }
    };

    ViewDefinition view;
    view.name = "viz_proctree";
    view.with_query("ts >= " + std::to_string(gmin) +
                    " and ts <= " + std::to_string(gmax));
    auto files = select_viz_target_files(
        index, params, static_cast<double>(gmin), static_cast<double>(gmax));
    bool single_file = !params.get("file").empty();
    co_await scan_view_events(index, files, view, static_cast<double>(gmin),
                              static_cast<double>(gmax), 0, slots, on_batch,
                              req.cancel_token, single_file);

    ankerl::unordered_dense::map<std::int64_t, std::uint64_t> first_ts;
    ankerl::unordered_dense::map<std::int64_t, std::int64_t> parent_of;
    ankerl::unordered_dense::map<std::int64_t, std::uint64_t> spawn_of;
    std::vector<std::pair<std::uint64_t, std::int64_t>> inf_forks;  // (ts, pid)
    for (auto& a : accs) {
        for (auto& kv : a.first_ts) {
            auto it = first_ts.find(kv.first);
            if (it == first_ts.end())
                first_ts.emplace(kv.first, kv.second);
            else if (kv.second < it->second)
                it->second = kv.second;
        }
        // Explicit edges from the fork event's args.ret (child pid + spawn ts).
        for (auto& [ts, ppid, child] : a.forks) {
            if (child > 0) {
                parent_of[child] = ppid;
                spawn_of[child] = ts;
            } else {
                inf_forks.emplace_back(ts, ppid);
            }
        }
    }
    // args.ppid metadata fills in any process not linked by a fork event.
    for (auto& a : accs)
        for (auto& kv : a.ppid)
            if (parent_of.find(kv.first) == parent_of.end())
                parent_of.emplace(kv.first, kv.second);
    std::sort(inf_forks.begin(), inf_forks.end());

    // Merge host (hhash -> hostname resolved) and I/O bytes per process.
    dftracer::utils::StringViewMap<std::string> hh;
    ankerl::unordered_dense::map<std::int64_t, std::string> pid_hhash;
    ankerl::unordered_dense::map<std::int64_t, std::uint64_t> bytes;
    ankerl::unordered_dense::map<std::int64_t, std::uint64_t> io_ops;
    ankerl::unordered_dense::map<std::int64_t, double> io_busy;
    ankerl::unordered_dense::map<std::int64_t, std::string> rank;
    for (auto& a : accs) {
        for (auto& kv : a.hh) hh.emplace(kv.first, kv.second);
        for (auto& kv : a.pid_hhash) pid_hhash.emplace(kv.first, kv.second);
        for (auto& kv : a.bytes) bytes[kv.first] += kv.second;
        for (auto& kv : a.io_ops) io_ops[kv.first] += kv.second;
        for (auto& kv : a.io_busy) io_busy[kv.first] += kv.second;
        for (auto& kv : a.rank) rank.emplace(kv.first, kv.second);
    }

    std::vector<std::pair<std::uint64_t, std::int64_t>>
        procs;  // (first_ts, pid)
    procs.reserve(first_ts.size());
    for (auto& kv : first_ts) procs.emplace_back(kv.second, kv.first);
    std::sort(procs.begin(), procs.end());

    // Time-inference fallback (traces without args.ret/ppid): link a process to
    // the nearest preceding clone in another process.
    std::vector<bool> used(inf_forks.size(), false);
    std::vector<ProcNode> nodes;
    nodes.reserve(procs.size());
    const double proc_dur_us =
        dftracer::utils::utilities::composites::dft::time_metric_us_scale(
            index.time_metric());
    for (auto& [fts, pid] : procs) {
        std::int64_t parent = -1;
        std::uint64_t spawn_ts = 0;
        auto pit = parent_of.find(pid);
        auto sit = spawn_of.find(pid);
        // A fork cannot spawn a child that already existed before it: reject
        // such edges (pid reuse across runs) and fall back to time inference.
        bool valid = pit != parent_of.end() &&
                     (sit == spawn_of.end() || sit->second <= fts);
        if (valid) {
            parent = pit->second;
            if (sit != spawn_of.end()) spawn_ts = sit->second - base;
        } else {
            auto hi = std::upper_bound(
                inf_forks.begin(), inf_forks.end(),
                std::make_pair(fts, std::numeric_limits<std::int64_t>::max()));
            for (auto it = hi; it != inf_forks.begin();) {
                --it;
                auto idx = static_cast<std::size_t>(it - inf_forks.begin());
                if (!used[idx] && it->second != pid) {
                    used[idx] = true;
                    parent = it->second;
                    spawn_ts = it->first - base;
                    break;
                }
            }
        }
        std::string_view host;
        auto hp = pid_hhash.find(pid);
        if (hp != pid_hhash.end()) {
            auto hn = hh.find(hp->second);
            if (hn != hh.end()) host = hn->second;
        }
        auto bp = bytes.find(pid);
        auto op = io_ops.find(pid);
        auto ib = io_busy.find(pid);
        auto rk = rank.find(pid);
        nodes.push_back({pid, parent, index.native_to_us(spawn_ts),
                         index.native_to_us(fts - base), host,
                         bp != bytes.end() ? bp->second : 0,
                         op != io_ops.end() ? op->second : 0,
                         (ib != io_busy.end() ? ib->second : 0.0) * proc_dur_us,
                         rk != rank.end() ? &rk->second : nullptr});
    }

    auto& sb = scratch_json_builder();
    sb.start_object();
    sb.append_key_value("nodes", nodes);
    sb.end_object();
    co_return HttpResponse::ok(std::string(sb));
}

// GET /api/v1/viz/columns: the complete set of groupable columns in the trace
// (top-level scalar fields + args keys), harvested during the summary scan.
static coro::CoroTask<HttpResponse> handle_viz_columns(
    const HttpRequest& /*req*/, const QueryParams& /*params*/,
    TraceIndex& index) {
    // Prefer the durable set stored at index build (available immediately, no
    // scan). Fall back to the summary harvest for indexes built before column
    // discovery existed.
    std::vector<std::string> columns;
    bool ready = true;
    {
        ankerl::unordered_dense::set<std::string> roots;
        for (const auto& f : index.files())
            if (!f.index_path.empty()) roots.insert(f.index_path);
        ankerl::unordered_dense::set<std::string> merged;
        for (const auto& r : roots) {
            try {
                utilities::indexer::IndexDatabase db(
                    r, rocksdb::RocksDatabase::OpenMode::ReadOnly);
                for (auto& c : db.query_all_columns())
                    merged.emplace(std::move(c));
            } catch (...) {
            }
        }
        columns.assign(merged.begin(), merged.end());
        std::sort(columns.begin(), columns.end());
    }
    if (columns.empty()) {
        const VizSummary* s = co_await ensure_viz_summary(index);
        if (s) columns = s->columns;
        ready = s != nullptr;  // false => client retries after summary builds
    }

    auto& b = scratch_json_builder();
    b.start_object();
    b.escape_and_append_with_quotes("columns");
    b.append_colon();
    b.start_array();
    bool first = true;
    for (const auto& c : columns) {
        if (!first) b.append_comma();
        first = false;
        b.escape_and_append_with_quotes(c);
    }
    b.end_array();
    b.append_comma();
    b.append_key_value("ready", ready);
    b.end_object();
    co_return HttpResponse::ok(std::string(b));
}

static coro::CoroTask<HttpResponse> handle_viz_breaks(
    const HttpRequest& /*req*/, const QueryParams& params, TraceIndex& index) {
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

void register_viz_api(Router& router, TraceIndex& index) {
    auto* index_ptr = &index;
    const RouteParam BEGIN{"begin", "Window start (us)", true, "0"};
    const RouteParam END{"end", "Window end (us)", true, "999999999"};
    const RouteParam SUMMARY{"summary", "LOD level (1=full detail)", true, "1"};

    router.get(
        "/api/v1/viz/proctree",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_viz_proctree(req, params, *index_ptr);
        },
        RouteDoc{
            "Inferred process/fork hierarchy with host, rank, and I/O.",
            "Visualization",
            {{"file", "Limit to one trace file", false, ""}},
            R"({"nodes":[{"pid":100,"parent":-1,"host":"node01","rank":"0",)"
            R"("bytes":16384,"io_ops":4,"io_busy":600.0}]})"});

    router.get(
        "/api/v1/viz/counters",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_viz_counters(req, params, *index_ptr);
        },
        RouteDoc{"Read/write bytes and I/O op counts per time bucket.",
                 "Visualization",
                 {BEGIN, END, SUMMARY},
                 R"({"buckets":[{"ts":0,"read_bytes":4096,"write_bytes":0,)"
                 R"("read_ops":1,"write_ops":0}]})"});

    router.get(
        "/api/v1/viz/breaks",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_viz_breaks(req, params, *index_ptr);
        },
        RouteDoc{
            "Globally-idle time gaps and multi-run detection.",
            "Visualization",
            {{"ts_normalize", "Normalize to global min (default on)", false,
              "1"}},
            R"({"gaps":[{"begin":50000,"end":900000}],"multi_run":true})"});

    router.get(
        "/api/v1/viz/columns",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_viz_columns(req, params, *index_ptr);
        },
        RouteDoc{"Groupable columns present in the trace.",
                 "Visualization",
                 {},
                 R"({"columns":["cat","name","mhost","fhash"],"ready":true})"});

    router.get(
        "/api/v1/viz/events",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_viz_events(req, params, *index_ptr);
        },
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
        "/api/v1/viz/density",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_viz_density(req, params, *index_ptr);
        },
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
        "/api/v1/viz/stats",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_viz_stats(req, params, *index_ptr);
        },
        RouteDoc{"Per-name aggregation over a time range (Analyze).",
                 "Visualization",
                 {BEGIN, END, SUMMARY},
                 R"({"count":100,"total_dur":5000,"names":[{"name":"read",)"
                 R"("count":50,"total":2500,"avg":50,"min":10,"max":90}]})"});

    router.get(
        "/api/v1/viz/calltree",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_viz_calltree(req, params, *index_ptr);
        },
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
        "/api/v1/viz/histogram",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_viz_histogram(req, params, *index_ptr);
        },
        RouteDoc{"Duration distribution: percentiles + log-spaced buckets.",
                 "Visualization",
                 {BEGIN,
                  END,
                  SUMMARY,
                  {"query", "DSL predicate to narrow to one op", false, ""}},
                 R"({"min":10,"max":900,"p50":150,"p99":880,"buckets":[]})"});

    router.get(
        "/api/v1/viz/layers",
        [index_ptr](const HttpRequest& req,
                    const QueryParams& params) -> coro::CoroTask<HttpResponse> {
            co_return co_await handle_viz_layers(req, params, *index_ptr);
        },
        RouteDoc{"Operation-name to category map; declared vs I/O files.",
                 "Visualization",
                 {},
                 R"({"layers":{"read":"POSIX","write":"POSIX"},)"
                 R"("total_files":2,"io_files":2})"});
}

}  // namespace dftracer::utils::server
