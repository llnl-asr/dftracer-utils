// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/reader/indexed_file_reader_utility.h>
#include <dftracer/utils/trace/reader/internal/reader_factory.h>

namespace dftracer::utils::trace::reader {

coro::CoroTask<dstd::Result<dstd::SharedPtr<reader::internal::Reader>>>
IndexedFileReaderUtility::operator()(const IndexedReadInput& input) const {
    // Validate input
    if (!dftracer::utils::common::input_exists(input.file_path)) {
        co_return common::make_error<"File does not exist: {}">(
            common::ErrorCode::NotFound, input.file_path);
    }

    const dstd::String normalized_index_path =
        input.index_path.empty()
            ? dftracer::utils::trace::internal::determine_index_path(
                  input.file_path, "")
            : dftracer::utils::trace::index::normalize_index_root(
                  input.index_path);

    // Step 1: Check if index needs to be built/rebuilt
    bool need_build =
        !dftracer::utils::common::input_exists(
            dftracer::utils::trace::index::store_of(normalized_index_path)) ||
        input.force_rebuild;

    if (need_build) {
        // Remove old index if forcing rebuild
        if (input.force_rebuild && dftracer::utils::common::input_exists(
                                       dftracer::utils::trace::index::store_of(
                                           normalized_index_path))) {
            // Force rebuild drops the handle this process keeps before removing
            // the store. The other artifacts in the root stay.
            const dstd::String store =
                dftracer::utils::trace::index::store_of(normalized_index_path);
            dftracer::utils::trace::index::release_root(normalized_index_path);
            auto removed = dstd::fs::remove_all(dstd::PathView(store));
            if (!removed.has_value()) co_return removed.error();
            common::forget_input(store);
        }

        auto created = dftracer::utils::trace::index::gzip::
            CheckpointIndexerFactory::try_create(
                input.file_path, input.index_path, input.checkpoint_size, true);
        if (!created) co_return created.error();
        dstd::SharedPtr<dftracer::utils::trace::index::gzip::CheckpointIndexer>
            indexer = std::move(*created);
        if (!indexer) {
            co_return common::make_error<"No indexer for file: {}">(
                common::ErrorCode::InvalidArgument, input.file_path);
        }
        auto building = indexer->try_build_async();
        auto built = co_await std::move(building);
        if (!built) co_return built.error();
    }

    // Step 2: Create and return Reader
    co_return reader::internal::ReaderFactory::create(input.file_path,
                                                      normalized_index_path);
}

}  // namespace dftracer::utils::trace::reader
