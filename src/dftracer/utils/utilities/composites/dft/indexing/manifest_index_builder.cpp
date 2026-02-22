#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/index_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/manifest_index_builder.h>
#include <dftracer/utils/utilities/composites/dft/indexing/manifest_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>

namespace dftracer::utils::utilities::composites::dft::indexing {

ManifestIndexBuildOutput ManifestIndexBuilderUtility::process(
    const ManifestIndexBuildInput& input) {
    ManifestIndexBuildOutput output;
    output.file_path = input.file_path;

    try {
        // 1. Resolve midx path
        std::string midx_path =
            determine_manifest_index_path(input.file_path, input.index_dir);
        output.midx_path = midx_path;

        // 2. Check if already indexed
        if (!input.force_rebuild && fs::exists(midx_path)) {
            try {
                ManifestIndexDatabase midx(midx_path);
                midx.init_schema();
                int fid = midx.get_file_info_id(input.file_path);
                if (fid >= 0) {
                    DFTRACER_UTILS_LOG_INFO("Skipping already-indexed file: %s",
                                            input.file_path.c_str());
                    output.success = true;
                    output.was_skipped = true;
                    return output;
                }
            } catch (...) {
                // Database corrupt or unreadable -- rebuild
            }
        }

        // 3. Build gzip index
        std::string idx_path =
            internal::determine_index_path(input.file_path, input.index_dir);
        auto idx_input =
            composites::dft::IndexBuildUtilityInput::from_file(input.file_path)
                .with_checkpoint_size(input.checkpoint_size)
                .with_force_rebuild(input.force_rebuild)
                .with_index(idx_path);
        composites::dft::IndexBuilderUtility{}.process(idx_input);

        // 4. Collect metadata
        auto meta_input =
            composites::dft::MetadataCollectorUtilityInput::from_file(
                input.file_path)
                .with_checkpoint_size(input.checkpoint_size)
                .with_force_rebuild(false)
                .with_index(idx_path);
        auto metadata =
            composites::dft::MetadataCollectorUtility{}.process(meta_input);

        if (!metadata.success) {
            output.error_message =
                "Failed to collect metadata for " + input.file_path;
            return output;
        }

        // 5. Parallel chunk indexing with manifest collection

        std::size_t file_size = metadata.uncompressed_size;
        std::size_t num_ckpts = metadata.num_checkpoints;

        // Only need manifest data -- disable bloom dimensions
        ChunkIndexerConfig indexer_config;
        indexer_config.build_manifest = true;
        indexer_config.index_name = false;
        indexer_config.index_cat = false;
        indexer_config.index_pid = false;
        indexer_config.index_tid = false;
        indexer_config.index_hhash = false;
        indexer_config.index_fhash = false;
        indexer_config.index_shash = false;

        std::vector<ChunkIndexerInput> chunk_inputs;
        if (num_ckpts == 0) {
            ChunkIndexerInput ci;
            ci.with_file_path(input.file_path)
                .with_idx_path(idx_path)
                .with_checkpoint_size(input.checkpoint_size)
                .with_checkpoint_idx(0)
                .with_byte_range(0, file_size)
                .with_config(indexer_config)
                .with_batch_size(input.batch_size);
            chunk_inputs.push_back(std::move(ci));
        } else {
            std::size_t bytes_per = file_size / num_ckpts;
            for (std::size_t i = 0; i < num_ckpts; ++i) {
                std::size_t start = i * bytes_per;
                std::size_t end =
                    (i + 1 == num_ckpts) ? file_size : (i + 1) * bytes_per;
                ChunkIndexerInput ci;
                ci.with_file_path(input.file_path)
                    .with_idx_path(idx_path)
                    .with_checkpoint_size(input.checkpoint_size)
                    .with_checkpoint_idx(static_cast<std::uint64_t>(i))
                    .with_byte_range(start, end)
                    .with_config(indexer_config)
                    .with_batch_size(input.batch_size);
                chunk_inputs.push_back(std::move(ci));
            }
        }

        // Process each chunk inline (process() is synchronous)
        std::vector<ChunkIndexerOutput> results;
        results.reserve(chunk_inputs.size());

        for (auto& ci : chunk_inputs) {
            ChunkIndexerUtility idx;
            results.push_back(idx.process(ci));
        }

        // 7. Persist to .midx
        ManifestIndexDatabase midx(midx_path);
        midx.init_schema();

        std::uint64_t file_hash = 0;
        if (fs::exists(input.file_path)) {
            file_hash =
                static_cast<std::uint64_t>(fs::file_size(input.file_path));
        }
        int fid = midx.get_or_create_file_info(input.file_path, file_hash);

        midx.begin_transaction();
        try {
            std::size_t total_events = 0;

            for (auto& r : results) {
                if (!r.success) continue;

                total_events += r.events_processed;

                for (const auto& g : r.event_line_groups) {
                    queries::insert_event_range(midx.db(), fid,
                                                r.checkpoint_idx, g.cat, g.name,
                                                g.line_numbers);
                }

                for (const auto& g : r.metadata_line_groups) {
                    queries::insert_metadata_lines(midx.db(), fid,
                                                   r.checkpoint_idx,
                                                   g.meta_type, g.line_numbers);
                }
            }

            midx.commit_transaction();

            output.success = true;
            output.events_processed = total_events;
            output.chunks_processed = results.size();

            DFTRACER_UTILS_LOG_INFO(
                "Persisted manifest index for %s "
                "(%zu chunks)",
                input.file_path.c_str(), results.size());
        } catch (const std::exception& e) {
            output.error_message =
                std::string("Failed to persist manifest index: ") + e.what();
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to persist manifest index for "
                "%s: %s",
                input.file_path.c_str(), e.what());
        }
    } catch (const std::exception& e) {
        output.error_message = e.what();
        DFTRACER_UTILS_LOG_ERROR("ManifestIndexBuilder failed for %s: %s",
                                 input.file_path.c_str(), e.what());
    }

    return output;
}

}  // namespace
   // dftracer::utils::utilities::composites::dft::indexing
