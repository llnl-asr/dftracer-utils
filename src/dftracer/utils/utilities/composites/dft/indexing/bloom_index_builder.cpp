#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/sqlite/async.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/index_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_builder.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>

#include <unordered_set>

namespace dftracer::utils::utilities::composites::dft::indexing {

coro::CoroTask<BloomIndexBuildOutput> BloomIndexBuilderUtility::process(
    const BloomIndexBuildInput& input) {
    BloomIndexBuildOutput output;
    output.file_path = input.file_path;

    try {
        // 1. Resolve bidx path
        std::string bidx_path =
            determine_bloom_index_path(input.file_path, input.index_dir);
        output.bidx_path = bidx_path;

        // 2. Check if already indexed with correct dimensions
        if (!input.force_rebuild && fs::exists(bidx_path)) {
            bool skip = co_await dftracer::utils::sqlite::run([&] {
                try {
                    BloomIndexDatabase bidx(bidx_path);
                    bidx.init_schema();
                    int fid = bidx.get_file_info_id(input.file_path);
                    if (fid >= 0) {
                        auto existing_dims =
                            queries::query_index_dimensions(bidx.db(), fid);
                        std::unordered_set<std::string> existing_set(
                            existing_dims.begin(), existing_dims.end());
                        for (const auto& dim : input.dimensions) {
                            if (existing_set.find(dim) == existing_set.end()) {
                                return false;
                            }
                        }
                        return true;
                    }
                } catch (...) {
                    // Database corrupt or unreadable -- rebuild
                }
                return false;
            });
            if (skip) {
                DFTRACER_UTILS_LOG_INFO("Skipping already-indexed file: %s",
                                        input.file_path.c_str());
                output.success = true;
                output.was_skipped = true;
                co_return output;
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
        co_await composites::dft::IndexBuilderUtility{}.process(idx_input);

        // 4. Collect metadata
        auto meta_input =
            composites::dft::MetadataCollectorUtilityInput::from_file(
                input.file_path)
                .with_checkpoint_size(input.checkpoint_size)
                .with_force_rebuild(false)
                .with_index(idx_path);
        auto metadata =
            co_await composites::dft::MetadataCollectorUtility{}.process(
                meta_input);

        if (!metadata.success) {
            output.error_message =
                "Failed to collect metadata for " + input.file_path;
            co_return output;
        }

        // 5. Parallel chunk indexing

        std::size_t file_size = metadata.uncompressed_size;
        std::size_t num_ckpts = metadata.num_checkpoints;

        std::vector<ChunkIndexerInput> chunk_inputs;
        if (num_ckpts == 0) {
            ChunkIndexerInput ci;
            ci.with_file_path(input.file_path)
                .with_idx_path(idx_path)
                .with_checkpoint_size(input.checkpoint_size)
                .with_checkpoint_idx(0)
                .with_byte_range(0, file_size)
                .with_config(input.indexer_config)
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
                    .with_config(input.indexer_config)
                    .with_batch_size(input.batch_size);
                chunk_inputs.push_back(std::move(ci));
            }
        }

        // Process each chunk inline (process() is synchronous)
        std::vector<ChunkIndexerOutput> results;
        results.reserve(chunk_inputs.size());

        for (auto& ci : chunk_inputs) {
            ChunkIndexerUtility idx;
            results.push_back(co_await idx.process(ci));
        }

        // 7. Persist to .bidx
        co_await dftracer::utils::sqlite::run([&] {
            BloomIndexDatabase bidx(bidx_path);
            bidx.init_schema();

            std::uint64_t file_hash = 0;
            if (fs::exists(input.file_path)) {
                file_hash =
                    static_cast<std::uint64_t>(fs::file_size(input.file_path));
            }
            int fid = bidx.get_or_create_file_info(input.file_path, file_hash);

            bidx.begin_transaction();
            try {
                std::unordered_map<std::string, BloomFilter> file_blooms;
                HashResolutions all_hr;
                std::size_t total_events = 0;

                for (auto& r : results) {
                    if (!r.success) continue;

                    total_events += r.events_processed;

                    // Chunk bloom filters
                    for (auto& [dim, bloom] : r.bloom_filters) {
                        auto blob = bloom.serialize();
                        queries::insert_chunk_bloom_filter(
                            bidx.db(), fid, r.checkpoint_idx, dim, blob.data(),
                            static_cast<int>(blob.size()), bloom.num_entries());

                        auto it = file_blooms.find(dim);
                        if (it == file_blooms.end()) {
                            file_blooms.emplace(dim, std::move(bloom));
                        } else {
                            it->second.merge_from(bloom);
                        }
                    }

                    // Chunk statistics
                    queries::insert_chunk_statistics(
                        bidx.db(), fid, r.checkpoint_idx, r.statistics);

                    // Hash resolutions
                    for (auto& [dim, resolutions] : r.hash_resolutions) {
                        for (auto& [hash, resolved] : resolutions) {
                            all_hr[dim][hash] = resolved;
                        }
                    }
                }

                // File-level bloom filters
                for (auto& [dim, bloom] : file_blooms) {
                    auto blob = bloom.serialize();
                    queries::insert_file_bloom_filter(
                        bidx.db(), fid, dim, blob.data(),
                        static_cast<int>(blob.size()), bloom.num_entries());
                }

                // Hash resolutions
                for (const auto& [dim, resolutions] : all_hr) {
                    for (const auto& [hash, resolved] : resolutions) {
                        queries::insert_hash_resolution(bidx.db(), fid, dim,
                                                        hash, resolved);
                    }
                }

                // Index dimensions
                for (const auto& dim : input.dimensions) {
                    queries::insert_index_dimension(bidx.db(), fid, dim);
                }

                bidx.commit_transaction();

                output.success = true;
                output.events_processed = total_events;
                output.chunks_processed = results.size();

                DFTRACER_UTILS_LOG_INFO(
                    "Persisted bloom index for %s "
                    "(%zu chunks, %zu dimensions)",
                    input.file_path.c_str(), results.size(),
                    input.dimensions.size());
            } catch (const std::exception& e) {
                output.error_message =
                    std::string("Failed to persist bloom index: ") + e.what();
                DFTRACER_UTILS_LOG_ERROR(
                    "Failed to persist bloom index for %s: %s",
                    input.file_path.c_str(), e.what());
            }
        });
    } catch (const std::exception& e) {
        output.error_message = e.what();
        DFTRACER_UTILS_LOG_ERROR("BloomIndexBuilder failed for %s: %s",
                                 input.file_path.c_str(), e.what());
    }

    co_return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
