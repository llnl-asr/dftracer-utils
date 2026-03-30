#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_IMPL_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_IMPL_H

#include <dftracer/utils/core/coro/coroutine_traits.h>
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task_traits.h>

#include <cstdio>
#include <type_traits>

namespace dftracer::utils {

// ============================================================================
// Input decoding helper
//
// Converts std::any to the typed input expected by the user function.
// Handles: std::any passthrough, tuple from vector<any>, typed vector,
// and concrete type via any_cast.
// ============================================================================

namespace detail {

template <typename T>
T decode_input(const std::any& input) {
    if constexpr (std::is_same_v<T, std::any>) {
        return input;
    } else if constexpr (is_tuple_v<T>) {
        try {
            auto vec = std::any_cast<std::vector<std::any>>(input);
            return vector_to_tuple<T>(vec);
        } catch (const std::bad_any_cast&) {
            return std::any_cast<T>(input);
        }
    } else if constexpr (is_std_vector_v<T>) {
        using Elem = vector_element_type_t<T>;
        try {
            auto vec = std::any_cast<std::vector<std::any>>(input);
            return vector_any_to_typed<Elem>(vec);
        } catch (const std::bad_any_cast&) {
            return std::any_cast<T>(input);
        }
    } else {
        return std::any_cast<T>(input);
    }
}

}  // namespace detail

// ============================================================================
// wrap_function: type-erased coroutine factory
//
// Wraps any user callable into the uniform signature:
//   CoroTask<std::any>(CoroScope&, const std::any&)
// ============================================================================

template <typename Func>
std::function<coro::CoroTask<std::any>(CoroScope&, const std::any&)>
Task::wrap_function(Func&& func) {
    using Traits = detail::function_traits<std::decay_t<Func>>;
    using InputType = typename Traits::input_type;
    using OutputType = typename Traits::output_type;
    constexpr bool has_ctx = Traits::has_context;
    constexpr bool is_coro = Traits::is_coroutine;

    auto func_ptr =
        std::make_shared<std::decay_t<Func>>(std::forward<Func>(func));

    return [func_ptr](CoroScope& ctx,
                      const std::any& input) -> coro::CoroTask<std::any> {
        // Build the invocation based on input type and context
        // Note: avoid IIFE pattern here as it causes GCC 11/13 coroutine bugs.
        // Instead, use explicit if-constexpr branches for coroutine invocation.

        if constexpr (std::is_void_v<InputType>) {
            // No input argument
            if constexpr (is_coro) {
                if constexpr (has_ctx) {
                    auto user_coro = (*func_ptr)(ctx);
                    if constexpr (std::is_void_v<OutputType>) {
                        co_await std::move(user_coro);
                        co_return std::any{};
                    } else {
                        auto result = co_await std::move(user_coro);
                        co_return std::any(std::move(result));
                    }
                } else {
                    auto user_coro = (*func_ptr)();
                    if constexpr (std::is_void_v<OutputType>) {
                        co_await std::move(user_coro);
                        co_return std::any{};
                    } else {
                        auto result = co_await std::move(user_coro);
                        co_return std::any(std::move(result));
                    }
                }
            } else {
                if constexpr (has_ctx) {
                    if constexpr (std::is_void_v<OutputType>) {
                        (*func_ptr)(ctx);
                        co_return std::any{};
                    } else {
                        co_return std::any((*func_ptr)(ctx));
                    }
                } else {
                    if constexpr (std::is_void_v<OutputType>) {
                        (*func_ptr)();
                        co_return std::any{};
                    } else {
                        co_return std::any((*func_ptr)());
                    }
                }
            }
        } else {
            // Has input -- decode it
            InputType typed_input = detail::decode_input<InputType>(input);

            if constexpr (is_coro) {
                if constexpr (detail::is_tuple_v<InputType> && has_ctx) {
                    auto user_coro = detail::apply_tuple_with_context(
                        *func_ptr, ctx, typed_input);
                    if constexpr (std::is_void_v<OutputType>) {
                        co_await std::move(user_coro);
                        co_return std::any{};
                    } else {
                        auto result = co_await std::move(user_coro);
                        co_return std::any(std::move(result));
                    }
                } else if constexpr (detail::is_tuple_v<InputType>) {
                    auto user_coro =
                        detail::apply_tuple(*func_ptr, typed_input);
                    if constexpr (std::is_void_v<OutputType>) {
                        co_await std::move(user_coro);
                        co_return std::any{};
                    } else {
                        auto result = co_await std::move(user_coro);
                        co_return std::any(std::move(result));
                    }
                } else if constexpr (has_ctx) {
                    auto user_coro = (*func_ptr)(ctx, typed_input);
                    if constexpr (std::is_void_v<OutputType>) {
                        co_await std::move(user_coro);
                        co_return std::any{};
                    } else if constexpr (std::is_same_v<OutputType, std::any>) {
                        co_return co_await std::move(user_coro);
                    } else {
                        auto result = co_await std::move(user_coro);
                        co_return std::any(std::move(result));
                    }
                } else {
                    auto user_coro = (*func_ptr)(typed_input);
                    if constexpr (std::is_void_v<OutputType>) {
                        co_await std::move(user_coro);
                        co_return std::any{};
                    } else {
                        auto result = co_await std::move(user_coro);
                        co_return std::any(std::move(result));
                    }
                }
            } else {
                if constexpr (std::is_void_v<OutputType>) {
                    if constexpr (detail::is_tuple_v<InputType> && has_ctx) {
                        detail::apply_tuple_with_context(*func_ptr, ctx,
                                                         typed_input);
                    } else if constexpr (detail::is_tuple_v<InputType>) {
                        detail::apply_tuple(*func_ptr, typed_input);
                    } else if constexpr (has_ctx) {
                        (*func_ptr)(ctx, typed_input);
                    } else {
                        (*func_ptr)(typed_input);
                    }
                    co_return std::any{};
                } else {
                    if constexpr (detail::is_tuple_v<InputType> && has_ctx) {
                        co_return std::any(detail::apply_tuple_with_context(
                            *func_ptr, ctx, typed_input));
                    } else if constexpr (detail::is_tuple_v<InputType>) {
                        co_return std::any(
                            detail::apply_tuple(*func_ptr, typed_input));
                    } else if constexpr (has_ctx) {
                        co_return std::any((*func_ptr)(ctx, typed_input));
                    } else {
                        co_return std::any((*func_ptr)(typed_input));
                    }
                }
            }
        }
    };
}

// ============================================================================
// Type deduction helpers
// ============================================================================

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
    input_combiner_ =
        [combiner](const std::vector<std::any>& inputs) -> std::any {
        if (inputs.size() != sizeof...(Args)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "Combiner expects %zu inputs but received %zu",
                          sizeof...(Args), inputs.size());
            throw PipelineError(PipelineError::VALIDATION_ERROR, buf);
        }

        if constexpr (sizeof...(Args) == 1) {
            return combiner(std::any_cast<Args...>(inputs[0]));
        } else {
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
    using traits = detail::function_traits<std::decay_t<Func>>;
    using func_type = typename traits::template as_std_function<std::any>;
    func_type typed_combiner = std::forward<Func>(combiner);
    return with_combiner(typed_combiner);
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_IMPL_H
