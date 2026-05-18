#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/provenance_tracker.h>
#include <dftracer/utils/utilities/indexer/internal/transaction_scope.h>
#include <dftracer/utils/utilities/indexer/provenance_database.h>

#include <cstdint>
#include <unordered_map>

namespace dftracer::utils::utilities::composites::dft::reorganize {

void ProvenanceTracker::record(int source_file_idx, int checkpoint_idx,
                               int output_chunk_idx, int output_line_start,
                               int output_line_end, int event_count) {
    records_.push_back(ProvenanceRecord{source_file_idx, checkpoint_idx,
                                        output_chunk_idx, output_line_start,
                                        output_line_end, event_count});
}

coro::CoroTask<void> ProvenanceTracker::flush_to_db(
    const ExtractionPlan& plan, const std::string& group_name,
    const std::string& group_query,
    const std::vector<fileio::ChunkInfo>& chunks,
    const std::string& /*output_dir*/) {
    using indexer::ProvenanceDatabase;

    for (const auto& chunk : chunks) {
        try {
            std::string provenance_path =
                indexer::determine_provenance_index_path(chunk.path);
            ProvenanceDatabase pdb(provenance_path);
            pdb.init_schema();

            std::uint64_t out_hash = 0;
            if (fs::exists(chunk.path)) {
                out_hash =
                    static_cast<std::uint64_t>(fs::file_size(chunk.path));
            }
            int fid = pdb.get_or_create_file_info(chunk.path, out_hash);
            pdb.insert_info(fid, "version", "2.0");
            pdb.insert_info(fid, "tool", "dftracer_organize");
            pdb.insert_group(fid, group_name, group_query);

            for (std::size_t si = 0; si < plan.source_files.size(); ++si) {
                const auto& src = plan.source_files[si];
                pdb.insert_source(fid, static_cast<int>(si), src.file_path,
                                  static_cast<int>(src.num_checkpoints));
            }

            std::unordered_map<std::uint64_t, int> seq_counter;
            for (const auto& rec : records_) {
                if (rec.output_chunk_idx != chunk.chunk_index) continue;
                std::uint64_t k =
                    (static_cast<std::uint64_t>(rec.source_file_idx) << 32) |
                    static_cast<std::uint32_t>(rec.checkpoint_idx);
                int seq = seq_counter[k]++;
                pdb.insert_segment(fid, rec.source_file_idx, rec.checkpoint_idx,
                                   seq, rec.output_line_start,
                                   rec.output_line_end, rec.event_count);
            }
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR("Provenance write failed for %s: %s",
                                     chunk.path.c_str(), e.what());
        }
    }

    co_return;
}

}  // namespace dftracer::utils::utilities::composites::dft::reorganize
