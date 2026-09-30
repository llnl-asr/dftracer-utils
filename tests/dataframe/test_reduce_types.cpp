#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using dftracer::utils::DFTUtilsException;
using dftracer::utils::dataframe::Agg;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::GroupAgg;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

namespace {

Series i64(std::vector<std::int64_t> v) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
}

// A Bool column; `valid` (when non-empty) marks the non-null slots.
Series bools(const std::vector<bool>& v, const std::vector<bool>& valid = {}) {
    const std::size_t nbytes = (v.size() + 7) / 8;
    std::vector<std::uint8_t> bits(nbytes, 0);
    std::vector<std::uint8_t> vbits(nbytes, 0);
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i]) bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        if (valid.empty() || valid[i])
            vbits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
    return Series::flat(TypeId::Bool, bits.data(),
                        static_cast<std::int64_t>(v.size()),
                        valid.empty() ? nullptr : vbits.data());
}

Series strs(const std::vector<std::string_view>& v,
            const std::vector<bool>& valid = {}) {
    std::vector<std::uint8_t> vbits((v.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < v.size(); ++i)
        if (valid.empty() || valid[i])
            vbits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return Series::strings(std::span<const std::string_view>(v),
                           valid.empty() ? nullptr : vbits.data());
}

Series timestamps(const std::vector<std::int64_t>& v) {
    return Series::flat(TypeId::Timestamp, v.data(),
                        static_cast<std::int64_t>(v.size()));
}

std::string error_text(dftu_scalar s) {
    REQUIRE(s.kind == DFTU_SCALAR_TAG_ERR);
    REQUIRE(s.value.err != nullptr);
    REQUIRE(s.value.err->message != nullptr);
    return s.value.err->message;
}

bool names(const std::string& text, const char* a, const char* b) {
    return text.find(a) != std::string::npos &&
           text.find(b) != std::string::npos;
}

}  // namespace

TEST_CASE("a bool column counts true values") {
    Series flag = bools({true, false, true, true});
    CHECK(flag.sum().i64() == 3);
    CHECK(flag.mean() == doctest::Approx(0.75));
    CHECK(flag.min().i64() == 0);
    CHECK(flag.max().i64() == 1);

    // Nulls are skipped, as for numbers.
    Series with_null = bools({true, true, false}, {true, false, true});
    CHECK(with_null.sum().i64() == 1);
    CHECK(with_null.mean() == doctest::Approx(0.5));
}

TEST_CASE("a comparison mask reduces the same way") {
    Series x = i64({1, 2, 3, 4});
    Series mask = x > 2;
    CHECK(mask.sum().i64() == 2);
}

TEST_CASE("a bool reduction agrees with the group-by sum") {
    const std::int64_t expected = bools({true, false, true, true}).sum().i64();
    DataFrame df;
    df.names = {"k", "flag"};
    df.columns.push_back(i64({7, 7, 7, 7}));
    df.columns.push_back(bools({true, false, true, true}));
    DataFrame g = df.group_by("k", {GroupAgg{Agg::Sum, "flag", "total"}});
    REQUIRE(g.num_rows() == 1);
    CHECK(g.column("total").data<std::int64_t>()[0] == expected);
}

TEST_CASE("min and max of a string column are the bytewise extremes") {
    Series s = strs({"b", "a", "d", "c"});
    CHECK(s.min().str() == "a");
    CHECK(s.max().str() == "d");

    Series with_null = strs({"zz", "a", "q"}, {true, false, true});
    CHECK(with_null.min().str() == "q");
    CHECK(with_null.max().str() == "zz");

    Series none = strs({"x", "y"}, {false, false});
    CHECK(none.min().str().empty());
    CHECK(none.max().str().empty());
}

TEST_CASE("sum and mean of a string column are refused") {
    Series s = strs({"a", "b"});
    const dftu_scalar r = dftu_series_reduce(s.handle(), DFTU_REDUCE_SUM);
    CHECK(names(error_text(r), "sum", "string"));
    CHECK_THROWS_AS((void)s.sum(), DFTUtilsException);
    CHECK_THROWS_AS((void)s.mean(), DFTUtilsException);

    // A string column with no valid value is still refused for sum and mean.
    Series none = strs({"x"}, {false});
    CHECK_THROWS_AS((void)none.sum(), DFTUtilsException);
    CHECK_THROWS_AS((void)none.mean(), DFTUtilsException);
}

TEST_CASE("the refusal names the reduction and the type") {
    Series s = strs({"a"});
    try {
        (void)s.sum();
        FAIL("sum of a string column must throw");
    } catch (const DFTUtilsException& e) {
        CHECK(names(e.what(), "sum", "string"));
    }
}

TEST_CASE("sum of a timestamp column is refused, max still works") {
    Series t = timestamps({30, 10, 20});
    const dftu_scalar r = dftu_series_reduce(t.handle(), DFTU_REDUCE_SUM);
    CHECK(names(error_text(r), "sum", "timestamp"));
    CHECK_THROWS_AS((void)t.sum(), DFTUtilsException);
    CHECK(t.max().i64() == 30);
    CHECK(t.min().i64() == 10);
}

TEST_CASE("a code that is not sum, min or max is refused") {
    Series x = i64({1, 2, 3});
    for (dftu_reduce_op op : {DFTU_REDUCE_COUNT, DFTU_REDUCE_MEAN}) {
        const dftu_scalar r = dftu_series_reduce(x.handle(), op);
        CHECK(r.kind == DFTU_SCALAR_TAG_ERR);
    }
}

TEST_CASE("numeric reductions and an empty column are unchanged") {
    Series x = i64({3, 1, 4});
    CHECK(x.sum().i64() == 8);
    CHECK(x.min().i64() == 1);
    CHECK(x.max().i64() == 4);
    CHECK(x.mean() == doctest::Approx(8.0 / 3.0));

    Series none = i64({});
    CHECK(none.sum().i64() == 0);
    CHECK(none.mean() == 0.0);
}
