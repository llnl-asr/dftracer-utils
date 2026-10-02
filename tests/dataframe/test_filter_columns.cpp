#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dftracer::utils::dataframe;

namespace {

DataFrame run(dftracer::utils::coro::CoroTask<DataFrame> t) {
    DataFrame out =
        dftracer::utils::default_runtime().submit(std::move(t)).get();
    for (Series& c : out.columns) c = c.materialize();
    return out;
}

DataFrame two_ints(const std::vector<std::int64_t>& a,
                   const std::vector<std::uint8_t>& av,
                   const std::vector<std::int64_t>& b,
                   const std::vector<std::uint8_t>& bv) {
    DataFrame d;
    d.names = {"a", "b"};
    const auto n = static_cast<std::int64_t>(a.size());
    d.columns.push_back(
        Series::flat_i64(a.data(), n, av.empty() ? nullptr : av.data()));
    d.columns.push_back(
        Series::flat_i64(b.data(), n, bv.empty() ? nullptr : bv.data()));
    return d;
}

std::vector<std::int64_t> column_of(const DataFrame& d, const char* name) {
    const Series& s = d.column(name);
    return {s.data<std::int64_t>(), s.data<std::int64_t>() + s.length()};
}

}  // namespace

TEST_CASE("filter keeps the rows where a column is at least another") {
    DataFrame d = two_ints({1, 5, 3}, {}, {2, 4, 3}, {});
    DataFrame out = run(
        d.lazy().filter(expr_cmp_expr(CmpOp::Ge, col(0), col(1))).collect(2));
    CHECK(column_of(out, "a") == std::vector<std::int64_t>{5, 3});
    CHECK(column_of(out, "b") == std::vector<std::int64_t>{4, 3});
}

TEST_CASE("a null on either side leaves the row out") {
    // a = 1, null, 3 and b = 2, 4, null; only row 0 has both sides.
    DataFrame d = two_ints({1, 0, 3}, {0x05}, {2, 4, 0}, {0x03});
    DataFrame out = run(
        d.lazy().filter(expr_cmp_expr(CmpOp::Lt, col(0), col(1))).collect(2));
    REQUIRE(out.num_rows() == 1);
    CHECK(column_of(out, "a")[0] == 1);
}

TEST_CASE("columns that cannot compare fail with an error naming both types") {
    DataFrame d;
    d.names = {"s", "i"};
    std::vector<std::string_view> words{"a", "b"};
    std::vector<std::int64_t> ints{1, 2};
    d.columns.push_back(
        Series::strings(std::span<const std::string_view>(words)));
    d.columns.push_back(Series::flat_i64(ints.data(), 2));
    try {
        run(d.lazy()
                .filter(expr_cmp_expr(CmpOp::Lt, col(0), col(1)))
                .collect(2));
        FAIL("a string column compared with an integer column did not fail");
    } catch (const std::exception& e) {
        const std::string msg = e.what();
        CHECK(msg.find("string") != std::string::npos);
        CHECK((msg.find("int64") != std::string::npos ||
               msg.find("integer") != std::string::npos));
    }
}

TEST_CASE("the lazy filter equals the eager filter of the same mask") {
    DataFrame d = two_ints({1, 5, 3, 9, 2}, {}, {2, 4, 3, 1, 8}, {});
    const Expr pred = expr_cmp_expr(CmpOp::Gt, col(0), col(1));
    std::vector<const Series*> inputs{&d.columns[0], &d.columns[1]};
    DataFrame eager = d.filter(eval(pred, inputs));
    DataFrame lazy = run(d.lazy().filter(pred).collect(2));
    CHECK(column_of(lazy, "a") == column_of(eager, "a"));
    CHECK(column_of(lazy, "b") == column_of(eager, "b"));
    CHECK(column_of(lazy, "a") == std::vector<std::int64_t>{5, 9});
}
