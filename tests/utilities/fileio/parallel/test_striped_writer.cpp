// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdio>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::utilities::fileio::parallel;

namespace {

dstd::String tmp_path(const char* name) {
    return dstd::String(dftu_utils_test::make_unique_test_path(name).str());
}

dstd::String read_all(const dstd::String& path) {
    return dstd::fs::read_file(dstd::PathView(path)).value_or(dstd::String());
}

dstd::ByteView sv(const dstd::String& s) {
    return dstd::ByteView(s.data(), s.size());
}

}  // namespace

TEST_CASE("StripedWriter - header, chunks, footer land in single file") {
    dstd::String path = tmp_path("test_striped_basic.txt");
    std::remove(path.c_str());

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        auto w = make_striped_writer();
        if (co_await w->open(dstd::String(path), 4, false, nullptr) != 0)
            co_return 1;
        if (co_await w->write_header(sv("HDR\n")) != 0) co_return 2;
        if (co_await w->write_chunk(0, sv("worker0\n")) != 0) co_return 3;
        if (co_await w->write_chunk(1, sv("worker1\n")) != 0) co_return 4;
        if (co_await w->write_chunk(2, sv("worker2\n")) != 0) co_return 5;
        if (co_await w->write_footer(sv("END\n")) != 0) co_return 6;
        if (co_await w->close() != 0) co_return 7;
        co_return 0;
    };

    CHECK(dftu_utils_test::ok(runtime.submit(task(), "striped_basic").get()) ==
          0);

    auto content = read_all(path);
    // All bytes must be present regardless of interleave order.
    CHECK(content.size() ==
          dstd::String("HDR\nworker0\nworker1\nworker2\nEND\n").size());

    dstd::Set<dstd::String> lines;
    dstd::StringView rest = content;
    while (!rest.empty()) {
        const std::size_t eol = rest.find('\n');
        lines.insert(dstd::String(rest.substr(0, eol)));
        rest = eol == dstd::StringView::npos ? dstd::StringView{}
                                             : rest.substr(eol + 1);
    }
    CHECK(lines.count("HDR") == 1);
    CHECK(lines.count("worker0") == 1);
    CHECK(lines.count("worker1") == 1);
    CHECK(lines.count("worker2") == 1);
    CHECK(lines.count("END") == 1);

    dstd::fs::remove(path).discard();
}

TEST_CASE("StripedWriter - empty chunks are no-ops") {
    dstd::String path = tmp_path("test_striped_empty.txt");
    std::remove(path.c_str());

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        auto w = make_striped_writer();
        if (co_await w->open(dstd::String(path), 2, false, nullptr) != 0)
            co_return 1;
        if (co_await w->write_chunk(0, dstd::ByteView()) != 0) co_return 2;
        if (co_await w->write_chunk(1, dstd::ByteView()) != 0) co_return 3;
        if (co_await w->close() != 0) co_return 4;
        co_return 0;
    };
    CHECK(dftu_utils_test::ok(runtime.submit(task(), "striped_empty").get()) ==
          0);
    CHECK(dstd::fs::exists(path).value());
    CHECK(dstd::fs::file_size(path).value() == 0);
    dstd::fs::remove(path).discard();
}

TEST_CASE("StripedWriter - output_paths returns single entry") {
    dstd::String path = tmp_path("test_striped_paths.txt");
    std::remove(path.c_str());

    Runtime runtime(2);
    dstd::Vector<dstd::String> paths;
    auto task = [&]() -> CoroTask<int> {
        auto w = make_striped_writer();
        if (co_await w->open(dstd::String(path), 4, false, nullptr) != 0)
            co_return 1;
        paths = w->output_paths();
        co_await w->close();
        co_return 0;
    };
    CHECK(dftu_utils_test::ok(runtime.submit(task(), "striped_paths").get()) ==
          0);
    REQUIRE(paths.size() == 1);
    CHECK(paths[0] == path);
    dstd::fs::remove(path).discard();
}
