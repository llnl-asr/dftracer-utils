// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/intern_cache.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/lookup.h>
#include <dftracer/utils/index/plan/prefilter.h>
#include <dftracer/utils/json/json.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <dftracer/utils/json/value_path.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <dftracer/utils/utilities/fileio/file_process_types.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <simdjson.h>

#include <cstring>

namespace dftracer::utils::trace::views {

using dftracer::utils::json::JsonValue;

namespace {

// Whether a data record at `ts` lasting `dur` is kept by the view's time
// window; a record without a numeric ts is outside any window.
bool in_window(bool has_ts, double ts, double dur, const ViewDefinition& view) {
    if (!view.window) return true;
    if (!has_ts) return false;
    const auto [begin, end] = *view.window;
    // A zero-duration event overlaps only where it starts.
    if (!view.window_overlap || dur <= 0) return begin <= ts && ts < end;
    return ts < end && ts + dur > begin;
}

// The window test for a schema's time field, in its unit.
bool in_role_window(simdjson::dom::element root, const ViewDefinition& view) {
    const JsonValue json(root);
    const JsonValue t = json.at(view.time_path);
    double ts = 0;
    if (t.is_number()) {
        ts = t.get<double>() * view.time_factor;
    } else if (t.is_string()) {
        const auto us =
            dftracer::utils::index::iso8601_micros(t.get<dstd::StringView>());
        if (!us) return false;
        ts = static_cast<double>(*us);
    } else {
        return false;
    }
    double dur = 0;
    if (view.window_overlap && !view.duration_path.empty()) {
        const JsonValue d = json.at(view.duration_path);
        if (d.is_number()) dur = d.get<double>() * view.duration_factor;
    }
    return in_window(true, ts, dur, view);
}

bool in_window(simdjson::dom::element root, const ViewDefinition& view) {
    if (!view.window) return true;
    if (!view.time_path.empty()) return in_role_window(root, view);
    double ts = 0, dur = 0;
    const bool has_ts = root["ts"].get_double().get(ts) == simdjson::SUCCESS;
    if (view.window_overlap &&
        root["dur"].get_double().get(dur) != simdjson::SUCCESS)
        dur = 0;
    return in_window(has_ts, ts, dur, view);
}

// The length of the record on a line: without trailing blanks and the comma
// a JSON-array trace ends records with; 0 for the array's "[" and "]" lines.
std::size_t record_size(const char* line, std::size_t len) {
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                       line[len - 1] == '\r'))
        --len;
    if (len > 0 && line[len - 1] == ',') --len;
    const dstd::StringView record(line, len);
    return record == "[" || record == "]" ? 0 : len;
}

// Decode a parsed record into `ev`, with the view's decoder. A fold that
// captures schema needs every leaf, so it gets no projection.
void decode_into(detail::FoldEvent& ev, simdjson::dom::element root,
                 const ViewScannerInput& input, bool schema,
                 detail::DecodeHints& hints) {
    const bool skip_never = schema && input.fold_skip_never_indexed;
    if (input.view.by_path) {
        detail::decode_record_into(
            ev, root, *input.fold_intern, schema,
            schema || input.view.paths.empty() ? nullptr : &input.view.paths,
            input.view.record_schema, &input.view.path_fields,
            schema ? detail::INDEX_MAX_CHILDREN : 0, &hints, skip_never);
        return;
    }
    detail::extract_fold_event_into(ev, root, *input.fold_intern,
                                    input.fold_needs_args,
                                    input.fold_extra_fields, schema, &hints);
}

// As decode_into, reading the record on demand, for a line the DOM parser
// refuses. `schema` is whether a dftracer event enumerates its schema leaves.
void decode_into(detail::FoldEvent& ev, json::Value& record,
                 const ViewScannerInput& input, bool schema,
                 detail::DecodeHints& hints,
                 dftracer::utils::dstd::InternCache* cache) {
    if (input.view.by_path) {
        const bool by_path_schema = input.fold_capture_schema;
        const bool skip_never = by_path_schema && input.fold_skip_never_indexed;
        detail::decode_record_into(
            ev, record, *input.fold_intern, by_path_schema,
            by_path_schema || input.view.paths.empty() ? nullptr
                                                       : &input.view.paths,
            input.view.record_schema, &input.view.path_fields,
            by_path_schema ? detail::INDEX_MAX_CHILDREN : 0, &hints, skip_never,
            cache);
        return;
    }
    detail::extract_fold_event_into(ev, record, *input.fold_intern,
                                    input.fold_needs_args, cache,
                                    input.fold_extra_fields, schema, &hints);
}

