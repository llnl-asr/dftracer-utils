#pragma once
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#define DFTRACER_UTILS_HAVE_KQUEUE 1

#include <dftracer/utils/core/io/io_backend.h>
#include <sys/stat.h>

#include <cstddef>
#include <string>

#include "io_completion_thread.h"
#include "io_thread_pool.h"

namespace dftracer::utils {
class Executor;
}

namespace dftracer::utils::io {

struct IoRequest;  // defined in thread_pool_backend.h

/// kqueue + thread pool I/O backend (macOS, FreeBSD, etc.).
/// File I/O is handled by the thread pool (kqueue cannot watch regular
/// files). A kqueue reactor is set up for future network I/O (socket
/// read/write/accept). The completion thread blocks on kevent(),
/// currently only watching a user event used for clean shutdown.
class KqueueThreadPoolBackend : public IoBackend {
   public:
    explicit KqueueThreadPoolBackend(Executor& executor,
                                     std::size_t pool_size = 4,
                                     unsigned batch_threshold = 0);
    ~KqueueThreadPoolBackend() override;

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
    std::string name() const override { return "kqueue+threadpool"; }

    /// Called by await_suspend via SubmitContext::submit.
    static void submit_to_pool(SubmitContext* ctx, IoAwaitable* awaitable);

   private:
    /// Execute blocking syscall and resume coroutine.
    static void execute_request(IoRequest* req);

    /// Kqueue loop run by the completion thread. Currently only watches
    /// a user event for shutdown; will be extended for socket I/O.
    void kqueue_loop();

    Executor& executor_;
    IoThreadPool pool_;
    IoCompletionThread completion_thread_;
    int kqueue_fd_ = -1;

    /// User event identifier for shutdown signaling.
    static constexpr uintptr_t SHUTDOWN_IDENT = 0xDEAD;
};

}  // namespace dftracer::utils::io

#endif  // kqueue platforms
