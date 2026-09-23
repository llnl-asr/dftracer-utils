#ifndef DFTRACER_UTILS_TRACE_STATISTICS_STATS_VIEW_H
#define DFTRACER_UTILS_TRACE_STATISTICS_STATS_VIEW_H

#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/trace/indexing/chunk_statistics.h>
#include <dftracer/utils/trace/views/view.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::statistics {

using StatCount = std::pair<std::string, std::uint64_t>;

/// Which breakdowns a report needs beyond categories (always computed, since
/// they carry the global duration/timestamp aggregates).
struct StatNeeds {
    bool names = true;
    bool pid_tids = true;
};

/// Trace statistics collected from a StatsView. Strings are owned, so the
/// result outlives the View batches it was read from.
struct ViewStats {
    std::uint64_t total_events = 0;
    std::vector<StatCount> category_counts;
    std::vector<StatCount> name_counts;
    std::vector<StatCount> pid_tid_counts;

    std::uint64_t min_timestamp_us = 0;
    std::uint64_t max_timestamp_us = 0;

    std::uint64_t duration_count = 0;
    std::int64_t duration_sum_us = 0;
    std::uint64_t duration_min_us = 0;
    std::uint64_t duration_max_us = 0;
    double duration_m2 = 0.0;  ///< central sum of squares (Welford M2)

    double duration_mean() const {
        return duration_count ? static_cast<double>(duration_sum_us) /
                                    static_cast<double>(duration_count)
                              : 0.0;
    }
};

/// A statistics view over one or more trace files: a thin composable wrapper
/// that expresses each stat as a View group-by aggregate. Every accessor
/// builds a fresh View (collect() consumes it), so a StatsView is cheap to
/// hold and describes a source, not a materialized result.
class StatsView {
   public:
    static StatsView from_file(std::string file, std::string index = "") {
        StatsView s;
        s.file_ = std::move(file);
        s.index_ = std::move(index);
        return s;
    }
    static StatsView from_files(std::vector<views::ViewFile> files) {
        StatsView s;
        s.files_ = std::move(files);
        s.multi_ = true;
        return s;
    }

    /// A fresh View over this source.
    views::View view() const {
        return multi_ ? views::View::from_files(files_)
                      : views::View::from_file(file_, index_);
    }

    coro::CoroTask<std::vector<StatCount>> category_counts() const {
        return group_counts(cat_agg());
    }
    coro::CoroTask<std::vector<StatCount>> name_counts() const {
        return group_counts(name_agg());
    }

    coro::CoroTask<std::vector<StatCount>> pid_tid_counts() const {
        dataframe::DataFrame b = co_await pid_agg().collect().collect();
        const dataframe::Series& n = b.columns[col(b, "n")];
        const dataframe::Series& pid = b.columns[0];
        const dataframe::Series& tid = b.columns[1];
        std::vector<StatCount> out;
        out.reserve(static_cast<std::size_t>(b.num_rows()));
        for (std::int64_t i = 0; i < b.num_rows(); ++i)
            out.emplace_back(std::string(pid.string_at(i)) + ":" +
                                 std::string(tid.string_at(i)),
                             static_cast<std::uint64_t>(i64_at(n, i)));
        co_return out;
    }

