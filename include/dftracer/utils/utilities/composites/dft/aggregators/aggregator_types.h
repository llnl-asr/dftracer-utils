#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATOR_TYPES_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATOR_TYPES_H

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_key.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_metrics.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

/// Context for converting aggregation data to dfanalyzer-compatible Arrow
/// format.
struct DfanalyzerContext {
    /// Hash tables for resolving fhash/hhash to file_name/host_name.
    const std::unordered_map<std::string, std::string>* file_hashes = nullptr;
    const std::unordered_map<std::string, std::string>* host_hashes = nullptr;

    const common::query::Query* query_filter = nullptr;

    /// Time origin (minimum time_bucket) for normalization.
    std::uint64_t time_origin = 0;

    /// Time resolution (microseconds per output unit, default 1e6 = seconds).
    double time_resolution = 1e6;

    /// Time granularity in seconds (bucket width for time_range computation).
    double time_granularity = 1.0;
};

enum class AggregationBatchType { EVENT, PROFILE, SYSTEM };

struct CountConfidenceInterval {
    double lower = 0.0;
    double upper = 0.0;
};

struct AggregationEntry {
    AggregationKey key;
    AggregationMetrics metrics;
    bool is_approximated = false;
    CountConfidenceInterval count_ci;

    AggregationEntry() = default;
    AggregationEntry(AggregationKey k, AggregationMetrics m)
        : key(std::move(k)), metrics(std::move(m)) {}

    /// Create a ValueMap from the key and metrics for query evaluation.
    /// Includes cat, name, pid, tid, hhash, fhash, time_bucket, extra_keys,
    /// and aggregation metrics (count, dur_total, dur_min, dur_max, etc.).
    common::query::ValueMap to_value_map() const {
        common::query::ValueMap fields;
        // Key fields
        fields["cat"] = std::string(key.cat());
        fields["name"] = std::string(key.name());
        fields["pid"] = static_cast<uint64_t>(key.pid);
        fields["tid"] = static_cast<uint64_t>(key.tid);
        if (!key.hhash().empty()) {
            fields["hhash"] = std::string(key.hhash());
        }
        if (!key.fhash().empty()) {
            fields["fhash"] = std::string(key.fhash());
        }
        fields["time_bucket"] = key.time_bucket;
        // Include extra_keys (args fields used for grouping)
        if (key.extra_keys) {
            for (const auto& [key_id, value_id] : *key.extra_keys) {
                auto key_str =
                    std::string(aggregation_intern().resolve(key_id));
                auto value_str =
                    std::string(aggregation_intern().resolve(value_id));
                fields[key_str] = value_str;
            }
        }
        // Aggregation metrics
        fields["count"] = metrics.count;
        fields["dur_total"] = metrics.duration.total;
        fields["dur_min"] = metrics.duration.min;
        fields["dur_max"] = metrics.duration.max;
        fields["dur_mean"] = metrics.duration.mean;
        fields["size_total"] = metrics.size.total;
        fields["size_min"] = metrics.size.min;
        fields["size_max"] = metrics.size.max;
        fields["size_mean"] = metrics.size.mean;
        fields["ts"] = metrics.ts;
        fields["te"] = metrics.te;
        // Custom metrics (arbitrary args fields aggregated as numeric stats)
        if (metrics.custom_metrics) {
            for (const auto& [name, stats] : *metrics.custom_metrics) {
                fields[name + "_total"] = stats.total;
                fields[name + "_min"] = stats.min;
                fields[name + "_max"] = stats.max;
                fields[name + "_mean"] = stats.mean;
            }
        }
        return fields;
    }

    /// Check if this entry matches a query.
    bool matches(const common::query::Query& query) const {
        return query.evaluate(to_value_map());
    }
};

struct AggregationBatch {
    std::vector<AggregationEntry> entries;
    AggregationBatchType batch_type = AggregationBatchType::EVENT;
    std::size_t total_events_processed = 0;
    std::size_t total_files_processed = 0;
    std::size_t total_bytes_processed = 0;
    bool has_approximated_entries = false;

    // When set, to_arrow() uses these instead of discovering from entries.
    // All batches in an IPC file must use the same columns for a consistent
    // schema.
    const std::vector<std::uint32_t>* global_extra_key_ids = nullptr;
    const std::vector<std::string>* global_custom_metric_names = nullptr;

    /// Filter entries by query, returning a new batch with matching entries.
    AggregationBatch filter(const common::query::Query& query) const {
        AggregationBatch filtered;
        filtered.batch_type = batch_type;
        filtered.total_events_processed = total_events_processed;
        filtered.total_files_processed = total_files_processed;
        filtered.total_bytes_processed = total_bytes_processed;
        filtered.has_approximated_entries = has_approximated_entries;
        filtered.global_extra_key_ids = global_extra_key_ids;
        filtered.global_custom_metric_names = global_custom_metric_names;

        for (const auto& entry : entries) {
            if (entry.matches(query)) {
                filtered.entries.push_back(entry);
            }
        }
        return filtered;
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    common::arrow::ArrowExportResult to_arrow() const;

    /// Convert to dfanalyzer-compatible Arrow format.
    /// Outputs columns matching dfanalyzer schema:
    /// - Events/Profiles: cat, func_name, pid, tid, file_hash, host_hash,
    ///   file_name, host_name, proc_name, io_cat, acc_pat, count, time, size,
    ///   time_min, time_max, size_min, size_max, time_range, time_start,
    ///   time_end
    /// - System: host_hash, time_range, sys_cpu_*, sys_mem_*
    common::arrow::ArrowExportResult to_dfanalyzer_arrow(
        const DfanalyzerContext& ctx) const;
#endif
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATOR_TYPES_H
