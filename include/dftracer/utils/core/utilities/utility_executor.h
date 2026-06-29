#ifndef DFTRACER_UTILS_UTILITIES_BEHAVIORS_UTILITY_EXECUTOR_H
#define DFTRACER_UTILS_UTILITIES_BEHAVIORS_UTILITY_EXECUTOR_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/utility.h>

#include <memory>

namespace dftracer {
namespace utils {
namespace utilities {
namespace behaviors {

/**
 * @brief Runs a utility's process(), injecting the CoroScope context when the
 *        utility needs it.
 *
 * @tparam I Input type
 * @tparam O Output type
 * @tparam Tags Variadic tag types
 */
template <typename I, typename O, typename... Tags>
class UtilityExecutor {
   private:
    std::shared_ptr<Utility<I, O, Tags...>> utility_;

   public:
    /**
     * @brief Construct an executor for the given utility.
     *
     * @param utility The utility to execute
     */
    explicit UtilityExecutor(std::shared_ptr<Utility<I, O, Tags...>> utility)
        : utility_(std::move(utility)) {}

    /**
     * @brief Execute the utility's process() without a CoroScope context.
     *
     * @param input Input to process
     * @return Output result
     * @throws Any exception propagated from the utility's process()
     */
    coro::CoroTask<O> execute(const I& input) {
        co_return co_await utility_->process(input);
    }

    /**
     * @brief Execute the utility with a CoroScope context.
     *
     * Sets the context reference before calling process() and clears it
     * afterwards (including on error), so context-needing utilities can emit
     * dynamic tasks for the duration of execution.
     *
     * @param ctx Task context for dynamic task emission
     * @param input Input to process
     * @return Output result
     * @throws Any exception propagated from the utility's process()
     */
    coro::CoroTask<O> execute(CoroScope& ctx, const I& input) {
        utility_->set_context(ctx);
        try {
            O result = co_await utility_->process(input);
            utility_->clear_context();
            co_return result;
        } catch (...) {
            utility_->clear_context();
            throw;
        }
    }

    std::shared_ptr<Utility<I, O, Tags...>> get_utility() const {
        return utility_;
    }
};

}  // namespace behaviors
}  // namespace utilities
}  // namespace utils
}  // namespace dftracer

#endif  // DFTRACER_UTILS_UTILITIES_BEHAVIORS_UTILITY_EXECUTOR_H
