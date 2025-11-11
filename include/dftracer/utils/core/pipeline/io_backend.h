#ifndef DFTRACER_UTILS_CORE_PIPELINE_IO_BACKEND_H
#define DFTRACER_UTILS_CORE_PIPELINE_IO_BACKEND_H

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dftracer::utils {

/**
 * IOCompletion - Result of an async I/O operation
 */
struct IOCompletion {
    std::coroutine_handle<> coro_handle;  // Coroutine to resume
    std::vector<char> data;               // Data read (for reads)
    int error_code;                       // errno value (0 = success)
    std::uint64_t operation_id;           // ID of the completed operation

    IOCompletion() : error_code(0), operation_id(0) {}

    IOCompletion(std::coroutine_handle<> h, std::vector<char> d, int err,
                 std::uint64_t id)
        : coro_handle(h),
          data(std::move(d)),
          error_code(err),
          operation_id(id) {}
};

/**
 * IOBackend - Abstract interface for platform-specific async I/O
 *
 * This is an internal interface used by IOExecutor.
 * Platform-specific implementations:
 * - Linux: io_uring
 * - macOS/BSD: kqueue
 * - Fallback: thread pool
 *
 * NOT exposed to users - internal to pipeline infrastructure.
 */
class IOBackend {
   public:
    virtual ~IOBackend() = default;

    /**
     * Submit async read operation
     *
     * @param fd File descriptor to read from
     * @param offset Byte offset in file
     * @param size Number of bytes to read
     * @param handle Coroutine handle to resume on completion
     * @return Operation ID (unique identifier for tracking)
     */
    virtual std::uint64_t submit_read(int fd, std::size_t offset,
                                      std::size_t size,
                                      std::coroutine_handle<> handle) = 0;

    /**
     * Submit async write operation
     *
     * @param fd File descriptor to write to
     * @param offset Byte offset in file
     * @param data Data to write
     * @param handle Coroutine handle to resume on completion
     * @return Operation ID
     */
    virtual std::uint64_t submit_write(int fd, std::size_t offset,
                                       const std::vector<char>& data,
                                       std::coroutine_handle<> handle) = 0;

    /**
     * Wait for I/O completions (blocking with timeout)
     *
     * Blocks until at least one I/O operation completes or timeout expires.
     *
     * @param max_batch Maximum number of completions to return
     * @param timeout Maximum time to wait
     * @return Vector of completed I/O operations (may be empty on timeout)
     */
    virtual std::vector<IOCompletion> wait_for_completions(
        std::size_t max_batch = 256,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(10)) = 0;

    /**
     * Cancel a pending I/O operation
     *
     * @param operation_id ID returned by submit_read/write
     * @return true if cancelled, false if already completed
     */
    virtual bool cancel_operation(std::uint64_t operation_id) = 0;

    /**
     * Get number of pending I/O operations
     */
    virtual std::size_t get_pending_count() const = 0;

    /**
     * Check if backend is available on this platform
     */
    virtual bool is_available() const = 0;

    /**
     * Get backend name (for debugging/logging)
     */
    virtual const char* name() const = 0;

    /**
     * Factory method to create platform-specific I/O backend
     *
     * Returns the best available backend for current platform:
     * - Linux: IOUringBackend (if available)
     * - macOS/BSD: KqueueBackend
     * - Fallback: ThreadPoolIOBackend
     *
     * @return Unique pointer to IOBackend implementation
     */
    static std::unique_ptr<IOBackend> create();

    /**
     * Check if async I/O is available on this platform
     * @return true if io_uring or kqueue is available
     */
    static bool is_available_on_platform();
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_IO_BACKEND_H
