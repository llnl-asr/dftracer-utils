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

#include <atomic>
#include <ctime>

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

AggregatorInput& AggregatorInput::with_executor_threads(std::size_t n) {
    executor_threads = n;
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
    builder.declare_schema({
        {"cat", ColumnType::STRING},
        {"name", ColumnType::STRING},
        {"pid", ColumnType::UINT64},
        {"tid", ColumnType::UINT64},
        {"hhash", ColumnType::STRING},
        {"fhash", ColumnType::STRING},
        {"time_bucket", ColumnType::UINT64},
        {"count", ColumnType::UINT64},
        {"dur_total", ColumnType::UINT64},
        {"dur_min", ColumnType::UINT64},
        {"dur_max", ColumnType::UINT64},
        {"dur_mean", ColumnType::DOUBLE},
        {"size_total", ColumnType::UINT64},
        {"size_min", ColumnType::UINT64},
        {"size_max", ColumnType::UINT64},
        {"size_mean", ColumnType::DOUBLE},
        {"ts", ColumnType::UINT64},
        {"te", ColumnType::UINT64},
    });
    builder.reserve(entries.size());

    for (const auto& [key, metrics] : entries) {
        builder.append_string(0, key.cat);
        builder.append_string(1, key.name);
        builder.append_uint64(2, key.pid);
        builder.append_uint64(3, key.tid);
        builder.append_string(4, key.hhash);
        builder.append_string(5, key.fhash);
        builder.append_uint64(6, key.time_bucket);
        builder.append_uint64(7, metrics.count);
        builder.append_uint64(8, metrics.duration.total);
        // min is initialized to uint64_max when no events — clamp to 0
        builder.append_uint64(9, metrics.count > 0 ? metrics.duration.min : 0);
        builder.append_uint64(10, metrics.duration.max);
        builder.append_double(11, metrics.duration.mean);
        builder.append_uint64(12, metrics.size.total);
        builder.append_uint64(13, metrics.count > 0 ? metrics.size.min : 0);
        builder.append_uint64(14, metrics.size.max);
        builder.append_double(15, metrics.size.mean);
        builder.append_uint64(16, metrics.ts);
        builder.append_uint64(17, metrics.te);
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

    for (const auto& file_path : input_files) {
        std::string idx_path = composites::dft::internal::determine_index_path(
            file_path, effective_index_dir);

        // Build (or reuse) the checkpoint index.
        auto idx_input = indexer::IndexBuildConfig::for_file(file_path)
                             .with_checkpoint_size(input.checkpoint_size)
                             .with_force_rebuild(input.force_rebuild)
                             .with_index_dir(effective_index_dir);
        co_await indexer::IndexBuilderUtility{}.process(idx_input);

        // Collect file metadata (line count, size, etc.).
        auto meta_input =
            composites::dft::MetadataCollectorUtilityInput::from_file(file_path)
                .with_checkpoint_size(input.checkpoint_size)
                .with_force_rebuild(input.force_rebuild)
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

        // Aggregate each chunk and merge incrementally.
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
    resolver_input.aggregations = agg_results;
    resolver_input.trackers = agg_results.trackers;
    resolver_input.config = input.config;

    AssociationResolverUtility resolver;
    auto resolver_output = co_await resolver.process(resolver_input);

    // Yield the resolved aggregations in bounded batches.
    const std::size_t batch_sz = input.event_batch_size;
    AggregationBatch batch;
    batch.total_events_processed =
        resolver_output.aggregations.total_events_processed;
    batch.total_files_processed =
        resolver_output.aggregations.total_files_processed;
    batch.total_bytes_processed =
        resolver_output.aggregations.total_bytes_processed;

    for (auto& [key, metrics] : resolver_output.aggregations.aggregations) {
        batch.entries.emplace_back(std::move(key), std::move(metrics));
        if (batch.entries.size() >= batch_sz) {
            co_yield std::move(batch);
            batch = AggregationBatch{};
            batch.total_events_processed =
                resolver_output.aggregations.total_events_processed;
            batch.total_files_processed =
                resolver_output.aggregations.total_files_processed;
            batch.total_bytes_processed =
                resolver_output.aggregations.total_bytes_processed;
        }
    }

    if (!batch.entries.empty()) {
        co_yield std::move(batch);
    }

    // Clean up the temporary index directory if we created it.
    if (!temp_index_dir.empty()) {
        std::error_code ec;
        fs::remove_all(temp_index_dir, ec);
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
