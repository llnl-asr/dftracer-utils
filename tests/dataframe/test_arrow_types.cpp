#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
// clang-format off
// nanoarrow.h must precede arrow.h: arrow.h (via plugins/arrow_abi.h) defines
// ArrowSchema/ArrowArray under the ARROW_C_DATA_INTERFACE guard but not
// ArrowArrayStream, which nanoarrow.h's inline helpers then need complete.
#include <nanoarrow/nanoarrow.h>
#include <dftracer/utils/dataframe/arrow.h>
// clang-format on

using dftracer::utils::dataframe::DataType;
using dftracer::utils::dataframe::decimal128;
using dftracer::utils::dataframe::duration_of;
using dftracer::utils::dataframe::fixed_size_binary;
using dftracer::utils::dataframe::fixed_size_list_of;
using dftracer::utils::dataframe::large_list_of;
using dftracer::utils::dataframe::list_of;
using dftracer::utils::dataframe::map_of;
using dftracer::utils::dataframe::OwnedArrow;
using dftracer::utils::dataframe::scalar;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::struct_of;
using dftracer::utils::dataframe::time_of;
using dftracer::utils::dataframe::timestamp;
using dftracer::utils::dataframe::TimeUnit;
using dftracer::utils::dataframe::TypeId;

namespace {

/// Owns a nanoarrow ArrowSchema+ArrowArray pair built through the low-level
/// append API, releasing both on destruction.
struct BuiltArrow {
    ArrowSchema schema{};
    ArrowArray array{};
    ~BuiltArrow() {
        if (array.release) array.release(&array);
        if (schema.release) schema.release(&schema);
    }
};

void append_i64(ArrowArray* a, std::int64_t v) {
    REQUIRE(ArrowArrayAppendInt(a, v) == NANOARROW_OK);
}
void append_str(ArrowArray* a, const std::string& s) {
    ArrowStringView sv{s.data(), static_cast<std::int64_t>(s.size())};
    REQUIRE(ArrowArrayAppendString(a, sv) == NANOARROW_OK);
}
void finish(ArrowArray* a) {
    ArrowError err;
    REQUIRE(ArrowArrayFinishBuildingDefault(a, &err) == NANOARROW_OK);
}

}  // namespace

