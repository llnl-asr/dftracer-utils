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

#include <stdexcept>

namespace dftracer::utils::utilities::reader::internal {

dstd::SharedPtr<Reader> ReaderFactory::create(dstd::CStringView archive_path,
                                              dstd::StringView index_path,
                                              std::size_t index_ckpt_size) {
    common::ArchiveFormat format = common::FormatDetector::detect(archive_path);

    DFTRACER_UTILS_LOG_DEBUG(
        "ReaderFactory::create_reader - detected format: %d for file: %s",
        static_cast<int>(format), archive_path.c_str());

    switch (format) {
        case common::ArchiveFormat::Gzip:
            return dstd::make_shared<GzipReader>(archive_path, index_path,
                                                 index_ckpt_size);

        default:
            throw ReaderError(
                ReaderError::InvalidArgument,
                common::str_cat("Unsupported archive format for file: ",
                                archive_path.view()));
    }
}

dstd::SharedPtr<Reader> ReaderFactory::create(
    dstd::SharedPtr<dftracer::utils::index::gzip::CheckpointIndexer> indexer) {
    if (!indexer) {
        throw ReaderError(ReaderError::InvalidArgument,
                          "Indexer cannot be null");
    }

    return dstd::make_shared<GzipReader>(indexer);
}

}  // namespace dftracer::utils::utilities::reader::internal
