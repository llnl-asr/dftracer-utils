#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/trace/views/pipeline.h>
#include <doctest/doctest.h>

#include <stdexcept>
#include <string>

#include "test_view_common.h"

namespace views_detail = dftracer::utils::trace::views::detail;

TEST_SUITE("Pipeline plan") {
    TEST_CASE("execute rejects a shape no lowering handles") {
        dftracer::utils::StringIntern intern;

        views_detail::Pipeline with_ops;
        with_ops.ops.push_back(
            {views_detail::NodeKind::StreamOp, nullptr, nullptr, nullptr});
        CHECK_THROWS_AS(run(views_detail::execute(with_ops, intern)),
                        std::logic_error);

        views_detail::Pipeline incomplete;
        CHECK_THROWS_AS(run(views_detail::execute(incomplete, intern)),
                        std::logic_error);

        views_detail::Pipeline null_sink;
        null_sink.sinks.push_back(
            {views_detail::NodeKind::Sink, nullptr, nullptr, nullptr});
        CHECK_THROWS_AS(run(views_detail::execute(null_sink, intern)),
                        std::logic_error);
    }

    TEST_CASE("a row query lowered to the identity pipeline returns its rows") {
        TestEnvironment env(200);
        std::string gz = test_view_common::create_mixed_trace(env, 30, 20);
        std::string idx = determine_index_path(gz, "");

        dataframe::DataFrame all =
            run(View::from_file(gz, idx).metadata(false).collect_frame());
        REQUIRE(all.num_rows() == 50);
        CHECK(bhas(all, "cat"));
        CHECK(bhas(all, "name"));
        CHECK(bhas(all, "dur"));

        dataframe::DataFrame posix = run(View::from_file(gz, idx)
                                             .metadata(false)
                                             .query(R"(cat == "POSIX")")
                                             .sort_by("dur", true)
                                             .offset(2)
                                             .limit(10)
                                             .collect_frame());
        REQUIRE(posix.num_rows() == 10);
        for (std::int64_t i = 0; i < posix.num_rows(); ++i)
            CHECK(bstr(posix, i, "cat") == "POSIX");
        // create_mixed_trace gives the POSIX events dur 10..39; descending,
        // skipping two, the page starts at 37.
        CHECK(bnum(posix, 0, "dur") == doctest::Approx(37.0));
        CHECK(bnum(posix, 9, "dur") == doctest::Approx(28.0));
    }
}
