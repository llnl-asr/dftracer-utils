// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/batch.h>
#include <dftracer/utils/arrow/internal.h>
#include <dftracer/utils/arrow/ipc.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <cstdint>
#include <cstdio>
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

dstd::Path temp_path(const dstd::StringView name) { return dstd::Path{name}; }

/// Three files of two record batches each, returns their paths.
dstd::Vector<dstd::Path> write_files() {
    dstd::Vector<dstd::Path> paths;
    const char* names[] = {"ipc_scan_0.arrow", "ipc_scan_1.arrow",
                           "ipc_scan_2.arrow"};
    for (const char* name : names) {
        dstd::Path path = temp_path(dstd::StringView(name));
        auto writer = Writer::open(path);
        REQUIRE(writer.has_value());
        REQUIRE(writer->write(sample_frame(3)));
        REQUIRE(writer->write(sample_frame(2)));
        REQUIRE(writer->finish());
        paths.push_back(path);
    }
    return paths;
}

/// Four files of three record batches of two rows; row `i` of batch `b` of file
/// `f` holds `f * 100 + b * 10 + i` in column `a`.
dstd::Vector<dstd::Path> write_ordered_files() {
    dstd::Vector<dstd::Path> paths;
    const char* names[] = {"ipc_order_0.arrow", "ipc_order_1.arrow",
                           "ipc_order_2.arrow", "ipc_order_3.arrow"};
    for (std::int64_t f = 0; f < 4; ++f) {
        dstd::Path path = temp_path(dstd::StringView(names[f]));
        auto writer = Writer::open(path);
        REQUIRE(writer.has_value());
        for (std::int64_t b = 0; b < 3; ++b) {
            dstd::Vector<std::int64_t> a;
            dstd::Vector<double> other;
            for (std::int64_t i = 0; i < 2; ++i) {
                a.push_back(f * 100 + b * 10 + i);
                other.push_back(static_cast<double>(i));
            }
            DataFrame frame;
            frame.names.push_back(dstd::SharedString{"a"});
            frame.names.push_back(dstd::SharedString{"b"});
            frame.columns.push_back(Series::flat_i64(a.data(), 2));
            frame.columns.push_back(Series::flat_f64(other.data(), 2));
            REQUIRE(writer->write(frame));
        }
        REQUIRE(writer->finish());
        paths.push_back(path);
    }
    return paths;
}

void append(dstd::Vector<std::uint8_t>& out, const void* from,
            std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(from);
    out.insert(out.end(), bytes, bytes + size);
}

/// A well-formed file that names one record batch block. With `inside` false
/// the block lies outside the file, which a reader must refuse instead of
/// reading it; with `inside` true it lies inside, over garbage bytes, which a
/// reader must refuse when it decodes the batch.
bool write_block_file(const dstd::Path& path, bool inside) {
    auto batch = to_arrow(sample_frame(2));
    if (!batch) return false;

    ArrowIpcEncoder encoder;
    ArrowError error;
    error.message[0] = '\0';
    ArrowBuffer schema_message;
    ArrowBufferInit(&schema_message);
    ArrowIpcEncoderInit(&encoder);
    bool ok = ArrowIpcEncoderEncodeSchema(&encoder, batch->schema(), &error) ==
                  NANOARROW_OK &&
              ArrowIpcEncoderFinalizeBuffer(&encoder, 1, &schema_message) ==
                  NANOARROW_OK;
    ArrowIpcEncoderReset(&encoder);

    ArrowIpcFooter footer;
    ArrowIpcFooterInit(&footer);
    if (ok)
        ok = ArrowSchemaDeepCopy(batch->schema(), &footer.schema) ==
             NANOARROW_OK;
    ArrowIpcFileBlock block{};
    block.offset =
        inside ? 8 + static_cast<std::int64_t>(schema_message.size_bytes)
               : 1 << 20;
    block.metadata_length = 64;
    block.body_length = 64;
    if (ok)
        ok = ArrowBufferAppend(&footer.record_batch_blocks, &block,
                               sizeof(block)) == NANOARROW_OK;

    ArrowBuffer footer_buffer;
    ArrowBufferInit(&footer_buffer);
    if (ok) {
        ArrowIpcEncoderInit(&encoder);
        ok = ArrowIpcEncoderEncodeFooter(&encoder, &footer, &error) ==
                 NANOARROW_OK &&
             ArrowIpcEncoderFinalizeBuffer(&encoder, 0, &footer_buffer) ==
                 NANOARROW_OK;
        ArrowIpcEncoderReset(&encoder);
    }
    ArrowIpcFooterReset(&footer);

    dstd::Vector<std::uint8_t> bytes;
    const std::uint8_t header[8] = {'A', 'R', 'R', 'O', 'W', '1', 0, 0};
    append(bytes, header, sizeof(header));
    append(bytes, schema_message.data,
           static_cast<std::size_t>(schema_message.size_bytes));
    if (inside) {
        std::uint8_t garbage[128];
        std::memset(garbage, 0xff, sizeof(garbage));
        append(bytes, garbage, sizeof(garbage));
    }
    const std::int32_t continuation = -1;
    const std::int32_t end_of_stream = 0;
    append(bytes, &continuation, sizeof(continuation));
    append(bytes, &end_of_stream, sizeof(end_of_stream));
    append(bytes, footer_buffer.data,
           static_cast<std::size_t>(footer_buffer.size_bytes));
    const auto footer_size =
        static_cast<std::int32_t>(footer_buffer.size_bytes);
    append(bytes, &footer_size, sizeof(footer_size));
    const std::uint8_t magic[6] = {'A', 'R', 'R', 'O', 'W', '1'};
    append(bytes, magic, sizeof(magic));

    ArrowBufferReset(&schema_message);
    ArrowBufferReset(&footer_buffer);
    if (!ok) return false;

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const bool written =
        std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    return written;
}

