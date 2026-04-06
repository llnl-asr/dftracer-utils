#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/utilities/common/json/json.h>
#include <dftracer/utils/utilities/common/query/evaluator.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <yyjson.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace dftracer::utils::utilities::composites::dft::views {

using dftracer::utils::utilities::common::json::JsonValue;

ViewReaderInput& ViewReaderInput::with_file_path(const std::string& path) {
    file_path = path;
    return *this;
}

ViewReaderInput& ViewReaderInput::with_index_path(const std::string& path) {
    index_path = path;
    return *this;
}

ViewReaderInput& ViewReaderInput::with_checkpoint_size(std::size_t sz) {
    checkpoint_size = sz;
    return *this;
}

ViewReaderInput& ViewReaderInput::with_byte_range(std::size_t start,
                                                  std::size_t end) {
    start_byte = start;
    end_byte = end;
    return *this;
}

ViewReaderInput& ViewReaderInput::with_checkpoint_idx(std::uint64_t idx) {
    checkpoint_idx = idx;
    return *this;
}

ViewReaderInput& ViewReaderInput::with_batch_size(std::size_t sz) {
    batch_size = sz;
    return *this;
}

ViewReaderInput& ViewReaderInput::with_event_batch_size(std::size_t sz) {
    event_batch_size = sz;
    return *this;
}

ViewReaderInput& ViewReaderInput::with_view(const ViewDefinition& v) {
    view = v;
    return *this;
}

// Hash metadata types that need smart filtering (FH, HH, SH).
// These carry a "value" field containing the hash string that other events
// reference via hhash/fhash/shash in their args.
static const std::unordered_set<std::string_view> HASH_METADATA_NAMES = {
    "FH", "HH", "SH"};

// Flush hash metadata entries referenced by a matched event into the batch.
static void collect_referenced_hashes_batch(
    const JsonValue& json,
    std::unordered_map<
        std::string, std::string, dftracer::utils::TransparentStringHash,
        dftracer::utils::TransparentStringEqual>& pending_metadata,
    std::unordered_set<std::string, dftracer::utils::TransparentStringHash,
                       dftracer::utils::TransparentStringEqual>& emitted_hashes,
    ViewReaderBatch& batch) {
    auto args = json["args"];
    if (!args.exists()) return;

    static const char* hash_fields[] = {"hhash", "fhash", "shash"};
    for (const char* field : hash_fields) {
        auto val = args[field];
        if (!val.exists()) continue;

        std::string_view hash_sv = val.get<std::string_view>();
        if (hash_sv.empty() || emitted_hashes.count(hash_sv)) continue;

        auto it = pending_metadata.find(hash_sv);
        if (it != pending_metadata.end()) {
            // Metadata events are owned strings (from earlier chunks).
            // Move into owned_events and add string_view pointing there.
            batch.owned_events.push_back(std::move(it->second));
            batch.events.push_back(batch.owned_events.back());
            batch.events_matched++;
            emitted_hashes.insert(std::string(hash_sv));
            pending_metadata.erase(it);
        }
    }
}

