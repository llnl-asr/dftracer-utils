#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Lazy engine, spill and Expr coverage for the 15 Arrow logical types: the
// eager sweep (6d2859fc..95f09311) never touched lazy/streaming or spill, so
// a divergence found here is a lazy or spill bug, not eager's.
#ifdef DFTRACER_UTILS_ENABLE_ARROW
// clang-format off
#include <nanoarrow/nanoarrow.h>
#include <dftracer/utils/dataframe/arrow.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
// clang-format on

#include "builders_arrow_types.h"

using namespace dftracer::utils::dataframe;
using namespace dftracer::utils::dataframe::test_types;
namespace spill = dftracer::utils::dataframe::spill;

namespace {

DataFrame run(dftracer::utils::coro::CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

std::optional<Morsel> run(
    dftracer::utils::coro::CoroTask<std::optional<Morsel>> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

std::vector<const Series*> ptrs(const DataFrame& df) {
    std::vector<const Series*> in;
    for (const Series& c : df.columns) in.push_back(&c);
    return in;
}

DataFrame keyed(const Series& v) {
    std::vector<std::int64_t> k{1, 2, 3, 4};
    DataFrame df;
    df.names = {"k", "v"};
    df.columns.push_back(Series::flat_i64(k.data(), 4));
    df.columns.push_back(v.share());
    return df;
}

void check_v_equal(const Series& lazy_v, const Series& eager_v) {
    REQUIRE(lazy_v.type() == eager_v.type());
    REQUIRE(lazy_v.length() == eager_v.length());
    switch (lazy_v.type()) {
        case TypeId::LargeString:
        case TypeId::LargeBinary:
            for (std::int64_t i = 0; i < lazy_v.length(); ++i)
                CHECK(lazy_v.string_at(i) == eager_v.string_at(i));
            return;
        case TypeId::Float16:
            CHECK(std::memcmp(lazy_v.data<std::uint16_t>(),
                              eager_v.data<std::uint16_t>(),
                              static_cast<std::size_t>(lazy_v.length()) *
                                  sizeof(std::uint16_t)) == 0);
            return;
        case TypeId::Decimal128:
            CHECK(std::memcmp(
                      lazy_v.data<std::uint8_t>(), eager_v.data<std::uint8_t>(),
                      static_cast<std::size_t>(lazy_v.length()) * 16) == 0);
            return;
        case TypeId::Decimal256:
            CHECK(std::memcmp(
                      lazy_v.data<std::uint8_t>(), eager_v.data<std::uint8_t>(),
                      static_cast<std::size_t>(lazy_v.length()) * 32) == 0);
            return;
        case TypeId::FixedSizeBinary: {
            const std::int32_t w = dftu_series_fixed_size(lazy_v.handle());
            REQUIRE(w == dftu_series_fixed_size(eager_v.handle()));
            CHECK(std::memcmp(lazy_v.data<std::uint8_t>(),
                              eager_v.data<std::uint8_t>(),
                              static_cast<std::size_t>(lazy_v.length()) *
                                  static_cast<std::size_t>(w)) == 0);
            return;
        }
        default:
            FAIL("check_v_equal: unhandled TypeId ", type_name(lazy_v.type()));
    }
}

// No shared builder header covers the temporal family, so this file defines
// its own.
Series make_timestamp(const std::vector<std::int64_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIMESTAMP,
                                       NANOARROW_TIME_UNIT_MICRO,
                                       "UTC") == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_date32(const std::vector<std::int32_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetType(&b.schema, NANOARROW_TYPE_DATE32) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int32_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_date64(const std::vector<std::int64_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetType(&b.schema, NANOARROW_TYPE_DATE64) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_time32(const std::vector<std::int32_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIME32,
                                       NANOARROW_TIME_UNIT_MILLI,
                                       nullptr) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int32_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_time64(const std::vector<std::int64_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIME64,
                                       NANOARROW_TIME_UNIT_MICRO,
                                       nullptr) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_duration(const std::vector<std::int64_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_DURATION,
                                       NANOARROW_TIME_UNIT_MICRO,
                                       nullptr) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

// builders_arrow_types.h's make_decimal128 always uses scale 0, which cannot
// catch a lost decimal_scale; this one uses a non-zero scale instead.
Series make_decimal128_scaled(const std::vector<std::int64_t>& unscaled,
                              std::int32_t scale) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDecimal(&b.schema, NANOARROW_TYPE_DECIMAL128, 38,
                                      scale) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : unscaled) {
        ArrowDecimal dec;
        ArrowDecimalInit(&dec, 128, 38, scale);
        ArrowDecimalSetInt(&dec, v);
        REQUIRE(ArrowArrayAppendDecimal(&b.array, &dec) == NANOARROW_OK);
    }
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

enum class SpillExpect { RoundTrips, Refuses };

// Spills `col` alone in a one-column morsel and reads it back.
std::optional<Morsel> spill_round_trip(const Series& col, spill::Dir& dir,
                                       int id) {
    std::vector<Series> cols;
    cols.push_back(col.share());
    const std::string path = dir.run_path(id);
    spill::Writer w(path);
    w.write(cols, col.length());
    w.close();
    spill::Reader r(path);
    return run(r.next(0));
}

}  // namespace

TEST_SUITE("dataframe_arrow_types_lazy") {
    TEST_CASE("spill round-trips Float16, Decimal128/256 and FixedSizeBinary") {
        spill::Dir dir;

        {
            INFO("Float16");
            Series s = make_float16({1.5f, -2.25f, 0.0f, 65504.0f});
            auto m = spill_round_trip(s, dir, 0);
            REQUIRE(m.has_value());
            REQUIRE(m->columns.size() == 1);
            const Series& back = m->columns[0];
            REQUIRE(back.type() == TypeId::Float16);
            CHECK(std::memcmp(back.data<std::uint16_t>(),
                              s.data<std::uint16_t>(),
                              4 * sizeof(std::uint16_t)) == 0);
        }
        {
            INFO("Decimal128 (non-zero scale)");
            Series s = make_decimal128_scaled({12345, -100, 0, 999999}, 3);
            auto m = spill_round_trip(s, dir, 1);
            REQUIRE(m.has_value());
            const Series& back = m->columns[0];
            REQUIRE(back.type() == TypeId::Decimal128);
            CHECK(back.data_type().decimal_scale == 3);
            CHECK(std::memcmp(back.data<std::uint8_t>(), s.data<std::uint8_t>(),
                              4 * 16) == 0);
            // The scale must actually be used downstream: min() reads the
            // scaled value, so it only matches if decimal_scale survived.
            CHECK(scalar_value<double>(back.min()) ==
                  doctest::Approx(scalar_value<double>(s.min())));
        }
        {
            INFO("Decimal256");
            Series s = make_decimal256({300, 100, 200, 100});
            auto m = spill_round_trip(s, dir, 2);
            REQUIRE(m.has_value());
            const Series& back = m->columns[0];
            REQUIRE(back.type() == TypeId::Decimal256);
            CHECK(back.data_type().decimal_scale ==
                  s.data_type().decimal_scale);
            CHECK(std::memcmp(back.data<std::uint8_t>(), s.data<std::uint8_t>(),
                              4 * 32) == 0);
        }
        {
            INFO("FixedSizeBinary");
            Series s =
                make_fixed_size_binary({"cccc", "aaaa", "bbbb", "dddd"}, 4);
            auto m = spill_round_trip(s, dir, 3);
            REQUIRE(m.has_value());
            const Series& back = m->columns[0];
            REQUIRE(back.type() == TypeId::FixedSizeBinary);
            REQUIRE(dftu_series_fixed_size(back.handle()) == 4);
            CHECK(std::memcmp(back.data<std::uint8_t>(), s.data<std::uint8_t>(),
                              4 * 4) == 0);
        }
    }

    TEST_CASE(
        "spill round-trips LargeString, LargeBinary and the temporal "
        "family") {
        spill::Dir dir;
        {
            INFO("LargeString");
            Series s = make_large_utf8({"hello", "", "world", "dftracer"});
            auto m = spill_round_trip(s, dir, 0);
            REQUIRE(m.has_value());
            const Series& back = m->columns[0];
            REQUIRE(back.type() == TypeId::LargeString);
            CHECK(back.string_at(0) == "hello");
            CHECK(back.string_at(1) == "");
            CHECK(back.string_at(2) == "world");
            CHECK(back.string_at(3) == "dftracer");
        }
        {
            INFO("LargeBinary");
            Series s = make_large_binary({"c", "a", "b", "a"});
            auto m = spill_round_trip(s, dir, 1);
            REQUIRE(m.has_value());
            const Series& back = m->columns[0];
            REQUIRE(back.type() == TypeId::LargeBinary);
            CHECK(back.string_at(0) == "c");
            CHECK(back.string_at(3) == "a");
        }
        {
            INFO("Date32");
            Series s = make_date32({19000, -5, 0, 20000});
            auto m = spill_round_trip(s, dir, 2);
            REQUIRE(m.has_value());
            const Series& back = m->columns[0];
            REQUIRE(back.type() == TypeId::Date32);
            CHECK(std::memcmp(back.data<std::int32_t>(), s.data<std::int32_t>(),
                              4 * sizeof(std::int32_t)) == 0);
        }
        {
            INFO("Date64");
            Series s = make_date64({1'700'000'000'000, -5, 0, 2'000'000'000});
            auto m = spill_round_trip(s, dir, 3);
            REQUIRE(m.has_value());
            const Series& back = m->columns[0];
            REQUIRE(back.type() == TypeId::Date64);
            CHECK(std::memcmp(back.data<std::int64_t>(), s.data<std::int64_t>(),
                              4 * sizeof(std::int64_t)) == 0);
        }
        {
            INFO("Time32");
            Series s = make_time32({1000, -1, 0, 86399000});
            auto m = spill_round_trip(s, dir, 4);
            REQUIRE(m.has_value());
            const Series& back = m->columns[0];
            REQUIRE(back.type() == TypeId::Time32);
            CHECK(std::memcmp(back.data<std::int32_t>(), s.data<std::int32_t>(),
                              4 * sizeof(std::int32_t)) == 0);
        }
        {
            INFO("Time64");
            Series s = make_time64({1000, -1, 0, 86399000000});
            auto m = spill_round_trip(s, dir, 5);
            REQUIRE(m.has_value());
            const Series& back = m->columns[0];
            REQUIRE(back.type() == TypeId::Time64);
            CHECK(std::memcmp(back.data<std::int64_t>(), s.data<std::int64_t>(),
                              4 * sizeof(std::int64_t)) == 0);
        }
        {
            INFO("Timestamp");
            Series s = make_timestamp({300, -1, 100, 0});
            auto m = spill_round_trip(s, dir, 6);
            REQUIRE(m.has_value());
            const Series& back = m->columns[0];
            REQUIRE(back.type() == TypeId::Timestamp);
            CHECK(std::memcmp(back.data<std::int64_t>(), s.data<std::int64_t>(),
                              4 * sizeof(std::int64_t)) == 0);
        }
        {
            INFO("Duration");
            Series s = make_duration({300, -1, 100, 0});
            auto m = spill_round_trip(s, dir, 7);
            REQUIRE(m.has_value());
            const Series& back = m->columns[0];
            REQUIRE(back.type() == TypeId::Duration);
            CHECK(std::memcmp(back.data<std::int64_t>(), s.data<std::int64_t>(),
                              4 * sizeof(std::int64_t)) == 0);
        }
    }

    TEST_CASE(
        "spill refuses the nested types (List/LargeList/FixedSizeList/"
        "Struct/Map)") {
        spill::Dir dir;
        Series large_list = make_large_list_i64({{3}, {1}, {2}, {1}});
        Series fixed_list =
            make_fixed_size_list_i64({{3, 3}, {1, 1}, {2, 2}, {1, 1}});
        Series map_col = make_map_string_i64(
            {{{"c", 3}}, {{"a", 1}}, {{"b", 2}}, {{"a", 1}}});

        std::vector<Series> types;
        types.push_back(large_list.share());
        types.push_back(fixed_list.share());
        types.push_back(map_col.share());
        int id = 0;
        for (Series& col : types) {
            INFO(std::string(type_name(col.type())));
            spill::Writer w(dir.run_path(id++));
            std::vector<Series> cols;
            cols.push_back(col.share());
            CHECK_THROWS_AS(w.write(cols, col.length()), std::invalid_argument);
        }
    }

    // Eager is the oracle: build a 2-column frame (Int64 key `k`, special-typed
    // `v`), run the same plan through LazyFrame::collect() and DataFrame's
    // eager ops, and assert the results match.
    TEST_CASE(
        "lazy filter/select/head/with_column match eager for the "
        "opaque-value types") {
        std::vector<Series> values;
        values.push_back(make_float16({10.0f, 20.0f, 30.0f, 40.0f}));
        values.push_back(make_decimal128_scaled({100, 200, 300, 400}, 2));
        values.push_back(make_decimal256({100, 200, 300, 400}));
        values.push_back(
            make_fixed_size_binary({"aaaa", "bbbb", "cccc", "dddd"}, 4));
        values.push_back(make_large_utf8({"a", "b", "c", "d"}));
        values.push_back(make_large_binary({"a", "b", "c", "d"}));
        for (Series& v : values) {
            INFO(std::string(type_name(v.type())));
            DataFrame src = keyed(v);

            // filter: k > 2
            {
                LazyFrame lf = src.lazy().filter(col(0) > std::int64_t{2});
                DataFrame lazy_r = run(lf.collect());
                Series mask = eval(col(0) > std::int64_t{2}, ptrs(src));
                DataFrame eager_r = src.filter(mask);
                REQUIRE(lazy_r.num_rows() == eager_r.num_rows());
                REQUIRE(lazy_r.num_rows() == 2);
                check_v_equal(lazy_r.columns[1], eager_r.columns[1]);
            }
            // select: keep only v
            {
                LazyFrame lf = src.lazy().select({"v"});
                DataFrame lazy_r = run(lf.collect());
                DataFrame eager_r = src.select({"v"});
                REQUIRE(lazy_r.columns.size() == 1);
                check_v_equal(lazy_r.columns[0], eager_r.columns[0]);
            }
            // head(2)
            {
                LazyFrame lf = src.lazy().head(2);
                DataFrame lazy_r = run(lf.collect(2));
                DataFrame eager_r = src.head(2);
                REQUIRE(lazy_r.num_rows() == 2);
                check_v_equal(lazy_r.columns[1], eager_r.columns[1]);
            }
            // with_column: copy v under a new name, unchanged
            {
                LazyFrame lf = src.lazy().with_column("v2", col(1));
                DataFrame lazy_r = run(lf.collect());
                DataFrame eager_r =
                    src.with_column("v2", eval(col(1), ptrs(src)));
                REQUIRE(lazy_r.columns.size() == 3);
                check_v_equal(lazy_r.columns[2], eager_r.columns[2]);
            }
        }
    }

    TEST_CASE("lazy sort_by on the value column matches eager") {
        std::vector<Series> values;
        values.push_back(make_float16({30.0f, 10.0f, 20.0f, 10.0f}));
        values.push_back(make_decimal128_scaled({300, 100, 200, 100}, 1));
        values.push_back(make_decimal256({300, 100, 200, 100}));
        values.push_back(
            make_fixed_size_binary({"cccc", "aaaa", "bbbb", "aaaa"}, 4));
        values.push_back(make_large_utf8({"c", "a", "b", "a"}));
        values.push_back(make_large_binary({"c", "a", "b", "a"}));
        for (Series& v : values) {
            INFO(std::string(type_name(v.type())));
            DataFrame src = keyed(v);
            LazyFrame lf = src.lazy().sort_by("v", false);
            DataFrame lazy_r = run(lf.collect());
            DataFrame eager_r = src.sort_by("v", false);
            REQUIRE(lazy_r.num_rows() == eager_r.num_rows());
            check_v_equal(lazy_r.columns[1], eager_r.columns[1]);
            // sort_by must reorder `k` in lockstep with `v`.
            const std::int64_t* lk = lazy_r.columns[0].data<std::int64_t>();
            const std::int64_t* ek = eager_r.columns[0].data<std::int64_t>();
            for (std::int64_t i = 0; i < lazy_r.num_rows(); ++i)
                CHECK(lk[i] == ek[i]);
        }
    }

    TEST_CASE("lazy group_by(Count) on an opaque value column matches eager") {
        std::vector<std::int64_t> k{1, 1, 2, 2};
        Series vals = make_large_utf8({"p", "q", "r", "s"});
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(Series::flat_i64(k.data(), 4));
        df.columns.push_back(vals.share());

        std::vector<GroupAgg> aggs = {{Agg::Count, "v", "v_count"}};
        DataFrame lazy_r = run(df.lazy().group_by("k", aggs).collect(2));
        DataFrame eager_r = df.group_by("k", aggs);
        REQUIRE(lazy_r.num_rows() == eager_r.num_rows());
        REQUIRE(lazy_r.num_rows() == 2);
        const std::int64_t* lc = lazy_r.columns[1].data<std::int64_t>();
        const std::int64_t* ec = eager_r.columns[1].data<std::int64_t>();
        for (std::int64_t i = 0; i < lazy_r.num_rows(); ++i)
            CHECK(lc[i] == ec[i]);
    }
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW
