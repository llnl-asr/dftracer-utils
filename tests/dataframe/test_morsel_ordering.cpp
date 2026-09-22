#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using dftracer::utils::coro::CoroTask;
using dftracer::utils::dataframe::Cursor;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::InMemorySource;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::Morsel;
using dftracer::utils::dataframe::Ordering;
using dftracer::utils::dataframe::Series;

namespace {

DataFrame make_df(std::int64_t n) {
    std::vector<std::int64_t> a(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) a[static_cast<std::size_t>(i)] = i;
    DataFrame df;
    df.names = {"a"};
    df.columns.push_back(Series::flat_i64(a.data(), n));
    return df;
}

struct DrainResult {
    std::vector<std::int64_t> batch_indices;
    std::vector<Ordering> orderings;
    std::vector<int> ordered_columns;
    std::vector<bool> ordered_descendings;
    std::vector<std::vector<std::int64_t>> ordered_column_values;
    std::int64_t total_rows = 0;
};

CoroTask<DrainResult> drain(std::unique_ptr<Cursor> cur,
                            std::int64_t max_rows) {
    DrainResult out;
    while (auto m = co_await cur->next(max_rows)) {
        out.batch_indices.push_back(m->batch_index);
        out.orderings.push_back(m->ordering);
        out.ordered_columns.push_back(m->ordered_column);
        out.ordered_descendings.push_back(m->ordered_descending);
        std::vector<std::int64_t> vals;
        if (m->ordering == Ordering::ByColumn && m->ordered_column >= 0) {
            const Series& c =
                m->columns[static_cast<std::size_t>(m->ordered_column)];
            vals.reserve(static_cast<std::size_t>(m->rows));
            for (std::int64_t i = 0; i < m->rows; ++i)
                vals.push_back(c.data<std::int64_t>()[i]);
        }
        out.ordered_column_values.push_back(std::move(vals));
        out.total_rows += m->rows;
    }
    co_return out;
}

DrainResult run_drain(std::unique_ptr<Cursor> cur, std::int64_t max_rows) {
    return dftracer::utils::default_runtime()
        .submit(drain(std::move(cur), max_rows))
        .get();
}

}  // namespace

TEST_CASE(
    "InMemorySource morsels are Sequence-ordered with a contiguous "
    "batch_index") {
    auto source = std::make_shared<InMemorySource>(make_df(10));
    LazyFrame lf = LazyFrame::scan(source);
    DrainResult r = run_drain(lf.open_cursor(), 3);

    REQUIRE(r.batch_indices.size() > 1);
    for (std::size_t i = 0; i < r.orderings.size(); ++i)
        CHECK(r.orderings[i] == Ordering::Sequence);
    for (std::size_t i = 0; i < r.batch_indices.size(); ++i)
        CHECK(r.batch_indices[i] == static_cast<std::int64_t>(i));
    CHECK(r.total_rows == 10);
}

TEST_CASE("select/with_column/fill_null preserve Sequence ordering") {
    auto source = std::make_shared<InMemorySource>(make_df(10));
    LazyFrame lf =
        LazyFrame::scan(source)
            .select({"a"})
            .with_column("b",
                         dftracer::utils::dataframe::col(0) *
                             dftracer::utils::dataframe::lit(std::int64_t{2}))
            .fill_null(0);
    DrainResult r = run_drain(lf.open_cursor(), 3);

    REQUIRE(r.batch_indices.size() > 1);
    for (std::size_t i = 0; i < r.orderings.size(); ++i)
        CHECK(r.orderings[i] == Ordering::Sequence);
    for (std::size_t i = 0; i < r.batch_indices.size(); ++i)
        CHECK(r.batch_indices[i] == static_cast<std::int64_t>(i));
}

