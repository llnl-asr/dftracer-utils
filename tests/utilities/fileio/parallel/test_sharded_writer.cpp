// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <doctest/doctest.h>
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

void cleanup(const dstd::Vector<dstd::String>& paths) {
    for (const auto& p : paths) std::remove(p.c_str());
}

}  // namespace

TEST_CASE("ShardedWriter - one shard per worker with header/footer placement") {
    dstd::String base = tmp_path("test_sharded_basic");
    dstd::Vector<dstd::String> expected = {base + ".shard_0", base + ".shard_1",
                                           base + ".shard_2"};
    cleanup(expected);

    Runtime runtime(2);
    dstd::Vector<dstd::String> paths;
    auto task = [&]() -> CoroTask<int> {
        auto w = make_sharded_writer();
        if (co_await w->open(dstd::String(base), 3, false, nullptr) != 0)
            co_return 1;
        if (co_await w->write_header(sv("HDR\n")) != 0) co_return 2;
        if (co_await w->write_chunk(0, sv("A\n")) != 0) co_return 3;
        if (co_await w->write_chunk(1, sv("B\n")) != 0) co_return 4;
        if (co_await w->write_chunk(2, sv("C\n")) != 0) co_return 5;
        if (co_await w->write_footer(sv("END\n")) != 0) co_return 6;
        paths = w->output_paths();
        if (co_await w->close() != 0) co_return 7;
        co_return 0;
    };
    CHECK(runtime.submit(task(), "sharded_basic").get() == 0);

    REQUIRE(paths.size() == 3);
    CHECK(paths == expected);

    CHECK(read_all(paths[0]) == "HDR\nA\n");  // header + worker 0
    CHECK(read_all(paths[1]) == "B\n");
    CHECK(read_all(paths[2]) == "C\nEND\n");  // worker 2 + footer

    cleanup(expected);
}

TEST_CASE("ShardedWriter - gzip_extension appends .gz to shard names") {
    dstd::String base = tmp_path("test_sharded_gz");
    dstd::Vector<dstd::String> expected = {base + ".shard_0.gz",
                                           base + ".shard_1.gz"};
    cleanup(expected);

    Runtime runtime(2);
    dstd::Vector<dstd::String> paths;
    auto task = [&]() -> CoroTask<int> {
        auto w = make_sharded_writer();
        if (co_await w->open(dstd::String(base), 2, true, nullptr) != 0)
            co_return 1;
        if (co_await w->write_chunk(0, sv("X")) != 0) co_return 2;
        if (co_await w->write_chunk(1, sv("Y")) != 0) co_return 3;
        paths = w->output_paths();
        if (co_await w->close() != 0) co_return 4;
        co_return 0;
    };
    CHECK(runtime.submit(task(), "sharded_gz").get() == 0);
    CHECK(paths == expected);
    for (const auto& p : expected) CHECK(dstd::fs::exists(p).value());
    cleanup(expected);
}

TEST_CASE("ShardedWriter - out-of-range worker_idx fails") {
    dstd::String base = tmp_path("test_sharded_oor");
    cleanup({base + ".shard_0"});

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        auto w = make_sharded_writer();
        if (co_await w->open(dstd::String(base), 1, false, nullptr) != 0)
            co_return 1;
        auto rc = co_await w->write_chunk(5, sv("oops"));
        co_await w->close();
        co_return rc == 0 ? 99 : 0;
    };
    CHECK(runtime.submit(task(), "sharded_oor").get() == 0);
    cleanup({base + ".shard_0"});
}
