// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

namespace {

const dstd::Vector<double> BINS = {0, 80, 100, 120, 1e9};

dataframe::Series f64(dstd::Vector<double> v,
                      dstd::Vector<std::uint8_t> valid = {}) {
    return dataframe::Series::flat_f64(v.data(),
                                       static_cast<std::int64_t>(v.size()),
                                       valid.empty() ? nullptr : valid.data());
}

dstd::Vector<std::int32_t> ints(const dataframe::Series& s) {
    return dstd::Vector<std::int32_t>(s.data<std::int32_t>(),
                                      s.data<std::int32_t>() + s.length());
}

}  // namespace

TEST_CASE("the default is unchanged") {
    dataframe::Series r = f64({80, 100, 120, 95}).cut(f64(BINS));
    REQUIRE(r.valid());
    CHECK(ints(r) == dstd::Vector<std::int32_t>{2, 3, 4, 2});
}

TEST_CASE("right-closed interior bins equal pandas cut(labels=False)") {
    dataframe::Series r =
        f64({80, 100, 120, 95}).cut(f64(BINS), DFTU_CUT_RIGHT | DFTU_CUT_INNER);
    REQUIRE(r.valid());
    CHECK(ints(r) == dstd::Vector<std::int32_t>{0, 1, 2, 1});
}

TEST_CASE("values outside the range are null with inner bins") {
    dataframe::Series r =
        f64({0, 2e9}).cut(f64(BINS), DFTU_CUT_RIGHT | DFTU_CUT_INNER);
    REQUIRE(r.valid());
    CHECK(r.is_null(0));
    CHECK(r.is_null(1));
}

TEST_CASE("right-closed with outer bins") {
    dataframe::Series r = f64({80, 0, 2e9}).cut(f64(BINS), DFTU_CUT_RIGHT);
    REQUIRE(r.valid());
    CHECK(ints(r) == dstd::Vector<std::int32_t>{1, 0, 5});
}

TEST_CASE("a null value gives a null bin and an unknown flag is refused") {
    dataframe::Series r =
        f64({80, 0.0, 95}, {0x05}).cut(f64(BINS), DFTU_CUT_RIGHT);
    REQUIRE(r.valid());
    CHECK(r.data<std::int32_t>()[0] == 1);
    CHECK(r.is_null(1));
    CHECK_FALSE(f64({1.0}).cut(f64(BINS), 4).valid());
}

namespace {

dataframe::Series i64(dstd::Vector<std::int64_t> v) {
    return dataframe::Series::flat_i64(
        v.data(), static_cast<std::int64_t>(v.size()), nullptr);
}

}  // namespace

TEST_CASE("large integers compare exactly, not as doubles") {
    // Near 1.7e18 a double steps by 256, so e-1, e and e+1 are one double.
    const std::int64_t e = 1704067200000000000LL;
    const dstd::Vector<std::int64_t> values = {e - 1, e, e + 1};
    dataframe::Series left = i64(values).cut(i64({e}));
    CHECK(ints(left) == dstd::Vector<std::int32_t>{0, 1, 1});
    dataframe::Series right = i64(values).cut(i64({e}), DFTU_CUT_RIGHT);
    CHECK(ints(right) == dstd::Vector<std::int32_t>{0, 0, 1});
}

TEST_CASE("unsorted and sorted input give the same bins") {
    const dstd::Vector<std::int64_t> breaks = {10, 20, 30};
    dataframe::Series sorted =
        i64({5, 10, 15, 20, 25, 30, 35}).cut(i64(breaks));
    CHECK(ints(sorted) == dstd::Vector<std::int32_t>{0, 1, 1, 2, 2, 3, 3});
    dataframe::Series shuffled =
        i64({35, 5, 20, 10, 30, 15, 25}).cut(i64(breaks), DFTU_CUT_RIGHT);
    CHECK(ints(shuffled) == dstd::Vector<std::int32_t>{3, 0, 1, 0, 2, 1, 2});
}

