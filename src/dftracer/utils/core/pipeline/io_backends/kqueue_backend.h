#ifndef DFTRACER_UTILS_CORE_PIPELINE_IO_BACKENDS_KQUEUE_BACKEND_H
#define DFTRACER_UTILS_CORE_PIPELINE_IO_BACKENDS_KQUEUE_BACKEND_H

#ifdef __APPLE__

#include <dftracer/utils/core/pipeline/io_backend.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace dftracer::utils {

// Track pending kqueue operations
struct KqueuePendingOperation {
    std::coroutine_handle<> coro_handle;
    std::vector<char> buffer;  // For read operations
    std::size_t size;
    std::size_t offset;
    int fd;
    bool is_read;
    std::uint64_t operation_id;

    KqueuePendingOperation() = default;
    KqueuePendingOperation(std::coroutine_handle<> h, std::size_t s,
                           std::size_t off, int file_fd, bool read,
                           std::uint64_t id);
};

/**
 * kqueue backend for macOS/BSD
 * Async I/O using kqueue event notification
 */
class KqueueBackend : public IOBackend {
   private:
    int kq_{-1};  // kqueue file descriptor
    std::atomic<std::uint64_t> next_op_id_{1};
    std::map<std::uint64_t, KqueuePendingOperation> pending_ops_;
    mutable std::mutex pending_mutex_;

   public:
    KqueueBackend();
    ~KqueueBackend() override;

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

#endif  // __APPLE__

#endif  // DFTRACER_UTILS_CORE_PIPELINE_IO_BACKENDS_KQUEUE_BACKEND_H
