#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_PROVENANCE_TRACKER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_PROVENANCE_TRACKER_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>
#include <dftracer/utils/utilities/fileio/chunk_writer.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::reorganize {

struct ProvenanceRecord {
    int source_file_idx;
    int checkpoint_idx;
    int output_chunk_idx;
    int output_line_start;
    int output_line_end;
    int event_count;
};

class ProvenanceTracker {
   public:
    ProvenanceTracker() = default;

    void record(int source_file_idx, int checkpoint_idx, int output_chunk_idx,
                int output_line_start, int output_line_end, int event_count);

    coro::CoroTask<void> flush_to_db(
        const ExtractionPlan& plan, const std::string& group_name,
        const std::string& group_query,
        const std::vector<fileio::ChunkInfo>& chunks,
        const std::string& output_dir);

    std::size_t record_count() const { return records_.size(); }
    const std::vector<ProvenanceRecord>& records() const { return records_; }

   private:
    std::vector<ProvenanceRecord> records_;
};

}  // namespace dftracer::utils::utilities::composites::dft::reorganize

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_PROVENANCE_TRACKER_H
