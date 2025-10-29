#ifndef DFTRACER_UTILS_UTILITIES_BEHAVIORS_UTILITY_EXECUTOR_H
#define DFTRACER_UTILS_UTILITIES_BEHAVIORS_UTILITY_EXECUTOR_H

#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/core/utilities/utility_traits.h>

#include <exception>
#include <memory>
#include <optional>

namespace dftracer {
namespace utils {
namespace utilities {
namespace behaviors {

/**
 * @brief Executes a utility with behavior chain wrapping.
 *
 * UtilityExecutor orchestrates the execution of a utility by:
 * 1. Running before_process hooks on all behaviors
 * 2. Executing the utility's process() method
 * 3. Running after_process hooks on all behaviors
 * 4. Handling errors through behavior on_error hooks
 *
 * This class bridges utilities and behaviors, providing a unified
 * execution path regardless of which process() overload the utility
 * implements.
 *
 * @tparam I Input type
 * @tparam O Output type
 * @tparam Tags Variadic tag types
 */
template <typename I, typename O, typename... Tags>
class UtilityExecutor {
   private:
    std::shared_ptr<Utility<I, O, Tags...>> utility_;
    BehaviorChain<I, O> behavior_chain_;

   public:
    /**
     * @brief Construct executor with utility and behavior chain.
     *
     * @param utility The utility to execute
     * @param chain Behavior chain to wrap execution
     */
    UtilityExecutor(std::shared_ptr<Utility<I, O, Tags...>> utility,
                    BehaviorChain<I, O> chain)
        : utility_(utility), behavior_chain_(std::move(chain)) {}

    /**
     * @brief Execute utility without context using middleware pattern.
     *
     * Builds a middleware chain where each behavior wraps execution.
     * Behaviors can intercept, skip, transform, retry, or cache execution.
     *
     * @param input Input to process
     * @return Output result
     * @throws Any exception from utility or behaviors
     */
    O execute(const I& input) {
        return behavior_chain_.process(
            input, [this](const I& inp) { return utility_->process(inp); });
    }

    /**
     * @brief Execute utility with context using middleware pattern.
     *
     * Sets context reference before calling process(), then clears it after.
     *
     * @param input Input to process
     * @param ctx Task context for dynamic task emission
     * @return Output result
     * @throws Any exception from utility or behaviors
     */
    O execute_with_context(const I& input, TaskContext& ctx) {
        return behavior_chain_.process(input, [this, &ctx](const I& inp) {
            utility_->set_context(ctx);

            try {
                O result = utility_->process(inp);
                utility_->clear_context();
                return result;
            } catch (...) {
                utility_->clear_context();
                throw;
            }
        });
    }

    /**
     * @brief Get reference to underlying utility.
     */
    std::shared_ptr<Utility<I, O, Tags...>> get_utility() const {
        return utility_;
    }

    /**
     * @brief Get reference to behavior chain.
     */
    BehaviorChain<I, O>& get_behavior_chain() { return behavior_chain_; }

    /**
     * @brief Get const reference to behavior chain.
     */
    const BehaviorChain<I, O>& get_behavior_chain() const {
        return behavior_chain_;
    }
};

}  // namespace behaviors
}  // namespace utilities
}  // namespace utils
}  // namespace dftracer

#endif  // DFTRACER_UTILS_UTILITIES_BEHAVIORS_UTILITY_EXECUTOR_H
