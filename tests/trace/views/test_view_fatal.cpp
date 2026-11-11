// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_source.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <task_result.h>
#include <trace/views/test_view_common.h>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;

TEST_SUITE("View misuse") {
    TEST_CASE("reading a session result before execute ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        TraceSession session;
        Deferred<dataframe::DataFrame> pending =
            session.collect(dataframe::DataFrame{}.lazy());
        (void)pending.get();
    }

    TEST_CASE("reading an empty session result ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        Deferred<dataframe::DataFrame> empty;
        (void)empty.get();
    }

    TEST_CASE("registering on a session after execute ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        TraceSession session;
        Deferred<dataframe::DataFrame> first =
            session.collect(dataframe::DataFrame{}.lazy());
        test_view_common::run(session.execute());
        (void)session.collect(dataframe::DataFrame{}.lazy());
    }

    TEST_CASE("a terminal frame without its column ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        (void)trace::views::detail::stats_of(dataframe::DataFrame{});
    }

    TEST_CASE("an aggregate op the engine lacks ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        View v = View::from_files({}).agg({{static_cast<AggOp>(250), "", "n"}});
        (void)v.collect().get();
    }

    TEST_CASE("a per-arg reduction the dyn table lacks ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        auto plan = trace::views::detail::scan::agg(
            trace::views::detail::scan::from_files({}),
            {AggSpec(AggOp::Count)});
        auto mutated = dstd::make_shared<trace::views::detail::ViewPlan>(*plan);
        mutated->auto_numeric_metrics = true;
        mutated->numeric_arg_aggs = {AggSpec(AggOp::ArgMax, "x")};
        View v = trace::views::detail::view_of(
            trace::views::detail::scan::ScanPlan(std::move(mutated)));
        (void)v.collect().get();
    }

    TEST_CASE("an aggregate function the lowering lacks ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        using dataframe::field::AggFn;
        (void)View::from_files({}).agg(
            {dataframe::field::FieldAggExpr(static_cast<AggFn>(250), "x")});
    }

    TEST_CASE("a grouped result that does not fit its plan ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        auto flat = trace::views::detail::scan::agg(
            trace::views::detail::scan::from_files({}),
            {AggSpec(AggOp::Count)});
        auto state = test_view_common::run(
            trace::views::detail::build_engine_agg_state(*flat));
        auto grouped = trace::views::detail::scan::agg(
            trace::views::detail::scan::from_files({}),
            {AggSpec(AggOp::Count), AggSpec(AggOp::Sum, "x")});
        (void)trace::views::detail::finalize_engine_result(*state, *grouped);
    }

    TEST_CASE(
        "a group key of a type the engine cannot render ends the process" *
        doctest::skip()) {
        fresh_process::announce_abort();
        const std::int32_t days[1] = {1};
        dataframe::Series key = dataframe::Series::flat(
            dataframe::TypeId::Date32, days, 1, nullptr);
        const std::int64_t value[1] = {1};
        dataframe::Series val = dataframe::Series::flat_i64(value, 1, nullptr);
        dstd::Vector<dataframe::AggSpec> specs;
        specs.push_back(dataframe::AggSpec{dataframe::AggOp::Count, -1, "n"});
        auto state =
            dataframe::group_agg_state({&key}, {&val}, std::move(specs));
        auto plan = trace::views::detail::scan::group_by(
            trace::views::detail::scan::agg(
                trace::views::detail::scan::from_files({}),
                {AggSpec(AggOp::Count)}),
            {GroupKey::field("flag")});
        (void)trace::views::detail::finalize_engine_result(**state, *plan);
    }

    TEST_CASE(
        "a transform key over a column of the wrong type ends the process" *
        doctest::skip()) {
        fresh_process::announce_abort();
        dftracer::utils::dstd::StringIntern intern;
        trace::views::detail::FoldEvent ev;
        ev.by_path = true;
        ev.args.emplace_back(intern.get_or_insert("x"),
                             trace::views::detail::FoldEvent::ArgValue{2.5});
        dstd::Vector<trace::views::detail::FoldEvent> events{ev};
        trace::views::detail::AggInputSpec spec;
        spec.select = {"x"};
        spec.by_path = true;
        GroupKey gk = GroupKey::field("x");
        gk.transform = GroupKey::Transform::Lower;
        spec.transforms.push_back({gk, "x", "k"});
        (void)trace::views::detail::build_agg_input_frame(events, intern, spec,
                                                          nullptr);
    }

    TEST_CASE("a bucket over a column that is not numeric ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        dftracer::utils::dstd::StringIntern intern;
        trace::views::detail::FoldEvent ev;
        ev.name_id = intern.get_or_insert("read");
        dstd::Vector<trace::views::detail::FoldEvent> events{ev};
        trace::views::detail::AggInputSpec spec;
        spec.select = {"name"};
        spec.bucket_ts_src = "name";
        spec.bucket_interval = 1.0;
        (void)trace::views::detail::build_agg_input_frame(events, intern, spec,
                                                          nullptr);
    }

    TEST_CASE("the aggregation engine refuses ops it cannot run") {
        CHECK(fresh_process::run(
                  "an aggregate op the engine lacks ends the process")
                  .aborted_with("agg engine: unknown aggregate op"));
        CHECK(fresh_process::run(
                  "a per-arg reduction the dyn table lacks ends the process")
                  .aborted_with("agg engine: op has no per-arg dyn reduction"));
        CHECK(fresh_process::run(
                  "an aggregate function the lowering lacks ends the process")
                  .aborted_with("unhandled aggregate function"));
        CHECK(fresh_process::run(
                  "a grouped result that does not fit its plan ends the "
                  "process")
                  .aborted_with(
                      "agg engine: the grouped result does not fit the plan"));
        CHECK(
            fresh_process::run(
                "a group key of a type the engine cannot render ends the "
                "process")
                .aborted_with("agg engine: unexpected group-key column type"));
        CHECK(fresh_process::run(
                  "a transform key over a column of the wrong type ends the "
                  "process")
                  .aborted_with(
                      "agg engine: unexpected transform key-column type"));
        CHECK(fresh_process::run(
                  "a bucket over a column that is not numeric ends the process")
                  .aborted_with("agg engine: unexpected numeric column type"));
    }

    TEST_CASE("session misuse is fatal") {
        const char* const message =
            "ViewSession result read before execute() resolved it";
        CHECK(fresh_process::run(
                  "reading a session result before execute ends the process")
                  .aborted_with(message));
        CHECK(fresh_process::run(
                  "reading an empty session result ends the process")
                  .aborted_with(message));
        CHECK(fresh_process::run(
                  "registering on a session after execute ends the process")
                  .aborted_with("TraceSession::collect after execute()"));
    }

    TEST_CASE("a terminal frame without its column is fatal") {
        CHECK(fresh_process::run(
                  "a terminal frame without its column ends the process")
                  .aborted_with("view terminal: the result frame has no row"));
    }
}
