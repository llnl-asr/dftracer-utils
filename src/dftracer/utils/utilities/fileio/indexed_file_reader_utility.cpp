// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>

namespace dftracer::utils::utilities::fileio {

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
            : dftracer::utils::index::normalize_index_root(input.index_path);

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
            const dstd::String store =
                dftracer::utils::index::store_of(normalized_index_path);
            dftracer::utils::index::release_root(normalized_index_path);
            auto removed = dstd::fs::remove_all(dstd::PathView(store));
            if (!removed.has_value()) co_return removed.error();
            common::forget_input(store);
        }

        // Build new index. The indexer still reports a failure as an
        // exception.
        dstd::SharedPtr<dftracer::utils::index::gzip::CheckpointIndexer>
            indexer;
        try {
            indexer =
                dftracer::utils::index::gzip::CheckpointIndexerFactory::create(
                    input.file_path, input.index_path, input.checkpoint_size,
                    true);
        } catch (...) {
            co_return common::error_of_exception(std::current_exception());
        }
        if (!indexer) {
            co_return common::make_error<"No indexer for file: {}">(
                common::ErrorCode::InvalidArgument, input.file_path);
        }
        auto building = indexer->build_async();
        auto built = co_await coro::settle(std::move(building));
        if (built.stopped) co_await coro::stop_here();
        if (built.exception) {
            co_return common::error_of_exception(built.exception);
        }
    }

    // Step 2: Create and return Reader
    co_return reader::internal::ReaderFactory::create(input.file_path,
                                                      normalized_index_path);
}

}  // namespace dftracer::utils::utilities::fileio