    /// Collect the requested breakdowns over one execute(): each aggregate is
    /// served from its rollup when materialized, and any that are not share the
    /// single scan.
    coro::CoroTask<ViewStats> collect(StatNeeds needs = {}) const {
        ViewStats st;
        st.min_timestamp_us = std::numeric_limits<std::uint64_t>::max();
        st.duration_min_us = std::numeric_limits<std::uint64_t>::max();

        views::ViewSession run = view().session();
        views::Deferred<dataframe::DataFrame> cat =
            run.collect(cat_keys(), cat_specs());
        views::Deferred<dataframe::DataFrame> name, pid;
        if (needs.names) name = run.collect(name_keys(), count_specs());
        if (needs.pid_tids) pid = run.collect(pid_keys(), count_specs());
        co_await run.execute();

        // The category pass also carries the global duration/timestamp
        // aggregates needed to combine into the summary.
        const dataframe::DataFrame& c = cat.get();
        const dataframe::Series& cn = c.columns[col(c, "n")];
        const dataframe::Series& cdsum = c.columns[col(c, "dsum")];
        const dataframe::Series& cdmin = c.columns[col(c, "dmin")];
        const dataframe::Series& cdmax = c.columns[col(c, "dmax")];
        const dataframe::Series& cdmean = c.columns[col(c, "dmean")];
        const dataframe::Series& cdvar = c.columns[col(c, "dvar")];
        const dataframe::Series& ctmin = c.columns[col(c, "tmin")];
        const dataframe::Series& ctmax = c.columns[col(c, "tmax")];

        for (std::int64_t i = 0; i < c.num_rows(); ++i) {
            const std::uint64_t ni = static_cast<std::uint64_t>(i64_at(cn, i));
            st.category_counts.emplace_back(
                std::string(c.columns[0].string_at(i)), ni);
            st.total_events += ni;
            st.duration_count += ni;
            st.duration_sum_us += i64_at(cdsum, i);
            st.duration_min_us =
                std::min(st.duration_min_us,
                         static_cast<std::uint64_t>(i64_at(cdmin, i)));
            st.duration_max_us =
                std::max(st.duration_max_us,
                         static_cast<std::uint64_t>(i64_at(cdmax, i)));
            st.min_timestamp_us =
                std::min(st.min_timestamp_us,
                         static_cast<std::uint64_t>(i64_at(ctmin, i)));
            st.max_timestamp_us =
                std::max(st.max_timestamp_us,
                         static_cast<std::uint64_t>(i64_at(ctmax, i)));
        }

        // Combine per-group M2 into a global Welford M2:
        //   M2 = sum_i ( var_i*n_i + n_i*(mean_i - grand_mean)^2 ).
        const double grand_mean = st.duration_mean();
        for (std::int64_t i = 0; i < c.num_rows(); ++i) {
            const double ni = static_cast<double>(i64_at(cn, i));
            const double delta = f64_at(cdmean, i) - grand_mean;
            st.duration_m2 += f64_at(cdvar, i) * ni + ni * delta * delta;
        }

        if (needs.names) {
            const dataframe::DataFrame& nb = name.get();
            const dataframe::Series& nn = nb.columns[col(nb, "n")];
            st.name_counts.reserve(static_cast<std::size_t>(nb.num_rows()));
            for (std::int64_t i = 0; i < nb.num_rows(); ++i)
                st.name_counts.emplace_back(
                    std::string(nb.columns[0].string_at(i)),
                    static_cast<std::uint64_t>(i64_at(nn, i)));
        }
        if (needs.pid_tids) {
            const dataframe::DataFrame& pb = pid.get();
            const dataframe::Series& pn = pb.columns[col(pb, "n")];
            const dataframe::Series& pid_col = pb.columns[0];
            const dataframe::Series& tid_col = pb.columns[1];
            st.pid_tid_counts.reserve(static_cast<std::size_t>(pb.num_rows()));
            for (std::int64_t i = 0; i < pb.num_rows(); ++i)
                st.pid_tid_counts.emplace_back(
                    std::string(pid_col.string_at(i)) + ":" +
                        std::string(tid_col.string_at(i)),
                    static_cast<std::uint64_t>(i64_at(pn, i)));
        }
        co_return st;
    }

    /// Persist the rollups so a later collect() reads them instead of scanning.
    /// Idempotent: only the selected groups whose rollup does not already exist
    /// are (re)built, and if all are present it is a no-op with no scan.
    coro::CoroTask<void> materialize(StatNeeds needs = {}) const {
        const bool want_cat = !cat_agg().reconstruct_if_cached().has_value();
        const bool want_name =
            needs.names && !name_agg().reconstruct_if_cached().has_value();
        const bool want_pid =
            needs.pid_tids && !pid_agg().reconstruct_if_cached().has_value();
        if (!want_cat && !want_name && !want_pid) co_return;

        views::ViewSession run = view().session();
        if (want_cat) run.materialize(cat_keys(), cat_specs());
        if (want_name) run.materialize(name_keys(), count_specs());
        if (want_pid) run.materialize(pid_keys(), count_specs());
        co_await run.execute();
    }

   private:
    std::string file_, index_;
    std::vector<views::ViewFile> files_;
    bool multi_ = false;

    static std::size_t col(const dataframe::DataFrame& b, const char* name) {
        for (std::size_t i = 0; i < b.names.size(); ++i)
            if (b.names[i] == name) return i;
        return 0;
    }

