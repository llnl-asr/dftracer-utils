// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_GZIP_CHECKPOINT_INDEXER_FACTORY_H
#define DFTRACER_UTILS_INDEX_GZIP_CHECKPOINT_INDEXER_FACTORY_H

#include <dftracer/utils/common/archive_format.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/format_detector.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>

namespace dftracer::utils::index::gzip {

/**
 * Factory for creating indexers based on archive format detection
 */
class CheckpointIndexerFactory {
   public:
    /**
     * Create an indexer for the given gzip file.
     *
     * @param archive_path Path to the gzip file (.gz)
     * @param index_path Path to the `.dftindex` store (optional - will be
     * auto-generated if empty)
     * @param checkpoint_size Checkpoint size in bytes
     * @param force Force rebuilding the index even if it exists
     * @return Shared pointer to the appropriate indexer, or nullptr if format
     * not supported; an Indexer error when the path is empty or missing or
     * `checkpoint_size` is 0
     */
    static dstd::Result<dstd::SharedPtr<CheckpointIndexer>> try_create(
        dstd::CStringView archive_path, dstd::CStringView index_path = "",
        std::uint64_t checkpoint_size =
            common::constants::indexer::DEFAULT_CHECKPOINT_SIZE,
        bool force = false);

    /**
     * Detect the format of an archive file
     *
     * @param archive_path Path to the archive file
     * @return Detected archive format
     */
    static common::ArchiveFormat detect_format(dstd::CStringView archive_path);

    /**
     * Generate appropriate index file path for the given archive format
     *
     * @param archive_path Path to the archive file
     * @param format Archive format (auto-detected if UNKNOWN)
     * @return Appropriate index file path
     */
    static dstd::String generate_index_path(
        dstd::CStringView archive_path,
        common::ArchiveFormat format = common::ArchiveFormat::Unknown);

   private:
    CheckpointIndexerFactory() = delete;  // Static-only class
};

}  // namespace dftracer::utils::index::gzip

#endif  // DFTRACER_UTILS_INDEX_GZIP_CHECKPOINT_INDEXER_FACTORY_H
