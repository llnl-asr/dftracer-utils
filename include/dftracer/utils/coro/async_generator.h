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

template <typename T>
struct is_coro_task : std::false_type {};
template <typename T>
struct is_coro_task<CoroTask<T>> : std::true_type {};
template <typename T>
inline constexpr bool is_coro_task_v = is_coro_task<T>::value;

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
 * the task that reads it: a stop or a cancel of that task ends the wait, and
 * the read raises dstd::Condition::Cancelled. The operations on streams are in
 * `coro/stream.h`.
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
            p.parent_cancel = nullptr;
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
            if constexpr (requires { awaiting.promise().propagate_cancel(); }) {
                p.parent_cancel = [](std::coroutine_handle<> h) noexcept {
                    return std::coroutine_handle<Promise>::from_address(
                               h.address())
                        .promise()
                        .propagate_cancel();
                };
            }
            handle_.resume();
            if constexpr (requires { awaiting.promise().propagate_cancel(); }) {
                if (p.cancelled && awaiting.promise().propagate_cancel()) {
                    return true;
                }
            }
            return !p.handoff.exchange(true, std::memory_order_acq_rel);
        }

        dstd::Optional<T> await_resume() {
            if (!handle_) {
                return dstd::nullopt;
            }
            if (handle_.promise().cancelled) {
                throw_cancelled("generator cancelled");
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
