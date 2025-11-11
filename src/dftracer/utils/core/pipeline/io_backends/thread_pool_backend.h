#ifndef DFTRACER_UTILS_CORE_PIPELINE_IO_BACKENDS_THREAD_POOL_BACKEND_H
#define DFTRACER_UTILS_CORE_PIPELINE_IO_BACKENDS_THREAD_POOL_BACKEND_H

#include <concurrentqueue.h>
#include <dftracer/utils/core/pipeline/io_backend.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace dftracer::utils {

// Track pending thread pool operations
struct ThreadPoolPendingOperation {
    std::coroutine_handle<> coro_handle;
    std::vector<char> buffer;
    int error_code{0};
    std::uint64_t operation_id;
    bool completed{false};

    ThreadPoolPendingOperation() = default;
    ThreadPoolPendingOperation(std::coroutine_handle<> h, std::uint64_t id)
        : coro_handle(h), operation_id(id) {}
};

/**
 * Simple thread pool-based I/O backend (fallback)
 * Used when io_uring (Linux) or kqueue (macOS) are not available
 */
class ThreadPoolIOBackend : public IOBackend {
   private:
    static constexpr std::size_t DEFAULT_THREAD_POOL_SIZE = 4;

    std::atomic<std::uint64_t> next_op_id_{1};

    std::vector<std::thread> worker_threads_;
    moodycamel::ConcurrentQueue<std::function<void()>> task_queue_;
    std::atomic<bool> shutdown_requested_{false};

    std::map<std::uint64_t, ThreadPoolPendingOperation> pending_ops_;
    mutable std::mutex pending_mutex_;

    moodycamel::ConcurrentQueue<IOCompletion> completed_ops_;

    void worker_loop();
    void enqueue_task(std::function<void()> task);
    void complete_operation(std::uint64_t op_id, std::vector<char> data,
                            int error);

   public:
    ThreadPoolIOBackend();
    ~ThreadPoolIOBackend() override;

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

#endif  // DFTRACER_UTILS_CORE_PIPELINE_IO_BACKENDS_THREAD_POOL_BACKEND_H
