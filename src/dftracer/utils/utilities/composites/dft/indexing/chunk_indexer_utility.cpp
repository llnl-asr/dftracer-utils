#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <yyjson.h>

#include <cstring>
#include <string_view>

// Import JsonValue from common json namespace
using dftracer::utils::utilities::common::json::JsonValue;

namespace dftracer::utils::utilities::composites::dft::indexing {

namespace {

// Hash dimension names
static const std::string DIM_HHASH = "hhash";
static const std::string DIM_FHASH = "fhash";
static const std::string DIM_SHASH = "shash";

// Convert a JsonValue to string for bloom filter insertion.
// Handles strings, integers, floats, bools.
std::string json_value_to_string(const JsonValue& val) {
    if (val.is_string()) {
        return val.get<std::string>();
    } else if (val.is_uint()) {
        return std::to_string(val.get<std::uint64_t>());
    } else if (val.is_int()) {
        return std::to_string(val.get<std::int64_t>());
    } else if (val.is_number()) {
        return std::to_string(val.get<double>());
    } else if (val.is_bool()) {
        return val.get<bool>() ? "true" : "false";
    }
    return {};
}

}  // namespace

ChunkIndexerOutput ChunkIndexerUtility::process(
    const ChunkIndexerInput& input) {
    ChunkIndexerOutput output;
    output.checkpoint_idx = input.checkpoint_idx;
    output.events_processed = 0;
    output.success = false;

    // Initialize bloom filters for each configured dimension
    auto make_bloom = [&]() {
        return BloomFilter(input.config.expected_entries_per_chunk,
                           input.config.false_positive_rate);
    };

    if (input.config.index_name)
        output.bloom_filters.emplace("name", make_bloom());
    if (input.config.index_cat)
        output.bloom_filters.emplace("cat", make_bloom());
    if (input.config.index_pid)
        output.bloom_filters.emplace("pid", make_bloom());
    if (input.config.index_tid)
        output.bloom_filters.emplace("tid", make_bloom());
    if (input.config.index_hhash)
        output.bloom_filters.emplace(DIM_HHASH, make_bloom());
    if (input.config.index_fhash)
        output.bloom_filters.emplace(DIM_FHASH, make_bloom());
    if (input.config.index_shash)
        output.bloom_filters.emplace(DIM_SHASH, make_bloom());

    for (const auto& dim : input.config.extra_dimensions) {
        output.bloom_filters.emplace(dim, make_bloom());
    }

    // Create reader
    auto reader_input = composites::IndexedReadInput::from_file(input.file_path)
                            .with_checkpoint_size(input.checkpoint_size)
                            .with_index(input.idx_path);

    composites::IndexedFileReaderUtility reader_utility;
    auto reader = reader_utility.process(reader_input);

    if (!reader) {
        DFTRACER_UTILS_LOG_ERROR(
            "ChunkIndexer: Failed to create reader for %s checkpoint %llu",
            input.file_path.c_str(),
            static_cast<unsigned long long>(input.checkpoint_idx));
        return output;
    }

    auto stream = reader->stream(
        reader::internal::StreamConfig()
            .stream_type(reader::internal::StreamType::MULTI_LINES_BYTES)
            .range_type(reader::internal::RangeType::BYTE_RANGE)
            .buffer_size(input.batch_size)
            .from(input.start_byte)
            .to(input.end_byte));

    if (!stream) {
        DFTRACER_UTILS_LOG_ERROR(
            "ChunkIndexer: Failed to create stream for %s checkpoint %llu",
            input.file_path.c_str(),
            static_cast<unsigned long long>(input.checkpoint_idx));
        return output;
    }

    while (!stream->done()) {
        auto chunk = stream->read();

        if (chunk.empty()) {
            break;
        }

        std::size_t bytes_read = chunk.size();
        const char* data = chunk.data();
        std::size_t pos = 0;

        while (pos < bytes_read) {
            const char* line_start = data + pos;
            const char* newline = static_cast<const char*>(
                memchr(line_start, '\n', bytes_read - pos));

            if (!newline) {
                break;
            }

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

                        if (ph == "M") {
                            // Metadata event: collect hash resolutions
                            std::string_view name_sv =
                                json["name"].get<std::string_view>();
                            JsonValue args = json["args"];

                            if (args.exists()) {
                                std::string hash_val =
                                    args["value"].get<std::string>();
                                std::string resolved =
                                    args["name"].get<std::string>();

                                if (!hash_val.empty() && !resolved.empty()) {
                                    if (name_sv == "HH") {
                                        output.hash_resolutions[DIM_HHASH]
                                                               [hash_val] =
                                            resolved;
                                    } else if (name_sv == "FH") {
                                        output.hash_resolutions[DIM_FHASH]
                                                               [hash_val] =
                                            resolved;
                                    } else if (name_sv == "SH") {
                                        output.hash_resolutions[DIM_SHASH]
                                                               [hash_val] =
                                            resolved;
                                    }
                                }
                            }
                        } else {
                            // Regular event: index into bloom filters + stats
                            std::string_view name_sv =
                                json["name"].get<std::string_view>();
                            std::string_view cat_sv =
                                json["cat"].get<std::string_view>();
                            std::uint64_t pid =
                                json["pid"].get<std::uint64_t>();
                            std::uint64_t tid =
                                json["tid"].get<std::uint64_t>();
                            std::uint64_t ts = json["ts"].get<std::uint64_t>();
                            std::uint64_t dur =
                                json["dur"].get<std::uint64_t>();

                            // Update statistics
                            output.statistics.update_from_event(
                                name_sv, cat_sv, pid, tid, ts, dur);

                            // Add to bloom filters
                            if (input.config.index_name && !name_sv.empty()) {
                                output.bloom_filters.at("name").add(name_sv);
                            }
                            if (input.config.index_cat && !cat_sv.empty()) {
                                output.bloom_filters.at("cat").add(cat_sv);
                            }
                            if (input.config.index_pid) {
                                std::string pid_str = std::to_string(pid);
                                output.bloom_filters.at("pid").add(pid_str);
                            }
                            if (input.config.index_tid) {
                                std::string tid_str = std::to_string(tid);
                                output.bloom_filters.at("tid").add(tid_str);
                            }

                            JsonValue args = json["args"];
                            if (args.exists()) {
                                // Hash dimensions: add hash to bloom
                                if (input.config.index_hhash) {
                                    std::string_view hhash =
                                        args["hhash"].get<std::string_view>();
                                    if (!hhash.empty()) {
                                        output.bloom_filters.at(DIM_HHASH).add(
                                            hhash);
                                    }
                                }

                                if (input.config.index_fhash) {
                                    std::string_view fhash =
                                        args["fhash"].get<std::string_view>();
                                    if (!fhash.empty()) {
                                        output.bloom_filters.at(DIM_FHASH).add(
                                            fhash);
                                    }
                                }

                                if (input.config.index_shash) {
                                    // shash can be under cmd_hash or exec_hash
                                    std::string_view shash =
                                        args["cmd_hash"]
                                            .get<std::string_view>();
                                    if (shash.empty()) {
                                        shash = args["exec_hash"]
                                                    .get<std::string_view>();
                                    }
                                    if (!shash.empty()) {
                                        output.bloom_filters.at(DIM_SHASH).add(
                                            shash);
                                    }
                                }

                                // Extra dimensions: arbitrary nested dot-paths
                                for (const auto& dim :
                                     input.config.extra_dimensions) {
                                    JsonValue val = args.at(dim.c_str());
                                    if (val.exists()) {
                                        std::string str_val =
                                            json_value_to_string(val);
                                        if (!str_val.empty()) {
                                            output.bloom_filters.at(dim).add(
                                                str_val);
                                        }
                                    }
                                }
                            }

                            output.events_processed++;
                        }
                    }
                    yyjson_doc_free(doc);
                }
            }

            pos = (newline - data) + 1;
        }
    }

    output.success = true;
    return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
