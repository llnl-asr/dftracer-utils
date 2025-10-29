#ifndef DFTRACER_UTILS_CORE_UTILITIES_BEHAVIORS_BEHAVIOR_CHAIN_H
#define DFTRACER_UTILS_CORE_UTILITIES_BEHAVIORS_BEHAVIOR_CHAIN_H

#include <dftracer/utils/core/utilities/behaviors/behavior.h>

#include <memory>
#include <vector>

namespace dftracer::utils::utilities::behaviors {

/**
 * @brief Chains multiple behaviors together for sequential execution.
 *
 * BehaviorChain manages a collection of behaviors and invokes their hooks
 * in order. Behaviors are executed in the order they were added.
 *
 * Hook execution order:
 * - before_process(): In forward order (first added, first executed)
 * - after_process(): In reverse order (last added, first executed, like
 * middleware)
 * - on_error(): In forward order until one handles the error
 *
 * @tparam I Input type
 * @tparam O Output type
 */
template <typename I, typename O>
class BehaviorChain {
   private:
    std::vector<std::shared_ptr<UtilityBehavior<I, O>>> behaviors_;

   public:
    BehaviorChain() = default;

    /**
     * @brief Add a behavior to the chain.
     *
     * Behaviors are executed in the order they are added.
     *
     * @param behavior Shared pointer to behavior to add
     */
    void add_behavior(std::shared_ptr<UtilityBehavior<I, O>> behavior) {
        if (behavior) {
            behaviors_.push_back(std::move(behavior));
        }
    }

    /**
     * @brief Invoke before_process() on all behaviors.
     *
     * Calls behaviors in forward order (first added, first executed).
     *
     * @param input Input that will be passed to utility
     */
    void before_process(const I& input) {
        for (auto& behavior : behaviors_) {
            behavior->before_process(input);
        }
    }

    /**
     * @brief Invoke after_process() on all behaviors.
     *
     * Calls behaviors in reverse order (last added, first executed).
     * This allows behaviors to wrap results like middleware layers.
     *
     * @param input Input that was passed to utility
     * @param result Initial result from utility.process()
     * @return Final transformed result after all behaviors
     */
    O after_process(const I& input, O result) {
        // Apply behaviors in reverse order (like middleware)
        for (auto it = behaviors_.rbegin(); it != behaviors_.rend(); ++it) {
            result = (*it)->after_process(input, result);
        }
        return result;
    }

    /**
     * @brief Invoke on_error() on behaviors until one handles it.
     *
     * Calls behaviors in forward order. Processing stops when a behavior:
     * - Returns BehaviorErrorResult (retry or rethrow decision made)
     * - Returns recovery value (std::optional<O> with value)
     *
     * If all behaviors return std::nullopt, returns rethrow by default.
     *
     * @param input Input that was passed to utility
     * @param e Exception that was thrown
     * @param attempt Current attempt number (0-indexed)
     * @return BehaviorErrorResult, recovery value, or rethrow if unhandled
     */
    std::variant<BehaviorErrorResult, std::optional<O>> on_error(
        const I& input, const std::exception& e, std::size_t attempt) {
        for (auto& behavior : behaviors_) {
            auto result = behavior->on_error(input, e, attempt);

            // Check if behavior returned an error action (retry/rethrow)
            if (std::holds_alternative<BehaviorErrorResult>(result)) {
                return result;  // Explicit action, stop chain
            }

            // Check if behavior returned a recovery value
            auto& optional_result = std::get<std::optional<O>>(result);
            if (optional_result.has_value()) {
                return result;  // Recovery value, stop chain
            }

            // Behavior returned nullopt, continue to next behavior
        }

        // No behavior handled it, rethrow by default
        return BehaviorErrorResult::rethrow();
    }

    /**
     * @brief Execute the middleware chain.
     *
     * Builds a middleware chain where each behavior can wrap execution.
     * Behaviors are executed in forward order (first added, outermost wrapper).
     *
     * Example with [Monitor, Cache, Retry]:
     * - Monitor wraps Cache wraps Retry wraps utility
     * - Execution flow: Monitor → Cache (hit?) → Retry → utility
     *
     * @param input Input to process
     * @param core Core function to execute
     * @return Final result after all middleware
     */
    O process(const I& input, std::function<O(const I&)> core) {
        // Build middleware chain from right to left
        auto next = core;

        // Wrap with each behavior in reverse order
        // (so first behavior added becomes outermost wrapper)
        for (auto it = behaviors_.rbegin(); it != behaviors_.rend(); ++it) {
            auto& behavior = *it;
            // Capture current next function
            auto current_next = next;
            // Wrap it with this behavior
            next = [behavior, current_next](const I& inp) -> O {
                return behavior->process(inp, current_next);
            };
        }

        // Execute the fully wrapped chain
        return next(input);
    }

    /**
     * @brief Check if chain is empty.
     * @return true if no behaviors in chain
     */
    bool empty() const { return behaviors_.empty(); }

    /**
     * @brief Get number of behaviors in chain.
     * @return Number of behaviors
     */
    std::size_t size() const { return behaviors_.size(); }

    /**
     * @brief Clear all behaviors from chain.
     */
    void clear() { behaviors_.clear(); }
};

}  // namespace dftracer::utils::utilities::behaviors

#endif  // DFTRACER_UTILS_CORE_UTILITIES_BEHAVIORS_BEHAVIOR_CHAIN_H