/// A column with its own buffers, so a value read works on a flat column.
Series gather(const Series& column) {
    return column.is_flat() ? column.share() : column.materialize();
}

Series column(const DataFrame& frame, const dstd::StringView name) {
    for (std::size_t i = 0; i < frame.names.size(); ++i)
        if (dstd::StringView(frame.names[i].view()) == name)
            return frame.columns[i].share();
    return Series{};
}

/// i64, f64, bool, string, dictionary and selection columns, 4 rows each.
DataFrame typed_frame() {
    const std::int64_t ints[4] = {10, -20, 30, 40};
    const double reals[4] = {1.5, -2.25, 0.0, 7.75};
    dstd::Vector<dstd::StringView> words;
    words.push_back(dstd::StringView("aa"));
    words.push_back(dstd::StringView("bb"));
    words.push_back(dstd::StringView("aa"));
    words.push_back(dstd::StringView("cc"));
    const dstd::Span<const dstd::StringView> text(words.data(), words.size());

    DataFrame frame;
    frame.names.push_back(dstd::SharedString{"i64"});
    frame.columns.push_back(Series::flat_i64(ints, 4));
    frame.names.push_back(dstd::SharedString{"f64"});
    frame.columns.push_back(Series::flat_f64(reals, 4));
    frame.names.push_back(dstd::SharedString{"b"});
    frame.columns.push_back(Series::flat_i64(ints, 4).gt(0));
    frame.names.push_back(dstd::SharedString{"s"});
    frame.columns.push_back(Series::strings(text));
    frame.names.push_back(dstd::SharedString{"d"});
    frame.columns.push_back(Series::strings(text).dictionary_encode());
    frame.names.push_back(dstd::SharedString{"sel"});
    frame.columns.push_back(
        Series::flat_i64(ints, 4).filter(Series::flat_i64(ints, 4).gt(-1000)));
    return frame;
}

