#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_PERFETTO_TRACE_WRITER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_PERFETTO_TRACE_WRITER_UTILITY_H

#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_key.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_metrics.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_resolver_utility.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

enum class PerfettoEventFormat { COUNTER, ASYNC, REGULAR };

struct PerfettoTraceWriterInput {
    std::string output_path;
    AssociationResolverOutput resolver_output;
    bool compute_statistics = true;
    bool compute_percentiles = false;
    std::vector<double> percentiles;
    bool compress = false;
    int compression_level = 6;
    PerfettoEventFormat format = PerfettoEventFormat::COUNTER;
};

using PerfettoTraceWriterOutput = bool;

class PerfettoTraceWriterUtility
    : public utilities::Utility<PerfettoTraceWriterInput,
                                PerfettoTraceWriterOutput> {
   private:
    std::uint64_t generate_synthetic_tid(const AggregationKey& key) const;
    void append_json_string(std::string& buffer, const std::string& str) const;
    void append_double(std::string& buffer, double value) const;
    void append_metric_stats(std::string& buffer, const MetricStats& stats,
                             std::uint64_t count, bool compute_statistics,
                             bool compute_percentiles,
                             const std::vector<double>& percentiles) const;
    void append_event_args(std::string& buffer, const AggregationKey& key,
                           const AggregationMetrics& metrics,
                           bool compute_statistics, bool compute_percentiles,
                           const std::vector<double>& percentiles,
                           std::uint64_t real_tid = 0) const;

   public:
    coro::CoroTask<bool> process(
        const PerfettoTraceWriterInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_PERFETTO_TRACE_WRITER_UTILITY_H
