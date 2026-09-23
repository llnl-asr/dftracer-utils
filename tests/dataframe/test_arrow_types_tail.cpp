#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

// The tail of the Arrow-types audit: search_sorted, interpolate and rank,
// which the matrix test in test_arrow_types_matrix.cpp does not cover. Same
// table-driven shape and the same default-less is_audited() switch, so a new
// TypeId with no row here fails instead of being silently skipped.
#ifdef DFTRACER_UTILS_ENABLE_ARROW
// clang-format off
#include <nanoarrow/nanoarrow.h>
#include <dftracer/utils/dataframe/arrow.h>
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

struct TypeRow {
    TypeId id;
    Series (*build)();          // rows: {3, 1, 2, 1} in the type's own order
    Series (*build_needles)();  // 4 probes: below, mid-low, mid-high, above
    Support numeric_ops;        // search_sorted / interpolate: numeric-only
    Support nested_ops;         // rank: refuses only nested (None-domain) types
    double expect[4];  // build()'s rows widened to double, in row order
};

Series build_float16() { return make_float16({3.0f, 1.0f, 2.0f, 1.0f}); }
Series needles_float16() { return make_float16({0.0f, 1.5f, 2.5f, 4.0f}); }
Series build_decimal128() { return make_decimal128({300, 100, 200, 100}); }
Series build_decimal256() { return make_decimal256({300, 100, 200, 100}); }
Series needles_decimal128() { return make_decimal128({50, 150, 250, 400}); }
Series needles_decimal256() { return make_decimal256({50, 150, 250, 400}); }
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

const TypeRow TYPE_ROWS[] = {
    {TypeId::Float16,
     build_float16,
     needles_float16,
     C,
     C,
     {3.0, 1.0, 2.0, 1.0}},
    {TypeId::Decimal128,
     build_decimal128,
     needles_decimal128,
     C,
     C,
     {300.0, 100.0, 200.0, 100.0}},
    {TypeId::Decimal256,
     build_decimal256,
     needles_decimal256,
     C,
     C,
     {300.0, 100.0, 200.0, 100.0}},
    {TypeId::FixedSizeBinary, build_fixed_size_binary, nullptr, R, C, {}},
    {TypeId::LargeString, build_large_string, nullptr, R, C, {}},
    {TypeId::LargeBinary, build_large_binary, nullptr, R, C, {}},
    {TypeId::LargeList, build_large_list, nullptr, R, R, {}},
    {TypeId::FixedSizeList, build_fixed_size_list, nullptr, R, R, {}},
    {TypeId::Map, build_map, nullptr, R, R, {}},
};

/// Mirrors is_audited() in test_arrow_types_matrix.cpp: the types only Arrow
/// import can build. No default case, so a new TypeId forces a decision here.
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

}  // namespace