bool in_role_window(json::Value& root, const ViewDefinition& view) {
    double ts = 0;
    const bool has_ts =
        json::at_path(root, view.time_path, [&](json::Value& t) {
            switch (t.type()) {
                case json::Value::Type::Int:
                case json::Value::Type::Double: {
                    const auto d = t.as_double();
                    if (!d) return false;
                    ts = *d * view.time_factor;
                    return true;
                }
                case json::Value::Type::String: {
                    const auto text = t.as_string();
                    const auto us =
                        text ? dftracer::utils::index::iso8601_micros(*text)
                             : dstd::nullopt;
                    if (!us) return false;
                    ts = static_cast<double>(*us);
                    return true;
                }
                default:
                    return false;
            }
        });
    if (!has_ts) return false;
    double dur = 0;
    if (view.window_overlap && !view.duration_path.empty())
        json::at_path(root, view.duration_path, [&](json::Value& d) {
            const auto number = d.type() == json::Value::Type::Int ||
                                        d.type() == json::Value::Type::Double
                                    ? d.as_double()
                                    : dstd::nullopt;
            if (number) dur = *number * view.duration_factor;
            return number.has_value();
        });
    return in_window(true, ts, dur, view);
}

bool in_window(json::Value& root, const ViewDefinition& view) {
    if (!view.window) return true;
    if (!view.time_path.empty()) return in_role_window(root, view);
    double ts = 0, dur = 0;
    bool has_ts = false;
    if (auto t = root.find("ts"))
        if (const auto d = t->as_double()) {
            ts = *d;
            has_ts = true;
        }
    if (view.window_overlap)
        if (auto t = root.find("dur"))
            if (const auto d = t->as_double()) dur = *d;
    return in_window(has_ts, ts, dur, view);
}

bool query_matches(json::Value& event, const duql::Query& query,
                   bool args_fallback) {
    json::detail::EventView view;
    if (!json::detail::ValueAccess::event(event, view)) return false;
    const duql::OnDemandEvent line(*view.doc, view.text, *view.strings);
    const bool kept = duql::evaluate(duql::detail::QueryAccess::root(query),
                                     line, args_fallback);
    if (line.broken) json::detail::ValueAccess::mark_broken(event);
    return kept;
}

// A lookup that failed on a record left its error in its slot, since the
// evaluator returns one value per record; the scan raises it between chunks.
dstd::Result<void> check_lookups(const duql::Query& query) {
    if (const auto failure =
            duql::lookup_failure(duql::detail::QueryAccess::root(query)))
        return common::make_error(common::ErrorCode::InvalidArgument,
                                  failure->message);
    return {};
}

struct LineScan {
    const ViewScannerInput& input;
    const dstd::Optional<duql::Query>* query;
    bool use_query;
    ViewScannerBatch& batch;
    detail::FoldEventBuffer& pool;
    detail::DecodeHints& hints;
    dftracer::utils::dstd::InternCache* cache;
    json::Document& document;
    dftracer::utils::json::RecordParser& parser;
};

void emit_event(LineScan& ls, const char* line_start, std::size_t json_size) {
    ls.batch.events.emplace_back(line_start, json_size);
}

