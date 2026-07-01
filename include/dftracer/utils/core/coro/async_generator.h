#ifndef DFTRACER_UTILS_CORE_CORO_ASYNC_GENERATOR_H
#define DFTRACER_UTILS_CORE_CORO_ASYNC_GENERATOR_H

#include <dftracer/utils/core/common/exception_helpers.h>

#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

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

// F() -> AsyncGenerator<T> (zero-arg factory)
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

// F(T) -> AsyncGenerator<U> (one-arg, returns generator)
template <typename F, typename T, typename = void>
struct is_flat_map_fn : std::false_type {};

template <typename F, typename T>
struct is_flat_map_fn<
    F, T, std::enable_if_t<is_async_generator_v<std::invoke_result_t<F, T>>>>
    : std::true_type {};

template <typename F, typename T>
inline constexpr bool is_flat_map_fn_v = is_flat_map_fn<F, T>::value;

// F(T) -> U where U is NOT an AsyncGenerator (one-arg, returns plain value)
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

template <typename T, typename F,
          std::enable_if_t<detail::is_map_fn_v<F, T>, int> = 0>
AsyncGenerator<std::invoke_result_t<F, T>> map(AsyncGenerator<T> source,
                                               F func) {
    while (auto val = co_await source.next()) {
        co_yield std::invoke(func, std::move(*val));
    }
}

template <typename T, typename F,
          std::enable_if_t<detail::is_flat_map_fn_v<F, T>, int> = 0,
          typename InnerGen = std::invoke_result_t<F, T>,
          typename U = typename InnerGen::value_type>
AsyncGenerator<U> flat_map(AsyncGenerator<T> source, F func) {
    while (auto val = co_await source.next()) {
        auto inner = std::invoke(func, std::move(*val));
        while (auto out = co_await inner.next()) {
            co_yield std::move(*out);
        }
    }
}

template <typename T, typename F>
AsyncGenerator<T> filter(AsyncGenerator<T> source, F pred) {
    while (auto val = co_await source.next()) {
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
    while (auto val = co_await first.next()) {
        co_yield std::move(*val);
    }
    auto second = factory();
    while (auto val = co_await second.next()) {
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
 *   gen >> func       flat_map: T -> AsyncGenerator of U
 *   gen | factory     lazy concat: append factory() after gen exhausted
 */
template <typename T>
class AsyncGenerator {
   public:
    using value_type = T;

    struct promise_type {
        std::optional<T> current_value_;
        std::exception_ptr exception_;
        std::coroutine_handle<> continuation_{};

        AsyncGenerator get_return_object() {
            return AsyncGenerator{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

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

    class NextAwaitable {
       private:
        std::coroutine_handle<promise_type> handle_;

       public:
        explicit NextAwaitable(std::coroutine_handle<promise_type> handle)
            : handle_(handle) {}

        bool await_ready() const noexcept { return !handle_ || handle_.done(); }

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
                rethrow_and_clear(handle_.promise().exception_);
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
            rethrow_and_clear(handle_.promise().exception_);
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
     * results are flattened (T -> AsyncGenerator of U).
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

#endif  // DFTRACER_UTILS_CORE_CORO_ASYNC_GENERATOR_H
