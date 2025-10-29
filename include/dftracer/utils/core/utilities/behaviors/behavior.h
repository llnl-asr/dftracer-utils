#ifndef DFTRACER_UTILS_CORE_UTILITIES_BEHAVIORS_BEHAVIOR_H
#define DFTRACER_UTILS_CORE_UTILITIES_BEHAVIORS_BEHAVIOR_H

#include <dftracer/utils/core/utilities/behaviors/behavior_error_result.h>

#include <exception>
#include <functional>
#include <optional>
#include <variant>

namespace dftracer::utils::utilities::behaviors {

/**
 * @brief Base interface for utility behaviors.
 *
 * Behaviors are composable middleware that can be applied to utilities to add
 * cross-cutting concerns like caching, retry logic, monitoring, etc.
 *
 * Behaviors can intercept execution at multiple points:
 * 1. process() - Wraps the execution (middleware pattern) - can skip,
 * transform, or retry
 * 2. before_process() - Called before execution (hook)
 * 3. after_process() - Called after execution (hook + transform)
 * 4. on_error() - Called on exception (error handling)
 *
 * The process() method is the primary interception point using middleware
 * pattern. Override it to wrap execution with custom logic (e.g., caching,
 * retry).
 *
 * @tparam I Input type for the utility
 * @tparam O Output type for the utility
 */
template <typename I, typename O>
class UtilityBehavior {
   public:
    using NextFunction = std::function<O(const I&)>;

    virtual ~UtilityBehavior() = default;

    /**
     * @brief Middleware-style process wrapper.
     *
     * This is the primary interception point. Override to wrap execution:
     * - Caching: Check cache, call next() on miss, store result
     * - Retry: Try/catch next(), retry on failure
     * - Monitoring: Time execution around next()
     * - Transform: Modify input before next(), output after next()
     *
     * The 'next' function represents the rest of the execution chain.
     * Call next(input) to continue execution, or skip it to short-circuit.
     *
     * Default implementation calls before_process, then next, then
     * after_process.
     *
     * @param input Input to process
     * @param next Next function in the chain (could be another behavior or the
     * utility)
     * @return Processed output
     */
    virtual O process(const I& input, NextFunction next) {
        before_process(input);
        O result = next(input);
        return after_process(input, std::move(result));
    }

    /**
     * @brief Hook called before utility.process().
     *
     * Use this for:
     * - Pre-validation
     * - Logging start time
     * - Rate limiting
     * - Pre-processing
     *
     * @param input Input that will be passed to utility
     */
    virtual void before_process([[maybe_unused]] const I& input) {}

    /**
     * @brief Hook called after utility.process() succeeds.
     *
     * Use this for:
     * - Result transformation
     * - Caching results
     * - Logging completion
     * - Post-processing
     *
     * @param input Input that was passed to utility
     * @param result Result from utility.process()
     * @return Potentially transformed result
     */
    virtual O after_process([[maybe_unused]] const I& input, O result) {
        return result;
    }

    /**
     * @brief Hook called when utility.process() throws exception.
     *
     * Can return one of three results:
     * - BehaviorErrorResult: Explicit retry or rethrow action
     * - std::optional<O> with value: Recovery value to use instead
     * - std::nullopt: Pass through to next behavior in chain
     *
     * Use this for:
     * - Error logging (return nullopt to pass through)
     * - Retry logic (return BehaviorErrorResult::retry() or ::rethrow())
     * - Fallback values (return std::optional<O> with value)
     * - Error recovery (return recovery value)
     *
     * @param input Input that was passed to utility
     * @param e Exception that was thrown
     * @param attempt Current attempt number (0-indexed)
     * @return BehaviorErrorResult, recovery value, or nullopt
     */
    virtual std::variant<BehaviorErrorResult, std::optional<O>> on_error(
        [[maybe_unused]] const I& input,
        [[maybe_unused]] const std::exception& e,
        [[maybe_unused]] std::size_t attempt) {
        return BehaviorErrorResult::rethrow();  // Default: rethrow
    }
};

}  // namespace dftracer::utils::utilities::behaviors

#endif  // DFTRACER_UTILS_CORE_UTILITIES_BEHAVIORS_BEHAVIOR_H
