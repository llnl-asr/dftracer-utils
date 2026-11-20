// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/parallel/parallel_writer.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdio>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::fileio::parallel;

namespace {

dstd::String tmp_path(dstd::StringView name) {
    return dstd::String(
        dftu_utils_test::make_unique_test_path(dstd::String(name)).str());
}

dstd::String read_all(dstd::StringView path) {
    return dstd::fs::read_file(dstd::PathView(path)).value_or(dstd::String());
}

dstd::ByteView sv(dstd::StringView s) {
    return dstd::ByteView(s.data(), s.size());
}

void cleanup(dstd::Span<const dstd::String> paths) {
    for (const auto& p : paths) std::remove(p.c_str());
}

}  // namespace

TEST_CASE("ShardedWriter - one shard per worker with header/footer placement") {
    dstd::String base = tmp_path("test_sharded_basic");
    dstd::Vector<dstd::String> expected = {dstd::fmt<"{}.shard_0">(base),
                                           dstd::fmt<"{}.shard_1">(base),
                                           dstd::fmt<"{}.shard_2">(base)};
    cleanup(expected);

    Runtime runtime(2);
    dstd::Vector<dstd::String> paths;
    auto task = [&]() -> CoroTask<int> {
        auto w = make_sharded_writer();
        auto open_result =
            co_await w->open(dstd::String(base), 3, false, nullptr);
        if (open_result != 0) co_return 1;
        auto write_header_result = co_await w->write_header(sv("HDR\n"));
        if (write_header_result != 0) co_return 2;
        auto write_chunk_result = co_await w->write_chunk(0, sv("A\n"));
        if (write_chunk_result != 0) co_return 3;
        auto write_chunk_result2 = co_await w->write_chunk(1, sv("B\n"));
        if (write_chunk_result2 != 0) co_return 4;
        auto write_chunk_result3 = co_await w->write_chunk(2, sv("C\n"));
        if (write_chunk_result3 != 0) co_return 5;
        auto write_footer_result = co_await w->write_footer(sv("END\n"));
        if (write_footer_result != 0) co_return 6;
        paths = w->output_paths();
        auto close_result = co_await w->close();
        if (close_result != 0) co_return 7;
        co_return 0;
    };
    CHECK(dftu_utils_test::ok(runtime.submit(task(), "sharded_basic").get()) ==
          0);

    REQUIRE(paths.size() == 3);
    CHECK(paths == expected);

    CHECK(read_all(paths[0]) == "HDR\nA\n");  // header + worker 0
    CHECK(read_all(paths[1]) == "B\n");
    CHECK(read_all(paths[2]) == "C\nEND\n");  // worker 2 + footer

    cleanup(expected);
}

TEST_CASE("ShardedWriter - gzip_extension appends .gz to shard names") {
    dstd::String base = tmp_path("test_sharded_gz");
    dstd::Vector<dstd::String> expected = {dstd::fmt<"{}.shard_0.gz">(base),
                                           dstd::fmt<"{}.shard_1.gz">(base)};
    cleanup(expected);

    Runtime runtime(2);
    dstd::Vector<dstd::String> paths;
    auto task = [&]() -> CoroTask<int> {
        auto w = make_sharded_writer();
        auto open_result =
            co_await w->open(dstd::String(base), 2, true, nullptr);
        if (open_result != 0) co_return 1;
        auto write_chunk_result = co_await w->write_chunk(0, sv("X"));
        if (write_chunk_result != 0) co_return 2;
        auto write_chunk_result2 = co_await w->write_chunk(1, sv("Y"));
        if (write_chunk_result2 != 0) co_return 3;
        paths = w->output_paths();
        auto close_result = co_await w->close();
        if (close_result != 0) co_return 4;
        co_return 0;
    };
    CHECK(dftu_utils_test::ok(runtime.submit(task(), "sharded_gz").get()) == 0);
    CHECK(paths == expected);
    for (const auto& p : expected) CHECK(dstd::fs::exists(p).value());
    cleanup(expected);
}

TEST_CASE("ShardedWriter - out-of-range worker_idx fails") {
    dstd::String base = tmp_path("test_sharded_oor");
    const dstd::String shard_files[] = {dstd::fmt<"{}.shard_0">(base)};
    cleanup(shard_files);

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        auto w = make_sharded_writer();
        auto open_result =
            co_await w->open(dstd::String(base), 1, false, nullptr);
        if (open_result != 0) co_return 1;
        auto rc = co_await w->write_chunk(5, sv("oops"));
        co_await w->close();
        co_return rc == 0 ? 99 : 0;
    };
    CHECK(dftu_utils_test::ok(runtime.submit(task(), "sharded_oor").get()) ==
          0);
    const dstd::String leftover[] = {dstd::fmt<"{}.shard_0">(base)};
    cleanup(leftover);
}