// Without a query or args, the decode's own walk reads ph, ts and dur, so the
// phase and the window are checked on the decoded event. A ts of 0 or a missing
// dur may be a value the decode does not keep, such as a fraction, so those
// lines check the window on the text.
void scan_decoded(LineScan& ls, json::Value& event, const char* line_start,
                  std::size_t json_size) {
    const ViewScannerInput& input = ls.input;
    const ViewDefinition& view = input.view;
    ViewScannerBatch& batch = ls.batch;
    detail::FoldEvent& ev = ls.pool.append();
    decode_into(ev, event, input, false, ls.hints, ls.cache);
    if (json::detail::ValueAccess::broken(event)) {
        ls.pool.discard_last();
        ++batch.lines_invalid;
        return;
    }
    bool match;
    if (ev.phase == RecordPhase::Metadata) {
        match = view.include_metadata;
    } else {
        batch.events_scanned++;
        if (!view.window) {
            match = true;
        } else if (ev.ts == 0 || (view.window_overlap && !ev.has_dur)) {
            json::detail::ValueAccess::rewind(event);
            match = in_window(event, view);
        } else {
            match = in_window(true, static_cast<double>(ev.ts),
                              static_cast<double>(ev.dur), view);
        }
    }
    if (!match) {
        ls.pool.discard_last();
        return;
    }
    batch.values_unconverted += ev.unconverted;
    if (input.fold_keep_raw) emit_event(ls, line_start, json_size);
    batch.events_matched++;
}

// One line read on demand: the window, the query and the fold decode all
// read the same parse, which a syntax error in a part they reach drops.
void scan_line(LineScan& ls, const char* line_start, std::size_t json_size) {
    const ViewScannerInput& input = ls.input;
    ViewScannerBatch& batch = ls.batch;
    auto parsed = ls.document.parse(dstd::StringView(line_start, json_size));
    if (!parsed || parsed->type() != json::Value::Type::Object) {
        ++batch.lines_invalid;
        return;
    }
    json::Value& event = *parsed;
    if (input.fold_intern && !input.fold_needs_args && !ls.use_query &&
        !input.view.by_path && input.view.time_path.empty() &&
        !input.fold_capture_schema) {
        scan_decoded(ls, event, line_start, json_size);
        return;
    }
    const RecordPhase phase =
        input.view.by_path ? RecordPhase::Unknown : trace::event_phase(event);
    const bool metadata = phase == RecordPhase::Metadata;
    bool match;
    if (metadata) {
        match = input.view.include_metadata &&
                (!ls.use_query ||
                 !(input.view.filter_metadata || input.view.metadata_records) ||
                 query_matches(event, **ls.query, input.view.args_fallback));
    } else {
        match = in_window(event, input.view) &&
                (!ls.use_query ||
                 query_matches(event, **ls.query, input.view.args_fallback));
    }
    if (json::detail::ValueAccess::broken(event)) {
        ++batch.lines_invalid;
        return;
    }
    if (!metadata) batch.events_scanned++;
    if (!match) return;
    if (input.fold_intern) {
        json::detail::ValueAccess::rewind(event);
        decode_into(ls.pool.append(), event, input,
                    !metadata && input.fold_capture_schema, ls.hints, ls.cache);
        if (json::detail::ValueAccess::broken(event)) {
            ls.pool.discard_last();
            ++batch.lines_invalid;
            return;
        }
        batch.values_unconverted += ls.pool.back().unconverted;
        if (input.fold_keep_raw) emit_event(ls, line_start, json_size);
    } else {
        emit_event(ls, line_start, json_size);
    }
    batch.events_matched++;
}

// One line read through the DOM, for a dftracer fold that captures schema. A
// line the DOM parser refuses goes to scan_line.
void scan_dom_line(LineScan& ls, const char* line_start,
                   std::size_t json_size) {
    const ViewScannerInput& input = ls.input;
    ViewScannerBatch& batch = ls.batch;
    auto result = ls.parser.parse(line_start, json_size);
    if (result.error()) {
        // A line the DOM parser refuses is read on demand.
        scan_line(ls, line_start, json_size);
        return;
    }
    if (!result.value_unsafe().is_object()) {
        ++batch.lines_invalid;
        return;
    }
    auto root = result.value_unsafe();
    JsonValue json(root);
    const RecordPhase phase = read_phase(json["ph"]);
    const bool metadata = phase == RecordPhase::Metadata;
    bool match;
    if (metadata) {
        // Metadata records bypass the event query unless they are rows the
        // query filters.
        match = input.view.include_metadata &&
                (!ls.use_query ||
                 !(input.view.filter_metadata || input.view.metadata_records) ||
                 duql::evaluate(duql::detail::QueryAccess::root(**ls.query),
                                json, input.view.args_fallback));
    } else {
        batch.events_scanned++;
        match = in_window(root, input.view) &&
                (!ls.use_query ||
                 duql::evaluate(duql::detail::QueryAccess::root(**ls.query),
                                json, input.view.args_fallback));
    }
    if (!match) return;
    decode_into(ls.pool.append(), root, input,
                !metadata && input.fold_capture_schema, ls.hints);
    batch.values_unconverted += ls.pool.back().unconverted;
    if (input.fold_keep_raw) emit_event(ls, line_start, json_size);
    batch.events_matched++;
}

}  // namespace

