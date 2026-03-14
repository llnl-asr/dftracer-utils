#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_BATCH_PROCESSOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_BATCH_PROCESSOR_UTILITY_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
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
        std::function<ItemOutput(CoroScope&, const ItemInput&)>;
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
        processor_ = [utility](CoroScope&,
                               const ItemInput& input) -> ItemOutput {
            // std::function cannot store coroutine lambdas
            return utility->process(input).get();
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
    coro::CoroTask<std::vector<ItemOutput>> process(
        const std::vector<ItemInput>& items) override {
        if (items.empty()) {
            co_return {};
        }

        CoroScope& ctx = this->context();

        std::vector<coro::SpawnFuture<ItemOutput>> futures;
        futures.reserve(items.size());

        for (const auto& item : items) {
            auto* proc = &processor_;
            futures.push_back(ctx.spawn(
                [proc, item](CoroScope& s) -> coro::CoroTask<ItemOutput> {
                    co_return (*proc)(s, item);
                }));
        }

        auto results = co_await coro::when_all(std::move(futures));

        if (comparator_.has_value()) {
            std::sort(results.begin(), results.end(), comparator_.value());
        }

        co_return results;
    }
};

}  // namespace dftracer::utils::utilities::composites

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_BATCH_PROCESSOR_UTILITY_H