TEST_CASE("a selection, as values or as breaks, cuts like the flat column") {
    const std::int64_t vals[] = {5, 99, 10, 99, 25, 30};
    const std::uint8_t keep[] = {0x35};  // rows 0, 2, 4, 5
    const dataframe::Series selected =
        dataframe::Series::flat_i64(vals, 6, keep).drop_nulls();
    REQUIRE(selected.encoding() == dataframe::Encoding::Selection);
    dataframe::Series left = selected.cut(i64({10, 20, 30}));
    REQUIRE(left.valid());
    CHECK(ints(left) == dstd::Vector<std::int32_t>{0, 1, 2, 3});

    const std::int64_t bvals[] = {20, 0, 10, 30};
    const std::uint8_t bkeep[] = {0x0D};  // rows 0, 2, 3
    const dataframe::Series breaks =
        dataframe::Series::flat_i64(bvals, 4, bkeep).drop_nulls();
    REQUIRE(breaks.encoding() == dataframe::Encoding::Selection);
    dataframe::Series both = i64({5, 10, 15, 20, 25, 30}).cut(breaks);
    REQUIRE(both.valid());
    CHECK(ints(both) == dstd::Vector<std::int32_t>{0, 1, 1, 2, 2, 3});

    const double fvals[] = {5.5, 99, 10.5, 99, 25.5};
    const std::uint8_t fkeep[] = {0x15};
    const dataframe::Series fsel =
        dataframe::Series::flat_f64(fvals, 5, fkeep).drop_nulls();
    REQUIRE(fsel.encoding() == dataframe::Encoding::Selection);
    dataframe::Series fr = fsel.cut(f64({10, 20}));
    REQUIRE(fr.valid());
    CHECK(ints(fr) == dstd::Vector<std::int32_t>{0, 1, 2});
}

TEST_CASE("the sorted walk handles nulls, descents, repeats and edges") {
    const dstd::Vector<std::int64_t> breaks = {10, 20, 20, 30};
    // A null in the middle keeps the walk; a descent falls back to a search.
    const std::int64_t vals[] = {5, 15, 0, 25, 7, 40, 20};
    const std::uint8_t valid[] = {0x7B};  // row 2 null
    const dataframe::Series v = dataframe::Series::flat_i64(vals, 7, valid);
    dataframe::Series left = v.cut(i64(breaks));
    REQUIRE(left.valid());
    CHECK(left.data<std::int32_t>()[0] == 0);
    CHECK(left.data<std::int32_t>()[1] == 1);
    CHECK(left.is_null(2));
    CHECK(left.data<std::int32_t>()[3] == 3);
    CHECK(left.data<std::int32_t>()[4] == 0);
    CHECK(left.data<std::int32_t>()[5] == 4);
    CHECK(left.data<std::int32_t>()[6] == 3);  // 20 equals the repeated edge
    dataframe::Series right = v.cut(i64(breaks), DFTU_CUT_RIGHT);
    CHECK(right.data<std::int32_t>()[6] == 1);
    CHECK(right.data<std::int32_t>()[3] == 3);
    CHECK(right.data<std::int32_t>()[5] == 4);
    dataframe::Series inner =
        v.cut(i64(breaks), DFTU_CUT_RIGHT | DFTU_CUT_INNER);
    CHECK(inner.is_null(0));  // below the first edge
    CHECK(inner.data<std::int32_t>()[1] == 0);
    CHECK(inner.is_null(5));  // above the last edge
    CHECK(inner.data<std::int32_t>()[6] == 0);
    dataframe::Series edges =
        i64({10, 20, 30}).cut(i64(breaks), DFTU_CUT_INNER);
    CHECK(edges.data<std::int32_t>()[0] == 0);
    CHECK(edges.data<std::int32_t>()[1] == 2);
    CHECK(edges.is_null(2));
}
