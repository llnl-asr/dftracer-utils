#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::coro::CoroTask;
using dftracer::utils::dataframe::col;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;
using dftracer::utils::dataframe::detail::apply_plan_rule;
using dftracer::utils::dataframe::detail::optimize_plan;
using dftracer::utils::dataframe::detail::plan_fingerprint;
using dftracer::utils::dataframe::detail::PLAN_PIPELINE;
using dftracer::utils::dataframe::detail::plan_rule_name;
using dftracer::utils::dataframe::detail::PlanRule;
using dftracer::utils::dataframe::detail::visit_plan;

namespace {

DataFrame run(CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

DataFrame make_df() {
    std::vector<std::int64_t> a{1, 2, 3, 4, 5, 6};
    std::vector<std::int64_t> b{10, 20, 30, 40, 50, 60};
    std::vector<std::int64_t> c{7, 7, 8, 8, 9, 9};
    DataFrame df;
    df.names = {"a", "b", "c"};
    df.columns.push_back(Series::flat_i64(a.data(), 6));
    df.columns.push_back(Series::flat_i64(b.data(), 6));
    df.columns.push_back(Series::flat_i64(c.data(), 6));
    return df;
}

std::vector<LazyFrame> sample_plans(const LazyFrame& base) {
    return {
        base,
        base.with_column("d", col(0) + col(1)).filter(col(0) > std::int64_t{2}),
        base.filter(col(0) > std::int64_t{2}).select({"a"}),
        base.sort_by("b", true).filter(col(1) > std::int64_t{15}),
        base.filter(col(2) > std::int64_t{7})
            .group_by(std::vector<std::string>{"c"},
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
        REQUIRE(PLAN_PIPELINE.size() == 3);
        CHECK(PLAN_PIPELINE[0] == PlanRule::PredicatePushdown);
        CHECK(PLAN_PIPELINE[1] == PlanRule::ProjectionPushdown);
        CHECK(PLAN_PIPELINE[2] == PlanRule::SourceAbsorption);
        CHECK(std::string(plan_rule_name(PlanRule::SourceAbsorption)) ==
              "source_absorption");
        CHECK(std::string(plan_rule_name(PlanRule::PredicatePushdown)) ==
              "predicate_pushdown");
        CHECK(std::string(plan_rule_name(PlanRule::ProjectionPushdown)) ==
              "projection_pushdown");
    }

    TEST_CASE("fingerprint is structural") {
        LazyFrame base = make_df().lazy();
        auto build = [&](std::int64_t lit) {
            return base.with_column("d", col(0) + col(1))
                .filter(col(0) > lit)
                .select({"a", "d"});
        };
        CHECK(plan_fingerprint(build(3)) == plan_fingerprint(build(3)));
        CHECK(plan_fingerprint(build(3)) != plan_fingerprint(build(4)));
        CHECK(plan_fingerprint(base.select({"a", "b"})) !=
              plan_fingerprint(base.select({"b", "a"})));
        CHECK(plan_fingerprint(base.sort_by("a", true)) !=
              plan_fingerprint(base.sort_by("a", false)));
        CHECK(plan_fingerprint(base.filter(col(0) > std::int64_t{1})) !=
              plan_fingerprint(base.filter(col(1) > std::int64_t{1})));
    }

    TEST_CASE("fingerprint covers child plans") {
        LazyFrame base = make_df().lazy();
        LazyFrame r1 = base.filter(col(0) > std::int64_t{1});
        LazyFrame r2 = base.filter(col(0) > std::int64_t{2});
        CHECK(plan_fingerprint(base.concat(r1)) ==
              plan_fingerprint(
                  base.concat(base.filter(col(0) > std::int64_t{1}))));
        CHECK(plan_fingerprint(base.concat(r1)) !=
              plan_fingerprint(base.concat(r2)));
        CHECK(plan_fingerprint(base.join(r1, {"a"})) !=
              plan_fingerprint(base.join(r2, {"a"})));
    }

    TEST_CASE("fingerprint separates sources") {
        CHECK(plan_fingerprint(make_df().lazy()) !=
              plan_fingerprint(make_df().lazy()));
    }

    TEST_CASE("predicate pushdown alone hoists the filter") {
        LazyFrame lf = make_df()
                           .lazy()
                           .with_column("d", col(0) + col(1))
                           .filter(col(0) > std::int64_t{3});
        LazyFrame moved = apply_plan_rule(lf, PlanRule::PredicatePushdown);
        CHECK(plan_fingerprint(moved) != plan_fingerprint(lf));
        const std::string plan = moved.explain();
        CHECK(plan.find("filter") < plan.find("with_column"));
        CHECK(plan_fingerprint(apply_plan_rule(
                  lf, PlanRule::ProjectionPushdown)) == plan_fingerprint(lf));
    }

    TEST_CASE("projection pushdown alone inserts the source projection") {
        LazyFrame lf =
            make_df().lazy().filter(col(0) > std::int64_t{2}).select({"a"});
        LazyFrame pruned = apply_plan_rule(lf, PlanRule::ProjectionPushdown);
        CHECK(plan_fingerprint(pruned) != plan_fingerprint(lf));
        CHECK(plan_fingerprint(apply_plan_rule(
                  lf, PlanRule::PredicatePushdown)) == plan_fingerprint(lf));
        DataFrame got = run(pruned.collect());
        DataFrame want = run(lf.collect());
        REQUIRE(got.num_rows() == want.num_rows());
        CHECK(got.names == want.names);
    }

    TEST_CASE("optimize_plan equals the pipeline applied rule by rule") {
        LazyFrame base = make_df().lazy();
        for (const LazyFrame& lf : sample_plans(base)) {
            LazyFrame stepwise = lf;
            for (PlanRule rule : PLAN_PIPELINE)
                stepwise = apply_plan_rule(stepwise, rule);
            CHECK(plan_fingerprint(optimize_plan(lf)) ==
                  plan_fingerprint(stepwise));
        }
    }

    TEST_CASE("every rule and the pipeline are idempotent") {
        LazyFrame base = make_df().lazy();
        for (const LazyFrame& lf : sample_plans(base)) {
            for (PlanRule rule : PLAN_PIPELINE) {
                LazyFrame once = apply_plan_rule(lf, rule);
                CHECK(plan_fingerprint(apply_plan_rule(once, rule)) ==
                      plan_fingerprint(once));
            }
            LazyFrame once = optimize_plan(lf);
            CHECK(plan_fingerprint(optimize_plan(once)) ==
                  plan_fingerprint(once));
        }
    }

    TEST_CASE("an optimized plan returns the same rows") {
        LazyFrame base = make_df().lazy();
        for (const LazyFrame& lf : sample_plans(base)) {
            DataFrame want = run(lf.collect());
            DataFrame got = run(optimize_plan(lf).collect());
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
        std::vector<std::pair<std::uint64_t, int>> seen;
        visit_plan(root, [&](const LazyFrame& lf, int depth) {
            seen.emplace_back(plan_fingerprint(lf), depth);
        });
        REQUIRE(seen.size() == 3);
        CHECK(seen[0] == std::make_pair(plan_fingerprint(root), 0));
        CHECK(seen[1] == std::make_pair(plan_fingerprint(inner), 1));
        CHECK(seen[2].second == 2);
    }

    TEST_CASE("visit_plan on a leaf plan visits only the root") {
        int calls = 0;
        visit_plan(make_df().lazy().select({"a"}),
                   [&](const LazyFrame&, int depth) {
                       CHECK(depth == 0);
                       ++calls;
                   });
        CHECK(calls == 1);
    }
}
