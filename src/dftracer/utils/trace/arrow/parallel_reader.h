// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_ARROW_PARALLEL_READER_H
#define DFTRACER_UTILS_TRACE_ARROW_PARALLEL_READER_H

#include <dftracer/utils/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/arrow/arrow_export.h>

namespace dftracer::utils::trace::arrow {

/**
 * Result from reading a single Arrow IPC file.
 * Batches are stored in shared_ptr to allow copying through dstd::SharedFuture.
 */
struct ArrowFileReadResult {
    dstd::String path;
    dstd::SharedPtr<dstd::Vector<ArrowExportResult>> batches;
    std::int64_t total_rows = 0;
    dstd::String error;
    bool success = true;

    ArrowFileReadResult()
        : batches(dstd::make_shared<dstd::Vector<ArrowExportResult>>()) {}
};

/**
 * Result from reading multiple Arrow IPC files in parallel.
 */
struct ParallelReadResult {
    dstd::Vector<ArrowFileReadResult> file_results;
    std::int64_t total_rows = 0;
    std::int64_t total_batches = 0;
    std::size_t files_read = 0;
    std::size_t files_failed = 0;
};

/**
 * Read a single Arrow IPC file as a coroutine.
 *
 * @param path Path to the Arrow IPC file.
 * @return @ref ArrowFileReadResult with batches or error.
 */
coro::CoroTask<ArrowFileReadResult> read_arrow_file_async(dstd::String path);

/**
 * Read multiple Arrow IPC files in parallel.
 *
 * Collects all results before returning.
 *
 * @param paths List of file paths to read.
 * @return @ref ParallelReadResult with all results.
 */
coro::CoroTask<ParallelReadResult> read_arrow_files_parallel(
    dstd::Vector<dstd::String> paths);

}  // namespace dftracer::utils::trace::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
#endif  // DFTRACER_UTILS_TRACE_ARROW_PARALLEL_READER_H
