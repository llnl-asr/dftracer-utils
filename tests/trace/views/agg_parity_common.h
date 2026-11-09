// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_AGG_PARITY_COMMON_H
#define DFTRACER_UTILS_TESTS_AGG_PARITY_COMMON_H

// The harness the two agg-engine parity binaries share: the trace the cases
// scan, the two collection paths, and the row-for-row comparisons. Split over
// two binaries so each fits its own Valgrind budget.

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/cache/rollup_store.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/comparator/compare_view.h>
#include <dftracer/utils/trace/views/aggfold.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/groupmap_oracle.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <utility>

namespace aggparity {

// Minimal external Fold for the fold-factory seam: counts events over the
// shared scan.
struct CountFold : dftracer::utils::trace::views::detail::Fold {
    dftracer::utils::dstd::SharedPtr<std::atomic<std::uint64_t>> total;
    std::uint64_t local = 0;
    explicit CountFold(
        dftracer::utils::dstd::SharedPtr<std::atomic<std::uint64_t>> t)
        : total(std::move(t)) {}
    bool accepts(const dftracer::utils::trace::views::detail::ScanShape&)
        const override {
        return true;
    }
    dftracer::utils::dstd::UniquePtr<
        dftracer::utils::trace::views::detail::Fold>
    slice() const override {
        return dftracer::utils::dstd::make_unique<CountFold>(total);
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
inline dftracer::utils::dstd::Vector<HistBin> hist_bins(
    const dftracer::utils::dataframe::DataFrame& b, std::int64_t row,
    dftracer::utils::dstd::StringView name) {
    const dftracer::utils::dataframe::Series& lc =
        b.columns[static_cast<std::size_t>(test_view_common::bcol(b, name))];
    const std::int32_t* off = lc.offsets();
    const dftracer::utils::dataframe::Series st =
        lc.child(0);  // struct<lo,hi,count>
    const dftracer::utils::dataframe::Series lo = st.child(0);
    const dftracer::utils::dataframe::Series hi = st.child(1);
    const dftracer::utils::dataframe::Series cnt = st.child(2);
    const double* lop = lo.data<double>();
    const double* hip = hi.data<double>();
    const std::uint64_t* cntp = cnt.data<std::uint64_t>();
    dftracer::utils::dstd::Vector<HistBin> out;
    for (std::int32_t i = off[row]; i < off[row + 1]; ++i)
        out.push_back({lop[i], hip[i], cntp[i]});
    return out;
}

// The engine collect path (run_collect_via_engine + post-ops), the production
// aggregation path every aggregating View collect takes.
inline dftracer::utils::dataframe::DataFrame engine_collect(
    const dftracer::utils::trace::views::detail::scan::ScanPlan& v) {
    // One pool for the whole binary: these cases each drive a collect, and a
    // fresh pool per call dominates the run under valgrind.
    static dftracer::utils::Runtime rt;
    dftracer::utils::dataframe::DataFrame result;
    dftu_utils_test::ok(rt.block_on(dftracer::utils::coro::named(
        "engine-collect", dftracer::utils::coro::with_scope(
                              [&](dftracer::utils::coro::Scope&)
                                  -> dftracer::utils::coro::CoroTask<void> {
                                  result = co_await dftracer::utils::trace::
                                      views::detail::run_collect_via_engine(*v);
                              }))));
    return dftracer::utils::trace::views::detail::apply_agg_post_ops(
        std::move(result), *v);
}

// Two aggregation frames equal, pairing rows by their composite key text so a
// differently-ordered group set still compares row for row.
inline void frames_equal(
    const dftracer::utils::dataframe::DataFrame& a,
    const dftracer::utils::dataframe::DataFrame& b,
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& keys) {
    REQUIRE(a.names.size() == b.names.size());
    for (std::size_t i = 0; i < a.names.size(); ++i) {
        CHECK(a.names[i] == b.names[i]);
        CHECK(a.columns[i].type() == b.columns[i].type());
    }
    REQUIRE(a.num_rows() == b.num_rows());
    auto keyed = [&](const dftracer::utils::dataframe::DataFrame& df) {
        dftracer::utils::dstd::Vector<dftracer::utils::dstd::Pair<
            dftracer::utils::dstd::String, std::int64_t>>
            rows;
        for (std::int64_t r = 0; r < df.num_rows(); ++r) {
            dftracer::utils::dstd::String s;
            for (const auto& k : keys)
                s += test_view_common::bstr(df, r, k) + '\x1f';
            rows.emplace_back(std::move(s), r);
        }
        std::sort(rows.begin(), rows.end());
        return rows;
    };
    const auto ar = keyed(a);
    const auto br = keyed(b);
    for (std::size_t i = 0; i < ar.size(); ++i)
        for (const auto& name : a.names) {
            const auto c =
                static_cast<std::size_t>(test_view_common::bcol(a, name));
            if (a.columns[c].type() ==
                dftracer::utils::dataframe::TypeId::String) {
                CHECK(a.columns[c].is_null(ar[i].second) ==
                      b.columns[c].is_null(br[i].second));
                CHECK(test_view_common::bstr(a, ar[i].second, name) ==
                      test_view_common::bstr(b, br[i].second, name));
            } else
                CHECK(test_view_common::bnum(a, ar[i].second, name) ==
                      doctest::Approx(
                          test_view_common::bnum(b, br[i].second, name)));
        }
}

// The agg-engine parity trace, written once per directory.
inline dftracer::utils::dstd::String write_agg_engine_trace(
    const dftracer::utils::dstd::String& dir) {
    const dftracer::utils::dstd::String pfw = dir + "/agg_engine.pfw";
    const dftracer::utils::dstd::String gz = pfw + ".gz";
    if (dftracer::utils::dstd::fs::exists(gz).value()) return gz;
    {
        dftu_utils_test::FileOut ofs(pfw);
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
    dftracer::utils::dstd::fs::remove(pfw).discard();
    return gz;
}

// The engine collect path and the GroupMap oracle over the same built plan.
inline dftracer::utils::dataframe::DataFrame collect_engine_plan(
    const dftracer::utils::trace::views::detail::scan::ScanPlan& v) {
    // One pool for the whole binary: a fresh one per collect dominates the
    // run under Valgrind.
    static dftracer::utils::Runtime rt;
    dftracer::utils::dataframe::DataFrame result;
    dftu_utils_test::ok(rt.block_on(dftracer::utils::coro::named(
        "agg-engine", dftracer::utils::coro::with_scope(
                          [&](dftracer::utils::coro::Scope&)
                              -> dftracer::utils::coro::CoroTask<void> {
                              result = co_await dftracer::utils::trace::views::
                                  detail::run_collect_via_engine(*v);
                          }))));
    return dftracer::utils::trace::views::detail::apply_agg_post_ops(
        std::move(result), *v);
}
inline dftracer::utils::dataframe::DataFrame collect_groupmap_plan(
    const dftracer::utils::trace::views::detail::scan::ScanPlan& v) {
    return gmoracle::groupmap_oracle(v);
}

inline void check_match(const dftracer::utils::dataframe::DataFrame& a0,
                        const dftracer::utils::dataframe::DataFrame& b0,
                        const dftracer::utils::dstd::String& key) {
    dftracer::utils::dataframe::DataFrame a =
        dftu_utils_test::ok(a0.sort_by(key, false));
    dftracer::utils::dataframe::DataFrame b =
        dftu_utils_test::ok(b0.sort_by(key, false));
    REQUIRE(a.names.size() == b.names.size());
    for (std::size_t i = 0; i < a.names.size(); ++i) {
        CHECK(a.names[i] == b.names[i]);
        CHECK(a.columns[i].type() == b.columns[i].type());
    }
    REQUIRE(a.num_rows() == b.num_rows());
    for (std::int64_t r = 0; r < a.num_rows(); ++r)
        for (const auto& name : a.names) {
            const auto c =
                static_cast<std::size_t>(test_view_common::bcol(a, name));
            if (a.columns[c].type() ==
                dftracer::utils::dataframe::TypeId::String) {
                CHECK(a.columns[c].is_null(r) == b.columns[c].is_null(r));
                CHECK(test_view_common::bstr(a, r, name) ==
                      test_view_common::bstr(b, r, name));
            } else
                CHECK(test_view_common::bnum(a, r, name) ==
                      doctest::Approx(test_view_common::bnum(b, r, name)));
        }
}

inline void check_match_multi(
    const dftracer::utils::dataframe::DataFrame& a0,
    const dftracer::utils::dataframe::DataFrame& b0,
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& keys) {
    REQUIRE(a0.names.size() == b0.names.size());
    for (std::size_t i = 0; i < a0.names.size(); ++i) {
        CHECK(a0.names[i] == b0.names[i]);
        CHECK(a0.columns[i].type() == b0.columns[i].type());
    }
    REQUIRE(a0.num_rows() == b0.num_rows());
    auto sort_key = [&](const dftracer::utils::dataframe::DataFrame& df,
                        std::int64_t r) {
        dftracer::utils::dstd::String s;
        for (const auto& k : keys) {
            s += test_view_common::bstr(df, r, k);
            s += '\x1f';
        }
        return s;
    };
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::Pair<
        dftracer::utils::dstd::String, std::int64_t>>
        ar, br;
    for (std::int64_t r = 0; r < a0.num_rows(); ++r)
        ar.emplace_back(sort_key(a0, r), r);
    for (std::int64_t r = 0; r < b0.num_rows(); ++r)
        br.emplace_back(sort_key(b0, r), r);
    std::sort(ar.begin(), ar.end());
    std::sort(br.begin(), br.end());
    for (std::size_t i = 0; i < ar.size(); ++i)
        for (const auto& name : a0.names) {
            const auto c =
                static_cast<std::size_t>(test_view_common::bcol(a0, name));
            if (a0.columns[c].type() ==
                dftracer::utils::dataframe::TypeId::String) {
                CHECK(a0.columns[c].is_null(ar[i].second) ==
                      b0.columns[c].is_null(br[i].second));
                CHECK(test_view_common::bstr(a0, ar[i].second, name) ==
                      test_view_common::bstr(b0, br[i].second, name));
            } else
                CHECK(test_view_common::bnum(a0, ar[i].second, name) ==
                      doctest::Approx(
                          test_view_common::bnum(b0, br[i].second, name)));
        }
}

inline void run_both_file(const dftracer::utils::dstd::String& file,
                          const dftracer::utils::dstd::String& file_idx,
                          const dftracer::utils::trace::views::GroupKey& gk,
                          const dftracer::utils::dstd::String& key_col,
                          std::uint64_t mem_budget) {
    auto build = [&] {
        dftracer::utils::trace::views::detail::scan::ScanPlan v =
            dftracer::utils::trace::views::detail::scan::from_file(
                dftracer::utils::dstd::String(file),
                dftracer::utils::dstd::String(file_idx));
        if (mem_budget)
            v = dftracer::utils::trace::views::detail::scan::memory_budget(
                v, mem_budget);
        return dftracer::utils::trace::views::detail::scan::agg(
            dftracer::utils::trace::views::detail::scan::group_by(v, {gk}),
            {
                {dftracer::utils::trace::views::AggOp::Count, "", "n"},
                {dftracer::utils::trace::views::AggOp::Sum, "dur", "sum_dur"},
                {dftracer::utils::trace::views::AggOp::Mean, "dur", "mean_dur"},
                {dftracer::utils::trace::views::AggOp::Min, "dur", "min_dur"},
                {dftracer::utils::trace::views::AggOp::Max, "dur", "max_dur"},
                {dftracer::utils::trace::views::AggOp::Var, "dur", "var_dur"},
                {dftracer::utils::trace::views::AggOp::Std, "dur", "std_dur"},
                {dftracer::utils::trace::views::AggOp::Pct, "dur", "p90_dur",
                 "", 0.9},
                {dftracer::utils::trace::views::AggOp::ArgMax, "name",
                 "top_name", "dur"},
                {dftracer::utils::trace::views::AggOp::SetUnion, "cat", "cats"},
            });
    };
    dftracer::utils::dataframe::DataFrame legacy =
        collect_groupmap_plan(build());
    dftracer::utils::dataframe::DataFrame engine = collect_engine_plan(build());
    check_match(legacy, engine, key_col);
}

inline void run_both_multi(
    dftracer::utils::dstd::Vector<dftracer::utils::trace::views::ViewFile>
        files,
    const dftracer::utils::dstd::Vector<
        dftracer::utils::trace::views::GroupKey>& gks,
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>&
        key_cols,
    std::uint64_t mem_budget) {
    auto build = [&] {
        dftracer::utils::trace::views::detail::scan::ScanPlan v =
            dftracer::utils::trace::views::detail::scan::from_files(files);
        if (mem_budget)
            v = dftracer::utils::trace::views::detail::scan::memory_budget(
                v, mem_budget);
        return dftracer::utils::trace::views::detail::scan::agg(
            dftracer::utils::trace::views::detail::scan::group_by(v, gks),
            {
                {dftracer::utils::trace::views::AggOp::Count, "", "n"},
                {dftracer::utils::trace::views::AggOp::Sum, "dur", "sum_dur"},
                {dftracer::utils::trace::views::AggOp::Mean, "dur", "mean_dur"},
                {dftracer::utils::trace::views::AggOp::Var, "dur", "var_dur"},
                {dftracer::utils::trace::views::AggOp::Pct, "dur", "p90_dur",
                 "", 0.9},
                {dftracer::utils::trace::views::AggOp::ArgMax, "name",
                 "top_name", "dur"},
            });
    };
    dftracer::utils::dataframe::DataFrame legacy =
        collect_groupmap_plan(build());
    dftracer::utils::dataframe::DataFrame engine = collect_engine_plan(build());
    check_match_multi(legacy, engine, key_cols);
}

// The environment both binaries build their traces under. Static: doctest
// re-enters a case per subcase, and each trace is written and indexed once.
inline dftu_utils_test::TestEnvironment& parity_env() {
    static dftu_utils_test::TestEnvironment env(200);
    return env;
}

// One index root per trace set, so each set is one first touch.
inline dftracer::utils::dstd::String parity_sub(const char* name) {
    const dftracer::utils::dstd::String d = parity_env().dir() + "/" + name;
    REQUIRE(dftracer::utils::dstd::fs::create_directories(d).has_value());
    return d;
}

}  // namespace aggparity

#endif  // DFTRACER_UTILS_TESTS_AGG_PARITY_COMMON_H
