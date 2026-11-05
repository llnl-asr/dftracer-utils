// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_ASYNC_GENERATOR_H
#define DFTRACER_UTILS_CORO_ASYNC_GENERATOR_H

#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/detail/frame.h>
#include <dftracer/utils/coro/detail/pool_resume.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/memory/detail/manager.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

template <typename T>
class CoroTask;

template <typename T>
class AsyncGenerator;

// ============================================================================
// SFINAE traits
// ============================================================================

namespace detail {

template <typename T>
struct is_async_generator : std::false_type {};

template <typename T>
struct is_async_generator<AsyncGenerator<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_async_generator_v = is_async_generator<T>::value;

/// F() -> AsyncGenerator<T> (zero-arg factory)
template <typename F, typename T, typename = void>
struct is_generator_factory : std::false_type {};

template <typename F, typename T>
struct is_generator_factory<
    F, T, std::enable_if_t<is_async_generator_v<std::invoke_result_t<F>>>> {
    static constexpr bool value =
        std::is_same_v<std::invoke_result_t<F>, AsyncGenerator<T>>;
};

template <typename F, typename T>
inline constexpr bool is_generator_factory_v =
    is_generator_factory<F, T>::value;

/// F(T) -> `AsyncGenerator<U>` (one-arg, returns generator)
template <typename F, typename T, typename = void>
struct is_flat_map_fn : std::false_type {};

template <typename F, typename T>
struct is_flat_map_fn<
    F, T, std::enable_if_t<is_async_generator_v<std::invoke_result_t<F, T>>>>
    : std::true_type {};

template <typename F, typename T>
inline constexpr bool is_flat_map_fn_v = is_flat_map_fn<F, T>::value;

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

/// F(T) -> U where U is NOT an @ref AsyncGenerator (one-arg, returns plain
/// value)
template <typename F, typename T, typename = void>
struct is_map_fn : std::false_type {};

template <typename F, typename T>
struct is_map_fn<F, T, std::void_t<std::invoke_result_t<F, T>>>
    : std::bool_constant<!is_async_generator_v<std::invoke_result_t<F, T>>> {};

template <typename F, typename T>
inline constexpr bool is_map_fn_v = is_map_fn<F, T>::value;

}  // namespace detail

// ============================================================================
// Free combinator functions
// ============================================================================

/**
 * @brief Yields `func(item)` for each item. `func` returns a value or a
 * @ref CoroTask of a value, which is awaited, so one item runs at a time.
 */
template <typename T, typename F,
          std::enable_if_t<detail::is_map_fn_v<F, T>, int> = 0,
          typename R = std::invoke_result_t<F, T>,
          typename U = typename detail::UnwrapTask<R>::type>
AsyncGenerator<U> map(AsyncGenerator<T> source, F func) {
    for (;;) {
        auto val = co_await source.next();
        if (!val) break;
        if constexpr (detail::is_coro_task_v<R>) {
            U mapped = co_await std::invoke(func, std::move(*val));
            co_yield std::move(mapped);
        } else {
            co_yield std::invoke(func, std::move(*val));
        }
    }
}

template <typename T, typename F,
          std::enable_if_t<detail::is_flat_map_fn_v<F, T>, int> = 0,
          typename InnerGen = std::invoke_result_t<F, T>,
          typename U = typename InnerGen::value_type>
AsyncGenerator<U> flat_map(AsyncGenerator<T> source, F func) {
    for (;;) {
        auto val = co_await source.next();
        if (!val) break;
        auto inner = std::invoke(func, std::move(*val));
        for (;;) {
            auto out = co_await inner.next();
            if (!out) break;
            co_yield std::move(*out);
        }
    }
}

template <typename T, typename F>
AsyncGenerator<T> filter(AsyncGenerator<T> source, F pred) {
    for (;;) {
        auto val = co_await source.next();
        if (!val) break;
        if (std::invoke(pred, *val)) {
            co_yield std::move(*val);
        }
    }
}

template <typename T>
AsyncGenerator<T> take(AsyncGenerator<T> source, std::size_t n) {
    std::size_t count = 0;
    while (count < n) {
        auto val = co_await source.next();
        if (!val) break;
        co_yield std::move(*val);
        ++count;
    }
}

template <typename T, typename F,
          std::enable_if_t<detail::is_generator_factory_v<F, T>, int> = 0>
