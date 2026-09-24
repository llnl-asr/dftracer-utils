#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/query/ast.h>
#include <dftracer/utils/query/fields.h>
#include <dftracer/utils/trace/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_planner_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::trace::views {

using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;

ViewPlannerInput& ViewPlannerInput::with_view(const ViewDefinition& v) {
    view = v;
    return *this;
}

ViewPlannerInput& ViewPlannerInput::with_file_path(const std::string& path) {
    file_path = path;
    return *this;
}

ViewPlannerInput& ViewPlannerInput::with_index_path(const std::string& path) {
    index_path = path;
    return *this;
}

ViewPlannerInput& ViewPlannerInput::with_uncompressed_size(std::size_t s) {
    uncompressed_size = s;
    return *this;
}

ViewPlannerInput& ViewPlannerInput::with_num_checkpoints(std::size_t n) {
    num_checkpoints = n;
    return *this;
}

ViewPlannerInput& ViewPlannerInput::with_bloom_cache(
    indexing::BloomFilterCache* c) {
    bloom_cache = c;
    return *this;
}

ViewPlannerInput& ViewPlannerInput::with_time_range(double b, double e) {
    time_range = {b, e};
    return *this;
}

ViewPlannerInput& ViewPlannerInput::with_scan_all_chunks(bool v) {
    scan_all_chunks = v;
    return *this;
}

ViewPlannerInput& ViewPlannerInput::with_cached_chunks(
    const std::vector<utilities::indexer::ChunkSpan>* spans,
    const std::vector<utilities::indexer::ChunkStatisticsResult>* stats) {
    cached_spans = spans;
    cached_stats = stats;
    return *this;
}

