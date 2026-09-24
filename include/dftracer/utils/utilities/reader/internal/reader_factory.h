#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_FACTORY_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_FACTORY_H

#include <dftracer/utils/core/common/format_detector.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>

#include <memory>
#include <string>
namespace dftracer::utils::utilities::reader::internal {

/**
 * Factory for creating appropriate reader implementations based on file format
 */
class ReaderFactory {
   public:
    /**
     * Create a reader for any supported archive format (returns Reader)
     */
    static std::shared_ptr<Reader> create(
        const std::string &archive_path, const std::string &index_path,
        std::size_t index_ckpt_size = dftracer::utils::index::gzip::
            CheckpointIndexer::DEFAULT_CHECKPOINT_SIZE);

    /**
     * Create a reader using an existing indexer (works with any indexer type)
     */
    static std::shared_ptr<Reader> create(
        std::shared_ptr<dftracer::utils::index::gzip::CheckpointIndexer>
            indexer);

    /**
     * Check if a reader type is supported for the given format
     */
   private:
    ReaderFactory() = delete;  // Static-only class
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_FACTORY_H
