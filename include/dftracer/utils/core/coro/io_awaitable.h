#ifndef DFTRACER_UTILS_CORE_CORO_IO_AWAITABLE_H
#define DFTRACER_UTILS_CORE_CORO_IO_AWAITABLE_H

#include <dftracer/utils/core/coro/task.h>

#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace dftracer::utils {
class Executor;
class IOExecutor;
}  // namespace dftracer::utils

namespace dftracer::utils::coro {

/**
 * IOAwaitable<T> - Awaitable type for async I/O operations
 *
 * This is the return type of ctx.spawn_io(). It represents an I/O operation
 * that will be offloaded to the I/O thread pool and resumed when complete.
 *
 * Design:
 * - Suspends calling coroutine
 * - Submits I/O operation to IOExecutor (if available)
 * - Falls back to synchronous execution if no IOExecutor
 * - Resumes on worker thread when I/O completes
 * - Returns result or throws exception
 *
 * Usage:
 * @code
 * auto data = co_await ctx.spawn_io([&]() {
 *     return read_file("data.bin");
 * });
 * @endcode
 */
template <typename T>
class IOAwaitable {
   public:
    using result_type = T;
    using io_func_type = std::function<T()>;

   private:
    io_func_type io_func_;                  // I/O operation to execute
    std::optional<T> result_;               // Result storage
    std::exception_ptr exception_;          // Exception storage
    bool ready_{false};                     // Is result already available?
    std::coroutine_handle<> continuation_;  // Coroutine to resume

   public:
    /**
     * Constructor for deferred I/O operation
     * @param func I/O operation to execute (executed on I/O thread)
     */
    explicit IOAwaitable(io_func_type func)
        : io_func_(std::move(func)), ready_(false) {}

    /**
     * Constructor for immediate result (no I/O needed)
     * Used when async I/O is disabled - returns immediately
     */
    static IOAwaitable make_ready(T value) {
        IOAwaitable awaitable(nullptr);
        awaitable.result_ = std::move(value);
        awaitable.ready_ = true;
        return awaitable;
    }

    /**
     * Constructor for immediate exception
     */
    static IOAwaitable make_exceptional(std::exception_ptr ex) {
        IOAwaitable awaitable(nullptr);
        awaitable.exception_ = ex;
        awaitable.ready_ = true;
        return awaitable;
    }

    // Move-only semantics
    IOAwaitable(const IOAwaitable&) = delete;
    IOAwaitable& operator=(const IOAwaitable&) = delete;

    IOAwaitable(IOAwaitable&& other) noexcept
        : io_func_(std::move(other.io_func_)),
          result_(std::move(other.result_)),
          exception_(std::move(other.exception_)),
          ready_(other.ready_),
          continuation_(other.continuation_) {
        other.ready_ = false;
        other.continuation_ = nullptr;
    }

    IOAwaitable& operator=(IOAwaitable&& other) noexcept {
        if (this != &other) {
            io_func_ = std::move(other.io_func_);
            result_ = std::move(other.result_);
            exception_ = std::move(other.exception_);
            ready_ = other.ready_;
            continuation_ = other.continuation_;
            other.ready_ = false;
            other.continuation_ = nullptr;
        }
        return *this;
    }

    // ========================================================================
    // Awaitable interface (for co_await)
    // ========================================================================

    /**
     * Check if result is already available (optimization)
     * If true, coroutine won't suspend
     */
    bool await_ready() const noexcept { return ready_; }

    /**
     * Suspend coroutine and submit I/O operation
     * @param h Coroutine handle to resume after I/O completes
     * @return true to suspend, false to resume immediately
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h);

    /**
     * Get result when coroutine resumes
     * @return Result of I/O operation
     * @throws std::exception_ptr if I/O failed
     */
    T await_resume() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }

        if (!result_.has_value()) {
            throw std::runtime_error(
                "IOAwaitable: No result available (I/O not completed?)");
        }

        return std::move(*result_);
    }

    // ========================================================================
    // Internal methods (called by IOExecutor)
    // ========================================================================

    /**
     * Execute the I/O operation (called by I/O thread)
     * Stores result or exception
     */
    void execute_io() {
        try {
            if constexpr (std::is_void_v<T>) {
                io_func_();
                result_ = std::nullopt;  // void result
            } else {
                result_ = io_func_();
            }
        } catch (...) {
            exception_ = std::current_exception();
        }
        ready_ = true;
    }

    /**
     * Set result directly (for testing or optimization)
     */
    void set_result(T value) {
        result_ = std::move(value);
        ready_ = true;
    }

    /**
     * Set exception directly
     */
    void set_exception(std::exception_ptr ex) {
        exception_ = ex;
        ready_ = true;
    }

    /**
     * Get the I/O function (for IOExecutor to execute)
     */
    io_func_type& get_io_func() { return io_func_; }
    const io_func_type& get_io_func() const { return io_func_; }

    /**
     * Get continuation coroutine (for IOExecutor to resume)
     */
    std::coroutine_handle<> get_continuation() const { return continuation_; }

    /**
     * Check if I/O is complete
     */
    bool is_ready() const { return ready_; }
};

/**
 * Specialization for void return type
 */
template <>
class IOAwaitable<void> {
   public:
    using result_type = void;
    using io_func_type = std::function<void()>;

   private:
    io_func_type io_func_;
    std::exception_ptr exception_;
    bool ready_{false};
    std::coroutine_handle<> continuation_;

   public:
    explicit IOAwaitable(io_func_type func)
        : io_func_(std::move(func)), ready_(false) {}

    static IOAwaitable make_ready() {
        IOAwaitable awaitable(nullptr);
        awaitable.ready_ = true;
        return awaitable;
    }

    static IOAwaitable make_exceptional(std::exception_ptr ex) {
        IOAwaitable awaitable(nullptr);
        awaitable.exception_ = ex;
        awaitable.ready_ = true;
        return awaitable;
    }

    // Move-only
    IOAwaitable(const IOAwaitable&) = delete;
    IOAwaitable& operator=(const IOAwaitable&) = delete;

    IOAwaitable(IOAwaitable&& other) noexcept
        : io_func_(std::move(other.io_func_)),
          exception_(std::move(other.exception_)),
          ready_(other.ready_),
          continuation_(other.continuation_) {
        other.ready_ = false;
        other.continuation_ = nullptr;
    }

    IOAwaitable& operator=(IOAwaitable&& other) noexcept {
        if (this != &other) {
            io_func_ = std::move(other.io_func_);
            exception_ = std::move(other.exception_);
            ready_ = other.ready_;
            continuation_ = other.continuation_;
            other.ready_ = false;
            other.continuation_ = nullptr;
        }
        return *this;
    }

    // Awaitable interface
    bool await_ready() const noexcept { return ready_; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h);

    void await_resume() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

    // Internal methods
    void execute_io() {
        try {
            if (io_func_) {
                io_func_();
            }
        } catch (...) {
            exception_ = std::current_exception();
        }
        ready_ = true;
    }

    void set_exception(std::exception_ptr ex) {
        exception_ = ex;
        ready_ = true;
    }

    io_func_type& get_io_func() { return io_func_; }
    const io_func_type& get_io_func() const { return io_func_; }

    std::coroutine_handle<> get_continuation() const { return continuation_; }

    bool is_ready() const { return ready_; }
};

}  // namespace dftracer::utils::coro

#include <dftracer/utils/core/coro/io_awaitable_impl.h>

#endif  // DFTRACER_UTILS_CORE_CORO_IO_AWAITABLE_H
