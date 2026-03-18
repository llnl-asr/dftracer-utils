#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_VISITOR_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_VISITOR_H

#include <cstddef>
#include <string_view>

namespace dftracer::utils::utilities::indexer {

class IndexDatabase;

class IndexVisitor {
   public:
    virtual ~IndexVisitor() = default;

    virtual void begin(std::size_t num_checkpoints) = 0;

    virtual void on_checkpoint(std::size_t checkpoint_idx) = 0;

    virtual void on_line(std::string_view line, std::size_t checkpoint_idx) = 0;

    virtual void finalize(IndexDatabase& db, int file_id) = 0;
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_VISITOR_H
