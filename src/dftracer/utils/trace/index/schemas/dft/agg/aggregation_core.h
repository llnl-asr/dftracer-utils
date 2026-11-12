// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_CORE_H
#define DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_CORE_H

#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_metrics.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

// The aggregation-tier accumulation, templated on an Accessor so callers only
// differ in how they read fields/args. AggregationFold drives it with a
// POD-backed accessor. The Accessor provides has_phase, is_metadata, is_system,
// is_profile, is_counter, is_aggregated, has_dur and arg_is_number(key),
// name, cat and arg_string(key) (empty on miss), pid, tid, ts, dur and
// metric_args(MetricArgs&).
namespace dftracer::utils::trace::index::schemas::dft::agg {

/// dftracer hashes are hex; reuse their bits rather than hashing again.
inline std::uint64_t agg_hash_of_hex(dstd::StringView sv) {
    std::uint64_t v = 0;
    for (char c : sv) {
        std::uint64_t d;
        if (dstd::is_ascii_digit(c))
            d = static_cast<std::uint64_t>(c - '0');
        else if (dstd::is_ascii_xdigit(c))
            d = static_cast<std::uint64_t>(dstd::to_ascii_lower(c) - 'a' + 10);
        else
            return dstd::hash::xxh3_64(sv);
        v = (v << 4) | d;
    }
    return v ? v : 1;
}

/// The time range of the events folded so far.
struct AggState {
    std::uint64_t min_time_bucket = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_time_bucket = 0;
    std::size_t events_processed = 0;
};

/// In the order of METRIC_ARG_NAMES.
enum class MetricArg : std::uint8_t {
    DftuCnt,
    DftCnt,
    Count,
    DurSum,
    Dur,
    DurMin,
    DurMax,
    RetSum,
    Ret,
    RetMin,
    RetMax,
    OffsetSum,
    Offset,
    OffsetMin,
    OffsetMax,
};

inline constexpr std::size_t METRIC_ARG_COUNT = 15;

inline constexpr std::array<dstd::StringView, METRIC_ARG_COUNT>
    METRIC_ARG_NAMES = {"dftu_cnt", "dft_cnt",    "count",     "dur_sum",
                        "dur",      "dur_min",    "dur_max",   "ret_sum",
                        "ret",      "ret_min",    "ret_max",   "offset_sum",
                        "offset",   "offset_min", "offset_max"};

/// The intern ids of METRIC_ARG_NAMES, resolved once per fold so an event's
/// args match by id.
struct MetricArgIds {
    std::array<std::uint32_t, METRIC_ARG_COUNT> ids;

    explicit MetricArgIds(dftracer::utils::dstd::StringIntern& intern) {
        for (std::size_t i = 0; i < METRIC_ARG_COUNT; ++i)
            ids[i] = intern.get_or_insert(METRIC_ARG_NAMES[i]);
    }
};

/// The fixed metric args an event carries. A value is 0 for an arg that is not
/// a number.
struct MetricArgs {
    std::array<std::uint64_t, METRIC_ARG_COUNT> values{};
    std::uint32_t present = 0;

    bool has(MetricArg a) const { return (present & bit(a)) != 0; }
    std::uint64_t get(MetricArg a) const {
        return values[static_cast<std::size_t>(a)];
    }
    /// Records `v` for `a`, keeping the first value of a repeated arg.
    void add(MetricArg a, std::uint64_t v) {
        if (present & bit(a)) return;
        present |= bit(a);
        values[static_cast<std::size_t>(a)] = v;
    }

