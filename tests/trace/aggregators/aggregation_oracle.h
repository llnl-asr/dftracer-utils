#ifndef DFTRACER_UTILS_TESTS_AGGREGATION_ORACLE_H
#define DFTRACER_UTILS_TESTS_AGGREGATION_ORACLE_H

// The per-event aggregator that the chunk partials replaced, kept as a test
// oracle: one AggregationMetrics accumulates the events of a group one by one
// by the metric rules the partials share (accumulate_event_metrics). The
// partials must merge to what it gives.

#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_core.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_metrics.h>
#include <dftracer/utils/utilities/common/statistics/distinct_sketch.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace dftracer::utils::index::schemas::dft::agg {

using CustomMetricsMap =
    ankerl::unordered_dense::map<std::string, MetricStats,
                                 TransparentStringHash, TransparentStringEqual>;

struct AggregationMetrics {
    std::uint64_t count = 0;
    MetricStats duration;
    MetricStats size;
    MetricStats offset;
    std::uint64_t ts = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t te = 0;
    std::unique_ptr<CustomMetricsMap> custom_metrics;
    utilities::common::statistics::DistinctSketch distinct_files;
    double sketch_accuracy = 0.01;

    explicit AggregationMetrics(double relative_accuracy = 0.01)
        : duration(relative_accuracy),
          size(relative_accuracy),
          offset(relative_accuracy),
          sketch_accuracy(relative_accuracy) {}

    void update_timestamp(std::uint64_t event_ts, std::uint64_t dur) {
        ts = std::min(ts, event_ts);
        te = std::max(te, event_ts + dur);
    }

    void merge_from(const AggregationMetrics& other) {
        count += other.count;
        duration.merge_from(other.duration);
        size.merge_from(other.size);
        offset.merge_from(other.offset);
        ts = std::min(ts, other.ts);
        te = std::max(te, other.te);
        distinct_files.merge_from(other.distinct_files);
        if (other.custom_metrics) {
            if (!custom_metrics)
                custom_metrics = std::make_unique<CustomMetricsMap>();
            for (const auto& [name, metric] : *other.custom_metrics)
                find_or_create(*custom_metrics, name, sketch_accuracy)
                    .merge_from(metric);
        }
    }
};

/// The sink that accumulates one event into an AggregationMetrics.
class OracleSink {
   public:
    OracleSink(AggregationMetrics& entry, const AggregationConfig& cfg)
        : entry_(entry),
          accuracy_(cfg.sketch_accuracy),
          percentiles_(cfg.compute_percentiles) {}

    void add_count(std::uint64_t n) { entry_.count += n; }
    void span(std::uint64_t ts, std::uint64_t len) {
        entry_.update_timestamp(ts, len);
    }
    void duration(std::uint64_t dur) {
        ++entry_.count;
        entry_.duration.update(dur, percentiles_);
    }
    void size(std::uint64_t v) { entry_.size.update(v, percentiles_); }
    void offset(std::uint64_t v) { entry_.offset.update(v, percentiles_); }
    void merge_duration(const dataframe::FieldStat& s) {
        entry_.duration.stat.merge(s);
    }
    void merge_size(const dataframe::FieldStat& s) {
        entry_.size.stat.merge(s);
    }
    void merge_offset(const dataframe::FieldStat& s) {
        entry_.offset.stat.merge(s);
    }
    void custom(std::string_view key, std::uint64_t u) {
        if (!entry_.custom_metrics)
            entry_.custom_metrics = std::make_unique<CustomMetricsMap>();
        find_or_create(*entry_.custom_metrics, key, accuracy_)
            .update(u, percentiles_);
    }
    dataframe::FieldStat& custom_stat(std::string_view key) {
        if (!entry_.custom_metrics)
            entry_.custom_metrics = std::make_unique<CustomMetricsMap>();
        return find_or_create(*entry_.custom_metrics, key, accuracy_).stat;
    }
    void note_custom(std::string_view) {}

   private:
    AggregationMetrics& entry_;
    double accuracy_;
    bool percentiles_;
};

/// Accumulates `acc` into `entry` the way the per-event aggregator did; the
/// caller picks the entry. Events the partials do not hold are skipped.
template <class Acc>
void oracle_event(const Acc& acc, AggregationMetrics& entry,
                  const AggregationConfig& cfg) {
    if (acc.is_metadata()) return;
    if (acc.is_system()) {
        const bool preaggregated = acc.arg_is_number("count") ||
                                   acc.arg_is_number("dftu_cnt") ||
                                   acc.arg_is_number("dft_cnt");
        if (!preaggregated) return;
    }
    const auto fhash = acc.arg_string("fhash");
    if (!cfg.group_by_file && !fhash.empty())
        entry.distinct_files.add_hash(agg_hash_of_hex(fhash));
    OracleSink sink(entry, cfg);
    accumulate_event_metrics(acc, cfg, sink);
}

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_TESTS_AGGREGATION_ORACLE_H
