// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>

using namespace dftracer::utils;

namespace {

dataframe::Series i64(dstd::Vector<std::int64_t> v) {
    return dataframe::Series::flat_i64(v.data(),
                                       static_cast<std::int64_t>(v.size()));
}

// A Bool column; `valid` (when non-empty) marks the non-null slots.
dataframe::Series bools(const dstd::Vector<bool>& v,
                        const dstd::Vector<bool>& valid = {}) {
    const std::size_t nbytes = (v.size() + 7) / 8;
    dstd::Vector<std::uint8_t> bits(nbytes, 0);
    dstd::Vector<std::uint8_t> vbits(nbytes, 0);
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i]) bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        if (valid.empty() || valid[i])
            vbits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
    return dataframe::Series::flat(dataframe::TypeId::Bool, bits.data(),
                                   static_cast<std::int64_t>(v.size()),
                                   valid.empty() ? nullptr : vbits.data());
}

dataframe::Series strs(const dstd::Vector<dstd::StringView>& v,
                       const dstd::Vector<bool>& valid = {}) {
    dstd::Vector<std::uint8_t> vbits((v.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < v.size(); ++i)
        if (valid.empty() || valid[i])
            vbits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return dataframe::Series::strings(dstd::Span<const dstd::StringView>(v),
                                      valid.empty() ? nullptr : vbits.data());
}

dataframe::Series timestamps(const dstd::Vector<std::int64_t>& v) {
    return dataframe::Series::flat(dataframe::TypeId::Timestamp, v.data(),
                                   static_cast<std::int64_t>(v.size()));
}

dstd::String error_text(dftu_scalar s) {
    REQUIRE(s.kind == DFTU_SCALAR_TAG_ERR);
    REQUIRE(s.value.err != nullptr);
    REQUIRE(s.value.err->message != nullptr);
    return s.value.err->message;
}

bool names(const dstd::String& text, const char* a, const char* b) {
    return text.find(a) != dstd::String::npos &&
           text.find(b) != dstd::String::npos;
}

}  // namespace

TEST_CASE("a bool column counts true values") {
    dataframe::Series flag = bools({true, false, true, true});
    CHECK(dftu_utils_test::ok(flag.sum()).i64() == 3);
    CHECK(dftu_utils_test::ok(flag.mean()) == doctest::Approx(0.75));
    CHECK(dftu_utils_test::ok(flag.min()).i64() == 0);
    CHECK(dftu_utils_test::ok(flag.max()).i64() == 1);

    // Nulls are skipped, as for numbers.
    dataframe::Series with_null =
        bools({true, true, false}, {true, false, true});
    CHECK(dftu_utils_test::ok(with_null.sum()).i64() == 1);
    CHECK(dftu_utils_test::ok(with_null.mean()) == doctest::Approx(0.5));
}

TEST_CASE("a comparison mask reduces the same way") {
    dataframe::Series x = i64({1, 2, 3, 4});
    dataframe::Series mask = x > 2;
    CHECK(dftu_utils_test::ok(mask.sum()).i64() == 2);
}

TEST_CASE("a bool reduction agrees with the group-by sum") {
    const std::int64_t expected =
        dftu_utils_test::ok(bools({true, false, true, true}).sum()).i64();
    dataframe::DataFrame df;
    df.names = {"k", "flag"};
    df.columns.push_back(i64({7, 7, 7, 7}));
    df.columns.push_back(bools({true, false, true, true}));
    dataframe::DataFrame g = dftu_utils_test::ok(df.group_by(
        "k", {dataframe::GroupAgg{dataframe::Agg::Sum, "flag", "total"}}));
    REQUIRE(g.num_rows() == 1);
    CHECK(g.column("total").data<std::int64_t>()[0] == expected);
}

TEST_CASE("min and max of a string column are the bytewise extremes") {
    dataframe::Series s = strs({"b", "a", "d", "c"});
    CHECK(dftu_utils_test::ok(s.min()).str() == "a");
    CHECK(dftu_utils_test::ok(s.max()).str() == "d");

    dataframe::Series with_null = strs({"zz", "a", "q"}, {true, false, true});
    CHECK(dftu_utils_test::ok(with_null.min()).str() == "q");
    CHECK(dftu_utils_test::ok(with_null.max()).str() == "zz");

    dataframe::Series none = strs({"x", "y"}, {false, false});
    CHECK(dftu_utils_test::ok(none.min()).str().empty());
    CHECK(dftu_utils_test::ok(none.max()).str().empty());
}

TEST_CASE("sum and mean of a string column are refused") {
    dataframe::Series s = strs({"a", "b"});
    const dftu_scalar r = dftu_series_reduce(s.handle(), DFTU_REDUCE_SUM);
    CHECK(names(error_text(r), "sum", "string"));
    dftu_utils_test::expect_error(
        s.sum(), dftracer::utils::common::ErrorCode::InvalidArgument);
    dftu_utils_test::expect_error(
        s.mean(), dftracer::utils::common::ErrorCode::InvalidArgument);

    // A string column with no valid value is still refused for sum and mean.
    dataframe::Series none = strs({"x"}, {false});
    dftu_utils_test::expect_error(
        none.sum(), dftracer::utils::common::ErrorCode::InvalidArgument);
    dftu_utils_test::expect_error(
        none.mean(), dftracer::utils::common::ErrorCode::InvalidArgument);
}

TEST_CASE("the refusal names the reduction and the type") {
    dataframe::Series s = strs({"a"});
    const auto r = s.sum();
    REQUIRE(!r.has_value());
    CHECK(names(r.error().c_str(), "sum", "string"));
}

TEST_CASE("sum of a timestamp column is refused, max still works") {
    dataframe::Series t = timestamps({30, 10, 20});
    const dftu_scalar r = dftu_series_reduce(t.handle(), DFTU_REDUCE_SUM);
    CHECK(names(error_text(r), "sum", "timestamp"));
    dftu_utils_test::expect_error(
        t.sum(), dftracer::utils::common::ErrorCode::InvalidArgument);
    CHECK(dftu_utils_test::ok(t.max()).i64() == 30);
    CHECK(dftu_utils_test::ok(t.min()).i64() == 10);
}

TEST_CASE("a code that is not sum, min or max is refused") {
    dataframe::Series x = i64({1, 2, 3});
    for (dftu_reduce_op op : {DFTU_REDUCE_COUNT, DFTU_REDUCE_MEAN}) {
        const dftu_scalar r = dftu_series_reduce(x.handle(), op);
        CHECK(r.kind == DFTU_SCALAR_TAG_ERR);
    }
}

TEST_CASE("numeric reductions and an empty column are unchanged") {
    dataframe::Series x = i64({3, 1, 4});
    CHECK(dftu_utils_test::ok(x.sum()).i64() == 8);
    CHECK(dftu_utils_test::ok(x.min()).i64() == 1);
    CHECK(dftu_utils_test::ok(x.max()).i64() == 4);
    CHECK(dftu_utils_test::ok(x.mean()) == doctest::Approx(8.0 / 3.0));

    dataframe::Series none = i64({});
    CHECK(dftu_utils_test::ok(none.sum()).i64() == 0);
    CHECK(dftu_utils_test::ok(none.mean()) == 0.0);
}
