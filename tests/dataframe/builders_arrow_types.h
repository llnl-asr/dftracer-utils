#ifndef DFTRACER_UTILS_TESTS_DATAFRAME_BUILDERS_ARROW_TYPES_H
#define DFTRACER_UTILS_TESTS_DATAFRAME_BUILDERS_ARROW_TYPES_H

// Arrow builders for the logical types that have no native Series constructor
// (they only ever enter the engine through Arrow import). Header-only so a
// doctest TU can use REQUIRE inside them.

#include <doctest/doctest.h>

// clang-format off
#include <nanoarrow/nanoarrow.h>
#include <dftracer/utils/dataframe/arrow.h>
// clang-format on

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe::test_types {

struct BuiltArrow {
    ArrowSchema schema{};
    ArrowArray array{};
    ~BuiltArrow() {
        if (array.release) array.release(&array);
        if (schema.release) schema.release(&schema);
    }
};

inline void finish_building(ArrowArray* a) {
    ArrowError err;
    REQUIRE(ArrowArrayFinishBuildingDefault(a, &err) == NANOARROW_OK);
}

inline Series make_float16(const std::vector<float>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetType(&b.schema, NANOARROW_TYPE_HALF_FLOAT) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (float v : values)
        REQUIRE(ArrowArrayAppendDouble(&b.array, static_cast<double>(v)) ==
                NANOARROW_OK);
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

inline Series make_decimal(ArrowType arrow_type, int bitwidth,
                           const std::vector<std::int64_t>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeDecimal(&b.schema, arrow_type, 38, 0) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (std::int64_t v : values) {
        struct ArrowDecimal dec;
        ArrowDecimalInit(&dec, bitwidth, 38, 0);
        ArrowDecimalSetInt(&dec, v);
        REQUIRE(ArrowArrayAppendDecimal(&b.array, &dec) == NANOARROW_OK);
    }
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

inline Series make_decimal128(const std::vector<std::int64_t>& values) {
    return make_decimal(NANOARROW_TYPE_DECIMAL128, 128, values);
}

inline Series make_decimal256(const std::vector<std::int64_t>& values) {
    return make_decimal(NANOARROW_TYPE_DECIMAL256, 256, values);
}

inline Series make_fixed_size_binary(const std::vector<std::string>& values,
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
        REQUIRE(static_cast<std::int32_t>(v.size()) == width);
        ArrowBufferView bv;
        bv.data.data = v.data();
        bv.size_bytes = width;
        REQUIRE(ArrowArrayAppendBytes(&b.array, bv) == NANOARROW_OK);
    }
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

inline Series make_large_utf8(const std::vector<std::string>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetType(&b.schema, NANOARROW_TYPE_LARGE_STRING) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (const std::string& v : values) {
        ArrowStringView sv{v.data(), static_cast<int64_t>(v.size())};
        REQUIRE(ArrowArrayAppendString(&b.array, sv) == NANOARROW_OK);
    }
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

inline Series make_large_binary(const std::vector<std::string>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetType(&b.schema, NANOARROW_TYPE_LARGE_BINARY) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (const std::string& v : values) {
        ArrowBufferView bv;
        bv.data.data = v.data();
        bv.size_bytes = static_cast<int64_t>(v.size());
        REQUIRE(ArrowArrayAppendBytes(&b.array, bv) == NANOARROW_OK);
    }
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

inline Series make_large_list_i64(
    const std::vector<std::vector<std::int64_t>>& rows) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetType(&b.schema, NANOARROW_TYPE_LARGE_LIST) ==
            NANOARROW_OK);
    REQUIRE(ArrowSchemaSetType(b.schema.children[0], NANOARROW_TYPE_INT64) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (const auto& row : rows) {
        for (std::int64_t v : row)
            REQUIRE(ArrowArrayAppendInt(b.array.children[0], v) ==
                    NANOARROW_OK);
        REQUIRE(ArrowArrayFinishElement(&b.array) == NANOARROW_OK);
    }
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

/// Every row must hold exactly `rows[0].size()` elements; that count becomes
/// the FixedSizeList width.
inline Series make_fixed_size_list_i64(
    const std::vector<std::vector<std::int64_t>>& rows) {
    REQUIRE_FALSE(rows.empty());
    const std::int32_t width = static_cast<std::int32_t>(rows[0].size());
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeFixedSize(&b.schema,
                                        NANOARROW_TYPE_FIXED_SIZE_LIST,
                                        width) == NANOARROW_OK);
    REQUIRE(ArrowSchemaSetType(b.schema.children[0], NANOARROW_TYPE_INT64) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (const auto& row : rows) {
        REQUIRE(static_cast<std::int32_t>(row.size()) == width);
        for (std::int64_t v : row)
            REQUIRE(ArrowArrayAppendInt(b.array.children[0], v) ==
                    NANOARROW_OK);
        REQUIRE(ArrowArrayFinishElement(&b.array) == NANOARROW_OK);
    }
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

inline Series make_map_string_i64(
    const std::vector<std::vector<std::pair<std::string, std::int64_t>>>&
        rows) {
    BuiltArrow b;
    REQUIRE(ArrowSchemaInitFromType(&b.schema, NANOARROW_TYPE_MAP) ==
            NANOARROW_OK);
    ArrowSchema* entries = b.schema.children[0];
    REQUIRE(ArrowSchemaSetType(entries->children[0], NANOARROW_TYPE_STRING) ==
            NANOARROW_OK);
    REQUIRE(ArrowSchemaSetType(entries->children[1], NANOARROW_TYPE_INT64) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    ArrowArray* entries_arr = b.array.children[0];
    for (const auto& row : rows) {
        for (const auto& kv : row) {
            ArrowStringView sv{kv.first.data(),
                               static_cast<int64_t>(kv.first.size())};
            REQUIRE(ArrowArrayAppendString(entries_arr->children[0], sv) ==
                    NANOARROW_OK);
            REQUIRE(ArrowArrayAppendInt(entries_arr->children[1], kv.second) ==
                    NANOARROW_OK);
            REQUIRE(ArrowArrayFinishElement(entries_arr) == NANOARROW_OK);
        }
        REQUIRE(ArrowArrayFinishElement(&b.array) == NANOARROW_OK);
    }
    finish_building(&b.array);
    return Series::from_arrow(&b.schema, &b.array);
}

}  // namespace dftracer::utils::dataframe::test_types

#endif  // DFTRACER_UTILS_TESTS_DATAFRAME_BUILDERS_ARROW_TYPES_H
