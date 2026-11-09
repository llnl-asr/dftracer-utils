// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/views/pipeline.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <trace/views/test_view_common.h>

#include <stdexcept>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftu_utils_test;
using namespace test_view_common;

using namespace dftracer::utils::trace;

TEST_SUITE("Pipeline plan") {
    TEST_CASE("execute rejects a shape no lowering handles") {
        dstd::StringIntern intern;

        views::detail::Pipeline with_ops;
        with_ops.ops.push_back(
            {views::detail::NodeKind::StreamOp, nullptr, nullptr, nullptr});
        CHECK_THROWS_AS(run(views::detail::execute(with_ops, intern)),
                        std::logic_error);

        views::detail::Pipeline incomplete;
        CHECK_THROWS_AS(run(views::detail::execute(incomplete, intern)),
                        std::logic_error);

        views::detail::Pipeline null_sink;
        null_sink.sinks.push_back(
            {views::detail::NodeKind::Sink, nullptr, nullptr, nullptr});
        CHECK_THROWS_AS(run(views::detail::execute(null_sink, intern)),
                        std::logic_error);
    }

    TEST_CASE("a row query lowered to the identity pipeline returns its rows") {
        TestEnvironment env(200);
        dstd::String gz = test_view_common::create_mixed_trace(env, 30, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        views::detail::scan::ScanPlan base =
            views::detail::scan::from_file(dstd::String(gz), dstd::String(idx));
        dataframe::DataFrame all =
            run(views::detail::scan::collect_frame(base));
        REQUIRE(all.num_rows() == 50);
        CHECK(bhas(all, "cat"));
        CHECK(bhas(all, "name"));
        CHECK(bhas(all, "dur"));

        views::detail::scan::ScanPlan sorted = views::detail::scan::sort_by(
            views::detail::scan::filter(
                base, dftu_utils_test::ok(
                          duql::Query::from_string(R"(cat == "POSIX")"))),
            "dur", true);
        views::detail::scan::ScanPlan posix_plan = views::detail::scan::limit(
            views::detail::scan::offset(sorted, 2), 10);
        dataframe::DataFrame posix =
            run(views::detail::scan::collect_frame(posix_plan));
        REQUIRE(posix.num_rows() == 10);
        for (std::int64_t i = 0; i < posix.num_rows(); ++i)
            CHECK(bstr(posix, i, "cat") == "POSIX");
        // create_mixed_trace gives the POSIX events dur 10..39; descending,
        // skipping two, the page starts at 37.
        CHECK(bnum(posix, 0, "dur") == doctest::Approx(37.0));
        CHECK(bnum(posix, 9, "dur") == doctest::Approx(28.0));
    }
}
