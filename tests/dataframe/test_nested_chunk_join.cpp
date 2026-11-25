// SPDX-License-Identifier: MIT
// Parts of a nested column stay chunks only when their real nested types are
// equal and every chunk is FLAT; the join takes binary and fixed-size binary
// children, keeps dictionaries, refuses int32 offsets that wrap; and a null
// column follows the nested type of its model.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dataframe/builders_types.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/reshape.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/pair.h>
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

using Entry = dstd::Pair<dstd::String, std::int64_t>;
using Opt = dstd::Optional<dstd::String>;

Series ints(dstd::Vector<std::int64_t> v) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
}

Series mask(std::uint8_t bits, std::int64_t n) {
    return Series::flat(TypeId::Bool, &bits, n);
}

Series struct_of(dstd::CStringView name, Series field) {
    dstd::Vector<Series> fields;
    fields.push_back(std::move(field));
    return Series::structs({name.c_str()}, std::move(fields));
}

Series list_of(dstd::Vector<std::int32_t> offsets, Series child) {
    return Series::list(offsets, std::move(child));
}

dstd::Result<Series> join(const Series& a, const Series& b) {
    return concat_columns(dftu_utils_test::span_of<const Series*>({&a, &b}));
}

dstd::Result<Series> chunked_of(Series a, Series b) {
    dstd::Vector<Series> parts;
    parts.push_back(std::move(a));
    parts.push_back(std::move(b));
    return chunked_column(std::move(parts));
}

dstd::Vector<std::int64_t> int_values(const Series& s) {
    const Series flat = s.materialize();
    const auto* v = flat.data<std::int64_t>();
    return dstd::Vector<std::int64_t>(v, v + flat.length());
}

Series codes_of(dstd::Vector<std::int32_t> codes) {
    return Series::flat(TypeId::Int32, codes.data(),
                        static_cast<std::int64_t>(codes.size()));
}

Series fsb4(dstd::Vector<dstd::String> values) {
    return make_fixed_size_binary(values, 4);
}

}  // namespace