TEST_SUITE("dataframe_arrow_types_tail") {
    TEST_CASE("every audited TypeId has a tail row") {
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

    TEST_CASE("search_sorted: numeric-domain types get a real position") {
        // Sorted ascending source has a tied lowest pair, e.g. {1, 1, 2, 3};
        // the needle set probes below, between the tie and 2, between 2 and
        // 3, and above every value.
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            if (row.numeric_ops == Support::Refuses) {
                CHECK_FALSE(s.search_sorted(s).valid());
                continue;
            }
            Series sorted = s.sort(false);
            REQUIRE(sorted.valid());
            Series needles = row.build_needles();
            REQUIRE(needles.valid());
            Series pos = sorted.search_sorted(needles);
            REQUIRE(pos.valid());
            REQUIRE(pos.length() == 4);
            const std::int64_t* p = pos.data<std::int64_t>();
            REQUIRE(p != nullptr);
            CHECK(p[0] == 0);  // below every value
            CHECK(p[1] == 2);  // past both tied lowest rows
            CHECK(p[2] == 3);  // past the third row
            CHECK(p[3] == 4);  // above every value
        }
    }

    TEST_CASE("search_sorted on a non-numeric self column refuses") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            if (row.numeric_ops == Support::Refuses) {
                CHECK_FALSE(s.search_sorted(s).valid());
            }
        }
    }

    TEST_CASE("interpolate: numeric-domain types fill the null interior") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            Series interpolated = s.interpolate();
            if (row.numeric_ops == Support::Refuses) {
                CHECK_FALSE(interpolated.valid());
                continue;
            }
            // No column here has an actual null, so interpolate degrades to
            // a widening copy: every row must equal read back as a double.
            REQUIRE(interpolated.valid());
            CHECK(interpolated.type() == TypeId::Float64);
            REQUIRE(interpolated.length() == 4);
            const double* d = interpolated.data<double>();
            REQUIRE(d != nullptr);
            CHECK(d[0] == doctest::Approx(row.expect[0]));
            CHECK(d[1] == doctest::Approx(row.expect[1]));
            CHECK(d[2] == doctest::Approx(row.expect[2]));
            CHECK(d[3] == doctest::Approx(row.expect[3]));
        }
    }

    TEST_CASE("rank: dense, min, average and ordinal tie-breaking") {
        // Rows are {3, 1, 2, 1}: value 1 (rows 1, 3) ties for the lowest rank.
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            Series dense = s.rank(RankMethod::Dense, false);
            Series min_r = s.rank(RankMethod::Min, false);
            Series avg = s.rank(RankMethod::Average, false);
            Series ordinal = s.rank(RankMethod::Ordinal, false);
            if (row.nested_ops == Support::Refuses) {
                CHECK_FALSE(dense.valid());
                CHECK_FALSE(min_r.valid());
                CHECK_FALSE(avg.valid());
                CHECK_FALSE(ordinal.valid());
                continue;
            }
            REQUIRE(dense.valid());
            REQUIRE(dense.type() == TypeId::Float64);
            const double* dv = dense.data<double>();
            REQUIRE(dv != nullptr);
            // Dense: two ties at rank 1 do not create a gap; rank 2 follows.
            CHECK(dv[0] == doctest::Approx(3.0));  // value 3
            CHECK(dv[1] == doctest::Approx(1.0));  // value 1 (tied)
            CHECK(dv[2] == doctest::Approx(2.0));  // value 2
            CHECK(dv[3] == doctest::Approx(1.0));  // value 1 (tied)

            REQUIRE(min_r.valid());
            const double* mv = min_r.data<double>();
            REQUIRE(mv != nullptr);
            // Min: both tied rows take the lower of the two ordinal slots.
            CHECK(mv[0] == doctest::Approx(4.0));
            CHECK(mv[1] == doctest::Approx(1.0));
            CHECK(mv[2] == doctest::Approx(3.0));
            CHECK(mv[3] == doctest::Approx(1.0));

            REQUIRE(avg.valid());
            const double* av = avg.data<double>();
            REQUIRE(av != nullptr);
            // Average: both tied rows take the mean of ordinal slots 1 and 2.
            CHECK(av[0] == doctest::Approx(4.0));
            CHECK(av[1] == doctest::Approx(1.5));
            CHECK(av[2] == doctest::Approx(3.0));
            CHECK(av[3] == doctest::Approx(1.5));

            REQUIRE(ordinal.valid());
            const double* ov = ordinal.data<double>();
            REQUIRE(ov != nullptr);
            // Ordinal: every row gets a distinct slot, in ascending order,
            // ties broken by original position (row 1 before row 3).
            CHECK(ov[0] == doctest::Approx(4.0));
            CHECK(ov[1] == doctest::Approx(1.0));
            CHECK(ov[2] == doctest::Approx(3.0));
            CHECK(ov[3] == doctest::Approx(2.0));
        }
    }
}
#else
TEST_SUITE("dataframe_arrow_types_tail") {
    TEST_CASE("Arrow disabled: nothing to exercise") { CHECK(true); }
}
#endif
