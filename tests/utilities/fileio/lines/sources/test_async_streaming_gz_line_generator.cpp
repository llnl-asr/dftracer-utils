// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::fileio::lines::sources;
using namespace dftracer::utils::utilities::fileio::lines;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::utilities;

namespace {

CoroTask<dstd::Vector<dstd::String>> collect_lines(AsyncGenerator<Line> gen) {
    dstd::Vector<dstd::String> result;
    while (auto line = co_await gen.next()) {
        result.push_back(dstd::String(line->content));
    }
    co_return result;
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

        auto lines =
            collect_lines(async_streaming_gz_lines(dstd::String(path))).get();
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

        auto lines =
            collect_lines(async_streaming_gz_lines(dstd::String(path), 2, 5))
                .get();
        REQUIRE(lines.size() == 4);
        CHECK(lines.front() == "b");
        CHECK(lines.back() == "e");
    }

    TEST_CASE("handles a member whose last line has no trailing newline") {
        dstd::Vector<std::uint8_t> file;
        append_member(file, "x\ny\n");
        append_member(file, "z");  // no trailing newline
        const dstd::String path = write_gz(file);

        auto lines =
            collect_lines(async_streaming_gz_lines(dstd::String(path))).get();
        REQUIRE(lines.size() == 3);
        CHECK(lines.back() == "z");
    }

    TEST_CASE("still reads a single-member file (streaming fallback)") {
        dstd::Vector<std::uint8_t> file;
        append_member(file, "only1\nonly2\n");
        const dstd::String path = write_gz(file);

        auto lines =
            collect_lines(async_streaming_gz_lines(dstd::String(path))).get();
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "only1");
        CHECK(lines[1] == "only2");
    }
}