void ScanCounts::add_to(ExportStats& st) const {
    st.events_matched += events_matched;
    st.events_scanned += events_scanned;
    st.lines_invalid += lines_invalid;
    st.values_unconverted += values_unconverted;
}

ViewScannerInput& ViewScannerInput::with_file_path(const dstd::String& path) {
    file_path = path;
    return *this;
}

ViewScannerInput& ViewScannerInput::with_index_path(const dstd::String& path) {
    index_path = path;
    return *this;
}

ViewScannerInput& ViewScannerInput::with_checkpoint_size(std::size_t sz) {
    checkpoint_size = sz;
    return *this;
}

ViewScannerInput& ViewScannerInput::with_byte_range(std::size_t start,
                                                    std::size_t end) {
    start_byte = start;
    end_byte = end;
    return *this;
}

ViewScannerInput& ViewScannerInput::with_checkpoint_idx(std::uint64_t idx) {
    checkpoint_idx = idx;
    return *this;
}

ViewScannerInput& ViewScannerInput::with_batch_size(std::size_t sz) {
    batch_size = sz;
    return *this;
}

ViewScannerInput& ViewScannerInput::with_event_batch_size(std::size_t sz) {
    event_batch_size = sz;
    return *this;
}

ViewScannerInput& ViewScannerInput::with_view(const ViewDefinition& v) {
    view = v;
    return *this;
}

// The top-level "ph" phase, or Unknown if absent. Uses simdjson On-Demand: SIMD
// structural indexing without building the full DOM the reader would otherwise
// pay for on every event, so the no-query/no-metadata path can drop metadata
// records cheaply. The padded buffer is required by On-Demand and reused across
// calls.
// A record's phase, and with a window its ts and dur, read without a DOM.
struct EventProbe {
    RecordPhase phase = RecordPhase::Unknown;
    bool has_ts = false;
    double ts = 0;
    double dur = 0;
};

static EventProbe probe_event(const char* data, std::size_t n,
                              const ViewDefinition& view) {
    thread_local simdjson::ondemand::parser parser;
    thread_local dstd::String padbuf;
    padbuf.assign(data, n);
    padbuf.resize(n + simdjson::SIMDJSON_PADDING);
    EventProbe p;
    simdjson::ondemand::document doc;
    simdjson::error_code code =
        parser.iterate(padbuf.data(), n, padbuf.size()).get(doc);
    if (json::detail::is_repairable(code)) {
        json::detail::repair_line(padbuf.data(), n);
        code = parser.iterate(padbuf.data(), n, padbuf.size()).get(doc);
    }
    if (code) return p;
    auto f = doc.find_field_unordered("ph");
    if (!f.error()) p.phase = read_phase(f.value_unsafe());
    if (!view.window) return p;
    p.has_ts = doc.find_field_unordered("ts").get_double().get(p.ts) ==
               simdjson::SUCCESS;
    if (view.window_overlap && doc.find_field_unordered("dur").get_double().get(
                                   p.dur) != simdjson::SUCCESS)
        p.dur = 0;
    return p;
}

