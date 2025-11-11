#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_TRAITS_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_TRAITS_H

#include <any>
#include <cstddef>
#include <functional>
#include <tuple>
#include <type_traits>
#include <vector>

namespace dftracer::utils {

class TaskContext;

// Helper to detect function signature
namespace detail {

// Extract function traits
// Primary template declaration with SFINAE support for generic lambdas
template <typename Func, typename = void>
struct function_traits;

// Function pointer
template <typename R, typename Arg>
struct function_traits<R (*)(Arg)> {
    using input_type = std::decay_t<Arg>;
    using output_type = R;
    static constexpr bool has_context = false;
};

template <typename R, typename Arg>
struct function_traits<R (*)(Arg, TaskContext&)> {
    using input_type = std::decay_t<Arg>;
    using output_type = R;
    static constexpr bool has_context = true;
};

template <typename R, typename Arg>
struct function_traits<R (*)(TaskContext&, Arg)> {
    using input_type = std::decay_t<Arg>;
    using output_type = R;
    static constexpr bool has_context = true;
};

// Lambda/functor (via operator())
template <typename C, typename R, typename Arg>
struct function_traits<R (C::*)(Arg) const> {
    using input_type = std::decay_t<Arg>;
    using output_type = R;
    static constexpr bool has_context = false;

    template <typename RetType>
    using as_std_function = std::function<RetType(Arg)>;
};

template <typename C, typename R, typename Arg>
struct function_traits<R (C::*)(Arg, TaskContext&) const> {
    using input_type = std::decay_t<Arg>;
    using output_type = R;
    static constexpr bool has_context = true;
};

template <typename C, typename R, typename Arg>
struct function_traits<R (C::*)(TaskContext&, Arg) const> {
    using input_type = std::decay_t<Arg>;
    using output_type = R;
    static constexpr bool has_context = true;
};

// Non-const member function
template <typename C, typename R, typename Arg>
struct function_traits<R (C::*)(Arg)> {
    using input_type = std::decay_t<Arg>;
    using output_type = R;
    static constexpr bool has_context = false;
};

template <typename C, typename R, typename Arg>
struct function_traits<R (C::*)(Arg, TaskContext&)> {
    using input_type = std::decay_t<Arg>;
    using output_type = R;
    static constexpr bool has_context = true;
};

template <typename C, typename R, typename Arg>
struct function_traits<R (C::*)(TaskContext&, Arg)> {
    using input_type = std::decay_t<Arg>;
    using output_type = R;
    static constexpr bool has_context = true;
};

// std::function
template <typename R, typename Arg>
struct function_traits<std::function<R(Arg)>> {
    using input_type = std::decay_t<Arg>;
    using output_type = R;
    static constexpr bool has_context = false;
};

template <typename R, typename Arg>
struct function_traits<std::function<R(Arg, TaskContext&)>> {
    using input_type = std::decay_t<Arg>;
    using output_type = R;
    static constexpr bool has_context = true;
};

template <typename R, typename Arg>
struct function_traits<std::function<R(TaskContext&, Arg)>> {
    using input_type = std::decay_t<Arg>;
    using output_type = R;
    static constexpr bool has_context = true;
};

// Void input specializations
template <typename R>
struct function_traits<R (*)()> {
    using input_type = void;
    using output_type = R;
    static constexpr bool has_context = false;
};

template <typename R>
struct function_traits<R (*)(TaskContext&)> {
    using input_type = void;
    using output_type = R;
    static constexpr bool has_context = true;
};

template <typename C, typename R>
struct function_traits<R (C::*)() const> {
    using input_type = void;
    using output_type = R;
    static constexpr bool has_context = false;
};

template <typename C, typename R>
struct function_traits<R (C::*)()> {
    using input_type = void;
    using output_type = R;
    static constexpr bool has_context = false;
};

template <typename C, typename R>
struct function_traits<R (C::*)(TaskContext&) const> {
    using input_type = void;
    using output_type = R;
    static constexpr bool has_context = true;
};

template <typename C, typename R>
struct function_traits<R (C::*)(TaskContext&)> {
    using input_type = void;
    using output_type = R;
    static constexpr bool has_context = true;
};

template <typename R>
struct function_traits<std::function<R()>> {
    using input_type = void;
    using output_type = R;
    static constexpr bool has_context = false;
};

template <typename R>
struct function_traits<std::function<R(TaskContext&)>> {
    using input_type = void;
    using output_type = R;
    static constexpr bool has_context = true;
};

// Multi-argument function specializations (for tuple-based combiners)
// 2 arguments (without context)
template <typename R, typename Arg1, typename Arg2>
struct function_traits<R (*)(Arg1, Arg2)> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr std::size_t arity = 2;
};

template <typename C, typename R, typename Arg1, typename Arg2>
struct function_traits<R (C::*)(Arg1, Arg2) const> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr std::size_t arity = 2;

