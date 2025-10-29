#ifndef DFTRACER_UTILS_CORE_UTILITIES_UTILITY_ADAPTER_H
#define DFTRACER_UTILS_CORE_UTILITIES_UTILITY_ADAPTER_H

#include <dftracer/utils/core/common/type_name.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/tasks/task_result.h>
#include <dftracer/utils/core/tasks/task_tag.h>
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
 * @brief Adapter that bridges Utility to Pipeline/Task system.
 *
 * This adapter provides a fluent interface for integrating utilities with
 * pipelines or emitting them as dynamic tasks.
 *
 * Usage:
 * @code
 * // Simple emission to pipeline
 * use(utility).emit_on(pipeline);
 *
 * // With dependency
 * use(utility).depends_on(parent_id).emit_on(pipeline);
 *
 * // Dynamic emission with explicit input
 * use(utility).with_input(data).emit_on(ctx);
 *
 * // Batch processing
 * use(utility).emit_on(pipeline, input_vector);
 * @endcode
 */
template <typename I, typename O, typename... Tags>
class UtilityAdapter {
   private:
    std::shared_ptr<Utility<I, O, Tags...>> utility_;
    behaviors::BehaviorChain<I, O> behavior_chain_;
    std::optional<TaskIndex> dependency_;
    std::optional<I> explicit_input_;

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
     * @brief Set task dependency.
     *
     * The emitted task will depend on the specified task ID.
     *
     * @param task_id Task ID that this utility's task should depend on
     * @return Reference to this adapter for chaining
     */
    UtilityAdapter& depends_on(TaskIndex task_id) {
        dependency_ = task_id;
        return *this;
    }

    /**
     * @brief Set explicit input value.
     *
     * Used when emitting to TaskContext with a specific input value,
     * rather than depending on pipeline input.
     *
     * @param input Input value for the utility
     * @return Reference to this adapter for chaining
     */
    UtilityAdapter& with_input(const I& input) {
        explicit_input_ = input;
        return *this;
    }

    /**
     * @brief Emit utility as task on pipeline (terminal operation).
     *
     * Adds the utility as a task to the pipeline with behaviors applied.
     * The utility will receive its input from the pipeline execution.
     *
     * @param pipeline Pipeline to add the task to
     * @return TaskResult with future and task ID
     */
    TaskResult<O> emit_on(Pipeline& pipeline) {
        using UtilityType = Utility<I, O, Tags...>;
        using ConcreteType =
            typename std::remove_reference<decltype(*utility_)>::type;

        TaskIndex dep = dependency_.value_or(-1);

        // Create executor with utility and behavior chain
        auto executor =
            std::make_shared<behaviors::UtilityExecutor<I, O, Tags...>>(
                utility_, behavior_chain_);

        // Detect if utility needs context
        if constexpr (UtilityType::template has_tag<tags::NeedsContext>() ||
                      detail::has_process_with_context_v<ConcreteType, I, O>) {
            return pipeline.add_task<I, O>(
                [executor](I input, TaskContext& ctx) -> O {
                    return executor->execute_with_context(input, ctx);
                },
                dep);
        } else {
            return pipeline.add_task<I, O>(
                [executor](I input, [[maybe_unused]] TaskContext& ctx) -> O {
                    return executor->execute(input);
                },
                dep);
        }
    }

    /**
     * @brief Emit utility as dynamic task on context (terminal operation).
     *
     * Emits the utility as a dynamic task from within another task with
     * behaviors applied. Requires explicit input to be set via with_input().
     *
     * @param ctx TaskContext for dynamic emission
     * @return TaskResult with future and task ID
     * @throws std::runtime_error if with_input() was not called
     */
    TaskResult<O> emit_on(TaskContext& ctx) {
        if (!explicit_input_) {
            throw std::runtime_error(
                "with_input() must be called before emit_on(TaskContext&)");
        }

        using UtilityType = Utility<I, O, Tags...>;
        using ConcreteType =
            typename std::remove_reference<decltype(*utility_)>::type;

        Input<I> input_wrapper = Input<I>::ref(explicit_input_.value());

        // Create executor with utility and behavior chain
        auto executor =
            std::make_shared<behaviors::UtilityExecutor<I, O, Tags...>>(
                utility_, behavior_chain_);

        if constexpr (UtilityType::template has_tag<tags::NeedsContext>() ||
                      detail::has_process_with_context_v<ConcreteType, I, O>) {
            return ctx.emit<I, O>(
                [executor](I in, TaskContext& task_ctx) -> O {
                    return executor->execute_with_context(in, task_ctx);
                },
                input_wrapper);
        } else {
            return ctx.emit<I, O>(
                [executor](I in, TaskContext& task_ctx) -> O {
                    return executor->execute(in);
                },
                input_wrapper);
        }
    }

    /**
     * @brief Emit utility as batch tasks on pipeline (terminal operation).
     *
     * Creates independent tasks for each input, allowing parallel execution.
     *
     * @param pipeline Pipeline to add tasks to
     * @param inputs Vector of input values
     * @return Vector of TaskResults for all tasks
     */
    std::vector<TaskResult<O>> emit_on(Pipeline& pipeline,
                                       const std::vector<I>& inputs) {
        std::vector<TaskResult<O>> results;
        results.reserve(inputs.size());

        for (const auto& input : inputs) {
            // Create new adapter for each input (don't reuse state)
            UtilityAdapter adapter(utility_);
            if (dependency_) {
                adapter.depends_on(dependency_.value());
            }
            results.push_back(adapter.emit_on(pipeline));
        }

        return results;
    }

    /**
     * @brief Emit utility as batch dynamic tasks (terminal operation).
     *
     * Creates independent dynamic tasks for each input.
     *
     * @param ctx TaskContext for dynamic emission
     * @param inputs Vector of input values
     * @return Vector of TaskResults for all tasks
     */
    std::vector<TaskResult<O>> emit_on(TaskContext& ctx,
                                       const std::vector<I>& inputs) {
        std::vector<TaskResult<O>> results;
        results.reserve(inputs.size());

        for (const auto& input : inputs) {
            UtilityAdapter adapter(utility_);
            adapter.with_input(input);
            results.push_back(adapter.emit_on(ctx));
        }

        return results;
    }
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
 * fluent, natural-language-like syntax:
 *
 * @code
 * auto utility = std::make_shared<MyUtility>();
 * use(utility).depends_on(id).emit_on(pipeline);
 * use(utility).with_input(data).emit_on(ctx);
 * @endcode
 *
 * The name "use" was chosen to read naturally in chains, like
 * instructions: "use this utility, depends on that task, emit on pipeline"
 *
 * This function automatically handles derived utility classes by deducing
 * the base Utility type from the Input, Output, and TagsTuple members.
 *
 * @param utility Shared pointer to utility (can be derived class)
 * @return UtilityAdapter ready for configuration
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
