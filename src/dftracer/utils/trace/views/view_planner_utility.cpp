// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_planner_utility.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::trace::views {

namespace plan = dftracer::utils::index::plan;
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

ViewPlannerInput& ViewPlannerInput::with_snapshot(const index::Snapshot* s) {
    snapshot = s;
    return *this;
}

coro::CoroTask<common::Result<ViewPlannerOutput>>
ViewPlannerUtility::operator()(const ViewPlannerInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("build view");
    ViewPlannerOutput output;

    std::uint64_t total_checkpoints =
        (input.num_checkpoints == 0) ? 1 : input.num_checkpoints;
    output.total_checkpoints = total_checkpoints;

    // Real per-chunk offsets are required: gzip members are non-uniformly
    // sized, so a uniform ckpt_idx*bytes_per estimate decodes the wrong bytes.
    std::vector<std::uint64_t> candidate_checkpoints;
    dstd::Vector<dftracer::utils::index::gzip::ChunkSpan> chunk_spans;
    const bool indexed = !input.index_path.empty();
    if (indexed) {
        try {
            dstd::Optional<dftracer::utils::index::Snapshot> owned;
            if (input.snapshot == nullptr)
                owned.emplace(
                    common::unwrap(plan::open_snapshot(input.index_path)));
            const dftracer::utils::index::Snapshot& snapshot =
                input.snapshot != nullptr ? *input.snapshot : *owned;
            if (!input.scan_all_chunks) {
                // The query and window describe data events; metadata the
                // scan returns lives in chunks of its own.
                const auto metadata =
                    input.view.filter_metadata    ? plan::MetadataUse::Records
                    : input.view.metadata_records ? plan::MetadataUse::All
                    : input.view.include_metadata ? plan::MetadataUse::Every
                                                  : plan::MetadataUse::None;
                // Named, not built inside co_await: GCC 12 destroys such a
                // temporary twice.
                plan::PruneRequest request{
                    dstd::String(dstd::StringView(input.index_path)),
                    dstd::String(dstd::StringView(input.file_path)),
                    input.view.query ? &*input.view.query : nullptr,
                    &snapshot,
                    input.time_range,
                    total_checkpoints,
                    !input.view.window_overlap,
                    metadata};
                auto pruned = co_await plan::prune_file(std::move(request));
                if (!pruned)
                    co_return dstd::Unexpected(std::move(pruned).error());
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
                    candidate_checkpoints.assign(pruned->candidates.begin(),
                                                 pruned->candidates.end());
                }
            }
            const auto file = snapshot.file(get_logical_path(input.file_path));
            if (file &&
                (!candidate_checkpoints.empty() || input.scan_all_chunks))
                chunk_spans = common::unwrap(
                    dftracer::utils::index::spans(snapshot, *file));
        } catch (const std::exception& e) {
            // Fail loudly: swallowing this leaves chunk_spans empty, which
            // reads downstream as a prune and silently drops the file.
            co_return common::make_error(
                common::ErrorCode::Indexer,
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

    // Byte ranges come from real chunk offsets. A uniform cut of several chunks
    // would decode the wrong bytes, so a missing table is an error.
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
        } else if (total_checkpoints == 1) {
            // One chunk is the whole file, so no byte cut can misplace it.
            DFTRACER_UTILS_LOG_DEBUG(
                "ViewPlanner: %s has no chunk table entry; read it whole",
                input.file_path.c_str());
            candidate.start_byte = 0;
            candidate.end_byte = input.uncompressed_size;
        } else {
            co_return common::make_error(
                common::ErrorCode::Indexer,
                std::string("ViewPlanner: the chunk table of ") +
                    input.file_path + " has " +
                    std::to_string(chunk_spans.size()) +
                    " chunks and cannot place chunk " +
                    std::to_string(ckpt_idx));
        }

        output.candidates.push_back(candidate);
    }

    output.file_may_match = !output.candidates.empty();
    output.skipped_checkpoints =
        total_checkpoints - candidate_checkpoints.size();
    co_return output;
}

}  // namespace dftracer::utils::trace::views
