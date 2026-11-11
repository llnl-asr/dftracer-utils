// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

using namespace dftracer::utils;
using namespace dftracer::utils::fileio::lines::sources;
using namespace dftracer::utils::fileio::lines;
using namespace dftracer::utils::coro;

namespace {

struct Collected {
    dstd::Vector<dstd::String> lines;
    dstd::Optional<dstd::Error> error;

    dstd::Result<int> status() const {
        if (error) return dstd::Result<int>(*error);
        return 0;
    }
};

CoroTask<Collected> collect_lines(AsyncGenerator<dstd::Result<Line>> gen) {
    Collected out;
    while (auto next = co_await gen.next()) {
        if (!*next) {
            out.error = next->error();
            break;
        }
        out.lines.push_back(dstd::String((*next)->content));
    }
    co_return out;
}

dstd::Vector<dstd::String> lines_of(dstd::Result<Collected> collected) {
    auto out = dftu_utils_test::ok(std::move(collected));
    REQUIRE_MESSAGE(!out.error, out.error->format().c_str());
    return std::move(out.lines);
}

// Compress `text` into one gzip member and append it to `out`.
void append_member(dstd::Vector<std::uint8_t>& out, const dstd::String& text) {
    fileio::compress::GzipMemberCompressor comp;
    auto member = comp.compress_member(text.data(), text.size());
    REQUIRE(member.has_value());
    out.insert(out.end(), member->begin(), member->end());
}

dstd::String write_gz(const dstd::Vector<std::uint8_t>& bytes) {
    dstd::Path p = dftu_utils_test::make_unique_test_path("stream_gz.pfw.gz");
    dstd::String ofs;
    ofs.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    dstd::fs::write_file(dstd::PathView(p), ofs).discard();
    return dstd::String(p.str());
}

}  // namespace

TEST_SUITE("AsyncStreamingGzLineGenerator") {
    TEST_CASE("reads all lines across multiple gzip members") {
        dstd::Vector<std::uint8_t> file;
        append_member(file, "line1\nline2\nline3\n");
        append_member(file, "line4\nline5\n");
        append_member(file, "line6\n");
        const dstd::String path = write_gz(file);

        auto lines = lines_of(
            collect_lines(async_streaming_gz_lines(dstd::String(path))).get());
        REQUIRE(lines.size() == 6);
        CHECK(lines[0] == "line1");
        CHECK(lines[2] == "line3");
        CHECK(lines[3] == "line4");
        CHECK(lines[5] == "line6");
    }

    TEST_CASE("honors a line range across members") {
        dstd::Vector<std::uint8_t> file;
        append_member(file, "a\nb\nc\n");
        append_member(file, "d\ne\nf\n");
        const dstd::String path = write_gz(file);

        auto lines = lines_of(
            collect_lines(async_streaming_gz_lines(dstd::String(path), 2, 5))
                .get());
        REQUIRE(lines.size() == 4);
        CHECK(lines.front() == "b");
        CHECK(lines.back() == "e");
    }

    TEST_CASE("handles a member whose last line has no trailing newline") {
        dstd::Vector<std::uint8_t> file;
        append_member(file, "x\ny\n");
        append_member(file, "z");  // no trailing newline
        const dstd::String path = write_gz(file);

        auto lines = lines_of(
            collect_lines(async_streaming_gz_lines(dstd::String(path))).get());
        REQUIRE(lines.size() == 3);
        CHECK(lines.back() == "z");
    }

    TEST_CASE("still reads a single-member file (streaming fallback)") {
        dstd::Vector<std::uint8_t> file;
        append_member(file, "only1\nonly2\n");
        const dstd::String path = write_gz(file);

        auto lines = lines_of(
            collect_lines(async_streaming_gz_lines(dstd::String(path))).get());
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "only1");
        CHECK(lines[1] == "only2");
    }

    TEST_CASE("a file that is not gzip is an error") {
        dstd::Vector<std::uint8_t> file;
        for (char c : dstd::String("this is not gzip data at all, 0123456789"))
            file.push_back(static_cast<std::uint8_t>(c));
        const dstd::String path = write_gz(file);

        auto result = dftu_utils_test::ok(
            collect_lines(async_streaming_gz_lines(dstd::String(path))).get());
        dftu_utils_test::expect_error(
            result.status(), dftracer::utils::common::ErrorCode::Compression,
            "gzip member failed to decompress");
    }

    TEST_CASE("a cut last member is an error unless truncation is recovered") {
        dstd::String text;
        for (int i = 0; i < 2000; ++i) text += dstd::fmt<"line number {}\n">(i);
        dstd::Vector<std::uint8_t> file;
        append_member(file, "first\nsecond\n");
        append_member(file, text);
        file.resize(file.size() - 12);
        const dstd::String path = write_gz(file);

        auto strict = dftu_utils_test::ok(
            collect_lines(
                async_streaming_gz_lines(dstd::String(path), 0, 0, false))
                .get());
        REQUIRE(strict.lines.size() == 2);
        CHECK(strict.lines[0] == "first");
        CHECK(strict.lines[1] == "second");
        dftu_utils_test::expect_error(
            strict.status(), dftracer::utils::common::ErrorCode::Compression,
            "gzip member failed to decompress");

        auto recovered = lines_of(
            collect_lines(async_streaming_gz_lines(dstd::String(path))).get());
        REQUIRE(recovered.size() >= 2);
        CHECK(recovered[0] == "first");
        CHECK(recovered[1] == "second");
    }

    TEST_CASE("a missing file is an Io error") {
        auto result = dftu_utils_test::ok(
            collect_lines(
                async_streaming_gz_lines(dstd::String("/nonexistent/x.gz")))
                .get());
        CHECK(result.lines.empty());
        dftu_utils_test::expect_error(
            result.status(), dftracer::utils::common::ErrorCode::Io,
            "Cannot open compressed file: /nonexistent/x.gz (errno=2)");
    }
}
