// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/format_detector.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/reader/error.h>
#include <dftracer/utils/utilities/reader/internal/gzip_reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>

namespace dftracer::utils::utilities::reader::internal {

dstd::Result<dstd::SharedPtr<Reader>> ReaderFactory::create(
    dstd::CStringView archive_path, dstd::StringView index_path,
    std::size_t index_ckpt_size) {
    common::ArchiveFormat format = common::FormatDetector::detect(archive_path);

    DFTRACER_UTILS_LOG_DEBUG(
        "ReaderFactory::create_reader - detected format: {} for file: {}",
        static_cast<int>(format), archive_path);

    switch (format) {
        case common::ArchiveFormat::Gzip: {
            dstd::SharedPtr<dftracer::utils::index::gzip::CheckpointIndexer>
                indexer;
            const dstd::String index(index_path);
            auto created =
                dftracer::utils::index::gzip::CheckpointIndexerFactory::
                    try_create(archive_path, index, index_ckpt_size, false);
            if (!created) {
                return reader_error<ReaderErrorType::InitializationError,
                                    "Failed to initialize reader with "
                                    "indexer: {}">(created.error().message());
            }
            indexer = std::move(*created);
            if (!indexer) {
                return reader_error(ReaderErrorType::InitializationError,
                                    "Failed to initialize reader with "
                                    "indexer: no indexer for the file");
            }
            if (const auto loaded = indexer->load(); !loaded) {
                return reader_error<ReaderErrorType::InitializationError,
                                    "Failed to initialize reader with "
                                    "indexer: {}">(loaded.error().message());
            }
            DFTRACER_UTILS_LOG_DEBUG(
                "Successfully created GZIP reader for gz: {} and index: {}",
                archive_path, index_path);
            return dstd::SharedPtr<Reader>(
                dstd::make_shared<GzipReader>(std::move(indexer)));
        }

        default:
            return reader_error<ReaderErrorType::InvalidArgument,
                                "Unsupported archive format for file: {}">(
                archive_path.view());
    }
}

dstd::Result<dstd::SharedPtr<Reader>> ReaderFactory::create(
    dstd::SharedPtr<dftracer::utils::index::gzip::CheckpointIndexer> indexer) {
    if (!indexer) {
        return reader_error(ReaderErrorType::InvalidArgument,
                            "Indexer cannot be null");
    }

    if (const auto loaded = indexer->load(); !loaded) {
        return reader_error<ReaderErrorType::InitializationError,
                            "Failed to initialize reader with indexer: {}">(
            loaded.error().message());
    }
    return dstd::SharedPtr<Reader>(
        dstd::make_shared<GzipReader>(std::move(indexer)));
}

}  // namespace dftracer::utils::utilities::reader::internal
