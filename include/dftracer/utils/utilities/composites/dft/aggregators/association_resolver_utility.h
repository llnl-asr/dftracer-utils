#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_ASSOCIATION_RESOLVER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_ASSOCIATION_RESOLVER_UTILITY_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_output.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_tracker.h>

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

struct AssociationResolverInput {
    EventAggregatorOutput aggregations;
    std::vector<std::shared_ptr<AssociationTracker>> trackers;
    AggregationConfig config;
};

struct AssociationResolverOutput {
    EventAggregatorOutput aggregations;
    std::unordered_set<std::uint64_t> root_pids;
    std::uint64_t trace_duration = 0;
    BoundaryTimeRangesMap boundary_ranges;
    bool success = true;
};

class AssociationResolverUtility
    : public utilities::Utility<AssociationResolverInput,
                                AssociationResolverOutput> {
   public:
    coro::CoroTask<AssociationResolverOutput> process(
        const AssociationResolverInput& input) override;

   private:
    void compute_trace_metadata(const AssociationTracker& tracker,
                                const EventAggregatorOutput& aggregations,
                                AssociationResolverOutput& output);
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_ASSOCIATION_RESOLVER_UTILITY_H
