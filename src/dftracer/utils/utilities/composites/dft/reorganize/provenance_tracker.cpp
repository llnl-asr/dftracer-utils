#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/rocksdb/async.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/provenance_tracker.h>
#include <dftracer/utils/utilities/indexer/internal/transaction_scope.h>
#include <dftracer/utils/utilities/indexer/provenance_database.h>

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
        auto provenance_path = std::make_shared<std::string>(
            indexer::determine_provenance_index_path(chunk.path));
        const auto* plan_ptr = &plan;
        const auto* group_name_ptr = &group_name;
        const auto* group_query_ptr = &group_query;
        const auto* chunk_ptr = &chunk;
        const auto* records_ptr = &records_;

        try {
            co_await rocksdb::run([plan_ptr, group_name_ptr, group_query_ptr,
                                   chunk_ptr, records_ptr, provenance_path] {
                ProvenanceDatabase pdb(*provenance_path);
                pdb.init_schema();

                std::uint64_t out_hash = 0;
                if (fs::exists(chunk_ptr->path)) {
                    out_hash = static_cast<std::uint64_t>(
                        fs::file_size(chunk_ptr->path));
                }
                int fid =
                    pdb.get_or_create_file_info(chunk_ptr->path, out_hash);

                indexer::internal::TransactionScope txn(pdb);
                pdb.insert_info(fid, "version", "2.0");
                pdb.insert_info(fid, "tool", "dftracer_organize");
                pdb.insert_group(fid, *group_name_ptr, *group_query_ptr);

                for (std::size_t si = 0; si < plan_ptr->source_files.size();
                     ++si) {
                    const auto& src = plan_ptr->source_files[si];
                    pdb.insert_source(fid, static_cast<int>(si), src.file_path,
                                      static_cast<int>(src.num_checkpoints));
                }

                for (const auto& rec : *records_ptr) {
                    if (rec.output_chunk_idx != chunk_ptr->chunk_index)
                        continue;
                    pdb.insert_segment(fid, rec.source_file_idx,
                                       rec.checkpoint_idx,
                                       rec.output_line_start,
                                       rec.output_line_end, rec.event_count);
                }

                txn.commit();
            });
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR("Provenance write failed for %s: %s",
                                     chunk.path.c_str(), e.what());
        }
    }

    co_return;
}

}  // namespace dftracer::utils::utilities::composites::dft::reorganize
