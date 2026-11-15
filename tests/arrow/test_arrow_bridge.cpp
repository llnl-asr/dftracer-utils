// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/batch.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstring>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::arrow;
using namespace dftracer::utils::dataframe;

namespace {

DataFrame sample_frame(std::int64_t rows) {
    dstd::Vector<std::int64_t> a;
    dstd::Vector<double> b;
    for (std::int64_t i = 0; i < rows; ++i) {
        a.push_back(i);
        b.push_back(static_cast<double>(i) * 0.5);
    }
    DataFrame frame;
    frame.names.push_back(dstd::SharedString{"a"});
    frame.names.push_back(dstd::SharedString{"b"});
    frame.columns.push_back(Series::flat_i64(a.data(), rows));
    frame.columns.push_back(Series::flat_f64(b.data(), rows));
    return frame;
}

}  // namespace

TEST_CASE("arrow: a column round trips through the C data interface") {
    dstd::Vector<std::int64_t> values{1, 2, 3};
    Series column = Series::flat_i64(values.data(), 3);

    auto batch = to_arrow(column);
    REQUIRE(batch.has_value());
    CHECK(batch->num_rows() == 3);
    CHECK(batch->is_valid());

    auto back = series_from_arrow(std::move(*batch));
    REQUIRE(back.has_value());
    CHECK(back->length() == 3);
    CHECK(back->type() == TypeId::Int64);
}

TEST_CASE("arrow: a frame round trips as a struct batch") {
    DataFrame frame = sample_frame(5);

    auto batch = to_arrow(frame);
    REQUIRE(batch.has_value());
    CHECK(batch->num_rows() == 5);
    CHECK(batch->schema()->n_children == 2);

    auto back = dataframe_from_arrow(std::move(*batch));
    REQUIRE(back.has_value());
    CHECK(back->names.size() == 2);
    CHECK(back->num_rows() == 5);
}

TEST_CASE("arrow: an empty batch is an error, not a crash") {
    Batch empty;
    CHECK(!empty.is_valid());
    CHECK(empty.num_rows() == 0);
    auto back = series_from_arrow(std::move(empty));
    CHECK(!back);
}

TEST_CASE("arrow: a primitive batch is an error and its array is released") {
    struct Hand {
        int* released;
    };
    int released = 0;
    Hand hand{&released};
    const void* buffers[2] = {nullptr, nullptr};

    ArrowSchema schema;
    REQUIRE(ArrowSchemaInitFromType(&schema, NANOARROW_TYPE_INT64) ==
            NANOARROW_OK);
    ArrowArray array;
    std::memset(&array, 0, sizeof(array));
    array.length = 3;
    array.null_count = 3;
    array.n_buffers = 2;
    array.buffers = buffers;
    array.private_data = &hand;
    array.release = [](ArrowArray* a) {
        ++*static_cast<Hand*>(a->private_data)->released;
        a->release = nullptr;
    };

    auto back = dataframe_from_arrow(Batch::adopt(&schema, &array));
    REQUIRE(!back);
    CHECK(back.error().is(dstd::Condition::InvalidArgument));
    CHECK(released == 1);
}

TEST_CASE("arrow: a released batch is empty") {
    auto batch = to_arrow(sample_frame(2));
    REQUIRE(batch.has_value());
    ArrowSchema schema{};
    ArrowArray array{};
    batch->release_into(&schema, &array);
    CHECK(!batch->is_valid());
    CHECK(schema.release != nullptr);
    CHECK(array.release != nullptr);
    schema.release(&schema);
    array.release(&array);
}
