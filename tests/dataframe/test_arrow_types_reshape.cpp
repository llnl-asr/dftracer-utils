#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Reshaping/dedupe-key coverage for the Arrow-only logical types:
// sort_by_multi, unique, to_dummies, pivot, unpivot and explode. Table-driven
// with a default-less switch, so a new TypeId with no row here fails.
#ifdef DFTRACER_UTILS_ENABLE_ARROW
// clang-format off
#include <nanoarrow/nanoarrow.h>
#include <dftracer/utils/dataframe/arrow.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/scalar.h>
// clang-format on

#include "builders_arrow_types.h"

using namespace dftracer::utils::dataframe;
using namespace dftracer::utils::dataframe::test_types;

namespace {

enum class Support {
    Computes,
    Refuses,
};

constexpr Support C = Support::Computes;
constexpr Support R = Support::Refuses;

enum Op {
    OP_SORT_MULTI,
    OP_UNIQUE,
    OP_TO_DUMMIES,
    OP_PIVOT,
    OP_UNPIVOT_CONCAT,
    OP_COUNT_,
};

struct TypeRow {
    TypeId id;
    Series (*build)();         // {third, first, second, first}: 3 distinct
    Series (*build_needle)();  // the value rows 1 and 3 share
    Support ops[OP_COUNT_];
    double expect_sum;         // sum of build(), for the unpivot concat check
};

Series build_float16() { return make_float16({3.0f, 1.0f, 2.0f, 1.0f}); }
Series build_decimal128() { return make_decimal128({300, 100, 200, 100}); }
Series build_decimal256() { return make_decimal256({300, 100, 200, 100}); }
Series build_fixed_size_binary() {
    return make_fixed_size_binary({"cccc", "aaaa", "bbbb", "aaaa"}, 4);
}
Series build_large_string() { return make_large_utf8({"c", "a", "b", "a"}); }
Series build_large_binary() { return make_large_binary({"c", "a", "b", "a"}); }
Series build_large_list() { return make_large_list_i64({{3}, {1}, {2}, {1}}); }
Series build_fixed_size_list() {
    return make_fixed_size_list_i64({{3, 3}, {1, 1}, {2, 2}, {1, 1}});
}
Series build_map() {
    return make_map_string_i64(
        {{{"c", 3}}, {{"a", 1}}, {{"b", 2}}, {{"a", 1}}});
}

Series needle_float16() { return make_float16({1.0f}); }
Series needle_decimal128() { return make_decimal128({100}); }
Series needle_decimal256() { return make_decimal256({100}); }
Series needle_fixed_size_binary() {
    return make_fixed_size_binary({"aaaa"}, 4);
}
Series needle_large_string() { return make_large_utf8({"a"}); }
Series needle_large_binary() { return make_large_binary({"a"}); }
Series needle_large_list() { return make_large_list_i64({{1}}); }
Series needle_fixed_size_list() { return make_fixed_size_list_i64({{1, 1}}); }
Series needle_map() { return make_map_string_i64({{{"a", 1}}}); }

const TypeRow TYPE_ROWS[] = {
    // id, build, needle, {sort_multi, unique, to_dummies, pivot,
    //   unpivot_concat}, expect_sum
    {TypeId::Float16, build_float16, needle_float16, {C, C, C, C, C}, 7.0},
    {TypeId::Decimal128,
     build_decimal128,
     needle_decimal128,
     {C, C, C, C, C},
     700.0},
    {TypeId::Decimal256,
     build_decimal256,
     needle_decimal256,
     {C, C, C, C, C},
     700.0},
    {TypeId::FixedSizeBinary,
     build_fixed_size_binary,
     needle_fixed_size_binary,
     {C, C, C, C, R},
     0.0},
    {TypeId::LargeString,
     build_large_string,
     needle_large_string,
     {C, C, C, C, R},
     0.0},
    {TypeId::LargeBinary,
     build_large_binary,
     needle_large_binary,
     {C, C, C, C, R},
     0.0},
    {TypeId::LargeList,
     build_large_list,
     needle_large_list,
     {R, R, R, R, R},
     0.0},
    {TypeId::FixedSizeList,
     build_fixed_size_list,
     needle_fixed_size_list,
     {R, R, R, R, R},
     0.0},
    {TypeId::Map, build_map, needle_map, {R, R, R, R, R}, 0.0},
};

/// Types this file owns. No default case, so a new TypeId forces a decision.
constexpr bool is_audited(TypeId t) {
    switch (t) {
        case TypeId::Float16:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
        case TypeId::FixedSizeBinary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Map:
            return true;
        case TypeId::Unknown:
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float32:
        case TypeId::Float64:
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::List:
        case TypeId::Struct:
        case TypeId::Date32:
        case TypeId::Date64:
        case TypeId::Time32:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
            return false;
    }
    return false;
}

DataFrame frame_of(const std::string& col_name, const Series& s) {
    DataFrame df;
    df.names = {col_name};
    df.columns.push_back(s.share());
    return df;
}

}  // namespace

