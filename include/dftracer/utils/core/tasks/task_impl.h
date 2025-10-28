#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_IMPL_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_IMPL_H

#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/tasks/task_context.h>

#include <tuple>
#include <type_traits>

namespace dftracer::utils {

// Helper to detect function signature
namespace detail {

// Extract function traits
template <typename Func>
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
struct function_traits<R (C::*)(TaskContext&) const> {
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
    static constexpr size_t arity = 2;
};

template <typename C, typename R, typename Arg1, typename Arg2>
struct function_traits<R (C::*)(Arg1, Arg2) const> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr size_t arity = 2;

    template <typename RetType>
    using as_std_function = std::function<RetType(Arg1, Arg2)>;
};

// 2 arguments (with context)
template <typename R, typename Arg1, typename Arg2>
struct function_traits<R (*)(TaskContext&, Arg1, Arg2)> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>>;
    using output_type = R;
    static constexpr bool has_context = true;
    static constexpr size_t arity = 2;
};

template <typename C, typename R, typename Arg1, typename Arg2>
struct function_traits<R (C::*)(TaskContext&, Arg1, Arg2) const> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>>;
    using output_type = R;
    static constexpr bool has_context = true;
    static constexpr size_t arity = 2;
};

// 3 arguments (without context)
template <typename R, typename Arg1, typename Arg2, typename Arg3>
struct function_traits<R (*)(Arg1, Arg2, Arg3)> {
    using input_type =
        std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>, std::decay_t<Arg3>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr size_t arity = 3;
};

template <typename C, typename R, typename Arg1, typename Arg2, typename Arg3>
struct function_traits<R (C::*)(Arg1, Arg2, Arg3) const> {
    using input_type =
        std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>, std::decay_t<Arg3>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr size_t arity = 3;

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
    static constexpr size_t arity = 3;
};

template <typename C, typename R, typename Arg1, typename Arg2, typename Arg3>
struct function_traits<R (C::*)(TaskContext&, Arg1, Arg2, Arg3) const> {
    using input_type =
        std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>, std::decay_t<Arg3>>;
    using output_type = R;
    static constexpr bool has_context = true;
    static constexpr size_t arity = 3;
};

// 4 arguments (without context)
template <typename R, typename Arg1, typename Arg2, typename Arg3,
          typename Arg4>
struct function_traits<R (*)(Arg1, Arg2, Arg3, Arg4)> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>,
                                  std::decay_t<Arg3>, std::decay_t<Arg4>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr size_t arity = 4;
};

template <typename C, typename R, typename Arg1, typename Arg2, typename Arg3,
          typename Arg4>
struct function_traits<R (C::*)(Arg1, Arg2, Arg3, Arg4) const> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>,
                                  std::decay_t<Arg3>, std::decay_t<Arg4>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr size_t arity = 4;

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
    static constexpr size_t arity = 4;
};

template <typename C, typename R, typename Arg1, typename Arg2, typename Arg3,
          typename Arg4>
struct function_traits<R (C::*)(TaskContext&, Arg1, Arg2, Arg3, Arg4) const> {
    using input_type = std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>,
                                  std::decay_t<Arg3>, std::decay_t<Arg4>>;
    using output_type = R;
    static constexpr bool has_context = true;
    static constexpr size_t arity = 4;
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
    static constexpr size_t arity = 5;
};

template <typename C, typename R, typename Arg1, typename Arg2, typename Arg3,
          typename Arg4, typename Arg5>
struct function_traits<R (C::*)(Arg1, Arg2, Arg3, Arg4, Arg5) const> {
    using input_type =
        std::tuple<std::decay_t<Arg1>, std::decay_t<Arg2>, std::decay_t<Arg3>,
                   std::decay_t<Arg4>, std::decay_t<Arg5>>;
    using output_type = R;
    static constexpr bool has_context = false;
    static constexpr size_t arity = 5;

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
    static constexpr size_t arity = 5;
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
    static constexpr size_t arity = 5;
};

// For lambdas and functors, deduce from operator()
template <typename Func>
struct function_traits
    : function_traits<decltype(&std::decay_t<Func>::operator())> {};

// Helper to check if a type is a tuple
template <typename T>
struct is_tuple : std::false_type {};