coro::AsyncGenerator<dstd::Result<ViewScannerBatch>>
ViewScannerUtility::operator()(const ViewScannerInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("read view");
    const dstd::Optional<duql::Query>* query_src =
        input.query ? &input.query : &input.view.query;

    const auto& query = *query_src;
    bool use_query = query.has_value();

    // Metadata lines bypass the query, so only a scan returning none may drop
    // lines unparsed.
    dstd::Optional<index::plan::Prefilter> prefilter;
    if (use_query && input.view.prefilter && !input.view.include_metadata)
        prefilter.emplace(*query);
    dstd::Optional<index::plan::Prefilter::Gate> gate;
    if (prefilter && !prefilter->empty()) gate.emplace(*prefilter);

    auto reader_input =
        utilities::fileio::IndexedReadInput::from_file(input.file_path)
            .with_index(input.index_path);
    if (input.checkpoint_size > 0) {
        reader_input.with_checkpoint_size(input.checkpoint_size);
    }
    dstd::SharedPtr<utilities::reader::internal::Reader> reader;
    if (input.indexer) {
        auto made =
            utilities::reader::internal::ReaderFactory::create(input.indexer);
        if (!made) {
            co_yield dstd::Result<ViewScannerBatch>(made.error());
            co_return;
        }
        reader = std::move(*made);
    } else {
        utilities::fileio::IndexedFileReaderUtility reader_utility;
        auto made = co_await reader_utility(reader_input);
        if (!made) {
            co_yield dstd::Result<ViewScannerBatch>(made.error());
            co_return;
        }
        reader = std::move(*made);
    }

    // Candidates are gzip-member-aligned byte ranges. A member's last event has
    // its content in this member but its terminating '\n' as the first byte of
    // the next member, and member-anchored seeking cannot look back past the
    // member start to reclaim it. Extend each range to the next newline so the
    // straddling event is read here; the next member then opens on that '\n'
    // and skips the resulting empty leading line, so nothing is dropped or
    // doubled.
    auto opened = reader->stream(
        utilities::reader::internal::StreamConfig()
            .stream_type(
                utilities::reader::internal::StreamType::MultiLinesBytes)
            .range_type(utilities::reader::internal::RangeType::ByteRange)
            .buffer_size(input.batch_size)
            .extend_to_line_boundary(true)
            .from(input.start_byte)
            .to(input.end_byte));
    if (!opened) {
        co_yield dstd::Result<ViewScannerBatch>(opened.error());
        co_return;
    }
    auto stream = std::move(*opened);

    ViewScannerBatch batch;
    detail::FoldEventBuffer local_pool;
    detail::FoldEventBuffer& pool =
        input.fold_buffer ? *input.fold_buffer : local_pool;

    dftracer::utils::json::RecordParser parser;
    dftracer::utils::json::Document document;
    detail::DecodeHints hints;
    dstd::Optional<dftracer::utils::dstd::InternCache> local_cache;
    dftracer::utils::dstd::InternCache* cache = input.fold_cache;
    if (!cache && input.fold_intern) {
        local_cache.emplace(*input.fold_intern);
        cache = &*local_cache;
    }
    // A dftracer event that also enumerates its schema leaves has no on-demand
    // form.
    const bool dom_decode =
        input.fold_intern && !input.view.by_path && input.fold_capture_schema;
    LineScan scan{input, query_src, use_query, batch, pool,
                  hints, cache,     document,  parser};

    while (!stream->done()) {
        auto read = co_await stream->read_async();
        if (!read) {
            co_yield dstd::Result<ViewScannerBatch>(read.error());
            co_return;
        }
        const auto chunk = *read;
        if (chunk.empty()) break;

        const char* data = chunk.data();
        std::size_t bytes_read = chunk.size();
        std::size_t pos = 0;

        // A schema's time field needs the parsed record for the window test.
        const bool probe_lines =
            !use_query && !input.view.include_metadata &&
            input.fold_intern == nullptr &&
            (!input.view.window || input.view.time_path.empty());
        while (pos < bytes_read) {
            const char* line_start = data + pos;
            const char* newline = static_cast<const char*>(
                memchr(line_start, '\n', bytes_read - pos));
            // Chunks end on a line boundary; bytes after the last newline
            // are the file's last line, which has none.
            if (!newline) newline = data + bytes_read;
            std::size_t line_size = newline - line_start;

            // Fast path: with no query and no metadata records, the only
            // decision is to drop "ph":"M" records and emit the rest, so a
            // targeted phase probe replaces the full DOM parse. Fold mode needs
            // the full parse to build the FoldEvent, so it skips this.
            if (line_size > 0 && probe_lines) {
                const char* s = line_start;
                const char* e = line_start + line_size;
                while (s < e && (*s == ' ' || *s == '\t')) ++s;
                if (s < e && *s == '{') {
                    const EventProbe p =
                        probe_event(line_start, line_size, input.view);
                    if (input.view.by_path ||
                        p.phase != RecordPhase::Metadata) {
                        batch.events_scanned++;
                        if (in_window(p.has_ts, p.ts, p.dur, input.view)) {
                            batch.events.emplace_back(
                                line_start, record_size(line_start, line_size));
                            batch.events_matched++;
                        }
                    }
                }
                pos = (newline - data) + 1;
                continue;
            }

            if (line_size > 0 && gate &&
                !gate->may_match(dstd::StringView(line_start, line_size))) {
                batch.events_scanned++;
                pos = (newline - data) + 1;
                continue;
            }

            const std::size_t json_size = record_size(line_start, line_size);
            if (json_size > 0) {
                if (dom_decode)
                    scan_dom_line(scan, line_start, json_size);
                else
                    scan_line(scan, line_start, json_size);
            }

            pos = (newline - data) + 1;
        }

        if (use_query)
            if (auto looked_up = check_lookups(*query); !looked_up) {
                co_yield dstd::Result<ViewScannerBatch>(looked_up.error());
                co_return;
            }

        // Yield at end of each chunk, string_view events point into
        // chunk data which is valid until the next co_await read_async().
        if (!batch.events.empty() || !pool.empty() || batch.lines_invalid > 0) {
            batch.fold_events = pool.events();
            co_yield dstd::Result<ViewScannerBatch>(std::move(batch));
            batch = ViewScannerBatch{};
            pool.clear();
        }
    }

    // Final batch: normally empty since batches are yielded per chunk above.
    if (!batch.events.empty() || !pool.empty() || batch.lines_invalid > 0) {
        batch.fold_events = pool.events();
        co_yield dstd::Result<ViewScannerBatch>(std::move(batch));
    }
}

}  // namespace dftracer::utils::trace::views

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/utilities/arrow/column_builder.h>

