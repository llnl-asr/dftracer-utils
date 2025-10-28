#ifndef DFTRACER_UTILS_CORE_UTILITIES_UTILITY_ADAPTER_H
#define DFTRACER_UTILS_CORE_UTILITIES_UTILITY_ADAPTER_H

#include <dftracer/utils/core/common/type_name.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/behaviors/default_behaviors.h>
#include <dftracer/utils/core/utilities/tags/monitored.h>
#include <dftracer/utils/core/utilities/tags/needs_context.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/core/utilities/utility_traits.h>

#include <memory>
#include <optional>
#include <vector>

namespace dftracer::utils::utilities {

/**
 * @brief Adapter that wraps Utility as a Task with behavior support.
 *
 * This adapter provides a fluent interface for converting utilities into tasks
 * with optional behaviors. The primary operation is as_task() which returns a
 * std::shared_ptr<Task> that can be used with the standard Task API.
 *
 * Design Philosophy:
 * - Utilities are just specialized functions that can be wrapped as tasks
 * - Behaviors are EXPLICIT and opt-in via with_behavior()
 * - The Task API handles dependencies, scheduling, and execution
 * - No coupling to Pipeline - work directly with Scheduler
 *
 * Usage:
 * @code
 * // Basic: Convert utility to task
 * auto task = use(utility).as_task();
 * scheduler.schedule(task, input);
 *
 * // With dependency (using Task API)
 * auto task = use(utility).as_task();
 * task->depends_on(parent_task);
 * scheduler.schedule(root_task, initial_input);
 *
 * // With explicit behavior
 * auto task = use(utility)
 *     .with_behavior(std::make_shared<TimingBehavior<I, O>>())
 *     .as_task();
 *
 * // Implicit conversion to Task
 * std::shared_ptr<Task> task = use(utility);  // Calls as_task() implicitly
 *
 * // Dynamic submission from within a task
 * auto outer_task = make_task([&](TaskContext& ctx) {
 *     auto inner = use(utility).as_task();
 *     auto future = ctx.submit_task(inner, data);
 *     return future.get();
 * });
 * @endcode
 */
template <typename I, typename O, typename... Tags>
class UtilityAdapter {
   private:
    std::shared_ptr<Utility<I, O, Tags...>> utility_;
    behaviors::BehaviorChain<I, O> behavior_chain_;

    /**
     * @brief Build behavior chain from utility's tags.
     *
     * Uses BehaviorFactory to create behaviors for each tag that has
     * a registered behavior creator.
     */
    void build_behaviors_from_tags() {
        auto& factory = behaviors::get_default_behavior_factory<I, O>();

        // Try to create behavior for each tag type
        build_behavior_for_tag<Tags...>(factory);
    }

    /**
     * @brief Recursively build behaviors for each tag type.
     */
    template <typename FirstTag, typename... RestTags>
    void build_behavior_for_tag(behaviors::BehaviorFactory<I, O>& factory) {
        // Check if factory has a behavior for this tag
        if (factory.template has<FirstTag>()) {
            // Get tag instance from utility
            auto tag = utility_->template get_tag<FirstTag>();

            // Special handling for Monitored tag - inject utility class name
            if constexpr (std::is_same_v<FirstTag, tags::Monitored>) {
                if (tag.utility_name == "Utility") {
                    // Extract and set the actual utility class name
                    std::string full_name = get_type_name(*utility_);
                    tag.utility_name = extract_class_name(full_name);
                }
            }

            // Create behavior from tag
            auto behavior = factory.template create<FirstTag>(tag);
            if (behavior) {
                behavior_chain_.add_behavior(behavior);
            }
        }

        // Process remaining tags
        if constexpr (sizeof...(RestTags) > 0) {
            build_behavior_for_tag<RestTags...>(factory);
        }
    }

    /**
     * @brief Base case for tag recursion.
     */
    template <typename... EmptyTags>
    void build_behavior_for_tag(
        [[maybe_unused]] behaviors::BehaviorFactory<I, O>& factory,
        std::enable_if_t<sizeof...(EmptyTags) == 0>* = nullptr) {
        // Base case: no tags to process
    }

   public:
    /**
     * @brief Construct adapter from utility.
     *
     * Automatically creates behaviors from the utility's tags using
     * the default BehaviorFactory.
     *
     * @param utility Shared pointer to the utility to adapt
     */
    explicit UtilityAdapter(std::shared_ptr<Utility<I, O, Tags...>> utility)
        : utility_(std::move(utility)) {
        build_behaviors_from_tags();
    }

