#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_INDEXED_FILE_READER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_INDEXED_FILE_READER_UTILITY_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace dftracer::utils::utilities::composites {

/**
 * @brief Workflow utility for managing indexed file reading.
 *
 * This workflow handles:
 * 1. Index existence checking
 * 2. Index building/rebuilding if needed
 * 3. Reader creation from indexed file
 *
 * This encapsulates the common pattern from your binaries where you need to
 * ensure an index exists before creating a Reader.
 *
 * Usage:
 * @code
 * IndexedFileReader reader_workflow;
 * auto reader = reader_workflow.process(
 *     IndexedReadInput{"file.gz", ".dftindex", checkpoint_size, false}
 * );
 * // Now use reader to read lines
 * @endcode
 */
class IndexedFileReaderUtility
    : public utilities::Utility<IndexedReadInput,
                                std::shared_ptr<reader::internal::Reader>> {
   public:
    /**
     * @brief Process index management and create Reader.
     *
     * @param input Index configuration
     * @return Shared pointer to Reader ready for use
     */
    coro::CoroTask<std::shared_ptr<reader::internal::Reader>> process(
        const IndexedReadInput& input) override {
        // Validate input
        if (!fs::exists(input.file_path)) {
            throw std::runtime_error("File does not exist: " + input.file_path);
        }

        const std::string normalized_index_path =
            input.index_path.empty()
                ? dft::internal::determine_index_path(input.file_path, "")
                : indexer::internal::normalize_index_root(input.index_path);

        // Step 1: Check if index needs to be built/rebuilt
        bool need_build =
            !fs::exists(normalized_index_path) || input.force_rebuild;

        if (need_build) {
            // Remove old index if forcing rebuild
            if (input.force_rebuild && fs::exists(normalized_index_path)) {
                // Force rebuild must discard the manager-owned DB instance
                // before removing the root directory so the next open is a
                // true reopen, not a reuse of the previous live handle.
                rocksdb::RocksDBManager::instance().reset(
                    normalized_index_path);
                fs::remove_all(normalized_index_path);
            }

            // Build new index
            auto indexer = dftracer::utils::utilities::indexer::internal::
                IndexerFactory::create(input.file_path, input.index_path,
                                       input.checkpoint_size, true);
            co_await indexer->build_async();
        } else {
            // Check if existing index needs rebuild
            auto indexer = dftracer::utils::utilities::indexer::internal::
                IndexerFactory::create(input.file_path, input.index_path,
                                       input.checkpoint_size, false);

            if (indexer->need_rebuild()) {
                // Rebuild the index
                // Drop the cached DB instance before deleting the store.
                rocksdb::RocksDBManager::instance().reset(
                    normalized_index_path);
                fs::remove_all(normalized_index_path);
                auto new_indexer = dftracer::utils::utilities::indexer::
                    internal::IndexerFactory::create(
                        input.file_path, input.index_path,
                        input.checkpoint_size, true);
                co_await new_indexer->build_async();
            }
        }

        // Step 2: Create and return Reader
        co_return reader::internal::ReaderFactory::create(
            input.file_path, normalized_index_path);
    }
};

}  // namespace dftracer::utils::utilities::composites

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_INDEXED_FILE_READER_UTILITY_H
