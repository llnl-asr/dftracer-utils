// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>

namespace dftracer::utils::utilities::fileio {

coro::CoroTask<dstd::SharedPtr<reader::internal::Reader>>
IndexedFileReaderUtility::operator()(const IndexedReadInput& input) const {
    // Validate input
    if (!dftracer::utils::common::input_exists(input.file_path)) {
        throw common::DFTUtilsException(
            common::ErrorCode::NotFound,
            "File does not exist: " + input.file_path);
    }

    // keep std: the index root helpers return std::string
    const std::string normalized_index_path =
        input.index_path.empty()
            ? dftracer::utils::trace::internal::determine_index_path(
                  input.file_path, "")
            : dftracer::utils::index::store::internal::normalize_index_root(
                  input.index_path);

    // Step 1: Check if index needs to be built/rebuilt
    bool need_build =
        !dftracer::utils::common::input_exists(
            dftracer::utils::index::store_of(normalized_index_path)) ||
        input.force_rebuild;

    if (need_build) {
        // Remove old index if forcing rebuild
        if (input.force_rebuild &&
            dftracer::utils::common::input_exists(
                dftracer::utils::index::store_of(normalized_index_path))) {
            // Force rebuild drops the handle this process keeps before removing
            // the store. The other artifacts in the root stay.
            // keep std: store_of returns std::string
            const std::string store =
                dftracer::utils::index::store_of(normalized_index_path);
            dftracer::utils::index::release_root(normalized_index_path);
            common::unwrap(dstd::fs::remove_all(dstd::PathView(store)));
            common::forget_input(store);
        }

        // Build new index
        // keep std: CheckpointIndexerFactory::create takes std::string
        const std::string file_path_std(input.file_path.data(),
                                        input.file_path.size());
        // keep std: CheckpointIndexerFactory::create takes std::string
        const std::string index_path_std(input.index_path.data(),
                                         input.index_path.size());
        auto indexer =
            dftracer::utils::index::gzip::CheckpointIndexerFactory::create(
                file_path_std, index_path_std, input.checkpoint_size, true);
        co_await indexer->build_async();
    }

    // Step 2: Create and return Reader
    co_return reader::internal::ReaderFactory::create(input.file_path,
                                                      normalized_index_path);
}

}  // namespace dftracer::utils::utilities::fileio
