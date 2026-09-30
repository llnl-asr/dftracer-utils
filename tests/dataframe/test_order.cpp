#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/kernels/order.h>
#include <doctest/doctest.h>

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