namespace dftracer::utils::trace::views {

using utilities::arrow::ArrowExportResult;
using utilities::arrow::ColumnType;
using utilities::arrow::RecordBatchBuilder;

dstd::Result<ArrowExportResult> ViewScannerBatch::to_arrow() const {
    RecordBatchBuilder builder;
    return to_arrow(builder);
}

dstd::Result<ArrowExportResult> ViewScannerBatch::to_arrow(
    RecordBatchBuilder& builder) const {
    builder.reserve(events.size());
    dftracer::utils::json::RecordParser parser;

    for (const auto& event_str : events) {
        auto result = parser.parse(event_str.data(), event_str.size());
        if (result.error()) continue;
        auto elem = result.value_unsafe();
        if (!elem.is_object()) continue;

        auto obj_result = elem.get_object();
        if (obj_result.error()) continue;
        auto obj = obj_result.value_unsafe();

        for (auto field : obj) {
            dstd::StringView key_sv = field.key;
            auto val = field.value;

            if (val.is_int64()) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::Int64);
                builder.append_int64(ci, val.get_int64().value_unsafe());
            } else if (val.is_uint64()) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::Uint64);
                builder.append_uint64(ci, val.get_uint64().value_unsafe());
            } else if (val.is_double()) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::Double);
                builder.append_double(ci, val.get_double().value_unsafe());
            } else if (val.is_bool()) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::Bool);
                builder.append_bool(ci, val.get_bool().value_unsafe());
            } else if (val.is_string()) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::String);
                builder.append_string(ci, val.get_string().value_unsafe());
            } else if (val.is_null()) {
                auto existing = builder.find_column(key_sv);
                if (existing) builder.append_null(*existing);
            } else {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::String);
                builder.append_string(ci, simdjson::minify(val));
            }
        }
        builder.end_row();
    }

    return builder.finish();
}

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_ENABLE_ARROW
