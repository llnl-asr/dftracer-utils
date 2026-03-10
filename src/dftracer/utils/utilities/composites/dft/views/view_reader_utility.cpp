#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/composites/dft/views/predicate_filter.h>
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

// ViewReaderInput fluent builders
ViewReaderInput& ViewReaderInput::with_file_path(const std::string& path) {
    file_path = path;
    return *this;
}

ViewReaderInput& ViewReaderInput::with_idx_path(const std::string& path) {
    idx_path = path;
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

ViewReaderInput& ViewReaderInput::with_view(const ViewDefinition& v) {
    view = v;
    return *this;
}

// Hash metadata types that need smart filtering (FH, HH, SH)
// These have a "value" field containing the hash string that other events
// reference via hhash/fhash/shash in their args.
static const std::unordered_set<std::string> HASH_METADATA_NAMES = {"FH", "HH",
                                                                    "SH"};

// Extract hash references from a matched event's args
static void collect_referenced_hashes(
    const JsonValue& json,
    std::unordered_map<std::string, std::string>& pending_metadata,
    std::unordered_set<std::string>& emitted_hashes,
    std::vector<std::string>& output_events, std::uint64_t& events_matched) {
    auto args = json["args"];
    if (!args.exists()) return;

    // Check each hash dimension
    static const char* hash_fields[] = {"hhash", "fhash", "shash"};
    for (const char* field : hash_fields) {
        auto val = args[field];
        if (!val.exists()) continue;

        std::string hash_val = val.get<std::string>();

        // Already emitted? Skip
        if (emitted_hashes.count(hash_val)) continue;

        // In pending buffer? Flush it
        auto it = pending_metadata.find(hash_val);
        if (it != pending_metadata.end()) {
            output_events.push_back(std::move(it->second));
            events_matched++;
            emitted_hashes.insert(hash_val);
            pending_metadata.erase(it);
        }
    }
}

coro::CoroTask<ViewReaderOutput> ViewReaderUtility::process(
    const ViewReaderInput& input) {
    ViewReaderOutput output;

    // Build predicate filters
    std::vector<PredicateFilter> filters;
    for (const auto& predicate : input.view.predicates) {
        filters.push_back(build_predicate_filter(predicate));
    }

    // Smart metadata buffering:
    // - Hash metadata (FH, HH, SH) → buffer keyed by hash value
    // - On matched event → flush referenced hashes from buffer
    // - thread_name/process_name → emit immediately (universal context)
    std::unordered_map<std::string, std::string> pending_metadata;
    std::unordered_set<std::string> emitted_hashes;

    // Create indexed reader
    auto reader_input = composites::IndexedReadInput::from_file(input.file_path)
                            .with_index(input.idx_path);
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
                yyjson_read_flag flg = YYJSON_READ_NOFLAG;
                yyjson_doc* doc =
                    yyjson_read_opts(const_cast<char*>(line_start), line_len,
                                     flg, nullptr, nullptr);

                if (doc) {
                    yyjson_val* root = yyjson_doc_get_root(doc);
                    if (root && yyjson_is_obj(root)) {
                        JsonValue json(root);
                        std::string_view ph =
                            json["ph"].get<std::string_view>();

                        if (ph == "M" && input.view.include_metadata) {
                            std::string name_str =
                                json["name"].get<std::string>();

                            if (HASH_METADATA_NAMES.count(name_str)) {
                                // Hash metadata → buffer keyed by value
                                auto args = json["args"];
                                if (args.exists()) {
                                    auto val = args["value"];
                                    if (val.exists()) {
                                        std::string hash_val =
                                            val.get<std::string>();
                                        if (!emitted_hashes.count(hash_val)) {
                                            pending_metadata[hash_val] =
                                                std::string(line_start,
                                                            line_len);
                                        }
                                    }
                                }
                            } else {
                                // Non-hash metadata (thread_name, etc.)
                                // Only emit if pid/tid match the
                                // predicate (or no pid/tid filter).
                                if (metadata_matches_identity(json, filters)) {
                                    output.events.emplace_back(line_start,
                                                               line_len);
                                    output.events_matched++;
                                }
                            }
                        } else if (ph != "M") {
                            output.events_scanned++;
                            // Check against predicate groups
                            if (matches_any_predicate(json, filters)) {
                                // Flush any referenced hash metadata first
                                if (input.view.include_metadata) {
                                    collect_referenced_hashes(
                                        json, pending_metadata, emitted_hashes,
                                        output.events, output.events_matched);
                                }
                                output.events.emplace_back(line_start,
                                                           line_len);
                                output.events_matched++;
                            }
                        }
                    }
                    yyjson_doc_free(doc);
                }
            }

            pos = (newline - data) + 1;
        }
    }

    // Pending metadata that was never referenced gets dropped

    output.success = true;
    co_return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::views