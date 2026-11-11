// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/format_detector.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/trace/index/gzip/gzip_indexer.h>
#include <dftracer/utils/trace/internal/utils.h>

namespace dftracer::utils::trace::index::gzip {

dstd::Result<dstd::SharedPtr<CheckpointIndexer>>
CheckpointIndexerFactory::try_create(dstd::CStringView archive_path,
                                     dstd::CStringView index_path,
                                     std::uint64_t checkpoint_size,
                                     bool force) {
    common::ArchiveFormat format =
        common::FormatDetector::detect(archive_path.view());
    dstd::String final_idx_path =
        index_path.empty() ? generate_index_path(archive_path, format)
                           : dstd::String(index_path.view());

    switch (format) {
        case common::ArchiveFormat::Gzip: {
            const dstd::String path(archive_path.view());
            auto checked = GzipIndexer::check_arguments(path, checkpoint_size);
            if (!checked) return checked.error();
            return dstd::SharedPtr<CheckpointIndexer>(
                dstd::make_shared<trace::index::gzip::GzipIndexer>(
                    path, final_idx_path, checkpoint_size, force));
        }

        case common::ArchiveFormat::Unknown:
        default:
            DFTRACER_UTILS_LOG_ERROR(
                "Unsupported or unrecognized archive format for file: {}",
                archive_path);
            return dstd::SharedPtr<CheckpointIndexer>();
    }
}

common::ArchiveFormat CheckpointIndexerFactory::detect_format(
    dstd::CStringView archive_path) {
    return common::FormatDetector::detect(archive_path.view());
}

dstd::String CheckpointIndexerFactory::generate_index_path(
    dstd::CStringView archive_path, common::ArchiveFormat format) {
    if (format == common::ArchiveFormat::Unknown) {
        format = common::FormatDetector::detect(archive_path.view());
    }

    switch (format) {
        case common::ArchiveFormat::Gzip:
            return trace::internal::determine_index_path(archive_path, "");

        case common::ArchiveFormat::Unknown:
        default:
            DFTRACER_UTILS_LOG_WARN(
                "Unknown format for {}, using root-local .dftindex",
                archive_path);
            return trace::internal::determine_index_path(archive_path, "");
    }
}

}  // namespace dftracer::utils::trace::index::gzip
