#ifndef DFTRACER_UTILS_INDEXER_FACTORY_H
#define DFTRACER_UTILS_INDEXER_FACTORY_H

#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/format_detector.h>
#include <dftracer/utils/indexer/indexer.h>

#include <memory>
#include <string>

namespace dftracer::utils {

/**
 * Factory for creating indexers based on archive format detection
 */
class IndexerFactory {
   public:
    /**
     * Create an indexer for the given archive file.
     * Automatically detects the format (GZIP vs TAR.GZ) and creates the
     * appropriate indexer.
     *
     * @param archive_path Path to the archive file (.gz or .tar.gz)
     * @param idx_path Path to the index file (optional - will be auto-generated
     * if empty)
     * @param checkpoint_size Checkpoint size in bytes
     * @param force Force rebuilding the index even if it exists
     * @return Shared pointer to the appropriate indexer, or nullptr if format
     * not supported
     */
    static std::shared_ptr<Indexer> create(
        const std::string &archive_path, const std::string &idx_path = "",
        std::uint64_t checkpoint_size =
            constants::indexer::DEFAULT_CHECKPOINT_SIZE,
        bool force = false);

    /**
     * Detect the format of an archive file
     *
     * @param archive_path Path to the archive file
     * @return Detected archive format
     */
    static ArchiveFormat detect_format(const std::string &archive_path);

    /**
     * Generate appropriate index file path for the given archive format
     *
     * @param archive_path Path to the archive file
     * @param format Archive format (auto-detected if UNKNOWN)
     * @return Appropriate index file path
     */
    static std::string generate_index_path(
        const std::string &archive_path,
        ArchiveFormat format = ArchiveFormat::UNKNOWN);

   private:
    IndexerFactory() = delete;  // Static-only class
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_INDEXER_FACTORY_H
