// SPDX-License-Identifier: MIT
// LazyFrame::asof and LazyFrame::interval under a memory budget must give the
// rows of the eager ops, in the same order, from sorted and merged inputs: the
// output leaves in several morsels cut by the budget, which a collect of both
// sides followed by the eager op never does.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

using I = dstd::Optional<std::int64_t>;
using S = dstd::Optional<dstd::String>;

dstd::Vector<std::uint8_t> bits_of(const dstd::Vector<bool>& valid) {
    dstd::Vector<std::uint8_t> bits((valid.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < valid.size(); ++i)
        if (valid[i]) bits[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
    return bits;
}

Series i64s(const dstd::Vector<I>& v) {
    dstd::Vector<std::int64_t> d(v.size());
    dstd::Vector<bool> valid(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        valid[i] = v[i].has_value();
        d[i] = v[i].value_or(0);
    }
    const auto bits = bits_of(valid);
    return Series::flat(TypeId::Int64, d.data(),
                        static_cast<std::int64_t>(d.size()), bits.data());
}

// A null time is a null in half the rows and a NaN in the others.
Series f64s(const dstd::Vector<I>& v) {
    dstd::Vector<double> d(v.size());
    dstd::Vector<bool> valid(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        valid[i] = v[i].has_value() || i % 2 == 0;
        d[i] = v[i] ? static_cast<double>(*v[i]) * 0.5
                    : (i % 2 == 0 ? std::numeric_limits<double>::quiet_NaN()
                                  : 0.0);
    }
    const auto bits = bits_of(valid);
    return Series::flat(TypeId::Float64, d.data(),
                        static_cast<std::int64_t>(d.size()), bits.data());
}

Series strs(const dstd::Vector<S>& v) {
    dstd::Vector<dstd::StringView> views;
    dstd::Vector<bool> valid(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        valid[i] = v[i].has_value();
        views.emplace_back(v[i] ? dstd::StringView(*v[i]) : dstd::StringView());
    }
    const auto bits = bits_of(valid);
    return Series::strings(dstd::Span<const dstd::StringView>(views),
                           bits.data());
}

template <class... Cols>
DataFrame frame(dstd::Vector<dstd::String> names, Cols&&... cols) {
    DataFrame out;
    out.names = std::move(names);
    (out.columns.push_back(std::move(cols)), ...);
    return out;
}

coro::CoroTask<dstd::Vector<DataFrame>> stream_all(LazyFrame lf,
                                                   std::int64_t max_rows) {
    dstd::Vector<DataFrame> out;
    auto g = lf.stream(max_rows);
    while (auto df = co_await g.next())
        out.push_back(dftu_utils_test::ok(std::move(*df)));
    co_return out;
}

dstd::Vector<DataFrame> morsels(const LazyFrame& lf,
                                std::int64_t max_rows = 1'000'000) {
    return dftu_utils_test::ok(dftracer::utils::default_runtime()
                                   .submit(stream_all(lf, max_rows))
                                   .get());
}

dstd::Vector<dstd::String> row_keys(const DataFrame& d) {
    dstd::Vector<Series> cols;
    for (const Series& c : d.columns) cols.push_back(c.materialize());
    dstd::Vector<dstd::String> keys;
    for (std::int64_t r = 0; r < d.num_rows(); ++r) {
        dataframe::KeyRefusal refusal;
        keys.push_back(dataframe::row_key(cols, r, refusal));
        REQUIRE_FALSE(refusal.refused());
    }
    return keys;
}

dstd::Vector<dstd::String> row_keys(const dstd::Vector<DataFrame>& parts) {
    dstd::Vector<dstd::String> keys;
    for (const DataFrame& p : parts)
        for (dstd::String& k : row_keys(p)) keys.push_back(std::move(k));
    return keys;
}

// The lazy rows must equal the eager rows in value and order, and the lazy
// morsels must carry the eager names and types.
void expect_same(const dstd::Vector<DataFrame>& lazy, const DataFrame& eager) {
    CHECK(row_keys(lazy) == row_keys(eager));
    if (eager.num_rows() == 0) return;
    REQUIRE(!lazy.empty());
    CHECK(lazy.front().names == eager.names);
    for (const DataFrame& p : lazy)
        for (std::size_t c = 0; c < eager.columns.size(); ++c)
            CHECK(p.columns[c].type() == eager.columns[c].type());
}

struct Inputs {
    DataFrame left;
    DataFrame right;
};

enum class TimeKind { Int, Float };

// Left and right frames with times that tie, by keys with nulls, nulls (or NaN
// for a float) in the times, a column both sides name `v`, and an input order
// that is not sorted.
Inputs make_inputs(std::uint32_t seed, std::int64_t nl, std::int64_t nr,
                   TimeKind kind, bool string_key) {
    std::mt19937 rng(seed);
    const auto pick = [&](int n) {
        return static_cast<std::int64_t>(rng() % static_cast<unsigned>(n));
    };
    const auto key = [&](dstd::Vector<I>& g, dstd::Vector<S>& s) {
        const std::int64_t k = pick(8) == 0 ? -1 : pick(4);
        g.push_back(k < 0 ? I{} : I{k});
        s.push_back(k < 0 ? S{} : S{dstd::String("k") + dstd::to_string(k)});
    };
    const auto time = [&](dstd::Vector<I>& t) {
        t.push_back(pick(20) == 0 ? I{} : I{pick(60)});
    };
    dstd::Vector<I> lg, lt, lv, rg, rt, rv, rv2;
    dstd::Vector<S> ls, rs;
    for (std::int64_t i = 0; i < nl; ++i) {
        key(lg, ls);
        time(lt);
        lv.push_back(I{i});
    }
    for (std::int64_t i = 0; i < nr; ++i) {
        key(rg, rs);
        time(rt);
        rv.push_back(I{i});
        rv2.push_back(pick(5) == 0 ? I{} : I{i * 10});
    }
    const bool flt = kind == TimeKind::Float;
    const Series lts = flt ? f64s(lt) : i64s(lt);
    const Series rts = flt ? f64s(rt) : i64s(rt);
    const Series lkey = string_key ? strs(ls) : i64s(lg);
    const Series rkey = string_key ? strs(rs) : i64s(rg);
    return {frame({"g", "ts", "v"}, lkey.share(), lts.share(), i64s(lv)),
            frame({"g", "ts", "v", "w"}, rkey.share(), rts.share(), i64s(rv),
                  i64s(rv2))};
}

constexpr std::uint64_t SMALL_BUDGET = 1 << 14;

LazyFrame budgeted(const DataFrame& d, std::uint64_t budget) {
    return d.lazy().memory_budget(budget);
}

dstd::Vector<DataFrame> run_asof(const Inputs& in, std::uint64_t budget,
                                 const dstd::Vector<dstd::String>& by,
                                 AsofDirection dir,
                                 dstd::Optional<double> tol) {
    return morsels(
        budgeted(in.left, budget).asof(in.right.lazy(), "ts", by, dir, tol));
}

}  // namespace

TEST_CASE("asof matches the eager op over groups, ties, nulls and any order") {
    const AsofDirection dirs[] = {AsofDirection::Backward,
                                  AsofDirection::Forward,
                                  AsofDirection::Nearest};
    const dstd::Vector<dstd::Vector<dstd::String>> bys = {{}, {"g"}};
    const dstd::Optional<double> tols[] = {dstd::nullopt, 0.0, 3.0, 6.5};
    std::uint32_t seed = 1;
    for (const TimeKind kind : {TimeKind::Int, TimeKind::Float}) {
        for (const bool string_key : {false, true}) {
            const Inputs in = make_inputs(seed++, 300, 200, kind, string_key);
            for (const auto& by : bys)
                for (const AsofDirection dir : dirs)
                    for (const auto& tol : tols) {
                        INFO("kind=" << static_cast<int>(kind) << " string_key="
                                     << string_key << " by=" << by.size()
                                     << " dir=" << static_cast<int>(dir)
                                     << " tol=" << (tol ? *tol : -1.0));
                        const DataFrame eager =
                            dftu_utils_test::ok(dataframe::asof(
                                in.left, in.right, "ts", by, dir, tol));
                        expect_same(run_asof(in, SMALL_BUDGET, by, dir, tol),
                                    eager);
                        expect_same(run_asof(in, 1, by, dir, tol), eager);
                    }
        }
    }
}

TEST_CASE("asof leaves in several morsels cut by the budget") {
    const Inputs in = make_inputs(99, 4000, 3000, TimeKind::Int, false);
    const dstd::Vector<DataFrame> parts = run_asof(
        in, SMALL_BUDGET, {"g"}, AsofDirection::Backward, dstd::nullopt);
    // Collect-then-eager would hand the whole result over in one morsel.
    CHECK(parts.size() > 4);
    const DataFrame eager = dftu_utils_test::ok(
        dataframe::asof(in.left, in.right, "ts", {"g"}, AsofDirection::Backward,
                        dstd::nullopt));
    CHECK(row_keys(parts) == row_keys(eager));
}

TEST_CASE("asof with an empty side") {
    const Inputs in = make_inputs(5, 40, 30, TimeKind::Int, false);
    const DataFrame none_l = in.left.slice(0, 0);
    const DataFrame none_r = in.right.slice(0, 0);
    for (const AsofDirection dir :
         {AsofDirection::Backward, AsofDirection::Nearest}) {
        expect_same(morsels(budgeted(none_l, SMALL_BUDGET)
                                .asof(in.right.lazy(), "ts", {"g"}, dir,
                                      dstd::nullopt)),
                    dftu_utils_test::ok(dataframe::asof(
                        none_l, in.right, "ts", {"g"}, dir, dstd::nullopt)));
        expect_same(
            morsels(budgeted(in.left, SMALL_BUDGET)
                        .asof(none_r.lazy(), "ts", {"g"}, dir, dstd::nullopt)),
            dftu_utils_test::ok(dataframe::asof(in.left, none_r, "ts", {"g"},
                                                dir, dstd::nullopt)));
        expect_same(
            morsels(budgeted(none_l, SMALL_BUDGET)
                        .asof(none_r.lazy(), "ts", {}, dir, dstd::nullopt)),
            dftu_utils_test::ok(
                dataframe::asof(none_l, none_r, "ts", {}, dir, dstd::nullopt)));
    }
}

TEST_CASE("asof refuses what the eager op refuses") {
    const Inputs in = make_inputs(3, 10, 10, TimeKind::Int, false);
    const LazyFrame left = budgeted(in.left, SMALL_BUDGET);
    dftu_utils_test::expect_error(
        left.asof(in.right.lazy(), "ts", {}, AsofDirection::Backward, -1.0)
            .status(),
        dftracer::utils::common::ErrorCode::InvalidArgument);
    dftu_utils_test::expect_error(
        left.asof(in.right.lazy(), "nope", {}, AsofDirection::Backward)
            .status(),
        dftracer::utils::common::ErrorCode::NotFound);
    const Inputs fl = make_inputs(3, 10, 10, TimeKind::Float, false);
    dftu_utils_test::expect_error(
        left.asof(fl.right.lazy(), "ts", {}, AsofDirection::Backward).status(),
        dftracer::utils::common::ErrorCode::InvalidArgument);
    const Inputs text = make_inputs(3, 10, 10, TimeKind::Int, true);
    dftu_utils_test::expect_error(
        budgeted(text.left, SMALL_BUDGET)
            .asof(text.right.lazy(), "g", {}, AsofDirection::Backward)
            .status(),
        dftracer::utils::common::ErrorCode::InvalidArgument);
}

TEST_CASE("interval matches the eager op with overlaps, nulls and outer") {
    std::uint32_t seed = 40;
    for (const bool string_key : {false, true}) {
        std::mt19937 rng(seed++);
        const auto pick = [&](int n) {
            return static_cast<std::int64_t>(rng() % static_cast<unsigned>(n));
        };
        dstd::Vector<I> lg, lp, lv, rg, rlo, rhi, rv;
        dstd::Vector<S> ls, rs;
        const auto key = [&](dstd::Vector<I>& g, dstd::Vector<S>& s) {
            const std::int64_t k = pick(8) == 0 ? -1 : pick(3);
            g.push_back(k < 0 ? I{} : I{k});
            s.push_back(k < 0 ? S{} : S{"k" + dstd::to_string(k)});
        };
        for (std::int64_t i = 0; i < 300; ++i) {
            key(lg, ls);
            lp.push_back(pick(15) == 0 ? I{} : I{pick(80)});
            lv.push_back(I{i});
        }
        for (std::int64_t i = 0; i < 120; ++i) {
            key(rg, rs);
            const std::int64_t lo = pick(80);
            rlo.push_back(pick(20) == 0 ? I{} : I{lo});
            rhi.push_back(pick(20) == 0 ? I{} : I{lo + pick(30) - 3});
            rv.push_back(I{i});
        }
        const DataFrame left =
            frame({"g", "point", "v"}, string_key ? strs(ls) : i64s(lg),
                  i64s(lp), i64s(lv));
        const DataFrame right =
            frame({"g", "lo", "hi", "v"}, string_key ? strs(rs) : i64s(rg),
                  i64s(rlo), i64s(rhi), i64s(rv));
        for (const bool outer : {false, true})
            for (const bool keyed : {false, true}) {
                const dstd::Vector<dstd::String> by =
                    keyed ? dstd::Vector<dstd::String>{"g"}
                          : dstd::Vector<dstd::String>{};
                INFO("string_key=" << string_key << " outer=" << outer
                                   << " keyed=" << keyed);
                const DataFrame eager = dftu_utils_test::ok(dataframe::interval(
                    left, right, "point", "lo", "hi", by, outer));
                for (const std::uint64_t budget :
                     {SMALL_BUDGET, std::uint64_t{1}})
                    expect_same(morsels(budgeted(left, budget)
                                            .interval(right.lazy(), "point",
                                                      "lo", "hi", by, outer)),
                                eager);
            }
    }
}

TEST_CASE("interval cuts the pairs of one point into morsels") {
    dstd::Vector<I> point{I{50}, I{500}};
    dstd::Vector<I> lo, hi, id;
    for (std::int64_t i = 0; i < 6000; ++i) {
        lo.push_back(I{i % 40});
        hi.push_back(I{100 + i % 7});
        id.push_back(I{i});
    }
    const DataFrame left = frame({"point"}, i64s(point));
    const DataFrame right =
        frame({"lo", "hi", "id"}, i64s(lo), i64s(hi), i64s(id));
    const DataFrame eager = dftu_utils_test::ok(
        dataframe::interval(left, right, "point", "lo", "hi", {}, true));
    const dstd::Vector<DataFrame> parts =
        morsels(budgeted(left, SMALL_BUDGET)
                    .interval(right.lazy(), "point", "lo", "hi", {}, true));
    CHECK(eager.num_rows() == 6001);
    CHECK(parts.size() > 3);
    CHECK(row_keys(parts) == row_keys(eager));
}

TEST_CASE("long intervals stay active across many right morsels") {
    dstd::Vector<I> point, lo, hi, id;
    for (std::int64_t i = 0; i < 3000; ++i) point.push_back(I{i});
    for (std::int64_t i = 0; i < 2000; ++i) {
        lo.push_back(I{i / 4});
        hi.push_back(I{i % 97 == 0 ? i / 4 + 2500 : i / 4 + 3});
        id.push_back(I{i});
    }
    const DataFrame left = frame({"point"}, i64s(point));
    const DataFrame right =
        frame({"lo", "hi", "id"}, i64s(lo), i64s(hi), i64s(id));
    for (const bool outer : {false, true}) {
        INFO("outer=" << outer);
        const DataFrame eager = dftu_utils_test::ok(
            dataframe::interval(left, right, "point", "lo", "hi", {}, outer));
        for (const std::uint64_t budget : {SMALL_BUDGET, std::uint64_t{1}})
            expect_same(morsels(budgeted(left, budget)
                                    .interval(budgeted(right, budget), "point",
                                              "lo", "hi", {}, outer)),
                        eager);
        // Small morsels with no budget, so the right side moves several morsels
        // between two flushes.
        expect_same(morsels(left.lazy().interval(right.lazy(), "point", "lo",
                                                 "hi", {}, outer),
                            64),
                    eager);
    }
}

TEST_CASE("interval with an empty side") {
    const DataFrame left = frame({"point"}, i64s({I{1}, I{5}}));
    const DataFrame right =
        frame({"lo", "hi", "x"}, i64s({I{0}}), i64s({I{9}}), i64s({I{7}}));
    const DataFrame none_l = left.slice(0, 0);
    const DataFrame none_r = right.slice(0, 0);
    for (const bool outer : {false, true}) {
        expect_same(morsels(budgeted(left, SMALL_BUDGET)
                                .interval(none_r.lazy(), "point", "lo", "hi",
                                          {}, outer)),
                    dftu_utils_test::ok(dataframe::interval(
                        left, none_r, "point", "lo", "hi", {}, outer)));
        expect_same(morsels(budgeted(none_l, SMALL_BUDGET)
                                .interval(right.lazy(), "point", "lo", "hi", {},
                                          outer)),
                    dftu_utils_test::ok(dataframe::interval(
                        none_l, right, "point", "lo", "hi", {}, outer)));
    }
}

TEST_CASE(
    "without a budget both plans are collected and the rows are the same") {
    const Inputs in = make_inputs(8, 120, 90, TimeKind::Int, false);
    const DataFrame eager = dftu_utils_test::ok(dataframe::asof(
        in.left, in.right, "ts", {"g"}, AsofDirection::Nearest, 4.0));
    expect_same(morsels(in.left.lazy().asof(in.right.lazy(), "ts", {"g"},
                                            AsofDirection::Nearest, 4.0)),
                eager);
}
