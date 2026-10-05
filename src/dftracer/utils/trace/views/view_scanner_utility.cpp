#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/index/plan/prefilter.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/json/json.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <dftracer/utils/utilities/fileio/file_process_types.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <simdjson.h>

#include <cstring>
#include <string>

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
            dftracer::utils::index::iso8601_micros(t.get<std::string_view>());
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
std::size_t record_length(const char* line, std::size_t len) {
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                       line[len - 1] == '\r'))
        --len;
    if (len > 0 && line[len - 1] == ',') --len;
    const std::string_view record(line, len);
    return record == "[" || record == "]" ? 0 : len;
}

// An owned event from a parsed record, with the view's decoder. A fold that
// captures schema needs every leaf, so it gets no projection.
detail::FoldEvent decode(simdjson::dom::element root,
                         const ViewScannerInput& input, bool schema,
                         detail::DecodeHints& hints) {
    if (input.view.by_path)
        return detail::decode_record(
            root, *input.fold_intern, schema,
            schema || input.view.paths.empty() ? nullptr : &input.view.paths,
            input.view.record_schema, &input.view.path_fields,
            schema ? detail::INDEX_MAX_CHILDREN : 0, &hints);
    return detail::extract_fold_event(root, *input.fold_intern,
                                      input.fold_needs_args,
                                      input.fold_extra_fields, schema, &hints);
}

}  // namespace

void ScanCounts::add_to(ExportStats& st) const {
    st.events_matched += events_matched;
    st.events_scanned += events_scanned;
    st.lines_invalid += lines_invalid;
    st.values_unconverted += values_unconverted;
}

ViewScannerInput& ViewScannerInput::with_file_path(const std::string& path) {
    file_path = path;
    return *this;
}

ViewScannerInput& ViewScannerInput::with_index_path(const std::string& path) {
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
    RecordPhase phase = RecordPhase::UNKNOWN;
    bool has_ts = false;
    double ts = 0;
    double dur = 0;
};