TEST_SUITE("nested chunk joins") {
    TEST_CASE("is_nested names every nested type and no other") {
        static_assert(is_nested(TypeId::Struct));
        static_assert(is_nested(TypeId::List));
        static_assert(is_nested(TypeId::LargeList));
        static_assert(is_nested(TypeId::FixedSizeList));
        static_assert(is_nested(TypeId::Map));
        static_assert(!is_nested(TypeId::Int64));
        static_assert(!is_nested(TypeId::String));
        static_assert(!is_nested(TypeId::FixedSizeBinary));
    }

    TEST_CASE("selection parts of one struct type become FLAT chunks") {
        Series a = struct_of("a", ints({1, 2, 3})).filter(mask(0b101, 3));
        Series b = struct_of("a", ints({4, 5})).filter(mask(0b10, 2));
        REQUIRE(a.encoding() == Encoding::Selection);
        REQUIRE(b.encoding() == Encoding::Selection);
        Series out =
            dftu_utils_test::ok(chunked_of(std::move(a), std::move(b)));
        REQUIRE(out.encoding() == Encoding::Chunked);
        CHECK(out.length() == 3);
        const dftu_series& h = *out.handle();
        REQUIRE(h.nested.size() == 2);
        for (const dftu_nested& chunk : h.nested)
            CHECK(chunk.series->encoding == Encoding::Flat);
        CHECK(int_values(out.materialize().child(0)) ==
              dstd::Vector<std::int64_t>{1, 3, 5});
    }

    TEST_CASE("selection parts of two list types stay chunks only if equal") {
        const dstd::Vector<std::int32_t> off{0, 1, 2, 3};
        Series a = list_of(off, ints({1, 2, 3})).filter(mask(0b011, 3));
        Series b =
            list_of(off,
                    Series::strings(dstd::Vector<dstd::String>{"x", "y", "z"}))
                .filter(mask(0b011, 3));
        REQUIRE(a.encoding() == Encoding::Selection);
        CHECK_FALSE(chunked_of(std::move(a), std::move(b)).has_value());
    }

    TEST_CASE("selection parts of two struct types join by field name") {
        Series a = struct_of("a", ints({1, 2})).filter(mask(0b11, 2));
        dstd::Vector<Series> fields;
        fields.push_back(Series::strings(dstd::Vector<dstd::String>{"p", "q"}));
        const dstd::Vector<double> reals{0.5, 1.5};
        fields.push_back(Series::flat(TypeId::Float64, reals.data(), 2));
        Series b = Series::structs({"b", "c"}, std::move(fields))
                       .filter(mask(0b11, 2));
        Series out =
            dftu_utils_test::ok(chunked_of(std::move(a), std::move(b)));
        CHECK(out.encoding() != Encoding::Chunked);
        REQUIRE(out.num_children() == 3);
        CHECK(out.field_name(0) == "a");
        CHECK(out.field_name(1) == "b");
        CHECK(out.field_name(2) == "c");
        CHECK(out.length() == 4);
    }

    TEST_CASE("list parts with binary children join") {
        const dstd::Vector<std::int32_t> off_a{0, 2, 3};
        const dstd::Vector<std::int32_t> off_b{0, 1};
        Series a = list_of(off_a, make_binary(Cells{dstd::String("ab"), Opt{},
                                                    dstd::String("cde")}));
        Series b = list_of(off_b, make_binary(Cells{dstd::String("z")}));
        Series out = dftu_utils_test::ok(join(a, b));
        REQUIRE(out.type() == TypeId::List);
        CHECK(out.length() == 3);
        Series kids = out.child(0);
        REQUIRE(kids.type() == TypeId::Binary);
        REQUIRE(kids.length() == 4);
        CHECK(kids.string_at(0) == "ab");
        CHECK(kids.is_null(1));
        CHECK(kids.string_at(2) == "cde");
        CHECK(kids.string_at(3) == "z");
    }

    TEST_CASE("large_list parts with large_binary children join") {
        const dstd::Vector<std::int64_t> off{0, 1, 2};
        Series a = make_large_list(
            off, make_large_binary(Cells{dstd::String("aa"), Opt{}}));
        Series b = make_large_list(
            off, make_large_binary(Cells{dstd::String("bb"), Opt{}}));
        Series out = dftu_utils_test::ok(join(a, b));
        REQUIRE(out.type() == TypeId::LargeList);
        Series kids = out.child(0);
        REQUIRE(kids.type() == TypeId::LargeBinary);
        REQUIRE(kids.length() == 4);
        CHECK(kids.string_at(0) == "aa");
        CHECK(kids.is_null(1));
        CHECK(kids.string_at(2) == "bb");
        CHECK(kids.is_null(3));
    }

    TEST_CASE("list parts with fixed_size_binary children join") {
        const dstd::Vector<std::int32_t> off{0, 2};
        Series a = list_of(off, fsb4({"abcd", "efgh"}));
        Series b = list_of(off, fsb4({"wxyz", "0123"}));
        Series out = dftu_utils_test::ok(join(a, b));
        REQUIRE(out.type() == TypeId::List);
        Series kids = out.child(0);
        REQUIRE(kids.type() == TypeId::FixedSizeBinary);
        CHECK(kids.handle()->fixed_size() == 4);
        REQUIRE(kids.length() == 4);
        CHECK(std::memcmp(kids.data<char>(), "abcdefghwxyz0123", 16) == 0);
    }

    TEST_CASE("fixed_size_binary parts join, and two widths fail") {
        Series a = fsb4({"abcd", "efgh"});
        Series b = fsb4({"wxyz"});
        const std::uint8_t bits = 0b0;
        set_validity(b.handle(), &bits, 1);
        Series out = dftu_utils_test::ok(join(a, b));
        REQUIRE(out.type() == TypeId::FixedSizeBinary);
        CHECK(out.handle()->fixed_size() == 4);
        REQUIRE(out.length() == 3);
        CHECK(out.null_count() == 1);
        CHECK(out.is_null(2));
        CHECK(std::memcmp(out.data<char>(), "abcdefgh", 8) == 0);
        Series c = make_fixed_size_binary(dstd::Vector<dstd::String>{"abc"}, 3);
        CHECK_FALSE(join(a, c).has_value());
    }

    TEST_CASE("null_column_like follows the type of a selection over a list") {
        const dstd::Vector<std::int32_t> off{0, 2, 3, 3};
        Series list = list_of(off, ints({1, 2, 3}));
        Series sel = list.filter(mask(0b101, 3));
        REQUIRE(sel.encoding() == Encoding::Selection);
        Series nulls = null_column_like(sel, 2);
        REQUIRE(nulls.valid());
        CHECK(nulls.data_type() == list.data_type());
        CHECK(nulls.length() == 2);
        CHECK(nulls.null_count() == 2);
    }

    TEST_CASE(
        "null_column_like follows the type of a selection over a struct") {
        Series st = struct_of("a", ints({1, 2, 3}));
        Series sel = st.filter(mask(0b101, 3));
        REQUIRE(sel.encoding() == Encoding::Selection);
        Series nulls = null_column_like(sel, 2);
        REQUIRE(nulls.valid());
        CHECK(nulls.data_type() == st.data_type());
        CHECK(nulls.length() == 2);
        CHECK(nulls.null_count() == 2);
    }

    TEST_CASE("null_column_like follows the type of a selection over a map") {
        const dstd::Vector<dstd::Vector<Entry>> rows{
            {{"a", 1}}, {{"b", 2}, {"c", 3}}, {}};
        Series map = make_map_string_i64(rows);
        Series sel = map.filter(mask(0b011, 3));
        REQUIRE(sel.encoding() == Encoding::Selection);
        Series nulls = null_column_like(sel, 2);
        REQUIRE(nulls.valid());
        CHECK(nulls.type() == TypeId::Map);
        CHECK(nulls.data_type() == map.data_type());
        CHECK(nulls.null_count() == 2);
    }

    TEST_CASE("null_column_like keeps the width of a fixed_size_binary") {
        Series fsb = fsb4({"abcd", "efgh"});
        Series nulls = null_column_like(fsb, 3);
        REQUIRE(nulls.valid());
        CHECK(nulls.type() == TypeId::FixedSizeBinary);
        CHECK(nulls.handle()->fixed_size() == 4);
        CHECK(nulls.length() == 3);
        CHECK(nulls.null_count() == 3);
        const dstd::Vector<std::int32_t> off{0, 2};
        Series list = list_of(off, fsb4({"abcd", "efgh"}));
        Series list_nulls = null_column_like(list, 2);
        REQUIRE(list_nulls.valid());
        CHECK(list_nulls.data_type() == list.data_type());
    }

    TEST_CASE("null_column_like of a dictionary keeps the text type") {
        Series dict = dftu_utils_test::ok(dictionary_of(
            codes_of({0, 1}),
            make_large_utf8(Cells{dstd::String("x"), dstd::String("y")})));
        Series nulls = null_column_like(dict, 2);
        REQUIRE(nulls.valid());
        CHECK(nulls.type() == TypeId::LargeString);
        CHECK(nulls.null_count() == 2);
    }

    TEST_CASE("dictionary parts over large_string stay one dictionary") {
        Series a = dftu_utils_test::ok(dictionary_of(
            codes_of({0, 1, 0}),
            make_large_utf8(Cells{dstd::String("x"), dstd::String("y")})));
        Series b = dftu_utils_test::ok(dictionary_of(
            codes_of({1, 0}),
            make_large_utf8(Cells{dstd::String("y"), dstd::String("z")})));
        REQUIRE(a.type() == TypeId::LargeString);
        Series out = dftu_utils_test::ok(join(a, b));
        CHECK(out.encoding() == Encoding::Dictionary);
        CHECK(out.type() == TypeId::LargeString);
        REQUIRE(out.length() == 5);
        const char* want[] = {"x", "y", "x", "z", "y"};
        for (std::int64_t i = 0; i < 5; ++i) CHECK(out.string_at(i) == want[i]);
        REQUIRE(out.handle()->child());
        CHECK(out.handle()->child()->length == 3);
    }

    TEST_CASE("dictionary parts with null entries stay one dictionary") {
        Series a = dftu_utils_test::ok(dictionary_of(
            codes_of({0, 1, 0}), make_string(Cells{dstd::String("p"), Opt{}})));
        Series b = dftu_utils_test::ok(dictionary_of(
            codes_of({0, 1}), make_string(Cells{dstd::String("q"), Opt{}})));
        Series out = dftu_utils_test::ok(join(a, b));
        CHECK(out.encoding() == Encoding::Dictionary);
        REQUIRE(out.length() == 5);
        CHECK(out.string_at(0) == "p");
        CHECK(out.is_null(1));
        CHECK(out.string_at(2) == "p");
        CHECK(out.string_at(3) == "q");
        CHECK(out.is_null(4));
    }

    TEST_CASE("list parts with dictionary children keep one dictionary") {
        const dstd::Vector<std::int32_t> off{0, 2};
        Series a =
            list_of(off, dftu_utils_test::ok(dictionary_of(
                             codes_of({0, 1}),
                             make_string(Cells{dstd::String("p"), Opt{}}))));
        Series b = list_of(
            off, dftu_utils_test::ok(dictionary_of(
                     codes_of({0, 0}), make_string(Cells{dstd::String("q")}))));
        Series out = dftu_utils_test::ok(join(a, b));
        Series kids = out.child(0);
        CHECK(kids.encoding() == Encoding::Dictionary);
        REQUIRE(kids.length() == 4);
        CHECK(kids.string_at(0) == "p");
        CHECK(kids.is_null(1));
        CHECK(kids.string_at(2) == "q");
        CHECK(kids.string_at(3) == "q");
    }

    TEST_CASE("list offsets past int32 fail in the join") {
        const dstd::Vector<std::int32_t> off{0, 1500000000};
        Series a = list_of(off, ints({1}));
        Series b = list_of(off, ints({2}));
        const auto out = join(a, b);
        REQUIRE_FALSE(out.has_value());
        CHECK(dstd::String(out.error().c_str()).find("offsets") !=
              dstd::String::npos);
    }

    TEST_CASE("map offsets past int32 fail in the join") {
        const dstd::Vector<dstd::Vector<Entry>> rows{{{"a", 1}}};
        Series a = make_map_string_i64(rows);
        Series b = make_map_string_i64(rows);
        reinterpret_cast<std::int32_t*>(writable(a.handle()->offsets))[1] =
            1500000000;
        reinterpret_cast<std::int32_t*>(writable(b.handle()->offsets))[1] =
            1500000000;
        const auto out = join(a, b);
        REQUIRE_FALSE(out.has_value());
        CHECK(dstd::String(out.error().c_str()).find("offsets") !=
              dstd::String::npos);
    }

    TEST_CASE("a diagonal concat fills an all-null part next to a list") {
        DataFrame nulls;
        nulls.names = dstd::make_shared_strings({"x"});
        nulls.columns.push_back(Series::nulls(TypeId::Int64, 2));
        DataFrame lists;
        lists.names = dstd::make_shared_strings({"x"});
        const dstd::Vector<std::int32_t> off{0, 1, 2};
        lists.columns.push_back(list_of(off, ints({7, 8})));
        for (bool nulls_first : {true, false}) {
            const DataFrame* parts[2] = {nulls_first ? &nulls : &lists,
                                         nulls_first ? &lists : &nulls};
            DataFrame out = dftu_utils_test::ok(
                concat(dstd::Span<const DataFrame* const>(parts, 2),
                       ConcatHow::Diagonal));
            REQUIRE(out.num_rows() == 4);
            const Series& x = out.columns[0];
            CHECK(x.type() == TypeId::List);
            CHECK(x.null_count() == 2);
            CHECK(x.is_null(nulls_first ? 0 : 2));
            CHECK(x.is_null(nulls_first ? 1 : 3));
            CHECK_FALSE(x.is_null(nulls_first ? 2 : 0));
        }
    }

    TEST_CASE("a diagonal concat fills an all-null part next to a struct") {
        DataFrame nulls;
        nulls.names = dstd::make_shared_strings({"x"});
        nulls.columns.push_back(Series::nulls(TypeId::Int64, 2));
        DataFrame structs;
        structs.names = dstd::make_shared_strings({"x"});
        structs.columns.push_back(struct_of("a", ints({7, 8})));
        const DataFrame* parts[2] = {&nulls, &structs};
        DataFrame out = dftu_utils_test::ok(concat(
            dstd::Span<const DataFrame* const>(parts, 2), ConcatHow::Diagonal));
        const Series& x = out.columns[0];
        CHECK(x.type() == TypeId::Struct);
        CHECK(x.num_children() == 1);
        CHECK(x.null_count() == 2);
    }

    TEST_CASE("a diagonal concat of a nested type and a scalar is an error") {
        const dstd::Vector<std::int64_t> off{0, 1, 2};
        DataFrame scalar;
        scalar.names = dstd::make_shared_strings({"x"});
        scalar.columns.push_back(ints({1, 2}));
        dstd::Vector<Series> nested;
        nested.push_back(make_large_list(off, ints({1, 2})));
        nested.push_back(make_fixed_size_list_i64(
            dstd::Vector<dstd::Vector<std::int64_t>>{{1, 2}, {3, 4}}));
        nested.push_back(make_map_string_i64(
            dstd::Vector<dstd::Vector<Entry>>{{{"a", 1}}, {{"b", 2}}}));
        for (Series& column : nested) {
            DataFrame other;
            other.names = dstd::make_shared_strings({"x"});
            other.columns.push_back(std::move(column));
            const DataFrame* parts[2] = {&scalar, &other};
            const auto out =
                concat(dstd::Span<const DataFrame* const>(parts, 2),
                       ConcatHow::Diagonal);
            REQUIRE_FALSE(out.has_value());
            CHECK(dstd::String(out.error().c_str()).find("incompatible") !=
                  dstd::String::npos);
        }
    }

    TEST_CASE("where on structs refuses other field names or types") {
        Series a = struct_of("a", ints({1, 2}));
        Series same = struct_of("a", ints({10, 20}));
        Series renamed = struct_of("b", ints({10, 20}));
        Series retyped = struct_of(
            "a", Series::strings(dstd::Vector<dstd::String>{"x", "y"}));
        Series picked = a.where(mask(0b01, 2), same);
        REQUIRE(picked.valid());
        CHECK(int_values(picked.child(0)) == dstd::Vector<std::int64_t>{1, 20});
        CHECK_FALSE(a.where(mask(0b01, 2), renamed).valid());
        CHECK_FALSE(a.where(mask(0b01, 2), retyped).valid());
    }

    TEST_CASE("a dictionary over no non-flat values stays a dictionary") {
        const dstd::Vector<std::int32_t> raw{0, 0};
        Series codes = Series::flat(TypeId::Int32, raw.data(), 2);
        codes.handle()->validity = new_buffer(1);
        writable(codes.handle()->validity)[0] = 0;
        codes.handle()->null_count = 2;
        Series values = Series::strings(dstd::Vector<dstd::String>{"a", "b"})
                            .filter(mask(0b00, 2));
        REQUIRE(values.encoding() == Encoding::Selection);
        REQUIRE(values.length() == 0);
        Series out = dftu_utils_test::ok(dictionary_of(codes, values));
        CHECK(out.encoding() == Encoding::Dictionary);
        CHECK(out.null_count() == 2);
    }

    TEST_CASE("a dictionary over non-flat values reads them") {
        Series values =
            Series::strings(dstd::Vector<dstd::String>{"a", "b", "c"})
                .filter(mask(0b101, 3));
        REQUIRE(values.encoding() == Encoding::Selection);
        Series out =
            dftu_utils_test::ok(dictionary_of(codes_of({1, 0, 1}), values));
        CHECK(out.encoding() == Encoding::Dictionary);
        CHECK(out.string_at(0) == "c");
        CHECK(out.string_at(1) == "a");
        CHECK(out.string_at(2) == "c");
    }
}
