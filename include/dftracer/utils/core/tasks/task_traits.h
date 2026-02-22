#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_TRAITS_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_TRAITS_H

#include <dftracer/utils/core/coro/coroutine_traits.h>

#include <any>
#include <cstddef>
#include <functional>
#include <tuple>
#include <type_traits>
#include <vector>

namespace dftracer::utils {

class CoroScope;

namespace detail {

// ============================================================================
// Step 1: Decompose callable signature (call_traits)
//
// Extracts return_type, args_tuple, and arity from any callable.
// 6 member-function-pointer specializations (const/non-const × noexcept)
// + 2 function-pointer specializations + callable-object delegation.
// ============================================================================

template <typename T, typename = void>
struct call_traits {
    static constexpr bool is_valid = false;
};

template <typename C, typename R, typename... Args>
struct call_traits_base {
    static constexpr bool is_valid = true;
    using return_type = R;
    using args_tuple = std::tuple<std::decay_t<Args>...>;
    using raw_args_tuple = std::tuple<Args...>;
    static constexpr std::size_t arity = sizeof...(Args);
};

// const member function (covers most lambdas)
template <typename C, typename R, typename... Args>
struct call_traits<R (C::*)(Args...) const> : call_traits_base<C, R, Args...> {
};

// non-const member function (mutable lambdas)
template <typename C, typename R, typename... Args>
struct call_traits<R (C::*)(Args...)> : call_traits_base<C, R, Args...> {};

// const noexcept member function (GCC 14 treats noexcept as part of type)
template <typename C, typename R, typename... Args>
struct call_traits<R (C::*)(Args...) const noexcept>
    : call_traits_base<C, R, Args...> {};

// non-const noexcept member function
template <typename C, typename R, typename... Args>
struct call_traits<R (C::*)(Args...) noexcept>
    : call_traits_base<C, R, Args...> {};

// Function pointer
template <typename R, typename... Args>
struct call_traits<R (*)(Args...)> : call_traits_base<void, R, Args...> {};

// Function pointer (noexcept)
template <typename R, typename... Args>
struct call_traits<R (*)(Args...) noexcept>
    : call_traits_base<void, R, Args...> {};

// Callable objects: delegate to operator()
template <typename F>
struct call_traits<F, std::void_t<decltype(&std::decay_t<F>::operator())>>
    : call_traits<decltype(&std::decay_t<F>::operator())> {};

// ============================================================================
// Step 2: Analyze args tuple to extract context/input
//
// Given a decayed args_tuple from call_traits, determines:
// - has_context: whether CoroScope is a parameter
// - input_type: the logical input type (void, T, or tuple<T1,T2,...>)
// ============================================================================

template <typename ArgsTuple>
struct analyze_args;

// () -> void input, no context
template <>
struct analyze_args<std::tuple<>> {
    using input_type = void;
    static constexpr bool has_context = false;
};

// (CoroScope) -> void input, has context
// Note: std::decay_t<CoroScope&> is CoroScope
template <>
struct analyze_args<std::tuple<CoroScope>> {
    using input_type = void;
    static constexpr bool has_context = true;
};

// (T) -> T input, no context
// This is less specialized than (CoroScope) above, so no ambiguity.
template <typename T>
struct analyze_args<std::tuple<T>> {
    using input_type = T;
    static constexpr bool has_context = false;
};

// (CoroScope, T) -> T input, has context
template <typename T>
struct analyze_args<std::tuple<CoroScope, T>> {
    using input_type = T;
    static constexpr bool has_context = true;
};

// (CoroScope, T1, T2, ...) -> tuple<T1,T2,...> input, has context
// More specialized than (T1, T2, Rest...) below due to CoroScope match.
template <typename T1, typename T2, typename... Rest>
struct analyze_args<std::tuple<CoroScope, T1, T2, Rest...>> {
    using input_type = std::tuple<T1, T2, Rest...>;
    static constexpr bool has_context = true;
};

// (T1, T2, ...) -> tuple<T1,T2,...> input, no context
template <typename T1, typename T2, typename... Rest>
struct analyze_args<std::tuple<T1, T2, Rest...>> {
    using input_type = std::tuple<T1, T2, Rest...>;
    static constexpr bool has_context = false;
};

// ============================================================================
// Step 3: make_std_function_from_tuple
//
// Converts args_tuple back into std::function<RetType(Args...)>.
// Used by with_combiner() to wrap lambdas as typed std::function.
// ============================================================================

template <typename RetType, typename ArgsTuple>
struct make_std_function_from_tuple;

template <typename RetType, typename... Args>
struct make_std_function_from_tuple<RetType, std::tuple<Args...>> {
    using type = std::function<RetType(Args...)>;
};

// ============================================================================
// Step 4: Combined function_traits (public API)
//
// The single entry point for all type deduction. Provides:
// - input_type, output_type, has_context, is_coroutine
// - as_std_function<RetType> alias template for with_combiner
// ============================================================================

template <typename Func>
struct function_traits {
    using CT = call_traits<std::decay_t<Func>>;
    static_assert(CT::is_valid,
                  "make_task: callable must have a non-generic operator(). "
                  "Use concrete parameter types, not auto.");

