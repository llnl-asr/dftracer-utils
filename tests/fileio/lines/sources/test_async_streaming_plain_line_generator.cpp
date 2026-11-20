// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/lines/sources/async_streaming_plain_line_generator.h>
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
    dstd::Vector<std::size_t> numbers;
    dstd::Optional<dstd::Error> error;

    dstd::Result<int> status() const {
        if (error) return dstd::Result<int>(*error);
        return 0;
    }
};

CoroTask<Collected> collect_lines(AsyncGenerator<dstd::Result<Line>> gen) {
    Collected out;
    while (true) {
        auto next = co_await gen.next();
        if (!next) break;
        if (!*next) {
            out.error = next->error();
            break;
        }
        out.lines.push_back(dstd::String((*next)->content));
        out.numbers.push_back((*next)->line_number);
    }
    co_return out;
}

Collected read(dstd::StringView path, std::size_t start = 0,
               std::size_t end = 0, std::size_t chunk = 1 << 20) {
    return dftu_utils_test::ok(
        collect_lines(
            async_streaming_plain_lines(dstd::String(path), start, end, chunk))
            .get());
}

dstd::String write_plain(dstd::StringView text) {
    dstd::Path p = dftu_utils_test::make_unique_test_path("plain_lines.pfw");
    dstd::fs::write_file(dstd::PathView(p), text).discard();
    return dstd::String(p.str());
}

}  // namespace

TEST_SUITE("AsyncStreamingPlainLineGenerator") {
    TEST_CASE("reads every line with 1-based numbers") {
        const dstd::String path = write_plain("a\nbb\nccc\n");
        auto out = read(path);
        REQUIRE_FALSE(out.error);
        REQUIRE(out.lines.size() == 3);
        CHECK(out.lines[0] == "a");
        CHECK(out.lines[2] == "ccc");
        CHECK(out.numbers[0] == 1);
        CHECK(out.numbers[2] == 3);
    }

    TEST_CASE("joins a line cut by a chunk boundary") {
        const dstd::String path = write_plain("alpha\nbeta\ngamma\ndelta\n");
        auto out = read(path, 0, 0, 4);
        REQUIRE_FALSE(out.error);
        REQUIRE(out.lines.size() == 4);
        CHECK(out.lines[0] == "alpha");
        CHECK(out.lines[1] == "beta");
        CHECK(out.lines[2] == "gamma");
        CHECK(out.lines[3] == "delta");
    }

    TEST_CASE("yields the last line without a trailing newline") {
        const dstd::String path = write_plain("x\ny\nz");
        auto out = read(path, 0, 0, 2);
        REQUIRE_FALSE(out.error);
        REQUIRE(out.lines.size() == 3);
        CHECK(out.lines.back() == "z");
        CHECK(out.numbers.back() == 3);
    }

    TEST_CASE("honors the start and end line") {
        const dstd::String path = write_plain("a\nb\nc\nd\ne\nf\n");
        auto out = read(path, 2, 5, 3);
        REQUIRE_FALSE(out.error);
        REQUIRE(out.lines.size() == 4);
        CHECK(out.lines.front() == "b");
        CHECK(out.lines.back() == "e");
        CHECK(out.numbers.front() == 2);
        CHECK(out.numbers.back() == 5);
    }

    TEST_CASE("keeps empty lines") {
        const dstd::String path = write_plain("a\n\nb\n");
        auto out = read(path);
        REQUIRE(out.lines.size() == 3);
        CHECK(out.lines[1].empty());
    }

    TEST_CASE("an empty file has no lines") {
        const dstd::String path = write_plain("");
        auto out = read(path);
        CHECK_FALSE(out.error);
        CHECK(out.lines.empty());
    }

    TEST_CASE("a missing file is a NotFound error") {
        auto out = read("/nonexistent/x.pfw");
        CHECK(out.lines.empty());
        dftu_utils_test::expect_error(
            out.status(), dftracer::utils::common::ErrorCode::NotFound,
            "File does not exist: /nonexistent/x.pfw");
    }
}
