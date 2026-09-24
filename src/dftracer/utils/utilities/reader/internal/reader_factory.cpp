#include <dftracer/utils/core/common/format_detector.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/reader/error.h>
#include <dftracer/utils/utilities/reader/internal/gzip_reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>

#include <stdexcept>

namespace dftracer::utils::utilities::reader::internal {

std::shared_ptr<Reader> ReaderFactory::create(const std::string &archive_path,
                                              const std::string &index_path,
                                              std::size_t index_ckpt_size) {
    ArchiveFormat format = FormatDetector::detect(archive_path);

    DFTRACER_UTILS_LOG_DEBUG(
        "ReaderFactory::create_reader - detected format: %d for file: %s",
        static_cast<int>(format), archive_path.c_str());

    switch (format) {
        case ArchiveFormat::GZIP:
            return std::make_shared<GzipReader>(archive_path, index_path,
                                                index_ckpt_size);

        default:
            throw ReaderError(
                ReaderError::INVALID_ARGUMENT,
                "Unsupported archive format for file: " + archive_path);
    }
}

std::shared_ptr<Reader> ReaderFactory::create(
    std::shared_ptr<dftracer::utils::index::gzip::CheckpointIndexer> indexer) {
    if (!indexer) {
        throw ReaderError(ReaderError::INVALID_ARGUMENT,
                          "Indexer cannot be null");
    }

    return std::make_shared<GzipReader>(indexer);
}

}  // namespace dftracer::utils::utilities::reader::internal
