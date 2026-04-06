#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reconstruction_planner.h>
#include <dftracer/utils/utilities/indexer/provenance_database.h>

#include <map>
#include <utility>

namespace dftracer::utils::utilities::composites::dft::reorganize {

namespace {

using dftracer::utils::utilities::indexer::ProvenanceDatabase;

}  // namespace

coro::CoroTask<ReconstructionPlan> ReconstructionPlannerUtility::process(
    const ReconstructionPlannerInput& input) {
    ReconstructionPlan plan;

    for (const auto& reorg_file : input.reorganized_files) {
        std::string provenance_path = internal::determine_provenance_index_path(
            reorg_file, input.index_dir);

        if (!fs::exists(provenance_path)) {
            continue;
        }

        ProvenanceDatabase pdb(
            provenance_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);

        int fid = pdb.get_file_info_id(reorg_file);
        if (fid < 0) continue;

        // Check if this file has provenance
        std::string tool = pdb.query_info(fid, "tool");
        if (tool.empty()) continue;

        // Read sources
        auto sources = pdb.query_sources(fid);
        if (sources.empty()) continue;

        // Build source_idx -> source info map
        std::map<int, ProvenanceDatabase::ProvenanceSource> source_map;
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
        auto segments = pdb.query_all_segments(fid);

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
