// SPDX-License-Identifier: MIT
// The SDK frame builder (plugins/plugin/frame.h): a column borrows the plugin's
// buffer with no copy, the buffer is freed once when the last column that uses
// it dies, and a frame refuses columns of different lengths.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/plugins/plugin/frame.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

/// Bytes held by every pool of the process, so a buffer that is never freed
/// shows up as a difference.
std::size_t live_bytes() {
    return memory::detail::Manager::get().root().live();
}

}  // namespace

TEST_CASE("a column borrows the plugin buffer with no copy") {
    const std::size_t before = live_bytes();
    const std::int64_t* buffer = nullptr;
    OwnedDataFrame frame;
    {
        dstd::Vector<std::int64_t> values{10, 20, 30, 40};
        buffer = values.data();
        FrameBuilder builder;
        builder.add("id", Column::from_int64(std::move(values)));
        auto built = builder.build();
        REQUIRE(built.has_value());
        frame = std::move(*built);
    }
    REQUIRE(frame);
    CHECK(dftu_dataframe_num_rows(frame.get()) == 4);
    CHECK(dftu_dataframe_num_columns(frame.get()) == 1);

    dftu_series* column = dftu_dataframe_column(frame.get(), "id");
    REQUIRE(column != nullptr);
    CHECK(dftu_series_type(column) == DFTU_TYPE_INT64);
    CHECK(dftu_series_length(column) == 4);
    // The very bytes the vector held: a copy would sit elsewhere.
    CHECK(dftu_series_data(column) == buffer);
    CHECK(static_cast<const std::int64_t*>(dftu_series_data(column))[3] == 40);
    dftu_series_free(column);

    CHECK(live_bytes() > before);   // the frame keeps the buffer alive
    frame.reset();
    CHECK(live_bytes() == before);  // ... and frees it with the last user
}

TEST_CASE("the engine's columns free the buffer once, with the last user") {
    const std::size_t before = live_bytes();
    {
        dstd::Vector<std::int64_t> values{1, 2, 3, 4, 5, 6};
        Column base = Column::from_int64(std::move(values));
        REQUIRE(base);
        CHECK(live_bytes() > before);

        // A slice and a filter both share the buffer they came from.
        Column slice(dftu_series_slice(base.get(), 2, 3));
        REQUIRE(slice);
        dstd::Vector<std::uint8_t> keep{0b00000101};
        Column mask = Column::from_bool(std::move(keep), 3);
        REQUIRE(mask);
        Column filtered(dftu_series_filter(slice.get(), mask.get()));
        REQUIRE(filtered);
        CHECK(dftu_series_length(slice.get()) == 3);
        CHECK(dftu_series_length(filtered.get()) == 2);

        base.reset();
        mask.reset();
        CHECK(live_bytes() > before);  // the slice still holds the buffer
        slice.reset();
        CHECK(live_bytes() > before);  // and so does the filter
        filtered.reset();
        CHECK(live_bytes() == before);
    }
    CHECK(live_bytes() == before);
}

TEST_CASE("a borrowed buffer is released exactly once") {
    const std::size_t before = live_bytes();
    int released = 0;
    {
        dstd::Vector<std::int64_t> data{7, 8, 9};
        Column column(dftu_series_new_flat_borrowed(
            DFTU_TYPE_INT64, data.data(), 3, nullptr,
            [](void* ctx) { ++*static_cast<int*>(ctx); }, &released));
        REQUIRE(column);
        // A share is a second user of the one buffer.
        Column shared(dftu_series_share(column.get()));
        REQUIRE(shared);
        CHECK(released == 0);
        column.reset();
        CHECK(released == 0);
        shared.reset();
        CHECK(released == 1);
    }
    CHECK(released == 1);
    CHECK(live_bytes() == before);
}

TEST_CASE("a length mismatch is an error and leaks nothing") {
    const std::size_t before = live_bytes();
    {
        FrameBuilder builder;
        builder.add("id",
                    Column::from_int64(dstd::Vector<std::int64_t>{1, 2, 3}));
        builder.add("dur", Column::from_double(dstd::Vector<double>{1.5, 2.5}));
        auto built = builder.build();
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error().condition() == dstd::Condition::InvalidArgument);
    }
    CHECK(live_bytes() == before);  // both buffers went with the builder

    FrameBuilder empty;
    auto none = empty.build();
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().condition() == dstd::Condition::InvalidArgument);
}

