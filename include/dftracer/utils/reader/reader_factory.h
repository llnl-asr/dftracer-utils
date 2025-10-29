#ifndef DFTRACER_UTILS_READER_READER_FACTORY_H
#define DFTRACER_UTILS_READER_READER_FACTORY_H

#include <dftracer/utils/core/common/format_detector.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/indexer/indexer_factory.h>
#include <dftracer/utils/reader/reader.h>

#include <memory>
#include <string>
namespace dftracer::utils {

/**
 * Factory for creating appropriate reader implementations based on file format
 */
class ReaderFactory {
   public:
    /**
     * Create a reader for any supported archive format (returns Reader)
     */
    static std::shared_ptr<Reader> create(
        const std::string &archive_path, const std::string &idx_path,
        std::size_t index_ckpt_size = Indexer::DEFAULT_CHECKPOINT_SIZE);

    /**
     * Create a reader using an existing indexer (works with any indexer type)
     */
    static std::shared_ptr<Reader> create(std::shared_ptr<Indexer> indexer);

    /**
     * Check if a reader type is supported for the given format
     */
    static bool is_format_supported(ArchiveFormat format);

   private:
    ReaderFactory() = delete;  // Static-only class
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_READER_READER_FACTORY_H
