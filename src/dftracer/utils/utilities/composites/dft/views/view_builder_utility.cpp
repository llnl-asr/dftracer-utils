#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::views {

using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;

// ViewBuilderInput fluent builders
ViewBuilderInput& ViewBuilderInput::with_view(const ViewDefinition& v) {
    view = v;
    return *this;
}

ViewBuilderInput& ViewBuilderInput::with_file_path(const std::string& path) {
    file_path = path;
    return *this;
}

ViewBuilderInput& ViewBuilderInput::with_index_path(const std::string& path) {
    index_path = path;
    return *this;
}

ViewBuilderInput& ViewBuilderInput::with_uncompressed_size(std::size_t s) {
    uncompressed_size = s;
    return *this;
}

ViewBuilderInput& ViewBuilderInput::with_num_checkpoints(std::size_t n) {
    num_checkpoints = n;
    return *this;
}

ViewBuilderInput& ViewBuilderInput::with_bloom_cache(
    indexing::BloomFilterCache* c) {
    bloom_cache = c;
    return *this;
}

ViewBuilderInput& ViewBuilderInput::with_time_range(double b, double e) {
    time_range = {b, e};
    return *this;
}

coro::CoroTask<Result<ViewBuilderOutput>> ViewBuilderUtility::process(
    const ViewBuilderInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("build view");
    ViewBuilderOutput output;

    std::uint64_t total_checkpoints =
        (input.num_checkpoints == 0) ? 1 : input.num_checkpoints;
    output.total_checkpoints = total_checkpoints;

    std::vector<std::uint64_t> candidate_checkpoints;

    if (input.view.query && !input.index_path.empty()) {
        indexing::ChunkPrunerInput pruner_input{
            input.index_path, input.file_path, *input.view.query,
            input.bloom_cache};
        indexing::ChunkPrunerUtility pruner;
        auto pruner_output = co_await pruner.process(pruner_input);

        if (pruner_output.success) {
            candidate_checkpoints = pruner_output.candidate_checkpoints;
            if (pruner_output.total_checkpoints > 0) {
                total_checkpoints = pruner_output.total_checkpoints;
                output.total_checkpoints = total_checkpoints;
            }

            if (!pruner_output.file_may_match &&
                candidate_checkpoints.empty()) {
                output.file_may_match = false;
                output.skipped_checkpoints = total_checkpoints;
                co_return output;
            }
        } else {
            for (std::uint64_t i = 0; i < total_checkpoints; ++i) {
                candidate_checkpoints.push_back(i);
            }
        }
    } else {
        for (std::uint64_t i = 0; i < total_checkpoints; ++i) {
            candidate_checkpoints.push_back(i);
        }
    }

    // Chunk-level time range skip: query per-chunk time bounds from
    // the bloom index and remove chunks that don't overlap the query.
    if (input.time_range && !input.index_path.empty() &&
        !candidate_checkpoints.empty()) {
        auto [t_begin, t_end] = *input.time_range;
        if (t_begin > 0 || t_end > 0) {
            try {
                IndexDatabase idx_db(input.index_path);
                int fid =
                    idx_db.get_file_info_id(get_logical_path(input.file_path));
                if (fid >= 0) {
                    auto chunk_stats = idx_db.query_chunk_statistics(fid);

                    std::unordered_map<std::uint64_t,
                                       std::pair<std::uint64_t, std::uint64_t>>
                        chunk_time_bounds;
                    chunk_time_bounds.reserve(chunk_stats.size());
                    for (const auto& cs : chunk_stats) {
                        chunk_time_bounds[cs.checkpoint_idx] = {
                            cs.stats.min_timestamp_us,
                            cs.stats.max_timestamp_us};
                    }

                    std::vector<std::uint64_t> time_filtered;
                    time_filtered.reserve(candidate_checkpoints.size());
                    for (auto ckpt : candidate_checkpoints) {
                        auto it = chunk_time_bounds.find(ckpt);
                        if (it == chunk_time_bounds.end()) {
                            time_filtered.push_back(ckpt);
                            continue;
                        }
                        double c_min = static_cast<double>(it->second.first);
                        double c_max = static_cast<double>(it->second.second);
                        if (c_max < t_begin || (t_end > 0 && c_min > t_end)) {
                            continue;
                        }
                        time_filtered.push_back(ckpt);
                    }
                    candidate_checkpoints = std::move(time_filtered);
                }
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_WARN(
                    "ViewBuilder: chunk time filter failed for %s: %s",
                    input.file_path.c_str(), e.what());
                // Keep all candidates on failure
            }
        }
    }

    // Compute byte ranges for each candidate checkpoint
    for (auto ckpt_idx : candidate_checkpoints) {
        ViewChunkCandidate candidate;
        candidate.checkpoint_idx = ckpt_idx;

        if (input.num_checkpoints > 0) {
            std::size_t bytes_per =
                input.uncompressed_size / input.num_checkpoints;
            candidate.start_byte = ckpt_idx * bytes_per;
            candidate.end_byte = (ckpt_idx + 1 == input.num_checkpoints)
                                     ? input.uncompressed_size
                                     : (ckpt_idx + 1) * bytes_per;
        } else {
            candidate.start_byte = 0;
            candidate.end_byte = input.uncompressed_size;
        }

        output.candidates.push_back(candidate);
    }

    output.file_may_match = !output.candidates.empty();
    output.skipped_checkpoints =
        total_checkpoints - candidate_checkpoints.size();
    co_return output;
}

}  // namespace dftracer::utils::utilities::composites::dft::views
