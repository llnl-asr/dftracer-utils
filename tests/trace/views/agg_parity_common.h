#ifndef DFTRACER_UTILS_TESTS_AGG_PARITY_COMMON_H
#define DFTRACER_UTILS_TESTS_AGG_PARITY_COMMON_H

// The harness the two agg-engine parity binaries share: the trace the cases
// scan, the two collection paths, and the row-for-row comparisons. Split over
// two binaries so each fits its own Valgrind budget.

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/trace/aggregators/aggregator_utility.h>
#include <dftracer/utils/trace/comparator/compare_view.h>
#include <dftracer/utils/trace/views/aggfold.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/rollup_store.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <utility>

#include "groupmap_oracle.h"
#include "test_view_common.h"

namespace aggparity {

// Minimal external Fold for the fold-factory seam: counts events over the
// shared scan.
struct CountFold : dftracer::utils::trace::views::detail::Fold {
    std::shared_ptr<std::atomic<std::uint64_t>> total;
    std::uint64_t local = 0;
    explicit CountFold(std::shared_ptr<std::atomic<std::uint64_t>> t)
        : total(std::move(t)) {}
    bool accepts(const dftracer::utils::trace::views::detail::ScanShape&)
        const override {
        return true;
    }
    std::unique_ptr<dftracer::utils::trace::views::detail::Fold> slice()
        const override {
        return std::make_unique<CountFold>(total);
    }
    void step(
        const dftracer::utils::trace::views::detail::FoldBatch& b) override {
        local += b.events.size();
    }
    void seal_unit(
        const dftracer::utils::trace::views::detail::ScanUnit&) override {}
    void drop_unit(
        const dftracer::utils::trace::views::detail::ScanUnit&) override {}
    void merge(dftracer::utils::trace::views::detail::Fold& o) override {
        local += static_cast<CountFold&>(o).local;
    }
    dftracer::utils::coro::CoroTask<bool> finalize(
        const dftracer::utils::trace::views::detail::CoverageSet&) override {
        total->fetch_add(local);
        co_return true;
    }
};

struct HistBin {
    double lower;
    double upper;
    std::uint64_t count;
};

// Bins of a list<struct<lo,hi,count>> hist column at `row`.
inline std::vector<HistBin> hist_bins(const dataframe::DataFrame& b,
                                      std::int64_t row, std::string_view name) {
    const dataframe::Series& lc =
        b.columns[static_cast<std::size_t>(bcol(b, name))];
    const std::int32_t* off = lc.offsets();
    const dataframe::Series st = lc.child(0);  // struct<lo,hi,count>
    const dataframe::Series lo = st.child(0);
    const dataframe::Series hi = st.child(1);
    const dataframe::Series cnt = st.child(2);
    const double* lop = lo.data<double>();
    const double* hip = hi.data<double>();
    const std::uint64_t* cntp = cnt.data<std::uint64_t>();
    std::vector<HistBin> out;
    for (std::int32_t i = off[row]; i < off[row + 1]; ++i)
        out.push_back({lop[i], hip[i], cntp[i]});
    return out;
}

using gmoracle::groupmap_oracle;

// The engine collect path (run_collect_via_engine + post-ops), the production
// aggregation path every aggregating View collect takes.
inline dataframe::DataFrame engine_collect(
    const dftracer::utils::trace::views::detail::scan::ScanPlan& v) {
    namespace detail = dftracer::utils::trace::views::detail;
    // One pool for the whole binary: these cases each drive a collect, and a
    // fresh pool per call dominates the run under valgrind.
    static Runtime rt;
    dataframe::DataFrame result;
    rt.run_blocking("engine-collect", [&](CoroScope&) -> coro::CoroTask<void> {
        result = co_await detail::run_collect_via_engine(*v);
    });
    return detail::apply_agg_post_ops(std::move(result), *v);
}

// Two aggregation frames equal, pairing rows by their composite key text so a
// differently-ordered group set still compares row for row.
inline void frames_equal(const dataframe::DataFrame& a,
                         const dataframe::DataFrame& b,
                         const std::vector<std::string>& keys) {
    REQUIRE(a.names.size() == b.names.size());
    for (std::size_t i = 0; i < a.names.size(); ++i) {
        CHECK(a.names[i] == b.names[i]);
        CHECK(a.columns[i].type() == b.columns[i].type());
    }
    REQUIRE(a.num_rows() == b.num_rows());
    auto keyed = [&](const dataframe::DataFrame& df) {
        std::vector<std::pair<std::string, std::int64_t>> rows;
        for (std::int64_t r = 0; r < df.num_rows(); ++r) {
            std::string s;
            for (const auto& k : keys) s += bstr(df, r, k) + '\x1f';
            rows.emplace_back(std::move(s), r);
        }
        std::sort(rows.begin(), rows.end());
        return rows;
    };
    const auto ar = keyed(a);
    const auto br = keyed(b);
    for (std::size_t i = 0; i < ar.size(); ++i)
        for (const auto& name : a.names) {
            const auto c = static_cast<std::size_t>(bcol(a, name));
            if (a.columns[c].type() == dataframe::TypeId::String)
                CHECK(bstr(a, ar[i].second, name) ==
                      bstr(b, br[i].second, name));
            else
                CHECK(bnum(a, ar[i].second, name) ==
                      doctest::Approx(bnum(b, br[i].second, name)));
        }
}

// The agg-engine parity trace, written once per directory.
inline std::string write_agg_engine_trace(const std::string& dir) {
    const std::string pfw = dir + "/agg_engine.pfw";
    const std::string gz = pfw + ".gz";
    if (fs::exists(gz)) return gz;
    {
        std::ofstream ofs(pfw);
        const char* names[] = {"read", "write", "open"};
        const char* cats[] = {"POSIX", "STDIO"};
        const int pids[] = {1, 2, 3};
        const int tids[] = {10, 20};
        int ts = 1000;
        for (int i = 0; i < 90; ++i) {
            ofs << R"({"ph":"X","name":")" << names[i % 3] << R"(","cat":")"
                << cats[i % 2] << R"(","pid":)" << pids[i % 3] << R"(,"tid":)"
                << tids[i % 2] << R"(,"ts":)" << ts << R"(,"dur":)"
                << (5 + (i % 17)) << R"(,"args":{}})" << "\n";
            ts += 100;
        }
    }
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