TEST_CASE("a string column round-trips its values") {
    const std::size_t before = live_bytes();
    {
        dstd::Vector<std::int32_t> offsets{0, 3, 3, 7};
        dstd::String bytes("abcdefg");
        // Row 1 is null, and its empty offset range stays empty.
        dstd::Vector<std::uint8_t> validity{0b00000101};
        Column column = Column::from_strings(
            std::move(offsets), std::move(bytes), std::move(validity));
        REQUIRE(column);
        CHECK(dftu_series_type(column.get()) == DFTU_TYPE_STRING);
        CHECK(dftu_series_length(column.get()) == 3);
        CHECK(dftu_series_null_count(column.get()) == 1);
        CHECK(dftu_series_is_null(column.get(), 1) == 1);
        CHECK(dftu_series_is_null(column.get(), 0) == 0);

        std::int64_t len = -1;
        const char* text = dftu_series_string_at(column.get(), 0, &len);
        REQUIRE(text != nullptr);
        CHECK(len == 3);
        CHECK(dstd::StringView(text, static_cast<std::size_t>(len)) == "abc");
        text = dftu_series_string_at(column.get(), 2, &len);
        REQUIRE(text != nullptr);
        CHECK(dstd::StringView(text, static_cast<std::size_t>(len)) == "defg");

        FrameBuilder builder;
        builder.add("name", std::move(column));
        auto built = builder.build();
        REQUIRE(built.has_value());
        OwnedDataFrame frame = std::move(*built);
        dftu_series* named = dftu_dataframe_column(frame.get(), "name");
        REQUIRE(named != nullptr);
        CHECK(dftu_series_length(named) == 3);
        dftu_series_free(named);
    }
    CHECK(live_bytes() == before);
}

TEST_CASE("a static column copies nothing and frees nothing") {
    static const std::int64_t TABLE[] = {100, 200, 300};
    const std::size_t before = live_bytes();
    {
        FrameBuilder builder;
        builder.add("v", Column::from_static_int64(TABLE));
        auto built = builder.build();
        REQUIRE(built.has_value());
        OwnedDataFrame frame = std::move(*built);
        dftu_series* column = dftu_dataframe_column(frame.get(), "v");
        REQUIRE(column != nullptr);
        CHECK(dftu_series_data(column) == TABLE);
        CHECK(static_cast<const std::int64_t*>(dftu_series_data(column))[2] ==
              300);
        dftu_series_free(column);
    }
    CHECK(live_bytes() == before);
}

