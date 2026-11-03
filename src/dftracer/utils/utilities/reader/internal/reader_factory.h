// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_FACTORY_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_FACTORY_H

#include <dftracer/utils/common/format_detector.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>

namespace dftracer::utils::utilities::reader::internal {

/**
 * Factory for creating appropriate reader implementations based on file format
 */
class ReaderFactory {
   public:
    /**
     * Create a reader for any supported archive format (returns Reader)
     */
    static dstd::SharedPtr<Reader> create(
        dstd::CStringView archive_path, dstd::StringView index_path,
        std::size_t index_ckpt_size = dftracer::utils::index::gzip::
            CheckpointIndexer::DEFAULT_CHECKPOINT_SIZE);

    /**
     * Create a reader using an existing indexer (works with any indexer type)
     */
    static dstd::SharedPtr<Reader> create(
        dstd::SharedPtr<dftracer::utils::index::gzip::CheckpointIndexer>
            indexer);

    /**
     * Check if a reader type is supported for the given format
     */
   private:
    ReaderFactory() = delete;  // Static-only class
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_FACTORY_H