template <typename... Args>
struct is_tuple<std::tuple<Args...>> : std::true_type {};

template <typename T>
inline constexpr bool is_tuple_v = is_tuple<T>::value;

// Helper to convert tuple<any, any, ...> to tuple<T1, T2, ...>
template <typename TargetTuple, typename AnyTuple, size_t... Is>
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
template <typename Func, typename Tuple, size_t... Is>
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
template <typename Func, typename Tuple, size_t... Is>
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

}  // namespace detail

// Template implementations

template <typename Func>
std::function<std::any(TaskContext&, const std::any&)> Task::wrap_function(
    Func&& func) {
    using Traits = detail::function_traits<std::decay_t<Func>>;
    using InputType = typename Traits::input_type;
    using OutputType = typename Traits::output_type;

    if constexpr (std::is_void_v<InputType>) {
        // Function takes no input (or only TaskContext)
        if constexpr (Traits::has_context) {
            // R(TaskContext&)
            return [f = std::forward<Func>(func)](TaskContext& ctx,
                                                  const std::any&) -> std::any {
                if constexpr (std::is_void_v<OutputType>) {
                    f(ctx);
                    return std::any{};
                } else {
                    return std::any(f(ctx));
                }
            };
        } else {
            // R()
            return [f = std::forward<Func>(func)](TaskContext&,
                                                  const std::any&) -> std::any {
                if constexpr (std::is_void_v<OutputType>) {
                    f();
                    return std::any{};
                } else {
                    return std::any(f());
                }
            };
        }
    } else {
        // Function takes input
        if constexpr (Traits::has_context) {
            // R(TaskContext&, InputType) or R(TaskContext&, Arg1, Arg2, ...)
            // for tuples
            if constexpr (detail::is_tuple_v<InputType>) {
                // Multi-argument function with context - unpack tuple with
                // context first The scheduler creates tuple<any, any, ...>, we
                // need to convert to tuple<T1, T2, ...>
                return [f = std::forward<Func>(func)](
                           TaskContext& ctx,
                           const std::any& input) -> std::any {
                    // Try to extract tuple<any, any, ...> based on arity
                    constexpr size_t arity = std::tuple_size_v<InputType>;
                    InputType typed_input;

                    if constexpr (arity == 2) {
                        auto any_tuple =
                            std::any_cast<std::tuple<std::any, std::any>>(
                                input);
                        typed_input =
                            detail::convert_any_tuple<InputType>(any_tuple);
                    } else if constexpr (arity == 3) {
                        auto any_tuple = std::any_cast<
                            std::tuple<std::any, std::any, std::any>>(input);
                        typed_input =
                            detail::convert_any_tuple<InputType>(any_tuple);
                    } else if constexpr (arity == 4) {
                        auto any_tuple = std::any_cast<
                            std::tuple<std::any, std::any, std::any, std::any>>(
                            input);
                        typed_input =
                            detail::convert_any_tuple<InputType>(any_tuple);
                    } else if constexpr (arity == 5) {
                        auto any_tuple = std::any_cast<std::tuple<
                            std::any, std::any, std::any, std::any, std::any>>(
                            input);
                        typed_input =
                            detail::convert_any_tuple<InputType>(any_tuple);
                    }

                    if constexpr (std::is_void_v<OutputType>) {
                        detail::apply_tuple_with_context(f, ctx, typed_input);
                        return std::any{};
                    } else {
                        return std::any(detail::apply_tuple_with_context(
                            f, ctx, typed_input));
                    }
                };
            } else {
                // Single-argument function with context
                return
                    [f = std::forward<Func>(func)](
                        TaskContext& ctx, const std::any& input) -> std::any {
                        auto typed_input = std::any_cast<InputType>(input);
                        if constexpr (std::is_void_v<OutputType>) {
                            f(ctx, typed_input);
                            return std::any{};
                        } else {
                            return std::any(f(ctx, typed_input));
                        }
                    };
            }
        } else {
            // R(InputType) or R(Arg1, Arg2, ...) for tuples
            if constexpr (detail::is_tuple_v<InputType>) {
                // Multi-argument function - unpack tuple
                // The scheduler creates tuple<any, any, ...>, we need to
                // convert to tuple<T1, T2, ...>
                return [f = std::forward<Func>(func)](
                           TaskContext&, const std::any& input) -> std::any {
                    // Try to extract tuple<any, any, ...> based on arity
                    constexpr size_t arity = std::tuple_size_v<InputType>;
                    InputType typed_input;

                    if constexpr (arity == 2) {
                        auto any_tuple =
                            std::any_cast<std::tuple<std::any, std::any>>(
                                input);
                        typed_input =
                            detail::convert_any_tuple<InputType>(any_tuple);
                    } else if constexpr (arity == 3) {
                        auto any_tuple = std::any_cast<
                            std::tuple<std::any, std::any, std::any>>(input);
                        typed_input =
                            detail::convert_any_tuple<InputType>(any_tuple);
                    } else if constexpr (arity == 4) {
                        auto any_tuple = std::any_cast<
                            std::tuple<std::any, std::any, std::any, std::any>>(
                            input);
                        typed_input =
                            detail::convert_any_tuple<InputType>(any_tuple);
                    } else if constexpr (arity == 5) {
                        auto any_tuple = std::any_cast<std::tuple<
                            std::any, std::any, std::any, std::any, std::any>>(
                            input);
                        typed_input =
                            detail::convert_any_tuple<InputType>(any_tuple);
                    }

                    if constexpr (std::is_void_v<OutputType>) {
                        detail::apply_tuple(f, typed_input);
                        return std::any{};
                    } else {
                        return std::any(detail::apply_tuple(f, typed_input));
                    }
                };
            } else {
                // Single-argument function
                return [f = std::forward<Func>(func)](
                           TaskContext&, const std::any& input) -> std::any {
                    auto typed_input = std::any_cast<InputType>(input);
                    if constexpr (std::is_void_v<OutputType>) {
                        f(typed_input);
                        return std::any{};
                    } else {
                        return std::any(f(typed_input));
                    }
                };
            }
        }
    }
}