void check_typed_frame(const DataFrame& out) {
    REQUIRE(out.names.size() == 6);
    REQUIRE(out.num_rows() == 4);

    Series i64 = gather(column(out, dstd::StringView("i64")));
    REQUIRE(i64.valid());
    CHECK(i64.type() == TypeId::Int64);
    const auto ints = i64.values<std::int64_t>();
    REQUIRE(ints.size() == 4);
    CHECK(ints[0] == 10);
    CHECK(ints[1] == -20);
    CHECK(ints[2] == 30);
    CHECK(ints[3] == 40);

    Series f64 = gather(column(out, dstd::StringView("f64")));
    REQUIRE(f64.valid());
    CHECK(f64.type() == TypeId::Float64);
    const auto reals = f64.values<double>();
    REQUIRE(reals.size() == 4);
    CHECK(reals[0] == 1.5);
    CHECK(reals[1] == -2.25);
    CHECK(reals[2] == 0.0);
    CHECK(reals[3] == 7.75);

    Series bits = gather(column(out, dstd::StringView("b")));
    REQUIRE(bits.valid());
    CHECK(bits.type() == TypeId::Bool);
    const auto bytes = bits.values<std::uint8_t>();
    REQUIRE(!bytes.empty());
    CHECK(((bytes[0] >> 0) & 1) == 1);
    CHECK(((bytes[0] >> 1) & 1) == 0);
    CHECK(((bytes[0] >> 2) & 1) == 1);
    CHECK(((bytes[0] >> 3) & 1) == 1);

    Series text = column(out, dstd::StringView("s"));
    REQUIRE(text.valid());
    CHECK(text.type() == TypeId::String);
    CHECK(text.string_at(0) == "aa");
    CHECK(text.string_at(1) == "bb");
    CHECK(text.string_at(2) == "aa");
    CHECK(text.string_at(3) == "cc");

    Series dict = column(out, dstd::StringView("d"));
    REQUIRE(dict.valid());
    CHECK(dict.string_at(0) == "aa");
    CHECK(dict.string_at(1) == "bb");
    CHECK(dict.string_at(2) == "aa");
    CHECK(dict.string_at(3) == "cc");

    Series sel = gather(column(out, dstd::StringView("sel")));
    REQUIRE(sel.valid());
    const auto picked = sel.values<std::int64_t>();
    REQUIRE(picked.size() == 4);
    CHECK(picked[0] == 10);
    CHECK(picked[1] == -20);
    CHECK(picked[2] == 30);
    CHECK(picked[3] == 40);
}

}  // namespace

TEST_CASE("ipc: a missing file is an error") {
    auto reader =
        Reader::open(temp_path(dstd::StringView("no_such_ipc.arrow")));
    CHECK(!reader);
}

TEST_CASE("ipc: truncated bytes are an error") {
    const std::uint8_t bytes[4] = {0xff, 0xff, 0xff, 0xff};
    auto reader = Reader::from_bytes(dstd::Span<const std::uint8_t>(bytes, 4));
    CHECK(!reader);
}

TEST_CASE("ipc: a cut stream reports an error, not a crash") {
    Writer writer = Writer::to_buffer();
    REQUIRE(writer.write(sample_frame(4)));
    REQUIRE(writer.write(sample_frame(4)));
    REQUIRE(writer.finish());
    auto bytes = writer.take_bytes();
    REQUIRE(bytes.size() > 64);

    // A cut 12 bytes from the end lands inside the last message, never on a
    // message boundary (messages are 8-byte aligned), so one read must fail.
    auto reader = Reader::from_bytes(
        dstd::Span<const std::uint8_t>(bytes.data(), bytes.size() - 12));
    bool errored = !reader.has_value();
    if (!errored) {
        for (int i = 0; i < 8; ++i) {
            auto next = reader->next();
            if (!next) {
                errored = true;
                break;
            }
            if (!next->has_value()) break;
        }
    }
    CHECK(errored);
}

TEST_CASE("ipc: a batch that claims more body than is left is a parse error") {
    Writer writer = Writer::to_buffer();
    REQUIRE(writer.write(sample_frame(1000)));
    REQUIRE(writer.write(sample_frame(1000)));
    REQUIRE(writer.finish());
    auto bytes = writer.take_bytes();
    REQUIRE(bytes.size() > 256);

    // The cut lands inside the last batch's body, so its message still verifies
    // and its length runs past the bytes that are left.
    const std::size_t keep = bytes.size() - 72;
    auto reader =
        Reader::from_bytes(dstd::Span<const std::uint8_t>(bytes.data(), keep));
    REQUIRE(reader.has_value());
    auto first = reader->next();
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK((**first).num_rows() == 1000);
    auto second = reader->next();
    REQUIRE(!second.has_value());
    CHECK(second.error().is(dstd::Condition::Parse));
}