    // A rollup source returns integer aggregates (count/sum/min/max) as
    // Float64, while a cold scan returns them as Int64. Read either.
    static std::int64_t i64_at(const dataframe::Series& c, std::int64_t i) {
        switch (c.type()) {
            case dataframe::TypeId::Float64:
                return static_cast<std::int64_t>(c.data<double>()[i]);
            case dataframe::TypeId::Float32:
                return static_cast<std::int64_t>(c.data<float>()[i]);
            case dataframe::TypeId::Int32:
                return c.data<std::int32_t>()[i];
            default:
                return c.data<std::int64_t>()[i];
        }
    }
    static double f64_at(const dataframe::Series& c, std::int64_t i) {
        switch (c.type()) {
            case dataframe::TypeId::Int64:
                return static_cast<double>(c.data<std::int64_t>()[i]);
            case dataframe::TypeId::Int32:
                return static_cast<double>(c.data<std::int32_t>()[i]);
            case dataframe::TypeId::Float32:
                return static_cast<double>(c.data<float>()[i]);
            default:
                return c.data<double>()[i];
        }
    }

    // The scan branches, the AggregatedView builders, and the count accessors
    // share these so a rollup is reused only on an exact query-shape match.
    static std::vector<views::GroupKey> cat_keys() {
        return {views::GroupKey::cat()};
    }
    static std::vector<views::AggSpec> cat_specs() {
        using views::AggOp;
        return {{AggOp::Count, "", "n"},       {AggOp::Sum, "dur", "dsum"},
                {AggOp::Min, "dur", "dmin"},   {AggOp::Max, "dur", "dmax"},
                {AggOp::Mean, "dur", "dmean"}, {AggOp::Var, "dur", "dvar"},
                {AggOp::Min, "ts", "tmin"},    {AggOp::Max, "ts", "tmax"}};
    }
    static std::vector<views::GroupKey> name_keys() {
        return {views::GroupKey::name()};
    }
    static std::vector<views::GroupKey> pid_keys() {
        return {views::GroupKey::pid(), views::GroupKey::tid()};
    }
    static std::vector<views::AggSpec> count_specs() {
        return {{views::AggOp::Count, "", "n"}};
    }

    views::AggregatedView cat_agg() const {
        return view().group_by(cat_keys()).agg(cat_specs());
    }
    views::AggregatedView name_agg() const {
        return view().group_by(name_keys()).agg(count_specs());
    }
    views::AggregatedView pid_agg() const {
        return view().group_by(pid_keys()).agg(count_specs());
    }

    coro::CoroTask<std::vector<StatCount>> group_counts(views::View v) const {
        dataframe::DataFrame b = co_await v.collect().collect();
        const dataframe::Series& n = b.columns[col(b, "n")];
        std::vector<StatCount> out;
        out.reserve(static_cast<std::size_t>(b.num_rows()));
        for (std::int64_t i = 0; i < b.num_rows(); ++i)
            out.emplace_back(std::string(b.columns[0].string_at(i)),
                             static_cast<std::uint64_t>(i64_at(n, i)));
        co_return out;
    }
};

namespace detail {
/// Process-lifetime intern so a ChunkStatistics' StringViewMap keys stay valid
/// after the owning ViewStats is gone. A one-shot stats CLI bounds this by the
/// trace's distinct category/name/pid:tid set and frees it at process exit.
inline std::string_view intern_stat_key(std::string_view s) {
    static std::mutex m;
    static std::deque<std::string> store;
    std::lock_guard<std::mutex> lock(m);
    store.emplace_back(s);
    return store.back();
}
}  // namespace detail

/// Fill a ChunkStatistics (the report data model) from collected ViewStats.
inline void fill_chunk_statistics(indexing::ChunkStatistics& cs,
                                  const ViewStats& v) {
    cs.total_events = v.total_events;
    for (const auto& [k, c] : v.category_counts)
        cs.category_counts[detail::intern_stat_key(k)] = c;
    for (const auto& [k, c] : v.name_counts)
        cs.name_counts[detail::intern_stat_key(k)] = c;
    for (const auto& [k, c] : v.pid_tid_counts)
        cs.pid_tid_counts[detail::intern_stat_key(k)] = c;
    cs.min_timestamp_us = v.min_timestamp_us;
    cs.max_timestamp_us = v.max_timestamp_us;
    cs.duration_count = v.duration_count;
    cs.duration_sum_us = v.duration_sum_us;
    cs.duration_min_us = v.duration_min_us;
    cs.duration_max_us = v.duration_max_us;
    cs.duration_m2 = v.duration_m2;
}

}  // namespace dftracer::utils::trace::statistics

#endif  // DFTRACER_UTILS_TRACE_STATISTICS_STATS_VIEW_H