// The engine collect path and the GroupMap oracle over the same built plan.
inline dataframe::DataFrame collect_engine_plan(
    const dftracer::utils::trace::views::detail::scan::ScanPlan& v) {
    namespace detail = dftracer::utils::trace::views::detail;
    // One pool for the whole binary: a fresh one per collect dominates the
    // run under Valgrind.
    static dftracer::utils::Runtime rt;
    dataframe::DataFrame result;
    rt.run_blocking("agg-engine",
                    [&](dftracer::utils::CoroScope&)
                        -> dftracer::utils::coro::CoroTask<void> {
                        result = co_await detail::run_collect_via_engine(*v);
                    });
    return detail::apply_agg_post_ops(std::move(result), *v);
}
inline dataframe::DataFrame collect_groupmap_plan(
    const dftracer::utils::trace::views::detail::scan::ScanPlan& v) {
    return groupmap_oracle(v);
}

inline void check_match(const dataframe::DataFrame& a0,
                        const dataframe::DataFrame& b0,
                        const std::string& key) {
    dataframe::DataFrame a = a0.sort_by(key, false);
    dataframe::DataFrame b = b0.sort_by(key, false);
    REQUIRE(a.names.size() == b.names.size());
    for (std::size_t i = 0; i < a.names.size(); ++i) {
        CHECK(a.names[i] == b.names[i]);
        CHECK(a.columns[i].type() == b.columns[i].type());
    }
    REQUIRE(a.num_rows() == b.num_rows());
    for (std::int64_t r = 0; r < a.num_rows(); ++r)
        for (const auto& name : a.names) {
            const auto c = static_cast<std::size_t>(bcol(a, name));
            if (a.columns[c].type() == dataframe::TypeId::String)
                CHECK(bstr(a, r, name) == bstr(b, r, name));
            else
                CHECK(bnum(a, r, name) == doctest::Approx(bnum(b, r, name)));
        }
}