TEST_CASE("every static column takes a span and borrows its bytes") {
    static const std::int32_t I32[] = {1, 2};
    static const std::uint64_t U64[] = {7, 8};
    static const double F64[] = {1.5, 2.5};
    static const std::int64_t STAMPS[] = {10, 20};
    static const std::uint8_t BITS[] = {0b00000010};
    static const std::int32_t OFFSETS32[] = {0, 3, 7};
    static const std::int64_t OFFSETS64[] = {0, 3, 7};
    static const char BYTES[] = "abcdefg";

    const std::size_t before = live_bytes();
    {
        FrameBuilder builder;
        builder.add("i32", Column::from_static_int32(I32));
        builder.add("u64", Column::from_static_uint64(U64));
        builder.add("f64", Column::from_static_double(F64));
        builder.add("stamp", Column::from_static_timestamp(STAMPS));
        builder.add("dur", Column::from_static_duration(STAMPS));
        auto built = builder.build();
        REQUIRE(built.has_value());
        OwnedDataFrame frame = std::move(*built);
        CHECK(dftu_dataframe_num_rows(frame.get()) == 2);

        dftu_series* i32 = dftu_dataframe_column(frame.get(), "i32");
        REQUIRE(i32 != nullptr);
        CHECK(dftu_series_type(i32) == DFTU_TYPE_INT32);
        CHECK(dftu_series_data(i32) == I32);
        dftu_series_free(i32);

        dftu_series* u64 = dftu_dataframe_column(frame.get(), "u64");
        REQUIRE(u64 != nullptr);
        CHECK(dftu_series_data(u64) == U64);
        dftu_series_free(u64);

        dftu_series* f64 = dftu_dataframe_column(frame.get(), "f64");
        REQUIRE(f64 != nullptr);
        CHECK(dftu_series_data(f64) == F64);
        CHECK(static_cast<const double*>(dftu_series_data(f64))[1] == 2.5);
        dftu_series_free(f64);

        dftu_series* stamp = dftu_dataframe_column(frame.get(), "stamp");
        REQUIRE(stamp != nullptr);
        CHECK(dftu_series_type(stamp) == DFTU_TYPE_TIMESTAMP);
        CHECK(dftu_series_data(stamp) == STAMPS);
        dftu_series_free(stamp);

        dftu_series* dur = dftu_dataframe_column(frame.get(), "dur");
        REQUIRE(dur != nullptr);
        CHECK(dftu_series_type(dur) == DFTU_TYPE_DURATION);
        dftu_series_free(dur);
    }
    CHECK(live_bytes() == before);

    // A Bool column takes its rows as well, because its bits are packed.
    {
        FrameBuilder builder;
        builder.add("b", Column::from_static_bool(BITS, 2));
        auto built = builder.build();
        REQUIRE(built.has_value());
        OwnedDataFrame frame = std::move(*built);
        dftu_series* bits = dftu_dataframe_column(frame.get(), "b");
        REQUIRE(bits != nullptr);
        CHECK(dftu_series_type(bits) == DFTU_TYPE_BOOL);
        CHECK(dftu_series_length(bits) == 2);
        CHECK(dftu_series_data(bits) == BITS);
        dftu_series_free(bits);
    }
    // Bits too short for the rows give a null column, not a read past them.
    CHECK_FALSE(Column::from_static_bool(BITS, 9));

    // A String column takes its offsets and its bytes, and the rows are the
    // offsets minus one.
    {
        FrameBuilder builder;
        builder.add("s", Column::from_static_strings(
                             dstd::Span<const std::int32_t>(OFFSETS32),
                             dstd::StringView(BYTES, 7)));
        auto built = builder.build();
        REQUIRE(built.has_value());
        OwnedDataFrame frame = std::move(*built);
        dftu_series* text = dftu_dataframe_column(frame.get(), "s");
        REQUIRE(text != nullptr);
        CHECK(dftu_series_type(text) == DFTU_TYPE_STRING);
        CHECK(dftu_series_length(text) == 2);
        CHECK(dftu_series_offsets(text) == OFFSETS32);
        CHECK(dftu_series_data(text) == BYTES);
        std::int64_t len = -1;
        const char* row = dftu_series_string_at(text, 1, &len);
        REQUIRE(row != nullptr);
        CHECK(dstd::StringView(row, static_cast<std::size_t>(len)) == "defg");
        dftu_series_free(text);
    }

    // int64 offsets make a LargeString column.
    {
        FrameBuilder builder;
        builder.add("s", Column::from_static_strings(
                             dstd::Span<const std::int64_t>(OFFSETS64),
                             dstd::StringView(BYTES, 7)));
        auto built = builder.build();
        REQUIRE(built.has_value());
        OwnedDataFrame frame = std::move(*built);
        dftu_series* text = dftu_dataframe_column(frame.get(), "s");
        REQUIRE(text != nullptr);
        CHECK(dftu_series_type(text) == DFTU_TYPE_LARGE_STRING);
        CHECK(dftu_series_length(text) == 2);
        CHECK(dftu_series_offsets64(text) == OFFSETS64);
        dftu_series_free(text);
    }

    // No offsets is no column, not a column of -1 rows.
    CHECK_FALSE(Column::from_static_strings(dstd::Span<const std::int32_t>(),
                                            dstd::StringView()));
    CHECK_FALSE(Column::from_static_strings(dstd::Span<const std::int64_t>(),
                                            dstd::StringView()));
}

TEST_CASE("a validity bitmap too short for the rows is refused") {
    // Three rows need one byte, and one byte is there, so this one is fine.
    Column ok = Column::from_int64(dstd::Vector<std::int64_t>{1, 2, 3},
                                   dstd::Vector<std::uint8_t>{0b00000010});
    REQUIRE(ok);
    CHECK(dftu_series_null_count(ok.get()) == 2);

    // Nine rows need two bytes; one byte is short, so the column is refused
    // instead of read past.
    Column short_validity = Column::from_int64(
        dstd::Vector<std::int64_t>(9, 0), dstd::Vector<std::uint8_t>{0xFF});
    CHECK_FALSE(short_validity);
}
