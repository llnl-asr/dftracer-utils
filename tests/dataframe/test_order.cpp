#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/kernels/order.h>
#include <dftracer/utils/dataframe/kernels/sort.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace df = dftracer::utils::dataframe;

namespace {

df::Series ints(const std::vector<std::int64_t>& v,
                const std::vector<std::uint8_t>& valid = {}) {
    std::vector<std::uint8_t> bits((v.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < v.size(); ++i)
        if (valid.empty() || valid[i])
            bits[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
    return df::Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()),
                                valid.empty() ? nullptr : bits.data());
}

std::vector<df::ColumnView> views(std::initializer_list<const df::Series*> s) {
    std::vector<df::ColumnView> out;
    for (const df::Series* c : s) out.emplace_back(*c);
    return out;
}

}  // namespace

TEST_CASE("order_rows sorts nulls last and ties by row") {
    const df::Series a = ints({3, 1, 2, 1, 9}, {1, 1, 1, 1, 0});
    const auto keys = views({&a});
    const std::vector<std::int64_t> want = {1, 3, 2, 0, 4};
    CHECK(df::order_rows(keys, 5) == want);
}

TEST_CASE("order_rows orders by several keys") {
    const df::Series a = ints({1, 1, 0, 1});
    const df::Series b = ints({5, 4, 9, 4});
    const auto keys = views({&a, &b});
    const std::vector<std::int64_t> want = {2, 1, 3, 0};
    CHECK(df::order_rows(keys, 4) == want);
    CHECK(df::order_rows({}, 3) == std::vector<std::int64_t>{0, 1, 2});
}

TEST_CASE("order_rows orders strings, unsigned and floats") {
    const df::Series s =
        df::Series::strings(std::vector<std::string>{"b", "a", "ab", "b"});
    CHECK(df::order_rows(views({&s}), 4) ==
          std::vector<std::int64_t>{1, 2, 0, 3});

    const std::uint64_t u[] = {5, 1, std::numeric_limits<std::uint64_t>::max()};
    const df::Series us = df::Series::flat(df::TypeId::Uint64, u, 3);
    CHECK(df::order_rows(views({&us}), 3) ==
          std::vector<std::int64_t>{1, 0, 2});

    const double f[] = {1.5, std::nan(""), -2.0, std::nan("")};
    const df::Series fs = df::Series::flat_f64(f, 4);
    CHECK(df::order_rows(views({&fs}), 4) ==
          std::vector<std::int64_t>{2, 0, 1, 3});
}

TEST_CASE("run_end finds the run of equal keys, nulls equal nulls") {
    const df::Series a = ints({1, 1, 2, 0, 0}, {1, 1, 1, 0, 0});
    const auto keys = views({&a});
    const auto order = df::order_rows(keys, 5);
    CHECK(df::run_end(keys, order, 0) == 2);
    CHECK(df::run_end(keys, order, 2) == 3);
    CHECK(df::run_end(keys, order, 3) == 5);
    CHECK(df::same_keys(keys, 3, 4));
    CHECK_FALSE(df::same_keys(keys, 0, 3));
}

TEST_CASE("a column view refuses an unordered type") {
    const df::Series a = ints({1, 2});
    const df::Series list =
        df::Series::list(std::vector<std::int32_t>{0, 1, 2}, ints({1, 2}));
    CHECK(df::ColumnView::supports(df::TypeId::Int64));
    CHECK_FALSE(df::ColumnView::supports(df::TypeId::List));
    CHECK_THROWS_AS((void)df::ColumnView(list), std::invalid_argument);
}

