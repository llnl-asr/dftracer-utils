#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/json/json.h>
#include <dftracer/utils/query/evaluator.h>
#include <dftracer/utils/trace/indexing/resolved_field_rewriter.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <dftracer/utils/utilities/fileio/file_process_types.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <simdjson.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace dftracer::utils::trace::views {

using dftracer::utils::json::JsonValue;

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

// Hash metadata types that need smart filtering (FH, HH, SH).
// These carry a "value" field containing the hash string that other events
// reference via hhash/fhash/shash in their args.
static const std::unordered_set<std::string_view> HASH_METADATA_NAMES = {
    "FH", "HH", "SH"};

static void collect_referenced_hashes_batch(
    const JsonValue& json,
    std::unordered_map<
        std::string, std::string, dftracer::utils::TransparentStringHash,
        dftracer::utils::TransparentStringEqual>& pending_metadata,
    std::unordered_set<std::string, dftracer::utils::TransparentStringHash,
                       dftracer::utils::TransparentStringEqual>& emitted_hashes,
    ViewScannerBatch& batch) {
    auto args = json["args"];
    if (!args.exists()) return;

    static const char* hash_fields[] = {"hhash",     "fhash",    "shash",
                                        "exec_hash", "cmd_hash", "cwd"};
    for (const char* field : hash_fields) {
        auto val = args[field];
        if (!val.exists()) continue;

        std::string_view hash_sv = val.get<std::string_view>();
        if (hash_sv.empty() || emitted_hashes.count(hash_sv)) continue;

        auto it = pending_metadata.find(hash_sv);
        if (it != pending_metadata.end()) {
            // Owned strings from earlier chunks; move into owned_events so the
            // string_view below stays valid.
            batch.owned_events.push_back(std::move(it->second));
            batch.events.push_back(batch.owned_events.back());
            batch.events_matched++;
            emitted_hashes.insert(std::string(hash_sv));
            pending_metadata.erase(it);
        }
    }
}

// The top-level "ph" phase, or Unknown if absent. Uses simdjson On-Demand: SIMD
// structural indexing without building the full DOM the reader would otherwise
// pay for on every event, so the no-query/no-metadata path can drop metadata
// records cheaply. The padded buffer is required by On-Demand and reused across
// calls.
static RecordPhase event_phase(const char* data, std::size_t n) {
    thread_local simdjson::ondemand::parser parser;
    thread_local std::string padbuf;
    padbuf.assign(data, n);
    padbuf.resize(n + simdjson::SIMDJSON_PADDING);
    simdjson::ondemand::document doc;
    if (parser.iterate(padbuf.data(), n, padbuf.size()).get(doc))
        return RecordPhase::UNKNOWN;
    auto f = doc.find_field_unordered("ph");
    if (f.error()) return RecordPhase::UNKNOWN;
    auto v = f.value_unsafe();
    return read_phase(v);
}

