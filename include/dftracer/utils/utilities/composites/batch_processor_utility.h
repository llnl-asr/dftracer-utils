#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_BATCH_PROCESSOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_BATCH_PROCESSOR_UTILITY_H

#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/core/utilities/utility_traits.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace dftracer::utils::utilities::composites {

/**
 * @brief Generic batch processor that processes a list of items in parallel.
 *
 * @tparam ItemInput Type of input items
 * @tparam ItemOutput Type of output from processing each item
 */
template <typename ItemInput, typename ItemOutput>
class BatchProcessorUtility
    : public utilities::Utility<std::vector<ItemInput>, std::vector<ItemOutput>,
                                utilities::tags::NeedsContext> {
   public:
    using ItemProcessorFn =
        std::function<ItemOutput(TaskContext&, const ItemInput&)>;
    using ComparatorFn =
        std::function<bool(const ItemOutput&, const ItemOutput&)>;

   private:
    ItemProcessorFn processor_;
    std::optional<ComparatorFn> comparator_;

   public:
    /**
     * @brief Construct batch processor with an item processing function.
     *
     * @param processor Function that processes a single item
     */
    explicit BatchProcessorUtility(ItemProcessorFn processor)
        : processor_(std::move(processor)), comparator_(std::nullopt) {}

    ~BatchProcessorUtility() override = default;

    /**
     * @brief Construct batch processor with a parallelizable utility.
     *
     * This constructor enforces at compile-time that the utility has the
     * tags::Parallelizable tag, ensuring thread-safety.
     *
     * @tparam UtilityType Type of the utility (must have tags::Parallelizable)
     * @param utility Shared pointer to the utility
     */
    template <typename UtilityType,
              typename = std::enable_if_t<utilities::detail::has_process_v<
                  UtilityType, ItemInput, ItemOutput>>>
    explicit BatchProcessorUtility(std::shared_ptr<UtilityType> utility)
        : comparator_(std::nullopt) {
        // Compile-time check: Utility must have tags::Parallelizable
        static_assert(
            utilities::has_tag_v<utilities::tags::Parallelizable, UtilityType>,
            "Utility must have tags::Parallelizable for parallel "
            "BatchProcessor! "
            "Add tags::Parallelizable to your Utility class template "
            "parameters.");

        // Create processor function from utility
        processor_ = [utility](TaskContext&,
                               const ItemInput& input) -> ItemOutput {
            return utility->process(input);
        };
    }

    /**
     * @brief Set a comparator for sorting results.
     *
     * @param comparator Function to compare two outputs for sorting
     * @return Reference to this processor for chaining
     */
    BatchProcessorUtility<ItemInput, ItemOutput>& with_comparator(
        ComparatorFn comparator) {
        comparator_ = std::move(comparator);
        return *this;
    }

    /**
     * @brief Process all items in parallel, optionally sorting results.
     *
     * @param items List of items to process
     * @return Vector of results (sorted if comparator was set)
     */
    std::vector<ItemOutput> process(
        const std::vector<ItemInput>& items) override {
        if (items.empty()) {
            return {};
        }

        // Get TaskContext for parallel execution
        TaskContext& ctx = this->context();

        // Submit parallel tasks for each item
        std::vector<std::shared_future<std::any>> futures;
        futures.reserve(items.size());

        for (const auto& item : items) {
            // Create task from processor - captures ctx from outer scope
            auto task = make_task(
                [proc = processor_, &ctx](ItemInput in) -> ItemOutput {
                    return proc(ctx, in);
                });

            // Submit task with input
            auto future = ctx.submit_task(task, std::any{item});
            futures.push_back(future);
        }

        // Wait for all tasks to complete
        std::vector<ItemOutput> results;
        results.reserve(futures.size());

        for (auto& future : futures) {
            std::any result_any = future.get();
            results.push_back(std::any_cast<ItemOutput>(result_any));
        }

        // Sort if comparator provided
        if (comparator_.has_value()) {
            std::sort(results.begin(), results.end(), comparator_.value());
        }

        return results;
    }
};

}  // namespace dftracer::utils::utilities::composites

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_BATCH_PROCESSOR_UTILITY_H