TEST_CASE("every float order in the library is the order of compare_doubles") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<double> base = {nan, 1.0,  -0.0, 0.0, -inf,
                                      inf, -1.0, nan,  0.0};
    std::vector<double> x;
    std::vector<float> xf;
    std::vector<std::int64_t> row;
    for (int rep = 0; rep < 200; ++rep)
        for (double v : base) {
            row.push_back(static_cast<std::int64_t>(x.size()));
            x.push_back(v);
            xf.push_back(static_cast<float>(v));
        }
    const auto n = static_cast<std::int64_t>(x.size());

    // The reference: a stable sort by compare_doubles.
    std::vector<std::int64_t> want(x.size());
    for (std::size_t i = 0; i < want.size(); ++i)
        want[i] = static_cast<std::int64_t>(i);
    std::stable_sort(
        want.begin(), want.end(), [&](std::int64_t a, std::int64_t b) {
            return df::compare_doubles(x[static_cast<std::size_t>(a)],
                                       x[static_cast<std::size_t>(b)]) < 0;
        });
    const auto order_of = [&](const df::Series& s) {
        std::vector<std::int64_t> got;
        for (std::int64_t i = 0; i < s.length(); ++i)
            got.push_back(s.data<std::int64_t>()[i]);
        return got;
    };
    const df::Series f64 = df::Series::flat_f64(x.data(), n);
    const df::Series f32 = df::Series::flat(df::TypeId::Float32, xf.data(), n);

    // The sort kernels.
    CHECK(order_of(df::argsort(f64, false).materialize()) == want);
    CHECK(order_of(df::argsort(f32, false).materialize()) == want);

    // The column view the window kernels and the spill merge read through.
    const df::ColumnView view(f64);
    for (std::int64_t a = 0; a < 20; ++a)
        for (std::int64_t b = 0; b < 20; ++b) {
            const int c = view.compare(a, b);
            const int w = df::compare_doubles(x[static_cast<std::size_t>(a)],
                                              x[static_cast<std::size_t>(b)]);
            CHECK(((c > 0) - (c < 0)) == ((w > 0) - (w < 0)));
        }

    // The eager sorts, and the lazy sort that spills its runs and merges them
    // through KeyData.
    df::DataFrame frame;
    frame.names = {"x", "row"};
    frame.columns.push_back(f64.share());
    frame.columns.push_back(df::Series::flat_i64(row.data(), n));
    CHECK(order_of(frame.sort_by("x", false).column("row").materialize()) ==
          want);
    CHECK(order_of(
              frame.sort_by_multi({"x"}, false).column("row").materialize()) ==
          want);
    df::DataFrame spilled = dftracer::utils::default_runtime()
                                .submit(frame.lazy()
                                            .memory_budget(4096)
                                            .sort_by("x", false)
                                            .collect(64))
                                .get();
    CHECK(order_of(spilled.column("row").materialize()) == want);
}

TEST_CASE(
    "a float column with NaN and nulls sorts values, then NaN, then nulls") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<double> base = {nan, 1.0,  -0.0, 0.0, -inf,
                                      inf, -1.0, nan,  0.0};
    const std::int64_t n = 900;
    std::vector<double> x(static_cast<std::size_t>(n));
    std::vector<std::int64_t> row(static_cast<std::size_t>(n));
    std::vector<std::uint8_t> valid(static_cast<std::size_t>((n + 7) / 8), 0);
    const auto is_null = [](std::int64_t i) { return i % 7 == 0; };
    for (std::int64_t i = 0; i < n; ++i) {
        const auto z = static_cast<std::size_t>(i);
        x[z] = base[z % base.size()];
        row[z] = i;
        if (!is_null(i))
            valid[z >> 3] |= static_cast<std::uint8_t>(1U << (z & 7));
    }
    std::vector<std::int64_t> want(row);
    std::stable_sort(
        want.begin(), want.end(), [&](std::int64_t a, std::int64_t b) {
            if (is_null(a) || is_null(b)) return !is_null(a) && is_null(b);
            return df::compare_doubles(x[static_cast<std::size_t>(a)],
                                       x[static_cast<std::size_t>(b)]) < 0;
        });
    const auto order_of = [](const df::Series& s) {
        std::vector<std::int64_t> got;
        for (std::int64_t i = 0; i < s.length(); ++i)
            got.push_back(s.data<std::int64_t>()[i]);
        return got;
    };
    df::DataFrame frame;
    frame.names = {"x", "row"};
    frame.columns.push_back(df::Series::flat_f64(x.data(), n, valid.data()));
    frame.columns.push_back(df::Series::flat_i64(row.data(), n));
    CHECK(order_of(df::argsort(frame.columns[0], false).materialize()) == want);
    CHECK(order_of(frame.sort_by("x", false).column("row").materialize()) ==
          want);
    CHECK(order_of(frame.sort_by_multi({"x", "row"}, false)
                       .column("row")
                       .materialize()) == want);
    const df::DataFrame spilled = dftracer::utils::default_runtime()
                                      .submit(frame.lazy()
                                                  .memory_budget(4096)
                                                  .sort_by("x", false)
                                                  .collect(64))
                                      .get();
    CHECK(order_of(spilled.column("row").materialize()) == want);
}