coro::AsyncGenerator<ViewScannerBatch> ViewScannerUtility::operator()(
    const ViewScannerInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("read view");
    const std::optional<query::Query>* query_src =
        input.query ? &input.query : &input.view.query;

    // Resolve virtual fields (resolved.*/r.*) to concrete hash in-clauses via
    // the index before per-event evaluation. Holds the rewrite for its
    // lifetime.
    std::optional<query::Query> rewritten;
    if (*query_src && !input.index_path.empty() &&
        dftracer::utils::trace::indexing::has_resolved_fields(**query_src)) {
        try {
            utilities::indexer::IndexDatabase db(
                input.index_path,
                dftracer::utils::utilities::indexer::IndexOpenMode::ReadOnly);
            if (auto rw =
                    dftracer::utils::trace::indexing::rewrite_resolved_fields(
                        **query_src, db)) {
                rewritten = std::move(rw);
                query_src = &rewritten;
            }
        } catch (...) {
        }
    }

    const auto& query = *query_src;
    bool use_query = query.has_value();

    // Smart metadata buffering:
    // - Hash metadata (FH, HH, SH) -> buffer keyed by hash value
    // - On matched event -> flush referenced hashes from buffer
    // - thread_name/process_name -> emit immediately (universal context)
    std::unordered_map<std::string, std::string,
                       dftracer::utils::TransparentStringHash,
                       dftracer::utils::TransparentStringEqual>
        pending_metadata;
    std::unordered_set<std::string, dftracer::utils::TransparentStringHash,
                       dftracer::utils::TransparentStringEqual>
        emitted_hashes;

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

    simdjson::dom::parser parser;

    while (!stream->done()) {
        auto chunk = co_await stream->read_async();
        if (chunk.empty()) break;

        const char* data = chunk.data();
        std::size_t bytes_read = chunk.size();
        std::size_t pos = 0;

        while (pos < bytes_read) {
            const char* line_start = data + pos;
            const char* newline = static_cast<const char*>(
                memchr(line_start, '\n', bytes_read - pos));
            if (!newline) break;
            std::size_t line_len = newline - line_start;

            // Fast path: with no query and no metadata harvesting, the only
            // decision is to drop "ph":"M" records and emit the rest, so a
            // targeted phase probe replaces the full DOM parse. Fold mode needs
            // the full parse to build the FoldEvent, so it skips this.
            if (line_len > 0 && !use_query && !input.view.include_metadata &&
                input.fold_intern == nullptr) {
                const char* s = line_start;
                const char* e = line_start + line_len;
                while (s < e && (*s == ' ' || *s == '\t')) ++s;
                if (s < e && *s == '{' &&
                    event_phase(line_start, line_len) !=
                        RecordPhase::METADATA) {
                    batch.events_scanned++;
                    batch.events.emplace_back(line_start, line_len);
                    batch.events_matched++;
                }
                pos = (newline - data) + 1;
                continue;
            }

            if (line_len > 0) {
                auto result = parser.parse(line_start, line_len);
                if (!result.error()) {
                    auto root = result.value_unsafe();
                    if (root.is_object()) {
                        JsonValue json(root);
                        RecordPhase phase = read_phase(json["ph"]);

                        if (phase == RecordPhase::METADATA &&
                            input.view.include_metadata) {
                            std::string_view name_sv =
                                json["name"].get<std::string_view>();

                            if (HASH_METADATA_NAMES.count(name_sv) &&
                                !input.view.emit_all_metadata) {
                                auto args = json["args"];
                                if (args.exists()) {
                                    auto val = args["value"];
                                    if (val.exists()) {
                                        std::string_view hash_sv =
                                            val.get<std::string_view>();
                                        if (!emitted_hashes.count(hash_sv)) {
                                            pending_metadata[std::string(
                                                hash_sv)] =
                                                std::string(line_start,
                                                            line_len);
                                        }
                                    }
                                }
                            } else if (input.view.filter_metadata &&
                                       use_query && !query->evaluate(json)) {
                                // phase("metadata") filters the records too;
                                // otherwise metadata bypasses the event query.
                            } else if (input.fold_intern) {
                                // Fold mode: parse-once metadata for a
                                // dictionary/bloom fold, no re-parse
                                // downstream.
                                batch.fold_events.push_back(
                                    detail::extract_fold_event(
                                        root, *input.fold_intern,
                                        input.fold_needs_args,
                                        input.fold_extra_fields));
                                batch.events_matched++;
                            } else {
                                // Non-hash metadata: string_view into chunk
                                batch.events.emplace_back(line_start, line_len);
                                batch.events_matched++;
                            }
                        } else if (phase != RecordPhase::METADATA) {
                            batch.events_scanned++;
                            bool event_match =
                                !use_query || query->evaluate(json);
                            if (event_match) {
                                if (input.view.include_metadata) {
                                    collect_referenced_hashes_batch(
                                        json, pending_metadata, emitted_hashes,
                                        batch);
                                }
                                if (input.fold_intern) {
                                    // Parse-once: extract the owned event here
                                    // so the fold consumer never re-parses.
                                    batch.fold_events.push_back(
                                        detail::extract_fold_event(
                                            root, *input.fold_intern,
                                            input.fold_needs_args,
                                            input.fold_extra_fields,
                                            input.fold_capture_schema));
                                    if (input.fold_keep_raw)
                                        batch.events.emplace_back(line_start,
                                                                  line_len);
                                } else {
                                    // Zero-copy: string_view into chunk data
                                    batch.events.emplace_back(line_start,
                                                              line_len);
                                }
                                batch.events_matched++;
                            }
                        }
                    }
                }
            }

            pos = (newline - data) + 1;
        }

        // Yield at end of each chunk, string_view events point into
        // chunk data which is valid until the next co_await read_async().
        if (!batch.events.empty() || !batch.fold_events.empty()) {
            co_yield std::move(batch);
            batch = ViewScannerBatch{};
        }
    }

    // Final batch: normally empty since batches are yielded per chunk above.
    if (!batch.events.empty() || !batch.fold_events.empty()) {
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
    simdjson::dom::parser parser;

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