AsyncGenerator<T> concat(AsyncGenerator<T> first, F factory) {
    for (;;) {
        auto val = co_await first.next();
        if (!val) break;
        co_yield std::move(*val);
    }
    auto second = factory();
    for (;;) {
        auto val = co_await second.next();
        if (!val) break;
        co_yield std::move(*val);
    }
}

// ============================================================================
// AsyncGenerator<T>
// ============================================================================

/**
 * AsyncGenerator<T> - Asynchronous lazy sequence generator.
 *
 * Supports internal co_await via symmetric transfer. Combinators
 * (map, flat_map, filter, take, concat) enable fluent composition.
 *
 * Operators:
 *   gen > func        map: T -> U
 *   gen >> func       flat_map: T -> @ref AsyncGenerator of U
 *   gen | factory     lazy concat: append factory() after gen exhausted
 */
template <typename T>
class AsyncGenerator {
   public:
    using value_type = T;

    struct promise_type : detail::FrameAllocation {
        alignas(T) std::byte value_storage[sizeof(T)];
        bool has_value = false;
        std::exception_ptr exception;
        std::coroutine_handle<> continuation{};
        /// The @ref CoroTask handoff, once per next(): see
        /// CoroTask::await_suspend.
        std::atomic<bool> handoff{false};
        detail::RunContext context;

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

        template <typename U>
        auto await_transform(U&& awaitable) noexcept {
            return detail::in_pool(std::forward<U>(awaitable), context);
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

        bool await_ready() const noexcept { return !handle_ || handle_.done(); }

        bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
            if (!handle_ || handle_.done()) return false;
            auto& p = handle_.promise();
            p.continuation = awaiting;
            p.handoff.store(false, std::memory_order_relaxed);
            handle_.resume();
            return !p.handoff.exchange(true, std::memory_order_acq_rel);
        }

        dstd::Optional<T> await_resume() {
            if (!handle_) {
                return dstd::nullopt;
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

    // ========================================================================
    // Fluent combinators
    // ========================================================================

    template <typename F, std::enable_if_t<detail::is_map_fn_v<F, T>, int> = 0>
    auto map(F&& func) && {
        return coro::map(std::move(*this), std::forward<F>(func));
    }

    template <typename F,
              std::enable_if_t<detail::is_flat_map_fn_v<F, T>, int> = 0>
    auto flat_map(F&& func) && {
        return coro::flat_map(std::move(*this), std::forward<F>(func));
    }

    template <typename F>
    AsyncGenerator<T> filter(F&& pred) && {
        return coro::filter(std::move(*this), std::forward<F>(pred));
    }

    AsyncGenerator<T> take(std::size_t n) && {
        return coro::take(std::move(*this), n);
    }

    AsyncGenerator<T> concat(AsyncGenerator<T> other) && {
        return coro::concat(std::move(*this), [o = std::move(other)]() mutable {
            return std::move(o);
        });
    }

    template <typename F,
              std::enable_if_t<detail::is_generator_factory_v<F, T>, int> = 0>
    AsyncGenerator<T> concat(F&& factory) && {
        return coro::concat(std::move(*this), std::forward<F>(factory));
    }

    // ========================================================================
    // Operators
    // ========================================================================

    /**
     * operator> : map -- transform each element (T -> U).
     */
    template <typename F, std::enable_if_t<detail::is_map_fn_v<F, T>, int> = 0>
    auto operator>(F&& func) && {
        return std::move(*this).map(std::forward<F>(func));
    }

    /**
     * operator>> : flat_map -- each element produces a sub-generator,
     * results are flattened (T -> @ref AsyncGenerator of U).
     */
    template <typename F,
              std::enable_if_t<detail::is_flat_map_fn_v<F, T>, int> = 0>
    auto operator>>(F&& func) && {
        return std::move(*this).flat_map(std::forward<F>(func));
    }

    /**
     * operator| with generator : concat (lazy under the hood).
     */
    friend AsyncGenerator<T> operator|(AsyncGenerator<T>&& lhs,
                                       AsyncGenerator<T>&& rhs) {
        return coro::concat(std::move(lhs), [r = std::move(rhs)]() mutable {
            return std::move(r);
        });
    }

    /**
     * operator| with factory : lazy concat
     * factory() called after lhs is exhausted.
     */
    template <typename F,
              std::enable_if_t<detail::is_generator_factory_v<F, T>, int> = 0>
    friend AsyncGenerator<T> operator|(AsyncGenerator<T>&& lhs, F&& factory) {
        return coro::concat(std::move(lhs), std::forward<F>(factory));
    }
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_ASYNC_GENERATOR_H