coro::AsyncGenerator<ViewReaderBatch> ViewReaderUtility::process(
    const ViewReaderInput& input) {
    const auto& query = input.query ? input.query : input.view.query;
    bool use_query = query.has_value();

    // Smart metadata buffering:
    // - Hash metadata (FH, HH, SH) → buffer keyed by hash value
    // - On matched event → flush referenced hashes from buffer
    // - thread_name/process_name → emit immediately (universal context)
    std::unordered_map<std::string, std::string,
                       dftracer::utils::TransparentStringHash,
                       dftracer::utils::TransparentStringEqual>
        pending_metadata;
    std::unordered_set<std::string, dftracer::utils::TransparentStringHash,
                       dftracer::utils::TransparentStringEqual>
        emitted_hashes;

    auto reader_input = composites::IndexedReadInput::from_file(input.file_path)
                            .with_index(input.index_path);
    if (input.checkpoint_size > 0) {
        reader_input.with_checkpoint_size(input.checkpoint_size);
    }
    composites::IndexedFileReaderUtility reader_utility;
    auto reader = co_await reader_utility.process(reader_input);

    auto stream = reader->stream(
        reader::internal::StreamConfig()
            .stream_type(reader::internal::StreamType::MULTI_LINES_BYTES)
            .range_type(reader::internal::RangeType::BYTE_RANGE)
            .buffer_size(input.batch_size)
            .from(input.start_byte)
            .to(input.end_byte));

    ViewReaderBatch batch;

    // NOTE(perf): reusable yyjson allocator
    char yy_buf[common::json::YYJSON_LINE_POOL_SIZE];
    yyjson_alc yy_alc;
    yyjson_alc_pool_init(&yy_alc, yy_buf, sizeof(yy_buf));

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

            if (line_len > 0) {
                yyjson_doc* doc =
                    yyjson_read_opts(const_cast<char*>(line_start), line_len,
                                     YYJSON_READ_NOFLAG, &yy_alc, nullptr);

                if (doc) {
                    yyjson_val* root = yyjson_doc_get_root(doc);
                    if (root && yyjson_is_obj(root)) {
                        JsonValue json(root);
                        std::string_view ph =
                            json["ph"].get<std::string_view>();

                        if (ph == "M" && input.view.include_metadata) {
                            std::string_view name_sv =
                                json["name"].get<std::string_view>();

                            if (HASH_METADATA_NAMES.count(name_sv)) {
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
                            } else {
                                // Non-hash metadata: string_view into chunk
                                batch.events.emplace_back(line_start, line_len);
                                batch.events_matched++;
                            }
                        } else if (ph != "M") {
                            batch.events_scanned++;
                            bool event_match =
                                !use_query || query->evaluate(json);
                            if (event_match) {
                                if (input.view.include_metadata) {
                                    collect_referenced_hashes_batch(
                                        json, pending_metadata, emitted_hashes,
                                        batch);
                                }
                                // Zero-copy: string_view into chunk data
                                batch.events.emplace_back(line_start, line_len);
                                batch.events_matched++;
                            }
                        }
                    }
                    yyjson_doc_free(doc);
                }
            }

            pos = (newline - data) + 1;
        }

        // Yield at end of each chunk, string_view events point into
        // chunk data which is valid until the next co_await read_async().
        if (!batch.events.empty()) {
            co_yield std::move(batch);
            batch = ViewReaderBatch{};
        }
    }

    // Final batch (shouldn't normally have leftovers since we yield per-chunk)
    if (!batch.events.empty()) {
        co_yield std::move(batch);
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::views

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/utilities/common/arrow/column_builder.h>

namespace dftracer::utils::utilities::composites::dft::views {

using common::arrow::ArrowExportResult;
using common::arrow::ColumnType;
using common::arrow::RecordBatchBuilder;

ArrowExportResult ViewReaderBatch::to_arrow() const {
    RecordBatchBuilder builder;
    builder.reserve(events.size());
    std::vector<yyjson_doc*> held_docs;
    std::vector<std::string> held_serialized;

    for (const auto& event_str : events) {
        yyjson_doc* doc = yyjson_read(event_str.data(), event_str.size(), 0);
        if (!doc) continue;
        yyjson_val* root = yyjson_doc_get_root(doc);
        if (!root || !yyjson_is_obj(root)) {
            yyjson_doc_free(doc);
            continue;
        }
        held_docs.push_back(doc);

        yyjson_obj_iter it;
        yyjson_obj_iter_init(root, &it);
        yyjson_val* key;
        while ((key = yyjson_obj_iter_next(&it))) {
            yyjson_val* val = yyjson_obj_iter_get_val(key);
            std::string_view key_sv(yyjson_get_str(key), yyjson_get_len(key));

            if (yyjson_is_int(val)) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::INT64);
                builder.append_int64(ci, yyjson_get_sint(val));
            } else if (yyjson_is_uint(val)) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::UINT64);
                builder.append_uint64(ci, yyjson_get_uint(val));
            } else if (yyjson_is_real(val)) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::DOUBLE);
                builder.append_double(ci, yyjson_get_real(val));
            } else if (yyjson_is_bool(val)) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::BOOL);
                builder.append_bool(ci, yyjson_get_bool(val));
            } else if (yyjson_is_str(val)) {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::STRING);
                builder.append_string(
                    ci,
                    std::string_view(yyjson_get_str(val), yyjson_get_len(val)));
            } else if (yyjson_is_null(val)) {
                // Only append null to an existing column; skip if new —
                // we don't know the type yet and STRING would corrupt later
                // typed appends.
                auto existing = builder.find_column(key_sv);
                if (existing) builder.append_null(*existing);
            } else {
                auto ci = builder.add_or_get_column(key_sv, ColumnType::STRING);
                std::size_t jlen;
                char* js = yyjson_val_write(val, 0, &jlen);
                if (js) {
                    held_serialized.emplace_back(js, jlen);
                    free(js);
                    builder.append_string(ci, held_serialized.back());
                } else {
                    builder.append_null(ci);
                }
            }
        }
        builder.end_row();
    }

    auto result = builder.finish();
    for (auto* d : held_docs) yyjson_doc_free(d);
    return result;
}

}  // namespace dftracer::utils::utilities::composites::dft::views

#endif  // DFTRACER_UTILS_ENABLE_ARROW
