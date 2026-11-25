// SPDX-License-Identifier: MIT
// concat joins LargeList, FixedSizeList and Map columns natively: the offsets
// are rebased, the children joined, the nulls kept, and FixedSizeList parts of
// two sizes fail.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dataframe/builders_types.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <span_list.h>
#include <task_result.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;
using namespace dftracer::utils::dataframe::test_types;

namespace {

using Rows = dstd::Vector<dstd::Vector<std::int64_t>>;

dstd::Result<Series> join(const Series& a, const Series& b) {
    return dataframe::concat_columns(
        dftu_utils_test::span_of<const Series*>({&a, &b}));
}

template <class Off>
dstd::Vector<Off> offsets(const Series& s) {
    const auto* o = reinterpret_cast<const Off*>(s.handle()->offsets.data());
    return dstd::Vector<Off>(o, o + s.length() + 1);
}

dstd::Vector<std::int64_t> ints(const Series& s) {
    const Series flat = s.materialize();
    const auto* v = flat.data<std::int64_t>();
    return dstd::Vector<std::int64_t>(v, v + flat.length());
}

}  // namespace

TEST_SUITE("concat nested types") {
    TEST_CASE("two large_list parts join with rebased offsets") {
        const Rows a_rows{{1, 2}, {}};
        const Rows b_rows{{3}, {4, 5, 6}};
        Series a = make_large_list_i64(a_rows);
        Series b = make_large_list_i64(b_rows);
        Series out = dftu_utils_test::ok(join(a, b));
        REQUIRE(out.type() == TypeId::LargeList);
        CHECK(out.length() == 4);
        CHECK(offsets<std::int64_t>(out) ==
              dstd::Vector<std::int64_t>{0, 2, 2, 3, 6});
        CHECK(ints(out.child(0)) ==
              dstd::Vector<std::int64_t>{1, 2, 3, 4, 5, 6});
    }

    TEST_CASE("a part sliced into its child reads only its own rows") {
        Series child = make_fixed<std::int64_t>(
            TypeId::Int64, dstd::Vector<std::int64_t>{9, 1, 2, 9});
        const dstd::Vector<std::int64_t> off{1, 3};
        Series a = make_large_list(off, std::move(child));
        const Rows b_rows{{7}};
        Series b = make_large_list_i64(b_rows);
        Series out = dftu_utils_test::ok(join(a, b));
        CHECK(offsets<std::int64_t>(out) ==
              dstd::Vector<std::int64_t>{0, 2, 3});
        CHECK(ints(out.child(0)) == dstd::Vector<std::int64_t>{1, 2, 7});
    }

    TEST_CASE("fixed_size_list parts join, and two sizes fail") {
        const Rows a_rows{{1, 2}, {3, 4}};
        const Rows b_rows{{5, 6}};
        const Rows c_rows{{7, 8, 9}};
        Series a = make_fixed_size_list_i64(a_rows);
        Series b = make_fixed_size_list_i64(b_rows);
        Series out = dftu_utils_test::ok(join(a, b));
        REQUIRE(out.type() == TypeId::FixedSizeList);
        CHECK(out.length() == 3);
        CHECK(out.handle()->fixed_size() == 2);
        CHECK(ints(out.child(0)) ==
              dstd::Vector<std::int64_t>{1, 2, 3, 4, 5, 6});
        Series c = make_fixed_size_list_i64(c_rows);
        CHECK_FALSE(join(a, c).has_value());
    }

    TEST_CASE("map parts join their entries and keep their nulls") {
        using Entry = dstd::Pair<dstd::String, std::int64_t>;
        const dstd::Vector<dstd::Vector<Entry>> a_rows{{{"a", 1}}, {}};
        const dstd::Vector<dstd::Vector<Entry>> b_rows{{{"b", 2}, {"c", 3}}};
        Series a = make_map_string_i64(a_rows);
        Series b = make_map_string_i64(b_rows);
        // the second row of a is null
        a.handle()->validity = new_buffer(1);
        writable(a.handle()->validity)[0] = 0x01;
        a.handle()->null_count = 1;
        Series out = dftu_utils_test::ok(join(a, b));
        REQUIRE(out.type() == TypeId::Map);
        CHECK(out.length() == 3);
        CHECK(out.null_count() == 1);
        CHECK_FALSE(out.is_null(0));
        CHECK(out.is_null(1));
        CHECK_FALSE(out.is_null(2));
        CHECK(offsets<std::int32_t>(out) ==
              dstd::Vector<std::int32_t>{0, 1, 1, 3});
        CHECK(out.child(0).length() == 3);
    }

    TEST_CASE("joined map keys keep their string type, not a view") {
        using Entry = dstd::Pair<dstd::String, std::int64_t>;
        const dstd::Vector<dstd::Vector<Entry>> a_rows{{{"a", 1}}};
        const dstd::Vector<dstd::Vector<Entry>> b_rows{{{"b", 2}}};
        Series out = dftu_utils_test::ok(
            join(make_map_string_i64(a_rows), make_map_string_i64(b_rows)));
        Series keys = out.child(0).child(0);
        CHECK(keys.type() == TypeId::String);
        CHECK(keys.encoding() == Encoding::Flat);
    }

    TEST_CASE("nested parts of one whole type stay chunks") {
        const Rows a_rows{{1, 2}, {3}};
        const Rows b_rows{{4}};
        dstd::Vector<Series> parts;
        parts.push_back(make_large_list_i64(a_rows));
        parts.push_back(make_large_list_i64(b_rows));
        Series out = dftu_utils_test::ok(chunked_column(std::move(parts)));
        CHECK(out.encoding() == Encoding::Chunked);
        CHECK(out.length() == 3);
        CHECK(out.data_type() == make_large_list_i64(a_rows).data_type());
    }

    TEST_CASE("fixed_size_list parts of two sizes go to concat, which fails") {
        dstd::Vector<Series> parts;
        parts.push_back(make_fixed_size_list_i64(Rows{{1, 2}}));
        parts.push_back(make_fixed_size_list_i64(Rows{{3, 4}}));
        Series same = dftu_utils_test::ok(chunked_column(std::move(parts)));
        CHECK(same.encoding() == Encoding::Chunked);
        dstd::Vector<Series> mixed;
        mixed.push_back(make_fixed_size_list_i64(Rows{{1, 2}}));
        mixed.push_back(make_fixed_size_list_i64(Rows{{3, 4, 5}}));
        CHECK_FALSE(chunked_column(std::move(mixed)).has_value());
    }
}