TEST_SUITE("dataframe_arrow_types") {
    TEST_CASE("Timestamp(Micro, UTC) round-trips schema and values") {
        BuiltArrow b;
        ArrowSchemaInit(&b.schema);
        REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIMESTAMP,
                                           NANOARROW_TIME_UNIT_MICRO,
                                           "UTC") == NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        append_i64(&b.array, 1700000000000000LL);
        append_i64(&b.array, 1700000001000000LL);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        DataType expected = timestamp(TimeUnit::Micro, "UTC");
        CHECK(s.data_type() == expected);
        CHECK(s.data_type().time_unit == TimeUnit::Micro);
        CHECK(s.data_type().timezone == "UTC");
        const std::int64_t* d = s.data<std::int64_t>();
        REQUIRE(d != nullptr);
        CHECK(d[0] == 1700000000000000LL);
        CHECK(d[1] == 1700000001000000LL);

        // The zone is type metadata: with_timezone swaps it, shares the
        // buffers and moves no tick; "" makes the column naive.
        Series tokyo = s.with_timezone("Asia/Tokyo");
        REQUIRE(tokyo.valid());
        CHECK(tokyo.data_type() == timestamp(TimeUnit::Micro, "Asia/Tokyo"));
        CHECK(tokyo.data<std::int64_t>() == d);
        CHECK(tokyo.data<std::int64_t>()[1] == 1700000001000000LL);
        CHECK(s.data_type().timezone == "UTC");
        CHECK(tokyo.with_timezone("").data_type() ==
              timestamp(TimeUnit::Micro));
        CHECK_FALSE(Series::flat_i64(d, 2).with_timezone("UTC").valid());

        OwnedArrow out = s.to_arrow();
        CHECK(static_cast<bool>(out));
        Series back = Series::from_arrow(out.schema(), out.array());
        REQUIRE(back.valid());
        CHECK(back.data_type() == expected);
    }

    TEST_CASE("Naive Timestamp(Nano) round-trips with no timezone") {
        BuiltArrow b;
        ArrowSchemaInit(&b.schema);
        REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIMESTAMP,
                                           NANOARROW_TIME_UNIT_NANO,
                                           nullptr) == NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        append_i64(&b.array, 42);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        DataType expected = timestamp(TimeUnit::Nano);
        CHECK(s.data_type() == expected);
        CHECK(s.data_type().timezone.empty());
    }

    TEST_CASE("Date32 round-trips as Int32 physical storage") {
        BuiltArrow b;
        REQUIRE(ArrowSchemaInitFromType(&b.schema, NANOARROW_TYPE_DATE32) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        append_i64(&b.array, 19000);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        CHECK(s.data_type() == scalar(TypeId::Date32));
        CHECK(dftracer::utils::dataframe::physical_type(TypeId::Date32) ==
              TypeId::Int32);
        const std::int32_t* d = s.data<std::int32_t>();
        REQUIRE(d != nullptr);
        CHECK(d[0] == 19000);

        OwnedArrow out = s.to_arrow();
        Series back = Series::from_arrow(out.schema(), out.array());
        REQUIRE(back.valid());
        CHECK(back.data_type() == scalar(TypeId::Date32));
    }

    TEST_CASE("Duration(Second) round-trips") {
        BuiltArrow b;
        ArrowSchemaInit(&b.schema);
        REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_DURATION,
                                           NANOARROW_TIME_UNIT_SECOND,
                                           nullptr) == NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        append_i64(&b.array, 3600);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        DataType expected = duration_of(TimeUnit::Second);
        CHECK(s.data_type() == expected);

        OwnedArrow out = s.to_arrow();
        Series back = Series::from_arrow(out.schema(), out.array());
        REQUIRE(back.valid());
        CHECK(back.data_type() == expected);
    }

    TEST_CASE("Decimal128(38, 9) round-trips precision and scale") {
        BuiltArrow b;
        ArrowSchemaInit(&b.schema);
        REQUIRE(ArrowSchemaSetTypeDecimal(&b.schema, NANOARROW_TYPE_DECIMAL128,
                                          38, 9) == NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        struct ArrowDecimal dec;
        ArrowDecimalInit(&dec, 128, 38, 9);
        ArrowDecimalSetInt(&dec, 123456789);
        REQUIRE(ArrowArrayAppendDecimal(&b.array, &dec) == NANOARROW_OK);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        DataType expected = decimal128(38, 9);
        CHECK(s.data_type() == expected);
        CHECK(s.data_type().decimal_precision == 38);
        CHECK(s.data_type().decimal_scale == 9);
        REQUIRE(s.length() == 1);

        OwnedArrow out = s.to_arrow();
        Series back = Series::from_arrow(out.schema(), out.array());
        REQUIRE(back.valid());
        CHECK(back.data_type() == expected);
        REQUIRE(back.length() == 1);
    }

    TEST_CASE("FixedSizeBinary(4) round-trips size and bytes") {
        BuiltArrow b;
        ArrowSchemaInit(&b.schema);
        REQUIRE(ArrowSchemaSetTypeFixedSize(&b.schema,
                                            NANOARROW_TYPE_FIXED_SIZE_BINARY,
                                            4) == NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        ArrowBufferView bv;
        bv.data.data = "abcd";
        bv.size_bytes = 4;
        REQUIRE(ArrowArrayAppendBytes(&b.array, bv) == NANOARROW_OK);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        DataType expected = fixed_size_binary(4);
        CHECK(s.data_type() == expected);
        CHECK(s.data_type().fixed_size == 4);
        const char* d = s.data<char>();
        REQUIRE(d != nullptr);
        CHECK(std::string(d, 4) == "abcd");

        OwnedArrow out = s.to_arrow();
        Series back = Series::from_arrow(out.schema(), out.array());
        REQUIRE(back.valid());
        CHECK(back.data_type() == expected);
    }

    TEST_CASE("LargeString round-trips 64-bit-offset values") {
        BuiltArrow b;
        REQUIRE(ArrowSchemaInitFromType(
                    &b.schema, NANOARROW_TYPE_LARGE_STRING) == NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        append_str(&b.array, "hello");
        append_str(&b.array, "large string world");
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        CHECK(s.data_type() == scalar(TypeId::LargeString));
        REQUIRE(s.length() == 2);
        // LargeString's 64-bit offsets are a distinct physical layout from
        // String's; string_at() only reads the 32-bit-offset layout, so
        // check the raw byte buffer directly.
        const char* d = s.data<char>();
        REQUIRE(d != nullptr);
        CHECK(std::string(d, 23) == "hellolarge string world");

        OwnedArrow out = s.to_arrow();
        Series back = Series::from_arrow(out.schema(), out.array());
        REQUIRE(back.valid());
        CHECK(back.data_type() == scalar(TypeId::LargeString));
        REQUIRE(back.length() == 2);
        const char* d2 = back.data<char>();
        REQUIRE(d2 != nullptr);
        CHECK(std::string(d2, 23) == "hellolarge string world");

        // Confirm the exported array is a genuine 64-bit-offset Arrow array
        // (not a copy down to 32-bit offsets): read its offsets buffer raw.
        ArrowSchemaView view;
        ArrowError err;
        REQUIRE(ArrowSchemaViewInit(&view, out.schema(), &err) == NANOARROW_OK);
        CHECK(view.type == NANOARROW_TYPE_LARGE_STRING);
        const auto* offs =
            static_cast<const std::int64_t*>(out.array()->buffers[1]);
        REQUIRE(offs != nullptr);
        CHECK(offs[0] == 0);
        CHECK(offs[1] == 5);
        CHECK(offs[2] == 23);
    }

    TEST_CASE("LargeList<Int64> round-trips") {
        BuiltArrow b;
        REQUIRE(ArrowSchemaInitFromType(&b.schema, NANOARROW_TYPE_LARGE_LIST) ==
                NANOARROW_OK);
        REQUIRE(ArrowSchemaSetType(b.schema.children[0],
                                   NANOARROW_TYPE_INT64) == NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        append_i64(b.array.children[0], 1);
        append_i64(b.array.children[0], 2);
        REQUIRE(ArrowArrayFinishElement(&b.array) == NANOARROW_OK);
        append_i64(b.array.children[0], 3);
        REQUIRE(ArrowArrayFinishElement(&b.array) == NANOARROW_OK);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        CHECK(s.data_type() == large_list_of(scalar(TypeId::Int64)));
        REQUIRE(s.length() == 2);

        OwnedArrow out = s.to_arrow();
        Series back = Series::from_arrow(out.schema(), out.array());
        REQUIRE(back.valid());
        CHECK(back.data_type() == large_list_of(scalar(TypeId::Int64)));
        REQUIRE(back.length() == 2);
    }

    TEST_CASE("FixedSizeList<Int64>(2) round-trips element count") {
        BuiltArrow b;
        ArrowSchemaInit(&b.schema);
        REQUIRE(ArrowSchemaSetTypeFixedSize(&b.schema,
                                            NANOARROW_TYPE_FIXED_SIZE_LIST,
                                            2) == NANOARROW_OK);
        REQUIRE(ArrowSchemaSetType(b.schema.children[0],
                                   NANOARROW_TYPE_INT64) == NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        append_i64(b.array.children[0], 10);
        append_i64(b.array.children[0], 11);
        REQUIRE(ArrowArrayFinishElement(&b.array) == NANOARROW_OK);
        append_i64(b.array.children[0], 20);
        append_i64(b.array.children[0], 21);
        REQUIRE(ArrowArrayFinishElement(&b.array) == NANOARROW_OK);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        DataType expected = fixed_size_list_of(scalar(TypeId::Int64), 2);
        CHECK(s.data_type() == expected);
        CHECK(s.data_type().fixed_size == 2);
        REQUIRE(s.length() == 2);

        OwnedArrow out = s.to_arrow();
        Series back = Series::from_arrow(out.schema(), out.array());
        REQUIRE(back.valid());
        CHECK(back.data_type() == expected);
    }

    TEST_CASE("Map<String, Int64> round-trips as List<Struct{key, value}>") {
        BuiltArrow b;
        REQUIRE(ArrowSchemaInitFromType(&b.schema, NANOARROW_TYPE_MAP) ==
                NANOARROW_OK);
        ArrowSchema* entries = b.schema.children[0];
        REQUIRE(ArrowSchemaSetType(entries->children[0],
                                   NANOARROW_TYPE_STRING) == NANOARROW_OK);
        REQUIRE(ArrowSchemaSetType(entries->children[1],
                                   NANOARROW_TYPE_INT64) == NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        ArrowArray* entries_arr = b.array.children[0];
        append_str(entries_arr->children[0], "a");
        append_i64(entries_arr->children[1], 1);
        REQUIRE(ArrowArrayFinishElement(entries_arr) == NANOARROW_OK);
        append_str(entries_arr->children[0], "b");
        append_i64(entries_arr->children[1], 2);
        REQUIRE(ArrowArrayFinishElement(entries_arr) == NANOARROW_OK);
        REQUIRE(ArrowArrayFinishElement(&b.array) == NANOARROW_OK);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        DataType expected =
            map_of(scalar(TypeId::String), scalar(TypeId::Int64));
        CHECK(s.data_type() == expected);
        CHECK(dftracer::utils::dataframe::physical_type(TypeId::Map) ==
              TypeId::List);
        REQUIRE(s.length() == 1);

        OwnedArrow out = s.to_arrow();
        Series back = Series::from_arrow(out.schema(), out.array());
        REQUIRE(back.valid());
        CHECK(back.data_type() == expected);
        REQUIRE(back.length() == 1);
    }

    TEST_CASE("List<Timestamp(Micro, UTC)> nests logical parameters") {
        BuiltArrow b;
        REQUIRE(ArrowSchemaInitFromType(&b.schema, NANOARROW_TYPE_LIST) ==
                NANOARROW_OK);
        REQUIRE(ArrowSchemaSetTypeDateTime(
                    b.schema.children[0], NANOARROW_TYPE_TIMESTAMP,
                    NANOARROW_TIME_UNIT_MICRO, "UTC") == NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        append_i64(b.array.children[0], 100);
        append_i64(b.array.children[0], 200);
        REQUIRE(ArrowArrayFinishElement(&b.array) == NANOARROW_OK);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        DataType expected = list_of(timestamp(TimeUnit::Micro, "UTC"));
        CHECK(s.data_type() == expected);
        CHECK(s.data_type().fields[0].type.time_unit == TimeUnit::Micro);
        CHECK(s.data_type().fields[0].type.timezone == "UTC");

        OwnedArrow out = s.to_arrow();
        Series back = Series::from_arrow(out.schema(), out.array());
        REQUIRE(back.valid());
        CHECK(back.data_type() == expected);
    }

    TEST_CASE("Struct{ts: Timestamp, amount: Decimal128} nests both phases") {
        BuiltArrow b;
        REQUIRE(ArrowSchemaInitFromType(&b.schema, NANOARROW_TYPE_STRUCT) ==
                NANOARROW_OK);
        REQUIRE(ArrowSchemaAllocateChildren(&b.schema, 2) == NANOARROW_OK);
        ArrowSchemaInit(b.schema.children[0]);
        REQUIRE(ArrowSchemaSetTypeDateTime(
                    b.schema.children[0], NANOARROW_TYPE_TIMESTAMP,
                    NANOARROW_TIME_UNIT_MICRO, "UTC") == NANOARROW_OK);
        REQUIRE(ArrowSchemaSetName(b.schema.children[0], "ts") == NANOARROW_OK);
        ArrowSchemaInit(b.schema.children[1]);
        REQUIRE(ArrowSchemaSetTypeDecimal(b.schema.children[1],
                                          NANOARROW_TYPE_DECIMAL128, 38,
                                          9) == NANOARROW_OK);
        REQUIRE(ArrowSchemaSetName(b.schema.children[1], "amount") ==
                NANOARROW_OK);

        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        append_i64(b.array.children[0], 1700000000000000LL);
        struct ArrowDecimal dec;
        ArrowDecimalInit(&dec, 128, 38, 9);
        ArrowDecimalSetInt(&dec, 42);
        REQUIRE(ArrowArrayAppendDecimal(b.array.children[1], &dec) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayFinishElement(&b.array) == NANOARROW_OK);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        DataType expected =
            struct_of({{"ts", timestamp(TimeUnit::Micro, "UTC"), true},
                       {"amount", decimal128(38, 9), true}});
        CHECK(s.data_type() == expected);

        OwnedArrow out = s.to_arrow();
        Series back = Series::from_arrow(out.schema(), out.array());
        REQUIRE(back.valid());
        CHECK(back.data_type() == expected);
    }

    TEST_CASE("An unsupported Arrow type fails loudly, not silently") {
        BuiltArrow b;
        REQUIRE(ArrowSchemaInitFromType(
                    &b.schema, NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        struct ArrowInterval iv;
        ArrowIntervalInit(&iv, NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO);
        REQUIRE(ArrowArrayAppendInterval(&b.array, &iv) == NANOARROW_OK);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        CHECK_FALSE(s.valid());
    }

    TEST_CASE("No collected field ever reports TypeId::Unknown") {
        BuiltArrow b;
        ArrowSchemaInit(&b.schema);
        REQUIRE(ArrowSchemaSetTypeDateTime(&b.schema, NANOARROW_TYPE_TIMESTAMP,
                                           NANOARROW_TIME_UNIT_MICRO,
                                           nullptr) == NANOARROW_OK);
        REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
                NANOARROW_OK);
        REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
        append_i64(&b.array, 7);
        finish(&b.array);

        Series s = Series::from_arrow(&b.schema, &b.array);
        REQUIRE(s.valid());
        CHECK(s.data_type().id != TypeId::Unknown);
    }
}
#else
TEST_SUITE("dataframe_arrow_types") {
    TEST_CASE("Arrow disabled: nothing to exercise") { CHECK(true); }
}
#endif
