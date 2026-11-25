// SPDX-License-Identifier: MIT
// take and where read FixedSizeList and Map columns natively, where keeps the
// text of a nested child in its own type, and a dictionary over no values
// stays a dictionary.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dataframe/builders_types.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/reshape.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;
using namespace dftracer::utils::dataframe::test_types;

namespace {

using Rows = dstd::Vector<dstd::Vector<std::int64_t>>;
using Entry = dstd::Pair<dstd::String, std::int64_t>;

Series take(const Series& s, dstd::Span<const std::int64_t> at) {
    return s.take(at);
}

Series mask(std::uint8_t bits, std::int64_t n) {
    return Series::flat(TypeId::Bool, &bits, n);
}

}  // namespace

TEST_SUITE("nested take and where") {
    TEST_CASE("take on a fixed_size_list reorders rows and nulls a -1 row") {
        const Rows rows{{1, 2}, {3, 4}, {5, 6}};
        Series s = make_fixed_size_list_i64(rows);
        const dstd::Vector<std::int64_t> at{2, 0, -1};
        Series out = take(s, at);
        REQUIRE(out.valid());
        REQUIRE(out.type() == TypeId::FixedSizeList);
        CHECK(out.handle()->fixed_size() == 2);
        CHECK(out.length() == 3);
        CHECK(out.is_null(2));
        Series child = out.child(0);
        REQUIRE(child.length() == 6);
        CHECK(child.data<std::int64_t>()[0] == 5);
        CHECK(child.data<std::int64_t>()[1] == 6);
        CHECK(child.data<std::int64_t>()[2] == 1);
        CHECK(child.data<std::int64_t>()[3] == 2);
    }

    TEST_CASE("take on a map reorders its entries") {
        const dstd::Vector<dstd::Vector<Entry>> rows{{{"a", 1}},
                                                     {{"b", 2}, {"c", 3}}};
        Series s = make_map_string_i64(rows);
        const dstd::Vector<std::int64_t> at{1, 0};
        Series out = take(s, at);
        REQUIRE(out.valid());
        REQUIRE(out.type() == TypeId::Map);
        const auto* off =
            reinterpret_cast<const std::int32_t*>(out.handle()->offsets.data());
        CHECK(off[0] == 0);
        CHECK(off[1] == 2);
        CHECK(off[2] == 3);
        CHECK(out.child(0).length() == 3);
    }

    TEST_CASE("where on a fixed_size_list takes the null arm where false") {
        const Rows rows{{1, 2}, {3, 4}};
        Series s = make_fixed_size_list_i64(rows);
        Series out = s.where(mask(0b01, 2), null_column_like(s, 2));
        REQUIRE(out.valid());
        CHECK(out.type() == TypeId::FixedSizeList);
        CHECK_FALSE(out.is_null(0));
        CHECK(out.is_null(1));
        CHECK(out.child(0).data<std::int64_t>()[0] == 1);
    }

    TEST_CASE("where on a map keeps its keys flat strings") {
        const dstd::Vector<dstd::Vector<Entry>> rows{{{"a", 1}}, {{"b", 2}}};
        Series s = make_map_string_i64(rows);
        Series out = s.where(mask(0b10, 2), null_column_like(s, 2));
        REQUIRE(out.valid());
        CHECK(out.type() == TypeId::Map);
        CHECK(out.is_null(0));
        CHECK_FALSE(out.is_null(1));
        Series keys = out.child(0).child(0);
        CHECK(keys.type() == TypeId::String);
        CHECK(keys.encoding() == Encoding::Flat);
        // the input keeps its own children
        CHECK(s.child(0).child(0).encoding() == Encoding::Flat);
    }

    TEST_CASE(
        "a dictionary over no values is all null and stays a dictionary") {
        const dstd::Vector<std::int32_t> raw{0, 0, 0};
        Series codes = Series::flat(TypeId::Int32, raw.data(), 3);
        codes.handle()->validity = new_buffer(1);
        writable(codes.handle()->validity)[0] = 0;
        codes.handle()->null_count = 3;
        Series values = Series::nulls(TypeId::String, 0);
        auto out = dictionary_of(codes, values);
        REQUIRE(out.has_value());
        CHECK(out->encoding() == Encoding::Dictionary);
        CHECK(out->type() == TypeId::String);
        CHECK(out->null_count() == 3);
        REQUIRE(out->handle()->child());
        CHECK(out->handle()->child()->length == 1);
    }
}
