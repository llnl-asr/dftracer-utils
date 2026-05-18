#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregator_summary_utility.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace dftracer::utils::utilities::composites::dft::aggregators {

coro::CoroTask<void> AggregatorSummaryUtility::process(
    const AggregatorSummaryInput& input) {
    const auto& aggregations = input.aggregations;

    std::printf("=== Aggregation Summary ===\n");
    std::printf("Total unique aggregation keys: %zu\n", aggregations.size());

    std::uint64_t total_events = 0;
    for (const auto& [key, metrics] : aggregations) {
        total_events += metrics.count;
    }
    std::printf("Total events aggregated: %llu\n",
                static_cast<unsigned long long>(total_events));

    StringViewMap<std::uint64_t> category_counts;
    for (const auto& [key, metrics] : aggregations) {
        auto cat = key.cat();
        auto it = category_counts.find(cat);
        if (it == category_counts.end()) {
            category_counts.emplace(std::string(cat), metrics.count);
        } else {
            it->second += metrics.count;
        }
    }

    std::printf("\nEvents by category:\n");
    for (const auto& [cat, count] : category_counts) {
        std::printf("  %s: %llu events\n", cat.c_str(),
                    static_cast<unsigned long long>(count));
    }

    std::printf("\n");
    co_return;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
