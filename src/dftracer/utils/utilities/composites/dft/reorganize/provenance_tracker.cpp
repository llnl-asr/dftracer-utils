#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/provenance_tracker.h>
#include <dftracer/utils/utilities/indexer/provenance_database.h>

namespace dftracer::utils::utilities::composites::dft::reorganize {

void ProvenanceTracker::record(int source_file_idx, int checkpoint_idx,
                               int output_chunk_idx, int output_line_start,
                               int output_line_end, int event_count) {
    records_.push_back(ProvenanceRecord{source_file_idx, checkpoint_idx,
                                        output_chunk_idx, output_line_start,
                                        output_line_end, event_count});
}

void ProvenanceTracker::flush_to_db(
    const ExtractionPlan& plan, const std::string& group_name,
    const std::string& group_query,
    const std::vector<fileio::ChunkInfo>& chunks,
    const std::string& /*output_dir*/) {
    using indexer::ProvenanceDatabase;

    for (const auto& chunk : chunks) {
        std::string pidx_path = chunk.path + ".pidx";

        try {
            ProvenanceDatabase pdb(pidx_path);
            pdb.init_schema();

            std::uint64_t out_hash = 0;
            if (fs::exists(chunk.path)) {
                out_hash =
                    static_cast<std::uint64_t>(fs::file_size(chunk.path));
            }
            int fid = pdb.get_or_create_file_info(chunk.path, out_hash);

            pdb.begin_transaction();

            pdb.insert_info("version", "2.0");
            pdb.insert_info("tool", "dftracer_organize");
            pdb.insert_group(group_name, group_query);

            for (std::size_t si = 0; si < plan.source_files.size(); ++si) {
                const auto& src = plan.source_files[si];
                pdb.insert_source(fid, static_cast<int>(si), src.file_path,
                                  static_cast<int>(src.num_checkpoints));
            }

            for (const auto& rec : records_) {
                if (rec.output_chunk_idx != chunk.chunk_index) continue;
                pdb.insert_segment(rec.source_file_idx, rec.checkpoint_idx,
                                   rec.output_line_start, rec.output_line_end,
                                   rec.event_count);
            }

            pdb.commit_transaction();
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR("Provenance write failed for %s: %s",
                                     chunk.path.c_str(), e.what());
        }
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::reorganize
