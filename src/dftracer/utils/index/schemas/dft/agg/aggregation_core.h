#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_CORE_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_CORE_H

#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_metrics.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The aggregation-tier accumulation, templated on an Accessor so callers only
// differ in how they read fields/args. AggregationFold drives it with a
// POD-backed accessor.
// The Accessor concept:
//   is_metadata()/is_system()/is_profile()/is_counter()/is_aggregated() -> bool
//   name()/cat() -> string_view ; pid()/tid()/ts()/dur() -> uint64
//   arg_exists(key) -> bool
//   arg_is_number(key) -> bool
//   arg_uint(key) -> uint64      (get<uint64> semantics: clamp, 0 on miss)
//   arg_double(key) -> double    (get<double> semantics: 0 on miss)
//   arg_string(key) -> string_view (empty on miss / non-string)
//   for_each_numeric_arg(fn) with fn(string_view key, uint64 u, double d) over
//     every numeric arg (non-numbers skipped by the accessor).
namespace dftracer::utils::index::schemas::dft::agg {

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

/// The mutable accumulation state of the fold: the time range of the events.
struct AggState {
    std::uint64_t min_time_bucket = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_time_bucket = 0;
    std::size_t events_processed = 0;
};

/// Applies one event's metric rules to `sink`. The rules live here once; the
/// Sink decides where the numbers go (a partial).
/// A Sink provides:
///   add_count(n) ; span(ts, len) ; duration(dur) ; size(v) ; offset(v)
///   merge_duration(FieldStat) ; merge_size(FieldStat) ;
///   merge_offset(FieldStat)
/// duration(dur) counts the event; add_count(n) counts a pre-aggregated one.
template <class Acc, class Sink>
void accumulate_event_metrics(const Acc& acc, const AggregationConfig& cfg,
                              Sink& sink) {
    using dftracer::utils::dataframe::FieldStat;
    std::uint64_t ev_count = 1;
    if (acc.is_counter()) {
        ev_count = acc.arg_exists("dftu_cnt")  ? acc.arg_uint("dftu_cnt")
                   : acc.arg_exists("dft_cnt") ? acc.arg_uint("dft_cnt")
                   : acc.arg_exists("count")   ? acc.arg_uint("count")
                                               : 1;
        sink.add_count(ev_count);

        const bool has_dur_sum = acc.arg_exists("dur_sum");
        if (has_dur_sum || acc.arg_exists("dur")) {
            FieldStat tmp;
            tmp.n = ev_count;
            tmp.sum = static_cast<double>(has_dur_sum ? acc.arg_uint("dur_sum")
                                                      : acc.arg_uint("dur"));
            tmp.min =
                static_cast<double>(acc.arg_exists("dur_min")
                                        ? acc.arg_uint("dur_min")
                                        : (has_dur_sum ? acc.arg_uint("dur_sum")
                                                       : acc.arg_uint("dur")));
            tmp.max =
                static_cast<double>(acc.arg_exists("dur_max")
                                        ? acc.arg_uint("dur_max")
                                        : (has_dur_sum ? acc.arg_uint("dur_sum")
                                                       : acc.arg_uint("dur")));
            sink.merge_duration(tmp);
        }

        const bool has_ret_sum = acc.arg_exists("ret_sum");
        if (has_ret_sum || acc.arg_exists("ret")) {
            FieldStat tmp;
            tmp.n = ev_count;
            tmp.sum = static_cast<double>(has_ret_sum ? acc.arg_uint("ret_sum")
                                                      : acc.arg_uint("ret"));
            tmp.min =
                static_cast<double>(acc.arg_exists("ret_min")
                                        ? acc.arg_uint("ret_min")
                                        : (has_ret_sum ? acc.arg_uint("ret_sum")
                                                       : acc.arg_uint("ret")));
            tmp.max =
                static_cast<double>(acc.arg_exists("ret_max")
                                        ? acc.arg_uint("ret_max")
                                        : (has_ret_sum ? acc.arg_uint("ret_sum")
                                                       : acc.arg_uint("ret")));
            sink.merge_size(tmp);
        }

        const bool off_sum = acc.arg_exists("offset_sum");
        const bool off_plain = acc.arg_exists("offset");
        const bool off_min = acc.arg_exists("offset_min");
        const bool off_max = acc.arg_exists("offset_max");
        if (off_sum || off_plain || off_min || off_max) {
            FieldStat tmp;
            tmp.n = ev_count;
            tmp.sum = static_cast<double>(off_sum ? acc.arg_uint("offset_sum")
                                          : off_plain ? acc.arg_uint("offset")
                                                      : 0);
            tmp.min = static_cast<double>(
                off_min     ? acc.arg_uint("offset_min")
                : off_plain ? acc.arg_uint("offset")
                            : static_cast<std::uint64_t>(tmp.sum));
            tmp.max = static_cast<double>(
                off_max     ? acc.arg_uint("offset_max")
                : off_plain ? acc.arg_uint("offset")
                            : static_cast<std::uint64_t>(tmp.sum));
            sink.merge_offset(tmp);
        }

        sink.span(acc.ts(), cfg.time_interval_us);
    } else {
        sink.duration(acc.dur());
        sink.span(acc.ts(), acc.dur());

        if (acc.arg_exists("ret") &&
            trace::internal::is_data_transfer_op(acc.cat(), acc.name())) {
            sink.size(acc.arg_uint("ret"));
        }
        if (acc.arg_exists("offset")) {
            sink.offset(acc.arg_uint("offset"));
        }
    }
}

/// Accounts one event in `st`: it widens the time range of the fold (its
/// metrics are the partials', see agg_partial_update.h).
template <class Acc>
void aggregate_event(const Acc& acc, AggState& st,
                     const AggregationConfig& cfg) {
    if (acc.is_metadata()) return;

    const auto time_bucket = compute_time_bucket(acc.ts(), cfg);
    if (time_bucket < st.min_time_bucket) st.min_time_bucket = time_bucket;
    if (time_bucket > st.max_time_bucket) st.max_time_bucket = time_bucket;
    st.events_processed++;
}

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_CORE_H
