#pragma once

#include <dftracer/utils/core/io/io_backend.h>
#include <sys/stat.h>

#include <cstddef>
#include <string>

#include "io_thread_pool.h"

namespace dftracer::utils {
class Executor;
}

namespace dftracer::utils::io {

/// I/O operation types.
enum class IoOp { READ, WRITE, OPEN, CLOSE, FSYNC, FTRUNCATE, FSTAT };

/// Request descriptor that doubles as SubmitContext.
/// Heap-allocated per I/O operation, freed after completion.
struct IoRequest : SubmitContext {
    IoOp op = IoOp::READ;
    int fd = -1;
    void* buf = nullptr;
    std::size_t len = 0;
    off_t offset = 0;
    const char* path = nullptr;
    int flags = 0;
    mode_t mode = 0;
    struct stat* stat_buf = nullptr;
    IoAwaitable* awaitable = nullptr;
    Executor* executor = nullptr;
    IoThreadPool* pool = nullptr;
};

/// Pure thread pool I/O backend -- universal fallback.
/// Runs every I/O operation on a small dedicated thread pool.
class ThreadPoolBackend : public IoBackend {
   public:
    explicit ThreadPoolBackend(Executor& executor, std::size_t pool_size = 4,
                               unsigned batch_threshold = 0);

    void start() override;
    void stop() override;

    IoAwaitable submit_read(int fd, void* buf, std::size_t len,
                            off_t offset) override;
    IoAwaitable submit_write(int fd, const void* buf, std::size_t len,
                             off_t offset) override;
    IoAwaitable submit_open(const char* path, int flags, mode_t mode) override;
    IoAwaitable submit_close(int fd) override;
    IoAwaitable submit_fsync(int fd) override;
    IoAwaitable submit_ftruncate(int fd, off_t length) override;
    IoAwaitable submit_fstat(int fd, struct stat* buf) override;

    std::size_t poll(int timeout_ms) override;
    int flush() override;
    std::string name() const override;

    /// Called by await_suspend via SubmitContext::submit.
    /// Submits the IoRequest to the thread pool.
    static void submit_to_pool(SubmitContext* ctx, IoAwaitable* awaitable);

   private:
    /// Execute the blocking syscall and resume the coroutine.
    static void execute_request(IoRequest* req);

    Executor& executor_;
    IoThreadPool pool_;
};

}  // namespace dftracer::utils::io
