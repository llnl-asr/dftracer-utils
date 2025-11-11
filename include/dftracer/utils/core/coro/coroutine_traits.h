#ifndef DFTRACER_UTILS_CORE_CORO_COROUTINE_TRAITS_H
#define DFTRACER_UTILS_CORE_CORO_COROUTINE_TRAITS_H

#include <coroutine>
#include <type_traits>

namespace dftracer::utils::coro {

// Forward declarations
template <typename T>
class CoroTask;

template <typename T>
class Generator;

// ============================================================================
// Type trait: is_coro_task
// Detect if a type is CoroTask<T>
// ============================================================================

template <typename T>
struct is_coro_task : std::false_type {};

template <typename T>
struct is_coro_task<CoroTask<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_coro_task_v = is_coro_task<T>::value;

// ============================================================================
// Type trait: is_generator
// Detect if a type is Generator<T>
// ============================================================================

template <typename T>
struct is_generator : std::false_type {};

template <typename T>
struct is_generator<Generator<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_generator_v = is_generator<T>::value;

// ============================================================================
// Type trait: is_awaitable
// Detect if a type can be used with co_await
// ============================================================================

namespace detail {

// Check if type has await_ready, await_suspend, await_resume
template <typename T, typename = void>
struct has_awaitable_interface : std::false_type {};

template <typename T>
struct has_awaitable_interface<
    T, std::void_t<decltype(std::declval<T>().await_ready()),
                   decltype(std::declval<T>().await_suspend(
                       std::declval<std::coroutine_handle<>>())),
                   decltype(std::declval<T>().await_resume())>>
    : std::true_type {};

}  // namespace detail

template <typename T>
struct is_awaitable : detail::has_awaitable_interface<T> {};

template <typename T>
inline constexpr bool is_awaitable_v = is_awaitable<T>::value;

// ============================================================================
// Type trait: coro_task_value_type
// Extract T from CoroTask<T>
// ============================================================================

template <typename T>
struct coro_task_value_type;

template <typename T>
struct coro_task_value_type<CoroTask<T>> {
    using type = T;
};

template <typename T>
using coro_task_value_type_t = typename coro_task_value_type<T>::type;

// ============================================================================
// Type trait: generator_value_type
// Extract T from Generator<T>
// ============================================================================

template <typename T>
struct generator_value_type;

template <typename T>
struct generator_value_type<Generator<T>> {
    using type = T;
};

template <typename T>
using generator_value_type_t = typename generator_value_type<T>::type;

// ============================================================================
// Type trait: awaitable_result_type
// Extract result type from awaitable (return type of await_resume())
// ============================================================================

namespace detail {

template <typename T, typename = void>
struct awaitable_result_type_impl {
    using type = void;
};

template <typename T>
struct awaitable_result_type_impl<
    T, std::void_t<decltype(std::declval<T>().await_resume())>> {
    using type = decltype(std::declval<T>().await_resume());
};

}  // namespace detail

template <typename T>
struct awaitable_result_type
    : detail::awaitable_result_type_impl<std::decay_t<T>> {};

template <typename T>
using awaitable_result_type_t = typename awaitable_result_type<T>::type;

// ============================================================================
// Type trait: unwrap_coro_task
// Unwrap CoroTask<T> to T, otherwise return as-is
// Useful for type deduction in make_task()
// ============================================================================

template <typename T>
struct unwrap_coro_task {
    using type = T;
};

template <typename T>
struct unwrap_coro_task<CoroTask<T>> {
    using type = T;
};

template <typename T>
using unwrap_coro_task_t = typename unwrap_coro_task<T>::type;

// ============================================================================
// Concept: Awaitable (C++20 concept)
// Requires: await_ready, await_suspend, await_resume
// ============================================================================

#if __cpp_concepts >= 201907L

template <typename T>
concept Awaitable = requires(T t, std::coroutine_handle<> h) {
    { t.await_ready() } -> std::convertible_to<bool>;
    { t.await_suspend(h) };
    { t.await_resume() };
};

#endif

// ============================================================================
// Helper: get_awaitable
// Handles both awaitables and operator co_await overloads
// ============================================================================

namespace detail {

template <typename T>
auto get_awaitable_impl(T&& value, int)
    -> decltype(std::forward<T>(value).operator co_await()) {
    return std::forward<T>(value).operator co_await();
}

template <typename T>
auto get_awaitable_impl(T&& value, long) -> T&& {
    return std::forward<T>(value);
}

}  // namespace detail

template <typename T>
auto get_awaitable(T&& value) {
    return detail::get_awaitable_impl(std::forward<T>(value), 0);
}

// ============================================================================
// Type trait: is_coroutine_function
// Detect if a function returns a coroutine type (CoroTask, Generator, etc.)
// ============================================================================

template <typename Func, typename = void>
struct is_coroutine_function : std::false_type {};

template <typename Func>
struct is_coroutine_function<
    Func, std::void_t<typename std::invoke_result_t<Func>::promise_type>>
    : std::true_type {};

template <typename Func>
inline constexpr bool is_coroutine_function_v =
    is_coroutine_function<Func>::value;

// ============================================================================
// Utility: make_awaitable
// Convert any value into an awaitable (immediate completion)
// ============================================================================

template <typename T>
class ReadyAwaitable {
   private:
    T value_;

   public:
    explicit ReadyAwaitable(T value) : value_(std::move(value)) {}

    bool await_ready() const noexcept { return true; }

    void await_suspend(std::coroutine_handle<>) const noexcept {}

    T await_resume() { return std::move(value_); }
};

// Specialization for void
template <>
class ReadyAwaitable<void> {
   public:
    ReadyAwaitable() = default;

    bool await_ready() const noexcept { return true; }

    void await_suspend(std::coroutine_handle<>) const noexcept {}

    void await_resume() const noexcept {}
};

/**
 * Create immediately ready awaitable from value
 * Useful for fallback when async I/O is not available
 */
template <typename T>
ReadyAwaitable<std::decay_t<T>> make_ready_awaitable(T&& value) {
    return ReadyAwaitable<std::decay_t<T>>(std::forward<T>(value));
}

inline ReadyAwaitable<void> make_ready_awaitable() {
    return ReadyAwaitable<void>();
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_COROUTINE_TRAITS_H
