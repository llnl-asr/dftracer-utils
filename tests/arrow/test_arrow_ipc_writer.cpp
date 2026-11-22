// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/arrow/internal.h>
#include <dftracer/utils/arrow/ipc.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

dstd::Path temp_path(dstd::StringView name) { return dstd::Path{name}; }

}  // namespace

TEST_CASE("ipc: a buffer writer round trips several batches") {
    DataFrame frame = sample_frame(4);
    Writer writer = Writer::to_buffer();
    for (int i = 0; i < 3; ++i) REQUIRE(writer.write(frame));
    REQUIRE(writer.finish());
    auto bytes = writer.take_bytes();
    REQUIRE(!bytes.empty());

    auto reader = Reader::from_bytes(
        dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()));
    REQUIRE(reader.has_value());

    std::int64_t batches = 0;
    std::int64_t rows = 0;
    while (true) {
        auto next = reader->next();
        REQUIRE(next.has_value());
        if (!next->has_value()) break;
        ++batches;
        rows += (**next).num_rows();
    }
    CHECK(batches == 3);
    CHECK(rows == 12);
}

TEST_CASE("ipc: a file writer round trips") {
    dstd::Path path = temp_path("writer_frame.arrow");
    DataFrame frame = sample_frame(6);
    {
        auto writer = Writer::open(path);
        REQUIRE(writer.has_value());
        REQUIRE(writer->write(frame));
        REQUIRE(writer->finish());
    }

    auto reader = Reader::open(path);
    REQUIRE(reader.has_value());
    auto next = reader->next();
    REQUIRE(next.has_value());
    REQUIRE(next->has_value());
    CHECK((**next).num_rows() == 6);
    auto end = reader->next();
    REQUIRE(end.has_value());
    CHECK(!end->has_value());
}

TEST_CASE("ipc: a string slice inside a struct is written with its own bytes") {
    constexpr std::int64_t ROWS = 10000;
    dstd::Vector<dstd::String> values;
    for (std::int64_t i = 0; i < ROWS; ++i)
        values.push_back(dstd::String(200, 'a') +
                         dstd::String{std::to_string(i).c_str()});
    const Series big = Series::strings(values);
    dstd::Vector<std::int64_t> k;
    for (std::int64_t i = 0; i < 10; ++i) k.push_back(i);
    dstd::Vector<Series> fields;
    fields.push_back(big.slice(9990, 10));
    fields.push_back(Series::flat_i64(k.data(), 10));
    REQUIRE(dftu_series_offsets(fields[0].handle())[0] != 0);
    DataFrame frame;
    frame.names.push_back(dstd::SharedString{"st"});
    frame.columns.push_back(Series::structs({"s", "k"}, std::move(fields)));

    for (Compression compression : {Compression::None, Compression::Zstd}) {
        WriterOptions options;
        options.compression = compression;
        Writer writer = Writer::to_buffer(options);
        REQUIRE(writer.write(frame));
        REQUIRE(writer.finish());
        auto bytes = writer.take_bytes();
        CHECK(bytes.size() < 6000);

        auto reader = Reader::from_bytes(
            dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()));
        REQUIRE(reader.has_value());
        auto next = reader->next();
        REQUIRE(next.has_value());
        REQUIRE(next->has_value());
        const Series st = (*next)->columns[0].share();
        const Series s = st.child(0);
        REQUIRE(s.length() == 10);
        for (std::int64_t i = 0; i < 10; ++i)
            CHECK(s.string_at(i) == values[9990 + i]);
    }
}

TEST_CASE("ipc: a writer with no frame is an error") {
    auto writer = Writer::open(temp_path("writer_empty.arrow"));
    REQUIRE(writer.has_value());
    CHECK(!writer->finish());
}

TEST_CASE("ipc: a written file starts with the 8-byte ARROW1 magic") {
    dstd::Path path = temp_path("writer_magic.arrow");
    {
        auto writer = Writer::open(path);
        REQUIRE(writer.has_value());
        REQUIRE(writer->write(sample_frame(2)));
        REQUIRE(writer->finish());
    }

    std::uint8_t head[8] = {0};
    std::FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    REQUIRE(std::fread(head, 1, sizeof(head), file) == sizeof(head));
    std::fclose(file);

    const std::uint8_t expected[8] = {'A', 'R', 'R', 'O', 'W', '1', 0, 0};
    CHECK(std::memcmp(head, expected, sizeof(expected)) == 0);
}

TEST_CASE("ipc: a frame with other column types is an error") {
    Writer writer = Writer::to_buffer();
    REQUIRE(writer.write(sample_frame(2)));

    dstd::Vector<double> a{1.0, 2.0};
    dstd::Vector<std::int64_t> b{1, 2};
    DataFrame swapped;
    swapped.names.push_back(dstd::SharedString{"a"});
    swapped.names.push_back(dstd::SharedString{"b"});
    swapped.columns.push_back(Series::flat_f64(a.data(), 2));
    swapped.columns.push_back(Series::flat_i64(b.data(), 2));

    auto written = writer.write(swapped);
    REQUIRE(!written);
    CHECK(written.error().is(dstd::Condition::InvalidArgument));
}

TEST_CASE("ipc: a failed first write leaves the writer usable") {
    Writer writer = Writer::to_buffer();
    arrow::detail::fail_next_ipc_write_for_test();
    CHECK(!writer.write(sample_frame(2)));
    REQUIRE(writer.write(sample_frame(2)));
    REQUIRE(writer.write(sample_frame(2)));
    REQUIRE(writer.finish());
    auto bytes = writer.take_bytes();

    auto reader = Reader::from_bytes(
        dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()));
    REQUIRE(reader.has_value());
    std::int64_t rows = 0;
    while (true) {
        auto next = reader->next();
        REQUIRE(next.has_value());
        if (!next->has_value()) break;
        rows += (**next).num_rows();
    }
    CHECK(rows == 4);
}

TEST_CASE("ipc: writing after finish is an error") {
    Writer writer = Writer::to_buffer();
    REQUIRE(writer.write(sample_frame(2)));
    REQUIRE(writer.finish());
    CHECK(!writer.write(sample_frame(2)));
}

TEST_CASE("ipc: zstd compression round trips") {
    DataFrame frame = sample_frame(8);
    WriterOptions options;
    options.compression = Compression::Zstd;
    Writer writer = Writer::to_buffer(options);
    REQUIRE(writer.write(frame));
    REQUIRE(writer.finish());
    auto bytes = writer.take_bytes();

    auto reader = Reader::from_bytes(
        dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()));
    REQUIRE(reader.has_value());
    auto next = reader->next();
    REQUIRE(next.has_value());
    REQUIRE(next->has_value());
    CHECK((**next).num_rows() == 8);
}
