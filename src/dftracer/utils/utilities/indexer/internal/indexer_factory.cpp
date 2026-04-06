#include <dftracer/utils/core/common/format_detector.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/indexer/internal/tar/tar_indexer.h>

namespace dftracer::utils::utilities::indexer::internal {

std::shared_ptr<Indexer> IndexerFactory::create(const std::string &archive_path,
                                                const std::string &index_path,
                                                std::uint64_t checkpoint_size,
                                                bool force) {
    ArchiveFormat format = FormatDetector::detect(archive_path);
    std::string final_idx_path = index_path.empty()
                                     ? generate_index_path(archive_path, format)
                                     : index_path;

    switch (format) {
        case ArchiveFormat::GZIP:
            return std::make_shared<gzip::GzipIndexer>(
                archive_path, final_idx_path, checkpoint_size, force);

        case ArchiveFormat::TAR_GZ:
            return std::make_shared<tar::TarIndexer>(
                archive_path, final_idx_path, checkpoint_size, force);

        case ArchiveFormat::UNKNOWN:
        default:
            DFTRACER_UTILS_LOG_ERROR(
                "Unsupported or unrecognized archive format for file: %s",
                archive_path.c_str());
            return nullptr;
    }
}

ArchiveFormat IndexerFactory::detect_format(const std::string &archive_path) {
    return FormatDetector::detect(archive_path);
}

std::string IndexerFactory::generate_index_path(const std::string &archive_path,
                                                ArchiveFormat format) {
    if (format == ArchiveFormat::UNKNOWN) {
        format = FormatDetector::detect(archive_path);
    }

    switch (format) {
        case ArchiveFormat::GZIP:
        case ArchiveFormat::TAR_GZ:
            return composites::dft::internal::determine_index_path(archive_path,
                                                                   "");

        case ArchiveFormat::UNKNOWN:
        default:
            DFTRACER_UTILS_LOG_WARN(
                "Unknown format for %s, using root-local .dftindex",
                archive_path.c_str());
            return composites::dft::internal::determine_index_path(archive_path,
                                                                   "");
    }
}

}  // namespace dftracer::utils::utilities::indexer::internal
