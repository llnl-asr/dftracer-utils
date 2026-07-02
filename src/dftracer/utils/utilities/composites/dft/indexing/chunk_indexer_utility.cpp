#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/common/json/json_doc_guard.h>
#include <dftracer/utils/utilities/common/json/parser.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/chunk_line_scanner.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>

#include <cstring>
#include <map>
#include <string_view>

using dftracer::utils::utilities::common::json::JsonParser;
using dftracer::utils::utilities::common::json::ondemand_value_to_string;

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
    DFTRACER_UTILS_TRACE_SCOPE("index chunk");
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
                .with_index(input.index_path);

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

    // Initialize hash resolutions from existing state (additive, no
    // double-count risk).  Statistics and event counts are recomputed from
    // scratch during the re-scan to avoid inflating totals.
    if (existing) {
        output.hash_resolutions = existing->hash_resolutions;
    }

    if (!need_rescan) {
        // Nothing to do - all dimensions already indexed.
        // Safe to carry over existing statistics verbatim.
        if (existing) {
            output.statistics = existing->statistics;
            output.events_processed = existing->events_processed;
        }
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

    std::map<std::pair<std::string, std::string>, std::vector<std::uint32_t>>
        event_lines;
    std::map<std::string, std::vector<std::uint32_t>> metadata_lines;

    // On-Demand parser for lazy field access - only parses what we use
    JsonParser parser;

    // Pre-check which bloom filters we need
    const bool need_name = output.bloom_filters.count(DIM_NAME) > 0;
    const bool need_cat = output.bloom_filters.count(DIM_CAT) > 0;
    const bool need_pid = output.bloom_filters.count(DIM_PID) > 0;
    const bool need_tid = output.bloom_filters.count(DIM_TID) > 0;
    const bool need_hhash = output.bloom_filters.count(DIM_HHASH) > 0;
    const bool need_fhash = output.bloom_filters.count(DIM_FHASH) > 0;
    const bool need_shash = output.bloom_filters.count(DIM_SHASH) > 0;
    const bool has_extra_dims = !input.config.extra_dimensions.empty();

    co_await dft::internal::scan_chunk_lines(*stream, [&](std::string_view
                                                              line_sv,
                                                          std::uint32_t
                                                              line_number) {
        if (!parser.parse(line_sv)) {
            return;
        }

        // Extract ph first to determine event type
        auto ph = parser.get_string("ph");
        if (!ph) {
            return;
        }

        bool is_metadata = (*ph == "M");

        if (is_metadata) {
            // Metadata event: extract name and args in single pass
            // Re-parse to get fresh document state
            parser.parse(line_sv);

            std::string event_name;
            std::string hash_val;
            std::string resolved;

            parser.for_each_field(
                [&](std::string_view key, simdjson::ondemand::value val) {
                    if (key == "name") {
                        auto s = val.get_string();
                        if (!s.error()) event_name = std::string(s.value());
                    } else if (key == "args") {
                        auto obj = val.get_object();
                        if (!obj.error()) {
                            for (auto field : obj.value()) {
                                if (field.error()) continue;
                                auto fkey = field.unescaped_key();
                                if (fkey.error()) continue;
                                auto fval = field.value();
                                if (fval.error()) continue;

                                if (fkey.value() == "value") {
                                    auto s = fval.value().get_string();
                                    if (!s.error())
                                        hash_val = std::string(s.value());
                                } else if (fkey.value() == "name") {
                                    auto s = fval.value().get_string();
                                    if (!s.error())
                                        resolved = std::string(s.value());
                                }
                            }
                        }
                    }
                });

            if (!hash_val.empty() && !resolved.empty()) {
                if (event_name == "HH") {
                    output.hash_resolutions[DIM_HHASH][hash_val] = resolved;
                } else if (event_name == "FH") {
                    output.hash_resolutions[DIM_FHASH][hash_val] = resolved;
                } else if (event_name == "SH") {
                    output.hash_resolutions[DIM_SHASH][hash_val] = resolved;
                }
            }

            if (collect_manifest) {
                metadata_lines[event_name].push_back(line_number);
            }
        } else {
            // Regular event: re-parse for fresh state and extract
            // fields
            parser.parse(line_sv);
            auto name_opt = parser.get_string("name");
            std::string_view name = name_opt.value_or("");
            auto cat_opt = parser.get_string("cat");
            std::string_view cat = cat_opt.value_or("");

            auto pid = parser.get_uint64("pid").value_or(0);
            auto tid = parser.get_uint64("tid").value_or(0);
            auto ts = parser.get_uint64("ts").value_or(0);
            auto dur = parser.get_uint64("dur").value_or(0);

            // Update statistics
            output.statistics.update_from_event(name, cat, pid, tid, ts, dur);

            // Add to bloom filters
            if (need_name && !name.empty()) {
                output.bloom_filters[DIM_NAME].add(name);
            }

            if (need_cat && !cat.empty()) {
                output.bloom_filters[DIM_CAT].add(cat);
            }

            if (need_pid) {
                char pid_buf[32];
                int n = std::snprintf(pid_buf, sizeof(pid_buf), "%llu",
                                      static_cast<unsigned long long>(pid));
                output.bloom_filters[DIM_PID].add(std::string_view(pid_buf, n));
            }

            if (need_tid) {
                char tid_buf[32];
                int n = std::snprintf(tid_buf, sizeof(tid_buf), "%llu",
                                      static_cast<unsigned long long>(tid));
                output.bloom_filters[DIM_TID].add(std::string_view(tid_buf, n));
            }

            // Process args for hash dimensions and extra dimensions
            if (need_hhash || need_fhash || need_shash || has_extra_dims) {
                parser.for_each_field("args", [&](std::string_view key,
                                                  simdjson::ondemand::value
                                                      val) {
                    if (need_hhash && key == "hhash") {
                        auto s = val.get_string();
                        if (!s.error() && !s.value().empty()) {
                            output.bloom_filters[DIM_HHASH].add(s.value());
                        }
                    } else if (need_fhash && key == "fhash") {
                        auto s = val.get_string();
                        if (!s.error() && !s.value().empty()) {
                            output.bloom_filters[DIM_FHASH].add(s.value());
                        }
                    } else if (need_shash &&
                               (key == "cmd_hash" || key == "exec_hash")) {
                        auto s = val.get_string();
                        if (!s.error() && !s.value().empty()) {
                            output.bloom_filters[DIM_SHASH].add(s.value());
                        }
                    } else if (has_extra_dims) {
                        // Check if this key matches any extra dimension
                        for (const auto& dim : input.config.extra_dimensions) {
                            // Check for exact match (flat key)
                            if (key == dim) {
                                std::string str_val =
                                    ondemand_value_to_string(val);
                                if (!str_val.empty()) {
                                    output.bloom_filters[dim].add(str_val);
                                }
                                break;
                            }
                            // Check for nested key (e.g., "io.size")
                            auto dot_pos = dim.find('.');
                            if (dot_pos != std::string::npos) {
                                std::string_view prefix(dim.data(), dot_pos);
                                if (key == prefix) {
                                    // Navigate into nested object
                                    std::string_view suffix(
                                        dim.data() + dot_pos + 1,
                                        dim.size() - dot_pos - 1);
                                    auto obj = val.get_object();
                                    if (!obj.error()) {
                                        for (auto field : obj.value()) {
                                            if (field.error()) continue;
                                            auto fkey = field.unescaped_key();
                                            if (fkey.error()) continue;
                                            if (fkey.value() == suffix) {
                                                auto fval = field.value();
                                                if (fval.error()) continue;
                                                std::string str_val =
                                                    ondemand_value_to_string(
                                                        fval.value());
                                                if (!str_val.empty()) {
                                                    output.bloom_filters[dim]
                                                        .add(str_val);
                                                }
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                });
            }

            if (collect_manifest) {
                event_lines[{std::string(cat), std::string(name)}].push_back(
                    line_number);
            }
            output.events_processed++;
        }
    });

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
