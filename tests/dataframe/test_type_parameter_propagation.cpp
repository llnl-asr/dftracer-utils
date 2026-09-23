#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// gather_column (kernels/filter.cpp) and dftu_series_slice (abi.cpp) used to
// copy a source column's TypeId but not its time_unit/timezone/
// decimal_precision/decimal_scale/fixed_size, so every op built on take/slice
// (head, tail, take, sort, unique, reverse, drop_nulls, DataFrame::head/
// filter/slice, and the group_by key) silently dropped those parameters:
// Decimal128/256 and Time32 segfaulted on Arrow export, Timestamp/Time64/
// Duration exported with the wrong unit or timezone. This asserts every
// parametric type keeps its parameters (and correct values) through every
// reachable op, so a future `new dftu_series()` site that forgets them fails
// here instead of shipping a silent wrong-answer.
#ifdef DFTRACER_UTILS_ENABLE_ARROW
// clang-format off
#include <nanoarrow/nanoarrow.h>
#include <dftracer/utils/dataframe/arrow.h>
#include <dftracer/utils/dataframe/dataframe.h>
// clang-format on

using namespace dftracer::utils::dataframe;

namespace {

struct BuiltArrow {
    ArrowSchema schema{};
    ArrowArray array{};
    ~BuiltArrow() {
        if (array.release) array.release(&array);
        if (schema.release) schema.release(&schema);
    }
};

void finish(ArrowArray* a) {
    ArrowError err;
    REQUIRE(ArrowArrayFinishBuildingDefault(a, &err) == NANOARROW_OK);
}

Series make_timestamp(const std::vector<std::int64_t>& values, TimeUnit unit,
                      const char* tz) {
    ArrowTimeUnit au = unit == TimeUnit::Second  ? NANOARROW_TIME_UNIT_SECOND
                       : unit == TimeUnit::Milli ? NANOARROW_TIME_UNIT_MILLI
                       : unit == TimeUnit::Micro ? NANOARROW_TIME_UNIT_MICRO
                                                 : NANOARROW_TIME_UNIT_NANO;
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIMESTAMP, au,
                                       tz) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_time32(const std::vector<std::int32_t>& values, TimeUnit unit) {
    ArrowTimeUnit au = unit == TimeUnit::Second ? NANOARROW_TIME_UNIT_SECOND
                                                : NANOARROW_TIME_UNIT_MILLI;
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIME32, au,
                                       nullptr) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int32_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_time64(const std::vector<std::int64_t>& values, TimeUnit unit) {
    ArrowTimeUnit au = unit == TimeUnit::Micro ? NANOARROW_TIME_UNIT_MICRO
                                               : NANOARROW_TIME_UNIT_NANO;
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIME64, au,
                                       nullptr) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_duration(const std::vector<std::int64_t>& values, TimeUnit unit) {
    ArrowTimeUnit au = unit == TimeUnit::Second  ? NANOARROW_TIME_UNIT_SECOND
                       : unit == TimeUnit::Milli ? NANOARROW_TIME_UNIT_MILLI
                       : unit == TimeUnit::Micro ? NANOARROW_TIME_UNIT_MICRO
                                                 : NANOARROW_TIME_UNIT_NANO;
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_DURATION, au,
                                       nullptr) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values)
        REQUIRE(ArrowArrayAppendInt(&b.array, v) == NANOARROW_OK);
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_decimal(ArrowType arrow_type, int bitwidth, std::int32_t precision,
                    std::int32_t scale,
                    const std::vector<std::int64_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDecimal(&b.schema, arrow_type, precision,
                                      scale) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values) {
        ArrowDecimal dec;
        ArrowDecimalInit(&dec, bitwidth, precision, scale);
        ArrowDecimalSetInt(&dec, v);
        REQUIRE(ArrowArrayAppendDecimal(&b.array, &dec) == NANOARROW_OK);
    }
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

