#ifndef DFTRACER_UTILS_CORE_CORO_ASYNC_GENERATOR_H
#define DFTRACER_UTILS_CORE_CORO_ASYNC_GENERATOR_H

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace dftracer::utils::coro {

/**
 * AsyncGenerator<T> - Asynchronous lazy sequence generator
 *
 * Usage:
 * @code
 * AsyncGenerator<Data> read_files(CoroScope& ctx) {
 *     for (int i = 0; i < 100; i++) {
 *         // Async I/O operation
 *         auto data = co_await ctx.spawn_io([i]() {
 *             return read_file(i);
 *         });
 *         co_yield data;
 *     }
 * }
 *
 * // Async iteration
 * auto gen = read_files(ctx);
 * while (auto value = co_await gen.next()) {
 *     process(*value);
 * }
 * @endcode
 */
template <typename T>
class AsyncGenerator {
   public:
    /**
     * Promise type for async generator coroutine
     */
    struct promise_type {
        std::optional<T> current_value_;
        std::exception_ptr exception_;

        /**
         * Create AsyncGenerator from promise
         */
        AsyncGenerator get_return_object() {
            return AsyncGenerator{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        /**
         * Suspend at start
         */
        std::suspend_always initial_suspend() noexcept { return {}; }

        /**
         * Suspend at end
         */
        std::suspend_always final_suspend() noexcept { return {}; }

        /**
         * Store yielded value and suspend
         */
        std::suspend_always yield_value(T value) {
            current_value_ = std::move(value);
            return {};
        }

        /**
         * Generator completed
         */
        void return_void() noexcept {}

        /**
         * Capture exception
         */
        void unhandled_exception() { exception_ = std::current_exception(); }
    };

    /**
     * Awaitable for next value
     */
    class NextAwaitable {
       private:
        std::coroutine_handle<promise_type> handle_;

       public:
        explicit NextAwaitable(std::coroutine_handle<promise_type> handle)
            : handle_(handle) {}

        /**
         * Check if value is immediately available
         */
        bool await_ready() const noexcept { return handle_ && handle_.done(); }

        /**
         * Suspend and resume generator to produce next value
         */
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<> awaiting) noexcept {
            if (!handle_ || handle_.done()) {
                // Generator exhausted, resume awaiting coroutine immediately
                return awaiting;
            }

            // Resume generator to produce next value
            // Generator will suspend at next co_yield or completion
            handle_.resume();

            // Resume awaiting coroutine with result
            return awaiting;
        }

        /**
         * Get the produced value (or nullopt if done)
         */
        std::optional<T> await_resume() {
            if (!handle_) {
                return std::nullopt;
            }

            if (handle_.promise().exception_) {
                std::rethrow_exception(handle_.promise().exception_);
            }

            if (handle_.done()) {
                return std::nullopt;
            }

            if (handle_.promise().current_value_) {
                return std::move(handle_.promise().current_value_);
            }

            return std::nullopt;
        }
    };

   private:
    std::coroutine_handle<promise_type> handle_;

   public:
    /**
     * Construct AsyncGenerator
     */
    explicit AsyncGenerator(std::coroutine_handle<promise_type> handle)
        : handle_(handle) {}

    /**
     * Destructor
     */
    ~AsyncGenerator() {
        if (handle_) {
            handle_.destroy();
        }
    }

    /**
     * Move-only semantics
     */
    AsyncGenerator(const AsyncGenerator&) = delete;
    AsyncGenerator& operator=(const AsyncGenerator&) = delete;

    AsyncGenerator(AsyncGenerator&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    AsyncGenerator& operator=(AsyncGenerator&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    /**
     * Get next value asynchronously
     *
     * @return Awaitable that yields std::optional<T>
     *         - Some(value) if value produced
     *         - None if generator exhausted
     *
     * Usage:
     * @code
     * while (auto value = co_await gen.next()) {
     *     process(*value);
     * }
     * @endcode
     */
    NextAwaitable next() {
        if (!handle_) {
            return NextAwaitable{nullptr};
        }
        return NextAwaitable{handle_};
    }

    /**
     * Check if generator is done
     */
    bool done() const { return !handle_ || handle_.done(); }

    /**
     * Check if generator has pending exception
     */
    bool has_exception() const {
        return handle_ && handle_.promise().exception_ != nullptr;
    }

    /**
     * Rethrow pending exception
     */
    void rethrow_if_exception() const {
        if (handle_ && handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
    }
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_ASYNC_GENERATOR_H