TEST_CASE("ipc: a footer block outside the file is a parse error") {
    dstd::Path path = temp_path(dstd::StringView("bad_block.arrow"));
    REQUIRE(write_block_file(path, false));
    auto reader = Reader::open(path);
    REQUIRE(!reader);
    CHECK(reader.error().is(dstd::Condition::Parse));
}

TEST_CASE("ipc: a bad batch in a later file fails the scan") {
    dstd::Vector<dstd::Path> paths = write_files();
    dstd::Path bad = temp_path(dstd::StringView("ipc_garbage.arrow"));
    REQUIRE(write_block_file(bad, true));
    paths.push_back(bad);

    // Every footer is well formed, so the scan starts and fails on the decode.
    auto lazy =
        scan_ipc(dstd::Span<const dstd::Path>(paths.data(), paths.size()));
    REQUIRE(lazy.has_value());
    auto collected = default_runtime().block_on(lazy->collect());
    REQUIRE(!collected);
    CHECK(collected.error().is(dstd::Condition::Parse));
}

TEST_CASE("ipc: a typed frame round trips through a buffer") {
    DataFrame frame = typed_frame();
    Writer writer = Writer::to_buffer();
    REQUIRE(writer.write(frame));
    REQUIRE(writer.finish());
    auto bytes = writer.take_bytes();

    auto reader = Reader::from_bytes(
        dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()));
    REQUIRE(reader.has_value());
    auto next = reader->next();
    REQUIRE(next.has_value());
    REQUIRE(next->has_value());
    check_typed_frame(**next);
    auto end = reader->next();
    REQUIRE(end.has_value());
    CHECK(!end->has_value());
}

TEST_CASE("ipc: a typed frame round trips through a file") {
    dstd::Path path = temp_path(dstd::StringView("typed.arrow"));
    {
        auto writer = Writer::open(path);
        REQUIRE(writer.has_value());
        REQUIRE(writer->write(typed_frame()));
        REQUIRE(writer->finish());
    }

    auto reader = Reader::open(path);
    REQUIRE(reader.has_value());
    auto next = reader->next();
    REQUIRE(next.has_value());
    REQUIRE(next->has_value());
    check_typed_frame(**next);
}

TEST_CASE("ipc: scan_ipc reads every file and honours a projection") {
    dstd::Vector<dstd::Path> paths = write_files();
    auto lazy =
        scan_ipc(dstd::Span<const dstd::Path>(paths.data(), paths.size()));
    REQUIRE(lazy.has_value());

    dstd::Vector<dstd::String> projection;
    projection.push_back(dstd::String("a"));
    auto collected =
        default_runtime().block_on(lazy->select(projection).collect());
    REQUIRE(collected.has_value());
    CHECK(collected->names.size() == 1);
    CHECK(dstd::StringView(collected->names[0].view()) == "a");
    CHECK(collected->num_rows() == 15);
}

TEST_CASE("ipc: scan_ipc gives every row in file order") {
    dstd::Vector<dstd::Path> paths = write_ordered_files();
    auto lazy =
        scan_ipc(dstd::Span<const dstd::Path>(paths.data(), paths.size()));
    REQUIRE(lazy.has_value());

    dstd::Vector<dstd::String> projection;
    projection.push_back(dstd::String("a"));
    auto collected =
        default_runtime().block_on(lazy->select(projection).collect());
    REQUIRE(collected.has_value());
    REQUIRE(collected->names.size() == 1);
    REQUIRE(collected->num_rows() == 24);

    Series a = gather(collected->columns[0]);
    REQUIRE(a.valid());
    const auto values = a.values<std::int64_t>();
    REQUIRE(values.size() == 24);
    std::size_t at = 0;
    for (std::int64_t f = 0; f < 4; ++f) {
        for (std::int64_t b = 0; b < 3; ++b) {
            for (std::int64_t i = 0; i < 2; ++i) {
                INFO("row " << at << " of " << values.size());
                CHECK(values[at] == f * 100 + b * 10 + i);
                ++at;
            }
        }
    }
}