inline void check_match_multi(const dataframe::DataFrame& a0,
                              const dataframe::DataFrame& b0,
                              const std::vector<std::string>& keys) {
    REQUIRE(a0.names.size() == b0.names.size());
    for (std::size_t i = 0; i < a0.names.size(); ++i) {
        CHECK(a0.names[i] == b0.names[i]);
        CHECK(a0.columns[i].type() == b0.columns[i].type());
    }
    REQUIRE(a0.num_rows() == b0.num_rows());
    auto sort_key = [&](const dataframe::DataFrame& df, std::int64_t r) {
        std::string s;
        for (const auto& k : keys) {
            s += bstr(df, r, k);
            s += '\x1f';
        }
        return s;
    };
    std::vector<std::pair<std::string, std::int64_t>> ar, br;
    for (std::int64_t r = 0; r < a0.num_rows(); ++r)
        ar.emplace_back(sort_key(a0, r), r);
    for (std::int64_t r = 0; r < b0.num_rows(); ++r)
        br.emplace_back(sort_key(b0, r), r);
    std::sort(ar.begin(), ar.end());
    std::sort(br.begin(), br.end());
    for (std::size_t i = 0; i < ar.size(); ++i)
        for (const auto& name : a0.names) {
            const auto c = static_cast<std::size_t>(bcol(a0, name));
            if (a0.columns[c].type() == dataframe::TypeId::String)
                CHECK(bstr(a0, ar[i].second, name) ==
                      bstr(b0, br[i].second, name));
            else
                CHECK(bnum(a0, ar[i].second, name) ==
                      doctest::Approx(bnum(b0, br[i].second, name)));
        }
}

inline void run_both_file(const std::string& file, const std::string& file_idx,
                          const GroupKey& gk, const std::string& key_col,
                          std::uint64_t mem_budget) {
    auto build = [&] {
        namespace scan = dftracer::utils::trace::views::detail::scan;
        scan::ScanPlan v = scan::from_file(file, file_idx);
        if (mem_budget) v = scan::memory_budget(v, mem_budget);
        return scan::agg(scan::group_by(v, {gk}),
                         {
                             {AggOp::Count, "", "n"},
                             {AggOp::Sum, "dur", "sum_dur"},
                             {AggOp::Mean, "dur", "mean_dur"},
                             {AggOp::Min, "dur", "min_dur"},
                             {AggOp::Max, "dur", "max_dur"},
                             {AggOp::Var, "dur", "var_dur"},
                             {AggOp::Std, "dur", "std_dur"},
                             {AggOp::Pct, "dur", "p90_dur", "", 0.9},
                             {AggOp::ArgMax, "name", "top_name", "dur"},
                             {AggOp::SetUnion, "cat", "cats"},
                         });
    };
    dataframe::DataFrame legacy = collect_groupmap_plan(build());
    dataframe::DataFrame engine = collect_engine_plan(build());
    check_match(legacy, engine, key_col);
}

inline void run_both_multi(std::vector<ViewFile> files,
                           const std::vector<GroupKey>& gks,
                           const std::vector<std::string>& key_cols,
                           std::uint64_t mem_budget) {
    auto build = [&] {
        namespace scan = dftracer::utils::trace::views::detail::scan;
        scan::ScanPlan v = scan::from_files(files);
        if (mem_budget) v = scan::memory_budget(v, mem_budget);
        return scan::agg(scan::group_by(v, gks),
                         {
                             {AggOp::Count, "", "n"},
                             {AggOp::Sum, "dur", "sum_dur"},
                             {AggOp::Mean, "dur", "mean_dur"},
                             {AggOp::Var, "dur", "var_dur"},
                             {AggOp::Pct, "dur", "p90_dur", "", 0.9},
                             {AggOp::ArgMax, "name", "top_name", "dur"},
                         });
    };
    dataframe::DataFrame legacy = collect_groupmap_plan(build());
    dataframe::DataFrame engine = collect_engine_plan(build());
    check_match_multi(legacy, engine, key_cols);
}

// The environment both binaries build their traces under. Static: doctest
// re-enters a case per subcase, and each trace is written and indexed once.
inline TestEnvironment& parity_env() {
    static TestEnvironment env(200);
    return env;
}

// One index root per trace set, so each set is one first touch.
inline std::string parity_sub(const char* name) {
    const std::string d = parity_env().get_dir() + "/" + name;
    fs::create_directories(d);
    return d;
}

}  // namespace aggparity

using namespace aggparity;  // NOLINT(google-build-using-namespace)

#endif                      // DFTRACER_UTILS_TESTS_AGG_PARITY_COMMON_H
