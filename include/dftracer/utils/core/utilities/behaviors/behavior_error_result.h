#ifndef DFTRACER_UTILS_CORE_UTILITIES_BEHAVIORS_BEHAVIOR_ERROR_RESULT_H
#define DFTRACER_UTILS_CORE_UTILITIES_BEHAVIORS_BEHAVIOR_ERROR_RESULT_H

#include <exception>

namespace dftracer::utils::utilities::behaviors {

/**
 * @brief Action to take when a behavior handles an error.
 */
enum class BehaviorErrorAction {
    Retry,   // Retry the operation
    Rethrow  // Stop retrying and rethrow the exception
};

/**
 * @brief Result of behavior error handling indicating action to take.
 *
 * Used by behaviors to signal how the executor should handle an error:
 * - Retry: Continue retry loop
 * - Rethrow: Stop and rethrow the exception
 */
struct BehaviorErrorResult {
    BehaviorErrorAction action;
    std::exception_ptr exception;

    /**
     * @brief Create a retry result.
     *
     * Signals that the operation should be retried.
     *
     * @param ex The exception that occurred (defaults to current exception)
     * @return BehaviorErrorResult with Retry action
     */
    static BehaviorErrorResult retry(
        std::exception_ptr ex = std::current_exception()) {
        return BehaviorErrorResult{BehaviorErrorAction::Retry, ex};
    }

    /**
     * @brief Create a rethrow result.
     *
     * Signals that the exception should be rethrown to stop execution.
     *
     * @param ex The exception to rethrow (defaults to current exception)
     * @return BehaviorErrorResult with Rethrow action
     */
    static BehaviorErrorResult rethrow(
        std::exception_ptr ex = std::current_exception()) {
        return BehaviorErrorResult{BehaviorErrorAction::Rethrow, ex};
    }

    /**
     * @brief Check if this result indicates a retry should occur.
     * @return true if action is Retry
     */
    bool should_retry() const { return action == BehaviorErrorAction::Retry; }

    /**
     * @brief Check if this result indicates the exception should be rethrown.
     * @return true if action is Rethrow
     */
    bool should_rethrow() const {
        return action == BehaviorErrorAction::Rethrow;
    }
};

}  // namespace dftracer::utils::utilities::behaviors

#endif  // DFTRACER_UTILS_CORE_UTILITIES_BEHAVIORS_BEHAVIOR_ERROR_RESULT_H
