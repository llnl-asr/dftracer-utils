#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VISITORS_MANIFEST_VISITOR_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VISITORS_MANIFEST_VISITOR_H

#include <dftracer/utils/utilities/composites/dft/dft_event_visitor.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::indexer {
class IndexBatchSink;
}

namespace dftracer::utils::utilities::composites::dft::visitors {

class ManifestVisitor : public DftEventVisitor {
   public:
    ManifestVisitor() = default;

    void begin(std::size_t num_checkpoints) override;
    void on_checkpoint(std::size_t checkpoint_idx) override;
    void on_event(const EventRecord& record) override;

    std::unique_ptr<DftEventVisitor> create_parallel_slice() const override;
    void merge_parallel_slice(DftEventVisitor& slice) override;
    void set_line_offset(std::size_t offset) override { line_offset_ = offset; }
    std::size_t parallel_event_count() const override { return event_count_; }

    void finalize(indexer::IndexBatchSink& writer, int file_id);

    /// Emit per-checkpoint event/metadata line records and clear the
    /// vectors. Used for mid-chunk slice rotation.
    void flush_per_checkpoint_to_sink(indexer::IndexBatchSink& sink,
                                      int file_id);

    /// Emit file-level records (observed pids). Call once at end-of-file.
    void finalize_file_to_sink(indexer::IndexBatchSink& sink, int file_id);

   private:
    void ensure_chunk(std::size_t checkpoint_idx);

    using EventKey = std::pair<std::string, std::string>;
    using LineVec = std::vector<std::uint32_t>;

    std::vector<std::map<EventKey, LineVec>> event_lines_;
    std::vector<std::map<std::string, LineVec>> metadata_lines_;
    std::unordered_set<std::uint64_t> observed_pids_;
    std::size_t event_count_ = 0;
    std::size_t line_offset_ = 0;
    std::size_t base_idx_ = 0;
};

}  // namespace dftracer::utils::utilities::composites::dft::visitors

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VISITORS_MANIFEST_VISITOR_H
