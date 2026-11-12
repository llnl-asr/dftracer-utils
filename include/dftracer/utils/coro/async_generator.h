// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_ASYNC_GENERATOR_H
#define DFTRACER_UTILS_CORO_ASYNC_GENERATOR_H

#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/detail/frame.h>
#include <dftracer/utils/coro/detail/pool_resume.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/memory/detail/manager.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

template <typename T>
class AsyncGenerator;

namespace detail {

template <typename T>
struct is_async_generator : std::false_type {};
template <typename T>
struct is_async_generator<AsyncGenerator<T>> : std::true_type {};
template <typename T>
inline constexpr bool is_async_generator_v = is_async_generator<T>::value;

template <typename R>
struct UnwrapTask {
    using type = R;
};
template <typename T>
struct UnwrapTask<CoroTask<T>> {
    using type = T;
};

}  // namespace detail

/**
 * @brief Asynchronous lazy sequence.
 *
 * A wait inside it follows the same cancel rules as a @ref CoroTask, through
 * the task that reads it: a stop or a cancel of that task ends the wait, the
 * generator ends as cancelled and the task that reads it stops. The operations
 * on streams are in `coro/stream.h`.
 */
template <typename T>
class AsyncGenerator {
   public:
    using value_type = T;

    struct promise_type : PromiseBase {
        alignas(T) std::byte value_storage[sizeof(T)];
        bool has_value = false;
        std::exception_ptr exception;

        T* value_ptr() noexcept {
            return std::launder(reinterpret_cast<T*>(value_storage));
        }
        void clear_value() noexcept {
            if (has_value) {
                value_ptr()->~T();
                has_value = false;
            }
        }
        ~promise_type() { clear_value(); }

        AsyncGenerator get_return_object() {
            return AsyncGenerator{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        detail::EnterPoolOnStart initial_suspend() noexcept {
            return {&context};
        }

        auto yield_value(T value) noexcept {
            clear_value();
            ::new (static_cast<void*>(value_storage)) T(std::move(value));
            has_value = true;
            return ToConsumer{&context};
        }

        auto final_suspend() noexcept { return ToConsumer{&context}; }

        detail::ValueQuery<bool> await_transform(
            this_coro::StopRequestedTag) noexcept {
            return {this->cancel_requested()};
        }

        detail::ExecutorQuery await_transform(this_coro::ExecutorTag) noexcept {
            return {this->executor != nullptr ? this->executor
                                              : detail::current_executor()};
        }

        template <typename U>
        auto await_transform(U&& awaitable) noexcept {
            bool at_cancel = false;
            if constexpr (!requires {
                              requires std::remove_cvref_t<U>::CANCEL_EXEMPT;
                          }) {
                at_cancel = this->cancel_requested();
            }
            return detail::InPool<U&&>{static_cast<U&&>(awaitable), &context,
                                       false, at_cancel};
        }

        void return_void() noexcept {}

        void unhandled_exception() { exception = std::current_exception(); }

        // The consumer may destroy the frame once the exchange is done.
        struct ToConsumer {
            const detail::RunContext* context;

            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                auto& p = h.promise();
                std::coroutine_handle<> next = p.continuation;
                if (next && p.handoff.exchange(true, std::memory_order_acq_rel))
                    next.resume();
            }
            void await_resume() noexcept { context->enter(); }
        };
    };

    class NextAwaitable {
       private:
        std::coroutine_handle<promise_type> handle_;

       public:
        explicit NextAwaitable(std::coroutine_handle<promise_type> handle)
            : handle_(handle) {}

        bool await_ready() const noexcept {
            return !handle_ || handle_.done() || handle_.promise().cancelled;
        }

        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> awaiting) noexcept {
            auto& p = handle_.promise();
            p.continuation = awaiting;
            p.handoff.store(false, std::memory_order_relaxed);
            // Set on every read: a reader before this one may be gone.
            p.set_root_promise(nullptr);
            p.parent_end = detail::cancel_end_for<Promise>();
            Executor* awaiter_executor = nullptr;
            if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
                p.set_root_promise(awaiting.promise().get_root_promise());
                awaiter_executor = awaiting.promise().get_executor();
            }
            if (awaiter_executor == nullptr) {
                awaiter_executor = detail::current_executor();
            }
            p.continuation_executor = awaiter_executor;
            if (p.executor == nullptr) p.executor = awaiter_executor;
            handle_.resume();
            if (!p.handoff.exchange(true, std::memory_order_acq_rel))
                return true;
            if constexpr (requires { awaiting.promise().propagate_cancel(); }) {
                if (p.cancelled && awaiting.promise().propagate_cancel()) {
                    return true;
                }
            }
            return false;
        }

        dstd::Optional<T> await_resume() {
            if (!handle_) {
                return dstd::nullopt;
            }
            if (handle_.promise().cancelled) {
                detail::cancelled_unstoppable(
                    "a stopped generator read by a frame that cannot stop");
            }
            if (handle_.promise().exception) {
                common::rethrow_and_clear(handle_.promise().exception);
            }
            if (handle_.done()) {
                return dstd::nullopt;
            }
            auto& promise = handle_.promise();
            if (promise.has_value) {
                dstd::Optional<T> out(std::move(*promise.value_ptr()));
                promise.clear_value();
                return out;
            }
            return dstd::nullopt;
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

    NextAwaitable next() {
        if (!handle_) {
            return NextAwaitable{nullptr};
        }
        return NextAwaitable{handle_};
    }

    /// The read of @ref next that gives how it ended and never ends the reader:
    /// an item or the end (an empty inner optional), a stop, or an exception.
    class SettledNext {
       public:
        static constexpr bool CANCEL_EXEMPT = true;

        explicit SettledNext(std::coroutine_handle<promise_type> handle)
            : next_(handle), handle_(handle) {}

        bool await_ready() const noexcept { return next_.await_ready(); }

        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> awaiting) noexcept {
            return next_.await_suspend(awaiting);
        }

        detail::Settled<dstd::Optional<T>> await_resume() noexcept(
            std::is_nothrow_move_constructible_v<T>) {
            detail::Settled<dstd::Optional<T>> out;
            if (!handle_) {
                out.value.emplace();
                return out;
            }
            auto& promise = handle_.promise();
            if (promise.cancelled) {
                out.stopped = true;
            } else if (promise.exception) {
                out.exception = std::move(promise.exception);
                promise.exception = nullptr;
            } else if (handle_.done() || !promise.has_value) {
                out.value.emplace();
            } else {
                out.value.emplace(std::move(*promise.value_ptr()));
                promise.clear_value();
            }
            return out;
        }

       private:
        NextAwaitable next_;
        std::coroutine_handle<promise_type> handle_;
    };

    SettledNext settled_next() { return SettledNext{handle_}; }

    bool done() const { return !handle_ || handle_.done(); }

    bool has_exception() const {
        return handle_ && handle_.promise().exception != nullptr;
    }

    void rethrow_if_exception() {
        if (handle_ && handle_.promise().exception) {
            common::rethrow_and_clear(handle_.promise().exception);
        }
    }

    /// Ends a wait inside this generator once @p flag is set, for a reader that
    /// is not a task. Wake the waiter with Executor::wake_cancelled. The flag
    /// must outlive the generator.
    void stop_on(const std::atomic<bool>* flag) noexcept {
        if (handle_) handle_.promise().cancel_source = flag;
    }
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_ASYNC_GENERATOR_H
