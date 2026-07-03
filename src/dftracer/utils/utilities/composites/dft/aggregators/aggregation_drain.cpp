#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_drain.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_visitor.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator.h>

namespace dftracer::utils::utilities::composites::dft::aggregators {

std::vector<std::string> merge_aggregation_visitors(
    std::vector<std::vector<std::unique_ptr<DftEventVisitor>>>& extra_visitors,
    EventAggregator* merger) {
    std::vector<std::string> processed_files;
    if (!merger) return processed_files;

    for (auto& file_visitors : extra_visitors) {
        for (auto& visitor : file_visitors) {
            auto* agg_visitor =
                dynamic_cast<AggregationVisitor*>(visitor.get());
            if (agg_visitor) {
                for (const auto& k : agg_visitor->observed_extra_keys())
                    merger->add_observed_extra_key(k);
                for (const auto& m : agg_visitor->observed_custom_metrics())
                    merger->add_observed_custom_metric(m);
                auto output = agg_visitor->take_output();
                processed_files.push_back(output.file_path);
                merger->merge_chunk(std::move(output));
            }
        }
        file_visitors.clear();
    }
    return processed_files;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