TEST_CASE("ipc: a read-ahead holds no more than the budget, or one big file") {
    dstd::Vector<dstd::Path> paths;
    const char* small_names[] = {"ipc_budget_0.arrow", "ipc_budget_1.arrow",
                                 "ipc_budget_2.arrow"};
    for (const char* name : small_names) {
        dstd::Path path = temp_path(dstd::StringView(name));
        auto writer = Writer::open(path);
        REQUIRE(writer.has_value());
        REQUIRE(writer->write(sample_frame(2)));
        REQUIRE(writer->finish());
        paths.push_back(path);
    }
    dstd::Path big = temp_path(dstd::StringView("ipc_budget_big.arrow"));
    {
        auto writer = Writer::open(big);
        REQUIRE(writer.has_value());
        REQUIRE(writer->write(sample_frame(4000)));
        REQUIRE(writer->finish());
    }
    paths.push_back(big);

    const std::uint64_t small = dstd::fs::file_size(paths[0].view()).value();
    const std::uint64_t largest = dstd::fs::file_size(big.view()).value();
    REQUIRE(largest > 3 * small);
    // Exactly the three small files, so the big one may start only once they
    // are done.
    const std::uint64_t budget = 3 * small;

    auto lazy =
        scan_ipc(dstd::Span<const dstd::Path>(paths.data(), paths.size()));
    REQUIRE(lazy.has_value());
    arrow::detail::reset_ipc_in_flight_bytes_for_test();
    auto collected =
        default_runtime().block_on(lazy->memory_budget(budget).collect());
    REQUIRE(collected.has_value());
    CHECK(collected->num_rows() == 3 * 2 + 4000);

    const std::uint64_t peak =
        arrow::detail::peak_ipc_in_flight_bytes_for_test();
    CHECK(peak <= largest);
    CHECK(peak > budget);
}

TEST_CASE("ipc: a file with other columns is an error") {
    dstd::Vector<dstd::Path> paths = write_files();
    {
        dstd::Path path = temp_path(dstd::StringView("ipc_other.arrow"));
        auto writer = Writer::open(path);
        REQUIRE(writer.has_value());
        dstd::Vector<std::int64_t> c{1, 2};
        DataFrame frame;
        frame.names.push_back(dstd::SharedString{"a"});
        frame.names.push_back(dstd::SharedString{"c"});
        frame.columns.push_back(Series::flat_i64(c.data(), 2));
        frame.columns.push_back(Series::flat_i64(c.data(), 2));
        REQUIRE(writer->write(frame));
        REQUIRE(writer->finish());
        paths.push_back(path);
    }

    auto lazy =
        scan_ipc(dstd::Span<const dstd::Path>(paths.data(), paths.size()));
    REQUIRE(lazy.has_value());
    auto collected = default_runtime().block_on(lazy->collect());
    REQUIRE(!collected);
    CHECK(collected.error().is(dstd::Condition::InvalidArgument));
}

TEST_CASE("ipc: a reader opens a file that holds the stream format") {
    // A `.arrow` file written from `to_ipc` bytes holds a stream, not a file.
    Writer writer = Writer::to_buffer();
    REQUIRE(writer.write(sample_frame(5)));
    REQUIRE(writer.finish());
    auto bytes = writer.take_bytes();
    dstd::Path path = temp_path(dstd::StringView("stream_format.arrow"));
    {
        std::FILE* out = std::fopen(path.c_str(), "wb");
        REQUIRE(out != nullptr);
        REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), out) ==
                bytes.size());
        std::fclose(out);
    }

    auto reader = Reader::open(path);
    REQUIRE(reader.has_value());
    auto next = reader->next();
    REQUIRE(next.has_value());
    REQUIRE(next->has_value());
    CHECK((**next).num_rows() == 5);
}

TEST_CASE("ipc: read_ipc is scan_ipc collected") {
    dstd::Vector<dstd::Path> paths = write_files();
    auto frame =
        read_ipc(dstd::Span<const dstd::Path>(paths.data(), paths.size()));
    REQUIRE(frame.has_value());
    CHECK(frame->names.size() == 2);
    CHECK(frame->num_rows() == 15);
}

TEST_CASE("ipc: an empty path list is an error") {
    dstd::Vector<dstd::Path> paths;
    auto lazy =
        scan_ipc(dstd::Span<const dstd::Path>(paths.data(), paths.size()));
    CHECK(!lazy);
}
