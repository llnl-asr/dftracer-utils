// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_METRICS_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_METRICS_H

#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>

#include <bit>
#include <cstdint>

namespace dftracer::utils::index::schemas::dft::agg {

using dataframe::DDSketch;

/// One metric's aggregate: the shared @ref
/// dftracer::utils::dataframe::FieldStat "FieldStat" atom plus an optional
/// DDSketch. The sketch stays beside the POD atom (never inside it) so @ref
/// dftracer::utils::dataframe::FieldStat "FieldStat" merges stay
/// trivially-copyable/vectorizable. Read the atom via the accessors below;
/// write it through `stat` directly.
struct MetricStats {
    dftracer::utils::dataframe::FieldStat stat;
    dstd::UniquePtr<DDSketch> sketch;
    double sketch_accuracy = 0.01;

    explicit MetricStats(double relative_accuracy = 0.01)
        : sketch_accuracy(relative_accuracy) {}

    MetricStats(const MetricStats& other)
        : stat(other.stat),
          sketch(other.sketch ? dstd::make_unique<DDSketch>(*other.sketch)
                              : nullptr),
          sketch_accuracy(other.sketch_accuracy) {}

    MetricStats& operator=(const MetricStats& other) {
        if (this != &other) {
            stat = other.stat;
            sketch = other.sketch ? dstd::make_unique<DDSketch>(*other.sketch)
                                  : nullptr;
            sketch_accuracy = other.sketch_accuracy;
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
    /// The running mean as the pair shift + cmean, and the central moments
    /// (sums of (x - mean)^k), as persisted.
    double shift() const { return stat.shift; }
    double cmean() const { return stat.cmean; }
    double m2() const { return stat.cm2; }
    double m3() const { return stat.cm3; }
    double m4() const { return stat.cm4; }

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
    double stddev() const;
    double skewness() const;
    double kurtosis() const;
};

/// Return the entry for `name`, inserting a value constructed from `accuracy`
/// if absent. Works for any transparent-lookup map whose mapped_type is
/// constructible from a sketch accuracy. Returns a reference (no copy).
template <typename Map>
typename Map::mapped_type& find_or_create(Map& map, dstd::StringView name,
                                          double accuracy) {
    auto it = map.find(name);
    if (it == map.end()) {
        it =
            map.emplace(dstd::String(name), typename Map::mapped_type(accuracy))
                .first;
    }
    return it->second;
}

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_METRICS_H
