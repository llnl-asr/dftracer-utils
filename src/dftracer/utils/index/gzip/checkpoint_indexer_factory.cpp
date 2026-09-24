#include <dftracer/utils/core/common/format_detector.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/trace/internal/utils.h>

namespace dftracer::utils::index::gzip {

std::shared_ptr<CheckpointIndexer> CheckpointIndexerFactory::create(
    const std::string &archive_path, const std::string &index_path,
    std::uint64_t checkpoint_size, bool force) {
    ArchiveFormat format = FormatDetector::detect(archive_path);
    std::string final_idx_path = index_path.empty()
                                     ? generate_index_path(archive_path, format)
                                     : index_path;

    switch (format) {
        case ArchiveFormat::GZIP:
            return std::make_shared<index::gzip::GzipIndexer>(
                archive_path, final_idx_path, checkpoint_size, force);

        case ArchiveFormat::UNKNOWN:
        default:
            DFTRACER_UTILS_LOG_ERROR(
                "Unsupported or unrecognized archive format for file: %s",
                archive_path.c_str());
            return nullptr;
    }
}

ArchiveFormat CheckpointIndexerFactory::detect_format(
    const std::string &archive_path) {
    return FormatDetector::detect(archive_path);
}

std::string CheckpointIndexerFactory::generate_index_path(
    const std::string &archive_path, ArchiveFormat format) {
    if (format == ArchiveFormat::UNKNOWN) {
        format = FormatDetector::detect(archive_path);
    }

    switch (format) {
        case ArchiveFormat::GZIP:
            return trace::internal::determine_index_path(archive_path, "");

        case ArchiveFormat::UNKNOWN:
        default:
            DFTRACER_UTILS_LOG_WARN(
                "Unknown format for %s, using root-local .dftindex",
                archive_path.c_str());
            return trace::internal::determine_index_path(archive_path, "");
    }
}

}  // namespace dftracer::utils::index::gzip
