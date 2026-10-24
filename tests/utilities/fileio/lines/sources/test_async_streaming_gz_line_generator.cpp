#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::fileio::lines::sources;
using namespace dftracer::utils::utilities::fileio::lines;
using namespace dftracer::utils::coro;
using dftracer::utils::utilities::fileio::compress::GzipMemberCompressor;

namespace {

CoroTask<std::vector<std::string>> collect_lines(AsyncGenerator<Line> gen) {
    std::vector<std::string> result;
    while (auto line = co_await gen.next()) {
        result.push_back(std::string(line->content));
    }
    co_return result;
}

// Compress `text` into one gzip member and append it to `out`.
void append_member(std::vector<std::uint8_t>& out, const std::string& text) {
    GzipMemberCompressor comp;
    auto member = comp.compress_member(text.data(), text.size());
    REQUIRE(member.has_value());
    out.insert(out.end(), member->begin(), member->end());
}

std::string write_gz(const std::vector<std::uint8_t>& bytes) {
    fs::path p = dftu_utils_test::make_unique_test_path("stream_gz.pfw.gz");
    std::ofstream ofs(p, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ofs.close();
    return p.string();
}

}  // namespace

TEST_SUITE("AsyncStreamingGzLineGenerator") {
    TEST_CASE("reads all lines across multiple gzip members") {
        std::vector<std::uint8_t> file;
        append_member(file, "line1\nline2\nline3\n");
        append_member(file, "line4\nline5\n");
        append_member(file, "line6\n");
        const std::string path = write_gz(file);

        auto lines = collect_lines(async_streaming_gz_lines(path)).get();
        REQUIRE(lines.size() == 6);
        CHECK(lines[0] == "line1");
        CHECK(lines[2] == "line3");
        CHECK(lines[3] == "line4");
        CHECK(lines[5] == "line6");
    }

    TEST_CASE("honors a line range across members") {
        std::vector<std::uint8_t> file;
        append_member(file, "a\nb\nc\n");
        append_member(file, "d\ne\nf\n");
        const std::string path = write_gz(file);

        auto lines = collect_lines(async_streaming_gz_lines(path, 2, 5)).get();
        REQUIRE(lines.size() == 4);
        CHECK(lines.front() == "b");
        CHECK(lines.back() == "e");
    }

    TEST_CASE("handles a member whose last line has no trailing newline") {
        std::vector<std::uint8_t> file;
        append_member(file, "x\ny\n");
        append_member(file, "z");  // no trailing newline
        const std::string path = write_gz(file);

        auto lines = collect_lines(async_streaming_gz_lines(path)).get();
        REQUIRE(lines.size() == 3);
        CHECK(lines.back() == "z");
    }

    TEST_CASE("still reads a single-member file (streaming fallback)") {
        std::vector<std::uint8_t> file;
        append_member(file, "only1\nonly2\n");
        const std::string path = write_gz(file);

        auto lines = collect_lines(async_streaming_gz_lines(path)).get();
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "only1");
        CHECK(lines[1] == "only2");
    }
}
