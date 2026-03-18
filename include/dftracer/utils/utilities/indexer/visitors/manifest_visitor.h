#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_VISITORS_MANIFEST_VISITOR_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_VISITORS_MANIFEST_VISITOR_H

#include <dftracer/utils/utilities/indexer/index_visitor.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::indexer {

class ManifestVisitor : public IndexVisitor {
   public:
    ManifestVisitor() = default;

    void begin(std::size_t num_checkpoints) override;
    void on_checkpoint(std::size_t checkpoint_idx) override;
    void on_line(std::string_view line, std::size_t checkpoint_idx) override;
    void finalize(IndexDatabase& db, int file_id) override;

   private:
    void ensure_chunk(std::size_t checkpoint_idx);

    using EventKey = std::pair<std::string, std::string>;
    using LineVec = std::vector<std::uint32_t>;

    std::vector<std::map<EventKey, LineVec>> event_lines_;
    std::vector<std::map<std::string, LineVec>> metadata_lines_;
    std::uint32_t chunk_line_ = 0;
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_VISITORS_MANIFEST_VISITOR_H
