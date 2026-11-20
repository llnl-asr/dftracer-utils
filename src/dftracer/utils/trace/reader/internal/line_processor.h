// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_READER_INTERNAL_LINE_PROCESSOR_H
#define DFTRACER_UTILS_TRACE_READER_INTERNAL_LINE_PROCESSOR_H

#include <dftracer/utils/coro/task.h>

#include <cstddef>

namespace dftracer::utils::trace::reader::internal {

/**
 * Base class for processing lines during streaming read operations.
 * Provides zero-copy callback interface for line-by-line processing.
 */
class LineProcessor {
   public:
    virtual ~LineProcessor() = default;

    /**
     * Process a single line of data.
     * @param line The line bytes (not null-terminated). The caller keeps
     *        them alive until the returned task completes.
     * @return true to continue processing, false to stop early
     */
    virtual coro::CoroTask<bool> process(dstd::StringView line) = 0;

    /**
     * Called before processing begins.
     * Override to perform initialization.
     * @param start_line Starting line number (1-based)
     * @param end_line Ending line number (1-based)
     */
    virtual void begin([[maybe_unused]] std::size_t start_line,
                       [[maybe_unused]] std::size_t end_line) {}

    /**
     * Called after processing completes.
     * Override to perform cleanup or finalization.
     */
    virtual void end() {}
};

}  // namespace dftracer::utils::trace::reader::internal

#endif  // DFTRACER_UTILS_TRACE_READER_INTERNAL_LINE_PROCESSOR_H
