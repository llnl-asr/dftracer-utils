#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_augmentation.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace dftracer::utils::utilities::composites::dft::aggregators {

namespace {

using hash::Fnv1aHashBuilder;

// Key for grouping entries during shrinking (merge)
struct MergeKey {
    std::uint32_t cat_id;
    std::uint32_t name_id;
    std::uint64_t pid;
    std::uint64_t tid;
    std::uint32_t hhash_id;
    std::uint32_t fhash_id;
    std::uint64_t target_bucket;  // computed from source bucket

    bool operator==(const MergeKey& other) const {
        return cat_id == other.cat_id && name_id == other.name_id &&
               pid == other.pid && tid == other.tid &&
               hhash_id == other.hhash_id && fhash_id == other.fhash_id &&
               target_bucket == other.target_bucket;
    }
};

struct MergeKeyHash {
    std::size_t operator()(const MergeKey& k) const {
        Fnv1aHashBuilder h;
        h.update_value(k.cat_id);
        h.update_value(k.name_id);
        h.update_value(k.pid);
        h.update_value(k.tid);
        h.update_value(k.hhash_id);
        h.update_value(k.fhash_id);
        h.update_value(k.target_bucket);
        return static_cast<std::size_t>(h.finish());
    }
};

// Shrink: merge multiple source buckets into one target bucket
AggregationBatch shrink_batch(const AggregationBatch& input,
                              std::uint64_t source_interval_us,
                              std::uint64_t target_interval_us) {
    AggregationBatch result;
    result.batch_type = input.batch_type;
    result.total_events_processed = input.total_events_processed;
    result.total_files_processed = input.total_files_processed;
    result.total_bytes_processed = input.total_bytes_processed;
    result.has_approximated_entries = false;
    result.global_extra_key_ids = input.global_extra_key_ids;
    result.global_custom_metric_names = input.global_custom_metric_names;

    // Group entries by MergeKey
    std::unordered_map<MergeKey, AggregationEntry, MergeKeyHash> merged;

    for (const auto& entry : input.entries) {
        const auto& key = entry.key;
        const auto& metrics = entry.metrics;

        // Compute target bucket from source bucket
        std::uint64_t source_time = key.time_bucket * source_interval_us;
        std::uint64_t target_bucket = source_time / target_interval_us;

        MergeKey mk{key.cat_id,   key.name_id,  key.pid,      key.tid,
                    key.hhash_id, key.fhash_id, target_bucket};

        auto it = merged.find(mk);
        if (it == merged.end()) {
            AggregationEntry new_entry;
            new_entry.key = key;
            new_entry.key.time_bucket = target_bucket;
            new_entry.metrics = metrics;
            new_entry.is_approximated = false;
            merged.emplace(mk, std::move(new_entry));
        } else {
            // Merge metrics
            it->second.metrics.merge_from(metrics);
        }
    }

    result.entries.reserve(merged.size());
    for (auto& [_, entry] : merged) {
        result.entries.push_back(std::move(entry));
    }

    return result;
}

// Expand: split one source bucket into multiple target buckets
AggregationBatch expand_batch(const AggregationBatch& input,
                              std::uint64_t source_interval_us,
                              std::uint64_t target_interval_us) {
    AggregationBatch result;
    result.batch_type = input.batch_type;
    result.total_events_processed = input.total_events_processed;
    result.total_files_processed = input.total_files_processed;
    result.total_bytes_processed = input.total_bytes_processed;
    result.has_approximated_entries = true;
    result.global_extra_key_ids = input.global_extra_key_ids;
    result.global_custom_metric_names = input.global_custom_metric_names;

    for (const auto& entry : input.entries) {
        const auto& key = entry.key;
        const auto& metrics = entry.metrics;

        // Source bucket boundaries
        std::uint64_t bucket_start = key.time_bucket * source_interval_us;
        std::uint64_t bucket_end = bucket_start + source_interval_us;

        // Actual event span from ts/te
        std::uint64_t ts = metrics.ts;
        std::uint64_t te = metrics.te;

        // Clamp ts/te to bucket boundaries
        ts = std::max(ts, bucket_start);
        te = std::min(te, bucket_end);

        // Handle edge case: ts >= te (all events at same instant or invalid)
        if (ts >= te) {
            // Use original ts to determine target bucket
            std::uint64_t original_ts = metrics.ts;
            if (original_ts < bucket_start) original_ts = bucket_start;
            if (original_ts >= bucket_end) original_ts = bucket_end - 1;

            std::uint64_t target_bucket = original_ts / target_interval_us;

            AggregationEntry new_entry;
            new_entry.key = key;
            new_entry.key.time_bucket = target_bucket;
            new_entry.is_approximated = true;
            new_entry.metrics = metrics;
            new_entry.count_ci =
                compute_poisson_ci(static_cast<double>(metrics.count));

            result.entries.push_back(std::move(new_entry));
            continue;
        }

        std::uint64_t span = te - ts;

        // Compute first and last target buckets that overlap with [ts, te]
        std::uint64_t first_target = ts / target_interval_us;
        std::uint64_t last_target = (te - 1) / target_interval_us;

        // Distribute across overlapping sub-buckets
        double total_weight = 0.0;
        std::vector<std::pair<std::uint64_t, double>> bucket_weights;

        for (std::uint64_t tb = first_target; tb <= last_target; ++tb) {
            std::uint64_t tb_start = tb * target_interval_us;
            std::uint64_t tb_end = tb_start + target_interval_us;

            // Overlap with [ts, te]
            std::uint64_t overlap_start = std::max(ts, tb_start);
            std::uint64_t overlap_end = std::min(te, tb_end);

            if (overlap_start < overlap_end) {
                double weight =
                    static_cast<double>(overlap_end - overlap_start) / span;
                bucket_weights.emplace_back(tb, weight);
                total_weight += weight;
            }
        }

        // Normalize weights (should sum to ~1.0)
        if (total_weight > 0.0) {
            for (auto& [_, w] : bucket_weights) {
                w /= total_weight;
            }
        }

        // Create sub-bucket entries
        double count = static_cast<double>(metrics.count);
        std::uint64_t count_sum = 0;

        for (std::size_t i = 0; i < bucket_weights.size(); ++i) {
            auto& [tb, weight] = bucket_weights[i];

            AggregationEntry new_entry;
            new_entry.key = key;
            new_entry.key.time_bucket = tb;
            new_entry.is_approximated = true;

            // Distribute count by weight
            double sub_count = count * weight;

            // For the last bucket, adjust to ensure sum equals original
            std::uint64_t sub_count_int;
            if (i == bucket_weights.size() - 1) {
                sub_count_int = metrics.count - count_sum;
            } else {
                sub_count_int =
                    static_cast<std::uint64_t>(std::round(sub_count));
                count_sum += sub_count_int;
            }

            // Create metrics for sub-bucket
            new_entry.metrics = metrics;  // Copy all fields
            new_entry.metrics.count = sub_count_int;

            // Scale duration total by weight
            new_entry.metrics.duration.total = static_cast<std::uint64_t>(
                std::round(metrics.duration.total * weight));
            new_entry.metrics.duration.count = sub_count_int;

            // Scale size total by weight
            new_entry.metrics.size.total = static_cast<std::uint64_t>(
                std::round(metrics.size.total * weight));
            new_entry.metrics.size.count = sub_count_int;

            // Keep min/max conservative (can't know which sub-bucket had them)
            // mean stays the same
            // variance stays the same (conservative)

            // Compute confidence interval
            new_entry.count_ci = compute_poisson_ci(sub_count);

            // Scale custom metrics by weight
            if (new_entry.metrics.custom_metrics) {
                for (auto& [name, stat] : *new_entry.metrics.custom_metrics) {
                    stat.total = static_cast<std::uint64_t>(
                        std::round(stat.total * weight));
                    stat.count = sub_count_int;
                }
            }

            // Only add if count > 0
            if (sub_count_int > 0) {
                result.entries.push_back(std::move(new_entry));
            }
        }
    }

    return result;
}

}  // namespace

AggregationBatch augment_batch(const AggregationBatch& input,
                               const AugmentationConfig& config) {
    // Pass through if intervals match
    if (config.source_interval_us == config.target_interval_us) {
        AggregationBatch result;
        result.batch_type = input.batch_type;
        result.total_events_processed = input.total_events_processed;
        result.total_files_processed = input.total_files_processed;
        result.total_bytes_processed = input.total_bytes_processed;
        result.has_approximated_entries = false;
        result.global_extra_key_ids = input.global_extra_key_ids;
        result.global_custom_metric_names = input.global_custom_metric_names;

        result.entries.reserve(input.entries.size());
        for (const auto& entry : input.entries) {
            AggregationEntry new_entry;
            new_entry.key = entry.key;
            new_entry.metrics = entry.metrics;
            new_entry.is_approximated = false;
            result.entries.push_back(std::move(new_entry));
        }

        return result;
    }

    // Shrink: target > source (fewer, larger buckets)
    if (config.target_interval_us > config.source_interval_us) {
        return shrink_batch(input, config.source_interval_us,
                            config.target_interval_us);
    }

    // Expand: target < source (more, smaller buckets)
    return expand_batch(input, config.source_interval_us,
                        config.target_interval_us);
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