TEST_SUITE("dataframe_arrow_types_reshape") {
    TEST_CASE("every audited TypeId has a matrix row") {
        for (std::int32_t code = 0;
             code <= static_cast<std::int32_t>(TypeId::Map); ++code) {
            const TypeId t = static_cast<TypeId>(code);
            if (!is_audited(t)) continue;
            bool found = false;
            for (const TypeRow& row : TYPE_ROWS)
                if (row.id == t) found = true;
            INFO("no TYPE_ROWS entry for ", type_name(t));
            CHECK(found);
        }
    }

    TEST_CASE("sort_by_multi orders by the real value, not a collapsed key") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            DataFrame df;
            df.names = {"k", "id"};
            df.columns.push_back(row.build().share());
            std::vector<std::int64_t> ids{0, 1, 2, 3};
            df.columns.push_back(Series::flat_i64(ids.data(), 4));

            if (row.ops[OP_SORT_MULTI] == Support::Refuses) {
                CHECK_THROWS_AS(df.sort_by_multi({"k"}, false),
                                std::invalid_argument);
                continue;
            }
            DataFrame sorted = df.sort_by_multi({"k"}, false);
            REQUIRE(sorted.num_rows() == 4);
            Series id_col = sorted.column("id");
            const std::int64_t* p = id_col.data<std::int64_t>();
            REQUIRE(p != nullptr);
            CHECK(p[0] == 1);
            CHECK(p[1] == 3);
            CHECK(p[2] == 2);
            CHECK(p[3] == 0);
        }
    }

    TEST_CASE("unique drops exactly the duplicate row, not more or fewer") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            DataFrame df = frame_of("k", row.build());
            if (row.ops[OP_UNIQUE] == Support::Refuses) {
                CHECK_THROWS_AS(df.unique(), std::invalid_argument);
                continue;
            }
            DataFrame u = df.unique();
            REQUIRE(u.num_rows() == 3);
            Series mask = u.column("k").is_in(row.build_needle());
            REQUIRE(mask.valid());
            const std::uint8_t* bits = mask.data<std::uint8_t>();
            REQUIRE(bits != nullptr);
            int hits = 0;
            for (std::int64_t i = 0; i < 3; ++i)
                hits += (bits[i >> 3] >> (i & 7)) & 1;
            CHECK(hits == 1);
        }
    }

    TEST_CASE("to_dummies names and bit-packs each distinct value") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            DataFrame df = frame_of("k", row.build());
            if (row.ops[OP_TO_DUMMIES] == Support::Refuses) {
                CHECK_THROWS_AS(df.to_dummies("k"), std::invalid_argument);
                continue;
            }
            DataFrame d = df.to_dummies("k");
            REQUIRE(d.num_columns() == 3);
            CHECK(d.names[0] != d.names[1]);
            CHECK(d.names[1] != d.names[2]);
            CHECK(d.names[0] != d.names[2]);
            // Ascending order: col 0 = "first" (rows 1,3), col 1 = "second"
            // (row 2), col 2 = "third" (row 0).
            const std::int8_t* c0 = d.column(d.names[0]).data<std::int8_t>();
            const std::int8_t* c1 = d.column(d.names[1]).data<std::int8_t>();
            const std::int8_t* c2 = d.column(d.names[2]).data<std::int8_t>();
            REQUIRE((c0 && c1 && c2));
            CHECK((c0[0] == 0 && c0[1] == 1 && c0[2] == 0 && c0[3] == 1));
            CHECK((c1[0] == 0 && c1[1] == 0 && c1[2] == 1 && c1[3] == 0));
            CHECK((c2[0] == 1 && c2[1] == 0 && c2[2] == 0 && c2[3] == 0));
        }
    }

    TEST_CASE("pivot keys distinct index values into distinct rows") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            DataFrame df;
            df.names = {"idx", "on", "val"};
            df.columns.push_back(row.build().share());
            std::vector<std::int64_t> on{0, 1, 0, 1};
            std::vector<std::int64_t> val{0, 1, 2, 3};
            df.columns.push_back(Series::flat_i64(on.data(), 4));
            df.columns.push_back(Series::flat_i64(val.data(), 4));

            if (row.ops[OP_PIVOT] == Support::Refuses) {
                CHECK_THROWS_AS(df.pivot("idx", "on", "val", "first"),
                                std::invalid_argument);
                continue;
            }
            DataFrame p = df.pivot("idx", "on", "val", "first");
            REQUIRE(p.num_rows() == 3);
            REQUIRE(p.num_columns() == 3);
            Series col0 = p.column("0");
            Series col1 = p.column("1");
            REQUIRE(col0.valid());
            REQUIRE(col1.valid());
            const std::int64_t* c0 = col0.data<std::int64_t>();
            const std::int64_t* c1 = col1.data<std::int64_t>();
            REQUIRE((c0 && c1));
            // Rows: {first, second, third}. (first, on=1) keeps the
            // first-seen value (1), not row 3's later duplicate-key value.
            CHECK(col0.is_null(0));
            CHECK(c1[0] == 1);
            CHECK_FALSE(col0.is_null(1));
            CHECK(c0[1] == 2);
            CHECK(col1.is_null(1));
            CHECK_FALSE(col0.is_null(2));
            CHECK(c0[2] == 0);
            CHECK(col1.is_null(2));
        }
    }

    TEST_CASE("unpivot concatenates value_vars without losing rows") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            DataFrame df;
            df.names = {"id", "v1", "v2"};
            std::vector<std::int64_t> id{0, 1, 2, 3};
            df.columns.push_back(Series::flat_i64(id.data(), 4));
            df.columns.push_back(row.build().share());
            df.columns.push_back(row.build().share());

            if (row.ops[OP_UNPIVOT_CONCAT] == Support::Refuses) {
                CHECK_THROWS_AS(df.unpivot({"id"}, {"v1", "v2"}),
                                std::invalid_argument);
                continue;
            }
            DataFrame m = df.unpivot({"id"}, {"v1", "v2"});
            REQUIRE(m.num_rows() == 8);
            Series value = m.column("value");
            REQUIRE(value.valid());
            CHECK(value.type() == row.id);
            CHECK(scalar_value<double>(value.sum()) ==
                  doctest::Approx(2.0 * row.expect_sum));
        }
    }

    TEST_CASE("explode refuses every non-List type by name") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            DataFrame df = frame_of("k", row.build());
            CHECK_THROWS_AS(df.explode("k"), std::invalid_argument);
            CHECK_THROWS_AS(df.unnest("k"), std::invalid_argument);
        }
    }

    TEST_CASE(
        "unnest drops empty lists, keeps them on request, flattens "
        "a struct element") {
        std::int64_t ids[3] = {1, 2, 3};
        std::int64_t counts[3] = {5, 7, 9};
        std::vector<Series> fields;
        fields.push_back(Series::strings({"read", "write", "open"}));
        fields.push_back(Series::flat_i64(counts, 3));
        Series elems = Series::structs({"value", "count"}, std::move(fields));
        DataFrame df;
        df.names = {"pid", "tk"};
        df.columns.push_back(Series::flat_i64(ids, 3));
        df.columns.push_back(Series::list({0, 2, 2, 3}, std::move(elems)));

        DataFrame dropped = df.unnest("tk");
        CHECK(dropped.names ==
              std::vector<std::string>{"pid", "value", "count"});
        REQUIRE(dropped.num_rows() == 3);
        Series dpid = dropped.column("pid").materialize();
        CHECK(dpid.data<std::int64_t>()[1] == 1);
        CHECK(dpid.data<std::int64_t>()[2] == 3);
        CHECK(dropped.column("count").materialize().data<std::int64_t>()[1] ==
              7);
        CHECK(dropped.column("value").materialize().string_at(2) == "open");

        DataFrame kept = df.unnest("tk", true);
        REQUIRE(kept.num_rows() == 4);
        Series pid = kept.column("pid").materialize();
        CHECK(pid.data<std::int64_t>()[2] == 2);
        CHECK(pid.data<std::int64_t>()[3] == 3);
        Series count = kept.column("count").materialize();
        CHECK(count.is_null(2));
        CHECK(count.data<std::int64_t>()[3] == 9);

        DataFrame plain = df.explode("tk");
        CHECK(plain.names == std::vector<std::string>{"pid", "tk"});
        CHECK(plain.num_rows() == 4);
        CHECK_THROWS_AS(df.unnest("nope"), std::out_of_range);
    }
}
#else
TEST_SUITE("dataframe_arrow_types_reshape") {
    TEST_CASE("Arrow disabled: nothing to exercise") { CHECK(true); }
}
#endif
