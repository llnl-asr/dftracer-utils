#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATOR_TYPES_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATOR_TYPES_H

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_key.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_metrics.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store {
/// DfanalyzerContext::hash_db holds a pointer only; the full definition is
/// pulled in behind DFTRACER_UTILS_ENABLE_ARROW, so forward declare here to
/// keep the field usable when Arrow is disabled.
class IndexDatabase;
}  // namespace dftracer::utils::index::store

namespace dftracer::utils::index::schemas::dft::agg {

/// Context for converting aggregation data to dfanalyzer-compatible Arrow
/// format.
struct DfanalyzerContext {
    /// Hash tables for resolving fhash/hhash to file_name/host_name.
    const StringViewMap<std::string>* file_hashes = nullptr;
    const StringViewMap<std::string>* host_hashes = nullptr;

    /// Resolves hashes on demand; preferred over the tables above, which cost
    /// the trace's entire file list to load.
    const index::store::IndexDatabase* hash_db = nullptr;

    const duql::Query* query_filter = nullptr;

    /// Time origin (minimum time_bucket) for normalization.
    std::uint64_t time_origin = 0;

    /// Time resolution (microseconds per output unit, default 1e6 = seconds).
    double time_resolution = 1e6;

    /// Time granularity in seconds (bucket width for time_range computation).
    double time_granularity = 1.0;

    /// Shard-scan progress, shared across the scan's tasks. Set together or
    /// left null.
    std::atomic<std::size_t>* shards_done = nullptr;
    std::size_t total_shards = 0;
    const std::function<void(std::size_t, std::size_t)>* progress = nullptr;
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

    /// ValueMap of this entry's fields (cat, name, pid, tid, hhash, fhash,
    /// time_bucket, extra_keys, and aggregation metrics), for query evaluation.
    duql::ValueMap to_value_map(const StringIntern& intern) const {
        duql::ValueMap fields;
        fields["cat"] = std::string(key.cat(intern));
        fields["name"] = std::string(key.name(intern));
        fields["pid"] = static_cast<uint64_t>(key.pid);
        fields["tid"] = static_cast<uint64_t>(key.tid);
        if (!key.hhash(intern).empty()) {
            fields["hhash"] = std::string(key.hhash(intern));
        }
        char fbuf[::dftracer::utils::hash::HEX64_DIGITS];
        if (auto fh = key.fhash_str(intern, fbuf); !fh.empty()) {
            fields["fhash"] = std::string(fh);
        }
        fields["time_bucket"] = key.time_bucket;
        if (key.extra_keys) {
            for (const auto& [key_id, value_id] : *key.extra_keys) {
                auto key_str = std::string(intern.resolve(key_id));
                auto value_str = std::string(intern.resolve(value_id));
                fields[key_str] = value_str;
            }
        }
        fields["count"] = metrics.count;
        fields["dur_total"] = metrics.duration.total();
        fields["dur_min"] = metrics.duration.min();
        fields["dur_max"] = metrics.duration.max();
        fields["dur_mean"] = metrics.duration.mean();
        fields["size_total"] = metrics.size.total();
        fields["size_min"] = metrics.size.min();
        fields["size_max"] = metrics.size.max();
        fields["size_mean"] = metrics.size.mean();
        fields["ts"] = metrics.ts;
        fields["te"] = metrics.te;
        if (metrics.custom_metrics) {
            for (const auto& [name, stats] : *metrics.custom_metrics) {
                fields[name + "_total"] = stats.total();
                fields[name + "_min"] = stats.min();
                fields[name + "_max"] = stats.max();
                fields[name + "_mean"] = stats.mean();
            }
        }
        return fields;
    }

    bool matches(const duql::Query& query, const StringIntern& intern) const {
        return query.evaluate(to_value_map(intern));
    }
};

struct AggregationBatch {
    std::vector<AggregationEntry> entries;
    AggregationBatchType batch_type = AggregationBatchType::EVENT;
    std::size_t total_events_processed = 0;
    std::size_t total_files_processed = 0;
    std::size_t total_bytes_processed = 0;
    bool has_approximated_entries = false;

    /// When set, to_arrow() uses these instead of discovering from entries.
    /// All batches in an IPC file must use the same columns for a consistent
    /// schema.
    const std::vector<std::uint32_t>* global_extra_key_ids = nullptr;
    const std::vector<std::string>* global_custom_metric_names = nullptr;

    /// The table the entries' string ids belong to.
    AggInternPtr intern;

    const StringIntern& strings() const {
        if (!intern) {
            throw DFTUtilsException(ErrorCode::INTERNAL,
                                    "aggregation batch has no intern table");
        }
        return intern->intern;
    }

    AggregationBatch filter(const duql::Query& query) const {
        AggregationBatch filtered;
        filtered.batch_type = batch_type;
        filtered.total_events_processed = total_events_processed;
        filtered.total_files_processed = total_files_processed;
        filtered.total_bytes_processed = total_bytes_processed;
        filtered.has_approximated_entries = has_approximated_entries;
        filtered.global_extra_key_ids = global_extra_key_ids;
        filtered.global_custom_metric_names = global_custom_metric_names;
        filtered.intern = intern;

        for (const auto& entry : entries) {
            if (entry.matches(query, intern->intern)) {
                filtered.entries.push_back(entry);
            }
        }
        return filtered;
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    utilities::common::arrow::ArrowExportResult to_arrow() const;
#endif
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATOR_TYPES_H
