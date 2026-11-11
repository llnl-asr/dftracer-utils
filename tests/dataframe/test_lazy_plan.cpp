// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

DataFrame run(coro::CoroTask<dstd::Result<DataFrame>> t) {
    return dftu_utils_test::ok(
        dftracer::utils::default_runtime().submit(std::move(t)).get());
}

DataFrame make_df() {
    dstd::Vector<std::int64_t> a{1, 2, 3, 4, 5, 6};
    dstd::Vector<std::int64_t> b{10, 20, 30, 40, 50, 60};
    dstd::Vector<std::int64_t> c{7, 7, 8, 8, 9, 9};
    DataFrame df;
    df.names = {"a", "b", "c"};
    df.columns.push_back(Series::flat_i64(a.data(), 6));
    df.columns.push_back(Series::flat_i64(b.data(), 6));
    df.columns.push_back(Series::flat_i64(c.data(), 6));
    return df;
}

dstd::Vector<LazyFrame> sample_plans(const LazyFrame& base) {
    return {
        base,
        base.with_column("d", col(0) + col(1)).filter(col(0) > std::int64_t{2}),
        base.filter(col(0) > std::int64_t{2}).select({"a"}),
        base.sort_by("b", true).filter(col(1) > std::int64_t{15}),
        base.filter(col(2) > std::int64_t{7})
            .group_by(dstd::Vector<dstd::String>{"c"},
                      {{dftracer::utils::dataframe::Agg::Sum, "a", "sum_a"}})
            .sort_by("c"),
        base.select({"b", "a"}).filter(col(1) > std::int64_t{1}),
        base.join(base.select({"a", "c"}), {"a"})
            .filter(col(0) > std::int64_t{1}),
    };
}

}  // namespace

