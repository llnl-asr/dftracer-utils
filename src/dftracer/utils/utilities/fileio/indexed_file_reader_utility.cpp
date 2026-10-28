#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>

#include <string>

namespace dftracer::utils::utilities::fileio {

coro::CoroTask<std::shared_ptr<reader::internal::Reader>>
IndexedFileReaderUtility::operator()(const IndexedReadInput& input) const {
    // Validate input
    if (!dftracer::utils::input_exists(input.file_path)) {
        throw DFTUtilsException(ErrorCode::NotFound,
                                "File does not exist: " + input.file_path);
    }

    const std::string normalized_index_path =
        input.index_path.empty()
            ? dftracer::utils::trace::internal::determine_index_path(
                  input.file_path, "")
            : dftracer::utils::index::store::internal::normalize_index_root(
                  input.index_path);

    // Step 1: Check if index needs to be built/rebuilt
    bool need_build =
        !input_exists(normalized_index_path) || input.force_rebuild;

    if (need_build) {
        // Remove old index if forcing rebuild
        if (input.force_rebuild && input_exists(normalized_index_path)) {
            // Force rebuild drops the handle this process keeps before
            // removing the root directory, so the next open reads the new
            // directory.
            dftracer::utils::index::store::release_index(normalized_index_path);
            fs::remove_all(normalized_index_path);
            forget_input(normalized_index_path);
        }

        // Build new index
        auto indexer =
            dftracer::utils::index::gzip::CheckpointIndexerFactory::create(
                input.file_path, input.index_path, input.checkpoint_size, true);
        co_await indexer->build_async();
    }

    // Step 2: Create and return Reader
    co_return reader::internal::ReaderFactory::create(input.file_path,
                                                      normalized_index_path);
}

}  // namespace dftracer::utils::utilities::fileio
