#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_IMPL_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_IMPL_H

#include <dftracer/utils/core/coro/coroutine_traits.h>
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/tasks/task_traits.h>

#include <tuple>
#include <type_traits>

namespace dftracer::utils {

// Template implementations

template <typename Func>
std::function<coro::CoroTask<std::any>(TaskContext&, const std::any&)>
Task::wrap_function(Func&& func) {
    using Traits = detail::function_traits<std::decay_t<Func>>;
    using InputType = typename Traits::input_type;
    using RawOutputType = typename Traits::output_type;

    constexpr bool is_coroutine = coro::is_coro_task_v<RawOutputType>;
    using OutputType = coro::unwrap_coro_task_t<RawOutputType>;

    auto func_ptr =
        std::make_shared<std::decay_t<Func>>(std::forward<Func>(func));

    if constexpr (std::is_void_v<InputType>) {
        if constexpr (Traits::has_context) {
            return [func_ptr](TaskContext& ctx,
                              const std::any&) -> coro::CoroTask<std::any> {
                if constexpr (is_coroutine) {
                    auto user_coro = (*func_ptr)(ctx);
                    if constexpr (std::is_void_v<OutputType>) {
                        co_await std::move(user_coro);
                        co_return std::any{};
                    } else {
                        auto result = co_await std::move(user_coro);
                        co_return std::any(std::move(result));
                    }
                } else {
                    if constexpr (std::is_void_v<OutputType>) {
                        (*func_ptr)(ctx);
                        co_return std::any{};
                    } else {
                        co_return std::any((*func_ptr)(ctx));
                    }
                }
            };
        } else {
            return [func_ptr](TaskContext&,
                              const std::any&) -> coro::CoroTask<std::any> {
                if constexpr (is_coroutine) {
                    auto user_coro = (*func_ptr)();
                    if constexpr (std::is_void_v<OutputType>) {
                        co_await std::move(user_coro);
                        co_return std::any{};
                    } else {
                        auto result = co_await std::move(user_coro);
                        co_return std::any(std::move(result));
                    }
                } else {
                    if constexpr (std::is_void_v<OutputType>) {
                        (*func_ptr)();
                        co_return std::any{};
                    } else {
                        co_return std::any((*func_ptr)());
                    }
                }
            };
        }
    } else {
        if constexpr (Traits::has_context) {
            return [func_ptr](
                       TaskContext& ctx,
                       const std::any& input) -> coro::CoroTask<std::any> {
                InputType typed_input;
                if constexpr (detail::is_tuple_v<InputType>) {
                    try {
                        auto vec = std::any_cast<std::vector<std::any>>(input);
                        typed_input = detail::vector_to_tuple<InputType>(vec);
                    } catch (const std::bad_any_cast&) {
                        typed_input = std::any_cast<InputType>(input);
                    }
                } else if constexpr (detail::is_std_vector_v<InputType>) {
                    using ElemType = detail::vector_element_type_t<InputType>;
                    try {
                        auto vec = std::any_cast<std::vector<std::any>>(input);
                        typed_input =
                            detail::vector_any_to_typed<ElemType>(vec);
                    } catch (const std::bad_any_cast&) {
                        typed_input = std::any_cast<InputType>(input);
                    }
                } else if constexpr (std::is_same_v<InputType, std::any>) {
                    typed_input = input;
                } else {
                    typed_input = std::any_cast<InputType>(input);
                }
                if constexpr (is_coroutine) {
                    // Note: Avoid IIFE pattern here as it causes GCC 11/13
                    // coroutine bugs
                    if constexpr (detail::is_tuple_v<InputType>) {
                        auto user_coro = detail::apply_tuple_with_context(
                            *func_ptr, ctx, typed_input);
                        if constexpr (std::is_void_v<OutputType>) {
                            co_await std::move(user_coro);
                            co_return std::any{};
                        } else {
                            auto result = co_await std::move(user_coro);
                            co_return std::any(std::move(result));
                        }
                    } else {
                        auto user_coro = (*func_ptr)(ctx, typed_input);
                        if constexpr (std::is_void_v<OutputType>) {
                            co_await std::move(user_coro);
                            co_return std::any{};
                        } else if constexpr (std::is_same_v<OutputType,
                                                            std::any>) {
                            // Already std::any, no need to wrap
                            co_return co_await std::move(user_coro);
                        } else {
                            auto result = co_await std::move(user_coro);
                            co_return std::any(std::move(result));
                        }
                    }
                } else {
                    if constexpr (std::is_void_v<OutputType>) {
                        if constexpr (detail::is_tuple_v<InputType>) {
                            detail::apply_tuple_with_context(*func_ptr, ctx,
                                                             typed_input);
                        } else {
                            (*func_ptr)(ctx, typed_input);
                        }
                        co_return std::any{};
                    } else {
                        if constexpr (detail::is_tuple_v<InputType>) {
                            co_return std::any(detail::apply_tuple_with_context(
                                *func_ptr, ctx, typed_input));
                        } else {
                            co_return std::any((*func_ptr)(ctx, typed_input));
                        }
                    }
                }
            };
        } else {
            return [func_ptr](
                       TaskContext&,
                       const std::any& input) -> coro::CoroTask<std::any> {
                InputType typed_input;
                if constexpr (detail::is_tuple_v<InputType>) {
                    try {
                        auto vec = std::any_cast<std::vector<std::any>>(input);
                        typed_input = detail::vector_to_tuple<InputType>(vec);
                    } catch (const std::bad_any_cast&) {
                        typed_input = std::any_cast<InputType>(input);
                    }
                } else if constexpr (detail::is_std_vector_v<InputType>) {
                    using ElemType = detail::vector_element_type_t<InputType>;
                    try {
                        auto vec = std::any_cast<std::vector<std::any>>(input);
                        typed_input =
                            detail::vector_any_to_typed<ElemType>(vec);
                    } catch (const std::bad_any_cast&) {
                        typed_input = std::any_cast<InputType>(input);
                    }
                } else if constexpr (std::is_same_v<InputType, std::any>) {
                    typed_input = input;
                } else {
                    typed_input = std::any_cast<InputType>(input);
                }
                if constexpr (is_coroutine) {
                    // Note: Avoid IIFE pattern here as it causes GCC 11/13
                    // coroutine bugs
                    if constexpr (detail::is_tuple_v<InputType>) {
                        auto user_coro =
                            detail::apply_tuple(*func_ptr, typed_input);
                        if constexpr (std::is_void_v<OutputType>) {
                            co_await std::move(user_coro);
                            co_return std::any{};
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
                        if constexpr (detail::is_tuple_v<InputType>) {
                            detail::apply_tuple(*func_ptr, typed_input);
                        } else {
                            (*func_ptr)(typed_input);
                        }
                        co_return std::any{};
                    } else {
                        if constexpr (detail::is_tuple_v<InputType>) {
                            co_return std::any(
                                detail::apply_tuple(*func_ptr, typed_input));
                        } else {
                            co_return std::any((*func_ptr)(typed_input));
                        }
                    }
                }
            };
        }
    }
}

template <typename Func>
std::type_index Task::deduce_input_type() {
    using Traits = detail::function_traits<std::decay_t<Func>>;
    using RawInputType = typename Traits::input_type;
    using InputType = coro::unwrap_coro_task_t<RawInputType>;

    if constexpr (std::is_void_v<InputType>) {
        return typeid(void);
    } else {
        return typeid(InputType);
    }
}

template <typename Func>
std::type_index Task::deduce_output_type() {
    using Traits = detail::function_traits<std::decay_t<Func>>;
    using RawOutputType = typename Traits::output_type;
    using OutputType = coro::unwrap_coro_task_t<RawOutputType>;

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
            std::ostringstream oss;
            oss << "Combiner expects " << sizeof...(Args)
                << " inputs but received " << inputs.size();
            throw PipelineError(PipelineError::VALIDATION_ERROR, oss.str());
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
    using traits =
        detail::function_traits<decltype(&std::decay_t<Func>::operator())>;
    using func_type = typename traits::template as_std_function<std::any>;
    func_type typed_combiner = std::forward<Func>(combiner);
    return with_combiner(typed_combiner);
}

// ============================================================================
// TaskContext::spawn_untracked template implementation
// ============================================================================

template <typename Func>
auto TaskContext::spawn_untracked(Func&& func) -> TaskFuture<
    typename std::invoke_result_t<Func, TaskContext&>::value_type> {
    auto task = make_task(std::forward<Func>(func));
    return spawn_untracked(task);
}

// ============================================================================
// TaskFuture::get() implementations
// ============================================================================

template <typename T>
T TaskFuture<T>::get() {
    if (!task_) {
        throw std::runtime_error("Invalid TaskFuture");
    }
    std::any result_any = task_->get_future().get();

    if constexpr (!std::is_void_v<T>) {
        if constexpr (std::is_same_v<T, std::any>) {
            return result_any;
        } else {
            return std::any_cast<T>(result_any);
        }
    }
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_IMPL_H
