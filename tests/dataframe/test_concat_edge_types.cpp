// SPDX-License-Identifier: MIT
// Edge types of concat and where: a diagonal concat widens binary and refuses
// types that have no common one, a join of dictionaries keeps its value type
// and refuses a code out of range, and where refuses operands whose type
// parameters differ.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dataframe/builders_types.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/reshape.h>
#include <dftracer/utils/dataframe/kernels/dictionary.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <span_list.h>
#include <task_result.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;
using namespace dftracer::utils::dataframe::test_types;

namespace {

using Opt = dstd::Optional<dstd::String>;

DataFrame frame_of(Series column) {
    DataFrame frame;
    frame.names = dstd::make_shared_strings({"x"});
    frame.columns.push_back(std::move(column));
    return frame;
}

dstd::Result<DataFrame> diagonal(const DataFrame& a, const DataFrame& b) {
    const DataFrame* parts[2] = {&a, &b};
    return concat(dstd::Span<const DataFrame* const>(parts, 2),
                  ConcatHow::Diagonal);
}

dstd::Result<Series> joined(const Series& a, const Series& b) {
    const Series* parts[2] = {&a, &b};
    return concat_columns(dstd::Span<const Series* const>(parts, 2));
}

Series codes_of(dstd::Vector<std::int32_t> codes) {
    return Series::flat(TypeId::Int32, codes.data(),
                        static_cast<std::int64_t>(codes.size()));
}

}  // namespace

TEST_SUITE("concat and where edge types") {
    TEST_CASE("a diagonal concat of binary and large_binary keeps the bytes") {
        DataFrame a = frame_of(make_binary(Cells{dstd::String("ab"), Opt{}}));
        DataFrame b = frame_of(make_large_binary(Cells{dstd::String("cd")}));
        for (bool narrow_first : {true, false}) {
            DataFrame out = dftu_utils_test::ok(narrow_first ? diagonal(a, b)
                                                             : diagonal(b, a));
            const Series col = out.columns[0].materialize();
            REQUIRE(col.length() == 3);
            CHECK(col.null_count() == 1);
            const std::int64_t at_ab = narrow_first ? 0 : 1;
            const std::int64_t at_cd = narrow_first ? 2 : 0;
            CHECK(col.string_at(at_ab) == "ab");
            CHECK(col.string_at(at_cd) == "cd");
            CHECK(col.is_null(narrow_first ? 1 : 2));
        }
    }

    TEST_CASE(
        "a diagonal concat of fixed_size_binary and int64 is a type error") {
        DataFrame a = frame_of(make_fixed_size_binary(
            dstd::Vector<dstd::String>{"abcd", "efgh"}, 4));
        const dstd::Vector<std::int64_t> values{1, 2};
        DataFrame b = frame_of(Series::flat_i64(values.data(), 2));
        for (bool fixed_first : {true, false}) {
            const auto out = fixed_first ? diagonal(a, b) : diagonal(b, a);
            REQUIRE_FALSE(out.has_value());
            CHECK(dstd::String(out.error().c_str()).find("incompatible") !=
                  dstd::String::npos);
        }
    }

    TEST_CASE("dictionaries with no value join into a null dictionary") {
        Series a = dftu_utils_test::ok(
            dictionary_of(codes_of({0, 0}), make_large_utf8(Cells{Opt{}})));
        Series b = dftu_utils_test::ok(
            dictionary_of(codes_of({0}), make_large_utf8(Cells{Opt{}})));
        Series out = dftu_utils_test::ok(joined(a, b));
        REQUIRE(out.valid());
        CHECK(out.encoding() == Encoding::Dictionary);
        CHECK(out.type() == TypeId::LargeString);
        REQUIRE(out.length() == 3);
        CHECK(out.null_count() == 3);
        CHECK(out.handle()->child()->length == 1);
    }

    TEST_CASE("a valid row over an empty dictionary is an error") {
        Series values = make_string(Cells{});
        Series bad = dictionary_from_codes(dstd::Vector<std::int32_t>{0},
                                           std::move(values), nullptr);
        Series good = dftu_utils_test::ok(dictionary_of(
            codes_of({0}), make_string(Cells{dstd::String("x")})));
        const auto out = joined(bad, good);
        REQUIRE_FALSE(out.has_value());
        CHECK(dstd::String(out.error().c_str()).find("out of range") !=
              dstd::String::npos);
    }

    TEST_CASE("a code out of range in a dictionary join is an error") {
        Series bad = dictionary_from_codes(
            dstd::Vector<std::int32_t>{0, 5},
            make_string(Cells{dstd::String("x")}), nullptr);
        Series good = dftu_utils_test::ok(dictionary_of(
            codes_of({0}), make_string(Cells{dstd::String("x")})));
        for (bool bad_first : {true, false}) {
            const auto out = bad_first ? joined(bad, good) : joined(good, bad);
            REQUIRE_FALSE(out.has_value());
            CHECK(dstd::String(out.error().c_str()).find("out of range") !=
                  dstd::String::npos);
        }
    }

    TEST_CASE("a null column of no rows has no validity bitmap") {
        const Series like = make_string(Cells{dstd::String("x")});
        const Series none = null_column_like(like, 0);
        REQUIRE(none.valid());
        CHECK(none.length() == 0);
        CHECK_FALSE(static_cast<bool>(none.handle()->validity.block()));
        const Series list = null_column_like(
            Series::list(dstd::Vector<std::int32_t>{0, 1},
                         make_string(Cells{dstd::String("x")})),
            0);
        REQUIRE(list.valid());
        CHECK_FALSE(static_cast<bool>(list.handle()->validity.block()));
    }

    TEST_CASE("where refuses fixed_size_binary operands of different widths") {
        Series wide = make_fixed_size_binary(
            dstd::Vector<dstd::String>{"abcd", "efgh"}, 4);
        Series narrow =
            make_fixed_size_binary(dstd::Vector<dstd::String>{"ab", "cd"}, 2);
        const std::uint8_t bits = 0b01;
        const Series mask = Series::flat(TypeId::Bool, &bits, 2);
        CHECK_FALSE(wide.where(mask, narrow).valid());
        Series same = make_fixed_size_binary(
            dstd::Vector<dstd::String>{"wxyz", "0123"}, 4);
        const Series picked = wide.where(mask, same);
        REQUIRE(picked.valid());
        CHECK(std::memcmp(picked.data<char>(), "abcd0123", 8) == 0);
    }
}
