#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <yyjson.h>

#include <cstring>
#include <map>
#include <string_view>

// Import JsonValue from common json namespace
using dftracer::utils::utilities::common::json::JsonValue;

namespace dftracer::utils::utilities::composites::dft::indexing {

namespace {

// Hash dimension names
static const std::string DIM_HHASH = "hhash";
static const std::string DIM_FHASH = "fhash";
static const std::string DIM_SHASH = "shash";

// Dimension name constants
static const std::string DIM_NAME = "name";
static const std::string DIM_CAT = "cat";
static const std::string DIM_PID = "pid";
static const std::string DIM_TID = "tid";

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

// Build set of dimensions to index based on config
std::vector<std::string> get_target_dimensions(
    const ChunkIndexerConfig& config) {
    std::vector<std::string> dims;

    if (config.index_name) dims.push_back(DIM_NAME);
    if (config.index_cat) dims.push_back(DIM_CAT);
    if (config.index_pid) dims.push_back(DIM_PID);
    if (config.index_tid) dims.push_back(DIM_TID);
    if (config.index_hhash) dims.push_back(DIM_HHASH);
    if (config.index_fhash) dims.push_back(DIM_FHASH);
    if (config.index_shash) dims.push_back(DIM_SHASH);

    for (const auto& extra : config.extra_dimensions) {
        dims.push_back(extra);
    }

    return dims;
}

}  // namespace

coro::CoroTask<ChunkIndexerOutput> ChunkIndexerUtility::process(
    const ChunkIndexerInput& input) {
    ChunkIndexerOutput output;
    output.checkpoint_idx = input.checkpoint_idx;
    output.events_processed = 0;
    output.success = false;

    // Check if we have existing state for incremental re-scanning
    const ChunkIndexState* existing = nullptr;
    if (input.existing_state) {
        existing = input.existing_state.get();
    }

    // Determine which dimensions need to be indexed
    std::vector<std::string> target_dims = get_target_dimensions(input.config);
    std::vector<std::string> missing_dims;

    if (existing) {
        // Compute missing dimensions from existing state
        missing_dims = existing->indexed_dims.missing_dimensions(input.config);

        // Check if config parameters (false_positive_rate,
        // expected_entries_per_chunk) have changed since last index
        std::size_t current_hash = input.config.compute_hash();
        bool config_changed =
            existing->config_hash != 0 && existing->config_hash != current_hash;

        if (config_changed) {
            DFTRACER_UTILS_LOG_INFO(
                "ChunkIndexer: Config changed for checkpoint %llu, "
                "forcing full re-index",
                static_cast<unsigned long long>(input.checkpoint_idx));
            missing_dims = target_dims;
        }

        // If no dimensions are missing and config hasn't changed, return
        // existing state
        if (missing_dims.empty()) {
            DFTRACER_UTILS_LOG_INFO(
                "ChunkIndexer: All dimensions already indexed for checkpoint "
                "%llu, skipping re-scan",
                static_cast<unsigned long long>(input.checkpoint_idx));

            // Copy existing state to output
            output.bloom_filters.clear();
            output.hash_resolutions = existing->hash_resolutions;
            output.statistics = existing->statistics;
            output.events_processed = existing->events_processed;
            output.success = true;
            co_return output;
        }

        DFTRACER_UTILS_LOG_INFO(
            "ChunkIndexer: Incremental re-scan for checkpoint %llu, "
            "missing %zu dimensions",
            static_cast<unsigned long long>(input.checkpoint_idx),
            missing_dims.size());
    } else {
        // No existing state - need to index all dimensions
        missing_dims = target_dims;
    }

    // Initialize bloom filters for missing dimensions
    auto make_bloom = [&]() {
        return BloomFilter(input.config.expected_entries_per_chunk,
                           input.config.false_positive_rate);
    };

    // Create bloom filters only for dimensions that need indexing
    for (const auto& dim : missing_dims) {
        output.bloom_filters.emplace(dim, make_bloom());
    }

    // If we have existing bloom filters, we need to read the chunk data
    // to populate the missing ones
    bool collect_manifest = input.config.build_manifest;
    bool need_rescan = !missing_dims.empty() || collect_manifest;

    // Create reader if needed
    std::shared_ptr<reader::internal::Reader> reader;
    if (need_rescan) {
        auto reader_input =
            composites::IndexedReadInput::from_file(input.file_path)
                .with_checkpoint_size(input.checkpoint_size)
                .with_index(input.idx_path);

        composites::IndexedFileReaderUtility reader_utility;
        reader = co_await reader_utility.process(reader_input);

        if (!reader) {
            DFTRACER_UTILS_LOG_ERROR(
                "ChunkIndexer: Failed to create reader for %s checkpoint %llu",
                input.file_path.c_str(),
                static_cast<unsigned long long>(input.checkpoint_idx));
            co_return output;
        }
    }

    // Initialize statistics and hash resolutions
    if (existing) {
        // Start with existing statistics and resolutions
        output.statistics = existing->statistics;
        output.hash_resolutions = existing->hash_resolutions;
        output.events_processed = existing->events_processed;
    }

    if (!need_rescan) {
        // Nothing to do - all dimensions already indexed
        output.success = true;
        co_return output;
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
        co_return output;
    }

    std::uint32_t line_number = 0;
    std::map<std::pair<std::string, std::string>, std::vector<std::uint32_t>>
        event_lines;
    std::map<std::string, std::vector<std::uint32_t>> metadata_lines;

    while (!stream->done()) {
        auto chunk = co_await stream->read_async();

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
                            if (collect_manifest) {
                                std::string meta_type(name_sv);
                                metadata_lines[meta_type].push_back(
                                    line_number);
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

                            // Update statistics (always update for accuracy)
                            output.statistics.update_from_event(
                                name_sv, cat_sv, pid, tid, ts, dur);

                            // Add to bloom filters for missing dimensions only
                            auto it = output.bloom_filters.find(DIM_NAME);
                            if (it != output.bloom_filters.end() &&
                                !name_sv.empty()) {
                                it->second.add(name_sv);
                            }

                            it = output.bloom_filters.find(DIM_CAT);
                            if (it != output.bloom_filters.end() &&
                                !cat_sv.empty()) {
                                it->second.add(cat_sv);
                            }

                            it = output.bloom_filters.find(DIM_PID);
                            if (it != output.bloom_filters.end()) {
                                std::string pid_str = std::to_string(pid);
                                it->second.add(pid_str);
                            }

                            it = output.bloom_filters.find(DIM_TID);
                            if (it != output.bloom_filters.end()) {
                                std::string tid_str = std::to_string(tid);
                                it->second.add(tid_str);
                            }

                            JsonValue args = json["args"];
                            if (args.exists()) {
                                // Hash dimensions: add hash to bloom
                                it = output.bloom_filters.find(DIM_HHASH);
                                if (it != output.bloom_filters.end()) {
                                    std::string_view hhash =
                                        args["hhash"].get<std::string_view>();
                                    if (!hhash.empty()) {
                                        it->second.add(hhash);
                                    }
                                }

                                it = output.bloom_filters.find(DIM_FHASH);
                                if (it != output.bloom_filters.end()) {
                                    std::string_view fhash =
                                        args["fhash"].get<std::string_view>();
                                    if (!fhash.empty()) {
                                        it->second.add(fhash);
                                    }
                                }

                                it = output.bloom_filters.find(DIM_SHASH);
                                if (it != output.bloom_filters.end()) {
                                    // shash can be under cmd_hash or exec_hash
                                    std::string_view shash =
                                        args["cmd_hash"]
                                            .get<std::string_view>();
                                    if (shash.empty()) {
                                        shash = args["exec_hash"]
                                                    .get<std::string_view>();
                                    }
                                    if (!shash.empty()) {
                                        it->second.add(shash);
                                    }
                                }

                                // Extra dimensions: arbitrary nested dot-paths
                                for (const auto& dim :
                                     input.config.extra_dimensions) {
                                    it = output.bloom_filters.find(dim);
                                    if (it != output.bloom_filters.end()) {
                                        JsonValue val = args.at(dim.c_str());
                                        if (val.exists()) {
                                            std::string str_val =
                                                json_value_to_string(val);
                                            if (!str_val.empty()) {
                                                it->second.add(str_val);
                                            }
                                        }
                                    }
                                }
                            }

                            if (collect_manifest) {
                                event_lines[{std::string(cat_sv),
                                             std::string(name_sv)}]
                                    .push_back(line_number);
                            }
                            output.events_processed++;
                        }
                    }
                    yyjson_doc_free(doc);
                }
            }

            pos = (newline - data) + 1;
            line_number++;
        }
    }

    if (collect_manifest) {
        output.event_line_groups.reserve(event_lines.size());
        for (auto& [key, lines] : event_lines) {
            EventLineGroup g;
            g.cat = key.first;
            g.name = key.second;
            g.line_numbers = std::move(lines);
            output.event_line_groups.push_back(std::move(g));
        }
        output.metadata_line_groups.reserve(metadata_lines.size());
        for (auto& [meta_type, lines] : metadata_lines) {
            MetadataLineGroup g;
            g.meta_type = meta_type;
            g.line_numbers = std::move(lines);
            output.metadata_line_groups.push_back(std::move(g));
        }
    }

    output.success = true;
    co_return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
