// SPDX-License-Identifier: MIT
// The SDK frame builder (plugins/plugin/frame.h): a column borrows the plugin's
// buffer with no copy, the buffer is freed once when the last column that uses
// it dies, and a frame refuses columns of different lengths.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
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
        builder.add("v", Column::from_static_int64(TABLE, 3));
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
