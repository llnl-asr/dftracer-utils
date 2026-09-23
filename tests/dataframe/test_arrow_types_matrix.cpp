#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// One table of {TypeId, per-op expectation} covering every Arrow logical type
// the engine can build, so an op that neither computes nor refuses shows up as
// a failure instead of a plausible zero. TYPE_ROWS must have an entry for
// every TypeId is_audited() marks true, and is_audited() has no default case,
// so a newly added TypeId cannot slip through unclassified.
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

/// What an op does with a column of this type. Computes: a real answer, which
/// the case below asserts by value. Refuses: a named refusal, surfaced as an
/// invalid Series, a sentinel scalar, or a thrown std::invalid_argument.
enum class Support {
    Computes,
    Refuses,
};

enum Op {
    OP_ORDER,       // sort / argsort / is_sorted
    OP_REDUCE,      // min / max / sum / mean / arg_min
    OP_DISTINCT,    // nunique / unique / is_unique
    OP_IS_IN,       //
    OP_GROUP_ABI,   // dftu_series_group_by
    OP_GROUP_AGG,   // DataFrame::group_by (AggState)
    OP_SELECT,      // take / head / reverse
    OP_CAST_I64,    // cast to Int64
    OP_CAST_FROM,   // cast a Float64 column to this type
    OP_HASH,        // fnv1a / dictionary_encode
    OP_VALUE_COUNTS,
    OP_ARITHMETIC,  // add
    OP_COMPARE,     // dftu_series_compare against a scalar
    OP_COUNT_,
};

// Every builder lays its rows out as {third, first, second, first} in the
// type's own order, so one set of expectations covers all of them: not sorted,
// three distinct values, ascending order {1, 3, 2, 0}, and row 1's value
// present twice.
struct TypeRow {
    TypeId id;
    Series (*build)();
    Series (*build_needle)();  // one row holding the value rows 1 and 3 share
    Support ops[OP_COUNT_];
    double expect_min;
    double expect_max;
    double expect_sum;
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

constexpr Support C = Support::Computes;
constexpr Support R = Support::Refuses;

const TypeRow TYPE_ROWS[] = {
    // id, build, needle, {order, reduce, distinct, is_in, gby_abi, gby_agg,
    // select,
    //             cast_i64, cast_from, hash, value_counts, arithmetic,
    //             compare},
    // min, max, sum
    {TypeId::Float16,
     build_float16,
     needle_float16,
     {C, C, C, C, C, C, C, C, C, R, C, C, R},
     1.0,
     3.0,
     7.0},
    {TypeId::Decimal128,
     build_decimal128,
     needle_decimal128,
     {C, C, C, C, C, C, C, R, R, R, C, C, R},
     100.0,
     300.0,
     700.0},
    {TypeId::Decimal256,
     build_decimal256,
     needle_decimal256,
     {C, C, C, C, C, C, C, R, R, R, C, C, R},
     100.0,
     300.0,
     700.0},
    {TypeId::FixedSizeBinary,
     build_fixed_size_binary,
     needle_fixed_size_binary,
     {C, R, C, C, C, C, C, R, R, R, C, R, R},
     0.0,
     0.0,
     0.0},
    {TypeId::LargeString,
     build_large_string,
     needle_large_string,
     {C, R, C, C, C, C, C, C, R, C, C, R, R},
     0.0,
     0.0,
     0.0},
    {TypeId::LargeBinary,
     build_large_binary,
     needle_large_binary,
     {C, R, C, C, C, C, C, R, R, C, C, R, R},
     0.0,
     0.0,
     0.0},
    {TypeId::LargeList,
     build_large_list,
     needle_large_list,
     {R, R, R, R, R, R, C, R, R, R, R, R, R},
     0.0,
     0.0,
     0.0},
    {TypeId::FixedSizeList,
     build_fixed_size_list,
     needle_fixed_size_list,
     {R, R, R, R, R, R, R, R, R, R, R, R, R},
     0.0,
     0.0,
     0.0},
    {TypeId::Map,
     build_map,
     needle_map,
     {R, R, R, R, R, R, R, R, R, R, R, R, R},
     0.0,
     0.0,
     0.0},
};

/// True for the types this matrix owns: the ones only Arrow import can build,
/// which is exactly where a silently-wrong kernel result would go unnoticed.
/// The plain scalar and 32-bit-offset types are covered by test_series and
/// test_methods, and the temporal family by test_arrow_types_compute. No
/// default case, so adding a TypeId forces a decision here.
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

bool mask_bit(const Series& mask, std::int64_t i) {
    const std::uint8_t* bits = mask.data<std::uint8_t>();
    REQUIRE(bits != nullptr);
    return ((bits[i >> 3] >> (i & 7)) & 1) != 0;
}

Series count_ones(std::int64_t n) {
    static std::vector<std::int64_t> ones;
    ones.assign(static_cast<std::size_t>(n), 1);
    return Series::flat_i64(ones.data(), n);
}

DataFrame keyed_frame(const Series& keys) {
    DataFrame df;
    df.names = {"k", "v"};
    df.columns.push_back(keys.share());
    df.columns.push_back(count_ones(keys.length()).share());
    return df;
}

}  // namespace

