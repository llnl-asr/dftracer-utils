#ifndef DFTRACER_UTILS_TRACE_AGGREGATORS_AGGREGATION_CORE_H
#define DFTRACER_UTILS_TRACE_AGGREGATORS_AGGREGATION_CORE_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/trace/aggregators/aggregation_config.h>
#include <dftracer/utils/trace/aggregators/aggregation_logic.h>
#include <dftracer/utils/trace/aggregators/aggregation_metrics.h>
#include <dftracer/utils/trace/aggregators/aggregation_output.h>
#include <dftracer/utils/trace/aggregators/aggregation_serialization.h>
#include <dftracer/utils/trace/aggregators/reserved_args.h>
#include <dftracer/utils/trace/aggregators/system_metrics.h>
#include <dftracer/utils/trace/aggregators/system_metrics_serialization.h>
#include <dftracer/utils/trace/args_map.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The aggregation-tier accumulation, templated on an Accessor so callers only
// differ in how they read fields/args. AggregationFold drives it with a
// POD-backed accessor.
// The Accessor concept:
//   is_metadata()/is_system()/is_profile()/is_counter() -> bool
//   name()/cat() -> string_view ; pid()/tid()/ts()/dur() -> uint64
//   arg_exists(key) -> bool
//   arg_is_number(key) -> bool
//   arg_uint(key) -> uint64      (get<uint64> semantics: clamp, 0 on miss)
//   arg_double(key) -> double    (get<double> semantics: 0 on miss)
//   arg_string(key) -> string_view (empty on miss / non-string)
//   for_each_numeric_arg(fn) with fn(string_view key, uint64 u, double d) over
//     every numeric arg (non-numbers skipped by the accessor).
namespace dftracer::utils::trace::aggregators {

/// dftracer hashes are hex; reuse their bits rather than hashing again.
inline std::uint64_t agg_hash_of_hex(std::string_view sv) {
    std::uint64_t v = 0;
    for (char c : sv) {
        std::uint64_t d;
        if (c >= '0' && c <= '9')
            d = static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = static_cast<std::uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = static_cast<std::uint64_t>(c - 'A' + 10);
        else
            return utilities::hash::fnv1a_hash(sv);
        v = (v << 4) | d;
    }
    return v ? v : 1;
}

/// The mutable accumulation state, shared verbatim by the visitor and the fold.
struct AggState {
    std::unordered_map<std::string, AggregationMetrics, TransparentStringHash,
                       TransparentStringEqual>
        local_buffer;
    std::unordered_map<std::string, SystemAggregationMetrics,
                       TransparentStringHash, TransparentStringEqual>
        system_buffer;
    AggregationMetrics* last_entry = nullptr;
    std::string_view last_key;
    std::string key_buf;
    std::string system_key_buf;
    std::unordered_set<std::string> observed_extra_keys;
    std::unordered_set<std::string> observed_custom_metrics;
    std::unordered_set<std::string> observed_system_metrics;
    std::uint64_t min_time_bucket = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_time_bucket = 0;
    std::size_t events_processed = 0;
};

/// Non-preaggregated system events: keyed by hhash + name + time_bucket, every
/// numeric arg (except hhash/fhash) folded into a SystemAggregationMetrics.
template <class Acc>
void aggregate_system_event(const Acc& acc, AggState& st,
                            const AggregationConfig& cfg) {
    auto hhash = acc.arg_string("hhash");
    auto time_bucket = compute_time_bucket(acc.ts(), acc.dur(), cfg);
    if (time_bucket < st.min_time_bucket) st.min_time_bucket = time_bucket;
    if (time_bucket > st.max_time_bucket) st.max_time_bucket = time_bucket;

    serialize_system_key_into(st.system_key_buf, hhash, acc.name(),
                              time_bucket);
    auto [it, inserted] =
        st.system_buffer.try_emplace(st.system_key_buf, cfg.sketch_accuracy);
    auto& entry = it->second;
    entry.count++;
    entry.update_timestamp(acc.ts());
    const bool compute_percentiles = cfg.compute_percentiles;

    acc.for_each_numeric_arg([&](std::string_view k, std::uint64_t, double d) {
        if (k == "hhash" || k == "fhash") return;
        entry.update_metric(k, d, compute_percentiles);
        st.observed_system_metrics.insert(std::string(k));
    });
    st.events_processed++;
}

/// Accumulate one event into `st`. The AssociationTracker feed and the
/// flush-threshold check are the caller's job (out-of-band / backend-specific).
template <class Acc>
void aggregate_event(const Acc& acc, AggState& st, const AggregationConfig& cfg,
                     std::uint32_t config_hash, StringIntern& intern) {
    if (acc.is_metadata()) return;

    bool is_preaggregated_system = false;
    if (acc.is_system()) {
        const bool is_preaggregated =
            acc.arg_is_number("count") || acc.arg_is_number("dftu_cnt");
        if (!is_preaggregated) {
            aggregate_system_event(acc, st, cfg);
            return;
        }
        is_preaggregated_system = true;
    }

    AggMapType map_type = AggMapType::EVENT;
    if (is_preaggregated_system) {
        map_type = AggMapType::SYSTEM;
    } else if (acc.is_profile()) {
        map_type = AggMapType::PROFILE;
    }

    auto hhash = acc.arg_string("hhash");
    auto fhash = acc.arg_string("fhash");
    auto bucket_ts = (map_type == AggMapType::PROFILE && acc.ts() > 0)
                         ? acc.ts() - 1
                         : acc.ts();
    auto time_bucket = compute_time_bucket(bucket_ts, acc.dur(), cfg);
    if (time_bucket < st.min_time_bucket) st.min_time_bucket = time_bucket;
    if (time_bucket > st.max_time_bucket) st.max_time_bucket = time_bucket;

    std::vector<std::pair<std::string_view, std::string_view>> extra_keys_vec;
    std::vector<std::pair<std::string_view, std::string_view>>* extra_ptr =
        nullptr;
    if (!cfg.extra_group_keys.empty()) {
        for (const auto& extra_key : cfg.extra_group_keys) {
            auto value = acc.arg_string(extra_key);
            if (!value.empty()) {
                extra_keys_vec.emplace_back(extra_key, value);
                st.observed_extra_keys.emplace(extra_key);
            }
        }
        if (!extra_keys_vec.empty()) extra_ptr = &extra_keys_vec;
    }

    std::string cat_storage;
    std::string_view cat_lower =
        internal::to_lower_ascii(acc.cat(), cat_storage);
    auto key_fhash = cfg.group_by_file ? fhash : std::string_view{};
    serialize_agg_key_into(st.key_buf, config_hash, map_type, cat_lower,
                           acc.name(), acc.pid(), acc.tid(), hhash, key_fhash,
                           time_bucket, intern, extra_ptr);

    AggregationMetrics* entry_ptr;
    if (st.last_entry != nullptr && st.last_key == st.key_buf) {
        entry_ptr = st.last_entry;
    } else {
        auto [it, inserted] =
            st.local_buffer.try_emplace(st.key_buf, cfg.sketch_accuracy);
        entry_ptr = &it->second;
        st.last_entry = entry_ptr;
        st.last_key = it->first;
    }
    auto& entry = *entry_ptr;
    if (!cfg.group_by_file && !fhash.empty())
        entry.distinct_files.add_hash(agg_hash_of_hex(fhash));
    const bool compute_percentiles = cfg.compute_percentiles;

    std::uint64_t ev_count = 1;
    if (acc.is_counter()) {
        const bool has_dft_cnt = acc.arg_exists("dftu_cnt");
        ev_count = has_dft_cnt               ? acc.arg_uint("dftu_cnt")
                   : acc.arg_exists("count") ? acc.arg_uint("count")
                                             : 1;
        entry.count += ev_count;

        const bool has_dur_sum = acc.arg_exists("dur_sum");
        if (has_dur_sum || acc.arg_exists("dur")) {
            MetricStats tmp(cfg.sketch_accuracy);
            tmp.stat.n = ev_count;
            tmp.stat.sum = static_cast<double>(
                has_dur_sum ? acc.arg_uint("dur_sum") : acc.arg_uint("dur"));
            tmp.stat.min =
                static_cast<double>(acc.arg_exists("dur_min")
                                        ? acc.arg_uint("dur_min")
                                        : (has_dur_sum ? acc.arg_uint("dur_sum")
                                                       : acc.arg_uint("dur")));
            tmp.stat.max =
                static_cast<double>(acc.arg_exists("dur_max")
                                        ? acc.arg_uint("dur_max")
                                        : (has_dur_sum ? acc.arg_uint("dur_sum")
                                                       : acc.arg_uint("dur")));
            entry.duration.merge_from(tmp);
        }

        const bool has_ret_sum = acc.arg_exists("ret_sum");
        if (has_ret_sum || acc.arg_exists("ret")) {
            MetricStats tmp(cfg.sketch_accuracy);
            tmp.stat.n = ev_count;
            tmp.stat.sum = static_cast<double>(
                has_ret_sum ? acc.arg_uint("ret_sum") : acc.arg_uint("ret"));
            tmp.stat.min =
                static_cast<double>(acc.arg_exists("ret_min")
                                        ? acc.arg_uint("ret_min")
                                        : (has_ret_sum ? acc.arg_uint("ret_sum")
                                                       : acc.arg_uint("ret")));
            tmp.stat.max =
                static_cast<double>(acc.arg_exists("ret_max")
                                        ? acc.arg_uint("ret_max")
                                        : (has_ret_sum ? acc.arg_uint("ret_sum")
                                                       : acc.arg_uint("ret")));
            entry.size.merge_from(tmp);
        }

        const bool off_sum = acc.arg_exists("offset_sum");
        const bool off_plain = acc.arg_exists("offset");
        const bool off_min = acc.arg_exists("offset_min");
        const bool off_max = acc.arg_exists("offset_max");
        if (off_sum || off_plain || off_min || off_max) {
            MetricStats tmp(cfg.sketch_accuracy);
            tmp.stat.n = ev_count;
            tmp.stat.sum =
                static_cast<double>(off_sum     ? acc.arg_uint("offset_sum")
                                    : off_plain ? acc.arg_uint("offset")
                                                : 0);
            tmp.stat.min = static_cast<double>(
                off_min     ? acc.arg_uint("offset_min")
                : off_plain ? acc.arg_uint("offset")
                            : static_cast<std::uint64_t>(tmp.stat.sum));
            tmp.stat.max = static_cast<double>(
                off_max     ? acc.arg_uint("offset_max")
                : off_plain ? acc.arg_uint("offset")
                            : static_cast<std::uint64_t>(tmp.stat.sum));
            entry.offset.merge_from(tmp);
        }

        entry.update_timestamp(acc.ts(), cfg.time_interval_us);
    } else {
        entry.update_duration(acc.dur(), compute_percentiles);
        entry.update_timestamp(acc.ts(), acc.dur());

        if (acc.arg_exists("ret") &&
            internal::is_data_transfer_op(acc.cat(), acc.name())) {
            entry.update_size(acc.arg_uint("ret"), compute_percentiles);
        }
        if (acc.arg_exists("offset")) {
            entry.update_offset(acc.arg_uint("offset"), compute_percentiles);
        }
    }

    if (cfg.track_default_args) {
        const bool is_counter_ev = acc.is_counter();
        acc.for_each_numeric_arg(
            [&](std::string_view k, std::uint64_t u, double) {
                if (is_reserved_arg(k)) return;
                if (is_counter_ev && is_preagg_suffix(k)) return;
                for (const auto& gk : cfg.extra_group_keys)
                    if (gk == k) return;
                for (const auto& cf : cfg.custom_metric_fields)
                    if (cf == k) return;
                entry.update_custom_metric(k, u, compute_percentiles);
                // The visitor collected these in seal_local_buffer by scanning
                // the accumulated custom_metrics maps; record the name here so
                // the fold's observed set (a build-time out-of-band output, not
                // a CF record) matches.
                st.observed_custom_metrics.insert(std::string(k));
            });
    }

    for (const auto& field : cfg.custom_metric_fields) {
        if (acc.is_counter()) {
            // Each of sum/min/max reads its "<field>_sum|min|max" arg, falling
            // back to the bare "<field>" arg when that suffix is absent (the
            // visitor's a_sum/a_min/a_max = args[suffix] else args[field]).
            const std::string sum_suffix = std::string(field) + "_sum";
            const std::string min_suffix = std::string(field) + "_min";
            const std::string max_suffix = std::string(field) + "_max";
            const std::string_view sum_k = acc.arg_exists(sum_suffix)
                                               ? std::string_view(sum_suffix)
                                               : field;
            if (acc.arg_exists(sum_k) && acc.arg_is_number(sum_k)) {
                if (!entry.custom_metrics) {
                    entry.custom_metrics = std::make_unique<CustomMetricsMap>();
                }
                auto& cm = *entry.custom_metrics;
                auto& stats = find_or_create(cm, field, cfg.sketch_accuracy);
                dftracer::utils::dataframe::FieldStat add;
                add.n = ev_count;
                add.sum = static_cast<double>(acc.arg_uint(sum_k));
                const std::string_view min_k =
                    acc.arg_exists(min_suffix) ? std::string_view(min_suffix)
                                               : field;
                add.min = (acc.arg_exists(min_k) && acc.arg_is_number(min_k))
                              ? static_cast<double>(acc.arg_uint(min_k))
                          : stats.stat.n ? stats.stat.min
                                         : add.sum;
                const std::string_view max_k =
                    acc.arg_exists(max_suffix) ? std::string_view(max_suffix)
                                               : field;
                add.max = (acc.arg_exists(max_k) && acc.arg_is_number(max_k))
                              ? static_cast<double>(acc.arg_uint(max_k))
                          : stats.stat.n ? stats.stat.max
                                         : add.sum;
                stats.stat.merge(add);
                st.observed_custom_metrics.insert(std::string(field));
            }
        } else {
            if (acc.arg_exists(field) && acc.arg_is_number(field)) {
                entry.update_custom_metric(field, acc.arg_uint(field),
                                           compute_percentiles);
                st.observed_custom_metrics.insert(std::string(field));
            }
        }
    }

    st.events_processed++;
}

}  // namespace dftracer::utils::trace::aggregators

#endif  // DFTRACER_UTILS_TRACE_AGGREGATORS_AGGREGATION_CORE_H
