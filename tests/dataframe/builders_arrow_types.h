// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_DATAFRAME_BUILDERS_ARROW_TYPES_H
#define DFTRACER_UTILS_TESTS_DATAFRAME_BUILDERS_ARROW_TYPES_H

// Arrow builders for the logical types that have no native Series constructor
// (they only ever enter the engine through Arrow import). Header-only so a
// doctest TU can use REQUIRE inside them.

#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/ipc.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <utility>

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

/// The bridge's export as caller-owned structs: the batch hands both structs
/// out and the caller releases them.
inline void export_series(const Series& col, ArrowSchema* schema,
                          ArrowArray* array) {
    auto batch = arrow::to_arrow(col);
    REQUIRE(batch.has_value());
    batch->release_into(schema, array);
}

/// The batch the bridge produced for `value`, which the test then reads or
/// imports.
inline arrow::Batch exported(const Series& col) {
    auto batch = arrow::to_arrow(col);
    REQUIRE(batch.has_value());
    return std::move(*batch);
}

inline arrow::Batch exported(const DataFrame& frame) {
    auto batch = arrow::to_arrow(frame);
    REQUIRE(batch.has_value());
    return std::move(*batch);
}

inline Series import_series(const ArrowSchema* schema,
                            const ArrowArray* array) {
    // A batch owns both structs, so it gets a copy of the caller's schema and
    // takes the array over, which leaves the caller's schema releasable.
    ArrowSchema copy;
    REQUIRE(ArrowSchemaDeepCopy(schema, &copy) == NANOARROW_OK);
    auto series = arrow::series_from_arrow(
        arrow::Batch::adopt(&copy, const_cast<ArrowArray*>(array)));
    REQUIRE(series.has_value());
    return std::move(*series);
}

inline DataFrame import_frame(const ArrowSchema* schema,
                              const ArrowArray* array) {
    ArrowSchema copy;
    REQUIRE(ArrowSchemaDeepCopy(schema, &copy) == NANOARROW_OK);
    auto frame = arrow::dataframe_from_arrow(
        arrow::Batch::adopt(&copy, const_cast<ArrowArray*>(array)));
    REQUIRE(frame.has_value());
    return std::move(*frame);
}

/// The batch's array for a test that rewrites it in place (a slice).
inline ArrowArray* mutable_array(arrow::Batch& batch) {
    return const_cast<ArrowArray*>(batch.array());
}

/// The IPC stream bytes of `frame`, as a Python consumer would write them.
inline dstd::Vector<std::uint8_t> export_ipc(const DataFrame& frame) {
    arrow::Writer writer = arrow::Writer::to_buffer();
    REQUIRE(writer.write(frame));
    REQUIRE(writer.finish());
    return writer.take_bytes();
}

/// The one record batch of an IPC stream, read back.
inline DataFrame import_ipc(const dstd::Vector<std::uint8_t>& bytes) {
    auto reader = arrow::Reader::from_bytes(
        dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()));
    REQUIRE(reader.has_value());
    auto next = reader->next();
    REQUIRE(next.has_value());
    REQUIRE(next->has_value());
    return std::move(**next);
}

inline Series make_float16(const dstd::Vector<float>& values) {
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
    return import_series(&b.schema, &b.array);
}

inline Series make_decimal(ArrowType arrow_type, int bitwidth,
                           const dstd::Vector<std::int64_t>& values) {
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
    return import_series(&b.schema, &b.array);
}

inline Series make_decimal128(const dstd::Vector<std::int64_t>& values) {
    return make_decimal(NANOARROW_TYPE_DECIMAL128, 128, values);
}

inline Series make_decimal256(const dstd::Vector<std::int64_t>& values) {
    return make_decimal(NANOARROW_TYPE_DECIMAL256, 256, values);
}

inline Series make_fixed_size_binary(const dstd::Vector<dstd::String>& values,
                                     std::int32_t width) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetTypeFixedSize(&b.schema,
                                        NANOARROW_TYPE_FIXED_SIZE_BINARY,
                                        width) == NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (const dstd::String& v : values) {
        REQUIRE(static_cast<std::int32_t>(v.size()) == width);
        ArrowBufferView bv;
        bv.data.data = v.data();
        bv.size_bytes = width;
        REQUIRE(ArrowArrayAppendBytes(&b.array, bv) == NANOARROW_OK);
    }
    finish_building(&b.array);
    return import_series(&b.schema, &b.array);
}

inline Series make_large_utf8(const dstd::Vector<dstd::String>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetType(&b.schema, NANOARROW_TYPE_LARGE_STRING) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (const dstd::String& v : values) {
        ArrowStringView sv{v.data(), static_cast<int64_t>(v.size())};
        REQUIRE(ArrowArrayAppendString(&b.array, sv) == NANOARROW_OK);
    }
    finish_building(&b.array);
    return import_series(&b.schema, &b.array);
}

inline Series make_large_binary(const dstd::Vector<dstd::String>& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetType(&b.schema, NANOARROW_TYPE_LARGE_BINARY) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (const dstd::String& v : values) {
        ArrowBufferView bv;
        bv.data.data = v.data();
        bv.size_bytes = static_cast<int64_t>(v.size());
        REQUIRE(ArrowArrayAppendBytes(&b.array, bv) == NANOARROW_OK);
    }
    finish_building(&b.array);
    return import_series(&b.schema, &b.array);
}

inline Series make_large_list_i64(
    const dstd::Vector<dstd::Vector<std::int64_t>>& rows) {
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
    return import_series(&b.schema, &b.array);
}

/// Every row must hold exactly `rows[0].size()` elements; that count becomes
/// the FixedSizeList width.
inline Series make_fixed_size_list_i64(
    const dstd::Vector<dstd::Vector<std::int64_t>>& rows) {
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
    return import_series(&b.schema, &b.array);
}

inline Series make_map_string_i64(
    const dstd::Vector<dstd::Vector<dstd::Pair<dstd::String, std::int64_t>>>&
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
    return import_series(&b.schema, &b.array);
}

}  // namespace dftracer::utils::dataframe::test_types

#endif  // DFTRACER_UTILS_TESTS_DATAFRAME_BUILDERS_ARROW_TYPES_H