    template <typename RetType>
    using as_std_function = std::function<RetType(Arg1, Arg2)>;
};

// 2 arguments (with context)
template <typename R, typename Arg1, typename Arg2>
struct function_traits<R (*)(TaskContext&, Arg1, Arg2)> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>>;
    using output_type = R;
    static constexpr bool has_context = true;
    static constexpr std::size_t arity = 2;
};

template <typename C, typename R, typename Arg1, typename Arg2>
struct function_traits<R (C::*)(TaskContext&, Arg1, Arg2) const> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>>;
    using output_type = R;
    static constexpr bool has_context = true;
    static constexpr std::size_t arity = 2;
};

// 3 arguments (without context)
template <typename R, typename Arg1, typename Arg2, typename Arg3>
struct function_traits<R (*)(Arg1, Arg2, Arg3)> {
    using input_type =
        std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>, std::decay_t<Arg3>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr std::size_t arity = 3;
};

template <typename C, typename R, typename Arg1, typename Arg2, typename Arg3>
struct function_traits<R (C::*)(Arg1, Arg2, Arg3) const> {
    using input_type =
        std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>, std::decay_t<Arg3>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr std::size_t arity = 3;

    template <typename RetType>
    using as_std_function = std::function<RetType(Arg1, Arg2, Arg3)>;
};

// 3 arguments (with context)
template <typename R, typename Arg1, typename Arg2, typename Arg3>
struct function_traits<R (*)(TaskContext&, Arg1, Arg2, Arg3)> {
    using input_type =
        std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>, std::decay_t<Arg3>>;
    using output_type = R;
    static constexpr bool has_context = true;
    static constexpr std::size_t arity = 3;
};

template <typename C, typename R, typename Arg1, typename Arg2, typename Arg3>
struct function_traits<R (C::*)(TaskContext&, Arg1, Arg2, Arg3) const> {
    using input_type =
        std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>, std::decay_t<Arg3>>;
    using output_type = R;
    static constexpr bool has_context = true;
    static constexpr std::size_t arity = 3;
};

// 4 arguments (without context)
template <typename R, typename Arg1, typename Arg2, typename Arg3,
          typename Arg4>
struct function_traits<R (*)(Arg1, Arg2, Arg3, Arg4)> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>,
                                  std::decay_t<Arg3>, std::decay_t<Arg4>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr std::size_t arity = 4;
};

template <typename C, typename R, typename Arg1, typename Arg2, typename Arg3,
          typename Arg4>
struct function_traits<R (C::*)(Arg1, Arg2, Arg3, Arg4) const> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>,
                                  std::decay_t<Arg3>, std::decay_t<Arg4>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr std::size_t arity = 4;

    template <typename RetType>
    using as_std_function = std::function<RetType(Arg1, Arg2, Arg3, Arg4)>;
};

// 4 arguments (with context)
template <typename R, typename Arg1, typename Arg2, typename Arg3,
          typename Arg4>
struct function_traits<R (*)(TaskContext&, Arg1, Arg2, Arg3, Arg4)> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>,
                                  std::decay_t<Arg3>, std::decay_t<Arg4>>;
    using output_type = R;
    static constexpr bool has_context = true;
    static constexpr std::size_t arity = 4;
};

template <typename C, typename R, typename Arg1, typename Arg2, typename Arg3,
          typename Arg4>
struct function_traits<R (C::*)(TaskContext&, Arg1, Arg2, Arg3, Arg4) const> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>,
                                  std::decay_t<Arg3>, std::decay_t<Arg4>>;
    using output_type = R;
    static constexpr bool has_context = true;
    static constexpr std::size_t arity = 4;
};

// 5 arguments (without context)
template <typename R, typename Arg1, typename Arg2, typename Arg3,
          typename Arg4, typename Arg5>
struct function_traits<R (*)(Arg1, Arg2, Arg3, Arg4, Arg5)> {
    using input_type =
        std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>, std::decay_t<Arg3>,
                   std::decay_t<Arg4>, std::decay_t<Arg5>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr std::size_t arity = 5;
};