    /**
     * @brief Add a custom behavior to the chain.
     *
     * Manually adds a behavior beyond those automatically created from tags.
     * Useful for custom behaviors or when not using tags.
     *
     * @param behavior Shared pointer to behavior to add
     * @return Reference to this adapter for chaining
     *
     * Usage:
     * @code
     * use(utility)
     *     .with_behavior(std::make_shared<MyCustomBehavior<I, O>>())
     *     .emit_on(pipeline);
     * @endcode
     */
    UtilityAdapter& with_behavior(
        std::shared_ptr<behaviors::UtilityBehavior<I, O>> behavior) {
        behavior_chain_.add_behavior(behavior);
        return *this;
    }

    /**
     * @brief Check if utility needs TaskContext at compile time.
     */
    static constexpr bool needs_context() {
        using UtilityType = Utility<I, O, Tags...>;
        using ConcreteType =
            typename std::remove_reference<decltype(*utility_)>::type;

        return UtilityType::template has_tag<tags::NeedsContext>() ||
               detail::has_process_with_context_v<ConcreteType, I, O>;
    }

    /**
     * @brief Convert utility to a Task (primary operation).
     *
     * Creates a std::shared_ptr<Task> that wraps the utility with any behaviors
     * that have been added. The returned task can be used with the standard
     * Task API (depends_on, with_name, etc.) and scheduled via Scheduler.
     *
     * The task automatically detects if the utility needs TaskContext and
     * creates the appropriate function signature.
     *
     * @return Shared pointer to Task wrapping this utility
     *
     * @example
     * @code
     * auto task = use(utility).as_task();
     * task->with_name("MyUtility");
     * task->depends_on(parent_task);
     * scheduler.schedule(root, input);
     * auto result = task->get<OutputType>();
     * @endcode
     */
    std::shared_ptr<Task> as_task() {
        using UtilityType = Utility<I, O, Tags...>;
        using ConcreteType =
            typename std::remove_reference<decltype(*utility_)>::type;

        // Create executor with utility and behavior chain
        auto executor =
            std::make_shared<behaviors::UtilityExecutor<I, O, Tags...>>(
                utility_, behavior_chain_);

        // Get utility name for task naming
        std::string task_name = utility_->get_name();
        if (task_name.empty()) {
            task_name = "Utility";
        }

        // Create task based on whether utility needs context
        if constexpr (UtilityType::template has_tag<tags::NeedsContext>() ||
                      detail::has_process_with_context_v<ConcreteType, I, O>) {
            return make_task(
                [executor](TaskContext& ctx, I input) -> O {
                    return executor->execute_with_context(ctx, input);
                },
                task_name);
        } else {
            return make_task(
                [executor](I input) -> O { return executor->execute(input); },
                task_name);
        }
    }

    /**
     * @brief Implicit conversion to Task for convenience.
     *
     * Allows using UtilityAdapter directly where a std::shared_ptr<Task> is
     * expected:
     * @code
     * std::shared_ptr<Task> task = use(utility);  // Implicit conversion
     * @endcode
     */
    operator std::shared_ptr<Task>() { return as_task(); }
};

// Helper to expand tuple tags into parameter pack
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
 * @brief Factory function to create a UtilityAdapter with natural syntax.
 *
 * This function provides convenient template argument deduction and enables
 * fluent, natural-language-like syntax for wrapping utilities as tasks:
 *
 * @code
 * auto utility = std::make_shared<MyUtility>();
 *
 * // Basic usage: Convert to task
 * auto task = use(utility).as_task();
 * scheduler.schedule(task, input);
 *
 * // With behaviors (explicit)
 * auto task = use(utility)
 *     .with_behavior(std::make_shared<MonitoringBehavior<I, O>>())
 *     .as_task();
 *
 * // Implicit conversion
 * std::shared_ptr<Task> task = use(utility);
 *
 * // Use with Task API
 * auto parent = make_task([]() { return 42; });
 * auto child = use(utility).as_task();
 * child->depends_on(parent);
 * @endcode
 *
 * The name "use" was chosen to read naturally: "use this utility as a task".
 *
 * This function automatically handles derived utility classes by deducing
 * the base Utility type from the Input, Output, and TagsTuple members.
 *
 * @param utility Shared pointer to utility (can be derived class)
 * @return UtilityAdapter ready for conversion to Task via as_task()
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