coro::CoroTask<Result<ViewPlannerOutput>> ViewPlannerUtility::operator()(
    const ViewPlannerInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("build view");
    ViewPlannerOutput output;

    std::uint64_t total_checkpoints =
        (input.num_checkpoints == 0) ? 1 : input.num_checkpoints;
    output.total_checkpoints = total_checkpoints;

    std::vector<std::uint64_t> candidate_checkpoints;

    // The pruner opens the index and loads every chunk's bloom filters. When
    // the query constrains only ts (which the time filter below already prunes
    // from cached chunk stats), the pruner can drop nothing extra, so skip it
    // and treat every chunk as a candidate - same result as the pruner's own
    // failure fallback, without the per-file open + bloom load.
    bool query_prunable = false;
    if (input.view.query) {
        for (const auto& f : query::collect_fields(input.view.query->root())) {
            if (f != "ts") {
                query_prunable = true;
                break;
            }
        }
    }

    if (input.scan_all_chunks) {
        // Candidates are filled below once the real checkpoint count is known.
    } else if (query_prunable && !input.index_path.empty()) {
        indexing::ChunkPrunerInput pruner_input{
            input.index_path, input.file_path, *input.view.query,
            input.bloom_cache};
        indexing::ChunkPrunerUtility pruner;
        auto pruner_output = co_await pruner(pruner_input);

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
            // The pruner reports "may match" with no candidates when it had
            // nothing to prune with (an index built without chunk dimension
            // stats or bloom filters, e.g. a basic auto-built index). Empty
            // candidates would otherwise be read as "scan nothing" and drop
            // the whole file, so fall back to scanning every chunk.
            if (candidate_checkpoints.empty()) {
                for (std::uint64_t i = 0; i < total_checkpoints; ++i) {
                    candidate_checkpoints.push_back(i);
                }
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

    // Real per-chunk offsets are required: gzip members are non-uniformly
    // sized, so a uniform ckpt_idx*bytes_per estimate decodes the wrong bytes.
    // Prefer caller-supplied chunk metadata (the server reads it from the
    // immutable index once and caches it); otherwise read the index here.
    std::vector<utilities::indexer::ChunkSpan> chunk_spans_local;
    std::vector<utilities::indexer::ChunkStatisticsResult> chunk_stats_local;
    const std::vector<utilities::indexer::ChunkSpan>* chunk_spans =
        &chunk_spans_local;
    const std::vector<utilities::indexer::ChunkStatisticsResult>* chunk_stats =
        nullptr;
    const bool indexed = !input.index_path.empty() || input.cached_spans;
    if (!candidate_checkpoints.empty() || input.scan_all_chunks) {
        if (input.cached_spans) {
            chunk_spans = input.cached_spans;
            chunk_stats = input.cached_stats;
        } else if (!input.index_path.empty()) {
            try {
                // Read-only: TraceIndex already holds the shared index open
                // read-only; a read-write open would fail to upgrade.
                IndexDatabase idx_db(input.index_path,
                                     dftracer::utils::utilities::indexer::
                                         IndexOpenMode::ReadOnly);
                int fid =
                    idx_db.get_file_info_id(get_logical_path(input.file_path));
                if (fid >= 0) {
                    chunk_spans_local = idx_db.query_chunk_spans(fid);
                    if (input.time_range && !input.scan_all_chunks) {
                        chunk_stats_local = idx_db.query_chunk_statistics(fid);
                        chunk_stats = &chunk_stats_local;
                    }
                }
            } catch (const std::exception& e) {
                // Fail loudly: swallowing this leaves chunk_spans empty, which
                // reads downstream as a prune and silently drops the file.
                co_return make_error(
                    ErrorCode::INDEXER,
                    std::string("ViewPlanner: index read failed for ") +
                        input.file_path + ": " + e.what());
            }
        }

        if (input.scan_all_chunks) {
            for (std::uint64_t i = 0; i < chunk_spans->size(); ++i)
                candidate_checkpoints.push_back(i);
        } else if (input.time_range && chunk_stats) {
            auto [t_begin, t_end] = *input.time_range;
            if (t_begin > 0 || t_end > 0) {
                std::unordered_map<std::uint64_t,
                                   std::pair<std::uint64_t, std::uint64_t>>
                    chunk_time_bounds;
                for (const auto& cs : *chunk_stats)
                    chunk_time_bounds[cs.checkpoint_idx] = {
                        cs.stats.min_timestamp_us, cs.stats.max_timestamp_us};

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
                    // Corrupt bounds (min > max) can't be trusted; keep the
                    // chunk rather than risk dropping its events.
                    if (c_min > c_max) {
                        time_filtered.push_back(ckpt);
                        continue;
                    }
                    if (c_max < t_begin || (t_end > 0 && c_min > t_end))
                        continue;
                    time_filtered.push_back(ckpt);
                }
                candidate_checkpoints = std::move(time_filtered);
            }
        }
    }

    // scan_all_chunks with no usable chunk table: cover the whole file via
    // uniform chunks so the union still spans it.
    if (input.scan_all_chunks && candidate_checkpoints.empty()) {
        for (std::uint64_t i = 0; i < total_checkpoints; ++i)
            candidate_checkpoints.push_back(i);
    }

    // Compute byte ranges from real chunk offsets; fall back to a uniform
    // estimate only when the chunk table is unavailable.
    for (auto ckpt_idx : candidate_checkpoints) {
        ViewChunkCandidate candidate;
        candidate.checkpoint_idx = ckpt_idx;

        if (ckpt_idx < chunk_spans->size()) {
            const auto& span = (*chunk_spans)[ckpt_idx];
            candidate.start_byte = span.uc_offset;
            candidate.end_byte = span.uc_offset + span.uc_size;
        } else if (!indexed) {
            // With no index to seek by, a byte cut re-reads the lines around
            // it, so the file is read as one unit.
            candidate.checkpoint_idx = 0;
            candidate.start_byte = 0;
            candidate.end_byte = input.uncompressed_size;
            output.candidates.push_back(candidate);
            break;
        } else if (input.num_checkpoints > 0) {
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

}  // namespace dftracer::utils::trace::views
