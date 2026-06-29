#ifndef DFTRACER_UTILS_CORE_UTILITIES_UTILITY_ADAPTER_H
#define DFTRACER_UTILS_CORE_UTILITIES_UTILITY_ADAPTER_H

#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/tags/needs_context.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/core/utilities/utility_traits.h>

#include <memory>

namespace dftracer::utils::utilities {

/**
 * @brief Adapter that wraps a Utility as a Task.
 *
 * Provides the fluent `use(utility).as_task()` API: it returns a
 * std::shared_ptr<Task> usable with the standard Task API (depends_on, etc.).
 * Execution goes through UtilityExecutor, which injects the CoroScope context
 * for context-needing utilities and applies env-gated monitoring.
 *
 * Usage:
 * @code
 * auto task = use(utility).as_task();
 * task->depends_on(parent_task);
 * @endcode
 *
 * @tparam I Input type
 * @tparam O Output type
 * @tparam Tags Variadic tag types
 */
template <typename I, typename O, typename... Tags>
class UtilityAdapter {
   private:
    std::shared_ptr<Utility<I, O, Tags...>> utility_;

   public:
    explicit UtilityAdapter(std::shared_ptr<Utility<I, O, Tags...>> utility)
        : utility_(std::move(utility)) {}

    /**
     * @brief Check if the utility needs CoroScope at compile time.
     */
    static constexpr bool needs_context() {
        using UtilityType = Utility<I, O, Tags...>;
        using ConcreteType =
            typename std::remove_reference<decltype(*utility_)>::type;

        return UtilityType::template has_tag<tags::NeedsContext>() ||
               detail::has_process_with_context_v<ConcreteType, I, O>;
    }

    /**
     * @brief Convert the utility to a Task.
     *
     * Detects whether the utility needs CoroScope and builds the matching task
     * signature, executing via UtilityExecutor.
     */
    std::shared_ptr<Task> as_task() {
        using UtilityType = Utility<I, O, Tags...>;
        using ConcreteType =
            typename std::remove_reference<decltype(*utility_)>::type;

        auto executor =
            std::make_shared<behaviors::UtilityExecutor<I, O, Tags...>>(
                utility_);

        if constexpr (UtilityType::template has_tag<tags::NeedsContext>() ||
                      detail::has_process_with_context_v<ConcreteType, I, O>) {
            return make_task(
                [executor](CoroScope& ctx, I input) -> coro::CoroTask<O> {
                    co_return co_await executor->execute(ctx, input);
                },
                UtilityType::get_name());
        } else {
            return make_task(
                [executor](I input) -> coro::CoroTask<O> {
                    co_return co_await executor->execute(input);
                },
                UtilityType::get_name());
        }
    }

    /**
     * @brief Implicit conversion to Task for convenience.
     */
    operator std::shared_ptr<Task>() { return as_task(); }
};

// Helper to expand tuple tags into a parameter pack.
namespace detail {
template <typename I, typename O, typename TagsTuple>
struct UseHelper;

template <typename I, typename O, typename... Tags>
struct UseHelper<I, O, std::tuple<Tags...>> {
    using UtilityType = Utility<I, O, Tags...>;

    static UtilityAdapter<I, O, Tags...> create(
        std::shared_ptr<UtilityType> utility) {
        return UtilityAdapter<I, O, Tags...>(utility);
    }
};
}  // namespace detail

/**
 * @brief Factory function to create a UtilityAdapter: `use(utility).as_task()`.
 *
 * Reads naturally as "use this utility as a task". Deduces the base Utility
 * type from the utility's Input/Output/TagsTuple, so it also works with derived
 * utility classes.
 *
 * @code
 * auto utility = std::make_shared<MyUtility>();
 *
 * // Basic usage: convert to a task and schedule it
 * auto task = use(utility).as_task();
 * scheduler.schedule(task, input);
 *
 * // Implicit conversion to std::shared_ptr<Task>
 * std::shared_ptr<Task> task = use(utility);
 *
 * // Use with the Task API
 * auto parent = make_task([]() { return 42; });
 * auto child = use(utility).as_task();
 * child->depends_on(parent);
 * @endcode
 *
 * @param utility Shared pointer to the utility (may be a derived class)
 * @return UtilityAdapter ready for conversion to a Task via as_task()
 */
template <typename DerivedUtility>
auto use(std::shared_ptr<DerivedUtility> utility) {
    using I = typename DerivedUtility::Input;
    using O = typename DerivedUtility::Output;
    using TagsTuple = typename DerivedUtility::TagsTuple;

    return detail::UseHelper<I, O, TagsTuple>::create(
        std::static_pointer_cast<
            typename detail::UseHelper<I, O, TagsTuple>::UtilityType>(utility));
}

}  // namespace dftracer::utils::utilities

#endif  // DFTRACER_UTILS_CORE_UTILITIES_UTILITY_ADAPTER_H