TEST_SUITE("lazy_plan") {
    TEST_CASE("pipeline runs predicate, projection, then source absorption") {
        REQUIRE(dataframe::detail::PLAN_PIPELINE.size() == 3);
        CHECK(dataframe::detail::PLAN_PIPELINE[0] ==
              dataframe::detail::PlanRule::PredicatePushdown);
        CHECK(dataframe::detail::PLAN_PIPELINE[1] ==
              dataframe::detail::PlanRule::ProjectionPushdown);
        CHECK(dataframe::detail::PLAN_PIPELINE[2] ==
              dataframe::detail::PlanRule::SourceAbsorption);
        CHECK(dstd::String(dataframe::detail::plan_rule_name(
                  dataframe::detail::PlanRule::SourceAbsorption)) ==
              "source_absorption");
        CHECK(dstd::String(dataframe::detail::plan_rule_name(
                  dataframe::detail::PlanRule::PredicatePushdown)) ==
              "predicate_pushdown");
        CHECK(dstd::String(dataframe::detail::plan_rule_name(
                  dataframe::detail::PlanRule::ProjectionPushdown)) ==
              "projection_pushdown");
    }

    TEST_CASE("fingerprint is structural") {
        LazyFrame base = make_df().lazy();
        auto build = [&](std::int64_t lit) {
            return base.with_column("d", col(0) + col(1))
                .filter(col(0) > lit)
                .select({"a", "d"});
        };
        CHECK(dataframe::detail::plan_fingerprint(build(3)) ==
              dataframe::detail::plan_fingerprint(build(3)));
        CHECK(dataframe::detail::plan_fingerprint(build(3)) !=
              dataframe::detail::plan_fingerprint(build(4)));
        CHECK(dataframe::detail::plan_fingerprint(base.select({"a", "b"})) !=
              dataframe::detail::plan_fingerprint(base.select({"b", "a"})));
        CHECK(dataframe::detail::plan_fingerprint(base.sort_by("a", true)) !=
              dataframe::detail::plan_fingerprint(base.sort_by("a", false)));
        CHECK(dataframe::detail::plan_fingerprint(
                  base.filter(col(0) > std::int64_t{1})) !=
              dataframe::detail::plan_fingerprint(
                  base.filter(col(1) > std::int64_t{1})));
    }

    TEST_CASE("fingerprint covers child plans") {
        LazyFrame base = make_df().lazy();
        LazyFrame r1 = base.filter(col(0) > std::int64_t{1});
        LazyFrame r2 = base.filter(col(0) > std::int64_t{2});
        CHECK(dataframe::detail::plan_fingerprint(base.concat(r1)) ==
              dataframe::detail::plan_fingerprint(
                  base.concat(base.filter(col(0) > std::int64_t{1}))));
        CHECK(dataframe::detail::plan_fingerprint(base.concat(r1)) !=
              dataframe::detail::plan_fingerprint(base.concat(r2)));
        CHECK(dataframe::detail::plan_fingerprint(base.join(r1, {"a"})) !=
              dataframe::detail::plan_fingerprint(base.join(r2, {"a"})));
    }

    TEST_CASE("fingerprint separates sources") {
        CHECK(dataframe::detail::plan_fingerprint(make_df().lazy()) !=
              dataframe::detail::plan_fingerprint(make_df().lazy()));
    }

    TEST_CASE("predicate pushdown alone hoists the filter") {
        LazyFrame lf = make_df()
                           .lazy()
                           .with_column("d", col(0) + col(1))
                           .filter(col(0) > std::int64_t{3});
        LazyFrame moved = dataframe::detail::apply_plan_rule(
            lf, dataframe::detail::PlanRule::PredicatePushdown);
        CHECK(dataframe::detail::plan_fingerprint(moved) !=
              dataframe::detail::plan_fingerprint(lf));
        const dstd::String plan = moved.explain();
        CHECK(plan.find("filter") < plan.find("with_column"));
        CHECK(dataframe::detail::plan_fingerprint(
                  dataframe::detail::apply_plan_rule(
                      lf, dataframe::detail::PlanRule::ProjectionPushdown)) ==
              dataframe::detail::plan_fingerprint(lf));
    }

    TEST_CASE("projection pushdown alone inserts the source projection") {
        LazyFrame lf =
            make_df().lazy().filter(col(0) > std::int64_t{2}).select({"a"});
        LazyFrame pruned = dataframe::detail::apply_plan_rule(
            lf, dataframe::detail::PlanRule::ProjectionPushdown);
        CHECK(dataframe::detail::plan_fingerprint(pruned) !=
              dataframe::detail::plan_fingerprint(lf));
        CHECK(dataframe::detail::plan_fingerprint(
                  dataframe::detail::apply_plan_rule(
                      lf, dataframe::detail::PlanRule::PredicatePushdown)) ==
              dataframe::detail::plan_fingerprint(lf));
        DataFrame got = run(pruned.collect());
        DataFrame want = run(lf.collect());
        REQUIRE(got.num_rows() == want.num_rows());
        CHECK(got.names == want.names);
    }

    TEST_CASE("optimize_plan equals the pipeline applied rule by rule") {
        LazyFrame base = make_df().lazy();
        for (const LazyFrame& lf : sample_plans(base)) {
            LazyFrame stepwise = lf;
            for (dataframe::detail::PlanRule rule :
                 dataframe::detail::PLAN_PIPELINE)
                stepwise = dataframe::detail::apply_plan_rule(stepwise, rule);
            CHECK(dataframe::detail::plan_fingerprint(
                      dataframe::detail::optimize_plan(lf)) ==
                  dataframe::detail::plan_fingerprint(stepwise));
        }
    }

    TEST_CASE("every rule and the pipeline are idempotent") {
        LazyFrame base = make_df().lazy();
        for (const LazyFrame& lf : sample_plans(base)) {
            for (dataframe::detail::PlanRule rule :
                 dataframe::detail::PLAN_PIPELINE) {
                LazyFrame once = dataframe::detail::apply_plan_rule(lf, rule);
                CHECK(dataframe::detail::plan_fingerprint(
                          dataframe::detail::apply_plan_rule(once, rule)) ==
                      dataframe::detail::plan_fingerprint(once));
            }
            LazyFrame once = dataframe::detail::optimize_plan(lf);
            CHECK(dataframe::detail::plan_fingerprint(
                      dataframe::detail::optimize_plan(once)) ==
                  dataframe::detail::plan_fingerprint(once));
        }
    }

    TEST_CASE("an optimized plan returns the same rows") {
        LazyFrame base = make_df().lazy();
        for (const LazyFrame& lf : sample_plans(base)) {
            DataFrame want = run(lf.collect());
            DataFrame got = run(dataframe::detail::optimize_plan(lf).collect());
            CHECK(got.names == want.names);
            REQUIRE(got.num_rows() == want.num_rows());
            for (std::size_t c = 0; c < want.columns.size(); ++c) {
                REQUIRE(got.columns[c].type() == want.columns[c].type());
                if (want.columns[c].type() != TypeId::Int64) continue;
                for (std::int64_t r = 0; r < want.num_rows(); ++r)
                    CHECK(got.columns[c].data<std::int64_t>()[r] ==
                          want.columns[c].data<std::int64_t>()[r]);
            }
        }
    }

    TEST_CASE("visit_plan reaches join and concat children depth first") {
        LazyFrame base = make_df().lazy();
        LazyFrame inner = base.concat(base.filter(col(0) > std::int64_t{1}));
        LazyFrame root =
            base.join(inner, {"a"}).filter(col(0) > std::int64_t{2});
        dstd::Vector<dstd::Pair<std::uint64_t, int>> seen;
        dataframe::detail::visit_plan(root, [&](const LazyFrame& lf,
                                                int depth) {
            seen.emplace_back(dataframe::detail::plan_fingerprint(lf), depth);
        });
        REQUIRE(seen.size() == 3);
        CHECK(seen[0] == dstd::Pair<std::uint64_t, int>{
                             dataframe::detail::plan_fingerprint(root), 0});
        CHECK(seen[1] == dstd::Pair<std::uint64_t, int>{
                             dataframe::detail::plan_fingerprint(inner), 1});
        CHECK(seen[2].second == 2);
    }

    TEST_CASE("visit_plan on a leaf plan visits only the root") {
        int calls = 0;
        dataframe::detail::visit_plan(make_df().lazy().select({"a"}),
                                      [&](const LazyFrame&, int depth) {
                                          CHECK(depth == 0);
                                          ++calls;
                                      });
        CHECK(calls == 1);
    }
}
