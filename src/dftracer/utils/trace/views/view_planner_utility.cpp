#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_planner_utility.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::trace::views {

namespace plan = dftracer::utils::index::plan;
using dftracer::utils::index::store::IndexDatabase;
using dftracer::utils::index::store::internal::get_logical_path;

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

ViewPlannerInput& ViewPlannerInput::with_time_range(double b, double e) {
    time_range = {b, e};
    return *this;
}

ViewPlannerInput& ViewPlannerInput::with_scan_all_chunks(bool v) {
    scan_all_chunks = v;
    return *this;
}

coro::CoroTask<Result<ViewPlannerOutput>> ViewPlannerUtility::operator()(
    const ViewPlannerInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("build view");
    ViewPlannerOutput output;

    std::uint64_t total_checkpoints =
        (input.num_checkpoints == 0) ? 1 : input.num_checkpoints;
    output.total_checkpoints = total_checkpoints;

    // Real per-chunk offsets are required: gzip members are non-uniformly
    // sized, so a uniform ckpt_idx*bytes_per estimate decodes the wrong bytes.
    std::vector<std::uint64_t> candidate_checkpoints;
    std::vector<dftracer::utils::index::gzip::ChunkSpan> chunk_spans;
    const bool indexed = !input.index_path.empty();
    if (indexed) {
        try {
            // Read-only: TraceIndex already holds the shared index open
            // read-only; a read-write open would fail to upgrade.
            IndexDatabase idx_db(
                input.index_path,
                dftracer::utils::index::store::IndexOpenMode::ReadOnly);
            if (!input.scan_all_chunks) {
                // The query and window describe data events; metadata the
                // scan returns lives in chunks of its own.
                const auto metadata =
                    input.view.filter_metadata    ? plan::MetadataUse::RECORDS
                    : input.view.metadata_records ? plan::MetadataUse::ALL
                    : input.view.include_metadata ? plan::MetadataUse::EVERY
                                                  : plan::MetadataUse::NONE;
                auto pruned = co_await plan::prune_file(plan::PruneRequest{
                    input.index_path, input.file_path,
                    input.view.query ? &*input.view.query : nullptr, &idx_db,
                    input.time_range, total_checkpoints,
                    !input.view.window_overlap, metadata});
                if (!pruned)
                    co_return unexpected<DFTUtilsError>(pruned.error());
                if (pruned->total_chunks > 0) {
                    total_checkpoints = pruned->total_chunks;
                    output.total_checkpoints = total_checkpoints;
                }
                if (!pruned->file_may_match) {
                    output.file_may_match = false;
                    output.skipped_checkpoints = total_checkpoints;
                    co_return output;
                }
                if (pruned->all_chunks) {
                    for (std::uint64_t i = 0; i < total_checkpoints; ++i)
                        candidate_checkpoints.push_back(i);
                } else {
                    candidate_checkpoints = std::move(pruned->candidates);
                }
            }
            int fid =
                idx_db.get_file_info_id(get_logical_path(input.file_path));
            if (fid >= 0 &&
                (!candidate_checkpoints.empty() || input.scan_all_chunks))
                chunk_spans = idx_db.query_chunk_spans(fid);
        } catch (const std::exception& e) {
            // Fail loudly: swallowing this leaves chunk_spans empty, which
            // reads downstream as a prune and silently drops the file.
            co_return make_error(
                ErrorCode::INDEXER,
                std::string("ViewPlanner: index read failed for ") +
                    input.file_path + ": " + e.what());
        }
        if (input.scan_all_chunks) {
            for (std::uint64_t i = 0; i < chunk_spans.size(); ++i)
                candidate_checkpoints.push_back(i);
        }
    } else {
        for (std::uint64_t i = 0; i < total_checkpoints; ++i)
            candidate_checkpoints.push_back(i);
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

        if (ckpt_idx < chunk_spans.size()) {
            const auto& span = chunk_spans[ckpt_idx];
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
