#ifndef DFTRACER_UTILS_CORE_PIPELINE_IO_BACKENDS_IO_URING_BACKEND_H
#define DFTRACER_UTILS_CORE_PIPELINE_IO_BACKENDS_IO_URING_BACKEND_H

#ifdef __linux__

#include <dftracer/utils/core/pipeline/io_backend.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dftracer::utils {

/**
 * io_uring backend for Linux
 * High-performance async I/O using io_uring
 */
class IOUringBackend : public IOBackend {
   private:
    // struct io_uring ring_;  // TODO: Add when liburing is available
    std::atomic<std::uint64_t> next_op_id_{1};

   public:
    IOUringBackend();
    ~IOUringBackend() override;

    std::uint64_t submit_read(int fd, std::size_t offset, std::size_t size,
                              std::coroutine_handle<> handle) override;

    std::uint64_t submit_write(int fd, std::size_t offset,
                               const std::vector<char>& data,
                               std::coroutine_handle<> handle) override;

    std::vector<IOCompletion> wait_for_completions(
        std::size_t max_batch, std::chrono::milliseconds timeout) override;

    bool cancel_operation(std::uint64_t operation_id) override;

    std::size_t get_pending_count() const override;

    bool is_available() const override;

    const char* name() const override;
};

}  // namespace dftracer::utils

#endif  // __linux__

#endif  // DFTRACER_UTILS_CORE_PIPELINE_IO_BACKENDS_IO_URING_BACKEND_H
