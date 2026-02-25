#include <dftracer/utils/utilities/composites/dft/indexing/manifest_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reconstruction_planner.h>

#include <filesystem>
#include <map>
#include <utility>

namespace dftracer::utils::utilities::composites::dft::reorganize {

namespace {

using indexing::determine_manifest_index_path;
using indexing::ManifestIndexDatabase;
using indexing::queries::ProvenanceSegment;
using indexing::queries::ProvenanceSource;
using indexing::queries::query_all_provenance_segments;
using indexing::queries::query_provenance_info;
using indexing::queries::query_provenance_sources;

}  // namespace

coro::CoroTask<ReconstructionPlan> ReconstructionPlannerUtility::process(
    const ReconstructionPlannerInput& input) {
    ReconstructionPlan plan;

    for (const auto& reorg_file : input.reorganized_files) {
        std::string midx_path =
            determine_manifest_index_path(reorg_file, input.index_dir);

        if (!std::filesystem::exists(midx_path)) {
            continue;
        }

        ManifestIndexDatabase midx(midx_path);
        midx.init_schema();

        int fid = midx.get_file_info_id(reorg_file);
        if (fid < 0) continue;

        // Check if this file has provenance
        std::string tool = query_provenance_info(midx.db(), "tool");
        if (tool.empty()) continue;

        // Read sources
        auto sources = query_provenance_sources(midx.db(), fid);
        if (sources.empty()) continue;

        // Build source_idx -> source info map
        std::map<int, ProvenanceSource> source_map;
        for (const auto& src : sources) {
            source_map[src.source_idx] = src;

            auto& recon = plan.files[src.path];
            if (recon.original_path.empty()) {
                recon.original_path = src.path;
                recon.num_checkpoints = src.num_checkpoints;
                recon.event_hash = src.event_hash;
            }
        }

        // Read all segments
        auto segments = query_all_provenance_segments(midx.db());

        for (const auto& seg : segments) {
            auto src_it = source_map.find(seg.source_idx);
            if (src_it == source_map.end()) continue;

            const auto& src = src_it->second;
            auto& recon = plan.files[src.path];

            ReconstructionSegment rseg;
            rseg.reorg_file = reorg_file;
            rseg.output_line_start = seg.output_line_start;
            rseg.output_line_end = seg.output_line_end;
            rseg.source_checkpoint = seg.source_checkpoint;
            rseg.event_count = seg.event_count;

            recon.checkpoint_segments[seg.source_checkpoint].push_back(
                std::move(rseg));

            plan.total_segments++;
            plan.total_events += seg.event_count;
        }
    }

    co_return plan;
}

}  // namespace
   // dftracer::utils::utilities::composites::dft::reorganize