   private:
    static std::uint32_t bit(MetricArg a) {
        return 1U << static_cast<unsigned>(a);
    }
};

static_assert(METRIC_ARG_COUNT <= 32);

/// Applies one event's metric rules to `sink`. A Sink provides add_count(n),
/// span(ts, len), duration(dur), size(v), offset(v) and merge_duration,
/// merge_size and merge_offset(FieldStat). add_count counts events with no
/// duration (a pre-aggregated count or a record without dur).
template <class Acc, class Sink>
void accumulate_event_metrics(const Acc& acc, const AggregationConfig& cfg,
                              Sink& sink) {
    using dftracer::utils::dataframe::FieldStat;
    MetricArgs args;
    acc.metric_args(args);
    if (acc.is_counter()) {
        const std::uint64_t ev_count =
            args.has(MetricArg::DftuCnt)  ? args.get(MetricArg::DftuCnt)
            : args.has(MetricArg::DftCnt) ? args.get(MetricArg::DftCnt)
            : args.has(MetricArg::Count)  ? args.get(MetricArg::Count)
                                          : 1;
        sink.add_count(ev_count);

        const bool has_dur_sum = args.has(MetricArg::DurSum);
        if (has_dur_sum || args.has(MetricArg::Dur)) {
            const std::uint64_t sum =
                args.get(has_dur_sum ? MetricArg::DurSum : MetricArg::Dur);
            FieldStat tmp;
            tmp.n = ev_count;
            tmp.sum = static_cast<double>(sum);
            tmp.min = static_cast<double>(args.has(MetricArg::DurMin)
                                              ? args.get(MetricArg::DurMin)
                                              : sum);
            tmp.max = static_cast<double>(args.has(MetricArg::DurMax)
                                              ? args.get(MetricArg::DurMax)
                                              : sum);
            sink.merge_duration(tmp);
        }

        const bool has_ret_sum = args.has(MetricArg::RetSum);
        if (has_ret_sum || args.has(MetricArg::Ret)) {
            const std::uint64_t sum =
                args.get(has_ret_sum ? MetricArg::RetSum : MetricArg::Ret);
            FieldStat tmp;
            tmp.n = ev_count;
            tmp.sum = static_cast<double>(sum);
            tmp.min = static_cast<double>(args.has(MetricArg::RetMin)
                                              ? args.get(MetricArg::RetMin)
                                              : sum);
            tmp.max = static_cast<double>(args.has(MetricArg::RetMax)
                                              ? args.get(MetricArg::RetMax)
                                              : sum);
            sink.merge_size(tmp);
        }

        const bool off_sum = args.has(MetricArg::OffsetSum);
        const bool off_plain = args.has(MetricArg::Offset);
        const bool off_min = args.has(MetricArg::OffsetMin);
        const bool off_max = args.has(MetricArg::OffsetMax);
        if (off_sum || off_plain || off_min || off_max) {
            FieldStat tmp;
            tmp.n = ev_count;
            tmp.sum =
                static_cast<double>(off_sum     ? args.get(MetricArg::OffsetSum)
                                    : off_plain ? args.get(MetricArg::Offset)
                                                : 0);
            tmp.min = static_cast<double>(
                off_min     ? args.get(MetricArg::OffsetMin)
                : off_plain ? args.get(MetricArg::Offset)
                            : static_cast<std::uint64_t>(tmp.sum));
            tmp.max = static_cast<double>(
                off_max     ? args.get(MetricArg::OffsetMax)
                : off_plain ? args.get(MetricArg::Offset)
                            : static_cast<std::uint64_t>(tmp.sum));
            sink.merge_offset(tmp);
        }

        sink.span(acc.ts(), cfg.time_interval_us);
    } else {
        if (acc.has_dur())
            sink.duration(acc.dur());
        else
            sink.add_count(1);
        sink.span(acc.ts(), acc.dur());

        if (args.has(MetricArg::Ret) &&
            trace::internal::is_data_transfer_op(acc.cat(), acc.name())) {
            sink.size(args.get(MetricArg::Ret));
        }
        if (args.has(MetricArg::Offset)) {
            sink.offset(args.get(MetricArg::Offset));
        }
    }
}

/// Widens the time range of `st` by one event. Its metrics go to the partials
/// (see agg_partial_update.h).
template <class Acc>
void aggregate_event(const Acc& acc, AggState& st,
                     const AggregationConfig& cfg) {
    if (acc.is_metadata()) return;

    const auto time_bucket = compute_time_bucket(acc.ts(), cfg);
    if (time_bucket < st.min_time_bucket) st.min_time_bucket = time_bucket;
    if (time_bucket > st.max_time_bucket) st.max_time_bucket = time_bucket;
    st.events_processed++;
}

}  // namespace dftracer::utils::trace::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_CORE_H
