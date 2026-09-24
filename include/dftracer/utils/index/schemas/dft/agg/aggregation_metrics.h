#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_METRICS_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_METRICS_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/utilities/common/statistics/ddsketch.h>
#include <dftracer/utils/utilities/common/statistics/distinct_sketch.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace dftracer::utils::index::schemas::dft::agg {

using utilities::common::statistics::DDSketch;

/// One metric's aggregate: the shared FieldStat atom plus an optional DDSketch.
/// The sketch stays beside the POD atom (never inside it) so FieldStat merges
/// stay trivially-copyable/vectorizable. Read the atom via the accessors below;
/// write it through `stat` directly.
struct MetricStats {
    dftracer::utils::dataframe::FieldStat stat;
    std::unique_ptr<DDSketch> sketch;
    double sketch_accuracy_ = 0.01;

    explicit MetricStats(double relative_accuracy = 0.01)
        : sketch_accuracy_(relative_accuracy) {}

    MetricStats(const MetricStats& other)
        : stat(other.stat),
          sketch(other.sketch ? std::make_unique<DDSketch>(*other.sketch)
                              : nullptr),
          sketch_accuracy_(other.sketch_accuracy_) {}

    MetricStats& operator=(const MetricStats& other) {
        if (this != &other) {
            stat = other.stat;
            sketch = other.sketch ? std::make_unique<DDSketch>(*other.sketch)
                                  : nullptr;
            sketch_accuracy_ = other.sketch_accuracy_;
        }
        return *this;
    }

    MetricStats(MetricStats&&) = default;
    MetricStats& operator=(MetricStats&&) = default;

    std::uint64_t count() const { return stat.n; }
    double total() const { return stat.sum; }
    double min() const { return stat.min; }
    double max() const { return stat.max; }
    double mean() const {
        return stat.n ? stat.sum / static_cast<double>(stat.n) : 0.0;
    }
    double m2() const { return stat.sumsq; }
    double m3() const { return stat.m3; }
    double m4() const { return stat.m4; }

    /// Exact integer total/min/max, valid when the metric accumulated integers
    /// (the tier's ts/dur/size/offset/custom metrics are all uint64). Lets the
    /// wire persist the exact value rather than one rounded through a double.
    bool exact_u64() const {
        return stat.domain != dftracer::utils::dataframe::FieldStatDomain::F64;
    }
    std::uint64_t total_u64() const {
        return std::bit_cast<std::uint64_t>(stat.esum);
    }
    std::uint64_t min_u64() const {
        return std::bit_cast<std::uint64_t>(stat.emin);
    }
    std::uint64_t max_u64() const {
        return std::bit_cast<std::uint64_t>(stat.emax);
    }

    void update(double value, bool compute_percentiles = false);
    void update(std::uint64_t value, bool compute_percentiles = false);
    void merge_from(const MetricStats& other);
    double get_stddev() const;
    double get_skewness() const;
    double get_kurtosis() const;
};

using CustomMetricsMap =
    ankerl::unordered_dense::map<std::string, MetricStats,
                                 TransparentStringHash, TransparentStringEqual>;

/// Return the entry for `name`, inserting a value constructed from `accuracy`
/// if absent. Works for any transparent-lookup map whose mapped_type is
/// constructible from a sketch accuracy. Returns a reference (no copy).
template <typename Map>
typename Map::mapped_type& find_or_create(Map& map, std::string_view name,
                                          double accuracy) {
    auto it = map.find(name);
    if (it == map.end()) {
        it = map.emplace(std::string(name), typename Map::mapped_type(accuracy))
                 .first;
    }
    return it->second;
}

struct AggregationMetrics {
    std::uint64_t count = 0;

    MetricStats duration;
    MetricStats size;
    MetricStats offset;

    std::uint64_t ts = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t te = 0;

    std::unique_ptr<StringViewMap<std::string>> boundary_associations;
    std::uint64_t parent_pid = 0;

    std::unique_ptr<CustomMetricsMap> custom_metrics;

    /// Only populated when the file hash is out of the key.
    utilities::common::statistics::DistinctSketch distinct_files;

    double sketch_accuracy = 0.01;

    explicit AggregationMetrics(double relative_accuracy = 0.01)
        : duration(relative_accuracy),
          size(relative_accuracy),
          offset(relative_accuracy),
          sketch_accuracy(relative_accuracy) {}

    AggregationMetrics(const AggregationMetrics& other)
        : count(other.count),
          duration(other.duration),
          size(other.size),
          offset(other.offset),
          ts(other.ts),
          te(other.te),
          boundary_associations(
              other.boundary_associations
                  ? std::make_unique<StringViewMap<std::string>>(
                        *other.boundary_associations)
                  : nullptr),
          parent_pid(other.parent_pid),
          custom_metrics(
              other.custom_metrics
                  ? std::make_unique<CustomMetricsMap>(*other.custom_metrics)
                  : nullptr),
          distinct_files(other.distinct_files),
          sketch_accuracy(other.sketch_accuracy) {}

    AggregationMetrics& operator=(const AggregationMetrics& other) {
        if (this != &other) {
            count = other.count;
            duration = other.duration;
            size = other.size;
            offset = other.offset;
            ts = other.ts;
            te = other.te;
            boundary_associations =
                other.boundary_associations
                    ? std::make_unique<StringViewMap<std::string>>(
                          *other.boundary_associations)
                    : nullptr;
            parent_pid = other.parent_pid;
            custom_metrics =
                other.custom_metrics
                    ? std::make_unique<CustomMetricsMap>(*other.custom_metrics)
                    : nullptr;
            sketch_accuracy = other.sketch_accuracy;
        }
        return *this;
    }

    AggregationMetrics(AggregationMetrics&&) = default;
    AggregationMetrics& operator=(AggregationMetrics&&) = default;

    void update_duration(std::uint64_t dur, bool compute_percentiles = false);
    void update_size(std::uint64_t sz, bool compute_percentiles = false);
    void update_offset(std::uint64_t off, bool compute_percentiles = false);
    void update_timestamp(std::uint64_t event_ts, std::uint64_t dur);
    void update_timestamp_clamped(std::uint64_t event_ts, std::uint64_t dur,
                                  std::uint64_t bucket_start,
                                  std::uint64_t bucket_size);
    void update_custom_metric(std::string_view name, std::uint64_t value,
                              bool compute_percentiles = false);

    void merge_from(const AggregationMetrics& other);
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_METRICS_H
