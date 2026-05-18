#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_PARALLEL_READER_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_PARALLEL_READER_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils {
class CoroScope;
}

namespace dftracer::utils::utilities::common::arrow {

using dftracer::utils::CoroScope;

/**
 * Result from reading a single Arrow IPC file.
 * Batches are stored in shared_ptr to allow copying through std::shared_future.
 */
struct ArrowFileReadResult {
    std::string path;
    std::shared_ptr<std::vector<ArrowExportResult>> batches;
    std::int64_t total_rows = 0;
    std::string error;
    bool success = true;

    ArrowFileReadResult()
        : batches(std::make_shared<std::vector<ArrowExportResult>>()) {}
};

/**
 * Result from reading multiple Arrow IPC files in parallel.
 */
struct ParallelReadResult {
    std::vector<ArrowFileReadResult> file_results;
    std::int64_t total_rows = 0;
    std::int64_t total_batches = 0;
    std::size_t files_read = 0;
    std::size_t files_failed = 0;
};

/**
 * Read a single Arrow IPC file as a coroutine.
 *
 * @param path Path to the Arrow IPC file.
 * @return ArrowFileReadResult with batches or error.
 */
coro::CoroTask<ArrowFileReadResult> read_arrow_file_async(std::string path);

/**
 * Read multiple Arrow IPC files in parallel.
 *
 * Collects all results before returning. For streaming results as they
 * complete, use read_arrow_files_streaming instead.
 *
 * @param paths List of file paths to read.
 * @return ParallelReadResult with all results.
 */
coro::CoroTask<ParallelReadResult> read_arrow_files_parallel(
    std::vector<std::string> paths);

/**
 * Callback type for streaming file results.
 * Return false to cancel remaining reads.
 */
using FileResultCallback = std::function<bool(ArrowFileReadResult&&)>;

/**
 * Read multiple Arrow IPC files in parallel, streaming results via callback.
 *
 * Results are delivered in completion order (whichever file finishes first).
 * This is more memory-efficient for large numbers of files.
 *
 * Must be run within a CoroScope (via runtime.scope() or run_coro_scope).
 *
 * @param scope CoroScope for spawning parallel tasks.
 * @param paths List of file paths to read.
 * @param callback Called for each file result. Return false to cancel.
 * @return Summary stats (files_read, files_failed, total_rows, total_batches).
 */
coro::CoroTask<ParallelReadResult> read_arrow_files_streaming(
    CoroScope& scope, std::vector<std::string> paths,
    FileResultCallback callback);

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_PARALLEL_READER_H
