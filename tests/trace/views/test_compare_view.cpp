#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/comparator/compare_view.h>
#include <doctest/doctest.h>

#include <string>

#include "test_view_common.h"

using namespace test_view_common;
namespace comparator = dftracer::utils::trace::comparator;

namespace {

std::size_t col(const dataframe::DataFrame& b, const std::string& name) {
    for (std::size_t i = 0; i < b.names.size(); ++i)
        if (b.names[i] == name) return i;
    throw std::runtime_error("no column: " + name);
}

// Row index whose group-key (column 0) string equals `cat`.
std::int64_t row_of(const dataframe::DataFrame& b, const std::string& cat) {
    for (std::int64_t i = 0; i < b.num_rows(); ++i)
        if (b.columns[0].string_at(i) == cat) return i;
    return -1;
}

}  // namespace

// CompareView aggregates two Views in parallel and joins their DataFrames,
// appending delta_/pct_ columns - the whole comparison on the View/DataFrame
// engine, no bespoke aggregator or comparison tree.
TEST_SUITE("CompareView") {
    TEST_CASE("count delta + pct_change between two views") {
        TestEnvironment base_env(200);
        TestEnvironment var_env(200);
        REQUIRE(base_env.is_valid());
        REQUIRE(var_env.is_valid());
        std::string base_gz = create_mixed_trace(base_env, 30, 20);
        std::string var_gz = create_mixed_trace(var_env, 40, 20);
        std::string base_idx = determine_index_path(base_gz, "");
        std::string var_idx = determine_index_path(var_gz, "");

        dftracer::utils::Runtime rt;
        dataframe::DataFrame result;
        rt.run_blocking("compare",
                        [&](dftracer::utils::CoroScope&)
                            -> dftracer::utils::coro::CoroTask<void> {
                            auto view =
                                comparator::CompareView::of(
                                    View::from_file(base_gz, base_idx),
                                    View::from_file(var_gz, var_idx))
                                    .group_by({GroupKey::cat()})
                                    .agg({AggSpec(AggOp::Count, "", "count")});
                            result = co_await view.collect();
                        });

        REQUIRE(result.num_rows() == 2);  // posix, stdio

        const std::int64_t posix = row_of(result, "posix");
        const std::int64_t stdio = row_of(result, "stdio");
        REQUIRE(posix >= 0);
        REQUIRE(stdio >= 0);

        // Join outputs are selections; materialize before raw data<> reads.
        dataframe::Series lc =
            result.columns[col(result, "l_count")].materialize();
        dataframe::Series rc =
            result.columns[col(result, "r_count")].materialize();
        dataframe::Series dc =
            result.columns[col(result, "delta_count")].materialize();
        dataframe::Series pc =
            result.columns[col(result, "pct_count")].materialize();
        const auto* l = lc.data<std::int64_t>();
        const auto* r = rc.data<std::int64_t>();
        const auto* d = dc.data<std::int64_t>();
        const auto* p = pc.data<double>();

        // baseline posix=30 -> variant 40: delta 10, +33.3%.
        CHECK(l[posix] == 30);
        CHECK(r[posix] == 40);
        CHECK(d[posix] == 10);
        CHECK(p[posix] == doctest::Approx(100.0 * 10.0 / 30.0));

        // stdio unchanged: 20 -> 20, delta 0, 0%.
        CHECK(d[stdio] == 0);
        CHECK(p[stdio] == doctest::Approx(0.0));
    }
}
