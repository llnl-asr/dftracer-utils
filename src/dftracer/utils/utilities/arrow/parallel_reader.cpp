// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/vector.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/utilities/arrow/ipc_reader.h>
#include <dftracer/utils/utilities/arrow/parallel_reader.h>

using namespace dftracer::utils;

namespace dftracer::utils::utilities::arrow {

coro::CoroTask<ArrowFileReadResult> read_arrow_file_async(dstd::String path) {
    ArrowFileReadResult result;
    result.path = path;

    IpcReader reader;
    int rc = reader.open(path);
    if (rc != 0) {
        result.success = false;
        result.error = dstd::fmt<"Failed to open file: {}">(path);
        co_return result;
    }

    *result.batches = reader.read_all();
    for (const auto& batch : *result.batches) {
        result.total_rows += batch.num_rows();
    }

    result.success = true;
    co_return result;
}

coro::CoroTask<ParallelReadResult> read_arrow_files_parallel(
    dstd::Vector<dstd::String> paths) {
    ParallelReadResult result;

    if (paths.empty()) {
        co_return result;
    }

    dstd::Vector<coro::CoroTask<ArrowFileReadResult>> tasks;
    tasks.reserve(paths.size());

    for (auto& path : paths) {
        tasks.push_back(read_arrow_file_async(std::move(path)));
    }

    result.file_results = co_await coro::when_all(std::move(tasks));

    for (const auto& fr : result.file_results) {
        if (fr.success) {
            result.files_read++;
            result.total_rows += fr.total_rows;
            result.total_batches += fr.batches->size();
        } else {
            result.files_failed++;
        }
    }

    co_return result;
}

}  // namespace dftracer::utils::utilities::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
