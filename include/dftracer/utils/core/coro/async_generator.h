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
 * Supports internal co_await (e.g. async I/O) via symmetric transfer.
 * The consumer suspends until the generator either co_yields a value
 * or reaches its final suspension point.
 *
 * Usage:
 * @code
 * AsyncGenerator<Data> read_files(CoroScope& ctx) {
 *     for (int i = 0; i < 100; i++) {
 *         auto data = co_await io::async_read(fd, buf, len);
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
    struct promise_type {
        std::optional<T> current_value_;
        std::exception_ptr exception_;
        // Consumer coroutine waiting for the next value.
        std::coroutine_handle<> continuation_{};

        AsyncGenerator get_return_object() {
            return AsyncGenerator{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        // On co_yield: store value, then resume the consumer via symmetric
        // transfer so the consumer's await_resume() can retrieve it.
        auto yield_value(T value) noexcept {
            current_value_ = std::move(value);
            struct YieldToConsumer {
                std::coroutine_handle<> continuation;
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<>) noexcept {
                    return continuation;
                }
                void await_resume() noexcept {}
            };
            return YieldToConsumer{continuation_};
        }

        // On completion: resume the consumer so it sees done() == true.
        auto final_suspend() noexcept {
            struct FinalToConsumer {
                std::coroutine_handle<> continuation;
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<>) noexcept {
                    if (continuation) return continuation;
                    return std::noop_coroutine();
                }
                void await_resume() noexcept {}
            };
            return FinalToConsumer{continuation_};
        }

        void return_void() noexcept {}

        void unhandled_exception() { exception_ = std::current_exception(); }
    };

    /**
     * Awaitable returned by next().  Suspends the consumer and transfers
     * control to the generator via symmetric transfer.  The generator
     * resumes the consumer when it co_yields or completes.
     */
    class NextAwaitable {
       private:
        std::coroutine_handle<promise_type> handle_;

       public:
        explicit NextAwaitable(std::coroutine_handle<promise_type> handle)
            : handle_(handle) {}

        bool await_ready() const noexcept { return !handle_ || handle_.done(); }

        // Store the consumer as the continuation, then transfer to the
        // generator.  The generator will resume us via yield_value or
        // final_suspend.
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<> awaiting) noexcept {
            if (!handle_ || handle_.done()) {
                return awaiting;
            }
            handle_.promise().continuation_ = awaiting;
            return handle_;
        }

        std::optional<T> await_resume() {
            if (!handle_) {
                return std::nullopt;
            }
            if (handle_.promise().exception_) {
                auto ex = std::move(handle_.promise().exception_);
                handle_.promise().exception_ = nullptr;
                std::rethrow_exception(std::move(ex));
            }
            if (handle_.done()) {
                return std::nullopt;
            }
            if (handle_.promise().current_value_) {
                auto val = std::move(handle_.promise().current_value_);
                handle_.promise().current_value_.reset();
                return val;
            }
            return std::nullopt;
        }
    };

   private:
    std::coroutine_handle<promise_type> handle_;

   public:
    explicit AsyncGenerator(std::coroutine_handle<promise_type> handle)
        : handle_(handle) {}

    ~AsyncGenerator() {
        if (handle_) {
            handle_.destroy();
        }
    }

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
     * Get next value asynchronously.
     *
     * @return Awaitable that yields std::optional<T>:
     *         - Some(value) if a value was produced
     *         - None if the generator is exhausted
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

    bool done() const { return !handle_ || handle_.done(); }

    bool has_exception() const {
        return handle_ && handle_.promise().exception_ != nullptr;
    }

    void rethrow_if_exception() {
        if (handle_ && handle_.promise().exception_) {
            auto ex = std::move(handle_.promise().exception_);
            handle_.promise().exception_ = nullptr;
            std::rethrow_exception(std::move(ex));
        }
    }
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_ASYNC_GENERATOR_H