template <typename C, typename R, typename Arg1, typename Arg2, typename Arg3,
          typename Arg4, typename Arg5>
struct function_traits<R (C::*)(Arg1, Arg2, Arg3, Arg4, Arg5) const> {
    using input_type =
        std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>, std::decay_t<Arg3>,
                   std::decay_t<Arg4>, std::decay_t<Arg5>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr std::size_t arity = 5;

    template <typename RetType>
    using as_std_function =
        std::function<RetType(Arg1, Arg2, Arg3, Arg4, Arg5)>;
};

// 5 arguments (with context)
template <typename R, typename Arg1, typename Arg2, typename Arg3,
          typename Arg4, typename Arg5>
struct function_traits<R (*)(TaskContext&, Arg1, Arg2, Arg3, Arg4, Arg5)> {
    using input_type =
        std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>, std::decay_t<Arg3>,
                   std::decay_t<Arg4>, std::decay_t<Arg5>>;
    using output_type = R;
    static constexpr bool has_context = true;
    static constexpr std::size_t arity = 5;
};

template <typename C, typename R, typename Arg1, typename Arg2, typename Arg3,
          typename Arg4, typename Arg5>
struct function_traits<R (C::*)(TaskContext&, Arg1, Arg2, Arg3, Arg4, Arg5)
                           const> {
    using input_type =
        std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>, std::decay_t<Arg3>,
                   std::decay_t<Arg4>, std::decay_t<Arg5>>;
    using output_type = R;
    static constexpr bool has_context = true;
    static constexpr std::size_t arity = 5;
};

// Helper to detect if a type has a non-templated (unique) call operator
template <typename T, typename = void>
struct has_unique_call_operator : std::false_type {};

template <typename T>
struct has_unique_call_operator<
    T, std::void_t<decltype(&std::decay_t<T>::operator())>> : std::true_type {};

template <typename T>
inline constexpr bool has_unique_call_operator_v =
    has_unique_call_operator<T>::value;

// Primary template for generic lambdas (auto parameters) - fallback
// These require explicit type handling at the call site
template <typename Func, typename>
struct function_traits {
    using input_type = std::any;   // Unknown input - use std::any
    using output_type = std::any;  // Unknown output - use std::any
    static constexpr bool has_context = false;
    static constexpr bool is_generic = true;
};

// Specialization for non-generic callables (lambdas with concrete types)
template <typename Func>
struct function_traits<Func, std::enable_if_t<has_unique_call_operator_v<Func>>>
    : function_traits<decltype(&std::decay_t<Func>::operator())> {
    static constexpr bool is_generic = false;
};

// Helper to check if a type is a tuple
template <typename T>
struct is_tuple : std::false_type {};

template <typename... Args>
struct is_tuple<std::tuple<Args...>> : std::true_type {};

template <typename T>
inline constexpr bool is_tuple_v = is_tuple<T>::value;

// Helper to check if a type is a std::vector
template <typename T>
struct is_std_vector : std::false_type {};

template <typename T, typename Alloc>
struct is_std_vector<std::vector<T, Alloc>> : std::true_type {};

template <typename T>
inline constexpr bool is_std_vector_v = is_std_vector<T>::value;

// Helper to get vector element type
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

// Helper to convert vector<any> to vector<T>
template <typename T>
std::vector<T> vector_any_to_typed(const std::vector<std::any>& vec) {
    std::vector<T> result;
    result.reserve(vec.size());
    for (const auto& item : vec) {
        result.push_back(std::any_cast<T>(item));
    }
    return result;
}

// Helper to convert tuple<any, any, ...> to tuple<T1, T2, ...>
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

// Helper to apply a tuple to a function
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

// Helper to apply a tuple to a function WITH TaskContext as first arg
template <typename Func, typename Tuple, std::size_t... Is>
auto apply_tuple_with_context_impl(Func&& func, TaskContext& ctx, Tuple&& tuple,
                                   std::index_sequence<Is...>) {
    return std::forward<Func>(func)(
        ctx, std::get<Is>(std::forward<Tuple>(tuple))...);
}

template <typename Func, typename Tuple>
auto apply_tuple_with_context(Func&& func, TaskContext& ctx, Tuple&& tuple) {
    return apply_tuple_with_context_impl(
        std::forward<Func>(func), ctx, std::forward<Tuple>(tuple),
        std::make_index_sequence<std::tuple_size_v<std::decay_t<Tuple>>>{});
}

// Helper to convert vector<any> to typed tuple
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