static EventProbe probe_event(const char* data, std::size_t n,
                              const ViewDefinition& view) {
    thread_local simdjson::ondemand::parser parser;
    thread_local std::string padbuf;
    padbuf.assign(data, n);
    padbuf.resize(n + simdjson::SIMDJSON_PADDING);
    EventProbe p;
    simdjson::ondemand::document doc;
    if (parser.iterate(padbuf.data(), n, padbuf.size()).get(doc)) return p;
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

coro::AsyncGenerator<ViewScannerBatch> ViewScannerUtility::operator()(
    const ViewScannerInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("read view");
    const std::optional<duql::Query>* query_src =
        input.query ? &input.query : &input.view.query;

    const auto& query = *query_src;
    bool use_query = query.has_value();

    // Metadata lines bypass the query, so only a scan returning none may drop
    // lines unparsed.
    std::optional<index::plan::Prefilter> prefilter;
    if (use_query && input.view.prefilter && !input.view.include_metadata)
        prefilter.emplace(*query);
    std::optional<index::plan::Prefilter::Gate> gate;
    if (prefilter && !prefilter->empty()) gate.emplace(*prefilter);

    auto reader_input =
        utilities::fileio::IndexedReadInput::from_file(input.file_path)
            .with_index(input.index_path);
    if (input.checkpoint_size > 0) {
        reader_input.with_checkpoint_size(input.checkpoint_size);
    }
    utilities::fileio::IndexedFileReaderUtility reader_utility;
    auto reader = co_await reader_utility(reader_input);

    // Candidates are gzip-member-aligned byte ranges. A member's last event has
    // its content in this member but its terminating '\n' as the first byte of
    // the next member, and member-anchored seeking cannot look back past the
    // member start to reclaim it. Extend each range to the next newline so the
    // straddling event is read here; the next member then opens on that '\n'
    // and skips the resulting empty leading line, so nothing is dropped or
    // doubled.
    auto stream = reader->stream(
        utilities::reader::internal::StreamConfig()
            .stream_type(
                utilities::reader::internal::StreamType::MULTI_LINES_BYTES)
            .range_type(utilities::reader::internal::RangeType::BYTE_RANGE)
            .buffer_size(input.batch_size)
            .extend_to_line_boundary(true)
            .from(input.start_byte)
            .to(input.end_byte));

    ViewScannerBatch batch;

    dftracer::utils::json::RecordParser parser;
    detail::DecodeHints hints;

    while (!stream->done()) {
        auto chunk = co_await stream->read_async();
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
            std::size_t line_len = newline - line_start;

            // Fast path: with no query and no metadata records, the only
            // decision is to drop "ph":"M" records and emit the rest, so a
            // targeted phase probe replaces the full DOM parse. Fold mode needs
            // the full parse to build the FoldEvent, so it skips this.
            if (line_len > 0 && probe_lines) {
                const char* s = line_start;
                const char* e = line_start + line_len;
                while (s < e && (*s == ' ' || *s == '\t')) ++s;
                if (s < e && *s == '{') {
                    const EventProbe p =
                        probe_event(line_start, line_len, input.view);
                    if (input.view.by_path ||
                        p.phase != RecordPhase::METADATA) {
                        batch.events_scanned++;
                        if (in_window(p.has_ts, p.ts, p.dur, input.view)) {
                            batch.events.emplace_back(
                                line_start,
                                record_length(line_start, line_len));
                            batch.events_matched++;
                        }
                    }
                }
                pos = (newline - data) + 1;
                continue;
            }

            if (line_len > 0 && gate &&
                !gate->may_match(std::string_view(line_start, line_len))) {
                batch.events_scanned++;
                pos = (newline - data) + 1;
                continue;
            }

            const std::size_t json_len = record_length(line_start, line_len);
            if (json_len > 0) {
                auto result = parser.parse(line_start, json_len);
                const bool object =
                    !result.error() && result.value_unsafe().is_object();
                if (!object) ++batch.lines_invalid;
                if (object) {
                    auto root = result.value_unsafe();
                    {
                        JsonValue json(root);
                        const RecordPhase phase = input.view.by_path
                                                      ? RecordPhase::UNKNOWN
                                                      : read_phase(json["ph"]);
                        const bool metadata = phase == RecordPhase::METADATA;
                        bool match;
                        if (metadata) {
                            // Metadata records bypass the event query unless
                            // they are rows the query filters.
                            match = input.view.include_metadata &&
                                    (!use_query ||
                                     !(input.view.filter_metadata ||
                                       input.view.metadata_records) ||
                                     query->evaluate(json,
                                                     input.view.args_fallback));
                        } else {
                            batch.events_scanned++;
                            match = in_window(root, input.view) &&
                                    (!use_query ||
                                     query->evaluate(json,
                                                     input.view.args_fallback));
                        }
                        if (match) {
                            if (input.fold_intern) {
                                // Parse-once: the fold consumer never
                                // re-parses.
                                batch.fold_events.push_back(decode(
                                    root, input,
                                    !metadata && input.fold_capture_schema,
                                    hints));
                                batch.values_unconverted +=
                                    batch.fold_events.back().unconverted;
                                if (input.fold_keep_raw)
                                    batch.events.emplace_back(line_start,
                                                              json_len);
                            } else {
                                batch.events.emplace_back(line_start, json_len);
                            }
                            batch.events_matched++;
                        }
                    }
                }
            }

            pos = (newline - data) + 1;
        }

        // Yield at end of each chunk, string_view events point into
        // chunk data which is valid until the next co_await read_async().
        if (!batch.events.empty() || !batch.fold_events.empty() ||
            batch.lines_invalid > 0) {
            co_yield std::move(batch);
            batch = ViewScannerBatch{};
        }
    }

    // Final batch: normally empty since batches are yielded per chunk above.
    if (!batch.events.empty() || !batch.fold_events.empty() ||
        batch.lines_invalid > 0) {
        co_yield std::move(batch);
    }
}

}  // namespace dftracer::utils::trace::views

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/utilities/common/arrow/column_builder.h>

namespace dftracer::utils::trace::views {

using utilities::common::arrow::ArrowExportResult;
using utilities::common::arrow::ColumnType;
using utilities::common::arrow::RecordBatchBuilder;

ArrowExportResult ViewScannerBatch::to_arrow() const {
    RecordBatchBuilder builder;
    return to_arrow(builder);
}

ArrowExportResult ViewScannerBatch::to_arrow(
    RecordBatchBuilder& builder) const {
    builder.reserve(events.size());
    std::vector<std::string> held_serialized;
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
            std::string_view key_sv = field.key;
            auto val = field.value;

            if (val.is_int64()) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::INT64);
                builder.append_int64(ci, val.get_int64().value_unsafe());
            } else if (val.is_uint64()) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::UINT64);
                builder.append_uint64(ci, val.get_uint64().value_unsafe());
            } else if (val.is_double()) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::DOUBLE);
                builder.append_double(ci, val.get_double().value_unsafe());
            } else if (val.is_bool()) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::BOOL);
                builder.append_bool(ci, val.get_bool().value_unsafe());
            } else if (val.is_string()) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::STRING);
                builder.append_string(ci, val.get_string().value_unsafe());
            } else if (val.is_null()) {
                auto existing = builder.find_column(key_sv);
                if (existing) builder.append_null(*existing);
            } else {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::STRING);
                held_serialized.push_back(simdjson::minify(val));
                builder.append_string(ci, held_serialized.back());
            }
        }
        builder.end_row();
    }

    return builder.finish();
}

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_ENABLE_ARROW
