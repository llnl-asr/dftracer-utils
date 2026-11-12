// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_CHECKPOINT_INDEXER_HELPERS_H
#define DFTRACER_UTILS_TESTS_CHECKPOINT_INDEXER_HELPERS_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_indexer_factory.h>
#include <task_result.h>

#include <cstdint>

// A test that expects an indexer to be created and built reads the result
// through these, which end the case when it is an error.
namespace dftu_utils_test {

inline dftracer::utils::dstd::SharedPtr<
    dftracer::utils::trace::index::gzip::CheckpointIndexer>
create_indexer(
    dftracer::utils::dstd::CStringView archive_path,
    dftracer::utils::dstd::CStringView index_path = "",
    std::uint64_t checkpoint_size =
        dftracer::utils::common::constants::indexer::DEFAULT_CHECKPOINT_SIZE,
    bool force = false) {
    return ok(dftracer::utils::trace::index::gzip::CheckpointIndexerFactory::
                  try_create(archive_path, index_path, checkpoint_size, force));
}

inline void build_indexer(
    const dftracer::utils::trace::index::gzip::CheckpointIndexer& indexer) {
    ok(indexer.try_build());
}

inline dftracer::utils::coro::CoroTask<void> build_indexer_async(
    const dftracer::utils::trace::index::gzip::CheckpointIndexer& indexer) {
    ok(co_await indexer.try_build_async());
}

}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_CHECKPOINT_INDEXER_HELPERS_H