TEST_SUITE("dataframe_arrow_types_matrix") {
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

    TEST_CASE("sort, argsort and is_sorted") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            Series order = s.argsort(false);
            Series sorted = s.sort(false);
            if (row.ops[OP_ORDER] == Support::Refuses) {
                CHECK_FALSE(order.valid());
                CHECK_FALSE(sorted.valid());
                CHECK_FALSE(s.is_sorted(false));
                continue;
            }
            REQUIRE(order.valid());
            REQUIRE(order.length() == 4);
            const std::int64_t* ix = order.data<std::int64_t>();
            REQUIRE(ix != nullptr);
            CHECK(ix[0] == 1);
            CHECK(ix[1] == 3);
            CHECK(ix[2] == 2);
            CHECK(ix[3] == 0);
            REQUIRE(sorted.valid());
            CHECK(sorted.type() == row.id);
            CHECK(sorted.is_sorted(false));
            CHECK_FALSE(s.is_sorted(false));
        }
    }

    TEST_CASE("min, max, sum and mean") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            if (row.ops[OP_REDUCE] == Support::Refuses) {
                CHECK(scalar_value<double>(s.min()) == 0.0);
                CHECK(scalar_value<double>(s.max()) == 0.0);
                CHECK(scalar_value<double>(s.sum()) == 0.0);
                CHECK(s.arg_min() == -1);
                CHECK(s.arg_max() == -1);
                continue;
            }
            CHECK(scalar_value<double>(s.min()) ==
                  doctest::Approx(row.expect_min));
            CHECK(scalar_value<double>(s.max()) ==
                  doctest::Approx(row.expect_max));
            CHECK(scalar_value<double>(s.sum()) ==
                  doctest::Approx(row.expect_sum));
            CHECK(s.mean() == doctest::Approx(row.expect_sum / 4.0));
            CHECK(s.arg_min() == 1);
            CHECK(s.arg_max() == 0);
        }
    }

    TEST_CASE("count is the non-null row count for every type") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            CHECK(s.count() == s.length());
            Series kept = s.drop_nulls();
            REQUIRE(kept.valid());
            CHECK(kept.length() == s.length());
        }
    }

    TEST_CASE("nunique, unique and is_unique") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            Series uniq = s.unique();
            Series mask = s.is_unique();
            if (row.ops[OP_DISTINCT] == Support::Refuses) {
                CHECK(s.nunique() == 0);
                CHECK_FALSE(uniq.valid());
                CHECK_FALSE(mask.valid());
                continue;
            }
            CHECK(s.nunique() == 3);
            REQUIRE(uniq.valid());
            CHECK(uniq.length() == 3);
            CHECK(uniq.type() == row.id);
            REQUIRE(mask.valid());
            REQUIRE(mask.length() == 4);
            CHECK(mask_bit(mask, 0) == true);   // third value, once
            CHECK(mask_bit(mask, 1) == false);  // first value, twice
            CHECK(mask_bit(mask, 2) == true);   // second value, once
            CHECK(mask_bit(mask, 3) == false);
        }
    }

    TEST_CASE("is_in matches the exact repeated value") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            if (row.ops[OP_IS_IN] == Support::Refuses) {
                CHECK_FALSE(s.is_in(s.share()).valid());
                continue;
            }
            Series needle = row.build_needle();
            REQUIRE(needle.valid());
            Series mask = s.is_in(needle);
            REQUIRE(mask.valid());
            REQUIRE(mask.length() == 4);
            CHECK(mask_bit(mask, 0) == false);
            CHECK(mask_bit(mask, 1) == true);
            CHECK(mask_bit(mask, 2) == false);
            CHECK(mask_bit(mask, 3) == true);
        }
    }

    TEST_CASE("group_by through the ABI primitive") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            Series values = count_ones(s.length());
            dftu_series* out_keys = nullptr;
            dftu_series* out_values[5] = {};
            const std::int32_t n = dftu_series_group_by(
                s.handle(), values.handle(), DFTU_REDUCE_COUNT, &out_keys,
                out_values, 5);
            if (row.ops[OP_GROUP_ABI] == Support::Refuses) {
                CHECK(n == 0);
                CHECK(out_keys == nullptr);
                continue;
            }
            REQUIRE(n == 1);
            REQUIRE(out_keys != nullptr);
            CHECK(dftu_series_length(out_keys) == 3);
            Series counts{out_values[0]};
            REQUIRE(counts.valid());
            REQUIRE(counts.length() == 3);
            const std::int64_t* c = counts.data<std::int64_t>();
            REQUIRE(c != nullptr);
            CHECK(c[0] == 1);  // third value, first seen
            CHECK(c[1] == 2);  // first value, seen twice
            CHECK(c[2] == 1);  // second value
            dftu_series_free(out_keys);
        }
    }

    TEST_CASE("group_by through DataFrame (AggState) refuses these key types") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            DataFrame df = keyed_frame(s);
            if (row.ops[OP_GROUP_AGG] == Support::Refuses) {
                CHECK_THROWS_AS(
                    df.group_by("k", {GroupAgg{Agg::Count, "v", "n"}}),
                    std::invalid_argument);
                continue;
            }
            DataFrame g = df.group_by("k", {GroupAgg{Agg::Count, "v", "n"}});
            CHECK(g.num_rows() == 3);
            CHECK(g.column("k").type() == row.id);
        }
    }

    TEST_CASE("take, head and reverse keep the logical type") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            std::vector<std::int64_t> ix{2, 0};
            Series taken = s.take(ix);
            Series first = s.head(1);
            Series flipped = s.reverse();
            if (row.ops[OP_SELECT] == Support::Refuses) {
                CHECK_FALSE(taken.valid());
                CHECK_FALSE(first.valid());
                CHECK_FALSE(flipped.valid());
                continue;
            }
            REQUIRE(taken.valid());
            CHECK(taken.length() == 2);
            CHECK(taken.type() == row.id);
            REQUIRE(first.valid());
            CHECK(first.length() == 1);
            REQUIRE(flipped.valid());
            CHECK(flipped.length() == 4);
        }
    }

    TEST_CASE("cast to Int64") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            Series casted = s.cast(TypeId::Int64);
            if (row.ops[OP_CAST_I64] == Support::Refuses) {
                CHECK_FALSE(casted.valid());
                continue;
            }
            REQUIRE(casted.valid());
            CHECK(casted.type() == TypeId::Int64);
            if (row.id == TypeId::LargeString) {
                // A String parses row by row; these letters are not numbers.
                CHECK(casted.null_count() == 4);
                continue;
            }
            const std::int64_t* d = casted.data<std::int64_t>();
            REQUIRE(d != nullptr);
            CHECK(d[0] == 3);
            CHECK(d[1] == 1);
        }
        Series digits =
            make_large_utf8({"3", "1", "2", "1"}).cast(TypeId::Int64);
        REQUIRE(digits.valid());
        CHECK(digits.data<std::int64_t>()[0] == 3);
        CHECK(digits.null_count() == 0);
    }

    TEST_CASE("casting INTO these types") {
        std::vector<double> vals = {1.5, 2.5};
        Series a = Series::flat_f64(vals.data(), 2);
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series casted = a.cast(row.id);
            if (row.ops[OP_CAST_FROM] == Support::Refuses) {
                CHECK_FALSE(casted.valid());
                continue;
            }
            REQUIRE(casted.valid());
            CHECK(casted.type() == row.id);
            CHECK(casted.length() == 2);
        }
    }

    TEST_CASE("fnv1a and dictionary_encode") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            Series hashed = s.fnv1a();
            Series dict = s.dictionary_encode();
            if (row.ops[OP_HASH] == Support::Refuses) {
                CHECK_FALSE(hashed.valid());
                CHECK_FALSE(dict.valid());
                continue;
            }
            REQUIRE(hashed.valid());
            CHECK(hashed.type() == TypeId::Uint64);
            const std::uint64_t* h = hashed.data<std::uint64_t>();
            REQUIRE(h != nullptr);
            CHECK(h[1] == h[3]);  // equal values hash equal
            CHECK(h[0] != h[1]);
            REQUIRE(dict.valid());
            CHECK(dict.length() == 4);
        }
    }

    TEST_CASE("value_counts") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            dftu_series* h = s.handle();
            if (row.ops[OP_VALUE_COUNTS] == Support::Refuses) {
                CHECK(dftu_series_value_counts(h) == nullptr);
                continue;
            }
            dftu_dataframe* vc = dftu_series_value_counts(h);
            REQUIRE(vc != nullptr);
            CHECK(dftu_dataframe_num_rows(vc) == 3);
            dftu_dataframe_free(vc);
        }
    }

    TEST_CASE("arithmetic promotes or refuses, never computes on raw bits") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            Series other = row.build();
            REQUIRE(s.valid());
            Series sum = s.add(other);
            if (row.ops[OP_ARITHMETIC] == Support::Refuses) {
                CHECK_FALSE(sum.valid());
                continue;
            }
            REQUIRE(sum.valid());
            if (row.id == TypeId::Decimal128 || row.id == TypeId::Decimal256) {
                CHECK(sum.type() == row.id);
            } else {
                CHECK((sum.type() == TypeId::Float32 ||
                       sum.type() == TypeId::Float64));
            }
            CHECK(sum.length() == 4);
        }
    }

    TEST_CASE("comparison against a scalar") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            dftu_series* mask =
                dftu_series_compare(s.handle(), DFTU_CMP_GT, i64(0));
            if (row.ops[OP_COMPARE] == Support::Refuses) {
                CHECK(mask == nullptr);
                continue;
            }
            REQUIRE(mask != nullptr);
            Series m{mask};
            CHECK(m.length() == 4);
        }
    }

    TEST_CASE("describe reports the fixed row set for every type") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            DataFrame df;
            df.names = {"k"};
            df.columns.push_back(s.share());
            DataFrame d = df.describe();
            CHECK(d.num_rows() == 6);
        }
    }

    TEST_CASE("fill_null") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            // No kernel has a lane type for any of these, so fill_null
            // refuses rather than returning the column untouched.
            CHECK_FALSE(s.fillna(i64(0)).valid());
        }
    }
}
#else
TEST_SUITE("dataframe_arrow_types_matrix") {
    TEST_CASE("Arrow disabled: nothing to exercise") { CHECK(true); }
}
#endif
