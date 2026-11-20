// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_ARROW_ARROW_HELPERS_H
#define DFTRACER_UTILS_TESTS_ARROW_ARROW_HELPERS_H

// Conversion helpers shared by the Arrow bridge tests: build an Arrow array by
// hand, take a Batch apart, and round-trip a Series or DataFrame through the
// bridge or the IPC stream. Header-only so a doctest TU can use REQUIRE inside
// them.

#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/ipc.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
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
inline DataFrame import_ipc(dstd::Span<const std::uint8_t> bytes) {
    auto reader = arrow::Reader::from_bytes(bytes);
    REQUIRE(reader.has_value());
    auto next = reader->next();
    REQUIRE(next.has_value());
    REQUIRE(next->has_value());
    return std::move(**next);
}

}  // namespace dftracer::utils::dataframe::test_types

#endif  // DFTRACER_UTILS_TESTS_ARROW_ARROW_HELPERS_H