    using raw_return = typename CT::return_type;
    using AA = analyze_args<typename CT::args_tuple>;

    static constexpr bool has_context = AA::has_context;
    using input_type = typename AA::input_type;

    static constexpr bool is_coroutine = coro::is_coro_task_v<raw_return>;
    using output_type = coro::unwrap_coro_task_t<raw_return>;

    static constexpr std::size_t arity = CT::arity;
    static constexpr bool is_generic = false;

    // For with_combiner: builds std::function from raw args (preserving
    // original qualifiers like const& -- NOT decayed)
    template <typename RetType>
    using as_std_function = typename make_std_function_from_tuple<
        RetType, typename CT::raw_args_tuple>::type;
};

// ============================================================================
// Tuple / vector helpers (unchanged from original)
// ============================================================================

template <typename T>
struct is_tuple : std::false_type {};

template <typename... Args>
struct is_tuple<std::tuple<Args...>> : std::true_type {};

template <typename T>
inline constexpr bool is_tuple_v = is_tuple<T>::value;

template <typename T>
struct is_std_vector : std::false_type {};

template <typename T, typename Alloc>
struct is_std_vector<std::vector<T, Alloc>> : std::true_type {};

template <typename T>
inline constexpr bool is_std_vector_v = is_std_vector<T>::value;

template <typename T>
struct vector_element_type {
    using type = void;
};

template <typename T, typename Alloc>
struct vector_element_type<std::vector<T, Alloc>> {
    using type = T;
};

template <typename T>
using vector_element_type_t = typename vector_element_type<T>::type;

template <typename T>
std::vector<T> vector_any_to_typed(const std::vector<std::any>& vec) {
    std::vector<T> result;
    result.reserve(vec.size());
    for (const auto& item : vec) {
        result.push_back(std::any_cast<T>(item));
    }
    return result;
}

template <typename TargetTuple, typename AnyTuple, std::size_t... Is>
TargetTuple convert_any_tuple_impl(const AnyTuple& any_tuple,
                                   std::index_sequence<Is...>) {
    using std::get;
    return TargetTuple(std::any_cast<std::tuple_element_t<Is, TargetTuple>>(
        get<Is>(any_tuple))...);
}

template <typename TargetTuple, typename AnyTuple>
TargetTuple convert_any_tuple(const AnyTuple& any_tuple) {
    return convert_any_tuple_impl<TargetTuple>(
        any_tuple, std::make_index_sequence<std::tuple_size_v<TargetTuple>>{});
}

template <typename Func, typename Tuple, std::size_t... Is>
auto apply_tuple_impl(Func&& func, Tuple&& tuple, std::index_sequence<Is...>) {
    return std::forward<Func>(func)(
        std::get<Is>(std::forward<Tuple>(tuple))...);
}

template <typename Func, typename Tuple>
auto apply_tuple(Func&& func, Tuple&& tuple) {
    return apply_tuple_impl(
        std::forward<Func>(func), std::forward<Tuple>(tuple),
        std::make_index_sequence<std::tuple_size_v<std::decay_t<Tuple>>>{});
}

template <typename Func, typename Tuple, std::size_t... Is>
auto apply_tuple_with_context_impl(Func&& func, CoroScope& ctx, Tuple&& tuple,
                                   std::index_sequence<Is...>) {
    return std::forward<Func>(func)(
        ctx, std::get<Is>(std::forward<Tuple>(tuple))...);
}

template <typename Func, typename Tuple>
auto apply_tuple_with_context(Func&& func, CoroScope& ctx, Tuple&& tuple) {
    return apply_tuple_with_context_impl(
        std::forward<Func>(func), ctx, std::forward<Tuple>(tuple),
        std::make_index_sequence<std::tuple_size_v<std::decay_t<Tuple>>>{});
}

template <typename TargetTuple, std::size_t... Is>
TargetTuple vector_to_tuple_impl(const std::vector<std::any>& vec,
                                 std::index_sequence<Is...>) {
    return std::make_tuple(
        std::any_cast<std::tuple_element_t<Is, TargetTuple>>(vec[Is])...);
}

template <typename TargetTuple>
TargetTuple vector_to_tuple(const std::vector<std::any>& vec) {
    return vector_to_tuple_impl<TargetTuple>(
        vec, std::make_index_sequence<std::tuple_size_v<TargetTuple>>{});
}

}  // namespace detail

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_TRAITS_H
