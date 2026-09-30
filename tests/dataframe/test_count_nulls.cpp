#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::Series;

namespace df = dftracer::utils::dataframe;

namespace {

// g = a, a, a, b and x = 1.0, null, 3.0, 5.0: group a holds one null.
struct Fixture {
    std::vector<std::string_view> keys{"a", "a", "a", "b"};
    std::vector<double> xs{1.0, 0.0, 3.0, 5.0};
    std::vector<std::uint8_t> valid{0x0d};  // rows 0, 2, 3 are present
    Series g = Series::strings(std::span<const std::string_view>(keys));
    Series x = Series::flat_f64(xs.data(), 4, valid.data());

    DataFrame run(std::vector<df::AggExprSpec> specs) {
        std::vector<const Series*> inputs = {&g, &x};
        return df::group_agg_expr(df::expr_col(0), specs, inputs, "g");
    }
};

}  // namespace

TEST_CASE("count of a column skips nulls per group") {
    Fixture f;
    DataFrame r = f.run({df::agg_count_valid(df::expr_col(1), "n")});
    REQUIRE(r.num_rows() == 2);
    CHECK(r.column("n").data<std::int64_t>()[0] == 2);  // a: 1.0, null, 3.0
    CHECK(r.column("n").data<std::int64_t>()[1] == 1);  // b: 5.0
}

TEST_CASE("count of a column of nulls is 0") {
    std::vector<std::string_view> keys{"a", "a"};
    std::vector<double> xs{0.0, 0.0};
    std::vector<std::uint8_t> valid{0x00};
    Series g = Series::strings(std::span<const std::string_view>(keys));
    Series x = Series::flat_f64(xs.data(), 2, valid.data());
    std::vector<const Series*> inputs = {&g, &x};
    DataFrame r = df::group_agg_expr(
        df::expr_col(0), {df::agg_count_valid(df::expr_col(1), "n")}, inputs,
        "g");
    REQUIRE(r.num_rows() == 1);
    CHECK(r.column("n").data<std::int64_t>()[0] == 0);
    CHECK(x.count() == 0);
}

TEST_CASE("count with no column still counts rows") {
    Fixture f;
    DataFrame r = f.run({df::agg_count("n")});
    REQUIRE(r.num_rows() == 2);
    CHECK(r.column("n").data<std::int64_t>()[0] == 3);
    CHECK(r.column("n").data<std::int64_t>()[1] == 1);
}

TEST_CASE("sum over count equals mean when the column has nulls") {
    Fixture f;
    DataFrame r = f.run({df::agg_sum(df::expr_col(1), "s"),
                         df::agg_count_valid(df::expr_col(1), "n"),
                         df::agg_mean(df::expr_col(1), "m")});
    REQUIRE(r.num_rows() == 2);
    const double* s = r.column("s").data<double>();
    const double* m = r.column("m").data<double>();
    const std::int64_t* n = r.column("n").data<std::int64_t>();
    for (int i = 0; i < 2; ++i) CHECK(s[i] / static_cast<double>(n[i]) == m[i]);
    CHECK(m[0] == 2.0);
}

TEST_CASE("the C ABI builder carries the column and the op") {
    dftu_agg_spec s = dftu_agg_count_valid(nullptr, "n");
    CHECK(s.op == DFTU_AGG_COUNT_VALID);
    CHECK(std::string(s.out) == "n");
}
