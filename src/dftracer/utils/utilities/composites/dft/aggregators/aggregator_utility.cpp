#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_output.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/chunk_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/chunk_mapper_utility.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#endif

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <set>

namespace dftracer::utils::utilities::composites::dft::aggregators {

// ---------------------------------------------------------------------------
// AggregatorInput fluent builders
// ---------------------------------------------------------------------------

AggregatorInput& AggregatorInput::with_directory(const std::string& dir) {
    directory = dir;
    return *this;
}

AggregatorInput& AggregatorInput::with_config(const AggregationConfig& cfg) {
    config = cfg;
    return *this;
}

AggregatorInput& AggregatorInput::with_checkpoint_size(std::size_t sz) {
    checkpoint_size = sz;
    return *this;
}

AggregatorInput& AggregatorInput::with_index_dir(const std::string& dir) {
    index_dir = dir;
    return *this;
}

AggregatorInput& AggregatorInput::with_force_rebuild(bool force) {
    force_rebuild = force;
    return *this;
}

AggregatorInput& AggregatorInput::with_chunk_size_mb(std::size_t mb) {
    chunk_size_mb = mb;
    return *this;
}

AggregatorInput& AggregatorInput::with_batch_size_mb(std::size_t mb) {
    batch_size_mb = mb;
    return *this;
}

AggregatorInput& AggregatorInput::with_event_batch_size(std::size_t sz) {
    event_batch_size = sz;
    return *this;
}

// ---------------------------------------------------------------------------
// AggregationBatch::to_arrow
// ---------------------------------------------------------------------------

#ifdef DFTRACER_UTILS_ENABLE_ARROW
using common::arrow::ArrowExportResult;
using common::arrow::ColumnType;
using common::arrow::RecordBatchBuilder;

ArrowExportResult AggregationBatch::to_arrow() const {
    RecordBatchBuilder builder;

    // Discover the union of extra key IDs and custom metric names.
    std::set<std::uint32_t> extra_key_id_set;
    std::set<std::string_view, std::less<>> custom_metric_name_set;
    for (const auto& [key, metrics] : entries) {
        if (key.extra_keys && !key.extra_keys->empty()) {
            for (const auto& [k, v] : *key.extra_keys) {
                extra_key_id_set.insert(k);
            }
        }
        if (metrics.custom_metrics && !metrics.custom_metrics->empty()) {
            for (const auto& [name, _] : *metrics.custom_metrics) {
                custom_metric_name_set.insert(name);
            }
        }
    }
    std::vector<std::uint32_t> extra_key_ids(extra_key_id_set.begin(),
                                             extra_key_id_set.end());
    std::vector<std::string_view> custom_metric_names(
        custom_metric_name_set.begin(), custom_metric_name_set.end());

    // Build schema: batch_type + fixed columns + extra keys + custom metrics
    std::vector<common::arrow::ColumnSpec> schema = {
        {"batch_type", ColumnType::INT64},  {"cat", ColumnType::STRING},
        {"name", ColumnType::STRING},       {"pid", ColumnType::UINT64},
        {"tid", ColumnType::UINT64},        {"hhash", ColumnType::STRING},
        {"fhash", ColumnType::STRING},      {"time_bucket", ColumnType::UINT64},
        {"count", ColumnType::UINT64},      {"dur_total", ColumnType::UINT64},
        {"dur_min", ColumnType::UINT64},    {"dur_max", ColumnType::UINT64},
        {"dur_mean", ColumnType::DOUBLE},   {"dur_std", ColumnType::DOUBLE},
        {"size_total", ColumnType::UINT64}, {"size_min", ColumnType::UINT64},
        {"size_max", ColumnType::UINT64},   {"size_mean", ColumnType::DOUBLE},
        {"size_std", ColumnType::DOUBLE},   {"ts", ColumnType::UINT64},
        {"te", ColumnType::UINT64},
    };
    for (auto id : extra_key_ids) {
        schema.push_back({std::string(aggregation_intern().resolve(id)),
                          ColumnType::STRING});
    }
    // Custom metric suffixed names — need owned strings for ColumnSpec
    struct MetricSuffix {
        const char* suffix;
        ColumnType type;
    };
    static constexpr MetricSuffix cm_schema[] = {
        {"_total", ColumnType::UINT64}, {"_min", ColumnType::UINT64},
        {"_max", ColumnType::UINT64},   {"_mean", ColumnType::DOUBLE},
        {"_std", ColumnType::DOUBLE},
    };
    for (auto cm : custom_metric_names) {
        for (const auto& [suffix, type] : cm_schema) {
            std::string col_name;
            col_name.reserve(cm.size() + 8);
            col_name.append(cm);
            col_name.append(suffix);
            schema.push_back({std::move(col_name), type});
        }
    }

    builder.declare_schema(schema);
    builder.reserve(entries.size());

    for (const auto& [key, metrics] : entries) {
        std::size_t ci = 0;
        builder.append_int64(ci++, static_cast<int64_t>(batch_type));
        builder.append_string(ci++, key.cat());
        builder.append_string(ci++, key.name());
        builder.append_uint64(ci++, key.pid);
        builder.append_uint64(ci++, key.tid);
        builder.append_string(ci++, key.hhash());
        builder.append_string(ci++, key.fhash());
        builder.append_uint64(ci++, key.time_bucket);
        builder.append_uint64(ci++, metrics.count);
        builder.append_uint64(ci++, metrics.duration.total);
        builder.append_uint64(ci++,
                              metrics.count > 0 ? metrics.duration.min : 0);
        builder.append_uint64(ci++, metrics.duration.max);
        builder.append_double(ci++, metrics.duration.mean);
        builder.append_double(ci++, metrics.get_stddev_duration());
        builder.append_uint64(ci++, metrics.size.total);
        builder.append_uint64(ci++, metrics.count > 0 ? metrics.size.min : 0);
        builder.append_uint64(ci++, metrics.size.max);
        builder.append_double(ci++, metrics.size.mean);
        builder.append_double(ci++, metrics.get_stddev_size());
        builder.append_uint64(ci++, metrics.ts);
        builder.append_uint64(ci++, metrics.te);

        for (auto extra_key_id : extra_key_ids) {
            bool found_extra_key = false;
            if (key.extra_keys) {
                for (const auto& [present_id, value_id] : *key.extra_keys) {
                    if (present_id == extra_key_id) {
                        builder.append_string(
                            ci++, aggregation_intern().resolve(value_id));
                        found_extra_key = true;
                        break;
                    }
                }
            }
            if (!found_extra_key) {
                builder.append_null(ci++);
            }
        }

        for (auto cm : custom_metric_names) {
            if (metrics.custom_metrics) {
                auto it = metrics.custom_metrics->find(cm);
                if (it != metrics.custom_metrics->end()) {
                    const auto& ms = it->second;
                    builder.append_uint64(ci++, ms.total);
                    builder.append_uint64(ci++, metrics.count > 0 ? ms.min : 0);
                    builder.append_uint64(ci++, ms.max);
                    builder.append_double(ci++, ms.mean);
                    builder.append_double(ci++, ms.get_stddev(metrics.count));
                    continue;
                }
            }
            for (std::size_t j = 0; j < std::size(cm_schema); ++j)
                builder.append_null(ci++);
        }

        builder.end_row();
    }

    return builder.finish();
}
#endif  // DFTRACER_UTILS_ENABLE_ARROW

// ---------------------------------------------------------------------------
// AggregatorUtility::process
// ---------------------------------------------------------------------------

coro::AsyncGenerator<AggregationBatch> AggregatorUtility::process(
    const AggregatorInput& input) {
    // Resolve index directory — create a temp one if not specified.
    std::string effective_index_dir = input.index_dir;
    std::string temp_index_dir;
    if (effective_index_dir.empty()) {
        try {
            auto temp_path = fs::temp_directory_path();
            temp_path /= "dftracer_idx_" + std::to_string(std::time(nullptr)) +
                         "_" + std::to_string(getpid());
            temp_index_dir = temp_path.string();
            fs::create_directories(temp_index_dir);
        } catch (const fs::filesystem_error&) {
            temp_index_dir = "/tmp/dftracer_idx_" +
                             std::to_string(std::time(nullptr)) + "_" +
                             std::to_string(getpid());
            fs::create_directories(temp_index_dir);
        }
        effective_index_dir = temp_index_dir;
    }

    // Discover input files.
    filesystem::PatternDirectoryScannerUtility scanner;
    filesystem::PatternDirectoryScannerUtilityInput scan_input{
        input.directory, {".pfw", ".pfw.gz"}, false};
    auto matched_entries = co_await scanner.process(scan_input);

    std::vector<std::string> input_files;
    input_files.reserve(matched_entries.size());
    for (const auto& entry : matched_entries) {
        input_files.push_back(entry.path.string());
    }

    if (input_files.empty()) {
        DFTRACER_UTILS_LOG_WARN("No .pfw or .pfw.gz files found in: %s",
                                input.directory.c_str());
        co_yield AggregationBatch{};
        co_return;
    }

    // Sequential pipeline: index → metadata → chunk map → aggregate → merge.
    // Parallelism at the file/chunk level is left to the caller (e.g. the
    // CLI binary uses CoroScope workers; Python callers use the Runtime).
    EventAggregatorUtility merger;
    std::atomic<int> global_chunk_idx{0};

    if (input.force_rebuild && !input_files.empty()) {
        const std::string shared_index_path =
            composites::dft::internal::determine_index_path(
                input_files.front(), effective_index_dir);
        if (fs::exists(shared_index_path)) {
            fs::remove_all(shared_index_path);
        }
    }

    for (const auto& file_path : input_files) {
        bool is_compressed =
            file_path.size() >= 3 &&
            file_path.compare(file_path.size() - 3, 3, ".gz") == 0;

        std::string idx_path;
        if (is_compressed) {
            idx_path = composites::dft::internal::determine_index_path(
                file_path, effective_index_dir);
            auto idx_input = indexer::IndexBuildConfig::for_file(file_path)
                                 .with_checkpoint_size(input.checkpoint_size)
                                 .with_force_rebuild(false)
                                 .with_index_dir(effective_index_dir);
            co_await indexer::IndexBuilderUtility{}.process(idx_input);
        }

        // Collect file metadata (line count, size, etc.).
        auto meta_input =
            composites::dft::MetadataCollectorUtilityInput::from_file(file_path)
                .with_checkpoint_size(input.checkpoint_size)
                .with_force_rebuild(false)
                .with_index(idx_path);
        auto metadata =
            co_await composites::dft::MetadataCollectorUtility{}.process(
                meta_input);

        if (!metadata.success) {
            DFTRACER_UTILS_LOG_WARN("Skipping file (metadata failed): %s",
                                    file_path.c_str());
            continue;
        }

        // Partition the file into byte-range chunks.
        FileChunkMapperUtility file_mapper;
        auto file_chunks = co_await file_mapper.process(
            FileChunkMapperInput::from_metadata(metadata)
                .with_config(input.config)
                .with_checkpoint_size(input.checkpoint_size)
                .with_target_chunk_size(input.chunk_size_mb)
                .with_batch_size(input.batch_size_mb * 1024 * 1024));

        int start_idx =
            global_chunk_idx.fetch_add(static_cast<int>(file_chunks.size()));
        for (int i = 0; i < static_cast<int>(file_chunks.size()); ++i) {
            file_chunks[i].chunk_index = start_idx + i;
        }

        for (auto& chunk : file_chunks) {
            ChunkAggregatorUtility agg;
            auto output = co_await agg.process(chunk);
            merger.merge_chunk(std::move(output));
        }
    }

    // Finalize the merged aggregation map.
    auto agg_results = merger.finalize();

    // Resolve process-parent associations and boundary events.
    AssociationResolverInput resolver_input;
    resolver_input.trackers = std::move(agg_results.trackers);
    resolver_input.aggregations = std::move(agg_results);
    resolver_input.config = input.config;

    AssociationResolverUtility resolver;
    auto resolver_output = co_await resolver.process(resolver_input);

    // Yield resolved aggregations in bounded batches, separated by type.
    const std::size_t batch_sz = input.event_batch_size;
    const auto& resolved = resolver_output.aggregations;

    auto yield_map = [&](AggregationMap& map, AggregationBatchType type)
        -> coro::AsyncGenerator<AggregationBatch> {
        AggregationBatch batch;
        batch.batch_type = type;
        batch.total_events_processed = resolved.total_events_processed;
        batch.total_files_processed = resolved.total_files_processed;
        batch.total_bytes_processed = resolved.total_bytes_processed;
        for (auto& [key, metrics] : map) {
            batch.entries.emplace_back(std::move(key), std::move(metrics));
            if (batch.entries.size() >= batch_sz) {
                co_yield std::move(batch);
                batch = AggregationBatch{};
                batch.batch_type = type;
                batch.total_events_processed = resolved.total_events_processed;
                batch.total_files_processed = resolved.total_files_processed;
                batch.total_bytes_processed = resolved.total_bytes_processed;
            }
        }
        if (!batch.entries.empty()) {
            co_yield std::move(batch);
        }
    };

    // Events
    auto event_gen = yield_map(resolver_output.aggregations.aggregations,
                               AggregationBatchType::EVENT);
    while (auto b = co_await event_gen.next()) co_yield std::move(*b);

    // Profiles
    auto profile_gen =
        yield_map(resolver_output.aggregations.profile_aggregations,
                  AggregationBatchType::PROFILE);
    while (auto b = co_await profile_gen.next()) co_yield std::move(*b);

    // System
    auto system_gen =
        yield_map(resolver_output.aggregations.system_aggregations,
                  AggregationBatchType::SYSTEM);
    while (auto b = co_await system_gen.next()) co_yield std::move(*b);

    // Clean up the temporary index directory if we created it.
    if (!temp_index_dir.empty()) {
        std::error_code ec;
        fs::remove_all(temp_index_dir, ec);
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