Series make_fixed_size_binary(const std::vector<std::string>& values,
                              std::int32_t width) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeFixedSize(&b.schema,
                                        NANOARROW_TYPE_FIXED_SIZE_BINARY,
                                        width) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (const std::string& v : values) {
        ArrowBufferView bv;
        bv.data.data = v.data();
        bv.size_bytes = width;
        REQUIRE(ArrowArrayAppendBytes(&b.array, bv) == NANOARROW_OK);
    }
    finish(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

// Round-trips `s` through to_arrow/from_arrow and returns the reimported
// column: exercises the export path a segfaulting gather would crash in, not
// just the in-memory dftu_series fields.
Series arrow_round_trip(const Series& s) {
    OwnedArrow out = s.to_arrow();
    REQUIRE(static_cast<bool>(out));
    Series back = Series::from_arrow(out.schema(), out.array());
    REQUIRE(back.valid());
    return back;
}

void check_bytes_equal(const Series& a, std::int64_t ai, const Series& b,
                       std::int64_t bi, std::size_t width) {
    Series am = a.is_flat() ? a.share() : a.materialize();
    Series bm = b.is_flat() ? b.share() : b.materialize();
    REQUIRE(am.data<std::uint8_t>() != nullptr);
    REQUIRE(bm.data<std::uint8_t>() != nullptr);
    CHECK(std::memcmp(
              am.data<std::uint8_t>() + static_cast<std::size_t>(ai) * width,
              bm.data<std::uint8_t>() + static_cast<std::size_t>(bi) * width,
              width) == 0);
}

// One parametric-type case: a 5-row source column, the DataType every
// reachable op must preserve, and a byte width for value comparison.
struct Case {
    std::string name;
    Series source;
    DataType expected;
    std::size_t width;
};

// Every op that gathers/reindexes rows and so must carry type parameters
// through gather_column or dftu_series_slice.
void check_op(const Case& c, const Series& result, std::int64_t src_row,
              const std::string& op = "") {
    CAPTURE(op);
    REQUIRE(result.valid());
    CHECK(result.data_type() == c.expected);
    Series back = arrow_round_trip(result);
    CHECK(back.data_type() == c.expected);
    if (src_row >= 0) check_bytes_equal(back, 0, c.source, src_row, c.width);
}

void check_all_ops(const Case& c) {
    INFO(c.name);
    CHECK(c.source.data_type() == c.expected);

    check_op(c, c.source.head(3), 0, "head");
    check_op(c, c.source.tail(2), 3, "tail");
    check_op(c, c.source.take({4, 1}), 4, "take");
    check_op(c, c.source.slice(2, 2), 2, "slice");
    check_op(c, c.source.reverse(), -1, "reverse");
    check_op(c, c.source.sort(false), -1, "sort");
    check_op(c, c.source.unique(), -1, "unique");
    check_op(c, c.source.drop_nulls(), -1, "drop_nulls");

    std::vector<std::uint8_t> mask{0b0001'0101};  // keep rows 0, 2, 4
    Series m = Series::flat(TypeId::Bool, mask.data(), 5);
    check_op(c, c.source.filter(m), 0, "filter");

    DataFrame df;
    df.names = {"id", "col"};
    df.columns.push_back(
        Series::flat_i64(std::vector<std::int64_t>{0, 1, 2, 3, 4}.data(), 5));
    df.columns.push_back(c.source.share());
    check_op(c, df.head(3).column("col"), 0, "df.head");
    check_op(c, df.filter(m).column("col"), 0, "df.filter");
    check_op(c, df.slice(1, 2).column("col"), 1, "df.slice");
}

}  // namespace

TEST_SUITE("dataframe_type_parameter_propagation") {
    TEST_CASE("Timestamp(Nano, tz) keeps unit and timezone through every op") {
        std::vector<std::int64_t> v{1609459200000000000LL, 2, 3, 4, 5};
        check_all_ops({"Timestamp",
                       make_timestamp(v, TimeUnit::Nano, "America/Chicago"),
                       timestamp(TimeUnit::Nano, "America/Chicago"), 8});
    }

    TEST_CASE("Time32(Second) keeps its unit through every op") {
        std::vector<std::int32_t> v{1, 2, 3, 4, 5};
        DataType expected = time_of(TimeUnit::Second);
        REQUIRE(expected.id == TypeId::Time32);
        check_all_ops(
            {"Time32", make_time32(v, TimeUnit::Second), expected, 4});
    }

    TEST_CASE("Time64(Nano) keeps its unit through every op") {
        std::vector<std::int64_t> v{1, 2, 3, 4, 5};
        DataType expected = time_of(TimeUnit::Nano);
        REQUIRE(expected.id == TypeId::Time64);
        check_all_ops({"Time64", make_time64(v, TimeUnit::Nano), expected, 8});
    }

    TEST_CASE("Duration(Milli) keeps its unit through every op") {
        std::vector<std::int64_t> v{1, 2, 3, 4, 5};
        check_all_ops({"Duration", make_duration(v, TimeUnit::Milli),
                       duration_of(TimeUnit::Milli), 8});
    }

    TEST_CASE("Decimal128(38, 9) keeps precision and scale through every op") {
        std::vector<std::int64_t> v{1, 2, 3, 4, 5};
        check_all_ops({"Decimal128",
                       make_decimal(NANOARROW_TYPE_DECIMAL128, 128, 38, 9, v),
                       decimal128(38, 9), 16});
    }

    TEST_CASE("Decimal256(38, 9) keeps precision and scale through every op") {
        std::vector<std::int64_t> v{1, 2, 3, 4, 5};
        check_all_ops({"Decimal256",
                       make_decimal(NANOARROW_TYPE_DECIMAL256, 256, 38, 9, v),
                       decimal256(38, 9), 32});
    }

    TEST_CASE("FixedSizeBinary(7) keeps its width through every op") {
        std::vector<std::string> v{"aaaaaaa", "bbbbbbb", "ccccccc", "ddddddd",
                                   "eeeeeee"};
        check_all_ops({"FixedSizeBinary", make_fixed_size_binary(v, 7),
                       fixed_size_binary(7), 7});
    }

    TEST_CASE("group_by keeps a Timestamp key's unit and timezone") {
        // Two rows per key so the group actually folds, not just passes
        // through: id 0 and 2 share key row 0's value, id 1 and 3 share row
        // 1's value.
        std::vector<std::int64_t> ts{1609459200000000LL, 1700000000000000LL,
                                     1609459200000000LL, 1700000000000000LL};
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(make_timestamp(ts, TimeUnit::Micro, "UTC"));
        df.columns.push_back(Series::flat_i64(
            std::vector<std::int64_t>{10, 20, 30, 40}.data(), 4));

        DataFrame g = df.group_by("k", {GroupAgg{Agg::Sum, "v", "total"},
                                        GroupAgg{Agg::Count, "", "n"}});
        REQUIRE(g.num_rows() == 2);

        Series key = g.column("k");
        DataType expected = timestamp(TimeUnit::Micro, "UTC");
        CHECK(key.data_type() == expected);
        Series key_back = arrow_round_trip(key);
        CHECK(key_back.data_type() == expected);

        const std::int64_t* kd = key_back.data<std::int64_t>();
        const std::int64_t* total = g.column("total").data<std::int64_t>();
        const std::int64_t* n = g.column("n").data<std::int64_t>();
        REQUIRE(kd != nullptr);
        for (std::int64_t r = 0; r < 2; ++r) {
            CHECK(n[r] == 2);
            if (kd[r] == ts[0])
                CHECK(total[r] == 40);  // rows 0 and 2: 10 + 30
            else if (kd[r] == ts[1])
                CHECK(total[r] == 60);  // rows 1 and 3: 20 + 40
            else
                FAIL("unexpected group key value ", kd[r]);
        }
    }
}
#else
TEST_SUITE("dataframe_type_parameter_propagation") {
    TEST_CASE("Arrow disabled: nothing to exercise") { CHECK(true); }
}
#endif