template <typename Func>
std::type_index Task::deduce_input_type() {
    using Traits = detail::function_traits<std::decay_t<Func>>;
    using InputType = typename Traits::input_type;

    if constexpr (std::is_void_v<InputType>) {
        return typeid(void);
    } else {
        return typeid(InputType);
    }
}

template <typename Func>
std::type_index Task::deduce_output_type() {
    using Traits = detail::function_traits<std::decay_t<Func>>;
    using OutputType = typename Traits::output_type;

    if constexpr (std::is_void_v<OutputType>) {
        return typeid(void);
    } else {
        return typeid(OutputType);
    }
}

// ============================================================================
// with_combiner implementations
// ============================================================================

template <typename... Args>
std::shared_ptr<Task> Task::with_combiner(
    std::function<std::any(Args...)> combiner) {
    // Wrap the tuple-based combiner to work with vector<any>
    input_combiner_ =
        [combiner](const std::vector<std::any>& inputs) -> std::any {
        if (inputs.size() != sizeof...(Args)) {
            std::ostringstream oss;
            oss << "Combiner expects " << sizeof...(Args)
                << " inputs but received " << inputs.size();
            throw PipelineError(PipelineError::VALIDATION_ERROR, oss.str());
        }

        // Special case: single argument (no tuple unpacking needed)
        if constexpr (sizeof...(Args) == 1) {
            return combiner(std::any_cast<Args...>(inputs[0]));
        } else {
            // Multiple arguments: unpack vector into tuple and call combiner
            return unpack_and_call(combiner, inputs,
                                   std::index_sequence_for<Args...>{});
        }
    };
    has_custom_combiner_ = true;
    return shared_from_this();
}

template <typename Func>
auto Task::with_combiner(Func&& combiner) -> std::enable_if_t<
    !std::is_same_v<std::decay_t<Func>,
                    std::function<std::any(const std::vector<std::any>&)>>,
    std::shared_ptr<Task>> {
    using traits =
        detail::function_traits<decltype(&std::decay_t<Func>::operator())>;
    using func_type = typename traits::template as_std_function<std::any>;
    func_type typed_combiner = std::forward<Func>(combiner);
    return with_combiner(typed_combiner);
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_IMPL_H