TEST_CASE("sort_by claims ByColumn ordering by the sort key, and it holds") {
    auto source = std::make_shared<InMemorySource>(make_df(10));
    LazyFrame lf = LazyFrame::scan(source).sort_by("a", /*descending=*/true);
    DrainResult r = run_drain(lf.open_cursor(), 1 << 20);

    REQUIRE(!r.orderings.empty());
    for (std::size_t i = 0; i < r.orderings.size(); ++i) {
        CHECK(r.orderings[i] == Ordering::ByColumn);
        CHECK(r.ordered_columns[i] == 0);
        CHECK(r.ordered_descendings[i] == true);
        for (std::size_t j = 1; j < r.ordered_column_values[i].size(); ++j)
            CHECK(r.ordered_column_values[i][j - 1] >=
                  r.ordered_column_values[i][j]);
    }
}

TEST_CASE("filter downstream of sort_by still claims and holds the ordering") {
    auto source = std::make_shared<InMemorySource>(make_df(10));
    LazyFrame lf =
        LazyFrame::scan(source)
            .sort_by("a", /*descending=*/false)
            .filter(dftracer::utils::dataframe::col(0) > std::int64_t{2});
    DrainResult r = run_drain(lf.open_cursor(), 1 << 20);

    REQUIRE(!r.orderings.empty());
    for (std::size_t i = 0; i < r.orderings.size(); ++i) {
        CHECK(r.orderings[i] == Ordering::ByColumn);
        CHECK(r.ordered_columns[i] == 0);
        CHECK(r.ordered_descendings[i] == false);
        for (std::size_t j = 1; j < r.ordered_column_values[i].size(); ++j)
            CHECK(r.ordered_column_values[i][j - 1] <=
                  r.ordered_column_values[i][j]);
    }
}

TEST_CASE("with_row_index claims ByColumn ordering by the new index column") {
    auto source = std::make_shared<InMemorySource>(make_df(10));
    LazyFrame lf = LazyFrame::scan(source).with_row_index("idx");
    DrainResult r = run_drain(lf.open_cursor(), 3);

    REQUIRE(!r.orderings.empty());
    for (std::size_t i = 0; i < r.orderings.size(); ++i) {
        CHECK(r.orderings[i] == Ordering::ByColumn);
        CHECK(r.ordered_columns[i] == 0);
        CHECK(r.ordered_descendings[i] == false);
        for (std::size_t j = 1; j < r.ordered_column_values[i].size(); ++j)
            CHECK(r.ordered_column_values[i][j - 1] <
                  r.ordered_column_values[i][j]);
    }
}

TEST_CASE(
    "an op that reorders rows (group_by after sort_by) clears the claim") {
    auto source = std::make_shared<InMemorySource>(make_df(10));
    dftracer::utils::dataframe::GroupAgg count_agg;
    count_agg.op = dftracer::utils::dataframe::Agg::Count;
    count_agg.out = "n";
    LazyFrame lf = LazyFrame::scan(source)
                       .sort_by("a", /*descending=*/false)
                       .group_by("a", {count_agg});
    DrainResult r = run_drain(lf.open_cursor(), 1 << 20);

    REQUIRE(!r.orderings.empty());
    for (Ordering o : r.orderings) CHECK(o == Ordering::Unordered);
}

TEST_CASE("group_by reports Unordered downstream") {
    auto source = std::make_shared<InMemorySource>(make_df(10));
    dftracer::utils::dataframe::GroupAgg count_agg;
    count_agg.op = dftracer::utils::dataframe::Agg::Count;
    count_agg.out = "n";
    LazyFrame lf = LazyFrame::scan(source).group_by("a", {count_agg});
    DrainResult r = run_drain(lf.open_cursor(), 1 << 20);

    REQUIRE(!r.orderings.empty());
    for (Ordering o : r.orderings) CHECK(o == Ordering::Unordered);
}

TEST_CASE("unique reports Unordered downstream") {
    auto source = std::make_shared<InMemorySource>(make_df(10));
    LazyFrame lf = LazyFrame::scan(source).unique();
    DrainResult r = run_drain(lf.open_cursor(), 1 << 20);

    REQUIRE(!r.orderings.empty());
    for (Ordering o : r.orderings) CHECK(o == Ordering::Unordered);
}
