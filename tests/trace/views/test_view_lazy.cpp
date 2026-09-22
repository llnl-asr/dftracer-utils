#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test_view_common.h"

TEST_SUITE("View - lazy collect") {
    TEST_CASE("View::collect() returns a LazyFrame matching collect_frame()") {
        const auto& s = shared_trace();
        View v = View::from_file(s.gz, s.idx)
                     .metadata(false)
                     .group_by({GroupKey::cat()})
                     .agg({{AggOp::Count, "", "n"}});

        dataframe::LazyFrame lz = v.collect();
        dataframe::DataFrame via_lazy = run(lz.collect());
        dataframe::DataFrame via_eager = run(v.collect_frame());

        REQUIRE(via_lazy.num_rows() == via_eager.num_rows());
        REQUIRE(via_lazy.num_rows() == 2);
        for (std::int64_t i = 0; i < via_lazy.num_rows(); ++i) {
            CHECK(bstr(via_lazy, i, "cat") == bstr(via_eager, i, "cat"));
            CHECK(bnum(via_lazy, i, "n") == bnum(via_eager, i, "n"));
        }
    }

    TEST_CASE("View::collect() lazy plan composes filter + select") {
        const auto& s = shared_trace();
        View base = View::from_file(s.gz, s.idx).metadata(false);

        dataframe::DataFrame all_events = run(base.collect_frame());

        dataframe::LazyFrame lz =
            base.query(R"(cat == "POSIX")").collect().select({"cat", "name"});
        dataframe::DataFrame subset = run(lz.collect());

        CHECK(subset.num_rows() == 30);
        REQUIRE(bhas(subset, "cat"));
        REQUIRE(bhas(subset, "name"));
        CHECK(subset.columns.size() == 2);
        for (std::int64_t i = 0; i < subset.num_rows(); ++i)
            CHECK(bstr(subset, i, "cat") == "POSIX");
        CHECK(all_events.num_rows() == 50);
    }
}
