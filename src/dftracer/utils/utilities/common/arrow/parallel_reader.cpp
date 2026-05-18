#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/common/arrow/ipc_reader.h>
#include <dftracer/utils/utilities/common/arrow/parallel_reader.h>

#include <exception>

namespace dftracer::utils::utilities::common::arrow {

using dftracer::utils::coro::CoroTask;
using dftracer::utils::coro::when_all;

CoroTask<ArrowFileReadResult> read_arrow_file_async(std::string path) {
    ArrowFileReadResult result;
    result.path = path;

    try {
        IpcReader reader;
        int rc = reader.open(path);
        if (rc != 0) {
            result.success = false;
            result.error = "Failed to open file: " + path;
            co_return result;
        }

        *result.batches = reader.read_all();
        for (const auto& batch : *result.batches) {
            result.total_rows += batch.num_rows();
        }

        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.error = e.what();
    }

    co_return result;
}

CoroTask<ParallelReadResult> read_arrow_files_parallel(
    std::vector<std::string> paths) {
    ParallelReadResult result;

    if (paths.empty()) {
        co_return result;
    }

    std::vector<CoroTask<ArrowFileReadResult>> tasks;
    tasks.reserve(paths.size());

    for (auto& path : paths) {
        tasks.push_back(read_arrow_file_async(std::move(path)));
    }

    result.file_results = co_await when_all(std::move(tasks));

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

CoroTask<ParallelReadResult> read_arrow_files_streaming(
    CoroScope& /*scope*/, std::vector<std::string> paths,
    FileResultCallback callback) {
    if (paths.empty()) {
        co_return ParallelReadResult{};
    }

    std::vector<CoroTask<ArrowFileReadResult>> tasks;
    tasks.reserve(paths.size());

    for (auto& path : paths) {
        tasks.push_back(read_arrow_file_async(std::move(path)));
    }

    auto results = co_await when_all(std::move(tasks));

    ParallelReadResult summary;
    bool cancelled = false;

    for (auto& result : results) {
        if (result.success) {
            summary.files_read++;
            summary.total_rows += result.total_rows;
            summary.total_batches += result.batches->size();
        } else {
            summary.files_failed++;
        }

        if (!cancelled && !callback(std::move(result))) {
            cancelled = true;
        }
    }

    co_return summary;
}

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
